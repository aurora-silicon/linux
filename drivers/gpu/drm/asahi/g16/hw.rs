// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(
    dead_code,
    reason = "Firmware layout catalog also describes fields used by host wire regressions."
)]

//! Classic GPU hardware and firmware-ABI constants (layout values are shared by several
//! modules; change one only together with its users). The tuned policy values are in
//! [`tuning`].

use core::ops::Range;

/// Identification register values of a qualified GPU.
pub(crate) struct HwId {
    /// SGX identity word (`sgx + 0xd04000`).
    pub(crate) version: u32,
    /// SGX layout word (`sgx + 0xd04010`).
    pub(crate) counts: u32,
    /// Bits of `version` and `counts` that must match; the others are not qualified.
    pub(crate) version_mask: u32,
    pub(crate) counts_mask: u32,
}

impl HwId {
    /// Mask both words exactly; rows give their values with `..HwId::EXACT`.
    pub(crate) const EXACT: HwId = HwId {
        version: 0,
        counts: 0,
        version_mask: u32::MAX,
        counts_mask: u32::MAX,
    };

    /// Whether the identification registers read `version` and `counts`.
    pub(crate) const fn matches(&self, version: u32, counts: u32) -> bool {
        (version ^ self.version) & self.version_mask == 0
            && (counts ^ self.counts) & self.counts_mask == 0
    }
}

pub(crate) use crate::fwid::FirmwareBuild;

/// Static configuration of a SoC supported by this backend.
pub(crate) struct Config {
    /// SoC identifier reported to userspace.
    pub(crate) chip_id: u32,
    /// GPU generation reported to userspace.
    pub(crate) gpu_generation: u32,
    pub(crate) gpu_hal_generation: u32,
    /// GPU variant reported to userspace.
    pub(crate) gpu_variant: crate::hw::GpuVariant,
    /// GPU revision reported to userspace.
    pub(crate) gpu_revision: u32,
    /// Input address bits translated by each UAT root.
    pub(crate) uat_ias: u32,
    /// Output (physical) address bits of the UAT.
    pub(crate) uat_oas: u32,
    /// Clusters and cores per cluster.
    pub(crate) num_clusters: u32,
    pub(crate) num_cores_per_cluster: u32,
    /// Qualified identification register values.
    pub(crate) hw_ids: &'static [HwId],
    /// Compatible of the original ADT SGX node.
    pub(crate) adt_compatible: &'static [u8],
    /// NUL-terminated compatible of the native display subsystem supplier.
    pub(crate) display_compatible: &'static [u8],
    /// Qualified boot-loader handoffs.
    pub(crate) layouts: &'static [&'static FwLayout],
    /// Qualified firmware builds.
    pub(crate) firmware: &'static [FirmwareBuild],
    /// Whether this compatible also has a legacy driver.
    pub(crate) needs_handoff_marker: bool,
    /// GPU node property holding the boot loader's handoff version (the handoff marker).
    pub(crate) handoff_property: &'static core::ffi::CStr,
    /// `apple,firmware-compat` values this backend owns on a compatible that another backend
    /// also serves; empty when the handoff layout alone selects the backend.
    pub(crate) firmware_compat: &'static [[u32; 3]],
    /// T8132 retains the PMP bridge even on firmware that does not send an AGX vote.
    pub(crate) prepare_pmp: bool,
    /// PMGR register offset of the GFX domain.
    pub(crate) power_offset: u32,
    /// Number of valid fault-bank selectors in the SGX status window.
    pub(crate) fault_banks: usize,
    /// GFXCAXI2AF needs host enable writes before the 27.2 firmware starts.
    pub(crate) axi2af: bool,
    /// Offsets to zero, in order, in the host block before InitData on 27.2.
    pub(crate) host_block_zero_order: &'static [usize],
    /// Whether the host probes scratch and raises the ready word after zeroing.
    pub(crate) host_block_ready: bool,
    /// Whether the TVM gate and firmware warm restart are supported.
    pub(crate) deep_sleep: bool,
    /// Whether render work accepts scratch for ray queries.
    pub(crate) supports_render_scs: bool,
    /// Whether FS->compute cross-engine barriers require `BarrierType::Hold` (type 1), as on G15.
    pub(crate) cross_engine_hold: bool,
    /// Largest tile buffer a render command may use, in bytes per tile (sample size times
    /// utile width, utile height and samples). Reported as DRM_ASAHI_FEATURE_TILE_BUFFER_48K
    /// when it reaches [`sizes::TILE_BUFFER_48K`].
    pub(crate) max_tile_buffer: u32,
}

impl Config {
    pub(crate) const fn core_slots(&self) -> u32 {
        self.num_clusters * self.num_cores_per_cluster
    }
}

/// The J713/J773g T8132 configuration. Hardware admission also checks the exact TEXT image.
pub(crate) mod t8132 {
    use super::{Config, FirmwareBuild, HwId, FW_25G83, FW_26B123};

    pub(crate) const CONFIG: Config = Config {
        chip_id: 0x8132,
        gpu_generation: 16,
        gpu_hal_generation: kernel::uapi::drm_asahi_gpu_hal_generation_DRM_ASAHI_GPU_HAL_200,
        gpu_variant: crate::hw::GpuVariant::G,
        gpu_revision: 0x10,
        uat_ias: 42,
        uat_oas: 42,
        num_clusters: 1,
        num_cores_per_cluster: 10,
        hw_ids: &[HwId {
            version: 0x0a021100,
            counts: 0x0011010a,
            ..HwId::EXACT
        }],
        adt_compatible: b"gpu,t8132",
        display_compatible: b"apple,t8132-display-subsystem\0",
        layouts: &[&FW_25G83, &FW_26B123],
        firmware: &[
            FirmwareBuild {
                // 25G83 (build 2 of armfw_g16g.im4p) as iBoot loads it on a T8132 Air. Besides
                // the entropy, iBoot fills a header field, the record block the header points at
                // (0x5c8df, 0x231) and the tunables block its TUNS/TUNZ records describe
                // (0x5cb10, 0x6a0). With those zeroed, the loaded image hashes the same as the
                // TEXT of the firmware file.
                name: "Apple GFX 25G83 (normalized loaded image, boot-filled ranges)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: &[(0x23c, 8), (0x5c8df, 0x231), (0x5cb10, 0x6a0)],
                text_sha256: [
                    0x3a, 0x41, 0xe4, 0x77, 0xb1, 0x2d, 0x48, 0x8d, 0x0c, 0x27, 0x7d, 0x1d, 0x30,
                    0x6b, 0x60, 0x88, 0x5e, 0x67, 0x5e, 0xc7, 0x98, 0x54, 0xca, 0xaf, 0x90, 0x29,
                    0x35, 0xc2, 0x7e, 0xf8, 0x76, 0x5e,
                ],
            },
            // The per-board entries below stay until an 8-core and a 10-core J713 on 26.6.2
            // have booted with only the normalized entry above.
            FirmwareBuild {
                name: "Apple GFX 25G83 (normalized loaded image)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: &[],
                text_sha256: [
                    0x5f, 0x65, 0xf6, 0x14, 0x58, 0x72, 0x54, 0x70, 0xb4, 0xb0, 0xc0, 0x20, 0x28,
                    0x47, 0x7b, 0xf2, 0xea, 0x57, 0x3a, 0xfb, 0x4e, 0x0c, 0x49, 0x6e, 0xf9, 0x95,
                    0xe9, 0x6a, 0xff, 0xb0, 0xd8, 0x6c,
                ],
            },
            FirmwareBuild {
                // The same 25G83 image (build 2 of armfw_g16g.im4p) as loaded by iBoot on an
                // 8-core J713: only the block iBoot fills in after the entropy records differs.
                name: "Apple GFX 25G83 (normalized loaded image, 8-core J713)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: &[],
                text_sha256: [
                    0x3b, 0x36, 0x8e, 0xe4, 0x2d, 0xcc, 0xc6, 0x26, 0xe8, 0x49, 0x67, 0xff, 0x40,
                    0x87, 0x64, 0x71, 0x0a, 0x64, 0xbf, 0xf1, 0x59, 0x4f, 0x3c, 0xfd, 0x2c, 0x6e,
                    0x4d, 0x34, 0xd0, 0x65, 0x24, 0x8b,
                ],
            },
            FirmwareBuild {
                // The same 25G83 image as loaded by iBoot on a 10-core J713 (Mac16,12, 24 GB):
                // same sizes and entropy records as the 8-core entry, a different iBoot-filled
                // block (issue #1704).
                name: "Apple GFX 25G83 (normalized loaded image, 10-core J713)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: &[],
                text_sha256: [
                    0xd9, 0x61, 0x42, 0xd1, 0xef, 0x52, 0xaf, 0x13, 0x25, 0x08, 0xca, 0x06, 0xb7,
                    0xd0, 0x14, 0x58, 0xe2, 0x91, 0x0b, 0x85, 0xc8, 0xd7, 0x55, 0x78, 0xbf, 0x01,
                    0xc8, 0x29, 0x86, 0x8b, 0xc9, 0xcf,
                ],
            },
            FirmwareBuild {
                // The 25G83 image as loaded by iBoot on a J773g (Mac mini). iBoot rewrites
                // the header word at 0x224 and the 8-byte DILS payload at 0x5c917 on every
                // boot there; they are zeroed before hashing.
                name: "Apple GFX 25G83 (normalized loaded image, J773g)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: &[(0x224, 4), (0x5c917, 8)],
                text_sha256: [
                    0x91, 0x54, 0xc7, 0xf8, 0x4b, 0xce, 0x32, 0x15, 0x1e, 0x8a, 0x97, 0x41, 0x7d,
                    0x6a, 0x20, 0x7a, 0x9e, 0xaf, 0x03, 0x54, 0xb4, 0xb8, 0xd6, 0xf0, 0x2a, 0xf5,
                    0x29, 0x95, 0x40, 0xd3, 0xe8, 0x5a,
                ],
            },
            FirmwareBuild {
                // Build b000 (G16G B0) of armfw_g16g.im4p in 25G83, as iBoot loads it on any
                // machine: the boot-filled ranges below are zeroed, which gives the digest of
                // the shipped image with the same ranges zeroed.
                name: "Apple GFX 25G83 (build b000, iBoot-filled ranges zeroed)",
                text_size: 0x60000,
                data_size: 0x13c000,
                entropy: &[[0x5c8df, 0x5c9c2]],
                boot_filled: BOOT_FILLED_25G83_B000,
                text_sha256: [
                    0xb9, 0xea, 0x7e, 0x6b, 0x2a, 0xca, 0x70, 0xa2, 0x75, 0xa0, 0xb0, 0x68, 0xee,
                    0x79, 0x1e, 0xd5, 0x05, 0x27, 0xe8, 0xdb, 0x76, 0x19, 0x64, 0xd2, 0x73, 0x3f,
                    0xa4, 0x07, 0xa6, 0xf2, 0x22, 0x6c,
                ],
            },
            FirmwareBuild {
                // The 26B123 (macOS 27.2; 26B131 carries the same image) build b000 as loaded
                // on a J773g, with every range iBoot fills zeroed (`BOOT_FILLED_26B123_B000`).
                // DATA is the `fw-data` region: __DATA (0xd0000) and its __DATA_SHARED_RO tail
                // (0x8000), which the boot loader reserves as one.
                name: "Apple GFX 26B123 (build b000, iBoot-filled ranges zeroed)",
                text_size: 0x64000,
                data_size: 0xd8000,
                entropy: &[[0x60a1b, 0x60afe]],
                boot_filled: BOOT_FILLED_26B123_B000,
                text_sha256: [
                    0x48, 0x4c, 0x43, 0xcb, 0x79, 0x89, 0x00, 0xdd, 0x14, 0x65, 0x4e, 0xd5, 0x4e,
                    0xfe, 0x16, 0x24, 0xb4, 0x3d, 0xe3, 0xce, 0x80, 0x28, 0xb7, 0x71, 0x42, 0x5d,
                    0x63, 0xbc, 0xc6, 0xa7, 0x2c, 0x92,
                ],
            },
        ],
        needs_handoff_marker: false,
        handoff_property: super::G16_HANDOFF_PROPERTY,
        firmware_compat: &[],
        prepare_pmp: true,
        power_offset: 0x250,
        fault_banks: 8,
        axi2af: true,
        host_block_zero_order: &super::start_27_2::HOST_BLOCK_ZERO_ORDER,
        host_block_ready: true,
        deep_sleep: true,
        supports_render_scs: true,
        cross_engine_hold: false,
        // macOS 26B123 programs 48 KiB tiles (MSAA4, 32x16 utiles) on this GPU.
        max_tile_buffer: super::sizes::TILE_BUFFER_48K,
    };

