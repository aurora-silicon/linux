// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! The initrd attach phase and its real-root completion.

use kernel::{prelude::*, sync::Arc};

use crate::SepData;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Phase {
    Normal,
    KeyStoreOnly,
    Complete,
}

impl Phase {
    pub(crate) fn new(keystore_only: bool) -> Self {
        if keystore_only {
            Self::KeyStoreOnly
        } else {
            Self::Normal
        }
    }

    pub(crate) fn needs_completion(self) -> Result<bool> {
        match self {
            Self::Normal => Err(EINVAL),
            Self::KeyStoreOnly => Ok(true),
            Self::Complete => Ok(false),
        }
    }

    /// Called only after completion succeeds; a failed open remains retryable.
    pub(crate) fn completed(&mut self) {
        *self = Self::Complete;
    }
}

/// An enclave success cannot override a refused persistent-state write.
pub(crate) fn unseal_allowed(before: u32, after: u32) -> bool {
    before == after
}

extern "C" {
    fn sep_keystore_only() -> bool;
    fn sep_complete_attach_register(
        dev: *mut c_void,
        ctx: *mut c_void,
        complete: unsafe extern "C" fn(*mut c_void) -> c_int,
    ) -> c_int;
    fn sep_complete_attach_unregister(dev: *mut c_void);
}

pub(crate) fn requested() -> bool {
    // SAFETY: the parameter is immutable after module loading.
    unsafe { sep_keystore_only() }
}

/// Holds the callback context alive until sysfs has drained all writers.
pub(crate) struct Attribute {
    sep: Arc<SepData>,
    registered: bool,
}

impl Attribute {
    pub(crate) fn register(sep: Arc<SepData>) -> Result<Self> {
        // SAFETY: the owned Arc pins the context until unregister has drained
        // sysfs callbacks. The shim keeps no pointer after unregister returns.
        kernel::error::to_result(unsafe {
            sep_complete_attach_register(
                sep.dev.as_raw().cast(),
                Arc::as_ptr(&sep).cast_mut().cast(),
                complete,
            )
        })?;
        Ok(Self {
            sep,
            registered: true,
        })
    }

    pub(crate) fn unregister(&mut self) {
        if self.registered {
            // SAFETY: matched to the successful register and called only once.
            unsafe { sep_complete_attach_unregister(self.sep.dev.as_raw().cast()) };
            self.registered = false;
        }
    }
}

impl Drop for Attribute {
    fn drop(&mut self) {
        self.unregister();
    }
}

/// # Safety
/// The shim calls with the context pinned by Attribute, under sysfs active
/// protection; unregister waits for this callback before releasing the Arc.
unsafe extern "C" fn complete(ctx: *mut c_void) -> c_int {
    // SAFETY: guaranteed by the registration above.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    match sep.complete_attach() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

#[kernel::macros::kunit_tests(apple_sep_attach)]
mod tests {
    use super::*;

    #[test]
    fn normal_attach_rejects_completion() {
        assert_eq!(Phase::new(false).needs_completion(), Err(EINVAL));
    }

    #[test]
    fn completion_is_idempotent() {
        let mut phase = Phase::new(true);
        assert_eq!(phase.needs_completion(), Ok(true));
        phase.completed();
        assert_eq!(phase.needs_completion(), Ok(false));
        phase.completed();
        assert_eq!(phase.needs_completion(), Ok(false));
    }

    #[test]
    fn failed_completion_can_retry() {
        let phase = Phase::new(true);
        assert_eq!(phase.needs_completion(), Ok(true));
        // An error never calls completed().
        assert_eq!(phase.needs_completion(), Ok(true));
    }

    #[test]
    fn refused_initrd_write_fails_unseal_closed() {
        assert!(unseal_allowed(0, 0));
        assert!(unseal_allowed(17, 17));
        assert!(!unseal_allowed(0, 1));
        assert!(!unseal_allowed(u32::MAX, 0));
    }
}
