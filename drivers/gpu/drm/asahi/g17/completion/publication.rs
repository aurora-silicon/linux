// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Host-only ordering of timestamp writes, independent of scheduler credits.
//!
//! Links point from earlier logical ordinals to later ones. They retain
//! only public output state, never a packet, physical render lease or VM job pin.
//! All link storage is allocated before acceptance. Completion neither allocates
//! nor waits for another command; the last prerequisite publishes a ready result.

use super::Output;
use crate::g17::{dependency::PRODUCER_FAILED, status::VmStatus};
use core::sync::atomic::{AtomicBool, Ordering};
use kernel::{new_mutex, prelude::*, sync::{Arc, Mutex}};

type Links = Option<KBox<Link>>;

/// One step of `Publication::pending_within()`, taken under the state lock and finished off it.
enum Probe {
    /// A failed predecessor's link, consumed.
    Consumed((Arc<Publication>, KBox<Link>)),
    /// Settled clean: the dropped predecessor links and the successors to release.
    Settled(KVec<(Arc<Publication>, KBox<Link>)>, Links),
    /// Ask this unsettled timestamp-free predecessor.
    Next(Arc<Publication>),
}

/// How many unsettled timestamp-free predecessors deep `Publication::is_pending()` follows
/// before it answers pending. Commands link the newest pending entry of their prefixes, so an
/// unretired pipeline is a chain of such links; the bound keeps the probe off the stack, and a
/// probe that stops early only leaves the settlement to `finish()`.
const PENDING_PROBE_DEPTH: usize = 4;

struct Link {
    // Populated only when attached to a predecessor's successor list so
    // `deferred_links` held on the target itself never forms a self-Arc cycle.
    target: Option<Arc<Publication>>,
    result: Result,
    next: Links,
    // Reuse an already-consumed link as a traversal frame instead of recursing
    // or allocating when publication unlocks a long chain of ready successors.
    resume: Links,
}

struct State {
    // Pre-allocated predecessor links kept on the successor until `finish()`
    // runs (or `is_pending()` observes all predecessors settled cleanly).
    // Always drained before attaching to any predecessor so no Arc cycle forms.
    deferred_links: KVec<(Arc<Publication>, KBox<Link>)>,
    remaining: usize,
    result: Option<Result<[u64; 4]>>,
    predecessor_error: Option<Error>,
    claimed: bool,
    published: Option<Result>,
    // All predecessors published cleanly while this timestamp-free publication
    // still has no result of its own; successors have already been released.
    upstream_released: bool,
    successors: Links,
}

impl State {
    fn claim(&mut self) -> Option<Result<[u64; 4]>> {
        if self.remaining != 0 || self.claimed {
            return None;
        }
        let result = self.result.take()?;
        self.claimed = true;
        Some(match (result, self.predecessor_error) {
            (Ok(_), Some(error)) => Err(error),
            (result, _) => result,
        })
    }
}

#[pin_data]
pub(crate) struct Publication {
    output: Output,
    status: Arc<VmStatus>,
    has_outputs: bool,
    // True while this publication may still write host timestamps or deliver a
    // deferred predecessor error. Cleared only once no host write or error
    // remains, and never set again.
    pending: AtomicBool,
    // True once this publication published Ok (timestamps written and fence
    // signalled) or settled cleanly without outputs and has not since failed.
    // Lets in-order successors bypass locking the predecessor in `finish()`.
    settled_clean: AtomicBool,
    #[pin]
    state: Mutex<State>,
}

