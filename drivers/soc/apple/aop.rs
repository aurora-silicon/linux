// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Apple AOP driver
//!
//! Copyright (C) The Asahi Linux Contributors

use core::{
    arch::asm,
    cmp,
    mem,
    ptr::{self, NonNull},
    slice, //
};

use kernel::{
    bindings,
    c_str,
    device,
    device::Core,
    dma::{
        Coherent,
        Device,
        DmaMask, //
    },
    error::from_err_ptr,
    fmt,
    io::{
        mem::IoMem,
        Io,
        RelaxedMmio, //
    },
    iosys_map::IoSysMapRef,
    module_platform_driver,
    new_condvar,
    new_mutex,
    of,
    platform,
    prelude::*,
    soc::apple::aop::{
        from_fourcc,
        EPICService,
        FakehidListener,
        AOP, //
    },
    soc::apple::rtkit,
    sync::{
        aref::ARef,
        atomic::{
            Acquire,
            Atomic,
            Release, //
        },
        Arc,
        ArcBorrow,
        CondVar,
        CondVarTimeoutResult,
        Mutex, //
    },
    time::msecs_to_jiffies,
    types::{
        ForeignOwnable,
        ScopeGuard, //
    },
    workqueue::{
        impl_has_work,
        new_work,
        OwnedQueue,
        Work,
        WorkItem, //
    }, //
};

const AOP_MAX_CALLS: usize = 8;
const AOP_MMIO_SIZE: usize = 0x1e0000;
const ASC_MMIO_SIZE: usize = 0x4000;
const BOOTARGS_OFFSET: usize = 0x22c;
const BOOTARGS_SIZE: usize = 0x230;
const CPU_CONTROL: usize = 0x44;
const CPU_RUN: u32 = 0x1 << 4;
const AFK_ENDPOINT_START: u8 = 0x20;
const AFK_ENDPOINT_COUNT: u8 = 0xf;
const AFK_OPC_GET_BUF: u64 = 0x89;
const AFK_OPC_INIT: u64 = 0x80;
const AFK_OPC_INIT_RX: u64 = 0x8b;
const AFK_OPC_INIT_TX: u64 = 0x8a;
const AFK_OPC_INIT_UNK: u64 = 0x8c;
const AFK_OPC_SEND: u64 = 0xa2;
const AFK_OPC_START_ACK: u64 = 0x86;
const AFK_OPC_SHUTDOWN_ACK: u64 = 0xc1;
const AFK_OPC_RECV: u64 = 0x85;
const AFK_MSG_GET_BUF_ACK: u64 = 0xa1 << 48;
const AFK_MSG_INIT: u64 = AFK_OPC_INIT << 48;
const AFK_MSG_INIT_ACK: u64 = 0xa0 << 48;
const AFK_MSG_START: u64 = 0xa3 << 48;
const AFK_MSG_SHUTDOWN: u64 = 0xc0 << 48;
const AFK_RB_BLOCK_STEP: usize = 0x40;
const EPIC_TYPE_NOTIFY: u32 = 0;
const EPIC_CATEGORY_REPORT: u8 = 0x00;
const EPIC_CATEGORY_NOTIFY: u8 = 0x10;
const EPIC_CATEGORY_REPLY: u8 = 0x20;
const EPIC_SUBTYPE_STD_SERVICE: u16 = 0xc0;
const EPIC_SUBTYPE_FAKEHID_REPORT: u16 = 0xc4;
const EPIC_SUBTYPE_RETCODE: u16 = 0x84;
const EPIC_SUBTYPE_RETCODE_PAYLOAD: u16 = 0xa0;
const EPIC_SUBTYPE_STRING: u16 = 0x8a;
const QE_MAGIC1: u32 = from_fourcc(b" POI");
const QE_MAGIC2: u32 = from_fourcc(b" POA");
/// Bound on the wait for the reply to an EPIC call. The firmware can stop
/// answering, and a caller must not be left in D state forever.
const EPIC_CALL_TIMEOUT_MS: u32 = 5000;
/// Bound on the wait for an endpoint's shutdown acknowledgment.
const AFK_SHUTDOWN_TIMEOUT_MS: u32 = 5000;

fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

/// Index into the AFK endpoint tables for RTKit endpoint `ep`, if it is one
/// of the endpoints this driver drives.
fn afk_endpoint_index(ep: u8) -> Option<usize> {
    ep.checked_sub(AFK_ENDPOINT_START)
        .filter(|i| *i < AFK_ENDPOINT_COUNT)
        .map(usize::from)
}

/// Reads the little-endian `u16` at `off`; `b` must hold it.
fn le_u16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

/// Reads the little-endian `u32` at `off`; `b` must hold it.
fn le_u32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

#[inline(always)]
fn mem_sync() {
    unsafe {
        asm!("dsb sy");
    }
}

#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct QEHeader {
    magic: u32,
    size: u32,
    channel: u32,
    ty: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct EPICHeader {
    version: u8,
    seq: u16,
    _pad0: u8,
    _unk0: u32,
    timestamp: u64,
    // Subheader
    length: u32,
    sub_version: u8,
    category: u8,
    subtype: u16,
    tag: u16,
    _unk1: u16,
    _pad1: u64,
    inline_len: u32,
}

/// The EPIC header fields that message dispatch needs.
struct EPICHeaderFields {
    category: u8,
    subtype: u16,
    tag: u16,
}

impl EPICHeaderFields {
    /// Splits `msg` into its decoded header and its payload, or returns `None`
    /// when `msg` is too short to hold a header.
    fn decode(msg: &[u8]) -> Option<(EPICHeaderFields, &[u8])> {
        if msg.len() < mem::size_of::<EPICHeader>() {
            return None;
        }
        let (hdr, data) = msg.split_at(mem::size_of::<EPICHeader>());
        let fields = EPICHeaderFields {
            category: hdr[mem::offset_of!(EPICHeader, category)],
            subtype: le_u16(hdr, mem::offset_of!(EPICHeader, subtype)),
            tag: le_u16(hdr, mem::offset_of!(EPICHeader, tag)),
        };
        Some((fields, data))
    }
}

