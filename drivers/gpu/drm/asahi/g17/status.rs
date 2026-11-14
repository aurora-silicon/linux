// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! VM admission errors and client failure reporting. No firmware state is accessed here.

use crate::debug::*;
use crate::gem;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};
use kernel::{new_mutex, prelude::*, str::BStr, sync::Mutex, task::Pid};

const DEBUG_CLASS: DebugFlags = DebugFlags::Errors;

const MIRROR_MAX_SIZE: usize = 64 * 1024;

/// A VM and its accepted jobs share this status, including after queue destruction.
/// Client failure is separate from hardware admission: recovery may preserve the
/// VM while failed accepted work must still be reported before signalling fences.
#[pin_data]
pub(crate) struct VmStatus {
    error: AtomicI32,
    reported_error: AtomicI32,
    /// The client's VM handle is gone: no client can see a lost device any more.
    client_closed: AtomicBool,
    /// A queue of this VM went away while its work was still in the GPU, and the wait for that
    /// work ended. Never cleared.
    teardown_expired: AtomicBool,
    /// A failure of this VM has been logged as a warning.
    warned: AtomicBool,
    /// A GPU recovery found this VM's work guilty.
    recovery_guilty: AtomicBool,
    /// The process that created the VM, named in the failure report.
    tgid: Pid,
    comm: [u8; 16],
    /// The client's mapped words, each retained with its object only while the
    /// client's VM handle exists; released by `release_mirror` at VM close. The
    /// fence-error count lives here too, so it and its word change together.
    #[pin]
    mirror: Mutex<Mirrors>,
}

/// One aligned word inside the retained, vmapped mirror object.
struct Mirror<T> {
    word: *mut T,
    _owner: gem::ObjectRef,
}
// SAFETY: The word points into the mapping the retained owner keeps alive; it is
// only accessed atomically through this pointer while the mutex is held.
unsafe impl<T> Send for Mirror<T> {}

struct Mirrors {
    /// The status word (DRM_ASAHI_PARAM_GROUP_VM_STATUS_MIRROR).
    status: Option<Mirror<i32>>,
    /// Output fences of this VM's submissions that signalled with an error. One
    /// increment per failed fence: a u64 never wraps.
    fence_errors: u64,
    /// The word that mirrors `fence_errors` (DRM_ASAHI_PARAM_GROUP_VM_FENCE_ERROR_MIRROR).
    fence_error_word: Option<Mirror<u64>>,
}

/// Check one aligned `T` inside a GEM object a client registers, and map it.
fn map_word<T>(owner: &mut gem::ObjectRef, offset: usize) -> Result<*mut T> {
    if owner.size() > MIRROR_MAX_SIZE
        || offset % core::mem::align_of::<T>() != 0
        || offset
            .checked_add(core::mem::size_of::<T>())
            .is_none_or(|end| end > owner.size())
    {
        return Err(EINVAL);
    }
    let base = owner.vmap()?.addr.cast::<u8>();
    // SAFETY: The checked range is inside the page-aligned mapping, which the
    // caller retains with `owner` for as long as it keeps the pointer.
    Ok(unsafe { base.add(offset) }.cast::<T>())
}

impl VmStatus {
    /// Called from the creating client's ioctl: the VM records who it belongs to.
    pub(crate) fn new() -> impl PinInit<Self> {
        let task = kernel::current!();
        // SAFETY: `comm` is a fixed array inside the live current task. A concurrent rename by
        // another thread of the process may tear the copy, as with C's unlocked `current->comm`.
        let comm: [u8; 16] = unsafe {
            core::ptr::read_volatile(core::ptr::addr_of!((*task.as_ptr()).comm).cast())
        };
        let tgid = task.tgid();
        pin_init!(Self {
            error: AtomicI32::new(0),
            reported_error: AtomicI32::new(0),
            client_closed: AtomicBool::new(false),
            teardown_expired: AtomicBool::new(false),
            warned: AtomicBool::new(false),
            recovery_guilty: AtomicBool::new(false),
            tgid,
            comm,
            mirror <- new_mutex!(
                Mirrors {
                    status: None,
                    fence_errors: 0,
                    fence_error_word: None,
                },
                "Asahi VM status mirror"
            ),
        })
    }

