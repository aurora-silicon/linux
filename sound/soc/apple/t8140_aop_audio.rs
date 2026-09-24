// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Apple T8140 (J700, MacBook Neo) AOP audio
//!
//! The AOP firmware owns the audio fabric on this SoC, so the paths the AP
//! sees are its services:
//!
//! * `lpai`, the low-power microphone: 16 kHz stereo S32 frames (24
//!   meaningful bits) produced into a 512 KiB host ring mapped through
//!   dart-aop stream 9, with a cursor report every 3,200 frames.  There is
//!   no DMA engine on the host side: the ring is bound to the firmware over
//!   the AOP setup port (the AOP driver owns it, the firmware binds it once
//!   per boot) and the driver copies each reported span into the ALSA
//!   buffer of its own card, the Low-Power Audio Interface.
//! * `cout`/`cin` (the CS42L83 jack on MCA1, base-ns ADMAC TX2/RX2), `spkr`
//!   (the MAX98360A speakers on LEAP TX0, leap-ns ADMAC TX0) and `tap `
//!   (the speaker sense, base-ns RX1): ASoC back-end DAIs that attach the
//!   service, request its power states (`pw0 ` idle, `pwrd` running, the
//!   firmware then runs the serializer clocks) and runtime-power the
//!   AP-side PMGR leaves, with DPCM front-end PCMs on the ADMAC channels.
//!   The macaudio J700 variant links them to the codecs.
//!
//! The speaker front-end is copy-only: the LEAP consumes float32 and the
//! driver converts each S32 sample through the amplifier's full scale and
//! the "Speaker Playback Volume" control, which the card's volume lock holds
//! 20 dB down whenever no speaker-protection daemon owns it.
//!
//! Copyright (C) The Asahi Linux Contributors
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{
    arch::asm,
    mem,
    ptr,
    slice, //
};

use kernel::{
    bindings,
    c_str,
    device,
    device::property::FwNode,
    device::Core,
    dma::{
        Device as DmaDevice,
        DmaMask, //
    },
    module_platform_driver,
    new_spinlock,
    of,
    platform,
    prelude::*,
    soc::apple::aop::{
        from_fourcc,
        EPICService,
        ReportListener,
        SourceRing,
        AOP, //
    },
    str::CString,
    sync::{
        aref::ARef,
        atomic::{
            Atomic,
            Relaxed, //
        },
        Arc,
        SpinLock, //
    },
    types::{ForeignOwnable, Opaque},
    workqueue::{
        self,
        impl_has_work,
        new_work,
        Work,
        WorkItem, //
    }, //
};

const EPIC_SUBTYPE_WRAPPED_CALL: u16 = 0x20;
const EPIC_SUBTYPE_PRODUCER_REPORT: u16 = 0x20;
const CALLTYPE_AUDIO_ATTACH_DEVICE: u32 = 0xc3000002;
const CALLTYPE_AUDIO_GET_PROP: u32 = 0xc3000004;
const CALLTYPE_AUDIO_SET_PROP: u32 = 0xc3000005;
const AUDIO_DEV_LPAI: u32 = from_fourcc(b"lpai");
const PROP_POWER_STATE: u32 = 200;
const PROP_POWER_REQUEST: u32 = 202;
const PROP_CHANNEL_CONTROL: u32 = 300;
const PROP_STREAM_FORMAT: u32 = 301;
const PROP_RING_GEOMETRY: u32 = 302;
/// Output services: 4-byte boolean, 1 mutes (with the firmware's ramp).
const PROP_OUTPUT_MUTE: u32 = 700;
const POWER_STATE_RUN: u32 = from_fourcc(b"runn");
const POWER_STATE_IDLE: u32 = from_fourcc(b"idle");

const AUDIO_DEV_SPKR: u32 = from_fourcc(b"spkr");
const AUDIO_DEV_TAP: u32 = from_fourcc(b"tap ");
const AUDIO_DEV_COUT: u32 = from_fourcc(b"cout");
const AUDIO_DEV_CIN: u32 = from_fourcc(b"cin ");
/// The high-quality microphone array (ADT /arm-io/iop-audio-controller/
/// audio-hp-mic, identifier 'iaph' -- ADT FourCCs are byte-reversed).  Unlike
/// `lpai` it is the machine's only `iop-audio-s` stream: its data comes back
/// over leap-s ADMAC RX0, not the AOP source ring, and macOS reaches it
/// through the HPMicAudioDriver exclave rather than this EPIC service.
const AUDIO_DEV_HPAI: u32 = from_fourcc(b"hpai");
/// kIOReturnBusy from the firmware: the device is already attached.
const AUDIO_RET_BUSY: u32 = 0xe00002d5;
const SPKR_POWER_STATE_PW0: u32 = from_fourcc(b"pw0 ");
const SPKR_POWER_STATE_PWRD: u32 = from_fourcc(b"pwrd");
const RATE: u32 = 16000;
const CHANNELS: u32 = 2;
const FRAME_BYTES: usize = 8;
const RING_BYTES: usize = 0x80000;
const PERIOD_FRAMES: usize = 3200;
const PERIOD_BYTES: usize = PERIOD_FRAMES * FRAME_BYTES;
const PERIODS_MAX: u32 = 16;
const REPORT_MIN_LEN: usize = 0x68;

// Firmware power timestamps use the 24 MHz always-on timebase. The
// architectural counter can run at a different rate (1 GHz on T8140).
const AOP_TIMEBASE_HZ: u64 = 24_000_000;

fn aop_counter_ticks(counter: u64, frequency: u64) -> Result<u64> {
    if frequency == 0 || frequency > u32::MAX as u64 {
        return Err(EINVAL);
    }
    // Split whole seconds from the remainder to avoid overflowing a u64
    // multiplication on an otherwise ordinary long-running system.
    let seconds = counter / frequency;
    let remainder = (counter % frequency) * AOP_TIMEBASE_HZ / frequency;
    seconds
        .checked_mul(AOP_TIMEBASE_HZ)
        .and_then(|ticks| ticks.checked_add(remainder))
        .ok_or(EOVERFLOW)
}