/// A service announcement (report subtype [`EPIC_SUBTYPE_STD_SERVICE`]):
/// a NUL-padded 32-byte service name followed by the channel the service
/// answers on. Its trailing 8 bytes are not used here.
const EPIC_ANNOUNCE_NAME_LEN: usize = 32;
const EPIC_ANNOUNCE_CHANNEL_OFFSET: usize = 32;
const EPIC_ANNOUNCE_LEN: usize = 44;

#[pin_data]
struct FutureValue<T> {
    #[pin]
    val: Mutex<Option<T>>,
    #[pin]
    completion: CondVar,
}

impl<T> FutureValue<T> {
    fn pin_init() -> impl PinInit<FutureValue<T>> {
        pin_init!(
            FutureValue {
                val <- new_mutex!(None),
                completion <- new_condvar!()
            }
        )
    }
    fn complete(&self, val: T) {
        *self.val.lock() = Some(val);
        self.completion.notify_all();
    }
    /// Waits at most `timeout_ms` for the value; `None` if it did not arrive.
    ///
    /// The wait is uninterruptible: a firmware transaction must not be
    /// abandoned because the calling task has a signal pending, or its reply
    /// would arrive for a call that no longer exists.
    fn wait_timeout(&self, timeout_ms: u32) -> Option<T> {
        let mut ret_guard = self.val.lock();
        let mut left = msecs_to_jiffies(timeout_ms);
        while ret_guard.is_none() {
            match self.completion.wait_timeout(&mut ret_guard, left) {
                CondVarTimeoutResult::Timeout => break,
                CondVarTimeoutResult::Woken { jiffies }
                | CondVarTimeoutResult::Signal { jiffies } => left = jiffies,
            }
        }
        ret_guard.take()
    }
    fn reset(&self) {
        *self.val.lock() = None;
    }
}

struct AFKRingBuffer {
    offset: usize,
    block_size: usize,
    buf_size: usize,
}

struct CallResult {
    retcode: u32,
    extra_data: Option<KVec<u8>>,
}

/// An EPIC call in flight on an endpoint. The slot index plus one is the tag
/// the firmware echoes in the reply.
enum CallSlot {
    /// The caller waits for the reply; the buffer receives the reply payload.
    Pending(Arc<FutureValue<CallResult>>, Option<KVec<u8>>),
    /// The caller gave up waiting. The slot stays reserved, so that its tag is
    /// not handed to a later call that the late reply would then complete,
    /// until that reply arrives and is dropped.
    Abandoned,
}

struct AFKEndpoint {
    index: u8,
    /// The AFK handshake was started; only such endpoints are shut down.
    started: bool,
    iomem: Option<Coherent<[u8]>>,
    txbuf: Option<AFKRingBuffer>,
    rxbuf: Option<AFKRingBuffer>,
    seq: u16,
    calls: [Option<CallSlot>; AOP_MAX_CALLS],
    /// Messages this endpoint could not handle, for throttling their logging.
    dropped: u32,
}

impl AFKEndpoint {
    fn new(index: u8) -> AFKEndpoint {
        AFKEndpoint {
            index,
            started: false,
            iomem: None,
            txbuf: None,
            rxbuf: None,
            seq: 0,
            calls: [const { None }; AOP_MAX_CALLS],
            dropped: 0,
        }
    }

    fn start(&self, rtkit: Pin<&mut rtkit::RtKit<AopData>>) -> Result<()> {
        rtkit.send_message(self.index, AFK_MSG_INIT)
    }

    fn stop(&self, rtkit: Pin<&mut rtkit::RtKit<AopData>>) -> Result<()> {
        rtkit.send_message(self.index, AFK_MSG_SHUTDOWN)
    }

    fn recv_message(
        &mut self,
        client: ArcBorrow<'_, AopData>,
        rtkit: Pin<&mut rtkit::RtKit<AopData>>,
        msg: u64,
    ) -> Result<()> {
        let opc = msg >> 48;
        match opc {
            AFK_OPC_INIT => {
                rtkit.send_message(self.index, AFK_MSG_INIT_ACK)?;
            }
            AFK_OPC_GET_BUF => {
                self.recv_get_buf(client.dev.clone(), rtkit, msg)?;
            }
            AFK_OPC_INIT_UNK => {} // no-op
            AFK_OPC_START_ACK => {}
            AFK_OPC_INIT_RX => {
                if self.rxbuf.is_some() {
                    dev_err!(
                        client.dev,
                        "Got InitRX message with existing rxbuf at endpoint {}",
                        self.index
                    );
                    return Err(EIO);
                }
                self.rxbuf = Some(self.parse_ring_buf(msg)?);
                if self.txbuf.is_some() {
                    rtkit.send_message(self.index, AFK_MSG_START)?;
                }
            }
            AFK_OPC_INIT_TX => {
                if self.txbuf.is_some() {
                    dev_err!(
                        client.dev,
                        "Got InitTX message with existing txbuf at endpoint {}",
                        self.index
                    );
                    return Err(EIO);
                }
                self.txbuf = Some(self.parse_ring_buf(msg)?);
                if self.rxbuf.is_some() {
                    rtkit.send_message(self.index, AFK_MSG_START)?;
                }
            }
            AFK_OPC_RECV => {
                self.recv_rb(client)?;
            }
            AFK_OPC_SHUTDOWN_ACK => {
                client.shutdown_complete(self.index);
            }
            _ => dev_err!(
                client.dev,
                "AFK endpoint {} got unknown message {}",
                self.index,
                msg
            ),
        }
        Ok(())
    }

