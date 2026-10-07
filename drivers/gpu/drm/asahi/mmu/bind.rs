// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Prepared userspace mapping transactions. Host callback resources are reserved for the
//! complete ioctl before its first visible mapping changes; leaf commits retain their
//! individual interruptible GPUVM reservation acquisition and quarantine admission check.

use super::*;

pub(crate) struct PreparedUserMap {
    gem: ARef<gem::Object>,
    ctx: StepContext,
    addr: u64,
    size: u64,
    offset: u64,
    single_page: bool,
    context: context::PreparedBinding,
    _residency: Option<residency::Lease>,
}

pub(crate) struct PreparedUserUnmap {
    _residency: Option<residency::Lease>,
    pub(super) iova: u64,
    pub(super) size: u64,
    pub(super) ctx: StepContext,
}

// SAFETY: This owns a thread-safe residency lease, a numeric range and unused,
// unlinked split-node allocations; step_remap moves each node out of ctx before
// linking it, so a retained ctx never owns a linked GPUVA.
unsafe impl Send for PreparedUserUnmap {}

pub(super) enum PreparedUserBindOp {
    Map(PreparedUserMap),
    Unmap(PreparedUserUnmap),
}

pub(crate) struct PreparedUserBindBatch {
    pub(super) ops: KVec<PreparedUserBindOp>,
}

impl PreparedUserBindBatch {
    pub(crate) fn new(capacity: usize) -> Result<Self> {
        Ok(Self {
            ops: KVec::with_capacity(capacity, GFP_KERNEL)?,
        })
    }
    pub(crate) fn push_map(&mut self, map: PreparedUserMap) -> Result {
        Ok(self.ops.push(PreparedUserBindOp::Map(map), GFP_KERNEL)?)
    }
    pub(crate) fn push_unmap(&mut self, unmap: PreparedUserUnmap) -> Result {
        Ok(self
            .ops
            .push(PreparedUserBindOp::Unmap(unmap), GFP_KERNEL)?)
    }
}

impl Vm {
    /// Prepares host state without taking DMA reservations or changing any leaf.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn prepare_bind_object(
        &self,
        gem: &gem::Object,
        addr: u64,
        size: u64,
        offset: u64,
        prot: Prot,
        single_page: bool,
    ) -> Result<PreparedUserMap> {
        gem::validate_vm_binding(gem, self)?;
        let residency = self.enter_residency_metadata()?;
        let context = self.prepare_context_binding(addr, size, offset)?;
        let mut ctx = StepContext {
            new_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            prev_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            next_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            prot,
            ..Default::default()
        };
        let vm_bo = self.inner.obtain_bo(gem)?;
        {
            let needs_sgt = vm_bo.inner().inner.lock().sgt.is_none();
            if needs_sgt {
                // Pin without the BO mutex: alias construction can hold the GEM
                // reservation while acquiring this mutex.
                let sgt = gem.owned_sg_table()?;
                let mut bo = vm_bo.inner().inner.lock();
                if bo.sgt.is_none() {
                    if bo.sg_vec.is_none() {
                        let mut segments = KVVec::new();
                        let mut offset = 0;
                        for range in sgt.iter() {
                            let address = range.dma_address() as usize;
                            let length = range.dma_len() as usize;
                            segments.push((offset, address..address + length), GFP_KERNEL)?;
                            offset += length;
                        }
                        bo.sg_vec = Some(segments);
                    }
                    bo.sgt = Some(sgt);
                }
                // Release the mutex before a losing initializer drops its SG
                // owner. The declaration order also preserves this on errors.
                drop(bo);
            }
        }
        ctx.vm_bo = Some(vm_bo);
        if (addr | size | offset) & UAT_PGMSK as u64 != 0 {
            return Err(EINVAL);
        }
        Ok(PreparedUserMap {
            _residency: residency,
            gem: gem.into(),
            ctx,
            addr,
            size,
            offset,
            single_page,
            context,
        })
    }

    pub(crate) fn prepare_user_unmap(&self, iova: u64, size: u64) -> Result<PreparedUserUnmap> {
        if (iova | size) & UAT_PGMSK as u64 != 0 || !self.inner.range_valid(iova, size) {
            return Err(EINVAL);
        }
        let residency = self.enter_residency_metadata()?;
        Ok(PreparedUserUnmap {
            _residency: residency,
            iova,
            size,
            ctx: StepContext {
                prev_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
                next_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
                ..Default::default()
            },
        })
    }

    /// Commits in ioctl order. Quarantine may defer an all-unmap batch, but can
    /// never allow a map to replace a mapping still owned by failed work.
    pub(crate) fn commit_prepared_user_bind_batch(
        &self,
        batch: &mut PreparedUserBindBatch,
    ) -> Result {
        if self.defer_bind_batch(batch, 0)? {
            return Ok(());
        }
        let mut index = 0;
        while index < batch.ops.len() {
            let result = match &mut batch.ops[index] {
                PreparedUserBindOp::Map(map) => {
                    let mut inner = self.inner.exec_lock(Some(&map.gem), true)?;
                    match self.mapping_commit() {
                        Ok(commit) => {
                            let (flags, repeat) = if map.single_page {
                                (gpuvm::GpuVaFlags::REPEAT, UAT_PGSZ as u32)
                            } else {
                                (gpuvm::GpuVaFlags::NONE, 0)
                            };
                            let result = inner.sm_map(
                                &mut map.ctx,
                                map.addr,
                                map.size,
                                map.offset,
                                repeat,
                                flags,
                            );
                            let retired = result.is_ok().then(|| {
                                let retired = self.commit_context_binding(&map.gem, &map.context);
                                self.track_shared_binding(
                                    &map.gem,
                                    map.addr,
                                    map.size,
                                    map.offset,
                                    map.single_page,
                                );
                                retired
                            });
                            drop(inner);
                            drop(commit);
                            drop(retired);
                            Some(result)
                        }
                        Err(error) if error == EIO => {
                            drop(inner);
                            None
                        }
                        Err(error) => return Err(error),
                    }
                }
                PreparedUserBindOp::Unmap(unmap) => {
                    let mut inner = self.inner.exec_lock(None, true)?;
                    match self.mapping_commit() {
                        Ok(commit) => {
                            let result = inner.sm_unmap(&mut unmap.ctx, unmap.iova, unmap.size);
                            let retired = result.is_ok().then(|| {
                                let retired = self.untrack_context_ranges(core::iter::once(
                                    unmap.iova..unmap.iova + unmap.size,
                                ));
                                self.untrack_shared_range(unmap.iova, unmap.size);
                                retired
                            });
                            drop(inner);
                            drop(commit);
                            drop(retired);
                            Some(result)
                        }
                        Err(error) if error == EIO => {
                            drop(inner);
                            None
                        }
                        Err(error) => return Err(error),
                    }
                }
            };
            if let Some(result) = result {
                result?;
                index += 1;
            } else if self.defer_bind_batch(batch, index)? {
                return Ok(());
            }
        }
        Ok(())
    }
}
