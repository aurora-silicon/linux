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

/// A qualified GPU firmware build, identified by its loaded TEXT.
pub(crate) struct FirmwareBuild {
    pub(crate) name: &'static str,
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    /// Offset of the GKTS marker in TEXT; the 8 entropy bytes after it are zeroed before hashing.
    pub(crate) marker: u32,
    pub(crate) text_sha256: [u8; 32],
}

/// The J713/T8132 configuration. Hardware admission also checks the exact TEXT image.
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
                marker: 0x5c8df,
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
                marker: 0x5c8df,
                text_sha256: [
                    0x3b, 0x36, 0x8e, 0xe4, 0x2d, 0xcc, 0xc6, 0x26, 0xe8, 0x49, 0x67, 0xff, 0x40,
                    0x87, 0x64, 0x71, 0x0a, 0x64, 0xbf, 0xf1, 0x59, 0x4f, 0x3c, 0xfd, 0x2c, 0x6e,
                    0x4d, 0x34, 0xd0, 0x65, 0x24, 0x8b,
                ],
            },
        ],
    };
}

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

/// PPL handoff magic.
pub(crate) const PPL_MAGIC: u64 = 0x4b1d000000000002;

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

    // Two persistent compute units, each in its own 64 MiB interval and reused across
    // application queues. Every alias is inside Mesa's reserved driver hole.
    pub(crate) const fn compute_base(k: u64) -> u64 {
        0x7001000000 + k * 0x04000000
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
    /// The qualified M4 protocol permits only one published command at a time.
    pub(crate) const FLIGHTS: usize = 1;
    /// Inherited diagnostic histogram width; M4 does not fold jobs into a compute horizon.
    pub(crate) const HORIZON_JOBS: u32 = 1;
    /// Two jobs may be admitted per logical queue so exact prior-command dependencies can
    /// advance independently of a preceding job's aggregate fence. Firmware stays serialized.
    pub(crate) const SCHEDULER_CREDITS: u32 = 2;
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
