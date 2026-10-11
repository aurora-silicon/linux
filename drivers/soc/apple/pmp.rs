// SPDX-License-Identifier: (GPL-2.0-only OR MIT) AND GPL-2.0-only
#![recursion_limit = "2048"]

//! Apple PMP driver
//!
//! Copyright (C) The Asahi Linux Contributors

use core::mem;

use kernel::{
    bindings,
    device::{
        self,
        Core, //
    },
    devres::Devres,
    dma::{
        Coherent,
        CoherentBox, //
        Device as _,
        DmaMask,
    },
    io::{
        mem::IoMem,
        Io, //
    },
    iosys_map::IoSysMapRef,
    kvec,
    module_platform_driver,
    new_mutex,
    of,
    platform,
    prelude::*,
    soc::apple::rtkit,
    str::CString,
    sync::{
        aref::ARef,
        Arc,
        Mutex, //
    },
    transmute::{
        AsBytes,
        FromBytes, //
    },
    types::ForeignOwnable, //
};

const PMP_MMIO_SIZE: usize = 0x80000;
const ASC_MMIO_SIZE: usize = 0x4000;
const BOOTARGS_OFFSET: usize = 0x22c;
const BOOTARGS_SIZE: usize = 0x230;
const CPU_CONTROL: usize = 0x44;
const CPU_RUN: u32 = 0x1 << 4;
const PMP_ENDPOINT: u8 = 0x20;
const OPC_GET_IOVA_TABLE: u64 = 0x10;
const OPC_MALLOC: u64 = 0x12;
const OPC_FREE: u64 = 0x14;
const OPC_SET_BUF: u64 = 0x30;
const OPC_REGISTER_IOREG: u64 = 0x32;
const OPC_SET_IOREG: u64 = 0x34;
const OPC_UPDATE_IOREG_V2: u64 = 0x36;
const OPC_ACK_MASK: u64 = 0x1;
const OPC_SHIFT: u32 = 48;
const MALLOC_SIZE_MASK: u64 = 0xFFFFFF;
const MSG_IOVA_MASK: u64 = 0xFFFFFFFFFFFF;
const SET_IOREG_INDEX_MASK: u64 = 0xFFFF;
const PIO_VM_BASE: u64 = 0xc0000000;
const PIO_GRANULARITY: u64 = 0x1000000;

const fn from_fourcc(b: &[u8; 4]) -> u32 {
    b[3] as u32 | (b[2] as u32) << 8 | (b[1] as u32) << 16 | (b[0] as u32) << 24
}

// Validate the complete table before changing it. The firmware table contains
// mixed-width entries, but the board configuration patched here is all u32.
fn patch_bootarg_table(bytes: &mut [u8], patches: &[(u32, u32)]) -> Result<()> {
    for apply in [false, true] {
        let mut rest = &mut *bytes;
        while !rest.is_empty() {
            if rest.len() < 8 {
                return Err(EINVAL);
            }
            let (header, tail) = rest.split_at_mut(8);
            let key = u32::from_le_bytes(header[..4].try_into().unwrap());
            let size = u32::from_le_bytes(header[4..].try_into().unwrap()) as usize;
            if size > tail.len() {
                return Err(EINVAL);
            }
            let (value, tail) = tail.split_at_mut(size);
            if let Some((_, replacement)) = patches.iter().find(|(k, _)| *k == key) {
                if size != mem::size_of::<u32>() {
                    return Err(EINVAL);
                }
                if apply {
                    value.copy_from_slice(&replacement.to_le_bytes());
                }
            }
            rest = tail;
        }
    }
    Ok(())
}

struct PmpAllocation {
    addr: u64,
    alloc: Coherent<[u8]>,
}

struct RegistryDescriptor {
    name: [u8; 48],
    size: u32,
    id: u16,
}

impl RegistryDescriptor {
    fn parse(bytes: &[u8]) -> Result<Self> {
        // M5 firmware sends name[48], format[8], unit[8], then three u64s:
        // value length, firmware-assigned ID, and flags. IDs are low16 on wire.
        if bytes.len() < 88 {
            return Err(EINVAL);
        }
        let size = u64::from_le_bytes(bytes[64..72].try_into().unwrap());
        let id = u64::from_le_bytes(bytes[72..80].try_into().unwrap());
        let name: [u8; 48] = bytes[..48].try_into().unwrap();
        if size > u32::MAX as u64 || id > u16::MAX as u64 || !name.contains(&0) {
            return Err(EINVAL);
        }
        Ok(Self {
            name,
            size: size as u32,
            id: id as u16,
        })
    }
}

