// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Kernel-owned authorization, hardware leases and asynchronous disposal.

use crate::{
    key_context_state::{Cleanup, Phase, State},
    key_request as keys,
    key_request::Completion,
    key_wait::Event,
    scrd, SepData,
};
use kernel::alloc::flags::GFP_KERNEL_ACCOUNT;
use kernel::prelude::*;
use kernel::sync::{atomic::Relaxed, Arc, Mutex};
use kernel::workqueue::{impl_has_work, new_work, Work, WorkItem};

pub(crate) const MAX_CONTEXTS: usize = 16;
pub(crate) const MAX_REQUESTS: usize = 32;

/// Owned provisional credential while the capture worker finishes retiring.
/// If publication loses to cancellation, this owner still disposes it once.
pub(crate) struct Credential {
    sep: Arc<SepData>,
    handle: [u8; scrd::SCRD_ACM_HANDLE_LEN],
    transferred: bool,
}

impl Credential {
    pub(crate) fn new(sep: Arc<SepData>, handle: [u8; scrd::SCRD_ACM_HANDLE_LEN]) -> Self {
        Self {
            sep,
            handle,
            transferred: false,
        }
    }

    fn into_handle(mut self) -> [u8; scrd::SCRD_ACM_HANDLE_LEN] {
        self.transferred = true;
        let handle = self.handle;
        crate::image::wipe(&mut self.handle);
        handle
    }
}

impl Drop for Credential {
    fn drop(&mut self) {
        if !self.transferred {
            let _ = self.sep.delete_unlock_evaluation(&self.handle);
        }
        crate::image::wipe(&mut self.handle);
    }
}

/// `Context::bind` operations: what a bound context may run on its key.
pub(crate) const SCOPE_SIGN: u32 = 1;
pub(crate) const SCOPE_DECRYPT: u32 = 2;

/// The one key a bound context may use, and how. A vault session bound to
/// its item key for decryption cannot then decrypt a passkey's secret or sign
/// with another key, however long it lives.
struct Scope {
    record: KVec<u8>,
    ops: u32,
}

impl Scope {
    fn new(record: KVec<u8>, ops: u32) -> Result<Self> {
        if ops == 0 || ops & !(SCOPE_SIGN | SCOPE_DECRYPT) != 0 {
            return Err(EINVAL);
        }
        Ok(Self { record, ops })
    }

    /// Whether `op` on the key `record` is within this scope.
    fn allows(&self, record: &[u8], op: u32) -> bool {
        self.record.as_slice() == record && self.ops & op != 0
    }
}

struct Grant([u8; scrd::SCRD_ACM_HANDLE_LEN]);
impl Drop for Grant {
    fn drop(&mut self) {
        crate::image::wipe(&mut self.0);
    }
}

struct Inner {
    life: State,
    grant: Option<Grant>,
    deliveries: KVec<Arc<Completion>>,
    ids: keys::RequestIds,
    closing: bool,
    /// When the matched grant stops authorizing (monotonic ns); 0 never.
    expires_ns: u64,
    scope: Option<Scope>,
}

#[pin_data]
pub(crate) struct Context {
    pub(crate) sep: Arc<SepData>,
    generation: u64,
    /// The user whose bag this context authorizes keys under: its match runs
    /// in that user's domain, and the bag's lock revokes it. None for the
    /// identity bag's biometric keys.
    pub(crate) bag: Option<u32>,
    /// How long one match authorizes, from the match; 0 until revoked.
    lifetime_ns: u64,
    #[pin]
    inner: Mutex<Inner>,
    changed: Event,
    #[pin]
    work: Work<Context>,
}

impl_has_work! { impl HasWork<Self> for Context { self.work } }

/// Only a running hardware use retains this lease; user copying never does.
pub(crate) struct Lease<'a> {
    context: &'a Arc<Context>,
    grant: Grant,
}
impl Lease<'_> {
    pub(crate) fn handle(&self) -> &[u8; scrd::SCRD_ACM_HANDLE_LEN] {
        &self.grant.0
    }
}
impl Drop for Lease<'_> {
    fn drop(&mut self) {
        self.context.inner.lock().life.release();
        self.context.changed.wake();
        self.context.schedule();
    }
}