    /// What iBoot writes into the 25G83 build b000 TEXT it loads, in TEXT order:
    /// - the header word at 0x224, which changes from boot to boot, and the DATA address at 0x23c;
    /// - the value of each of the 39 boot parameter records the header at 0x22c places from
    ///   0x5c8df to 0x5cb10 (a tag, a 32-bit length and the value: carveout addresses below the
    ///   top of DRAM, a per-boot slide, the entropy);
    /// - the entry count and the 0x34c-byte body of the two tunable tables at 0x5cb10 and 0x5ce60,
    ///   which the shipped image leaves empty and iBoot populates per board.
    pub(crate) const BOOT_FILLED_25G83_B000: &[(u32, u32)] = &[
        (0x224, 4),
        (0x23c, 8),
        (0x5c8e7, 8),
        (0x5c8f7, 8),
        (0x5c907, 8),
        (0x5c917, 8),
        (0x5c927, 8),
        (0x5c937, 8),
        (0x5c947, 8),
        (0x5c957, 4),
        (0x5c963, 1),
        (0x5c96c, 8),
        (0x5c97c, 8),
        (0x5c98c, 4),
        (0x5c998, 8),
        (0x5c9a8, 1),
        (0x5c9b1, 1),
        (0x5c9ba, 8),
        (0x5c9ca, 8),
        (0x5c9da, 8),
        (0x5c9ea, 8),
        (0x5c9fa, 8),
        (0x5ca0a, 8),
        (0x5ca1a, 8),
        (0x5ca2a, 8),
        (0x5ca3a, 8),
        (0x5ca4a, 8),
        (0x5ca5a, 8),
        (0x5ca6a, 8),
        (0x5ca7a, 4),
        (0x5ca86, 8),
        (0x5ca96, 4),
        (0x5caa2, 4),
        (0x5caae, 8),
        (0x5cabe, 8),
        (0x5cace, 1),
        (0x5cad7, 8),
        (0x5cae7, 8),
        (0x5caf7, 4),
        (0x5cb03, 1),
        (0x5cb0c, 4),
        (0x5cb13, 1),
        (0x5cb14, 0x34c),
        (0x5ce63, 1),
        (0x5ce64, 0x34c),
    ];

    /// What iBoot writes into the 26B123 build b000 TEXT it loads: the same two header words and
    /// the same 39 boot parameter records and two tunable tables as 25G83, which the 26B123 image
    /// places 0x413c bytes later (the parameter records from 0x60a1b, the tables from 0x60c4c).
    pub(crate) const BOOT_FILLED_26B123_B000: &[(u32, u32)] = &[
        (0x224, 4),
        (0x23c, 8),
        (0x60a23, 8),
        (0x60a33, 8),
        (0x60a43, 8),
        (0x60a53, 8),
        (0x60a63, 8),
        (0x60a73, 8),
        (0x60a83, 8),
        (0x60a93, 4),
        (0x60a9f, 1),
        (0x60aa8, 8),
        (0x60ab8, 8),
        (0x60ac8, 4),
        (0x60ad4, 8),
        (0x60ae4, 1),
        (0x60aed, 1),
        (0x60af6, 8),
        (0x60b06, 8),
        (0x60b16, 8),
        (0x60b26, 8),
        (0x60b36, 8),
        (0x60b46, 8),
        (0x60b56, 8),
        (0x60b66, 8),
        (0x60b76, 8),
        (0x60b86, 8),
        (0x60b96, 8),
        (0x60ba6, 8),
        (0x60bb6, 4),
        (0x60bc2, 8),
        (0x60bd2, 4),
        (0x60bde, 4),
        (0x60bea, 8),
        (0x60bfa, 8),
        (0x60c0a, 1),
        (0x60c13, 8),
        (0x60c23, 8),
        (0x60c33, 4),
        (0x60c3f, 1),
        (0x60c48, 4),
        (0x60c4f, 1),
        (0x60c50, 0x34c),
        (0x60f9f, 1),
        (0x60fa0, 0x34c),
    ];
}

/// T6030 (G15S), qualified with the b000 image loaded on J516s.
pub(crate) mod t6030 {
    use super::super::fw::initdata;
    use super::{Config, FirmwareBuild, FwLayout, HwId, Scratch, FW_26B123, HOST_BLOCK, UPPER};

