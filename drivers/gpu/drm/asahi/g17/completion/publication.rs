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

struct Link {
    target: Arc<Publication>,
    result: Result,
    next: Links,
    // Reuse an already-consumed link as a traversal frame instead of recursing
    // or allocating when publication unlocks a long chain of ready successors.
    resume: Links,
}

struct State {
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
                state <- new_mutex!(State {
                    // Acceptance installs all links before releasing this sentinel.
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
        self.pending.load(Ordering::Acquire)
    }

    /// One terminal result, offered separately from its physical-retirement proof.
    pub(crate) fn finish(&self, result: Result<[u64; 4]>) {
        let ready = {
            let mut state = self.state.lock();
            if state.claimed || state.result.is_some() {
                return;
            }
            state.result = Some(result);
            state.claim()
        };
        if let Some(result) = ready {
            Self::drain(self.publish(result));
        }
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
                self.pending.store(false, Ordering::Release);
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

    fn predecessor_done(&self, result: Result) -> Links {
        let (ready, settled) = {
            let mut state = self.state.lock();
            debug_assert!(state.remaining != 0);
            if result.is_err() && state.predecessor_error.is_none() {
                state.predecessor_error = Some(PRODUCER_FAILED);
            }
            state.remaining -= 1;
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
                self.pending.store(false, Ordering::Release);
                (None, state.successors.take())
            } else {
                (None, None)
            }
        };
        if let Some(result) = ready {
            return self.publish(result);
        }
        let mut links = settled;
        let mut next = links.as_mut();
        while let Some(link) = next {
            link.result = Ok(());
            next = link.next.as_mut();
        }
        links
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
        Self::drain(link.target.predecessor_done(immediate));
    }

    /// Iterative depth-first notification, O(nodes + links), with no stack growth.
    fn drain(mut pending: Links) {
        let mut frames: Links = None;
        loop {
            if let Some(mut link) = pending.take() {
                pending = link.next.take();
                let children = link.target.predecessor_done(link.result);
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

/// Temporary backwards references exist only during fallible setup. Installed
/// links retain the successor only, so closing a queue cannot form an Arc cycle.
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
                target: target.clone(),
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
        {
            let mut state = self.target.state.lock();
            debug_assert!(state.remaining == 1 && !state.claimed);
            // The fully allocated link vector bounds this count below usize::MAX.
            state.remaining += self.links.len();
        }
        for (predecessor, link) in self.links {
            predecessor.add_successor(link);
        }
        Publication::drain(self.target.predecessor_done(Ok(())));
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
}
