// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Channel, device-control and event records.

/// Records per ring.
pub(crate) const RING_SLOTS: u32 = 256;

/// Ring state cells (offsets in a 0x30-byte state block): the consumer's read index, the
/// completion index and the producer's write index.
pub(crate) const STATE_READ: usize = 0x00;
pub(crate) const STATE_COMPLETION: usize = 0x10;
pub(crate) const STATE_WRITE: usize = 0x20;

/// A submit channel record.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct SubmitRecord {
    pub(crate) unk_0: u64,
    /// Queue-info VA of the queue to run.
    pub(crate) qinfo: u64,
    pub(crate) kind: u32,
    /// Work-ring head the queue runs to.
    pub(crate) head: u16,
    pub(crate) event: u8,
    pub(crate) new_queue: u8,
}

const _: () = assert!(core::mem::size_of::<SubmitRecord>() == 0x18);
const _: () = assert!(core::mem::offset_of!(SubmitRecord, qinfo) == 0x8);
const _: () = assert!(core::mem::offset_of!(SubmitRecord, kind) == 0x10);
const _: () = assert!(core::mem::offset_of!(SubmitRecord, head) == 0x14);
const _: () = assert!(core::mem::offset_of!(SubmitRecord, event) == 0x16);
const _: () = assert!(core::mem::offset_of!(SubmitRecord, new_queue) == 0x17);

impl SubmitRecord {
    /// The little-endian record bytes.
    pub(crate) fn to_bytes(self) -> [u8; 0x18] {
        let mut b = [0u8; 0x18];
        b[0..8].copy_from_slice(&self.unk_0.to_le_bytes());
        b[8..16].copy_from_slice(&self.qinfo.to_le_bytes());
        b[16..20].copy_from_slice(&self.kind.to_le_bytes());
        b[20..22].copy_from_slice(&self.head.to_le_bytes());
        b[22] = self.event;
        b[23] = self.new_queue;
        b
    }
}

/// Device-control opcodes of the qualified 25G83 M4 firmware, and of each firmware ABI.
///
/// Before each record the firmware compares its power tally (FirmwareData) with the host's
/// generation word and stays in its power-up path while they differ; a record bumps the tally
/// only if its opcode's tally byte is set. The host sets the generation to the device-control
/// producer, so every opcode here has its tally byte set in every ABI.
pub(crate) mod devctl {
    /// Re-import Globals.
    pub(crate) const IMPORT_GLOBALS: u32 = 0x34;
    /// Start.
    pub(crate) const START: u32 = 0x0a;
    /// Initial no-op, staged before the initdata doorbell.
    pub(crate) const NOP: u32 = 0x1a;
    /// PB growth reply.
    pub(crate) const GROW_REPLY: u32 = 7;

    /// The opcodes the host sends, in one firmware ABI.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) struct Opcodes {
        pub(crate) import_globals: u32,
        pub(crate) start: u32,
        pub(crate) nop: u32,
        pub(crate) grow_reply: u32,
    }

    pub(crate) const V25G83: Opcodes = Opcodes {
        import_globals: IMPORT_GLOBALS,
        start: START,
        nop: NOP,
        grow_reply: GROW_REPLY,
    };

    /// 26B123 removed 25G83's 0x0f, so 0x10..0x1b moved down by one (the consume-only no-op
    /// 0x1a is 0x19; 26B123's 0x1a is the old 0x1b, which leaves the tally alone and must
    /// never be the boot no-op), and inserted one before the old 0x2f, so 0x2f..0x37 moved up
    /// by one (re-import Globals is 0x35). Start and the growth reply keep their numbers.
    pub(crate) const V26B123: Opcodes = Opcodes {
        import_globals: 0x35,
        start: 0x0a,
        nop: 0x19,
        grow_reply: 7,
    };
}

/// A device-control record (0x40 bytes, opcode first).
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct DevCtl {
    pub(crate) words: [u32; 16],
}

const _: () = assert!(core::mem::size_of::<DevCtl>() == 0x40);

impl DevCtl {
    pub(crate) const fn op(opcode: u32) -> Self {
        let mut words = [0; 16];
        words[0] = opcode;
        Self { words }
    }

    /// The opcode.
    pub(crate) const fn opcode(&self) -> u32 {
        self.words[0]
    }

    /// The little-endian record bytes.
    pub(crate) fn to_bytes(self) -> [u8; 0x40] {
        let mut b = [0u8; 0x40];
        for (i, w) in self.words.iter().enumerate() {
            b[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        b
    }
}

/// Event types.
pub(crate) mod event {
    pub(crate) const FAULT: u32 = 0;
    pub(crate) const STAGE: u32 = 1;
    pub(crate) const TIMEOUT: u32 = 4;
    pub(crate) const PB_GROW: u32 = 7;
    pub(crate) const CHANNEL_ERROR: u32 = 8;
}

/// An event record.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct EventRecord {
    pub(crate) words: [u32; 18],
}

const _: () = assert!(core::mem::size_of::<EventRecord>() == 0x48);