struct RegistryEntry {
    id: u16,
    value: KVec<u8>,
}

struct IovaTableEntry {
    _host_addr: u64,
    _pio_base: u64,
    _size: u64,
}

// SAFETY: TODO:
unsafe impl AsBytes for IovaTableEntry {}
// SAFETY: TODO:
unsafe impl FromBytes for IovaTableEntry {}

struct PmpState {
    iova_table: Option<Coherent<[IovaTableEntry]>>,
    allocs: KVec<PmpAllocation>,
    value_buf: Option<u64>,
    host_value_buf: Option<u64>,
    ioreg_entries: KVec<u32>,
    registry_entries: KVec<RegistryEntry>,
}

impl PmpState {
    fn new(registry_v2: bool) -> Result<PmpState> {
        Ok(PmpState {
            iova_table: None,
            allocs: KVec::with_capacity(10, GFP_KERNEL)?,
            value_buf: None,
            host_value_buf: None,
            ioreg_entries: KVec::with_capacity(if registry_v2 { 0 } else { 340 }, GFP_KERNEL)?,
            registry_entries: KVec::with_capacity(if registry_v2 { 340 } else { 0 }, GFP_KERNEL)?,
        })
    }
    fn find_alloc(&self, addr: u64) -> Option<usize> {
        // Due to how pmp manages memory, iterating in reverse will
        // usually result in us getting the right one on the first try
        for (i, e) in self.allocs.iter().enumerate().rev() {
            if e.addr == addr {
                return Some(i);
            }
        }
        None
    }
    fn get_buf(&mut self, addr: u64) -> Option<&mut Coherent<[u8]>> {
        let idx = self.find_alloc(addr)?;
        Some(&mut self.allocs[idx].alloc)
    }
}

#[pin_data]
struct PmpData {
    dev: ARef<device::Device>,
    registry_v2: bool,
    pmp_mmio: Pin<KBox<Devres<IoMem<PMP_MMIO_SIZE>>>>,
    asc_mmio: Pin<KBox<Devres<IoMem<ASC_MMIO_SIZE>>>>,
    #[pin]
    rtkit: Mutex<Option<rtkit::RtKit<PmpData>>>,
    // The registry-v2 RtKit is owned by PmpDriver, avoiding the context's Arc cycle.
    // It drains callbacks before releasing the context and its DMA buffers.
    #[pin]
    registry_rtkit: Mutex<Option<usize>>,
    #[pin]
    stopped: Mutex<bool>,
    #[pin]
    state: Mutex<PmpState>,
}