    /// Decodes an InitRX/InitTX message: a ring of `size` bytes at `offset`
    /// in the shared buffer, laid out as three header blocks (buffer size,
    /// read pointer, write pointer) followed by `buf_size` bytes of entries.
    fn parse_ring_buf(&self, msg: u64) -> Result<AFKRingBuffer> {
        let msg = msg as usize;
        let size = ((msg >> 16) & 0xFFFF) * AFK_RB_BLOCK_STEP;
        let offset = ((msg >> 32) & 0xFFFF) * AFK_RB_BLOCK_STEP;
        let iomem_size = self.iomem.as_ref().ok_or(ENXIO)?.size();
        let buf_size = self.iomem_read32(offset)? as usize;
        let block_size = size.checked_sub(buf_size).ok_or(EIO)? / 3;
        let end = offset.checked_add(size).ok_or(EIO)?;
        if end > iomem_size || buf_size == 0 || block_size == 0 || !block_size.is_power_of_two() {
            return Err(EIO);
        }
        Ok(AFKRingBuffer {
            offset,
            block_size,
            buf_size,
        })
    }

    /// Returns a pointer to `len` bytes at `off` in the shared buffer after
    /// checking that they lie inside it.
    fn iomem_ptr(&self, off: usize, len: usize) -> Result<*mut u8> {
        let iomem = self.iomem.as_ref().ok_or(ENXIO)?;
        let end = off.checked_add(len).ok_or(EIO)?;
        if end > iomem.size() {
            return Err(EIO);
        }
        // SAFETY: `off + len` does not exceed the size of the allocation, so
        // the offset pointer stays inside it.
        Ok(unsafe { iomem.as_mut_ptr().cast::<u8>().add(off) })
    }

    /// Writes one of the ring's pointer words. The firmware polls the word
    /// concurrently, so the write is volatile.
    fn iomem_write32(&mut self, off: usize, data: u32) -> Result<()> {
        if off % mem::align_of::<u32>() != 0 {
            return Err(EIO);
        }
        let ptr = self.iomem_ptr(off, mem::size_of::<u32>())?;
        // SAFETY: `ptr` is valid for four bytes and aligned for a `u32`. A
        // volatile write is the kernel's WRITE_ONCE() for a word the device
        // reads at any time.
        unsafe { ptr.cast::<u32>().write_volatile(data.to_le()) };
        Ok(())
    }

    /// Reads one of the ring's pointer words. The firmware updates the word
    /// concurrently, so the read is volatile.
    fn iomem_read32(&self, off: usize) -> Result<u32> {
        if off % mem::align_of::<u32>() != 0 {
            return Err(EIO);
        }
        let ptr = self.iomem_ptr(off, mem::size_of::<u32>())?;
        // SAFETY: `ptr` is valid for four bytes and aligned for a `u32`. A
        // volatile read is the kernel's READ_ONCE() for a word the device
        // writes at any time.
        Ok(u32::from_le(unsafe { ptr.cast::<u32>().read_volatile() }))
    }

    /// Copies a ring entry out of the shared buffer. Callers only read
    /// entries between the read and the write pointer, which the firmware
    /// has finished writing and does not touch again until the read pointer
    /// passes them.
    fn memcpy_from_iomem(&self, off: usize, target: &mut [u8]) -> Result<()> {
        let src = self.iomem_ptr(off, target.len())?;
        // SAFETY: `src` is valid for `target.len()` bytes, `target` is a
        // distinct allocation, and by the ring protocol (see above) the device
        // does not write the entry while it is copied.
        unsafe { ptr::copy_nonoverlapping(src, target.as_mut_ptr(), target.len()) };
        Ok(())
    }

    /// Copies a ring entry into the shared buffer. Callers only write between
    /// the write and the read pointer, which the firmware does not read until
    /// the write pointer is advanced past the entry.
    fn memcpy_to_iomem(&mut self, off: usize, src: &[u8]) -> Result<()> {
        let dst = self.iomem_ptr(off, src.len())?;
        // SAFETY: `dst` is valid for `src.len()` bytes, `src` is a distinct
        // allocation, and by the ring protocol (see above) the device does
        // not access the entry while it is written.
        unsafe { ptr::copy_nonoverlapping(src.as_ptr(), dst, src.len()) };
        Ok(())
    }

    fn recv_get_buf(
        &mut self,
        dev: ARef<device::Device>,
        rtkit: Pin<&mut rtkit::RtKit<AopData>>,
        msg: u64,
    ) -> Result<()> {
        let size = ((msg & 0xFFFF0000) >> 16) as usize * AFK_RB_BLOCK_STEP;
        if self.iomem.is_some() {
            dev_err!(
                dev,
                "Got GetBuf message with existing buffer on endpoint {}",
                self.index
            );
            return Err(EIO);
        }
        // SAFETY: This runs from the RTKit receive worker. The RTKit handle
        // is dropped by `AopData::remove()`, which runs from unbind or from a
        // failed probe while the device is still bound, and no callback runs
        // once that drop has returned. So the device is bound for as long as
        // this callback runs.
        let bound_dev = unsafe { dev.as_bound() };
        let iomem = Coherent::<u8>::zeroed_slice(bound_dev, size, GFP_KERNEL)?;
        let iova = iomem.dma_handle();
        // Own the buffer before its address leaves the host: should the
        // doorbell fail after the firmware has seen the message, the buffer
        // must not go back to the allocator while the firmware uses it.
        self.iomem = Some(iomem);
        rtkit.send_message(self.index, AFK_MSG_GET_BUF_ACK | iova)?;
        Ok(())
    }

