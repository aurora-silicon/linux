// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple T8152 (J873, Mac mini M6), T8142 (J813, MacBook Air M5) and T6050
//! (J714s, MacBook Pro M5 Pro) AOP audio
//!
//! The AOP firmware owns the audio fabric, as on T8140. The ADT
//! (/arm-io/iop-audio-controller) names its services; all of them run on the
//! base-ns ADMAC:
//!
//! * `cout`/`cin`: the CS42L84 headphone jack, base-ns TX5/RX5 (Linux
//!   channels 10 and 11).
//! * `spkr`: the SN012776 speaker amplifier, base-ns TX1 (channel 2), its
//!   sense data on RX1.
//! * `hpai` (J813): the microphone array, three PDM microphones the firmware
//!   delivers as float32 on leap-s ADMAC RX0. It has no codec, so it is the
//!   capture PCM of a small card of its own.
//!
//! Each service is an ASoC back-end DAI: startup attaches it and requests
//! `pw0 ` (which also brings up the audio fabric), prepare requests `pwrd`
//! (the firmware then runs the serializer and its clocks) and shutdown goes
//! back to `pw0 `. On T8152 and T8142 the serializer (MCA) leaves are the
//! AP's to power; on T6050 the firmware powers them itself. The front-end PCMs
//! are plain dmaengine PCMs on the base-ns channels except for speaker V/I
//! sense on multi-amp streams, which is served from a driver-owned cyclic ring
//! (`SenseRing`) armed on the stopped serializer before every `pwrd` so sense
//! capture starts and restarts stay frame-aligned. Macaudio links them to the
//! codecs.
//!
//! Copyright (C) The Asahi Linux Contributors
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{
    ffi::c_void,
    mem, ptr, slice,
    sync::atomic::{
        fence, AtomicBool, AtomicI32, AtomicI64, AtomicPtr, AtomicU64, AtomicUsize, Ordering,
    },
};

use kernel::{
    bindings, c_str, device,
    device::Core,
    error::from_err_ptr,
    module_platform_driver, new_mutex, of, platform,
    prelude::*,
    soc::apple::aop::{from_fourcc, EPICService, AOP},
    str::CString,
    sync::{
        aref::ARef,
        atomic::{Atomic, AtomicFlag, Relaxed},
        Arc, ArcBorrow, Mutex,
    },
    types::{ForeignOwnable, Opaque},
    workqueue::{self, impl_has_work, new_work, Work, WorkItem},
};

const EPIC_SUBTYPE_WRAPPED_CALL: u16 = 0x20;
const CALLTYPE_AUDIO_ATTACH_DEVICE: u32 = 0xc3000002;
const CALLTYPE_AUDIO_GET_PROP: u32 = 0xc3000004;
const CALLTYPE_AUDIO_SET_PROP: u32 = 0xc3000005;
const PROP_POWER_STATE: u32 = 200;
const PROP_POWER_REQUEST: u32 = 202;
const POWER_STATE_PW0: u32 = from_fourcc(b"pw0 ");
const POWER_STATE_PWRD: u32 = from_fourcc(b"pwrd");
/// kIOReturnBusy: the device is already attached (a module reload).
const AUDIO_RET_BUSY: u32 = 0xe00002d5;
const AOP_TIMEBASE_HZ: u64 = 24_000_000;

/// Leaf power-up attempts while the firmware raises the fabric after `pw0 `.
const PD_GET_ATTEMPTS: u32 = 60;
const PD_GET_RETRY_MS: i64 = 10;
/// The MCA serializer leaves (audio_mca0_m .. audio_mca5_m).
const MCA_COUNT: usize = 6;
const MCA_NAMES: [&CStr; MCA_COUNT] = [c"mca0", c"mca1", c"mca2", c"mca3", c"mca4", c"mca5"];
/// The microphone array's LEAP leaves: the leap-s ADMAC, its RX0 channel and
/// the LEAP serializer. A LEAP ADMAC's channel registers only answer with
/// these powered (an access without them faults the bus).
const LEAP_NAMES: [&CStr; 3] = [c"leap_s_a", c"leap_rx0", c"leap_mca"];
/// The microphone array's stream (ADT audio-hp-mic-proxy physical
/// description): 48 kHz, three float32 channels.
const HPAI_RATE: u32 = 48000;
const HPAI_CHANNELS: u32 = 3;
const HPAI_START_WORK: u64 = 1;

fn aop_timestamp() -> u64 {
    // SAFETY: both helpers read the architected timer and take no arguments.
    let (counter, rate) =
        unsafe { (bindings::arch_timer_counter(), bindings::arch_timer_rate()) };
    let rate = u64::from(rate);
    (counter / rate) * AOP_TIMEBASE_HZ + (counter % rate) * AOP_TIMEBASE_HZ / rate
}

fn le_u32(buf: &[u8], off: usize) -> Result<u32> {
    let b = buf.get(off..off + 4).ok_or(EINVAL)?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct AudioAttachDevice {
    _zero0: u32,
    unk0: u32,
    calltype: u32,
    _zero1: [u8; 16],
    cookie: u32,
    len: u64,
    dev_id: u32,
    _zero2: u32,
}

#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct AudioGetDeviceProp {
    _zero0: u32,
    unk0: u32,
    calltype: u32,
    _zero1: [u8; 16],
    _pad0: u32,
    len: u64,
    dev_id: u32,
    modifier: u32,
    unk6: u32,
}

#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct AudioSetDeviceProp<T: Copy + Default> {
    _zero0: u32,
    unk0: u32,
    calltype: u32,
    _zero1: [u8; 16],
    _pad0: u32,
    len: u64,
    dev_id: u32,
    modifier: u32,
    len2: u32,
    data: T,
}

/// Property 202: the device, a per-device sequence, the request time in the
/// firmware's 24 MHz timebase and the target state.
#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct PowerRequest {
    dev_id: u32,
    sequence: u32,
    timestamp: u64,
    _zero0: u32,
    state: u32,
    param: u32,
    _zero1: [u8; 20],
}

/// A named power domain of this device, runtime-powered around the streams
/// that need it.
struct PowerDomain {
    dev: ptr::NonNull<bindings::device>,
}

// SAFETY: the virtual domain device belongs to the PM core, which serializes
// its runtime-PM state; this handle only passes the pointer to those calls.
unsafe impl Send for PowerDomain {}
// SAFETY: as for `Send`.
unsafe impl Sync for PowerDomain {}

impl PowerDomain {
    fn attach(dev: &device::Device, name: &CStr) -> Result<Option<Self>> {
        // SAFETY: `dev` is live for the call and `name` is NUL-terminated.
        let pd = from_err_ptr(unsafe {
            bindings::dev_pm_domain_attach_by_name(dev.as_raw(), name.as_char_ptr())
        })?;
        Ok(ptr::NonNull::new(pd).map(|dev| PowerDomain { dev }))
    }

    /// Takes a runtime-PM reference, retrying while the parent fabric that
    /// the firmware brings up after `pw0 ` is still off.
    fn get(&self) -> Result<()> {
        let mut rc = 0;
        for _ in 0..PD_GET_ATTEMPTS {
            // SAFETY: the domain device stays attached for the lifetime of `self`.
            rc = unsafe { bindings::pm_runtime_get_sync(self.dev.as_ptr()) };
            if rc >= 0 {
                return Ok(());
            }
            // SAFETY: as above; undo the usage count and the error state.
            unsafe {
                bindings::pm_runtime_put_noidle(self.dev.as_ptr());
                bindings::__pm_runtime_set_status(
                    self.dev.as_ptr(),
                    bindings::rpm_status_RPM_SUSPENDED as u32,
                );
            }
            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(PD_GET_RETRY_MS));
        }
        Err(Error::from_errno(rc))
    }

    fn put(&self) {
        // SAFETY: the domain device stays attached for the lifetime of `self`.
        unsafe { bindings::pm_runtime_put_sync(self.dev.as_ptr()) };
    }
}

impl Drop for PowerDomain {
    fn drop(&mut self) {
        // SAFETY: attached in `attach`; every `get` has been balanced by then.
        unsafe { bindings::dev_pm_domain_detach(self.dev.as_ptr(), true) };
    }
}

/// One firmware audio service driven as an ASoC back-end.
struct Service {
    dev_id: u32,
    name: &'static str,
    sequence: Atomic<u32>,
    /// In `pwrd`.
    running: AtomicFlag,
    /// Stop and restart the serializer on every prepare, so that a stream
    /// restarted after an underrun begins on a frame boundary. The mono
    /// speaker has one slot per frame and keeps its clocks running instead:
    /// its amplifier stays unmuted between prepares and reports "bad TDM
    /// clock" whenever the bit clock stops under it.
    restart_on_prepare: bool,
}

impl Service {
    const fn new(dev_id: u32, name: &'static str, restart_on_prepare: bool) -> Service {
        Service {
            dev_id,
            name,
            sequence: Atomic::new(0),
            running: AtomicFlag::new(false),
            restart_on_prepare,
        }
    }
}

/// Serialized power state of the speaker back-end (`spkr`).
struct SpkrState {
    open: bool,
    running: bool,
    /// True only after `pw0 ` has been verified and before any `pwrd`
    /// transition attempt. `SenseRing` may only be armed while
    /// `!running && pw0_verified`.
    pw0_verified: bool,
    resume_run: bool,
}

impl SpkrState {
    const fn new() -> Self {
        Self {
            open: false,
            running: false,
            pw0_verified: false,
            resume_run: false,
        }
    }

    fn ensure_pw0(
        &mut self,
        spkr_running: &AtomicFlag,
        set_pw0: impl FnOnce() -> Result<()>,
    ) -> Result<()> {
        if self.running || (self.open && !self.pw0_verified) {
            self.pw0_verified = false;
            set_pw0()?;
            self.running = false;
            self.pw0_verified = true;
            spkr_running.store(false, Relaxed);
        }
        Ok(())
    }

    fn start_with_ring(
        &mut self,
        spkr_running: &AtomicFlag,
        ring_stop: impl Fn(),
        ring_start: impl FnOnce() -> Result<()>,
        set_pwrd: impl FnOnce() -> Result<()>,
    ) -> Result<()> {
        if self.running || !self.pw0_verified {
            return Err(EIO);
        }
        ring_stop();
        ring_start()?;
        self.pw0_verified = false;
        if let Err(e) = set_pwrd() {
            ring_stop();
            return Err(e);
        }
        self.running = true;
        spkr_running.store(true, Relaxed);
        Ok(())
    }

    fn stop(
        &mut self,
        spkr_running: &AtomicFlag,
        has_ring: bool,
        set_pw0: impl FnOnce() -> Result<()>,
        ring_stop: impl FnOnce(),
    ) -> Result<()> {
        self.ensure_pw0(spkr_running, set_pw0)?;
        if has_ring {
            ring_stop();
        }
        Ok(())
    }

    fn prepare_with_ring(
        &mut self,
        spkr_running: &AtomicFlag,
        quiesce: impl FnOnce() -> Result<()>,
        set_pw0: impl FnOnce() -> Result<()>,
        ring_stop: impl Fn(),
        ring_start: impl FnOnce() -> Result<()>,
        set_pwrd: impl FnOnce() -> Result<()>,
    ) -> Result<()> {
        if self.running || !self.pw0_verified {
            quiesce()?;
            self.pw0_verified = false;
            set_pw0()?;
            self.running = false;
            self.pw0_verified = true;
            spkr_running.store(false, Relaxed);
        }
        self.start_with_ring(spkr_running, ring_stop, ring_start, set_pwrd)
    }

    fn suspend(
        &mut self,
        spkr_running: &AtomicFlag,
        has_ring: bool,
        set_pw0: impl FnOnce() -> Result<()>,
        ring_stop: impl FnOnce(),
    ) -> Result<()> {
        let was_running = self.running;
        let res = self.ensure_pw0(spkr_running, set_pw0);
        if has_ring {
            ring_stop();
        }
        self.resume_run = was_running;
        res
    }