impl PmpData {
    fn new(dev: &platform::Device<Core>, registry_v2: bool) -> Result<Arc<PmpData>> {
        let pmp_req = dev.io_request_by_name(c"pmp").ok_or(EINVAL)?;
        let pmp_mmio = KBox::pin_init(pmp_req.iomap_sized::<PMP_MMIO_SIZE>(), GFP_KERNEL)?;
        let asc_req = dev.io_request_by_name(c"asc").ok_or(EINVAL)?;
        let asc_mmio = KBox::pin_init(asc_req.iomap_sized::<ASC_MMIO_SIZE>(), GFP_KERNEL)?;
        let pmp_state = PmpState::new(registry_v2)?;
        Arc::pin_init(
            try_pin_init!(
                PmpData {
                    dev: dev.as_ref().into(),
                    registry_v2,
                    pmp_mmio,
                    asc_mmio,
                    rtkit <- new_mutex!(None),
                    registry_rtkit <- new_mutex!(None),
                    stopped <- new_mutex!(false),
                    state <- new_mutex!(pmp_state)
                }
            ),
            GFP_KERNEL,
        )
    }
    fn start_cpu(&self, dev: &platform::Device<Core>) -> Result<()> {
        let asc_mmio = self.asc_mmio.access(dev.as_ref())?.relaxed();
        let val = asc_mmio.read32(CPU_CONTROL);
        asc_mmio.write32(val | CPU_RUN, CPU_CONTROL);
        Ok(())
    }
    fn start(&self) -> Result<()> {
        if self.registry_v2 {
            let raw = self.context_handle()?;
            // SAFETY: Probe owns RtKit and firmware has no application
            // endpoint yet; C serializes system messages and power waits.
            unsafe {
                kernel::error::to_result(bindings::apple_rtkit_wake(raw))?;
                return kernel::error::to_result(bindings::apple_rtkit_start_ep(raw, PMP_ENDPOINT));
            }
        }
        let mut guard = self.rtkit.lock();
        let mut rtk = guard.as_mut().as_pin_mut().unwrap();
        rtk.as_mut().wake()?;
        rtk.start_endpoint(PMP_ENDPOINT)
    }
    fn context_handle(&self) -> Result<*mut bindings::apple_rtkit> {
        self.registry_rtkit
            .lock()
            .map(|p| p as *mut bindings::apple_rtkit)
            .ok_or(ENODEV)
    }
    fn cold_preflight(&self, dev: &platform::Device<Core>) -> Result {
        let io = self.pmp_mmio.access(dev.as_ref())?;
        let asc = self.asc_mmio.access(dev.as_ref())?;
        // Captured T6050 cold marker. Retained contexts carry live DMA
        // addresses and cannot be cold-reprobed after their buffers retire.
        if asc.read32(CPU_CONTROL) & CPU_RUN != 0
            || io.read64(0x7bcd8) != 0xfeed1b00
            || io.read64(0x7bce0) != 0
            || io.read64(0x7bcc8) != 0
            || io.read64(0x7bcd0) != 0
            || io.read32(0x7bcc4) != 0
            || io.read32(0x7bcc0) != 0
        {
            return Err(EBUSY);
        }
        Ok(())
    }
    fn stop_context(&self) -> Result {
        let mut stopped = self.stopped.lock();
        if *stopped {
            return Ok(());
        }
        // SAFETY: Only probe, core unbind, and the registered reboot callback
        // call this method. PmpDriver owns these resources through each call;
        // its first dropped field unregisters and drains the callback.
        let dev = unsafe { self.dev.as_bound() };
        let io = self.pmp_mmio.access(dev)?;
        let saves = io.read32(0x7bcc4);
        let wakes = io.read32(0x7bcc0);
        let raw = self.context_handle()?;
        // Do not hold the reply mutex while waiting: RX callbacks need it
        // to finish outstanding registry exchanges. Ownership remains with
        // the driver, and the stop mutex serializes unbind/reboot.
        unsafe { kernel::error::to_result(bindings::apple_rtkit_idle(raw))? };
        let delta = io.read32(0x7bcc4).wrapping_sub(saves);
        if io.read64(0x7bcd8) != 0xcafe4b0b
            || io.read64(0x7bce0) != 0x10024a8
            || io.read64(0x7bcc8) == 0
            || delta == 0
            || delta >= 1u32 << 31
            || io.read32(0x7bcc0) != wakes
        {
            return Err(Error::from_errno(-(bindings::EPROTO as i32)));
        }
        let asc = self.asc_mmio.access(dev)?;
        asc.write32(asc.read32(CPU_CONTROL) & !CPU_RUN, CPU_CONTROL);
        if asc.read32(CPU_CONTROL) & CPU_RUN != 0 {
            return Err(EIO);
        }
        *stopped = true;
        Ok(())
    }
    fn patch_bootargs(&self, dev: &platform::Device<Core>, patches: &[(u32, u32)]) -> Result<()> {
        let io = self.pmp_mmio.access(dev.as_ref())?.relaxed();
        let offset = io.read32(BOOTARGS_OFFSET) as usize;
        let size = io.read32(BOOTARGS_SIZE) as usize;
        // Bound the allocation as well as the MMIO copy. Firmware supplies both
        // fields, and an invalid table must not reach the writeback below.
        if offset < BOOTARGS_SIZE + mem::size_of::<u32>()
            || offset > io.maxsize()
            || size == 0
            || size > io.maxsize() - offset
        {
            return Err(EINVAL);
        }
        let mut arg_bytes = kvec![0u8; size]?;
        io.try_memcpy_fromio(&mut arg_bytes, offset)?;
        patch_bootarg_table(&mut arg_bytes, patches)?;
        io.try_memcpy_toio(offset, &arg_bytes)
    }
    fn get_iova_table(&self) -> Result<u64> {
        let mut state = self.state.lock();
        if state.iova_table.is_some() {
            dev_err!(self.dev, "Asked for iova table with existing buffer");
            return Err(EIO);
        }
        let node = self.dev.fwnode().ok_or(EIO)?;
        let mut pio_base = PIO_VM_BASE;
        let prop_name = c"apple,pio-ranges";
        if !node.property_present(prop_name) {
            return Ok((OPC_GET_IOVA_TABLE | OPC_ACK_MASK) << OPC_SHIFT);
        }
        let n_entries = node.property_count_elem::<u64>(prop_name)? / 2;
        let ranges = node
            .property_read_array_vec::<u64>(prop_name, n_entries * 2)?
            .required_by(&self.dev)?;
        // SAFETY: TODO: ensure self.dev is bound
        let bound_dev = unsafe { self.dev.as_bound() };
        let mut table = CoherentBox::<[IovaTableEntry]>::zeroed_slice(bound_dev, 170, GFP_KERNEL)?;

        let domain = unsafe { bindings::iommu_get_domain_for_dev(self.dev.as_raw()) };
        for i in 0..n_entries {
            let host_addr = ranges[i * 2];
            let size = ranges[i * 2 + 1];
            unsafe {
                let err = bindings::iommu_map(
                    domain,
                    pio_base as usize,
                    host_addr,
                    size as usize,
                    (bindings::IOMMU_READ | bindings::IOMMU_WRITE | bindings::IOMMU_MMIO) as i32,
                    bindings::GFP_KERNEL,
                );
                if err != 0 {
                    return Err(Error::from_errno(err));
                }
            }
            table[i] = IovaTableEntry {
                _host_addr: host_addr,
                _pio_base: pio_base,
                _size: size,
            };
            pio_base += PIO_GRANULARITY;
        }
        let table: Coherent<[IovaTableEntry]> = table.into();
        let msg = (OPC_GET_IOVA_TABLE | OPC_ACK_MASK) << OPC_SHIFT | table.dma_handle();
        state.iova_table = Some(table);
        Ok(msg)
    }
    fn malloc(&self, size: u64) -> Result<u64> {
        // SAFETY: TODO: ensure self.dev is bound
        let bound_dev = unsafe { self.dev.as_bound() };
        let iomem = Coherent::<u8>::zeroed_slice(bound_dev, size as usize, GFP_KERNEL)?;
        let mut state = self.state.lock();
        let addr = iomem.dma_handle();
        let msg = (OPC_MALLOC | OPC_ACK_MASK) << OPC_SHIFT | addr;
        state.allocs.push(
            PmpAllocation {
                addr: addr,
                alloc: iomem,
            },
            GFP_KERNEL,
        )?;
        Ok(msg)
    }
    fn free(&self, addr: u64) -> Result<u64> {
        let mut state = self.state.lock();
        if let Some(idx) = state.find_alloc(addr) {
            if state.value_buf == Some(addr) {
                state.value_buf = None;
            }
            if state.host_value_buf == Some(addr) {
                state.host_value_buf = None;
            }
            state.allocs.swap_remove(idx);
        } else {
            dev_err!(
                self.dev,
                "Attempted to free memory that was not allocated {}",
                addr
            );
            return Err(EIO);
        }
        let msg = (OPC_FREE | OPC_ACK_MASK) << OPC_SHIFT;
        Ok(msg)
    }
    fn set_buf(&self, addr: u64) -> Result<u64> {
        let mut state = self.state.lock();
        if state.value_buf.is_some() {
            dev_err!(self.dev, "Setting a buffer when one exists");
            return Err(EIO);
        }
        let ptr_buf = if let Some(s) = state.get_buf(addr) {
            s
        } else {
            dev_err!(self.dev, "Unable to find buffer");
            return Err(EIO);
        };
        let pointers_size = if self.registry_v2 { 16 } else { 8 };
        if ptr_buf.size() < pointers_size {
            dev_err!(self.dev, "Buffer too small");
            return Err(EIO);
        }
        // Firmware relinquishes the request buffer until the reply is sent.
        let pointers = unsafe { ptr_buf.as_ref() };
        let ptr = u64::from_le_bytes(pointers[..8].try_into().unwrap());
        if self.registry_v2 {
            let host_ptr = u64::from_le_bytes(pointers[8..16].try_into().unwrap());
            if state.get_buf(ptr).is_none() || state.get_buf(host_ptr).is_none() {
                return Err(EINVAL);
            }
            state.host_value_buf = Some(host_ptr);
        }
        state.value_buf = Some(ptr);
        let msg = (OPC_SET_BUF | OPC_ACK_MASK) << OPC_SHIFT;
        Ok(msg)
    }
    fn register_ioreg(&self, addr: u64) -> Result<u64> {
        let mut state = self.state.lock();
        let msg_buf = if let Some(s) = state.get_buf(addr) {
            s
        } else {
            dev_err!(self.dev, "Unable to find buffer");
            return Err(EIO);
        };
        if msg_buf.size() < 0x44 {
            dev_err!(self.dev, "Buffer too small");
            return Err(EIO);
        }
        // SAFETY: TODO
        let mut size = u32::from_le_bytes(unsafe {
            <&[u8] as TryInto<[u8; 4]>>::try_into(&msg_buf.as_ref()[0x40..0x44]).unwrap()
        });
        if size == 0 {
            let mut name_vec = KVec::with_capacity(0x31, GFP_KERNEL)?;
            name_vec
                .extend_from_slice(unsafe { &msg_buf.as_ref()[0..0x30] }, GFP_KERNEL)
                .unwrap();
            name_vec.push(0, GFP_KERNEL).unwrap();
            let name_str = CStr::from_bytes_until_nul(&name_vec).unwrap();
            let name_str = CString::try_from_fmt(fmt!("apple,tunable-{name_str}"))?;
            let node = self.dev.fwnode().ok_or(EIO)?;
            if state.value_buf.is_none() {
                dev_err!(self.dev, "Value buf not set");
                return Err(EIO);
            }
            let val_buf_addr = state.value_buf.unwrap();
            let val_buf = if let Some(s) = state.get_buf(val_buf_addr) {
                s
            } else {
                dev_err!(self.dev, "Unable to find value buffer");
                return Err(EIO);
            };
            if node.property_present(&name_str) {
                let len = node.property_count_elem::<u8>(&name_str)?;
                let data = node
                    .property_read_array_vec::<u8>(&name_str, len)?
                    .required_by(&self.dev)?;
                unsafe {
                    val_buf.as_mut()[0..len].copy_from_slice(&data);
                }
                size = len as u32;
            } else {
            }
        }
        state.ioreg_entries.push(size, GFP_KERNEL)?;
        let index = state.ioreg_entries.len() as u64;
        let msg = (OPC_REGISTER_IOREG | OPC_ACK_MASK) << OPC_SHIFT | (index << 32) | size as u64;
        Ok(msg)
    }
    fn set_ioreg(&self, index: u64) -> Result<u64> {
        let len = *self
            .state
            .lock()
            .ioreg_entries
            .get(index as usize)
            .ok_or(EIO)? as u64;
        let msg = (OPC_SET_IOREG | OPC_ACK_MASK) << OPC_SHIFT | len;
        Ok(msg)
    }
    fn register_ioreg_v2(&self, addr: u64) -> Result<u64> {
        let mut state = self.state.lock();
        let request = state.get_buf(addr).ok_or(EINVAL)?;
        // SAFETY: The firmware has transferred this allocation for the request
        // and waits for our response before reusing it.
        let descriptor = RegistryDescriptor::parse(unsafe { request.as_ref() })?;
        if state.registry_entries.iter().any(|e| e.id == descriptor.id) {
            return Err(EINVAL);
        }
        let value_addr = state.value_buf.ok_or(EINVAL)?;
        let value_buf = state.get_buf(value_addr).ok_or(EINVAL)?;
        let value = if descriptor.size == 0 {
            let name = CStr::from_bytes_until_nul(&descriptor.name).map_err(|_| EINVAL)?;
            let property = CString::try_from_fmt(fmt!("apple,tunable-{name}"))?;
            let node = self.dev.fwnode().ok_or(EIO)?;
            if !node.property_present(&property) {
                return Ok((OPC_REGISTER_IOREG | OPC_ACK_MASK) << OPC_SHIFT);
            }
            let len = node.property_count_elem::<u8>(&property)?;
            if len > value_buf.size() || len > u32::MAX as usize {
                return Err(EINVAL);
            }
            let value = node
                .property_read_array_vec::<u8>(&property, len)?
                .required_by(&self.dev)?;
            // SAFETY: This is the negotiated shared value buffer; firmware
            // consumes the supplied bytes only after our acknowledgement.
            unsafe { value_buf.as_mut()[..len].copy_from_slice(&value) };
            value
        } else {
            let len = descriptor.size as usize;
            if len > value_buf.size() {
                return Err(EINVAL);
            }
            // SAFETY: As above, firmware waits for the acknowledgement before
            // reusing the shared value buffer.
            let mut value = KVec::with_capacity(len, GFP_KERNEL)?;
            value.extend_from_slice(unsafe { &value_buf.as_ref()[..len] }, GFP_KERNEL)?;
            value
        };
        let size = value.len() as u64;
        if size != 0 {
            state.registry_entries.push(
                RegistryEntry {
                    id: descriptor.id,
                    value,
                },
                GFP_KERNEL,
            )?;
        }
        // The ID came from the firmware descriptor; the reply carries length.
        Ok((OPC_REGISTER_IOREG | OPC_ACK_MASK) << OPC_SHIFT | size)
    }
    fn change_ioreg_v2(&self, id: u16, remove: bool) -> Result<u64> {
        let mut state = self.state.lock();
        let opcode = if remove { 0x35 } else { 0x37 };
        let Some(index) = state.registry_entries.iter().position(|e| e.id == id) else {
            return Ok(opcode << OPC_SHIFT);
        };
        let size = state.registry_entries[index].value.len();
        if remove {
            state.registry_entries.swap_remove(index);
        } else {
            let addr = state.value_buf.ok_or(EINVAL)?;
            let alloc_index = state.find_alloc(addr).ok_or(EINVAL)?;
            let PmpState {
                allocs,
                registry_entries,
                ..
            } = &mut *state;
            let buffer = &allocs[alloc_index].alloc;
            if size > buffer.size() {
                return Err(EINVAL);
            }
            // SAFETY: Firmware waits for this update's reply before reusing
            // the shared buffer, and the stored length is bounds-checked.
            registry_entries[index]
                .value
                .copy_from_slice(unsafe { &buffer.as_ref()[..size] });
        }
        Ok(opcode << OPC_SHIFT | size as u64)
    }
    fn recv_message(&self, msg: u64) -> Result<()> {
        let opc = (msg >> OPC_SHIFT) & 0xFF;
        let reply = match opc {
            OPC_GET_IOVA_TABLE => self.get_iova_table()?,
            OPC_MALLOC => self.malloc(msg & MALLOC_SIZE_MASK)?,
            OPC_FREE => self.free(msg & MSG_IOVA_MASK)?,
            OPC_SET_BUF => self.set_buf(msg & MSG_IOVA_MASK)?,
            OPC_REGISTER_IOREG if self.registry_v2 => {
                self.register_ioreg_v2(msg & MSG_IOVA_MASK)?
            }
            OPC_REGISTER_IOREG => self.register_ioreg(msg & MSG_IOVA_MASK)?,
            OPC_SET_IOREG if self.registry_v2 => self.change_ioreg_v2(msg as u16, true)?,
            OPC_SET_IOREG => self.set_ioreg(msg & SET_IOREG_INDEX_MASK)?,
            OPC_UPDATE_IOREG_V2 if self.registry_v2 => self.change_ioreg_v2(msg as u16, false)?,
            _ => {
                dev_err!(self.dev, "Got unknown message {}", msg);
                return Err(EIO);
            }
        };
        if self.registry_v2 {
            let guard = self.registry_rtkit.lock();
            let raw = guard.ok_or(ENODEV)? as *mut bindings::apple_rtkit;
            // SAFETY: The driver's owner drains C RX before releasing this
            // context; C mailbox TX is internally serialized.
            return unsafe {
                kernel::error::to_result(bindings::apple_rtkit_send_message(
                    raw,
                    PMP_ENDPOINT,
                    reply,
                    core::ptr::null_mut(),
                    false,
                ))
            };
        }
        let mut rtk_guard = self.rtkit.lock();
        let rtk = rtk_guard.as_mut().as_pin_mut().unwrap();
        rtk.send_message(PMP_ENDPOINT, reply)?;
        Ok(())
    }
}