impl Publication {
    pub(super) fn new(
        output: Output,
        status: Arc<VmStatus>,
        has_outputs: bool,
    ) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                output,
                status,
                has_outputs,
                pending: AtomicBool::new(true),
                settled_clean: AtomicBool::new(false),
                state <- new_mutex!(State {
                    deferred_links: KVec::new(),
                    // Acceptance installs deferred links before releasing this sentinel.
                    remaining: 1,
                    result: None,
                    predecessor_error: None,
                    claimed: false,
                    published: None,
                    upstream_released: false,
                    successors: None,
                }, "G17 host timestamp publication"),
            }),
            GFP_KERNEL,
        )
    }

    pub(super) fn output(&self) -> &Output {
        &self.output
    }

    /// Whether this command itself writes host timestamps.
    pub(crate) fn has_outputs(&self) -> bool {
        self.has_outputs
    }

    /// Whether this publication still has an unpublished host timestamp write
    /// or an unresolved or failed predecessor chain.
    pub(crate) fn is_pending(&self) -> bool {
        self.pending_within(PENDING_PROBE_DEPTH)
    }

    fn pending_within(&self, depth: usize) -> bool {
        if !self.pending.load(Ordering::Acquire) {
            return false;
        }
        if self.has_outputs {
            return true;
        }
        loop {
            let step = {
                let mut state = self.state.lock();
                if !self.pending.load(Ordering::Acquire) {
                    return false;
                }
                if state.published.is_some()
                    || state.claimed
                    || state.deferred_links.is_empty()
                {
                    return self.pending.load(Ordering::Acquire);
                }
                let failed = state
                    .deferred_links
                    .iter()
                    .position(|(pred, _)| pred.failed_after_release());
                if let Some(index) = failed {
                    // Terminal and settles nothing: consume the link and keep the failure for
                    // `finish()`, so the probe never retries this predecessor.
                    if state.predecessor_error.is_none() {
                        state.predecessor_error = Some(PRODUCER_FAILED);
                    }
                    Probe::Consumed(state.deferred_links.swap_remove(index))
                } else {
                    let unsettled = state
                        .deferred_links
                        .iter()
                        .find(|(pred, _)| !pred.settled_clean.load(Ordering::Acquire))
                        .map(|(pred, _)| pred.clone());
                    match unsettled {
                        None if state.predecessor_error.is_none() => {
                            let old = core::mem::take(&mut state.deferred_links);
                            state.upstream_released = true;
                            self.settled_clean.store(true, Ordering::Release);
                            self.pending.store(false, Ordering::Release);
                            Probe::Settled(old, state.successors.take())
                        }
                        // Pending until `finish()` publishes the consumed failure.
                        None => return true,
                        Some(pred) if !pred.has_outputs => Probe::Next(pred),
                        Some(_) => return true,
                    }
                }
            };
            match step {
                Probe::Consumed(link) => drop(link),
                Probe::Settled(old, successors) => {
                    drop(old);
                    // Successors that attached before this settlement are released here, as
                    // `predecessors_done_ok()` releases them.
                    Self::drain(Self::released_ok(successors));
                    return false;
                }
                Probe::Next(pred) => {
                    if depth == 0 || pred.pending_within(depth - 1) {
                        return true;
                    }
                }
            }
        }
    }

    /// Released its successors as upstream-clean, and then published a failure.
    fn failed_after_release(&self) -> bool {
        !self.pending.load(Ordering::Acquire) && !self.settled_clean.load(Ordering::Acquire)
    }

    /// Mark successor links released by a clean upstream settlement.
    fn released_ok(mut links: Links) -> Links {
        let mut next = links.as_mut();
        while let Some(link) = next {
            link.result = Ok(());
            next = link.next.as_mut();
        }
        links
    }

    /// One terminal result, offered separately from its physical-retirement proof.
    pub(crate) fn finish(self: &Arc<Self>, result: Result<[u64; 4]>) {
        let (ready, to_attach) = {
            let mut state = self.state.lock();
            if state.claimed || state.result.is_some() {
                return;
            }
            state.result = Some(result);
            let deferred = core::mem::take(&mut state.deferred_links);
            if deferred.is_empty() {
                (state.claim(), deferred)
            } else {
                // Hold a sentinel (+1) while checking/attaching predecessors
                // off-lock so `remaining` cannot reach 0 prematurely.
                state.remaining += 1 + deferred.len();
                (None, deferred)
            }
        };
        if let Some(result) = ready {
            Self::drain(self.publish(result));
            return;
        }
        if !to_attach.is_empty() {
            self.attach_deferred(to_attach);
        }
    }

    fn attach_deferred(self: &Arc<Self>, deferred: KVec<(Arc<Publication>, KBox<Link>)>) {
        let mut fast_ok = 0usize;
        for (predecessor, mut link) in deferred {
            if predecessor.settled_clean.load(Ordering::Acquire) {
                fast_ok += 1;
                drop(link);
            } else {
                link.target = Some(self.clone());
                predecessor.add_successor(link);
            }
        }
        Self::drain(self.predecessors_done_ok(1 + fast_ok));
    }

    fn publish(&self, result: Result<[u64; 4]>) -> Links {
        if let Err(error) = result {
            // Inherited failure is visible to the client without inventing a
            // permanent admission failure in this command's VM.
            if error != ECANCELED {
                self.status.report_failure(error);
            }
        }
        self.output.publish(result, &self.status);
        let result = result.map(|_| ());
        let mut links = {
            let mut state = self.state.lock();
            // A late link is acknowledged only after timestamp writes, the public
            // fence and aggregate membership have all been published off this lock.
            state.published = Some(result);
            if result.is_ok() {
                self.settled_clean.store(true, Ordering::Release);
                self.pending.store(false, Ordering::Release);
            } else {
                self.settled_clean.store(false, Ordering::Release);
            }
            state.successors.take()
        };
        let mut next = links.as_mut();
        while let Some(link) = next {
            link.result = result;
            next = link.next.as_mut();
        }
        links
    }

    fn predecessors_done_ok(&self, count: usize) -> Links {
        let (ready, settled) = {
            let mut state = self.state.lock();
            debug_assert!(state.remaining >= count);
            state.remaining -= count;
            if let Some(ready) = state.claim() {
                (Some(ready), None)
            } else if state.remaining == 0
                && !self.has_outputs
                && state.predecessor_error.is_none()
                && !state.upstream_released
            {
                // Every predecessor published cleanly and this command writes
                // no host timestamps, so downstream commands need not wait on
                // or chain behind its GPU execution.
                state.upstream_released = true;
                self.settled_clean.store(true, Ordering::Release);
                self.pending.store(false, Ordering::Release);
                (None, state.successors.take())
            } else {
                (None, None)
            }
        };
        if let Some(result) = ready {
            return self.publish(result);
        }
        Self::released_ok(settled)
    }

    fn predecessor_done(&self, result: Result) -> Links {
        if result.is_ok() {
            return self.predecessors_done_ok(1);
        }
        let ready = {
            let mut state = self.state.lock();
            debug_assert!(state.remaining != 0);
            if state.predecessor_error.is_none() {
                state.predecessor_error = Some(PRODUCER_FAILED);
            }
            state.remaining -= 1;
            state.claim()
        };
        if let Some(result) = ready {
            return self.publish(result);
        }
        None
    }

    fn add_successor(&self, mut link: KBox<Link>) {
        let immediate = {
            let mut state = self.state.lock();
            if let Some(result) = state.published {
                result
            } else if state.upstream_released {
                Ok(())
            } else {
                link.next = state.successors.take();
                state.successors = Some(link);
                return;
            }
        };
        if let Some(target) = link.target.take() {
            Self::drain(target.predecessor_done(immediate));
        }
    }

    /// Iterative depth-first notification, O(nodes + links), with no stack growth.
    fn drain(mut pending: Links) {
        let mut frames: Links = None;
        loop {
            if let Some(mut link) = pending.take() {
                pending = link.next.take();
                let children = link
                    .target
                    .take()
                    .map_or(None, |target| target.predecessor_done(link.result));
                if children.is_some() {
                    link.resume = pending.take();
                    link.next = frames.take();
                    frames = Some(link);
                    pending = children;
                }
            } else if let Some(mut frame) = frames.take() {
                frames = frame.next.take();
                pending = frame.resume.take();
            } else {
                break;
            }
        }
    }
}