    fn resume_with_ring(
        &mut self,
        spkr_running: &AtomicFlag,
        set_pw0: impl FnOnce() -> Result<()>,
        ring_stop: impl Fn(),
        ring_start: impl FnOnce() -> Result<()>,
        set_pwrd: impl FnOnce() -> Result<()>,
    ) -> Result<()> {
        if !mem::take(&mut self.resume_run) || !self.open {
            return Ok(());
        }
        self.ensure_pw0(spkr_running, set_pw0)?;
        self.start_with_ring(spkr_running, ring_stop, ring_start, set_pwrd)
    }
}

const SENSE_RATE: u64 = 48000;
const NSEC_PER_SEC: u64 = 1_000_000_000;
/// 2.5 ms periods (120 frames at 48 kHz), 32 of them: an 80 ms ring, delivered
/// in whole-frame chunks small enough that a 512-frame front-end period
/// completes well within its headroom. Both 8-channel (16 B/frame -> 1920 B)
/// and 12-channel (24 B/frame -> 2880 B) periods stay aligned to whole frames
/// and to the ADMAC's 48-byte RX FIFO threshold.
const SENSE_RING_PERIOD_FRAMES: usize = 120;
const SENSE_RING_PERIODS: usize = 32;
const SENSE_RING_FRAMES: usize = SENSE_RING_PERIOD_FRAMES * SENSE_RING_PERIODS;
const SENSE_RING_NS: u64 = SENSE_RING_FRAMES as u64 * NSEC_PER_SEC / SENSE_RATE;
const SENSE_RING_PERIOD_NS: u64 = SENSE_RING_PERIOD_FRAMES as u64 * NSEC_PER_SEC / SENSE_RATE;
/// SNDRV_PCM_STATE_RUNNING and _XRUN (cast macros the bindings do not carry).
const PCM_STATE_RUNNING: bindings::snd_pcm_state_t = 3;
const PCM_STATE_XRUN: bindings::snd_pcm_state_t = 4;

/// The ring may have gone round once or more in `elapsed_ns` since the last
/// callback: its position alone cannot tell a lap from no advance, or two laps
/// from one. The DMA runs at the sense rate and its reported position lags it
/// by less than a ring period, so a lap needs at least the ring's time less a
/// period.
fn sense_ring_may_lap(elapsed_ns: i64) -> bool {
    elapsed_ns < 0 || elapsed_ns as u64 + SENSE_RING_PERIOD_NS >= SENSE_RING_NS
}

/// Returns whether `incoming` frames fit strictly inside the capture front-end
/// ring buffer of `buffer_frames` without overwriting unread frames
/// (`unread_alsa`) or frames already copied since the last pointer sync
/// (`pending`), and without advancing the modulo pointer back to the same
/// position (an exact full-buffer advance is ambiguous to ALSA's pointer check).
fn sense_ring_has_fe_space(
    buffer_frames: usize,
    unread_alsa: usize,
    pending: usize,
    incoming: usize,
) -> bool {
    unread_alsa
        .checked_add(pending)
        .and_then(|used| used.checked_add(incoming))
        .is_some_and(|total| total < buffer_frames)
}

/// The driver-owned speaker sense capture (base-ns ADMAC RX): a cyclic ring
/// armed on the stopped serializer before every `pwrd`, so ring offset 0 is
/// slot 0 of a frame and every whole-frame offset is a frame start. The sense
/// front-end copies whole frames out of this ring instead of starting or
/// stopping the ADMAC channel while the serializer runs, which otherwise slips
/// the V/I sense slots modulo the channel count.
///
/// The ring fields change only with the channel stopped and synchronized (no
/// callback runs); `start` publishes them before the channel starts.
struct SenseRing {
    frame_bytes: usize,
    chan: AtomicPtr<bindings::dma_chan>,
    cpu: AtomicPtr<u8>,
    handle: AtomicU64,
    cookie: AtomicI32,
    /// Ring offset (a whole frame) up to which the callback has consumed the data.
    done: AtomicUsize,
    /// When `done` was last moved (or the ring started), CLOCK_MONOTONIC ns.
    done_at: AtomicI64,
    /// True while a cyclic transfer is active; cleared before terminating the
    /// channel so `sense_ring_done` never consumes a terminated channel's
    /// zero-residue status.
    active: AtomicBool,
    /// The attached sense front-end, or null; cleared under that front-end's
    /// stream lock, which the callback holds while it uses the front-end.
    sub: AtomicPtr<bindings::snd_pcm_substream>,
    /// Frames delivered to the front-end since its start; changed under its
    /// stream lock.
    fe_pos: AtomicUsize,
    /// The front-end runs (its trigger sets this under the stream lock).
    fe_running: AtomicBool,
}

impl SenseRing {
    fn new(frame_bytes: usize) -> Self {
        Self {
            frame_bytes,
            chan: AtomicPtr::new(ptr::null_mut()),
            cpu: AtomicPtr::new(ptr::null_mut()),
            handle: AtomicU64::new(0),
            cookie: AtomicI32::new(0),
            done: AtomicUsize::new(0),
            done_at: AtomicI64::new(0),
            active: AtomicBool::new(false),
            sub: AtomicPtr::new(ptr::null_mut()),
            fe_pos: AtomicUsize::new(0),
            fe_running: AtomicBool::new(false),
        }
    }

    fn period_bytes(&self) -> usize {
        SENSE_RING_PERIOD_FRAMES * self.frame_bytes
    }

    fn ring_bytes(&self) -> usize {
        SENSE_RING_FRAMES * self.frame_bytes
    }

    /// Requests the channel and the ring (once per speaker session) and starts
    /// the cyclic transfer. The caller holds `spkr_state` with the fabric
    /// powered and the serializer stopped (`pw0 `).
    fn start(&self, dev: &device::Device) -> Result<()> {
        let ring_bytes = self.ring_bytes();
        let period_bytes = self.period_bytes();
        let mut chan = self.chan.load(Ordering::Acquire);
        if chan.is_null() {
            // SAFETY: the device is live and the name is NUL-terminated.
            chan = from_err_ptr(unsafe {
                bindings::dma_request_chan(dev.as_raw(), c"sense".as_char_ptr())
            })?;
            let mut handle: bindings::dma_addr_t = 0;
            // SAFETY: the channel's device is the ADMAC; it outlives the ring,
            // which `release` frees before releasing the channel.
            let cpu = unsafe {
                bindings::dma_alloc_attrs(
                    (*(*chan).device).dev,
                    ring_bytes,
                    &mut handle,
                    bindings::GFP_KERNEL,
                    0,
                )
            };
            if cpu.is_null() {
                // SAFETY: requested above, unused.
                unsafe { bindings::dma_release_channel(chan) };
                return Err(ENOMEM);
            }
            self.cpu.store(cpu.cast(), Ordering::Release);
            self.handle.store(handle as u64, Ordering::Release);
            self.chan.store(chan, Ordering::Release);
        }
        let handle = self.handle.load(Ordering::Acquire) as bindings::dma_addr_t;
        let mut cfg = bindings::dma_slave_config::default();
        cfg.direction = bindings::dma_transfer_direction_DMA_DEV_TO_MEM;
        cfg.src_addr_width = bindings::dma_slave_buswidth_DMA_SLAVE_BUSWIDTH_2_BYTES;
        cfg.src_port_window_size = 4;
        // SAFETY: `chan` is our live, stopped channel; the descriptor is ours
        // until submitted.
        unsafe {
            let dma = (*chan).device;
            let ret = match (*dma).device_config {
                Some(f) => f(chan, &mut cfg),
                None => ENOSYS.to_errno(),
            };
            if ret < 0 {
                return Err(Error::from_errno(ret));
            }
            let prep = (*dma).device_prep_dma_cyclic.ok_or(ENOSYS)?;
            let desc = prep(
                chan,
                handle,
                ring_bytes,
                period_bytes,
                bindings::dma_transfer_direction_DMA_DEV_TO_MEM,
                bindings::dma_ctrl_flags_DMA_PREP_INTERRUPT as _,
            );
            if desc.is_null() {
                return Err(EIO);
            }
            (*desc).callback = Some(sense_ring_done);
            (*desc).callback_param = ptr::from_ref(self).cast_mut().cast();
            self.done.store(0, Ordering::Release);
            // SAFETY: no preconditions.
            self.done_at.store(bindings::ktime_get(), Ordering::Release);
            let cookie = ((*desc).tx_submit.ok_or(EIO)?)(desc);
            if cookie < 0 {
                return Err(Error::from_errno(cookie));
            }
            self.cookie.store(cookie, Ordering::Release);
            if let Some(f) = (*dma).device_issue_pending {
                self.active.store(true, Ordering::Release);
                f(chan);
            }
        }
        Ok(())
    }

    /// Stops the transfer and waits for its callback; a no-op without a channel.
    fn stop(&self) {
        self.active.store(false, Ordering::Release);
        let chan = self.chan.load(Ordering::Acquire);
        if chan.is_null() {
            return;
        }
        // SAFETY: our live channel; terminate and synchronize are valid on an
        // idle channel too.
        unsafe {
            let dma = (*chan).device;
            if let Some(f) = (*dma).device_terminate_all {
                f(chan);
            }
            if let Some(f) = (*dma).device_synchronize {
                f(chan);
            }
        }
    }

    /// Stops, then frees the ring and releases the channel (with the fabric
    /// still powered).
    fn release(&self) {
        self.stop();
        let chan = self.chan.swap(ptr::null_mut(), Ordering::AcqRel);
        if chan.is_null() {
            return;
        }
        // SAFETY: the transfer is stopped and synchronized; the ring was
        // allocated on this channel's device in `start`.
        unsafe {
            bindings::dma_free_attrs(
                (*(*chan).device).dev,
                self.ring_bytes(),
                self.cpu.swap(ptr::null_mut(), Ordering::AcqRel).cast(),
                self.handle.load(Ordering::Acquire) as bindings::dma_addr_t,
                0,
            );
            bindings::dma_release_channel(chan);
        }
    }

    fn attach(&self, substream: *mut bindings::snd_pcm_substream) {
        self.fe_running.store(false, Ordering::Relaxed);
        self.fe_pos.store(0, Ordering::Relaxed);
        self.sub.store(substream, Ordering::Release);
    }

    /// Detaches the front-end under its stream lock: a callback that read the
    /// pointer before takes the same lock, sees the front-end gone and leaves
    /// it alone.
    ///
    /// # Safety
    ///
    /// `substream` must be the attached front-end, in its close.
    unsafe fn detach(&self, substream: *mut bindings::snd_pcm_substream) {
        // SAFETY: by the caller's contract.
        unsafe {
            let flags = bindings::_snd_pcm_stream_lock_irqsave(substream);
            self.sub.store(ptr::null_mut(), Ordering::Release);
            self.fe_running.store(false, Ordering::Relaxed);
            bindings::snd_pcm_stream_unlock_irqrestore(substream, flags);
        }
    }

