#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for sync-input chain deduplication and lock-free M5 cross-QID compute publication.

Tests both the real C `collect_leaf()` extracted from
`drivers/gpu/drm/asahi/g17/sync_input.c` and the real Rust `Hold`,
`ReaderHint`, and `resolve_direct_sources()` logic extracted from
`sync_input.rs` and `runtime.rs`:
1. Timeline chain deduplication (C): 8 unsignaled same-context leaves
   collapse to the single latest seqno with zero refcount leaks; distinct
   contexts stay distinct; failed earlier leaves (`status < 0`) are never
   coalesced or dropped.
2. Lock-free `compute_direct()` cross-QID resolution (Rust):
   - Resolves `PublishedSource` into a `Frontier` `KickDependency`, takes
     `Hold::new(&readers)` which is immediately observed by
     `ReaderHint::has_readers()` and blocks `reset_for_owner()` without
     locking the producer queue, and marks `note_firmware_consumer()`.
   - Safely falls back (`None`, zero leaked `Hold`s) on unpublished source,
     epoch/generation mismatch, active recovery, cancelled/quarantined
     source, and failed source fence; skips already-retired sources.
3. Structural verification of `runtime/backend.rs`, `runtime.rs`,
   `queue/compute.rs`, `fence.rs`, `completion.rs`, and `job.rs`.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        idx = source.find(part, at + 1)
        if idx < 0:
            return False
        at = idx
    return True


def build_c_harness(mutation=None):
    c_src = (G17 / 'sync_input.c').read_text()
    collect_leaf = item(c_src, 'static int collect_leaf(')
    if mutation == 'no-chain-dedup':
        collect_leaf = collect_leaf.replace(
            'if (coalesce_contexts && status == 0 && leaf->context != 0 &&',
            'if (coalesce_contexts && 0 &&',
            1,
        )
    elif mutation == 'coalesce-failed-leaf':
        collect_leaf = collect_leaf.replace(
            'status == 0 && leaf->context != 0 &&\n\t\t    out[i].status == 0 && ',
            'leaf->context != 0 &&\n\t\t    ',
            1,
        )

    return f'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t u64;

struct dma_fence {{
	uint64_t context;
	uint64_t seqno;
	int status;
	int refs;
}};

struct asahi_g17_fence_leaf {{
	struct dma_fence *fence;
	int status;
}};

static struct dma_fence *dma_fence_get(struct dma_fence *f)
{{
	f->refs++;
	return f;
}}

static void dma_fence_put(struct dma_fence *f)
{{
	assert(f->refs > 0);
	f->refs--;
}}

static int asahi_g17_fence_status(struct dma_fence *f)
{{
	return f->status;
}}

static bool dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{{
	assert(f1->context == f2->context);
	return (int64_t)(f1->seqno - f2->seqno) > 0;
}}

{collect_leaf}

static void release_all(struct asahi_g17_fence_leaf *out, size_t count)
{{
	for (size_t i = 0; i < count; ++i)
		dma_fence_put(out[i].fence);
}}

