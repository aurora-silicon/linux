// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Linux-only state. SEP anti-replay records live in `xart_store`.

use crate::shim;
use kernel::prelude::*;

pub(crate) const STORE_PATH: &CStr = c"/var/lib/aurora-sep-host-state.bin";

const BLOCK_SIZE: usize = 0x8000;
const BLOCK_COUNT: usize = 72;
pub(crate) const STORE_SIZE: usize = BLOCK_SIZE * BLOCK_COUNT;
const SLOT_COUNT: usize = BLOCK_COUNT - 1;
pub(crate) const MAX_VALUE: usize = BLOCK_SIZE;

const MAGIC: [u8; 16] = *b"AURORA-SEP-STOR\x01";
const VERSION: u32 = 1;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct Key {
    pub(crate) kind: u8,
    pub(crate) uuid: [u8; 16],
}

impl Key {
    pub(crate) const fn root(kind: u8) -> Key {
        Key {
            kind,
            uuid: [0u8; 16],
        }
    }
}

#[derive(Clone, Copy)]
struct Slot {
    used: bool,
    kind: u8,
    uuid: [u8; 16],
    len: u32,
}

impl Slot {
    const FREE: Slot = Slot {
        used: false,
        kind: 0,
        uuid: [0; 16],
        len: 0,
    };

    fn matches(&self, key: &Key) -> bool {
        self.used && self.kind == key.kind && self.uuid == key.uuid
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Intent {
    None,
    Write { slot: u16, kind: u8 },
}

const SB_MAGIC: usize = 0;
const SB_VERSION: usize = 16;
const SB_SLOT_COUNT: usize = 20;
const SB_BLOCK_SIZE: usize = 24;
const SB_GENERATION: usize = 28;
const SB_INTENT_KIND: usize = 36;
const SB_INTENT_SLOT: usize = 37;
const SB_INTENT_TYPE: usize = 39;
const SB_SLOT_TABLE: usize = 64;
const SLOT_ENTRY_SIZE: usize = 24;

const INTENT_NONE: u8 = 0;
const INTENT_WRITE: u8 = 1;

// Not internally locked; the caller holds a mutex around all access.
pub(crate) struct Store {
    file: shim::StoreFile,
    slots: [Slot; SLOT_COUNT],
    generation: u64,
    pub(crate) recovered: bool,
    pub(crate) fresh: bool,
}

fn le32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn le64(buf: &[u8], off: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&buf[off..off + 8]);
    u64::from_le_bytes(v)
}

impl Store {
    pub(crate) fn open() -> Result<Store> {
        let file = shim::StoreFile::open(STORE_PATH)?;

        let mut store = Store {
            file,
            slots: [Slot::FREE; SLOT_COUNT],
            generation: 0,
            recovered: false,
            fresh: false,
        };

        let size = store.file.size()?;
        if size < STORE_SIZE as u64 {
            store.create()?;
            return Ok(store);
        }

        let mut sb = KVec::with_capacity(BLOCK_SIZE, GFP_KERNEL)?;
        sb.resize(BLOCK_SIZE, 0, GFP_KERNEL)?;
        store.file.read_exact(0, &mut sb)?;

        if sb[SB_MAGIC..SB_MAGIC + 16] != MAGIC
            || le32(&sb, SB_VERSION) != VERSION
            || le32(&sb, SB_SLOT_COUNT) as usize != SLOT_COUNT
            || le32(&sb, SB_BLOCK_SIZE) as usize != BLOCK_SIZE
        {
            store.create()?;
            return Ok(store);
        }

        store.generation = le64(&sb, SB_GENERATION);
        for i in 0..SLOT_COUNT {
            let off = SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE;
            let mut uuid = [0u8; 16];
            uuid.copy_from_slice(&sb[off + 2..off + 18]);
            store.slots[i] = Slot {
                used: sb[off] != 0,
                kind: sb[off + 1],
                uuid,
                len: le32(&sb, off + 18),
            };
        }

        // Crash recovery: writes are copy-on-write, so discard the intent, never complete it.
        let intent_kind = sb[SB_INTENT_KIND];
        if intent_kind != INTENT_NONE {
            store.recovered = true;
            let slot = u16::from_le_bytes([sb[SB_INTENT_SLOT], sb[SB_INTENT_SLOT + 1]]) as usize;
            if intent_kind == INTENT_WRITE && slot < SLOT_COUNT {
                store.slots[slot] = Slot::FREE;
            }
            store.commit()?;
        }

        Ok(store)
    }