fn aop_timestamp() -> Result<u64> {
    let counter: u64;
    let frequency: u64;
    // SAFETY: architectural read-only timer registers; no private EL2
    // register access is needed for the firmware's timestamp domain.
    unsafe {
        asm!("isb", "mrs {counter}, cntvct_el0", "mrs {frequency}, cntfrq_el0",
             counter = out(reg) counter, frequency = out(reg) frequency,
             options(nostack, preserves_flags));
    }
    aop_counter_ticks(counter, frequency)
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

impl AudioAttachDevice {
    fn new(dev_id: u32) -> AudioAttachDevice {
        AudioAttachDevice {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_ATTACH_DEVICE,
            len: 0x2c,
            dev_id,
            ..AudioAttachDevice::default()
        }
    }
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

impl AudioGetDeviceProp {
    fn new(dev_id: u32, modifier: u32) -> AudioGetDeviceProp {
        AudioGetDeviceProp {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_GET_PROP,
            len: 0x30,
            dev_id,
            modifier,
            unk6: 1,
            ..AudioGetDeviceProp::default()
        }
    }
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

impl<T: Copy + Default> AudioSetDeviceProp<T> {
    fn new(dev_id: u32, modifier: u32, data: T) -> AudioSetDeviceProp<T> {
        AudioSetDeviceProp {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_SET_PROP,
            len: mem::size_of::<T>() as u64 + 0x30,
            dev_id,
            modifier,
            len2: mem::size_of::<T>() as u32,
            data,
            ..AudioSetDeviceProp::default()
        }
    }
}

/// Property 300: supported / enabled / history channel masks.
#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct ChannelControl {
    supported: u32,
    enabled: u32,
    history: u32,
}

/// Property 202, the power request: the device (the FourCC for the output
/// services, 0 for lpai), a per-device request sequence, the request time
/// in the firmware's 24 MHz timebase, the target state FourCC and a
/// parameter (1 for lpai, 0 for the output services).
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

impl PowerRequest {
    fn new(dev_id: u32, sequence: u32, state: u32, param: u32) -> Result<PowerRequest> {
        Ok(PowerRequest {
            dev_id,
            sequence,
            timestamp: aop_timestamp()?,
            state,
            param,
            ..PowerRequest::default()
        })
    }
}

/// One firmware audio service driven as an ASoC back-end: attached once,
/// requested into `pw0 ` (idle) at startup and `pwrd` (running) when the
/// stream starts, back to `pw0 ` at shutdown, with its serializer's PMGR
/// leaves runtime-powered for the stream's lifetime.
struct Service {
    dev_id: u32,
    name: &'static str,
    /// Request sequence of the property 202 messages.
    sequence: Atomic<u32>,
    /// In `pwrd`.
    running: Atomic<u32>,
}

impl Service {
    const fn new(dev_id: u32, name: &'static str) -> Service {
        Service {
            dev_id,
            name,
            sequence: Atomic::new(0),
            running: Atomic::new(0),
        }
    }
}

/// The low-power microphone stream, guarded by a spinlock that is never taken
/// with the ALSA stream lock held (trigger only touches the atomic run
/// flag), so the report path may call snd_pcm_period_elapsed() while holding
/// it and close cannot free the substream under a report.
struct StreamState {
    substream: *mut bindings::snd_pcm_substream,
    /// absolute producer byte count at the last report
    last_absolute: Option<u64>,
    /// absolute producer byte count already copied into the ALSA buffer
    copied: Option<u64>,
    hw_ptr_bytes: usize,
    period_acc: usize,
    buffer_bytes: usize,
    overruns: u32,
}

unsafe impl Send for StreamState {}

#[pin_data]
struct SndSocT8140AopData {
    dev: ARef<device::Device>,
    fwnode: ARef<FwNode>,
    adata: Arc<dyn AOP>,
    service: EPICService,
    ring: Arc<SourceRing>,
    ring_bytes: Atomic<u32>,
    sequence: Atomic<u32>,
    powered: Atomic<u32>,
    /// The microphone stream is triggered (set and cleared under the ALSA
    /// stream lock, read by the report path).
    running: Atomic<u32>,
    hw_ptr_frames: Atomic<u32>,
    /// One warning per producer-report fault class and driver instance.
    report_warnings: [Atomic<u32>; 5],
    /// The back-end services: the jack's output and headset microphone,
    /// the speakers and their sense.
    cout: Service,
    cin: Service,
    spkr: Service,
    tap: Service,
    /// The high-quality microphone array (leap-s ADMAC RX0).
    hpai: Service,
    /// Virtual power-domain devices (dev_pm_domain_attach_by_name) of the
    /// serializer leaves, held for the driver's lifetime and runtime-powered
    /// around streams; 0 if absent: audio_mca1_m (jack), audio_mca0_m
    /// (sense), audio_leap_tx0 and audio_leap_mca (speakers).
    pd_mca1: Atomic<usize>,
    pd_mca0: Atomic<usize>,
    pd_tx0: Atomic<usize>,
    pd_leap_mca: Atomic<usize>,
    /// audio_leap_rx0: the microphone array's leap-s RX0 leaf, the exact
    /// sibling of `pd_tx0`.  A LEAP ADMAC only answers on a channel whose own
    /// leaf is powered.
    pd_rx0: Atomic<usize>,
    /// A speaker stream has been triggered and not yet stopped.
    spkr_want_run: Atomic<u32>,
    /// A HQ-mic stream has been triggered and not yet stopped.
    hpai_want_run: Atomic<u32>,
    /// "Speaker Playback Volume" (REG_SPKR_VOLUME), 0..=SPKR_VOLUME_MAX.
    spkr_volume: Atomic<u32>,
    /// DMA channels of the front-end PCMs, [FE DAI id][stream] (raw dma_chan pointers).
    #[pin]
    fe_chans: SpinLock<[[usize; 2]; FE_COUNT]>,
    #[pin]
    stream: SpinLock<StreamState>,
}

unsafe impl Send for SndSocT8140AopData {}
unsafe impl Sync for SndSocT8140AopData {}

impl SndSocT8140AopData {
    fn epic_wrapped_call<T>(&self, data: &T) -> Result<u32> {
        // SAFETY: T is a plain packed firmware record.
        let msg_bytes =
            unsafe { slice::from_raw_parts(data as *const T as *const u8, mem::size_of::<T>()) };
        self.adata
            .epic_call(&self.service, EPIC_SUBTYPE_WRAPPED_CALL, msg_bytes)
    }

    /// The raw form: hands back the firmware's return code with the reply and
    /// logs nothing, so a caller that expects some calls to be refused (a
    /// property sweep) does not fill the log with errors.
    fn epic_wrapped_call_ret_code<T>(&self, data: &T, ret_len: usize) -> Result<(u32, KVec<u8>)> {
        // SAFETY: T is a plain packed firmware record.
        let msg_bytes =
            unsafe { slice::from_raw_parts(data as *const T as *const u8, mem::size_of::<T>()) };
        self.adata
            .epic_call_ret(&self.service, EPIC_SUBTYPE_WRAPPED_CALL, msg_bytes, ret_len)
    }

    fn epic_wrapped_call_ret<T>(&self, data: &T, ret_len: usize) -> Result<KVec<u8>> {
        let (retcode, ret) = self.epic_wrapped_call_ret_code(data, ret_len)?;
        if retcode != 0 {
            // Not "lpai": this helper serves every device on the service, and
            // naming one of them made an hpai failure read as an lpai failure.
            dev_err!(
                self.dev,
                "audio wrapped call failed, return code {:#x}\n",
                retcode
            );
            return Err(EIO);
        }
        Ok(ret)
    }

    /// Attach an audio device.  The firmware keeps attachments for its
    /// lifetime (there is no detach in the protocol we know), so a reload of
    /// this module sees kIOReturnBusy for a device it attached earlier.
    fn attach_device(&self, dev_id: u32, name: &str) -> Result<()> {
        let ret = self.epic_wrapped_call(&AudioAttachDevice::new(dev_id))?;
        match ret {
            0 => Ok(()),
            AUDIO_RET_BUSY => {
                dev_dbg!(
                    self.dev,
                    "{} already attached by an earlier instance\n",
                    name
                );
                Ok(())
            }
            _ => {
                dev_err!(
                    self.dev,
                    "unable to attach {}, return code {:#x}\n",
                    name,
                    ret
                );
                Err(EIO)
            }
        }
    }

    fn attach_lpai(&self) -> Result<()> {
        self.attach_device(AUDIO_DEV_LPAI, "lpai")
    }

    fn set_channel_control(&self) -> Result<()> {
        let ctl = ChannelControl {
            supported: 3,
            enabled: 3,
            history: 3,
        };
        let ret = self.epic_wrapped_call(&AudioSetDeviceProp::new(
            AUDIO_DEV_LPAI,
            PROP_CHANNEL_CONTROL,
            ctl,
        ))?;
        if ret != 0 {
            dev_err!(
                self.dev,
                "unable to set lpai channels, return code {:#x}\n",
                ret
            );
            return Err(EIO);
        }
        Ok(())
    }

    /// Reply layout after the retcode: u32 length, then the property bytes.
    fn get_prop(&self, modifier: u32, len: usize) -> Result<KVec<u8>> {
        let ret = self
            .epic_wrapped_call_ret(&AudioGetDeviceProp::new(AUDIO_DEV_LPAI, modifier), 4 + len)?;
        if ret.len() < 4 + len {
            dev_err!(
                self.dev,
                "lpai property {} reply too short ({})\n",
                modifier,
                ret.len()
            );
            return Err(EIO);
        }
        let reported = u32::from_le_bytes(ret[..4].try_into().unwrap()) as usize;
        if reported != len {
            dev_err!(
                self.dev,
                "lpai property {} length {} (expected {})\n",
                modifier,
                reported,
                len
            );
            return Err(EIO);
        }
        Ok(ret)
    }

    fn read_geometry(&self) -> Result<()> {
        // 302: FourCC/pad, ring frames u64, report frames u64
        let geo = self.get_prop(PROP_RING_GEOMETRY, 24)?;
        let frames = u64::from_le_bytes(geo[12..20].try_into().unwrap());
        let quantum = u64::from_le_bytes(geo[20..28].try_into().unwrap());
        if frames == 0
            || frames > (RING_BYTES / FRAME_BYTES) as u64
            || quantum == 0
            || quantum > frames
        {
            dev_err!(
                self.dev,
                "invalid lpai ring geometry: {} frames, {} per report\n",
                frames,
                quantum
            );
            return Err(EIO);
        }
        self.ring_bytes
            .store((frames as usize * FRAME_BYTES) as u32, Relaxed);
        // 301: stream format, sample rate at +4
        let fmt = self.get_prop(PROP_STREAM_FORMAT, 16)?;
        let rate = u32::from_le_bytes(fmt[8..12].try_into().unwrap());
        if rate != RATE {
            dev_err!(self.dev, "unexpected lpai sample rate {}\n", rate);
            return Err(EIO);
        }
        dev_dbg!(
            self.dev,
            "lpai: {} Hz, ring {} frames ({} bytes), {} frames per report\n",
            rate,
            frames,
            frames * FRAME_BYTES as u64,
            quantum
        );
        Ok(())
    }

    fn set_power(&self, state: u32) -> Result<()> {
        let sequence = self.sequence.fetch_add(1, Relaxed) + 1;
        let req = PowerRequest::new(0, sequence, state, 1)?;
        let ret = self.epic_wrapped_call(&AudioSetDeviceProp::new(
            AUDIO_DEV_LPAI,
            PROP_POWER_REQUEST,
            req,
        ))?;
        if ret != 0 {
            dev_err!(
                self.dev,
                "lpai power request {:#x} rejected: {:#x}\n",
                state,
                ret
            );
            return Err(EIO);
        }
        let st = self.get_prop(PROP_POWER_STATE, 4)?;
        let now = u32::from_le_bytes(st[4..8].try_into().unwrap());
        if now != state {
            dev_err!(
                self.dev,
                "lpai power state {:#x} after requesting {:#x}\n",
                now,
                state
            );
            return Err(EIO);
        }
        Ok(())
    }

    // ---- the back-end services ----

    /// Property 202 with the service's sequence and a 24 MHz timestamp;
    /// `pw0 ` is confirmed through property 200 like the native host does.
    fn service_set_power(&self, svc: &Service, state: u32) -> Result<()> {
        let sequence = svc.sequence.fetch_add(1, Relaxed) + 1;
        let req = PowerRequest::new(svc.dev_id, sequence, state, 0)?;
        let ret = self.epic_wrapped_call(&AudioSetDeviceProp::new(
            svc.dev_id,
            PROP_POWER_REQUEST,
            req,
        ))?;
        if ret != 0 {
            dev_err!(
                self.dev,
                "{} power request {:#x} rejected: {:#x}\n",
                svc.name,
                state,
                ret
            );
            return Err(EIO);
        }
        if state == SPKR_POWER_STATE_PW0 {
            let ret = self
                .epic_wrapped_call_ret(&AudioGetDeviceProp::new(svc.dev_id, PROP_POWER_STATE), 8)?;
            if ret.len() < 8 {
                return Err(EIO);
            }
            let now = u32::from_le_bytes(ret[4..8].try_into().unwrap());
            if now != state {
                dev_err!(
                    self.dev,
                    "{} power state {:#x} after requesting {:#x}\n",
                    svc.name,
                    now,
                    state
                );
                return Err(EIO);
            }
        }
        Ok(())
    }

    /// Runtime-power (or release) serializer leaves, parents first on the
    /// way up.
    fn domains(&self, svc: &Service, pds: &[&Atomic<usize>], on: bool) -> Result<()> {
        if pds.iter().any(|pd| pd.load(Relaxed) == 0) {
            dev_err!(
                self.dev,
                "{}: its power domains are not attached\n",
                svc.name
            );
            return Err(ENODEV);
        }
        if on {
            for (i, pd) in pds.iter().enumerate() {
                let dev = pd.load(Relaxed) as *mut bindings::device;
                // SAFETY: the virtual domain devices live until Drop detaches them.
                let mut rc = unsafe { bindings::pm_runtime_get_sync(dev) };
                if rc < 0 {
                    // A leaf the firmware also manages was once seen missing
                    // the PMGR driver's poll window on its first power-up
                    // after boot; a failed resume leaves the virtual device
                    // in the runtime-PM error state, so clear that and try
                    // once more before giving up.
                    dev_warn!(
                        self.dev,
                        "{}: power domain {} did not power up: {}, retrying\n",
                        svc.name,
                        i,
                        rc
                    );
                    // SAFETY: `dev` is a power-domain device attached at probe and detached only in
                    // `Drop`.
                    unsafe {
                        bindings::pm_runtime_put_noidle(dev);
                        bindings::__pm_runtime_set_status(
                            dev,
                            bindings::rpm_status_RPM_SUSPENDED as u32,
                        );
                    }
                    kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
                    // SAFETY: `dev` is a power-domain device attached at probe and detached only in
                    // `Drop`.
                    rc = unsafe { bindings::pm_runtime_get_sync(dev) };
                }
                if rc < 0 {
                    dev_err!(
                        self.dev,
                        "{}: power domain {} did not power up: {}\n",
                        svc.name,
                        i,
                        rc
                    );
                    // SAFETY: `dev` is a power-domain device attached at probe and detached only in
                    // `Drop`.
                    unsafe { bindings::pm_runtime_put_noidle(dev) };
                    for done in pds[..i].iter() {
                        // SAFETY: `done` holds a power-domain device attached at probe; the get
                        // above succeeded for it.
                        unsafe {
                            bindings::pm_runtime_put_sync(
                                done.load(Relaxed) as *mut bindings::device
                            )
                        };
                    }
                    return Err(EIO);
                }
            }
        } else {
            for pd in pds.iter().rev() {
                // SAFETY: as above.
                unsafe { bindings::pm_runtime_put_sync(pd.load(Relaxed) as *mut bindings::device) };
            }
        }
        Ok(())
    }

    /// Back-end startup: attach the service, power its leaves, `pw0 `.
    fn service_startup(&self, svc: &Service, pds: &[&Atomic<usize>]) -> Result<()> {
        self.attach_device(svc.dev_id, svc.name)?;
        self.domains(svc, pds, true)?;
        if let Err(e) = self.service_set_power(svc, SPKR_POWER_STATE_PW0) {
            let _ = self.domains(svc, pds, false);
            return Err(e);
        }
        svc.running.store(0, Relaxed);
        dev_dbg!(self.dev, "{}: attached, powered, pw0\n", svc.name);
        Ok(())
    }

    /// `pwrd`: the firmware starts the serializer clocks and the stream.
    fn service_run(&self, svc: &Service) -> Result<()> {
        if svc.running.load(Relaxed) != 0 {
            return Ok(());
        }
        self.service_set_power(svc, SPKR_POWER_STATE_PWRD)?;
        svc.running.store(1, Relaxed);
        dev_dbg!(self.dev, "{}: pwrd\n", svc.name);
        Ok(())
    }

    /// Back-end shutdown: `pw0 ` if running, then the leaves.
    fn service_shutdown(&self, svc: &Service, pds: &[&Atomic<usize>]) {
        if svc.running.load(Relaxed) != 0 {
            if let Err(e) = self.service_set_power(svc, SPKR_POWER_STATE_PW0) {
                dev_err!(self.dev, "{}: pw0 failed: {:?}\n", svc.name, e);
            }
            svc.running.store(0, Relaxed);
        }
        let _ = self.domains(svc, pds, false);
        dev_dbg!(self.dev, "{}: pw0, released\n", svc.name);
    }

    // ---- 3.5 mm jack: cout (MCA1, base-ns TX2) and cin (the headset
    // microphone, base-ns RX2), one MCA1 power reference per open direction ----

    fn jack_service(&self, playback: bool) -> &Service {
        if playback {
            &self.cout
        } else {
            &self.cin
        }
    }

    fn jack_startup(&self, playback: bool) -> Result<()> {
        self.service_startup(self.jack_service(playback), &[&self.pd_mca1])
    }

    /// pwrd starts the MCA1 clocks; the codec's PLL locks to them when ASoC
    /// unmutes it right after this.
    fn jack_prepare(&self, playback: bool) -> Result<()> {
        self.service_run(self.jack_service(playback))
    }

    fn jack_shutdown(&self, playback: bool) {
        self.service_shutdown(self.jack_service(playback), &[&self.pd_mca1])
    }

    // ---- speaker sense: the native SpeakerTap, base-ns RX1 under the tap
    // service on MCA0.  The words the amplifier receives come back on this
    // stream (the card's "Speaker Sense" capture, device 2): it observes the
    // serial data, it senses no current or voltage. ----

    fn sense_startup(&self) -> Result<()> {
        self.service_startup(&self.tap, &[&self.pd_mca0])
    }

    fn sense_prepare(&self) -> Result<()> {
        self.service_run(&self.tap)
    }

    fn sense_shutdown(&self) {
        self.service_shutdown(&self.tap, &[&self.pd_mca0])
    }

    // ---- the high-quality microphone array: the hpai service feeding
    // leap-s ADMAC RX0. The host holds its RX0 leaf and the shared LEAP MCA
    // domain while the firmware service owns the microphone fabric. ----

    /// The array's LEAP leaves: its own RX0 channel leaf, plus the LEAP MCA
    /// the speaker path also holds.
    fn hpai_domain_refs(&self) -> [&Atomic<usize>; 2] {
        [&self.pd_rx0, &self.pd_leap_mca]
    }

    fn hpai_startup(&self) -> Result<()> {
        self.service_startup(&self.hpai, &self.hpai_domain_refs())
    }

    /// Deliberately does NOT start the array.  `prepare` runs before the ALSA
    /// trigger, i.e. before `snd_dmaengine_pcm_trigger` has started the ADMAC,
    /// and `pwrd` makes the LEAP start *producing* immediately.  Samples the
    /// array emits into a ring the DMA is not yet servicing are lost, and the
    /// capture returns whatever was in the buffer for its whole duration --
    /// measured at about one capture in three being pure noise, all-or-nothing.
    /// The start moves to `hpai_go`, queued from trigger START.  This is the
    /// capture mirror of the speaker rule: the DMA must be running before the
    /// LEAP starts moving samples.
    fn hpai_prepare(&self) -> Result<()> {
        Ok(())
    }

    /// HQ-mic start (`pwrd`), from the system workqueue right after trigger
    /// START: a sleeping firmware call, so it cannot run in the atomic trigger.
    fn hpai_go(&self) -> Result<()> {
        if self.hpai_want_run.load(Relaxed) == 0 || self.hpai.running.load(Relaxed) != 0 {
            return Ok(());
        }
        self.service_run(&self.hpai)
    }

    fn hpai_shutdown(&self) {
        self.hpai_want_run.store(0, Relaxed);
        self.service_shutdown(&self.hpai, &self.hpai_domain_refs())
    }

    // ---- internal speakers: the spkr service on LEAP TX0 (leap-ns ADMAC
    // channel 0); the leaves are tx0 (-> leap_c -> leap_a -> audio_fr ->
    // audio_a), leap_mca (-> audio_p) and the MCA0 serializer the LEAP
    // drives the amplifiers through -- shared with the sense capture, so
    // both hold it and neither can power it off under the other ----

    /// Output mute (property 700) is independent of the amplifier's SD_MODE;
    /// the amplifier stays off across a failure here.
    fn spkr_set_mute(&self, muted: bool) -> Result<()> {
        let ret = self.epic_wrapped_call(&AudioSetDeviceProp::new(
            AUDIO_DEV_SPKR,
            PROP_OUTPUT_MUTE,
            muted as u32,
        ))?;
        if ret != 0 {
            dev_err!(self.dev, "speaker mute request rejected: {:#x}\n", ret);
            return Err(EIO);
        }
        Ok(())
    }

    fn spkr_domain_refs(&self) -> [&Atomic<usize>; 3] {
        [&self.pd_tx0, &self.pd_leap_mca, &self.pd_mca0]
    }

    fn spkr_startup(&self) -> Result<()> {
        self.service_startup(&self.spkr, &self.spkr_domain_refs())
    }

    /// A PCM underrun stops DMA without closing the back-end. Retire its
    /// previous firmware run during prepare so the next trigger queues a
    /// fresh pwrd and unmute after DMA has restarted.
    fn spkr_prepare(&self) -> Result<()> {
        if self.spkr.running.load(Relaxed) == 0 {
            return Ok(());
        }
        self.spkr_set_mute(true)?;
        self.service_set_power(&self.spkr, SPKR_POWER_STATE_PW0)?;
        self.spkr.running.store(0, Relaxed);
        Ok(())
    }

    /// Speaker stream start, from the system workqueue right after trigger
    /// START: the DMA must be running before pwrd starts the LEAP consuming
    /// (issued from prepare, before the DMA, the wire stays silent), then the
    /// firmware unmute, the value the native amplifier driver writes after
    /// enabling its output.
    fn spkr_go(&self) -> Result<()> {
        if self.spkr_want_run.load(Relaxed) == 0 || self.spkr.running.load(Relaxed) != 0 {
            return Ok(());
        }
        self.service_run(&self.spkr)?;
        if let Err(e) = self.spkr_set_mute(false) {
            let _ = self.service_set_power(&self.spkr, SPKR_POWER_STATE_PW0);
            self.spkr.running.store(0, Relaxed);
            return Err(e);
        }
        Ok(())
    }

    /// Speaker back-end shutdown: mute, then the service; the amplifier is
    /// already off (codec trigger STOP) and a queued start was flushed.
    fn spkr_shutdown(&self) {
        self.spkr_want_run.store(0, Relaxed);
        if self.spkr.running.load(Relaxed) != 0 {
            if let Err(e) = self.spkr_set_mute(true) {
                dev_err!(self.dev, "spkr: mute failed: {:?}\n", e);
            }
        }
        self.service_shutdown(&self.spkr, &self.spkr_domain_refs());
    }

    fn release_fe_channel(&self, fe: usize, stream: usize) {
        let chan = core::mem::replace(&mut self.fe_chans.lock()[fe][stream], 0);
        if chan == 0 {
            return;
        }
        // SAFETY: the successful request owns this channel and, for LEAP,
        // its register-domain references until release has completed.
        unsafe { bindings::dma_release_channel(chan as *mut bindings::dma_chan) };
        if fe == FE_SPKR {
            let _ = self.domains(&self.spkr, &self.spkr_domain_refs(), false);
        } else if fe == FE_HPAI {
            let _ = self.domains(&self.hpai, &self.hpai_domain_refs(), false);
        }
    }

    fn ring_slice(&self, off: usize, len: usize) -> &[u8] {
        // SAFETY: the ring is a coherent allocation of RING_BYTES; callers
        // keep off + len within the active ring.
        unsafe { &self.ring.buf.as_ref()[off..off + len] }
    }
}

/// Copy `len` bytes from the source ring (absolute producer offset `from`)
/// into the ALSA buffer at `hw_ptr`, both wrapping.
fn copy_span(
    data: &SndSocT8140AopData,
    dma_area: *mut u8,
    buffer_bytes: usize,
    ring_bytes: usize,
    from: u64,
    len: usize,
    hw_ptr: usize,
) -> usize {
    let mut src = (from % ring_bytes as u64) as usize;
    let mut dst = hw_ptr;
    let mut left = len;
    while left > 0 {
        let n = left.min(ring_bytes - src).min(buffer_bytes - dst);
        let s = data.ring_slice(src, n);
        // SAFETY: dst + n <= buffer_bytes, the ALSA buffer stays mapped while
        // the substream is set in the stream state.
        unsafe { ptr::copy_nonoverlapping(s.as_ptr(), dma_area.add(dst), n) };
        src = (src + n) % ring_bytes;
        dst = (dst + n) % buffer_bytes;
        left -= n;
    }
    dst
}

impl ReportListener for SndSocT8140AopData {
    fn process_report(&self, _subtype: u16, report: &[u8]) -> Result<()> {
        if report.len() < REPORT_MIN_LEN {
            if self.report_warnings[0].xchg(1, Relaxed) == 0 {
                dev_warn!(
                    self.dev,
                    "short lpai producer report ({} bytes)\n",
                    report.len()
                );
            }
            return Ok(());
        }
        let count = u64::from_le_bytes(report[0x40..0x48].try_into().unwrap());
        let Some(absolute) = count
            .checked_add(1)
            .and_then(|frames| frames.checked_mul(FRAME_BYTES as u64))
        else {
            if self.report_warnings[1].xchg(1, Relaxed) == 0 {
                dev_warn!(self.dev, "overflowing lpai producer counter\n");
            }
            return Ok(());
        };
        let frames = absolute / FRAME_BYTES as u64;
        let cursor = u64::from_le_bytes(report[0x60..0x68].try_into().unwrap());
        let ring_bytes = self.ring_bytes.load(Relaxed) as usize;
        if ring_bytes == 0 || cursor != absolute % ring_bytes as u64 {
            if self.report_warnings[2].xchg(1, Relaxed) == 0 {
                dev_warn!(
                    self.dev,
                    "inconsistent lpai report: frames {} cursor {:#x}\n",
                    frames,
                    cursor
                );
            }
            return Ok(());
        }

        let mut st = self.stream.lock();
        st.last_absolute = Some(absolute);
        if self.running.load(Relaxed) == 0 || st.substream.is_null() {
            return Ok(());
        }
        let copied = match st.copied {
            Some(c) => c,
            None => {
                // start at a completed producer position, not at ring zero
                st.copied = Some(absolute);
                return Ok(());
            }
        };
        if absolute < copied {
            if self.report_warnings[3].xchg(1, Relaxed) == 0 {
                dev_warn!(self.dev, "lpai producer moved backwards\n");
            }
            st.copied = Some(absolute);
            return Ok(());
        }
        let delta = (absolute - copied) as usize;
        if delta >= ring_bytes {
            st.overruns = st.overruns.saturating_add(1);
            st.copied = Some(absolute);
            if self.report_warnings[4].xchg(1, Relaxed) == 0 {
                dev_warn!(self.dev, "lpai source overrun ({} bytes)\n", delta);
            }
            return Ok(());
        }
        if delta == 0 {
            return Ok(());
        }
        // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for
        // the op's duration (between hw_params and hw_free for the buffer).
        let runtime = unsafe { (*st.substream).runtime };
        // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for
        // the op's duration (between hw_params and hw_free for the buffer).
        let dma_area = unsafe { (*runtime).dma_area };
        let buffer_bytes = st.buffer_bytes;
        let new_ptr = copy_span(
            self,
            dma_area,
            buffer_bytes,
            ring_bytes,
            copied,
            delta,
            st.hw_ptr_bytes,
        );
        st.hw_ptr_bytes = new_ptr;
        st.copied = Some(absolute);
        st.period_acc += delta;
        self.hw_ptr_frames
            .store((new_ptr / FRAME_BYTES) as u32, Relaxed);
        if st.period_acc >= PERIOD_BYTES {
            st.period_acc %= PERIOD_BYTES;
            // SAFETY: the substream is valid while it is registered in
            // the stream state, which close clears under this lock.
            unsafe { bindings::snd_pcm_period_elapsed(st.substream) };
        }
        Ok(())
    }
}

/// The speaker stream start (pwrd + unmute), queued by the back-end DAI's
/// trigger START: those are sleeping firmware calls, and the DMA must already
/// be running when the LEAP starts consuming.
#[pin_data]
struct SpkrStartWork {
    data: Arc<SndSocT8140AopData>,
    #[pin]
    work: Work<SpkrStartWork>,
}

impl_has_work! {
    impl HasWork<Self> for SpkrStartWork { self.work }
}

impl WorkItem for SpkrStartWork {
    type Pointer = Arc<SpkrStartWork>;

    fn run(this: Arc<SpkrStartWork>) {
        if let Err(e) = this.data.spkr_go() {
            dev_err!(this.data.dev, "spkr: start failed: {:?}\n", e);
        }
    }
}

/// The HQ-mic stream start (`pwrd`), queued by the back-end DAI's trigger
/// START for the same reason as the speaker's: the ADMAC must already be
/// running before the LEAP starts producing.
#[pin_data]
struct HpaiStartWork {
    data: Arc<SndSocT8140AopData>,
    #[pin]
    work: Work<HpaiStartWork>,
}

impl_has_work! {
    impl HasWork<Self> for HpaiStartWork { self.work }
}

impl WorkItem for HpaiStartWork {
    type Pointer = Arc<HpaiStartWork>;

    fn run(this: Arc<HpaiStartWork>) {
        if let Err(e) = this.data.hpai_go() {
            dev_err!(this.data.dev, "hpai: start failed: {:?}\n", e);
        }
    }
}

unsafe extern "C" fn lpai_pcm_open(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: the PCM's private data is the `Arc` handed to ALSA at probe, released in
    // `lpai_pcm_free_private` after the last op.
    let data = unsafe { Arc::<SndSocT8140AopData>::borrow((*substream).private_data.cast()) };
    let hw = bindings::snd_pcm_hardware {
        info: bindings::SNDRV_PCM_INFO_MMAP
            | bindings::SNDRV_PCM_INFO_MMAP_VALID
            | bindings::SNDRV_PCM_INFO_INTERLEAVED,
        formats: bindings::BINDINGS_SNDRV_PCM_FMTBIT_S32_LE,
        subformats: 0,
        rates: bindings::SNDRV_PCM_RATE_16000,
        rate_min: RATE,
        rate_max: RATE,
        channels_min: CHANNELS,
        channels_max: CHANNELS,
        buffer_bytes_max: PERIOD_BYTES * PERIODS_MAX as usize,
        period_bytes_min: PERIOD_BYTES,
        period_bytes_max: PERIOD_BYTES,
        periods_min: 2,
        periods_max: PERIODS_MAX,
        fifo_size: 0,
    };
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    unsafe {
        (*(*substream).runtime).hw = hw;
    }
    data.running.store(0, Relaxed);
    let mut st = data.stream.lock();
    st.substream = substream;
    st.copied = None;
    st.hw_ptr_bytes = 0;
    st.period_acc = 0;
    data.hw_ptr_frames.store(0, Relaxed);
    0
}

unsafe extern "C" fn lpai_pcm_close(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: the PCM's private data is the `Arc` handed to ALSA at probe, released in
    // `lpai_pcm_free_private` after the last op.
    let data = unsafe { Arc::<SndSocT8140AopData>::borrow((*substream).private_data.cast()) };
    data.running.store(0, Relaxed);
    {
        let mut st = data.stream.lock();
        st.substream = ptr::null_mut();
    }
    if data.powered.load(Relaxed) != 0 {
        if let Err(e) = data.set_power(POWER_STATE_IDLE) {
            dev_err!(data.dev, "unable to return lpai to idle\n");
            return e.to_errno();
        }
        data.powered.store(0, Relaxed);
    }
    0
}

unsafe extern "C" fn lpai_pcm_prepare(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: the PCM's private data is the `Arc` handed to ALSA at probe, released in
    // `lpai_pcm_free_private` after the last op.
    let data = unsafe { Arc::<SndSocT8140AopData>::borrow((*substream).private_data.cast()) };
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    let runtime = unsafe { (*substream).runtime };
    {
        let mut st = data.stream.lock();
        // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for
        // the op's duration (between hw_params and hw_free for the buffer).
        st.buffer_bytes = unsafe { (*runtime).dma_bytes };
        st.hw_ptr_bytes = 0;
        st.period_acc = 0;
        st.copied = None;
        data.hw_ptr_frames.store(0, Relaxed);
    }
    if data.powered.load(Relaxed) == 0 {
        if let Err(e) = data.set_power(POWER_STATE_RUN) {
            dev_err!(data.dev, "unable to run lpai\n");
            return e.to_errno();
        }
        data.powered.store(1, Relaxed);
    }
    0
}

/// Called with the ALSA stream lock held: only the run flag changes here,
/// the counters were reset by prepare.
unsafe extern "C" fn lpai_pcm_trigger(
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
) -> i32 {
    // SAFETY: the PCM's private data is the `Arc` handed to ALSA at probe, released in
    // `lpai_pcm_free_private` after the last op.
    let data = unsafe { Arc::<SndSocT8140AopData>::borrow((*substream).private_data.cast()) };
    match cmd as u32 {
        bindings::SNDRV_PCM_TRIGGER_START | bindings::SNDRV_PCM_TRIGGER_RESUME => {
            data.running.store(1, Relaxed);
            0
        }
        bindings::SNDRV_PCM_TRIGGER_STOP | bindings::SNDRV_PCM_TRIGGER_SUSPEND => {
            data.running.store(0, Relaxed);
            0
        }
        _ => EINVAL.to_errno(),
    }
}

unsafe extern "C" fn lpai_pcm_pointer(
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: the PCM's private data is the `Arc` handed to ALSA at probe, released in
    // `lpai_pcm_free_private` after the last op.
    let data = unsafe { Arc::<SndSocT8140AopData>::borrow((*substream).private_data.cast()) };
    data.hw_ptr_frames.load(Relaxed) as bindings::snd_pcm_uframes_t
}

unsafe extern "C" fn lpai_pcm_free_private(pcm: *mut bindings::snd_pcm) {
    // SAFETY: this is the `Arc` leaked into the PCM's private data at probe; ALSA calls this once,
    // when the PCM goes away.
    unsafe {
        Arc::<SndSocT8140AopData>::from_foreign((*pcm).private_data.cast());
    }
}

fn copy_str(target: &mut [u8], source: &[u8]) {
    target[..source.len()].copy_from_slice(source)
}

// ---- ASoC component: DPCM front-end PCMs on the ADMAC channels, back-end
// DAIs on the AOP output services (macaudio's t8140 variant links them) ----

/// Front-end PCM DAIs ("j700-pcm-N"): 0 primary playback (the jack),
/// 1 secondary playback (the speaker, later), 2 capture (unused).
const FE_COUNT: usize = 4;
/// DMA channel names per front-end and stream (dma-names on the audio
/// child): [playback, capture].  The primary front-end is the jack: cout on
/// base-ns TX2, cin (the headset microphone) on base-ns RX2.  The speaker
/// front-end's "spkr" channel is requested once the speaker link exists:
/// requesting it touches the LEAP ADMAC.
const FE_DMA_NAMES: [[Option<&core::ffi::CStr>; 2]; FE_COUNT] = [
    [Some(c"cout"), Some(c"cin")],
    [Some(c"spkr"), None],
    [None, Some(c"spkr-tap")],
    // the HQ microphone array, leap-s ADMAC RX0
    [None, Some(c"hpai")],
];
/// The front-end that plays the internal speaker (j700-pcm-1, the card's
/// "Secondary" PCM): its data goes through `spkr_copy` below.
const FE_SPKR: usize = 1;
/// The microphone-array front-end (j700-pcm-3) is a plain capture link.
const FE_HPAI: usize = 3;
/// SNDRV_PCM_FMTBIT_S32_LE (format index 10).
const FMTBIT_S32_LE: u64 = 1 << 10;
/// The LEAP microphone DMA supplies IEEE 754 binary32 samples directly.
const FMTBIT_FLOAT_LE: u64 = 1 << 14;

/// Digital ceiling of the speaker path, as a Q31 gain applied to every
/// sample userspace writes: 0.5, the amplifier's full scale.
///
/// Measured at the SpeakerTap on 2026-09-17 (hb51, amp off), the words on the
/// speaker wire are exactly `2 × float × 2^31` -- a float of 6 791 000/2^31
/// came back as a clean 1 kHz sine with peak 13 582 000 and nothing else in
/// the path.  A float of 0.5 is therefore the I2S full scale (0 dBFS on the
/// wire, 7.3 V peak at the MAX98360A's output on the J700) and anything
/// above it would wrap.  Protection is the userspace daemon's job, exactly
/// as on the other Apple laptops: it owns "Speaker Playback Volume" and
/// governs it from the sense stream with the speaker's thermal model; when
/// no daemon holds the lock, macaudio keeps the control 20 dB down (the
/// level Apple's own bring-up profile of this speaker uses).
const SPKR_CEILING_Q31: i64 = 1 << 30;

/// "Speaker Playback Volume": the software gain below the ceiling that the
/// speaker-protection daemon and the card's volume lock drive, 0.5 dB per
/// step from -63.0 dB (1) to 0 dB (127) relative to SPKR_CEILING_Q31, 0 =
/// mute; entry i = round(SPKR_CEILING_Q31 * 10^(-(127 - i) / 40)).
const SPKR_VOLUME_MAX: u32 = 127;
const _: () = assert!(SPKR_VOLUME_Q31[SPKR_VOLUME_MAX as usize] as i64 == SPKR_CEILING_Q31);
const SPKR_VOLUME_Q31: [u32; 128] = [
    0, 760151, 805193, 852903, 903441, 956973, 1013678, 1073742, 1137365, 1204758, 1276145,
    1351761, 1431858, 1516701, 1606571, 1701766, 1802602, 1909413, 2022553, 2142397, 2269342,
    2403809, 2546243, 2697118, 2856932, 3026216, 3205530, 3395470, 3596664, 3809780, 4035523,
    4274643, 4527932, 4796229, 5080423, 5381457, 5700328, 6038094, 6395874, 6774853, 7176288,
    7601510, 8051928, 8529034, 9034412, 9569734, 10136776, 10737418, 11373650, 12047581, 12761445,
    13517609, 14318577, 15167006, 16065708, 17017661, 18026021, 19094130, 20225528, 21423966,
    22693416, 24038085, 25462431, 26971175, 28569318, 30262156, 32055302, 33954698, 35966640,
    38097798, 40355234, 42746432, 45279317, 47962285, 50804230, 53814569, 57003283, 60380940,
    63958736, 67748529, 71762882, 76015100, 80519278, 85290345, 90344115, 95697341, 101367765,
    107374182, 113736503, 120475814, 127614455, 135176087, 143185773, 151670064, 160657080,
    170176611, 180260209, 190941298, 202255281, 214239660, 226934158, 240380852, 254624313,
    269711752, 285693178, 302621563, 320553018, 339546978, 359666402, 380977976, 403552340,
    427464319, 452793173, 479622855, 508042296, 538145694, 570032831, 603809400, 639587356,
    677485290, 717628817, 760150998, 805192776, 852903448, 903441154, 956973408, 1013677647,
    1073741824,
];
/// The component's only "register": the speaker volume.
const REG_SPKR_VOLUME: u32 = 0;
/// SNDRV_CTL_ELEM_IFACE_MIXER (a __force-cast macro bindgen cannot import).
const SNDRV_CTL_ELEM_IFACE_MIXER: bindings::snd_ctl_elem_iface_t = 2;
/// DECLARE_TLV_DB_SCALE(-6350, 50, 1): SNDRV_CTL_TLVT_DB_SCALE, 8 bytes,
/// min -63.50 dB, 0.50 dB steps, the lowest value mutes.
static SPKR_VOLUME_TLV: [u32; 4] = [1, 8, (-6350i32) as u32, 50 | 0x10000];
/// Managed buffer sizes for the PCMs.
const PCM_BUFFER_PREALLOC: usize = 256 * 1024;
const PCM_BUFFER_MAX: usize = 1024 * 1024;
/// Copy-time gain changes must not sit behind seconds of queued speaker data.
/// At 48 kHz stereo S32 this bounds queued audio to 4096 frames (85.4 ms).
const SPKR_BUFFER_MAX: usize = 32 * 1024;
const RATE_48000_BIT: u32 = 1 << 7; // SNDRV_PCM_RATE_48000

/// Sync wrapper for the static ASoC descriptor tables (raw pointers inside).
struct SyncCell<T>(T);
unsafe impl<T> Sync for SyncCell<T> {}

/// Audio data from a live ASoC component's separately owned context.
///
/// # Safety
///
/// `component` must belong to this driver's live per-instance descriptor.
/// The descriptor/context outlives synchronous registration and unregister.
unsafe fn component_data<'a>(
    component: *mut bindings::snd_soc_component,
) -> &'a SndSocT8140AopData {
    // SAFETY: by the caller's contract.
    unsafe { &component_context(component).data }
}

