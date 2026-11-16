// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Single-owner delivery transitions, independent of user copies and firmware.

use crate::key_request::Output;
use kernel::prelude::*;

pub(crate) enum State {
    Pending,
    Complete(Result<Output>),
    Abandoned(Error),
    Claimed,
}

impl State {
    pub(crate) fn pending(&self) -> bool {
        matches!(self, Self::Pending)
    }

    pub(crate) fn complete(&mut self, result: Result<Output>, expired: bool) -> bool {
        if !self.pending() {
            return false;
        }
        let result = result.map_err(|error| match error {
            ERESTARTSYS | ERESTARTNOINTR | ERESTARTNOHAND | ERESTART_RESTARTBLOCK => EINTR,
            EBUSY => EIO,
            _ => error,
        });
        // A late failure retains its diagnosis; only work without a terminal
        // failure, or successful undelivered output, can expire.
        *self = Self::Complete(match result {
            Err(error) => Err(error),
            Ok(_) if expired => Err(ETIMEDOUT),
            result => result,
        });
        true
    }

    pub(crate) fn cancel(&mut self, reason: Error) -> Result {
        match self {
            Self::Claimed => return Err(EALREADY),
            Self::Complete(Err(_)) | Self::Abandoned(_) => return Err(ENOENT),
            _ => *self = Self::Abandoned(reason),
        }
        Ok(())
    }

    pub(crate) fn expire(&mut self) {
        if matches!(self, Self::Pending | Self::Complete(Ok(_))) {
            *self = Self::Abandoned(ETIMEDOUT);
        }
    }

    pub(crate) fn ready(&self) -> bool {
        !self.pending()
    }

    pub(crate) fn claim(&mut self) -> Result<Output> {
        match self {
            Self::Pending => Err(EAGAIN),
            Self::Abandoned(error) | Self::Complete(Err(error)) => Err(*error),
            Self::Claimed => Err(ENODATA),
            Self::Complete(_) => {
                let Self::Complete(result) = core::mem::replace(self, Self::Claimed) else {
                    return Err(EIO);
                };
                result
            }
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_key_result)]
mod tests {
    use super::*;

    fn done() -> Result<Output> {
        Ok(Output::Authenticated)
    }

    #[test]
    fn a_result_is_claimed_once() {
        let mut state = State::Pending;
        assert_eq!(state.claim().err(), Some(EAGAIN));
        assert!(state.complete(done(), false));
        assert!(!state.complete(Err(EIO), false));
        assert!(matches!(state.claim(), Ok(Output::Authenticated)));
        assert_eq!(state.claim().err(), Some(ENODATA));
        assert_eq!(state.cancel(ECANCELED), Err(EALREADY));
    }

    #[test]
    fn a_cancel_before_the_claim_wins() {
        let mut state = State::Pending;
        assert!(state.complete(done(), false));
        assert_eq!(state.cancel(ECANCELED), Ok(()));
        assert_eq!(state.claim().err(), Some(ECANCELED));
        assert_eq!(state.cancel(ECANCELED), Err(ENOENT));
    }

    #[test]
    fn expiry_ends_success_but_keeps_a_failure() {
        let mut state = State::Pending;
        assert!(state.complete(done(), true));
        assert_eq!(state.claim().err(), Some(ETIMEDOUT));
        let mut state = State::Pending;
        assert!(state.complete(done(), false));
        state.expire();
        assert_eq!(state.claim().err(), Some(ETIMEDOUT));
        let mut state = State::Pending;
        assert!(state.complete(Err(EACCES), true));
        state.expire();
        assert_eq!(state.claim().err(), Some(EACCES));
    }

    #[test]
    fn restarts_and_busy_are_not_reported_as_such() {
        let mut state = State::Pending;
        assert!(state.complete(Err(ERESTARTSYS), false));
        assert_eq!(state.claim().err(), Some(EINTR));
        let mut state = State::Pending;
        assert!(state.complete(Err(EBUSY), false));
        assert_eq!(state.claim().err(), Some(EIO));
    }
}

