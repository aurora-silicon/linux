// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Architecture-neutral UAT address-space geometry.
//!
//! G13/G14 and G15-G17 all use 16 KiB pages and a three-level walk, but the
//! number of input-address bits per TTBR changed from 39 to 42. Keep this
//! geometry separate from page-table entry policy: the latter changed as well
//! on G15+ and is not implemented yet.

/// Number of bits in a UAT page offset.
pub(crate) const UAT_PGBIT: usize = 14;
/// UAT page size.
pub(crate) const UAT_PGSZ: usize = 1 << UAT_PGBIT;
/// UAT page offset mask.
pub(crate) const UAT_PGMSK: usize = UAT_PGSZ - 1;

/// Number of address bits selected by each full page-table level.
pub(crate) const UAT_LVBIT: usize = UAT_PGBIT - 3; // log2(sizeof(u64))
/// Number of translation levels.
pub(crate) const UAT_LEVELS: usize = 3;

const KERNEL_VA_WINDOW_BASE_OFFSET: u64 = 0x20_0000_0000;
/// Offset of the driver-managed kernel VA window top (see above).
const KERNEL_VA_WINDOW_TOP_OFFSET: u64 = 0x30_0000_0000;

/// UAT input-address geometry for one TTBR root.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct UatGeometry {
    input_address_bits: u8,
}

impl UatGeometry {
    pub(crate) const fn new(input_address_bits: u8) -> Option<Self> {
        match input_address_bits {
            39 | 42 => Some(Self { input_address_bits }),
            _ => None,
        }
    }

    /// Size of the virtual address space translated by one root.
    pub(crate) const fn root_size(self) -> u64 {
        1u64 << self.input_address_bits
    }

    /// Mask applied before walking a selected root.
    pub(crate) const fn root_mask(self) -> u64 {
        self.root_size() - 1
    }

    /// Number of live entries in the top-level page table.
    #[cfg(test)]
    pub(crate) const fn root_entries(self) -> usize {
        1usize << (self.input_address_bits as usize - self.root_shift())
    }

    /// Shift of the top-level page-table index.
    pub(crate) const fn root_shift(self) -> usize {
        UAT_PGBIT + (UAT_LEVELS - 1) * UAT_LVBIT
    }

    /// Top-level page-table index for a virtual address after TTBR selection.
    pub(crate) const fn root_index(self, addr: u64) -> usize {
        ((addr & self.root_mask()) >> self.root_shift()) as usize
    }

    /// TTBR selector bit immediately above the address handled by one root.
    #[cfg(test)]
    pub(crate) const fn ttbr_selector(self) -> u64 {
        self.root_size()
    }

    /// Canonical base of the upper/TTBR1 address space.
    pub(crate) const fn upper_canonical_base(self) -> u64 {
        !self.root_mask()
    }

    /// Top of the lower/user (TTBR0) address space: the per-root translation
    /// size. `1 << 39` on AGX2, `1 << 42` on AGX3.
    pub(crate) const fn user_va_top(self) -> u64 {
        self.root_size()
    }

    /// Base of the driver-managed kernel VA window inside the upper root.
    pub(crate) const fn kernel_va_base(self) -> u64 {
        self.upper_canonical_base() + KERNEL_VA_WINDOW_BASE_OFFSET
    }

    /// Top of the driver-managed kernel VA window inside the upper root.
    pub(crate) const fn kernel_va_top(self) -> u64 {
        self.upper_canonical_base() + KERNEL_VA_WINDOW_TOP_OFFSET
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_evidence_backed_geometries_are_accepted() {
        assert!(UatGeometry::new(38).is_none());
        assert!(UatGeometry::new(39).is_some());
        assert!(UatGeometry::new(40).is_none());
        assert!(UatGeometry::new(42).is_some());
        assert!(UatGeometry::new(43).is_none());
    }

    #[test]
    fn three_level_shifts_match_the_recovered_walk() {
        let g15 = UatGeometry::new(42).unwrap();

        assert_eq!(UAT_PGBIT, 14);
        assert_eq!(UAT_PGSZ, 0x4000);
        assert_eq!(UAT_PGMSK, 0x3fff);
        assert_eq!(UAT_PGBIT + UAT_LVBIT, 25);
        assert_eq!(g15.root_shift(), 36);
    }

    #[test]
    fn root_width_grows_from_three_to_six_bits() {
        let old = UatGeometry::new(39).unwrap();
        let new = UatGeometry::new(42).unwrap();

        assert_eq!(old.root_entries(), 8);
        assert_eq!(new.root_entries(), 64);
        assert_eq!(old.root_mask(), 0x7f_ffff_ffff);
        assert_eq!(new.root_mask(), 0x3ff_ffff_ffff);
    }

    #[test]
    fn ttbr_selector_tracks_the_per_root_width() {
        let old = UatGeometry::new(39).unwrap();
        let new = UatGeometry::new(42).unwrap();

        assert_eq!(old.ttbr_selector(), 0x80_0000_0000);
        assert_eq!(new.ttbr_selector(), 0x400_0000_0000);
        assert_eq!(old.upper_canonical_base(), 0xffff_ff80_0000_0000);
        assert_eq!(new.upper_canonical_base(), 0xffff_fc00_0000_0000);
    }

    #[test]
    fn user_va_top_tracks_the_per_root_width() {
        assert_eq!(UatGeometry::new(39).unwrap().user_va_top(), 1 << 39);
        assert_eq!(UatGeometry::new(42).unwrap().user_va_top(), 1 << 42);
    }

    #[test]
    fn high_kernel_addresses_use_the_full_six_bit_root_index() {
        let old = UatGeometry::new(39).unwrap();
        let new = UatGeometry::new(42).unwrap();
        let kernel_iova = 0xffff_ffa0_0000_0000;

        assert_eq!(old.root_index(kernel_iova), 2);
        assert_eq!(new.root_index(kernel_iova), 58);
    }

    #[test]
    fn kernel_window_reproduces_the_proven_39_bit_layout_and_recomputes_for_42() {
        let old = UatGeometry::new(39).unwrap();
        let new = UatGeometry::new(42).unwrap();

        assert_eq!(old.kernel_va_base(), 0xffff_ffa0_0000_0000);
        assert_eq!(old.kernel_va_top(), 0xffff_ffb0_0000_0000);

        // Same driver-chosen offset under the bit-42 selector.
        assert_eq!(new.kernel_va_base(), 0xffff_fc20_0000_0000);
        assert_eq!(new.kernel_va_top(), 0xffff_fc30_0000_0000);

        for geometry in [old, new] {
            let base = geometry.kernel_va_base();
            let top = geometry.kernel_va_top();
            assert!(base < top);
            // The window is upper-half canonical for its root: every bit
            // above the root width is set, and the window stays inside
            // one root's translation range.
            assert_eq!(
                base & !geometry.root_mask(),
                geometry.upper_canonical_base()
            );
            assert_eq!(
                (top - 1) & !geometry.root_mask(),
                geometry.upper_canonical_base()
            );
            // Both ends land in the top-level table.
            assert!(geometry.root_index(base) < geometry.root_entries());
            assert!(geometry.root_index(top - 1) < geometry.root_entries());
        }
    }

    #[test]
    fn root_indices_stay_within_the_selected_geometry() {
        for bits in [39, 42] {
            let geometry = UatGeometry::new(bits).unwrap();
            assert_eq!(geometry.root_index(0), 0);
            assert_eq!(
                geometry.root_index(geometry.root_mask()),
                geometry.root_entries() - 1
            );
        }
    }
}