unsafe impl Send for PmpData {}
unsafe impl Sync for PmpData {}

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
impl rtkit::Operations for PmpData {
    type Data = Arc<PmpData>;
    type Buffer = NoBuffer;

    fn recv_message(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, _ep: u8, msg: u64) {
        let ret = data.recv_message(msg);
        if let Err(e) = ret {
            dev_err!(data.dev, "Failed to handle rtkit message, error: {:?}", e);
        }
    }

    fn crashed(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, _crashlog: Option<&[u8]>) {
        dev_err!(data.dev, "PMP firmware crashed");
    }
}

struct PmpReboot {
    block: bindings::notifier_block,
    data: Arc<PmpData>,
}

unsafe impl Send for PmpReboot {}

unsafe extern "C" fn pmp_reboot(
    block: *mut bindings::notifier_block,
    _event: usize,
    _arg: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    // SAFETY: This exact stable KBox block is registered while its driver
    // owns RtKit. Unregister drains callbacks before that owner is dropped.
    let state = unsafe { &*kernel::container_of!(block, PmpReboot, block) };
    match state.data.stop_context() {
        Ok(()) => bindings::NOTIFY_OK as i32,
        Err(e) => {
            dev_err!(state.data.dev, "PMP reboot stop failed: {:?}", e);
            bindings::NOTIFY_BAD as i32
        }
    }
}

