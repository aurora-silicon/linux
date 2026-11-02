// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G17 firmware builds.
//!
//! Firmware builds that share the interface version ([`super::initdata::INTERFACE_VERSION`])
//! still differ in a few shared layouts and queue rules. A [`Variant`] names those differences
//! for one build; each SoC's [`crate::g17::Config`] points at the variant of the firmware it
//! runs.

/// Where a firmware build keeps the host words of a queue's pointer block
/// ([`super::queue::PointerBlock`]).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct PointerLayout {
    /// Offset of the item-ring producer.
    pub(crate) producer: usize,
    /// Offset of the word the host copies the producer to before it acknowledges a recovery,
    /// if the build has one.
    pub(crate) recovery_target: Option<usize>,
}

/// How a firmware build acknowledges the initialization root the host hands it.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum InitAck {
    /// With an `INITDATA_ACK` message on the firmware endpoint.
    Message,
    /// By setting word `+0x14` of the primary status A to 1; it sends no message.
    Status,
}

/// The initialization graph of a firmware build.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum InitGraph {
    /// Two instances, as the A18 Pro firmware (26A428) runs.
    G17P,
    /// One instance carrying both roles, with the 25G83 layouts ([`super::initdata::v25g83`]).
    V25G83,
    /// Two instances, as the M5 firmware (26A428) runs: a different interface word, policy,
    /// hardware-data and allocation layout ([`super::m5::initdata`]).
    M5,
}

/// How the host makes published compute work runnable.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Submission {
    /// The work ring and its doorbell; the firmware wakes the cores for the work the ring
    /// names. DRM compute and render submission use it.
    Doorbell,
    /// The work ring and its doorbell, then the M5 host command port (SGX `0x4090`/`0x4098`)
    /// that makes the queue runnable (`Port::publish_m5_compute`). Only the arithmetic
    /// qualification submits this way; DRM submission is not supported yet, so the profile
    /// advertises no submission features.
    M5HostCommand,
}

/// The layouts and rules of one G17 firmware build.
pub(crate) struct Variant {
    /// The initialization graph.
    pub(crate) graph: InitGraph,
    /// How the firmware acknowledges its initialization.
    pub(crate) init_ack: InitAck,
    /// The host ends the power-on transition with `POWER_ON_DONE` once every coprocessor
    /// is initialized.
    pub(crate) power_on_done: bool,
    /// First QID the host may use; the firmware keeps the lower ones for itself.
    pub(crate) qid_floor: usize,
    /// Host words of a queue's pointer block.
    pub(crate) pointers: PointerLayout,
    /// The host registers each render pair through the GPU's MMIO queue ports.
    pub(crate) mmio_pair_registration: bool,
    /// Render kick entries may name an MCache table.
    pub(crate) mcache: bool,
    /// Renders may set the few-primitives auxiliary framebuffer flag.
    pub(crate) few_primitives: bool,
    /// The firmware runs ray-traversal work (ray queries and intersection programs).
    pub(crate) ray_traversal: bool,
    /// Tiling descriptors report whether the fragment queue still had earlier kicks
    /// outstanding (`fragment_busy`).
    pub(crate) fragment_busy: bool,
    /// Render item rings carry lead commands: InitBufferManager before the first tiling pass
    /// of a buffer manager.
    pub(crate) lead_commands: bool,
    /// Register programs of the render descriptors.
    pub(crate) programs: super::render::RegisterPrograms,
    /// How published compute work becomes runnable.
    pub(crate) submission: Submission,
    /// The performance effort the host publishes in Region C while submitted work streams
    /// (`power::Effort`); idle publishes zero.
    pub(crate) effort: u32,
    /// Where the driver places its objects and what a client VM reserves.
    pub(crate) vm_layout: &'static crate::g17::vm_layout::VmLayout,
}

/// A firmware build a SoC supports, identified by its loaded TEXT.
pub(crate) struct Build {
    /// macOS version (major, minor, patch), used to order image candidates.
    pub(crate) os_version: [u32; 3],
    /// Name, for the log.
    pub(crate) name: &'static str,
    /// The admitted loaded images, measured from the `fw-text` region.
    pub(crate) images: &'static [crate::fwid::FirmwareBuild],
    /// Layouts and rules of the build.
    pub(crate) variant: &'static Variant,
}

