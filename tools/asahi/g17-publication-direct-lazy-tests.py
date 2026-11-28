#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host test for direct timestamp-writer linking and lazy Publication attachment.

Verifies:
1. `collect_publication_predecessors()` in `queue/mod.rs` links directly to
   pending timestamp writers (`has_outputs()`) and failed entries (`status() < 0`)
   via `timestamp_publications`, preventing an O(N^2) clique of intermediate
   timestamp-free pass-through links, while `PublicationChain::link()` keeps the
   newest uncovered pending timestamp-free entry linked so its failure still
   reaches a covering successor, whether it fails before or after the successor
   is enqueued.
2. `Publication` in `completion/publication.rs` defers predecessor attachment
   until `finish()` with `target: None` in `deferred_links` (no Arc self-cycle
   when dropped unfinished), skips locking cleanly settled predecessors on the
   in-order path, and preserves exact out-of-order / failed predecessor
   propagation and settlement semantics.
"""
import argparse
import os
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


def build_harness(mutation=None):
    pub_src = (G17 / 'completion/publication.rs').read_text()
    q_src = (G17 / 'queue/mod.rs').read_text()
    job_src = (G17 / 'job.rs').read_text()

    collect_fn = item(q_src, 'fn collect_publication_predecessors<')
    build_fn = item(q_src, 'fn build<E: FrontierEntry>(')
    activate_fn = item(pub_src, 'pub(crate) fn activate(self)')
    new_prep_fn = item(pub_src, 'pub(crate) fn new<\'a>(')
    finish_fn = item(pub_src, 'pub(crate) fn finish(self: &Arc<Self>,')
    attach_fn = item(pub_src, 'fn attach_deferred(self: &Arc<Self>,')
    publish_fn = item(pub_src, 'fn publish(&self, result: Result<[u64; 4]>) -> Links')
    is_pending_fn = item(pub_src, 'fn pending_within(&self, depth: usize) -> bool')
    chain_fn = item(q_src, 'fn link(self, direct: &[Arc<Publication>], out: &mut KVec<Arc<Publication>>)')

    if mutation == 'clique-chain':
        collect_fn = collect_fn.replace(
            'if pub_ref.has_outputs() || entry.status() < 0 {',
            'if true {',
            1,
        )
    elif mutation == 'skip-failed-status-direct':
        collect_fn = collect_fn.replace(
            'if pub_ref.has_outputs() || entry.status() < 0 {',
            'if pub_ref.has_outputs() {',
            1,
        )
    elif mutation == 'eager-activate':
        activate_fn = activate_fn.replace(
            'state.deferred_links = self.links;',
            'for (p, l) in self.links { p.add_successor(l); }',
            1,
        )
    elif mutation == 'cycle-target-some':
        new_prep_fn = new_prep_fn.replace(
            'target: None,',
            'target: Some(target.clone()),',
            1,
        )
    elif mutation == 'skip-error-clear-settled':
        publish_fn = publish_fn.replace(
            'self.settled_clean.store(false, Ordering::Release);',
            '',
            1,
        )
    elif mutation == 'drop-chain-link':
        build_fn = build_fn.replace('chain.link(&predecessors, &mut chained)?;', '', 2)
    elif mutation == 'retry-failed-pred':
        is_pending_fn = is_pending_fn.replace('.position(|(pred, _)| pred.failed_after_release())', '.position(|_| false)', 1)
    elif mutation == 'keep-attached-successors':
        is_pending_fn = is_pending_fn.replace('Probe::Settled(old, state.successors.take())', 'Probe::Settled(old, None)', 1)
    elif mutation == 'skip-fast-check':
        attach_fn = attach_fn.replace(
            'if predecessor.settled_clean.load(Ordering::Acquire) {',
            'if false {',
            1,
        )

    src_checks = {
        'fences_field': 'pub(crate) timestamp_publications: Option<Arc<KVec<Arc<super::completion::Publication>>>>,' in job_src,
        'build_uses_collect': 'collect_publication_predecessors(entry, &mut predecessors)?;' in build_fn,
        'collect_direct_writer': 'if pub_ref.has_outputs() || entry.status() < 0 {' in collect_fn
            and 'entry.timestamp_publications()' in collect_fn,
        'lazy_activate': 'state.deferred_links = self.links;' in activate_fn
            and 'predecessor.add_successor' not in activate_fn,
        'no_cycle_prep': 'target: None,' in new_prep_fn,
        'fast_settled_check': 'if predecessor.settled_clean.load(Ordering::Acquire) {' in attach_fn,
        'clear_settled_on_err': 'self.settled_clean.store(false, Ordering::Release);' in publish_fn,
        'finish_takes_deferred': 'core::mem::take(&mut state.deferred_links)' in finish_fn,
        'is_pending_settles': 'state.upstream_released = true;' in is_pending_fn,
        'bounded_probe': 'pred.pending_within(depth - 1)' in is_pending_fn,
        'consume_failed': '.position(|(pred, _)| pred.failed_after_release())' in is_pending_fn
            and 'Probe::Consumed(state.deferred_links.swap_remove(index))' in is_pending_fn,
        'release_attached': 'Probe::Settled(old, state.successors.take())' in is_pending_fn
            and 'Self::drain(Self::released_ok(successors));' in is_pending_fn,
        'covers_field': 'pub(crate) covers: Option<u64>,' in job_src,
        'chain_links': build_fn.count('chain.link(&predecessors, &mut chained)?;') == 2
            and 'Order::contains(covered, *sequence)' in chain_fn,
    }

    # Behavioral Rust model mirroring the exact lazy Publication + direct-writer rules.
    use_clique = mutation == 'clique-chain'
    skip_failed_direct = mutation == 'skip-failed-status-direct'
    eager_activate = mutation == 'eager-activate'
    cycle_target_some = mutation == 'cycle-target-some'
    skip_err_clear = mutation == 'skip-error-clear-settled'
    skip_fast = mutation == 'skip-fast-check'
    drop_chain = mutation == 'drop-chain-link'
    retry_failed = mutation == 'retry-failed-pred'
    keep_attached = mutation == 'keep-attached-successors'

    return f'''
use std::sync::{{Arc, Mutex, Weak, atomic::{{AtomicBool, AtomicI32, AtomicUsize, Ordering}}}};

const PRODUCER_FAILED: i32 = -5;
const EIO: i32 = -5;
const PENDING_PROBE_DEPTH: usize = 4;

type Links = Option<Box<Link>>;

enum Probe {{
    Consumed((Arc<Publication>, Box<Link>)),
    Settled(Vec<(Arc<Publication>, Box<Link>)>, Links),
    Next(Arc<Publication>),
}}

struct Link {{
    target: Option<Arc<Publication>>,
    result: Result<(), i32>,
    next: Links,
    resume: Links,
}}

struct State {{
    deferred_links: Vec<(Arc<Publication>, Box<Link>)>,
    remaining: usize,
    result: Option<Result<(), i32>>,
    predecessor_error: Option<i32>,
    claimed: bool,
    published: Option<Result<(), i32>>,
    upstream_released: bool,
    successors: Links,
}}

impl State {{
    fn claim(&mut self) -> Option<Result<(), i32>> {{
        if self.remaining != 0 || self.claimed {{
            return None;
        }}
        let result = self.result.take()?;
        self.claimed = true;
        Some(match (result, self.predecessor_error) {{
            (Ok(()), Some(err)) => Err(err),
            (res, _) => res,
        }})
    }}
}}

struct Publication {{
    has_outputs: bool,
    pending: AtomicBool,
    settled_clean: AtomicBool,
    fence_status: AtomicI32,
    add_succ_calls: AtomicUsize,
    state: Mutex<State>,
}}

impl Publication {{
    fn new(has_outputs: bool) -> Arc<Self> {{
        Arc::new(Self {{
            has_outputs,
            pending: AtomicBool::new(true),
            settled_clean: AtomicBool::new(false),
            fence_status: AtomicI32::new(0),
            add_succ_calls: AtomicUsize::new(0),
            state: Mutex::new(State {{
                deferred_links: Vec::new(),
                remaining: 1,
                result: None,
                predecessor_error: None,
                claimed: false,
                published: None,
                upstream_released: false,
                successors: None,
            }}),
        }})
    }}

    fn has_outputs(&self) -> bool {{ self.has_outputs }}

    fn is_pending(&self) -> bool {{
        self.pending_within(PENDING_PROBE_DEPTH)
    }}

    fn pending_within(&self, depth: usize) -> bool {{
        if !self.pending.load(Ordering::Acquire) {{
            return false;
        }}
        if self.has_outputs {{
            return true;
        }}
        loop {{
            let step = {{
                let mut state = self.state.lock().unwrap();
                if !self.pending.load(Ordering::Acquire) {{
                    return false;
                }}
                if state.published.is_some() || state.claimed || state.deferred_links.is_empty() {{
                    return self.pending.load(Ordering::Acquire);
                }}
                let failed = if {str(retry_failed).lower()} {{
                    None
                }} else {{
                    state.deferred_links.iter().position(|(p, _)| p.failed_after_release())
                }};
                if let Some(index) = failed {{
                    if state.predecessor_error.is_none() {{
                        state.predecessor_error = Some(PRODUCER_FAILED);
                    }}
                    Probe::Consumed(state.deferred_links.swap_remove(index))
                }} else {{
                    let unsettled = state
                        .deferred_links
                        .iter()
                        .find(|(p, _)| !p.settled_clean.load(Ordering::Acquire))
                        .map(|(p, _)| p.clone());
                    match unsettled {{
                        None if state.predecessor_error.is_none() => {{
                            let old = std::mem::take(&mut state.deferred_links);
                            state.upstream_released = true;
                            self.settled_clean.store(true, Ordering::Release);
                            self.pending.store(false, Ordering::Release);
                            let succ = if {str(keep_attached).lower()} {{ None }} else {{ state.successors.take() }};
                            Probe::Settled(old, succ)
                        }}
                        None => return true,
                        Some(p) if !p.has_outputs => Probe::Next(p),
                        Some(_) => return true,
                    }}
                }}
            }};
            match step {{
                Probe::Consumed(link) => drop(link),
                Probe::Settled(old, successors) => {{
                    drop(old);
                    Self::drain(Self::released_ok(successors));
                    return false;
                }}
                Probe::Next(p) => {{
                    if depth == 0 || p.pending_within(depth - 1) {{
                        return true;
                    }}
                }}
            }}
        }}
    }}

    fn failed_after_release(&self) -> bool {{
        !self.pending.load(Ordering::Acquire) && !self.settled_clean.load(Ordering::Acquire)
    }}

    fn released_ok(mut links: Links) -> Links {{
        let mut next = links.as_mut();
        while let Some(link) = next {{
            link.result = Ok(());
            next = link.next.as_mut();
        }}
        links
    }}

    fn finish(self: &Arc<Self>, result: Result<(), i32>) {{
        let (ready, to_attach) = {{
            let mut state = self.state.lock().unwrap();
            if state.claimed || state.result.is_some() {{
                return;
            }}
            state.result = Some(result);
            let deferred = std::mem::take(&mut state.deferred_links);
            if deferred.is_empty() {{
                (state.claim(), deferred)
            }} else {{
                state.remaining += 1 + deferred.len();
                (None, deferred)
            }}
        }};
        if let Some(res) = ready {{
            Self::drain(self.publish(res));
            return;
        }}
        if !to_attach.is_empty() {{
            self.attach_deferred(to_attach);
        }}
    }}

    fn attach_deferred(self: &Arc<Self>, deferred: Vec<(Arc<Publication>, Box<Link>)>) {{
        let mut fast_ok = 0usize;
        for (pred, mut link) in deferred {{
            if !{str(skip_fast).lower()} && pred.settled_clean.load(Ordering::Acquire) {{
                fast_ok += 1;
                drop(link);
            }} else {{
                link.target = Some(self.clone());
                pred.add_successor(link);
            }}
        }}
        Self::drain(self.predecessors_done_ok(1 + fast_ok));
    }}

    fn publish(&self, result: Result<(), i32>) -> Links {{
        self.fence_status.store(match result {{ Ok(()) => 1, Err(e) => e }}, Ordering::Release);
        let mut links = {{
            let mut state = self.state.lock().unwrap();
            state.published = Some(result);
            if result.is_ok() {{
                self.settled_clean.store(true, Ordering::Release);
                self.pending.store(false, Ordering::Release);
            }} else if !{str(skip_err_clear).lower()} {{
                self.settled_clean.store(false, Ordering::Release);
            }}
            state.successors.take()
        }};
        let mut next = links.as_mut();
        while let Some(link) = next {{
            link.result = result;
            next = link.next.as_mut();
        }}
        links
    }}

    fn predecessors_done_ok(&self, count: usize) -> Links {{
        let (ready, settled) = {{
            let mut state = self.state.lock().unwrap();
            state.remaining -= count;
            if let Some(ready) = state.claim() {{
                (Some(ready), None)
            }} else if state.remaining == 0
                && !self.has_outputs
                && state.predecessor_error.is_none()
                && !state.upstream_released
            {{
                state.upstream_released = true;
                self.settled_clean.store(true, Ordering::Release);
                self.pending.store(false, Ordering::Release);
                (None, state.successors.take())
            }} else {{
                (None, None)
            }}
        }};
        if let Some(res) = ready {{
            return self.publish(res);
        }}
        let mut links = settled;
        let mut next = links.as_mut();
        while let Some(link) = next {{
            link.result = Ok(());
            next = link.next.as_mut();
        }}
        links
    }}

    fn predecessor_done(&self, result: Result<(), i32>) -> Links {{
        if result.is_ok() {{
            return self.predecessors_done_ok(1);
        }}
        let ready = {{
            let mut state = self.state.lock().unwrap();
            if state.predecessor_error.is_none() {{
                state.predecessor_error = Some(PRODUCER_FAILED);
            }}
            state.remaining -= 1;
            state.claim()
        }};
        if let Some(res) = ready {{
            return self.publish(res);
        }}
        None
    }}

    fn add_successor(&self, mut link: Box<Link>) {{
        self.add_succ_calls.fetch_add(1, Ordering::Relaxed);
        let immediate = {{
            let mut state = self.state.lock().unwrap();
            if let Some(res) = state.published {{
                res
            }} else if state.upstream_released {{
                Ok(())
            }} else {{
                link.next = state.successors.take();
                state.successors = Some(link);
                return;
            }}
        }};
        if let Some(target) = link.target.take() {{
            Self::drain(target.predecessor_done(immediate));
        }}
    }}

    fn drain(mut pending: Links) {{
        let mut frames: Links = None;
        loop {{
            if let Some(mut link) = pending.take() {{
                pending = link.next.take();
                let children = link
                    .target
                    .take()
                    .and_then(|t| t.predecessor_done(link.result));
                if children.is_some() {{
                    link.resume = pending.take();
                    link.next = frames.take();
                    frames = Some(link);
                    pending = children;
                }}
            }} else if let Some(mut frame) = frames.take() {{
                frames = frame.next.take();
                pending = frame.resume.take();
            }} else {{
                break;
            }}
        }}
    }}
}}

fn activate(target: &Arc<Publication>, preds: &[Arc<Publication>]) {{
    let mut links = Vec::with_capacity(preds.len());
    for p in preds {{
        links.push((p.clone(), Box::new(Link {{
            target: if {str(cycle_target_some).lower()} {{ Some(target.clone()) }} else {{ None }},
            result: Ok(()),
            next: None,
            resume: None,
        }})));
    }}
    if {str(eager_activate).lower()} {{
        target.state.lock().unwrap().remaining += links.len();
        for (p, mut l) in links {{
            l.target = Some(target.clone());
            p.add_successor(l);
        }}
        Publication::drain(target.predecessors_done_ok(1));
        return;
    }}
    let mut state = target.state.lock().unwrap();
    state.remaining -= 1;
    if links.is_empty() && !target.has_outputs {{
        state.upstream_released = true;
        target.settled_clean.store(true, Ordering::Release);
        target.pending.store(false, Ordering::Release);
    }} else {{
        state.deferred_links = links;
    }}
}}

struct Entry {{
    seq: u64,
    covers: Option<u64>,
    pub_node: Arc<Publication>,
    inherited: Vec<Arc<Publication>>,
}}

fn has(out: &[Arc<Publication>], p: &Arc<Publication>) -> bool {{
    out.iter().any(|e| Arc::ptr_eq(e, p))
}}

// EnqueueOrdering::build() over one covered prefix: the flattened writers (inherited by later
// commands) and the chain links (PublicationChain::link), kept apart.
fn build(prefix: &[Entry]) -> (Vec<Arc<Publication>>, Vec<Arc<Publication>>) {{
    let mut direct = Vec::new();
    let mut pending = Vec::new();
    for e in prefix {{
        if e.pub_node.fence_status.load(Ordering::Acquire) > 0 || !e.pub_node.is_pending() {{
            continue;
        }}
        collect(e, &mut direct);
        pending.push(e);
    }}
    let mut chain = Vec::new();
    if !{str(drop_chain).lower()} {{
        let mut covered: Option<u64> = None;
        for e in pending.iter().rev() {{
            let inside = covered.is_some_and(|c| e.seq != 0 && e.seq <= c);
            if !inside && !e.pub_node.has_outputs() && !has(&direct, &e.pub_node) && !has(&chain, &e.pub_node) {{
                chain.push(e.pub_node.clone());
            }}
            covered = covered.max(e.covers);
        }}
    }}
    (direct, chain)
}}

fn all(direct: &[Arc<Publication>], chain: &[Arc<Publication>]) -> Vec<Arc<Publication>> {{
    direct.iter().chain(chain.iter()).cloned().collect()
}}

fn collect(entry: &Entry, out: &mut Vec<Arc<Publication>>) {{
    if !entry.pub_node.is_pending() {{
        return;
    }}
    let status = entry.pub_node.fence_status.load(Ordering::Acquire);
    let direct = if {str(use_clique).lower()} {{
        true
    }} else if {str(skip_failed_direct).lower()} {{
        entry.pub_node.has_outputs()
    }} else {{
        entry.pub_node.has_outputs() || status < 0
    }};
    if direct || entry.inherited.is_empty() {{
        if !out.iter().any(|e| Arc::ptr_eq(e, &entry.pub_node)) {{
            out.push(entry.pub_node.clone());
        }}
    }} else {{
        for anc in &entry.inherited {{
            if anc.is_pending() && !out.iter().any(|e| Arc::ptr_eq(e, anc)) {{
                out.push(anc.clone());
            }}
        }}
    }}
}}

fn main() {{
    // Static contract checks on actual kernel sources.
    assert!({str(src_checks['fences_field']).lower()}, "Fences must store timestamp_publications");
    assert!({str(src_checks['build_uses_collect']).lower()}, "EnqueueOrdering::build must use collect_publication_predecessors");
    assert!({str(src_checks['collect_direct_writer']).lower()}, "collect_publication_predecessors must link directly to writers/failed entries");
    assert!({str(src_checks['lazy_activate']).lower()}, "Prepared::activate must defer links instead of eager add_successor");
    assert!({str(src_checks['no_cycle_prep']).lower()}, "Prepared::new must set link.target = None");
    assert!({str(src_checks['fast_settled_check']).lower()}, "attach_deferred must fast-path settled_clean predecessors");
    assert!({str(src_checks['clear_settled_on_err']).lower()}, "publish(Err) must clear settled_clean");
    assert!({str(src_checks['finish_takes_deferred']).lower()}, "finish() must take deferred_links before attaching");
    assert!({str(src_checks['is_pending_settles']).lower()}, "is_pending() must settle clean deferred chains");
    assert!({str(src_checks['bounded_probe']).lower()}, "is_pending() must bound its predecessor probe");
    assert!({str(src_checks['consume_failed']).lower()}, "is_pending() must consume a predecessor that failed after releasing");
    assert!({str(src_checks['release_attached']).lower()}, "early settlement must release attached successors");
    assert!({str(src_checks['covers_field']).lower()}, "Fences must record the prefix it covers");
    assert!({str(src_checks['chain_links']).lower()}, "EnqueueOrdering::build must chain both covered prefixes");

    // 1. 64-deep timestamp-free chain behind 1 writer must stay O(1) links per node (NOT O(N^2) clique):
    // the writer plus the newest pending entry, which covers the older ones.
    let writer = Publication::new(true);
    activate(&writer, &[]);
    let mut entries = vec![Entry {{ seq: 1, covers: None, pub_node: writer.clone(), inherited: vec![] }}];
    let mut total_links = 0usize;
    for seq in 2..=65u64 {{
        let (preds, chain) = build(&entries);
        assert_eq!(preds.len(), 1, "must flatten to the direct timestamp writer, got {{}}", preds.len());
        assert!(Arc::ptr_eq(&preds[0], &writer));
        assert!(chain.len() <= 1, "chain must link only the newest pending entry, got {{}}", chain.len());
        total_links += preds.len() + chain.len();
        let node = Publication::new(false);
        activate(&node, &all(&preds, &chain));
        entries.push(Entry {{ seq, covers: Some(seq - 1), pub_node: node, inherited: preds }});
    }}
    assert!(total_links <= 2 * 64, "links {{}}", total_links);
    assert_eq!(writer.add_succ_calls.load(Ordering::Relaxed), 0, "lazy activate must not lock predecessor");

    // 2. In-order writer finish drains 0 links and successors skip add_successor completely.
    writer.finish(Ok(()));
    assert_eq!(writer.fence_status.load(Ordering::Acquire), 1);
    for e in &entries[1..] {{
        e.pub_node.finish(Ok(()));
        assert_eq!(e.pub_node.fence_status.load(Ordering::Acquire), 1);
    }}
    assert_eq!(writer.add_succ_calls.load(Ordering::Relaxed), 0, "in-order finish must skip predecessor lock");

    // 3. Out-of-order consumer finishing before producer fails when producer fails.
    let p2 = Publication::new(true);
    activate(&p2, &[]);
    let c2 = Publication::new(false);
    activate(&c2, &[p2.clone()]);
    c2.finish(Ok(()));
    assert_eq!(c2.fence_status.load(Ordering::Acquire), 0);
    p2.finish(Err(EIO));
    assert_eq!(c2.fence_status.load(Ordering::Acquire), PRODUCER_FAILED);

    // 4. Failed timestamp-free entry (status < 0) is linked directly even if inherited writer settled.
    let w3 = Publication::new(true);
    activate(&w3, &[]);
    let mid = Publication::new(false);
    activate(&mid, &[w3.clone()]);
    let mid_entry = Entry {{ seq: 2, covers: Some(1), pub_node: mid.clone(), inherited: vec![w3.clone()] }};
    w3.finish(Ok(()));
    mid.finish(Err(EIO));
    let mut preds3 = Vec::new();
    collect(&mid_entry, &mut preds3);
    assert_eq!(preds3.len(), 1, "failed entry must be linked directly");
    assert!(Arc::ptr_eq(&preds3[0], &mid));

    // 5. Settled consumer failing later still delivers error to late successor.
    let w4 = Publication::new(true);
    activate(&w4, &[]);
    let cons4 = Publication::new(false);
    activate(&cons4, &[w4.clone()]);
    w4.finish(Ok(()));
    assert!(!cons4.is_pending());
    cons4.finish(Err(EIO));
    let late4 = Publication::new(false);
    activate(&late4, &[cons4.clone()]);
    late4.finish(Ok(()));
    assert_eq!(late4.fence_status.load(Ordering::Acquire), PRODUCER_FAILED);

    // 6. Activated-then-dropped publication without finish() has ZERO Arc cycle!
    let w5 = Publication::new(true);
    activate(&w5, &[]);
    let weak_c5: Weak<Publication>;
    {{
        let c5 = Publication::new(false);
        activate(&c5, &[w5.clone()]);
        weak_c5 = Arc::downgrade(&c5);
    }}
    assert!(weak_c5.upgrade().is_none(), "unfinished deferred publication leaked via Arc self-cycle!");

    // 7./8. Timestamp-free B fails while its writer A is still pending (its fence still reads 0).
    // C covers B; it flattens B to A, and must still inherit B's failure when A succeeds,
    // whether B fails before (7) or after (8) C is enqueued.
    for fail_before in [true, false] {{
        let a = Publication::new(true);
        activate(&a, &[]);
        let b = Publication::new(false);
        activate(&b, &[a.clone()]);
        let mut prefix = vec![
            Entry {{ seq: 1, covers: None, pub_node: a.clone(), inherited: vec![] }},
            Entry {{ seq: 2, covers: Some(1), pub_node: b.clone(), inherited: vec![a.clone()] }},
        ];
        if fail_before {{
            b.finish(Err(EIO));
            assert_eq!(b.fence_status.load(Ordering::Acquire), 0);
        }}
        let (preds, chain) = build(&prefix);
        let c = Publication::new(true);
        activate(&c, &all(&preds, &chain));
        prefix.clear();
        if !fail_before {{
            b.finish(Err(EIO));
        }}
        c.finish(Ok(()));
        a.finish(Ok(()));
        assert_eq!(b.fence_status.load(Ordering::Acquire), EIO);
        assert_eq!(c.fence_status.load(Ordering::Acquire), PRODUCER_FAILED,
            "covering successor must inherit B's failure (fail_before={{}})", fail_before);
    }}

    // 9. B released its successors early (writer A clean) and then failed. P, still deferred on B,
    // must end its probe (not retry B forever) and publish B's failure.
    {{
        let a = Publication::new(true);
        activate(&a, &[]);
        let b = Publication::new(false);
        activate(&b, &[a.clone()]);
        let p = Publication::new(false);
        activate(&p, &[b.clone()]);
        a.finish(Ok(()));
        assert!(!b.is_pending(), "B settles once A published cleanly");
        b.finish(Err(EIO));
        assert!(b.failed_after_release());
        let probe = p.clone();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {{ let _ = tx.send(probe.is_pending()); }});
        let pending = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("probe of a predecessor that failed after releasing never ended");
        assert!(pending, "a consumed failure keeps P pending until finish()");
        p.finish(Ok(()));
        assert_eq!(p.fence_status.load(Ordering::Acquire), PRODUCER_FAILED);
    }}

    // 10. S finished and attached to B before B settled. B settling through is_pending() must
    // release S then, not when some unrelated completion finishes B.
    {{
        let a = Publication::new(true);
        activate(&a, &[]);
        let b = Publication::new(false);
        activate(&b, &[a.clone()]);
        let s = Publication::new(true);
        activate(&s, &[b.clone()]);
        s.finish(Ok(()));
        a.finish(Ok(()));
        assert_eq!(s.fence_status.load(Ordering::Acquire), 0);
        assert!(!b.is_pending());
        assert_eq!(s.fence_status.load(Ordering::Acquire), 1,
            "successor attached before an early settlement must be released by it");
    }}

    println!("PASS g17-publication-direct-lazy-tests");
    std::process::exit(0);
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=[
        'clique-chain',
        'skip-failed-status-direct',
        'eager-activate',
        'cycle-target-some',
        'skip-error-clear-settled',
        'skip-fast-check',
        'drop-chain-link',
        'retry-failed-pred',
        'keep-attached-successors',
    ])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-pub-lazy-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