    fn recv_rb(&mut self, client: ArcBorrow<'_, AopData>) -> Result<()> {
        let (buf_offset, block_size, buf_size) = match self.rxbuf.as_ref() {
            Some(b) => (b.offset, b.block_size, b.buf_size),
            None => {
                dev_err!(
                    client.dev,
                    "Got Recv message with no rxbuf at endpoint {}",
                    self.index
                );
                return Err(EIO);
            }
        };
        let mut rptr = self.iomem_read32(buf_offset + block_size)? as usize;
        let mut wptr = self.iomem_read32(buf_offset + block_size * 2)?;
        mem_sync();
        let base = buf_offset + block_size * 3;
        let mut msg_buf = KVec::new();
        const QEH_SIZE: usize = mem::size_of::<QEHeader>();
        while wptr as usize != rptr {
            let mut qeh_bytes = [0; QEH_SIZE];
            self.memcpy_from_iomem(base + rptr, &mut qeh_bytes)?;
            let mut qeh = unsafe { &*(qeh_bytes.as_ptr() as *const QEHeader) };
            if qeh.magic != QE_MAGIC1 && qeh.magic != QE_MAGIC2 {
                let magic = qeh.magic;
                dev_err!(
                    client.dev,
                    "Invalid magic on ep {}, got {:x}",
                    self.index,
                    magic
                );
                return Err(EIO);
            }
            let room = buf_size.checked_sub(rptr + QEH_SIZE).ok_or(EIO)?;
            if qeh.size as usize > room {
                rptr = 0;
                self.memcpy_from_iomem(base + rptr, &mut qeh_bytes)?;
                qeh = unsafe { &*(qeh_bytes.as_ptr() as *const QEHeader) };

                if qeh.magic != QE_MAGIC1 && qeh.magic != QE_MAGIC2 {
                    let magic = qeh.magic;
                    dev_err!(
                        client.dev,
                        "Invalid magic on ep {}, got {:x}",
                        self.index,
                        magic
                    );
                    return Err(EIO);
                }
            }
            msg_buf.resize(qeh.size as usize, 0, GFP_KERNEL)?;
            self.memcpy_from_iomem(base + rptr + QEH_SIZE, &mut msg_buf)?;
            // A message the endpoint cannot handle is skipped. The ring
            // position is still good, and not advancing past the entry would
            // read it again on every doorbell and stall the endpoint for good.
            match EPICHeaderFields::decode(&msg_buf) {
                None => {
                    self.drop_message(&client.dev, fmt!("short message ({} bytes)", msg_buf.len()))
                }
                Some((header, msg)) => {
                    if let Err(e) = self.handle_ipc(client, qeh, &header, msg) {
                        let channel = qeh.channel;
                        self.drop_message(
                            &client.dev,
                            fmt!(
                                "category {:#x} subtype {:#x} tag {} on channel {}: {:?}",
                                header.category,
                                header.subtype,
                                header.tag,
                                channel,
                                e
                            ),
                        );
                    }
                }
            }
            rptr = align_up(rptr + QEH_SIZE + qeh.size as usize, block_size) % buf_size;
            mem_sync();
            self.iomem_write32(buf_offset + block_size, rptr as u32)?;
            wptr = self.iomem_read32(buf_offset + block_size * 2)?;
            mem_sync();
        }
        Ok(())
    }
    /// Counts a message the endpoint could not handle and logs it. The log
    /// is throttled to the powers of two of the count, so that a stream of
    /// such messages cannot flood it.
    fn drop_message(&mut self, dev: &device::Device, why: fmt::Arguments<'_>) {
        self.dropped = self.dropped.saturating_add(1);
        if self.dropped.is_power_of_two() {
            dev_warn!(
                dev,
                "Endpoint {:#04x} dropped a message: {} ({} so far)",
                self.index,
                why,
                self.dropped
            );
        }
    }
    /// Dispatches one message. An error means the message was not handled;
    /// the caller logs it and skips the message.
    fn handle_ipc(
        &mut self,
        client: ArcBorrow<'_, AopData>,
        qhdr: &QEHeader,
        ehdr: &EPICHeaderFields,
        data: &[u8],
    ) -> Result<()> {
        let subtype = ehdr.subtype;
        if ehdr.category == EPIC_CATEGORY_REPORT {
            if subtype == EPIC_SUBTYPE_STD_SERVICE {
                if data.len() < EPIC_ANNOUNCE_LEN {
                    return Err(EMSGSIZE);
                }
                let name = &data[..EPIC_ANNOUNCE_NAME_LEN];
                let name = &name[..name.iter().position(|x| *x == 0).unwrap_or(name.len())];
                let chan = le_u32(data, EPIC_ANNOUNCE_CHANNEL_OFFSET);
                return Into::<Arc<_>>::into(client).register_service(self, chan, name);
            } else if subtype == EPIC_SUBTYPE_FAKEHID_REPORT {
                return client.process_fakehid_report(self, qhdr.channel, data);
            }
            return Err(EINVAL);
        } else if ehdr.category == EPIC_CATEGORY_REPLY {
            if subtype == EPIC_SUBTYPE_RETCODE_PAYLOAD
                || subtype == EPIC_SUBTYPE_RETCODE
                || subtype == EPIC_SUBTYPE_STRING
            {
                if data.len() < mem::size_of::<u32>() {
                    return Err(EMSGSIZE);
                }
                let retcode = le_u32(data, 0);
                let tag = ehdr.tag as usize;
                if tag == 0 || tag > self.calls.len() {
                    return Err(EINVAL);
                }
                let (future, ret) = match self.calls[tag - 1].take() {
                    Some(CallSlot::Pending(future, ret)) => (future, ret),
                    Some(CallSlot::Abandoned) => {
                        // The late reply to a call that timed out; its slot
                        // is free again.
                        dev_warn!(
                            client.dev,
                            "Late reply for tag {} on endpoint {}",
                            tag,
                            self.index
                        );
                        return Ok(());
                    }
                    // A reply with no call in flight.
                    None => return Err(ENOENT),
                };
                let extra_data = ret.map(|mut ret| {
                    let len = cmp::min(data.len() - 4, ret.len());
                    ret[..len].copy_from_slice(&data[4..(len + 4)]);
                    ret.truncate(len);
                    ret
                });
                future.complete(CallResult {
                    retcode,
                    extra_data,
                });

                return Ok(());
            }
            return Err(EINVAL);
        }
        Err(EINVAL)
    }
    /// Writes one entry into the transmit ring and returns the doorbell
    /// message that announces it. Nothing is visible to the firmware until
    /// that message is sent.
    fn write_entry(
        &mut self,
        client: &AopData,
        channel: u32,
        ty: u32,
        header: &[u8],
        data: &[u8],
    ) -> Result<u64> {
        let (buf_offset, block_size, buf_size) = match self.txbuf.as_ref() {
            Some(b) => (b.offset, b.block_size, b.buf_size),
            None => {
                dev_err!(
                    client.dev,
                    "Attempting to send message with no txbuf at endpoint {}",
                    self.index
                );
                return Err(EIO);
            }
        };
        let base = buf_offset + block_size * 3;
        mem_sync();
        let rptr = self.iomem_read32(buf_offset + block_size)? as usize;
        let mut wptr = self.iomem_read32(buf_offset + block_size * 2)? as usize;
        const QEH_SIZE: usize = mem::size_of::<QEHeader>();
        if wptr < rptr && wptr + QEH_SIZE >= rptr {
            dev_err!(client.dev, "Tx buffer full at endpoint {}", self.index);
            return Err(EIO);
        }
        let payload_len = header.len() + data.len();
        let qeh = QEHeader {
            magic: QE_MAGIC1,
            size: payload_len as u32,
            channel,
            ty,
        };
        let qeh_bytes = unsafe {
            slice::from_raw_parts(
                &qeh as *const QEHeader as *const u8,
                mem::size_of::<QEHeader>(),
            )
        };
        self.memcpy_to_iomem(base + wptr, qeh_bytes)?;
        if payload_len > buf_size.checked_sub(wptr + QEH_SIZE).ok_or(EIO)? {
            wptr = 0;
            self.memcpy_to_iomem(base + wptr, qeh_bytes)?;
        }
        self.memcpy_to_iomem(base + wptr + QEH_SIZE, header)?;
        self.memcpy_to_iomem(base + wptr + QEH_SIZE + header.len(), data)?;
        wptr = align_up(wptr + QEH_SIZE + payload_len, block_size) % buf_size;
        self.iomem_write32(buf_offset + block_size * 2, wptr as u32)?;
        Ok(wptr as u64 | (AFK_OPC_SEND << 48))
    }
    fn epic_notify(
        &mut self,
        client: &AopData,
        rtkit: Pin<&mut rtkit::RtKit<AopData>>,
        channel: u32,
        subtype: u16,
        data: &[u8],
        ret: Option<KVec<u8>>,
    ) -> Result<Arc<FutureValue<CallResult>>> {
        let Some(slot) = self.calls.iter().position(|c| c.is_none()) else {
            dev_err!(
                client.dev,
                "Too many inflight calls on endpoint {}",
                self.index
            );
            return Err(EIO);
        };
        let call = Arc::pin_init(FutureValue::pin_init(), GFP_KERNEL)?;
        let hdr = EPICHeader {
            version: 2,
            seq: self.seq,
            length: data.len() as u32,
            sub_version: 2,
            category: EPIC_CATEGORY_NOTIFY,
            subtype,
            tag: (slot + 1) as u16,
            ..EPICHeader::default()
        };
        // SAFETY: `hdr` is a packed plain-data struct that outlives the slice,
        // which covers exactly its bytes.
        let hdr_bytes = unsafe {
            slice::from_raw_parts(
                &hdr as *const EPICHeader as *const u8,
                mem::size_of::<EPICHeader>(),
            )
        };
        let doorbell = self.write_entry(client, channel, EPIC_TYPE_NOTIFY, hdr_bytes, data)?;
        self.seq = self.seq.wrapping_add(1);
        self.calls[slot] = Some(CallSlot::Pending(call.clone(), ret));
        if let Err(e) = rtkit.send_message(self.index, doorbell) {
            // The entry is in the ring and the next doorbell delivers it, so
            // its reply still arrives; keep the tag reserved until it does.
            self.calls[slot] = Some(CallSlot::Abandoned);
            return Err(e);
        }
        Ok(call)
    }
    /// Gives up on `call`: its slot stays reserved until the reply arrives.
    fn abandon_call(&mut self, call: &Arc<FutureValue<CallResult>>) {
        for slot in self.calls.iter_mut() {
            if let Some(CallSlot::Pending(pending, _)) = slot {
                if Arc::ptr_eq(pending, call) {
                    *slot = Some(CallSlot::Abandoned);
                    return;
                }
            }
        }
    }
}

