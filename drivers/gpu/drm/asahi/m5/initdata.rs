// SPDX-License-Identifier: GPL-2.0-only OR MIT

use core::result::Result;

/// Root size for the primary (`gfx-asc`) instance.
pub(crate) const ROOT_SIZE_PRIMARY: usize = 0x0b8;

/// Root size for the secondary (`gfx1-asc`) instance; the extra 0x10 bytes are
/// the two secondary-only pointers at `+0xb8`/`+0xc0`.
pub(crate) const ROOT_SIZE_SECONDARY: usize = 0x0c8;

pub(crate) const MAIN_CONFIG_SIZE: usize = 0x600;

pub(crate) const REGION_A_SIZE: usize = 0x4000;

pub(crate) const REGION_C_SIZE: usize = 0x1000;

pub(crate) const STATUS_BLOCK_SIZE: usize = 0x80;

pub(crate) const HW_DATA_BUNDLE_MIN_ALLOC: usize = 0x10000;

/// The native placement additionally hosts the twelve work rings and both main
/// configuration objects inside the same cluster, which then spans this much
/// (see [`WORK_RING_BUNDLE_OFFSETS`], [`NATIVE_PRIMARY_MAIN_BUNDLE_OFFSET`]).
pub(crate) const NATIVE_SHARED_CLUSTER_SIZE: usize = 0x28000;

/// The same tuple as the little-endian u64 the firmware compares in one go.
pub(crate) const INITDATA_VERSION_WORD: u64 = 0x0c8a_a322_0396_04c0;

/// Target/build-specific root encoding. This is a serialization choice, not
/// permission to start firmware or reuse the other target's hardware tables.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RootProfile {
    J700G17P,
    /// J813 Mac17,3 / 25G76, RTKit-3255.160.4.release. Both roots were captured
    /// before INBOX1 publication. Independently confirmed by the loaded GFX
    /// version comparison at +0x1fa5c and GFX1 at +0x21140.
    J813G17G25G76,
    /// J873 Mac18,5 / T8152 / G18G / 26A428, RTKit_release-3514.0.15
    /// (armfw_g18g-b000 on GFX, armfw1_g18g-b010 on GFX1). No capture of a
    /// native root exists; the encoding is source-constructed from the 26A428
    /// kext and firmware (see the "J873 / G18G" section below). Selecting it
    /// changes nothing for the other two profiles.
    J873G18G26A428,
}

impl RootProfile {
    pub(crate) const fn version_word(self) -> u64 {
        match self {
            Self::J700G17P => INITDATA_VERSION_WORD,
            Self::J813G17G25G76 => 0x0c8a_a322_039e_04c0,
            Self::J873G18G26A428 => G18G_26A428_INITDATA_VERSION_WORD,
        }
    }

    /// Bytes of the root object for `role`. J700/J813 keep the historical
    /// 0xb8/0xc8 split. G18G sizes both roots to 0xc8 because both firmware
    /// images copy 0xc8 bytes from each root (FW_PROVEN b000 0x21664-0x216a8).
    pub(crate) const fn root_size(self, role: InstanceRole) -> usize {
        match self {
            Self::J873G18G26A428 => G18G_26A428_ROOT_SIZE,
            Self::J700G17P | Self::J813G17G25G76 => root_size(role),
        }
    }

    /// Opcode of the one opening device-control record staged at main
    /// `+0x4c0`. G18G renumbered the opening handler: 0x16 is now an
    /// unrelated handler and 0x15 reaches the opening (FW_PROVEN jump table
    /// 0xfffffc0000006c60, handler b000 0x3ed0). The secondary keeps 0x2a.
    pub(crate) const fn control_opening_opcode(self, role: InstanceRole) -> u32 {
        match (self, role) {
            (Self::J873G18G26A428, InstanceRole::Primary) => {
                G18G_26A428_CONTROL_OPENING_OPCODE_PRIMARY
            }
            (Self::J873G18G26A428, InstanceRole::Secondary) => {
                G18G_26A428_CONTROL_OPENING_OPCODE_SECONDARY
            }
            (_, InstanceRole::Primary) => CONTROL_OPENING_OPCODE_PRIMARY,
            (_, InstanceRole::Secondary) => CONTROL_OPENING_OPCODE_SECONDARY,
        }
    }

    /// Bundle offsets of the five views published at main `+0x254`.
    pub(crate) const fn bundle_view_offsets(self) -> [usize; 5] {
        match self {
            Self::J873G18G26A428 => G18G_26A428_BUNDLE_VIEW_OFFSETS,
            Self::J700G17P | Self::J813G17G25G76 => BUNDLE_VIEW_OFFSETS,
        }
    }

    /// Extents each view must fit inside the bundle allocation.
    pub(crate) const fn bundle_view_extents(self) -> [usize; 5] {
        match self {
            Self::J873G18G26A428 => G18G_26A428_BUNDLE_VIEW_EXTENTS,
            Self::J700G17P | Self::J813G17G25G76 => BUNDLE_VIEW_EXTENTS,
        }
    }

    /// Bundle offset named twice by main `+0x08`/`+0x10` (the empty BRN
    /// table and the 0x88 block, which the allocator places together).
    pub(crate) const fn repeated_region_bundle_offset(self) -> usize {
        match self {
            Self::J873G18G26A428 => G18G_26A428_SHARED_LAYOUT.block_88,
            Self::J700G17P | Self::J813G17G25G76 => MAIN_REPEATED_REGION_BUNDLE_OFFSET,
        }
    }

}

const INITDATA_ADDR_MASK: u64 = 0x0000_0fff_ffff_ffff;

const INITDATA_ADDR_PREFIX: u64 = 0xffff_f000_0000_0000;

const ROOT_VERSION: usize = 0x00;

const ROOT_REGION_A: usize = 0x08;

const ROOT_MAIN_CONFIG: usize = 0x18;

const ROOT_REGION_C: usize = 0x20;

const ROOT_INSTANCE_KIND: usize = 0x28;

const ROOT_CONSTANT_2C: usize = 0x2c;

const ROOT_PAGE_SIZE: usize = 0x30;

const ROOT_PAGE_BITS: usize = 0x32;

const ROOT_LEVEL_COUNT: usize = 0x33;

const ROOT_LEVELS: usize = 0x34;

/// Root `+0xa8`: Status A pointer. (The M5-derived model misused this slot as
/// a role pointer.)
const ROOT_STATUS_A: usize = 0xa8;

/// Root `+0xb0`: Status B pointer. Real for the primary, zero for the
/// secondary. (The M5-derived model misused it as a cross-role pointer.)
const ROOT_STATUS_B: usize = 0xb0;

const ROOT_SECONDARY_EXTRA_0: usize = 0xb8;

const ROOT_SECONDARY_EXTRA_1: usize = 0xc0;

pub(crate) const UAT_PAGE_SIZE: u16 = 0x4000;

pub(crate) const UAT_PAGE_BITS: u8 = 14;

pub(crate) const UAT_LEVEL_COUNT: usize = 3;

/// `(index_shift, entry_count)` for the three levels below the root,
/// confirmed against a live table walk.
pub(crate) const UAT_LEVELS: [(u8, u16); UAT_LEVEL_COUNT] = [(36, 64), (25, 2048), (14, 2048)];

pub(crate) const UAT_LEVEL_DESC_SIZE: usize = 0x20;

/// Level descriptor `+0x10`: physical mask, identical in all three.
pub(crate) const UAT_LEVEL_PHYS_MASK: u64 = 0x0000_03ff_ffff_c000;

const MAIN_HW_DATA: usize = 0x00;

/// The same repeated-region address appears at `+0x08` and `+0x10`; it is the
/// bundle base plus [`MAIN_REPEATED_REGION_BUNDLE_OFFSET`], not the bundle
/// base itself. It reads zero at handoff (firmware writes into it), so the
/// host only has to provide it mapped.
const MAIN_REPEATED_0: usize = 0x08;

const MAIN_REPEATED_1: usize = 0x10;

pub(crate) const MAIN_REPEATED_REGION_BUNDLE_OFFSET: usize = 0xc500;

const MAIN_CHANNEL_TABLE: usize = 0x20;

/// Main `+0x254`: five unaligned u64 views into the hardware-data bundle.
const MAIN_BUNDLE_VIEWS: usize = 0x254;

/// Main `+0x2d0`: six qwords of region views.
const MAIN_REGION_VIEWS: usize = 0x2d0;

/// Main `+0x300`: u32 4, secondary only.
const MAIN_SECONDARY_KIND: usize = 0x300;

const MAIN_SECONDARY_KIND_VALUE: u32 = 4;

/// Main `+0x3e0`: u32 0xff, primary only.
const MAIN_PRIMARY_MASK: usize = 0x3e0;

const MAIN_PRIMARY_MASK_VALUE: u32 = 0xff;

const MAIN_PRIMARY_SCALAR_32C: usize = 0x32c;

const MAIN_PRIMARY_SCALAR_344: usize = 0x344;

/// Main `+0x471`: unaligned u64, secondary only, pointing *into* bundle view 2
/// at [`SECONDARY_EXTRA_VIEW_OFFSET`] (not a fresh allocation — the M5-derived
/// model had this at `+0x481` backed by an invented 0x12688-byte buffer; both
/// were wrong).
const MAIN_SECONDARY_EXTRA_PTR: usize = 0x471;

pub(crate) const SECONDARY_EXTRA_VIEW_INDEX: usize = 2;

pub(crate) const SECONDARY_EXTRA_VIEW_OFFSET: usize = 0xe40;

/// Main `+0x4c0` is both a scalar and a place: the leading u32 of the
/// device-control ring, which is embedded in the main configuration object at
/// exactly this offset on both instances. Writing the opening opcode here *is*
/// staging the opening device-control record (records are 0x40 bytes; fields
/// past the opcode are zero). The control channel's producer counter must then
/// be set to the role-specific value returned by
/// [`control_opening_producer`] before the descriptor is handed over.
const MAIN_CONTROL_RING: usize = 0x4c0;

pub(crate) const CONTROL_OPENING_OPCODE_PRIMARY: u32 = 0x16;

pub(crate) const CONTROL_OPENING_OPCODE_SECONDARY: u32 = 0x2a;

/// Bundle offsets of the five views named at main `+0x254`. They are views of
/// the common bundle, not independent objects: view 0 overlaps the tail of
/// the hardware-data object, and views 3 and 4 overlap each other by all but
/// 0x80 bytes.
pub(crate) const BUNDLE_VIEW_OFFSETS: [usize; 5] = [0x2740, 0x3380, 0x4400, 0xbc80, 0xbd00];

/// Logical extents of the five views.
pub(crate) const BUNDLE_VIEW_EXTENTS: [usize; 5] = [0x18c0, 0x0c80, 0x3c00, 0x3380, 0x3300];