#[allow(dead_code)]
struct PmpDriver {
    reboot: Option<PmpRebootRegistration>,
    // Field order drains RX before releasing the context's final Arc.
    registry_rtkit: Option<rtkit::RtKit<PmpData>>,
    data: Arc<PmpData>,
}

impl Drop for PmpDriver {
    fn drop(&mut self) {
        if self.registry_rtkit.is_some() {
            // Also covers failure to allocate the core's driver-data box
            // after probe constructed this owner and started firmware.
            if let Err(e) = self.data.stop_context() {
                dev_err!(
                    self.data.dev,
                    "PMP stop failed; retaining DMA until platform reset: {:?}",
                    e
                );
                self.registry_rtkit
                    .as_mut()
                    .unwrap()
                    .retain_shared_buffers_on_drop();
                core::mem::forget(self.data.clone());
            }
        }
    }
}

struct PmpRebootRegistration(KBox<PmpReboot>);

impl Drop for PmpRebootRegistration {
    fn drop(&mut self) {
        // Unregister holds no stop/reply mutex. The chain joins callbacks
        // before the boxed block and its context reference are freed.
        let ret = unsafe { bindings::unregister_reboot_notifier(&mut self.0.block) };
        assert_eq!(ret, 0, "PMP notifier could not be drained");
    }
}

