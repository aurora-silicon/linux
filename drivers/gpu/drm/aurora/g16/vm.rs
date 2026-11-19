// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! User VMs on G16.
//!
//! A user VM is a shared-layer [`mmu::Vm`] created by the adopted UAT, with tracked status
//! ([`mmu::Vm::with_tracked_status`]: VM_STATUS, mirror, job guards, deferred close; no residency
//! reclaim). At creation:
//!
//! - install the client aliases of the firmware objects marked for every user VM
//!   ([`object::Objects::clients`]) as driver mappings at their fixed GVAs, and reserve
//!   [`super::hw::client::CLIENT_APERTURE`], the pass-slot range and the PB page list
//!   (`vm.install_driver_mappings`): user binds there fail (EINVAL);
//! - bind it to an exclusive TTBAT slot ([`mmu::Uat::bind`]) for as long as its [`VmState`]
//!   lives; at most [`super::hw::sizes::MAX_VMS`] such VMs (ENOSPC beyond, checked before
//!   binding so the slot allocator never blocks).
//!
//! The [`VmState`] of a VM is shared by the [`Registry`] (while the client's VM handle exists),
//! the VM's queues and its jobs. It cannot be owned by the [`mmu::Vm`] itself: the slot binding
//! holds a `Vm` reference. The registry drops its reference once the shared lifetime reports the
//! client's handle closed ([`mmu::Vm::is_closed`]), via the shared close hook. After the last
//! queue, job or retained resource owner releases it, preallocated cleanup releases the slot
//! outside the completion mutex (failed jobs are retained until reboot, and their VM's slot
//! with them).
//!
//! User page tables are cleaned to the point of coherency by the shared `pgtable.rs` (enabled by
//! the adopted UAT); user objects are prepared at creation (`gpu::Gpu::prepare_new_objects`).
//! User leaves use the shared GPU-shared protections (AttrIndex 2) for NC and WRITEBACK objects
//! alike.

use core::ops::Range;
use core::sync::atomic::{AtomicU32, Ordering};
use kernel::{
    drm::gem::BaseObject,
    new_mutex,
    prelude::*,
    sync::{aref::ARef, Arc, Mutex, UniqueArc},
};

use super::{hw, object, Gpu};
use crate::{debug::*, g17::status::VmStatus, gem, mmu, module_parameters};

const DEBUG_CLASS: DebugFlags = DebugFlags::Mmu;

/// Descriptor bits the shared page-table code adds to every user leaf: valid page, non-global.
const USER_LEAF_BITS: u64 = 0x803;
const MAX_VMS_PER_FILE: u32 = 8;

/// A firmware object aliased at a fixed GVA in every user VM.
struct ClientAlias {
    /// The object (a GEM kernel object); its first `size` bytes are mapped.
    gem: ARef<gem::Object>,
    /// Page-aligned client GVA of the object's first page.
    gva: u64,
    /// Mapped size, whole pages.
    size: usize,
    /// The exact client leaf word ([`hw::pte::LEAF_NC`], [`hw::pte::LEAF_CACHED`] or
    /// [`hw::pte::LEAF_PB_LIST`]).
    leaf: u64,
}

impl ClientAlias {
    /// The client aliases of the firmware object allocator, validated: whole pages of the
    /// object, inside a range reserved for the kernel, and a leaf word that round-trips exactly
    /// through the shared page-table code.
    fn collect(objects: &object::Objects, aliases: &mut KVec<ClientAlias>) -> Result {
        let clients = objects.clients();
        if clients.len() > aliases.capacity() {
            return Err(ENOSPC);
        }
        for object in clients {
            let Some((gva, leaf)) = object.client() else {
                continue;
            };
            let alias = ClientAlias {
                gem: object.gem(),
                gva,
                size: object.bytes(),
                leaf,
            };
            alias.check()?;
            // The caller reserved the maximum inventory before taking the completion lock.
            aliases.push(alias, GFP_NOWAIT)?;
        }
        Ok(())
    }

