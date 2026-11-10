// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The low-power microphone (`lpai`) of the PDM AOP audio firmware
//!
//! A capture-only PCM next to the high-power array, on the same card.  The
//! low-power microphone is the always-available microphone the firmware runs
//! for listening; it is a developer device: UCM does not offer it, so it is
//! never a desktop source or the default input.
//!
//! On T8112 and T6000 class firmware the samples come inline in the audio
//! service's own reports (EPIC report subtype 0xee).  Each report is a packet
//! with the absolute producer byte count at its end and the interleaved
//! samples of the span that ends there; the stream format (rate, channels,
//! bytes per channel) is the device's property 302.  There is no DMA engine
//! and no host ring the firmware writes: the driver copies every packet into
//! the ALSA buffer.  `runn` and `idle` power requests (property 202) start and
//! stop the producer, and property 200 reads the state back.
//!
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{mem, ptr, slice};

use kernel::{
    bindings,
    device,
    new_mutex,
    new_spinlock,
    prelude::*,
    soc::apple::aop::{from_fourcc, EPICService, ReportListener, AOP},
    sync::{
        aref::ARef,
        atomic::{Acquire, Atomic, AtomicFlag, Relaxed, Release},
        Arc,
        ArcBorrow,
        Mutex,
        SpinLock, //
    },
    types::ForeignOwnable,
};

const EPIC_SUBTYPE_WRAPPED_CALL: u16 = 0x20;
/// The audio service's report that carries low-power microphone samples.
pub(crate) const EPIC_SUBTYPE_LPAI_SAMPLES: u16 = 0xee;
const CALLTYPE_AUDIO_GET_PROP: u32 = 0xc3000004;
const CALLTYPE_AUDIO_SET_PROP: u32 = 0xc3000005;
const AUDIO_DEV_LPAI: u32 = from_fourcc(b"lpai");

/// Property 200: the device's current power state (a FourCC).
const PROP_POWER_STATE: u32 = 200;
/// Property 202: a power request ([`PowerRequest`]).
const PROP_POWER_REQUEST: u32 = 202;
/// Property 302: the stream format of the inline samples ([`StreamFormat`]).
const PROP_STREAM_FORMAT: u32 = 302;
const POWER_STATE_RUN: u32 = from_fourcc(b"runn");
const POWER_STATE_IDLE: u32 = from_fourcc(b"idle");
/// The power request's parameter for an input device.
const POWER_PARAM_INPUT: u32 = 1;

/// Inline sample report: the outer packet carries the absolute, exclusive
/// producer byte count; the inner audio packet its kind, its total length
/// (header included) and then the samples.
const REPORT_PRODUCER_END: usize = 0x08;
const REPORT_INNER: usize = 0x14;
const INNER_KIND: usize = 0x04;
const INNER_PACKET_BYTES: usize = 0x1c;
const INNER_HEADER_BYTES: usize = 0x24;
/// Inner packet kind of a report that carries live samples (the others are
/// stream events, hardware errors and history records).
const INNER_KIND_SAMPLES: u32 = 1;

/// Firmware power requests carry a timestamp in the 24 MHz always-on
/// timebase; the architected counter may run at a different rate.
const AOP_TIMEBASE_HZ: u64 = 24_000_000;

/// The ALSA side: periods of 10 ms to 64 KiB, and a preallocated buffer that
/// holds several seconds of the largest format the firmware describes.
const PERIOD_MS_MIN: u32 = 10;
const PERIOD_BYTES_MAX: usize = 0x10000;
const BUFFER_BYTES_MAX: usize = 512 * 1024;
const PERIODS_MIN: u32 = 2;

/// Warning classes, one message per class and driver instance.
const WARN_MALFORMED: usize = 0;
const WARN_GAP: usize = 1;
const WARN_BACKWARDS: usize = 2;
const WARN_OVERRUN: usize = 3;
const WARN_COUNT: usize = 4;