    pub(crate) const CONFIG: Config = Config {
        chip_id: 0x6030,
        gpu_generation: 15,
        gpu_hal_generation: kernel::uapi::drm_asahi_gpu_hal_generation_DRM_ASAHI_GPU_HAL_LEGACY,
        gpu_variant: crate::hw::GpuVariant::S,
        gpu_revision: 0x11,
        uat_ias: 42,
        uat_oas: 42,
        num_clusters: 2,
        num_cores_per_cluster: 10,
        hw_ids: &[HwId {
            version: 0x07031100,
            counts: 0x00110209,
            ..HwId::EXACT
        }],
        adt_compatible: b"gpu,t6030",
        display_compatible: b"apple,t6030-display-subsystem\0",
        layouts: &[&LAYOUT],
        firmware: &[FirmwareBuild {
            name: "Apple GFX 26B123 G15S (build b000, iBoot-filled ranges zeroed)",
            text_size: LAYOUT.text_size,
            data_size: LAYOUT.data_size,
            entropy: &[[0x5be64, 0x5bf47]],
            boot_filled: BOOT_FILLED_B000,
            text_sha256: [
                0x31, 0xc7, 0xc7, 0x7a, 0x24, 0x85, 0x88, 0x7b, 0x66, 0x2e, 0xa2, 0x2d, 0xe6, 0xcc,
                0xc5, 0xf9, 0xf6, 0x87, 0xa2, 0x4b, 0x07, 0xbd, 0xbd, 0xdf, 0x95, 0x21, 0x1a, 0x06,
                0xcd, 0x5f, 0x57, 0x3b,
            ],
        }],
        needs_handoff_marker: true,
        handoff_property: super::G16_HANDOFF_PROPERTY,
        firmware_compat: &[],
        prepare_pmp: false,
        power_offset: 0x110,
        fault_banks: 2,
        axi2af: false,
        // Host-block control words zeroed read-before-write at cold 26B123 startup:
        // +0x0000 is the host device-control generation word; +0x1808 and +0x3010 are the
        // per-pipe host-block control words cleared before endpoints 0x20/0x21 start.
        host_block_zero_order: &[0, 0x1808, 0x3010],
        host_block_ready: false,
        deep_sleep: false,
        supports_render_scs: false,
        cross_engine_hold: true,
        max_tile_buffer: super::sizes::TILE_BUFFER_32K,
    };

    const LAYOUT: FwLayout = FwLayout {
        name: "26B123 G15S",
        segment_vas: &[UPPER, UPPER + 0x60000, UPPER + 0x18c000],
        text_size: 0x60000,
        data_size: 0x134000,
        abi: super::FwAbi::V26B123G15S,
        initdata: &initdata::G15S_26B123,
        scratch: Scratch {
            register: HOST_BLOCK,
            // The G15S scratch window leaves an unmapped guard page at VA +0x14000 while the
            // physical host-register window stays contiguous across the split (+0x14000..+0x20000).
            spans: &[(0, 0, 0x14000), (0x18000, 0x14000, 0xc000)],
            pointer: 0x12068,
        },
        // Offset on the PPL handoff page where the G15S 26B123 firmware records
        // whether the GPU is powered during idle power-gating.
        powered: Some(0x638),
        ..FW_26B123
    };

    /// Fixed normalization ranges of the qualified G15S b000 image. Only the
    /// values of iBoot's records, the two header fields and the tunable contents
    /// are ignored. The digest still covers the tags, lengths and table headers.
    const BOOT_FILLED_B000: &[(u32, u32)] = &[
        (0x224, 4),
        (0x23c, 8),
        (0x5be6c, 8),
        (0x5be7c, 8),
        (0x5be8c, 8),
        (0x5be9c, 8),
        (0x5beac, 8),
        (0x5bebc, 8),
        (0x5becc, 8),
        (0x5bedc, 4),
        (0x5bee8, 1),
        (0x5bef1, 8),
        (0x5bf01, 8),
        (0x5bf11, 4),
        (0x5bf1d, 8),
        (0x5bf2d, 1),
        (0x5bf36, 1),
        (0x5bf3f, 8),
        (0x5bf4f, 8),
        (0x5bf5f, 8),
        (0x5bf6f, 8),
        (0x5bf7f, 8),
        (0x5bf8f, 8),
        (0x5bf9f, 8),
        (0x5bfaf, 8),
        (0x5bfbf, 8),
        (0x5bfcf, 8),
        (0x5bfdf, 8),
        (0x5bfef, 1),
        (0x5bff8, 8),
        (0x5c008, 4),
        (0x5c014, 8),
        (0x5c024, 4),
        (0x5c030, 4),
        (0x5c03c, 4),
        (0x5c048, 8),
        (0x5c058, 8),
        (0x5c068, 1),
        (0x5c071, 8),
        (0x5c081, 8),
        (0x5c091, 4),
        (0x5c098, 1),
        (0x5c099, 0x2d4),
        (0x5c370, 1),
        (0x5c371, 0x2d4),
    ];
}

/// The handoff marker aura publishes for the T8132 and T6030 GPUs.
pub(crate) const G16_HANDOFF_PROPERTY: &core::ffi::CStr = c"apple,g16-handoff-version";

/// T8122 (G15G), qualified with the C0 image the macOS 27.2 boot chain loads (26B123).
///
/// The same compatible is served by the `m3` runtime for the 14.8.3 and 26.6.2 firmware, so this
/// row owns a device only when its `firmware-compat` is 27.2 and the handoff describes [`LAYOUT`]
/// ([`Config::firmware_compat`], `adt::claims`). Aura names the handoff version
/// `apple,m3-handoff-version` on this SoC (aura#448/#455).
pub(crate) mod t8122 {
    use super::super::fw::initdata;
    use super::{Config, FirmwareBuild, FwLayout, HwId, FW_26B123, UPPER};

    pub(crate) const CONFIG: Config = Config {
        chip_id: 0x8122,
        gpu_generation: 15,
        gpu_hal_generation: kernel::uapi::drm_asahi_gpu_hal_generation_DRM_ASAHI_GPU_HAL_LEGACY,
        gpu_variant: crate::hw::GpuVariant::G,
        // C0 (`crate::hw::GpuRevision::C0`), as the m3 runtime reports it.
        gpu_revision: 0x20,
        uat_ias: 42,
        uat_oas: 42,
        // One cluster of ten core slots; eight or ten are enabled.
        num_clusters: 1,
        num_cores_per_cluster: 10,
        // Family 7, variant 2 (G15G), revision 0x20; one die and one cluster. The other bits
        // of the two words are not qualified (`m3_soc::T8122.id`).
        hw_ids: &[HwId {
            version: 0x07022000,
            counts: 0x00010100,
            version_mask: 0xffffff00,
            counts_mask: 0x000fff00,
        }],
        adt_compatible: b"gpu,t8122",
        display_compatible: b"apple,t8122-display-subsystem\0",
        layouts: &[&LAYOUT],
        firmware: &[FirmwareBuild {
            // GKTS/ECAP-normalized loaded TEXT, measured on the J615 over three boots
            // (linux#2781, `m3_firmware::T8122_IMAGES_27`): only the two entropy values are
            // zeroed, which is this backend's normalization with no boot-filled ranges.
            name: "Apple GFX 26B123 G15G (C0, entropy zeroed)",
            text_size: LAYOUT.text_size,
            data_size: LAYOUT.data_size,
            entropy: &[[0x60f60, 0x61043]],
            boot_filled: &[],
            text_sha256: [
                0x74, 0x06, 0xa5, 0x68, 0x34, 0x28, 0x37, 0xa7, 0x9f, 0x22, 0xc0, 0x3f, 0x7d, 0x45,
                0x7f, 0x9f, 0x9d, 0x3e, 0x08, 0x8c, 0x4b, 0x78, 0xd7, 0x83, 0xe2, 0xdc, 0xad, 0x76,
                0x71, 0x6f, 0x16, 0xf5,
            ],
        }],
        needs_handoff_marker: true,
        handoff_property: c"apple,m3-handoff-version",
        firmware_compat: &[[27, 2, 0]],
        // The 27.2 start sends no PMP vote.
        prepare_pmp: false,
        // `ps_gfx` in t8122-pmgr.dtsi.
        power_offset: 0x1e0,
        fault_banks: 1,
        // The m3 27.2 start on the J615 writes no GFXCAXI2AF enables (no slot 22 either).
        axi2af: false,
        host_block_zero_order: &super::start_27_2::HOST_BLOCK_ZERO_ORDER,
        host_block_ready: true,
        deep_sleep: false,
        supports_render_scs: false,
        cross_engine_hold: true,
        max_tile_buffer: super::sizes::TILE_BUFFER_32K,
    };

    /// 26B123 G15G: __TEXT 0x64000 as on G16G, __DATA 0xb4000 and the 0x8000 __DATA_SHARED_RO
    /// tail, so the third segment is at UPPER + 0x118000 (aura's version-2 handoff).
    pub(crate) const LAYOUT: FwLayout = FwLayout {
        name: "26B123 G15G",
        segment_vas: &[UPPER, UPPER + 0x64000, UPPER + 0x118000],
        data_size: 0xbc000,
        initdata: &initdata::G15G_26B123,
        // The G15 family's 26B123 protocol: the J615 firmware installs root[1] at root + 3
        // pages with 0x403 attributes, the G15S convention, not G16G's.
        abi: super::FwAbi::V26B123G15S,
        // Not known for this firmware; the host keeps reading GPU registers while it runs, as
        // the m3 runtime does.
        powered: None,
        ..FW_26B123
    };
}

/// Which part of the loaded image a firmware page-table leaf maps.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Segment {
    Text,
    Data,
}