    fn check(&self) -> Result {
        let end = self.gva.checked_add(self.size as u64).ok_or(EINVAL)?;
        let leaf = mmu::Prot::from_pte(self.leaf).leaf_bits() | USER_LEAF_BITS;
        if self.size == 0
            || (self.gva | self.size as u64) & mmu::UAT_PGMSK as u64 != 0
            || self.size > self.gem.size()
            || leaf != self.leaf
            || !reserved_ranges()
                .iter()
                .any(|range| range.start <= self.gva && end <= range.end)
        {
            pr_err!(
                "G16: refusing client alias {:#x}+{:#x} leaf {:#x}\n",
                self.gva,
                self.size,
                self.leaf
            );
            return Err(EINVAL);
        }
        Ok(())
    }
}

/// G16 state of one user VM, shared by the registry, its queues and its jobs.
/// The final owner can disappear during resource retarget under the completion mutex.
/// Preallocated cleanup defers its reservation-taking VM destruction to a worker.
pub(crate) struct VmState(crate::cleanup::Deferred<VmStateInner>);

impl core::ops::Deref for VmState {
    type Target = VmStateInner;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

pub(crate) struct VmStateInner {
    /// The `mmu::Vm` identifier (`Gpu::ids.vm`).
    pub(crate) id: u64,
    /// The VM's TTBAT slot (ASID), for the work images' context.
    pub(crate) slot: u32,
    /// Nonzero root-installation incarnation, stable while this VM owns its TTBAT slot.
    pub(crate) generation: u8,
    /// The sticky status (shared with `file.rs`).
    pub(crate) status: Arc<VmStatus>,
    /// The VM: span checks, stream reads and job guards.
    pub(crate) vm: mmu::Vm,
    /// Private mappings are kept outside generic close's user GPUVA teardown.
    context_mappings: KVec<mmu::KernelMapping>,
    pub(crate) resources: Option<Arc<super::resources::Cache>>,
    #[allow(dead_code)]
    pub(crate) live: Arc<LiveToken>,
}

impl Drop for VmStateInner {
    fn drop(&mut self) {
        mod_pr_debug!("G16: VM {} releases context {}\n", self.id, self.slot);
        drop(self.resources.take());
        self.context_mappings.clear();
    }
}

/// Count a VM until every deferred arena has unmapped and released its binding.
pub(crate) struct LiveToken {
    registry: Arc<Registry>,
    bind: Option<mmu::VmBind>,
    file_id: u64,
}
impl Drop for LiveToken {
    fn drop(&mut self) {
        drop(self.bind.take());
        self.registry.release(self.file_id);
    }
}

/// Live-VM accounting and the VMs whose client handle is open.
#[pin_data]
pub(crate) struct Registry {
    /// VMs whose client handle may still be open, oldest first.
    #[pin]
    vms: Mutex<KVec<Arc<VmState>>>,
    /// Live [`VmState`]s (each holds a TTBAT slot) plus reservations being created.
    live: AtomicU32,
    /// Includes deferred/retained bindings after the file handle closes.
    #[pin]
    files: Mutex<KVec<(u64, u32)>>,
}

/// A reserved place among the live VMs, released unless a [`VmState`] takes it over.
struct Reservation<'a>(Option<&'a Registry>, u64);

impl Reservation<'_> {
    fn into_state(mut self) {
        self.0 = None;
    }
}

impl Drop for Reservation<'_> {
    fn drop(&mut self) {
        if let Some(registry) = self.0 {
            registry.release(self.1);
        }
    }
}

impl Registry {
    pub(crate) fn new() -> Result<Arc<Self>> {
        let vms = KVec::with_capacity(hw::sizes::MAX_VMS as usize, GFP_KERNEL)?;
        let files = KVec::with_capacity(hw::sizes::MAX_VMS as usize, GFP_KERNEL)?;
        Arc::pin_init(
            pin_init!(Self {
                vms <- new_mutex!(vms, "g16::Registry::vms"),
                live: AtomicU32::new(0),
                files <- new_mutex!(files, "g16::Registry::files"),
            }),
            GFP_KERNEL,
        )
    }