fn le_u32(buf: &[u8], off: usize) -> Option<u32> {
    let b = buf.get(off..off.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn le_u64(buf: &[u8], off: usize) -> Option<u64> {
    let b = buf.get(off..off.checked_add(8)?)?;
    Some(u64::from_le_bytes([
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
    ]))
}

fn aop_counter_ticks(counter: u64, frequency: u64) -> Result<u64> {
    if frequency == 0 || frequency > u64::from(u32::MAX) {
        return Err(EINVAL);
    }
    let seconds = counter / frequency;
    let remainder = (counter % frequency) * AOP_TIMEBASE_HZ / frequency;
    seconds
        .checked_mul(AOP_TIMEBASE_HZ)
        .and_then(|ticks| ticks.checked_add(remainder))
        .ok_or(EOVERFLOW)
}

fn aop_timestamp() -> Result<u64> {
    // SAFETY: both helpers read the architected timer and take no arguments.
    let (counter, rate) = unsafe { (bindings::arch_timer_counter(), bindings::arch_timer_rate()) };
    aop_counter_ticks(counter, u64::from(rate))
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
    /// As the T8140 driver sends it.
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
            ..Self::default()
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
    fn new(dev_id: u32, modifier: u32, data: T) -> Self {
        Self {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_SET_PROP,
            len: mem::size_of::<T>() as u64 + 0x30,
            dev_id,
            modifier,
            len2: mem::size_of::<T>() as u32,
            data,
            ..Self::default()
        }
    }
}

/// Property 202: the device, a per-device request sequence, the request time
/// in the 24 MHz timebase, the target state FourCC and the parameter (1 for
/// an input device).
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

static_assert!(mem::size_of::<PowerRequest>() == 48);

/// Property 302 of the inline-sample firmware: a non-zero sample-format
/// selector, the rate, the channels per frame and the bytes per channel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StreamFormat {
    pub(crate) rate: u32,
    pub(crate) channels: u32,
    pub(crate) bytes_per_channel: u32,
}

impl StreamFormat {
    /// Decodes the 16-byte property; `None` for a format this driver cannot
    /// present as an ALSA PCM.
    pub(crate) fn parse(prop: &[u8]) -> Option<Self> {
        let selector = le_u32(prop, 0x0)?;
        let fmt = Self {
            rate: le_u32(prop, 0x4)?,
            channels: le_u32(prop, 0x8)?,
            bytes_per_channel: le_u32(prop, 0xc)?,
        };
        if selector == 0 || fmt.rate == 0 || fmt.channels == 0 || fmt.channels > 8 {
            return None;
        }
        fmt.alsa_format()?;
        Some(fmt)
    }

    pub(crate) fn frame_bytes(&self) -> usize {
        (self.channels * self.bytes_per_channel) as usize
    }

    /// Signed little-endian PCM of the channel width.
    fn alsa_format(&self) -> Option<u64> {
        match self.bytes_per_channel {
            2 => Some(bindings::BINDINGS_SNDRV_PCM_FMTBIT_S16_LE),
            4 => Some(bindings::BINDINGS_SNDRV_PCM_FMTBIT_S32_LE),
            _ => None,
        }
    }
}

/// A valid inline sample report: the absolute byte range it covers and where
/// its samples sit in the report.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct SampleSpan {
    pub(crate) start: u64,
    pub(crate) end: u64,
    pub(crate) offset: usize,
    pub(crate) len: usize,
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum ReportError {
    /// Not a sample packet (another kind): not an error.
    NotSamples,
    Malformed,
}

/// Checks every length before it is used: the inner packet must fit the
/// report, the samples must be whole frames and lie within the producer
/// count.
pub(crate) fn parse_sample_report(
    report: &[u8],
    frame_bytes: usize,
) -> core::result::Result<SampleSpan, ReportError> {
    let end = le_u64(report, REPORT_PRODUCER_END).ok_or(ReportError::Malformed)?;
    let kind = le_u32(report, REPORT_INNER + INNER_KIND).ok_or(ReportError::Malformed)?;
    let packet = le_u32(report, REPORT_INNER + INNER_PACKET_BYTES).ok_or(ReportError::Malformed)?;
    if kind != INNER_KIND_SAMPLES {
        return Err(ReportError::NotSamples);
    }
    let packet = packet as usize;
    if packet < INNER_HEADER_BYTES || packet > report.len() - REPORT_INNER {
        return Err(ReportError::Malformed);
    }
    let len = packet - INNER_HEADER_BYTES;
    if frame_bytes == 0 || len % frame_bytes != 0 || len as u64 > end {
        return Err(ReportError::Malformed);
    }
    Ok(SampleSpan {
        start: end - len as u64,
        end,
        offset: REPORT_INNER + INNER_HEADER_BYTES,
        len,
    })
}

/// What a sample span means for a consumer at `copied`: the bytes of the
/// span to skip (already copied), and whether a gap was crossed.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Continuity {
    /// Copy the span from `skip` bytes in.
    Copy { skip: usize, gap: bool },
    /// The whole span is older than what was copied.
    Stale,
}