/// One range of leaves the boot loader builds in the firmware's own page tables: offsets from
/// [`UPPER`], the segment whose physical pages back them (at the same offset from the segment's
/// start), and the exact leaf attribute bits.
pub(crate) struct Leaves {
    pub(crate) range: Range<u64>,
    pub(crate) segment: Segment,
    pub(crate) attrs: u64,
}

/// The firmware ABI a build speaks to the host: its InitData, device-control, queue-info and
/// work-item layouts (`fw::{initdata, channels, graph, work}`, selected in `fw/mod.rs`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum FwAbi {
    V25G83,
    V26B123,
    V26B123G15S,
}

impl FwAbi {
    /// Offset of the table installed in root[1] after startup, and the
    /// descriptor attributes the firmware uses for it and the shared L2.
    pub(crate) const fn live_root_table(self) -> (u64, u64) {
        match self {
            Self::V25G83 | Self::V26B123 => (4 * PAGE, 0x00200000000004cb),
            Self::V26B123G15S => (3 * PAGE, 0x403),
        }
    }
}

/// How the firmware's MMIO register table in loaded DATA is filled.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum LoaderTable {
    /// The host writes the HW data register-map records at this DATA offset before the
    /// initdata doorbell (the firmware's table must be empty on cold entry).
    Host(u64),
    /// The host leaves loaded DATA alone: the firmware copies every named register-map slot
    /// and the SGX window from HW data itself while it parses InitData, as the T6041 and M5
    /// G16-family firmware do.
    Firmware,
}

/// How the host starts a firmware build: RTKit and the MMIO around RUN and the endpoints.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Start {
    /// 25G83, as qualified on hardware: RTKit wake (IOP power INIT first), the firmware's crash
    /// buffer mapped without a reply, the host generation word zeroed after the endpoints start
    /// and a 100 ms receive-ring drain before InitData.
    Wake25G83,
    /// 26B123 start sequence: see [`start_27_2`].
    MacOs27_2,
}

/// Host-owned SGX register block.
pub(crate) const HOST_BLOCK: u64 = 0xd60000;

/// The 26B123 start sequence up to the InitData doorbell:
/// 1. before RUN, bit 0 of GFXCAXI2AF `+0x104` and `+0x108` is set (read, then written);
/// 2. RTKit boots without an IOP power request: HELLO 12, the crash-log endpoint starts while
///    the endpoint map is handled, the firmware's own crash buffer request (its shared RO
///    segment) is sent back unchanged, the host hands it a 0x8000-byte log buffer of its own
///    ([`MSG_LOG_BUFFER`], 8 pages at a host-window VA), then AP power state ON (0x20);
/// 3. the host block at SGX+0xd60000 is zeroed in [`HOST_BLOCK_ZERO_ORDER`], the scratch word
///    at SGX+0xd640c0 is read, [`HOST_READY`] is read and set to 1;
/// 4. endpoints 0x20 and 0x21 start and the InitData doorbell follows at once.
pub(crate) mod start_27_2 {
    /// GFXCAXI2AF registers (register-map slot 22, PA 0x301000000), from SGX.
    pub(crate) const AXI2AF: usize = 0x1000000;
    /// The two GFXCAXI2AF words whose bit 0 the host sets before RUN.
    pub(crate) const AXI2AF_ENABLES: [usize; 2] = [0x104, 0x108];
    /// The 32-bit writes of zero to the host block (SGX+0xd60000).
    pub(crate) const HOST_BLOCK_ZERO_ORDER: [usize; 13] = [
        0x00, 0x20, 0x40, 0x08, 0x28, 0x48, 0x10, 0x30, 0x50, 0x18, 0x38, 0x58, 0x60,
    ];
    /// The host block word read and then set to 1 before the endpoints start.
    pub(crate) const HOST_READY: usize = 0x60;
    /// The scratch word read between zeroing the host block and setting [`HOST_READY`] (SGX offset).
    pub(crate) const SCRATCH_PROBE: usize = 0xd640c0;
    /// RTKit crash-log endpoint.
    pub(crate) const EP_CRASHLOG: u8 = 1;
    /// Host log buffer message on the crash-log endpoint: the size in 4 KiB pages (from ADT
    /// `coredump-copy-buffer`) at bit 44 and the buffer's address below it.
    pub(crate) const MSG_LOG_BUFFER: u64 = 0x9 << 52;
    pub(crate) const MSG_LOG_BUFFER_PAGES_SHIFT: u32 = 44;
    pub(crate) const MSG_LOG_BUFFER_ADDR_MASK: u64 = (1 << 44) - 1;
}

/// The two magic words of the PPL handoff page.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct PplMagic {
    /// What the AP publishes at +0.
    pub(crate) ap: u64,
    /// The only non-zero value +8 may hold at cold admission: what the firmware leaves there
    /// once it has run.
    pub(crate) firmware: u64,
    /// The firmware stores `firmware` at +8 from its MMU setup, before the RTKit handshake.
    pub(crate) answers: bool,
}

/// Firmware-visible alias of host registers, with the pointer InitData passes into it.
pub(crate) struct Scratch {
    pub(crate) register: u64,
    /// `(va_offset, pa_offset, bytes)` spans in ascending VA order.
    pub(crate) spans: &'static [(u64, u64, u64)],
    pub(crate) pointer: u64,
}

const T8132_SCRATCH: Scratch = Scratch {
    register: HOST_BLOCK,
    spans: &[(0, 0, 0x8000)],
    pointer: 0,
};

/// The loaded-image layout of one firmware ABI: the boot loader's handoff for it, the regions
/// it reserves, the leaves it builds in the firmware page tables, and where the boot path writes.
pub(crate) struct FwLayout {
    pub(crate) name: &'static str,
    /// `apple,g16-handoff-version`, `apple,firmware-segment-vas` and `-flags`.
    pub(crate) handoff_version: u32,
    pub(crate) segment_vas: &'static [u64],
    pub(crate) segment_flags: &'static [u32],
    /// Sizes of the reserved `fw-text` and `fw-data` regions (`fw-data` holds every segment
    /// after TEXT).
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    /// The firmware ABI of the builds this layout holds.
    pub(crate) abi: FwAbi,
    /// InitData wire layout, including the version word of this firmware image.
    pub(crate) initdata: &'static super::fw::initdata::Layout,
    pub(crate) scratch: Scratch,
    /// Every leaf the boot loader builds in the firmware's own page tables, in VA order, and
    /// the only leaves cold admission accepts (none: the firmware builds its own tree at
    /// start); `None` until known, which refuses the firmware.
    pub(crate) leaves: Option<&'static [Leaves]>,
    /// The PPL handoff page's magic words; `None` until known, which refuses the firmware.
    pub(crate) ppl_magic: Option<PplMagic>,
    /// How the firmware's register table in loaded DATA is filled; `None` until known, which
    /// refuses the firmware.
    pub(crate) loader_table: Option<LoaderTable>,
    /// How the host starts this firmware.
    pub(crate) start: Start,
    /// Where on the PPL handoff page the firmware records whether the GPU is powered, for a
    /// firmware that powers it down while idle; `None` for one that keeps it powered while it
    /// runs. While that byte is 0 the host reads no GPU register.
    pub(crate) powered: Option<usize>,
}

impl FwLayout {
    /// Whether a boot loader's handoff describes this layout.
    pub(crate) fn matches(&self, version: u32, vas: &[u64], flags: &[u32]) -> bool {
        version == self.handoff_version && vas == self.segment_vas && flags == self.segment_flags
    }

    /// What a firmware start needs that is not known for this layout yet; empty when the
    /// firmware can be started.
    pub(crate) fn unknown(&self) -> impl Iterator<Item = &'static str> {
        [
            self.leaves
                .is_none()
                .then_some("the leaves the boot loader builds in the firmware page tables"),
            self.ppl_magic
                .is_none()
                .then_some("the PPL handoff magic words"),
            self.loader_table.is_none().then_some(
                "whether the host writes the register map into the firmware's table in \
                 loaded DATA or the firmware copies it from HW data",
            ),
        ]
        .into_iter()
        .flatten()
    }
}

/// 25G83 (macOS 26.x): __TEXT 0x60000, __DATA 0x13c000.
pub(crate) const FW_25G83: FwLayout = FwLayout {
    name: "25G83",
    handoff_version: 1,
    segment_vas: &[UPPER, UPPER + 0x60000],
    segment_flags: &[1, 0],
    text_size: 0x60000,
    data_size: 0x13c000,
    abi: FwAbi::V25G83,
    initdata: &super::fw::initdata::V25G83,
    scratch: T8132_SCRATCH,
    leaves: Some(&[
        Leaves {
            range: 0..0x50000,
            segment: Segment::Text,
            attrs: 0x4c3,
        },
        Leaves {
            range: 0x50000..0x58000,
            segment: Segment::Text,
            attrs: 0x483,
        },
        Leaves {
            range: 0x58000..0x60000,
            segment: Segment::Text,
            attrs: 0x4c3,
        },
        Leaves {
            range: 0x60000..0x6c000,
            segment: Segment::Data,
            attrs: 0x00200000000004c3,
        },
        Leaves {
            range: 0x6c000..0x19c000,
            segment: Segment::Data,
            attrs: 0x0460000000000443,
        },
    ]),
    // The 25G83 firmware never writes +8; the AP's magic there is accepted.
    ppl_magic: Some(PplMagic {
        ap: PPL_MAGIC,
        firmware: PPL_MAGIC,
        answers: false,
    }),
    loader_table: Some(LoaderTable::Host(0x1332f0)),
    start: Start::Wake25G83,
    // The GPU stays powered from START until the firmware stops.
    powered: None,
};