impl Context {
    pub(crate) fn new(
        sep: Arc<SepData>,
        expected_generation: u64,
        bag: Option<u32>,
        lifetime_s: u32,
    ) -> Result<Arc<Self>> {
        let mut contexts = sep.key_contexts.lock();
        if sep.shutting_down.load(Relaxed) || sep.suspending.load(Relaxed) {
            return Err(ENODEV);
        }
        if expected_generation != sep.key_context_generation.load(Relaxed) {
            return Err(ENODEV);
        }
        if contexts.len() >= MAX_CONTEXTS {
            return Err(ENOSPC);
        }
        let owner = sep.clone();
        let generation = sep.key_context_generation.load(Relaxed);
        let context = Arc::pin_init(
            try_pin_init!(Self {
                sep: owner,
                generation,
                bag,
                lifetime_ns: u64::from(lifetime_s) * 1_000_000_000,
                inner <- kernel::new_mutex!(Inner {
                    life: State::new(),
                    grant: None,
                    deliveries: KVec::new(),
                    ids: keys::RequestIds::new(),
                    closing: false,
                    expires_ns: 0,
                    scope: None,
                }),
                changed: Event::new()?,
                work <- new_work!("sep_context_cleanup"),
            }),
            GFP_KERNEL_ACCOUNT,
        )?;
        contexts.push(context.clone(), GFP_KERNEL_ACCOUNT)?;
        Ok(context)
    }

    pub(crate) fn check_domain(&self) -> Result {
        if self.sep.shutting_down.load(Relaxed)
            || self.sep.suspending.load(Relaxed)
            || self.generation != self.sep.key_context_generation.load(Relaxed)
        {
            return Err(ENODEV);
        }
        Ok(())
    }

    fn schedule(self: &Arc<Self>) {
        // Shutdown takes this same registry lock before draining the queue.
        let _contexts = self.sep.key_contexts.lock();
        if self.sep.shutting_down.load(Relaxed) {
            return;
        }
        {
            let inner = self.inner.lock();
            if inner.life.phase != Phase::Revoking && !inner.closing {
                return;
            }
        }
        let _ = self.sep.key_context_queue.enqueue(self.clone());
    }

    /// Limits the context to one key record and the `SCOPE_*` operations in
    /// `ops`. Only once, and only before it is authenticated: a scope cannot
    /// widen what an existing authorization covers.
    pub(crate) fn bind(&self, record: KVec<u8>, ops: u32) -> Result {
        let scope = Scope::new(record, ops)?;
        let mut inner = self.inner.lock();
        if inner.life.phase != Phase::New || inner.scope.is_some() {
            return Err(EBUSY);
        }
        inner.scope = Some(scope);
        Ok(())
    }

    /// Refuses a key or an operation outside a bound context's scope.
    pub(crate) fn in_scope(&self, record: &[u8], op: u32) -> Result {
        match &self.inner.lock().scope {
            Some(scope) if !scope.allows(record, op) => Err(EPERM),
            _ => Ok(()),
        }
    }

    pub(crate) fn authenticate(&self, id: u64, completion: &Arc<Completion>) -> Result {
        let mut inner = self.inner.lock();
        inner.ids.check(id)?;
        inner
            .deliveries
            .push(completion.clone(), GFP_KERNEL_ACCOUNT)?;
        if let Err(error) = inner.life.authenticate() {
            inner.deliveries.pop();
            return Err(error);
        }
        inner.ids.admit(id)
    }

    pub(crate) fn finish_auth(
        self: &Arc<Self>,
        completion: &Completion,
        result: Result<(Credential, Result)>,
    ) {
        let mut inner = self.inner.lock();
        let status = match result {
            Ok((credential, status)) => {
                inner.grant = Some(Grant(credential.into_handle()));
                if self.lifetime_ns != 0 {
                    inner.expires_ns = crate::shim::monotonic_ns().saturating_add(self.lifetime_ns);
                }
                if self.sep.key_context_failed.load(Relaxed) {
                    Err(EIO)
                } else if completion.is_live() {
                    status
                } else {
                    Err(ECANCELED)
                }
            }
            Err(error) => Err(error),
        };
        if inner.life.authenticated(status) {
            if !completion.complete(Ok(keys::Output::Authenticated)) {
                inner.life.revoke(ECANCELED);
            }
        } else {
            completion.complete(Err(status.err().unwrap_or(EKEYREVOKED)));
        }
        drop(inner);
        self.changed.wake();
        self.schedule();
    }

