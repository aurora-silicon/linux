// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 (T8142) application contexts.
//!
//! The 25G76 firmware runs application work in a context whose lower half is the client VM's
//! root and whose upper half is a private, empty root: the firmware's own upper mappings are
//! not shared into application contexts. The context is claimed when a queue is set up and its
//! roots are published explicitly, once the work that names it has been prepared. Mapping
//! changes reach a published context through [`VmInner::tlbi_contexts_naming_root`].

use super::*;

/// A claimed M5 application context and the VMs it names.
pub(crate) struct M5ExecutionContext {
    inner: Arc<UatInner>,
    id: u8,
    low: Vm,
    high: Vm,
    published: bool,
}

impl Uat {
    /// Claims an application context for `vm`, a client VM of this UAT. Nothing is published.
    pub(crate) fn new_m5_execution_context(&self, vm: &Vm) -> Result<M5ExecutionContext> {
        self.check_client_vm(vm)?;
        let id = {
            let mut inner = self.inner.lock();
            let free = !inner.execution_contexts & EXECUTION_CONTEXT_MASK;
            if free == 0 {
                return Err(ENOSPC);
            }
            let id = free.trailing_zeros() as usize;
            inner.execution_contexts |= 1u64 << id;
            id
        };
        let high = match self.new_vm(0x8142_1000 + id as u64, self.geometry.user_range()) {
            Ok(high) => high,
            Err(error) => {
                self.inner.lock().execution_contexts &= !(1u64 << id);
                return Err(error);
            }
        };
        Ok(M5ExecutionContext {
            inner: self.inner.clone(),
            id: id as u8,
            low: vm.clone(),
            high,
            published: false,
        })
    }

    fn check_client_vm(&self, vm: &Vm) -> Result {
        let owner = vm.gpuvm.lock_inner();
        if owner.is_kernel || !core::ptr::eq(Arc::as_ptr(&owner.uat_inner), Arc::as_ptr(&self.inner))
        {
            return Err(EINVAL);
        }
        Ok(())
    }
}

impl M5ExecutionContext {
    /// The client VM named as the lower half.
    pub(crate) fn vm(&self) -> &Vm {
        &self.low
    }

    /// The hardware context number, as the firmware records name it.
    pub(crate) fn context_id(&self) -> u8 {
        self.id
    }

    /// The TTBAT words of this context.
    pub(crate) fn roots(&self) -> [u64; 2] {
        let id = usize::from(self.id);
        [tagged_root(self.low.ttb(), id), tagged_root(self.high.ttb(), id)]
    }

    /// Publishes the roots. A slot that is not empty is never replaced.
    pub(crate) fn publish(&mut self) -> Result {
        if self.published {
            return Err(EBUSY);
        }
        let [low, high] = self.roots();
        let id = usize::from(self.id);
        {
            let inner = self.inner.lock();
            inner.handoff_lock();
            let slot = &inner.ttbs()[id];
            let empty =
                slot.ttb0.load(Ordering::Acquire) == 0 && slot.ttb1.load(Ordering::Acquire) == 0;
            if empty {
                slot.ttb0.store(low, Ordering::Release);
                slot.ttb1.store(high, Ordering::Release);
                inner.publish_slot(id);
            }
            inner.handoff_unlock();
            if !empty {
                return Err(EBUSY);
            }
        }
        self.published = true;
        mem::sync();
        mem::tlbi_asid(self.id);
        mem::sync();
        Ok(())
    }

    /// Whether this context's roots are published and still in place.
    pub(crate) fn is_current(&self) -> bool {
        if !self.published {
            return false;
        }
        let [low, high] = self.roots();
        let inner = self.inner.lock();
        let slot = &inner.ttbs()[usize::from(self.id)];
        slot.ttb0.load(Ordering::Acquire) == low && slot.ttb1.load(Ordering::Acquire) == high
    }

    /// Names another client VM of the same UAT, keeping the context number. Published roots
    /// are retracted first; the caller has made sure no work of the previous VM is outstanding.
    pub(crate) fn rebind(&mut self, vm: &Vm) -> Result {
        {
            let owner = vm.gpuvm.lock_inner();
            if owner.is_kernel
                || !core::ptr::eq(Arc::as_ptr(&owner.uat_inner), Arc::as_ptr(&self.inner))
            {
                return Err(EINVAL);
            }
        }
        self.retract();
        self.low = vm.clone();
        Ok(())
    }

    fn retract(&mut self) {
        if !self.published {
            return;
        }
        let [low, high] = self.roots();
        let id = usize::from(self.id);
        {
            let inner = self.inner.lock();
            inner.handoff_lock();
            let slot = &inner.ttbs()[id];
            // Only our own pair is retracted.
            if slot.ttb0.load(Ordering::Acquire) == low && slot.ttb1.load(Ordering::Acquire) == high
            {
                slot.ttb0.store(0, Ordering::Release);
                slot.ttb1.store(0, Ordering::Release);
                inner.publish_slot(id);
            }
            inner.handoff_unlock();
        }
        mem::sync();
        mem::tlbi_asid(self.id);
        mem::sync();
        self.published = false;
    }
}

impl Drop for M5ExecutionContext {
    fn drop(&mut self) {
        self.retract();
        let id = usize::from(self.id);
        let mut inner = self.inner.lock();
        let slot = &inner.ttbs()[id];
        // A slot someone else filled is not ours to hand out again: keep it claimed.
        if slot.ttb0.load(Ordering::Acquire) == 0 && slot.ttb1.load(Ordering::Acquire) == 0 {
            inner.execution_contexts &= !(1u64 << id);
        }
    }
}