pub(crate) fn continuity(copied: Option<u64>, span: &SampleSpan) -> Continuity {
    match copied {
        None => Continuity::Copy {
            skip: 0,
            gap: false,
        },
        Some(c) if span.end <= c => Continuity::Stale,
        Some(c) if span.start >= c => Continuity::Copy {
            skip: 0,
            gap: span.start > c,
        },
        Some(c) => Continuity::Copy {
            skip: (c - span.start) as usize,
            gap: false,
        },
    }
}

/// Copies `src` into the ALSA ring buffer at `hw_ptr`, wrapping; returns the
/// new pointer.
///
/// # Safety
///
/// `dma_area` must point to `buffer_bytes` writable bytes, `hw_ptr` must be
/// below `buffer_bytes`, and the buffer must stay mapped for the call.
unsafe fn copy_into(dma_area: *mut u8, buffer_bytes: usize, hw_ptr: usize, src: &[u8]) -> usize {
    let mut dst = hw_ptr;
    let mut left = src;
    while !left.is_empty() {
        let n = left.len().min(buffer_bytes - dst);
        // SAFETY: dst + n <= buffer_bytes by the caller's contract.
        unsafe { ptr::copy_nonoverlapping(left.as_ptr(), dma_area.add(dst), n) };
        left = &left[n..];
        dst = (dst + n) % buffer_bytes;
    }
    dst
}

struct StreamState {
    substream: *mut bindings::snd_pcm_substream,
    /// Absolute producer byte count already copied into the ALSA buffer.
    copied: Option<u64>,
    hw_ptr_bytes: usize,
    period_acc: usize,
    period_bytes: usize,
    buffer_bytes: usize,
}

// SAFETY: the substream pointer is only dereferenced under the spinlock that
// holds this state, while ALSA keeps the substream alive: open sets it and
// close clears it under the same lock.
unsafe impl Send for StreamState {}

/// The low-power microphone capture of one AOP audio device.
#[pin_data]
pub(crate) struct LpaiCapture {
    dev: ARef<device::Device>,
    adata: Arc<dyn AOP>,
    service: EPICService,
    format: StreamFormat,
    sequence: Atomic<u32>,
    /// The PCM holds `runn`.
    powered: AtomicFlag,
    /// The stream is triggered (set and cleared under the ALSA stream lock,
    /// read by the report path).
    running: AtomicFlag,
    /// Stored with release after the frames before it are in the ALSA
    /// buffer, loaded with acquire by the pointer callback.
    hw_ptr_frames: Atomic<u32>,
    warnings: [AtomicFlag; WARN_COUNT],
    /// Serializes power requests and their read-back.
    #[pin]
    power: Mutex<()>,
    #[pin]
    stream: SpinLock<StreamState>,
}