/// 26B123 (macOS 27.2, also 26B131): __TEXT 0x64000, __DATA 0xd0000 and a __DATA_SHARED_RO
/// 0x8000 right after it (aura's handoff version 2; `fw-data` covers both, so the RTKit crash
/// log at the start of the shared read-only segment lies inside it).
///
/// The boot loader leaves the page-table region zero and the firmware builds its own tree
/// (root, the L2 at +0x4000 and the L3 at +0x8000, with the TEXT and DATA leaves) when it
/// starts, so only an empty tree is admitted. Once it has set up its MMU the firmware leaves
/// 0x4b1d000000000003 at +8 of the PPL handoff page (the 25G83 firmware never wrote there).
/// The AP announces the same word at +0 before RUN, as the G14 firmware-27.2 path does on
/// hardware (linux#2544: AP and firmware exchange 0x4b1d000000000003 through these two words,
/// the firmware answering before the RTKit handshake). The host leaves loaded DATA alone: the
/// firmware takes the register map from HW data while it parses InitData, the T6041 build of
/// this firmware starts on hardware without any DATA write (linux#1954), and 27.2 marks DATA
/// data-isolated. Its register table is at DATA + 0xc72e8 (60 slots).
pub(crate) const FW_26B123: FwLayout = FwLayout {
    name: "26B123",
    handoff_version: 2,
    segment_vas: &[UPPER, UPPER + 0x64000, UPPER + 0x134000],
    segment_flags: &[1, 0x10, 0],
    text_size: 0x64000,
    data_size: 0xd8000,
    abi: FwAbi::V26B123,
    initdata: &super::fw::initdata::V26B123,
    scratch: T8132_SCRATCH,
    // The firmware builds its own tree at start; admit only an empty tree.
    leaves: Some(&[]),
    ppl_magic: Some(PplMagic {
        ap: PPL_MAGIC_27_2,
        firmware: PPL_MAGIC_27_2,
        answers: true,
    }),
    loader_table: Some(LoaderTable::Firmware),
    start: Start::MacOs27_2,
    // The firmware powers the GPU down after its idle delay (region C 0x9c0) and back up for
    // the next work, under the PPL handoff lock, and sets this byte to 1 after each power-up
    // and to 0 after each power-down.
    powered: Some(0x648),
};

/// User VA range reported as `vm_start..vm_end`: 42 bits with the final two pages excluded.
pub(crate) const USER_RANGE: Range<u64> = 0x4000..0x3ffffff8000;

/// Bits per cluster in the enabled-core mask (`sgx + 0xe01500`).
pub(crate) const CLUSTER_BITS: u32 = 10;

/// Firmware command counter frequency. User results are converted to nanoseconds.
pub(crate) const COMMAND_TIMESTAMP_HZ: u64 = 24_000_000;

/// UAT page size.
pub(crate) const PAGE: u64 = 0x4000;
/// Physical address bits of UAT leaves.
pub(crate) const PA_MASK: u64 = 0x3ffffffc000;

/// Firmware upper half: root[0..1] are firmware-owned, root[2] is the host window.
pub(crate) const UPPER: u64 = 0xfffffc0000000000;
pub(crate) const HOST: u64 = 0xfffffc2000000000;
pub(crate) const HOST_SIZE: u64 = 0x1000000000;

/// Firmware UAT leaf words. Shared-layer `Prot` values for them are built with
/// `mmu::Prot::from_pte` and must round-trip to these exact words.
pub(crate) mod pte {
    /// Firmware-only shared RW data, AttrIndex 2 (inner non-cacheable for the ASC).
    pub(crate) const FW: u64 = 0x00c000000000044b;
    /// Firmware + GPU shared, AttrIndex 2.
    pub(crate) const SHARED: u64 = 0x00e000000000040b;
    /// The same two classes with AttrIndex 0 (inner+outer write-back for the ASC).
    pub(crate) const FW_CACHED: u64 = 0x00c0000000000443;
    pub(crate) const SHARED_CACHED: u64 = 0x00e0000000000403;
    /// Firmware device RW.
    pub(crate) const MMIO: u64 = 0x00c0000000000447;
    /// Table descriptor bits.
    pub(crate) const TABLE: u64 = 3;
    /// Context-0 lower work alias: GPU RW NC, non-global.
    pub(crate) const LOWER: u64 = 0x00c0000000000c8b;
    /// Client (user VM) driver leaves: NC, cached, PB list.
    pub(crate) const LEAF_NC: u64 = 0x00c0000000000c8b;
    pub(crate) const LEAF_CACHED: u64 = 0x00c0000000000c83;
    pub(crate) const LEAF_PB_LIST: u64 = 0x00e0000000000c03;
}

/// PPL handoff magic the AP publishes for the 25G83 firmware.
pub(crate) const PPL_MAGIC: u64 = 0x4b1d000000000002;
/// The PPL handoff magic of the 27.2 (26B123) GPU firmware, published by both sides.
pub(crate) const PPL_MAGIC_27_2: u64 = 0x4b1d000000000003;

/// UAT handoff region sizes of this firmware ABI (TTBAT page, TTBAT tables (the firmware kernel
/// root), PPL handoff page, shared L2 page); the addresses come from the ADT.
pub(crate) const TTBS_SIZE: u64 = 0x4000;
pub(crate) const TABLES_SIZE: u64 = 0x40000;
pub(crate) const HANDOFF_SIZE: u64 = 0x4000;
pub(crate) const SHARED_L2_SIZE: u64 = 0x4000;

/// Firmware object placement. Initdata is allocated dynamically in the same host window as
/// command backing; its pointers are encoded from the actual allocations.
pub(crate) mod va {
    pub(crate) const FW_ALLOC: core::ops::Range<u64> = 0xfffffc2000000000..0xfffffc2ff8000000;
    pub(crate) const SELF_CHECK: u64 = 0xfffffc2ff8000000;
    pub(crate) const SELF_CHECK_LOWER: u64 = 0x70ff000000;
}

/// Firmware channels and rings of the 25G83 initdata graph.
pub(crate) mod rings {
    /// Twelve submit channels, each consisting of two objects.
    pub(crate) const CHANNELS: u64 = 12;
    pub(crate) const STATE_SIZE: usize = 0x30;
    pub(crate) const CHANNEL_RING_SIZE: usize = 0x1800;
    pub(crate) const DEVCTL_RING_SIZE: usize = 0x4000;
    pub(crate) const EVENT_RING_SIZE: usize = 0x4800;
    pub(crate) const LOG_RINGS: usize = 9;
    pub(crate) const LOG_STATE_SIZE: usize = 0x1b0;
    pub(crate) const LOG_RING_SIZE: usize = 0x28800;
    pub(crate) const LOG_PAYLOAD_SIZE: usize = 0xd8;
    pub(crate) const LOG_PAYLOAD_TEXT_OFFSET: usize = 0x10;
    pub(crate) const LOG_PAYLOAD_TEXT_SIZE: usize = 0xc8;
    pub(crate) const TRACE_RING_SIZE: usize = 0x9000;
    pub(crate) const STATS_RING_SIZE: usize = 0x4800;
    pub(crate) const FWCTL_RING_SIZE: usize = 0x4000;
    pub(crate) const DEVCTL_RECORD: usize = 0x40;
    pub(crate) const EVENT_RECORD: usize = 0x48;
    pub(crate) const CHANNEL_RECORD: usize = 0x18;
}

/// RTKit endpoints and messages.
pub(crate) mod ep {
    /// Firmware endpoint: the initdata doorbell and ring-service requests.
    pub(crate) const FIRMWARE: u8 = 0x20;
    /// Doorbell endpoint: channel and device-control notifications.
    pub(crate) const DOORBELL: u8 = 0x21;
    /// Host message handing the firmware its initdata root.
    pub(crate) const INITDATA: u64 = 0x0081 << 48;
    /// Address bits of the initdata root carried by [`INITDATA`].
    pub(crate) const INITDATA_ADDR_MASK: u64 = 0xfffffffffff;
    /// Firmware message requesting that the receive rings be drained.
    pub(crate) const RING_SERVICE: u64 = 0x0042 << 48;
}

