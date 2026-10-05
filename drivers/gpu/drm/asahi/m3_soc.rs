// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The per-SoC facts of the M3 runtime backend.
//!
//! The runtime admits, identifies and configures an M3-family GPU from one of these tables. Every
//! value in a table comes from that SoC's device tree (as the boot loader fills it from its ADT),
//! from its hardware, or from existing driver code for that SoC; no value of one SoC stands in for
//! another's. A fact a SoC has no source for yet is `None`, or listed in `unported` when the code
//! that uses it is still written for another SoC, and the runtime then refuses that SoC right
//! after admission ([`Soc::require_complete`]), before it reads the firmware, maps a GPU register
//! or starts the GPU coprocessor.

use kernel::{device, prelude::*};

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
    /// Boards the runtime starts on by default and registers a render node on. On a T8122 it
    /// starts on no other board, whatever `asahi.m3_backend` says (`m3_params::t8122_backend`).
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
    /// Whether the power-management coefficients, the leakage coefficients and the operating
    /// points are this machine's, added by the boot loader from its ADT, rather than static
    /// device-tree values. Admission then requires every one of them (see
    /// `m3_board::check_boot_loader_power`), with no driver default standing in.
    pub(crate) power_from_boot_loader: bool,
    /// Code the runtime runs after admission that still holds another SoC's values.
    pub(crate) unported: &'static [&'static str],
}

impl Soc {
    /// Log every fact this table has no source for, and refuse the SoC if there is one.
    pub(crate) fn require_complete(&self, dev: &device::Device) -> Result {
        let mut missing = 0u32;
        let mut note = |what: &str| {
            dev_info!(dev, "M3 {}: no source yet for {}\n", self.gpu_name, what);
            missing += 1;
        };
        if self.images.is_empty() {
            note("the identity of the loaded GPU firmware image and its InitData version");
        }
        if self.firmware.is_none() {
            note("the layout of the loaded GPU firmware (segment sizes and VAs)");
        }
        if self.hwcfg.is_none() {
            note("the hardware configuration of the InitData (HwConfig)");
        }
        if self.io_mappings.is_none() {
            note("the firmware IO mappings of the InitData");
        }
        if self.sgx_setup.is_none() {
            note("the SGX setup write made before the firmware starts");
        }
        for &what in self.unported {
            note(what);
        }
        if missing == 0 {
            return Ok(());
        }
        dev_err!(
            dev,
            "M3 {}: {} facts have no source yet; not starting the GPU (the firmware was not read, no GPU register was mapped, the coprocessor was not started)\n",
            self.gpu_name,
            missing
        );
        Err(ENODEV)
    }
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
    power_from_boot_loader: false,
    unported: &[],
};

/// T8122 (M3, G15G): one die, one cluster of ten core slots (eight or ten of them active).
///
/// The compatibles, the mailbox and its interrupts are the T8122 device tree's. The identity is
/// the one the T8122 identity gate admits (`t8122_admission`: family 7, variant 2, revision 0x20,
/// core slots in the first core-mask word only), with the die count of the AGX3 identification
/// table (`hw::agx3::T8122`). The power configuration comes from the boot loader. Nothing else
/// has a source for this SoC yet, so the runtime refuses it after admission.
pub(crate) static T8122: Soc = Soc {
    name: "T8122",
    gpu_name: "G15G",
    chip_id: 0x8122,
    gpu_variant: hw::GpuVariant::G,
    gpu_revision: hw::GpuRevision::C0,
    gpu_revision_id: hw::GpuRevisionID::C0,
    board: "apple,t8122",
    gpu: "apple,agx-t8122",
    // The M3 MacBook Airs (13" J613, 15" J615). Allowed, not validated: the runtime has not run
    // on either, and starts only behind a boot loader that hands the GPU over.
    validated_boards: &[b"apple,j613", b"apple,j615"],
    clusters: 1,
    cores_per_cluster: 10,
    mailbox_compatibles: &[
        b"apple,t8122-agx-asc-mailbox\0",
        b"apple,t8122-asc-mailbox\0apple,asc-mailbox-v4\0",
    ],
    mailbox_interrupts: [0, 723, 4, 0, 724, 4, 0, 725, 4, 0, 726, 4],
    id: IdWords {
        version: 0x0702_2000,
        version_mask: 0xffff_ff00,
        // One die, one cluster per die; the other fields are not known.
        counts: 0x0001_0100,
        counts_mask: 0x000f_ff00,
    },
    images: &[],
    firmware: None,
    hwcfg: None,
    io_mappings: None,
    sgx_setup: None,
    power_from_boot_loader: true,
    unported: &[
        "the runtime's IO map table (m3_init_storage::IOMAPS), which holds T6030 addresses",
        "the runtime UAT setup (mmu::Uat::new_t6030_running), which is keyed to chip 0x6030",
        "the InitData upload check of the T6030 performance-state table (m3_config)",
        "the runtime HwDataB configuration words and unit masks, which are the T6030 values (initdata)",
        "the userspace feature flags, which are keyed to chip 0x6030 (file.rs)",
    ],
};