/// The stable context behind this component's descriptor.
///
/// # Safety
///
/// As for [`component_data`].
unsafe fn component_context<'a>(component: *mut bindings::snd_soc_component) -> &'a AsocContext {
    // SAFETY: by the caller's contract.
    let descriptor = unsafe { (*component).driver };
    // SAFETY: AsocContext is repr(C), with the descriptor as its first field.
    // C keeps the exact descriptor pointer passed to registration.
    unsafe { &*descriptor.cast::<AsocContext>() }
}

/// The ASoC runtime of a substream.
///
/// # Safety
///
/// `substream` must be live and belong to one of this component's links.
unsafe fn substream_rtd(
    substream: *mut bindings::snd_pcm_substream,
) -> *mut bindings::snd_soc_pcm_runtime {
    // SAFETY: by the caller's contract.
    unsafe { (*substream).private_data as *mut bindings::snd_soc_pcm_runtime }
}

/// Whether a runtime is a DPCM back-end (no PCM of its own).
///
/// # Safety
///
/// `rtd` must be a live ASoC runtime.
unsafe fn rtd_is_be(rtd: *mut bindings::snd_soc_pcm_runtime) -> bool {
    // SAFETY: by the caller's contract.
    unsafe { (*(*rtd).dai_link).no_pcm() != 0 }
}