/// Pre-allocated predecessor links held until acceptance. While deferred on the
/// successor, each link keeps `target: None` so no Arc cycle can form; once
/// attached to a predecessor on out-of-order completion, the predecessor owns
/// the link and the successor has already dropped its predecessor references.
pub(crate) struct Prepared {
    target: Arc<Publication>,
    links: KVec<(Arc<Publication>, KBox<Link>)>,
}

impl Prepared {
    pub(crate) fn new<'a>(
        target: Arc<Publication>,
        predecessors: impl Iterator<Item = &'a Arc<Publication>> + Clone,
    ) -> Result<Self> {
        let mut links = KVec::with_capacity(predecessors.clone().count(), GFP_KERNEL)?;
        for predecessor in predecessors {
            let link = KBox::new(Link {
                target: None,
                result: Ok(()),
                next: None,
                resume: None,
            }, GFP_KERNEL)?;
            links.push((predecessor.clone(), link), GFP_KERNEL)?;
        }
        Ok(Self { target, links })
    }

    /// Called after all fallible scheduler setup and before any GPU publication.
    pub(crate) fn activate(self) {
        let attach_now = {
            let mut state = self.target.state.lock();
            debug_assert!(state.remaining == 1 && !state.claimed);
            state.remaining -= 1;
            if state.result.is_some() {
                if self.links.is_empty() {
                    let ready = state.claim();
                    drop(state);
                    if let Some(result) = ready {
                        Publication::drain(self.target.publish(result));
                    }
                    return;
                }
                state.remaining += 1 + self.links.len();
                Some(self.links)
            } else {
                if self.links.is_empty() && !self.target.has_outputs {
                    state.upstream_released = true;
                    self.target.settled_clean.store(true, Ordering::Release);
                    self.target.pending.store(false, Ordering::Release);
                } else {
                    state.deferred_links = self.links;
                }
                None
            }
        };
        if let Some(links) = attach_now {
            self.target.attach_deferred(links);
        }
    }
}