impl LpaiCapture {
    fn wrapped_call<T>(&self, data: &T) -> Result<u32> {
        // SAFETY: T is a plain packed firmware record.
        let bytes =
            unsafe { slice::from_raw_parts(ptr::from_ref(data).cast::<u8>(), mem::size_of::<T>()) };
        self.adata
            .epic_call(&self.service, EPIC_SUBTYPE_WRAPPED_CALL, bytes)
    }

    /// Reads property `modifier` of `lpai`; the reply is the property length
    /// followed by its bytes, returned without the length.
    fn get_prop(
        adata: &Arc<dyn AOP>,
        service: &EPICService,
        dev: &device::Device,
        modifier: u32,
        len: usize,
    ) -> Result<KVec<u8>> {
        let req = AudioGetDeviceProp::new(AUDIO_DEV_LPAI, modifier);
        // SAFETY: the request is a plain packed firmware record.
        let bytes = unsafe {
            slice::from_raw_parts(ptr::from_ref(&req).cast::<u8>(), mem::size_of_val(&req))
        };
        let (retcode, ret) =
            adata.epic_call_ret(service, EPIC_SUBTYPE_WRAPPED_CALL, bytes, 4 + len)?;
        if retcode != 0 {
            dev_err!(dev, "lpai property {} refused: {:#x}\n", modifier, retcode);
            return Err(EIO);
        }
        if ret.len() < 4 + len || le_u32(&ret, 0) != Some(len as u32) {
            dev_err!(dev, "lpai property {}: unexpected reply\n", modifier);
            return Err(EIO);
        }
        let mut out = KVec::with_capacity(len, GFP_KERNEL)?;
        out.extend_from_slice(&ret[4..4 + len], GFP_KERNEL)?;
        Ok(out)
    }

    /// Reads the stream format and registers the sample listener.  `lpai` is
    /// already attached and its channel control set (the high-power array's
    /// setup needs both).  A firmware without the inline stream yields
    /// `Ok(None)`: the card then has only the array.
    pub(crate) fn new_inline(
        dev: &ARef<device::Device>,
        adata: &Arc<dyn AOP>,
        service: EPICService,
    ) -> Result<Option<Arc<Self>>> {
        let prop = match Self::get_prop(adata, &service, dev, PROP_STREAM_FORMAT, 16) {
            Ok(p) => p,
            Err(_) => return Ok(None),
        };
        let Some(format) = StreamFormat::parse(&prop) else {
            dev_warn!(dev, "lpai: unsupported stream format, no low-power PCM\n");
            return Ok(None);
        };
        dev_dbg!(
            dev,
            "lpai: {} Hz, {} channels, {} bytes per channel\n",
            format.rate,
            format.channels,
            format.bytes_per_channel
        );
        let this = Arc::pin_init(
            try_pin_init!(Self {
                dev: dev.clone(),
                adata: adata.clone(),
                service,
                format,
                sequence: Atomic::new(0),
                powered: AtomicFlag::new(false),
                running: AtomicFlag::new(false),
                hw_ptr_frames: Atomic::new(0),
                warnings: [const { AtomicFlag::new(false) }; WARN_COUNT],
                power <- new_mutex!(()),
                stream <- new_spinlock!(StreamState {
                    substream: ptr::null_mut(),
                    copied: None,
                    hw_ptr_bytes: 0,
                    period_acc: 0,
                    period_bytes: 0,
                    buffer_bytes: 0,
                }),
            }),
            GFP_KERNEL,
        )?;
        let listener: Arc<dyn ReportListener> = this.clone();
        adata.add_report_listener(service, EPIC_SUBTYPE_LPAI_SAMPLES, listener)?;
        Ok(Some(this))
    }

    /// Stops report delivery; returns once no report callback runs.
    pub(crate) fn detach(&self) {
        self.running.store(false, Relaxed);
        self.adata
            .remove_report_listener(&self.service, EPIC_SUBTYPE_LPAI_SAMPLES);
    }

