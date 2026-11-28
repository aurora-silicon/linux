// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounds on the memory the PMP firmware asks the kernel to hold for it.

/// The largest buffer one allocation request can ask for: its 24-bit size field,
/// which is also the mask the driver takes the size out of a request with.
pub(crate) const MALLOC_SIZE_MAX: u64 = 0xff_ffff;

/// Live allocations held for the firmware at once. A normal boot makes about ten.
pub(crate) const MAX_ALLOCS: usize = 64;

/// The total size of those allocations: four of the largest the firmware can ask for.
pub(crate) const MAX_ALLOC_BYTES: u64 = 4 * MALLOC_SIZE_MAX;

/// The highest register index a reply can carry: 16 bits, below the opcode.
pub(crate) const IOREG_INDEX_MAX: usize = 0xffff;

/// The firmware's live allocations: how many, and their total size.
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
pub(crate) struct AllocBudget {
    count: usize,
    bytes: u64,
}

impl AllocBudget {
    pub(crate) const fn new() -> Self {
        Self { count: 0, bytes: 0 }
    }

    /// Takes room for one more allocation of `size` bytes, or refuses it.
    pub(crate) fn claim(&mut self, size: u64) -> bool {
        if size > MALLOC_SIZE_MAX || self.count >= MAX_ALLOCS || size > MAX_ALLOC_BYTES - self.bytes
        {
            return false;
        }
        self.count += 1;
        self.bytes += size;
        true
    }

    /// Gives back the room of an allocation that was freed or never made.
    pub(crate) fn release(&mut self, size: u64) {
        self.count = self.count.saturating_sub(1);
        self.bytes = self.bytes.saturating_sub(size);
    }
}

/// Whether one more register, after `registered`, gets an index a reply can carry.
pub(crate) fn ioreg_fits(registered: usize) -> bool {
    registered < IOREG_INDEX_MAX
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_normal_boot_fits() {
        let mut budget = AllocBudget::new();
        for _ in 0..10 {
            assert!(budget.claim(0x4000));
        }
        assert!(budget.claim(MALLOC_SIZE_MAX));
    }

    #[test]
    fn the_number_of_live_allocations_is_bounded() {
        let mut budget = AllocBudget::new();
        let mut granted = 0;
        for _ in 0..100_000 {
            if budget.claim(0x40) {
                granted += 1;
            }
        }
        assert_eq!(granted, MAX_ALLOCS);
    }

    #[test]
    fn their_total_size_is_bounded() {
        let mut budget = AllocBudget::new();
        let mut bytes = 0;
        for _ in 0..MAX_ALLOCS {
            if budget.claim(MALLOC_SIZE_MAX) {
                bytes += MALLOC_SIZE_MAX;
            }
        }
        assert_eq!(bytes, MAX_ALLOC_BYTES);
        assert!(!budget.claim(1));
        assert!(!budget.claim(MALLOC_SIZE_MAX + 1));
    }

    #[test]
    fn a_freed_allocation_makes_room_again() {
        let mut budget = AllocBudget::new();
        for _ in 0..=MAX_ALLOCS {
            budget.claim(MALLOC_SIZE_MAX);
        }
        assert!(!budget.claim(0x1000));
        budget.release(MALLOC_SIZE_MAX);
        assert!(budget.claim(0x1000));
        assert!(!budget.claim(MALLOC_SIZE_MAX));
    }

    #[test]
    fn a_register_index_fits_its_reply() {
        assert!(ioreg_fits(0));
        assert!(ioreg_fits(340));
        assert!(ioreg_fits(IOREG_INDEX_MAX - 1));
        assert!(!ioreg_fits(IOREG_INDEX_MAX));
        assert!(!ioreg_fits(usize::MAX));
        // The index of the last one admitted stays below the opcode.
        assert_eq!(((IOREG_INDEX_MAX as u64) << 32) >> 48, 0);
    }
}