/// The context-0 mappings of the PB descriptor and UMA page-pool descriptor
/// tables published at main `+0x2d8` and `+0x2e8`, respectively. The matching
/// firmware-high mappings are published at `+0x2e0` and `+0x2f0`.
pub(crate) const REGION_VIEW_LOW_ADDRS: [u64; 2] = [0x70_0183_8000, 0x70_0184_0000];

pub(crate) const CHANNEL_TABLE_ENTRIES: usize = 17;

pub(crate) const CHANNEL_ENTRY_SIZE: usize = 0x20;

/// Channel entry layout: three *separate* 32-bit state-counter addresses at
/// `+0x00`/`+0x08`/`+0x10` (the counters themselves sit at 0x10 spacing in
/// their state block — do not collapse them into one state object), then the
/// ring base at `+0x18`.
pub(crate) const CHANNEL_ENTRY_STATE_COUNT: usize = 3;

pub(crate) const CHANNEL_STATE_SPACING: usize = 0x10;

/// Entries 0..11 are ordered TA_0, 3D_0, CL_0, TA_1, ... CL_3.
pub(crate) const WORK_CHANNEL_COUNT: usize = 12;

/// Entry 12 is device control. It continues the compact control-state grid
/// ([`WORK_STATE_GRID_OFFSETS`] has 13 entries, one more than there are work
/// channels), and its ring is embedded at main `+0x4c0`. The M5-derived model
/// refused this index, claiming a "cursor pair at +0x1a0/+0x1a8"; that was a
/// misread — those offsets are exactly this entry's state-counter addresses 0
/// and 1.
pub(crate) const CONTROL_CHANNEL_INDEX: usize = 12;

/// Entries 13 and 14 are report channels with split state objects: their
/// state addresses alias off status A rather than the compact grid, and the
/// firmware peer of each host-owned counter is at `+0x20` within its object.
pub(crate) const REPORT_CHANNEL_INDICES: [usize; 2] = [13, 14];

/// Entry 15 is partial: only its first state address, no ring. Entry 16 is
/// empty. Filling them like the rest declares channels that do not exist.
pub(crate) const PARTIAL_CHANNEL_INDEX: usize = 15;

/// Offsets of the channel state blocks (channels 0..=12) inside the compact
/// state grid. Native state is *not* in channel-table order: within each
/// workload group the work queues sit at descending offsets, and the control
/// channel's block follows at `+0x300`. Preserving these address identities is
/// part of the descriptor ABI.
pub(crate) const WORK_STATE_GRID_OFFSETS: [usize; 13] = [
    0x0c0, 0x1c0, 0x2c0, // TA_0, 3D_0, CL_0
    0x080, 0x180, 0x280, // TA_1, 3D_1, CL_1
    0x040, 0x140, 0x240, // TA_2, 3D_2, CL_2
    0x000, 0x100, 0x200, // TA_3, 3D_3, CL_3
    0x300, // device control
];

/// The primary status-B object runs from there to status A: `0xeb00` bytes.
pub(crate) const PRIMARY_STATUS_B_OBJECT_SIZE: usize = 0xeb00;

/// State-counter addresses of report channels 13/14, as offsets from status A.
pub(crate) const REPORT_STATE_STATUS_A_OFFSETS: [[usize; 3]; 2] =
    [[0x00040, 0x002c0, 0x00080], [0x00240, 0xa6ac0, 0x00280]];

/// Ring bases of report channels 13/14, as offsets from status A.
pub(crate) const REPORT_RING_STATUS_A_OFFSETS: [usize; 2] = [0x04ac0, 0xafac0];

/// Entry 15's sole state address, as an offset from status A.
pub(crate) const PARTIAL_STATE_STATUS_A_OFFSET: usize = 0x2d2c0;

/// Native bundle offsets of the twelve work rings (0x1800 each), in
/// channel-table order. These extend past [`HW_DATA_BUNDLE_MIN_ALLOC`]; a host
/// using this placement sizes the cluster [`NATIVE_SHARED_CLUSTER_SIZE`].
pub(crate) const WORK_RING_BUNDLE_OFFSETS: [usize; 12] = [
    0x10dc0, 0x16dc0, 0x1cdc0, 0x0f5c0, 0x155c0, 0x1b5c0, 0x0ddc0, 0x13dc0, 0x19dc0, 0x0c5c0,
    0x125c0, 0x185c0,
];

/// Native bundle offsets of the two main configuration objects.
pub(crate) const NATIVE_PRIMARY_MAIN_BUNDLE_OFFSET: usize = 0x1e5c0;

pub(crate) const NATIVE_SECONDARY_MAIN_BUNDLE_OFFSET: usize = 0x22a80;

pub(crate) const HW_REGISTER_MAP_ENTRY_SIZE: usize = 0x28;

pub(crate) const HW_REGISTER_MAP_SLOTS: usize = 53;

/// Two GPU mappings of one present completion-queue backing.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PKsmCompletionAliases {
    pub(crate) low: u64,
    pub(crate) high: u64,
}

pub(crate) const M5_25G76_COMPLETION_QUEUE_COUNT: usize = 4;

pub(crate) const M5_25G76_COMPLETION_BACKING_SIZE: usize = 0x800 * 0x40;

/// M5 publishes all four descriptors at hardware-data +0x2614, with ordinals
/// 0,1,2,3. Each names low/high mappings of its own retained 128-KiB backing.
/// The two native pre-handoff captures agree on every descriptor byte.
/// Construction only: this does not allocate, map, publish, or poll a queue.
pub(crate) fn encode_m5_25g76_completion_table(
    aliases: [G17PKsmCompletionAliases; M5_25G76_COMPLETION_QUEUE_COUNT],
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != M5_25G76_COMPLETION_QUEUE_COUNT * 0x20 {
        return Err(InitdataError::BufferSize);
    }
    let extent = M5_25G76_COMPLETION_BACKING_SIZE as u64;
    // Validate every range before mutating the output. Different queues must
    // not alias each other in either address space.
    for (i, alias) in aliases.iter().enumerate() {
        for address in [alias.low, alias.high] {
            required_address(address)?;
            let end = address.checked_add(extent).ok_or(InitdataError::AddressOutOfRange)?;
            required_address(end - 1)?;
        }
        for prior in &aliases[..i] {
            if (alias.low < prior.low + extent && prior.low < alias.low + extent)
                || (alias.high < prior.high + extent && prior.high < alias.high + extent)
            {
                return Err(InitdataError::PairAliasedObject);
            }
        }
    }
    out.fill(0);
    for (ordinal, alias) in aliases.iter().enumerate() {
        let offset = ordinal * 0x20;
        put_u64(out, offset, alias.low);
        put_u64(out, offset + 8, alias.high);
        put_u32(out, offset + 0x10, 0x800);
        put_u32(out, offset + 0x14, 0x40);
        put_u32(out, offset + 0x18, ordinal as u32);
    }
    Ok(())
}

/// Status block `+0x04` holds u32 1 pre-init. `+0x10`/`+0x14` are *post-ack
/// firmware state* and must be zero at handoff.
pub(crate) const STATUS_BLOCK_ONE_OFFSET: usize = 0x04;

pub(crate) const PRIMARY_STATUS_B_FWCTL_STATE: usize = 0x48e0;

pub(crate) const PRIMARY_STATUS_B_FWCTL_RING: usize = 0x48e8;

pub(crate) const PRIMARY_STATUS_B_CONFIG_HEADER: usize = 0xe434;

pub(crate) const PRIMARY_STATUS_B_CONFIG_OFFSET: usize = 0xe440;

/// The secondary root sits exactly this far above the primary root; the
/// secondary's initialisation message carries the primary's address plus this
/// delta in the same field, so the pair is not placed freely.
pub(crate) const SECONDARY_ROOT_DELTA: u64 = 0x8000;

/// Physical firmware instance encoded at root `+0x28`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum InstanceRole {
    /// `gfx-asc`: scheduler, device control, and all work channels.
    Primary = 0,
    /// `gfx1-asc`: control-only peer. Linux starts it after primary RTKit is ready.
    Secondary = 1,
}

pub(crate) const fn root_size(role: InstanceRole) -> usize {
    match role {
        InstanceRole::Primary => ROOT_SIZE_PRIMARY,
        InstanceRole::Secondary => ROOT_SIZE_SECONDARY,
    }
}

/// GPU virtual addresses of the objects both instances share.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SharedAddresses {
    /// Shared 16 KiB region A (root `+0x08`).
    pub(crate) region_a: u64,
    /// Shared region C (root `+0x20`), 0x1000 bytes.
    pub(crate) region_c: u64,
    /// Base of the contiguous hardware-data bundle. The hardware-data object
    /// sits at the base; the five main-config views, the repeated region, and
    /// the secondary's `+0x471` pointer are all derived offsets into it.
    pub(crate) hw_data_bundle: u64,
    /// Size of that one allocation; must be at least
    /// [`HW_DATA_BUNDLE_MIN_ALLOC`].
    pub(crate) hw_data_bundle_alloc: usize,
}

/// GPU virtual addresses of one instance's private objects.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct InstanceAddresses {
    /// This instance's root; its address rides the endpoint-0x20 doorbell.
    pub(crate) root: u64,
    pub(crate) main_config: u64,
    pub(crate) status_a: u64,
    /// Required for the primary; must be zero for the secondary (whose root
    /// still has the field, written zero).
    pub(crate) status_b: u64,
    /// Root `+0xb8`/`+0xc0`. Required for the secondary; the primary root is
    /// only 0xb8 bytes and cannot even hold them, so they must be zero there.
    pub(crate) secondary_extras: [u64; 2],
}

/// One channel-table entry: three state-counter addresses, then the ring base.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ChannelEntry {
    pub(crate) states: [u64; 3],
    pub(crate) ring: u64,
}

impl ChannelEntry {
    pub(crate) const EMPTY: ChannelEntry = ChannelEntry {
        states: [0; 3],
        ring: 0,
    };
}

/// The primary's global region views at main `+0x2d0`: a globally retained,
/// initially zero HWDSID accounting table (256 pairs of u32), followed by
/// the high halves of two low/high alias pairs (the low halves
/// are the fixed context-0 addresses in
/// [`REGION_VIEW_LOW_ADDRS`]). Each pair must map one physical page from both
/// address spaces — an invariant of the UAT mapping layer that cannot be
/// checked from the addresses alone.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PrimaryRegionViews {
    pub(crate) sentinel_high: u64,
    pub(crate) alias_high: [u64; 2],
}