    pub(crate) fn register(&self, completion: &Arc<Completion>) -> Result {
        let mut inner = self.inner.lock();
        if !inner.life.claim_allowed() {
            return Err(EKEYREVOKED);
        }
        if inner.deliveries.len() >= MAX_REQUESTS {
            return Err(EBUSY);
        }
        inner
            .deliveries
            .push(completion.clone(), GFP_KERNEL_ACCOUNT)?;
        Ok(())
    }

    pub(crate) fn forget(&self, completion: &Arc<Completion>) {
        self.inner
            .lock()
            .deliveries
            .retain(|active| !Arc::ptr_eq(active, completion));
    }

    pub(crate) fn acquire(self: &Arc<Self>) -> Result<Lease<'_>> {
        self.check_domain()?;
        if self.sep.key_context_failed.load(Relaxed) {
            return Err(EIO);
        }
        let mut inner = self.inner.lock();
        if inner.expires_ns != 0 && crate::shim::monotonic_ns() >= inner.expires_ns {
            drop(inner);
            self.revoke(EKEYEXPIRED);
            return Err(EKEYEXPIRED);
        }
        let grant = Grant(inner.grant.as_ref().ok_or(EKEYREVOKED)?.0);
        inner.life.acquire()?;
        Ok(Lease {
            context: self,
            grant,
        })
    }

    pub(crate) fn claim(&self, completion: &Arc<Completion>) -> Result<keys::Output> {
        let mut inner = self.inner.lock();
        // Lock order is context then delivery. Nothing holds either at copyout.
        if self.check_domain().is_err() {
            let _ = completion.cancel(ENODEV);
        }
        if !inner.life.claim_allowed() {
            let _ = completion.cancel(EKEYREVOKED);
        }
        if self.sep.key_context_failed.load(Relaxed) {
            let _ = completion.cancel(EIO);
        }
        let result = completion.claim();
        inner
            .deliveries
            .retain(|active| !Arc::ptr_eq(active, completion));
        result
    }

    pub(crate) fn mark_revoke(&self, reason: Error) {
        let mut inner = self.inner.lock();
        inner.life.revoke(reason);
        for delivery in &inner.deliveries {
            let _ = delivery.cancel(reason);
        }
        self.changed.wake();
    }

    pub(crate) fn revoke(self: &Arc<Self>, reason: Error) {
        self.mark_revoke(reason);
        self.schedule();
    }

    pub(crate) fn wait_revoke(&self, deadline_ns: u64) -> Result {
        loop {
            let sequence = self.changed.sequence();
            if let Some(result) = self.inner.lock().life.revocation_result() {
                return result;
            }
            if crate::shim::monotonic_ns() >= deadline_ns {
                return Err(ETIMEDOUT);
            }
            self.changed.wait(sequence, deadline_ns)?;
        }
    }

    pub(crate) fn cancel(self: &Arc<Self>, id: u64) -> Result {
        let mut inner = self.inner.lock();
        if inner.ids.cancel_ahead(id)? {
            return Ok(());
        }
        if id != inner.ids.last() {
            return Err(ENOENT);
        }
        if inner.life.phase == Phase::Authorized {
            return Err(EALREADY);
        }
        if inner.life.phase != Phase::Authenticating {
            return Err(ENOENT);
        }
        // Publish authorization and cancel under the same context -> delivery
        // lock order. Cancel cannot report success after a grant was published.
        inner.deliveries.first().ok_or(ENOENT)?.cancel(ECANCELED)?;
        inner.life.revoke(ECANCELED);
        for delivery in &inner.deliveries {
            let _ = delivery.cancel(ECANCELED);
        }
        drop(inner);
        self.changed.wake();
        self.schedule();
        Ok(())
    }

    pub(crate) fn query(&self) -> (u32, u32, i32) {
        let inner = self.inner.lock();
        (
            inner.life.phase as u32,
            inner.life.cleanup as u32,
            inner.life.reason.map_or(0, Error::to_errno),
        )
    }

    pub(crate) fn close(self: &Arc<Self>) {
        self.inner.lock().closing = true;
        self.revoke(EKEYREVOKED);
    }

    fn cleanup(&self) {
        let grant = {
            let mut inner = self.inner.lock();
            if !inner.life.claim_cleanup() {
                return;
            }
            inner.grant.take()
        };
        let result = if let Some(grant) = grant {
            // Cleanup shares the ordered command lane; SCRD serializes packets.
            self.sep.delete_unlock_evaluation(&grant.0)
        } else if self.sep.key_context_failed.load(Relaxed) {
            Err(EIO)
        } else {
            Ok(())
        };
        self.inner.lock().life.disposed(result);
        self.changed.wake();
    }
}

