// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The per-SoC facts of the M3 runtime backend.
//!
//! Each table describes the register windows, memory layout and configuration of one GPU.
//! Missing configuration is `None`; outstanding runtime requirements are listed in `unported`.
//! Admission rejects either before reading firmware, mapping GPU registers or starting the
//! GPU coprocessor ([`Soc::require_complete`]).

use kernel::{device, prelude::*};

use crate::{
    hw,
    initdata::G15RuntimeHwDataB,
    m3_board::KnownImage,
    m3_firmware::Layout,
    m3_init_storage::IoMap, //
};

/// The SGX identification words the runtime admits, each compared under its mask.
pub(crate) struct IdWords {
    /// SGX+0xd04000: family [31:24], variant [23:16], revision [15:8].
    pub(crate) version: u32,
    pub(crate) version_mask: u32,
    /// SGX+0xd04010: dies [19:16], clusters per die [15:8].
    pub(crate) counts: u32,
    pub(crate) counts_mask: u32,
}

/// The register windows the runtime admits, as CPU physical addresses (device-tree `reg` values
/// translated through `arm-io` `ranges`).
pub(crate) struct Windows {
    /// Base of the GPU coprocessor window (`asc`, ADT `gfx-asc` reg[0]).
    pub(crate) asc: u64,
    /// Base of the GPU window (`sgx`, ADT `sgx` reg[0]).
    pub(crate) sgx: u64,
    /// Base of the GPU coprocessor mailbox, a 16 KiB window inside `asc`.
    pub(crate) mailbox: u64,
}

/// The performance-state table the runtime accepts: checked on the generated InitData before
/// any GPU register is touched, and again on the uploaded HwDataB.
#[derive(Copy, Clone, Debug)]
pub(crate) enum PstateTable {
    /// A fixed table: `states` states above the off state, the highest at `top_mhz` MHz.
    Fixed { states: u32, top_mhz: u32 },
    /// The table of the device tree's operating points: one state per distinct voltage above
    /// the off state, the highest at the highest frequency
    /// (`t8122_admission::opp_table_shape`).
    DeviceTree,
}

/// Optional userspace features advertised for a SoC (`DRM_ASAHI_GET_PARAMS`). The runtime
/// implements them the same way on every SoC; a SoC advertises one once it has been validated
/// there. Soft faults are never advertised: the runtime does not apply `asahi.fault_control`.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Features {
    /// `DRM_ASAHI_FEATURE_COMPUTE_WIDE_VISIBILITY`.
    pub(crate) compute_wide_visibility: bool,
    /// `DRM_ASAHI_FEATURE_FRAGMENT_DEPENDENCY`, while `asahi.m3_early_tiling` is set.
    pub(crate) fragment_dependency: bool,
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
    /// The register windows of the GPU node and its mailbox.
    pub(crate) windows: Windows,
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
    /// The runtime's mapping of each of those IO mappings: the CPU physical block and the
    /// firmware VA it is mapped at. One per entry of `io_mappings`, covering it.
    pub(crate) iomaps: Option<&'static [IoMap]>,
    /// The performance-state table the runtime accepts.
    pub(crate) pstates: PstateTable,
    /// The SGX write (offset, value) made after the identity checks, before the firmware starts.
    pub(crate) sgx_setup: Option<(usize, u32)>,
    /// The runtime InitData's HwDataB configuration words and unit masks.
    pub(crate) hwdata_b: Option<&'static G15RuntimeHwDataB>,
    /// The optional userspace features advertised.
    pub(crate) features: Features,
    /// Whether the power-management coefficients, the leakage coefficients and the operating
    /// points are this machine's, added by the boot loader from its ADT, rather than static
    /// device-tree values. Admission then requires every one of them (see
    /// `m3_board::check_boot_loader_power`), with no driver default standing in except the
    /// idle-off standby timer, which an ADT may lack.
    pub(crate) power_from_boot_loader: bool,
    /// Code the runtime runs after admission that still holds another SoC's values.
    pub(crate) unported: &'static [&'static str],
}