/// Fixed per-VM client placement, identical in every VM, inside [`CLIENT_APERTURE`], plus the
/// pass slots and PB page list in the compact window. User binds into any of these fail.
pub(crate) mod client {
    use core::ops::Range;
    pub(crate) const CLIENT_APERTURE: Range<u64> = 0x7000000000..0x7100000000;
    pub(crate) const COMPACT_WINDOW: Range<u64> = 0x1000000000..0x9000000000;

    // Persistent compute units reused across application queues. Units 0 and 1 keep their
    // original 64 MiB placement below the render allocators; extension units (2..COMPUTE_UNITS)
    // pack at a 40 MiB stride from 0x7080000000 up to the SCS compute records (0x70b8000000),
    // clear of [`DYNAMIC`] and [`SCS_POOLS`] and leaving 7.25 MiB guard above each unit's
    // 32.75 MiB footprint (`0x20c0000`). Every alias is inside Mesa's reserved hole.
    pub(crate) const EXT_COMPUTE_STRIDE: u64 = 0x02800000;
    pub(crate) const fn compute_base(k: u64) -> u64 {
        if k < 2 {
            0x7001000000 + k * 0x04000000
        } else {
            0x7080000000 + (k - 2) * EXT_COMPUTE_STRIDE
        }
    }
    pub(crate) const fn compute_pool(k: u64) -> u64 {
        compute_base(k)
    }
    pub(crate) const fn compute_state(k: u64) -> u64 {
        compute_base(k) + 0x2000000
    }
    pub(crate) const fn compute_list(k: u64) -> u64 {
        compute_base(k) + 0x2020000
    }
    pub(crate) const fn compute_preempt(k: u64) -> u64 {
        compute_base(k) + 0x2030000
    }
    pub(crate) const fn compute_aux(k: u64) -> u64 {
        compute_base(k) + 0x2040000
    }
    pub(crate) const fn compute_context(k: u64) -> u64 {
        compute_base(k) + 0x2080000
    }
    pub(crate) const WORK_PAGE: u64 = 0x7000100000;
    pub(crate) const WORK_OFF: u64 = 0;
    pub(crate) const CWORK_STRIDE: u64 = 0x4000;

    // SCS-capable compute units (ray-query scratch), one 64 MiB interval each after the last
    // compute unit (0x70b7000000) and below the PB heap (0x70c0000000), holding the unit's
    // list, preempt, auxiliary and context records at the base units' offsets from their
    // state. Its UMA pool and freelist are in its SCS pool window ([`scs_compute_pool`]).
    pub(crate) const fn scs_compute_records(k: u64) -> u64 {
        0x70b8000000 + k * 0x04000000
    }
    /// Offsets of a compute unit's records from its state.
    pub(crate) const COMPUTE_LIST_OFF: u64 = compute_list(0) - compute_state(0);
    pub(crate) const COMPUTE_PREEMPT_OFF: u64 = compute_preempt(0) - compute_state(0);
    pub(crate) const COMPUTE_AUX_OFF: u64 = compute_aux(0) - compute_state(0);
    pub(crate) const COMPUTE_CONTEXT_OFF: u64 = compute_context(0) - compute_state(0);

    // Render allocator units contain one 32 MiB UMA pool and its persistent lists in a
    // 64 MiB stride. Per-VM local slots 0..4 live in 0x7010000000..0x7020000000; local slots
    // 4..MAX_VM_PB_SLOTS live in 0x7008000000..0x7010000000, above base compute units 0..2
    // (ending at 0x70070c0000) and below the render pass slot window (0x7020000000..0x7040000000).
    pub(crate) const fn render_base(k: u64) -> u64 {
        if k < 4 {
            0x7010000000 + k * 0x04000000
        } else {
            0x7008000000 + (k - 4) * 0x04000000
        }
    }
    pub(crate) const fn render_pool(k: u64) -> u64 {
        render_base(k)
    }
    pub(crate) const fn render_state(k: u64) -> u64 {
        render_base(k) + 0x2000000
    }
    pub(crate) const fn render_list(k: u64) -> u64 {
        render_base(k) + 0x2020000
    }
    pub(crate) const fn render_descriptor(k: u64) -> u64 {
        render_base(k) + 0x2030000
    }
    pub(crate) const fn render_pb_pages(k: u64) -> u64 {
        render_base(k) + 0x2040000
    }
    // SCS-capable render allocators: the list and descriptor in their render unit's interval
    // past its records, at the base allocator's offsets from its state. The UMA pool and
    // freelist are in the allocator's SCS pool window ([`scs_render_pool`]).
    pub(crate) const fn scs_render_records(k: u64) -> u64 {
        render_base(k) + 0x2100000
    }
    /// Kernel-placed client objects (PB page lists, `Objects::alloc_dynamic_client`).
    pub(crate) const DYNAMIC: Range<u64> = 0x7040000000..0x7060000000;
    /// SCS pool windows, after [`DYNAMIC`] and up to the CT window: each holds one
    /// SCS-capable UMA pool and, right after it, its freelist (`rt::window_bytes`). Windows 0
    /// and 1 are the SCS compute units', 2 and 3 the SCS render allocators'.
    pub(crate) const SCS_POOLS: u64 = 0x7060000000;
    pub(crate) const SCS_POOL_WINDOW: u64 = 0x8000000;
    pub(crate) const SCS_POOL_WINDOWS: u64 = 4;
    pub(crate) const fn scs_compute_pool(k: u64) -> u64 {
        SCS_POOLS + k * SCS_POOL_WINDOW
    }
    pub(crate) const fn scs_render_pool(k: u64) -> u64 {
        SCS_POOLS + (2 + k) * SCS_POOL_WINDOW
    }
    /// Offsets of a render allocator's records from its state.
    pub(crate) const RENDER_LIST_OFF: u64 = render_list(0) - render_state(0);
    pub(crate) const RENDER_DESCRIPTOR_OFF: u64 = render_descriptor(0) - render_state(0);
    pub(crate) const PB_HEAP: u64 = 0x70c0000000;
    pub(crate) const PB_HEAP_SIZE: u64 = 0x10000000;
    pub(crate) const fn pb_heap(k: u64) -> u64 {
        PB_HEAP + k * PB_HEAP_SIZE
    }
    pub(crate) const PB_BLOCK_LIST: u64 = 0x70e0000000;
    pub(crate) const PB_PAGE_LIST: u64 = 0x7012040000;
    pub(crate) const PB_PAGE_LIST_SIZE: u64 = 0x8000;

    // Host-chosen client VA layout for render pass resources within 0x7020000000..0x7040000000
    // (512 MiB). Firmware takes explicit 64-bit VAs for each pass object rather than indexing
    // by stride:
    // - Base slots `0..BASE_RENDER_SLOTS` keep 32 MiB intervals with full 16 MiB TPC / 8 MiB
    //   tilemap capacity for worst-case geometries.
    // - Extension slots `BASE_RENDER_SLOTS..RENDER_SLOTS` pack into 8 MiB intervals starting at
    //   `EXT_PASS_BASE` (`0x7024000000`) with 2 MiB TPC / 1 MiB tilemap capacity, fitting 48
    //   total pass slots (`0x7020000000..0x703b000000`, leaving 80 MiB free below 0x7040000000).
    pub(crate) const FS_WORK_PAGE: u64 = 0x7020000000;
    pub(crate) const FS_WORK_OFF: u64 = 0;
    pub(crate) const TA_WORK_PAGE: u64 = 0x7020004000;
    pub(crate) const TA_WORK_OFF: u64 = 0;
    pub(crate) const WORK_STRIDE: u64 = 0x02000000;
    pub(crate) const PASS_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const EXT_PASS_STRIDE: u64 = 0x00800000;
    pub(crate) const EXT_PASS_BASE: u64 =
        FS_WORK_PAGE + (super::sizes::BASE_RENDER_SLOTS as u64) * PASS_STRIDE;
    pub(crate) const PRE0_PAGE: u64 = 0x7020080000;
    pub(crate) const PRE1_PAGE: u64 = 0x70200a0000;
    pub(crate) const PRE2_PAGE: u64 = 0x70200c0000;
    pub(crate) const HEAP_META_PAGE: u64 = 0x7020100000;
    pub(crate) const SCENE_USER: u64 = 0x7020120000;
    pub(crate) const FS_SCRATCH: u64 = 0x7020140000;
    pub(crate) const AUX: u64 = 0x7020160000;
    pub(crate) const AUX_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const SCENE_LIST: u64 = 0x7020180000;
    pub(crate) const TPC: u64 = 0x7020400000;
    pub(crate) const TPC_SIZE: u64 = 0x1000000;
    pub(crate) const TILEMAP: u64 = 0x7021400000;
    pub(crate) const TILEMAP_SIZE: u64 = 0x800000;
    /// Extension pass slot TPC/tilemap sizes (2 MiB / 1 MiB, covering 4K multi-layer and 8K
    /// passes); base slots keep full `TPC_SIZE` / `TILEMAP_SIZE` for worst-case geometries.
    pub(crate) const TPC_SLOT_SIZE: u64 = 0x200000;
    pub(crate) const TILEMAP_SLOT_SIZE: u64 = 0x100000;
    pub(crate) const TPC_SLOT_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const EXT_TILEMAP_OFF: u64 = (TPC - FS_WORK_PAGE) + TPC_SLOT_SIZE;

