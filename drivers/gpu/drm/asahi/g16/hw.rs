// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(
    dead_code,
    reason = "Firmware layout catalog also describes fields used by host wire regressions."
)]

//! T8132 (G16G) hardware and firmware-ABI constants (layout values are shared by several
//! modules; change one only together with its users). The tuned policy values are in
//! [`tuning`].

use core::ops::Range;

/// Identification register values of a qualified G16G.
pub(crate) struct HwId {
    /// SGX identity word (`sgx + 0xd04000`).
    pub(crate) version: u32,
    /// SGX layout word (`sgx + 0xd04010`).
    pub(crate) counts: u32,
}

pub(crate) use crate::fwid::FirmwareBuild;

/// The J713/J773g T8132 configuration. Hardware admission also checks the exact TEXT image.
pub(crate) mod t8132 {
    use super::super::Config;
    use super::{FirmwareBuild, HwId};

    pub(crate) const CONFIG: Config = Config {
        chip_id: 0x8132,
        gpu_variant: crate::hw::GpuVariant::G,
        gpu_revision: 0x10,
        uat_ias: 42,
        uat_oas: 42,
        num_clusters: 1,
        num_cores_per_cluster: 10,
        hw_ids: &[HwId {
            version: 0x0a021100,
            counts: 0x0011010a,
        }],
        firmware: &[
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
    };

    /// What iBoot writes into the 25G83 build b000 TEXT it loads, in TEXT order:
    /// - the header word at 0x224, which changes from boot to boot, and the DATA address at 0x23c;
    /// - the value of each of the 39 boot parameter records the header at 0x22c places from
    ///   0x5c8df to 0x5cb10 (a tag, a 32-bit length and the value: carveout addresses below the
    ///   top of DRAM, a per-boot slide, the entropy);
    /// - the entry count and the 0x34c-byte body of the two tunable tables at 0x5cb10 and 0x5ce60
    ///   (headers 01/03 03 2a), which the shipped image leaves empty.
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
    /// places 0x413c bytes later (`_rtk_patchbay` at 0x60a1b, `_rtk_tunables` at 0x60c4c).
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
    /// Every leaf the boot loader builds in the firmware's own page tables, in VA order, and
    /// the only leaves cold admission accepts (none: the firmware builds its own tree at
    /// start); `None` until known, which refuses the firmware.
    pub(crate) leaves: Option<&'static [Leaves]>,
    /// The PPL handoff page's magic words; `None` until known, which refuses the firmware.
    pub(crate) ppl_magic: Option<PplMagic>,
    /// How the firmware's register table in loaded DATA is filled; `None` until known, which
    /// refuses the firmware.
    pub(crate) loader_table: Option<LoaderTable>,
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
};

/// 26B123 (macOS 27.2, also 26B131): __TEXT 0x64000, __DATA 0xd0000 and a __DATA_SHARED_RO
/// 0x8000 right after it (aura's handoff version 2; `fw-data` covers both, so the RTKit crash
/// log at the start of the shared read-only segment lies inside it).
///
/// The boot loader leaves the page-table region zero: the RTKit start code takes the tables
/// base from a boot-filled patchbay record, builds root[0], the L2 at +0x4000 and the L3 at
/// +0x8000 with the TEXT and DATA leaves itself and sets TTBR1_EL1. So only an empty tree is
/// admitted. When it sets up its MMU the firmware stores 0x4b1d000000000003 at +8 of the PPL
/// handoff page (the 25G83 firmware never wrote there). The AP announces the same word at +0
/// before RUN, as the G14 firmware-27.2 path does on hardware (linux#2544: AP and firmware
/// exchange 0x4b1d000000000003 through these two words, the firmware answering before the
/// RTKit handshake). The host leaves loaded DATA alone: the firmware copies every named
/// register-map slot and the SGX window from HW data while it parses InitData (both builds),
/// the T6041 build of this firmware starts on hardware without any DATA write (linux#1954),
/// and 27.2 marks DATA data-isolated. The 25G83 table address moved to DATA + 0xc72e8
/// (60 slots), should a write turn out to be needed.
pub(crate) const FW_26B123: FwLayout = FwLayout {
    name: "26B123",
    handoff_version: 2,
    segment_vas: &[UPPER, UPPER + 0x64000, UPPER + 0x134000],
    segment_flags: &[1, 0x10, 0],
    text_size: 0x64000,
    data_size: 0xd8000,
    abi: FwAbi::V26B123,
    // The firmware builds its own tree at start; admit only an empty tree.
    leaves: Some(&[]),
    ppl_magic: Some(PplMagic {
        ap: PPL_MAGIC_27_2,
        firmware: PPL_MAGIC_27_2,
        answers: true,
    }),
    loader_table: Some(LoaderTable::Firmware),
};

/// The firmware layouts this backend knows, newest last.
pub(crate) const FW_LAYOUTS: &[&FwLayout] = &[&FW_25G83, &FW_26B123];

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