    pub(crate) fn get(&self) -> i32 {
        self.error.load(Ordering::Acquire)
    }

    pub(crate) fn note_recovery_guilty(&self) {
        self.recovery_guilty.store(true, Ordering::Release);
    }

    /// A GPU recovery found this VM guilty: it hung or faulted the GPU, unlike a VM that
    /// failed only for its own client.
    pub(crate) fn recovery_guilty(&self) -> bool {
        self.recovery_guilty.load(Ordering::Acquire)
    }

    /// The client's VM handle is gone (`release_mirror`): the client submits nothing more.
    pub(crate) fn client_closed(&self) -> bool {
        self.client_closed.load(Ordering::Acquire)
    }

    /// Record permanent admission failure and report it to the client.
    #[track_caller]
    pub(crate) fn record(&self, error: Error) {
        self.report_failure(error);
        let _ = self.error.compare_exchange(
            0,
            error.to_errno(),
            Ordering::SeqCst,
            Ordering::SeqCst,
        );
    }

    pub(crate) fn reported_error(&self) -> i32 {
        self.reported_error.load(Ordering::Acquire)
    }

    /// Report failed accepted work without preventing recovery of the VM.
    /// Every reporter publishes the first error before returning: a losing
    /// reporter may signal its fence before the winning reporter resumes.
    ///
    /// The first failure reported to a VM is logged once, with the reporting site: the client treats
    /// any reported failure as a lost device, and nothing else in the kernel log names it.
    /// Teardown of a closed client fails its queued work with ENOENT (VM context gone),
    /// ECANCELED (queue destroyed) or, when the wait for work still in the GPU ends
    /// ([`Self::note_teardown_expired`]), ETIMEDOUT; nobody can see that, so it is only a debug
    /// message, and the first other failure of that VM, such as device loss (EIO, ENODEV, or
    /// ETIMEDOUT while no teardown wait of the VM has ended), is still logged as a warning.
    #[track_caller]
    pub(crate) fn report_failure(&self, error: Error) {
        let first = self.reported_error.compare_exchange(
            0,
            error.to_errno(),
            Ordering::SeqCst,
            Ordering::SeqCst,
        );
        let at = core::panic::Location::caller();
        let file = at.file().rsplit("asahi/").next().unwrap_or(at.file());
        if self.unobserved_teardown(error) {
            if first.is_ok() {
                mod_pr_debug!(
                    "GPU work of a closed client failed ({:?}, reported at {}:{})\n",
                    error,
                    file,
                    at.line()
                );
            }
        } else if !self.warned.swap(true, Ordering::AcqRel) {
            let len = self.comm.iter().position(|&b| b == 0).unwrap_or(self.comm.len());
            pr_warn!(
                "GPU work of a client failed ({:?}, reported at {}:{}, client {}[{}]); the client sees a lost device\n",
                error,
                file,
                at.line(),
                BStr::from_bytes(&self.comm[..len]),
                self.tgid
            );
        }
        let error = match first {
            Ok(_) => error.to_errno(),
            Err(first) => first,
        };
        let mirrors = self.mirror.lock();
        if let Some(mirror) = mirrors.status.as_ref() {
            // SAFETY: attach_mirror stored only an aligned word within a mapping the
            // retained owner keeps alive while this entry exists under the mutex.
            unsafe { AtomicI32::from_ptr(mirror.word) }.store(error, Ordering::SeqCst);
        }
    }

