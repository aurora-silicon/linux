// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned J713 initdata: 31 inventory objects, 12 submit channel pairs and the
//! exact 42-entry MMIO loader. All bytes are encoded in fresh GEM backing before
//! the boot owner publishes a root or loader address. Firmware-reachable
//! backing and mappings are retained by `Objects` until reboot.

use kernel::{device, prelude::*, sync::Arc};

use super::adt::GpuAdt;
use super::board;
use super::channel::{Channels, Ring};
use super::fw::initdata::{self as wire, control, globals, id, ready_control};
use super::hw;
use super::object::{Class, FwObject, Objects, Placement, Request};

/// Firmware graph handles. The small root.Control and large FirmwareData are
/// intentionally separate: only the former carries the READY acknowledgement.
pub(crate) struct InitData {
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
    loader: [u8; wire::LOADER_BYTES],
    /// The complete inventory, including firmware-only log and retention data.
    _graph: KVec<Arc<FwObject>>,
}

impl InitData {
    pub(crate) fn root_va(&self) -> Result<u64> {
        Ok(self.root.va)
    }

    /// Exact Main loader records which the boot owner publishes to firmware DATA.
    pub(crate) fn loader_bytes(&self) -> &[u8; wire::LOADER_BYTES] {
        &self.loader
    }

    /// A phase 1 or 3 stop is owned by the M4 recovery handshake.
    pub(crate) fn halted(&self) -> Result<bool> {
        let phase = self.control.as_ref().ok_or(ENODEV)?.r32(control::PHASE)?;
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
    let performance = board::qualify(dev, adt)?;
    let max_khz = performance.frequency[wire::PERF_STATES - 1] * 1000;
    let mut graph = KVec::with_capacity(wire::OBJECT_COUNT, GFP_KERNEL)?;
    for (i, size) in wire::OBJECT_SIZE.iter().copied().enumerate() {
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
    for size in wire::OBJECT_SIZE {
        images.push(KVVec::from_elem(0u8, size, GFP_KERNEL)?, GFP_KERNEL)?;
    }
    {
        let mut slices = KVec::with_capacity(wire::OBJECT_COUNT, GFP_KERNEL)?;
        for image in images.iter_mut() {
            slices.push(&mut image[..], GFP_KERNEL)?;
        }
        let array: &mut [&mut [u8]; wire::OBJECT_COUNT] =
            (&mut slices[..]).try_into().map_err(|_| EINVAL)?;
        wire::encode(array, &input, 9).map_err(|()| EINVAL)?;
    }
    let mut loader = [0u8; wire::LOADER_BYTES];
    loader.copy_from_slice(&images[id::MAIN][0x640..0x640 + wire::LOADER_BYTES]);
    // Host startup policy: Main and PowerPerformance contain S9, but the
    // published selectors remain S1 until the thermal owner admits a raise.
    for at in globals::PSTATE_SELECTORS {
        images[id::GLOBALS][at..at + 4].copy_from_slice(&100u32.to_le_bytes());
    }
    for i in 0..wire::OBJECT_COUNT {
        graph[i].write(0, &images[i])?;
    }

    dev_info!(dev, "M4 initdata: 31 objects, 12 submit channel pairs, 42 loader records, S1 startup (S9 tables)\n");
    Ok((
        InitData {
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
            loader,
            _graph: graph,
        },
        channels,
        max_khz,
    ))
}