struct ListenerEntry {
    svc: EPICService,
    listener: Arc<dyn FakehidListener>,
}

/// A service platform device registered by this driver. It is unregistered
/// explicitly by [`AopData::remove`]; nothing else touches the pointer.
struct ChildDevice(NonNull<bindings::platform_device>);

// SAFETY: The only operation on the pointer is `platform_device_unregister()`,
// which may be called from any thread.
unsafe impl Send for ChildDevice {}

impl ChildDevice {
    /// Unbinds the child's driver while the child stays registered.
    fn release_driver(&self) {
        // SAFETY: The pointer came from a successful
        // `platform_device_register_full()` and the device is still
        // registered, so its embedded `struct device` is valid.
        unsafe { bindings::device_release_driver(ptr::addr_of_mut!((*self.0.as_ptr()).dev)) };
    }

    fn unregister(self) {
        // SAFETY: The pointer came from a successful
        // `platform_device_register_full()` and is unregistered exactly once,
        // here, because this consumes `self`.
        unsafe { bindings::platform_device_unregister(self.0.as_ptr()) };
    }
}

#[pin_data]
struct AopData {
    dev: ARef<device::Device>,
    /// Runs the service registrations one at a time; drained on removal.
    registration_queue: OwnedQueue,
    /// Serializes queueing a registration with the start of removal, so that
    /// nothing is queued once the queue drains. Never held across a drain.
    #[pin]
    registration_gate: Mutex<()>,
    /// Set once by the first teardown; later teardowns return at once.
    removing: Atomic<bool>,
    /// Set when the transport starts closing; no call is started after it.
    transport_closing: Atomic<bool>,
    #[pin]
    rtkit: Mutex<Option<rtkit::RtKit<AopData>>>,
    #[pin]
    endpoints: [Mutex<AFKEndpoint>; AFK_ENDPOINT_COUNT as usize],
    #[pin]
    ep_shutdown: [FutureValue<()>; AFK_ENDPOINT_COUNT as usize],
    #[pin]
    hid_listeners: Mutex<KVec<ListenerEntry>>,
    #[pin]
    subdevices: Mutex<KVec<ChildDevice>>,
}