    /// Copies `bytes` (whole frames) from ring offset `from` into a running
    /// front-end's buffer.
    ///
    /// # Safety
    ///
    /// `sub` must be a substream this ring had attached. The substream itself
    /// belongs to the PCM and outlives the ring's use of it: the ring stops
    /// and synchronizes with this callback before the card can go away.
    unsafe fn deliver(
        &self,
        sub: *mut bindings::snd_pcm_substream,
        from: usize,
        bytes: usize,
        last_ns: i64,
    ) {
        let frame_bytes = self.frame_bytes;
        let ring_bytes = self.ring_bytes();
        // SAFETY: by the caller's contract; the stream lock orders this with
        // the trigger, the pointer callback and `detach`.
        let flags = unsafe { bindings::_snd_pcm_stream_lock_irqsave(sub) };
        let rt = if self.sub.load(Ordering::Acquire) == sub {
            // SAFETY: still attached under the lock; the runtime lives until
            // after the front-end's close.
            unsafe { (*sub).runtime }
        } else {
            ptr::null_mut()
        };
        // SAFETY: an attached front-end's runtime (see above).
        if !rt.is_null()
            && self.fe_running.load(Ordering::Relaxed)
            && unsafe { (*rt).state } == PCM_STATE_RUNNING
            && unsafe { (*rt).frame_bits } as usize == frame_bytes * 8
        {
            // SAFETY: no preconditions.
            if sense_ring_may_lap(unsafe { bindings::ktime_get() } - last_ns) {
                // SAFETY: a running front-end, under its stream lock.
                unsafe { bindings::snd_pcm_stop(sub, PCM_STATE_XRUN) };
            } else {
                // SAFETY: as above; status and control are valid while running.
                let (area, frames, period, hw_ptr, appl_ptr, boundary) = unsafe {
                    (
                        (*rt).dma_area,
                        (*rt).buffer_size as usize,
                        (*rt).period_size as usize,
                        (*(*rt).status).hw_ptr as usize,
                        (*(*rt).control).appl_ptr as usize,
                        (*rt).boundary as usize,
                    )
                };
                let ring = self.cpu.load(Ordering::Acquire);
                let incoming = bytes / frame_bytes;
                if !area.is_null() && !ring.is_null() && frames > 0 && incoming > 0 {
                    let mut pos = self.fe_pos.load(Ordering::Relaxed);
                    let unread_alsa = if hw_ptr >= appl_ptr {
                        hw_ptr - appl_ptr
                    } else if boundary > 0 {
                        hw_ptr + boundary - appl_ptr
                    } else {
                        frames
                    };
                    let pending = (pos % frames + frames - hw_ptr % frames) % frames;
                    if !sense_ring_has_fe_space(frames, unread_alsa, pending, incoming) {
                        // SAFETY: a running front-end, under its stream lock.
                        unsafe { bindings::snd_pcm_stop(sub, PCM_STATE_XRUN) };
                    } else {
                        let first = pos;
                        let mut src = from;
                        let mut left = incoming;
                        while left > 0 {
                            let off = pos % frames;
                            let n = left.min(frames - off).min((ring_bytes - src) / frame_bytes);
                            // SAFETY: `n` frames fit at `off` in the front-end
                            // buffer and at `src` in the ring; post-copy lap
                            // validation below rejects the batch if DMA could
                            // have reached `from` before the copy finished.
                            unsafe {
                                ptr::copy_nonoverlapping(
                                    ring.add(src),
                                    area.add(off * frame_bytes),
                                    n * frame_bytes,
                                )
                            };
                            pos += n;
                            left -= n;
                            src = (src + n * frame_bytes) % ring_bytes;
                        }
                        fence(Ordering::Acquire);
                        // SAFETY: no preconditions.
                        if sense_ring_may_lap(unsafe { bindings::ktime_get() } - last_ns) {
                            // SAFETY: a running front-end, under its stream lock.
                            unsafe { bindings::snd_pcm_stop(sub, PCM_STATE_XRUN) };
                        } else {
                            self.fe_pos.store(pos, Ordering::Relaxed);
                            if first / period.max(1) != pos / period.max(1) {
                                // SAFETY: a running, attached front-end, under its stream lock.
                                unsafe { bindings::snd_pcm_period_elapsed_under_stream_lock(sub) };
                            }
                        }
                    }
                }
            }
        }
        // SAFETY: locked above.
        unsafe { bindings::snd_pcm_stream_unlock_irqrestore(sub, flags) };
    }

    /// The ring may have lapped since the last callback: stop a running
    /// front-end with an XRUN rather than delivering frames shifted in time
    /// against playback.
    ///
    /// # Safety
    ///
    /// As for [`SenseRing::deliver`].
    unsafe fn lost(&self, sub: *mut bindings::snd_pcm_substream) {
        // SAFETY: as in `deliver`.
        let flags = unsafe { bindings::_snd_pcm_stream_lock_irqsave(sub) };
        if self.sub.load(Ordering::Acquire) == sub && self.fe_running.load(Ordering::Relaxed) {
            // SAFETY: an attached front-end, under its stream lock; its
            // runtime lives until after its close.
            let rt = unsafe { (*sub).runtime };
            // SAFETY: as above.
            if !rt.is_null() && unsafe { (*rt).state } == PCM_STATE_RUNNING {
                // SAFETY: a running front-end, under its stream lock.
                unsafe { bindings::snd_pcm_stop(sub, PCM_STATE_XRUN) };
            }
        }
        // SAFETY: locked above.
        unsafe { bindings::snd_pcm_stream_unlock_irqrestore(sub, flags) };
    }
}

/// The ring's period callback (the ADMAC tasklet): hands the whole frames
/// written since the last call to a running sense front-end, or drops them.
///
/// # Safety
///
/// `param` is the `SenseRing` that set up the transfer; `stop` synchronizes
/// with this callback before the ring changes or goes away.
unsafe extern "C" fn sense_ring_done(param: *mut c_void) {
    // SAFETY: by the function's contract.
    let ring = unsafe { &*param.cast::<SenseRing>() };
    if !ring.active.load(Ordering::Acquire) {
        return;
    }
    let chan = ring.chan.load(Ordering::Acquire);
    if chan.is_null() {
        return;
    }
    let mut state = bindings::dma_tx_state {
        last: 0,
        used: 0,
        residue: 0,
        in_flight_bytes: 0,
    };
    // SAFETY: our live channel and cookie.
    let status = unsafe {
        match (*(*chan).device).device_tx_status {
            Some(f) => f(chan, ring.cookie.load(Ordering::Acquire), &mut state),
            None => return,
        }
    };
    if !ring.active.load(Ordering::Acquire) {
        return;
    }
    // SAFETY: no preconditions.
    let now_ns = unsafe { bindings::ktime_get() };
    let ring_bytes = ring.ring_bytes();
    let frame_bytes = ring.frame_bytes;
    let residue = state.residue as usize;
    if status == bindings::dma_status_DMA_ERROR || residue > ring_bytes {
        return;
    }
    // The device wrote everything up to here before it reported the residue.
    fence(Ordering::SeqCst);
    let now = (ring_bytes - residue) % ring_bytes / frame_bytes * frame_bytes;
    let from = ring.done.swap(now, Ordering::AcqRel);
    let last_ns = ring.done_at.swap(now_ns, Ordering::AcqRel);
    let sub = ring.sub.load(Ordering::Acquire);
    if sense_ring_may_lap(now_ns - last_ns) {
        if !sub.is_null() {
            // SAFETY: a substream the ring had attached (`lost` re-checks under its lock).
            unsafe { ring.lost(sub) };
        }
        return;
    }
    let bytes = (now + ring_bytes - from) % ring_bytes;
    if bytes == 0 {
        return;
    }
    if !sub.is_null() {
        // SAFETY: a substream the ring had attached (`deliver` re-checks under its lock).
        unsafe { ring.deliver(sub, from, bytes, last_ns) };
    }
}

#[pin_data]
struct T8152AopAudio {
    dev: ARef<device::Device>,
    adata: Arc<dyn AOP>,
    service: EPICService,
    cout: Service,
    cin: Service,
    spkr: Service,
    #[pin]
    spkr_state: Mutex<SpkrState>,
    /// Driver-owned cyclic sense ring when the speaker stream returns V/I sense.
    sense_ring: Option<SenseRing>,
    mca: KVec<PowerDomain>,
    cfg: &'static HwConfig,
    hpai: Service,
    /// The microphone array's leaves (`LEAP_NAMES`); empty without one.
    leap: KVec<PowerDomain>,
    /// The microphone capture is triggered.
    hpai_want_run: AtomicFlag,
    #[pin]
    hpai_start_work: Work<Self, HPAI_START_WORK>,
}

impl_has_work! {
    impl HasWork<Self, HPAI_START_WORK> for T8152AopAudio { self.hpai_start_work }
}

/// The microphone array's `pwrd`, queued by the capture trigger (a sleeping
/// firmware call). The ADMAC is running by then: started before it, the LEAP
/// produces into a channel nobody services and the capture stays noise.
impl WorkItem<HPAI_START_WORK> for T8152AopAudio {
    type Pointer = Arc<Self>;

    fn run(this: Arc<Self>) {
        if let Err(e) = this.hpai_go() {
            dev_err!(this.dev, "hpai: start failed: {:?}\n", e);
        }
    }
}

impl T8152AopAudio {
    fn call_ret<T>(&self, data: &T, ret_len: usize) -> Result<(u32, KVec<u8>)> {
        // SAFETY: T is a plain packed firmware record.
        let msg = unsafe {
            slice::from_raw_parts(ptr::from_ref(data).cast::<u8>(), mem::size_of::<T>())
        };
        self.adata
            .epic_call_ret(&self.service, EPIC_SUBTYPE_WRAPPED_CALL, msg, ret_len)
    }

    fn call<T>(&self, data: &T) -> Result<u32> {
        // SAFETY: T is a plain packed firmware record.
        let msg = unsafe {
            slice::from_raw_parts(ptr::from_ref(data).cast::<u8>(), mem::size_of::<T>())
        };
        self.adata
            .epic_call(&self.service, EPIC_SUBTYPE_WRAPPED_CALL, msg)
    }

    /// The firmware keeps attachments for its lifetime, so a module reload
    /// sees kIOReturnBusy.
    fn attach(&self, svc: &Service) -> Result<()> {
        let ret = self.call(&AudioAttachDevice {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_ATTACH_DEVICE,
            len: 0x2c,
            dev_id: svc.dev_id,
            ..Default::default()
        })?;
        match ret {
            0 | AUDIO_RET_BUSY => Ok(()),
            _ => {
                dev_err!(self.dev, "unable to attach {}: {:#x}\n", svc.name, ret);
                Err(EIO)
            }
        }
    }

    fn power_state(&self, svc: &Service) -> Result<u32> {
        let (ret, data) = self.call_ret(
            &AudioGetDeviceProp {
                unk0: 0xffffffff,
                calltype: CALLTYPE_AUDIO_GET_PROP,
                len: 0x30,
                dev_id: svc.dev_id,
                modifier: PROP_POWER_STATE,
                unk6: 1,
                ..Default::default()
            },
            8,
        )?;
        if ret != 0 {
            return Err(EIO);
        }
        le_u32(&data, 4)
    }

    fn set_power(&self, svc: &Service, state: u32) -> Result<()> {
        let req = PowerRequest {
            dev_id: svc.dev_id,
            sequence: svc.sequence.fetch_add(1, Relaxed) + 1,
            timestamp: aop_timestamp(),
            state,
            ..Default::default()
        };
        let ret = self.call(&AudioSetDeviceProp {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_SET_PROP,
            len: mem::size_of::<PowerRequest>() as u64 + 0x30,
            dev_id: svc.dev_id,
            modifier: PROP_POWER_REQUEST,
            len2: mem::size_of::<PowerRequest>() as u32,
            data: req,
            ..Default::default()
        })?;
        if ret != 0 {
            dev_err!(
                self.dev,
                "{}: power request {:#x} rejected: {:#x}\n",
                svc.name,
                state,
                ret
            );
            return Err(EIO);
        }
        let now = self.power_state(svc)?;
        if now != state {
            dev_err!(
                self.dev,
                "{}: power state {:#x} after requesting {:#x}\n",
                svc.name,
                now,
                state
            );
            return Err(EIO);
        }
        Ok(())
    }

    fn leaves_up(&self, pds: &[PowerDomain], what: &str) -> Result<()> {
        for (i, pd) in pds.iter().enumerate() {
            if let Err(e) = pd.get() {
                dev_err!(self.dev, "{} leaf {} did not power up: {:?}\n", what, i, e);
                for pd in pds[..i].iter().rev() {
                    pd.put();
                }
                return Err(e);
            }
        }
        Ok(())
    }

    fn leaves_down(pds: &[PowerDomain]) {
        for pd in pds.iter().rev() {
            pd.put();
        }
    }

    fn mca_up(&self) -> Result<()> {
        self.leaves_up(&self.mca, "MCA")
    }

    fn mca_down(&self) {
        Self::leaves_down(&self.mca)
    }

    /// Back-end startup: attach, `pw0 ` (which raises the fabric), then the
    /// serializer leaves.
    fn startup(&self, svc: &Service) -> Result<()> {
        self.attach(svc)?;
        self.set_power(svc, POWER_STATE_PW0)?;
        if let Err(e) = self.mca_up() {
            let _ = self.set_power(svc, POWER_STATE_PW0);
            return Err(e);
        }
        svc.running.store(false, Relaxed);
        Ok(())
    }

    /// The sense capture's back-end. Opening the sense before any other stream
    /// since boot attaches `spkr` and raises `pw0 ` so the audio fabric is up
    /// without disturbing an already-running speaker stream.
    fn sense_startup(&self) -> Result<()> {
        let mut st = self.spkr_state.lock();
        self.attach(&self.spkr)?;
        if !st.running {
            st.pw0_verified = false;
            self.set_power(&self.spkr, POWER_STATE_PW0)?;
            st.pw0_verified = true;
        }
        Ok(())
    }