/// M5 25G76 descriptor fields +0x10 and +0x14 are independent. In slot 39
/// native publishes 0x10000 and 0x8000 respectively. The older RegisterWindow
/// encoder duplicates one size and must not serialize this target's table.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5RegisterWindow {
    pub(crate) slot: usize,
    pub(crate) phys: u64,
    pub(crate) device_va: u64,
    pub(crate) size_10: u32,
    pub(crate) size_14: u32,
    pub(crate) unk_18: u64,
    pub(crate) flag: u32,
}

impl M5RegisterWindow {
    /// Actual M5 PTE walks cover size_10 rounded to 16 KiB, including the
    /// four pages of slot 39. size_14 is independently serialized firmware
    /// metadata and must not truncate that mapping to two pages.
    pub(crate) fn mapped_pages(&self) -> Result<(u64, u64, u64), InitdataError> {
        let mask = UAT_PAGE_SIZE as u64 - 1;
        if self.size_10 == 0 || self.phys & mask != self.device_va & mask {
            return Err(InitdataError::RegisterWindow);
        }
        let physical = self.phys & !mask;
        let virtual_address = self.device_va & !mask;
        let end = self.device_va.checked_add(self.size_10 as u64)
            .and_then(|value| value.checked_add(mask))
            .ok_or(InitdataError::AddressOutOfRange)? & !mask;
        let span = end.checked_sub(virtual_address).ok_or(InitdataError::AddressOutOfRange)?;
        physical.checked_add(span).ok_or(InitdataError::AddressOutOfRange)?;
        Ok((physical, virtual_address, span))
    }
}

/// Source-constructed register descriptors from both identical pre-handoff
/// J813/25G76 captures. These are firmware virtual mappings, not a request to
/// access or power the devices. No runtime currently consumes this profile.
pub(crate) const M5_25G76_REGISTER_WINDOWS: [M5RegisterWindow; 22] = {
    const fn w(slot: usize, phys: u64, va: u64, size_10: u32, size_14: u32,
               unk_18: u64, flag: u32) -> M5RegisterWindow {
        M5RegisterWindow { slot, phys, device_va: 0xffff_fc21_8000_0000 + va,
                           size_10, size_14, unk_18, flag }
    }
    [
        w(0,  0x381014000, 0x000000, 0x04000, 0x04000, 0, 2),
        w(3,  0x220104000, 0x008000, 0x18000, 0x18000, 0, 2),
        w(8,  0x3a1350000, 0x028000, 0x04000, 0x04000, 0, 2),
        w(9,  0x3803d0000, 0x030000, 0x04000, 0x04000, 0, 2),
        w(10, 0x3803c0000, 0x038000, 0x04000, 0x04000, 0, 0),
        w(12, 0x42165c000, 0x040000, 0x04000, 0x04000, 0, 2),
        w(14, 0x380280000, 0x048000, 0x08000, 0x08000, 0, 0),
        w(15, 0x38c840000, 0x058000, 0x24000, 0x24000, 0, 2),
        w(17, 0x580000000, 0x080000, 0x21400, 0x21400, 0, 2),
        w(22, 0x581000000, 0x0a8000, 0x08000, 0x08000, 0, 2),
        w(24, 0x580ec0000, 0x0b8000, 0x10000, 0x10000, 0, 2),
        w(26, 0x580d04000, 0x0d0000, 0x08000, 0x08000, 0xd04000, 2),
        w(27, 0x580d0d000, 0x0e1000, 0x01000, 0x01000, 0xd0d000, 2),
        w(28, 0x580d50000, 0x0e8000, 0x10000, 0x10000, 0xd50000, 2),
        w(29, 0x580d10000, 0x100000, 0x04000, 0x04000, 0xd10000, 2),
        w(31, 0x580d40000, 0x108000, 0x04000, 0x04000, 0xd40000, 2),
        w(32, 0x580d60000, 0x110000, 0x08000, 0x08000, 0xd60000, 2),
        w(35, 0x580e00000, 0x120000, 0x04000, 0x04000, 0xe00000, 2),
        w(39, 0x580e08000, 0x128000, 0x10000, 0x08000, 0, 2),
        w(41, 0x580e33800, 0x143800, 0x04000, 0x04000, 0, 2),
        w(42, 0x580e35000, 0x151000, 0x04000, 0x04000, 0, 2),
        w(50, 0x580014400, 0x160400, 0x00400, 0x00400, 0x14400, 0),
    ]
};

pub(crate) fn encode_m5_25g76_register_map(out: &mut [u8]) -> Result<(), InitdataError> {
    if out.len() != HW_REGISTER_MAP_SLOTS * HW_REGISTER_MAP_ENTRY_SIZE {
        return Err(InitdataError::BufferSize);
    }
    out.fill(0);
    for window in M5_25G76_REGISTER_WINDOWS {
        let base = window.slot * HW_REGISTER_MAP_ENTRY_SIZE;
        put_u64(out, base, window.phys);
        put_u64(out, base + 8, window.device_va);
        put_u32(out, base + 0x10, window.size_10);
        put_u32(out, base + 0x14, window.size_14);
        put_u64(out, base + 0x18, window.unk_18);
        put_u32(out, base + 0x20, window.flag);
    }
    for slot in [2, 5, 6, 7, 30, 33, 34, 37, 38, 40, 43, 46, 47, 48, 49, 52] {
        put_u32(out, slot * HW_REGISTER_MAP_ENTRY_SIZE + 0x20, 2);
    }
    Ok(())
}

pub(crate) const M5_25G76_HW_DATA_SIZE: usize = 0x2708;

pub(crate) const M5_25G76_SHARED_PREFIX_SIZE: usize = 0xc500;

pub(crate) const M5_25G76_AON_PTD_SIZE: usize = 0xe4;

/// J813/25G76 AON PTD backing, captured twice before each role's handoff and
/// unchanged after a passing Metal workload. The three recorded words are
/// scoped to this platform's telemetry description; they are not generic
/// M5 constants. The allocation and GPU binding must remain caller-owned.
pub(crate) fn encode_m5_25g76_aon_ptd(out: &mut [u8]) -> Result<(), InitdataError> {
    if out.len() != M5_25G76_AON_PTD_SIZE {
        return Err(InitdataError::BufferSize);
    }
    out.fill(0);
    put_u32(out, 0, 0x6730_5452);
    put_u32(out, 4, 0x001d_0001);
    put_u32(out, 8, 0x0000_1401);
    Ok(())
}

const M5_25G76_STATUS_CONFIG_WORDS: &[(usize, u32)] = &[
    (0x010, 0x0000001f), (0x014, 0x3e0a3d71), (0x018, 0x401ae148),
    (0x01c, 0x00000028), (0x020, 0x00000001), (0x0c8, 0x00002af8),
    (0x0cc, 0x00001f40), (0x0d8, 0x41980000), (0x0dc, 0x3d0f5c28),
    (0x0f0, 0x000026ac), (0x0f4, 0x000000c8), (0x108, 0x42c80000),
    (0x10c, 0x43c80000), (0x110, 0x000000c8), (0x128, 0x00000001),
    (0x12c, 0x00005eb0), (0x130, 0x3e800000), (0x134, 0x3f99999a),
    (0x158, 0x0000000e), (0x1c0, 0x00000001), (0x1c4, 0x00000001),
    (0x1c8, 0x00000002), (0x1cc, 0x00000001), (0x1d0, 0x00000001),
    (0x1d4, 0x00000001), (0x1d8, 0x00000001), (0x588, 0x00000001),
    (0x58c, 0x0000000a), (0x590, 0x00000dac), (0x594, 0x000003e8),
    (0x598, 0x00000bb8), (0x59c, 0x000002bc), (0x5cc, 0x00000001),
    (0x5d0, 0x00000004), (0x5dc, 0x00001f40), (0x5e0, 0x000000c8),
    (0x5e4, 0x00000fa0), (0x5e8, 0x0000012c), (0x5ec, 0x000007d0),
    (0x5f0, 0x0000012c), (0x5f4, 0x000003e8), (0x5f8, 0x0000012c),
    (0x600, 0x00000001), (0x604, 0x41700000), (0x608, 0x40a00000),
    (0x60c, 0x00000020), (0x610, 0x000004b0), (0x620, 0x0000044c),
];

/// Build the primary status object before either firmware role is started.
/// Secondary root+0xb8 aliases status+0xe440. GFX1 copies that pointer to
/// firmware global 0x176248 at text+0x211d4 and later writes config+0x670,
/// +0x678 and +0x680 at text+0x1af94..0x1afc4. Those are firmware-owned
/// runtime state, not platform constants, and must start at zero even when
/// a primary pre-handoff capture (taken after secondary startup) has values.
pub(crate) fn encode_m5_25g76_primary_status_b(
    fwctl_state: u64,
    fwctl_ring: u64,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != PRIMARY_STATUS_B_OBJECT_SIZE {
        return Err(InitdataError::BufferSize);
    }
    required_address(fwctl_state)?;
    required_address(fwctl_ring)?;
    out.fill(0);
    encode_status_block(&mut out[..STATUS_BLOCK_SIZE])?;
    put_u64(out, PRIMARY_STATUS_B_FWCTL_STATE, fwctl_state);
    put_u64(out, PRIMARY_STATUS_B_FWCTL_RING, fwctl_ring);
    put_u32(out, PRIMARY_STATUS_B_CONFIG_HEADER, 1);
    for &(offset, value) in M5_25G76_STATUS_CONFIG_WORDS {
        put_u32(out, PRIMARY_STATUS_B_CONFIG_OFFSET + offset, value);
    }
    Ok(())
}

/// Shared private arena observed before primary initialization on J813.
/// The status and secondary-grid pointers name overlapping views of this
/// one backing. This extent is a measured mapped arena, not a claim about
/// the sizes of the native host's individual suballocations.
pub(crate) const M5_25G76_PRIVATE_CLUSTER_SIZE: usize = 0x178000;

pub(crate) fn encode_m5_25g76_private_cluster(
    fwctl_state: u64,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != M5_25G76_PRIVATE_CLUSTER_SIZE {
        return Err(InitdataError::BufferSize);
    }
    required_address(fwctl_state)?;
    let fwctl_ring = fwctl_state.checked_add(0x40).ok_or(InitdataError::AddressOutOfRange)?;
    required_address(fwctl_ring)?;
    out.fill(0);
    encode_m5_25g76_primary_status_b(fwctl_state, fwctl_ring, &mut out[0x340..0xee40])?;
    for offset in [0xee40, 0xc3100] {
        encode_status_block(&mut out[offset..offset + STATUS_BLOCK_SIZE])?;
    }
    encode_secondary_root_extra_1_for_profile(RootProfile::J813G17G25G76,
        &mut out[0x177400..0x177480])?;
    // The main-config encoders carry one measured opening record (0x16 /
    // 0x2a). Publish exactly one producer entry for each control channel.
    // All work-channel and report counters remain zero.
    put_u32(out, 0x320, 1);
    put_u32(out, 0x1773e0, 1);
    Ok(())
}