    /// Count an output fence of this VM that is about to signal with an error, any errno.
    /// The caller signals the fence only after this returns: a client that sees the fence
    /// signalled and then reads its registered word sees this increment.
    ///
    /// Only the kernel writes the word, with a single store of its own count under the
    /// mutex (never a read-modify-write of client memory), so concurrent counts reach the
    /// word in order and it never goes back.
    pub(crate) fn count_fence_error(&self) {
        let mut mirrors = self.mirror.lock();
        mirrors.fence_errors = mirrors.fence_errors.wrapping_add(1);
        if let Some(mirror) = mirrors.fence_error_word.as_ref() {
            // SAFETY: attach_fence_error_mirror stored only an 8-byte aligned word within a
            // mapping the retained owner keeps alive while this entry exists under the mutex.
            unsafe { AtomicU64::from_ptr(mirror.word) }
                .store(mirrors.fence_errors, Ordering::Release);
        }
    }

    /// Output fences of this VM that signalled with an error, counted so far.
    #[cfg(CONFIG_KUNIT = "y")]
    pub(crate) fn fence_errors(&self) -> u64 {
        self.mirror.lock().fence_errors
    }

    /// A teardown error of a client whose VM handle is already gone.
    pub(crate) fn unobserved_teardown(&self, error: Error) -> bool {
        self.client_closed.load(Ordering::Acquire)
            && (error == ENOENT
                || error == ECANCELED
                || (error == ETIMEDOUT && self.teardown_expired.load(Ordering::Acquire)))
    }

    /// A queue teardown stopped waiting for this VM's work, which now fails with ETIMEDOUT.
    pub(crate) fn note_teardown_expired(&self) {
        self.teardown_expired.store(true, Ordering::Release);
    }

    /// Rejected submissions and contention leave the VM usable.
    #[track_caller]
    pub(crate) fn record_if_device_loss(&self, error: Error) {
        if error == EIO || error == ETIMEDOUT || error == ENODEV {
            self.record(error);
        }
    }

    /// Retain one mapped GEM word until the client's VM handle is dropped.
    ///
    /// release_mirror marks the client closed before it takes the mutex, so a registration
    /// racing with VM destruction is refused (ENOENT) rather than kept past it. The last
    /// reference to a status can then drop in atomic context (a fence's last put): with
    /// both mirrors released, dropping it releases no object and sleeps nowhere.
    pub(crate) fn attach_mirror(&self, mut owner: gem::ObjectRef, offset: usize) -> Result {
        let word = map_word::<i32>(&mut owner, offset)?;
        let mut mirrors = self.mirror.lock();
        if self.client_closed() {
            return Err(ENOENT);
        }
        if mirrors.status.is_some() {
            return Err(EBUSY);
        }
        // No reporter runs while the mutex is held, so the word holds the first
        // reported error (or zero) when it becomes visible.
        let error = self.reported_error.load(Ordering::SeqCst);
        // SAFETY: The retained owner keeps this checked, aligned word mapped.
        unsafe { AtomicI32::from_ptr(word) }.store(error, Ordering::SeqCst);
        mirrors.status = Some(Mirror {
            word,
            _owner: owner,
        });
        Ok(())
    }

    /// Retain one mapped, 8-byte aligned GEM word that mirrors the fence-error count
    /// until the client's VM handle is dropped. The word starts at the current count.
    pub(crate) fn attach_fence_error_mirror(
        &self,
        mut owner: gem::ObjectRef,
        offset: usize,
    ) -> Result {
        let word = map_word::<u64>(&mut owner, offset)?;
        let mut mirrors = self.mirror.lock();
        // As in attach_mirror: a registration racing with VM destruction is refused.
        if self.client_closed() {
            return Err(ENOENT);
        }
        if mirrors.fence_error_word.is_some() {
            return Err(EBUSY);
        }
        // No count runs while the mutex is held, so the word holds the current count when
        // it becomes visible, and every later count stores after this.
        // SAFETY: The retained owner keeps this checked, aligned word mapped.
        unsafe { AtomicU64::from_ptr(word) }.store(mirrors.fence_errors, Ordering::Release);
        mirrors.fence_error_word = Some(Mirror {
            word,
            _owner: owner,
        });
        Ok(())
    }