    pub(crate) const fn pass_base(i: u64) -> u64 {
        if i < super::sizes::BASE_RENDER_SLOTS as u64 {
            FS_WORK_PAGE + i * PASS_STRIDE
        } else {
            EXT_PASS_BASE + (i - super::sizes::BASE_RENDER_SLOTS as u64) * EXT_PASS_STRIDE
        }
    }

    pub(crate) const fn pass_off(i: u64) -> u64 {
        pass_base(i) - FS_WORK_PAGE
    }

    pub(crate) const fn pass_tilemap(i: u64) -> u64 {
        if i < super::sizes::BASE_RENDER_SLOTS as u64 {
            TILEMAP + i * PASS_STRIDE
        } else {
            pass_base(i) + EXT_TILEMAP_OFF
        }
    }
    pub(crate) const CT: u64 = 0x7080000000;
    pub(crate) const CT_STRIDE: u64 = 0x1000000;
    pub(crate) const CT_SIZE: u64 = 0x400000;
}

/// Sizes and counts of the engine's objects.
pub(crate) mod sizes {
    /// Tile buffer limits of [`super::Config::max_tile_buffer`].
    pub(crate) const TILE_BUFFER_32K: u32 = 32 * 1024;
    pub(crate) const TILE_BUFFER_48K: u32 = 48 * 1024;
    pub(crate) const POOLS: usize = 16;
    pub(crate) const POOL_SIZE: u64 = 0x200000;
    pub(crate) const FREELIST_SEED: u32 = 8192;
    pub(crate) const QUEUE_CAP: u32 = 256;
    /// Independent firmware compute workqueue pipes sharing `Channel::Compute` and `sched_ctx`.
    /// Each pipe owns a disjoint `ComputeGraph` (`qinfo`, work ring, `qstate`, `fw_stamp`,
    /// `notifier`, `threshold`, `job_list`) and completion event (`work::compute::EVENTS`) so
    /// concurrent compute VMs lease disjoint workqueues instead of mixing ASIDs in one active
    /// `qinfo`. Pipe 0 exclusively owns `0..PIPE1_COMPUTE_START` for base-only and SCS VMs;
    /// secondary pipes `1..COMPUTE_PIPES` share `PIPE1_COMPUTE_START..COMPUTE_UNITS` (18 units
    /// for 7 secondary pipes, leaving 11 active units beyond the 7 undisplaced retired leases).
    pub(crate) const COMPUTE_PIPES: usize = 8;
    /// Maximum concurrent compute pipes one VM may occupy at once, preserving the render
    /// ring-entry bound (`MAX_RENDER_RING_ENTRIES = 5`) since cross-VM render/compute drains.
    pub(crate) const MAX_VM_COMPUTE_PIPES: usize = 2;
    const _: () = assert!(COMPUTE_PIPES >= 2 && COMPUTE_PIPES <= 16);
    const _: () = assert!(MAX_VM_COMPUTE_PIPES >= 2 && MAX_VM_COMPUTE_PIPES <= COMPUTE_PIPES);
    const _: () = assert!(COMPUTE_UNITS - PIPE1_COMPUTE_START >= (COMPUTE_PIPES - 1) * 2);
    /// Compute units claimable in one batched compute prefix (matches `COMPUTE_UNITS`).
    pub(crate) const COMPUTE_SLOTS: usize = COMPUTE_UNITS;
    /// Base physical PB/UMA allocator slots allocated per VM on first render.
    pub(crate) const BASE_PB_SLOTS: usize = 2;
    /// Device-wide warm physical PB/UMA allocator slots (`parameter_id` `0..WARM_PB_SLOTS`,
    /// hardware UMA slots `33..33 + WARM_PB_SLOTS`). Each VM maps two base units (`local_idx`
    /// `0..BASE_PB_SLOTS`) bound to a rotating `[home, alt]` pair in this pool so up to
    /// `WARM_PB_SLOTS` concurrent render VMs stay warm without extra per-VM memory.
    pub(crate) const WARM_PB_SLOTS: usize = 16;
    /// Lazily allocated recovery PB/UMA allocator slots (`WARM_PB_SLOTS..PB_SLOTS`, mapped at
    /// per-VM local VA indices `BASE_PB_SLOTS..MAX_VM_PB_SLOTS`) claimed only after a render
    /// hang permanently quarantines an allocator slot.
    pub(crate) const EXT_PB_SLOTS: usize = 4;
    /// Maximum PB/UMA allocator slots mapped in one VM's client VA space (`render_base(0..6)`).
    pub(crate) const MAX_VM_PB_SLOTS: usize = BASE_PB_SLOTS + EXT_PB_SLOTS;
    /// Total device-wide physical PB/UMA allocator slots (`parameter_id` `0..PB_SLOTS`, hardware
    /// UMA slots `RENDER_UMA_SLOT..RENDER_UMA_SLOT + PB_SLOTS`).
    pub(crate) const PB_SLOTS: usize = WARM_PB_SLOTS + EXT_PB_SLOTS;
    const _: () = assert!(WARM_PB_SLOTS >= BASE_PB_SLOTS);
    const _: () = assert!(MAX_VM_PB_SLOTS >= BASE_PB_SLOTS && MAX_VM_PB_SLOTS <= 6);
    const _: () = assert!(PB_SLOTS <= 32);
    const _: () =
        assert!(super::client::compute_context(1) + 0x40000 <= super::client::render_base(4));
    const _: () =
        assert!(super::client::render_descriptor(3) + 0x4000 <= super::client::FS_WORK_PAGE);
    const _: () =
        assert!(super::client::render_descriptor(5) + 0x4000 <= super::client::render_base(0));
    /// Worst-case firmware work-ring entries consumed per pipelined render pass on TA
    /// (`prev_barrier`, `ta_fs_barrier`, up to `COMPUTE_PIPES` `compute_barrier` entries,
    /// `ta_work`) and FS (`fs_dep`, up to `COMPUTE_PIPES` `fs_compute_barrier` entries,
    /// `own_barrier`, `fs_work`). `InitBM` runs only on an idle engine (`initbm` + `ta_work` = 2
    /// entries).
    pub(crate) const MAX_RENDER_RING_ENTRIES: usize = 5;
    /// Worst-case firmware work-ring entries per compute command (`fs2c` barrier + cross-pipe
    /// `c2c` barrier + `work`).
    pub(crate) const MAX_COMPUTE_RING_ENTRIES: usize = 3;
    /// Base render pass slots allocated on first render (full TPC/tilemap capacity).
    pub(crate) const BASE_RENDER_SLOTS: usize = 2;
    /// Retained render pass slots per VM at most. A slot is reused only after a later render
    /// retired (displacing it), so `n` slots keep up to `n - 1` renders in flight.
    ///
    /// Sized inside both the host client-VA window (`0x7020000000..0x7040000000`) and firmware
    /// queue/recovery capacities: `47` in-flight passes consume at most `47 * 4 = 188` entries of
    /// each 256-entry TA/FS work ring (`QUEUE_CAP = 256`, 255 usable, ring cap `63`), use
    /// per-allocator hardware UMA slots `RENDER_UMA_SLOT + slot` (`33..38`, strictly inside the
    /// firmware render slot range `33..64`), post at most `FLIGHTS = 32` `SubmitRecord`s (vs
    /// `AHEAD_RECORDS = 128` / `RING_SLOTS = 256`), and stay far below the firmware recovery
    /// `snapshot_entries = 448` limit.
    pub(crate) const RENDER_SLOTS: usize = 48;
    const _: () = assert!(RENDER_SLOTS >= BASE_RENDER_SLOTS && RENDER_SLOTS <= 64);
    const _: () = assert!(super::engine::RENDER_UMA_SLOT + (PB_SLOTS as u32) <= 65);
    const _: () = assert!((RENDER_SLOTS - 1) * MAX_RENDER_RING_ENTRIES < QUEUE_CAP as usize);
    const _: () = assert!(super::client::AUX + 0x20000 <= super::client::SCENE_LIST);
    const _: () = assert!(super::client::SCENE_LIST + 0x4000 <= super::client::TPC);
    const _: () = assert!(super::client::TPC + super::client::TPC_SIZE <= super::client::TILEMAP);
    const _: () = assert!(
        super::client::TILEMAP + super::client::TILEMAP_SIZE
            <= super::client::FS_WORK_PAGE + super::client::PASS_STRIDE
    );
    const _: () = assert!(
        super::client::EXT_TILEMAP_OFF + super::client::TILEMAP_SLOT_SIZE
            <= super::client::EXT_PASS_STRIDE
    );
    const _: () = assert!(super::client::pass_base(RENDER_SLOTS as u64) <= 0x7040000000);
    pub(crate) const CT_BUFS: usize = 0;
    pub(crate) const CLUSTER_PB_PAGES: u32 = 0x3c;
    pub(crate) const MAX_ATTACHMENTS: usize = 16;
    pub(crate) const MAX_CLIENT: usize = 512;
    pub(crate) const PB_PAGES: u32 = 8192;
    pub(crate) const PB_BLOCKS: u32 = 2048;
    pub(crate) const PB_LIST_RECORDS: u32 = 4096;
    pub(crate) const PB_GROW_STEP: u32 = 10;
    /// Published prefixes at most. Warm render commands may pipeline across VMs on disjoint
    /// hardware UMA slots, compute only behind its own VM's work, while cold `InitBM` renders
    /// and recovery paths drain.
    pub(crate) const FLIGHTS: usize = 32;
    /// Retained compute units per VM at most; each unit owns its own hardware UMA slot
    /// (`COMPUTE_UMA_SLOT + index`, fitting in `1..RENDER_UMA_SLOT = 33`). A unit is reused
    /// only after a later publication retired (it displaced the unit), so `n` units keep up to
    /// `n - 1` compute commands in flight (`23 * 2 = 46` entries of the 256-entry compute work
    /// ring `QUEUE_CAP`, well inside `AHEAD_RECORDS = 128`, `RING_SLOTS = 256`, and recovery
    /// `snapshot_entries = 448`).
    pub(crate) const COMPUTE_UNITS: usize = 24;
    const _: () = assert!(COMPUTE_UNITS >= 2);
    const _: () = assert!((COMPUTE_UNITS - 1) * MAX_COMPUTE_RING_ENTRIES < QUEUE_CAP as usize);
    const _: () = assert!(
        super::client::compute_context(2) + 0x40000 <= super::client::compute_base(3)
    );
    const _: () = assert!(
        super::client::compute_base(COMPUTE_UNITS as u64) <= super::client::PB_HEAP
    );
    const _: () = assert!(
        super::engine::COMPUTE_UMA_SLOT + (COMPUTE_UNITS as u32)
            <= super::engine::RENDER_UMA_SLOT
    );
    /// SCS-capable compute units per VM, bound dynamically to healthy compute ledger indices
    /// (and UMA slots) preferring pipe-0-exclusive indices `0..PIPE1_COMPUTE_START`.
    pub(crate) const SCS_COMPUTE_UNITS: usize = 2;
    /// First hardware compute ledger index usable by secondary compute pipes (`pipe >= 1`).
    /// Keeping `0..PIPE1_COMPUTE_START` exclusive to pipe 0 guarantees that pipe 1 never holds
    /// an undisplaced lease on those units, so 2-unit base-only and SCS compute VMs always have
    /// at least one claimable unit across up to `PIPE1_COMPUTE_START - 2` permanent quarantines.
    pub(crate) const PIPE1_COMPUTE_START: usize = 6;
    const _: () = assert!(
        PIPE1_COMPUTE_START >= SCS_COMPUTE_UNITS && PIPE1_COMPUTE_START + 2 <= COMPUTE_UNITS
    );
    /// Bytes of a base UMA pool (its seeds of 4 KiB).
    pub(crate) const POOL_BYTES: u64 = 0x2000000;
    const _: () = assert!(
        super::client::compute_base(COMPUTE_UNITS as u64) <= super::client::scs_compute_records(0)
            && super::client::scs_compute_records(SCS_COMPUTE_UNITS as u64 - 1)
                + super::client::COMPUTE_CONTEXT_OFF
                + 0x40000
                <= super::client::scs_compute_records(SCS_COMPUTE_UNITS as u64)
            && super::client::scs_compute_records(SCS_COMPUTE_UNITS as u64)
                <= super::client::PB_HEAP
            && super::client::WORK_PAGE
                + (COMPUTE_UNITS + SCS_COMPUTE_UNITS) as u64 * super::client::CWORK_STRIDE
                <= super::client::compute_base(0)
    );
    const _: () = assert!(
        super::client::render_pb_pages(0) + super::client::PB_PAGE_LIST_SIZE
            <= super::client::scs_render_records(0)
            && super::client::scs_render_records(0) + super::client::RENDER_DESCRIPTOR_OFF + 0x4000
                <= super::client::render_base(1)
            && super::client::scs_render_records(BASE_PB_SLOTS as u64 - 1)
                + super::client::RENDER_DESCRIPTOR_OFF
                + 0x4000
                <= super::client::render_base(BASE_PB_SLOTS as u64)
    );
    // One window per SCS compute unit and render allocator, between the dynamic range and the
    // CT window.
    const _: () = assert!(
        super::client::DYNAMIC.end <= super::client::scs_compute_pool(0)
            && super::client::scs_compute_pool(SCS_COMPUTE_UNITS as u64)
                == super::client::scs_render_pool(0)
            && super::client::scs_render_pool(BASE_PB_SLOTS as u64)
                == super::client::SCS_POOLS
                    + super::client::SCS_POOL_WINDOWS * super::client::SCS_POOL_WINDOW
            && super::client::scs_render_pool(BASE_PB_SLOTS as u64) <= super::client::CT
            && super::client::pass_base(RENDER_SLOTS as u64) <= super::client::DYNAMIC.start
    );
    /// Maximum jobs folded into one barrier-free compute horizon prefix.
    pub(crate) const HORIZON_JOBS: u32 = COMPUTE_UNITS as u32;
    /// Maximum commands accumulated in a busy-state horizon micro-coalescing batch before
    /// immediate publication, and maximum hold window (18 us) while a queue already has >= 2
    /// active commands in flight.
    pub(crate) const COALESCE_MAX_CMDS: usize = 6;
    pub(crate) const COALESCE_WINDOW_US: u32 = 18;
    pub(crate) const COALESCE_WINDOW_NS: u64 = (COALESCE_WINDOW_US as u64) * 1000;
    /// Per-queue retired CDM execution EWMA threshold (microseconds) at or above which
    /// barrier-free compute fans out across both CDM pipes instead of staying 1-pipe affine.
    pub(crate) const COMPUTE_HEAVY_EMA_US: u32 = 200;
    /// Elapsed wall-clock time (nanoseconds) on a sole active single-command compute flight
    /// that immediately classifies the queue as heavy before its first retirement updates EWMA.
    pub(crate) const COMPUTE_HEAVY_INFLIGHT_NS: u64 = 180_000;
    /// Jobs admitted to the engine per logical queue: enough to keep deep queues (`d=64`)
    /// queued back to back without scheduler backpressure stalls; exact prior-command
    /// dependencies still gate each one.
    pub(crate) const SCHEDULER_CREDITS: u32 = 64;
    /// Unconsumed records per 256-entry channel ring a prefix may be posted behind.
    pub(crate) const AHEAD_RECORDS: u32 = 128;
    /// Policy bound on logical queues reported in QUEUE_LIMITS (not a firmware limit).
    pub(crate) const MAX_QUEUES: u32 = 58;
    /// TTBAT user slots: user VMs hold one each for their lifetime (ASIDs 1..63).
    pub(crate) const MAX_VMS: u32 = 63;
}