#[cfg(CONFIG_KUNIT = "y")]
impl Publication {
    pub(crate) fn test_pair(
        contexts: &kernel::dma_fence::FenceContexts,
        status: &Arc<VmStatus>,
        has_outputs: bool,
    ) -> Result<(Arc<Self>, kernel::dma_fence::Fence)> {
        let fence = crate::g17::fence::independent(contexts)?;
        let handle = kernel::dma_fence::Fence::from_fence(&fence);
        let output = Output {
            fence,
            member: crate::g17::fence::Aggregate::test_member(contexts)?,
            destinations: super::Destinations::empty(),
        };
        Ok((Self::new(output, status.clone(), has_outputs)?, handle))
    }
}

#[kunit_tests(asahi_g17_publication_order)]
mod tests {
    use super::*;
    use crate::g17::{
        completion::Destinations,
        fence::{independent, Aggregate},
        job::fence_status,
    };
    use kernel::{c_str, dma_fence::{Fence, FenceContexts}, sync::LockClassKey};

    static KEY: Pin<&LockClassKey> = kernel::static_lock_class!();

    fn make_pub(
        contexts: &FenceContexts,
        status: &Arc<VmStatus>,
        has_outputs: bool,
    ) -> (Arc<Publication>, Fence) {
        let fence = independent(contexts).unwrap();
        let handle = Fence::from_fence(&fence);
        let output = Output {
            fence,
            member: Aggregate::test_member(contexts).unwrap(),
            destinations: Destinations::empty(),
        };
        (Publication::new(output, status.clone(), has_outputs).unwrap(), handle)
    }