#[pin_data]
struct AopServiceRegisterWork {
    name: &'static CStr,
    data: Arc<AopData>,
    service: EPICService,
    #[pin]
    work: Work<AopServiceRegisterWork>,
}

impl_has_work! {
    impl HasWork<Self, 0> for AopServiceRegisterWork { self.work }
}

impl AopServiceRegisterWork {
    fn new(
        name: &'static CStr,
        data: Arc<AopData>,
        service: EPICService,
    ) -> Result<Pin<KBox<Self>>> {
        KBox::pin_init(
            pin_init!(AopServiceRegisterWork {
                name, data, service,
                work <- new_work!("AopServiceRegisterWork::work"),
            }),
            GFP_KERNEL,
        )
    }
}

impl WorkItem for AopServiceRegisterWork {
    type Pointer = Pin<KBox<AopServiceRegisterWork>>;

    fn run(this: Pin<KBox<AopServiceRegisterWork>>) {
        // Held until the device has been registered: the registration takes
        // its own reference on the node only then.
        let fwnode = this
            .data
            .dev
            .fwnode()
            .and_then(|x| x.get_child_by_name(this.name));
        let info = bindings::platform_device_info {
            parent: this.data.dev.as_raw(),
            name: this.name.as_ptr() as *const _,
            id: bindings::PLATFORM_DEVID_AUTO,
            res: ptr::null_mut(),
            num_res: 0,
            data: &this.service as *const EPICService as *const _,
            size_data: mem::size_of::<EPICService>(),
            dma_mask: 0,
            fwnode: fwnode
                .as_ref()
                .map(|x| x.as_raw())
                .unwrap_or(ptr::null_mut()),
            swnode: ptr::null_mut(),
            properties: ptr::null_mut(),
            of_node_reused: false,
        };
        // The slot is reserved before the device exists, so that a device
        // that was registered is always tracked and gets unregistered. The
        // child's probe runs inside the registration and calls back into the
        // transport, but nothing on that path takes this lock.
        let mut subdevices = this.data.subdevices.lock();
        if subdevices.reserve(1, GFP_KERNEL).is_err() {
            dev_err!(
                this.data.dev,
                "Failed to allocate the device slot for service {:?}",
                this.name
            );
            return;
        }
        // SAFETY: `info` is a valid, fully initialized `platform_device_info`
        // whose pointers outlive the call.
        let pdev = unsafe { from_err_ptr(bindings::platform_device_register_full(&info)) };
        drop(fwnode);
        match pdev.and_then(|pdev| NonNull::new(pdev).ok_or(EINVAL)) {
            Err(e) => {
                dev_err!(
                    this.data.dev,
                    "Failed to create device for service {:?}: {:?}",
                    this.name,
                    e
                );
            }
            Ok(pdev) => {
                if let Err(child) = subdevices.push_within_capacity(ChildDevice(pdev)) {
                    // Cannot happen after the reservation above; never leave
                    // a registered device untracked.
                    child.0.unregister();
                }
            }
        }
    }
}

impl AopData {
    fn new(dev: &platform::Device<Core>) -> Result<Arc<AopData>> {
        let registration_queue = OwnedQueue::new_ordered(c_str!("apple-aop"))?;
        Arc::pin_init(
            pin_init!(
                AopData {
                    dev: dev.as_ref().into(),
                    registration_queue,
                    registration_gate <- new_mutex!(()),
                    removing: Atomic::new(false),
                    transport_closing: Atomic::new(false),
                    rtkit <- new_mutex!(None),
                    endpoints <- pin_init::pin_init_array_from_fn(|i| {
                        new_mutex!(AFKEndpoint::new(AFK_ENDPOINT_START + i as u8))
                    }),
                    ep_shutdown <- pin_init::pin_init_array_from_fn(|_| FutureValue::pin_init()),
                    hid_listeners <- new_mutex!(KVec::new()),
                    subdevices <- new_mutex!(KVec::new()),
                }
            ),
            GFP_KERNEL,
        )
    }
    fn start(&self) -> Result<()> {
        {
            let mut guard = self.rtkit.lock();
            let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            rtk.as_mut().wake()?;
        }
        for ep in 0..AFK_ENDPOINT_COUNT as usize {
            let rtk_ep_num = AFK_ENDPOINT_START + ep as u8;
            let mut guard = self.rtkit.lock();
            let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            if !rtk.as_mut().has_endpoint(rtk_ep_num) {
                continue;
            }
            rtk.as_mut().start_endpoint(rtk_ep_num)?;
            let mut ep_guard = self.endpoints[ep].lock();
            ep_guard.start(rtk.as_mut())?;
            ep_guard.started = true;
        }
        Ok(())
    }
    fn register_service(
        self: Arc<Self>,
        ep: &mut AFKEndpoint,
        channel: u32,
        name: &[u8],
    ) -> Result<()> {
        let svc = EPICService {
            channel,
            endpoint: ep.index,
        };
        let dev_name = match name {
            b"aop-audio" => c_str!("audio"),
            b"las" => c_str!("las"),
            b"als" => c_str!("als"),
            _ => {
                return Ok(());
            }
        };
        // The child's probe calls back into the transport, so it runs from a
        // work item with the endpoint lock dropped. The gate orders queueing
        // it against removal, which drains the queue after setting the flag.
        let gate = self.registration_gate.lock();
        if self.removing.load(Acquire) {
            return Ok(());
        }
        let work = AopServiceRegisterWork::new(dev_name, self.clone(), svc)?;
        self.registration_queue.enqueue(work);
        drop(gate);
        Ok(())
    }

