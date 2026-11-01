// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J616s (T6040) internal speakers on the AOP `spkr` stream, and the headphone jack on its `cout`
//! and `cin ` streams.
//!
//! The AOP firmware runs MCA cluster 0 for this stream (`spS `, serializer `ms00`, `mck0` from the
//! 12 MHz crystal): a 250-bit frame at 48 kHz with six 32-bit playback slots (24 valid bits) and
//! twelve 16-bit V/I sense slots coming back on the same serializer. The base-ns ADMAC moves both:
//! channel 0 (TX0) the playback, channel 1 (RX0) the sense, through AOP DART
//! stream 8. The AP side only attaches the
//! service, requests its power states and runs the DMA; the six SN012776 amps are their own codec
//! driver's, linked to this component by macaudio's J616 variant.
//!
//! This is an ASoC component next to the microphone card of the parent module: DPCM front-end PCMs
//! on the ADMAC channels (`j616-pcm-0..2`, the names macaudio's front-end links bind to) and one
//! back-end DAI, `spkr`, for the stream. The DAI table order is ABI: `sound-dai = <&aop_audio 3>`
//! is `spkr`.
//!
//! Power, from aurora's J616s control test (`spkr` attached, `pw1 `, `pwrd` with output flag 2,
//! `pw1 `, `idle`, each confirmed through property 200): `pw1 ` powers the audio fabric, the ADMAC
//! included, and `pwrd` runs the serializer clocks. The ADMAC registers only answer with the fabric
//! powered, so the back-end startup raises `pw1 ` before a front-end requests its channel (DPCM
//! starts the back-end first), and every front-end holds its own reference until it has released
//! the channel (DPCM shuts the back-end down before it closes the front-end). `pwrd` is requested
//! in the back-end prepare, before the amps unmute, and dropped in hw_free, after they muted: the
//! clocks run whenever an amp may be active.
//!
//! Alignment. Neither slot stream carries a frame marker: a channel lines up with its slots only
//! when its DMA meets a serializer that starts afresh (Asahi's MCA driver resets the serializer on
//! every trigger for this reason; here the firmware owns it). So every playback prepare takes a
//! running serializer down to `pw1 ` and back to `pwrd` (the playback DMA is stopped then, and the
//! playback channel starts on the fresh serializer, the order every playback start has always
//! had), and the sense is not a DMA channel of its front-end: the driver owns the sense channel,
//! arms it on the stopped serializer before every `pwrd`, and the sense front-end copies whole
//! frames out of that ring. A sense front-end that starts while the speakers play attaches at
//! the next whole frame; it never starts a channel mid-stream, which slipped the V/I pairs.
//! System sleep takes the serializer down with the streams suspended and brings it back, sense
//! ring first, before ASoC unmutes the amps.
//!
//! The 3.5 mm jack is two more services of the same firmware: `cout` (MCA cluster 2, serializer
//! `ms02`, `mck2` from `nco3` at 3.072 MHz: a 64-bit frame at 48 kHz, two 32-bit playback slots,
//! 24 valid bits) on base-ns ADMAC channel 4 (TX2), and `cin ` (the headset microphone, one slot
//! back on the same serializer) on channel 5 (RX2). The CS42L84 codec on i2c2 is its own driver's.
//! The firmware arbitrates the MCA and the audio power states between the speaker and jack streams,
//! so the two never program shared registers behind each other's back. Each service has its own
//! attachment and power states: `pw1 ` from back-end startup, `pwrd` (output flag 2 for `cout`,
//! input flag 1 for `cin `, as the microphones use) from prepare. System sleep takes both down
//! with the streams suspended and brings back what ran, before ASoC unmutes the codec.
//!
//! The jack is a second ASoC component on the same device, for a sound card of its own (macaudio's
//! J616 jack variant): front-ends `j616-jack-pcm-0` (the jack) and `j616-jack-pcm-1` (routes only)
//! and the `jack` back-end, `sound-dai = <&aop_audio 4>`. A component binds to one card only, and
//! a card binds only once every codec of it has probed: on its own card, a jack codec that never
//! answers costs the jack, not the speakers. The speaker component is the same with or without the
//! jack. The jack component exists only when the audio node lists the "cout" and "cin" DMA
//! channels.
//!
//! The structure (component context with the descriptor as its first field, DAI tables, dmaengine
//! front-ends) follows aurora's T8140 backend, aop_t8140.rs (Copyright (C) 2026 The Aurora Silicon
//! Contributors).

use core::{
    ffi::c_void,
    mem, ptr, slice,
    sync::atomic::{fence, AtomicBool, AtomicI32, AtomicPtr, AtomicU64, AtomicUsize, Ordering},
};

use kernel::{
    bindings, c_str, device,
    error::from_err_ptr,
    new_mutex,
    prelude::*,
    soc::apple::aop::from_fourcc,
    sync::{aref::ARef, Arc, Mutex},
    types::Opaque,
};

use super::{
    AudioAttachDevice, AudioSetDeviceProp, PowerSetting, SndSocAopData, AUDIO_RET_BUSY,
    EPIC_SUBTYPE_WRAPPED_CALL,
};

const CALLTYPE_AUDIO_GET_PROP: u32 = 0xc3000004;
const AUDIO_DEV_SPKR: u32 = from_fourcc(b"spkr");
/// The jack's output and headset microphone (ADT iop-audio-controller identifiers 'tuoc', ' nic').
const AUDIO_DEV_COUT: u32 = from_fourcc(b"cout");
const AUDIO_DEV_CIN: u32 = from_fourcc(b"cin ");
const PROP_POWER_STATE: u32 = 200;
const PROP_POWER_REQUEST: u32 = 202;
const STATE_IDLE: u32 = from_fourcc(b"idle");
const STATE_PW1: u32 = from_fourcc(b"pw1 ");
const STATE_PWRD: u32 = from_fourcc(b"pwrd");
/// The power request parameter for `pwrd` on an output stream (aurora: "output flag 2").
const PWRD_OUTPUT: u32 = 2;
/// The same for an input stream (the microphones' `pwrd` request uses 1).
const PWRD_INPUT: u32 = 1;

/// Property read (0xc3000004): the same header as the property write, with a trailing 1.
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

impl AudioGetDeviceProp {
    fn new(dev_id: u32, modifier: u32) -> Self {
        Self {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_GET_PROP,
            len: 0x30,
            dev_id,
            modifier,
            unk6: 1,
            ..Default::default()
        }
    }
}

/// A firmware audio service: attachment, power-state users and whether the serializer runs.
struct ServiceState {
    attached: bool,
    /// back-end directions started plus front-ends holding a DMA channel
    users: u32,
    running: bool,
    /// The serializer ran when the system suspended: resume brings it back.
    resume_run: bool,
    sequence: u32,
}

#[pin_data]
pub(crate) struct Service {
    aop: Arc<SndSocAopData>,
    dev_id: u32,
    /// The service name in log messages.
    tag: &'static str,
    /// The `pwrd` request parameter: output or input.
    pwrd_param: u32,
    /// The speaker service's sense ring; `None` for the jack's services.
    ring: Option<SenseRing>,
    #[pin]
    state: Mutex<ServiceState>,
}

