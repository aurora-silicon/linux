// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned J713 initdata: 31 inventory objects, 12 submit channel pairs and the
//! exact MMIO register map, in the layout of the admitted firmware's ABI. All bytes are
//! encoded in fresh GEM backing before the boot owner publishes a root or loader address.
//! Firmware-reachable backing and mappings are retained by `Objects` until reboot.

use kernel::{device, prelude::*, sync::Arc};

use super::adt::GpuAdt;
use super::board;
use super::channel::{Channels, Ring};
use super::fw::initdata::{self as wire, id, ready_control};
use super::hw::{self, FwAbi};
use super::object::{Class, FwObject, Objects, Placement, Request};

/// Firmware graph handles. The small root.Control and large FirmwareData are
/// intentionally separate: only the former carries the READY acknowledgement.
pub(crate) struct InitData {
    /// The firmware ABI these objects were encoded for.
    pub(crate) abi: FwAbi,
    pub(crate) globals: Option<Arc<FwObject>>,
    /// Recovery/scheduler owner: root.FirmwareData, not the small Control.
    pub(crate) control: Option<Arc<FwObject>>,
    pub(crate) ready_control: Option<Arc<FwObject>>,
    #[allow(dead_code)]
    pub(crate) main: Option<Arc<FwObject>>,
    /// Fault/recovery activity; retained under the historical field name.
    #[allow(dead_code)]
    pub(crate) hwdata: Option<Arc<FwObject>>,
    #[allow(dead_code)]
    pub(crate) runtime: Option<Arc<FwObject>>,
    #[allow(dead_code)]
    pub(crate) flags: Option<Arc<FwObject>>,
    pub(crate) ta_activity: Arc<FwObject>,
    pub(crate) fs_activity: Arc<FwObject>,
    pub(crate) compute_activity: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) uma: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) parameter: Arc<FwObject>,
    root: Arc<FwObject>,
    /// Rows of the board's firmware performance table, including off.
    pub(crate) rows: u32,
    /// The register-map records, slot 0 through the highest filled one.
    loader: KVec<u8>,
    /// The complete inventory, including firmware-only log and retention data.
    _graph: KVec<Arc<FwObject>>,
}

impl InitData {
    pub(crate) fn root_va(&self) -> Result<u64> {
        Ok(self.root.va)
    }

    /// Exact Main register-map records, which the boot owner publishes to firmware DATA when
    /// the layout has the host write them.
    pub(crate) fn loader_bytes(&self) -> &[u8] {
        &self.loader
    }

    /// The FirmwareData words of this ABI.
    pub(crate) fn firmware_data(&self) -> &'static wire::FirmwareData {
        &self.abi.initdata().firmware_data
    }

    /// Bring-up diagnostics: the FirmwareData recovery words (phase, generation, the cause word
    /// 0x10 after the phase, the scheduler word) and the 0x130 bytes from 0x100 below the phase
    /// word, which hold the fault record the firmware's recovery path writes before it sets
    /// phase 1 (26B123: +0x509c..+0x50c0, +0x50b0 a u64).
    pub(crate) fn log_firmware_state(&self, dev: &device::Device, why: &str) {
        let fd = self.firmware_data();
        let Some(c) = self.control.as_ref() else {
            dev_err!(dev, "G16 {}: no FirmwareData\n", why);
            return;
        };
        let r32 = |o: usize| c.r32(o).unwrap_or(u32::MAX);
        dev_err!(
            dev,
            "G16 {}: phase {:#x} generation {:#x} cause {:#x} scheduler {:#x} ready {:#x}\n",
            why,
            r32(fd.phase),
            c.r64(fd.generation).unwrap_or(u64::MAX),
            r32(fd.phase + 0x10),
            r32(fd.scheduler),
            self.readiness().unwrap_or(u32::MAX)
        );
        let base = fd.phase - 0x100;
        for row in 0..(0x130 / 0x20) {
            let at = base + row * 0x20;
            dev_err!(
                dev,
                "G16 {}: FD+{:#06x}: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}\n",
                why,
                at,
                r32(at),
                r32(at + 4),
                r32(at + 8),
                r32(at + 12),
                r32(at + 16),
                r32(at + 20),
                r32(at + 24),
                r32(at + 28)
            );
        }
    }

    /// A phase 1 or 3 stop is owned by the M4 recovery handshake.
    pub(crate) fn halted(&self) -> Result<bool> {
        let phase = self
            .control
            .as_ref()
            .ok_or(ENODEV)?
            .r32(self.firmware_data().phase)?;
        Ok(phase == 1 || phase == 3)
    }

    /// Firmware's actual READY acknowledgement in the small root.Control object.
    pub(crate) fn readiness(&self) -> Result<u32> {
        self.ready_control
            .as_ref()
            .ok_or(ENODEV)?
            .r32(ready_control::READY)
    }
}