    fn set_power(&self, state: u32) -> Result<()> {
        let _guard = self.power.lock();
        let sequence = self.sequence.fetch_add(1, Relaxed).wrapping_add(1);
        let req = PowerRequest {
            dev_id: AUDIO_DEV_LPAI,
            sequence,
            timestamp: aop_timestamp()?,
            state,
            param: POWER_PARAM_INPUT,
            ..PowerRequest::default()
        };
        let ret = self.wrapped_call(&AudioSetDeviceProp::new(
            AUDIO_DEV_LPAI,
            PROP_POWER_REQUEST,
            req,
        ))?;
        if ret != 0 {
            dev_err!(self.dev, "lpai power request {:#x} refused: {:#x}\n", state, ret);
            return Err(EIO);
        }
        let now = Self::get_prop(&self.adata, &self.service, &self.dev, PROP_POWER_STATE, 4)?;
        let now = le_u32(&now, 0).ok_or(EIO)?;
        if now != state {
            dev_err!(self.dev, "lpai power state {:#x} after requesting {:#x}\n", now, state);
            return Err(EIO);
        }
        Ok(())
    }

    fn warn_once(&self, class: usize) -> bool {
        !self.warnings[class].xchg(true, Relaxed)
    }

    /// Creates the capture PCM `device` of `card` for this microphone.
    pub(crate) fn new_pcm(self: &Arc<Self>, card: *mut bindings::snd_card, device: i32) -> Result {
        let mut pcm = ptr::null_mut();
        // SAFETY: `card` is a live, unregistered card; `pcm` receives the new PCM.
        let ret = unsafe { bindings::snd_pcm_new(card, c"lpai".as_ptr(), device, 0, 1, &mut pcm) };
        if ret < 0 {
            dev_err!(self.dev, "unable to allocate the low-power PCM\n");
            return Err(Error::from_errno(ret));
        }
        // SAFETY: `pcm` was created above; the ops are static and the private
        // data is an `Arc` released by `pcm_free_private`.  A vmalloc buffer
        // needs no device.
        unsafe {
            bindings::snd_pcm_set_ops(pcm, bindings::SNDRV_PCM_STREAM_CAPTURE as i32, &PCM_OPS);
            (*pcm).private_data = self.clone().into_foreign().cast();
            (*pcm).private_free = Some(pcm_free_private);
            (*pcm).info_flags = 0;
            let name = c"Low-Power Microphone".to_bytes_with_nul();
            let dst = &mut (*pcm).name;
            let n = name.len().min(dst.len());
            for (d, s) in dst.iter_mut().zip(name[..n].iter()) {
                *d = *s as _;
            }
            bindings::snd_pcm_set_managed_buffer_all(
                pcm,
                bindings::SNDRV_DMA_TYPE_VMALLOC as i32,
                ptr::null_mut(),
                BUFFER_BYTES_MAX,
                BUFFER_BYTES_MAX,
            );
        }
        Ok(())
    }

    fn hw(&self) -> bindings::snd_pcm_hardware {
        let frame = self.format.frame_bytes();
        let period_min = (self.format.rate * PERIOD_MS_MIN / 1000) as usize * frame;
        bindings::snd_pcm_hardware {
            info: bindings::SNDRV_PCM_INFO_MMAP
                | bindings::SNDRV_PCM_INFO_MMAP_VALID
                | bindings::SNDRV_PCM_INFO_INTERLEAVED
                | bindings::SNDRV_PCM_INFO_BLOCK_TRANSFER,
            formats: self.format.alsa_format().unwrap_or(0),
            subformats: 0,
            // SAFETY: a pure lookup.
            rates: unsafe { bindings::snd_pcm_rate_to_rate_bit(self.format.rate) },
            rate_min: self.format.rate,
            rate_max: self.format.rate,
            channels_min: self.format.channels,
            channels_max: self.format.channels,
            buffer_bytes_max: BUFFER_BYTES_MAX,
            period_bytes_min: period_min,
            period_bytes_max: PERIOD_BYTES_MAX,
            periods_min: PERIODS_MIN,
            periods_max: (BUFFER_BYTES_MAX / period_min) as u32,
            fifo_size: 0,
        }
    }
}