/// Caller-owned bindings in the power/performance policy allocation.
/// The host stores mach_absolute_time() at policy+0x651c. Supply the live
/// platform counter in its native timebase, never a captured timestamp.
#[derive(Clone, Copy)]
pub(crate) struct M5PolicyBindings {
    pub state: u64,
    pub aon_ptd: u64,
    pub boot_timestamp: u64,
}

/// Pre-handoff J813/25G76 scalars, identical across two cold boots after
/// removing owned pointers and the native boot timestamp. The repeated
/// 512-entry u16 table is encoded separately. Calibration fields whose
/// meanings remain unknown are scoped to this measured ten-core target.
const M5_25G76_POLICY_WORDS: &[(usize, u32)] = &[
    (0x3d98, 0xffffffff), (0x3db0, 0xffffffff), (0x5244, 0x00030d40),
    (0x5248, 0x00030d40), (0x5250, 0x00000004), (0x5254, 0x3f800000),
    (0x526c, 0x00000001), (0x5270, 0x00000001), (0x5280, 0x00000064),
    (0x5284, 0x00000001), (0x5288, 0x000004b0), (0x5290, 0x00000064),
    (0x52a0, 0x00000004), (0x52a4, 0x3f800000), (0x52ac, 0x00000001),
    (0x52b0, 0x00000001), (0x52c0, 0x00000064), (0x52c4, 0x00000001),
    (0x52c8, 0x000004b0), (0x52d0, 0x00000064), (0x5300, 0x3f828f5c),
    (0x5304, 0x3f828f5c), (0x5308, 0x3f828f5c), (0x530c, 0x3f828f5c),
    (0x5310, 0x3f828f5c), (0x5314, 0x3f828f5c), (0x5318, 0x3f828f5c),
    (0x531c, 0x3f828f5c), (0x5320, 0x3f828f5c), (0x5324, 0x3f828f5c),
    (0x5328, 0x3f828f5c), (0x532c, 0x3f828f5c), (0x5330, 0x3f828f5c),
    (0x5bf8, 0x00000271), (0x5c04, 0x3f7f2e9f), (0x5c0c, 0x3b516154),
    (0x5c14, 0x3ca59586), (0x5c20, 0x46bd6000), (0x5c24, 0x40a90fe4),
    (0x5c2c, 0xbd4ac5b4), (0x5c30, 0x00000028), (0x5c34, 0x000004b0),
    (0x5c38, 0x000004b0), (0x5c44, 0x00005eb0), (0x5c78, 0x000004b0),
    (0x5cb0, 0x00000008), (0x5cbc, 0x3f000000), (0x5cc4, 0x3f000000),
    (0x5ccc, 0x3f99999a), (0x5cd8, 0x47800000), (0x5cdc, 0x3e800000),
    (0x5ce8, 0x00000028), (0x5cec, 0x000004b0), (0x5cfc, 0x00005eb0),
    (0x5d08, 0x00000010), (0x5d10, 0x0005dc00), (0x5d68, 0x0000004a),
    (0x5d70, 0x0000005a), (0x5d74, 0x00000032), (0x5d78, 0x00000006),
    (0x5d80, 0x00000006), (0x5d84, 0x00000001), (0x5d8c, 0x3f4ccccd),
    (0x5d90, 0x3f7eb852), (0x5d94, 0x3e4ccccd), (0x5d98, 0x3ba3d70a),
    (0x5d9c, 0x3f144b3c), (0x5da0, 0x3f144b3c), (0x5da8, 0x42be0000),
    (0x5dac, 0x4029eb85), (0x5db0, 0x4029eb85), (0x5db4, 0x4139435e),
    (0x5db8, 0x00000064), (0x5dbc, 0x000004b0), (0x5dc0, 0x00000064),
    (0x5dcc, 0x0000004a), (0x5dd4, 0x00000001), (0x5e00, 0x00000064),
    (0x5e20, 0x00000401), (0x5e24, 0x00006400), (0x5e2c, 0x001f4000),
    (0x5e30, 0x0000c800), (0x5e34, 0x000fa000), (0x5e38, 0x00012c00),
    (0x5e3c, 0x0007d000), (0x5e40, 0x00012c00), (0x5e44, 0x0003e800),
    (0x5e48, 0x00012c00), (0x5e4c, 0x00000100), (0x5ea4, 0x00000001),
    (0x5ed0, 0x00000001), (0x74d8, 0x47800000), (0x74dc, 0x42200000),
    (0x74e0, 0x44960000), (0x74e4, 0x0000062a), (0x74f8, 0x00000028),
    (0x7500, 0x42c80000), (0x7504, 0x000004b0), (0x750c, 0x3f4ccccd),
    (0x7510, 0x3e4ccccd), (0x7550, 0xdcdcfbfa), (0x7558, 0x00001f40),
    (0x7560, 0x00000004), (0x7574, 0x3f800000), (0x757c, 0x3d0f5c28),
    (0x7588, 0x47800000), (0x758c, 0x41980000), (0x7598, 0x00000028),
    (0x759c, 0x000004b0), (0x75a0, 0x000004b0), (0x75ac, 0x00002af8),
    (0x75e0, 0x000004b0), (0x77f8, 0x00000002), (0x77fc, 0x42000006),
    (0x7a10, 0xb8520000), (0x7a14, 0x00003f7e), (0x7a18, 0xd70a0000),
    (0x7a1c, 0x00003ba3), (0x7a24, 0x000043c8), (0x7a30, 0x00004780),
    (0x7a34, 0x000042c8), (0x7a3c, 0x0384bac8), (0x7a40, 0x03e80000),
    (0x7a44, 0x03e80000), (0x7a4c, 0x80000000), (0x7a50, 0x26ac453b),
    (0x7a84, 0x03e80000), (0x7aa0, 0x00c80000), (0x7b58, 0x000004b0),
    (0x7b6c, 0x00000001), (0x7ba0, 0x00000101), (0x7ba4, 0x00000200),
    (0x7bbc, 0x00023f00), (0x7bc0, 0x00000100), (0x7bc4, 0x00000100),
    (0x7bdc, 0x00024000), (0x7be0, 0x00000100), (0x7be4, 0x00000100),
    (0x7bfc, 0x00024100), (0x7c48, 0x00000001), (0x7c50, 0x00005eb0),
    (0x7c54, 0x00005eb0), (0x7c58, 0x00005eb0), (0x7c68, 0x3f800000),
    (0x7c6c, 0x00000004), (0x7c70, 0x00000008), (0x7c74, 0x0002ee00),
    (0x7d44, 0x0000007c), (0x7d50, 0x3f77bdef), (0x7d58, 0x3d042108),
    (0x7d60, 0x3e0a3d71), (0x7d6c, 0x47800000), (0x7d70, 0x401ae148),
    (0x7d7c, 0x00000028), (0x7d80, 0x000004b0), (0x7d84, 0x000004b0),
    (0x7d8c, 0x46bd6000), (0x7d90, 0x00005eb0), (0x7d9c, 0x000000f8),
    (0x7da4, 0x005ad200), (0x7dc4, 0x000004b0), (0x7e04, 0x00000001),
    (0x7ed4, 0x00000032), (0x7ed8, 0x00000001), (0x7ee0, 0x3f638e39),
    (0x7ee4, 0x3f2aaaab), (0x7ee8, 0x3de38e39), (0x7eec, 0x3eaaaaab),
    (0x7ef0, 0xbecccccd), (0x7ef4, 0xbf4ccccd), (0x7efc, 0x47800000),
    (0x7f00, 0xc0a00000), (0x7f04, 0xc0a00000), (0x7f0c, 0x00000064),
    (0x7f10, 0x000004b0), (0x7f14, 0x00000064), (0x7f1c, 0x45fa0000),
    (0x7f20, 0x00000578), (0x7f28, 0x0000000e), (0x7f2c, 0x00000048),
    (0x7f30, 0x00000018), (0x7f34, 0x001a5e00), (0x7f3c, 0x0008ca00),
    (0x7f48, 0x47800000), (0x7f70, 0x00001f40), (0x7f74, 0x00000004),
    (0x7f78, 0x000009c4), (0x7f7c, 0x00000258), (0x7f80, 0x00000002),
    (0x7f84, 0x00000004), (0x7f88, 0x00000032), (0x7fb8, 0x47800000),
    (0x7fc8, 0x00000028), (0x7fcc, 0x000004b0), (0x8098, 0x00000001),
    (0x80ac, 0x0000000a), (0x80b8, 0x00000001), (0x8174, 0xac000000),
    (0x8178, 0xe800000d), (0x817c, 0xb8000003), (0x8180, 0xb000000b),
    (0x8184, 0xb0000004), (0x8188, 0xbc000004), (0x818c, 0x20000002),
    (0x8198, 0x00000001), (0x81b4, 0x00000004), (0x81c0, 0x3f780000),
    (0x81c8, 0x3d000000), (0x81d0, 0x40a00000), (0x81dc, 0x47800000),
    (0x81e0, 0x41700000), (0x81ec, 0x0000044c), (0x81f0, 0x000004b0),
    (0x81f4, 0x000004b0), (0x8200, 0x000004b0), (0x8234, 0x000004b0),
    (0x827c, 0x00000064), (0x8290, 0xdcdcfbfa), (0x8298, 0x0000007d),
    (0x829c, 0x00000001), (0x8ee0, 0xdcdcfbfb), (0x8ee8, 0x00000001),
    (0xaeec, 0x41c80000), (0xaef0, 0x41c80000), (0xb6a8, 0x67305452),
    (0xb6e8, 0x00000001), (0xb728, 0x5243464c), (0xb72c, 0x4758464c),
    (0xb738, 0x47584c50), (0xb748, 0x00000001), (0xb7a8, 0x00000001),
    (0xb7b4, 0x42b73333), (0xb7f4, 0x449b8000), (0xb834, 0x42bc0000),
    (0xb8e0, 0x0000186a),
];