fn pair(graph: &[Arc<FwObject>], state: usize, ring: usize) -> Ring {
    Ring {
        state: graph[state].clone(),
        ring: graph[ring].clone(),
    }
}

/// Builds the complete unpublished M4 graph. The return shape is shared with
/// the other backend, while all object layouts and policy are M4-specific.
pub(crate) fn build(
    dev: &device::Device,
    adt: &GpuAdt,
    objects: &mut Objects,
) -> Result<(InitData, Channels, u32)> {
    let abi = adt.layout.abi;
    let layout = abi.initdata();
    let performance = board::qualify(dev, adt)?;
    // The top row: S9 on J713, S10 on J773g.
    let ceiling = performance.count as u32 - 1;
    let rows = performance.count as u32;
    let max_khz = performance.frequency[ceiling as usize] * 1000;
    let mut graph = KVec::with_capacity(wire::OBJECT_COUNT, GFP_KERNEL)?;
    for (i, size) in layout.object_size.iter().copied().enumerate() {
        let lower = match i {
            id::UMA => Some(wire::BOOTSTRAP_START),
            id::PARAMETER => Some(wire::BOOTSTRAP_START + 0x8000),
            _ => None,
        };
        // These are the exact cache classes of the tested M4 owner. Main,
        // Runtime and the three engine activities are ASC-cached. Every other
        // inventory object is uncached; the two lower aliases are GPU-shared.
        let class = if lower.is_some() {
            Class::Shared
        } else if matches!(
            i,
            id::RUNTIME
                | id::MAIN
                | id::ACTIVITY_TILER
                | id::ACTIVITY_FRAGMENT
                | id::ACTIVITY_COMPUTE
        ) {
            Class::FwCached
        } else {
            Class::Fw
        };
        graph.push(
            objects.alloc(Request {
                size,
                class,
                placement: Placement::Anywhere,
                offset: 0,
                // Runtime uses these through context 0. No application may write
                // the firmware-global bootstrap allocator or parameter state.
                client: None,
                lower,
                cpu: true,
            })?,
            GFP_KERNEL,
        )?;
    }
    let channels = Channels::alloc_submit_and_attach(
        objects,
        pair(&graph, id::DEVCTRL_STATE, id::DEVCTRL_RING),
        pair(&graph, id::EVENT_STATE, id::EVENT_DATA),
        pair(&graph, id::LOG_STATE, id::LOG_ENTRIES),
        pair(&graph, id::KTRACE_STATE, id::KTRACE_DATA),
        pair(&graph, id::STATISTICS_STATE, id::STATISTICS_DATA),
        // FWCTL state and ring share one allocation (ring starts at+0x40).
        // The host never posts on this firmware-only control channel.
        pair(&graph, id::FWCTL, id::FWCTL),
    )?;
    let mut input = wire::Input {
        performance,
        va: [0; wire::OBJECT_COUNT],
        channel_state: [0; 12],
        channel_ring: [0; 12],
        loader_va: [0; wire::LOADER_COUNT],
        scratch: 0,
        lower_uma: wire::BOOTSTRAP_START,
        lower_parameter: wire::BOOTSTRAP_START + 0x8000,
        ktrace_mask: 0,
        progress_recovery: 1,
    };
    for i in 0..wire::OBJECT_COUNT {
        input.va[i] = graph[i].va;
    }
    for i in 0..12 {
        input.channel_state[i] = channels.submit[i].state.va;
        input.channel_ring[i] = channels.submit[i].ring.va;
    }
    for (slot, entry) in wire::LOADERS.iter().enumerate() {
        if entry.length == 0 {
            continue;
        }
        let offset = entry.pa & (wire::PAGE - 1);
        let at =
            objects.reserve_io((offset + u64::from(entry.length)).next_multiple_of(wire::PAGE))?;
        input.loader_va[slot] = at + offset;
        for bank in 0..if slot == 3 { 2 } else { 1 } {
            let (pa, size, delta) = wire::loader_span(slot, bank).map_err(|()| EINVAL)?;
            objects.map_mmio_class(at + u64::from(delta), pa, u64::from(size), entry.attrs)?;
        }
    }
    input.scratch = objects.reserve_io(0x8000)?;
    // This scratch alias deliberately has firmware RAM attrs, matching the
    // qualified owner, even though the target is an SGX register window.
    objects.map_mmio_class(input.scratch, 0x300d60000, 0x8000, hw::pte::FW)?;

    // Private CPU-side staging keeps the pure encoder independent of raw GEM
    // pointers. No staging references survive publication.
    let mut images = KVec::with_capacity(wire::OBJECT_COUNT, GFP_KERNEL)?;
    for size in layout.object_size {
        images.push(KVVec::from_elem(0u8, size, GFP_KERNEL)?, GFP_KERNEL)?;
    }
    {
        let mut slices = KVec::with_capacity(wire::OBJECT_COUNT, GFP_KERNEL)?;
        for image in images.iter_mut() {
            slices.push(&mut image[..], GFP_KERNEL)?;
        }
        let array: &mut [&mut [u8]; wire::OBJECT_COUNT] =
            (&mut slices[..]).try_into().map_err(|_| EINVAL)?;
        wire::encode_for(layout, array, &input, ceiling).map_err(|()| EINVAL)?;
    }
    let records = layout.loader_records();
    let mut loader = KVec::with_capacity(records * wire::LOADER_STRIDE, GFP_KERNEL)?;
    loader.extend_from_slice(
        images[id::MAIN]
            .get(0x640..0x640 + records * wire::LOADER_STRIDE)
            .ok_or(EINVAL)?,
        GFP_KERNEL,
    )?;
    // Host startup policy: Main and PowerPerformance contain the top row, but the
    // published selectors remain S1 until the thermal owner admits a raise.
    for at in layout.pstate_selectors {
        images[id::GLOBALS][at..at + 4].copy_from_slice(&100u32.to_le_bytes());
    }
    for i in 0..wire::OBJECT_COUNT {
        graph[i].write(0, &images[i])?;
    }

    dev_info!(
        dev,
        "M4 initdata ({}): 31 objects, 12 submit channel pairs, {} register-map slots, S1 startup (S{} tables)\n",
        layout.name,
        records,
        ceiling
    );
    Ok((
        InitData {
            abi,
            globals: Some(graph[id::GLOBALS].clone()),
            control: Some(graph[id::FIRMWARE_DATA].clone()),
            ready_control: Some(graph[id::CONTROL].clone()),
            main: Some(graph[id::MAIN].clone()),
            hwdata: Some(graph[id::ACTIVITY_RECOVERY].clone()),
            runtime: Some(graph[id::RUNTIME].clone()),
            flags: Some(graph[id::CONTROL].clone()),
            ta_activity: graph[id::ACTIVITY_TILER].clone(),
            fs_activity: graph[id::ACTIVITY_FRAGMENT].clone(),
            compute_activity: graph[id::ACTIVITY_COMPUTE].clone(),
            uma: graph[id::UMA].clone(),
            parameter: graph[id::PARAMETER].clone(),
            root: graph[id::ROOT].clone(),
            rows,
            loader,
            _graph: graph,
        },
        channels,
        max_khz,
    ))
}