impl ReportListener for LpaiCapture {
    fn process_report(&self, _subtype: u16, report: &[u8]) -> Result<()> {
        let frame = self.format.frame_bytes();
        let span = match parse_sample_report(report, frame) {
            Ok(s) => s,
            Err(ReportError::NotSamples) => return Ok(()),
            Err(ReportError::Malformed) => {
                if self.warn_once(WARN_MALFORMED) {
                    dev_warn!(self.dev, "malformed lpai report ({} bytes)\n", report.len());
                }
                return Ok(());
            }
        };
        let mut st = self.stream.lock();
        if !self.running.load(Relaxed) || st.substream.is_null() || st.buffer_bytes == 0 {
            return Ok(());
        }
        let skip = match continuity(st.copied, &span) {
            Continuity::Stale => {
                if self.warn_once(WARN_BACKWARDS) {
                    dev_warn!(self.dev, "lpai producer moved backwards\n");
                }
                return Ok(());
            }
            Continuity::Copy { skip, gap } => {
                if gap && self.warn_once(WARN_GAP) {
                    dev_warn!(self.dev, "lpai stream gap, continuing at the new position\n");
                }
                skip
            }
        };
        let samples = &report[span.offset + skip..span.offset + span.len];
        if samples.len() >= st.buffer_bytes {
            if self.warn_once(WARN_OVERRUN) {
                dev_warn!(self.dev, "lpai report larger than the ALSA buffer\n");
            }
            st.copied = Some(span.end);
            return Ok(());
        }
        // SAFETY: the substream is registered in the stream state, which
        // close clears under this lock, so it is live; its runtime and buffer
        // exist between hw_params and hw_free, which bracket prepare (where
        // `buffer_bytes` was taken) and every triggered state.
        let dma_area = unsafe { (*(*st.substream).runtime).dma_area };
        // SAFETY: as above; `hw_ptr_bytes` stays below `buffer_bytes`.
        let new_ptr = unsafe { copy_into(dma_area, st.buffer_bytes, st.hw_ptr_bytes, samples) };
        st.hw_ptr_bytes = new_ptr;
        st.copied = Some(span.end);
        st.period_acc += samples.len();
        self.hw_ptr_frames.store((new_ptr / frame) as u32, Release);
        if st.period_bytes != 0 && st.period_acc >= st.period_bytes {
            st.period_acc %= st.period_bytes;
            // SAFETY: the substream is valid while registered in the state.
            unsafe { bindings::snd_pcm_period_elapsed(st.substream) };
        }
        Ok(())
    }
}

/// # Safety
///
/// `substream` must be a live substream of a PCM made by
/// [`LpaiCapture::new_pcm`].
unsafe fn pcm_data<'a>(substream: *mut bindings::snd_pcm_substream) -> ArcBorrow<'a, LpaiCapture> {
    // SAFETY: by the caller's contract the private data is the PCM's `Arc`.
    unsafe { Arc::<LpaiCapture>::borrow((*substream).private_data.cast()) }
}

unsafe extern "C" fn pcm_open(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    // SAFETY: the runtime exists for the op's duration.
    unsafe { (*(*substream).runtime).hw = data.hw() };
    data.running.store(false, Relaxed);
    let mut st = data.stream.lock();
    st.substream = substream;
    st.copied = None;
    st.hw_ptr_bytes = 0;
    st.period_acc = 0;
    st.buffer_bytes = 0;
    data.hw_ptr_frames.store(0, Release);
    0
}

unsafe extern "C" fn pcm_close(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    data.running.store(false, Relaxed);
    data.stream.lock().substream = ptr::null_mut();
    if data.powered.xchg(false, Relaxed) {
        if let Err(e) = data.set_power(POWER_STATE_IDLE) {
            return e.to_errno();
        }
    }
    0
}