/// Initialize the six allocations following M5 hardware-data. Preserve the
/// hardware block and subsequent channel resources. No firmware is started.
pub(crate) fn encode_m5_25g76_bundle_policy(
    bindings: M5PolicyBindings,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() < M5_25G76_SHARED_PREFIX_SIZE {
        return Err(InitdataError::BufferSize);
    }
    required_address(bindings.state)?;
    required_address(bindings.aon_ptd)?;
    let state_end = bindings.state.checked_add(STATUS_BLOCK_SIZE as u64)
        .ok_or(InitdataError::AddressOutOfRange)?;
    let aon_end = bindings.aon_ptd.checked_add(M5_25G76_AON_PTD_SIZE as u64)
        .ok_or(InitdataError::AddressOutOfRange)?;
    required_address(state_end - 1)?;
    required_address(aon_end - 1)?;
    if bindings.state < aon_end && bindings.aon_ptd < state_end {
        return Err(InitdataError::PairAliasedObject);
    }
    out[0x2740..M5_25G76_SHARED_PREFIX_SIZE].fill(0);
    for &(offset, value) in M5_25G76_POLICY_WORDS {
        put_u32(out, offset, value);
    }
    for index in 0..512 {
        put_u16(out, 0x6ed7 + index * 2, 0x7fff);
    }
    put_u64(out, 0x8ed8, bindings.state);
    put_u64(out, 0xb6a0, bindings.aon_ptd);
    put_u64(out, 0xb75c, bindings.boot_timestamp);
    Ok(())
}

/// Recorded platform words outside the explicit register, performance and
/// owned-pointer fields. Meanings not yet derived are deliberately kept as
/// target-scoped measured values. Never insert runtime completion counters
/// or captured allocation addresses here. Reference hardware-data SHA-256:
/// 6f9ac0c4664f898a0ddb4e5df9ad160a23c1b52d3fd750750779cbdebe446be9.
const M5_25G76_HW_PLATFORM_WORDS: &[(usize, u32)] = &[
    (0x0004, 0x0000006f), (0x0008, 0xffc00000), (0x0014, 0x00000010),
    (0x001c, 0x00000010), (0x0020, 0xffff8000), (0x0024, 0x000002ff),
    (0x0028, 0x81400000), (0x002c, 0xfffffc21), (0x00e0, 0x00002008),
    (0x00e8, 0x20080000), (0x00f4, 0x00002008), (0x00f8, 0x000024cb),
    (0x00fc, 0xc98a2cfa), (0x0100, 0xf4f624cb), (0x0104, 0x1877e917),
    (0x0108, 0x38d924cb), (0x010c, 0xbdab0000), (0x02d8, 0xfff82000),
    (0x02dc, 0xd32d2cdb), (0x02e0, 0xf5002000), (0x02e4, 0x21dae926),
    (0x02e8, 0x38b62000), (0x02ec, 0xc7420008), (0x03e0, 0x00007fe0),
    (0x03e8, 0x7fe00000), (0x03f4, 0x00007fe0), (0x03f8, 0x00008000),
    (0x0400, 0x80000000), (0x040c, 0x00008000), (0x05d8, 0x4b232645),
    (0x05dc, 0x00000e98), (0x05e0, 0xd5a2ea5f), (0x05e4, 0x40003fff),
    (0x05e8, 0xca5e4000), (0x05ec, 0x4000f5a2), (0x0e90, 0x00008142),
    (0x0e94, 0x00000002), (0x0ea0, 0x00000001), (0x0ea4, 0x00000001),
    (0x0eb8, 0x00000001), (0x0ec0, 0x00000001), (0x0ec8, 0x00000001),
    (0x0ed0, 0x00005dc0), (0x0ed4, 0x00000001), (0x0ed8, 0x00000008),
    (0x0ee0, 0x00000001), (0x0ee4, 0x00000001), (0x0ee8, 0x00000001),
    (0x0f04, 0x0000001f), (0x0f24, 0x07800004), (0x0f28, 0x00000006),
    (0x0f34, 0x00000001), (0x0f38, 0x00000001), (0x0f4c, 0x00000031),
    (0x0f6c, 0x00000001), (0x0f88, 0x0000000a), (0x0f8c, 0x00000001),
    (0x0fac, 0x00000001), (0x0fb8, 0x00000020), (0x0fbc, 0x00000005),
    (0x0fc0, 0x0000000a), (0x0fc4, 0x0000000c), (0x2544, 0x00000004),
    (0x2548, 0x00000006), (0x254c, 0x00000003), (0x2550, 0x00000007),
    (0x2554, 0x00000007), (0x2568, 0x0000000a), (0x2570, 0x00000001),
    (0x258c, 0x00180000), (0x2594, 0x00000001), (0x2598, 0x00000001),
    (0x259c, 0x00000001), (0x25a4, 0x00000001), (0x25a8, 0x00000001),
    (0x25ac, 0x00000001), (0x25b4, 0xffffffff), (0x25b8, 0xffffffff),
    (0x25bc, 0xffffffff), (0x25c0, 0xffffffff), (0x25c4, 0xffffffff),
    (0x25c8, 0xffffffff), (0x25cc, 0xffffffff), (0x25d0, 0xffffffff),
    (0x25d4, 0xffffffff), (0x25d8, 0xffffffff), (0x25dc, 0xffffffff),
    (0x25e0, 0xffffffff), (0x25f4, 0x00000001), (0x2600, 0x00000001),
    (0x2608, 0x00000001), (0x2610, 0x00000100), (0x26b4, 0x00000001),
    (0x26b8, 0x00000001), (0x26e0, 0x000003e8), (0x26e8, 0x00000001),
];

/// Reconstruct the M5 reference hardware-data block with caller-owned queue
/// and QoS bindings. Performance values are measured on the ten-core J813
/// reference, not derived from ADT and not a generic M5 power configuration.
/// Both pre-handoff firmware roles supplied identical reference bytes.
/// This encoder grants no runtime admission and performs no MMIO accesses.
pub(crate) fn encode_m5_25g76_hw_data(
    aliases: [G17PKsmCompletionAliases; M5_25G76_COMPLETION_QUEUE_COUNT],
    qos_high: u64,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != M5_25G76_HW_DATA_SIZE {
        return Err(InitdataError::BufferSize);
    }
    required_address(qos_high)?;
    let qos_end = qos_high.checked_add(0xe40).ok_or(InitdataError::AddressOutOfRange)?;
    required_address(qos_end - 1)?;
    let mut completion = [0; 0x80];
    encode_m5_25g76_completion_table(aliases, &mut completion)?;
    for alias in aliases {
        if qos_high < alias.high + M5_25G76_COMPLETION_BACKING_SIZE as u64
            && alias.high < qos_end {
            return Err(InitdataError::PairAliasedObject);
        }
    }

    out.fill(0);
    for &(offset, value) in M5_25G76_HW_PLATFORM_WORDS {
        put_u32(out, offset, value);
    }
    encode_m5_25g76_register_map(&mut out[0x640..0xe88])?;
    out[0x2614..0x2694].copy_from_slice(&completion);
    put_u64(out, 0x26ac, qos_high);

    // Native includes index zero plus twelve operating points. Each voltage
    // occupies all sixteen lanes of its 0x40-byte state row.
    const FREQUENCY: [u32; 13] = [0, 338, 486, 636, 796, 888, 988, 1084, 1182, 1278, 1374, 1470, 1578];
    const CORE_VOLTAGE: [u32; 13] = [125, 615, 630, 660, 705, 735, 755, 800, 830, 865, 905, 945, 1010];
    const MEMORY_VOLTAGE: [u32; 13] = [770, 770, 770, 770, 770, 770, 770, 800, 830, 865, 905, 945, 1010];
    const RELATIVE_A: [u32; 13] = [0, 17, 23, 30, 42, 50, 58, 71, 82, 85, 89, 93, 100];
    const RELATIVE_B: [u32; 13] = [0, 0, 11, 24, 36, 44, 52, 60, 68, 75, 83, 91, 100];
    for index in 0..13 {
        put_u32(out, 0xfc8 + index * 4, FREQUENCY[index]);
        put_u32(out, 0x1848 + index * 4, 0x3f828f5c);
        put_u32(out, 0x18c8 + index * 4, RELATIVE_A[index]);
        put_u32(out, 0x1908 + index * 4, RELATIVE_B[index]);
        for lane in 0..16 {
            put_u32(out, 0x1008 + index * 0x40 + lane * 4, CORE_VOLTAGE[index]);
            put_u32(out, 0x1408 + index * 0x40 + lane * 4, MEMORY_VOLTAGE[index]);
        }
    }
    for index in 0..16 {
        put_u32(out, 0x19c8 + index * 4, index as u32);
    }
    Ok(())
}

/// Fail-closed validation errors for the grounded layout.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum InitdataError {
    BufferSize,
    AddressOutOfRange,
    NullRequiredAddress,
    RoleSpecificAddress,
    BundleAllocationTooSmall,
    ChannelTableShape,
    RegisterWindow,
    PairAliasedObject,
}

fn address_representable(address: u64) -> bool {
    let high = address & !INITDATA_ADDR_MASK;
    high == 0 || high == INITDATA_ADDR_PREFIX
}