    fn spkr_startup(&self) -> Result<()> {
        let mut st = self.spkr_state.lock();
        self.attach(&self.spkr)?;
        st.pw0_verified = false;
        self.set_power(&self.spkr, POWER_STATE_PW0)?;
        if let Err(e) = self.mca_up() {
            let _ = self.set_power(&self.spkr, POWER_STATE_PW0);
            return Err(e);
        }
        st.open = true;
        st.running = false;
        st.pw0_verified = true;
        st.resume_run = false;
        self.spkr.running.store(false, Relaxed);
        Ok(())
    }

    /// Speaker prepare: when a sense ring is in use, every playback prepare
    /// starts a fresh serializer with the ring armed first so offset 0 is slot
    /// 0 of a frame. If the serializer was still running or its `pw0 ` state is
    /// unconfirmed (re-prepare after an underrun, failed stop, or resume),
    /// `quiesce` mutes the amps before stopping the clock.
    fn spkr_prepare(&self, quiesce: impl FnOnce() -> Result<()>) -> Result<()> {
        let mut st = self.spkr_state.lock();
        let Some(ring) = &self.sense_ring else {
            if !st.running {
                st.pw0_verified = false;
                self.set_power(&self.spkr, POWER_STATE_PWRD)?;
                st.running = true;
                self.spkr.running.store(true, Relaxed);
            }
            return Ok(());
        };
        st.prepare_with_ring(
            &self.spkr.running,
            quiesce,
            || self.set_power(&self.spkr, POWER_STATE_PW0),
            || ring.stop(),
            || ring.start(&self.dev),
            || self.set_power(&self.spkr, POWER_STATE_PWRD),
        )
    }

    fn spkr_pw0(&self) -> Result<()> {
        if let Err(e) = self.set_power(&self.spkr, POWER_STATE_PW0) {
            dev_err!(self.dev, "spkr: pw0 failed: {:?}\n", e);
            return Err(e);
        }
        Ok(())
    }

    fn spkr_stop_locked(&self, st: &mut SpkrState) -> Result<()> {
        st.stop(
            &self.spkr.running,
            self.sense_ring.is_some(),
            || self.spkr_pw0(),
            || {
                if let Some(ring) = &self.sense_ring {
                    ring.stop();
                }
            },
        )
    }

    fn spkr_hw_free(&self) -> Result<()> {
        if self.sense_ring.is_none() {
            return Ok(());
        }
        let mut st = self.spkr_state.lock();
        self.spkr_stop_locked(&mut st)
    }

    fn spkr_shutdown(&self) {
        {
            let mut st = self.spkr_state.lock();
            let _ = self.spkr_stop_locked(&mut st);
            st.open = false;
            st.resume_run = false;
            // Release the ring and its ADMAC channel while the fabric is still
            // powered (`pw0 `) and before the MCA leaves power down.
            if let Some(ring) = &self.sense_ring {
                ring.release();
            }
        }
        self.mca_down();
    }

    fn spkr_suspend(&self) -> Result<()> {
        let Some(ring) = &self.sense_ring else {
            return Ok(());
        };
        let mut st = self.spkr_state.lock();
        st.suspend(&self.spkr.running, true, || self.spkr_pw0(), || ring.stop())
    }

    fn spkr_resume(&self) -> Result<()> {
        let Some(ring) = &self.sense_ring else {
            return Ok(());
        };
        let mut st = self.spkr_state.lock();
        st.resume_with_ring(
            &self.spkr.running,
            || self.spkr_pw0(),
            || ring.stop(),
            || ring.start(&self.dev),
            || self.set_power(&self.spkr, POWER_STATE_PWRD),
        )
    }

    /// `pwrd`: the firmware starts the serializer clocks and the stream.
    fn run(&self, svc: &Service) -> Result<()> {
        if svc.running.load(Relaxed) {
            return Ok(());
        }
        self.set_power(svc, POWER_STATE_PWRD)?;
        svc.running.store(true, Relaxed);
        Ok(())
    }

    /// The clocks run from prepare on: the codecs see a bit clock as soon
    /// as ASoC powers and unmutes them. A PCM underrun stops the DMA
    /// without closing the back-end; a service that restarts on prepare
    /// retires its run so that the next start begins afresh.
    fn prepare(&self, svc: &Service) -> Result<()> {
        if svc.restart_on_prepare && svc.running.load(Relaxed) {
            self.set_power(svc, POWER_STATE_PW0)?;
            svc.running.store(false, Relaxed);
        }
        self.run(svc)
    }

    fn shutdown(&self, svc: &Service) {
        if svc.running.load(Relaxed) {
            if let Err(e) = self.set_power(svc, POWER_STATE_PW0) {
                dev_err!(self.dev, "{}: pw0 failed: {:?}\n", svc.name, e);
            }
            svc.running.store(false, Relaxed);
        }
        self.mca_down();
    }

    /// Microphone open: attach, `pw0 ` (which raises the fabric), then the
    /// LEAP leaves, before the capture requests its ADMAC channel.
    fn hpai_startup(&self) -> Result<()> {
        self.attach(&self.hpai)?;
        self.set_power(&self.hpai, POWER_STATE_PW0)?;
        self.leaves_up(&self.leap, "LEAP")?;
        self.hpai.running.store(false, Relaxed);
        Ok(())
    }

    fn hpai_go(&self) -> Result<()> {
        if !self.hpai_want_run.load(Relaxed) {
            return Ok(());
        }
        self.run(&self.hpai)
    }

    /// Finish a queued firmware start before retiring the DMA runtime.
    fn flush_hpai_start(&self) {
        self.hpai_want_run.store(false, Relaxed);
        // SAFETY: the work item lives in `self`; flushing waits for a queued
        // `hpai_go`.
        unsafe { bindings::flush_work(Work::raw_get(&self.hpai_start_work)) };
    }

    /// Microphone close, after the channel is released: `pw0 ` then leaves.
    fn hpai_shutdown(&self) {
        self.flush_hpai_start();
        if self.hpai.running.load(Relaxed) {
            if let Err(e) = self.set_power(&self.hpai, POWER_STATE_PW0) {
                dev_err!(self.dev, "hpai: pw0 failed: {:?}\n", e);
            }
            self.hpai.running.store(false, Relaxed);
        }
        Self::leaves_down(&self.leap);
    }
}

// ---- the ASoC component ----

/// Front-end index of a runtime by CPU DAI id; back-ends have none.
const FE_DAI_ID_BASE: u32 = 1;
const FE_COUNT: usize = 3;
const BE_DAI_ID_COUT: u32 = 10;
const BE_DAI_ID_SPKR: u32 = 11;
/// DMA channel names (dma-names on the audio node) per front-end, [playback,
/// capture]: the jack, the speaker and the speaker's sense data.
const FE_DMA_NAMES: [[Option<&CStr>; 2]; FE_COUNT] = [
    [Some(c"cout"), Some(c"cin")],
    [Some(c"spkr"), None],
    [None, Some(c"sense")],
];

const PCM_BUFFER_MAX: usize = 1024 * 1024;
const PCM_PERIOD_BYTES_MIN: usize = 256;
const PCM_PERIOD_BYTES_MAX: usize = 0x10000;

/// The AOP-run serializers take 24-bit samples in 32-bit words.
const FORMATS: u64 = bindings::BINDINGS_SNDRV_PCM_FMTBIT_S24_LE;
/// SNDRV_PCM_FMTBIT_S16_LE (1 << SNDRV_PCM_FORMAT_S16_LE), which the
/// bindings do not export.
const FMTBIT_S16_LE: u64 = 1 << 2;

/// What differs between the machines: the shape of the firmware's `spkr`
/// stream (ADT iop-audio-controller/audio-speaker).
struct HwConfig {
    /// Speaker slots (one per amplifier).
    spkr_channels: u32,
    /// Words the amplifiers send back on the stream's RX side (V/I sense).
    sense_channels: u32,
    sense_formats: u64,
    /// The sense capture is the speaker stream's RX: serve it from a
    /// driver-owned cyclic ring armed before every `pwrd` (see `SenseRing`).
    sense_ring: bool,
    /// The AP powers the serializer (MCA) leaves.
    ap_mca: bool,
    /// The AP powers the microphone array's LEAP leaves. Without it the
    /// firmware raises them with the service's `pw0 `, and the DT's "hpai"
    /// DMA channel alone says whether there is a microphone array.
    ap_leap: bool,
}

/// J873: one amplifier in one 32-bit slot, nothing sent back.
static HW_CFG_T8152: HwConfig = HwConfig {
    spkr_channels: 1,
    sense_channels: 2,
    sense_formats: FORMATS,
    sense_ring: false,
    ap_mca: true,
    ap_leap: true,
};

/// J813: four amplifiers (left woofer, left tweeter, right woofer, right
/// tweeter) in 32-bit slots 0-3, their voltage and current sense back as
/// eight 16-bit slots (V, I per amplifier) on the stream's RX (base-ns RX0).
static HW_CFG_T8142: HwConfig = HwConfig {
    spkr_channels: 4,
    sense_channels: 8,
    sense_formats: FMTBIT_S16_LE,
    sense_ring: true,
    ap_mca: true,
    ap_leap: true,
};

/// J714s / J716c: six amplifiers in 32-bit slots 0-5, their voltage and
/// current sense back as twelve 16-bit slots on the stream's RX, as on J813.
/// The MCA and LEAP leaves have no AP power state: the firmware powers them.
static HW_CFG_T6050: HwConfig = HwConfig {
    spkr_channels: 6,
    sense_channels: 12,
    sense_formats: FMTBIT_S16_LE,
    sense_ring: true,
    ap_mca: false,
    ap_leap: false,
};

/// J815 (MacBook Air 15-inch, M5): the same T8142 speaker stream with six
/// amplifiers (ADT audio-speaker reg: 6 slots, mask 0x3f; 12 sense words,
/// mask 0xfff), in ADT speaker-config order: left woofer 1, right woofer 1,
/// left tweeter, right tweeter, left woofer 2, right woofer 2. Selected by
/// `apple,speaker-amps = <6>` on the audio node.
static HW_CFG_T8142_6AMP: HwConfig = HwConfig {
    spkr_channels: 6,
    sense_channels: 12,
    sense_formats: FMTBIT_S16_LE,
    sense_ring: true,
    ap_mca: true,
    ap_leap: true,
};

/// # Safety
///
/// `substream` must be live and belong to one of this component's links.
unsafe fn substream_rtd(
    substream: *mut bindings::snd_pcm_substream,
) -> *mut bindings::snd_soc_pcm_runtime {
    // SAFETY: by the caller's contract.
    unsafe { (*substream).private_data.cast() }
}

/// # Safety
///
/// `rtd` must be a live ASoC runtime.
unsafe fn rtd_is_be(rtd: *mut bindings::snd_soc_pcm_runtime) -> bool {
    // SAFETY: by the caller's contract.
    unsafe { (*(*rtd).dai_link).no_pcm() != 0 }
}

/// # Safety
///
/// `rtd` must be a live ASoC runtime of one of this component's links.
unsafe fn fe_index(rtd: *mut bindings::snd_soc_pcm_runtime) -> Option<usize> {
    // SAFETY: by the caller's contract.
    let id = unsafe { (**(*rtd).dais).id } as u32;
    if (FE_DAI_ID_BASE..FE_DAI_ID_BASE + FE_COUNT as u32).contains(&id) {
        Some((id - FE_DAI_ID_BASE) as usize)
    } else {
        None
    }
}

/// # Safety
///
/// `substream` must be live.
unsafe fn is_playback(substream: *mut bindings::snd_pcm_substream) -> bool {
    // SAFETY: by the caller's contract.
    unsafe { (*substream).stream == bindings::SNDRV_PCM_STREAM_PLAYBACK as i32 }
}

/// The context behind the component's descriptor (its first field).
#[repr(C)]
struct AsocContext {
    component: bindings::snd_soc_component_driver,
    data: Arc<T8152AopAudio>,
    dais: KBox<Opaque<[bindings::snd_soc_dai_driver; DAI_COUNT]>>,
    _ops: KBox<DaiOps>,
}