/// The first CPU DAI of a runtime.
///
/// # Safety
///
/// `rtd` must be a live ASoC runtime.
unsafe fn rtd_cpu_dai(rtd: *mut bindings::snd_soc_pcm_runtime) -> *mut bindings::snd_soc_dai {
    // SAFETY: by the caller's contract.
    unsafe { *(*rtd).dais }
}

/// DAI ids: 0 means "assign one" to ASoC, so the front-ends carry 1..=3
/// (index + 1) and the back-ends start at 10.
const FE_DAI_ID_BASE: u32 = 1;
const BE_DAI_ID_COUT: u32 = 10;
const BE_DAI_ID_SPKR: u32 = 11;

/// The front-end index of a runtime, `None` for a back-end.
///
/// # Safety
///
/// `rtd` must be a live ASoC runtime of one of this component's links.
unsafe fn fe_index(rtd: *mut bindings::snd_soc_pcm_runtime) -> Option<usize> {
    // SAFETY: by the caller's contract.
    let id = unsafe { (*rtd_cpu_dai(rtd)).id } as u32;
    if id >= FE_DAI_ID_BASE && id < FE_DAI_ID_BASE + FE_COUNT as u32 {
        Some((id - FE_DAI_ID_BASE) as usize)
    } else {
        None
    }
}