fn required_address(address: u64) -> Result<(), InitdataError> {
    if address == 0 {
        Err(InitdataError::NullRequiredAddress)
    } else if !address_representable(address) {
        Err(InitdataError::AddressOutOfRange)
    } else {
        Ok(())
    }
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn write_runs(bytes: &mut [u8], runs: &[(usize, &[u8])]) {
    for (offset, data) in runs {
        bytes[*offset..*offset + data.len()].copy_from_slice(data);
    }
}

fn validate_shared(shared: &SharedAddresses) -> Result<(), InitdataError> {
    required_address(shared.region_a)?;
    required_address(shared.region_c)?;
    required_address(shared.hw_data_bundle)?;
    if shared.hw_data_bundle_alloc < HW_DATA_BUNDLE_MIN_ALLOC {
        return Err(InitdataError::BundleAllocationTooSmall);
    }
    Ok(())
}

fn validate_instance(role: InstanceRole, inst: &InstanceAddresses) -> Result<(), InitdataError> {
    required_address(inst.root)?;
    required_address(inst.main_config)?;
    required_address(inst.status_a)?;
    match role {
        InstanceRole::Primary => {
            // The primary root has no +0xb8/+0xc0 fields at all; carrying
            // secondary extras there is a role-confusion bug at the caller.
            if inst.secondary_extras != [0, 0] {
                return Err(InitdataError::RoleSpecificAddress);
            }
            required_address(inst.status_b)?;
        }
        InstanceRole::Secondary => {
            // Every hardware-booting host hands the secondary a zero status-B;
            // a nonzero value there is ungrounded, so fail closed on it.
            if inst.status_b != 0 {
                return Err(InitdataError::RoleSpecificAddress);
            }
            for extra in inst.secondary_extras {
                required_address(extra)?;
            }
        }
    }
    Ok(())
}

/// Encode one 0x20-byte UAT level descriptor. Every field is derivable from
/// the level's index shift and entry count; the index mask
/// `(entries - 1) << shift` reproduces all three captured masks exactly.
fn encode_uat_level_descriptor(bytes: &mut [u8], offset: usize, shift: u8, entries: u16) {
    bytes[offset] = 8;
    bytes[offset + 1] = UAT_PAGE_BITS;
    bytes[offset + 2] = UAT_PAGE_BITS;
    bytes[offset + 3] = shift;
    put_u16(bytes, offset + 0x04, entries);
    put_u16(bytes, offset + 0x06, UAT_PAGE_SIZE);
    put_u64(bytes, offset + 0x08, 1);
    put_u64(bytes, offset + 0x10, UAT_LEVEL_PHYS_MASK);
    put_u64(bytes, offset + 0x18, (entries as u64 - 1) << shift);
}

/// Encode the common root layout with an explicitly selected firmware build.
/// The existing runtime continues to call the J700-only wrapper above.
pub(crate) fn encode_root_for_profile(
    profile: RootProfile,
    role: InstanceRole,
    shared: &SharedAddresses,
    inst: &InstanceAddresses,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    // Profile-sized: identical to root_size(role) for J700/J813.
    if out.len() != profile.root_size(role) {
        return Err(InitdataError::BufferSize);
    }
    validate_shared(shared)?;
    validate_instance(role, inst)?;

    out.fill(0);
    put_u64(out, ROOT_VERSION, profile.version_word());
    put_u64(out, ROOT_REGION_A, shared.region_a);
    // ROOT_ZERO_10 deliberately left zero.
    put_u64(out, ROOT_MAIN_CONFIG, inst.main_config);
    put_u64(out, ROOT_REGION_C, shared.region_c);
    put_u32(out, ROOT_INSTANCE_KIND, role as u32);
    put_u32(out, ROOT_CONSTANT_2C, 1);
    put_u16(out, ROOT_PAGE_SIZE, UAT_PAGE_SIZE);
    out[ROOT_PAGE_BITS] = UAT_PAGE_BITS;
    out[ROOT_LEVEL_COUNT] = UAT_LEVEL_COUNT as u8;
    for (index, (shift, entries)) in UAT_LEVELS.iter().enumerate() {
        encode_uat_level_descriptor(
            out,
            ROOT_LEVELS + index * UAT_LEVEL_DESC_SIZE,
            *shift,
            *entries,
        );
    }
    put_u64(out, ROOT_STATUS_A, inst.status_a);
    put_u64(out, ROOT_STATUS_B, inst.status_b);
    if role == InstanceRole::Secondary {
        put_u64(out, ROOT_SECONDARY_EXTRA_0, inst.secondary_extras[0]);
        put_u64(out, ROOT_SECONDARY_EXTRA_1, inst.secondary_extras[1]);
    }
    Ok(())
}

/// Derive the native channel table for one instance.
///
/// `state_grid` is this instance's compact channel-state block (0x40-stride,
/// permuted per [`WORK_STATE_GRID_OFFSETS`]); `status_a` anchors the trailing
/// report/partial channels; the ring for entry 12 is `main_config + 0x4c0`;
/// work rings for the primary sit in the bundle at
/// [`WORK_RING_BUNDLE_OFFSETS`] (which requires the
/// [`NATIVE_SHARED_CLUSTER_SIZE`] cluster). The secondary leaves entries
/// 0..11 empty: it is a control-only peer with no work channels.
pub(crate) fn derive_channel_table(
    role: InstanceRole,
    state_grid: u64,
    status_a: u64,
    main_config: u64,
    bundle: u64,
) -> [ChannelEntry; CHANNEL_TABLE_ENTRIES] {
    let mut table = [ChannelEntry::EMPTY; CHANNEL_TABLE_ENTRIES];
    let grid_states = |index: usize| {
        let base = state_grid + WORK_STATE_GRID_OFFSETS[index] as u64;
        [
            base,
            base + CHANNEL_STATE_SPACING as u64,
            base + 2 * CHANNEL_STATE_SPACING as u64,
        ]
    };
    if role == InstanceRole::Primary {
        for index in 0..WORK_CHANNEL_COUNT {
            table[index] = ChannelEntry {
                states: grid_states(index),
                ring: bundle + WORK_RING_BUNDLE_OFFSETS[index] as u64,
            };
        }
    }
    table[CONTROL_CHANNEL_INDEX] = ChannelEntry {
        states: grid_states(CONTROL_CHANNEL_INDEX),
        ring: main_config + MAIN_CONTROL_RING as u64,
    };
    for (slot, index) in REPORT_CHANNEL_INDICES.iter().enumerate() {
        let mut states = [0u64; 3];
        for (word, offset) in states.iter_mut().zip(REPORT_STATE_STATUS_A_OFFSETS[slot]) {
            *word = status_a + offset as u64;
        }
        table[*index] = ChannelEntry {
            states,
            ring: status_a + REPORT_RING_STATUS_A_OFFSETS[slot] as u64,
        };
    }
    table[PARTIAL_CHANNEL_INDEX] = ChannelEntry {
        states: [status_a + PARTIAL_STATE_STATUS_A_OFFSET as u64, 0, 0],
        ring: 0,
    };
    table
}

fn validate_populated_entry(entry: &ChannelEntry) -> Result<(), InitdataError> {
    // A populated channel needs all three state-counter addresses — a null
    // producer (state 2) on a populated entry makes firmware fault on address
    // zero — and a real ring.
    for state in entry.states {
        if state == 0 {
            return Err(InitdataError::ChannelTableShape);
        }
        if !address_representable(state) {
            return Err(InitdataError::AddressOutOfRange);
        }
    }
    required_address(entry.ring)
}

fn validate_channel_table(
    role: InstanceRole,
    main_config: u64,
    channels: &[ChannelEntry; CHANNEL_TABLE_ENTRIES],
) -> Result<(), InitdataError> {
    for (index, entry) in channels.iter().enumerate() {
        match index {
            0..=11 => match role {
                InstanceRole::Primary => validate_populated_entry(entry)?,
                // The secondary has no work channels at all.
                InstanceRole::Secondary => {
                    if *entry != ChannelEntry::EMPTY {
                        return Err(InitdataError::ChannelTableShape);
                    }
                }
            },
            12 => {
                validate_populated_entry(entry)?;
                // Device control is embedded in the main object, at exactly
                // main + 0x4c0 on both instances.
                if entry.ring != main_config + MAIN_CONTROL_RING as u64 {
                    return Err(InitdataError::ChannelTableShape);
                }
            }
            13 | 14 => validate_populated_entry(entry)?,
            15 => {
                // Partial: a first state address and nothing else.
                required_address(entry.states[0])?;
                if entry.states[1] != 0 || entry.states[2] != 0 || entry.ring != 0 {
                    return Err(InitdataError::ChannelTableShape);
                }
            }
            _ => {
                if *entry != ChannelEntry::EMPTY {
                    return Err(InitdataError::ChannelTableShape);
                }
            }
        }
    }
    Ok(())
}

/// The two complete M5/25G76 pre-handoff main configurations match the common
/// layout except primary +0x32c/+0x344 and secondary +0x300. Keep these
/// measured scalars build-specific; their meanings are not inferred here.
pub(crate) fn encode_main_config_for_profile(
    profile: RootProfile,
    role: InstanceRole,
    shared: &SharedAddresses,
    inst: &InstanceAddresses,
    channels: &[ChannelEntry; CHANNEL_TABLE_ENTRIES],
    region_views: Option<&PrimaryRegionViews>,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != MAIN_CONFIG_SIZE {
        return Err(InitdataError::BufferSize);
    }
    validate_shared(shared)?;
    validate_instance(role, inst)?;
    validate_channel_table(role, inst.main_config, channels)?;
    match (role, region_views) {
        (InstanceRole::Primary, Some(views)) => {
            required_address(views.sentinel_high)?;
            for high in views.alias_high {
                required_address(high)?;
            }
        }
        (InstanceRole::Secondary, None) => {}
        _ => return Err(InitdataError::RoleSpecificAddress),
    }
    // Every view must fit inside the one bundle allocation. (The last two end
    // exactly at 0xf000, inside the 0x10000 minimum.) The view table is the
    // profile's: J700/J813 keep BUNDLE_VIEW_OFFSETS/EXTENTS unchanged.
    let view_offsets = profile.bundle_view_offsets();
    for (offset, extent) in view_offsets.iter().zip(profile.bundle_view_extents()) {
        if offset + extent > shared.hw_data_bundle_alloc {
            return Err(InitdataError::BundleAllocationTooSmall);
        }
    }

    out.fill(0);
    // Firmware log enable. The emitter at A000 0xfffffc0000021fa0 reads this
    // word and returns immediately when it is zero; every buffer the facility
    // needs is already allocated and pointed at by the addresses below. The
    // records are plain ASCII. Shipping macOS provisions the whole facility
    // and then throws the messages away (drainFirmwareLogRing is `bti c; ret`),
    // which is why the captured value is zero. It stays zero here.
    let bundle = shared.hw_data_bundle;
    put_u64(out, MAIN_HW_DATA, bundle);
    let repeated = bundle + profile.repeated_region_bundle_offset() as u64;
    put_u64(out, MAIN_REPEATED_0, repeated);
    put_u64(out, MAIN_REPEATED_1, repeated);

    for (index, entry) in channels.iter().enumerate() {
        let base = MAIN_CHANNEL_TABLE + index * CHANNEL_ENTRY_SIZE;
        for (word, state) in entry.states.iter().enumerate() {
            put_u64(out, base + word * 8, *state);
        }
        put_u64(out, base + CHANNEL_ENTRY_STATE_COUNT * 8, entry.ring);
    }

    match (role, region_views) {
        (InstanceRole::Primary, Some(views)) => {
            // The five bare pointers are views into the one bundle
            // allocation, at fixed offsets. Unaligned stores at +0x254.
            for (index, offset) in view_offsets.iter().enumerate() {
                put_u64(out, MAIN_BUNDLE_VIEWS + index * 8, bundle + *offset as u64);
            }
            // High-only sentinel followed by two low/high region-view pairs.
            put_u64(out, MAIN_REGION_VIEWS, views.sentinel_high);
            put_u64(out, MAIN_REGION_VIEWS + 0x08, REGION_VIEW_LOW_ADDRS[0]);
            put_u64(out, MAIN_REGION_VIEWS + 0x10, views.alias_high[0]);
            put_u64(out, MAIN_REGION_VIEWS + 0x18, REGION_VIEW_LOW_ADDRS[1]);
            put_u64(out, MAIN_REGION_VIEWS + 0x20, views.alias_high[1]);
            if profile == RootProfile::J700G17P {
                put_u32(out, MAIN_PRIMARY_SCALAR_32C, 1);
                put_u32(out, MAIN_PRIMARY_SCALAR_344, 0xabcd_abcd);
            }
            // KEXT_PROVEN for G18G too (0xfffffe0008c31780: main+0x3e0=0xff).
            put_u32(out, MAIN_PRIMARY_MASK, MAIN_PRIMARY_MASK_VALUE);
            put_u32(out, MAIN_CONTROL_RING, profile.control_opening_opcode(role));
        }
        (InstanceRole::Secondary, _) => {
            // The secondary keeps the two low context-0 values but carries no
            // view or region addresses; the values are therefore not
            // properties of any region the host allocates.
            put_u64(out, MAIN_REGION_VIEWS + 0x08, REGION_VIEW_LOW_ADDRS[0]);
            put_u64(out, MAIN_REGION_VIEWS + 0x18, REGION_VIEW_LOW_ADDRS[1]);
            put_u32(out, MAIN_SECONDARY_KIND, match profile {
                RootProfile::J700G17P => MAIN_SECONDARY_KIND_VALUE,
                RootProfile::J813G17G25G76 => 0xc,
                // INFERRED: 26A428 stores accel+0x10a74 here (25G76:
                // accel+0xed30, measured 0xc on J813). Value not read on J873.
                RootProfile::J873G18G26A428 => G18G_26A428_MAIN_SECONDARY_KIND,
            });
            if profile == RootProfile::J873G18G26A428 {
                // KEXT_PROVEN (26A428 initFirmwareSharedData, g18g_key.c:4464):
                // role 1 main+0x481 = FW VA of the perf/policy allocation.
                // 25G76 stored it at +0x471 and also wrote an accelerator
                // pair at +0x479/+0x481, which 26A428 no longer writes.
                put_u64(out, G18G_26A428_MAIN_SECONDARY_POLICY_PTR,
                        bundle + G18G_26A428_SHARED_LAYOUT.policy as u64);
            } else {
                // One additional unaligned pointer *into* primary view 2, landing
                // five bytes before that view's first populated run.
                let extra = bundle
                    + BUNDLE_VIEW_OFFSETS[SECONDARY_EXTRA_VIEW_INDEX] as u64
                    + SECONDARY_EXTRA_VIEW_OFFSET as u64;
                put_u64(out, MAIN_SECONDARY_EXTRA_PTR, extra);
            }
            put_u32(out, MAIN_CONTROL_RING, profile.control_opening_opcode(role));
        }
        _ => unreachable!(),
    }
    Ok(())
}

/// Encode target-specific pre-handoff platform policy. M5's native policy is
/// kept separate from the experimental T8140 watchdog and idle overrides.
pub(crate) fn encode_region_c_for_profile(
    profile: RootProfile,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != REGION_C_SIZE {
        return Err(InitdataError::BufferSize);
    }
    if profile == RootProfile::J873G18G26A428 {
        // Derived from the J813 bytes below through the proven +4 shift.
        return encode_g18g_26a428_region_c(out);
    }
    out.fill(0);
    write_runs(out, REGION_C_RUNS);
    if profile == RootProfile::J813G17G25G76 {
        // Scalars measured before each ASC received its root on two cold
        // J813 / 25G76 boots. These are a scoped reference configuration,
        // not generic M5 calibration or a power-management qualification.
        // The final pair moved by 0x20; leaving the T8140 pair in place would
        // initialize unrelated M5 fields.
        for (offset, value) in [
            (0x084, 0x0000_5eb0),
            (0x088, 0x0000_04b0),
            (0x08c, 0x0000_04b0),
            (0x098, 0x0000_04b0),
            (0x7c4, 0x0000_04e2),
            (0x7e0, 0x3f33_3333),
            (0x9a8, 0x0000_0333),
            (0xe38, 0),
            (0xe3c, 0),
            (0xe58, 0x0000_0300),
            (0xe5c, 0x0000_0100),
        ] {
            put_u32(out, offset, value);
        }
        return Ok(());
    }
    // regionC+0x9cc is the KSM progress-watchdog threshold for data master 2
    // (compute); the firmware copies it to 0x1778d0 and its handler starts
    // `cbz w20 -> ret`, so 0 disables the watchdog. We ship 0x32 (50) here and
    // leave the other data master's slot at 0 -- i.e. we arm the watchdog on
    // exactly the queue we submit to. When work cannot progress for any other
    // reason this turns a stall into a self-sustaining recovery loop.
    for (offset, value) in [
        (0x7c, 1),
        (0x80, 1),
        (0x9c, 0x3000_001d),
        (0xa0, 0x3100_0002),
    ] {
        put_u32(out, offset, value);
    }
    Ok(())
}

/// Encode one 0x80-byte status block in its pre-init state: u32 1 at `+0x04`,
/// everything else zero. `+0x10`/`+0x14` are post-acknowledgement firmware
/// state and must be zero at handoff.
pub(crate) fn encode_status_block(out: &mut [u8]) -> Result<(), InitdataError> {
    if out.len() != STATUS_BLOCK_SIZE {
        return Err(InitdataError::BufferSize);
    }
    out.fill(0);
    put_u32(out, STATUS_BLOCK_ONE_OFFSET, 1);
    Ok(())
}

pub(crate) fn encode_secondary_root_extra_1_for_profile(
    profile: RootProfile,
    out: &mut [u8],
) -> Result<(), InitdataError> {
    if out.len() != STATUS_BLOCK_SIZE {
        return Err(InitdataError::BufferSize);
    }
    out.fill(0);
    write_runs(out, SECONDARY_ROOT_EXTRA_1_RUNS);
    // G18G: KEXT_PROVEN identical store geometry for this 0x60 object
    // (26A428 vs 25G76 R_60 stores, all at the same offsets); the J813 values
    // are carried unchanged (INFERRED, listed in G18G_26A428_UNPROVEN).
    if matches!(profile, RootProfile::J813G17G25G76 | RootProfile::J873G18G26A428) {
        put_u32(out, 0x10, 2);
    }
    Ok(())
}

/// Recorded T8140 region-C platform tuning constants (values known, meanings
/// not; generated from the hardware-validated `g17p_initdata.py`
/// `REGION_C_CONSTANTS`). 12 sparse runs, 180 bytes total. The final run
/// initializes the byte preceding the host power-assert counter and the
/// counter itself to their native cold value of zero.
pub(crate) const REGION_C_RUNS: &[(usize, &[u8])] = &[
    (0x0024, &[0xb8, 0x0b, 0x00, 0x00]),
    (
        0x0030,
        &[
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00,
            0x00, 0x00,
        ],
    ),
    (
        0x0054,
        &[
            0xff, 0xff, 0x28, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00,
        ],
    ),
    (0x0078, &[0x01, 0x00, 0x00, 0x00]),
    (
        0x0084,
        &[
            0x4e, 0x25, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00,
        ],
    ),
    (0x0098, &[0xe8, 0x03, 0x00, 0x00]),
    (
        0x07c4,
        &[
            0xd0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0xcd, 0xcc, 0x4c, 0x3f, 0xcd, 0xcc,
            0x4c, 0x3e, 0x66, 0x66, 0x66, 0x3f, 0xcd, 0xcc, 0xcc, 0x3d, 0x00, 0x00, 0x80, 0x3e,
            0x9a, 0x99, 0x19, 0x3f, 0x66, 0x66, 0x66, 0x3f, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
        ],
    ),
    (
        0x0998,
        &[
            0x28, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x00, 0x00,
        ],
    ),
    (
        0x09b8,
        &[
            0x02, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        ],
    ),
    (0x0e1c, &[0x00, 0x01, 0x00, 0x00]),
    (0x0e38, &[0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00]),
    (0x0e50, &[0x00, 0x00, 0x00, 0x00]),
];

pub(crate) const SECONDARY_ROOT_EXTRA_1_RUNS: &[(usize, &[u8])] = &[
    (0x14, &[0x01]),
    (0x2c, &[0x01]),
    (0x30, &[0x01]),
    (0x40, &[0x01]),
    (0x4c, &[0x01]),
    (0x50, &[0x6a, 0x18]),
];

/// Root `+0x00`: u16 {0x04c0, 0x83a0, 0x4339, 0x0c8b}. FW_PROVEN: b000
/// 0x2164c-0x21660 and b010 0x294f0-0x29504 compare root+0 with this word and
/// abort with "Host and FW Interface is mismatched" otherwise. KEXT_PROVEN:
/// AGXArmFirmware::initFirmwareData (0xfffffe0008c31f6c) writes it to both
/// roots.
pub(crate) const G18G_26A428_INITDATA_VERSION_WORD: u64 = 0x0c8b_4339_83a0_04c0;

/// Both roots are 0xc8 bytes. FW_PROVEN: each image copies 0xc8 bytes of its
/// root (b000 0x21664-0x216a8) and requires root+0x2c != 0 (b000 0x216ac).
/// Root field offsets are unchanged from 25G76 (KEXT_PROVEN g18g_key.c:3748).
pub(crate) const G18G_26A428_ROOT_SIZE: usize = 0xc8;

/// `AGXFirmware::allocFirmwareData` allocation-size table, 24 u64.
/// KEXT_PROVEN: 25G76 __const 0xfffffe0007139820. Index names are the
/// `KX_*` constants below.
pub(crate) const KEXT_ALLOCATION_SIZES_25G76: [usize; 24] = [
    0x2708, 0x0c18, 0x1048, 0x0e10, 0x6a30, 0x0068, 0x0800, 0x0088, 0x1800, 0x04c0, 0x4000, 0x0030,
    0xe440, 0x06a8, 0x0020, 0x01b0, 0x4800, 0x28800, 0x79800, 0x9000, 0x0060, 0x0008, 0x0e78, 0x00e4,
];

/// KEXT_PROVEN: 26A428 __const 0xfffffe000714f0d8. Only entries 0 (hw_data),
/// 4 (perf/policy), 12 (status B), 13 (status config) and 22 (region C)
/// differ from 25G76.
pub(crate) const KEXT_ALLOCATION_SIZES_26A428: [usize; 24] = [
    0x2848, 0x0c18, 0x1048, 0x0e10, 0x7800, 0x0068, 0x0800, 0x0088, 0x1800, 0x04c0, 0x4000, 0x0030,
    0xe730, 0x06d0, 0x0020, 0x01b0, 0x4800, 0x28800, 0x79800, 0x9000, 0x0060, 0x0008, 0x0e80, 0x00e4,
];

pub(crate) const KX_HW_DATA: usize = 0;

pub(crate) const KX_BLOCK_C18: usize = 1;

pub(crate) const KX_BLOCK_1048: usize = 2;

pub(crate) const KX_BLOCK_E10: usize = 3;

pub(crate) const KX_POLICY: usize = 4;

pub(crate) const KX_BLOCK_68: usize = 5;

pub(crate) const KX_BLOCK_800: usize = 6;

pub(crate) const KX_BLOCK_88: usize = 7;

pub(crate) const KX_DM_RING: usize = 8;

pub(crate) const KX_MAIN_CONFIG: usize = 9;

pub(crate) const KX_CONTROL_RING: usize = 10;

pub(crate) const KX_REGION_C: usize = 22;

/// `getSizeOfFWBRNTable` returns 0 in both kexts (KEXT_PROVEN
/// 0xfffffe00089ee74c / 0xfffffe0008c507e4).
pub(crate) const KEXT_BRN_TABLE_SIZE: usize = 0;

/// Both shared allocations are rounded to one 16 KiB page (allocateSharedData).
pub(crate) const KEXT_ALLOCATION_PAGE: usize = 0x4000;

pub(crate) const SHARED_ALLOCATION_ENTRIES: usize = 25;

const fn round_up_usize(value: usize, alignment: usize) -> usize {
    (value + alignment - 1) & !(alignment - 1)
}

/// The allocateSharedData packing rule (KEXT_PROVEN 0xfffffe0008c038d8):
/// each next entry starts at `(previous offset + previous size + 0x3f) & !0x3f`.
pub(crate) const fn next_packed_allocation(offset: usize, size: usize) -> usize {
    round_up_usize(offset + size, 0x40)
}

/// Offsets of `sizes` packed by [`next_packed_allocation`], the first at 0.
pub(crate) const fn packed_allocation_offsets<const N: usize>(sizes: &[usize; N]) -> [usize; N] {
    let mut out = [0usize; N];
    let mut index = 1;
    while index < N {
        out[index] = next_packed_allocation(out[index - 1], sizes[index - 1]);
        index += 1;
    }
    out
}

/// The 25 entries of shared allocation #1 in allocation order: hw_data, the
/// 0xc18/0x1048/0xe10 blocks, perf/policy, the 0x68/0x800 blocks, the BRN
/// table, the 0x88 block, twelve data-master rings, then main config and
/// device-control ring for role 0 and for role 1 (KEXT_PROVEN descriptor
/// build, 25G76 g17g_key.c:1224-1310 / 26A428 g18g_key.c:1236-1320).
pub(crate) const fn shared_allocation_sizes(table: &[usize; 24]) -> [usize; SHARED_ALLOCATION_ENTRIES] {
    let mut sizes = [0usize; SHARED_ALLOCATION_ENTRIES];
    sizes[0] = table[KX_HW_DATA];
    sizes[1] = table[KX_BLOCK_C18];
    sizes[2] = table[KX_BLOCK_1048];
    sizes[3] = table[KX_BLOCK_E10];
    sizes[4] = table[KX_POLICY];
    sizes[5] = table[KX_BLOCK_68];
    sizes[6] = table[KX_BLOCK_800];
    sizes[7] = KEXT_BRN_TABLE_SIZE;
    sizes[8] = table[KX_BLOCK_88];
    let mut ring = 0;
    while ring < WORK_CHANNEL_COUNT {
        sizes[9 + ring] = table[KX_DM_RING];
        ring += 1;
    }
    sizes[21] = table[KX_MAIN_CONFIG];
    sizes[22] = table[KX_CONTROL_RING];
    sizes[23] = table[KX_MAIN_CONFIG];
    sizes[24] = table[KX_CONTROL_RING];
    sizes
}

/// Offsets (from the start of the shared allocation #1 backing) of every
/// object in it.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SharedClusterLayout {
    pub(crate) hw_data: usize,
    pub(crate) block_c18: usize,
    pub(crate) block_1048: usize,
    pub(crate) block_e10: usize,
    pub(crate) policy: usize,
    pub(crate) block_68: usize,
    pub(crate) block_800: usize,
    pub(crate) brn_table: usize,
    pub(crate) block_88: usize,
    /// Data-master rings in allocation order (see [`WORK_CHANNEL_DM_INDEX`]).
    pub(crate) dm_rings: [usize; WORK_CHANNEL_COUNT],
    pub(crate) main_config: [usize; 2],
    pub(crate) control_ring: [usize; 2],
    pub(crate) end: usize,
    pub(crate) total: usize,
}