const _: () = assert!(mem::offset_of!(AsocContext, component) == 0);

/// # Safety
///
/// `component` must be this driver's live registered component.
unsafe fn component_data<'a>(component: *mut bindings::snd_soc_component) -> &'a T8152AopAudio {
    // SAFETY: by the caller's contract; C keeps the descriptor pointer we
    // registered, which is the start of the `AsocContext`.
    unsafe { &(*(*component).driver.cast::<AsocContext>()).data }
}

/// # Safety
///
/// `dai` must be a live DAI of this driver's registered component.
unsafe fn dai_data<'a>(dai: *mut bindings::snd_soc_dai) -> &'a T8152AopAudio {
    // SAFETY: by the caller's contract.
    unsafe { component_data((*dai).component) }
}

/// The dmaengine PCMs' common description: mmap-able, interleaved, periods
/// from 256 bytes to the ADMAC's segment size. Rates, channels and formats
/// are left to the caller (for a front-end, ASoC fills them in from the DAI).
fn pcm_hw() -> bindings::snd_pcm_hardware {
    bindings::snd_pcm_hardware {
        info: bindings::SNDRV_PCM_INFO_MMAP
            | bindings::SNDRV_PCM_INFO_MMAP_VALID
            | bindings::SNDRV_PCM_INFO_INTERLEAVED,
        buffer_bytes_max: PCM_BUFFER_MAX,
        period_bytes_min: PCM_PERIOD_BYTES_MIN,
        period_bytes_max: PCM_PERIOD_BYTES_MAX,
        periods_min: 2,
        periods_max: (PCM_BUFFER_MAX / PCM_PERIOD_BYTES_MIN) as u32,
        fifo_size: 16,
        ..Default::default()
    }
}

/// Sets up a dmaengine substream on `chan`: a managed buffer on the DMA
/// device (once per substream), `hw` refined by the channel and narrowed to
/// `formats`, then the dmaengine runtime. On failure the channel is still
/// the caller's.
///
/// # Safety
///
/// `substream` must be live and `chan` a channel the caller requested.
unsafe fn dmaengine_open(
    substream: *mut bindings::snd_pcm_substream,
    chan: *mut bindings::dma_chan,
    mut hw: bindings::snd_pcm_hardware,
    formats: u64,
) -> i32 {
    // SAFETY: by the caller's contract.
    unsafe {
        let mut ret = 0;
        if (*substream).dma_buffer.dev.type_ == bindings::SNDRV_DMA_TYPE_UNKNOWN as i32 {
            ret = bindings::snd_pcm_set_managed_buffer(
                substream,
                bindings::SNDRV_DMA_TYPE_DEV as i32,
                (*(*chan).device).dev,
                0,
                0,
            );
        }
        if ret == 0 {
            let mut dma_data = bindings::snd_dmaengine_dai_dma_data::default();
            ret = bindings::snd_dmaengine_pcm_refine_runtime_hwparams(
                substream,
                &mut dma_data,
                &mut hw,
                chan,
            );
        }
        if ret == 0 {
            hw.formats &= formats;
            (*(*substream).runtime).hw = hw;
            ret = bindings::snd_dmaengine_pcm_open(substream, chan);
        }
        ret
    }
}

/// The sense ring behind a front-end substream, `None` for every other stream.
///
/// # Safety
///
/// `component` must be this driver's live registered component and `substream`
/// a live substream of one of its links.
unsafe fn sense_ring<'a>(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> Option<&'a SenseRing> {
    // SAFETY: by the caller's contract.
    let (data, rtd) = unsafe { (component_data(component), substream_rtd(substream)) };
    // SAFETY: as above.
    if unsafe { rtd_is_be(rtd) || is_playback(substream) } {
        return None;
    }
    // SAFETY: as above.
    if unsafe { fe_index(rtd) } != Some(2) {
        return None;
    }
    data.sense_ring.as_ref()
}

/// Hardware description for the VMALLOC sense front-end: require at least one
/// ring period per ALSA period so even the minimum two-period buffer holds
/// incoming ring batches without XRUNning on the first callback.
fn sense_pcm_hw(formats: u64, ring_period_bytes: usize) -> bindings::snd_pcm_hardware {
    let mut hw = pcm_hw();
    hw.info |= bindings::SNDRV_PCM_INFO_BLOCK_TRANSFER | bindings::SNDRV_PCM_INFO_BATCH;
    hw.formats = formats;
    hw.period_bytes_min = hw.period_bytes_min.max(ring_period_bytes);
    hw.periods_max = (hw.buffer_bytes_max / hw.period_bytes_min) as u32;
    hw
}