    fn process_fakehid_report(&self, ep: &AFKEndpoint, ch: u32, data: &[u8]) -> Result<()> {
        let guard = self.hid_listeners.lock();
        for entry in &*guard {
            if entry.svc.endpoint == ep.index && entry.svc.channel == ch {
                return entry.listener.process_fakehid_report(data);
            }
        }
        Ok(())
    }

    fn shutdown_complete(&self, endpoint: u8) {
        if let Some(index) = afk_endpoint_index(endpoint) {
            self.ep_shutdown[index].complete(());
        }
    }

    /// Shuts down every started endpoint, waiting a bounded time for each
    /// acknowledgment. Returns the first error but still tries the rest.
    fn stop(&self) -> Result<()> {
        let mut ret = Ok(());
        for ep in 0..AFK_ENDPOINT_COUNT as usize {
            {
                let mut guard = self.rtkit.lock();
                let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
                let mut ep_guard = self.endpoints[ep].lock();
                if !ep_guard.started {
                    continue;
                }
                // Whatever happens below, the endpoint is not started again.
                ep_guard.started = false;
                self.ep_shutdown[ep].reset();
                if let Err(e) = ep_guard.stop(rtk.as_mut()) {
                    ret = ret.and(Err(e));
                    continue;
                }
            }
            if self.ep_shutdown[ep]
                .wait_timeout(AFK_SHUTDOWN_TIMEOUT_MS)
                .is_none()
            {
                dev_warn!(
                    self.dev,
                    "Endpoint {:#04x} did not acknowledge its shutdown",
                    AFK_ENDPOINT_START + ep as u8
                );
                ret = ret.and(Err(ETIMEDOUT));
            }
        }
        ret
    }

    fn patch_bootargs(
        &self,
        aop_mmio: &IoMem<AOP_MMIO_SIZE>,
        patches: &[(u32, u64)],
    ) -> Result<()> {
        let aop_mmio = aop_mmio.relaxed();
        let offset = aop_mmio.read32(BOOTARGS_OFFSET) as usize;
        let size = aop_mmio.read32(BOOTARGS_SIZE) as usize;
        let mut arg_bytes = KVec::<u8>::from_elem(0, size, GFP_KERNEL)?;
        aop_mmio.try_memcpy_fromio(&mut arg_bytes, offset)?;
        let mut idx = 0;
        while idx < size {
            let key = u32::from_le_bytes(arg_bytes[idx..idx + 4].try_into().unwrap());
            let size = u32::from_le_bytes(arg_bytes[idx + 4..idx + 8].try_into().unwrap()) as usize;
            idx += 8;
            for (k, v) in patches.iter() {
                if *k != key {
                    continue;
                }
                arg_bytes[idx..idx + size].copy_from_slice(&(*v as u64).to_le_bytes()[..size]);
                break;
            }
            idx += size;
        }
        aop_mmio.try_memcpy_toio(offset, &arg_bytes)
    }

    fn start_cpu(&self, asc_mmio: &RelaxedMmio<ASC_MMIO_SIZE>) -> Result<()> {
        let val = asc_mmio.read32(CPU_CONTROL);
        asc_mmio.write32(val | CPU_RUN, CPU_CONTROL);
        Ok(())
    }
}

impl AopData {
    /// Waits a bounded time for the reply to `call` on endpoint `ep_idx`.
    fn wait_call(
        &self,
        ep_idx: usize,
        svc: &EPICService,
        subtype: u16,
        call: Arc<FutureValue<CallResult>>,
    ) -> Result<CallResult> {
        if let Some(res) = call.wait_timeout(EPIC_CALL_TIMEOUT_MS) {
            return Ok(res);
        }
        self.endpoints[ep_idx].lock().abandon_call(&call);
        dev_err!(
            self.dev,
            "EPIC call {:#x} on channel {} timed out after {} ms",
            subtype,
            svc.channel,
            EPIC_CALL_TIMEOUT_MS
        );
        Err(ETIMEDOUT)
    }
}

impl AOP for AopData {
    fn epic_call(&self, svc: &EPICService, subtype: u16, msg_bytes: &[u8]) -> Result<u32> {
        if self.transport_closing.load(Acquire) {
            return Err(ENODEV);
        }
        let ep_idx = afk_endpoint_index(svc.endpoint).ok_or(EINVAL)?;
        let call = {
            let mut rtk_guard = self.rtkit.lock();
            let mut rtk = rtk_guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            let mut ep_guard = self.endpoints[ep_idx].lock();
            ep_guard.epic_notify(self, rtk.as_mut(), svc.channel, subtype, msg_bytes, None)?
        };
        Ok(self.wait_call(ep_idx, svc, subtype, call)?.retcode)
    }
    fn epic_call_ret(
        &self,
        svc: &EPICService,
        subtype: u16,
        msg_bytes: &[u8],
        ret_len: usize,
    ) -> Result<(u32, KVec<u8>)> {
        if self.transport_closing.load(Acquire) {
            return Err(ENODEV);
        }
        let ep_idx = afk_endpoint_index(svc.endpoint).ok_or(EINVAL)?;
        let call = {
            let mut rtk_guard = self.rtkit.lock();
            let mut rtk = rtk_guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            let mut ep_guard = self.endpoints[ep_idx].lock();
            let mut ret_buf = KVec::new();
            ret_buf.resize(ret_len, 0, GFP_KERNEL)?;
            ep_guard.epic_notify(
                self,
                rtk.as_mut(),
                svc.channel,
                subtype,
                msg_bytes,
                Some(ret_buf),
            )?
        };
        let res = self.wait_call(ep_idx, svc, subtype, call)?;
        Ok((res.retcode, res.extra_data.ok_or(EIO)?))
    }
    fn add_fakehid_listener(
        &self,
        svc: EPICService,
        listener: Arc<dyn FakehidListener>,
    ) -> Result<()> {
        let mut guard = self.hid_listeners.lock();
        if self.removing.load(Acquire) {
            return Err(ENODEV);
        }
        if guard.iter().any(|entry| entry.svc == svc) {
            return Err(EBUSY);
        }
        Ok(guard.push(ListenerEntry { svc, listener }, GFP_KERNEL)?)
    }
    fn remove_fakehid_listener(&self, svc: &EPICService) -> bool {
        let mut guard = self.hid_listeners.lock();
        for i in 0..guard.len() {
            if guard[i].svc == *svc {
                guard.swap_remove(i);
                return true;
            }
        }
        false
    }
    /// Takes the AOP down: from unbind, from a failed probe, or as a fallback
    /// from Drop. Only the first call does anything.
    fn remove(&self) {
        {
            let _gate = self.registration_gate.lock();
            if self.removing.xchg(true, Acquire) {
                return;
            }
        }
        // No registration is queued after this point, so once the queue is
        // empty the list of children is complete.
        self.registration_queue.drain();
        // Unbind the children while the transport still works, so that their
        // unbind can talk to their services. Then drop any listener a child
        // left behind: no report may reach a driver that is going away.
        let children = mem::take(&mut *self.subdevices.lock());
        for child in &children {
            child.release_driver();
        }
        self.hid_listeners.lock().clear();
        self.transport_closing.store(true, Release);
        if let Err(e) = self.stop() {
            dev_err!(self.dev, "Failed to stop AOP {:?}", e);
        }
        // Take the handle out of the shared state before dropping it: the
        // drop waits for the RTKit receive worker, which takes the same lock.
        // After it, no callback runs and the device may be unbound.
        let rtkit = self.rtkit.lock().take();
        drop(rtkit);
        for child in children {
            child.unregister();
        }
    }
}