pub(crate) const fn shared_cluster_layout(table: &[usize; 24]) -> SharedClusterLayout {
    let sizes = shared_allocation_sizes(table);
    let offsets = packed_allocation_offsets(&sizes);
    let mut dm_rings = [0usize; WORK_CHANNEL_COUNT];
    let mut ring = 0;
    while ring < WORK_CHANNEL_COUNT {
        dm_rings[ring] = offsets[9 + ring];
        ring += 1;
    }
    let end = offsets[24] + sizes[24];
    SharedClusterLayout {
        hw_data: offsets[0],
        block_c18: offsets[1],
        block_1048: offsets[2],
        block_e10: offsets[3],
        policy: offsets[4],
        block_68: offsets[5],
        block_800: offsets[6],
        brn_table: offsets[7],
        block_88: offsets[8],
        dm_rings,
        main_config: [offsets[21], offsets[23]],
        control_ring: [offsets[22], offsets[24]],
        end,
        total: round_up_usize(end, KEXT_ALLOCATION_PAGE),
    }
}

/// The 26A428 layouts: hw_data 0x2848 at 0, blocks at 0x2880/0x34c0/0x4540,
/// perf/policy 0x7800 at 0x5380, 0x68/0x800 at 0xcb80/0xcc00, BRN and 0x88 at
/// 0xd400, rings from 0xd4c0, main r0 0x1f4c0, control r0 0x1f980, main r1
/// 0x23980, control r1 0x23e40; status B 0x340, status config 0xea80, status
/// A 0xf180/0xc3440, tail 0x177700/0x177740/0x1777c0. Totals 0x28000 and
/// 0x178000, as in 25G76.
pub(crate) const G18G_26A428_SHARED_LAYOUT: SharedClusterLayout =
    shared_cluster_layout(&KEXT_ALLOCATION_SIZES_26A428);