    /// The client closed its DRM file: nobody sees the teardown errors of its queued work any
    /// more, even before its VM handle goes (the file drops its queues first).
    pub(crate) fn mark_client_closed(&self) {
        self.client_closed.store(true, Ordering::Release);
    }

    /// The client's VM handle is gone and nobody reads the mirrors any more. The
    /// mappings and object references drop outside the mutex (GEM release may sleep).
    /// Later fence errors are still counted, but no longer stored anywhere.
    pub(crate) fn release_mirror(&self) {
        self.mark_client_closed();
        let taken = {
            let mut mirrors = self.mirror.lock();
            (mirrors.status.take(), mirrors.fence_error_word.take())
        };
        drop(taken);
    }
}

#[kernel::macros::kunit_tests(asahi_g17_vm_status)]
mod tests {
    use super::VmStatus;
    use core::sync::atomic::Ordering;
    use kernel::prelude::*;

    #[test]
    fn only_a_closed_client_hides_teardown_errors() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        assert!(!status.unobserved_teardown(ENOENT));
        assert!(!status.unobserved_teardown(ECANCELED));
        status.release_mirror();
        assert!(status.unobserved_teardown(ENOENT));
        assert!(status.unobserved_teardown(ECANCELED));
        assert!(!status.unobserved_teardown(ETIMEDOUT));
        assert!(!status.unobserved_teardown(EIO));
        assert!(!status.unobserved_teardown(ENODEV));
    }

    #[test]
    fn a_closed_client_hides_the_end_of_its_teardown_wait() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        // A client that only destroyed a queue still sees the failure.
        status.note_teardown_expired();
        assert!(!status.unobserved_teardown(ETIMEDOUT));
        status.mark_client_closed();
        assert!(status.unobserved_teardown(ETIMEDOUT));
        assert!(!status.unobserved_teardown(EIO));
        status.report_failure(ETIMEDOUT);
        assert!(!status.warned.load(Ordering::Acquire));
    }

    #[test]
    fn a_closing_file_hides_teardown_errors_before_its_vm_goes() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        status.mark_client_closed();
        assert!(status.unobserved_teardown(ENOENT));
        assert!(status.unobserved_teardown(ECANCELED));
        assert!(!status.unobserved_teardown(ETIMEDOUT));
        status.report_failure(ENOENT);
        assert!(!status.warned.load(Ordering::Acquire));
        assert!(status.mirror.lock().status.is_none());
    }

    #[test]
    fn a_closed_client_still_keeps_its_first_error() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        status.release_mirror();
        status.report_failure(ENOENT);
        assert!(!status.warned.load(Ordering::Acquire));
        status.report_failure(ETIMEDOUT);
        assert_eq!(status.reported_error(), ENOENT.to_errno());
        assert_eq!(status.get(), 0);
        // Device loss after a hidden teardown error is still warned about.
        assert!(status.warned.load(Ordering::Acquire));
    }

    #[test]
    fn fence_errors_only_grow_and_survive_the_mirror() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        assert_eq!(status.fence_errors(), 0);
        status.count_fence_error();
        status.count_fence_error();
        assert_eq!(status.fence_errors(), 2);
        // A collateral cancellation counts without touching the reported status.
        assert_eq!(status.reported_error(), 0);
        status.release_mirror();
        assert!(status.mirror.lock().fence_error_word.is_none());
        status.count_fence_error();
        assert_eq!(status.fence_errors(), 3);
    }

    #[test]
    fn an_open_client_warns_on_its_first_error_only() {
        let Ok(status) = KBox::pin_init(VmStatus::new(), GFP_KERNEL) else {
            panic!("allocation failed");
        };
        status.report_failure(ENOENT);
        assert!(status.warned.load(Ordering::Acquire));
        assert_eq!(status.reported_error(), ENOENT.to_errno());
        status.report_failure(ETIMEDOUT);
        assert_eq!(status.reported_error(), ENOENT.to_errno());
    }
}