unsafe extern "C" fn pcm_prepare(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    {
        let mut st = data.stream.lock();
        // SAFETY: the runtime and its buffer exist between hw_params and
        // hw_free, which bracket prepare.
        let rt = unsafe { &*(*substream).runtime };
        st.buffer_bytes = rt.dma_bytes;
        st.period_bytes = rt.period_size as usize * data.format.frame_bytes();
        st.hw_ptr_bytes = 0;
        st.period_acc = 0;
        st.copied = None;
        data.hw_ptr_frames.store(0, Release);
    }
    if !data.powered.load(Relaxed) {
        if let Err(e) = data.set_power(POWER_STATE_RUN) {
            // Leave the firmware idle when its answer was a refusal or a
            // state other than `runn`.
            let _ = data.set_power(POWER_STATE_IDLE);
            return e.to_errno();
        }
        data.powered.store(true, Relaxed);
    }
    0
}

/// Called with the ALSA stream lock held: only the run flag changes here.
unsafe extern "C" fn pcm_trigger(substream: *mut bindings::snd_pcm_substream, cmd: i32) -> i32 {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    match cmd as u32 {
        bindings::SNDRV_PCM_TRIGGER_START | bindings::SNDRV_PCM_TRIGGER_RESUME => {
            data.running.store(true, Relaxed);
            0
        }
        bindings::SNDRV_PCM_TRIGGER_STOP | bindings::SNDRV_PCM_TRIGGER_SUSPEND => {
            data.running.store(false, Relaxed);
            0
        }
        _ => EINVAL.to_errno(),
    }
}

/// A report that saw the stream running holds the stream lock for its whole
/// copy, so taking the lock waits for it.
unsafe extern "C" fn pcm_sync_stop(substream: *mut bindings::snd_pcm_substream) -> i32 {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    drop(data.stream.lock());
    0
}

unsafe extern "C" fn pcm_pointer(
    substream: *mut bindings::snd_pcm_substream,
) -> bindings::snd_pcm_uframes_t {
    // SAFETY: ALSA calls this op with a live substream of the PCM.
    let data = unsafe { pcm_data(substream) };
    data.hw_ptr_frames.load(Acquire) as bindings::snd_pcm_uframes_t
}

unsafe extern "C" fn pcm_free_private(pcm: *mut bindings::snd_pcm) {
    // SAFETY: this is the `Arc` leaked into the PCM's private data in
    // `new_pcm`; ALSA calls this once, when the PCM goes away.
    unsafe { drop(Arc::<LpaiCapture>::from_foreign((*pcm).private_data.cast())) };
}

static PCM_OPS: bindings::snd_pcm_ops = bindings::snd_pcm_ops {
    open: Some(pcm_open),
    close: Some(pcm_close),
    prepare: Some(pcm_prepare),
    trigger: Some(pcm_trigger),
    pointer: Some(pcm_pointer),
    ioctl: None,
    hw_params: None,
    hw_free: None,
    sync_stop: Some(pcm_sync_stop),
    get_time_info: None,
    fill_silence: None,
    copy: None,
    page: None,
    mmap: None,
    ack: None,
};

#[kunit_tests(snd_soc_apple_aop_lpai)]
mod tests {
    use super::*;

    const FRAME: usize = 6;

    fn report(end: u64, kind: u32, packet: u32, extra: usize) -> KVec<u8> {
        let mut r = KVec::new();
        r.resize(REPORT_INNER + INNER_HEADER_BYTES + extra, 0, GFP_KERNEL)
            .unwrap();
        r[REPORT_PRODUCER_END..REPORT_PRODUCER_END + 8].copy_from_slice(&end.to_le_bytes());
        r[REPORT_INNER + INNER_KIND..REPORT_INNER + INNER_KIND + 4]
            .copy_from_slice(&kind.to_le_bytes());
        r[REPORT_INNER + INNER_PACKET_BYTES..REPORT_INNER + INNER_PACKET_BYTES + 4]
            .copy_from_slice(&packet.to_le_bytes());
        r
    }