impl WorkItem for Context {
    type Pointer = Arc<Self>;
    fn run(this: Arc<Self>) {
        this.cleanup();
        // Lock order: registry then context. Remove only after disposal; NEW
        // and closing contexts both count against the per-provider bound.
        let mut contexts = this.sep.key_contexts.lock();
        let inner = this.inner.lock();
        if inner.closing && inner.life.phase == Phase::Dead && inner.life.cleanup != Cleanup::Failed
        {
            contexts.retain(|context| !Arc::ptr_eq(context, &this));
        }
    }
}

impl SepData {
    fn context_snapshot(&self) -> [Option<Arc<Context>>; MAX_CONTEXTS] {
        let contexts = self.key_contexts.lock();
        let mut snapshot = core::array::from_fn(|_| None);
        for (slot, context) in snapshot.iter_mut().zip(contexts.iter()) {
            *slot = Some(context.clone());
        }
        snapshot
    }

    /// The bag of `uid` locked: its contexts stop authorizing at once.
    pub(crate) fn revoke_bag_contexts(&self, uid: u32) {
        for context in self.context_snapshot().into_iter().flatten() {
            if context.bag == Some(uid) {
                context.revoke(EKEYREVOKED);
            }
        }
    }

    pub(crate) fn revoke_key_contexts(&self, reason: Error) {
        self.key_context_generation.fetch_add(1, Relaxed);
        for context in self.context_snapshot().into_iter().flatten() {
            context.revoke(reason);
        }
    }

    pub(crate) fn drain_key_contexts(&self, deadline_ns: u64) -> Result {
        for context in self.context_snapshot().into_iter().flatten() {
            // Disposal failure poisons a physically idle provider. It does not
            // justify preventing sleep once every command/user has drained.
            match context.wait_revoke(deadline_ns) {
                Err(error) if error == EIO => {}
                result => result?,
            }
        }
        Ok(())
    }

    pub(crate) fn remove_key_contexts(&self) {
        // Shutdown has stopped every producer and the key worker was flushed.
        // Queue the final disposals before draining this dedicated queue.
        for context in self.context_snapshot().into_iter().flatten() {
            context.mark_revoke(ENODEV);
            let _ = self.key_context_queue.enqueue(context);
        }
        self.key_context_queue.drain();
        self.key_contexts.lock().clear();
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_key_context_scope)]
mod tests {
    use super::*;

    fn record(bytes: &[u8]) -> KVec<u8> {
        let mut out = KVec::new();
        out.extend_from_slice(bytes, GFP_KERNEL).unwrap();
        out
    }

    #[test]
    fn a_scope_names_known_operations() {
        assert!(Scope::new(record(b"key"), 0).is_err());
        assert!(Scope::new(record(b"key"), 4).is_err());
        assert!(Scope::new(record(b"key"), SCOPE_SIGN | 4).is_err());
        assert!(Scope::new(record(b"key"), SCOPE_SIGN | SCOPE_DECRYPT).is_ok());
    }

    #[test]
    fn a_scope_allows_only_its_key_and_operations() {
        let scope = Scope::new(record(b"vault key"), SCOPE_DECRYPT).unwrap();
        assert!(scope.allows(b"vault key", SCOPE_DECRYPT));
        assert!(!scope.allows(b"vault key", SCOPE_SIGN));
        assert!(!scope.allows(b"passkey key", SCOPE_DECRYPT));
        assert!(!scope.allows(b"vault ke", SCOPE_DECRYPT));
        let both = Scope::new(record(b"k"), SCOPE_SIGN | SCOPE_DECRYPT).unwrap();
        assert!(both.allows(b"k", SCOPE_SIGN) && both.allows(b"k", SCOPE_DECRYPT));
    }
}