    /// Reserves a place for a new VM after dropping the registry's references to closed VMs.
    fn reserve(&self, file_id: u64) -> Result<Reservation<'_>> {
        self.prune();
        // The shared slot allocator hands out a single slot with robust isolation.
        let limit = if module_parameters::robust_isolation.value() != 0 {
            1
        } else {
            hw::sizes::MAX_VMS
        };
        self.live
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |live| {
                (live < limit).then_some(live + 1)
            })
            .map_err(|_| ENOSPC)?;
        let rollback = kernel::types::ScopeGuard::new(|| {
            self.live.fetch_sub(1, Ordering::AcqRel);
        });
        {
            let mut files = self.files.lock();
            if let Some((_, count)) = files.iter_mut().find(|(id, _)| *id == file_id) {
                if *count >= MAX_VMS_PER_FILE {
                    return Err(ENOSPC);
                }
                *count += 1;
            } else {
                files.push((file_id, 1), GFP_NOWAIT)?;
            }
        }
        rollback.dismiss();
        Ok(Reservation(Some(self), file_id))
    }

    fn release(&self, file_id: u64) {
        let mut files = self.files.lock();
        if let Some(index) = files.iter().position(|(id, _)| *id == file_id) {
            files[index].1 -= 1;
            if files[index].1 == 0 {
                let _ = files.remove(index);
            }
        }
        self.live.fetch_sub(1, Ordering::AcqRel);
    }

    /// Drop the handle owner immediately, outside the registry lock. Other references
    /// still protect jobs, both hardware units' last owners, and deferred destruction.
    pub(crate) fn close(&self, id: u64) {
        let state = {
            let mut vms = self.vms.lock();
            let index = vms.iter().position(|state| state.id == id);
            index.and_then(|index| vms.remove(index).ok())
        };
        drop(state);
    }

    /// Drops the registry's references to VMs whose client handle is closed.
    fn prune(&self) {
        loop {
            let closed = {
                let mut vms = self.vms.lock();
                let index = vms.iter().position(|state| state.vm.is_closed());
                index.and_then(|index| vms.remove(index).ok())
            };
            let Some(closed) = closed else {
                break;
            };
            // No allocation while holding the registry lock, which fail_all() also needs.
            // Final VM destruction is deferred independently of this caller's locks.
            drop(closed);
        }
    }

    pub(crate) fn counts(&self) -> (u32, usize) {
        (self.live.load(Ordering::Acquire), self.vms.lock().len())
    }

    /// The driver mappings and reserved ranges of a new VM.
    fn driver_mappings(
        vm: &mmu::Vm,
        aliases: KVec<ClientAlias>,
    ) -> Result<(KVec<mmu::KernelMapping>, KVec<Range<u64>>)> {
        let mut mappings = KVec::with_capacity(aliases.len() + 1, GFP_KERNEL)?;
        // Every M4 client root owns a fresh zeroed 64 KiB context store. It is not a
        // shared firmware object and cannot alias another client's context. The mapping
        // retains its GEM pin until all queues/jobs and deferred closes have retired.
        let context = gem::new_kernel_object_wc(vm.get_resv_obj().dev(), 0x10000)?;
        let registered = context.gem.dev().registration_guard().ok_or(ENODEV)?;
        let parent: &kernel::platform::Device<kernel::device::Bound> = registered.as_ref();
        gem::prepare_new_object(&context.gem, parent.as_ref())?;
        mappings.push(
            vm.map_at(
                0x1000000000,
                0x10000,
                &context.gem,
                mmu::Prot::from_pte(hw::pte::LEAF_NC),
                false,
            )?,
            GFP_KERNEL,
        )?;
        for alias in aliases {
            mappings.push(
                vm.map_at(
                    alias.gva,
                    alias.size,
                    &alias.gem,
                    mmu::Prot::from_pte(alias.leaf),
                    false,
                )?,
                GFP_KERNEL,
            )?;
        }
        let ranges = reserved_ranges();
        let mut reserved = KVec::with_capacity(ranges.len(), GFP_KERNEL)?;
        for range in ranges {
            reserved.push(range, GFP_KERNEL)?;
        }
        Ok((mappings, reserved))
    }

    /// The state of a VM whose client handle is open.
    pub(crate) fn find(&self, vm: &mmu::Vm) -> Result<Arc<VmState>> {
        let id = vm.id();
        self.vms
            .lock()
            .iter()
            .find(|state| state.id == id)
            .cloned()
            .ok_or(ENOENT)
    }

    /// Latches `error` on every VM whose client handle is open. The fail-stop path calls it under
    /// the device lock.
    pub(crate) fn fail_all(&self, error: Error) {
        for state in self.vms.lock().iter() {
            state.status.record(error);
        }
    }
}