int main(void)
{{
	/* 1. 8 unsignaled same-context timeline leaves (out of order) -> 1 newest leaf. */
	struct dma_fence chain[8];
	uint64_t order[8] = {{3, 7, 1, 8, 5, 2, 6, 4}};
	for (int i = 0; i < 8; ++i) {{
		chain[i].context = 10;
		chain[i].seqno = order[i];
		chain[i].status = 0;
		chain[i].refs = 1;
	}}
	struct asahi_g17_fence_leaf out[16];
	size_t count = 0;
	for (int i = 0; i < 8; ++i) {{
		assert(collect_leaf(&chain[i], out, 16, &count, true) == 0);
	}}
	assert(count == 1);
	assert(out[0].fence->context == 10);
	assert(out[0].fence->seqno == 8);
	assert(out[0].status == 0);
	release_all(out, count);
	for (int i = 0; i < 8; ++i)
		assert(chain[i].refs == 1);

	/* 2. Failed earlier leaf + healthy later unsignaled leaf on same context -> BOTH kept. */
	struct dma_fence failed = {{ .context = 10, .seqno = 2, .status = -EIO, .refs = 1 }};
	struct dma_fence healthy = {{ .context = 10, .seqno = 9, .status = 0, .refs = 1 }};
	count = 0;
	assert(collect_leaf(&failed, out, 16, &count, true) == 0);
	assert(collect_leaf(&healthy, out, 16, &count, true) == 0);
	assert(count == 2);
	assert(out[0].status == -EIO && out[0].fence->seqno == 2);
	assert(out[1].status == 0 && out[1].fence->seqno == 9);
	release_all(out, count);
	assert(failed.refs == 1 && healthy.refs == 1);

	/* 3. Two distinct contexts coalesce independently to 1 newest leaf each. */
	struct dma_fence q0[3] = {{
		{{ .context = 11, .seqno = 1, .status = 0, .refs = 1 }},
		{{ .context = 11, .seqno = 4, .status = 0, .refs = 1 }},
		{{ .context = 11, .seqno = 2, .status = 0, .refs = 1 }},
	}};
	struct dma_fence q1[2] = {{
		{{ .context = 22, .seqno = 10, .status = 0, .refs = 1 }},
		{{ .context = 22, .seqno = 12, .status = 0, .refs = 1 }},
	}};
	count = 0;
	for (int i = 0; i < 3; ++i)
		assert(collect_leaf(&q0[i], out, 16, &count, true) == 0);
	for (int i = 0; i < 2; ++i)
		assert(collect_leaf(&q1[i], out, 16, &count, true) == 0);
	assert(count == 2);
	assert(out[0].fence->context == 11 && out[0].fence->seqno == 4);
	assert(out[1].fence->context == 22 && out[1].fence->seqno == 12);
	release_all(out, count);

	/* 4. Context 0 leaves are never coalesced. */
	struct dma_fence z0 = {{ .context = 0, .seqno = 1, .status = 0, .refs = 1 }};
	struct dma_fence z1 = {{ .context = 0, .seqno = 2, .status = 0, .refs = 1 }};
	count = 0;
	assert(collect_leaf(&z0, out, 16, &count, true) == 0);
	assert(collect_leaf(&z1, out, 16, &count, true) == 0);
	assert(count == 2);
	release_all(out, count);

	/* 5. Distinct-context ExportFences are all preserved so Backend::input_sources() retains
	 *    earlier error dependencies separately from firmware wait barriers. */
	struct dma_fence exp[4] = {{
		{{ .context = 101, .seqno = 0, .status = 0, .refs = 1 }},
		{{ .context = 102, .seqno = 0, .status = 0, .refs = 1 }},
		{{ .context = 103, .seqno = 0, .status = 0, .refs = 1 }},
		{{ .context = 104, .seqno = 0, .status = 0, .refs = 1 }},
	}};
	count = 0;
	for (int i = 0; i < 4; ++i)
		assert(collect_leaf(&exp[i], out, 16, &count, true) == 0);
	assert(count == 4);
	release_all(out, count);
	for (int i = 0; i < 4; ++i)
		assert(exp[i].refs == 1);
	return 0;
}}
'''


def build_rust_harness(mutation=None):
    sync_c = (G17 / 'sync_input.c').read_text()
    sync_rs = (G17 / 'sync_input.rs').read_text()
    backend_rs = (G17 / 'runtime/backend.rs').read_text()
    runtime_rs = (G17 / 'runtime.rs').read_text()
    compute_rs = (G17 / 'queue/compute.rs').read_text()
    completion_rs = (G17 / 'completion.rs').read_text()
    fence_rs = (G17 / 'fence.rs').read_text()
    job_rs = (G17 / 'job.rs').read_text()

    hold_struct = 'pub struct Hold(Arc<AtomicU32>);'
    hold_impl = item(sync_rs, 'impl Hold').replace('pub(super) ', 'pub ')
    hold_drop = item(sync_rs, 'impl Drop for Hold')
    hint_struct = item(sync_rs, 'pub(super) struct ReaderHint').replace('pub(super) ', 'pub ')
    hint_impl = item(sync_rs, 'impl ReaderHint').replace('pub(super) ', 'pub ')
    coalesce_enum = item(sync_rs, 'pub(super) enum Coalesce').replace('pub(super) ', 'pub ')
    coalesce_fn = item(sync_rs, 'pub(super) fn coalesce_source(').replace('pub(super) ', 'pub ')
    sources_impl = item(sync_rs, 'impl Sources')
    sources_check_fn = item(sources_impl, 'pub(super) fn check(').replace('pub(super) ', 'pub ')
    resolve_fn = item(runtime_rs, 'fn resolve_direct_sources(')
    input_sources_fn = item(backend_rs, 'fn input_sources(')
    compute_direct_fn = item(runtime_rs, 'fn compute_direct(')
    sources_pub_fn = item(runtime_rs, 'fn sources_published(')
    compute_host_impl = item(runtime_rs, 'impl compute::Host for ComputeHost<')
    begin_replay_fn = item(completion_rs, 'pub(crate) fn begin_replay(')
    complete_fn = item(completion_rs, 'fn complete_with_compute(')
    deferred_finish_fn = item(runtime_rs, 'pub(super) fn finish(&mut self)')
    submission_impl = item(fence_rs, 'impl Submission')
    note_error_fn = item(submission_impl, 'fn note_error(')
    same_vm_fn = item(submission_impl, 'pub(super) fn same_vm(')
    has_pub_epoch_fn = item(submission_impl, 'pub(super) fn has_published_epoch(')
    clear_pub_fn = item(submission_impl, 'pub(super) fn clear_published(')
    cancel_fn = item(job_rs, 'pub(crate) fn request_cancel(')

    if mutation == 'hint-blind-spot':
        hint_impl = hint_impl.replace(
            'count.load(Ordering::Acquire) != 0',
            'self.possible && count.load(Ordering::Acquire) != 0',
            1,
        )
    elif mutation == 'skip-epoch-check':
        resolve_fn = resolve_fn.replace(
            'source.submission.published_source(generation)?',
            'source.submission.published_source_unchecked()?',
            1,
        )
        resolve_fn = resolve_fn.replace(
            'if !source.submission.has_published_epoch(generation) {\n                return None;\n            }',
            '',
            2,
        )
    elif mutation == 'skip-recovery-recheck':
        resolve_fn = resolve_fn.replace(
            'if port.recovery_state().ok()? != 0 || port.generation() != generation {\n                return None;\n            }',
            '',
            2,
        )
    elif mutation == 'lock-per-leaf':
        input_sources_fn = input_sources_fn.replace(
            'let published_fast =',
            'let _ = self.shared.state.lock(); let published_fast =',
            1,
        )
    elif mutation == 'hold-m5-outer-compute-host':
        compute_host_impl = compute_host_impl.replace(
            'self.m5_outer = None;',
            '',
            1,
        )
    elif mutation == 'same-qid-emits-barrier':
        resolve_fn = resolve_fn.replace(
            'if published.dependency.qid() == target_qid {',
            'if false && published.dependency.qid() == target_qid {',
            1,
        )
    elif mutation == 'lock-has-published-epoch':
        has_pub_epoch_fn = has_pub_epoch_fn.replace(
            'self.published_epoch.load(Ordering::Acquire) == tag',
            'self.state.lock().published.as_ref().is_some_and(|s| s.epoch == epoch)',
            1,
        )
    elif mutation == 'lock-same-vm':
        same_vm_fn = same_vm_fn.replace(
            'self.vm_status',
            'self.state.lock().work',
            1,
        )
    elif mutation == 'drop-coalesced-error':
        coalesce_fn = coalesce_fn.replace(
            'coalesced_errors.push(earlier, GFP_KERNEL)?;',
            'let _ = earlier;',
            1,
        )
    elif mutation == 'skip-coalesced-error-check':
        sources_check_fn = sources_check_fn.replace(
            'for submission in &self.coalesced_errors { submission.source_status()?; }',
            '',
            1,
        )

    # Structural checks on production sources.
    collect_cap_ok = 'const INLINE_LEAVES: usize = 4;' in sync_rs
    leaf_loop_start = input_sources_fn.index('for leaf in &*leaves {')
    unchecked_block_start = input_sources_fn.index('if !unchecked.is_empty() {')
    leaf_loop = input_sources_fn[leaf_loop_start:unchecked_block_start]
    input_sources_ok = (
        'self.shared.state.lock()' not in leaf_loop
        and 'asahi_g17_export_coalesce_key' not in sync_c
        and ordered_ok(
            input_sources_fn,
            'submission.has_published_epoch(epoch)',
            'sync_input::coalesce_source(',
            'if !unchecked.is_empty() {',
            'let state = self.shared.state.lock();',
            'for idx in unchecked {',
            'sync_input::Sources::new(heads, sources, coalesced_errors, publication_waits)',
        )
        and 'self.check_source_inputs()' in complete_fn
        and 'completion.check_source_inputs()' in deferred_finish_fn
    )
    direct_sources_ok = (
        '(has_sources && !m5)' not in compute_direct_fn
        and ordered_ok(
            compute_direct_fn,
            'let target_qid = cells[0].as_ref()?.qid()?;',
            'self.resolve_direct_sources(port, generation, target_qid, m5, sources)?',
            'queue.qid() != target_qid',
            'queue.publish(',
            'direct_frontier.as_slice(),',
            'queue.publish_with_native_waits(',
            'direct_native.as_slice(),',
            'source.submission.note_firmware_consumer();',
            'sources.set_holds(direct_holds);',
            'drop(direct_holds);',
        )
    )
    lockfree_submission_ok = (
        'published_epoch: AtomicU64,' in fence_rs
        and 'vm_status: Option<Arc<super::status::VmStatus>>,' in fence_rs
        and 'self.state.lock()' not in has_pub_epoch_fn
        and 'self.published_epoch.load(Ordering::Acquire)' in has_pub_epoch_fn
        and 'self.state.lock()' not in same_vm_fn
        and 'self.vm_status' in same_vm_fn
        and 'if self.published_epoch.swap(0, Ordering::AcqRel) == 0 {' in clear_pub_fn
    )
    sources_published_ok = ordered_ok(
        sources_pub_fn,
        'source.submission.has_published_epoch(generation)',
        'let state = self.shared.state.lock();',
    )
    compute_host_ok = ordered_ok(
        compute_host_impl,
        'note_m5_submission_history_locked(guard, 1)',
        'self.m5_outer = None;',
        'self.port.notify(message)',
    )
    publish_invalidation_ok = (
        ordered_ok(
            compute_rs,
            'host.notify((0x83 << 48) | 0x02 | u64::from(policy.priority()) << 2)?;',
            'packet.completion.note_source_published(',
            'if packet.cancel_requested() {',
            'packet.completion.clear_source_published();',
        )
        and 'self.clear_published();' in note_error_fn
        and 'self.clear_source_published();' in begin_replay_fn
        and 'self.completion.clear_source_published();' in cancel_fn
    )

    # Adapt coalesce_source() and resolve_direct_sources() for host compilation.
    host_coalesce = coalesce_fn
    host_coalesce = host_coalesce.replace('KVec<', 'Vec<')
    host_coalesce = host_coalesce.replace('super::fw::kick::KICK_BARRIERS_MAX', 'KICK_BARRIERS_MAX')
    host_coalesce = host_coalesce.replace('coalesced_errors.push(earlier, GFP_KERNEL)?;', 'coalesced_errors.push(earlier);')
    host_coalesce = host_coalesce.replace('sources.push(Source { submission }, GFP_KERNEL)?;', 'sources.push(Source { submission });')
    host_coalesce = host_coalesce.replace('source_keys.push(pub_key, GFP_KERNEL)?;', 'source_keys.push(pub_key);')

    host_resolve = resolve_fn
    host_resolve = host_resolve.replace('super::port::Port', 'Port')
    host_resolve = host_resolve.replace('super::sync_input::Sources', 'Sources')
    host_resolve = host_resolve.replace('super::dependency::Frontier', 'Frontier')
    host_resolve = host_resolve.replace('super::sync_input::NativeWait', 'NativeWait')
    host_resolve = host_resolve.replace('super::sync_input::Hold', 'Hold')
    host_resolve = host_resolve.replace('KVec<NativeWait>', 'Vec<NativeWait>')
    host_resolve = host_resolve.replace('KVec<Hold>', 'Vec<Hold>')
    host_resolve = host_resolve.replace('KVec::new()', 'Vec::new()')
    host_resolve = host_resolve.replace('holds.push(hold, GFP_KERNEL).ok()?;', 'holds.push(hold);')
    host_resolve = host_resolve.replace('native.push(wait, GFP_KERNEL).ok()?;', 'native.push(wait);')

    return f'''