/// Per-SoC configuration.
struct PmpHwConfig {
    /// DMA mask to set before the firmware allocates anything, or `None` to
    /// keep the default.
    dma_mask: Option<DmaMask>,
    registry_v2: bool,
}

const HW_CFG_DEFAULT: PmpHwConfig = PmpHwConfig {
    dma_mask: None,
    registry_v2: false,
};

/// T8140: the DART translates 42 bits and its DMA window starts at 1 TiB.
const HW_CFG_T8140: PmpHwConfig = PmpHwConfig {
    dma_mask: Some(DmaMask::new::<42>()),
    registry_v2: false,
};

const HW_CFG_T6050: PmpHwConfig = PmpHwConfig {
    dma_mask: Some(DmaMask::new::<48>()),
    registry_v2: true,
};

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <PmpDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c"apple,t6000-pmp-v2"), &HW_CFG_DEFAULT),
        (of::DeviceId::new(c"apple,t8140-pmp-v2"), &HW_CFG_T8140),
        (of::DeviceId::new(c"apple,t6050-pmp-v2"), &HW_CFG_T6050),
    ]
);

impl platform::Driver for PmpDriver {
    type IdInfo = &'static PmpHwConfig;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let dev: ARef<device::Device> = pdev.as_ref().into();
        let cfg = info.ok_or(ENODEV)?;
        let registry_v2 = cfg.registry_v2;
        if let Some(mask) = cfg.dma_mask {
            // SAFETY: No allocations, mappings or firmware execution exist yet.
            unsafe { pdev.dma_set_mask_and_coherent(mask)? };
        }
        if registry_v2 {
            // Deferred-probe timeout can otherwise allow DMA to fall back to
            // physical addresses when the required DART provider failed.
            // SAFETY: The platform device is live throughout probe.
            if unsafe { bindings::iommu_get_domain_for_dev(dev.as_raw()) }.is_null() {
                dev_err!(
                    dev,
                    "PMP requires an attached IOMMU domain before CPU start"
                );
                return Err(EPROBE_DEFER);
            }
            // PMP messages carry a 48-bit DMA address. The inherited DART
            // aperture starts above 32 bits; the IOMMU constrains this mask to
            // that aperture. Configure before starting any RTKit allocations.
            // SAFETY: Probe has not published data or started firmware/DMA.
        }
        let data = PmpData::new(pdev, registry_v2)?;
        if registry_v2 {
            data.cold_preflight(pdev)?;
        }
        let node = dev.fwnode().ok_or(EIO)?;
        let dvid = node
            .property_read(c"apple,dram-vendor-id")
            .required_by(&dev)?;
        let bdid = node.property_read(c"apple,board-id").required_by(&dev)?;
        let mut patches = KVec::with_capacity(9, GFP_KERNEL)?;
        patches.push((from_fourcc(b"BDID"), bdid), GFP_KERNEL)?;
        patches.push((from_fourcc(b"DVID"), dvid), GFP_KERNEL)?;
        // Optional values are supplied by the bootloader's board profile. Do
        // not replace firmware defaults when a property is absent.
        for (property, key) in [
            (c"apple,dram-capacity", b"DCAP"),
            (c"apple,dram-channel-disable", b"DCHD"),
            (c"apple,pmc", b"PMC_"),
            (c"apple,pmc-msg-disabled", b"PMCX"),
            (c"apple,soc-chip-variant", b"CVAR"),
        ] {
            // Preserve the existing T6000 boot profile. The extra board/PMC
            // fields belong to the registry-v2 firmware ABI.
            if !registry_v2 && key != b"DCAP" {
                continue;
            }
            if node.property_present(property) {
                let value = node.property_read::<u32>(property).required_by(&dev)?;
                patches.push((from_fourcc(key), value), GFP_KERNEL)?;
            }
        }
        if registry_v2 && node.property_present(c"apple,pmc-pmgr") {
            let value = node
                .property_read::<u32>(c"apple,pmc-pmgr")
                .required_by(&dev)?;
            patches.push((from_fourcc(b"PMCV"), value & 1), GFP_KERNEL)?;
            patches.push((from_fourcc(b"PMCB"), (value >> 3) & 1), GFP_KERNEL)?;
        }
        data.patch_bootargs(pdev, &patches)?;
        let rtkit = rtkit::RtKit::<PmpData>::new(&dev, None, 0, data.clone())?;
        if registry_v2 {
            *data.registry_rtkit.lock() = Some(rtkit.as_raw() as usize);
            let mut owner = PmpDriver {
                reboot: None,
                registry_rtkit: Some(rtkit),
                data,
            };
            owner.data.start_cpu(pdev)?;
            owner.data.start()?;
            let mut reboot = KBox::new(
                PmpReboot {
                    block: bindings::notifier_block {
                        notifier_call: Some(pmp_reboot),
                        next: core::ptr::null_mut(),
                        priority: -10,
                    },
                    data: owner.data.clone(),
                },
                GFP_KERNEL,
            )?;
            // SAFETY: The boxed notifier has a stable address until its
            // registration guard unregisters and drains the callback chain.
            let ret = unsafe { bindings::register_reboot_notifier(&mut reboot.block) };
            if ret != 0 {
                return Err(Error::from_errno(ret));
            }
            owner.reboot = Some(PmpRebootRegistration(reboot));
            return Ok(owner);
        }
        *data.rtkit.lock() = Some(rtkit);
        data.start_cpu(pdev)?;
        data.start()?;
        Ok(PmpDriver {
            reboot: None,
            registry_rtkit: None,
            data,
        })
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        if this.data.registry_v2 {
            if let Err(e) = this.data.stop_context() {
                dev_err!(this.data.dev, "PMP unbind stop failed: {:?}", e);
            }
        }
    }
}

module_platform_driver! {
    type: PmpDriver,
    name: "apple_pmp",
    description: "Apple Power Management Processor",
    license: "GPL",
}
