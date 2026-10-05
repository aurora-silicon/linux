// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The per-SoC facts of the M3 runtime backend.
//!
//! The runtime admits, identifies and configures an M3-family GPU from one of these tables. Every
//! value in a table comes from that SoC's device tree (as the boot loader fills it from its ADT),
//! from its hardware, or from existing driver code for that SoC; no value of one SoC stands in for
//! another's.

use crate::{hw, m3_board::KnownImage, m3_firmware::Layout};

/// The SGX identification words the runtime admits, each compared under its mask.
pub(crate) struct IdWords {
    /// SGX+0xd04000: family [31:24], variant [23:16], revision [15:8].
    pub(crate) version: u32,
    pub(crate) version_mask: u32,
    /// SGX+0xd04010: dies [19:16], clusters per die [15:8].
    pub(crate) counts: u32,
    pub(crate) counts_mask: u32,
}

/// One firmware IO mapping of the runtime's InitData: HwDataB slot, physical address, total
/// size, element size, writable.
pub(crate) type IoMapping = (usize, u64, u32, u32, bool);

/// The M3 runtime's facts for one SoC.
pub(crate) struct Soc {
    /// SoC name, for logs.
    pub(crate) name: &'static str,
    /// GPU name, for logs.
    pub(crate) gpu_name: &'static str,
    pub(crate) chip_id: u32,
    pub(crate) gpu_variant: hw::GpuVariant,
    pub(crate) gpu_revision: hw::GpuRevision,
    pub(crate) gpu_revision_id: hw::GpuRevisionID,
    /// Root node compatible of the SoC.
    pub(crate) board: &'static str,
    /// GPU node compatible.
    pub(crate) gpu: &'static str,
    /// Boards the runtime starts on by default and registers a render node on.
    pub(crate) validated_boards: &'static [&'static [u8]],
    /// GPU clusters, and core slots per cluster.
    pub(crate) clusters: u32,
    pub(crate) cores_per_cluster: u32,
    /// Accepted compatible lists of the GPU coprocessor mailbox.
    pub(crate) mailbox_compatibles: &'static [&'static [u8]],
    /// The mailbox interrupts: send-empty, send-not-empty, recv-empty, recv-not-empty.
    pub(crate) mailbox_interrupts: [u32; 12],
    /// The identification words the runtime admits.
    pub(crate) id: IdWords,
    /// GPU firmware images the runtime can identify.
    pub(crate) images: &'static [KnownImage],
    /// The layout of the loaded firmware the runtime accepts.
    pub(crate) firmware: Option<&'static Layout>,
    /// The hardware configuration of the InitData.
    pub(crate) hwcfg: Option<&'static hw::HwConfig>,
    /// The firmware IO mappings of the InitData.
    pub(crate) io_mappings: Option<&'static [IoMapping]>,
    /// The SGX write (offset, value) made after the identity checks, before the firmware starts.
    pub(crate) sgx_setup: Option<(usize, u32)>,
}

/// T6030 (M3 Pro, G15S): one die, two clusters of ten core slots.
pub(crate) static T6030: Soc = Soc {
    name: "T6030",
    gpu_name: "G15S",
    chip_id: 0x6030,
    gpu_variant: hw::GpuVariant::S,
    gpu_revision: hw::GpuRevision::B1,
    gpu_revision_id: hw::GpuRevisionID::B1,
    board: "apple,t6030",
    gpu: "apple,agx-t6030",
    // The M3 Pro MacBook Pros (14" J514S, 16" J516S).
    validated_boards: &[b"apple,j514s", b"apple,j516s"],
    clusters: 2,
    cores_per_cluster: 10,
    mailbox_compatibles: &[
        b"apple,t6030-asc-mailbox\0apple,asc-mailbox-v4\0",
        b"apple,t6030-agx-asc-mailbox\0",
    ],
    mailbox_interrupts: [0, 832, 4, 0, 833, 4, 0, 834, 4, 0, 835, 4],
    id: IdWords {
        version: 0x0703_1100,
        version_mask: !0,
        counts: 0x0011_0209,
        counts_mask: !0,
    },
    images: &crate::m3_board::KNOWN_IMAGES,
    firmware: Some(&crate::m3_firmware::T6030_LAYOUT),
    hwcfg: Some(&hw::t6030::HWCONFIG_T6030),
    io_mappings: Some(&crate::m3_adt_config::T6030_IO_MAPPINGS),
    sgx_setup: Some((0xd14000, 0x70001)),
};