use std::sync::{{Arc, Mutex, atomic::{{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering}}}};

const EBUSY: i32 = -16;
const EIO: i32 = -5;
const ENOSPC: i32 = -28;
const EOVERFLOW: i32 = -75;
const PRODUCER_FAILED: i32 = -5;
const KICK_BARRIERS_MAX: usize = 31;
type Result<T = ()> = std::result::Result<T, i32>;

{hold_struct}
{hold_impl}
{hold_drop}

{hint_struct}
{hint_impl}

#[derive(Debug, PartialEq, Eq)]
{coalesce_enum}
{host_coalesce}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct KickDependency {{
    qid: u8,
    kick: u64,
}}
impl KickDependency {{
    fn qid(self) -> u8 {{ self.qid }}
}}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct NativeWait {{
    sequence: u64,
    dependency: KickDependency,
    stamp_va: u64,
    event_slot: u32,
}}

#[derive(Clone)]
struct PublishedSource {{
    epoch: u64,
    published: u64,
    dependency: KickDependency,
    stamp_va: u64,
    execution_share: u64,
    readers: Arc<AtomicU32>,
}}

struct Context {{
    share_id: u64,
}}
impl Context {{
    fn execution_share_id(&self) -> u64 {{ self.share_id }}
}}

struct Frontier {{
    items: Vec<(u64, KickDependency)>,
}}
impl Frontier {{
    fn new() -> Self {{ Self {{ items: Vec::new() }} }}
    fn insert(&mut self, seq: u64, dep: KickDependency) -> Result {{
        if let Some(old) = self.items.iter_mut().find(|(_, d)| d.qid == dep.qid) {{
            if seq > old.0 {{ *old = (seq, dep); }}
        }} else {{
            self.items.push((seq, dep));
        }}
        Ok(())
    }}
}}