/// Exact M4/Mesa reservations, including the per-client context and all private driver
/// mappings. Ordinary client roots never use the detached fixture's larger aperture.
pub(crate) fn reserved_ranges() -> [Range<u64>; 2] {
    [0x1000000000..0x1000010000, 0x7000000000..0x7100000000]
}

/// Creates a user VM (`gpu::Gpu::new_vm`).
pub(crate) fn new_vm(gpu: &Gpu, kernel_range: Range<u64>, file_id: u64) -> Result<mmu::Vm> {
    if gpu.shared.crashed.load(Ordering::Acquire) {
        return Err(EIO);
    }
    // Reserve outside the device lock: reclaim may wait for a fence whose poller needs it.
    let mut aliases = KVec::with_capacity(hw::sizes::MAX_CLIENT, GFP_KERNEL)?;
    // Snapshot only references and metadata under the device lock; map after releasing it.
    let (registry, factory) = {
        let state = gpu.shared.state.lock();
        let firmware = state.as_ref().ok_or(ENODEV)?;
        ClientAlias::collect(&firmware.objects, &mut aliases)?;
        (firmware.vms.clone(), firmware.private_factory.clone())
    };
    let reservation = registry.reserve(file_id)?;
    let id = gpu.ids.vm.next();
    let vm = gpu
        .uat
        .new_vm(id, kernel_range.clone())?
        .with_tracked_status()?;
    // Driver mappings retain their VM. Break that ownership if any later step fails,
    // including binding/state allocation and registry insertion.
    let failed_vm = kernel::types::ScopeGuard::new_with_data(vm.clone(), |vm| {
        if let Err(error) = vm.unmap_user_ranges(hw::USER_RANGE.clone(), kernel_range) {
            pr_err!("G16: closing unpublished VM failed: {:?}\n", error);
        }
        vm.bo_deferred_cleanup();
    });
    let status = vm.status().ok_or(EINVAL)?.clone();
    let (mappings, reserved) = Registry::driver_mappings(&vm, aliases)?;
    vm.install_driver_mappings(KVec::new(), reserved)?;
    // The context names the root only now: everything above is already visible to the walker.
    let bind = gpu.uat.bind(&vm)?;
    let slot = bind.slot();
    let generation = bind.generation();
    // Allocate storage before transferring the reservation, so an allocation failure
    // cannot release the live count both through a failed token and the reservation.
    let token = UniqueArc::<LiveToken>::new_uninit(GFP_KERNEL)?;
    let live: Arc<LiveToken> = token
        .write(LiveToken {
            registry: registry.clone(),
            bind: Some(bind),
            file_id,
        })
        .into();
    reservation.into_state();
    let resources = super::resources::Cache::new(
        factory,
        live.bind.as_ref().ok_or(EIO)?.clone(),
        live.clone(),
    )?;
    let inner = VmStateInner {
        id,
        slot,
        generation,
        status,
        vm: vm.clone(),
        context_mappings: mappings,
        resources: Some(resources),
        live,
    };
    let state = Arc::new(VmState(crate::cleanup::Deferred::new(inner)?), GFP_KERNEL)?;
    // Registry storage was bounded and preallocated at backend startup. NOWAIT also keeps
    // an invariant failure from entering reclaim under a lock required by fail_all().
    registry.vms.lock().push(state, GFP_NOWAIT)?;
    drop(failed_vm.dismiss());
    mod_pr_debug!("G16: VM {} context {}\n", id, slot);
    Ok(vm)
}

/// The state of a VM for a new queue.
pub(crate) fn lookup(gpu: &Gpu, vm: &mmu::Vm) -> Result<Arc<VmState>> {
    let registry = gpu.shared.state.lock().as_ref().ok_or(ENODEV)?.vms.clone();
    registry.find(vm)
}
