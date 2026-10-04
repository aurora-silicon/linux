// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! CPU mappings for reserved UAT tables, distinct from page-allocator ownership.

fn page_offset(base: u64, size: usize, physical: u64) -> Option<usize> {
    if (base | physical | size as u64) & 0x3fff != 0 {
        return None;
    }
    let offset = usize::try_from(physical.checked_sub(base)?).ok()?;
    (offset.checked_add(0x4000)? <= size).then_some(offset)
}

/// Check parent links only: leaf entries describe payloads, not table pages.
fn validate_tree<E>(
    root: u64,
    ias: u8,
    oas: u32,
    contains: impl Fn(u64) -> bool,
    read: impl Fn(u64, usize) -> Result<u64, E>,
    invalid: impl Fn() -> E,
) -> Result<(), E> {
    if !matches!(ias, 39 | 42) || !(14..=52).contains(&oas) || !contains(root) {
        return Err(invalid());
    }
    let mask = ((1u64 << oas) - 1) & !0x3fff;
    for index in 0..(1usize << (ias - 36)) {
        let value = read(root, index)?;
        if value == 0 {
            continue;
        }
        if value & 3 != 3 {
            return Err(invalid());
        }
        let middle = value & mask;
        if middle == root || !contains(middle) {
            return Err(invalid());
        }
        for index in 0..2048 {
            let value = read(middle, index)?;
            if value == 0 {
                continue;
            }
            if value & 3 != 3 {
                return Err(invalid());
            }
            let leaf = value & mask;
            if leaf == root || leaf == middle || !contains(leaf) {
                return Err(invalid());
            }
        }
    }
    Ok(())
}

#[cfg(not(test))]
mod implementation {
    use super::{page_offset, validate_tree};
    use crate::uat::UAT_PGSZ;
    use core::sync::atomic::{AtomicU64, Ordering};
    use kernel::{
        addr::PhysicalAddr,
        io::mem::{Mem, MemFlag},
        io::resource::Resource,
        prelude::*,
    };

    struct Region {
        base: PhysicalAddr,
        mapping: Mem,
    }

    /// Owns CPU mappings, never ownership of the reserved physical pages.
    pub(crate) struct ReservedTables {
        regions: KVec<Region>,
    }

    // SAFETY: Mappings stay live until Drop. The only memory access exposed is
    // through aligned AtomicU64 slices; table mutation is serialized by the
    // owning UatPageTable. Moving or sharing the mapping does not unmap it.
    unsafe impl Send for ReservedTables {}
    unsafe impl Sync for ReservedTables {}

    impl ReservedTables {
        /// # Safety
        /// Each resource must be permanently reserved normal RAM. The caller
        /// must exclude concurrent firmware/AP writers until table ownership
        /// has been transferred, and retain this owner for all table accesses.
        /// These mappings do not authorize DMA or establish firmware ownership.
        pub(crate) unsafe fn new(resources: KVec<Resource>) -> Result<Self> {
            unsafe {Self::new_with_flag(resources,MemFlag::WB)}
        }
        /// # Safety
        /// Same reservation and ownership as `new`. J514S maps these tables write-combining.
        pub(crate) unsafe fn new_wc(resources:KVec<Resource>)->Result<Self> {
            unsafe {Self::new_with_flag(resources,MemFlag::WC)}
        }
        unsafe fn new_with_flag(resources:KVec<Resource>,flag:MemFlag)->Result<Self> {
            let mut regions: KVec<Region> = KVec::new();
            for resource in resources {
                let base = resource.start();
                let size: usize = resource.size().try_into()?;
                if page_offset(base, size, base).is_none() {
                    return Err(EINVAL);
                }
                let end = base.checked_add(size as u64).ok_or(EOVERFLOW)?;
                if regions
                    .iter()
                    .any(|r| base < r.base + r.mapping.size() as u64 && r.base < end)
                {
                    return Err(EINVAL);
                }
                // SAFETY: Caller supplies reserved normal RAM and excludes
                // concurrent writers. WB matches the firmware's existing AP
                // mapping type. Publication is the page-table owner's duty.
                let mapping = unsafe { Mem::try_new(resource, flag.into()) }?;
                regions.push(Region { base, mapping }, GFP_KERNEL)?;
            }
            if regions.is_empty() {
                return Err(EINVAL);
            }
            Ok(Self { regions })
        }

