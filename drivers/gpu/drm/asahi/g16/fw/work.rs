// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 25G83 HAL200 work and microsequence record sizes.
//!
//! Render records use 66 TA and 82 FS register records, 0x4c-byte
//! timestamp commands and 0x40-byte dependencies. Each register is an
//! unaligned little-endian `(u32 selector, u64 value)` pair.
//!
//! Each work type's `Layout` holds what moves between firmware ABIs: 26B123 widens the
//! per-work binding generation from 1 to 8 bytes at the same offset, shifting later fixed
//! fields by 7 bytes and leaving the 16-byte span between the completion target and the
//! auxiliary target zeroed by the host.

/// Microsequence opcodes.
pub(crate) mod op {
    pub(crate) const COMPUTE_START: u32 = 0x0b;
    pub(crate) const COMPUTE_FINALIZE: u32 = 0x0c;
    pub(crate) const TA_START: u32 = 5;
    pub(crate) const TA_FINALIZE: u32 = 6;
    pub(crate) const FS_START: u32 = 7;
    pub(crate) const FS_FINALIZE: u32 = 8;
    pub(crate) const BARRIER: u32 = 4;
    pub(crate) const INIT_BM: u32 = 6;
    pub(crate) const END: u32 = 0x40000002;
    /// Start timestamp record; the end record is `TIMESTAMP & 0xff` (3).
    pub(crate) const TIMESTAMP: u32 = 0x80000003;
    pub(crate) const TIMESTAMP_END: u32 = 3;
}

/// Whether a dependency also carries the strict/internal edge flag.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum BarrierType {
    /// Wait for the producer's firmware stamp only.
    Stamp = 0,
    /// Strict prior-command or internal fragment edge.
    Hold = 1,
}

impl BarrierType {
    /// Type 0 if `stamp_only`, else type 1.
    pub(crate) const fn from_stamp_only(stamp_only: bool) -> Self {
        if stamp_only {
            BarrierType::Stamp
        } else {
            BarrierType::Hold
        }
    }
}

/// Work type words.
pub(crate) mod work_type {
    pub(crate) const TA: u32 = 0;
    pub(crate) const FS: u32 = 1;
    pub(crate) const COMPUTE: u32 = 3;
}

/// One `(u32 selector, u64 value)` register record.
pub(crate) const REG_RECORD: usize = 12;

/// M4 25G83 compute work images and sequences.
#[path = "work/compute.rs"]
pub(crate) mod compute;

/// TA work image and microsequence.
pub(crate) mod ta {
    pub(crate) const WORK_SIZE: usize = 0x940;
    /// Active non-zero head (`0..HEAD_ACTIVE_SIZE`) and tail (`TAIL_ACTIVE_START..WORK_SIZE`)
    /// spans of a TA work image (up to 74 G15S register records plus two optional SCS records).
    /// The intervening zero gap is cleared on full scrubs and skipped on clean same-VM reuse.
    pub(crate) const HEAD_ACTIVE_SIZE: usize = 0x3e0;
    pub(crate) const TAIL_ACTIVE_START: usize = 0x740;
    pub(crate) const SEQ_SIZE: usize = 0x300;
    pub(crate) const SEQ_OBJECT: usize = 0x1000;
    pub(crate) const REGS: usize = 0x40;
    pub(crate) const TAIL: usize = 0x8c0;
    /// TPC byte span the firmware may write for this render geometry.
    pub(crate) const TPC_BYTES: usize = 0x768;
    pub(crate) const TS_START: usize = 0x1cc;
    pub(crate) const TS_END: usize = 0x21c;
    pub(crate) const FINALIZE: usize = 0x268;
    pub(crate) const END: usize = 0x2fc;
    /// u32 of the StartTA command: nonzero when the stage's register list enables SCS
    /// (`compute::START_SCS`).
    pub(crate) const START_SCS: usize = 0x60;
    /// The binding generation.
    pub(crate) const GENERATION: usize = 0x917;