struct Submission {{
    retired: AtomicBool,
    error: AtomicI32,
    published_epoch: AtomicU64,
    firmware_consumer: AtomicBool,
    state_locks: AtomicU32,
    published: Mutex<Option<PublishedSource>>,
}}

impl Submission {{
    fn encode_epoch(epoch: u64) -> u64 {{ epoch.wrapping_add(1) }}
    fn new() -> Arc<Self> {{
        Arc::new(Self {{
            retired: AtomicBool::new(false),
            error: AtomicI32::new(0),
            published_epoch: AtomicU64::new(0),
            firmware_consumer: AtomicBool::new(false),
            state_locks: AtomicU32::new(0),
            published: Mutex::new(None),
        }})
    }}
    fn set_published(&self, src: PublishedSource) {{
        let tag = Self::encode_epoch(src.epoch);
        self.state_locks.fetch_add(1, Ordering::Relaxed);
        *self.published.lock().unwrap() = Some(src);
        self.published_epoch.store(tag, Ordering::Release);
    }}
    fn clear_published(&self) {{
        if self.published_epoch.swap(0, Ordering::AcqRel) == 0 {{
            return;
        }}
        self.state_locks.fetch_add(1, Ordering::Relaxed);
        *self.published.lock().unwrap() = None;
    }}
    fn source_status(&self) -> Result<bool> {{
        if self.error.load(Ordering::Acquire) != 0 {{
            return Err(PRODUCER_FAILED);
        }}
        Ok(self.retired.load(Ordering::Acquire))
    }}
    fn published_source(&self, epoch: u64) -> Option<PublishedSource> {{
        if !self.has_published_epoch(epoch) {{
            return None;
        }}
        self.state_locks.fetch_add(1, Ordering::Relaxed);
        self.published.lock().unwrap().as_ref().filter(|s| s.epoch == epoch).cloned()
    }}
    #[allow(dead_code)]
    fn published_source_unchecked(&self) -> Option<PublishedSource> {{
        if self.published_epoch.load(Ordering::Acquire) == 0 {{
            return None;
        }}
        self.state_locks.fetch_add(1, Ordering::Relaxed);
        self.published.lock().unwrap().clone()
    }}
    fn has_published_epoch(&self, epoch: u64) -> bool {{
        let tag = Self::encode_epoch(epoch);
        tag != 0 && self.published_epoch.load(Ordering::Acquire) == tag
    }}
    fn note_firmware_consumer(&self) {{
        self.firmware_consumer.store(true, Ordering::Release);
    }}
}}

