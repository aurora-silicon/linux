// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded FIFO for context operations that need no sensor lease.

use crate::{key_request::Input, key_request::Request, SepData};
use kernel::alloc::flags::GFP_KERNEL_ACCOUNT;
use kernel::prelude::*;
use kernel::sync::{
    atomic::{Acquire, Relaxed, Release},
    Arc,
};

pub(crate) const MAX_COMMANDS: usize = 32;

pub(crate) struct State {
    requests: KVec<Arc<Request>>,
}

impl State {
    pub(crate) fn new() -> Self {
        Self {
            requests: KVec::new(),
        }
    }

    fn admit(&mut self, request: Arc<Request>) -> Result {
        if self.requests.len() >= MAX_COMMANDS {
            return Err(EBUSY);
        }
        self.requests.push(request, GFP_KERNEL_ACCOUNT)?;
        Ok(())
    }

    fn finish(&mut self, request: &Arc<Request>) {
        self.requests.retain(|active| !Arc::ptr_eq(active, request));
    }

    pub(crate) fn cancel(&self, reason: Error) {
        for request in &self.requests {
            let _ = request.cancel(reason);
        }
    }
}

/// Marks the real frame wait, not merely an accepted authentication request.
/// The exclusive capture owner makes this a single sensor-wide interval.
pub(crate) struct SensorWait<'a>(&'a SepData);
impl<'a> SensorWait<'a> {
    pub(crate) fn new(sep: &'a SepData) -> Self {
        sep.key_sensor_waiting.store(true, Release);
        Self(sep)
    }
}
impl Drop for SensorWait<'_> {
    fn drop(&mut self) {
        self.0.key_sensor_waiting.store(false, Release);
    }
}

impl SepData {
    pub(crate) fn queue_context_command(this: Arc<Self>, request: Arc<Request>) -> Result {
        if !matches!(
            request.data.input,
            Input::ContextSign(_, _) | Input::ContextDecrypt(_, _)
        ) {
            return Err(EINVAL);
        }
        // Remove/PM set their admission flag, then take this same lock to join
        // every enqueue. FIFO dispatch and cleanup share one ordered queue.
        let mut commands = this.key_commands.lock();
        if this.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        if this.suspending.load(Relaxed) {
            return Err(EBUSY);
        }
        commands.admit(request.clone())?;
        let sep = this.clone();
        let work = request.clone();
        if let Err(error) = this
            .key_context_queue
            .try_spawn(GFP_KERNEL_ACCOUNT, move || {
                sep.run_context_command(&work);
            })
        {
            commands.finish(&request);
            return Err(error.into());
        }
        Ok(())
    }

    fn run_context_command(&self, work: &Arc<Request>) {
        let running = !self.shutting_down.load(Relaxed) && work.is_live() && self.capture_begin();
        if running {
            // Identity operations already serialize inside the upstream
            // key-bag helpers; they do not wait on the sensor owner.
            if work.is_live() {
                let result = (|| {
                    work.check_live()?;
                    let (context, decrypt) = match &work.data.input {
                        Input::ContextSign(context, _) => (context, false),
                        Input::ContextDecrypt(context, _) => (context, true),
                        _ => return Err(EINVAL),
                    };
                    let lease = context.acquire()?;
                    let blob = &work.data.blob;
                    let acm = Some(lease.handle());
                    let result = match (&work.data.input, context.bag) {
                        (Input::ContextSign(_, digest), None) => {
                            self.user_key_sign(blob, digest, acm)
                        }
                        (Input::ContextDecrypt(_, sealed), None) => {
                            self.user_key_unseal(blob, sealed, acm)
                        }
                        (Input::ContextSign(_, digest), Some(uid)) => crate::key::bag_user(uid)
                            .and_then(|u| self.user_key_sign_in_bag(u, blob, digest, acm)),
                        (Input::ContextDecrypt(_, sealed), Some(uid)) => crate::key::bag_user(uid)
                            .and_then(|u| self.user_key_unseal_in_bag(u, blob, sealed, acm)),
                        _ => Err(EINVAL),
                    }
                    .map(crate::Secret);
                    drop(lease);
                    if self.key_context_failed.load(Relaxed) {
                        return Err(EIO);
                    }
                    result.map(|value| {
                        if decrypt {
                            crate::key_request::Output::Plaintext(value)
                        } else {
                            crate::key_request::Output::Signature(value)
                        }
                    })
                })();
                if matches!(result, Err(error) if error == EIO) {
                    if let Input::ContextSign(context, _) | Input::ContextDecrypt(context, _) =
                        &work.data.input
                    {
                        context.revoke(EIO);
                    }
                }
                work.complete(result);
            } else {
                work.complete(Err(
                    if crate::shim::monotonic_ns() >= work.data.deadline_ns {
                        ETIMEDOUT
                    } else {
                        ECANCELED
                    },
                ));
            }
            self.capture_end();
        } else {
            let error = if self.shutting_down.load(Relaxed) {
                ENODEV
            } else if crate::shim::monotonic_ns() >= work.data.deadline_ns {
                ETIMEDOUT
            } else {
                ECANCELED
            };
            work.complete(Err(error));
        }
        self.key_commands.lock().finish(work);
        work.retire();
    }