    /// Hardware register list profile for TA work.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) enum RegProfile {
        G16G,
        G15S,
    }

    /// TA work fields that move between firmware ABIs.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) struct Layout {
        /// Width of the binding generation at [`GENERATION`].
        pub(crate) generation_bytes: usize,
        /// Target of the finalize record's +0x85 pointer.
        pub(crate) completion: usize,
        /// Target of the timestamp records' +0x2c ("auxiliary") pointer.
        pub(crate) auxiliary: usize,
        /// Register-list profile.
        pub(crate) profile: RegProfile,
        /// Whether TA `+0x760` carries the client TPC GVA (`true`, G15S) or firmware VA
        /// (`false`, G16G).
        pub(crate) tpc_is_gva: bool,
        /// Whether the inline UMA record ends with the extra engine byte at `+0x34`.
        pub(crate) inline_uma_engine_byte: bool,
        /// Byte at TA finalize `+0x8d`.
        pub(crate) finalize_flag: u8,
    }

    pub(crate) const V25G83: Layout = Layout {
        generation_bytes: 1,
        completion: 0x918,
        auxiliary: 0x928,
        profile: RegProfile::G16G,
        tpc_is_gva: false,
        inline_uma_engine_byte: true,
        finalize_flag: 1,
    };

    pub(crate) const V26B123: Layout = Layout {
        generation_bytes: 8,
        completion: 0x91f,
        auxiliary: 0x92f,
        profile: RegProfile::G16G,
        tpc_is_gva: false,
        inline_uma_engine_byte: true,
        finalize_flag: 1,
    };

    pub(crate) const V26B123_G15S: Layout = Layout {
        generation_bytes: 8,
        completion: 0x91f,
        auxiliary: 0x92f,
        profile: RegProfile::G15S,
        tpc_is_gva: true,
        inline_uma_engine_byte: false,
        finalize_flag: 0,
    };

    const fn valid(l: &Layout) -> bool {
        l.completion == GENERATION + l.generation_bytes
            && l.auxiliary == l.completion + 0x10
            && l.auxiliary + 8 <= WORK_SIZE
    }
    const _: () = assert!(valid(&V25G83) && valid(&V26B123) && valid(&V26B123_G15S));
    const _: () = assert!(REGS + (SEL.len() + 2) * super::REG_RECORD <= HEAD_ACTIVE_SIZE);
    const _: () = assert!(REGS + (SEL_G15S.len() + 2) * super::REG_RECORD <= HEAD_ACTIVE_SIZE);
    const _: () = assert!(HEAD_ACTIVE_SIZE <= TAIL_ACTIVE_START);
    const _: () = assert!(TAIL_ACTIVE_START <= WORK_SIZE);
    pub(crate) const SEL: [u32; 66] = [
        0x1748, 0x10141, 0x1c039, 0x1c9c8, 0x1c0a1, 0x1c031, 0x1c9c0, 0x1c051, 0x1c061, 0x10149,
        0x10139, 0x10111, 0x1c9b0, 0x10119, 0x1c9b8, 0x1c958, 0x1c950, 0x1c930, 0x1c880, 0x1c079,
        0x1c9d8, 0x10151, 0x1c199, 0x1c1a1, 0x1c1a9, 0x1c1b1, 0x1c1b9, 0x1c8f8, 0x1c0b1, 0x1c850,
        0x10131, 0x10121, 0x10129, 0x101b9, 0x1c069, 0x1c071, 0x1c081, 0x1c0a9, 0x10171, 0x10169,
        0xa309, 0x1c8e0, 0x1c8e8, 0x1c898, 0x101e1, 0x1c9e8, 0x1a099, 0x1a0a1, 0x1a069, 0x1a071,
        0x1a0c9, 0x1a0d1, 0x101c9, 0xd471, 0x1a0f1, 0x10799, 0x1c830, 0x1ca30, 0x16c39, 0x1c910,
        0xa5a1, 0xd419, 0x1ca10, 0x14a1, 0xa349, 0x10061,
    ];
    pub(crate) const SEL_G15S: [u32; 74] = [
        0x10141, 0x1c039, 0x1c9c8, 0x1c041, 0x1c9d0, 0x1c0a1, 0x1c031, 0x1c9c0, 0x1c051, 0x1c061,
        0x10149, 0x10139, 0x10111, 0x1c9b0, 0x10119, 0x1c9b8, 0x1c958, 0x1c950, 0x1c930, 0x1c880,
        0x1c898, 0x1c948, 0x1c888, 0x1c890, 0x1c918, 0x1c079, 0x1c9d8, 0x1c089, 0x1c9e0, 0x16c41,
        0x1ca40, 0x1c9a8, 0x1c920, 0x10151, 0x1c199, 0x1c1a1, 0x1c1a9, 0x1c1b1, 0x1c1b9, 0x1c8f8,
        0x1c0b1, 0x1c850, 0x10131, 0x10121, 0x10129, 0x101b9, 0x1c069, 0x1c071, 0x1c081, 0x1c0a9,
        0x10171, 0x10169, 0x12099, 0x101e1, 0x1c9e8, 0x1a099, 0x1a0a1, 0x1a069, 0x1a071, 0x1a0c9,
        0x1a0d1, 0x101c9, 0xd471, 0x1a0f1, 0x10799, 0xa5a1, 0xd419, 0x1ca10, 0x14a1, 0xa349,
        0x1c830, 0x1ca30, 0x16c39, 0x1c910,
    ];
}