/// The sense front-end's buffer: plain memory the ring callback copies whole
/// frames into.
fn sense_open(
    substream: *mut bindings::snd_pcm_substream,
    formats: u64,
    ring_period_bytes: usize,
) -> i32 {
    // SAFETY: a live substream in its open.
    unsafe {
        if (*substream).dma_buffer.dev.type_ == bindings::SNDRV_DMA_TYPE_UNKNOWN as i32 {
            let ret = bindings::snd_pcm_set_managed_buffer(
                substream,
                bindings::SNDRV_DMA_TYPE_VMALLOC as i32,
                ptr::null_mut(),
                0,
                0,
            );
            if ret < 0 {
                return ret;
            }
        }
        (*(*substream).runtime).hw = sense_pcm_hw(formats, ring_period_bytes);
    }
    0
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_open(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: by the caller's contract.
    let data = unsafe { component_data(component) };
    // SAFETY: as above.
    let rtd = unsafe { substream_rtd(substream) };
    // SAFETY: as above.
    if unsafe { rtd_is_be(rtd) } {
        return 0;
    }
    // SAFETY: as above.
    let Some(fe) = (unsafe { fe_index(rtd) }) else {
        return EINVAL.to_errno();
    };
    // SAFETY: as above.
    let playback = unsafe { is_playback(substream) };
    let Some(name) = FE_DMA_NAMES[fe][usize::from(!playback)] else {
        return ENODEV.to_errno();
    };
    if fe == 2 && !playback {
        if let Some(ring) = &data.sense_ring {
            let ret = sense_open(substream, data.cfg.sense_formats, ring.period_bytes());
            if ret < 0 {
                return ret;
            }
            ring.attach(substream);
            return 0;
        }
    }
    // SAFETY: the device is live and the name is NUL-terminated.
    let chan = match from_err_ptr(unsafe {
        bindings::dma_request_chan(data.dev.as_raw(), name.as_char_ptr())
    }) {
        Ok(chan) => chan,
        Err(e) => {
            dev_err!(data.dev, "DMA channel {:?} unavailable: {:?}\n", name, e);
            return e.to_errno();
        }
    };
    let formats = if fe == 2 { data.cfg.sense_formats } else { FORMATS };
    // SAFETY: the substream is live and the channel was requested above.
    let ret = unsafe { dmaengine_open(substream, chan, pcm_hw(), formats) };
    if ret < 0 {
        // SAFETY: the open failed, so the channel is still ours.
        unsafe { bindings::dma_release_channel(chan) };
    } else if fe == 1 && playback && data.sense_ring.is_some() {
        // The playback slots line up only when prepare starts a fresh
        // serializer: a paused DMA restarted mid-frame would shift them.
        // SAFETY: `dmaengine_open` initialized the runtime above.
        unsafe {
            (*(*substream).runtime).hw.info &=
                !(bindings::SNDRV_PCM_INFO_PAUSE | bindings::SNDRV_PCM_INFO_RESUME);
        }
    }
    ret
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_close(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // SAFETY: as above.
    if let Some(ring) = unsafe { sense_ring(component, substream) } {
        // SAFETY: the attached sense front-end, in its close.
        unsafe { ring.detach(substream) };
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_close_release_chan(substream) }
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_hw_params(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    params: *mut bindings::snd_pcm_hw_params,
) -> i32 {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) || sense_ring(component, substream).is_some() }
    {
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    let chan = unsafe { bindings::snd_dmaengine_pcm_get_chan(substream) };
    let mut cfg = bindings::dma_slave_config::default();
    // SAFETY: the substream and the parameters are live for the op.
    let rc = unsafe { bindings::snd_hwparams_to_dma_slave_config(substream, params, &mut cfg) };
    if rc < 0 {
        return rc;
    }
    // SAFETY: ASoC calls hw_params with live parameters.
    let channels = unsafe {
        (*params).intervals[(bindings::SNDRV_PCM_HW_PARAM_CHANNELS
            - bindings::SNDRV_PCM_HW_PARAM_FIRST_INTERVAL) as usize]
            .min
    };
    // One frame per burst, up to four words.
    let window = core::cmp::min(channels, 4);
    // SAFETY: the substream is live.
    if unsafe { is_playback(substream) } {
        cfg.dst_port_window_size = window;
    } else {
        cfg.src_port_window_size = window;
    }
    // SAFETY: `chan` is the live channel behind the substream.
    unsafe {
        match (*(*chan).device).device_config {
            Some(f) => f(chan, &mut cfg),
            None => ENOSYS.to_errno(),
        }
    }
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_trigger(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
) -> i32 {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // The sense attaches to the ring at its next whole frame; ALSA holds the
    // stream lock, which the ring callback takes too.
    // SAFETY: as above.
    if let Some(ring) = unsafe { sense_ring(component, substream) } {
        match cmd as u32 {
            bindings::SNDRV_PCM_TRIGGER_START => {
                ring.fe_pos.store(0, Ordering::Relaxed);
                ring.fe_running.store(true, Ordering::Relaxed);
            }
            bindings::SNDRV_PCM_TRIGGER_RESUME | bindings::SNDRV_PCM_TRIGGER_PAUSE_RELEASE => {
                ring.fe_running.store(true, Ordering::Relaxed);
            }
            bindings::SNDRV_PCM_TRIGGER_STOP
            | bindings::SNDRV_PCM_TRIGGER_SUSPEND
            | bindings::SNDRV_PCM_TRIGGER_PAUSE_PUSH => {
                ring.fe_running.store(false, Ordering::Relaxed);
            }
            _ => return EINVAL.to_errno(),
        }
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_trigger(substream, cmd) }
}

/// Wait for asynchronous DMA termination before ALSA reuses a front-end's buffer.
///
/// # Safety
///
/// Called by ASoC on this driver's component with a live substream.
unsafe extern "C" fn asoc_pcm_sync_stop(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) || sense_ring(component, substream).is_some() }
    {
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_sync_stop(substream) }
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_pointer(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return (-i64::from(bindings::ENOTSUPP)) as bindings::snd_pcm_uframes_t;
    }
    // SAFETY: as above; ALSA calls this under the stream lock with a live runtime.
    if let Some(ring) = unsafe { sense_ring(component, substream) } {
        // SAFETY: as above.
        let frames = unsafe { (*(*substream).runtime).buffer_size } as usize;
        return (ring.fe_pos.load(Ordering::Relaxed) % frames.max(1))
            as bindings::snd_pcm_uframes_t;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_pointer(substream) }
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component.
unsafe extern "C" fn dai_accept_fmt(_dai: *mut bindings::snd_soc_dai, _fmt: u32) -> i32 {
    // The firmware runs the serializers; format, slots and clocks are its.
    0
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component.
unsafe extern "C" fn dai_accept_sysclk(
    _dai: *mut bindings::snd_soc_dai,
    _id: i32,
    _freq: u32,
    _dir: i32,
) -> i32 {
    0
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component.
unsafe extern "C" fn dai_accept_bclk_ratio(_dai: *mut bindings::snd_soc_dai, _ratio: u32) -> i32 {
    0
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component.
unsafe extern "C" fn dai_accept_tdm_slot(
    _dai: *mut bindings::snd_soc_dai,
    _tx_mask: u32,
    _rx_mask: u32,
    _slots: i32,
    _slot_width: i32,
) -> i32 {
    0
}

/// The back-end's service for a stream; `None` for the speaker's sense
/// capture, which runs with the speaker's playback.
///
/// # Safety
///
/// `dai` must be a live DAI of this component, `substream` live.
unsafe fn be_service<'a>(
    dai: *mut bindings::snd_soc_dai,
    substream: *mut bindings::snd_pcm_substream,
) -> Option<(&'a T8152AopAudio, &'a Service)> {
    // SAFETY: by the caller's contract.
    let (data, id, playback) = unsafe { (dai_data(dai), (*dai).id as u32, is_playback(substream)) };
    let svc = match (id, playback) {
        (BE_DAI_ID_COUT, true) => &data.cout,
        (BE_DAI_ID_COUT, false) => &data.cin,
        (BE_DAI_ID_SPKR, true) => &data.spkr,
        _ => return None,
    };
    Some((data, svc))
}

/// Mutes a back-end's codecs (the amps); `__soc_pcm_prepare` unmutes them right
/// after `be_prepare`.
///
/// # Safety
///
/// `rtd` must be a live back-end runtime, in its prepare.
unsafe fn mute_codecs(rtd: *mut bindings::snd_soc_pcm_runtime) -> Result<()> {
    // SAFETY: by the caller's contract; the codec DAIs follow the CPU DAIs in `dais`.
    unsafe {
        let link = (*rtd).dai_link;
        let cpus = (*link).num_cpus as usize;
        for i in 0..(*link).num_codecs as usize {
            let ret = bindings::snd_soc_dai_digital_mute(
                *(*rtd).dais.add(cpus + i),
                1,
                bindings::SNDRV_PCM_STREAM_PLAYBACK as i32,
            );
            if ret < 0 {
                return Err(Error::from_errno(ret));
            }
        }
    }
    Ok(())
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_startup(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    let Some((data, svc)) = (unsafe { be_service(dai, substream) }) else {
        // SAFETY: as above.
        let (data, id) = unsafe { (dai_data(dai), (*dai).id as u32) };
        if id == BE_DAI_ID_SPKR {
            return match data.sense_startup() {
                Ok(()) => 0,
                Err(e) => e.to_errno(),
            };
        }
        return 0;
    };
    let res = if ptr::eq(svc, &data.spkr) {
        data.spkr_startup()
    } else {
        data.startup(svc)
    };
    match res {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_prepare(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    let Some((data, svc)) = (unsafe { be_service(dai, substream) }) else {
        return 0;
    };
    let res = if ptr::eq(svc, &data.spkr) {
        // SAFETY: the back-end runtime of this substream is live for the call.
        data.spkr_prepare(|| unsafe { mute_codecs(substream_rtd(substream)) })
    } else {
        data.prepare(svc)
    };
    match res {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_hw_free(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    if let Some((data, svc)) = unsafe { be_service(dai, substream) } {
        if ptr::eq(svc, &data.spkr) {
            if let Err(e) = data.spkr_hw_free() {
                return e.to_errno();
            }
        }
    }
    0
}

/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_shutdown(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) {
    // SAFETY: by the caller's contract.
    if let Some((data, svc)) = unsafe { be_service(dai, substream) } {
        if ptr::eq(svc, &data.spkr) {
            data.spkr_shutdown();
        } else {
            data.shutdown(svc);
        }
    }
}

/// # Safety
///
/// Called by ASoC on this driver's registered component.
unsafe extern "C" fn asoc_suspend(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    match unsafe { component_data(component) }.spkr_suspend() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
///
/// Called by ASoC on this driver's registered component.
unsafe extern "C" fn asoc_resume(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    match unsafe { component_data(component) }.spkr_resume() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

struct DaiOps {
    be: bindings::snd_soc_dai_ops,
    fe: bindings::snd_soc_dai_ops,
}

impl DaiOps {
    fn new() -> Self {
        let fe = bindings::snd_soc_dai_ops {
            set_fmt: Some(dai_accept_fmt),
            set_sysclk: Some(dai_accept_sysclk),
            set_bclk_ratio: Some(dai_accept_bclk_ratio),
            set_tdm_slot: Some(dai_accept_tdm_slot),
            ..Default::default()
        };
        DaiOps {
            be: bindings::snd_soc_dai_ops {
                startup: Some(be_startup),
                prepare: Some(be_prepare),
                hw_free: Some(be_hw_free),
                shutdown: Some(be_shutdown),
                ..fe
            },
            fe,
        }
    }
}

fn pcm_stream(name: &'static CStr, channels: u32) -> bindings::snd_soc_pcm_stream {
    pcm_stream_fmt(name, channels, FORMATS)
}

fn pcm_stream_fmt(
    name: &'static CStr,
    channels: u32,
    formats: u64,
) -> bindings::snd_soc_pcm_stream {
    bindings::snd_soc_pcm_stream {
        stream_name: name.as_char_ptr(),
        formats,
        rates: bindings::SNDRV_PCM_RATE_48000,
        rate_min: 48000,
        rate_max: 48000,
        channels_min: channels,
        channels_max: channels,
        ..Default::default()
    }
}

/// The DAI table, indexed by `sound-dai = <&aop_audio N>` in the DTS:
/// 0 cout (jack back-end), 1 and 2 the playback front-ends, 3 spkr (speaker
/// back-end), 4 the sense front-end.
const DAI_COUNT: usize = 2 + FE_COUNT;

fn dai_drivers(ops: &DaiOps, cfg: &HwConfig) -> [bindings::snd_soc_dai_driver; DAI_COUNT] {
    [
        bindings::snd_soc_dai_driver {
            name: c"cout".as_char_ptr(),
            id: BE_DAI_ID_COUT,
            ops: &ops.be,
            playback: pcm_stream(c"COUT TX", 2),
            capture: pcm_stream(c"CIN RX", 1),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c"j873-pcm-0".as_char_ptr(),
            id: FE_DAI_ID_BASE,
            ops: &ops.fe,
            playback: pcm_stream(c"PCM0 TX", 2),
            capture: pcm_stream(c"PCM0 RX", 1),
            ..Default::default()
        },
        // The speaker: one 32-bit slot per amplifier.
        bindings::snd_soc_dai_driver {
            name: c"j873-pcm-1".as_char_ptr(),
            id: FE_DAI_ID_BASE + 1,
            ops: &ops.fe,
            playback: pcm_stream(c"PCM1 TX", cfg.spkr_channels),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c"spkr".as_char_ptr(),
            id: BE_DAI_ID_SPKR,
            ops: &ops.be,
            playback: pcm_stream(c"SPKR TX", cfg.spkr_channels),
            capture: pcm_stream_fmt(c"SPKR RX", cfg.sense_channels, cfg.sense_formats),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c"j873-pcm-2".as_char_ptr(),
            id: FE_DAI_ID_BASE + 2,
            ops: &ops.fe,
            capture: pcm_stream_fmt(c"PCM2 RX", cfg.sense_channels, cfg.sense_formats),
            ..Default::default()
        },
    ]
}

impl AsocContext {
    fn new(data: Arc<T8152AopAudio>) -> Result<KBox<Self>> {
        let ops = KBox::new(DaiOps::new(), GFP_KERNEL)?;
        let dais = KBox::new(Opaque::new(dai_drivers(&ops, data.cfg)), GFP_KERNEL)?;
        let component = bindings::snd_soc_component_driver {
            name: c"t8152-aop-audio".as_char_ptr(),
            suspend: Some(asoc_suspend),
            resume: Some(asoc_resume),
            open: Some(asoc_pcm_open),
            close: Some(asoc_pcm_close),
            hw_params: Some(asoc_pcm_hw_params),
            trigger: Some(asoc_pcm_trigger),
            sync_stop: Some(asoc_pcm_sync_stop),
            pointer: Some(asoc_pcm_pointer),
            ..Default::default()
        };
        Ok(KBox::new(
            Self {
                component,
                data,
                dais,
                _ops: ops,
            },
            GFP_KERNEL,
        )?)
    }
}

// ---- the microphone array's card ----

/// # Safety
///
/// `substream` must be a live substream of the microphone PCM, whose private
/// data is the `Arc` leaked into it by `MicCard::new` (released in
/// `mic_pcm_free` after the last op).
unsafe fn mic_data<'a>(substream: *mut bindings::snd_pcm_substream) -> ArcBorrow<'a, T8152AopAudio> {
    // SAFETY: by the caller's contract.
    unsafe { Arc::<T8152AopAudio>::borrow((*(*substream).pcm).private_data.cast()) }
}

/// The LEAP produces float32 into the channel requested here; the channel's
/// registers only answer with the LEAP leaves up, so the service (and with
/// it the leaves) comes up before the request and goes down after the
/// release.
///
/// # Safety
///
/// Called by ALSA with a live substream of the microphone PCM.
unsafe extern "C" fn mic_pcm_open(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: by the caller's contract.
    let data = unsafe { mic_data(substream) };
    // A failed startup has undone its leaves; the service stays in `pw0 `.
    if let Err(e) = data.hpai_startup() {
        return e.to_errno();
    }
    // SAFETY: the device is live and the name is NUL-terminated.
    let chan = match from_err_ptr(unsafe {
        bindings::dma_request_chan(data.dev.as_raw(), c"hpai".as_char_ptr())
    }) {
        Ok(chan) => chan,
        Err(e) => {
            dev_err!(data.dev, "hpai: DMA channel unavailable: {:?}\n", e);
            data.hpai_shutdown();
            return e.to_errno();
        }
    };
    let hw = bindings::snd_pcm_hardware {
        rates: bindings::SNDRV_PCM_RATE_48000,
        rate_min: HPAI_RATE,
        rate_max: HPAI_RATE,
        channels_min: HPAI_CHANNELS,
        channels_max: HPAI_CHANNELS,
        ..pcm_hw()
    };
    // SAFETY: the substream is live and the channel was requested above.
    let ret = unsafe {
        dmaengine_open(substream, chan, hw, bindings::BINDINGS_SNDRV_PCM_FMTBIT_FLOAT_LE)
    };
    if ret < 0 {
        // SAFETY: the open failed, so the channel is still ours.
        unsafe { bindings::dma_release_channel(chan) };
        data.hpai_shutdown();
    }
    ret
}

/// # Safety
///
/// Called by ALSA with a live substream of the microphone PCM.
unsafe extern "C" fn mic_pcm_close(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: by the caller's contract.
    let data = unsafe { mic_data(substream) };
    data.flush_hpai_start();
    // SAFETY: the dmaengine runtime was set up in `mic_pcm_open`.
    let ret = unsafe { bindings::snd_dmaengine_pcm_close_release_chan(substream) };
    data.hpai_shutdown();
    ret
}

/// Finish queued firmware work and asynchronous DMA termination before ALSA
/// reuses or frees the capture buffer, as in the T8140 capture path.
///
/// # Safety
///
/// Called by ALSA with a live microphone substream and dmaengine runtime.
unsafe extern "C" fn mic_pcm_sync_stop(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: by the caller's contract.
    let data = unsafe { mic_data(substream) };
    data.flush_hpai_start();
    // SAFETY: the runtime remains live until close.
    unsafe { bindings::snd_dmaengine_pcm_sync_stop(substream) }
}

/// # Safety
///
/// Called by ALSA with a live substream of the microphone PCM and live
/// parameters.
unsafe extern "C" fn mic_pcm_hw_params(
    substream: *mut bindings::snd_pcm_substream,
    params: *mut bindings::snd_pcm_hw_params,
) -> i32 {
    // SAFETY: the dmaengine runtime was set up in `mic_pcm_open`.
    let chan = unsafe { bindings::snd_dmaengine_pcm_get_chan(substream) };
    let mut cfg = bindings::dma_slave_config::default();
    // SAFETY: the substream and the parameters are live for the op.
    let rc = unsafe { bindings::snd_hwparams_to_dma_slave_config(substream, params, &mut cfg) };
    if rc < 0 {
        return rc;
    }
    // Three words per frame, which the ADMAC cannot mark (it frames 2 or 4):
    // a plain word stream.
    // SAFETY: `chan` is the live channel behind the substream.
    unsafe {
        match (*(*chan).device).device_config {
            Some(f) => f(chan, &mut cfg),
            None => ENOSYS.to_errno(),
        }
    }
}

/// ALSA calls prepare unconditionally. Nothing to arm: the DMA start and
/// the queued firmware run belong to trigger.
///
/// # Safety
///
/// Called by ALSA with a live substream of the microphone PCM.
unsafe extern "C" fn mic_pcm_prepare(_substream: *mut bindings::snd_pcm_substream) -> i32 {
    0
}

/// With the stream lock held: the DMA first, then the firmware's `pwrd`
/// queued behind it.
///
/// # Safety
///
/// Called by ALSA with a live substream of the microphone PCM.
unsafe extern "C" fn mic_pcm_trigger(substream: *mut bindings::snd_pcm_substream, cmd: i32) -> i32 {
    // SAFETY: by the caller's contract.
    let data = unsafe { mic_data(substream) };
    // SAFETY: the dmaengine runtime was set up in `mic_pcm_open`.
    let ret = unsafe { bindings::snd_dmaengine_pcm_trigger(substream, cmd) };
    if ret < 0 {
        return ret;
    }
    match cmd as u32 {
        bindings::SNDRV_PCM_TRIGGER_START
        | bindings::SNDRV_PCM_TRIGGER_RESUME
        | bindings::SNDRV_PCM_TRIGGER_PAUSE_RELEASE => {
            data.hpai_want_run.store(true, Relaxed);
            // Err: already queued, and that run sees `hpai_want_run`.
            let _ = workqueue::system().enqueue::<_, HPAI_START_WORK>(Arc::from(data));
        }
        _ => data.hpai_want_run.store(false, Relaxed),
    }
    0
}

/// # Safety
///
/// Called once by ALSA when the PCM goes away.
unsafe extern "C" fn mic_pcm_free(pcm: *mut bindings::snd_pcm) {
    // SAFETY: the `Arc` leaked into the private data by `MicCard::new`.
    drop(unsafe { Arc::<T8152AopAudio>::from_foreign((*pcm).private_data.cast()) });
}

static MIC_PCM_OPS: bindings::snd_pcm_ops = bindings::snd_pcm_ops {
    open: Some(mic_pcm_open),
    close: Some(mic_pcm_close),
    ioctl: None,
    hw_params: Some(mic_pcm_hw_params),
    hw_free: None,
    prepare: Some(mic_pcm_prepare),
    trigger: Some(mic_pcm_trigger),
    sync_stop: Some(mic_pcm_sync_stop),
    pointer: Some(bindings::snd_dmaengine_pcm_pointer),
    get_time_info: None,
    fill_silence: None,
    copy: None,
    page: None,
    mmap: None,
    ack: None,
};

/// Copies a NUL-terminated string into a fixed-size field, truncated to fit.
fn copy_str(target: &mut [u8], source: &CStr) {
    let b = source.to_bytes();
    let n = b.len().min(target.len() - 1);
    target[..n].copy_from_slice(&b[..n]);
    target[n] = 0;
}

/// The microphone array's card: one capture PCM, named like the other AOP
/// microphone cards (AppleJxxxHPAI) so that the UCM and DSP configurations
/// for those find it.
struct MicCard(*mut bindings::snd_card);

impl MicCard {
    fn new(data: &Arc<T8152AopAudio>) -> Result<Self> {
        let fwnode = data.dev.fwnode().ok_or(ENODEV)?;
        let chassis = fwnode
            .property_read::<CString>(c_str!("apple,chassis-name"))
            .required_by(&data.dev)?;
        let kind = fwnode
            .property_read::<CString>(c_str!("apple,machine-kind"))
            .required_by(&data.dev)?;
        let id = CString::try_from_fmt(fmt!("Apple{}HPAI", *chassis))?;
        let shortname = CString::try_from_fmt(fmt!("{} {} HPAI", *kind, *chassis))?;
        let longname =
            CString::try_from_fmt(fmt!("{} {} High-Power Audio Interface", *kind, *chassis))?;
        let mut card = ptr::null_mut();
        // SAFETY: the device and the module are live; `card` receives the card.
        let ret = unsafe {
            bindings::snd_card_new(
                data.dev.as_raw(),
                -1,
                ptr::null(),
                kernel::module::this_module::<crate::LocalModule>().as_ptr(),
                0,
                &mut card,
            )
        };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        // From here on, dropping `this` frees the card.
        let this = MicCard(card);
        // SAFETY: the card is new and unregistered.
        unsafe {
            copy_str(&mut (*card).driver, c"aop_audio");
            copy_str(&mut (*card).id, &id);
            copy_str(&mut (*card).shortname, &shortname);
            copy_str(&mut (*card).longname, &longname);
        }
        let mut pcm = ptr::null_mut();
        // SAFETY: the card is live; `pcm` receives the PCM.
        let ret = unsafe { bindings::snd_pcm_new(card, c"hpai".as_char_ptr(), 0, 0, 1, &mut pcm) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        // SAFETY: `pcm` is live; the ops are static and the private data is
        // an `Arc` that `mic_pcm_free` releases.
        unsafe {
            bindings::snd_pcm_set_ops(pcm, bindings::SNDRV_PCM_STREAM_CAPTURE as i32, &MIC_PCM_OPS);
            (*pcm).private_data = data.clone().into_foreign().cast();
            (*pcm).private_free = Some(mic_pcm_free);
            copy_str(&mut (*pcm).name, c"Microphone Array");
        }
        // SAFETY: the card is live and complete.
        let ret = unsafe { bindings::snd_card_register(card) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(this)
    }
}

impl Drop for MicCard {
    fn drop(&mut self) {
        // SAFETY: created by `snd_card_new`; this waits for open files.
        unsafe { bindings::snd_card_free(self.0) };
    }
}

struct T8152AopAudioDriver {
    dev: ARef<device::Device>,
    asoc: KBox<AsocContext>,
    registered: AtomicFlag,
    /// Held for its drop, which frees the card after unbind.
    _mic: Option<MicCard>,
}

// SAFETY: the ASoC context and the card are only touched by ALSA/ASoC
// callbacks and by probe and remove, which the driver core serializes.
unsafe impl Send for T8152AopAudioDriver {}
// SAFETY: as for `Send`.
unsafe impl Sync for T8152AopAudioDriver {}

impl T8152AopAudioDriver {
    /// Unregisters the component. Must run from unbind: the component is
    /// device-managed and freed with the device's resources after unbind.
    fn unregister(&self) {
        if !self.registered.xchg(false, Relaxed) {
            return;
        }
        // SAFETY: registered in probe; the context outlives the unregister.
        unsafe {
            bindings::snd_soc_unregister_component_by_driver(
                self.dev.as_raw(),
                &self.asoc.component,
            )
        };
    }
}

kernel::of_device_table!(
    OF_TABLE,
    <T8152AopAudioDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c_str!("apple,t6050-aop-audio")), &HW_CFG_T6050),
        (of::DeviceId::new(c_str!("apple,t8142-aop-audio")), &HW_CFG_T8142),
        (of::DeviceId::new(c_str!("apple,t8152-aop-audio")), &HW_CFG_T8152),
    ]
);

impl platform::Driver for T8152AopAudioDriver {
    type IdInfo = &'static HwConfig;
    type Data<'bound> = Self;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let dev = ARef::<device::Device>::from(pdev.as_ref());
        let mut cfg: &'static HwConfig = info.copied().unwrap_or(&HW_CFG_T8152);
        // A machine whose speaker stream has more amplifiers than its SoC's
        // default says so in its DT. Only shapes this driver knows are
        // accepted: the firmware's stream must match the PCM exactly.
        if let Some(amps) = dev
            .fwnode()
            .and_then(|n| n.property_read::<u32>(c_str!("apple,speaker-amps")).optional())
        {
            cfg = match (amps, ptr::eq(cfg, &HW_CFG_T8142)) {
                (4, true) => &HW_CFG_T8142,
                (6, true) => &HW_CFG_T8142_6AMP,
                _ => {
                    dev_err!(dev, "unsupported apple,speaker-amps {}\n", amps);
                    return Err(EINVAL);
                }
            };
        }
        // SAFETY: this is a service device the AOP core registered.
        let adata = unsafe { <dyn AOP>::from_child(pdev.as_ref()) }?;
        // SAFETY: the AOP core sets the child's platform data to its EPIC service.
        let service = unsafe { (*dev.as_raw()).platform_data as *const EPICService };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: checked above; it lives as long as the child.
        let service = unsafe { *service };

        // Where the AP powers them, every serializer leaf is required: the
        // firmware's streams run on them, so the driver must not bind
        // without one (or before its provider has probed).
        let mca_names: &[&CStr] = if cfg.ap_mca { &MCA_NAMES } else { &[] };
        let mut mca = KVec::new();
        for &name in mca_names {
            match PowerDomain::attach(&dev, name) {
                Ok(Some(pd)) => mca.push(pd, GFP_KERNEL)?,
                Ok(None) => {
                    dev_err!(dev, "power domain {:?} is missing\n", name);
                    return Err(ENODEV);
                }
                Err(e) => {
                    if e != EPROBE_DEFER {
                        dev_err!(dev, "power domain {:?}: {:?}\n", name, e);
                    }
                    return Err(e);
                }
            }
        }

        // The microphone array, where the DT describes it: where the AP powers
        // its leaves, all of them or none; elsewhere its DMA channel.
        let leap_names: &[&CStr] = if cfg.ap_leap { &LEAP_NAMES } else { &[] };
        let mut leap = KVec::new();
        for &name in leap_names {
            match PowerDomain::attach(&dev, name) {
                Ok(Some(pd)) => leap.push(pd, GFP_KERNEL)?,
                Ok(None) => {}
                Err(e) => return Err(e),
            }
        }
        if !leap.is_empty() && leap.len() != LEAP_NAMES.len() {
            dev_err!(dev, "incomplete microphone power domains\n");
            return Err(ENODEV);
        }
        let has_mic = if cfg.ap_leap {
            !leap.is_empty()
        } else {
            dev.fwnode()
                .is_some_and(|node| node.property_match_string(c"dma-names", c"hpai").is_ok())
        };

        let sense_ring = cfg
            .sense_ring
            .then(|| SenseRing::new(cfg.sense_channels as usize * 2));
        let data_dev = dev.clone();
        let data = Arc::pin_init(
            pin_init!(T8152AopAudio {
                dev: data_dev,
                adata,
                service,
                cout: Service::new(from_fourcc(b"cout"), "cout", true),
                cin: Service::new(from_fourcc(b"cin "), "cin", true),
                spkr: Service::new(from_fourcc(b"spkr"), "spkr", false),
                spkr_state <- new_mutex!(SpkrState::new()),
                sense_ring,
                mca,
                cfg,
                hpai: Service::new(from_fourcc(b"hpai"), "hpai", false),
                leap,
                hpai_want_run: AtomicFlag::new(false),
                hpai_start_work <- new_work!("T8152AopAudio::hpai_start_work"),
            }),
            GFP_KERNEL,
        )?;
        for svc in [&data.cout, &data.cin, &data.spkr] {
            data.attach(svc)?;
        }

        // The microphones are optional: the speakers and the jack stay
        // usable without them.
        let mic = if !has_mic {
            None
        } else {
            // Initialize the shared output service before exposing HPAI.
            // This is standby only: no DMA, running state or codec unmute.
            // No userspace stream can race this before card registration.
            match data.set_power(&data.cout, POWER_STATE_PW0)
                .and_then(|_| MicCard::new(&data))
            {
                Ok(card) => Some(card),
                Err(e) => {
                    dev_err!(dev, "microphone card: {:?}\n", e);
                    None
                }
            }
        };

        let asoc = AsocContext::new(data)?;
        // SAFETY: the descriptor and the DAI table are heap allocations owned
        // by `asoc`, which the driver data keeps until after unregister.
        let ret = unsafe {
            bindings::snd_soc_register_component_d(
                dev.as_raw(),
                &asoc.component,
                asoc.dais.get().cast(),
                DAI_COUNT as i32,
            )
        };
        if ret < 0 {
            dev_err!(dev, "unable to register the ASoC component: {}\n", ret);
            return Err(Error::from_errno(ret));
        }
        Ok(Self {
            dev,
            asoc,
            registered: AtomicFlag::new(true),
            _mic: mic,
        })
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
        this.unregister();
        if let Some(ring) = &this.asoc.data.sense_ring {
            ring.release();
        }
    }
}

module_platform_driver! {
    type: T8152AopAudioDriver,
    name: "snd_soc_t8152_aop_audio",
    description: "Apple T8152 AOP audio: headphone jack, speakers and microphones",
    license: "Dual MIT/GPL",
}

#[kunit_tests(snd_soc_apple_t8152_aop_audio)]
mod tests {
    use super::*;

    const fn frames_ns(frames: u64) -> i64 {
        (frames * NSEC_PER_SEC / SENSE_RATE) as i64
    }

    #[test]
    fn sense_ring_lap_detection() {
        let ring_frames = SENSE_RING_FRAMES as u64;
        let period_frames = SENSE_RING_PERIOD_FRAMES as u64;

        assert!(!sense_ring_may_lap(0));
        assert!(!sense_ring_may_lap(frames_ns(period_frames)));
        assert!(!sense_ring_may_lap(frames_ns(ring_frames / 2)));
        assert!(!sense_ring_may_lap(
            frames_ns(ring_frames - period_frames) - 1
        ));

        assert!(sense_ring_may_lap(frames_ns(ring_frames - period_frames)));
        assert!(sense_ring_may_lap(frames_ns(ring_frames)));
        assert!(sense_ring_may_lap(frames_ns(
            2 * ring_frames + period_frames
        )));
        assert!(sense_ring_may_lap(-1));
    }

    #[test]
    fn sense_ring_geometry_covers_8ch_and_12ch() {
        assert_eq!(SENSE_RING_NS, 80_000_000);
        assert_eq!(SENSE_RING_PERIOD_NS, 2_500_000);

        // T8142 4-amp (8ch S16_LE = 16 B/frame) and T8142 6-amp / T6050 (12ch S16_LE = 24 B/frame)
        for ch in [HW_CFG_T8142.sense_channels, HW_CFG_T6050.sense_channels] {
            let frame = ch as usize * 2;
            let ring = SenseRing::new(frame);
            assert_eq!(ring.period_bytes() % frame, 0);
            assert_eq!(ring.ring_bytes() % ring.period_bytes(), 0);
            // Whole multiples of the ADMAC RX FIFO threshold (0x18 * 2 = 48 B)
            assert_eq!(ring.period_bytes() % 48, 0);
        }
    }

    #[test]
    fn sense_ring_fe_space_guards_overrun() {
        // Every supported multi-amp geometry constrains ALSA negotiation so the
        // smallest admitted buffer (2 periods of period_bytes_min) exceeds one
        // 120-frame ring batch and cannot XRUN on an empty capture buffer.
        for cfg in [&HW_CFG_T8142, &HW_CFG_T8142_6AMP, &HW_CFG_T6050] {
            let frame = cfg.sense_channels as usize * 2;
            let ring = SenseRing::new(frame);
            let hw = sense_pcm_hw(cfg.sense_formats, ring.period_bytes());
            assert_eq!(hw.period_bytes_min, ring.period_bytes());
            assert!(hw.period_bytes_min <= hw.period_bytes_max);
            assert_eq!(hw.periods_min, 2);
            assert!(hw.periods_min <= hw.periods_max);
            let min_period_frames = hw.period_bytes_min / frame;
            let min_buffer_frames = hw.periods_min as usize * min_period_frames;
            assert_eq!(min_period_frames, SENSE_RING_PERIOD_FRAMES);
            assert_eq!(min_buffer_frames, 2 * SENSE_RING_PERIOD_FRAMES);
            assert!(sense_ring_has_fe_space(
                min_buffer_frames,
                0,
                0,
                SENSE_RING_PERIOD_FRAMES
            ));
            assert!(!sense_ring_has_fe_space(
                min_buffer_frames,
                SENSE_RING_PERIOD_FRAMES,
                0,
                SENSE_RING_PERIOD_FRAMES
            ));
        }

        // Exact full-buffer advance (e.g. 120 frames into a 120-frame buffer) wraps the
        // modulo pointer back to the same offset, so it must trigger XRUN; strictly less fits.
        assert!(!sense_ring_has_fe_space(120, 0, 0, 120));
        assert!(sense_ring_has_fe_space(121, 0, 0, 120));
        assert!(!sense_ring_has_fe_space(121, 1, 0, 120));
        assert!(!sense_ring_has_fe_space(121, 0, 1, 120));
        // Coalesced callbacks exceeding remaining space after unread + pending frames trigger XRUN
        assert!(sense_ring_has_fe_space(1024, 512, 120, 240));
        assert!(!sense_ring_has_fe_space(1024, 512, 240, 360));
        // Overflow in unread + pending + incoming fails closed
        assert!(!sense_ring_has_fe_space(1024, usize::MAX, 0, 1));
    }

    #[test]
    fn spkr_state_requires_verified_pw0_before_rearm() {
        use core::cell::Cell;

        let spkr_running = AtomicFlag::new(false);
        let ring_starts = Cell::new(0u32);
        let ring_stops = Cell::new(0u32);
        let pw0_calls = Cell::new(0u32);
        let quiesce_calls = Cell::new(0u32);

        let mut st = SpkrState {
            open: true,
            running: false,
            pw0_verified: true,
            resume_run: false,
        };

        // Initial prepare arms ring and enters running (`pw0_verified` clears).
        assert!(st
            .prepare_with_ring(
                &spkr_running,
                || {
                    quiesce_calls.set(quiesce_calls.get() + 1);
                    Ok(())
                },
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Ok(())
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            )
            .is_ok());
        assert!(st.running);
        assert!(!st.pw0_verified);
        assert!(spkr_running.load(Relaxed));
        assert_eq!(ring_starts.get(), 1);
        assert_eq!(pw0_calls.get(), 0);

        // If AOP rejects `pw0 ` on stop (`hw_free` / `suspend`), running stays
        // true, `pw0_verified` stays false, and DMA is not stopped.
        let stops_before = ring_stops.get();
        assert_eq!(
            st.stop(
                &spkr_running,
                true,
                || Err(EIO),
                || ring_stops.set(ring_stops.get() + 1),
            ),
            Err(EIO)
        );
        assert!(st.running);
        assert!(!st.pw0_verified);
        assert!(spkr_running.load(Relaxed));
        assert_eq!(ring_stops.get(), stops_before);

        // Next prepare fails closed if `pw0 ` still fails, without rearming RX.
        assert_eq!(
            st.prepare_with_ring(
                &spkr_running,
                || {
                    quiesce_calls.set(quiesce_calls.get() + 1);
                    Ok(())
                },
                || Err(EIO),
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            ),
            Err(EIO)
        );
        assert_eq!(quiesce_calls.get(), 1);
        assert_eq!(ring_starts.get(), 1);
        assert!(st.running);

        // Even if `resume_run` were set while `pw0 ` is unverified and `pw0 `
        // still fails on resume, resume fails closed without arming RX.
        st.resume_run = true;
        assert_eq!(
            st.resume_with_ring(
                &spkr_running,
                || Err(EIO),
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            ),
            Err(EIO)
        );
        assert_eq!(ring_starts.get(), 1);

        // Once `pw0 ` succeeds, prepare verifies it and rearms the ring cleanly.
        assert!(st
            .prepare_with_ring(
                &spkr_running,
                || {
                    quiesce_calls.set(quiesce_calls.get() + 1);
                    Ok(())
                },
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Ok(())
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            )
            .is_ok());
        assert_eq!(pw0_calls.get(), 1);
        assert_eq!(ring_starts.get(), 2);
        assert!(st.running);

        // Suspend with failed `pw0 ` stops RX and records restart intent so
        // resume must verify `pw0 ` before rearming.
        let stops_before = ring_stops.get();
        assert_eq!(
            st.suspend(
                &spkr_running,
                true,
                || Err(EIO),
                || ring_stops.set(ring_stops.get() + 1),
            ),
            Err(EIO)
        );
        assert_eq!(ring_stops.get(), stops_before + 1);
        assert!(st.resume_run);
        assert!(st.running);
        assert!(!st.pw0_verified);

        // Suspend with verified `pw0 ` stops DMA, then resume restarts cleanly
        // without an extra `pw0 ` call.
        assert!(st
            .suspend(
                &spkr_running,
                true,
                || Ok(()),
                || ring_stops.set(ring_stops.get() + 1),
            )
            .is_ok());
        assert!(!st.running);
        assert!(st.pw0_verified);
        assert!(st.resume_run);
        assert!(st
            .resume_with_ring(
                &spkr_running,
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Ok(())
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            )
            .is_ok());
        assert_eq!(pw0_calls.get(), 1);
        assert_eq!(ring_starts.get(), 3);
        assert!(st.running);

        // An unconfirmed `pwrd` transition clears `pw0_verified` so the next
        // prepare re-verifies `pw0 ` before arming RX again.
        assert!(st
            .stop(
                &spkr_running,
                true,
                || Ok(()),
                || ring_stops.set(ring_stops.get() + 1),
            )
            .is_ok());
        assert_eq!(
            st.prepare_with_ring(
                &spkr_running,
                || Ok(()),
                || Ok(()),
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Err(EIO),
            ),
            Err(EIO)
        );
        assert!(!st.running);
        assert!(!st.pw0_verified);
        assert_eq!(ring_starts.get(), 4);
        assert!(st
            .prepare_with_ring(
                &spkr_running,
                || {
                    quiesce_calls.set(quiesce_calls.get() + 1);
                    Ok(())
                },
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Ok(())
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || Ok(()),
            )
            .is_ok());
        assert_eq!(pw0_calls.get(), 2);
        assert_eq!(ring_starts.get(), 5);
    }

    #[test]
    fn spkr_state_suspend_pw0_failure_and_resume() {
        use core::cell::Cell;

        let spkr_running = AtomicFlag::new(true);
        let ring_stops = Cell::new(0u32);
        let ring_starts = Cell::new(0u32);
        let pw0_calls = Cell::new(0u32);
        let pwrd_calls = Cell::new(0u32);

        let mut st = SpkrState {
            open: true,
            running: true,
            pw0_verified: false,
            resume_run: false,
        };

        // Failed `pw0 ` during suspend still stops/synchronizes RX and records
        // restart intent so resume verifies `pw0 ` before rearming.
        assert_eq!(
            st.suspend(
                &spkr_running,
                true,
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Err(EIO)
                },
                || ring_stops.set(ring_stops.get() + 1),
            ),
            Err(EIO)
        );
        assert_eq!(pw0_calls.get(), 1);
        assert_eq!(ring_stops.get(), 1);
        assert!(st.running);
        assert!(!st.pw0_verified);
        assert!(st.resume_run);
        assert!(spkr_running.load(Relaxed));

        // Resume fails closed without rearming RX if retry `pw0 ` still fails.
        assert_eq!(
            st.resume_with_ring(
                &spkr_running,
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Err(EIO)
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || {
                    pwrd_calls.set(pwrd_calls.get() + 1);
                    Ok(())
                },
            ),
            Err(EIO)
        );
        assert_eq!(pw0_calls.get(), 2);
        assert_eq!(ring_starts.get(), 0);
        assert_eq!(pwrd_calls.get(), 0);
        assert!(st.running);
        assert!(!st.pw0_verified);
        assert!(!st.resume_run);
        assert!(spkr_running.load(Relaxed));

        // Another suspend with `pw0 ` failing still stops RX and records intent;
        // when retry `pw0 ` succeeds on resume, RX is cleanly rearmed before `pwrd`.
        assert_eq!(
            st.suspend(
                &spkr_running,
                true,
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Err(EIO)
                },
                || ring_stops.set(ring_stops.get() + 1),
            ),
            Err(EIO)
        );
        assert_eq!(pw0_calls.get(), 3);
        assert_eq!(ring_stops.get(), 2);
        assert!(st.resume_run);

        assert!(st
            .resume_with_ring(
                &spkr_running,
                || {
                    pw0_calls.set(pw0_calls.get() + 1);
                    Ok(())
                },
                || ring_stops.set(ring_stops.get() + 1),
                || {
                    ring_starts.set(ring_starts.get() + 1);
                    Ok(())
                },
                || {
                    pwrd_calls.set(pwrd_calls.get() + 1);
                    Ok(())
                },
            )
            .is_ok());
        assert_eq!(pw0_calls.get(), 4);
        assert_eq!(ring_stops.get(), 3);
        assert_eq!(ring_starts.get(), 1);
        assert_eq!(pwrd_calls.get(), 1);
        assert!(st.running);
        assert!(!st.pw0_verified);
        assert!(!st.resume_run);
        assert!(spkr_running.load(Relaxed));
    }
}
