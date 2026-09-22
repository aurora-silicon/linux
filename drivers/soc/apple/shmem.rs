// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use kernel::device;
use kernel::dma;
use kernel::platform;
use kernel::prelude::*;

const ENTRY_SIZE: usize = 16;
const ENTRY_OFF_FOURCC: usize = 0;
const ENTRY_OFF_SIZE: usize = 4;
const ENTRY_OFF_OFFSET: usize = 8;

// Buffer-format constant, not PAGE_SIZE despite the coincidence.
const PAYLOAD_BASE: usize = 0x4000;
const PAYLOAD_ALIGN: usize = 0x4000;

const CINP_MIN_SIZE: usize = 0x8000;

const CINP_PAYLOAD: [u8; 1] = [0];

// Wire byte order, not byte-reversed; llun is the terminator spelling. The
// first item's fourcc is per-SoC and comes from the platform profile.
const FOURCC_OPLA: &[u8; 4] = b"OPLA";
const FOURCC_IPIS: &[u8; 4] = b"IPIS";
const FOURCC_TERM: &[u8; 4] = b"llun";

const PROP_LOCAL_POLICY: &CStr = c"local-policy-manifest";
const PROP_IBOOT: &CStr = c"iboot-manifest";

pub(crate) type ShMem = dma::Coherent<[u8]>;

#[derive(Clone, Copy)]
pub(crate) struct Region {
    pub(crate) offset: usize,
    // Entry size field = allocation size, not payload length.
    pub(crate) size: usize,
    pub(crate) payload_len: usize,
}

impl Region {
    fn place(offset: usize, payload_len: usize, min_size: usize) -> Region {
        let mut size = align_up(payload_len + 4, PAYLOAD_ALIGN);
        if size < min_size {
            size = min_size;
        }
        Region {
            offset,
            size,
            payload_len,
        }
    }

    fn end(&self) -> usize {
        self.offset + self.size
    }
}

pub(crate) struct Shmem {
    pub(crate) buf: ShMem,
}

const fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

fn write_at(buf: &mut ShMem, off: usize, src: &[u8]) -> Result<()> {
    let end = off.checked_add(src.len()).ok_or(EINVAL)?;
    if end > buf.len() {
        return Err(EINVAL);
    }
    // SAFETY: runs in probe before the SEP is told the buffer exists, and probe
    // is the only writer, so nothing else accesses it.
    unsafe {
        buf.as_mut()[off..end].copy_from_slice(src);
    }
    Ok(())
}

fn write_entry(
    buf: &mut ShMem,
    index: usize,
    fourcc: &[u8; 4],
    size: usize,
    offset: usize,
) -> Result<()> {
    let base = index * ENTRY_SIZE;
    if base + ENTRY_SIZE > PAYLOAD_BASE {
        return Err(EINVAL);
    }
    write_at(buf, base + ENTRY_OFF_FOURCC, fourcc)?;
    let size32: u32 = size.try_into().map_err(|_| EINVAL)?;
    write_at(buf, base + ENTRY_OFF_SIZE, &size32.to_le_bytes())?;
    // u32 offset at byte 8 (written as u64 with zero pad), never byte 12.
    let offset64: u64 = offset.try_into().map_err(|_| EINVAL)?;
    write_at(buf, base + ENTRY_OFF_OFFSET, &offset64.to_le_bytes())?;
    Ok(())
}

fn read_blob(dev: &device::Device, name: &CStr) -> Result<KVec<u8>> {
    let fwnode = dev.fwnode().ok_or(EIO)?;
    let len = fwnode.property_count_elem::<u8>(name)?;
    if len == 0 {
        return Err(ENODATA);
    }
    fwnode
        .property_read_array_vec::<u8>(name, len)?
        .required_by(dev)
}

fn read_manifest(dev: &device::Device<device::Core>, name: &CStr) -> Result<KVec<u8>> {
    match read_blob(dev, name) {
        Ok(v) => Ok(v),
        Err(e) => {
            dev_err!(
                dev,
                "refusing to attach: device-tree property '{}' missing or unreadable ({:?}); a CINP-only table would burn the one-shot registration and fault\n",
                name,
                e
            );
            Err(e)
        }
    }
}