unsafe extern "C" fn asoc_pcm_new(
    component: *mut bindings::snd_soc_component,
    rtd: *mut bindings::snd_soc_pcm_runtime,
) -> i32 {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(component) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(rtd) } {
        return 0;
    }
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let Some(fe) = (unsafe { fe_index(rtd) }) else {
        return EINVAL.to_errno();
    };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let pcm = unsafe { (*rtd).pcm };
    for stream in 0..2usize {
        let Some(name) = FE_DMA_NAMES[fe][stream] else {
            continue;
        };
        // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
        // `snd_soc_pcm_runtime` of one of this component's links.
        let substream = unsafe { (*pcm).streams[stream].substream };
        if substream.is_null() {
            continue;
        }
        // A LEAP ADMAC's per-channel SRAM registers only answer with that
        // channel's own leaf powered, so a LEAP channel is requested with it
        // on: tx0 for the speaker's leap-ns TX0, rx0 for the array's leap-s
        // RX0.  Requesting either with its leaf down writes
        // REG_CHAN_SRAM_CARVEOUT into a dead aperture, which takes an
        // asynchronous SError and panics the machine inside
        // dma_request_chan().
        let spkr_leap = data.spkr_domain_refs();
        let hpai_leap = data.hpai_domain_refs();
        let (leap_svc, leap): (&Service, &[&Atomic<usize>]) = if fe == FE_HPAI {
            (&data.hpai, &hpai_leap[..])
        } else {
            (&data.spkr, &spkr_leap[..])
        };
        let powered = fe == FE_SPKR || fe == FE_HPAI;
        if powered {
            if let Err(e) = data.domains(leap_svc, leap, true) {
                // Requesting a channel accesses its SRAM registers. Never
                // touch that aperture after its power prerequisite failed.
                return e.to_errno();
            }
        }
        // SAFETY: FFI call with the live platform device and a NUL-terminated channel name.
        let requested = kernel::error::from_err_ptr(unsafe {
            bindings::dma_request_chan(data.dev.as_raw(), name.as_ptr())
        });
        let chan = match requested {
            Ok(c) => c,
            Err(e) => {
                if powered {
                    let _ = data.domains(leap_svc, leap, false);
                }
                // A front-end can be left without a channel: its PCM exists
                // but opens with ENODEV.
                dev_warn!(
                    data.dev,
                    "front-end {}: DMA channel {:?} unavailable: {:?}\n",
                    fe,
                    name,
                    e
                );
                continue;
            }
        };
        // Keep the LEAP register aperture powered for close/terminate and
        // channel release too. Stream startup owns additional references.
        data.fe_chans.lock()[fe][stream] = chan as usize;
        // SAFETY: the substream and the DMA channel's device are live; ALSA frees the managed
        // buffer with the PCM.
        let ret = unsafe {
            bindings::snd_pcm_set_managed_buffer(
                substream,
                bindings::SNDRV_DMA_TYPE_DEV as i32,
                (*(*chan).device).dev,
                PCM_BUFFER_PREALLOC,
                PCM_BUFFER_MAX,
            )
        };
        if ret < 0 {
            for allocated in 0..2 {
                data.release_fe_channel(fe, allocated);
            }
            return ret;
        }
    }
    0
}

unsafe extern "C" fn asoc_pcm_free(
    component: *mut bindings::snd_soc_component,
    pcm: *mut bindings::snd_pcm,
) {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(component) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let rtd = unsafe { (*pcm).private_data as *mut bindings::snd_soc_pcm_runtime };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(rtd) } {
        return;
    }
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let Some(fe) = (unsafe { fe_index(rtd) }) else {
        return;
    };
    for stream in 0..2 {
        data.release_fe_channel(fe, stream);
    }
}

unsafe extern "C" fn asoc_pcm_open(
    component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(component) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let rtd = unsafe { substream_rtd(substream) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(rtd) } {
        return 0;
    }
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let Some(fe) = (unsafe { fe_index(rtd) }) else {
        return EINVAL.to_errno();
    };
    // SAFETY: the substream is live for the duration of the op.
    let stream = unsafe { (*substream).stream } as usize;
    let chan = data.fe_chans.lock()[fe][stream & 1] as *mut bindings::dma_chan;
    if chan.is_null() {
        return ENODEV.to_errno();
    }
    let mut hw = bindings::snd_pcm_hardware::default();
    if fe == FE_SPKR {
        // No mmap: every sample goes through spkr_copy's fixed ceiling and
        // the S32 -> float32 conversion the LEAP consumes.
        hw.info = bindings::SNDRV_PCM_INFO_INTERLEAVED;
    } else {
        hw.info = bindings::SNDRV_PCM_INFO_MMAP
            | bindings::SNDRV_PCM_INFO_MMAP_VALID
            | bindings::SNDRV_PCM_INFO_INTERLEAVED;
    }
    hw.periods_min = 2;
    hw.period_bytes_min = 256;
    hw.periods_max = (PCM_BUFFER_MAX / hw.period_bytes_min as usize) as u32;
    // dma_get_max_seg_size() is inline: the ADMAC sets no dma_parms, SZ_64K.
    hw.period_bytes_max = 0x10000;
    hw.buffer_bytes_max = PCM_BUFFER_MAX;
    hw.fifo_size = 16;
    if fe == FE_SPKR {
        hw.buffer_bytes_max = SPKR_BUFFER_MAX;
        hw.period_bytes_max = SPKR_BUFFER_MAX / 2;
        hw.periods_max = (SPKR_BUFFER_MAX / hw.period_bytes_min as usize) as u32;
    }
    let mut dma_data = bindings::snd_dmaengine_dai_dma_data::default();
    // SAFETY: the substream is live and `chan` is the channel requested for this front-end in
    // `asoc_pcm_new`.
    let rc = unsafe {
        bindings::snd_dmaengine_pcm_refine_runtime_hwparams(substream, &mut dma_data, &mut hw, chan)
    };
    if rc < 0 {
        return rc;
    }
    if fe == FE_SPKR {
        hw.formats &= FMTBIT_S32_LE;
    } else if fe == FE_HPAI {
        // DMA refinement describes bus widths, not sample representation.
        // The firmware produces float32; read() and mmap() expose those
        // bytes unchanged and must advertise their actual encoding.
        hw.formats &= FMTBIT_FLOAT_LE;
    }
    if (fe == FE_SPKR || fe == FE_HPAI) && hw.formats == 0 {
        return EINVAL.to_errno();
    }
    // snd_soc_set_runtime_hwparams() is inline: copy into the runtime.
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    unsafe { (*(*substream).runtime).hw = hw };
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_open(substream, chan) }
}

unsafe extern "C" fn asoc_pcm_close(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_close(substream) }
}

unsafe extern "C" fn asoc_pcm_hw_params(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    params: *mut bindings::snd_pcm_hw_params,
) -> i32 {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    let chan = unsafe { bindings::snd_dmaengine_pcm_get_chan(substream) };
    let mut cfg = bindings::dma_slave_config::default();
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    let rc = unsafe { bindings::snd_hwparams_to_dma_slave_config(substream, params, &mut cfg) };
    if rc < 0 {
        return rc;
    }
    // Interval query for the channel count: bindgen has no inline helper.
    // SAFETY: ASoC calls hw_params with a live `snd_pcm_hw_params`.
    let channels = unsafe {
        let iv = &(*params).intervals[(bindings::SNDRV_PCM_HW_PARAM_CHANNELS
            - bindings::SNDRV_PCM_HW_PARAM_FIRST_INTERVAL)
            as usize];
        iv.min
    };
    let window = core::cmp::min(channels, 4);
    // SAFETY: the substream is live for the duration of the op.
    if unsafe { (*substream).stream } == bindings::SNDRV_PCM_STREAM_PLAYBACK as i32 {
        cfg.dst_port_window_size = window;
    } else {
        cfg.src_port_window_size = window;
    }
    // SAFETY: `chan` is the live channel behind this substream's dmaengine runtime.
    unsafe {
        match (*(*chan).device).device_config {
            Some(f) => f(chan, &mut cfg),
            None => ENOSYS.to_errno(),
        }
    }
}

/// Clear old words before a new speaker stream or recovery from a revoked
/// protection lease; the queued firmware start follows the DMA trigger.
unsafe extern "C" fn asoc_pcm_prepare(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> i32 {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let rtd = unsafe { substream_rtd(substream) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(rtd) } || unsafe { fe_index(rtd) } != Some(FE_SPKR) {
        return 0;
    }
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    let runtime = unsafe { (*substream).runtime };
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    let (area, bytes) = unsafe { ((*runtime).dma_area, (*runtime).dma_bytes) };
    if !area.is_null() && bytes != 0 {
        // SAFETY: the managed buffer is dma_bytes long and no DMA runs before
        // trigger START.
        unsafe { core::ptr::write_bytes(area, 0, bytes) };
    }
    0
}

/// The float32 bit pattern of `v / 2^31`, built with integer arithmetic (the
/// kernel has no floating point).
fn f32_bits_of_q31(v: i32) -> u32 {
    if v == 0 {
        return 0;
    }
    let sign = if v < 0 { 1u32 << 31 } else { 0 };
    let m = v.unsigned_abs();
    let p = 31 - m.leading_zeros(); // position of the leading one, 0..=31
                                    // value = m * 2^-31 = 1.f * 2^(p - 31)
    let exp = (p as i32 - 31 + 127) as u32;
    let mant = if p >= 23 {
        (m >> (p - 23)) & 0x7f_ffff
    } else {
        (m << (23 - p)) & 0x7f_ffff
    };
    sign | (exp << 23) | mant
}

/// One S32 sample -> the float32 word the LEAP consumes, through the volume
/// table (whose top entry is the ceiling).
fn spkr_f32_bits(sample: i32, gain_q31: u32) -> u32 {
    let scaled = ((sample as i64) * gain_q31 as i64) >> 31; // |scaled| < SPKR_CEILING_Q31
    f32_bits_of_q31(scaled as i32)
}

/// Copy between userspace and the front-end buffers.  The speaker front-end
/// converts and limits; the jack front-ends copy verbatim (their buffers are
/// also mmap-able, this path serves read()/write()).
unsafe extern "C" fn asoc_pcm_copy(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    _channel: i32,
    pos: usize,
    iter: *mut bindings::iov_iter,
    bytes: usize,
) -> i32 {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    let rtd = unsafe { substream_rtd(substream) };
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(rtd) } {
        return EINVAL.to_errno();
    }
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    let runtime = unsafe { (*substream).runtime };
    // SAFETY: ALSA calls this op with a live substream; its runtime and DMA buffer exist for the
    // op's duration (between hw_params and hw_free for the buffer).
    let (area, dma_bytes) = unsafe { ((*runtime).dma_area, (*runtime).dma_bytes) };
    if area.is_null() || pos.checked_add(bytes).map_or(true, |end| end > dma_bytes) {
        return EINVAL.to_errno();
    }
    // SAFETY: bounds checked against the managed buffer above.
    let dst = unsafe { core::slice::from_raw_parts_mut(area.add(pos), bytes) };
    let playback = unsafe { substream_is_playback(substream) };
    if !playback {
        // SAFETY: ALSA hands us a live iov_iter for the whole call.
        let iov = unsafe { kernel::iov::IovIterDest::from_raw(iter) };
        return if iov.copy_to_iter(dst) == bytes {
            0
        } else {
            EFAULT.to_errno()
        };
    }
    // SAFETY: as above.
    let iov = unsafe { kernel::iov::IovIterSource::from_raw(iter) };
    if unsafe { fe_index(rtd) } != Some(FE_SPKR) {
        return if iov.copy_from_iter(dst) == bytes {
            0
        } else {
            EFAULT.to_errno()
        };
    }
    // S32_LE interleaved stereo in, float32 LE out, same byte count.
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(_component) };
    let volume = core::cmp::min(data.spkr_volume.load(Relaxed), SPKR_VOLUME_MAX) as usize;
    let gain = SPKR_VOLUME_Q31[volume];
    let mut chunk = [0u8; 512];
    let mut off = 0;
    while off < bytes {
        let n = core::cmp::min(chunk.len(), bytes - off) & !3;
        if n == 0 {
            return EINVAL.to_errno();
        }
        if iov.copy_from_iter(&mut chunk[..n]) != n {
            return EFAULT.to_errno();
        }
        for (i, word) in chunk[..n].chunks_exact(4).enumerate() {
            let sample = i32::from_le_bytes([word[0], word[1], word[2], word[3]]);
            let bits = spkr_f32_bits(sample, gain);
            dst[off + i * 4..off + i * 4 + 4].copy_from_slice(&bits.to_le_bytes());
        }
        off += n;
    }
    0
}