    #[test]
    fn consumer_retiring_before_delayed_producer_failure_inherits_error() {
        let contexts = FenceContexts::new(1, c_str!("asahi_pub_test"), KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        let (producer, prod_fence) = make_pub(&contexts, &status, true);
        let (consumer, cons_fence) = make_pub(&contexts, &status, false);
        let (downstream, down_fence) = make_pub(&contexts, &status, false);

        Prepared::new(producer.clone(), core::iter::empty()).unwrap().activate();
        Prepared::new(consumer.clone(), core::iter::once(&producer)).unwrap().activate();
        Prepared::new(downstream.clone(), core::iter::once(&consumer)).unwrap().activate();

        // Consumer and its transitive successor retire before the producer's
        // deferred failure runs: neither may signal success early.
        consumer.finish(Ok([1, 2, 0, 0]));
        downstream.finish(Ok([3, 4, 0, 0]));
        assert_eq!(fence_status(&prod_fence), 0);
        assert_eq!(fence_status(&cons_fence), 0);
        assert_eq!(fence_status(&down_fence), 0);

        producer.finish(Err(EIO));
        assert_eq!(fence_status(&prod_fence), EIO.to_errno());
        assert_eq!(fence_status(&cons_fence), PRODUCER_FAILED.to_errno());
        assert_eq!(fence_status(&down_fence), PRODUCER_FAILED.to_errno());
        assert!(producer.is_pending());
        assert!(consumer.is_pending());
        assert!(downstream.is_pending());
    }

    #[test]
    fn clean_producer_settles_timestamp_free_chain_while_in_flight() {
        let contexts = FenceContexts::new(1, c_str!("asahi_pub_settle"), KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        let (producer, prod_fence) = make_pub(&contexts, &status, true);
        let (consumer, cons_fence) = make_pub(&contexts, &status, false);
        let (downstream, down_fence) = make_pub(&contexts, &status, false);

        Prepared::new(producer.clone(), core::iter::empty()).unwrap().activate();
        Prepared::new(consumer.clone(), core::iter::once(&producer)).unwrap().activate();
        Prepared::new(downstream.clone(), core::iter::once(&consumer)).unwrap().activate();

        assert!(producer.is_pending());
        assert!(consumer.is_pending());
        assert!(downstream.is_pending());

        // Producer completes cleanly while consumer and downstream are still
        // executing on the GPU: both timestamp-free publications settle at once
        // without signalling their own completion fences early.
        producer.finish(Ok([1, 2, 0, 0]));
        assert!(fence_status(&prod_fence) > 0);
        assert!(!producer.is_pending());
        assert!(!consumer.is_pending());
        assert!(!downstream.is_pending());
        assert_eq!(fence_status(&cons_fence), 0);
        assert_eq!(fence_status(&down_fence), 0);

        // Downstream finishing before consumer still publishes immediately.
        downstream.finish(Ok([5, 6, 0, 0]));
        assert!(fence_status(&down_fence) > 0);
        assert_eq!(fence_status(&cons_fence), 0);

        consumer.finish(Ok([3, 4, 0, 0]));
        assert!(fence_status(&cons_fence) > 0);
    }

    #[test]
    fn settled_publication_failing_later_delivers_error_to_late_successor() {
        let contexts = FenceContexts::new(1, c_str!("asahi_pub_settle_err"), KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        let (producer, _) = make_pub(&contexts, &status, true);
        let (consumer, cons_fence) = make_pub(&contexts, &status, false);
        let (early, early_fence) = make_pub(&contexts, &status, false);

        Prepared::new(producer.clone(), core::iter::empty()).unwrap().activate();
        Prepared::new(consumer.clone(), core::iter::once(&producer)).unwrap().activate();
        Prepared::new(early.clone(), core::iter::once(&consumer)).unwrap().activate();

        producer.finish(Ok([1, 2, 0, 0]));
        assert!(!consumer.is_pending());
        assert!(!early.is_pending());

        // Consumer's own GPU execution later fails: early successor was already
        // released at settlement, while a late successor added after publish
        // observes consumer's error.
        consumer.finish(Err(EIO));
        assert_eq!(fence_status(&cons_fence), EIO.to_errno());

        early.finish(Ok([3, 4, 0, 0]));
        assert!(fence_status(&early_fence) > 0);

        let (late, late_fence) = make_pub(&contexts, &status, false);
        Prepared::new(late.clone(), core::iter::once(&consumer)).unwrap().activate();
        late.finish(Ok([5, 6, 0, 0]));
        assert_eq!(fence_status(&late_fence), PRODUCER_FAILED.to_errno());
    }

    #[test]
    fn in_order_writer_finish_drains_zero_deferred_links() {
        let contexts = FenceContexts::new(1, c_str!("asahi_pub_lazy"), KEY).unwrap();
        let status = Arc::pin_init(VmStatus::new(), GFP_KERNEL).unwrap();
        let (producer, prod_fence) = make_pub(&contexts, &status, true);
        Prepared::new(producer.clone(), core::iter::empty()).unwrap().activate();

        let mut consumers = KVec::with_capacity(16, GFP_KERNEL).unwrap();
        for _ in 0..16 {
            let (cons, fence) = make_pub(&contexts, &status, false);
            Prepared::new(cons.clone(), core::iter::once(&producer))
                .unwrap()
                .activate();
            consumers.push((cons, fence), GFP_KERNEL).unwrap();
        }
        // Deferred links stay on each successor; producer has zero eager successors.
        assert!(producer.state.lock().successors.is_none());

        producer.finish(Ok([1, 2, 0, 0]));
        assert!(fence_status(&prod_fence) > 0);
        assert!(producer.settled_clean.load(Ordering::Acquire));

        for (cons, fence) in consumers {
            cons.finish(Ok([3, 4, 0, 0]));
            assert!(fence_status(&fence) > 0);
        }
    }
}