    #[test]
    fn a_sample_report_covers_the_span_ending_at_the_producer_count() {
        let n = 10 * FRAME;
        let r = report(600, INNER_KIND_SAMPLES, (INNER_HEADER_BYTES + n) as u32, n);
        assert_eq!(
            parse_sample_report(&r, FRAME),
            Ok(SampleSpan {
                start: 540,
                end: 600,
                offset: REPORT_INNER + INNER_HEADER_BYTES,
                len: n
            })
        );
    }

    #[test]
    fn bad_lengths_and_other_kinds_are_rejected() {
        let n = 4 * FRAME;
        let other = report(600, 2, (INNER_HEADER_BYTES + n) as u32, n);
        assert_eq!(parse_sample_report(&other, FRAME), Err(ReportError::NotSamples));
        let long = report(600, INNER_KIND_SAMPLES, (INNER_HEADER_BYTES + n + 6) as u32, n);
        assert_eq!(parse_sample_report(&long, FRAME), Err(ReportError::Malformed));
        let partial = report(600, INNER_KIND_SAMPLES, (INNER_HEADER_BYTES + n - 1) as u32, n);
        assert_eq!(parse_sample_report(&partial, FRAME), Err(ReportError::Malformed));
        let short_hdr = report(600, INNER_KIND_SAMPLES, 8, n);
        assert_eq!(parse_sample_report(&short_hdr, FRAME), Err(ReportError::Malformed));
        let beyond = report(6, INNER_KIND_SAMPLES, (INNER_HEADER_BYTES + n) as u32, n);
        assert_eq!(parse_sample_report(&beyond, FRAME), Err(ReportError::Malformed));
        assert_eq!(parse_sample_report(&[0u8; 8], FRAME), Err(ReportError::Malformed));
    }

    #[test]
    fn continuity_follows_the_absolute_byte_count() {
        let span = |start, end| SampleSpan { start, end, offset: 0, len: (end - start) as usize };
        assert_eq!(continuity(None, &span(60, 120)), Continuity::Copy { skip: 0, gap: false });
        assert_eq!(continuity(Some(60), &span(60, 120)), Continuity::Copy { skip: 0, gap: false });
        assert_eq!(continuity(Some(30), &span(60, 120)), Continuity::Copy { skip: 0, gap: true });
        assert_eq!(continuity(Some(90), &span(60, 120)), Continuity::Copy { skip: 30, gap: false });
        assert_eq!(continuity(Some(120), &span(60, 120)), Continuity::Stale);
    }

    #[test]
    fn the_stream_format_is_taken_from_the_property() {
        let mut p = [0u8; 16];
        p[0..4].copy_from_slice(&1u32.to_le_bytes());
        p[4..8].copy_from_slice(&16000u32.to_le_bytes());
        p[8..12].copy_from_slice(&3u32.to_le_bytes());
        p[12..16].copy_from_slice(&2u32.to_le_bytes());
        let f = StreamFormat::parse(&p).unwrap();
        assert_eq!(f.frame_bytes(), 6);
        p[0..4].copy_from_slice(&0u32.to_le_bytes());
        assert!(StreamFormat::parse(&p).is_none());
        p[0..4].copy_from_slice(&1u32.to_le_bytes());
        p[12..16].copy_from_slice(&3u32.to_le_bytes());
        assert!(StreamFormat::parse(&p).is_none());
    }

    #[test]
    fn the_power_timestamp_counts_the_24_mhz_timebase() {
        assert_eq!(aop_counter_ticks(24_000_000, 24_000_000), Ok(24_000_000));
        assert_eq!(aop_counter_ticks(1_000_000_000, 1_000_000_000), Ok(24_000_000));
        assert!(aop_counter_ticks(1, 0).is_err());
    }
}
