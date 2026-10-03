// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Tracks canonical context resources and retains their fixed low presentation.
//!
//! Userspace owns low views 1 through 6. Only the reserved null view is created
//! here. Its installation is serialized per VM and retained through VM teardown.

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

    /// Arithmetic is preflighted before invalidating any retained candidate.
    pub(crate) fn track_context_binding(
        &self,
        gem: ARef<gem::Object>,
        address: u64,
        size: u64,
        object_offset: u64,
    ) -> Result {
        let Some(context) = self.context_bindings.as_ref() else {
            return Ok(());
        };
        let end = address.checked_add(size).ok_or(EOVERFLOW)?;
        let mut offsets = [None; CONTEXT_VIEWS.len()];
        for (index, view) in CONTEXT_VIEWS.iter().enumerate() {
            if address <= view.source && view.source + view.size as u64 <= end {
                offsets[index] = Some(usize::try_from(
                    object_offset
                        .checked_add(view.source - address)
                        .ok_or(EOVERFLOW)?,
                )?);
            }
        }
        let mut bindings = context.bindings.lock();
        let mut touched = false;
        for (index, view) in CONTEXT_VIEWS.iter().enumerate() {
            if address < view.source + view.size as u64 && view.source < end {
                bindings.slots[index] = None;
                touched = true;
            }
            if let Some(offset) = offsets[index] {
                bindings.slots[index] = Some(Binding {
                    gem: gem.clone(),
                    offset,
                });
                touched = true;
            }
        }
        if touched {
            bindings.generation = bindings.generation.wrapping_add(1);
        }
        Ok(())
    }

    pub(crate) fn untrack_context_range(&self, range: Range<u64>) {
        let Some(context) = self.context_bindings.as_ref() else {
            return;
        };
        let mut bindings = context.bindings.lock();
        let mut touched = false;
        for (index, view) in CONTEXT_VIEWS.iter().enumerate() {
            if range.start < view.source + view.size as u64 && view.source < range.end {
                bindings.slots[index] = None;
                touched = true;
            }
        }
        if touched {
            bindings.generation = bindings.generation.wrapping_add(1);
        }
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
