// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! One authorization epoch. State locks never cover firmware or user copies.

use kernel::prelude::*;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum Phase {
    New,
    Authenticating,
    Authorized,
    Revoking,
    Dead,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum Cleanup {
    None,
    Pending,
    Running,
    Confirmed,
    Failed,
}

pub(crate) struct State {
    pub(crate) phase: Phase,
    pub(crate) cleanup: Cleanup,
    pub(crate) reason: Option<Error>,
    users: u32,
}

impl State {
    pub(crate) fn new() -> Self {
        Self {
            phase: Phase::New,
            cleanup: Cleanup::None,
            reason: None,
            users: 0,
        }
    }

    pub(crate) fn authenticate(&mut self) -> Result {
        if self.phase != Phase::New {
            return Err(EKEYREVOKED);
        }
        self.phase = Phase::Authenticating;
        self.users = 1;
        Ok(())
    }

    pub(crate) fn authenticated(&mut self, result: Result) -> bool {
        self.users -= 1;
        if self.phase == Phase::Authenticating && result.is_ok() {
            self.phase = Phase::Authorized;
            return true;
        }
        self.revoke(result.err().unwrap_or(EKEYREVOKED));
        false
    }

    pub(crate) fn acquire(&mut self) -> Result {
        if self.phase != Phase::Authorized {
            return Err(EKEYREVOKED);
        }
        self.users = self.users.checked_add(1).ok_or(EOVERFLOW)?;
        Ok(())
    }

    pub(crate) fn release(&mut self) {
        self.users -= 1;
    }

    pub(crate) fn revoke(&mut self, reason: Error) {
        if matches!(self.phase, Phase::Revoking | Phase::Dead) {
            return;
        }
        self.phase = Phase::Revoking;
        self.cleanup = Cleanup::Pending;
        self.reason = Some(reason);
    }

    pub(crate) fn claim_cleanup(&mut self) -> bool {
        if self.phase != Phase::Revoking || self.users != 0 || self.cleanup != Cleanup::Pending {
            return false;
        }
        self.cleanup = Cleanup::Running;
        true
    }

    pub(crate) fn disposed(&mut self, result: Result) {
        self.cleanup = if result.is_ok() {
            Cleanup::Confirmed
        } else {
            Cleanup::Failed
        };
        self.phase = Phase::Dead;
    }

    pub(crate) fn revocation_result(&self) -> Option<Result> {
        match self.cleanup {
            Cleanup::Confirmed => Some(Ok(())),
            Cleanup::Failed => Some(Err(EIO)),
            _ => None,
        }
    }

    pub(crate) fn claim_allowed(&self) -> bool {
        self.phase == Phase::Authorized
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_key_context_state)]
mod tests {
    use super::*;

    #[test]
    fn one_authentication_then_operations_while_authorized() {
        let mut state = State::new();
        assert_eq!(state.acquire(), Err(EKEYREVOKED));
        assert!(!state.claim_allowed());
        assert_eq!(state.authenticate(), Ok(()));
        assert_eq!(state.authenticate(), Err(EKEYREVOKED));
        assert!(state.authenticated(Ok(())));
        assert!(state.phase == Phase::Authorized && state.claim_allowed());
        assert_eq!(state.acquire(), Ok(()));
        assert_eq!(state.acquire(), Ok(()));
        state.release();
        state.release();
        // A context authenticates once.
        assert_eq!(state.authenticate(), Err(EKEYREVOKED));
    }

    #[test]
    fn a_failed_authentication_revokes_with_its_reason() {
        let mut state = State::new();
        assert_eq!(state.authenticate(), Ok(()));
        assert!(!state.authenticated(Err(ECANCELED)));
        assert!(state.phase == Phase::Revoking && state.reason == Some(ECANCELED));
        assert_eq!(state.acquire(), Err(EKEYREVOKED));
    }

    #[test]
    fn cleanup_waits_for_users_and_runs_once() {
        let mut state = State::new();
        assert_eq!(state.authenticate(), Ok(()));
        assert!(state.authenticated(Ok(())));
        assert_eq!(state.acquire(), Ok(()));
        state.revoke(EKEYEXPIRED);
        // A later reason does not replace the first.
        state.revoke(EIO);
        assert_eq!(state.reason, Some(EKEYEXPIRED));
        assert!(!state.claim_allowed());
        assert!(!state.claim_cleanup());
        assert!(state.revocation_result().is_none());
        state.release();
        assert!(state.claim_cleanup());
        assert!(!state.claim_cleanup());
        state.disposed(Ok(()));
        assert!(state.phase == Phase::Dead);
        assert_eq!(state.revocation_result(), Some(Ok(())));
        // Dead stays dead.
        state.revoke(EIO);
        assert!(state.phase == Phase::Dead);
    }

    #[test]
    fn a_failed_disposal_is_reported() {
        let mut state = State::new();
        state.revoke(EKEYREVOKED);
        assert!(state.claim_cleanup());
        state.disposed(Err(EIO));
        assert_eq!(state.revocation_result(), Some(Err(EIO)));
    }
}