fn verify_layout(
    dev: &device::Device<device::Core>,
    cinp: &Region,
    opla: &Region,
    ipis: &Region,
    used: usize,
    capacity: usize,
    first_item: &[u8; 4],
) -> Result<()> {
    let regions = [
        (first_item, cinp),
        (FOURCC_OPLA, opla),
        (FOURCC_IPIS, ipis),
    ];

    for (name, r) in regions {
        let bad = r.offset < PAYLOAD_BASE
            || r.offset % PAYLOAD_ALIGN != 0
            || r.size % PAYLOAD_ALIGN != 0
            || r.size < r.payload_len + 4
            || r.end() > capacity;
        if bad {
            dev_err!(
                dev,
                "refusing to attach: item '{}' region is malformed (offset 0x{:x}, size 0x{:x}, payload {} bytes)\n",
                core::str::from_utf8(name).unwrap_or("????"),
                r.offset,
                r.size,
                r.payload_len
            );
            return Err(EINVAL);
        }
    }

    if cinp.offset != PAYLOAD_BASE
        || opla.offset != cinp.end()
        || ipis.offset != opla.end()
        || used != ipis.end()
    {
        dev_err!(
            dev,
            "refusing to attach: regions do not tile from 0x{:x} (CINP 0x{:x}+0x{:x}, OPLA 0x{:x}+0x{:x}, IPIS 0x{:x}+0x{:x}, used 0x{:x})\n",
            PAYLOAD_BASE,
            cinp.offset,
            cinp.size,
            opla.offset,
            opla.size,
            ipis.offset,
            ipis.size,
            used
        );
        return Err(EINVAL);
    }

    if cinp.size < CINP_MIN_SIZE {
        return Err(EINVAL);
    }

    if 4 * ENTRY_SIZE > PAYLOAD_BASE || used > capacity {
        return Err(ENOSPC);
    }

    Ok(())
}

pub(crate) fn build(
    pdev: &platform::Device<device::Core>,
    capacity: usize,
    first_item: &[u8; 4],
) -> Result<Shmem> {
    let dev: &device::Device<device::Core> = pdev.as_ref();

    // Read manifests before allocating: a CINP-only registration would burn the one-shot and fault.
    let opla_blob = read_manifest(dev, PROP_LOCAL_POLICY)?;
    let ipis_blob = read_manifest(dev, PROP_IBOOT)?;

    let cinp = Region::place(PAYLOAD_BASE, CINP_PAYLOAD.len(), CINP_MIN_SIZE);
    let opla = Region::place(cinp.end(), opla_blob.len(), 0);
    let ipis = Region::place(opla.end(), ipis_blob.len(), 0);
    let used = ipis.end();

    verify_layout(dev, &cinp, &opla, &ipis, used, capacity, first_item)?;

    let mut buf = dma::Coherent::<u8>::zeroed_slice(dev, capacity, GFP_KERNEL)?;

    // Payloads before entries: a failure leaves an all-zero table, not a valid-looking one.
    write_at(
        &mut buf,
        cinp.offset,
        &(cinp.payload_len as u32).to_le_bytes(),
    )?;
    write_at(&mut buf, cinp.offset + 4, &CINP_PAYLOAD)?;

    write_at(
        &mut buf,
        opla.offset,
        &(opla.payload_len as u32).to_le_bytes(),
    )?;
    write_at(&mut buf, opla.offset + 4, &opla_blob)?;

    write_at(
        &mut buf,
        ipis.offset,
        &(ipis.payload_len as u32).to_le_bytes(),
    )?;
    write_at(&mut buf, ipis.offset + 4, &ipis_blob)?;

    write_entry(&mut buf, 0, first_item, cinp.size, cinp.offset)?;
    write_entry(&mut buf, 1, FOURCC_OPLA, opla.size, opla.offset)?;
    write_entry(&mut buf, 2, FOURCC_IPIS, ipis.size, ipis.offset)?;
    write_entry(&mut buf, 3, FOURCC_TERM, 0, 0)?;

    Ok(Shmem { buf })
}