struct Source {{ submission: Arc<Submission> }}
struct Sources {{
    sources: Vec<Source>,
    coalesced_errors: Vec<Arc<Submission>>,
}}
impl Sources {{
    {sources_check_fn}
}}

fn complete_consumer(sources: &Sources, mut result: Result) -> Result {{
    if result.is_ok() {{
        if let Err(error) = sources.check() {{
            result = Err(error);
        }}
    }}
    result
}}

struct Port {{
    gen: AtomicU64,
    rec: AtomicU32,
    flip_rec_on_query: AtomicBool,
}}
impl Port {{
    fn generation(&self) -> u64 {{ self.gen.load(Ordering::Acquire) }}
    fn recovery_state(&self) -> Result<u32> {{
        if self.flip_rec_on_query.swap(false, Ordering::AcqRel) {{
            self.rec.store(1, Ordering::Release);
        }}
        Ok(self.rec.load(Ordering::Acquire))
    }}
}}

struct Backend {{
    context: Context,
}}
impl Backend {{
    {host_resolve}
}}

fn main() {{
    assert!({str(collect_cap_ok).lower()}, "sync_input::collect must start with inline capacity 4");
    assert!({str(input_sources_ok).lower()}, "input_sources must check PublishedSource lock-free and hoist state.lock outside leaf loop");
    assert!({str(direct_sources_ok).lower()}, "compute_direct must resolve M5 and G17P cross-QID sources lock-free and set holds + firmware_consumer");
    assert!({str(lockfree_submission_ok).lower()}, "Submission::has_published_epoch, same_vm, and empty clear_published must be lock-free");
    assert!({str(sources_published_ok).lower()}, "sources_published must check PublishedSource lock-free before state.lock");
    assert!({str(compute_host_ok).lower()}, "ComputeHost::notify must drop m5_outer before port.notify");
    assert!({str(publish_invalidation_ok).lower()}, "compute::Queue must publish after notify and invalidate on quarantine/replay/cancel");

    let backend = Backend {{ context: Context {{ share_id: 42 }} }};
    let port = Port {{
        gen: AtomicU64::new(7),
        rec: AtomicU32::new(0),
        flip_rec_on_query: AtomicBool::new(false),
    }};
    let readers = Arc::new(AtomicU32::new(0));
    let mut hint = ReaderHint::new();

    // 1. Happy path (M5): published cross-QID source (qid 5 -> target_qid 9) resolves lock-free,
    //    Hold blocks ReaderHint::reset_for_owner even though ReaderHint::hold(&mut self) was never called.
    let sub = Submission::new();
    // clear_published() on unpublished submission must take 0 state locks.
    sub.clear_published();
    assert_eq!(sub.state_locks.load(Ordering::Relaxed), 0);
    sub.set_published(PublishedSource {{
        epoch: 7,
        published: 42,
        dependency: KickDependency {{ qid: 5, kick: 100 }},
        stamp_va: 0xfeed_0000,
        execution_share: 42,
        readers: readers.clone(),
    }});
    let locks_before = sub.state_locks.load(Ordering::Relaxed);
    assert!(sub.has_published_epoch(7));
    assert!(!sub.has_published_epoch(8));
    assert_eq!(sub.state_locks.load(Ordering::Relaxed), locks_before, "has_published_epoch must be lock-free");
    let sources = Sources {{ sources: vec![Source {{ submission: sub.clone() }}], coalesced_errors: Vec::new() }};
    let (frontier, native, holds, mask) = backend.resolve_direct_sources(&port, 7, 9, true, &sources).unwrap();
    assert_eq!(frontier.items, vec![(42, KickDependency {{ qid: 5, kick: 100 }})]);
    assert!(native.is_empty());
    assert_eq!(holds.len(), 1);
    assert_eq!(mask, 1);
    assert_eq!(readers.load(Ordering::Acquire), 1);
    assert!(hint.has_readers(&readers), "lock-free Hold::new must be visible to ReaderHint::has_readers");
    assert_eq!(hint.reset_for_owner(&readers), Err(EBUSY), "lock-free Hold must block owner reset");
    sub.note_firmware_consumer();
    assert!(sub.firmware_consumer.load(Ordering::Acquire));
    drop(holds);
    assert_eq!(readers.load(Ordering::Acquire), 0);
    assert!(!hint.has_readers(&readers));
    assert_eq!(hint.reset_for_owner(&readers), Ok(()));

    // 1a. G17P shared execution context (same share_id 42): resolves to Frontier kick barrier, no NativeWait!
    let (f_g17p_same, n_g17p_same, h_g17p_same, m_g17p_same) =
        backend.resolve_direct_sources(&port, 7, 9, false, &sources).unwrap();
    assert_eq!(f_g17p_same.items, vec![(42, KickDependency {{ qid: 5, kick: 100 }})]);
    assert!(n_g17p_same.is_empty());
    assert_eq!(h_g17p_same.len(), 1);
    assert_eq!(m_g17p_same, 1);
    drop(h_g17p_same);
    assert_eq!(readers.load(Ordering::Acquire), 0);

    // 1a2. G17P cross-execution-context (different share_id): resolves to NativeWait (deduped by QID), empty Frontier!
    let other_ctx_backend = Backend {{ context: Context {{ share_id: 99 }} }};
    let sub_later = Submission::new();
    sub_later.set_published(PublishedSource {{
        epoch: 7,
        published: 45,
        dependency: KickDependency {{ qid: 5, kick: 103 }},
        stamp_va: 0xfeed_0000,
        execution_share: 42,
        readers: readers.clone(),
    }});
    let two_same_qid = Sources {{
        sources: vec![Source {{ submission: sub.clone() }}, Source {{ submission: sub_later.clone() }}],
        coalesced_errors: Vec::new(),
    }};
    let (f_g17p_xctx, n_g17p_xctx, h_g17p_xctx, m_g17p_xctx) =
        other_ctx_backend.resolve_direct_sources(&port, 7, 9, false, &two_same_qid).unwrap();
    assert!(f_g17p_xctx.items.is_empty());
    assert_eq!(n_g17p_xctx.len(), 1);
    assert_eq!(n_g17p_xctx[0].sequence, 45);
    assert_eq!(n_g17p_xctx[0].stamp_va, 0xfeed_0000);
    assert_eq!(n_g17p_xctx[0].event_slot, 5);
    assert_eq!(h_g17p_xctx.len(), 2);
    assert_eq!(m_g17p_xctx, 0b11);
    drop(h_g17p_xctx);
    assert_eq!(readers.load(Ordering::Acquire), 0);

    // 1b. Same-QID source (qid 5 -> target_qid 5): skipped cleanly on both M5 and G17P.
    let same_sub = Submission::new();
    same_sub.set_published(PublishedSource {{
        epoch: 7,
        published: 43,
        dependency: KickDependency {{ qid: 5, kick: 101 }},
        stamp_va: 0xfeed_0000,
        execution_share: 42,
        readers: readers.clone(),
    }});
    let same_sources = Sources {{ sources: vec![Source {{ submission: same_sub.clone() }}], coalesced_errors: Vec::new() }};
    for m5 in [true, false] {{
        let (f_same, n_same, h_same, m_same) =
            backend.resolve_direct_sources(&port, 7, 5, m5, &same_sources).unwrap();
        assert!(f_same.items.is_empty(), "same-QID source must not emit a duplicate explicit barrier");
        assert!(n_same.is_empty(), "same-QID source must not emit a NativeWait");
        assert!(h_same.is_empty(), "same-QID source must not take a self-reader Hold");
        assert_eq!(m_same, 0, "same-QID source must not set cross_qid_mask");
        assert_eq!(readers.load(Ordering::Acquire), 0);
        assert!(!same_sub.firmware_consumer.load(Ordering::Acquire));
    }}

    // 1c. Same-QID failed producer MUST still return None so error propagates!
    same_sub.error.store(EIO, Ordering::Release);
    assert!(backend.resolve_direct_sources(&port, 7, 5, true, &same_sources).is_none());
    assert!(backend.resolve_direct_sources(&port, 7, 5, false, &same_sources).is_none());
    assert_eq!(readers.load(Ordering::Acquire), 0);

    // 2. Unpublished source -> clean None fallback.
    let unpub = Submission::new();
    let s_unpub = Sources {{ sources: vec![Source {{ submission: unpub }}], coalesced_errors: Vec::new() }};
    assert!(backend.resolve_direct_sources(&port, 7, 9, true, &s_unpub).is_none());
    assert!(backend.resolve_direct_sources(&port, 7, 9, false, &s_unpub).is_none());

    // 3. Epoch/generation mismatch (port advanced to epoch 8 while source stays at epoch 7)
    //    -> clean None fallback, zero leaked Hold.
    port.gen.store(8, Ordering::Release);
    assert!(backend.resolve_direct_sources(&port, 8, 9, true, &sources).is_none());
    assert!(backend.resolve_direct_sources(&port, 8, 9, false, &sources).is_none());
    assert_eq!(readers.load(Ordering::Acquire), 0);
    port.gen.store(7, Ordering::Release);

    // 4. Recovery starts concurrently right after Hold -> clean None fallback, zero leaked Hold.
    port.flip_rec_on_query.store(true, Ordering::Release);
    assert!(backend.resolve_direct_sources(&port, 7, 9, true, &sources).is_none());
    assert_eq!(readers.load(Ordering::Acquire), 0);
    port.rec.store(0, Ordering::Release);
    port.flip_rec_on_query.store(true, Ordering::Release);
    assert!(backend.resolve_direct_sources(&port, 7, 9, false, &sources).is_none());
    assert_eq!(readers.load(Ordering::Acquire), 0);
    port.rec.store(0, Ordering::Release);

    // 5. Cancelled/quarantined source (clear_published) -> clean None fallback.
    let cancelled = Submission::new();
    cancelled.set_published(PublishedSource {{
        epoch: 7,
        published: 44,
        dependency: KickDependency {{ qid: 5, kick: 102 }},
        stamp_va: 0xfeed_0000,
        execution_share: 42,
        readers: readers.clone(),
    }});
    cancelled.clear_published();
    let s_can = Sources {{ sources: vec![Source {{ submission: cancelled }}], coalesced_errors: Vec::new() }};
    assert!(backend.resolve_direct_sources(&port, 7, 9, true, &s_can).is_none());
    assert!(backend.resolve_direct_sources(&port, 7, 9, false, &s_can).is_none());

    // 6. Failed producer -> clean None fallback (so slow path propagates PRODUCER_FAILED).
    let failed = Submission::new();
    failed.set_published(PublishedSource {{
        epoch: 7,
        published: 45,
        dependency: KickDependency {{ qid: 5, kick: 103 }},
        stamp_va: 0xfeed_0000,
        execution_share: 42,
        readers: readers.clone(),
    }});
    failed.error.store(EIO, Ordering::Release);
    let s_fail = Sources {{ sources: vec![Source {{ submission: failed }}], coalesced_errors: Vec::new() }};
    assert!(backend.resolve_direct_sources(&port, 7, 9, true, &s_fail).is_none());
    assert!(backend.resolve_direct_sources(&port, 7, 9, false, &s_fail).is_none());
    assert_eq!(readers.load(Ordering::Acquire), 0);

    // 7. Already-retired source -> skipped cleanly with empty frontier and 0 holds.
    let retired = Submission::new();
    retired.retired.store(true, Ordering::Release);
    let s_ret = Sources {{ sources: vec![Source {{ submission: retired }}], coalesced_errors: Vec::new() }};
    let (f_ret, n_ret, h_ret, m_ret) = backend.resolve_direct_sources(&port, 7, 9, false, &s_ret).unwrap();
    assert!(f_ret.items.is_empty());
    assert!(n_ret.is_empty());
    assert!(h_ret.is_empty());
    assert_eq!(m_ret, 0);

    // 8. Coalesced same-(epoch, QID) exports retain earlier error dependencies:
    //    when an earlier initially healthy export fails AFTER collection while the retained
    //    later export completes cleanly, the consumer outcome MUST fail with PRODUCER_FAILED.
    for reverse_order in [false, true] {{
        let early = Submission::new();
        let late = Submission::new();
        let other = Submission::new();
        early.set_published(PublishedSource {{
            epoch: 7,
            published: 100,
            dependency: KickDependency {{ qid: 5, kick: 200 }},
            stamp_va: 0xfeed_0000,
            execution_share: 42,
            readers: readers.clone(),
        }});
        late.set_published(PublishedSource {{
            epoch: 7,
            published: 105,
            dependency: KickDependency {{ qid: 5, kick: 205 }},
            stamp_va: 0xfeed_0000,
            execution_share: 42,
            readers: readers.clone(),
        }});
        other.set_published(PublishedSource {{
            epoch: 7,
            published: 102,
            dependency: KickDependency {{ qid: 6, kick: 202 }},
            stamp_va: 0xfeed_1000,
            execution_share: 42,
            readers: readers.clone(),
        }});
        let key_q5 = (8u64 << 8) | 5;
        let key_q6 = (8u64 << 8) | 6;
        let mut wait_sources = Vec::new();
        let mut keys = Vec::new();
        let mut coalesced_errors = Vec::new();
        let items: Vec<(Arc<Submission>, Option<(u64, u64)>)> = if reverse_order {{
            vec![
                (late.clone(), Some((key_q5, 105))),
                (early.clone(), Some((key_q5, 100))),
                (other.clone(), Some((key_q6, 102))),
            ]
        }} else {{
            vec![
                (early.clone(), Some((key_q5, 100))),
                (late.clone(), Some((key_q5, 105))),
                (other.clone(), Some((key_q6, 102))),
            ]
        }};
        for (s, k) in items {{
            coalesce_source(&mut wait_sources, &mut keys, &mut coalesced_errors, s, k).unwrap();
        }}
        // Duplicate of already-coalesced early submission is suppressed without growing error list.
        assert_eq!(
            coalesce_source(&mut wait_sources, &mut keys, &mut coalesced_errors, early.clone(), Some((key_q5, 100))),
            Ok(Coalesce::Duplicate)
        );
        // Firmware wait list collapsed to 2 (latest on QID 5 + QID 6); earlier QID 5 kept for error checks.
        assert_eq!(wait_sources.len(), 2);
        assert!(Arc::ptr_eq(&wait_sources[0].submission, &late));
        assert!(Arc::ptr_eq(&wait_sources[1].submission, &other));
        assert_eq!(coalesced_errors.len(), 1);
        assert!(Arc::ptr_eq(&coalesced_errors[0], &early));

        let coalesced = Sources {{ sources: wait_sources, coalesced_errors }};
        // Initially healthy: direct resolution emits only 2 barriers/holds (not 3).
        let (f_c, n_c, h_c, m_c) = backend.resolve_direct_sources(&port, 7, 9, true, &coalesced).unwrap();
        assert_eq!(f_c.items.len(), 2);
        assert!(n_c.is_empty());
        assert_eq!(h_c.len(), 2);
        assert_eq!(m_c, 0b11);
        drop(h_c);
        assert_eq!(readers.load(Ordering::Acquire), 0);

        // Late error after collection: retained later export and other QID succeed cleanly,
        // while earlier coalesced export fails independently.
        late.retired.store(true, Ordering::Release);
        other.retired.store(true, Ordering::Release);
        assert_eq!(complete_consumer(&coalesced, Ok(())), Ok(()));

        early.error.store(EIO, Ordering::Release);
        assert_eq!(late.source_status(), Ok(true));
        assert_eq!(coalesced.check(), Err(PRODUCER_FAILED));
        assert_eq!(complete_consumer(&coalesced, Ok(())), Err(PRODUCER_FAILED));
        assert!(backend.resolve_direct_sources(&port, 7, 9, true, &coalesced).is_none());
        assert!(backend.resolve_direct_sources(&port, 7, 9, false, &coalesced).is_none());
        assert_eq!(readers.load(Ordering::Acquire), 0);
    }}

    println!("PASS g17-m5-cross-qid-direct-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['no-chain-dedup',
                                               'coalesce-failed-leaf',
                                               'hint-blind-spot',
                                               'skip-epoch-check',
                                               'skip-recovery-recheck',
                                               'lock-per-leaf',
                                               'hold-m5-outer-compute-host',
                                               'same-qid-emits-barrier',
                                               'lock-has-published-epoch',
                                               'lock-same-vm',
                                               'drop-coalesced-error',
                                               'skip-coalesced-error-check'])
    args = parser.parse_args()
    cc = os.environ.get('HOSTCC', 'cc')
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-xqid-') as td:
        c_file = Path(td) / 'c_test.c'
        c_exe = Path(td) / 'c_test'
        c_file.write_text(build_c_harness(args.mutation))
        subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', '-o', str(c_exe), str(c_file)], check=True)
        res_c = subprocess.run([str(c_exe)])
        if res_c.returncode != 0:
            sys.exit(res_c.returncode)

        rs_file = Path(td) / 'rs_test.rs'
        rs_exe = Path(td) / 'rs_test'
        rs_file.write_text(build_rust_harness(args.mutation))
        subprocess.run([rustc, '-O', '-o', str(rs_exe), str(rs_file)], check=True)
        res_rs = subprocess.run([str(rs_exe)])
        sys.exit(res_rs.returncode)


if __name__ == '__main__':
    main()