/// How a SoC's firmware build is determined.
pub(crate) enum Firmware {
    /// The SoC runs a single build, which is not identified (A18 Pro).
    Fixed(&'static Variant),
    /// One of these exact loaded images. The loader's `apple,firmware-version` is only
    /// a candidate-ordering hint because multiple OS releases can share an iBoot string.
    Identified(&'static [Build]),
}

/// The A18 Pro firmware (26A428).
pub(crate) const V26A428: Variant = Variant {
    graph: InitGraph::G17P,
    init_ack: InitAck::Message,
    power_on_done: true,
    qid_floor: 0,
    pointers: PointerLayout {
        producer: 0x40,
        recovery_target: Some(0x50),
    },
    mmio_pair_registration: true,
    mcache: true,
    few_primitives: true,
    ray_traversal: true,
    fragment_busy: true,
    lead_commands: false,
    programs: super::render::RegisterPrograms::G17P,
    submission: Submission::Doorbell,
    effort: crate::g17::power::FULL_EFFORT,
    vm_layout: &crate::hw::t8140::dynamic::LAYOUT,
};

/// The M4 Pro firmware (25G83, `mBoot-18000.161.10`).
pub(crate) const V25G83: Variant = Variant {
    graph: InitGraph::V25G83,
    init_ack: InitAck::Status,
    // Never sent on the M4 Pro: the firmware has retired its opening record when it accepts
    // its initialization.
    power_on_done: false,
    // QIDs below 32 belong to the firmware's bootstrap protocol.
    qid_floor: 32,
    pointers: PointerLayout {
        producer: 0x50,
        recovery_target: None,
    },
    // Whether this build tolerates the MMIO registration is unknown; the M4 Pro has only run
    // without it.
    mmio_pair_registration: false,
    mcache: false,
    // A fragment stage with the few-primitives flag hangs.
    few_primitives: false,
    // A stalled ray traversal wedges the GPU without a firmware recovery.
    ray_traversal: false,
    // When a tiling pass runs out of memory with this set, the firmware resumes it without
    // adding memory and the tiler stops silently; macOS always writes 0.
    fragment_busy: false,
    lead_commands: true,
    programs: super::render::RegisterPrograms::V25G83,
    submission: Submission::Doorbell,
    effort: crate::g17::power::FULL_EFFORT,
    vm_layout: &crate::hw::t8140::dynamic::LAYOUT,
};

/// The M5 firmware (26A428, macOS 27.0), native. Every field is this build's own: the A18 Pro
/// build shares its firmware version but not its submission rules. Only the bootstrap graph
/// and the arithmetic qualification run on it; DRM submission is not supported yet.
pub(crate) const V26A428_M5: Variant = Variant {
    graph: InitGraph::M5,
    init_ack: InitAck::Message,
    power_on_done: true,
    // The qualification's QID 11 is the host's; no lower firmware-owned range is known.
    qid_floor: 0,
    // As the A18 Pro's; the qualification's inner cursors follow this producer.
    pointers: PointerLayout {
        producer: 0x40,
        recovery_target: Some(0x50),
    },
    mmio_pair_registration: false,
    // No M5 render path exists, so none of its features do.
    mcache: false,
    few_primitives: false,
    ray_traversal: false,
    fragment_busy: false,
    lead_commands: false,
    // A placeholder: M5 render register programs are not established, and no M5 render
    // descriptor is built.
    programs: super::render::RegisterPrograms::G17P,
    submission: Submission::M5HostCommand,
    // linux-aurora publishes zero on M5 (its submission_effort), leaving performance to the
    // firmware's own policy.
    effort: 0,
    // Mesa's M5 windows, not the A18 Pro's arenas, which lie in Mesa's read-only shadow.
    vm_layout: &crate::g17::m5_vm::LAYOUT,
};

const _: () = {
    assert!(super::queue::PointerBlock::valid_layout(&V26A428.pointers));
    assert!(super::queue::PointerBlock::valid_layout(&V25G83.pointers));
};
