// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple T8152 (J873, Mac mini M6) and T8142 (J813, MacBook Air M5) AOP audio
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
//! back to `pw0 `. The serializer (MCA) leaves are the AP's to power. The
//! front-end PCMs are plain dmaengine PCMs on the base-ns channels; macaudio
//! links them to the codecs.
//!
//! Copyright (C) The Asahi Linux Contributors
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{mem, ptr, slice};

use kernel::{
    bindings, c_str,
    device,
    device::Core,
    error::from_err_ptr,
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{from_fourcc, EPICService, AOP},
    str::CString,
    sync::{
        aref::ARef,
        atomic::{Atomic, AtomicFlag, Relaxed},
        Arc, ArcBorrow,
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

#[pin_data]
struct T8152AopAudio {
    dev: ARef<device::Device>,
    adata: Arc<dyn AOP>,
    service: EPICService,
    cout: Service,
    cin: Service,
    spkr: Service,
    mca: KVec<PowerDomain>,
    cfg: &'static HwConfig,
    /// The open sense capture front-end, if any.
    sense: Atomic<*mut bindings::snd_pcm_substream>,
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

    /// The sense capture's back-end. Its front-end requests the ADMAC
    /// channel next, and the ADMAC only answers while the firmware holds
    /// the audio fabric up, which it does while a service is in `pw0 ` or
    /// `pwrd`. Opening the sense before any other stream since boot (as
    /// speakersafetyd does) otherwise faults the bus (SError). Shutdown
    /// leaves the service in `pw0 `, as the other streams do.
    fn sense_startup(&self) -> Result<()> {
        self.attach(&self.spkr)?;
        if !self.spkr.running.load(Relaxed) {
            self.set_power(&self.spkr, POWER_STATE_PW0)?;
        }
        Ok(())
    }

    /// `pwrd`: the firmware starts the serializer clocks and the stream.
    fn run(&self, svc: &Service) -> Result<()> {
        if svc.running.load(Relaxed) {
            return Ok(());
        }
        if ptr::eq(svc, &self.spkr) && self.cfg.realign_sense {
            self.realign_sense();
        }
        self.set_power(svc, POWER_STATE_PWRD)?;
        svc.running.store(true, Relaxed);
        Ok(())
    }

    /// Before the speaker's serializer starts, restart a running sense
    /// capture at a frame boundary. The sense words are the serializer's RX,
    /// which the firmware stops anywhere in a frame; a capture that stays
    /// open across speaker streams (speakersafetyd keeps it) would carry
    /// the partial frame forward and attribute one amplifier's voltage and
    /// current to another. Nothing flows while the serializer is stopped,
    /// so this loses no data; the partial frame reads as zeros.
    fn realign_sense(&self) {
        let ss = self.sense.load(Relaxed);
        if ss.is_null() {
            return;
        }
        // SAFETY: the sense front-end's open and close and this back-end's
        // prepare all run under the card's DPCM mutex, so `ss` is live; its
        // stream lock orders this against the front-end's trigger.
        unsafe {
            bindings::snd_pcm_stream_lock_irq(ss);
            let rt = (*ss).runtime;
            if !rt.is_null() && (*rt).state == SNDRV_PCM_STATE_RUNNING {
                let chan = bindings::snd_dmaengine_pcm_get_chan(ss);
                let ops = &*(*chan).device;
                if let (Some(pause), Some(resume)) = (ops.device_pause, ops.device_resume) {
                    pause(chan);
                    let frame = ((*rt).frame_bits / 8) as usize;
                    let pos = bindings::snd_dmaengine_pcm_pointer(ss) as usize;
                    if frame != 0 && pos < (*rt).buffer_size as usize && !(*rt).dma_area.is_null() {
                        ptr::write_bytes((*rt).dma_area.add(pos * frame), 0, frame);
                    }
                    resume(chan);
                }
            }
            bindings::snd_pcm_stream_unlock_irq(ss);
        }
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

    /// Microphone close, after the channel is released: a queued start
    /// finishes first, then `pw0 ` and the leaves.
    fn hpai_shutdown(&self) {
        self.hpai_want_run.store(false, Relaxed);
        // SAFETY: the work item lives in `self`; flushing waits for a queued
        // `hpai_go`.
        unsafe { bindings::flush_work(Work::raw_get(&self.hpai_start_work)) };
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
/// SNDRV_PCM_STATE_RUNNING, likewise.
const SNDRV_PCM_STATE_RUNNING: bindings::snd_pcm_state_t = 3;

/// What differs between the machines: the shape of the firmware's `spkr`
/// stream (ADT iop-audio-controller/audio-speaker).
struct HwConfig {
    /// Speaker slots (one per amplifier).
    spkr_channels: u32,
    /// Words the amplifiers send back on the stream's RX side (V/I sense).
    sense_channels: u32,
    sense_formats: u64,
    /// The sense capture is the speaker stream's RX: restart it at a frame
    /// boundary whenever the speaker starts (see `realign_sense`).
    realign_sense: bool,
}

/// J873: one amplifier in one 32-bit slot, nothing sent back.
static HW_CFG_T8152: HwConfig = HwConfig {
    spkr_channels: 1,
    sense_channels: 2,
    sense_formats: FORMATS,
    realign_sense: false,
};

/// J813: four amplifiers (left woofer, left tweeter, right woofer, right
/// tweeter) in 32-bit slots 0-3, their voltage and current sense back as
/// eight 16-bit slots (V, I per amplifier) on the stream's RX (base-ns RX0).
static HW_CFG_T8142: HwConfig = HwConfig {
    spkr_channels: 4,
    sense_channels: 8,
    sense_formats: FMTBIT_S16_LE,
    realign_sense: true,
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
    } else if fe == 2 && !playback {
        data.sense.store(substream, Relaxed);
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
    let data = unsafe { component_data(component) };
    if data.sense.load(Relaxed) == substream {
        data.sense.store(ptr::null_mut(), Relaxed);
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
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
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
    // The sense capture: the frame size its channel restarts aligned to
    // (apple-admac reads a u32 here; see `realign_sense`).
    // SAFETY: by the caller's contract.
    let data = unsafe { component_data(component) };
    let mut frame_bytes = channels * cfg.src_addr_width as u32;
    if ptr::eq(substream, data.sense.load(Relaxed)) && data.cfg.realign_sense {
        cfg.peripheral_config = ptr::from_mut(&mut frame_bytes).cast();
        cfg.peripheral_size = mem::size_of::<u32>();
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
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
) -> i32 {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // SAFETY: the dmaengine runtime was set up in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_trigger(substream, cmd) }
}

/// # Safety
///
/// Called by ASoC on this driver's component with live arguments.
unsafe extern "C" fn asoc_pcm_pointer(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: by the caller's contract.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return (-i64::from(bindings::ENOTSUPP)) as bindings::snd_pcm_uframes_t;
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
    match data.startup(svc) {
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
    match data.prepare(svc) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
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
        data.shutdown(svc);
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
            open: Some(asoc_pcm_open),
            close: Some(asoc_pcm_close),
            hw_params: Some(asoc_pcm_hw_params),
            trigger: Some(asoc_pcm_trigger),
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
    // SAFETY: the dmaengine runtime was set up in `mic_pcm_open`.
    let ret = unsafe { bindings::snd_dmaengine_pcm_close_release_chan(substream) };
    data.hpai_shutdown();
    ret
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
    sync_stop: None,
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
        let cfg: &'static HwConfig = info.copied().unwrap_or(&HW_CFG_T8152);
        // SAFETY: this is a service device the AOP core registered.
        let adata = unsafe { <dyn AOP>::from_child(pdev.as_ref()) }?;
        // SAFETY: the AOP core sets the child's platform data to its EPIC service.
        let service = unsafe { (*dev.as_raw()).platform_data as *const EPICService };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: checked above; it lives as long as the child.
        let service = unsafe { *service };

        // Every serializer leaf is required: the firmware's streams run on
        // them, so the driver must not bind without one (or before its
        // provider has probed).
        let mut mca = KVec::new();
        for name in MCA_NAMES {
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

        // The microphone array, where the DT describes it: all of its leaves
        // or none.
        let mut leap = KVec::new();
        for name in LEAP_NAMES {
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

        let data_dev = dev.clone();
        let data = Arc::pin_init(
            pin_init!(T8152AopAudio {
                dev: data_dev,
                adata,
                service,
                cout: Service::new(from_fourcc(b"cout"), "cout", true),
                cin: Service::new(from_fourcc(b"cin "), "cin", true),
                spkr: Service::new(from_fourcc(b"spkr"), "spkr", false),
                mca,
                cfg,
                sense: Atomic::new(ptr::null_mut()),
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
        let mic = if data.leap.is_empty() {
            None
        } else {
            match MicCard::new(&data) {
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
    }
}

module_platform_driver! {
    type: T8152AopAudioDriver,
    name: "snd_soc_t8152_aop_audio",
    description: "Apple T8152 AOP audio: headphone jack and speaker",
    license: "Dual MIT/GPL",
}