impl Service {
    fn new(
        aop: Arc<SndSocAopData>,
        dev_id: u32,
        tag: &'static str,
        pwrd_param: u32,
        ring: bool,
    ) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Service {
                aop,
                dev_id,
                tag,
                pwrd_param,
                ring: ring.then(SenseRing::new),
                state <- new_mutex!(ServiceState {
                    attached: false,
                    users: 0,
                    running: false,
                    resume_run: false,
                    sequence: 0,
                }),
            }),
            GFP_KERNEL,
        )
    }

    fn call_ret<T>(&self, msg: &T, ret_len: usize) -> Result<KVec<u8>> {
        // SAFETY: T is a plain packed firmware record.
        let bytes =
            unsafe { slice::from_raw_parts(ptr::from_ref(msg).cast::<u8>(), mem::size_of::<T>()) };
        let (retcode, ret) = self.aop.adata.epic_call_ret(
            &self.aop.service,
            EPIC_SUBTYPE_WRAPPED_CALL,
            bytes,
            ret_len,
        )?;
        if retcode != 0 {
            dev_err!(
                self.aop.dev,
                "{} property read failed, return code {:#x}\n",
                self.tag,
                retcode
            );
            return Err(EIO);
        }
        Ok(ret)
    }

    /// Property 200: the service's current power state (reply: u32 length, then the FourCC).
    fn power_state(&self) -> Result<u32> {
        let ret = self.call_ret(&AudioGetDeviceProp::new(self.dev_id, PROP_POWER_STATE), 8)?;
        if ret.len() < 8 || u32::from_le_bytes([ret[0], ret[1], ret[2], ret[3]]) != 4 {
            dev_err!(
                self.aop.dev,
                "{} malformed power-state reply ({} bytes)\n",
                self.tag,
                ret.len()
            );
            return Err(EIO);
        }
        Ok(u32::from_le_bytes([ret[4], ret[5], ret[6], ret[7]]))
    }

    /// Property 202, then the property 200 readback: a state the firmware did not take fails the
    /// stream.
    fn set_power(&self, st: &mut ServiceState, state: u32, param: u32) -> Result<()> {
        st.sequence = st.sequence.wrapping_add(1);
        let msg = AudioSetDeviceProp::new(
            self.dev_id,
            PROP_POWER_REQUEST,
            PowerSetting::new(self.dev_id, st.sequence, state, param),
        );
        let ret = self.aop.epic_wrapped_call(&msg)?;
        if ret != 0 {
            dev_err!(
                self.aop.dev,
                "{} power request {:#x} rejected: {:#x}\n",
                self.tag,
                state,
                ret
            );
            return Err(EIO);
        }
        if self.power_state()? != state {
            return Err(EIO);
        }
        Ok(())
    }

    /// A user of the service: the first one attaches it (once per firmware lifetime) and raises
    /// `pw1 `.
    fn get(&self) -> Result<()> {
        let mut st = self.state.lock();
        if st.users == 0 {
            // A power-down that failed or timed out can leave the firmware in another state than
            // ours: note it; pw1 is valid from any.
            if st.attached {
                match self.power_state() {
                    Ok(now) if now != STATE_IDLE => {
                        dev_warn!(
                            self.aop.dev,
                            "{} found state {:#x} at startup\n",
                            self.tag,
                            now
                        )
                    }
                    _ => {}
                }
            }
            if !st.attached {
                let ret = self
                    .aop
                    .epic_wrapped_call(&AudioAttachDevice::new(self.dev_id))?;
                if ret != 0 && ret != AUDIO_RET_BUSY {
                    dev_err!(
                        self.aop.dev,
                        "{} attach failed, return code {:#x}\n",
                        self.tag,
                        ret
                    );
                    return Err(EIO);
                }
                st.attached = true;
            }
            self.set_power(&mut st, STATE_PW1, 0)?;
        }
        st.users += 1;
        Ok(())
    }

    /// The last user takes the service back through `pw1 ` (if it still runs) to `idle`.
    fn put(&self) {
        let mut st = self.state.lock();
        if st.users == 0 {
            dev_warn!(self.aop.dev, "{} unbalanced put\n", self.tag);
            return;
        }
        st.users -= 1;
        if st.users != 0 {
            return;
        }
        if st.running {
            if let Err(e) = self.set_power(&mut st, STATE_PW1, 0) {
                dev_err!(
                    self.aop.dev,
                    "{} pw1 at shutdown failed: {:?}\n",
                    self.tag,
                    e
                );
            }
            st.running = false;
        }
        // The channel goes while the fabric is still powered.
        if let Some(ring) = &self.ring {
            ring.release();
        }
        if let Err(e) = self.set_power(&mut st, STATE_IDLE, 0) {
            dev_err!(self.aop.dev, "{} idle failed: {:?}\n", self.tag, e);
        }
    }

    /// `pwrd`: the firmware starts the serializer clocks (before the amps or the codec unmute).
    ///
    /// The speaker service starts a fresh serializer every time: a running one goes down to
    /// `pw1 ` first (the sense ring sees a gap of the two firmware round trips; the playback DMA
    /// is stopped, as prepare only runs on a stopped stream), then the sense ring is armed and
    /// `pwrd` starts the serializer on it. The playback channel starts after this, in the
    /// trigger, from the FIFO the stopped stream drained into the running serializer.
    /// `quiesce` runs before a running serializer stops: amps left unmuted (a re-prepare after a
    /// resume or an xrun) must not see their clocks stop.
    fn run(&self, quiesce: impl FnOnce()) -> Result<()> {
        let mut st = self.state.lock();
        if st.users == 0 {
            return Err(EINVAL);
        }
        let Some(ring) = &self.ring else {
            if !st.running {
                self.set_power(&mut st, STATE_PWRD, self.pwrd_param)?;
                st.running = true;
            }
            return Ok(());
        };
        if st.running {
            quiesce();
            self.set_power(&mut st, STATE_PW1, 0)?;
            st.running = false;
        }
        self.start_with_ring(&mut st, ring)
    }

    /// Arms the sense ring on the stopped serializer, then `pwrd`.
    fn start_with_ring(&self, st: &mut ServiceState, ring: &SenseRing) -> Result<()> {
        // The serializer stopped a firmware round trip ago: the channel has drained what the
        // serializer last delivered.
        ring.stop();
        ring.start(&self.aop.dev)?;
        if let Err(e) = self.set_power(st, STATE_PWRD, self.pwrd_param) {
            ring.stop();
            return Err(e);
        }
        st.running = true;
        Ok(())
    }

    /// Back to `pw1 ` (after the amps or the codec muted); the sense ring stops after it.
    fn stop(&self) {
        let mut st = self.state.lock();
        self.stop_locked(&mut st);
    }

    fn stop_locked(&self, st: &mut ServiceState) {
        if st.running {
            if let Err(e) = self.set_power(st, STATE_PW1, 0) {
                dev_err!(self.aop.dev, "{} pw1 failed: {:?}\n", self.tag, e);
            }
            st.running = false;
        }
        if let Some(ring) = &self.ring {
            ring.stop();
        }
    }

    /// System sleep: the front-ends are suspended (their DMA stopped) and the amps muted; the
    /// serializer and the sense ring stop.
    fn suspend(&self) {
        let mut st = self.state.lock();
        st.resume_run = st.running;
        self.stop_locked(&mut st);
    }

    /// Resume: a serializer that ran comes back, sense ring first, before ASoC unmutes the amps of
    /// the suspended stream (the clocks must run when they do). The stream's own prepare then
    /// restarts it once more, with its DMA.
    fn resume(&self) -> Result<()> {
        let mut st = self.state.lock();
        if !mem::take(&mut st.resume_run) || st.users == 0 {
            return Ok(());
        }
        match &self.ring {
            Some(ring) => self.start_with_ring(&mut st, ring),
            None => {
                self.set_power(&mut st, STATE_PWRD, self.pwrd_param)?;
                st.running = true;
                Ok(())
            }
        }
    }
}

