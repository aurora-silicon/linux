// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Apple AOP driver
//!
//! Copyright (C) The Asahi Linux Contributors

mod aop_als_calibration;

use core::{
    arch::asm,
    cell::UnsafeCell,
    cmp, mem, ptr, slice,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use kernel::{
    bindings,
    c_str,
    device,
    device::{Bound, Core},
    dma::{
        Coherent,
        Device,
        DmaMask, //
    },
    error::{from_err_ptr, to_result},
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
        ReportListener,
        SourceRing,
        AOP, //
    },
    soc::apple::mailbox,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, ArcBorrow, CondVar, CondVarTimeoutResult, Mutex, MutexGuard},
    time::msecs_to_jiffies,
    types::{ForeignOwnable, ScopeGuard},
    workqueue::{
        self,
        impl_has_work,
        new_work,
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
/// 0x20..=0x2e.  This deliberately matches macOS, which was measured under the
/// hypervisor on 2026-09-20 starting exactly 0x08 (oslog), 0x20, 0x21, 0x22,
/// 0x23 and 0x2b on this AOP -- the same five AFK endpoints started here.
///
/// It is tempting to widen this: the J700 setup port sets up 0x30 and 0x32 too
/// (see SETUP_ENDPOINTS), giving them ring buffers that nothing then listens
/// on, and they looked like a candidate for the ambient light sensor's missing
/// sample path.  The trace settles it -- macOS never starts them either -- so
/// do not go there again without new evidence.
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

// T8140 (J700) AOP "setup port": the second mailbox the firmware boots
// through.  Everything below was measured by the native m1n1 microphone host
// (m1n1-aurora proxyclient/m1n1/fw/aop/j700_lpmic_native.py, RESULTS in
// artifacts/j700-native-lpmic-20260906).
const SETUP_PAGE: usize = 0x4000;
const SETUP_ARENA_PAGES: usize = 13;
const SETUP_MGMT_EP: u8 = 0;
const SETUP_TYPE_HELLO: u64 = 1;
const SETUP_TYPE_HELLO_REPLY: u64 = 2;
const SETUP_TYPE_UNK3: u64 = 3;
const SETUP_TYPE_UNK3_REPLY: u64 = 4;
const SETUP_TYPE_PWR_ACK: u64 = 7;
const SETUP_TYPE_EPMAP: u64 = 8;
const SETUP_TYPE_AP_PWR: u64 = 0xb;
const SETUP_HELLO_VERSION: u64 = 0xc000c;
const SETUP_EPMAP_LAST: u64 = 1 << 51;
const SETUP_AP_PWR_INIT: u64 = 0x220;
const SETUP_AP_PWR_ON: u64 = 0x20;
const SETUP_BUFFER_REQUEST: u64 = 0x12;
const SETUP_BUFFER_REQUEST_ACK: u64 = 0x1200000000000000;
const SETUP_ENDPOINTS: [u8; 5] = [0x20, 0x21, 0x23, 0x30, 0x32];
const SETUP_BOOT_TIMEOUT_MS: i64 = 15000;
const SETUP_SOURCE_EP: u8 = 0x20;
/// The ALS lives on setup-port endpoint 0x21, and the trusted side will not
/// power the CT817 until its calibration has been pushed there.
const SETUP_ALS_EP: u8 = 0x21;
/// ALS calibration is supplied by the machine through the firmware loader.
/// The AOP answers the calibration with 0x2000000000000004 rather than the
/// 0x2000000000000001 the source bind gets.
const SETUP_REPLY_READY_ALT: u64 = 0x2000000000000004;
const SETUP_SET_SOURCE_BUFFER: u64 = 0x6b803ce492bd9547;
const SETUP_REQUEST_TYPE: u64 = 3 << 60;
const SETUP_REQUEST_DONE: u64 = 4 << 60;
const SETUP_REPLY_READY: u64 = 0x2000000000000001;
const SETUP_REPLY_TIMEOUT_MS: i64 = 5000;
/// Bound on an EPIC call: the firmware can stop answering (e.g. a power
/// request against a fabric the AP turned off), and the caller must not be
/// wedged in D state forever.
const EPIC_CALL_TIMEOUT_MS: i64 = 5000;
const AFK_SHUTDOWN_TIMEOUT_MS: i64 = 5000;
const SERVICE_PUBLICATION_TIMEOUT_MS: usize = 5000;
const SETUP_RX_CAPACITY: usize = 64;
const SETUP_RX_WORK_ID: u64 = 1;

fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
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

/// EPIC v4 (t8140): the sub-header carries a timestamp before the tag.
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct EPICHeaderV4 {
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
    sub_timestamp: u64,
    tag: u16,
    _unk1: u16,
    inline_len: u32,
}

/// The fields handle_ipc needs, decoded from either sub-header layout.
struct EPICDecoded {
    category: u8,
    subtype: u16,
    tag: u16,
}

fn epic_decode(hdr_bytes: &[u8]) -> EPICDecoded {
    let sub = &hdr_bytes[16..];
    let sub_version = sub[4];
    let category = sub[5];
    let subtype = u16::from_le_bytes([sub[6], sub[7]]);
    let tag = if sub_version >= 4 {
        u16::from_le_bytes([sub[16], sub[17]])
    } else {
        u16::from_le_bytes([sub[8], sub[9]])
    };
    EPICDecoded {
        category,
        subtype,
        tag,
    }
}

/// Service announcements carry a 32-byte NUL-terminated name followed by
/// the channel at byte 0x20. Bytes after the NUL are ignored.
#[repr(C, packed)]
struct EPICServiceAnnounce {
    name: [u8; 32],
    channel: u32,
    _unk2: u32,
    _unk3: u32,
}

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
    /// Waits at most `timeout_ms`; `None` when the value never arrived.
    ///
    /// The wait is uninterruptible: a firmware transaction must not be
    /// abandoned because the calling task has a signal pending (a daemon
    /// closing its PCM on SIGTERM), or its prompt reply would arrive for a
    /// call that no longer exists.
    fn wait_timeout(&self, timeout_ms: i64) -> Option<T> {
        let mut ret_guard = self.val.lock();
        let mut left = msecs_to_jiffies(timeout_ms as u32);
        while ret_guard.is_none() {
            match self.completion.wait_timeout(&mut ret_guard, left) {
                CondVarTimeoutResult::Timeout => return ret_guard.take(),
                CondVarTimeoutResult::Woken { jiffies }
                | CondVarTimeoutResult::Signal { jiffies } => {
                    left = jiffies;
                }
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

struct AFKEndpoint {
    index: u8,
    started: bool,
    iomem: Option<Coherent<[u8]>>,
    txbuf: Option<AFKRingBuffer>,
    rxbuf: Option<AFKRingBuffer>,
    seq: u16,
    calls: [Option<Arc<FutureValue<CallResult>>>; AOP_MAX_CALLS],
    call_returns: [Option<KVec<u8>>; AOP_MAX_CALLS],
    /// Untagged replies cannot be matched safely after a transaction times out.
    call_desynchronized: bool,
}

unsafe impl Send for AFKEndpoint {}

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
            call_returns: [const { None }; AOP_MAX_CALLS],
            call_desynchronized: false,
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

    fn parse_ring_buf(&self, msg: u64) -> Result<AFKRingBuffer> {
        let msg = msg as usize;
        let size = ((msg >> 16) & 0xFFFF) * AFK_RB_BLOCK_STEP;
        let offset = ((msg >> 32) & 0xFFFF) * AFK_RB_BLOCK_STEP;
        let buf_size = self.iomem_read32(offset)? as usize;
        let block_size = (size - buf_size) / 3;
        Ok(AFKRingBuffer {
            offset,
            block_size,
            buf_size,
        })
    }
    fn iomem_write32(&mut self, off: usize, data: u32) -> Result<()> {
        let size = core::mem::size_of::<u32>();
        let data = data.to_le_bytes();
        let iomem = &self.iomem.as_mut().ok_or(ENXIO)?;
        let buf = unsafe { &mut iomem.as_mut()[off..off + size] };
        buf.copy_from_slice(&data);
        Ok(())
    }

    fn iomem_read32(&self, off: usize) -> Result<u32> {
        let size = core::mem::size_of::<u32>();
        let iomem = &self.iomem.as_ref().ok_or(ENXIO)?;
        let buf = unsafe { &iomem.as_ref()[off..off + size] };
        Ok(u32::from_le_bytes(buf.try_into().unwrap()))
    }

    fn memcpy_from_iomem(&self, off: usize, target: &mut [u8]) -> Result<()> {
        let iomem = &self.iomem.as_ref().ok_or(ENXIO)?;
        // SAFETY:
        // as_slice() checks that off and target.len() are whithin iomem's limits.
        unsafe {
            let src = &iomem.as_ref()[off..off + target.len()];
            target.copy_from_slice(src);
        }
        Ok(())
    }

    fn memcpy_to_iomem(&mut self, off: usize, src: &[u8]) -> Result<()> {
        let iomem = &self.iomem.as_mut().ok_or(ENXIO)?;
        // SAFETY:
        // as_slice_mut() checks that off and src.len() are whithin iomem's limits.
        unsafe {
            let target = &mut iomem.as_mut()[off..off + src.len()];
            target.copy_from_slice(src);
        }
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
        // SAFETY: dev is the parent AOP device. Probe-failure unwind and
        // unbind stop and drain RTKit callbacks before their core callback
        // returns, so this callback's device remains bound for this borrow.
        // The ARef alone would not provide that binding guarantee.
        let bound_dev = unsafe { dev.as_bound() };
        let iomem = Coherent::<u8>::zeroed_slice(bound_dev, size, GFP_KERNEL)?;
        let iova = iomem.dma_handle();
        // Retain ownership before publishing an address, including send errors.
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
            if qeh.size as usize > (buf_size - rptr - QEH_SIZE) {
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
            if msg_buf.len() < mem::size_of::<EPICHeader>() {
                dev_err!(client.dev, "Short EPIC message on ep {}", self.index);
                return Err(EIO);
            }
            let (hdr_bytes, msg) = msg_buf.split_at(mem::size_of::<EPICHeader>());
            let header = epic_decode(hdr_bytes);
            self.handle_ipc(client, qeh, &header, msg)?;
            rptr = align_up(rptr + QEH_SIZE + qeh.size as usize, block_size) % buf_size;
            mem_sync();
            self.iomem_write32(buf_offset + block_size, rptr as u32)?;
            wptr = self.iomem_read32(buf_offset + block_size * 2)?;
            mem_sync();
        }
        Ok(())
    }
    fn handle_ipc(
        &mut self,
        client: ArcBorrow<'_, AopData>,
        qhdr: &QEHeader,
        ehdr: &EPICDecoded,
        data: &[u8],
    ) -> Result<()> {
        let subtype = ehdr.subtype;
        if ehdr.category == EPIC_CATEGORY_REPORT {
            if subtype == EPIC_SUBTYPE_STD_SERVICE {
                if data.len() < mem::size_of::<EPICServiceAnnounce>() {
                    return Err(EIO);
                }
                // SAFETY: the complete packed announcement is present.
                let announce = unsafe { &*(data.as_ptr() as *const EPICServiceAnnounce) };
                let chan = announce.channel;
                let name_len = announce
                    .name
                    .iter()
                    .position(|x| *x == 0)
                    .unwrap_or(announce.name.len());
                // Record announcement metadata only in debug builds; the
                // advertised channel remains the qualified command route.
                let arrival = qhdr.channel;
                let unk2 = announce._unk2;
                let unk3 = announce._unk3;
                dev_dbg!(
                    client.dev,
                    "aop: announce ep {:#04x} arrival-chan {:#x} body-chan {:#x} flags {:#x} iface {:#x} name {}\n",
                    self.index,
                    arrival,
                    chan,
                    unk2,
                    unk3,
                    core::str::from_utf8(&announce.name[..name_len]).unwrap_or("<non-utf8>")
                );
                return Into::<Arc<_>>::into(client).register_service(
                    self,
                    chan,
                    &announce.name[..name_len],
                );
            } else if subtype == EPIC_SUBTYPE_FAKEHID_REPORT {
                return client.process_fakehid_report(self, qhdr.channel, data);
            } else if client.process_report(self, qhdr.channel, subtype, data)? {
                return Ok(());
            } else {
                dev_err!(
                    client.dev,
                    "Unexpected EPIC report subtype {:x} on endpoint {}",
                    subtype,
                    self.index
                );
                return Err(EIO);
            }
        } else if ehdr.category == EPIC_CATEGORY_REPLY {
            if subtype == EPIC_SUBTYPE_RETCODE_PAYLOAD
                || subtype == EPIC_SUBTYPE_RETCODE
                || subtype == EPIC_SUBTYPE_STRING
            {
                if data.len() < mem::size_of::<u32>() {
                    dev_err!(
                        client.dev,
                        "Retcode data too short on endpoint {}",
                        self.index
                    );
                    return Err(EIO);
                }
                let retcode = u32::from_ne_bytes(data[..4].try_into().unwrap());
                let mut tag = ehdr.tag as usize;
                if client.epic_v4 && (tag == 0 || tag > self.calls.len()) {
                    // The t8140 firmware does not echo the request tag in its
                    // replies; with one call in flight per endpoint (the native
                    // host never had more) the reply is for that call.
                    let inflight: KVec<usize> = {
                        let mut v = KVec::new();
                        for i in 0..self.calls.len() {
                            if self.calls[i].is_some() {
                                v.push(i + 1, GFP_KERNEL)?;
                            }
                        }
                        v
                    };
                    if inflight.len() == 1 {
                        tag = inflight[0];
                    }
                }
                if tag == 0 || tag - 1 >= self.calls.len() || self.calls[tag - 1].is_none() {
                    // A reply for a call that timed out, or no call at all:
                    // drop it, the rest of the ring is still good.
                    dev_warn!(
                        client.dev,
                        "Dropping a retcode with no call in flight (tag {:?}) on endpoint {}",
                        tag,
                        self.index
                    );
                    return Ok(());
                }
                let future = self.calls[tag - 1].take().unwrap();
                let extra_data = if let Some(mut ret) = self.call_returns[tag - 1].take() {
                    let len = cmp::min(data.len() - 4, ret.len());
                    ret[..len].copy_from_slice(&data[4..(len + 4)]);
                    ret.truncate(len);
                    Some(ret)
                } else {
                    None
                };
                future.complete(CallResult {
                    retcode,
                    extra_data,
                });

                return Ok(());
            } else {
                dev_err!(
                    client.dev,
                    "Unexpected EPIC reply subtype {:x} on endpoint {}",
                    subtype,
                    self.index
                );
                return Err(EIO);
            }
        }
        dev_err!(
            client.dev,
            "Unexpected EPIC category {:x} on endpoint {}",
            ehdr.category,
            self.index
        );
        Err(EIO)
    }
    fn send_rb(
        &mut self,
        client: &AopData,
        rtkit: Pin<&mut rtkit::RtKit<AopData>>,
        channel: u32,
        ty: u32,
        header: &[u8],
        data: &[u8],
    ) -> Result<()> {
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
        if payload_len > buf_size - wptr - QEH_SIZE {
            wptr = 0;
            self.memcpy_to_iomem(base + wptr, qeh_bytes)?;
        }
        self.memcpy_to_iomem(base + wptr + QEH_SIZE, header)?;
        self.memcpy_to_iomem(base + wptr + QEH_SIZE + header.len(), data)?;
        wptr = align_up(wptr + QEH_SIZE + payload_len, block_size) % buf_size;
        self.iomem_write32(buf_offset + block_size * 2, wptr as u32)?;
        let msg = wptr as u64 | (AFK_OPC_SEND << 48);
        rtkit.send_message(self.index, msg)
    }
    fn epic_notify(
        &mut self,
        client: &AopData,
        rtkit: Pin<&mut rtkit::RtKit<AopData>>,
        channel: u32,
        subtype: u16,
        category: u8,
        data: &[u8],
        ret: Option<KVec<u8>>,
    ) -> Result<Arc<FutureValue<CallResult>>> {
        if self.call_desynchronized {
            return Err(EIO);
        }
        let mut tag = 0;
        for i in 0..self.calls.len() {
            if self.calls[i].is_none() {
                tag = i + 1;
                break;
            }
        }
        if tag == 0 {
            dev_err!(
                client.dev,
                "Too many inflight calls on endpoint {}",
                self.index
            );
            return Err(EIO);
        }
        let call = Arc::pin_init(FutureValue::pin_init(), GFP_KERNEL)?;
        // 2026-09-20: this field was declared and never set, so every call this
        // driver has ever made advertised a reply capacity of zero.  macOS sets
        // it on every call -- 0xe4 for the ALS HID-descriptor fetch (subtype
        // 0x01), and 2 / 4 / 0x40 matching each property's width for the
        // property reads.  Property gets happen to be answered anyway with a
        // zero here, which is why it went unnoticed; do not assume the same of
        // a call the firmware has never been asked by this driver before.
        let inline_len = ret.as_ref().map(|b| b.len() as u32).unwrap_or(0);
        let hdr_v2 = EPICHeader {
            version: 2,
            seq: self.seq,
            length: data.len() as u32,
            sub_version: 2,
            category,
            subtype,
            tag: tag as u16,
            inline_len,
            ..EPICHeader::default()
        };
        let hdr_v4 = EPICHeaderV4 {
            version: 2,
            seq: self.seq,
            length: data.len() as u32,
            sub_version: 4,
            category,
            subtype,
            tag: tag as u16,
            inline_len,
            ..EPICHeaderV4::default()
        };
        self.call_returns[tag - 1] = ret;
        // Keep the transmit header consistent with this firmware variant.
        let tx_v4 = client.epic_v4;
        let hdr_bytes = if tx_v4 {
            // SAFETY: `hdr_v4` is a packed, plain-data header that lives
            // for the whole call; the slice covers exactly its bytes.
            unsafe {
                slice::from_raw_parts(
                    &hdr_v4 as *const EPICHeaderV4 as *const u8,
                    mem::size_of::<EPICHeaderV4>(),
                )
            }
        } else {
            unsafe {
                slice::from_raw_parts(
                    &hdr_v2 as *const EPICHeader as *const u8,
                    mem::size_of::<EPICHeader>(),
                )
            }
        };

        if let Err(err) = self.send_rb(client, rtkit, channel, EPIC_TYPE_NOTIFY, hdr_bytes, data) {
            self.call_returns[tag - 1] = None;
            // The ring pointer may already be published when the doorbell
            // fails. An untagged late reply must not reach a later caller.
            if tx_v4 {
                self.call_desynchronized = true;
            }
            return Err(err);
        }
        self.seq = self.seq.wrapping_add(1);
        self.calls[tag - 1] = Some(call.clone());
        Ok(call)
    }
    /// Drops a call that will never be waited on again (timed out).
    fn cancel_call(&mut self, call: &Arc<FutureValue<CallResult>>) {
        for i in 0..self.calls.len() {
            if let Some(c) = &self.calls[i] {
                if Arc::ptr_eq(c, call) {
                    self.calls[i] = None;
                    self.call_returns[i] = None;
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

unsafe impl Send for ListenerEntry {}

struct ReportListenerEntry {
    svc: EPICService,
    subtype: u16,
    listener: Arc<dyn ReportListener>,
}

unsafe impl Send for ReportListenerEntry {}

/// One of the five setup-port endpoints: a host-to-AOP message page and an
/// AOP-to-host reply window, both in the setup arena (dart-aop stream 1).
#[derive(Clone, Copy, Default)]
struct SetupEndpoint {
    ep: u8,
    tx_iova: u64,
    rx_iova: u64,
}

struct SetupState {
    mbox: Option<mailbox::Mailbox<SetupPortCallback>>,
    arena: Option<Coherent<[u8]>>,
    next_page: usize,
    endpoints: [Option<SetupEndpoint>; SETUP_ENDPOINTS.len()],
    map_done: bool,
    ap_ready: bool,
    power_sent: bool,
    failed: bool,
    /// last non-boot message received on the source endpoint
    reply: Option<u64>,
}

impl SetupState {
    fn new() -> SetupState {
        SetupState {
            mbox: None,
            arena: None,
            next_page: 0,
            endpoints: [None; SETUP_ENDPOINTS.len()],
            map_done: false,
            ap_ready: false,
            power_sent: false,
            failed: false,
            reply: None,
        }
    }

    fn endpoint(&self, ep: u8) -> Option<SetupEndpoint> {
        self.endpoints
            .iter()
            .flatten()
            .find(|e| e.ep == ep)
            .copied()
    }

    fn arena_offset(&self, iova: u64) -> Option<usize> {
        let base = self.arena_iova();
        let end = base + (SETUP_ARENA_PAGES * SETUP_PAGE) as u64;
        if iova < base || iova >= end {
            return None;
        }
        Some((iova - base) as usize)
    }

    fn arena_iova(&self) -> u64 {
        self.arena
            .as_ref()
            .map(|a| a.dma_handle() as u64)
            .unwrap_or(0)
    }

    fn endpoint_count(&self) -> usize {
        self.endpoints.iter().filter(|e| e.is_some()).count()
    }

    fn ready(&self) -> bool {
        self.ap_ready && self.endpoint_count() == SETUP_ENDPOINTS.len()
    }

    fn send(&self, ep: u8, word: u64) -> Result<()> {
        let mbox = self.mbox.as_ref().ok_or(ENXIO)?;
        mbox.send(
            mailbox::Message {
                msg0: word,
                msg1: ep as u32,
            },
            true,
        )
    }

    /// (5 << 60) | (host_to_aop << 54) | (pages << 48) | (iova >> 4)
    fn shared_descriptor(iova: u64, pages: usize, host_to_aop: bool) -> u64 {
        (5u64 << 60) | ((host_to_aop as u64) << 54) | ((pages as u64) << 48) | (iova >> 4)
    }
}

unsafe impl Send for SetupState {}

/// Single producer (serialized by the C mailbox RX lock), one private worker.
struct SetupRxRing {
    messages: UnsafeCell<[mailbox::Message; SETUP_RX_CAPACITY]>,
    read: AtomicUsize,
    write: AtomicUsize,
}

impl SetupRxRing {
    fn new() -> Self {
        Self {
            messages: UnsafeCell::new([mailbox::Message { msg0: 0, msg1: 0 }; SETUP_RX_CAPACITY]),
            read: AtomicUsize::new(0),
            write: AtomicUsize::new(0),
        }
    }

    fn push(&self, message: mailbox::Message) -> bool {
        let write = self.write.load(Ordering::Relaxed);
        let next = (write + 1) % SETUP_RX_CAPACITY;
        if next == self.read.load(Ordering::Acquire) {
            return false;
        }
        // SAFETY: the mailbox serializes producers; this slot has been released
        // by the sole consumer and is not published until the release store.
        unsafe {
            self.messages
                .get()
                .cast::<mailbox::Message>()
                .add(write)
                .write(message)
        };
        self.write.store(next, Ordering::Release);
        true
    }

    fn pop(&self) -> Option<mailbox::Message> {
        let read = self.read.load(Ordering::Relaxed);
        if read == self.write.load(Ordering::Acquire) {
            return None;
        }
        // SAFETY: only the ordered setup worker consumes; acquire observes the
        // initialized slot, and release makes it available to the producer.
        let message = unsafe {
            self.messages
                .get()
                .cast::<mailbox::Message>()
                .add(read)
                .read()
        };
        self.read
            .store((read + 1) % SETUP_RX_CAPACITY, Ordering::Release);
        Some(message)
    }
}

// SAFETY: the mailbox lock serializes producers and the private ordered queue
// has a single consumer. Release/acquire transfers ownership of each POD slot.
unsafe impl Sync for SetupRxRing {}
unsafe impl Send for SetupRxRing {}

struct SetupPortCallback;

impl mailbox::MailCallback for SetupPortCallback {
    type Data = Arc<AopData>;

    fn recv_message(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, msg: mailbox::Message) {
        if data.setup_rx_closed.load(Ordering::Acquire)
            || data.setup_rx_failed.load(Ordering::Acquire)
        {
            return;
        }
        if !data.setup_rx_ring.push(msg) {
            data.setup_rx_failed.store(true, Ordering::Release);
        }
        // Arc borrowing/cloning and queue_work do not allocate or sleep. This
        // fixed-size ingress is the only setup work performed in hard IRQ.
        let owner: Arc<AopData> = data.into();
        // Err only returns this extra Arc when the work is already queued.
        // That queued worker drains the ring, so this notification coalesces.
        let _ = owner
            .setup_queue
            .queue()
            .enqueue::<Arc<AopData>, SETUP_RX_WORK_ID>(owner.clone());
    }
}

/// Private queue: removal closes submissions before draining only this queue.
struct AopWorkqueue(*mut bindings::workqueue_struct);

impl AopWorkqueue {
    fn new(name: &'static CStr) -> Result<Self> {
        // SAFETY: the format and the NUL-terminated name pointer are valid.
        let raw = unsafe {
            bindings::alloc_workqueue_noprof(
                c_str!("%s").as_ptr().cast(),
                bindings::wq_flags_WQ_UNBOUND
                    | bindings::wq_flags___WQ_ORDERED
                    | bindings::wq_flags_WQ_MEM_RECLAIM,
                1,
                name.as_ptr().cast::<kernel::ffi::c_char>(),
            )
        };
        if raw.is_null() {
            return Err(ENOMEM);
        }
        Ok(Self(raw))
    }

    fn queue(&self) -> &workqueue::Queue {
        // SAFETY: this owner keeps the queue alive until every work item is drained.
        unsafe { workqueue::Queue::from_raw(self.0) }
    }

    fn drain(&self) {
        // SAFETY: the caller has closed submissions and holds no callback locks.
        unsafe { bindings::drain_workqueue(self.0) };
    }
}

impl Drop for AopWorkqueue {
    fn drop(&mut self) {
        // SAFETY: this is the sole queue owner; normal removal drained it first.
        unsafe { bindings::destroy_workqueue(self.0) };
    }
}

// SAFETY: the kernel synchronizes queue operations. Every queued item owns
// AopData, and core teardown closes ingress and drains before owner Drop.
unsafe impl Send for AopWorkqueue {}
unsafe impl Sync for AopWorkqueue {}

struct SourceRingBinding {
    ring: Arc<SourceRing>,
    confirmed: bool,
}

#[pin_data]
struct AopData {
    dev: ARef<device::Device>,
    epic_v4: bool,
    registration_queue: AopWorkqueue,
    setup_queue: AopWorkqueue,
    setup_rx_ring: SetupRxRing,
    setup_rx_closed: AtomicBool,
    setup_rx_failed: AtomicBool,
    #[pin]
    setup_rx_work: Work<AopData, SETUP_RX_WORK_ID>,
    #[pin]
    registration_gate: Mutex<()>,
    removing: AtomicBool,
    transport_closing: AtomicBool,
    cpu_started: AtomicBool,
    #[pin]
    setup: Mutex<SetupState>,
    #[pin]
    setup_cv: CondVar,
    #[pin]
    rtkit: Mutex<Option<rtkit::RtKit<AopData>>>,
    #[pin]
    endpoints: [Mutex<AFKEndpoint>; AFK_ENDPOINT_COUNT as usize],
    /// The EPIC v4 firmware answers without the request tag, so an endpoint
    /// carries one call at a time: callers queue here for their turn.
    #[pin]
    call_turn: [Mutex<()>; AFK_ENDPOINT_COUNT as usize],
    #[pin]
    ep_shutdown: [FutureValue<()>; AFK_ENDPOINT_COUNT as usize],
    #[pin]
    hid_listeners: Mutex<KVec<ListenerEntry>>,
    #[pin]
    report_listeners: Mutex<KVec<ReportListenerEntry>>,
    #[pin]
    subdevices: Mutex<KVec<*mut bindings::platform_device>>,
    #[pin]
    source_ring: Mutex<Option<SourceRingBinding>>,
}

unsafe impl Send for AopData {}
unsafe impl Sync for AopData {}

impl_has_work! {
    impl HasWork<Self, SETUP_RX_WORK_ID> for AopData { self.setup_rx_work }
}

impl WorkItem<SETUP_RX_WORK_ID> for AopData {
    type Pointer = Arc<AopData>;

    fn run(this: Arc<Self>) {
        if this.setup_rx_closed.load(Ordering::Acquire) {
            return;
        }
        if this.setup_rx_failed.load(Ordering::Acquire) {
            this.setup.lock().failed = true;
            this.setup_cv.notify_all();
            return;
        }
        while let Some(msg) = this.setup_rx_ring.pop() {
            if this.setup_rx_closed.load(Ordering::Acquire) {
                return;
            }
            let mut state = this.setup.lock();
            if this.setup_rx_failed.load(Ordering::Acquire) {
                state.failed = true;
                drop(state);
                this.setup_cv.notify_all();
                return;
            }
            if state.mbox.is_none() {
                continue;
            }
            let ep = (msg.msg1 & 0xff) as u8;
            if let Err(error) = this.setup_handle(&mut state, ep, msg.msg0) {
                state.failed = true;
                this.setup_rx_failed.store(true, Ordering::Release);
                dev_err!(
                    this.dev,
                    "setup port protocol error on endpoint {:#x}: {:?}\n",
                    ep,
                    error
                );
                drop(state);
                this.setup_cv.notify_all();
                return;
            }
            drop(state);
            this.setup_cv.notify_all();
        }
    }
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
        // Platform publishes driver data after probe returns. A child must not
        // dereference it before then. Failed probe closes this wait promptly.
        let mut parent_ready = false;
        for _ in 0..SERVICE_PUBLICATION_TIMEOUT_MS {
            if this.data.removing.load(Ordering::Acquire) {
                return;
            }
            // SAFETY: the helper takes the parent's device lock only if it
            // is immediately available, pairing with core probe publication.
            if unsafe { bindings::device_probe_data_ready(this.data.dev.as_raw()) } {
                parent_ready = true;
                break;
            }
            // SAFETY: private workqueue callbacks run in sleepable context.
            unsafe { bindings::msleep(1) };
        }
        if !parent_ready {
            dev_err!(this.data.dev, "AOP parent publication timed out\n");
            return;
        }
        // The ordered queue is the sole producer. Teardown drains it before
        // consuming this list, so the reserved slot cannot be stolen.
        if this.data.subdevices.lock().reserve(1, GFP_KERNEL).is_err() {
            dev_err!(this.data.dev, "Failed to reserve subdevice ownership\n");
            return;
        }
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
        let pdev = unsafe { from_err_ptr(bindings::platform_device_register_full(&info)) };
        match pdev {
            Err(e) => {
                dev_err!(
                    this.data.dev,
                    "Failed to create device for service {:?}: {:?}",
                    this.name,
                    e
                );
            }
            Ok(pdev) => {
                this.data
                    .subdevices
                    .lock()
                    .push_within_capacity(pdev)
                    .expect("ordered registration reserved a subdevice slot");
            }
        }
    }
}

impl AopData {
    fn new(dev: &platform::Device<Core>, epic_v4: bool) -> Result<Arc<AopData>> {
        let registration_queue = AopWorkqueue::new(c_str!("apple-aop-services"))?;
        let setup_queue = AopWorkqueue::new(c_str!("apple-aop-setup"))?;
        Arc::pin_init(
            pin_init!(
                AopData {
                    dev: dev.as_ref().into(),
                    epic_v4,
                    registration_queue,
                    setup_queue,
                    setup_rx_ring: SetupRxRing::new(),
                    setup_rx_closed: AtomicBool::new(false),
                    setup_rx_failed: AtomicBool::new(false),
                    setup_rx_work <- new_work!("AopData::setup_rx_work"),
                    registration_gate <- new_mutex!(()),
                    removing: AtomicBool::new(false),
                    transport_closing: AtomicBool::new(false),
                    cpu_started: AtomicBool::new(false),
                    setup <- new_mutex!(SetupState::new()),
                    setup_cv <- new_condvar!(),
                    rtkit <- new_mutex!(None),
                    endpoints <- pin_init::pin_init_array_from_fn(|i| {
                        new_mutex!(AFKEndpoint::new(AFK_ENDPOINT_START + i as u8))
                    }),
                    call_turn <- pin_init::pin_init_array_from_fn(|_| new_mutex!(())),
                    ep_shutdown <- pin_init::pin_init_array_from_fn(|_| FutureValue::pin_init()),
                    hid_listeners <- new_mutex!(KVec::new()),
                    report_listeners <- new_mutex!(KVec::new()),
                    subdevices <- new_mutex!(KVec::new()),
                    source_ring <- new_mutex!(None),
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
        self.start_afk()
    }
    fn start_afk(&self) -> Result<()> {
        for ep in 0..AFK_ENDPOINT_COUNT {
            let rtk_ep_num = AFK_ENDPOINT_START + ep;
            let mut guard = self.rtkit.lock();
            let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            if !rtk.as_mut().has_endpoint(rtk_ep_num) {
                continue;
            }
            // The J700 application map is 0x20 misc, 0x21 aop-audio,
            // 0x22 aop-voicetrigger, 0x23 als, 0x2b aop-audprov.  The firmware
            // blocks its own startup until every advertised endpoint is
            // enabled, so record which ones are actually present here.
            dev_dbg!(self.dev, "AFK: starting endpoint {:#04x}\n", rtk_ep_num);
            // These application endpoints are required on the qualified J700 map.
            let required = matches!(rtk_ep_num, 0x20 | 0x21 | 0x22 | 0x23 | 0x2b);
            if let Err(e) = rtk.as_mut().start_endpoint(rtk_ep_num) {
                if required {
                    return Err(e);
                }
                dev_warn!(
                    self.dev,
                    "AFK: endpoint {:#04x} did not start ({:?}), skipping\n",
                    rtk_ep_num,
                    e
                );
                continue;
            }
            let mut ep_guard = self.endpoints[ep as usize].lock();
            if let Err(e) = ep_guard.start(rtk.as_mut()) {
                if required {
                    return Err(e);
                }
                dev_warn!(
                    self.dev,
                    "AFK: endpoint {:#04x} AFK init failed ({:?}), skipping\n",
                    rtk_ep_num,
                    e
                );
            } else {
                ep_guard.started = true;
            }
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
        // Unknown services are logged when Rust debug assertions are enabled.
        dev_dbg!(
            self.dev,
            "aop: service announce ep {:#04x} channel {:#x} name {}\n",
            ep.index,
            channel,
            core::str::from_utf8(name).unwrap_or("<non-utf8>")
        );
        let dev_name = match name {
            b"aop-audio" => c_str!("audio"),
            b"las" => c_str!("las"),
            b"als" => c_str!("als"),
            _ => {
                return Ok(());
            }
        };
        // Serialize submission with removal, but never hold this lock while
        // draining: a child probe can call back into the transport.
        let gate = self.registration_gate.lock();
        if self.removing.load(Ordering::Acquire) {
            return Ok(());
        }
        let work = AopServiceRegisterWork::new(dev_name, self.clone(), svc)?;
        self.registration_queue.queue().enqueue(work);
        drop(gate);
        Ok(())
    }

    /// Returns Ok(true) when a registered listener took the report.
    fn process_report(&self, ep: &AFKEndpoint, ch: u32, subtype: u16, data: &[u8]) -> Result<bool> {
        if self.removing.load(Ordering::Acquire) {
            return Ok(true);
        }
        let guard = self.report_listeners.lock();
        for entry in &*guard {
            if entry.svc.endpoint == ep.index && entry.svc.channel == ch && entry.subtype == subtype
            {
                entry.listener.process_report(subtype, data)?;
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn process_fakehid_report(&self, ep: &AFKEndpoint, ch: u32, data: &[u8]) -> Result<()> {
        if self.removing.load(Ordering::Acquire) {
            return Ok(());
        }
        let guard = self.hid_listeners.lock();
        for entry in &*guard {
            if entry.svc.endpoint == ep.index && entry.svc.channel == ch {
                return entry.listener.process_fakehid_report(data);
            }
        }
        Ok(())
    }

    fn shutdown_complete(&self, endpoint: u8) {
        if let Some(index) = endpoint.checked_sub(AFK_ENDPOINT_START) {
            if let Some(done) = self.ep_shutdown.get(index as usize) {
                done.complete(());
            }
        }
    }

    fn stop(&self) -> Result<()> {
        for ep in 0..AFK_ENDPOINT_COUNT as usize {
            {
                let mut guard = self.rtkit.lock();
                let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
                let ep_guard = self.endpoints[ep].lock();
                if !ep_guard.started {
                    continue;
                }
                self.ep_shutdown[ep].reset();
                ep_guard.stop(rtk.as_mut())?;
            }
            if self.ep_shutdown[ep]
                .wait_timeout(AFK_SHUTDOWN_TIMEOUT_MS)
                .is_none()
            {
                return Err(ETIMEDOUT);
            }
            self.endpoints[ep].lock().started = false;
        }
        Ok(())
    }

    fn retain_dma_buffers(&self) {
        // Firmware did not confirm quiescence. Detach host callbacks but keep
        // every exposed allocation out of the allocator until a cold reboot.
        for endpoint in &self.endpoints {
            if let Some(buffer) = endpoint.lock().iomem.take() {
                mem::forget(buffer);
            }
        }
        if let Some(binding) = self.source_ring.lock().take() {
            mem::forget(binding);
        }
        if let Some(arena) = self.setup.lock().arena.take() {
            mem::forget(arena);
        }
    }

    fn teardown(&self) {
        {
            let _gate = self.registration_gate.lock();
            if self.removing.swap(true, Ordering::AcqRel) {
                return;
            }
        }
        self.registration_queue.drain();
        // Wait for in-flight report delivery and detach listeners before the
        // child drivers release their IIO/PCM objects. RPC replies stay live.
        self.hid_listeners.lock().clear();
        self.report_listeners.lock().clear();
        let children = {
            let mut guard = self.subdevices.lock();
            mem::replace(&mut *guard, KVec::new())
        };
        // Prevent every probe path from racing child unbind. Keep the device
        // registered: the source ring still needs its IOMMU/DMA context.
        for pdev in &children {
            // SAFETY: tracked platform devices stay registered until below.
            unsafe {
                let dev = ptr::addr_of_mut!((**pdev).dev);
                bindings::device_quarantine(dev);
                bindings::device_release_driver(dev);
            }
        }
        self.transport_closing.store(true, Ordering::Release);

        if self.cpu_started.load(Ordering::Acquire) {
            if let Err(e) = self.stop() {
                dev_warn!(self.dev, "AFK shutdown incomplete: {:?}\n", e);
            }
        }
        // Remove the handle from shared state before the blocking shutdown or
        // Drop; receive callbacks tolerate None and cannot deadlock this lock.
        let rtkit = { self.rtkit.lock().take() };
        let mut quiesced = !self.cpu_started.load(Ordering::Acquire);
        if let Some(mut rtkit) = rtkit {
            if !quiesced {
                match Pin::new(&mut rtkit).shutdown() {
                    Ok(()) => quiesced = true,
                    Err(e) => {
                        rtkit.retain_shared_buffers_on_drop();
                        dev_err!(
                            self.dev,
                            "AOP shutdown unconfirmed ({:?}); DMA retained, cold reboot required\n",
                            e
                        );
                    }
                }
            }
            drop(rtkit);
        }

        // Stop IRQ ingress first, then drain only our setup queue. No setup
        // mutex is held while mailbox Drop synchronizes the hard IRQ.
        self.setup_rx_closed.store(true, Ordering::Release);
        let setup_mailbox = { self.setup.lock().mbox.take() };
        drop(setup_mailbox);
        self.setup_queue.drain();
        if !quiesced {
            self.retain_dma_buffers();
            // The parent core device lock is held in probe/unbind/Drop. Keep
            // the registered children and their DMA domains, and block this
            // parent from probing again until a cold reboot recreates it.
            unsafe { bindings::device_quarantine_locked(self.dev.as_raw()) };
            dev_err!(
                self.dev,
                "retaining {} unbound AOP devices and DMA mappings; cold reboot required\n",
                children.len()
            );
            return;
        }

        // Free child-device DMA before device_del removes the IOMMU domain.
        let source = { self.source_ring.lock().take() };
        drop(source);
        for pdev in &children {
            // SAFETY: each unbound device is registered and owned exactly once.
            unsafe { bindings::platform_device_unregister(*pdev) };
        }
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

    /// Handle one setup-port message on the private queue under the setup mutex.
    fn setup_handle(&self, st: &mut SetupState, ep: u8, word: u64) -> Result<()> {
        if ep == SETUP_MGMT_EP {
            let ty = (word >> 52) & 0xff;
            match ty {
                SETUP_TYPE_HELLO => {
                    if word & 0xffffffff != SETUP_HELLO_VERSION {
                        dev_warn!(
                            self.dev,
                            "setup port: HELLO version {:#x}",
                            word & 0xffffffff
                        );
                    }
                    st.send(
                        SETUP_MGMT_EP,
                        (SETUP_TYPE_HELLO_REPLY << 52) | SETUP_HELLO_VERSION,
                    )
                }
                SETUP_TYPE_EPMAP => {
                    // Acknowledge each advertised endpoint-map fragment.
                    st.send(SETUP_MGMT_EP, word)?;
                    if word & SETUP_EPMAP_LAST != 0 {
                        st.map_done = true;
                    }
                    Ok(())
                }
                SETUP_TYPE_AP_PWR => {
                    st.ap_ready = (word & 0xffff) == SETUP_AP_PWR_ON;
                    Ok(())
                }
                SETUP_TYPE_UNK3 => st.send(SETUP_MGMT_EP, SETUP_TYPE_UNK3_REPLY << 52),
                SETUP_TYPE_PWR_ACK => Ok(()),
                _ => Err(EIO),
            }
        } else if word >> 56 == SETUP_BUFFER_REQUEST {
            self.setup_endpoint_request(st, ep, word)
        } else if st.endpoint(ep).is_some() {
            st.reply = Some(word);
            Ok(())
        } else {
            Err(EIO)
        }
    }

    /// The source-buffer bind: a 24-byte request in the endpoint's
    /// host-to-AOP page, (3 << 60) | len on the endpoint, the firmware's
    /// 0x2000000000000001, one status byte in the reply window, then
    /// 4 << 60 to release it.
    fn setup_bind_source(&self, iova: u64, size: u64) -> Result<()> {
        let mut st = self.setup.lock();
        let ep = st.endpoint(SETUP_SOURCE_EP).ok_or(ENXIO)?;
        let tx_off = st.arena_offset(ep.tx_iova).ok_or(EIO)?;
        let rx_off = st.arena_offset(ep.rx_iova).ok_or(EIO)?;
        let mut req = [0u8; 24];
        req[..8].copy_from_slice(&SETUP_SET_SOURCE_BUFFER.to_le_bytes());
        req[8..16].copy_from_slice(&iova.to_le_bytes());
        req[16..24].copy_from_slice(&size.to_le_bytes());
        {
            let arena = st.arena.as_mut().ok_or(ENXIO)?;
            // SAFETY: the arena is a coherent allocation of
            // SETUP_ARENA_PAGES pages and tx_off is a page inside it.
            unsafe {
                let buf = &mut arena.as_mut()[tx_off..tx_off + req.len()];
                buf.copy_from_slice(&req);
            }
        }
        mem_sync();
        st.reply = None;
        st.send(SETUP_SOURCE_EP, SETUP_REQUEST_TYPE | req.len() as u64)?;
        let mut waited = 0i64;
        while st.reply.is_none() {
            if st.failed || self.setup_rx_failed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            let _ = self
                .setup_cv
                .wait_interruptible_timeout(&mut st, msecs_to_jiffies(100));
            waited += 100;
            if waited >= SETUP_REPLY_TIMEOUT_MS && st.reply.is_none() {
                dev_err!(self.dev, "setup port: source bind timed out\n");
                return Err(ETIMEDOUT);
            }
        }
        let reply = st.reply.take().unwrap();
        if reply != SETUP_REPLY_READY {
            dev_err!(
                self.dev,
                "setup port: unexpected source bind reply {:#x}\n",
                reply
            );
            return Err(EIO);
        }
        mem_sync();
        let status = {
            let arena = st.arena.as_ref().ok_or(ENXIO)?;
            // SAFETY: rx_off is a page inside the arena.
            unsafe { arena.as_ref()[rx_off] }
        };
        st.send(SETUP_SOURCE_EP, SETUP_REQUEST_DONE)?;
        if status != 0 {
            dev_err!(self.dev, "setup port: source bind status {:#x}\n", status);
            return Err(EIO);
        }
        dev_dbg!(
            self.dev,
            "setup port: source ring {:#x} ({} bytes) bound\n",
            iova,
            size
        );
        Ok(())
    }

    /// Push the ALS calibration on setup-port endpoint 0x21.
    ///
    /// Without it the sensor answers every property correctly and accepts the
    /// arming interval, and then reports nothing at all, because the trusted
    /// side never powers the part.  With it the samples start immediately.
    /// The machine-supplied payload remains external to the driver.
    fn setup_push_als_calibration(&self) -> Result<()> {
        let calibration = aop_als_calibration::load(self.dev.as_ref())?;
        let mut st = self.setup.lock();
        let ep = match st.endpoint(SETUP_ALS_EP) {
            Some(ep) => ep,
            None => {
                dev_warn!(
                    self.dev,
                    "setup port: no endpoint {:#04x}; ALS uncalibrated\n",
                    SETUP_ALS_EP
                );
                return Ok(());
            }
        };
        let tx_off = st.arena_offset(ep.tx_iova).ok_or(EIO)?;
        let rx_off = st.arena_offset(ep.rx_iova).ok_or(EIO)?;
        {
            let arena = st.arena.as_mut().ok_or(ENXIO)?;
            // SAFETY: the arena is a coherent allocation of SETUP_ARENA_PAGES
            // pages and tx_off is a page inside it.
            unsafe {
                let end = tx_off.checked_add(calibration.data().len()).ok_or(EIO)?;
                let buf = arena.as_mut().get_mut(tx_off..end).ok_or(EIO)?;
                buf.copy_from_slice(calibration.data());
            }
        }
        mem_sync();
        st.reply = None;
        st.send(
            SETUP_ALS_EP,
            SETUP_REQUEST_TYPE | calibration.data().len() as u64,
        )?;
        let mut waited = 0i64;
        while st.reply.is_none() {
            if st.failed || self.setup_rx_failed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            let _ = self
                .setup_cv
                .wait_interruptible_timeout(&mut st, msecs_to_jiffies(100));
            waited += 100;
            if waited >= SETUP_REPLY_TIMEOUT_MS && st.reply.is_none() {
                dev_err!(self.dev, "setup port: ALS calibration timed out\n");
                return Err(ETIMEDOUT);
            }
        }
        let reply = st.reply.take().unwrap();
        if reply != SETUP_REPLY_READY && reply != SETUP_REPLY_READY_ALT {
            dev_err!(
                self.dev,
                "setup port: unexpected ALS calibration reply {:#x}\n",
                reply
            );
            return Err(EIO);
        }
        mem_sync();
        let status = {
            let arena = st.arena.as_ref().ok_or(ENXIO)?;
            // SAFETY: rx_off is a page inside the arena.
            unsafe { arena.as_ref()[rx_off] }
        };
        st.send(SETUP_ALS_EP, SETUP_REQUEST_DONE)?;
        if status != 0 {
            dev_err!(
                self.dev,
                "setup port: ALS calibration rejected ({:#x})\n",
                status
            );
            return Err(EIO);
        }
        dev_dbg!(
            self.dev,
            "setup port: ALS calibration pushed (reply {:#x}, status {:#x})\n",
            reply,
            status
        );
        Ok(())
    }

    /// Endpoint buffer request: one host-to-AOP page and a reply window of
    /// requested * 16 bytes, both handed out from the pre-mapped arena.
    fn setup_endpoint_request(&self, st: &mut SetupState, ep: u8, word: u64) -> Result<()> {
        let requested = (word & 0xffffffff) as usize;
        let slot = SETUP_ENDPOINTS.iter().position(|e| *e == ep).ok_or(EIO)?;
        if st.endpoints[slot].is_some() || (requested != 0x400 && requested != 0x1000) {
            return Err(EIO);
        }
        let rx_pages = (requested * 16).div_ceil(SETUP_PAGE);
        if st.next_page + 1 + rx_pages > SETUP_ARENA_PAGES {
            return Err(ENOMEM);
        }
        let base = st.arena_iova();
        let tx_iova = base + (st.next_page * SETUP_PAGE) as u64;
        st.next_page += 1;
        let rx_iova = base + (st.next_page * SETUP_PAGE) as u64;
        st.next_page += rx_pages;
        st.send(ep, SETUP_BUFFER_REQUEST_ACK)?;
        st.send(ep, SetupState::shared_descriptor(tx_iova, 1, true))?;
        st.send(ep, SetupState::shared_descriptor(rx_iova, rx_pages, false))?;
        st.endpoints[slot] = Some(SetupEndpoint {
            ep,
            tx_iova,
            rx_iova,
        });
        dev_dbg!(
            self.dev,
            "setup port: endpoint {:#x} tx {:#x} rx {:#x} ({} pages)\n",
            ep,
            tx_iova,
            rx_iova,
            rx_pages
        );
        Ok(())
    }

    /// After the primary port reports AP power ON: request the setup port's
    /// AP power state and wait for its ack and the five endpoint buffers.
    fn setup_finish_boot(&self) -> Result<()> {
        let mut st = self.setup.lock();
        if !st.map_done {
            dev_warn!(
                self.dev,
                "setup port: EPMAP not complete before AP power request\n"
            );
        }
        if !st.power_sent {
            st.send(SETUP_MGMT_EP, (SETUP_TYPE_AP_PWR << 52) | SETUP_AP_PWR_INIT)?;
            st.power_sent = true;
        }
        let mut waited = 0i64;
        while !st.ready() && !st.failed && !self.setup_rx_failed.load(Ordering::Acquire) {
            let _ = self
                .setup_cv
                .wait_interruptible_timeout(&mut st, msecs_to_jiffies(100));
            waited += 100;
            if waited >= SETUP_BOOT_TIMEOUT_MS {
                break;
            }
        }
        let ready = st.ready() && !st.failed && !self.setup_rx_failed.load(Ordering::Acquire);
        let count = st.endpoint_count();
        let ap_ready = st.ap_ready;
        let failed = st.failed || self.setup_rx_failed.load(Ordering::Acquire);
        drop(st);
        if !ready {
            dev_err!(
                self.dev,
                "setup port: boot incomplete (ap_ready {} endpoints {}/{} failed {})\n",
                ap_ready,
                count,
                SETUP_ENDPOINTS.len(),
                failed
            );
            return Err(ETIMEDOUT);
        }
        dev_dbg!(self.dev, "setup port: dual-mailbox boot complete\n");
        Ok(())
    }

    fn start_cpu(&self, asc_mmio: &RelaxedMmio<ASC_MMIO_SIZE>) -> Result<()> {
        let val = asc_mmio.read32(CPU_CONTROL);
        asc_mmio.write32(val | CPU_RUN, CPU_CONTROL);
        self.cpu_started.store(true, Ordering::Release);
        Ok(())
    }
}

impl AopData {
    /// Serializes the calls on an endpoint whose replies carry no tag; the
    /// guard is held until the reply (or the timeout) so that a reply can
    /// only belong to the one call in flight.
    fn take_call_turn(&self, ep_idx: u8) -> Option<MutexGuard<'_, ()>> {
        self.epic_v4.then(|| self.call_turn[ep_idx as usize].lock())
    }
    /// Waits for a call's reply with a bound; on timeout the call slot is
    /// released. Untagged endpoints are then closed to further calls until
    /// reinitialized, so a late reply cannot complete a different request.
    fn wait_call(
        &self,
        ep_idx: u8,
        svc: &EPICService,
        subtype: u16,
        call: Arc<FutureValue<CallResult>>,
    ) -> Result<CallResult> {
        match call.wait_timeout(EPIC_CALL_TIMEOUT_MS) {
            Some(res) => Ok(res),
            None => {
                let mut ep_guard = self.endpoints[ep_idx as usize].lock();
                ep_guard.cancel_call(&call);
                // Without a request tag, a delayed reply could otherwise
                // complete the next caller with the previous result. Only
                // reinitializing the endpoint establishes a fresh boundary.
                if self.epic_v4 {
                    ep_guard.call_desynchronized = true;
                }
                dev_err!(
                    self.dev,
                    "EPIC call {:#x} on channel {} timed out after {} ms\n",
                    subtype,
                    svc.channel,
                    EPIC_CALL_TIMEOUT_MS
                );
                Err(ETIMEDOUT)
            }
        }
    }
}

impl AOP for AopData {
    fn epic_call(&self, svc: &EPICService, subtype: u16, msg_bytes: &[u8]) -> Result<u32> {
        if self.transport_closing.load(Ordering::Acquire) {
            return Err(ENODEV);
        }
        let ep_idx = svc.endpoint - AFK_ENDPOINT_START;
        let _turn = self.take_call_turn(ep_idx);
        let call = {
            let mut rtk_guard = self.rtkit.lock();
            let mut rtk = rtk_guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            let mut ep_guard = self.endpoints[ep_idx as usize].lock();
            ep_guard.epic_notify(
                self,
                rtk.as_mut(),
                svc.channel,
                subtype,
                EPIC_CATEGORY_NOTIFY,
                msg_bytes,
                None,
            )?
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
        if self.transport_closing.load(Ordering::Acquire) {
            return Err(ENODEV);
        }
        let ep_idx = svc.endpoint - AFK_ENDPOINT_START;
        let _turn = self.take_call_turn(ep_idx);
        let call = {
            let mut rtk_guard = self.rtkit.lock();
            let mut rtk = rtk_guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
            let mut ep_guard = self.endpoints[ep_idx as usize].lock();
            let mut ret_buf = KVec::new();
            ret_buf.resize(ret_len, 0, GFP_KERNEL)?;
            ep_guard.epic_notify(
                self,
                rtk.as_mut(),
                svc.channel,
                subtype,
                EPIC_CATEGORY_NOTIFY,
                msg_bytes,
                Some(ret_buf),
            )?
        };
        let res = self.wait_call(ep_idx, svc, subtype, call)?;
        Ok((res.retcode, res.extra_data.unwrap()))
    }

    fn add_fakehid_listener(
        &self,
        svc: EPICService,
        listener: Arc<dyn FakehidListener>,
    ) -> Result<()> {
        let mut guard = self.hid_listeners.lock();
        if self.removing.load(Ordering::Acquire) {
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
    fn add_report_listener(
        &self,
        svc: EPICService,
        subtype: u16,
        listener: Arc<dyn ReportListener>,
    ) -> Result<()> {
        let mut guard = self.report_listeners.lock();
        if self.removing.load(Ordering::Acquire) {
            return Err(ENODEV);
        }
        if guard
            .iter()
            .any(|entry| entry.svc == svc && entry.subtype == subtype)
        {
            return Err(EBUSY);
        }
        Ok(guard.push(
            ReportListenerEntry {
                svc,
                subtype,
                listener,
            },
            GFP_KERNEL,
        )?)
    }
    fn remove_report_listener(&self, svc: &EPICService, subtype: u16) -> bool {
        let mut guard = self.report_listeners.lock();
        for i in 0..guard.len() {
            if guard[i].svc == *svc && guard[i].subtype == subtype {
                guard.swap_remove(i);
                return true;
            }
        }
        false
    }
    fn source_ring(&self, dev: &device::Device<Bound>, size: usize) -> Result<Arc<SourceRing>> {
        if !self.epic_v4 || self.transport_closing.load(Ordering::Acquire) {
            return Err(ENODEV);
        }
        let mut guard = self.source_ring.lock();
        if let Some(binding) = guard.as_ref() {
            if !binding.confirmed {
                return Err(EIO);
            }
            if binding.ring.size() < size {
                return Err(EBUSY);
            }
            return Ok(binding.ring.clone());
        }
        let buf = Coherent::<u8>::zeroed_slice(dev, size, GFP_KERNEL)?;
        let iova = buf.dma_handle() as u64;
        // Complete fallible host ownership before giving the address to firmware.
        let ring = Arc::new(SourceRing { buf, iova }, GFP_KERNEL)?;
        *guard = Some(SourceRingBinding {
            ring: ring.clone(),
            confirmed: false,
        });
        // Even a timeout may mean the firmware accepted the ring. Keep pending
        // ownership and reject later requests instead of rebinding or freeing it.
        self.setup_bind_source(iova, size as u64)?;
        if let Some(binding) = &mut *guard {
            binding.confirmed = true;
        }
        Ok(ring)
    }
    fn remove(&self) {
        self.teardown();
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
        let Some(index) = ep.checked_sub(AFK_ENDPOINT_START) else {
            return;
        };
        if index >= AFK_ENDPOINT_COUNT {
            return;
        }
        let mut guard = data.rtkit.lock();
        let Some(mut rtk) = guard.as_mut().as_pin_mut() else {
            return;
        };
        let mut ep_guard = data.endpoints[index as usize].lock();
        if let Err(e) = ep_guard.recv_message(data, rtk.as_mut(), msg) {
            dev_err!(data.dev, "Failed to handle rtkit message, error: {:?}", e);
        }
    }

    fn crashed(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, _crashlog: Option<&[u8]>) {
        dev_err!(data.dev, "AOP firmware crashed");
    }
}

#[repr(transparent)]
struct AopDriver(Arc<dyn AOP>);

struct AopHwConfig {
    ec0p: u64,
    alig: u64,
    aopt: u64,
    /// t8140: no bootarg patching, a second "setup" mailbox to boot through,
    /// EPIC v4 on the AFK endpoints.
    t8140: bool,
}

const HW_CFG_T8103: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 1,
    alig: 128,
    t8140: false,
};
const HW_CFG_T8112: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 0,
    alig: 128,
    t8140: false,
};
const HW_CFG_T6000: AopHwConfig = AopHwConfig {
    ec0p: 0x020000,
    aopt: 0,
    alig: 64,
    t8140: false,
};
const HW_CFG_T6020: AopHwConfig = AopHwConfig {
    ec0p: 0x0100_00000000,
    aopt: 0,
    alig: 64,
    t8140: false,
};
const HW_CFG_T8140: AopHwConfig = AopHwConfig {
    ec0p: 0,
    aopt: 0,
    alig: 0,
    t8140: true,
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
        (of::DeviceId::new(c_str!("apple,t8140-aop")), &HW_CFG_T8140),
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
        let data = AopData::new(pdev, cfg.t8140)?;
        let probe_guard = ScopeGuard::new_with_data(data.clone(), |data| data.teardown());
        let aop_mmio = aop_mmio.access(pdev.as_ref())?;
        if !cfg.t8140 {
            data.patch_bootargs(
                aop_mmio,
                &[
                    (from_fourcc(b"EC0p"), cfg.ec0p),
                    (from_fourcc(b"nCal"), 0x0),
                    (from_fourcc(b"alig"), cfg.alig),
                    (from_fourcc(b"AOPt"), cfg.aopt),
                ],
            )?;
        }
        let rtkit = rtkit::RtKit::<AopData>::new(pdev.as_ref(), None, 0, data.clone())?;
        *data.rtkit.lock() = Some(rtkit);
        if cfg.t8140 {
            // The setup port must be answered from the first HELLO on, so it
            // is opened (and its 13-page arena mapped through dart-aop stream
            // 1, the setup mailbox's own IOMMU stream) before the CPU runs.
            let setup_mbox = mailbox::Mailbox::<SetupPortCallback>::new_byname(
                pdev.as_ref(),
                c_str!("setup"),
                data.clone(),
            )?;
            let setup_dev = setup_mbox.device();
            // SAFETY: no DMA through the setup mailbox device is in flight yet.
            unsafe {
                to_result(bindings::dma_set_mask_and_coherent(
                    setup_dev.as_raw(),
                    DmaMask::new::<42>().value(),
                ))?;
            }
            // SAFETY: the mailbox provider is bound for as long as we hold the
            // mailbox client.
            let bound = unsafe { setup_dev.as_bound() };
            let arena =
                Coherent::<u8>::zeroed_slice(bound, SETUP_ARENA_PAGES * SETUP_PAGE, GFP_KERNEL)?;
            dev_dbg!(
                pdev.as_ref(),
                "setup port: arena {} pages at iova {:#x}\n",
                SETUP_ARENA_PAGES,
                arena.dma_handle() as u64
            );
            let mut st = data.setup.lock();
            st.arena = Some(arena);
            st.mbox = Some(setup_mbox);
            drop(st);
        }
        let asc_mmio = asc_mmio.access(pdev.as_ref())?.relaxed();
        data.start_cpu(asc_mmio)?;
        if cfg.t8140 {
            // Primary port RTKit v12 handshake to AP power ON, with setup RX
            // serviced by its private queue; then the setup port's own AP
            // power state and endpoint buffers.
            {
                let mut guard = data.rtkit.lock();
                let mut rtk = guard.as_mut().as_pin_mut().ok_or(ENODEV)?;
                rtk.as_mut().wake()?;
            }
            data.setup_finish_boot()?;
            // Before any EPIC client opens the ALS: the trusted side wants the
            // calibration first, and a failure here must not cost us the AOP.
            if let Err(e) = data.setup_push_als_calibration() {
                dev_warn!(
                    pdev.as_ref(),
                    "t8140 AOP: ALS calibration failed ({:?})\n",
                    e
                );
            }
            dev_dbg!(
                pdev.as_ref(),
                "t8140 AOP: both ports up, starting AFK endpoints\n"
            );
            data.start_afk()?;
        } else {
            data.start()?;
        }
        let data = data as Arc<dyn AOP>;
        drop(probe_guard.dismiss());
        Ok(Self(data))
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        // Driver data and devres are still live here. Drop runs after both
        // have been revoked and is only an idempotent fallback.
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
    firmware: ["apple/t8140-ct817-cal.bin"],
}