unsafe extern "C" fn asoc_pcm_trigger(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
) -> i32 {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return 0;
    }
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_trigger(substream, cmd) }
}

unsafe extern "C" fn asoc_pcm_pointer(
    _component: *mut bindings::snd_soc_component,
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: ASoC hands its ops a live substream/PCM whose private data is the
    // `snd_soc_pcm_runtime` of one of this component's links.
    if unsafe { rtd_is_be(substream_rtd(substream)) } {
        return ENOTSUPP_POINTER;
    }
    // SAFETY: the substream is live and its dmaengine runtime was set up by
    // `snd_dmaengine_pcm_open` in `asoc_pcm_open`.
    unsafe { bindings::snd_dmaengine_pcm_pointer(substream) }
}

/// -ENOTSUPP as the pointer callback returns it (mca does the same).
const ENOTSUPP_POINTER: bindings::snd_pcm_uframes_t = (-524i64) as bindings::snd_pcm_uframes_t;

/// The component's register file: the speaker volume, read and written by
/// the standard volsw control helpers.
unsafe extern "C" fn asoc_component_read(
    component: *mut bindings::snd_soc_component,
    reg: u32,
) -> u32 {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(component) };
    match reg {
        REG_SPKR_VOLUME => data.spkr_volume.load(Relaxed),
        _ => 0,
    }
}

unsafe extern "C" fn asoc_component_write(
    component: *mut bindings::snd_soc_component,
    reg: u32,
    val: u32,
) -> i32 {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let data = unsafe { component_data(component) };
    match reg {
        REG_SPKR_VOLUME => {
            data.spkr_volume
                .store(core::cmp::min(val, SPKR_VOLUME_MAX), Relaxed);
            0
        }
        _ => EINVAL.to_errno(),
    }
}

/// Writable, like the C compound literal: the card's volume lock updates
/// `platform_max` in place.
const SPKR_VOLUME_MC: bindings::soc_mixer_control = bindings::soc_mixer_control {
    min: 0,
    max: SPKR_VOLUME_MAX as i32,
    platform_max: SPKR_VOLUME_MAX as i32,
    reg: REG_SPKR_VOLUME as i32,
    rreg: REG_SPKR_VOLUME as i32,
    shift: 0,
    rshift: 0,
    // SAFETY: zero is the C initializer's value for the remaining fields
    // (sign_bit, invert, autodisable, topology object).
    ..unsafe { core::mem::zeroed() }
};

/// SOC_SINGLE_TLV("Speaker Playback Volume", REG_SPKR_VOLUME, 0, 127, 0, tlv);
/// `private_value` (the mixer control's address) is filled in at probe, as
/// const evaluation cannot cast a pointer.
const ASOC_CONTROLS: [bindings::snd_kcontrol_new; 1] = [bindings::snd_kcontrol_new {
    iface: SNDRV_CTL_ELEM_IFACE_MIXER,
    name: c"Speaker Playback Volume".as_ptr(),
    access: bindings::SNDRV_CTL_ELEM_ACCESS_TLV_READ | bindings::SNDRV_CTL_ELEM_ACCESS_READWRITE,
    info: Some(bindings::snd_soc_info_volsw),
    get: Some(bindings::snd_soc_get_volsw),
    put: Some(bindings::snd_soc_put_volsw),
    tlv: bindings::snd_kcontrol_new__bindgen_ty_1 {
        p: SPKR_VOLUME_TLV.as_ptr(),
    },
    private_value: 0,
    // SAFETY: zero is a valid value for the remaining fields (device, subdevice,
    // index, count, lock, unlock).
    ..unsafe { core::mem::zeroed() }
}];

const ASOC_COMPONENT: bindings::snd_soc_component_driver = bindings::snd_soc_component_driver {
    name: c"j700-aop-audio".as_ptr(),
    read: Some(asoc_component_read),
    write: Some(asoc_component_write),
    controls: ptr::null(),
    num_controls: 1,
    open: Some(asoc_pcm_open),
    close: Some(asoc_pcm_close),
    hw_params: Some(asoc_pcm_hw_params),
    prepare: Some(asoc_pcm_prepare),
    trigger: Some(asoc_pcm_trigger),
    pointer: Some(asoc_pcm_pointer),
    copy: Some(asoc_pcm_copy),
    pcm_new: Some(asoc_pcm_new),
    pcm_free: Some(asoc_pcm_free),
    // SAFETY: all-zero is a valid (empty) component driver; the fields
    // above are the ones this component implements.
    ..unsafe { core::mem::zeroed() }
};

/// Driver data from one of this component's DAIs.
///
/// # Safety
///
/// `dai` must be a live DAI of this driver's registered component.
unsafe fn dai_data<'a>(dai: *mut bindings::snd_soc_dai) -> &'a SndSocT8140AopData {
    // SAFETY: by the caller's contract.
    unsafe { component_data((*dai).component) }
}

unsafe extern "C" fn dai_accept_fmt(_dai: *mut bindings::snd_soc_dai, _fmt: u32) -> i32 {
    // The AOP runs the serializers: I2S, 32-bit slots, clocks from the firmware.
    0
}

unsafe extern "C" fn dai_accept_sysclk(
    _dai: *mut bindings::snd_soc_dai,
    _id: i32,
    _freq: u32,
    _dir: i32,
) -> i32 {
    0
}

unsafe extern "C" fn dai_accept_bclk_ratio(_dai: *mut bindings::snd_soc_dai, _ratio: u32) -> i32 {
    0
}

/// # Safety
///
/// `substream` must be live.
unsafe fn substream_is_playback(substream: *mut bindings::snd_pcm_substream) -> bool {
    // SAFETY: by the caller's contract.
    let stream = unsafe { (*substream).stream };
    stream == bindings::SNDRV_PCM_STREAM_PLAYBACK as i32
}