impl Soc {
    /// Reject incomplete configuration before accessing the GPU.
    pub(crate) fn require_complete(&self, dev: &device::Device) -> Result {
        let mut missing = 0u32;
        let mut note = |what: &str| {
            dev_info!(dev, "M3 {}: missing {}\n", self.gpu_name, what);
            missing += 1;
        };
        if !self.images.iter().any(|image| image.initdata_magic.is_some()) {
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
        if self.iomaps.is_none() {
            note("the runtime's mapping of the firmware IO mappings (IO maps)");
        }
        if self.sgx_setup.is_none() {
            note("the SGX setup write made before the firmware starts");
        }
        if self.hwdata_b.is_none() {
            note("the runtime HwDataB configuration words and unit masks");
        }
        for &what in self.unported {
            note(what);
        }
        if missing == 0 {
            return Ok(());
        }
        dev_err!(
            dev,
            "M3 {}: {} configuration requirements missing; GPU startup disabled\n",
            self.gpu_name,
            missing
        );
        Err(ENODEV)
    }
}

/// The HwDataB configuration words and unit masks of the T6030 runtime InitData, as checked on
/// J514S and J516S (the runtime's earlier fixed values).
pub(crate) static T6030_HWDATA_B: G15RuntimeHwDataB = G15RuntimeHwDataB {
    unk_454: 1,
    unk_464: 1,
    unk_a7c: 0x1_0000_0001,
    unk_a98: 0,
    unk_abc: 4,
    unk_ae4: 0x31,
    unk_b20: 0x14,
    unk_b24: 3,
    unk_554: 0,
    unk_17b8: 5,
    unit_mask_a: 0x7_0000_0003,
    unit_mask_b: 7,
    unk_1808: 1,
    unk_1818: 1,
};

/// Every SoC the M3 runtime has a table for.
pub(crate) static SOCS: [&Soc; 2] = [&T6030, &T8122];

/// The table of the SoC with chip id `chip_id`, if the M3 runtime has one.
pub(crate) fn by_chip(chip_id: u32) -> Option<&'static Soc> {
    SOCS.iter().copied().find(|soc| soc.chip_id == chip_id)
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
    // The T6030 device tree (t6030-gpu.dtsi): asc, sgx and the mailbox at asc + 0x8000.
    windows: Windows {
        asc: 0x2_9240_0000,
        sgx: 0x2_9000_0000,
        mailbox: 0x2_9240_8000,
    },
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
    iomaps: Some(&crate::m3_init_storage::T6030_IOMAPS),
    // The J514S/J516S runtime table: eight voltage-sorted states up to 1380 MHz.
    pstates: PstateTable::Fixed {
        states: 8,
        top_mhz: 1380,
    },
    sgx_setup: Some((0xd14000, 0x70001)),
    hwdata_b: Some(&T6030_HWDATA_B),
    // Validated on J514S and J516S.
    features: Features {
        compute_wide_visibility: true,
        fragment_dependency: true,
    },
    power_from_boot_loader: false,
    unported: &[],
};

/// T8122 (M3, G15G): one die, one cluster of ten core slots (eight or ten of them active).
///
/// The compatibles, the mailbox and its interrupts are the T8122 device tree's. The identity is
/// the one the T8122 identity gate admits (`t8122_admission`: family 7, variant 2, revision 0x20,
/// core slots in the first core-mask word only), with the die count of the AGX3 identification
/// table (`hw::agx3::T8122`). The hardware configuration is `hw::t8122`; power configuration
/// comes from the boot loader. The runtime rejects the SoC while the rest of its configuration is
/// missing, before accessing the GPU.
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
    // The J613 ADT: sgx reg[0] (child 0x80000000 + arm-io 0x210000000) and gfx-asc reg[0]; the
    // mailbox at asc + 0x8000 (t8122-gpu.dtsi). The same addresses as on T6030.
    windows: Windows {
        asc: 0x2_9240_0000,
        sgx: 0x2_9000_0000,
        mailbox: 0x2_9240_8000,
    },
    id: IdWords {
        version: 0x0702_2000,
        version_mask: 0xffff_ff00,
        // One die, one cluster per die; the other fields are not known.
        counts: 0x0001_0100,
        counts_mask: 0x000f_ff00,
    },
    images: &crate::m3_board::KNOWN_IMAGES_T8122,
    firmware: Some(&crate::m3_firmware::T8122_LAYOUT),
    hwcfg: Some(&hw::t8122::HWCONFIG_T8122),
    io_mappings: Some(&crate::m3_adt_config::T8122_IO_MAPPINGS),
    iomaps: None,
    // The boot loader's ladder from this machine's ADT (J613: eight voltages, up to 1338 MHz).
    pstates: PstateTable::DeviceTree,
    sgx_setup: None,
    hwdata_b: None,
    // Neither is validated on G15G yet; userspace keeps its default ordering and visibility.
    features: Features {
        compute_wide_visibility: false,
        fragment_dependency: false,
    },
    power_from_boot_loader: true,
    unported: &[
        "T8122 runtime allocation layout and fixed control words",
        "T8122 conservative performance-state ceiling",
    ],
};