/// FS work image and microsequence.
pub(crate) mod fs {
    pub(crate) const WORK_SIZE: usize = 0xca0;
    /// Active non-zero head (`0..HEAD_ACTIVE_SIZE`) and tail (`TAIL_ACTIVE_START..WORK_SIZE`)
    /// spans of an FS work image (including two optional SCS register records). The intervening
    /// zero gap is cleared on full scrubs and skipped on clean same-VM reuse.
    pub(crate) const HEAD_ACTIVE_SIZE: usize = 0x470;
    pub(crate) const TAIL_ACTIVE_START: usize = 0x780;
    pub(crate) const SEQ_SIZE: usize = 0x380;
    pub(crate) const SEQ_OBJECT: usize = 0x1000;
    pub(crate) const REGS: usize = 0x80;
    pub(crate) const TAIL: usize = 0xc08;
    pub(crate) const TS_START: usize = 0x1ec;
    pub(crate) const TS_END: usize = 0x23c;
    pub(crate) const FINALIZE: usize = 0x288;
    pub(crate) const END: usize = 0x344;
    /// u32 of the StartFS command: nonzero when the stage's register list enables SCS
    /// (`compute::START_SCS`).
    pub(crate) const START_SCS: usize = 0x78;
    /// The binding generation.
    pub(crate) const GENERATION: usize = 0xc5f;

    /// Hardware register list and work-header profile for FS work.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) enum RegProfile {
        G16G,
        G15S,
    }

    /// FS work fields that move between firmware ABIs.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) struct Layout {
        /// Width of the binding generation at [`GENERATION`].
        pub(crate) generation_bytes: usize,
        /// Target of the finalize record's +0xa5 pointer.
        pub(crate) completion: usize,
        /// Target of the timestamp records' +0x2c ("auxiliary") pointer.
        pub(crate) auxiliary: usize,
        /// u8 flag the host sets to 1.
        pub(crate) flag: usize,
        /// Register-list and header profile.
        pub(crate) profile: RegProfile,
        /// Microsequence length advertised in FS work `+0x1c`.
        pub(crate) seq_size: usize,
        /// Scene offset written to StartFS `+0x34`.
        pub(crate) scene_field_off: u64,
        /// Whether the inline UMA record ends with the extra engine byte at `+0x34`.
        pub(crate) inline_uma_engine_byte: bool,
        /// Start timestamp record offset in the FS microsequence.
        pub(crate) ts_start: usize,
        /// Finalize command offset in the FS microsequence.
        pub(crate) finalize: usize,
        /// Whether FS finalize packs the event directly at `+0xad` with `END` at `+0xb4` (G15S),
        /// rather than G16G's `+0xad/+0xb1/+0xb2` + `END` at `+0xbc`.
        pub(crate) compact_finalize: bool,
    }

    pub(crate) const V25G83: Layout = Layout {
        generation_bytes: 1,
        completion: 0xc60,
        auxiliary: 0xc70,
        flag: 0xc84,
        profile: RegProfile::G16G,
        seq_size: SEQ_SIZE,
        scene_field_off: 0x54,
        inline_uma_engine_byte: true,
        ts_start: TS_START,
        finalize: FINALIZE,
        compact_finalize: false,
    };

    pub(crate) const V26B123: Layout = Layout {
        generation_bytes: 8,
        completion: 0xc67,
        auxiliary: 0xc77,
        flag: 0xc8b,
        profile: RegProfile::G16G,
        seq_size: SEQ_SIZE,
        scene_field_off: 0x54,
        inline_uma_engine_byte: true,
        ts_start: TS_START,
        finalize: FINALIZE,
        compact_finalize: false,
    };

    pub(crate) const V26B123_G15S: Layout = Layout {
        generation_bytes: 8,
        completion: 0xc67,
        auxiliary: 0xc77,
        flag: 0xc8b,
        profile: RegProfile::G15S,
        seq_size: 0x340,
        scene_field_off: 0x4c,
        inline_uma_engine_byte: false,
        ts_start: 0x1e4,
        finalize: 0x280,
        compact_finalize: true,
    };

    const fn valid(l: &Layout) -> bool {
        l.completion == GENERATION + l.generation_bytes
            && l.auxiliary == l.completion + 0x10
            && l.flag == l.completion + 0x24
            && l.flag < WORK_SIZE
            && l.seq_size <= SEQ_SIZE
            && l.finalize + if l.compact_finalize { 0xb8 } else { 0xc0 } <= l.seq_size
    }
    const _: () = assert!(valid(&V25G83) && valid(&V26B123) && valid(&V26B123_G15S));
    const _: () = assert!(REGS + (SEL.len() + 2) * super::REG_RECORD <= HEAD_ACTIVE_SIZE);
    const _: () = assert!(REGS + (SEL_G15S.len() + 2) * super::REG_RECORD <= HEAD_ACTIVE_SIZE);
    const _: () = assert!(HEAD_ACTIVE_SIZE <= TAIL_ACTIVE_START);
    const _: () = assert!(TAIL_ACTIVE_START <= WORK_SIZE);
    pub(crate) const SEL: [u32; 82] = [
        0x1739, 0x10009, 0x15379, 0x15381, 0x15369, 0x15371, 0x15131, 0x15139, 0x100a1, 0x15069,
        0x15071, 0x16058, 0x10019, 0x100b1, 0x16030, 0x100d9, 0xa301, 0x10791, 0x16098, 0x15109,
        0x15101, 0x15021, 0x15211, 0x15049, 0x10051, 0x15321, 0x15301, 0x15309, 0x15311, 0x15319,
        0x15349, 0x15351, 0x15329, 0x15331, 0x15339, 0x15341, 0x15231, 0x15221, 0x15239, 0x15229,
        0x15401, 0x15421, 0x15409, 0x15429, 0x153c1, 0x15411, 0x153c9, 0x15431, 0x153d1, 0x15419,
        0x153d9, 0x15439, 0x16429, 0x16060, 0x16431, 0x10039, 0x16020, 0x16451, 0x15359, 0x100b8,
        0x16461, 0x16090, 0x101e9, 0x160a8, 0x16068, 0x1a0a9, 0x1a0b1, 0x1a079, 0x1a081, 0x1a0d9,
        0x1a0e1, 0x101c1, 0xd469, 0x1a0f9, 0xa5a9, 0xd429, 0x160e0, 0x1499, 0xa341, 0x1c838,
        0x1ca28, 0x10069,
    ];
    pub(crate) const SEL_G15S: [u32; 82] = [
        0x1739, 0x10009, 0x15379, 0x15381, 0x15369, 0x15371, 0x15131, 0x15139, 0x100a1, 0x15069,
        0x15071, 0x16058, 0x10019, 0x100b1, 0x16030, 0x100d9, 0x10791, 0x16098, 0x15109, 0x15101,
        0x15021, 0x15211, 0x15049, 0x10051, 0x15321, 0x15301, 0x15309, 0x15311, 0x15319, 0x15349,
        0x15351, 0x15329, 0x15331, 0x15339, 0x15341, 0x15231, 0x15221, 0x15239, 0x15229, 0x15401,
        0x15421, 0x15409, 0x15429, 0x153c1, 0x15411, 0x153c9, 0x15431, 0x153d1, 0x15419, 0x153d9,
        0x15439, 0x16429, 0x16060, 0x16431, 0x10039, 0x16020, 0x16451, 0x15359, 0x100b8, 0x16461,
        0x16090, 0x120a1, 0x101e9, 0x160a8, 0x16068, 0x160b8, 0x1a0a9, 0x1a0b1, 0x1a079, 0x1a081,
        0x1a0d9, 0x1a0e1, 0x101c1, 0xd469, 0x1a0f9, 0x1c838, 0x1ca28, 0xa5a9, 0xd429, 0x160e0,
        0x1499, 0xa341,
    ];
    pub(crate) const MAX_END_COPIES: usize = 8;
    pub(crate) const SEQ_CAPACITY: usize = SEQ_SIZE + MAX_END_COPIES * super::ts_record::SIZE;
}