const SENSE_FRAME_BYTES: usize = SENSE_CHANNELS as usize * 2;
/// 10 ms periods, eight of them: whole frames, so ring offsets on period edges stay on frames.
const SENSE_RING_PERIOD_BYTES: usize = 480 * SENSE_FRAME_BYTES;
const SENSE_RING_BYTES: usize = 8 * SENSE_RING_PERIOD_BYTES;
/// SNDRV_PCM_STATE_RUNNING (a cast macro the bindings do not carry).
const PCM_STATE_RUNNING: bindings::snd_pcm_state_t = 3;

/// The driver-owned sense capture (base-ns ADMAC RX0): a cyclic ring armed before every `pwrd`,
/// so ring offset 0 is slot 0 of a frame and every whole-frame offset is a frame start.
///
/// The ring fields change only with the channel stopped and synchronized (no callback runs);
/// `start` publishes them before the channel starts.
struct SenseRing {
    chan: AtomicPtr<bindings::dma_chan>,
    cpu: AtomicPtr<u8>,
    handle: AtomicU64,
    cookie: AtomicI32,
    /// Ring offset (a whole frame) up to which the callback has consumed the data.
    done: AtomicUsize,
    /// The attached sense front-end, or null; cleared under that front-end's stream lock, which
    /// the callback holds while it uses the front-end.
    sub: AtomicPtr<bindings::snd_pcm_substream>,
    /// Frames delivered to the front-end since its start; changed under its stream lock.
    fe_pos: AtomicUsize,
    /// The front-end runs (its trigger sets this under the stream lock).
    fe_running: AtomicBool,
}

impl SenseRing {
    fn new() -> Self {
        Self {
            chan: AtomicPtr::new(ptr::null_mut()),
            cpu: AtomicPtr::new(ptr::null_mut()),
            handle: AtomicU64::new(0),
            cookie: AtomicI32::new(0),
            done: AtomicUsize::new(0),
            sub: AtomicPtr::new(ptr::null_mut()),
            fe_pos: AtomicUsize::new(0),
            fe_running: AtomicBool::new(false),
        }
    }

