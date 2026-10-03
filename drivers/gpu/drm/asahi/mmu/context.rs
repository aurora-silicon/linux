// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Tracks canonical context resources and retains their fixed low presentation.
//!
//! Userspace owns low views 1 through 6. Only the reserved null view is created
//! here. Its installation is serialized per VM and retained through VM teardown.
//! Commits take bindings after the GPUVM exec lock; deferred unmaps take it
//! after the VM lifetime lock. Readers release bindings before taking either.

use super::*;
use crate::hw::t8140::CONTEXT_VIEWS;
use kernel::drm::gem::BaseObject;

struct Binding {
    gem: ARef<gem::Object>,
    offset: usize,
}
struct Bindings {
    generation: u64,
    slots: [Option<Binding>; CONTEXT_VIEWS.len()],
}
/// All arithmetic for a canonical map is checked before its page-table commit.
pub(super) struct PreparedBinding {
    address: u64,
    object_offset: u64,
    touched: u8,
    covered: u8,
}

/// Final GEM release can acquire DMA reservations. Keep replaced references
/// until the caller has released its GPUVM and mapping-admission locks.
pub(super) struct RetiredBindings([Option<Binding>; CONTEXT_VIEWS.len()]);

impl RetiredBindings {
    fn new() -> Self {
        Self(core::array::from_fn(|_| None))
    }
}

#[pin_data]
pub(super) struct ContextBindings {
    #[pin]
    bindings: Mutex<Bindings>,
    #[pin]
    install: Mutex<()>,
}
impl ContextBindings {
    pub(super) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                bindings <- new_mutex!(Bindings { generation: 0,
                    slots: core::array::from_fn(|_| None) }, "VM context resources"),
                install <- new_mutex!((), "VM context alias installation"),
            }),
            GFP_KERNEL,
        )
    }
}

impl Vm {
    pub(crate) fn validate_context_binding(
        &self,
        address: u64,
        size: u64,
        single_page: bool,
    ) -> Result {
        if self.context_bindings.is_none() || !single_page {
            return Ok(());
        }
        let end = address.checked_add(size).ok_or(EOVERFLOW)?;
        if CONTEXT_VIEWS
            .iter()
            .any(|view| address < view.source + view.size as u64 && view.source < end)
        {
            return Err(EINVAL);
        }
        Ok(())
    }

    pub(super) fn prepare_context_binding(
        &self,
        address: u64,
        size: u64,
        object_offset: u64,
    ) -> Result<PreparedBinding> {
        let mut update = PreparedBinding {
            address,
            object_offset,
            touched: 0,
            covered: 0,
        };
        if self.context_bindings.is_none() {
            return Ok(update);
        }
        let end = address.checked_add(size).ok_or(EOVERFLOW)?;
        for (index, view) in CONTEXT_VIEWS.iter().enumerate() {
            if address < view.source + view.size as u64 && view.source < end {
                update.touched |= 1 << index;
            }
            if address <= view.source && view.source + view.size as u64 <= end {
                update.covered |= 1 << index;
                usize::try_from(
                    object_offset
                        .checked_add(view.source - address)
                        .ok_or(EOVERFLOW)?,
                )?;
            }
        }
        Ok(update)
    }

    /// Called after a successful leaf commit, while its exec lock still orders
    /// mapping changes. The prepared update cannot fail after visibility.
    pub(super) fn commit_context_binding(
        &self,
        gem: &ARef<gem::Object>,
        update: &PreparedBinding,
    ) -> RetiredBindings {
        let mut retired = RetiredBindings::new();
        if update.touched == 0 {
            return retired;
        }
        let Some(context) = self.context_bindings.as_ref() else {
            return retired;
        };
        let mut bindings = context.bindings.lock();
        for (index, slot) in bindings.slots.iter_mut().enumerate() {
            if update.touched & (1 << index) != 0 {
                retired.0[index] = slot.take();
            }
            if update.covered & (1 << index) != 0 {
                // Coverage proves source >= address. Preparation checked this
                // exact sum and its usize conversion against the same view.
                let offset = (update.object_offset
                    + (CONTEXT_VIEWS[index].source - update.address)) as usize;
                *slot = Some(Binding {
                    gem: gem.clone(),
                    offset,
                });
            }
        }
        bindings.generation = bindings.generation.wrapping_add(1);
        retired
    }

