// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Positive page-coverage results for one exclusively host-owned page table.
//! The page-table owner must invalidate before every possible table mutation.
//! This stores permissions only; it never proves physical backing identity.

#[derive(Clone, Copy)]
struct Entry { start: u64, end: u64, read: bool, write: bool }
pub(crate) struct Cache { entries: [Option<Entry>; 32], next: usize }
impl Cache {
    pub(crate) const fn new() -> Self { Self { entries: [None; 32], next: 0 } }
    pub(crate) fn invalidate(&mut self) { self.entries.fill(None); self.next = 0; }
    pub(crate) fn covers(&self, start: u64, end: u64, read: bool, write: bool) -> bool {
        start < end && self.entries.iter().flatten().any(|e|
            e.start <= start && end <= e.end && (!read || e.read) && (!write || e.write))
    }
    pub(crate) fn remember(&mut self, start: u64, end: u64, read: bool, write: bool) {
        if start >= end { return; }
        self.entries[self.next] = Some(Entry { start, end, read, write });
        self.next = (self.next + 1) % self.entries.len();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn permissions_holes_and_mutations() {
        let mut c = Cache::new();
        c.remember(0x4000, 0x8000, true, false);
        c.remember(0xc000, 0x10000, true, true);
        assert!(c.covers(0x4001, 0x7fff, true, false));
        assert!(!c.covers(0x4000, 0x8000, false, true));
        assert!(!c.covers(0x4000, 0x10000, true, false));
        assert!(!c.covers(0x8000, 0x8000, false, false));
        assert!(!c.covers(u64::MAX, 0, false, false));
        assert!(c.covers(0xc000, 0x10000, true, true));
        // An unmap/remap or permission downgrade must discard all old proof.
        c.invalidate();
        assert!(!c.covers(0xc000, 0x10000, false, true));
        c.remember(0xc000, 0x10000, true, false);
        assert!(!c.covers(0xc000, 0x10000, false, true));
        for i in 0..100 { c.remember(0x20000+i*0x4000, 0x24000+i*0x4000, true, true); }
        assert!(!c.covers(0xc000, 0x10000, true, false));
    }
}