    // Persistent compute units, each in its own 64 MiB interval and reused across
    // application queues. Units 0 and 1 keep their original placement below the render
    // allocators; further units (up to [`super::sizes::COMPUTE_UNITS`]) live between the
    // unused CT window and the PB heap, clear of the dynamic PB page-list range
    // 0x7040000000..0x7080000000. Every alias is inside Mesa's reserved driver hole.
    pub(crate) const fn compute_base(k: u64) -> u64 {
        if k < 2 {
            0x7001000000 + k * 0x04000000
        } else {
            0x7090000000 + (k - 2) * 0x04000000
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

    // Render allocator units contain one 32 MiB UMA pool and its persistent lists.
    pub(crate) const fn render_base(k: u64) -> u64 {
        0x7010000000 + k * 0x08000000
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
    pub(crate) const PB_HEAP: u64 = 0x70c0000000;
    pub(crate) const PB_HEAP_SIZE: u64 = 0x10000000;
    pub(crate) const fn pb_heap(k: u64) -> u64 {
        PB_HEAP + k * PB_HEAP_SIZE
    }
    pub(crate) const PB_BLOCK_LIST: u64 = 0x70e0000000;
    pub(crate) const PB_PAGE_LIST: u64 = 0x7012040000;
    pub(crate) const PB_PAGE_LIST_SIZE: u64 = 0x8000;

    // Render pass resources, each in a distinct 256 MiB interval.
    pub(crate) const FS_WORK_PAGE: u64 = 0x7020000000;
    pub(crate) const FS_WORK_OFF: u64 = 0;
    pub(crate) const TA_WORK_PAGE: u64 = 0x7020004000;
    pub(crate) const TA_WORK_OFF: u64 = 0;
    pub(crate) const WORK_STRIDE: u64 = 0x10000000;
    pub(crate) const PASS_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const PRE0_PAGE: u64 = 0x7020080000;
    pub(crate) const PRE1_PAGE: u64 = 0x70200a0000;
    pub(crate) const PRE2_PAGE: u64 = 0x70200c0000;
    pub(crate) const HEAP_META_PAGE: u64 = 0x7020100000;
    pub(crate) const SCENE_USER: u64 = 0x7020120000;
    pub(crate) const FS_SCRATCH: u64 = 0x7020140000;
    pub(crate) const AUX: u64 = 0x7020160000;
    pub(crate) const AUX_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const TPC: u64 = 0x7024000000;
    pub(crate) const TPC_SIZE: u64 = 0x1000000;
    pub(crate) const TILEMAP: u64 = 0x7026000000;
    pub(crate) const TILEMAP_SIZE: u64 = 0x800000;
    pub(crate) const TPC_SLOT_SIZE: u64 = TPC_SIZE;
    pub(crate) const TILEMAP_SLOT_SIZE: u64 = TILEMAP_SIZE;
    pub(crate) const TPC_SLOT_STRIDE: u64 = WORK_STRIDE;
    pub(crate) const CT: u64 = 0x7080000000;
    pub(crate) const CT_STRIDE: u64 = 0x1000000;
    pub(crate) const CT_SIZE: u64 = 0x400000;
}

/// Sizes and counts of the engine's objects.
pub(crate) mod sizes {
    pub(crate) const POOLS: usize = 16;
    pub(crate) const POOL_SIZE: u64 = 0x200000;
    pub(crate) const FREELIST_SEED: u32 = 8192;
    pub(crate) const QUEUE_CAP: u32 = 256;
    pub(crate) const COMPUTE_SLOTS: usize = 2;
    pub(crate) const RENDER_SLOTS: usize = 2;
    pub(crate) const CT_BUFS: usize = 0;
    pub(crate) const CLUSTER_PB_PAGES: u32 = 0x3c;
    pub(crate) const MAX_ATTACHMENTS: usize = 16;
    pub(crate) const MAX_CLIENT: usize = 320;
    pub(crate) const PB_PAGES: u32 = 8192;
    pub(crate) const PB_BLOCKS: u32 = 2048;
    pub(crate) const PB_LIST_RECORDS: u32 = 4096;
    pub(crate) const PB_GROW_STEP: u32 = 10;
    /// Published prefixes at most. A render is published only with nothing else in flight;
    /// compute commands may queue behind each other on the one firmware compute queue, each
    /// in its own retained unit ([`COMPUTE_UNITS`]).
    pub(crate) const FLIGHTS: usize = 16;
    /// Retained compute units per VM at most, all in hardware UMA slot 1. A unit is reused
    /// only after a later publication retired (it displaced the unit), so `n` units keep up
    /// to `n - 1` compute commands in flight.
    pub(crate) const COMPUTE_UNITS: usize = 8;
    const _: () = assert!(super::client::compute_base(COMPUTE_UNITS as u64) <= super::client::PB_HEAP);
    /// Inherited diagnostic histogram width; M4 does not fold jobs into a compute horizon.
    pub(crate) const HORIZON_JOBS: u32 = 1;
    /// Jobs admitted to the engine per logical queue: enough to keep a queue's compute
    /// commands in flight back to back; exact prior-command dependencies still gate each one.
    pub(crate) const SCHEDULER_CREDITS: u32 = 16;
    /// Unconsumed records per 256-entry channel ring a prefix may be posted behind.
    pub(crate) const AHEAD_RECORDS: u32 = 64;
    /// Policy bound on logical queues reported in QUEUE_LIMITS (not a firmware limit).
    pub(crate) const MAX_QUEUES: u32 = 58;
    /// TTBAT user slots: user VMs hold one each for their lifetime (ASIDs 1..63).
    pub(crate) const MAX_VMS: u32 = 63;
}

/// Firmware channels, doorbells, seeds and identifiers.
pub(crate) mod engine {
    pub(crate) const DOORBELL: u64 = 0x0083000000000000;
    pub(crate) const DEVCTL_DOORBELL: u64 = 0x0084000000000011;
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
