// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! One owned request; contexts retain delivery records, never request inputs.

use crate::{key_result::State, key_wait::Event};
use kernel::alloc::flags::GFP_KERNEL_ACCOUNT;
use kernel::prelude::*;
use kernel::sync::{
    atomic::{Acquire, Atomic, Release},
    Arc, Mutex,
};

pub(crate) enum Input {
    Authenticate(Arc<crate::key_context::Context>),
    ContextSign(
        Arc<crate::key_context::Context>,
        [u8; crate::key::DIGEST_LEN],
    ),
    ContextDecrypt(Arc<crate::key_context::Context>, KVec<u8>),
}

pub(crate) enum Output {
    Authenticated,
    Signature(crate::Secret),
    Plaintext(crate::Secret),
}

pub(crate) struct KeyRequest {
    pub(crate) blob: KVec<u8>,
    pub(crate) input: Input,
    pub(crate) deadline_ns: u64,
}

#[pin_data]
pub(crate) struct Completion {
    #[pin]
    phase: Mutex<State>,
    changed: Event,
    deadline_ns: u64,
    retired: Atomic<bool>,
}

impl Completion {
    fn new(deadline_ns: u64) -> Result<Arc<Self>> {
        Arc::pin_init(
            try_pin_init!(Self {
                phase <- kernel::new_mutex!(State::Pending),
                changed: Event::new()?,
                deadline_ns,
                retired: Atomic::new(false),
            }),
            GFP_KERNEL_ACCOUNT,
        )
    }

    pub(crate) fn is_live(&self) -> bool {
        self.phase.lock().pending() && crate::shim::monotonic_ns() < self.deadline_ns
    }

    pub(crate) fn complete(&self, result: Result<Output>) -> bool {
        let mut phase = self.phase.lock();
        let expired = crate::shim::monotonic_ns() >= self.deadline_ns;
        let accepted = phase.complete(result, expired);
        drop(phase);
        self.changed.wake();
        accepted && !expired
    }

    /// Success is deliverable only after the worker releases capture ownership.
    pub(crate) fn retire(&self) {
        self.retired.store(true, Release);
        self.changed.wake();
    }

    pub(crate) fn cancel(&self, reason: Error) -> Result {
        let result = self.phase.lock().cancel(reason);
        self.changed.wake();
        result
    }

    /// Caller may hold the context lock, never a userspace mapping.
    pub(crate) fn claim(&self) -> Result<Output> {
        let mut phase = self.phase.lock();
        if crate::shim::monotonic_ns() >= self.deadline_ns {
            phase.expire();
        }
        if matches!(*phase, State::Complete(Ok(_))) && !self.retired.load(Acquire) {
            return Err(EAGAIN);
        }
        phase.claim()
    }

    pub(crate) fn wait_ready(&self) -> Result {
        loop {
            let sequence = self.changed.sequence();
            {
                let mut phase = self.phase.lock();
                if crate::shim::monotonic_ns() >= self.deadline_ns {
                    phase.expire();
                }
                if phase.ready()
                    && (!matches!(*phase, State::Complete(Ok(_))) || self.retired.load(Acquire))
                {
                    return Ok(());
                }
            }
            if let Err(error) = self.changed.wait(sequence, self.deadline_ns) {
                let _ = self.cancel(error);
                return Ok(());
            }
        }
    }
}

/// The ids of one file's or one context's requests. Each new request takes
/// a larger id than the last; a cancel may name an id not used yet, and that
/// request is then refused with ECANCELED when it comes.
pub(crate) struct RequestIds {
    last: u64,
    cancelled_through: u64,
}

impl RequestIds {
    pub(crate) const fn new() -> Self {
        Self {
            last: 0,
            cancelled_through: 0,
        }
    }

    /// Whether `id` may start a new request.
    pub(crate) fn check(&self, id: u64) -> Result {
        if id == 0 {
            return Err(EINVAL);
        }
        if id <= self.cancelled_through {
            return Err(ECANCELED);
        }
        if id <= self.last {
            return Err(EINVAL);
        }
        Ok(())
    }

    /// Checks `id` and records it as the latest request.
    pub(crate) fn admit(&mut self, id: u64) -> Result {
        self.check(id)?;
        self.last = id;
        Ok(())
    }

    /// A cancel of `id`: true when it names an id not used yet, which is
    /// then recorded; false for one already used, which the caller ends.
    pub(crate) fn cancel_ahead(&mut self, id: u64) -> Result<bool> {
        if id == 0 {
            return Err(EINVAL);
        }
        if id > self.last {
            self.cancelled_through = self.cancelled_through.max(id);
            return Ok(true);
        }
        Ok(false)
    }

    pub(crate) fn last(&self) -> u64 {
        self.last
    }
}

pub(crate) struct Request {
    pub(crate) id: u64,
    pub(crate) data: KeyRequest,
    pub(crate) completion: Arc<Completion>,
}

impl Request {
    pub(crate) fn new(id: u64, data: KeyRequest) -> Result<Arc<Self>> {
        let completion = Completion::new(data.deadline_ns)?;
        Ok(Arc::new(
            Self {
                id,
                data,
                completion,
            },
            GFP_KERNEL_ACCOUNT,
        )?)
    }

    pub(crate) fn is_live(&self) -> bool {
        self.completion.is_live()
    }
    pub(crate) fn check_live(&self) -> Result {
        if self.is_live() {
            return Ok(());
        }
        Err(if crate::shim::monotonic_ns() >= self.data.deadline_ns {
            ETIMEDOUT
        } else {
            ECANCELED
        })
    }
    pub(crate) fn complete(&self, result: Result<Output>) {
        self.completion.complete(result);
    }
    pub(crate) fn cancel(&self, reason: Error) -> Result {
        self.completion.cancel(reason)
    }
    pub(crate) fn retire(&self) {
        self.completion.retire();
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_key_request_ids)]
mod request_ids_tests {
    use super::*;

    #[test]
    fn ids_must_rise_and_zero_is_never_one() {
        let mut ids = RequestIds::new();
        assert_eq!(ids.admit(0), Err(EINVAL));
        assert_eq!(ids.admit(5), Ok(()));
        assert_eq!(ids.admit(5), Err(EINVAL));
        assert_eq!(ids.admit(3), Err(EINVAL));
        assert_eq!(ids.admit(6), Ok(()));
        assert_eq!(ids.last(), 6);
    }

    #[test]
    fn a_cancel_ahead_refuses_that_request_when_it_comes() {
        let mut ids = RequestIds::new();
        assert_eq!(ids.admit(1), Ok(()));
        assert_eq!(ids.cancel_ahead(0), Err(EINVAL));
        assert_eq!(ids.cancel_ahead(1), Ok(false));
        assert_eq!(ids.cancel_ahead(4), Ok(true));
        assert_eq!(ids.check(2), Err(ECANCELED));
        assert_eq!(ids.admit(4), Err(ECANCELED));
        assert_eq!(ids.last(), 1);
        assert_eq!(ids.admit(5), Ok(()));
        // An earlier, smaller cancel does not lower the mark.
        assert_eq!(ids.cancel_ahead(3), Ok(false));
        assert_eq!(ids.admit(6), Ok(()));
    }
}