    /// Requests the channel and the ring (once per power-up of the service) and starts the cyclic
    /// transfer. The caller holds the service lock with the fabric powered and the serializer
    /// stopped.
    fn start(&self, dev: &device::Device) -> Result<()> {
        let mut chan = self.chan.load(Ordering::Acquire);
        if chan.is_null() {
            // SAFETY: the device is live and the name NUL-terminated.
            chan = from_err_ptr(unsafe {
                bindings::dma_request_chan(dev.as_raw(), c_str!("sense").as_char_ptr())
            })?;
            let mut handle: bindings::dma_addr_t = 0;
            // SAFETY: the channel's device is the ADMAC behind AOP DART stream 8; it outlives the
            // ring, which `release` frees before the channel.
            let cpu = unsafe {
                bindings::dma_alloc_attrs(
                    (*(*chan).device).dev,
                    SENSE_RING_BYTES,
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
        // FRAME_4_WORDS, as the front-ends' streams (see `pcm_hw_params`).
        cfg.src_port_window_size = 4;
        // SAFETY: `chan` is our live, stopped channel; the descriptor is ours until submitted.
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
                SENSE_RING_BYTES,
                SENSE_RING_PERIOD_BYTES,
                bindings::dma_transfer_direction_DMA_DEV_TO_MEM,
                bindings::dma_ctrl_flags_DMA_PREP_INTERRUPT as _,
            );
            if desc.is_null() {
                return Err(EIO);
            }
            (*desc).callback = Some(sense_ring_done);
            (*desc).callback_param = ptr::from_ref(self).cast_mut().cast();
            self.done.store(0, Ordering::Release);
            let cookie = ((*desc).tx_submit.ok_or(EIO)?)(desc);
            if cookie < 0 {
                return Err(Error::from_errno(cookie));
            }
            self.cookie.store(cookie, Ordering::Release);
            if let Some(f) = (*dma).device_issue_pending {
                f(chan);
            }
        }
        Ok(())
    }

    /// Stops the transfer and waits for its callback; a no-op without a channel.
    fn stop(&self) {
        let chan = self.chan.load(Ordering::Acquire);
        if chan.is_null() {
            return;
        }
        // SAFETY: our live channel; terminate and synchronize are valid on an idle one.
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

    /// Stops, then frees the ring and the channel (with the fabric still powered).
    fn release(&self) {
        self.stop();
        let chan = self.chan.swap(ptr::null_mut(), Ordering::AcqRel);
        if chan.is_null() {
            return;
        }
        // SAFETY: the transfer is stopped and synchronized; the ring was allocated on this
        // channel's device in `start`.
        unsafe {
            bindings::dma_free_attrs(
                (*(*chan).device).dev,
                SENSE_RING_BYTES,
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

    /// Detaches the front-end under its stream lock: a callback that read the pointer before
    /// takes the same lock, sees the front-end gone and leaves it alone. No waiting.
    ///
    /// # Safety
    ///
    /// `substream` must be the attached front-end, in its close.
    unsafe fn detach(&self, substream: *mut bindings::snd_pcm_substream) {
        // SAFETY: by the caller's contract.
        unsafe {
            let flags = bindings::_snd_pcm_stream_lock_irqsave(substream);
            self.sub.store(ptr::null_mut(), Ordering::Release);
            bindings::snd_pcm_stream_unlock_irqrestore(substream, flags);
        }
    }

    /// Copies `bytes` (whole frames) from ring offset `from` into a running front-end's buffer.
    ///
    /// # Safety
    ///
    /// `sub` must be a substream this ring had attached. The substream itself belongs to the PCM
    /// and outlives the ring's use of it: the ring stops and synchronizes with this callback when
    /// the service's last user goes, before the card can go.
    unsafe fn deliver(&self, sub: *mut bindings::snd_pcm_substream, from: usize, bytes: usize) {
        // SAFETY: by the caller's contract; the stream lock orders this with the trigger, the
        // pointer callback and `detach`.
        let flags = unsafe { bindings::_snd_pcm_stream_lock_irqsave(sub) };
        // Still attached under the lock: the runtime lives until after the front-end's close.
        let rt = if self.sub.load(Ordering::Acquire) == sub {
            // SAFETY: as above.
            unsafe { (*sub).runtime }
        } else {
            ptr::null_mut()
        };
        // SAFETY: an attached front-end's runtime (see above).
        if !rt.is_null()
            && self.fe_running.load(Ordering::Relaxed)
            && unsafe { (*rt).state } == PCM_STATE_RUNNING
            && unsafe { (*rt).frame_bits } as usize == SENSE_FRAME_BYTES * 8
        {
            // SAFETY: as above.
            let (area, frames, period) = unsafe {
                (
                    (*rt).dma_area,
                    (*rt).buffer_size as usize,
                    (*rt).period_size as usize,
                )
            };
            let ring = self.cpu.load(Ordering::Acquire);
            let mut pos = self.fe_pos.load(Ordering::Relaxed);
            let first = pos;
            let mut src = from;
            let mut left = bytes / SENSE_FRAME_BYTES;
            while left > 0 && frames > 0 {
                let off = pos % frames;
                let n = left
                    .min(frames - off)
                    .min((SENSE_RING_BYTES - src) / SENSE_FRAME_BYTES);
                // SAFETY: `n` frames fit at `off` in the front-end buffer and at `src` in the
                // ring; the DMA wrote them before the residue we read said so.
                unsafe {
                    ptr::copy_nonoverlapping(
                        ring.add(src),
                        area.add(off * SENSE_FRAME_BYTES),
                        n * SENSE_FRAME_BYTES,
                    )
                };
                pos += n;
                left -= n;
                src = (src + n * SENSE_FRAME_BYTES) % SENSE_RING_BYTES;
            }
            self.fe_pos.store(pos, Ordering::Relaxed);
            // Once per completed period, as ALSA's interrupt accounting expects: it treats a call
            // that advanced less than a period, some jiffies after the last one, as a missed
            // buffer wrap (and the sense front-end's rate is 0 until a playback sets it, which
            // makes that window two jiffies), so a period longer than the ring's 10 ms overran.
            if first / period.max(1) != pos / period.max(1) {
                // SAFETY: a running, attached front-end, under its stream lock.
                unsafe { bindings::snd_pcm_period_elapsed_under_stream_lock(sub) };
            }
        }
        // SAFETY: locked above.
        unsafe { bindings::snd_pcm_stream_unlock_irqrestore(sub, flags) };
    }
}

/// The ring's period callback (the ADMAC tasklet): hands the whole frames written since the last
/// call to a running sense front-end, or drops them.
///
/// # Safety
///
/// `param` is the `SenseRing` that set up the transfer; `stop` synchronizes with this callback
/// before the ring changes or goes away.
unsafe extern "C" fn sense_ring_done(param: *mut c_void) {
    // SAFETY: by the function's contract.
    let ring = unsafe { &*param.cast::<SenseRing>() };
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
    let residue = state.residue as usize;
    if status == bindings::dma_status_DMA_ERROR || residue > SENSE_RING_BYTES {
        return;
    }
    // The device wrote everything up to here before it reported the residue.
    fence(Ordering::SeqCst);
    let now =
        (SENSE_RING_BYTES - residue) % SENSE_RING_BYTES / SENSE_FRAME_BYTES * SENSE_FRAME_BYTES;
    let from = ring.done.swap(now, Ordering::AcqRel);
    let bytes = (now + SENSE_RING_BYTES - from) % SENSE_RING_BYTES;
    if bytes == 0 {
        return;
    }
    let sub = ring.sub.load(Ordering::Acquire);
    if !sub.is_null() {
        // SAFETY: a substream the ring had attached (`deliver` re-checks under its lock).
        unsafe { ring.deliver(sub, from, bytes) };
    }
}

// ---- the ASoC component ----

const FE_COUNT: usize = 5;
/// DAI ids: 0 lets ASoC assign one, so the front-ends carry 1..=5 (the speaker component's three,
/// then the jack component's two) and the back-ends 10 and 11.
const FE_DAI_ID_BASE: u32 = 1;
const BE_DAI_ID_SPKR: u32 = 10;
const BE_DAI_ID_JACK: u32 = 11;
/// The jack back-end's `sound-dai` argument on the audio node: 0..=3 are the speaker component's
/// DAIs by table index.
const DT_DAI_JACK: u32 = 4;
/// Which service's power a front-end stream needs.
#[derive(Clone, Copy)]
enum Svc {
    Spkr,
    Cout,
    Cin,
}
/// A front-end stream: its channel name on the audio node, its service and its sample formats.
struct FeStream {
    dma: &'static CStr,
    svc: Svc,
    formats: u64,
    /// The sense: served from the service's ring, not from a channel of its own.
    ring: bool,
}
/// Per front-end and stream [playback, capture]. The speaker card: j616-pcm-0 is macaudio's
/// "Primary" (the jack on other machines; none here), j616-pcm-1 its "Secondary", the speakers,
/// j616-pcm-2 the speaker sense. The jack card: j616-jack-pcm-0 its "Primary", the jack,
/// j616-jack-pcm-1 its "Secondary" (none).
static FE_STREAMS: [[Option<FeStream>; 2]; FE_COUNT] = [
    [None, None],
    [
        Some(FeStream {
            dma: c_str!("spkr"),
            svc: Svc::Spkr,
            formats: PLAYBACK_FORMATS,
            ring: false,
        }),
        None,
    ],
    [
        None,
        Some(FeStream {
            dma: c_str!("sense"),
            svc: Svc::Spkr,
            formats: SENSE_FORMATS,
            ring: true,
        }),
    ],
    [
        Some(FeStream {
            dma: c_str!("cout"),
            svc: Svc::Cout,
            formats: PLAYBACK_FORMATS,
            ring: false,
        }),
        Some(FeStream {
            dma: c_str!("cin"),
            svc: Svc::Cin,
            formats: PLAYBACK_FORMATS,
            ring: false,
        }),
    ],
    [None, None],
];
const SPKR_CHANNELS: u32 = 6;
const SENSE_CHANNELS: u32 = 12;
const JACK_CHANNELS: u32 = 2;
const HEADSET_CHANNELS: u32 = 1;
/// The serializer takes 24-bit samples from the low bits of each 32-bit DMA word (as the AOP's
/// other 24-in-32 streams do; the jack's records say 24 valid bits as well); the sense words are
/// 16-bit. SNDRV_PCM_FMTBIT_S24_LE and _S16_LE (formats 6 and 2; the bindings only carry the float
/// one).
const PLAYBACK_FORMATS: u64 = 1 << 6;
const SENSE_FORMATS: u64 = 1 << 2;
const PCM_BUFFER_MAX: usize = 1024 * 1024;
const PCM_PERIOD_BYTES_MIN: usize = 256;
const PCM_PERIOD_BYTES_MAX: usize = 0x10000;
/// One ADMAC frame (FRAME_4_WORDS): the jack's 8- and 4-byte frames need whole bursts per period.
const ADMAC_BURST_BYTES: u32 = 16;

/// Everything ASoC keeps pointers into, one per component; C retains the address of `component`,
/// so this lives from before registration until unregistration has finished every callback.
#[repr(C)]
struct AsocContext {
    component: bindings::snd_soc_component_driver,
    spkr: Arc<Service>,
    cout: Arc<Service>,
    cin: Arc<Service>,
    dais: KVec<Opaque<bindings::snd_soc_dai_driver>>,
    _ops: KBox<DaiOps>,
}

const _: () = assert!(mem::offset_of!(AsocContext, component) == 0);

/// # Safety
///
/// `component` must belong to this module's registered component.
unsafe fn context<'a>(component: *mut bindings::snd_soc_component) -> &'a AsocContext {
    // SAFETY: by the caller's contract; the descriptor is the first field of its repr(C) context
    // and C keeps the exact pointer passed to registration.
    unsafe { &*(*component).driver.cast::<AsocContext>() }
}

impl AsocContext {
    fn service(&self, svc: Svc) -> &Service {
        match svc {
            Svc::Spkr => &self.spkr,
            Svc::Cout => &self.cout,
            Svc::Cin => &self.cin,
        }
    }
}

/// The service behind a back-end DAI and direction, and whether its prepare runs the serializer:
/// the speaker sense rides on the playback's `pwrd`, the jack's directions are separate services.
///
/// # Safety
///
/// `dai` must be a DAI of this module's registered component and `substream` live.
unsafe fn be_service<'a>(
    dai: *mut bindings::snd_soc_dai,
    substream: *mut bindings::snd_pcm_substream,
) -> (&'a Service, bool) {
    // SAFETY: by the caller's contract.
    let (ctx, id, playback) = unsafe {
        (
            context((*dai).component),
            (*dai).id as u32,
            is_playback(substream),
        )
    };
    match (id, playback) {
        (BE_DAI_ID_JACK, true) => (&ctx.cout, true),
        (BE_DAI_ID_JACK, false) => (&ctx.cin, true),
        (_, playback) => (&ctx.spkr, playback),
    }
}

/// The front-end stream of a runtime and substream, `None` for a back-end or a stream this machine
/// does not have.
///
/// # Safety
///
/// `rtd` must be a live runtime of one of this component's links and `substream` live.
unsafe fn fe_stream(
    rtd: *mut bindings::snd_soc_pcm_runtime,
    substream: *mut bindings::snd_pcm_substream,
) -> Option<(usize, &'static FeStream)> {
    // SAFETY: by the caller's contract.
    let (fe, playback) = unsafe { (fe_index(rtd)?, is_playback(substream)) };
    Some((fe, FE_STREAMS[fe][usize::from(!playback)].as_ref()?))
}

/// # Safety
///
/// `substream` must be live and belong to one of this component's links.
unsafe fn rtd_of(
    substream: *mut bindings::snd_pcm_substream,
) -> *mut bindings::snd_soc_pcm_runtime {
    // SAFETY: by the caller's contract.
    unsafe { (*substream).private_data.cast() }
}

/// # Safety
///
/// `rtd` must be a live ASoC runtime.
unsafe fn is_be(rtd: *mut bindings::snd_soc_pcm_runtime) -> bool {
    // SAFETY: by the caller's contract.
    unsafe { (*(*rtd).dai_link).no_pcm() != 0 }
}

/// The front-end index of a runtime (from its CPU DAI's id), `None` for a back-end.
///
/// # Safety
///
/// `rtd` must be a live ASoC runtime of one of this component's links.
unsafe fn fe_index(rtd: *mut bindings::snd_soc_pcm_runtime) -> Option<usize> {
    // SAFETY: by the caller's contract; the first DAI of a runtime is its CPU DAI.
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

/// The sense ring behind a front-end substream, `None` for every other stream.
///
/// # Safety
///
/// As for `fe_stream`, and `component` must be this module's registered component.
unsafe fn sense_ring<'a>(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> Option<&'a SenseRing> {
    // SAFETY: by the caller's contract.
    let (ctx, rtd) = unsafe { (context(component), rtd_of(substream)) };
    // SAFETY: as above.
    if unsafe { is_be(rtd) } {
        return None;
    }
    // SAFETY: as above.
    let (_, stream) = unsafe { fe_stream(rtd, substream) }?;
    if !stream.ring {
        return None;
    }
    ctx.service(stream.svc).ring.as_ref()
}

/// The sense front-end's buffer: plain memory the ring callback copies whole frames into.
fn sense_open(substream: *mut bindings::snd_pcm_substream) -> i32 {
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
        let mut hw = dmaengine_hw();
        hw.info |= bindings::SNDRV_PCM_INFO_BLOCK_TRANSFER | bindings::SNDRV_PCM_INFO_BATCH;
        hw.formats = SENSE_FORMATS;
        (*(*substream).runtime).hw = hw;
    }
    0
}

fn dmaengine_hw() -> bindings::snd_pcm_hardware {
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

/// Front-end open: a power-state reference (the back-end already raised `pw1 `, which powers the
/// ADMAC), then the channel and the dmaengine runtime on it.
///
/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_open(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: ASoC calls the component op with live arguments of one of its links.
    let (ctx, rtd) = unsafe { (context(component), rtd_of(substream)) };
    // SAFETY: as above.
    if unsafe { is_be(rtd) } {
        return 0;
    }
    // SAFETY: as above.
    if unsafe { fe_index(rtd) }.is_none() {
        return EINVAL.to_errno();
    }
    // SAFETY: as above.
    let Some((fe, stream)) = (unsafe { fe_stream(rtd, substream) }) else {
        return ENODEV.to_errno();
    };
    let svc = ctx.service(stream.svc);
    if let Err(e) = svc.get() {
        return e.to_errno();
    }
    if let (true, Some(ring)) = (stream.ring, &svc.ring) {
        let ret = sense_open(substream);
        if ret < 0 {
            svc.put();
            return ret;
        }
        ring.attach(substream);
        return 0;
    }
    // SAFETY: the device is live and the name NUL-terminated.
    let chan = match from_err_ptr(unsafe {
        bindings::dma_request_chan(svc.aop.dev.as_raw(), stream.dma.as_char_ptr())
    }) {
        Ok(chan) => chan,
        Err(e) => {
            dev_err!(
                svc.aop.dev,
                "{} front-end {}: no DMA channel {:?}: {:?}\n",
                svc.tag,
                fe,
                stream.dma,
                e
            );
            svc.put();
            return e.to_errno();
        }
    };
    let mut ret = pcm_open_dmaengine(substream, chan, stream.formats);
    if ret >= 0 && !matches!(stream.svc, Svc::Spkr) {
        // SAFETY: the runtime exists for the op.
        ret = unsafe {
            bindings::snd_pcm_hw_constraint_step(
                (*substream).runtime,
                0,
                bindings::SNDRV_PCM_HW_PARAM_PERIOD_BYTES as _,
                ADMAC_BURST_BYTES as _,
            )
        };
        if ret < 0 {
            // SAFETY: the dmaengine runtime owns the channel since the open succeeded.
            unsafe { bindings::snd_dmaengine_pcm_close_release_chan(substream) };
            svc.put();
            return ret;
        }
    }
    if ret < 0 {
        // SAFETY: the channel was requested above and is not attached to the substream.
        unsafe { bindings::dma_release_channel(chan) };
        svc.put();
    }
    ret
}

fn pcm_open_dmaengine(
    substream: *mut bindings::snd_pcm_substream,
    chan: *mut bindings::dma_chan,
    formats: u64,
) -> i32 {
    // SAFETY: a live substream and a channel the caller requested.
    let unallocated =
        unsafe { (*substream).dma_buffer.dev.type_ == bindings::SNDRV_DMA_TYPE_UNKNOWN as i32 };
    if unallocated {
        // SAFETY: as above; the channel's device is the ADMAC behind AOP DART stream 8 and outlives
        // the buffer.
        let ret = unsafe {
            bindings::snd_pcm_set_managed_buffer(
                substream,
                bindings::SNDRV_DMA_TYPE_DEV as i32,
                (*(*chan).device).dev,
                0,
                0,
            )
        };
        if ret < 0 {
            return ret;
        }
    }
    let mut hw = dmaengine_hw();
    let mut dma_data = bindings::snd_dmaengine_dai_dma_data::default();
    // SAFETY: the substream and the channel are live for the call.
    let ret = unsafe {
        bindings::snd_dmaengine_pcm_refine_runtime_hwparams(substream, &mut dma_data, &mut hw, chan)
    };
    if ret < 0 {
        return ret;
    }
    hw.formats &= formats;
    if hw.formats == 0 {
        return EINVAL.to_errno();
    }
    // The slots line up only when a prepare restarts the serializer (`Service::run`): a paused
    // DMA restarted mid-frame by resume or pause-release would shift them, so take neither.
    hw.info &= !(bindings::SNDRV_PCM_INFO_PAUSE | bindings::SNDRV_PCM_INFO_RESUME);
    // SAFETY: the runtime exists for the op; on success the dmaengine runtime owns `chan`.
    unsafe {
        (*(*substream).runtime).hw = hw;
        bindings::snd_dmaengine_pcm_open(substream, chan)
    }
}

/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_close(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: ASoC calls the component op with live arguments of one of its links.
    let (ctx, rtd) = unsafe { (context(component), rtd_of(substream)) };
    // SAFETY: as above.
    if unsafe { is_be(rtd) } {
        return 0;
    }
    // SAFETY: as above.
    let Some((_, stream)) = (unsafe { fe_stream(rtd, substream) }) else {
        return 0;
    };
    let svc = ctx.service(stream.svc);
    if let (true, Some(ring)) = (stream.ring, &svc.ring) {
        // SAFETY: the attached sense front-end, in its close.
        unsafe { ring.detach(substream) };
        svc.put();
        return 0;
    }
    // The channel goes first, with the fabric still powered by this front-end's reference. SAFETY:
    // the dmaengine runtime was set up by `pcm_open`.
    let ret = unsafe { bindings::snd_dmaengine_pcm_close_release_chan(substream) };
    svc.put();
    ret
}

/// The ADMAC moves one frame per burst of up to four words: FRAME_4_WORDS for every stream, as the
/// ADT's records for these streams select (0x22 32-bit, 0x21 16-bit; the jack's records list both).
///
/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_hw_params(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    params: *mut bindings::snd_pcm_hw_params,
) -> i32 {
    // SAFETY: as for `pcm_open`.
    if unsafe { is_be(rtd_of(substream)) || sense_ring(component, substream).is_some() } {
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up by `pcm_open`.
    let chan = unsafe { bindings::snd_dmaengine_pcm_get_chan(substream) };
    let mut cfg = bindings::dma_slave_config::default();
    // SAFETY: live substream and parameters.
    let rc = unsafe { bindings::snd_hwparams_to_dma_slave_config(substream, params, &mut cfg) };
    if rc < 0 {
        return rc;
    }
    // SAFETY: as above.
    if unsafe { is_playback(substream) } {
        cfg.dst_port_window_size = 4;
    } else {
        cfg.src_port_window_size = 4;
    }
    // SAFETY: `chan` is the live channel behind this substream.
    unsafe {
        match (*(*chan).device).device_config {
            Some(f) => f(chan, &mut cfg),
            None => ENOSYS.to_errno(),
        }
    }
}

/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_trigger(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
) -> i32 {
    // SAFETY: as for `pcm_open`.
    if unsafe { is_be(rtd_of(substream)) } {
        return 0;
    }
    // The sense attaches to the ring at its next whole frame; ALSA holds the stream lock, which
    // the ring callback takes too.
    // SAFETY: as above.
    if let Some(ring) = unsafe { sense_ring(component, substream) } {
        match cmd as u32 {
            bindings::SNDRV_PCM_TRIGGER_START => {
                ring.fe_pos.store(0, Ordering::Relaxed);
                ring.fe_running.store(true, Ordering::Relaxed);
            }
            bindings::SNDRV_PCM_TRIGGER_RESUME | bindings::SNDRV_PCM_TRIGGER_PAUSE_RELEASE => {
                ring.fe_running.store(true, Ordering::Relaxed)
            }
            bindings::SNDRV_PCM_TRIGGER_STOP
            | bindings::SNDRV_PCM_TRIGGER_SUSPEND
            | bindings::SNDRV_PCM_TRIGGER_PAUSE_PUSH => {
                ring.fe_running.store(false, Ordering::Relaxed)
            }
            _ => return EINVAL.to_errno(),
        }
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up by `pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_trigger(substream, cmd) }
}

/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_sync_stop(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: as for `pcm_open`.
    if unsafe { is_be(rtd_of(substream)) || sense_ring(component, substream).is_some() } {
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up by `pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_sync_stop(substream) }
}

/// # Safety
///
/// Called by ASoC on this module's registered component with live arguments.
unsafe extern "C" fn pcm_pointer(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: as for `pcm_open`.
    if unsafe { is_be(rtd_of(substream)) } {
        return (-i64::from(bindings::ENOTSUPP)) as bindings::snd_pcm_uframes_t;
    }
    // SAFETY: as above; ALSA calls this under the stream lock with a live runtime.
    if let Some(ring) = unsafe { sense_ring(component, substream) } {
        // SAFETY: as above.
        let frames = unsafe { (*(*substream).runtime).buffer_size } as usize;
        return (ring.fe_pos.load(Ordering::Relaxed) % frames.max(1))
            as bindings::snd_pcm_uframes_t;
    }
    // SAFETY: the dmaengine runtime was set up by `pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_pointer(substream) }
}

// The firmware runs the serializer: format, slots and clocks are its own, whatever the machine
// driver asks.
unsafe extern "C" fn dai_set_fmt(_dai: *mut bindings::snd_soc_dai, _fmt: u32) -> i32 {
    0
}
unsafe extern "C" fn dai_set_tdm_slot(
    _dai: *mut bindings::snd_soc_dai,
    _tx: u32,
    _rx: u32,
    _slots: i32,
    _width: i32,
) -> i32 {
    0
}
unsafe extern "C" fn dai_set_sysclk(
    _dai: *mut bindings::snd_soc_dai,
    _id: i32,
    _freq: u32,
    _dir: i32,
) -> i32 {
    0
}
unsafe extern "C" fn dai_set_bclk_ratio(_dai: *mut bindings::snd_soc_dai, _ratio: u32) -> i32 {
    0
}

/// Back-end startup: `pw1 ` (the first user attaches the service).
///
/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_startup(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    match unsafe { be_service(dai, substream) }.0.get() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// Prepare: `pwrd`. ASoC prepares this CPU DAI before it unmutes the amps or the codec.
///
/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_prepare(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    let (svc, runs) = unsafe { be_service(dai, substream) };
    if !runs {
        return 0;
    }
    // SAFETY: the back-end runtime of this substream is live for the call.
    match svc.run(|| unsafe { mute_codecs(rtd_of(substream)) }) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// Mutes a back-end's codecs (the amps); `__soc_pcm_prepare` unmutes them right after this
/// prepare.
///
/// # Safety
///
/// `rtd` must be a live back-end runtime, in its prepare.
unsafe fn mute_codecs(rtd: *mut bindings::snd_soc_pcm_runtime) {
    // SAFETY: by the caller's contract; the codec DAIs follow the CPU DAIs in `dais`.
    unsafe {
        let link = (*rtd).dai_link;
        let cpus = (*link).num_cpus as usize;
        for i in 0..(*link).num_codecs as usize {
            bindings::snd_soc_dai_digital_mute(
                *(*rtd).dais.add(cpus + i),
                1,
                bindings::SNDRV_PCM_STREAM_PLAYBACK as i32,
            );
        }
    }
}

/// hw_free: back to `pw1 `. ASoC mutes the amps or the codec before it frees the CPU DAI.
///
/// # Safety
///
/// Called by ASoC with a live DAI of this component and a live substream.
unsafe extern "C" fn be_hw_free(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: by the caller's contract.
    let (svc, runs) = unsafe { be_service(dai, substream) };
    if runs {
        svc.stop();
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
    unsafe { be_service(dai, substream) }.0.put();
}

struct DaiOps {
    fe: bindings::snd_soc_dai_ops,
    be: bindings::snd_soc_dai_ops,
}

impl DaiOps {
    fn new() -> Self {
        let fe = bindings::snd_soc_dai_ops {
            set_fmt: Some(dai_set_fmt),
            set_tdm_slot: Some(dai_set_tdm_slot),
            set_sysclk: Some(dai_set_sysclk),
            set_bclk_ratio: Some(dai_set_bclk_ratio),
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

fn stream(name: &'static CStr, formats: u64, channels: u32) -> bindings::snd_soc_pcm_stream {
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

/// The speaker component: [0..=2] the front-ends, [3] the back-end. The order is ABI
/// (`sound-dai = <&aop_audio 3>`).
fn speaker_dais(ops: &DaiOps) -> [bindings::snd_soc_dai_driver; 4] {
    let mut dais = [
        bindings::snd_soc_dai_driver {
            name: c_str!("j616-pcm-0").as_char_ptr(),
            id: FE_DAI_ID_BASE,
            ops: &ops.fe,
            // No stream of this card's: both directions exist only for macaudio's fixed routes
            // ("PCM0 TX", "PCM0 RX"), whose widgets must exist for the card to bind; opening either
            // fails (ENODEV).
            playback: stream(c_str!("PCM0 TX"), PLAYBACK_FORMATS, SPKR_CHANNELS),
            capture: stream(c_str!("PCM0 RX"), SENSE_FORMATS, SENSE_CHANNELS),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c_str!("j616-pcm-1").as_char_ptr(),
            id: FE_DAI_ID_BASE + 1,
            ops: &ops.fe,
            playback: stream(c_str!("PCM1 TX"), PLAYBACK_FORMATS, SPKR_CHANNELS),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c_str!("j616-pcm-2").as_char_ptr(),
            id: FE_DAI_ID_BASE + 2,
            ops: &ops.fe,
            capture: stream(c_str!("PCM2 RX"), SENSE_FORMATS, SENSE_CHANNELS),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c_str!("spkr").as_char_ptr(),
            id: BE_DAI_ID_SPKR,
            ops: &ops.be,
            playback: stream(c_str!("SPKR TX"), PLAYBACK_FORMATS, SPKR_CHANNELS),
            capture: stream(c_str!("SPKR RX"), SENSE_FORMATS, SENSE_CHANNELS),
            ..Default::default()
        },
    ];
    // The sense runs at the playback's rate (macaudio reads the back-end's symmetric rate).
    dais[3].set_symmetric_rate(1);
    dais
}

/// The jack component: [0] the jack's front-end, [1] the second front-end macaudio's fixed routes
/// need, [2] the back-end (`sound-dai = <&aop_audio 4>`, see `jack_of_xlate_dai_name`).
fn jack_dais(ops: &DaiOps) -> [bindings::snd_soc_dai_driver; 3] {
    let mut dais = [
        bindings::snd_soc_dai_driver {
            name: c_str!("j616-jack-pcm-0").as_char_ptr(),
            id: FE_DAI_ID_BASE + 3,
            ops: &ops.fe,
            playback: stream(c_str!("PCM0 TX"), PLAYBACK_FORMATS, JACK_CHANNELS),
            capture: stream(c_str!("PCM0 RX"), PLAYBACK_FORMATS, HEADSET_CHANNELS),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c_str!("j616-jack-pcm-1").as_char_ptr(),
            id: FE_DAI_ID_BASE + 4,
            ops: &ops.fe,
            // No stream: "PCM1 TX" exists for macaudio's fixed routes; opening it fails (ENODEV).
            playback: stream(c_str!("PCM1 TX"), PLAYBACK_FORMATS, JACK_CHANNELS),
            ..Default::default()
        },
        bindings::snd_soc_dai_driver {
            name: c_str!("jack").as_char_ptr(),
            id: BE_DAI_ID_JACK,
            ops: &ops.be,
            playback: stream(c_str!("JACK TX"), PLAYBACK_FORMATS, JACK_CHANNELS),
            capture: stream(c_str!("JACK RX"), PLAYBACK_FORMATS, HEADSET_CHANNELS),
            ..Default::default()
        },
    ];
    // Both directions share the serializer's frame.
    dais[2].set_symmetric_rate(1);
    dais
}

/// Both components sit on the audio node, and ASoC resolves a `sound-dai` there by asking each
/// component in turn (newest first) until one answers. The speaker component answers 0..=3 by
/// table index and refuses 4 (EINVAL, out of range). This one answers 4 and defers the rest: a
/// speaker card that probes after this component registered but before the speaker component did
/// must wait for it, not fail.
///
/// # Safety
///
/// Called by ASoC with live arguments.
unsafe extern "C" fn jack_of_xlate_dai_name(
    _component: *mut bindings::snd_soc_component,
    args: *const bindings::of_phandle_args,
    dai_name: *mut *const kernel::ffi::c_char,
) -> i32 {
    // SAFETY: by the caller's contract.
    unsafe {
        if (*args).args_count != 1 || (*args).args[0] != DT_DAI_JACK {
            return EPROBE_DEFER.to_errno();
        }
        *dai_name = c_str!("jack").as_char_ptr();
    }
    0
}

/// One registered component; dropping it unregisters it (and with it any card that uses its DAIs).
struct Registration {
    dev: ARef<device::Device>,
    ctx: KBox<AsocContext>,
}

impl Registration {
    fn new(
        aop: &SndSocAopData,
        services: [&Arc<Service>; 3],
        component: bindings::snd_soc_component_driver,
        dai_drivers: fn(&DaiOps) -> Result<KVec<Opaque<bindings::snd_soc_dai_driver>>>,
    ) -> Result<Self> {
        let ops = KBox::new(DaiOps::new(), GFP_KERNEL)?;
        let dais = dai_drivers(&ops)?;
        let [spkr, cout, cin] = services.map(Arc::clone);
        let ctx = KBox::new(
            AsocContext {
                component,
                spkr,
                cout,
                cin,
                dais,
                _ops: ops,
            },
            GFP_KERNEL,
        )?;
        // SAFETY: the descriptor and the DAI table are heap allocations owned by `ctx`, which
        // `Self` keeps until after unregistration.
        let ret = unsafe {
            bindings::snd_soc_register_component_d(
                aop.dev.as_raw(),
                &ctx.component,
                Opaque::cast_into(ctx.dais.as_ptr()),
                ctx.dais.len() as i32,
            )
        };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(Self {
            dev: aop.dev.clone(),
            ctx,
        })
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: registered in `new`; the context stays allocated until unregistration has
        // finished every PCM and DAI callback.
        unsafe {
            bindings::snd_soc_unregister_component_by_driver(self.dev.as_raw(), &self.ctx.component)
        };
    }
}

fn to_kvec<const N: usize>(
    dais: [bindings::snd_soc_dai_driver; N],
) -> Result<KVec<Opaque<bindings::snd_soc_dai_driver>>> {
    let mut v = KVec::with_capacity(N, GFP_KERNEL)?;
    for dai in dais {
        v.push(Opaque::new(dai), GFP_KERNEL)?;
    }
    Ok(v)
}

fn component_driver(name: &'static CStr) -> bindings::snd_soc_component_driver {
    bindings::snd_soc_component_driver {
        name: name.as_char_ptr(),
        open: Some(pcm_open),
        close: Some(pcm_close),
        hw_params: Some(pcm_hw_params),
        trigger: Some(pcm_trigger),
        sync_stop: Some(pcm_sync_stop),
        pointer: Some(pcm_pointer),
        ..Default::default()
    }
}

/// System sleep for the speaker service (see `Service::suspend`).
///
/// # Safety
///
/// Called by ASoC on the registered speaker component.
unsafe extern "C" fn spkr_suspend(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    unsafe { context(component) }.spkr.suspend();
    0
}

/// # Safety
///
/// Called by ASoC on the registered speaker component.
unsafe extern "C" fn spkr_resume(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    match unsafe { context(component) }.spkr.resume() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// System sleep for the jack's services: both directions share the jack's serializer.
///
/// # Safety
///
/// Called by ASoC on the registered jack component.
unsafe extern "C" fn jack_suspend(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    let ctx = unsafe { context(component) };
    ctx.cin.suspend();
    ctx.cout.suspend();
    0
}

/// # Safety
///
/// Called by ASoC on the registered jack component.
unsafe extern "C" fn jack_resume(component: *mut bindings::snd_soc_component) -> i32 {
    // SAFETY: by the caller's contract.
    let ctx = unsafe { context(component) };
    let cout = ctx.cout.resume();
    let cin = ctx.cin.resume();
    match cout.and(cin) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// The speaker component, and the jack's where the audio node lists its DMA channels.
pub(crate) struct SpeakerComponent {
    _spkr: Registration,
    _jack: Option<Registration>,
}

impl SpeakerComponent {
    pub(crate) fn register(aop: Arc<SndSocAopData>) -> Result<Self> {
        let spkr = Service::new(aop.clone(), AUDIO_DEV_SPKR, "spkr", PWRD_OUTPUT, true)?;
        let cout = Service::new(aop.clone(), AUDIO_DEV_COUT, "cout", PWRD_OUTPUT, false)?;
        let cin = Service::new(aop.clone(), AUDIO_DEV_CIN, "cin", PWRD_INPUT, false)?;
        let services = [&spkr, &cout, &cin];
        let has_jack = [c_str!("cout"), c_str!("cin")].iter().all(|name| {
            aop.fwnode
                .property_match_string(c_str!("dma-names"), name)
                .is_ok()
        });
        // The jack goes first (see `jack_of_xlate_dai_name`); its failure costs the jack only.
        let jack = if has_jack {
            let mut driver = component_driver(c_str!("t6040-aop-jack"));
            driver.of_xlate_dai_name = Some(jack_of_xlate_dai_name);
            driver.suspend = Some(jack_suspend);
            driver.resume = Some(jack_resume);
            match Registration::new(&aop, services, driver, |ops| to_kvec(jack_dais(ops))) {
                Ok(r) => Some(r),
                Err(e) => {
                    dev_err!(
                        aop.dev,
                        "Unable to register the jack ASoC component: {:?}\n",
                        e
                    );
                    None
                }
            }
        } else {
            None
        };
        let mut spkr_driver = component_driver(c_str!("t6040-aop-spkr"));
        spkr_driver.suspend = Some(spkr_suspend);
        spkr_driver.resume = Some(spkr_resume);
        let spkr = Registration::new(&aop, services, spkr_driver, |ops| {
            to_kvec(speaker_dais(ops))
        })
        .inspect_err(|e| {
            dev_err!(
                aop.dev,
                "Unable to register the speaker ASoC component: {}\n",
                e.to_errno()
            )
        })?;
        Ok(Self {
            _spkr: spkr,
            _jack: jack,
        })
    }
}

// SAFETY: the raw pointers in the contexts are C descriptors that are only read after
// registration.
unsafe impl Send for SpeakerComponent {}
// SAFETY: as above.
unsafe impl Sync for SpeakerComponent {}