    pub(crate) fn cancel_context_commands(&self, reason: Error) {
        self.key_commands.lock().cancel(reason);
    }

    pub(crate) fn context_sensor_waiting(
        &self,
        context: &Arc<crate::key_context::Context>,
    ) -> bool {
        if !self.key_sensor_waiting.load(Acquire) {
            return false;
        }
        let Some(request) = self.capture.lock().key() else {
            return false;
        };
        request.is_live()
            && matches!(&request.data.input,
                Input::Authenticate(active) if Arc::ptr_eq(active, context))
    }
}

impl SepData {
    /// Starts `request`'s authentication. A request that has ended (cancel,
    /// deadline) keeps the capture until its worker has cleaned up; a new
    /// one waits for that, for at most `WORKER_EXIT_WAIT_MS`, rather than
    /// fail with EBUSY, which the caller cannot tell from a sensor in use.
    pub(crate) fn queue_key(this: Arc<Self>, request: Arc<Request>) -> Result {
        let mut waited: u32 = 0;
        loop {
            match Self::try_queue_key(this.clone(), request.clone()) {
                Err(error)
                    if error == EBUSY
                        && waited < crate::WORKER_EXIT_WAIT_MS
                        && this.key_capture_ending() =>
                {
                    kernel::time::delay::fsleep(kernel::time::Delta::from_millis(10));
                    waited += 10;
                }
                result => return result,
            }
        }
    }

    /// The capture is held by a key request that has already ended.
    fn key_capture_ending(&self) -> bool {
        self.capture
            .lock()
            .key()
            .is_some_and(|active| !active.is_live())
    }

    /// Whether a key touch's capture is still wanted: false once the context
    /// request that owns it has ended. Other key touches end only by fault.
    pub(crate) fn key_capture_wanted(&self) -> bool {
        self.capture
            .lock()
            .key()
            .map_or(true, |active| active.is_live())
    }

    fn try_queue_key(this: Arc<Self>, request: Arc<Request>) -> Result {
        let Input::Authenticate(context) = &request.data.input else {
            return Err(EINVAL);
        };
        let mut capture = this.capture.lock();
        if this.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        if this.suspending.load(Relaxed) {
            return Err(EBUSY);
        }
        if this.key_context_failed.load(Relaxed) {
            return Err(EIO);
        }
        capture.start_key(request.clone())?;
        if let Err(error) = context.authenticate(request.id, &request.completion) {
            capture.finish_key(&request);
            return Err(error);
        }
        let sep = this.clone();
        let work = request.clone();
        if let Err(_error) = this.key_auth_queue.try_spawn(GFP_KERNEL_ACCOUNT, move || {
            sep.run_key_authentication(&work);
        }) {
            capture.finish_key(&request);
            drop(capture);
            context.finish_auth(&request.completion, Err(EIO));
            context.forget(&request.completion);
            request.retire();
            return match request.completion.claim() {
                Err(error) => Err(error),
                Ok(_) => Err(EIO),
            };
        }
        Ok(())
    }

    fn run_key_authentication(&self, request: &Arc<Request>) {
        let Input::Authenticate(context) = &request.data.input else {
            return;
        };
        let result = (|| {
            request.check_live()?;
            context.check_domain()?;
            if !self.capture_begin() {
                return Err(ENODEV);
            }
            let result = self.key_authorize(request.clone());
            self.capture_end();
            result
        })();
        context.finish_auth(
            &request.completion,
            result.map(|credential| (credential, Ok(()))),
        );
        self.capture.lock().finish_key(request);
        request.retire();
    }

    pub(crate) fn cancel_key(&self, reason: Error) {
        if let Some(request) = self.capture.lock().key() {
            let _ = request.cancel(reason);
        }
    }
}