/// Timestamp command including the M4 context and 64-bit UUID fields.
pub(crate) mod ts_record {
    pub(crate) const SIZE: usize = 0x4c;
}

/// Barrier record.
pub(crate) mod barrier {
    pub(crate) const SIZE: usize = 0x40;
    pub(crate) const STAMP_VA: usize = 0x4;
    pub(crate) const STAMP_VA2: usize = 0xc;
    pub(crate) const WAIT: usize = 0x14;
    pub(crate) const EVENT: usize = 0x20;
    pub(crate) const OWN: usize = 0x24;
    pub(crate) const TYPE: usize = 0x3c;
    /// UUID of render predecessor waits.
    pub(crate) const RENDER_WAIT_UUID: u32 = 0x3d420;
}

/// InitBM record (0x20 bytes).
pub(crate) mod initbm {
    pub(crate) const SIZE: usize = 0x20;
    pub(crate) const CTX: usize = 0x4;
    pub(crate) const PARAMETER_ID: usize = 0x8;
    pub(crate) const PB_MANAGER: usize = 0x14;
    pub(crate) const STAMP: usize = 0x1c;
}

/// Per-pass scene including UMA results and embedded prefix records.
pub(crate) mod scene {
    pub(crate) const SIZE: usize = 0x4000;
    /// Active prefix of a render scene touched by the host or firmware (`0x000..0x780`,
    /// rounded up to 2 KiB). Full pass scrubs zero the entire `SIZE`; clean same-VM reuse only
    /// rewrites `0..ACTIVE_SIZE`.
    pub(crate) const ACTIVE_SIZE: usize = 0x800;
}
