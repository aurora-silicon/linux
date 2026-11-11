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
//! T6041 firmware produces into a host ring instead.  The AP allocates it on
//! the audio node's own DART stream (the I/O buffer device's stream) and
//! hands its address and size to the firmware's I/O buffer device `adpx`
//! (property 300), once per AOP lifetime.  `lpai` property 301 is the stream
//! format and 302 the ring geometry (ring frames, frames per report); the
//! firmware's property-change reports (EPIC subtype 0x20, property 305) carry
//! the producer position in the same fields as the T8140 ring's.  The first
//! report after a start only sets the position, so audio from before the start
//! is never copied.
//!
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{mem, ptr, slice};

use kernel::{
    bindings,
    device,
    new_mutex,
    new_spinlock,
    prelude::*,
    soc::apple::aop::{from_fourcc, EPICService, ReportListener, SourceRing, AOP},
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
/// The T6041 firmware's property-change reports, which carry the ring producer position.
const EPIC_SUBTYPE_PROPERTY_CHANGE: u16 = 0x20;
const CALLTYPE_AUDIO_ATTACH_DEVICE: u32 = 0xc3000002;
const CALLTYPE_AUDIO_GET_PROP: u32 = 0xc3000004;
const CALLTYPE_AUDIO_SET_PROP: u32 = 0xc3000005;
const AUDIO_DEV_LPAI: u32 = from_fourcc(b"lpai");
/// The T6041 I/O buffer device: takes the host ring the low-power microphone produces into.
const AUDIO_DEV_IO_BUFFER: u32 = from_fourcc(b"adpx");
/// kIOReturnBusy from the firmware: the device is already attached.
const AUDIO_RET_BUSY: u32 = 0xe00002d5;

/// Property 200: the device's current power state (a FourCC).
const PROP_POWER_STATE: u32 = 200;
/// Property 202: a power request ([`PowerRequest`]).
const PROP_POWER_REQUEST: u32 = 202;
/// Property 302: the stream format of the inline samples ([`StreamFormat`]).
const PROP_STREAM_FORMAT: u32 = 302;
/// T6041: `adpx` property 300, the host ring ([`IoBufferBinding`]).
const PROP_IO_BUFFER: u32 = 300;
/// T6041: `lpai` property 301, the stream format (as the inline firmware's 302).
const PROP_RING_FORMAT: u32 = 301;
/// T6041: `lpai` property 302, the ring geometry: clock domain, a reserved
/// word, the ring's frames and the frames per producer report.
const PROP_RING_GEOMETRY: u32 = 302;
const GEOMETRY_RING_FRAMES: usize = 0x08;
const GEOMETRY_REPORT_FRAMES: usize = 0x10;
/// T6041: the stream status property whose changes report the producer.
const PROP_STREAM_STATUS: u32 = 305;
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

/// T6041 property-change report: an envelope magic, the property and the
/// length of its status, then the status words, among them the index of the
/// last completed frame and the ring's write cursor in bytes.
const CHANGE_MAGIC: usize = 0x24;
const CHANGE_PROPERTY: usize = 0x30;
const CHANGE_STATUS_BYTES: usize = 0x34;
const STATUS_LAST_FRAME: usize = 0x40;
const STATUS_RING_CURSOR: usize = 0x60;
const CHANGE_REPORT_MIN: usize = 0x68;
const PROPERTY_CHANGE_MAGIC: u32 = from_fourcc(b"pchg");
const STREAM_STATUS_BYTES: u32 = 48;

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
const WARN_CURSOR: usize = 4;
const WARN_COUNT: usize = 5;

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
struct AudioAttachDevice {
    _zero0: u32,
    unk0: u32,
    calltype: u32,
    _zero1: [u8; 16],
    _cookie: u32,
    len: u64,
    dev_id: u32,
    _zero2: u32,
}

impl AudioAttachDevice {
    fn new(dev_id: u32) -> Self {
        Self {
            unk0: 0xffffffff,
            calltype: CALLTYPE_AUDIO_ATTACH_DEVICE,
            len: 0x2c,
            dev_id,
            ..Self::default()
        }
    }
}

/// T6041 `adpx` property 300: the ring's first IOVA and its length.
#[repr(C, packed(4))]
#[derive(Clone, Copy, Default)]
struct IoBufferBinding {
    iova: u64,
    bytes: u64,
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

/// The stream format property (302 of the inline-sample firmware, 301 of the
/// T6041 one): a non-zero sample-format selector, the rate, the channels per
/// frame and the bytes per channel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StreamFormat {
    pub(crate) rate: u32,
    pub(crate) channels: u32,
    /// The bytes each sample takes in a frame.
    pub(crate) bytes_per_channel: u32,
    /// The significant bits of a sample, most-significant aligned.
    pub(crate) valid_bits: u32,
}

/// The T6041 stream format (`lpai` property 301) gives the sample's valid
/// bytes; the ring holds every sample in a 4-byte container, most-significant
/// aligned (24 valid bits over a zero low byte), so a frame is four bytes per
/// channel.  The ring geometry and the producer cursor are checked against
/// that frame size.
const RING_CONTAINER_BYTES: u32 = 4;

impl StreamFormat {
    /// Decodes the inline firmware's 16-byte property, whose last word is the
    /// sample size; `None` for a format this driver cannot present.
    pub(crate) fn parse(prop: &[u8]) -> Option<Self> {
        let bytes = le_u32(prop, 0xc)?;
        Self::new(prop, bytes, bytes)
    }

    /// Decodes the T6041 property, whose last word is the valid bytes of a
    /// sample in its 4-byte container.
    pub(crate) fn parse_ring(prop: &[u8]) -> Option<Self> {
        let valid = le_u32(prop, 0xc)?;
        Self::new(prop, RING_CONTAINER_BYTES, valid)
    }

    fn new(prop: &[u8], bytes: u32, valid: u32) -> Option<Self> {
        let selector = le_u32(prop, 0x0)?;
        let fmt = Self {
            rate: le_u32(prop, 0x4)?,
            channels: le_u32(prop, 0x8)?,
            bytes_per_channel: bytes,
            valid_bits: valid.checked_mul(8)?,
        };
        if selector == 0
            || fmt.rate == 0
            || fmt.channels == 0
            || fmt.channels > 8
            || valid == 0
            || valid > bytes
        {
            return None;
        }
        fmt.alsa_format()?;
        Some(fmt)
    }

    pub(crate) fn frame_bytes(&self) -> usize {
        (self.channels * self.bytes_per_channel) as usize
    }

    /// Signed little-endian PCM of the container width.
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

/// A T6041 producer report: the absolute producer byte count and the ring's
/// write cursor, `None` unless it is a well-formed stream-status change.
pub(crate) fn parse_ring_report(report: &[u8], frame_bytes: usize) -> Option<(u64, u64)> {
    if report.len() < CHANGE_REPORT_MIN
        || le_u32(report, CHANGE_MAGIC)? != PROPERTY_CHANGE_MAGIC
        || le_u32(report, CHANGE_PROPERTY)? != PROP_STREAM_STATUS
        || le_u32(report, CHANGE_STATUS_BYTES)? != STREAM_STATUS_BYTES
    {
        return None;
    }
    let absolute = le_u64(report, STATUS_LAST_FRAME)?
        .checked_add(1)?
        .checked_mul(frame_bytes as u64)?;
    Some((absolute, le_u64(report, STATUS_RING_CURSOR)?))
}

/// What the consumer at `copied` does with a ring producer position.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum RingStep {
    /// Take the position without copying (first report, backwards, overrun).
    Rebase,
    Copy(usize),
}

pub(crate) fn ring_step(copied: Option<u64>, absolute: u64, ring_bytes: usize) -> RingStep {
    match copied {
        Some(c) if absolute >= c && ((absolute - c) as usize) < ring_bytes => {
            RingStep::Copy((absolute - c) as usize)
        }
        _ => RingStep::Rebase,
    }
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

/// Where the samples come from.
enum Source {
    /// In the sample reports themselves.
    Inline,
    /// In a host ring of `ring_bytes` the firmware produces into.
    Ring { ring: Arc<SourceRing>, ring_bytes: usize },
}

/// The low-power microphone capture of one AOP audio device.
#[pin_data]
pub(crate) struct LpaiCapture {
    dev: ARef<device::Device>,
    adata: Arc<dyn AOP>,
    service: EPICService,
    format: StreamFormat,
    source: Source,
    /// The report subtype the listener takes.
    subtype: u16,
    /// The power request's device field: `lpai` on the inline firmware, 0 on T6041.
    power_dev_id: u32,
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
        Self::register(dev, adata, service, format, Source::Inline, EPIC_SUBTYPE_LPAI_SAMPLES, AUDIO_DEV_LPAI)
            .map(Some)
    }

    /// T6041: binds the host ring and registers the producer listener.  `lpai`
    /// is already attached with its channel masks set.  The ring is allocated
    /// on `bus`, the audio node, whose DART stream is the I/O buffer device's;
    /// a node without one yields `Ok(None)`.
    ///
    /// The geometry is read before the binding so the ring has exactly the
    /// size the firmware describes (frames times frame bytes).
    pub(crate) fn new_ring(
        bus: &device::Device<device::Bound>,
        dev: &ARef<device::Device>,
        adata: &Arc<dyn AOP>,
        service: EPICService,
        has_iommu: bool,
    ) -> Result<Option<Arc<Self>>> {
        if !has_iommu {
            return Ok(None);
        }
        let attach = AudioAttachDevice::new(AUDIO_DEV_IO_BUFFER);
        // SAFETY: the request is a plain packed firmware record.
        let bytes = unsafe {
            slice::from_raw_parts(ptr::from_ref(&attach).cast::<u8>(), mem::size_of_val(&attach))
        };
        match adata.epic_call(&service, EPIC_SUBTYPE_WRAPPED_CALL, bytes)? {
            0 | AUDIO_RET_BUSY => {}
            ret => {
                dev_err!(dev, "unable to attach the lpai I/O buffer: {:#x}\n", ret);
                return Err(EIO);
            }
        }
        let geo = Self::get_prop(adata, &service, dev, PROP_RING_GEOMETRY, 24)?;
        let prop = Self::get_prop(adata, &service, dev, PROP_RING_FORMAT, 16)?;
        let Some(format) = StreamFormat::parse_ring(&prop) else {
            dev_warn!(
                dev,
                "lpai: unsupported stream format ({} Hz, {} channels, {} bytes), no low-power PCM\n",
                le_u32(&prop, 0x4).unwrap_or(0),
                le_u32(&prop, 0x8).unwrap_or(0),
                le_u32(&prop, 0xc).unwrap_or(0)
            );
            return Ok(None);
        };
        let frames = le_u64(&geo, GEOMETRY_RING_FRAMES).ok_or(EIO)?;
        let quantum = le_u64(&geo, GEOMETRY_REPORT_FRAMES).ok_or(EIO)?;
        let ring_bytes = frames
            .checked_mul(format.frame_bytes() as u64)
            .filter(|b| *b > 0 && *b <= u64::from(u32::MAX))
            .ok_or(EIO)? as usize;
        if quantum == 0 || quantum > frames / 2 {
            dev_err!(dev, "invalid lpai ring geometry: {} frames, {} per report\n", frames, quantum);
            return Err(EIO);
        }
        let ring = adata.bound_ring(bus, ring_bytes, &mut |iova, len| {
            let req = AudioSetDeviceProp::new(
                AUDIO_DEV_IO_BUFFER,
                PROP_IO_BUFFER,
                IoBufferBinding { iova, bytes: len },
            );
            // SAFETY: the request is a plain packed firmware record.
            let bytes = unsafe {
                slice::from_raw_parts(ptr::from_ref(&req).cast::<u8>(), mem::size_of_val(&req))
            };
            match adata.epic_call(&service, EPIC_SUBTYPE_WRAPPED_CALL, bytes)? {
                0 => Ok(()),
                ret => {
                    dev_err!(dev, "lpai ring binding refused: {:#x}\n", ret);
                    Err(EIO)
                }
            }
        })?;
        dev_dbg!(
            dev,
            "lpai: {} Hz, {} channels, ring {} frames, {} per report\n",
            format.rate,
            format.channels,
            frames,
            quantum
        );
        Self::register(
            dev,
            adata,
            service,
            format,
            Source::Ring { ring, ring_bytes },
            EPIC_SUBTYPE_PROPERTY_CHANGE,
            0,
        )
        .map(Some)
    }

    fn register(
        dev: &ARef<device::Device>,
        adata: &Arc<dyn AOP>,
        service: EPICService,
        format: StreamFormat,
        source: Source,
        subtype: u16,
        power_dev_id: u32,
    ) -> Result<Arc<Self>> {
        let this = Arc::pin_init(
            try_pin_init!(Self {
                dev: dev.clone(),
                adata: adata.clone(),
                service,
                format,
                source,
                subtype,
                power_dev_id,
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
        adata.add_report_listener(service, subtype, listener)?;
        Ok(this)
    }

    /// Stops report delivery; returns once no report callback runs.
    pub(crate) fn detach(&self) {
        self.running.store(false, Relaxed);
        self.adata.remove_report_listener(&self.service, self.subtype);
    }

    fn set_power(&self, state: u32) -> Result<()> {
        let _guard = self.power.lock();
        let sequence = self.sequence.fetch_add(1, Relaxed).wrapping_add(1);
        let req = PowerRequest {
            dev_id: self.power_dev_id,
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

impl LpaiCapture {
    /// Copies what a ring producer report announces.
    fn process_ring_report(&self, ring: &SourceRing, ring_bytes: usize, report: &[u8]) -> Result<()> {
        let frame = self.format.frame_bytes();
        let Some((absolute, cursor)) = parse_ring_report(report, frame) else {
            // Other property changes share the subtype.
            return Ok(());
        };
        if cursor != absolute % ring_bytes as u64 {
            if self.warn_once(WARN_CURSOR) {
                dev_warn!(self.dev, "inconsistent lpai report: {} bytes, cursor {:#x}\n", absolute, cursor);
            }
            return Ok(());
        }
        let mut st = self.stream.lock();
        if !self.running.load(Relaxed) || st.substream.is_null() || st.buffer_bytes == 0 {
            return Ok(());
        }
        let len = match ring_step(st.copied, absolute, ring_bytes) {
            RingStep::Rebase => {
                if let Some(c) = st.copied {
                    let class = if absolute < c { WARN_BACKWARDS } else { WARN_OVERRUN };
                    if self.warn_once(class) {
                        dev_warn!(self.dev, "lpai ring position lost, continuing at the producer\n");
                    }
                }
                st.copied = Some(absolute);
                return Ok(());
            }
            RingStep::Copy(0) => return Ok(()),
            RingStep::Copy(len) => len,
        };
        let from = st.copied.unwrap_or(absolute);
        // SAFETY: as for the inline reports below.
        let dma_area = unsafe { (*(*st.substream).runtime).dma_area };
        let mut src = (from % ring_bytes as u64) as usize;
        let mut dst = st.hw_ptr_bytes;
        let mut left = len;
        while left > 0 {
            let n = left.min(ring_bytes - src).min(st.buffer_bytes - dst);
            // SAFETY: dst + n <= buffer_bytes, and the buffer stays mapped
            // while the substream is set in the stream state.
            let span = unsafe { slice::from_raw_parts_mut(dma_area.add(dst), n) };
            ring.read(src, span)?;
            src = (src + n) % ring_bytes;
            dst = (dst + n) % st.buffer_bytes;
            left -= n;
        }
        self.advance(&mut st, absolute, dst, len);
        Ok(())
    }

    fn advance(&self, st: &mut StreamState, absolute: u64, new_ptr: usize, len: usize) {
        st.hw_ptr_bytes = new_ptr;
        st.copied = Some(absolute);
        st.period_acc += len;
        self.hw_ptr_frames
            .store((new_ptr / self.format.frame_bytes()) as u32, Release);
        if st.period_bytes != 0 && st.period_acc >= st.period_bytes {
            st.period_acc %= st.period_bytes;
            // SAFETY: the substream is valid while registered in the state.
            unsafe { bindings::snd_pcm_period_elapsed(st.substream) };
        }
    }
}

impl ReportListener for LpaiCapture {
    fn process_report(&self, _subtype: u16, report: &[u8]) -> Result<()> {
        if let Source::Ring { ring, ring_bytes } = &self.source {
            return self.process_ring_report(ring, *ring_bytes, report);
        }
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
        self.advance(&mut st, span.end, new_ptr, samples.len());
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
    let width = data.format.bytes_per_channel * 8;
    if data.format.valid_bits < width {
        // SAFETY: as above; the constraint only describes the samples.
        let ret = unsafe {
            bindings::snd_pcm_hw_constraint_msbits(
                (*substream).runtime,
                0,
                width,
                data.format.valid_bits,
            )
        };
        if ret < 0 {
            return ret;
        }
    }
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
        assert_eq!((f.frame_bytes(), f.valid_bits), (6, 16));
        assert_eq!(f.alsa_format(), Some(bindings::BINDINGS_SNDRV_PCM_FMTBIT_S16_LE));
        // T6041: three valid bytes in each 4-byte container.
        p[12..16].copy_from_slice(&3u32.to_le_bytes());
        let r = StreamFormat::parse_ring(&p).unwrap();
        assert_eq!((r.frame_bytes(), r.valid_bits), (12, 24));
        assert_eq!(r.alsa_format(), Some(bindings::BINDINGS_SNDRV_PCM_FMTBIT_S32_LE));
        assert!(StreamFormat::parse(&p).is_none());
        p[12..16].copy_from_slice(&5u32.to_le_bytes());
        assert!(StreamFormat::parse_ring(&p).is_none());
        p[12..16].copy_from_slice(&2u32.to_le_bytes());
        p[0..4].copy_from_slice(&0u32.to_le_bytes());
        assert!(StreamFormat::parse(&p).is_none());
    }

    fn change(last_frame: u64, cursor: u64, property: u32) -> [u8; CHANGE_REPORT_MIN] {
        let mut r = [0u8; CHANGE_REPORT_MIN];
        r[CHANGE_MAGIC..CHANGE_MAGIC + 4].copy_from_slice(&PROPERTY_CHANGE_MAGIC.to_le_bytes());
        r[CHANGE_PROPERTY..CHANGE_PROPERTY + 4].copy_from_slice(&property.to_le_bytes());
        r[CHANGE_STATUS_BYTES..CHANGE_STATUS_BYTES + 4]
            .copy_from_slice(&STREAM_STATUS_BYTES.to_le_bytes());
        r[STATUS_LAST_FRAME..STATUS_LAST_FRAME + 8].copy_from_slice(&last_frame.to_le_bytes());
        r[STATUS_RING_CURSOR..STATUS_RING_CURSOR + 8].copy_from_slice(&cursor.to_le_bytes());
        r
    }

    #[test]
    fn a_ring_report_gives_the_producer_after_the_last_completed_frame() {
        let r = change(3199, 38400, PROP_STREAM_STATUS);
        assert_eq!(parse_ring_report(&r, 12), Some((38400, 38400)));
        assert_eq!(parse_ring_report(&change(3199, 38400, 304), 12), None);
        assert_eq!(parse_ring_report(&r[..CHANGE_REPORT_MIN - 1], 12), None);
        assert_eq!(parse_ring_report(&change(u64::MAX, 0, PROP_STREAM_STATUS), 12), None);
    }

    #[test]
    fn the_ring_consumer_starts_at_the_producer_and_rebases_on_loss() {
        assert_eq!(ring_step(None, 38400, 768000), RingStep::Rebase);
        assert_eq!(ring_step(Some(38400), 76800, 768000), RingStep::Copy(38400));
        assert_eq!(ring_step(Some(76800), 38400, 768000), RingStep::Rebase);
        assert_eq!(ring_step(Some(0), 768000, 768000), RingStep::Rebase);
        assert_eq!(ring_step(Some(0), 767988, 768000), RingStep::Copy(767988));
    }

    #[test]
    fn the_power_timestamp_counts_the_24_mhz_timebase() {
        assert_eq!(aop_counter_ticks(24_000_000, 24_000_000), Ok(24_000_000));
        assert_eq!(aop_counter_ticks(1_000_000_000, 1_000_000_000), Ok(24_000_000));
        assert!(aop_counter_ticks(1, 0).is_err());
    }
}