unsafe extern "C" fn cout_dai_startup(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    // SAFETY: the substream is live for the duration of the op.
    match data.jack_startup(unsafe { substream_is_playback(substream) }) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

unsafe extern "C" fn cout_dai_prepare(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    // SAFETY: the substream is live for the duration of the op.
    match data.jack_prepare(unsafe { substream_is_playback(substream) }) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

unsafe extern "C" fn cout_dai_shutdown(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    // SAFETY: the substream is live for the duration of the op.
    data.jack_shutdown(unsafe { substream_is_playback(substream) });
}

unsafe extern "C" fn spkr_dai_startup(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    // SAFETY: the substream is live for the duration of the op.
    let ret = if unsafe { substream_is_playback(substream) } {
        data.spkr_startup()
    } else {
        data.sense_startup()
    };
    match ret {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// The sense (capture) side runs from prepare: the RX DMA is armed by then
/// and a capture may start before or after the speaker plays.
unsafe extern "C" fn spkr_dai_prepare(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: the substream is live for the duration of the op.
    if unsafe { substream_is_playback(substream) } {
        // SAFETY: this component's stable context owns the queued start.
        let drv = unsafe { component_context((*dai).component) };
        drv.flush_spkr_start();
        return match drv.data.spkr_prepare() {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        };
    }
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    match data.sense_prepare() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// Atomic context: queue the sleeping start, or note the stop (playback
/// only; the sense side needs nothing here).
unsafe extern "C" fn spkr_dai_trigger(
    substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: the substream is live for the duration of the op.
    if !unsafe { substream_is_playback(substream) } {
        return 0;
    }
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let drv = unsafe { component_context((*dai).component) };
    match cmd as u32 {
        bindings::SNDRV_PCM_TRIGGER_START
        | bindings::SNDRV_PCM_TRIGGER_RESUME
        | bindings::SNDRV_PCM_TRIGGER_PAUSE_RELEASE => {
            drv.data.spkr_want_run.store(1, Relaxed);
            if let Some(work) = drv.spkr_start.as_ref() {
                // Err = already queued: the pending run will see want_run.
                let _ = workqueue::system().enqueue(work.clone());
            }
            0
        }
        bindings::SNDRV_PCM_TRIGGER_STOP
        | bindings::SNDRV_PCM_TRIGGER_SUSPEND
        | bindings::SNDRV_PCM_TRIGGER_PAUSE_PUSH => {
            drv.data.spkr_want_run.store(0, Relaxed);
            0
        }
        _ => EINVAL.to_errno(),
    }
}

unsafe extern "C" fn spkr_dai_shutdown(
    substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let drv = unsafe { component_context((*dai).component) };
    // SAFETY: the substream is live for the duration of the op.
    if !unsafe { substream_is_playback(substream) } {
        drv.data.sense_shutdown();
        return;
    }
    drv.flush_spkr_start();
    drv.data.spkr_shutdown();
}

/// The microphone array's capture DAI holds its domains at startup. The
/// sleeping firmware start is queued from trigger after the RX DMA starts.
unsafe extern "C" fn hpai_dai_startup(
    _substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    match data.hpai_startup() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

unsafe extern "C" fn hpai_dai_prepare(
    _substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls DAI ops with a DAI of our registered component.
    let data = unsafe { dai_data(dai) };
    match data.hpai_prepare() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

unsafe extern "C" fn hpai_dai_trigger(
    _substream: *mut bindings::snd_pcm_substream,
    cmd: i32,
    dai: *mut bindings::snd_soc_dai,
) -> i32 {
    // SAFETY: ASoC calls this op on a component whose descriptor/context stays live
    // throughout registration, use and unregister.
    let drv = unsafe { component_context((*dai).component) };
    match cmd as u32 {
        bindings::SNDRV_PCM_TRIGGER_START
        | bindings::SNDRV_PCM_TRIGGER_RESUME
        | bindings::SNDRV_PCM_TRIGGER_PAUSE_RELEASE => {
            drv.data.hpai_want_run.store(1, Relaxed);
            if let Some(work) = drv.hpai_start.as_ref() {
                // Err = already queued: the pending run will see want_run.
                let _ = workqueue::system().enqueue(work.clone());
            }
            0
        }
        bindings::SNDRV_PCM_TRIGGER_STOP
        | bindings::SNDRV_PCM_TRIGGER_SUSPEND
        | bindings::SNDRV_PCM_TRIGGER_PAUSE_PUSH => {
            drv.data.hpai_want_run.store(0, Relaxed);
            0
        }
        _ => EINVAL.to_errno(),
    }
}

unsafe extern "C" fn hpai_dai_shutdown(
    _substream: *mut bindings::snd_pcm_substream,
    dai: *mut bindings::snd_soc_dai,
) {
    // SAFETY: the registered component owns this pinned driver instance.
    let drv = unsafe { component_context((*dai).component) };
    drv.data.hpai_want_run.store(0, Relaxed);
    if let Some(work) = drv.hpai_start.as_ref() {
        // SAFETY: wait for any firmware start before releasing its domains.
        unsafe { bindings::flush_work(workqueue::Work::raw_get(&work.work)) };
    }
    drv.data.hpai_shutdown();
}

static HPAI_DAI_OPS: SyncCell<bindings::snd_soc_dai_ops> = SyncCell(bindings::snd_soc_dai_ops {
    startup: Some(hpai_dai_startup),
    prepare: Some(hpai_dai_prepare),
    trigger: Some(hpai_dai_trigger),
    shutdown: Some(hpai_dai_shutdown),
    set_fmt: Some(dai_accept_fmt),
    set_sysclk: Some(dai_accept_sysclk),
    set_bclk_ratio: Some(dai_accept_bclk_ratio),
    // SAFETY: zero is an empty ops table.
    ..unsafe { core::mem::zeroed() }
});

static SPKR_DAI_OPS: SyncCell<bindings::snd_soc_dai_ops> = SyncCell(bindings::snd_soc_dai_ops {
    startup: Some(spkr_dai_startup),
    prepare: Some(spkr_dai_prepare),
    trigger: Some(spkr_dai_trigger),
    shutdown: Some(spkr_dai_shutdown),
    set_fmt: Some(dai_accept_fmt),
    set_sysclk: Some(dai_accept_sysclk),
    set_bclk_ratio: Some(dai_accept_bclk_ratio),
    // SAFETY: zero is an empty ops table.
    ..unsafe { core::mem::zeroed() }
});

static COUT_DAI_OPS: SyncCell<bindings::snd_soc_dai_ops> = SyncCell(bindings::snd_soc_dai_ops {
    startup: Some(cout_dai_startup),
    prepare: Some(cout_dai_prepare),
    shutdown: Some(cout_dai_shutdown),
    set_fmt: Some(dai_accept_fmt),
    set_sysclk: Some(dai_accept_sysclk),
    set_bclk_ratio: Some(dai_accept_bclk_ratio),
    // SAFETY: zero is an empty ops table.
    ..unsafe { core::mem::zeroed() }
});

static FE_DAI_OPS: SyncCell<bindings::snd_soc_dai_ops> = SyncCell(bindings::snd_soc_dai_ops {
    set_fmt: Some(dai_accept_fmt),
    set_sysclk: Some(dai_accept_sysclk),
    set_bclk_ratio: Some(dai_accept_bclk_ratio),
    // SAFETY: zero is an empty ops table.
    ..unsafe { core::mem::zeroed() }
});

const fn pcm_stream_ch(
    name: &'static core::ffi::CStr,
    formats: u64,
    channels: u32,
) -> bindings::snd_soc_pcm_stream {
    bindings::snd_soc_pcm_stream {
        stream_name: name.as_ptr(),
        formats,
        // An empty subformat mask fails a non-DPCM open: snd_soc_runtime_calc_hw()
        // does `hw->subformats &= p->subformats`, and snd_pcm_hw_constraints_complete()
        // then refines SNDRV_PCM_HW_PARAM_SUBFORMAT against nothing and returns
        // -EINVAL.  The three DPCM front ends never reached that path, so 0 went
        // unnoticed until the microphone array's plain link.
        subformats: bindings::BINDINGS_SNDRV_PCM_SUBFMTBIT_STD,
        rates: RATE_48000_BIT,
        rate_min: 48000,
        rate_max: 48000,
        channels_min: channels,
        channels_max: channels,
        sig_bits: 0,
    }
}

const fn pcm_stream(name: &'static core::ffi::CStr, formats: u64) -> bindings::snd_soc_pcm_stream {
    pcm_stream_ch(name, formats, 2)
}

const fn empty_stream() -> bindings::snd_soc_pcm_stream {
    bindings::snd_soc_pcm_stream {
        stream_name: ptr::null(),
        formats: 0,
        subformats: 0,
        rates: 0,
        rate_min: 0,
        rate_max: 0,
        channels_min: 0,
        channels_max: 0,
        sig_bits: 0,
    }
}

/// The AOP-configured serializers take 24-bit samples from the low bits
/// of each 32-bit DMA word (macOS "slot: 32bits smpl: 24bits"): S24_LE.
const OUT_FORMATS: u64 = bindings::BINDINGS_SNDRV_PCM_FMTBIT_S24_LE;

/// DAI table: [0] the cout back-end (DT index 0), the front-end PCMs, then
/// [4] the speaker back-end (DT index 4).
/// Order is ABI: `sound-dai = <&aop_audio N>` in the machine DTS indexes this
/// array, so `<&aop_audio 0>` is cout and `<&aop_audio 4>` is spkr. Append new
/// DAIs at the end; inserting one renumbers every DAI after it and silently
/// repoints the existing dai-links.
static ASOC_DAIS: SyncCell<[bindings::snd_soc_dai_driver; 2 + FE_COUNT]> = SyncCell([
    bindings::snd_soc_dai_driver {
        name: c"cout".as_ptr(),
        id: BE_DAI_ID_COUT,
        ops: &COUT_DAI_OPS.0,
        playback: pcm_stream(c"COUT TX", OUT_FORMATS),
        // cin: the headset microphone, one 32-bit slot per frame
        capture: pcm_stream_ch(c"CIN RX", OUT_FORMATS, 1),
        // SAFETY: zero is a valid base for the remaining fields.
        ..unsafe { core::mem::zeroed() }
    },
    bindings::snd_soc_dai_driver {
        name: c"j700-pcm-0".as_ptr(),
        id: FE_DAI_ID_BASE,
        ops: &FE_DAI_OPS.0,
        playback: pcm_stream(c"PCM0 TX", OUT_FORMATS),
        // macaudio routes "Headset Capture" into PCM0 RX (mono cin)
        capture: pcm_stream_ch(c"PCM0 RX", OUT_FORMATS, 1),
        // SAFETY: all-zero is a valid (empty) value of this C descriptor; the fields set above are
        // the ones used.
        ..unsafe { core::mem::zeroed() }
    },
    bindings::snd_soc_dai_driver {
        name: c"j700-pcm-1".as_ptr(),
        id: FE_DAI_ID_BASE + 1,
        ops: &FE_DAI_OPS.0,
        // the speaker front-end: S32 in, converted through the ceiling
        playback: pcm_stream(c"PCM1 TX", FMTBIT_S32_LE),
        capture: empty_stream(),
        // SAFETY: all-zero is a valid (empty) value of this C descriptor; the fields set above are
        // the ones used.
        ..unsafe { core::mem::zeroed() }
    },
    bindings::snd_soc_dai_driver {
        name: c"j700-pcm-2".as_ptr(),
        id: FE_DAI_ID_BASE + 2,
        ops: &FE_DAI_OPS.0,
        playback: empty_stream(),
        // the speaker sense front-end: the words on the speaker wire
        capture: pcm_stream(c"PCM2 RX", FMTBIT_S32_LE),
        // SAFETY: all-zero is a valid (empty) value of this C descriptor; the fields set above are
        // the ones used.
        ..unsafe { core::mem::zeroed() }
    },
    // The internal speaker back-end: LEAP TX0 under the AOP spkr service,
    // the MAX98360A's SD_MODE driven by its own codec driver; its capture
    // side is the SpeakerTap (base-ns RX1, the tap service).
    bindings::snd_soc_dai_driver {
        name: c"spkr".as_ptr(),
        id: BE_DAI_ID_SPKR,
        ops: &SPKR_DAI_OPS.0,
        playback: pcm_stream(c"SPKR TX", FMTBIT_S32_LE),
        capture: pcm_stream(c"SPKR RX", FMTBIT_S32_LE),
        // SAFETY: all-zero is a valid (empty) value of this C descriptor; the fields set above are
        // the ones used.
        ..unsafe { core::mem::zeroed() }
    },
    bindings::snd_soc_dai_driver {
        name: c"j700-pcm-3".as_ptr(),
        id: FE_DAI_ID_BASE + 3,
        // The array has no codec and no serializer of ours -- it is a PDM part
        // on the AOP's own sense PDM controller -- so there is nothing for a
        // back-end to represent and this front-end drives the hpai service
        // itself.  HPAI_DAI_OPS is a superset of FE_DAI_OPS.
        ops: &HPAI_DAI_OPS.0,
        playback: empty_stream(),
        // the HQ microphone array: 48 kHz stereo, 8 bytes per frame
        capture: pcm_stream(c"PCM3 RX", FMTBIT_FLOAT_LE),
        // SAFETY: all-zero is a valid (empty) value of this C descriptor; the fields set above are
        // the ones used.
        ..unsafe { core::mem::zeroed() }
    },
]);

/// C retains the address of `component`, so allocate this context before
/// registration and keep it alive until unregister has finished all callbacks.
#[repr(C)]
struct AsocContext {
    component: bindings::snd_soc_component_driver,
    data: Arc<SndSocT8140AopData>,
    spkr_start: Option<Arc<SpkrStartWork>>,
    hpai_start: Option<Arc<HpaiStartWork>>,
    _controls: KBox<[bindings::snd_kcontrol_new; 1]>,
    // The C mixer helpers modify platform_max under their control locks.
    _volume: KBox<Opaque<bindings::soc_mixer_control>>,
}

const _: () = assert!(mem::offset_of!(AsocContext, component) == 0);

impl AsocContext {
    /// Called from sleeping prepare/shutdown paths while ALSA serializes
    /// this stream. A previously queued start must finish before mute/pw0.
    fn flush_spkr_start(&self) {
        self.data.spkr_want_run.store(0, Relaxed);
        if let Some(work) = self.spkr_start.as_ref() {
            // SAFETY: the context retains the work item until this flush
            // completes; no stream trigger races ALSA prepare/shutdown.
            unsafe {
                let raw = workqueue::Work::raw_get(&work.work);
                bindings::flush_work(raw);
            }
        }
    }

    fn new(data: Arc<SndSocT8140AopData>) -> Result<KBox<Self>> {
        let spkr_data = data.clone();
        let hpai_data = data.clone();
        let spkr_start = Arc::pin_init(
            pin_init!(SpkrStartWork {
                data: spkr_data,
                work <- new_work!("SpkrStartWork::work"),
            }),
            GFP_KERNEL,
        )?;
        let hpai_start = Arc::pin_init(
            pin_init!(HpaiStartWork {
                data: hpai_data,
                work <- new_work!("HpaiStartWork::work"),
            }),
            GFP_KERNEL,
        )?;
        let volume = KBox::new(Opaque::new(SPKR_VOLUME_MC), GFP_KERNEL)?;
        let mut controls = ASOC_CONTROLS;
        controls[0].private_value = volume.get() as usize as _;
        let controls = KBox::new(controls, GFP_KERNEL)?;
        let mut component = ASOC_COMPONENT;
        component.controls = controls.as_ptr();
        Ok(KBox::new(
            Self {
                component,
                data,
                spkr_start: Some(spkr_start),
                hpai_start: Some(hpai_start),
                _controls: controls,
                _volume: volume,
            },
            GFP_KERNEL,
        )?)
    }
}

struct SndSocT8140AopDriver {
    card: *mut bindings::snd_card,
    data: Arc<SndSocT8140AopData>,
    asoc: Option<KBox<AsocContext>>,
    asoc_registered: bool,
    closed: Atomic<bool>,
}

impl SndSocT8140AopDriver {
    const VTABLE: bindings::snd_pcm_ops = bindings::snd_pcm_ops {
        open: Some(lpai_pcm_open),
        close: Some(lpai_pcm_close),
        prepare: Some(lpai_pcm_prepare),
        trigger: Some(lpai_pcm_trigger),
        pointer: Some(lpai_pcm_pointer),
        ioctl: None,
        hw_params: None,
        hw_free: None,
        sync_stop: None,
        get_time_info: None,
        fill_silence: None,
        copy: None,
        page: None,
        mmap: None,
        ack: None,
    };

    fn new(data: Arc<SndSocT8140AopData>) -> Result<Self> {
        // Own probe cleanup before the first fallible work-item allocation.
        let mut this = SndSocT8140AopDriver {
            card: ptr::null_mut(),
            data: data.clone(),
            asoc: None,
            asoc_registered: false,
            closed: Atomic::new(false),
        };
        this.asoc = Some(AsocContext::new(data.clone())?);
        // SAFETY: FFI call with the live device and the module; `this.card` receives the new card.
        let ret = unsafe {
            bindings::snd_card_new(
                data.dev.as_raw(),
                -1,
                ptr::null(),
                THIS_MODULE.as_ptr(),
                0,
                &mut this.card,
            )
        };
        if ret < 0 {
            dev_err!(data.dev, "Unable to allocate sound card\n");
            return Err(Error::from_errno(ret));
        }
        // Named like the other AOP audio cards: Apple<chassis>LPAI.
        let chassis = data
            .fwnode
            .property_read::<CString>(c_str!("apple,chassis-name"))
            .required_by(&data.dev)?;
        let machine_kind = data
            .fwnode
            .property_read::<CString>(c_str!("apple,machine-kind"))
            .required_by(&data.dev)?;
        let id_str = CString::try_from_fmt(fmt!("Apple{}LPAI", *chassis))?;
        let shortname = CString::try_from_fmt(fmt!("{} {} LPAI", *machine_kind, *chassis))?;
        let longname = CString::try_from_fmt(fmt!(
            "{} {} Low-Power Audio Interface",
            *machine_kind,
            *chassis
        ))?;
        // SAFETY: `card` was created by `snd_card_new` above; the fixed-size name fields are filled
        // with NUL-terminated strings that fit.
        unsafe {
            copy_str(&mut (*this.card).driver, c"aop_audio".to_bytes_with_nul());
            copy_str(&mut (*this.card).id, id_str.to_bytes_with_nul());
            copy_str(&mut (*this.card).shortname, shortname.to_bytes_with_nul());
            copy_str(&mut (*this.card).longname, longname.to_bytes_with_nul());
        }

        let mut pcm = ptr::null_mut();
        // SAFETY: `card` was created by `snd_card_new` above; `pcm` receives the new PCM.
        let ret =
            unsafe { bindings::snd_pcm_new(this.card, longname.as_ptr() as _, 0, 0, 1, &mut pcm) };
        if ret < 0 {
            dev_err!(data.dev, "Unable to allocate PCM device\n");
            return Err(Error::from_errno(ret));
        }
        // SAFETY: `pcm` was created by `snd_pcm_new` above; the vtable is static and the private
        // data is an `Arc` released by `lpai_pcm_free_private`.
        unsafe {
            bindings::snd_pcm_set_ops(
                pcm,
                bindings::SNDRV_PCM_STREAM_CAPTURE as i32,
                &Self::VTABLE,
            );
            (*pcm).private_data = data.clone().into_foreign() as _;
            (*pcm).private_free = Some(lpai_pcm_free_private);
            (*pcm).info_flags = 0;
            copy_str(&mut (*pcm).name, c"aop_audio".to_bytes_with_nul());
            bindings::snd_pcm_set_managed_buffer_all(
                pcm,
                bindings::SNDRV_DMA_TYPE_VMALLOC as i32,
                ptr::null_mut(),
                PERIOD_BYTES * 4,
                PERIOD_BYTES * PERIODS_MAX as usize,
            );
        }

        // SAFETY: `card` was created by `snd_card_new` above and is not registered yet.
        let ret = unsafe { bindings::snd_card_register(this.card) };
        if ret < 0 {
            dev_err!(data.dev, "Unable to register sound card\n");
            return Err(Error::from_errno(ret));
        }

        // The ASoC side: the jack (cout) and speaker back-ends, the
        // front-end PCMs and the speaker volume control.
        let context = this.asoc.as_ref().ok_or(ENODEV)?;
        // SAFETY: the descriptor and its referenced controls are heap-stable
        // and fully initialized. They remain owned by `this` through C's
        // synchronous registration callbacks, even before drvdata publication.
        let ret = unsafe {
            bindings::snd_soc_register_component(
                data.dev.as_raw(),
                &context.component,
                ASOC_DAIS.0.as_ptr() as *mut bindings::snd_soc_dai_driver,
                ASOC_DAIS.0.len() as i32,
            )
        };
        if ret < 0 {
            dev_err!(data.dev, "unable to register the ASoC component: {}\n", ret);
            return Err(Error::from_errno(ret));
        }
        this.asoc_registered = true;
        Ok(this)
    }
}

impl SndSocT8140AopDriver {
    /// Driver-core calls this before revoking drvdata/devres. Drop is also
    /// used on construction failure, when any acquired resources remain live.
    fn teardown(&self) {
        if self.closed.xchg(true, Relaxed) {
            return;
        }
        self.data.running.store(0, Relaxed);
        self.data.hpai_want_run.store(0, Relaxed);
        self.data.spkr_want_run.store(0, Relaxed);
        // Stop reports before ALSA destroys any substream or module callback.
        self.data
            .adata
            .remove_report_listener(&self.data.service, EPIC_SUBTYPE_PRODUCER_REPORT);
        if self.asoc_registered {
            if let Some(context) = self.asoc.as_ref() {
                // SAFETY: the context remains owned until after unregister
                // completes PCM, DAI and control callbacks. No drvdata lookup
                // is needed by those callbacks.
                unsafe {
                    bindings::snd_soc_unregister_component_by_driver(
                        self.data.dev.as_raw(),
                        &context.component,
                    )
                };
            }
        }
        if let Some(context) = self.asoc.as_ref() {
            self.data.hpai_want_run.store(0, Relaxed);
            self.data.spkr_want_run.store(0, Relaxed);
            if let Some(work) = context.hpai_start.as_ref() {
                // SAFETY: work remains owned by the stable context; no new
                // trigger can arrive after component unregister.
                unsafe { bindings::flush_work(workqueue::Work::raw_get(&work.work)) };
            }
            if let Some(work) = context.spkr_start.as_ref() {
                // SAFETY: as above.
                unsafe { bindings::flush_work(workqueue::Work::raw_get(&work.work)) };
            }
        }
        // Covers partial PCM registration too; cleared slots are idempotent.
        for fe in 0..FE_COUNT {
            for stream in 0..2 {
                self.data.release_fe_channel(fe, stream);
            }
        }
        if !self.card.is_null() {
            // SAFETY: the card is freed once, while the binding remains live.
            unsafe { bindings::snd_card_free(self.card) };
        }
        for slot in [
            &self.data.pd_mca1,
            &self.data.pd_mca0,
            &self.data.pd_tx0,
            &self.data.pd_leap_mca,
            &self.data.pd_rx0,
        ] {
            let pd = slot.xchg(0, Relaxed);
            if pd != 0 {
                // SAFETY: all channel releases and stream shutdown callbacks
                // completed before detaching their attached domain device.
                unsafe { bindings::dev_pm_domain_detach(pd as *mut bindings::device, true) };
            }
        }
        if self.data.powered.load(Relaxed) != 0 {
            if self.data.set_power(POWER_STATE_IDLE).is_ok() {
                self.data.powered.store(0, Relaxed);
            }
        }
    }
}

impl Drop for SndSocT8140AopDriver {
    fn drop(&mut self) {
        self.teardown();
    }
}

unsafe impl Send for SndSocT8140AopDriver {}
unsafe impl Sync for SndSocT8140AopDriver {}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SndSocT8140AopDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("apple,t8140-aop-audio")), ())]
);

impl platform::Driver for SndSocT8140AopDriver {
    type IdInfo = ();

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        _info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let dev = ARef::<device::Device>::from(pdev.as_ref());
        let parent = pdev.as_ref().parent().ok_or(ENODEV)?;
        let parent_data = parent.get_drvdata::<core::ffi::c_void>();
        if parent_data.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: AOP synchronously releases child drivers before freeing its
        // drvdata. Driver-core serializes this probe against that child release.
        let adata_ptr = unsafe { Pin::<KBox<Arc<dyn AOP>>>::borrow(parent_data) };
        let adata = (&*adata_ptr).clone();
        // SAFETY: this live child owns the AOP-supplied platform-data record.
        let service = unsafe { (*dev.as_raw()).platform_data as *const EPICService };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: the validated service pointer remains live with the child.
        let svc = unsafe { *service };

        // The source ring: 512 KiB through our own IOMMU stream (dart-aop 9),
        // owned by the AOP driver because the firmware binds it once per boot.
        // SAFETY: no DMA is in flight on this device yet.
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::new::<42>())? };
        // SAFETY: the device is bound while probe runs.
        let bound = unsafe { dev.as_bound() };
        let ring = adata.source_ring(bound, RING_BYTES)?;
        let ring_iova = ring.iova;

        let data_dev = dev.clone();
        let fwnode = ARef::from(dev.fwnode().ok_or(ENOENT)?);
        let data = Arc::pin_init(
            pin_init!(SndSocT8140AopData {
                dev: data_dev,
                fwnode,
                adata,
                service: svc,
                ring,
                ring_bytes: Atomic::new(0),
                sequence: Atomic::new(0),
                powered: Atomic::new(0),
                running: Atomic::new(0),
                hw_ptr_frames: Atomic::new(0),
                report_warnings: [const { Atomic::new(0) }; 5],
                cout: Service::new(AUDIO_DEV_COUT, "cout"),
                cin: Service::new(AUDIO_DEV_CIN, "cin"),
                spkr: Service::new(AUDIO_DEV_SPKR, "spkr"),
                tap: Service::new(AUDIO_DEV_TAP, "tap"),
                hpai: Service::new(AUDIO_DEV_HPAI, "hpai"),
                pd_mca1: Atomic::new(0),
                pd_mca0: Atomic::new(0),
                pd_tx0: Atomic::new(0),
                pd_leap_mca: Atomic::new(0),
                pd_rx0: Atomic::new(0),
                spkr_want_run: Atomic::new(0),
                hpai_want_run: Atomic::new(0),
                spkr_volume: Atomic::new(SPKR_VOLUME_MAX),
                fe_chans <- new_spinlock!([[0usize; 2]; FE_COUNT]),
                stream <- new_spinlock!(StreamState {
                    substream: ptr::null_mut(),
                    last_absolute: None,
                    copied: None,
                    hw_ptr_bytes: 0,
                    period_acc: 0,
                    buffer_bytes: 0,
                    overruns: 0,
                }),
            }),
            GFP_KERNEL,
        )?;

        data.attach_lpai()?;
        data.set_channel_control()?;
        data.read_geometry()?;
        let listener: Arc<dyn ReportListener> = data.clone();
        data.adata
            .add_report_listener(svc, EPIC_SUBTYPE_PRODUCER_REPORT, listener)?;
        dev_dbg!(
            dev,
            "low-power microphone ready (source ring at {:#x})\n",
            ring_iova
        );
        // The jack path's serializer domain (audio_mca1_m), held for the
        // driver's lifetime and runtime-powered around cout streams.
        // SAFETY: FFI call with the live platform device and a NUL-terminated domain name.
        let pd = unsafe { bindings::dev_pm_domain_attach_by_name(dev.as_raw(), c"mca1".as_ptr()) };
        match kernel::error::from_err_ptr(pd) {
            Ok(p) if !p.is_null() => data.pd_mca1.store(p as usize, Relaxed),
            Ok(_) => dev_warn!(
                dev,
                "no mca1 power domain: the jack path will not power its serializer\n"
            ),
            Err(e) => dev_warn!(dev, "mca1 power domain attach failed: {:?}\n", e),
        }
        // The speaker path's LEAP leaves (tx0 -> leap_c -> leap_a -> audio_fr
        // -> audio_a, leap_mca -> audio_p), runtime-powered around streams.
        for (name, slot) in [
            (c"tx0", &data.pd_tx0),
            (c"mca", &data.pd_leap_mca),
            (c"mca0", &data.pd_mca0),
            (c"rx0", &data.pd_rx0),
        ] {
            // SAFETY: FFI call with the live platform device and a NUL-terminated domain name.
            let pd = unsafe { bindings::dev_pm_domain_attach_by_name(dev.as_raw(), name.as_ptr()) };
            match kernel::error::from_err_ptr(pd) {
                Ok(p) if !p.is_null() => slot.store(p as usize, Relaxed),
                Ok(_) => dev_warn!(
                    dev,
                    "no {:?} power domain: the speaker or sense path is unavailable\n",
                    name
                ),
                Err(e) => dev_warn!(dev, "{:?} power domain attach failed: {:?}\n", name, e),
            }
        }
        Self::new(data.clone())
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        this.teardown();
    }
}

module_platform_driver! {
    type: SndSocT8140AopDriver,
    name: "snd_soc_t8140_aop_audio",
    description: "Apple T8140 AOP audio: microphones, headphone jack and speakers",
    license: "Dual MIT/GPL",
}