    /// Successful user unmaps and accepted deferred unmaps invalidate the
    /// matching candidates. Returning their owners avoids destruction under
    /// the caller's exec or lifetime lock.
    pub(super) fn untrack_context_ranges(
        &self,
        ranges: impl Iterator<Item = Range<u64>>,
    ) -> RetiredBindings {
        let mut retired = RetiredBindings::new();
        let Some(context) = self.context_bindings.as_ref() else {
            return retired;
        };
        let mut bindings = context.bindings.lock();
        for range in ranges {
            let mut touched = false;
            for (index, view) in CONTEXT_VIEWS.iter().enumerate() {
                if range.start < view.source + view.size as u64 && view.source < range.end {
                    if let Some(binding) = bindings.slots[index].take() {
                        retired.0[index] = Some(binding);
                    }
                    touched = true;
                }
            }
            if touched {
                bindings.generation = bindings.generation.wrapping_add(1);
            }
        }
        retired
    }

    pub(crate) fn untrack_context_object(&self, gem: &gem::Object) {
        let Some(context) = self.context_bindings.as_ref() else {
            return;
        };
        let mut bindings = context.bindings.lock();
        let mut touched = false;
        for slot in &mut bindings.slots {
            if slot
                .as_ref()
                .is_some_and(|binding| core::ptr::eq(&*binding.gem, gem))
            {
                *slot = None;
                touched = true;
            }
        }
        if touched {
            bindings.generation = bindings.generation.wrapping_add(1);
        }
    }

    pub(crate) fn translate_iova(&self, address: u64) -> Result<u64> {
        self.inner
            .exec_lock(None, false)?
            .page_table
            .translate_iova(address)
    }

    /// Requires a job mapping guard before validating or installing these views.
    pub(crate) fn install_job_context_aliases(&self) -> Result {
        let context = self.context_bindings.as_ref().ok_or(EINVAL)?;
        let (generation, snapshot) = {
            let bindings = context.bindings.lock();
            let mut snapshot = KVec::with_capacity(CONTEXT_VIEWS.len(), GFP_KERNEL)?;
            for slot in &bindings.slots {
                let binding = slot.as_ref().ok_or(ENOENT)?;
                snapshot.push((binding.gem.clone(), binding.offset), GFP_KERNEL)?;
            }
            (bindings.generation, snapshot)
        };
        for (index, (gem, offset)) in snapshot.iter().enumerate().skip(1) {
            let view = CONTEXT_VIEWS[index];
            if !self.covers_range(view.source, view.size as u64, true, view.write)
                || !self.covers_range(view.low, view.size as u64, true, view.write)
            {
                return Err(EFAULT);
            }
            if offset.checked_add(view.size).ok_or(EOVERFLOW)? > gem.size() {
                return Err(ERANGE);
            }
            for page in (0..view.size).step_by(UAT_PGSZ) {
                if self.translate_iova(view.source + page as u64)?
                    != self.translate_iova(view.low + page as u64)?
                {
                    return Err(EFAULT);
                }
            }
        }
        let _install = context.install.lock();
        let view = CONTEXT_VIEWS[0];
        let (gem, offset) = &snapshot[0];
        // Null presentation requires writes in addition to the canonical view's
        // ordinary read requirement.
        if !self.covers_range(view.source, view.size as u64, true, true) {
            return Err(EFAULT);
        }
        let end = offset.checked_add(view.size).ok_or(EOVERFLOW)?;
        if end > gem.size() {
            return Err(ERANGE);
        }
        let mut existing = 0;
        for page in (0..view.size).step_by(UAT_PGSZ) {
            if self.covers_range(view.low + page as u64, UAT_PGSZ as u64, true, true) {
                existing += 1;
                if self.translate_iova(view.source + page as u64)?
                    != self.translate_iova(view.low + page as u64)?
                {
                    return Err(EFAULT);
                }
            }
        }
        if existing != 0 {
            if existing != view.size / UAT_PGSZ {
                return Err(EFAULT);
            }
            return if context.bindings.lock().generation == generation {
                Ok(())
            } else {
                Err(EAGAIN)
            };
        }
        let mut mappings = KVec::with_capacity(1, GFP_KERNEL)?;
        let mapping = self.map_in_range(
            gem,
            *offset..end,
            UAT_PGSZ as u64,
            view.low..view.low + view.size as u64,
            PROT_GPU_SHARED_RW,
            false,
        )?;
        for page in (0..view.size).step_by(UAT_PGSZ) {
            if self.translate_iova(view.source + page as u64)?
                != self.translate_iova(view.low + page as u64)?
            {
                return Err(EFAULT);
            }
        }
        mappings.push(mapping, GFP_KERNEL)?;
        if context.bindings.lock().generation != generation {
            return Err(EAGAIN);
        }
        self.append_driver_mappings(mappings)
    }
}