struct NoBuffer;
impl rtkit::Buffer for NoBuffer {
    fn iova(&self) -> Result<usize> {
        unreachable!()
    }
    fn buf(&mut self) -> Result<IoSysMapRef<'_, u8>> {
        unreachable!()
    }
}

#[vtable]
impl rtkit::Operations for AopData {
    type Data = Arc<AopData>;
    type Buffer = NoBuffer;

    fn recv_message(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, ep: u8, msg: u64) {
        let Some(index) = afk_endpoint_index(ep) else {
            dev_warn!(
                data.dev,
                "Message {:#x} on unexpected endpoint {:#04x}",
                msg,
                ep
            );
            return;
        };
        let mut guard = data.rtkit.lock();
        let Some(mut rtk) = guard.as_mut().as_pin_mut() else {
            return;
        };
        let mut ep_guard = data.endpoints[index].lock();
        let ret = ep_guard.recv_message(data, rtk.as_mut(), msg);
        if let Err(e) = ret {
            dev_err!(data.dev, "Failed to handle rtkit message, error: {:?}", e);
        }
    }

    fn crashed(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, _crashlog: Option<&[u8]>) {
        dev_err!(data.dev, "AOP firmware crashed");
    }
}

/// The driver data of the AOP. Children reach the `Arc` through
/// `AOP::from_child()`, which relies on this being `repr(transparent)` over
/// it.
#[repr(transparent)]
struct AopDriver(Arc<dyn AOP>);

struct AopHwConfig {
    ec0p: u64,
    alig: u64,
    aopt: u64,
}

const HW_CFG_T8103: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 1,
    alig: 128,
};
const HW_CFG_T8112: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 0,
    alig: 128,
};
const HW_CFG_T6000: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 0,
    alig: 64,
};
const HW_CFG_T6020: AopHwConfig = AopHwConfig {
    ec0p: 0x0100_00000000,
    aopt: 0,
    alig: 64,
};

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <AopDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c_str!("apple,t8103-aop")), &HW_CFG_T8103),
        (of::DeviceId::new(c_str!("apple,t8112-aop")), &HW_CFG_T8112),
        (of::DeviceId::new(c_str!("apple,t6000-aop")), &HW_CFG_T6000),
        (of::DeviceId::new(c_str!("apple,t6020-aop")), &HW_CFG_T6020),
    ]
);

impl platform::Driver for AopDriver {
    type IdInfo = &'static AopHwConfig;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let cfg = info.ok_or(ENODEV)?;
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::new::<42>())? };
        let aop_req = pdev.io_request_by_index(0).ok_or(EINVAL)?;
        let aop_mmio = KBox::pin_init(aop_req.iomap_sized::<AOP_MMIO_SIZE>(), GFP_KERNEL)?;
        let asc_req = pdev.io_request_by_index(1).ok_or(EINVAL)?;
        let asc_mmio = KBox::pin_init(asc_req.iomap_sized::<ASC_MMIO_SIZE>(), GFP_KERNEL)?;
        let data = AopData::new(pdev)?;
        // Whatever fails below leaves the AOP running and its children
        // registering; the same teardown as unbind's cleans that up.
        let probe_guard = ScopeGuard::new_with_data(data.clone(), |data| data.remove());
        let aop_mmio = aop_mmio.access(pdev.as_ref())?;
        data.patch_bootargs(
            aop_mmio,
            &[
                (from_fourcc(b"EC0p"), cfg.ec0p),
                (from_fourcc(b"nCal"), 0x0),
                (from_fourcc(b"alig"), cfg.alig),
                (from_fourcc(b"AOPt"), cfg.aopt),
            ],
        )?;
        let rtkit = rtkit::RtKit::<AopData>::new(pdev.as_ref(), None, 0, data.clone())?;
        *data.rtkit.lock() = Some(rtkit);
        let asc_mmio = asc_mmio.access(pdev.as_ref())?.relaxed();
        data.start_cpu(asc_mmio)?;
        data.start()?;
        probe_guard.dismiss();
        let data = data as Arc<dyn AOP>;
        Ok(Self(data))
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        // The device is still bound here, which the DMA allocations the
        // teardown frees require. Drop only repeats it as a no-op.
        this.0.remove();
    }
}

impl Drop for AopDriver {
    fn drop(&mut self) {
        self.0.remove();
    }
}

unsafe impl Send for AopDriver {}

module_platform_driver! {
    type: AopDriver,
    name: "apple_aop",
    description: "AOP driver",
    license: "Dual MIT/GPL",
}