/// Main `+0x254` views (KEXT_PROVEN 26A428 initFirmwareSharedData: the FW VAs
/// of the 0xc18, 0x1048, 0xe10, 0x68 and 0x800 blocks, this+0x310..0x338).
pub(crate) const G18G_26A428_BUNDLE_VIEW_OFFSETS: [usize; 5] = [
    G18G_26A428_SHARED_LAYOUT.block_c18,
    G18G_26A428_SHARED_LAYOUT.block_1048,
    G18G_26A428_SHARED_LAYOUT.block_e10,
    G18G_26A428_SHARED_LAYOUT.block_68,
    G18G_26A428_SHARED_LAYOUT.block_800,
];

pub(crate) const G18G_26A428_BUNDLE_VIEW_EXTENTS: [usize; 5] = [
    KEXT_ALLOCATION_SIZES_26A428[KX_BLOCK_C18],
    KEXT_ALLOCATION_SIZES_26A428[KX_BLOCK_1048],
    KEXT_ALLOCATION_SIZES_26A428[KX_BLOCK_E10],
    KEXT_ALLOCATION_SIZES_26A428[KX_BLOCK_68],
    KEXT_ALLOCATION_SIZES_26A428[KX_BLOCK_800],
];

/// Role-1 main `+0x481`: FW VA of the perf/policy allocation (KEXT_PROVEN).
pub(crate) const G18G_26A428_MAIN_SECONDARY_POLICY_PTR: usize = 0x481;

/// Role-1 main `+0x300`. INFERRED: the J813 value (26A428 source accel+0x10a74).
pub(crate) const G18G_26A428_MAIN_SECONDARY_KIND: u32 = 0xc;

/// J873 ADT gpu-num-perf-states. ADT_PROVEN.
pub(crate) const J873_GPU_NUM_PERF_STATES: u32 = 13;

/// Region-C ADT tunables. KEXT_PROVEN: +0x7c8 = accel+0x106b0
/// (gpu-idleoff-standby-timer; J813 +0x7c4 carried its ADT 1250) and +0x7e4 =
/// accel+0x106cc (gpu-idleoff-min_conf); ADT_PROVEN values 1500 and 0.8f.
/// +0x9c = this+0xf14 * 100 (25G76 +0x98 = this+0xf40 * 100 = 1200 on J813):
/// INFERRED to be the perf-state count again.
pub(crate) const G18G_26A428_REGION_C_ADT_WORDS: [(usize, u32); 3] = [
    (0x7c8, 1500),
    (0x7e4, 0x3f4c_cccd),
    (0x09c, J873_GPU_NUM_PERF_STATES * 100),
];

/// Stable region-C inputs from the same eight initialization snapshots.
/// +0x38 is the new 26A428 feature bit, no longer a guessed zero.
pub(crate) const G18G_26A428_REGION_C_CAPTURED_WORDS: [(usize, u32); 7] = [
    (0x0038, 1), (0x0088, 0x00008f16), (0x008c, 0x00000514),
    (0x0090, 0x00000514), (0x09b0, 0x00000200),
    (0x09dc, 0x00000032), (0x0e0c, 0x000000fa),
];

/// Encode the G18G region C: the J813 bytes, the 25G76 fields from +0x38 on
/// moved up by 4 (FW_PROVEN), the J873 ADT tunables and stable native J873
/// inputs. The new +0x38 feature bit (accel+0x6b9 bit 2) is captured as one.
pub(crate) fn encode_g18g_26a428_region_c(out: &mut [u8]) -> Result<(), InitdataError> {
    if out.len() != REGION_C_SIZE {
        return Err(InitdataError::BufferSize);
    }
    encode_region_c_for_profile(RootProfile::J813G17G25G76, out)?;
    // The J813 object never reaches its 0xe78 end; shifting in place keeps the
    // tail zero and needs no second 4 KiB buffer on the kernel stack.
    let shifted = KEXT_ALLOCATION_SIZES_25G76[KX_REGION_C];
    if out[shifted..].iter().any(|byte| *byte != 0) {
        return Err(InitdataError::BufferSize);
    }
    out.copy_within(0x38..shifted, 0x3c);
    out[0x38..0x3c].fill(0);
    for (offset, value) in G18G_26A428_REGION_C_ADT_WORDS {
        put_u32(out, offset, value);
    }
    for (offset, value) in G18G_26A428_REGION_C_CAPTURED_WORDS {
        put_u32(out, offset, value);
    }
    // Idle power-off policy word (Region C +0x30). The firmware loads
    // it on every cold wake, which undoes the IdlePowerOff(0) sent once at boot.
    // 0 keeps the scheduler idle event from powering the cores down between
    // jobs. Measured on the M6: 8.6 percent less time per op.
    put_u32(out, 0x30, 0);
    Ok(())
}

/// FW_PROVEN (jump table 0xfffffc0000006c60, 56 entries from opcode 4): the
/// opening/region-C reload handler (b000 0x3ed0) is reached by 0x15, 0x2a
/// and 0x30.
pub(crate) const G18G_26A428_CONTROL_OPENING_OPCODE_PRIMARY: u32 = 0x15;

pub(crate) const G18G_26A428_CONTROL_OPENING_OPCODE_SECONDARY: u32 = 0x2a;