/// Firmware channels, doorbells, seeds and identifiers.
pub(crate) mod engine {
    pub(crate) const DOORBELL: u64 = 0x0083000000000000;
    pub(crate) const DEVCTL_INIT_DOORBELL: u64 = DOORBELL | 0x11;
    pub(crate) const DEVCTL_DOORBELL: u64 = 0x0084000000000011;
    pub(crate) const RECEIVE_DOORBELL: u64 = DOORBELL | 0x10;
    pub(crate) const G15S_RECEIVE_DOORBELL: u64 = 0x0087000000000010;
    pub(crate) const EVENT_SLOT: u32 = 2;
    pub(crate) const COMPUTE_UMA_SLOT: u32 = 1;
    pub(crate) const RENDER_UMA_SLOT: u32 = 33;
    pub(crate) const COMPUTE_SEED: u32 = 0xc1000000;
    pub(crate) const TA_SEED: u32 = 0x7a000000;
    pub(crate) const FS_SEED: u32 = 0x3d000000;
    pub(crate) const COMPUTE_UUID: u32 = 0xc10000;
    pub(crate) const TA_UUID: u32 = 0x7a0000;
    pub(crate) const FS_UUID: u32 = 0x3d0000;
}

/// Active M4 policy. Protocol fields and cache attributes live in their wire builders;
/// unsupported G15 batching, clustering and recovery controls are deliberately absent.
pub(crate) mod tuning {
    /// Command objects use uncached mappings from creation, including work images,
    /// microsequences and the embedded compute manager. Session-global initdata
    /// keeps its own explicit cache classes.
    pub(crate) const FW_CACHED: bool = false;
    /// Handle EP 0x20 drain requests in the mailbox interrupt.
    pub(crate) const EARLY_WAKE: bool = true;
    /// Scheduler timeout per job, ms; the engine's deadlines own failure (always "no hang").
    pub(crate) const SCHED_TIMEOUT_MS: u32 = 3000;
}