    fn create(&mut self) -> Result<()> {
        self.slots = [Slot::FREE; SLOT_COUNT];
        self.generation = 1;
        self.fresh = true;

        let mut zero = KVec::with_capacity(BLOCK_SIZE, GFP_KERNEL)?;
        zero.resize(BLOCK_SIZE, 0, GFP_KERNEL)?;
        for b in 0..BLOCK_COUNT {
            self.file.write_all((b * BLOCK_SIZE) as u64, &zero)?;
        }

        self.commit()?;
        Ok(())
    }

    fn commit(&mut self) -> Result<()> {
        self.write_superblock(Intent::None)?;
        self.file.sync()
    }

    fn write_superblock(&mut self, intent: Intent) -> Result<()> {
        let mut sb = KVec::with_capacity(BLOCK_SIZE, GFP_KERNEL)?;
        sb.resize(BLOCK_SIZE, 0, GFP_KERNEL)?;

        sb[SB_MAGIC..SB_MAGIC + 16].copy_from_slice(&MAGIC);
        sb[SB_VERSION..SB_VERSION + 4].copy_from_slice(&VERSION.to_le_bytes());
        sb[SB_SLOT_COUNT..SB_SLOT_COUNT + 4].copy_from_slice(&(SLOT_COUNT as u32).to_le_bytes());
        sb[SB_BLOCK_SIZE..SB_BLOCK_SIZE + 4].copy_from_slice(&(BLOCK_SIZE as u32).to_le_bytes());
        sb[SB_GENERATION..SB_GENERATION + 8].copy_from_slice(&self.generation.to_le_bytes());

        let (ikind, islot, itype) = match intent {
            Intent::None => (INTENT_NONE, 0u16, 0u8),
            Intent::Write { slot, kind } => (INTENT_WRITE, slot, kind),
        };
        sb[SB_INTENT_KIND] = ikind;
        sb[SB_INTENT_SLOT..SB_INTENT_SLOT + 2].copy_from_slice(&islot.to_le_bytes());
        sb[SB_INTENT_TYPE] = itype;

        for (i, slot) in self.slots.iter().enumerate() {
            let off = SB_SLOT_TABLE + i * SLOT_ENTRY_SIZE;
            sb[off] = u8::from(slot.used);
            sb[off + 1] = slot.kind;
            sb[off + 2..off + 18].copy_from_slice(&slot.uuid);
            sb[off + 18..off + 22].copy_from_slice(&slot.len.to_le_bytes());
        }

        self.file.write_all(0, &sb)
    }

    fn find(&self, key: &Key) -> Option<usize> {
        self.slots.iter().position(|s| s.matches(key))
    }

    fn find_free(&self) -> Option<usize> {
        self.slots.iter().position(|s| !s.used)
    }

    fn block_offset(slot: usize) -> u64 {
        ((slot + 1) * BLOCK_SIZE) as u64
    }

    pub(crate) fn read(&mut self, key: &Key) -> Result<Option<KVec<u8>>> {
        if !(0xf0..=0xf5).contains(&key.kind) {
            return Err(EINVAL);
        }
        let Some(idx) = self.find(key) else {
            return Ok(None);
        };
        let len = self.slots[idx].len as usize;
        let mut value = KVec::with_capacity(len, GFP_KERNEL)?;
        value.resize(len, 0, GFP_KERNEL)?;
        if len > 0 {
            self.file.read_exact(Self::block_offset(idx), &mut value)?;
        }
        Ok(Some(value))
    }

    pub(crate) fn write(&mut self, key: &Key, value: &[u8]) -> Result<()> {
        if !(0xf0..=0xf5).contains(&key.kind) {
            return Err(EINVAL);
        }
        if value.len() > MAX_VALUE {
            return Err(ENOSPC);
        }

        let old = self.find(key);
        let fresh = self.find_free().ok_or(ENOSPC)?;

        // 1. Record the intent, durably, before touching any data block.
        self.write_superblock(Intent::Write {
            slot: fresh as u16,
            kind: key.kind,
        })?;
        self.file.sync()?;

        // 2. Fill the new block and make it durable.
        if !value.is_empty() {
            self.file.write_all(Self::block_offset(fresh), value)?;
        }
        self.file.sync()?;

        // 3. Switch the slot table, release the old block, clear the intent.
        self.slots[fresh] = Slot {
            used: true,
            kind: key.kind,
            uuid: key.uuid,
            len: value.len() as u32,
        };
        if let Some(old) = old {
            self.slots[old] = Slot::FREE;
        }
        self.generation = self.generation.wrapping_add(1);
        self.commit()
    }
}

pub(crate) const fn crc16_ccitt_false(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xffff;
    let mut i = 0;
    while i < data.len() {
        crc ^= (data[i] as u16) << 8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
            bit += 1;
        }
        i += 1;
    }
    crc
}

// Canonical CCITT-FALSE check value; guards against a reflected/XOR variant.
static_assert!(crc16_ccitt_false(b"123456789") == 0x29B1);