        pub(crate) fn contains(&self, physical: PhysicalAddr) -> bool {
            self.regions
                .iter()
                .any(|r| page_offset(r.base, r.mapping.size(), physical).is_some())
        }

        pub(crate) fn with_page<T>(
            &self,
            physical: PhysicalAddr,
            cb: impl FnOnce(&[AtomicU64]) -> Result<T>,
        ) -> Result<T> {
            for region in &self.regions {
                if let Some(offset) = page_offset(region.base, region.mapping.size(), physical) {
                    // SAFETY: The entire aligned page is inside this retained
                    // normal-RAM mapping. Only atomic references are exposed;
                    // the closure cannot extend their lifetime past the owner.
                    let entries = unsafe {
                        core::slice::from_raw_parts(
                            region.mapping.ptr().add(offset).cast::<AtomicU64>(),
                            UAT_PGSZ / 8,
                        )
                    };
                    return cb(entries);
                }
            }
            Err(EFAULT)
        }

        /// Validate every inherited table address before the normal walker may
        /// follow it. Leaf payloads need not be reserved table memory. Walking
        /// has exactly three levels; aliases are allowed but cycles are not.
        pub(crate) fn validate_root(&self, root: PhysicalAddr, ias: u8, oas: u32) -> Result {
            validate_tree(
                root,
                ias,
                oas,
                |physical| self.contains(physical),
                |physical, index| {
                    self.with_page(physical, |entries| {
                        Ok(entries[index].load(Ordering::Acquire))
                    })
                },
                || EINVAL,
            )
        }
    }
}

#[cfg(not(test))]
pub(crate) use implementation::ReservedTables;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reservations_cover_whole_pages_only() {
        assert_eq!(page_offset(0x10000, 0x8000, 0x14000), Some(0x4000));
        for physical in [0xc000, 0x10008, 0x18000, u64::MAX] {
            assert_eq!(page_offset(0x10000, 0x8000, physical), None);
        }
        assert_eq!(page_offset(0x10000, 0x4001, 0x10000), None);
        assert_eq!(page_offset(0x10000, 0, 0x10000), None);
    }
    #[test]
    fn inherited_graph_rejects_cycles_and_foreign_tables() {
        use std::collections::BTreeMap;
        let mut entries = BTreeMap::new();
        let reserved = |p| [0x10000, 0x14000, 0x18000].contains(&p);
        let validate = |entries: &BTreeMap<(u64, usize), u64>| {
            validate_tree(
                0x10000,
                42,
                42,
                reserved,
                |p, i| Ok(*entries.get(&(p, i)).unwrap_or(&0)),
                || (),
            )
        };
        entries.insert((0x10000, 63), 0x14003);
        entries.insert((0x14000, 2047), 0x18003);
        // Payload is outside the reserved table pool and must not be walked.
        entries.insert((0x18000, 0), 0x40000003);
        assert_eq!(validate(&entries), Ok(()));
        for invalid in [0x10003, 0x14003, 0x1c003, 0x18001] {
            entries.insert((0x14000, 2047), invalid);
            assert_eq!(validate(&entries), Err(()));
        }
        entries.insert((0x10000, 63), 0x10003);
        assert_eq!(validate(&entries), Err(()));
    }

    #[test]
    fn geometry_bounds_the_root_walk() {
        use std::cell::Cell;
        for (ias, count) in [(39, 8), (42, 64)] {
            let reads = Cell::new(0);
            assert_eq!(
                validate_tree(
                    0x10000,
                    ias,
                    42,
                    |_| true,
                    |_, _| {
                        reads.set(reads.get() + 1);
                        Ok(0)
                    },
                    || ()
                ),
                Ok(())
            );
            assert_eq!(reads.get(), count);
        }
        assert_eq!(
            validate_tree(
                0x10000,
                48,
                42,
                |_| true,
                |_, _| -> Result<u64, ()> { panic!("invalid geometry must not read") },
                || ()
            ),
            Err(())
        );
    }
}
