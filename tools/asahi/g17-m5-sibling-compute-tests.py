#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for M5 Pro in-context sibling compute channels.

Compiles production `Context::next_compute_identity()`, `multi_channel()`,
channel selection, sibling admission, release ordering, and multi-channel exit
acknowledgment logic extracted from the G17 driver against host stubs, and
checks structural invariants for dedicated per-sibling ComputePool, lock-free
primary-first `compute_direct()` routing, and recovery/quarantine interaction:
1. Per-context atomic monotonic `(predecessor_id, current_id)` in `object_ids`
   and aligned 1-based `submit_sequence == completion_ordinal` (including
   multi-threaded concurrency proving zero ID collisions across sibling QIDs).
2. Dedicated per-sibling `ComputePool` (no `PoolBorrow`, no shared `0x7c/0x80`
   pool control contention): a warm reusable sibling requires a same-VM
   populated fitting pool, a vacant or other-VM idle queue is re-armed with
   fresh backing, and a fresh sibling allocates its own `PoolBacking::new` +
   pool descriptor (g17-m5-sibling-rearm-tests.py runs the re-arm path).
3. Lock-free `ComputeCell` atomic `(configuring, active_len, last_published)`
   mirror updated via `QueueGuard::drop()`, with primary-first routing in
   `compute_direct()` so idle channel 0 never locks sibling cell mutexes.
4. `multi_channel()` board selection, lazy sibling acquisition (cap at 3),
   `select_compute_channel()` spread & dependent-kick stickiness,
   sibling-before-primary release/rollback/growth order, and multi-channel
   exit acknowledgment + `compute_bound()` / `release_compute_witness()`.

The fan-out gate (input sources and short kicks stay on the primary channel) has
its own test, g17-m5-sibling-gate-tests.py; here the primary runs sustained heavy
work so every sibling path is open.
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


def build_harness(mutation=None):
    freelist_src = (G17 / 'freelist.rs').read_text()
    context_src = (G17 / 'context.rs').read_text()
    fw_src = (G17 / 'fw/m5/compute.rs').read_text()
    runtime_src = (G17 / 'runtime.rs').read_text()
    backend_src = (G17 / 'runtime/backend.rs').read_text()
    exit_src = (G17 / 'runtime/compute_exit.rs').read_text()
    queue_src = (G17 / 'queue/compute.rs').read_text()
    pairs_src = (G17 / 'pairs.rs').read_text()
    min_in_flight = re.search(r'pub\(crate\) const SIBLING_MIN_IN_FLIGHT: usize = [0-9]+;', pairs_src).group(0)
    overlaps_fn = (min_in_flight + '\n' + item(pairs_src, 'pub(crate) fn overlaps(')).replace('pub(crate) ', '')

    next_id_fn = item(context_src, 'pub(crate) fn next_compute_identity(').replace('pub(crate) ', 'pub ')
    multi_fn = item(fw_src, 'pub(crate) fn multi_channel(').replace('pub(crate) ', '')
    initdata_src = (G17 / 'fw/m5/initdata.rs').read_text()
    channels_fn = item(initdata_src, 'pub(crate) const fn compute_channels(').replace('pub(crate) ', '')

    cell_struct = item(runtime_src, 'pub(super) struct ComputeCell')
    cell_impl = item(runtime_src, 'impl ComputeCell')
    guard_drop = item(runtime_src, 'impl Drop for QueueGuard<')
    channels_of_fn = item(runtime_src, 'fn compute_channels_of(')
    next_sib_fn = item(runtime_src, 'fn next_sibling_index(')
    source_fn = item(runtime_src, 'fn sibling_source(')
    source_enum = item(runtime_src, 'pub(super) enum SiblingSource').replace('pub(super) ', '')
    headroom_fn = item(runtime_src, 'fn has_compute_sibling_headroom(')
    wants_sib_fn = item(runtime_src, 'fn wants_compute_sibling(')
    bind_sib_fn = item(runtime_src, 'fn bind_reusable_sibling(')
    try_add_sib_fn = item(runtime_src, 'fn try_add_compute_sibling(')
    select_fn = item(runtime_src, 'fn select_compute_channel(')
    release_fn = item(runtime_src, 'pub(super) fn release_compute(')
    ack_exit_fn = item(exit_src, 'pub(super) fn acknowledge_compute_exit(')
    pub_exit_fn = item(exit_src, 'fn publish_compute_exit(')
    rollback_fn = item(runtime_src, 'fn rollback_unpublished_compute(\n')
    backend_rollback_fn = item(backend_src, 'fn rollback_unpublished_compute(&self)')
    grow_fn = item(runtime_src, 'fn grow_compute(')
    drain_sib_fn = item(runtime_src, 'fn drain_compute_siblings(')
    direct_fn = item(runtime_src, 'fn compute_direct(')
    readiness_fn = item(runtime_src, 'fn compute_readiness(&self, packet:')
    remember_fn = item(runtime_src, 'fn remember_direct(')
    claim_outer_fn = item(runtime_src, 'fn claim_outer(')
    bound_fn = item(runtime_src, 'fn compute_bound(')
    witness_fn = item(runtime_src, 'fn release_compute_witness(')
    parent_pred_start = queue_src.index('let parent_retired = self.active.len == 0')
    parent_pred_end = queue_src.index('let m5_args =', parent_pred_start)
    parent_pred = queue_src[parent_pred_start:parent_pred_end]
    m5_args_start = parent_pred_end
    m5_args_end = queue_src.index('let mut m5_dependency_types =', m5_args_start)
    m5_args_block = queue_src[m5_args_start:m5_args_end]

    if mutation == 'fixed-object-ids':
        next_id_fn = (
            'pub fn next_compute_identity(&self) -> (u32, u64) {\n'
            '    let next = self.compute_ordinal.fetch_add(1, Ordering::AcqRel).wrapping_add(1);\n'
            '    (next, 0x0000_0001_0000_0001)\n'
            '}'
        )
    elif mutation == 'primary-before-sibling':
        release_fn = release_fn.replace('for primary_pass in [false, true]',
                                        'for primary_pass in [true, false]', 1)
    elif mutation == 'exit-ack-stops-first':
        ack_exit_fn = ack_exit_fn.replace(
            'matched |= queue.acknowledge_exit(cookie);',
            'if queue.acknowledge_exit(cookie) { return; }',
            1,
        )
    elif mutation == 'direct-allows-multi-barrier':
        direct_fn = direct_fn.replace(
            '            || (bound_count > 1 && packet.order.wait_through[1].is_some())\n', '', 1)
    elif mutation == 'direct-blocks-sibling':
        direct_fn = direct_fn.replace(
            '                } else if can_add_sibling && all_busy {\n'
            '                    return None;\n'
            '                } else {\n',
            '                } else {\n',
            1,
        )
    elif mutation == 'direct-locks-sibling-when-idle':
        direct_fn = direct_fn.replace(
            'let (configuring, active, last_published) = cell.load_hint();',
            'let queue = cell.lock()?; let (configuring, active, last_published) = (queue.configuring(), queue.active_len() as u32, queue.last_published());',
            1,
        )
    elif mutation == 'direct-skips-multi':
        direct_fn = direct_fn.replace(
            'bound_count > super::pairs::COMPUTE_CHANNELS_MAX',
            'bound_count > 1',
            1,
        )
    elif mutation == 'same-qid-pipelined':
        parent_pred = parent_pred.replace(
            'parent_dependency(board, parent_retired)',
            'parent_dependency(board, true)',
            1,
        )

    reg_methods = '\n'.join([
        channels_of_fn,
        next_sib_fn,
        source_fn,
        headroom_fn,
        wants_sib_fn,
        select_fn,
        release_fn,
    ])
    reg_methods = reg_methods.replace('pub(super) ', 'pub ')
    reg_methods = reg_methods.replace('super::job::Packet', 'Packet')
    reg_methods = reg_methods.replace('super::freelist::ComputeCapacity', 'ComputeCapacity')
    reg_methods = reg_methods.replace('super::pairs::', '')

    ack_code = ack_exit_fn.replace('pub(super) ', 'pub ')
    ack_code = re.sub(r'crate::g17::trace::teardown\([^;]*\);', '', ack_code)

    # Structural invariant booleans compiled into the host binary so every check
    # fails with a Rust assertion panic (exit 101) when violated.
    dedicated_pool_ok = (
        'PoolBorrow' not in freelist_src
        and 'PoolBorrow' not in queue_src
        and 'shared_pool' not in queue_src
        and 'PoolBacking::new(' in try_add_sib_fn
        and 'install_compute_pool(pool.pool_descriptor()?)' in try_add_sib_fn
        and ordered_ok(source_fn, 'if queue.pool_vacant() {', '} else if queue.pool_serves(vm) {',
                       '!queue.pool_fits(required)', '&mut warm')
        and 'let Some(SiblingSource::Warm(qid)) = self.sibling_source(owner, context, required) else' in bind_sib_fn
        and 'qid,\n                    channel_index,\n                    required,' in try_add_sib_fn
    )
    identity_wired_ok = (
        'packet.context.next_compute_identity()' in parent_pred
        and 'object_ids: m5_object_ids,' in m5_args_block
        and 'submit_sequence: if board.completion_inputs() {\n                u64::from(m5_ordinal)\n' in m5_args_block
        and 'completion_ordinal: crate::g17::fw::m5::compute::completion_ordinal(board, m5_ordinal),' in m5_args_block
        and 'peek_compute_ordinal' not in queue_src
    )
    cell_atomic_ok = (
        'active_len: core::sync::atomic::AtomicU32' in cell_struct
        and 'configuring: core::sync::atomic::AtomicBool' in cell_struct
        and 'last_published: core::sync::atomic::AtomicU64' in cell_struct
        and 'fn sync_stats(' in cell_impl
        and 'fn load_hint(' in cell_impl
        and 'self.cell.sync_stats(Some(queue));' in guard_drop
    )
    rollback_order_ok = ordered_ok(
        rollback_fn,
        'for primary_pass in [false, true]',
        'queue.is_primary() == primary_pass',
        '.rollback_unpublished(owner, context)?',
    ) and ordered_ok(
        backend_rollback_fn,
        'firmware.queues.rollback_unpublished_compute(',
        'drop(state);',
        'self.finish_retired_compute(&mut retired)',
    )
    grow_releases_siblings_ok = (
        ordered_ok(
            drain_sib_fn,
            'queue.owner() == Some(owner) && !queue.is_primary()',
            'if busy {',
            'return Err(EAGAIN);',
            '.release_owner()?',
            'entry.queue.take()',
        )
        and ordered_ok(
            grow_fn,
            'firmware.queues.drain_compute_siblings(self.owner, &mut retired_siblings)?;',
            'firmware.queues.compute_primary(self.owner)',
            'self.finish_retired_compute(&mut retired_siblings)?;',
        )
    )
    exit_propagates_ok = ordered_ok(
        pub_exit_fn,
        'if context.kill_published()',
        'queue.record_exit(cookie, now);',
        'if other.exit_candidate(context, age)',
        'other.record_exit(cookie, now);',
    )
    # Lock-free primary-first selection: compute_direct must NEVER call cell.lock() during
    # channel selection, must short-circuit to channel 0 when primary_active == 0 && !primary_configuring,
    # and must only lock target_cell once.
    select_section = direct_fn[:direct_fn.index('let target_cell =')]
    direct_guards_ok = (
        'cell.lock()' not in select_section
        and ordered_ok(
            direct_fn,
            'bound_count > super::pairs::COMPUTE_CHANNELS_MAX',
            'bound_count > 1 && packet.order.wait_through[1].is_some()',
            'let (primary_configuring, primary_active, _) = primary.load_hint();',
            'if primary_active == 0 && !primary_configuring',
            'let (configuring, active, last_published) = cell.load_hint();',
            'if can_add_sibling && all_busy',
            'self.outer_compute.lock()',
            'claim.remaining() != 0 && claim.priority() == usize::from(priority)',
            'queue.pool_fits(required)',
            'm5 && queue.outer_slots() != 1',
            'self.context.compute_binding_count() as usize != cached_count',
        )
        and 'registry.compute_cells(self.owner)' in remember_fn
    )
    readiness_and_outer_ok = (
        ordered_ok(
            readiness_fn,
            'bound_count <= super::pairs::COMPUTE_CHANNELS_MAX',
            'cells[1..].iter().flatten()',
            'queue.pool_fits(required)',
            'queue.claim()',
            'self.claim_compute_outer_direct(outer_slots)',
        )
        and '.map(|queue| queue.outer_slots())' in claim_outer_fn
        and '.max()' in claim_outer_fn
    )
    same_qid_serialized_ok = (
        'parent_dependency(board, parent_retired)' in parent_pred
        and 'same_context_independent' not in parent_pred
    )
    multi_witness_and_bound_ok = (
        'self.compute_primary(owner)' in bound_fn
        and '!queue.is_primary()' in bound_fn
        and 'queue.pool_fits(required)' in bound_fn
        and 'pending |= queue.spared_pending();' in witness_fn
    )

    return f'''
#![allow(dead_code, unused_variables, unused_mut)]
use std::cell::RefCell;
use std::collections::HashSet;
use std::sync::{{Arc, atomic::{{AtomicU32, AtomicUsize, Ordering}}}};

const EINVAL: i32 = -22;
const EAGAIN: i32 = -11;
const EIO: i32 = -5;
const EOVERFLOW: i32 = -75;
type Result<T = ()> = std::result::Result<T, i32>;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct ComputeCapacity(u32);

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
{source_enum}

{overlaps_fn}

struct Board {{
    compute_channels: u8,
}}
impl Board {{
    {channels_fn}
}}

{multi_fn}

struct Order {{
    wait_through: [Option<u64>; 2],
}}
struct Completion {{
    sources: Option<()>,
}}
impl Completion {{
    fn sources(&self) -> Option<&()> {{ self.sources.as_ref() }}
}}
struct Packet {{
    order: Order,
    completion: Completion,
}}

struct Context {{
    id: u32,
    vm_id: u32,
    compute_ordinal: AtomicU32,
}}
impl Context {{
    fn new(id: u32, vm_id: u32) -> Arc<Self> {{
        Arc::new(Self {{ id, vm_id, compute_ordinal: AtomicU32::new(0) }})
    }}
    fn vm(&self) -> u32 {{ self.vm_id }}
    {next_id_fn}
}}

struct Queue {{
    qid: u8,
    owner: Option<u64>,
    channel_index: u8,
    active_len: usize,
    replays: usize,
    configuring: bool,
    last_published: u64,
    sustained: bool,
    reusable: bool,
    needs_fresh_backing: bool,
    pool_vacant: bool,
    pool_vm: u32,
    pool_cap: u32,
    has_ring: bool,
    context: Option<Arc<Context>>,
    exit_cookie: Option<u64>,
    exit_acked: bool,
    unpublished: bool,
    released_log: Arc<RefCell<Vec<(u8, u8)>>>,
}}
impl Queue {{
    fn qid(&self) -> u8 {{ self.qid }}
    fn owner(&self) -> Option<u64> {{ self.owner }}
    fn channel_index(&self) -> u8 {{ self.channel_index }}
    fn is_primary(&self) -> bool {{ self.channel_index == 0 }}
    fn active_len(&self) -> usize {{ self.active_len }}
    fn has_replays(&self) -> bool {{ self.replays != 0 }}
    fn configuring(&self) -> bool {{ self.configuring }}
    fn last_published(&self) -> u64 {{ self.last_published }}
    fn sustained(&self) -> bool {{ self.sustained }}
    fn reusable(&self) -> bool {{ self.reusable }}
    fn needs_fresh_backing(&self) -> bool {{ self.needs_fresh_backing }}
    fn pool_vacant(&self) -> bool {{ self.pool_vacant }}
    fn pool_serves(&self, vm: u32) -> bool {{ self.pool_vm == vm }}
    fn pool_fits(&self, req: ComputeCapacity) -> bool {{ self.pool_cap >= req.0 }}
    fn has_ring(&self) -> bool {{ self.has_ring }}
    fn context(&self) -> Option<&Arc<Context>> {{ self.context.as_ref() }}
    fn rearm_open(&self, _owner: u64) -> bool {{ true }}
    fn released_at(&self) -> u64 {{ self.last_published }}
    fn vacancy_pending(&self) -> bool {{ false }}
    fn release_owner(&mut self) -> Result<bool> {{
        self.released_log.borrow_mut().push((self.qid, self.channel_index));
        self.owner = None;
        self.channel_index = 0;
        Ok(self.unpublished)
    }}
    fn acknowledge_exit(&mut self, cookie: u64) -> bool {{
        if self.exit_cookie == Some(cookie) {{
            self.exit_acked = true;
            true
        }} else {{
            false
        }}
    }}
}}

struct Slot(Option<RefCell<Queue>>);
impl Slot {{
    fn as_deref(&self) -> Option<std::cell::Ref<'_, Queue>> {{
        self.0.as_ref().map(|q| q.borrow())
    }}
    fn as_deref_mut(&mut self) -> Option<std::cell::RefMut<'_, Queue>> {{
        self.0.as_ref().map(|q| q.borrow_mut())
    }}
}}

struct Entry {{
    reservation: Reservation,
    queue: Slot,
}}
#[derive(Copy, Clone)]
struct Reservation {{ id: u8 }}

struct Qids {{ fit: bool, cancelled: Vec<u8> }}
impl Qids {{
    fn compute_fits(&self, _queues: usize) -> bool {{ self.fit }}
    fn cancel_unpublished(&mut self, id: u8) -> Result {{
        self.cancelled.push(id);
        Ok(())
    }}
}}

struct Admission {{ queues: u32 }}
impl Admission {{
    fn queues(&self) -> u32 {{ self.queues }}
    fn render_demand(&self, extra: usize) -> usize {{ self.queues as usize + extra }}
}}

struct Teardown {{ releasing_id: Option<u32>, render_acked: bool }}
impl Teardown {{
    fn releasing(&self, ctx: &Arc<Context>) -> bool {{ self.releasing_id == Some(ctx.id) }}
    fn acknowledge_render_kill(&mut self, _cookie: u64) -> bool {{
        self.render_acked = true;
        true
    }}
}}

static RENDER_KILLS: [AtomicUsize; 2] = [AtomicUsize::new(0), AtomicUsize::new(0)];
const RENDER_KILL_ACKNOWLEDGED: usize = 0;

struct Registry {{
    compute: Vec<Option<Entry>>,
    qids: Qids,
    admission: Admission,
    teardown: Teardown,
    build_room_noted: usize,
}}

impl Registry {{
    fn note_build_room(&mut self) {{ self.build_room_noted += 1; }}
    {reg_methods}
    {ack_code}
}}

fn make_entry(q: Queue) -> Option<Entry> {{
    let qid = q.qid;
    Some(Entry {{
        reservation: Reservation {{ id: qid }},
        queue: Slot(Some(RefCell::new(q))),
    }})
}}

fn test_atomic_compute_identity() {{
    let ctx = Context::new(1, 100);
    // Sequential check: 1-based ordinal with monotonic (predecessor_id << 32) | current_id.
    assert_eq!(ctx.next_compute_identity(), (1, (1u64 << 32) | 1));
    assert_eq!(ctx.next_compute_identity(), (2, (1u64 << 32) | 2));
    assert_eq!(ctx.next_compute_identity(), (3, (2u64 << 32) | 3));
    assert_eq!(ctx.next_compute_identity(), (4, (3u64 << 32) | 4));

    // Concurrent check across 4 threads simulating direct sibling compute kicks:
    // every kick must receive a unique ordinal and a unique non-zero current_id.
    let shared = Context::new(2, 200);
    let mut threads = Vec::new();
    for _ in 0..4 {{
        let c = shared.clone();
        threads.push(std::thread::spawn(move || {{
            let mut out = Vec::with_capacity(500);
            for _ in 0..500 {{
                out.push(c.next_compute_identity());
            }}
            out
        }}));
    }}
    let mut ordinals = HashSet::new();
    let mut current_ids = HashSet::new();
    for t in threads {{
        for (ord, ids) in t.join().unwrap() {{
            let pred = (ids >> 32) as u32;
            let cur = ids as u32;
            assert_ne!(pred, 0);
            assert_ne!(cur, 0);
            assert_eq!(cur, ord);
            assert!(ordinals.insert(ord), "duplicate shared ordinal {{}}", ord);
            assert!(current_ids.insert(cur), "duplicate object_ids current_id {{}}", cur);
        }}
    }}
    assert_eq!(ordinals.len(), 2000);
}}

fn main() {{
    // 1. Structural guards for dedicated pools, monotonic identities, lock-free ComputeCell hints,
    //    compute_direct primary-first routing, grow, rollback, and exit propagation.
    assert!({str(dedicated_pool_ok).lower()}, "siblings must own dedicated ComputePools with no PoolBorrow");
    assert!({str(identity_wired_ok).lower()}, "multi-channel M5 compute must wire atomic next_compute_identity to object_ids, submit_sequence, and completion_ordinal");
    assert!({str(cell_atomic_ok).lower()}, "ComputeCell must mirror active_len/configuring/last_published atomically via QueueGuard::drop");
    assert!({str(rollback_order_ok).lower()}, "rollback_unpublished_compute must roll back siblings before primary");
    assert!({str(grow_releases_siblings_ok).lower()}, "grow_compute must drain and release siblings before primary growth");
    assert!({str(exit_propagates_ok).lower()}, "publish_compute_exit must propagate kill cookie across all channels of context");
    assert!({str(direct_guards_ok).lower()}, "compute_direct must use lock-free primary-first routing without locking sibling cells, guard barriers, reuse outer_compute, and yield when siblings can be added");
    assert!({str(readiness_and_outer_ok).lower()}, "compute_readiness/claim_outer must validate all bound channels and take max outer_slots");
    assert!({str(same_qid_serialized_ok).lower()}, "multi-channel M5 Pro must keep COMPLETION_DEPENDENCY while a physical QID has an active parent");
    assert!({str(multi_witness_and_bound_ok).lower()}, "compute_bound and release_compute_witness must inspect all channels of owner");

    // 2. Per-context monotonic object_ids & aligned submit_sequence == completion_ordinal.
    test_atomic_compute_identity();

    // 3. multi_channel() board selection.
    assert!(multi_channel(&Board {{ compute_channels: 3 }}));
    assert!(multi_channel(&Board {{ compute_channels: 2 }}));
    assert!(!multi_channel(&Board {{ compute_channels: 1 }}));

    // 4. Lazy sibling admission + dedicated-pool headroom + round-robin selection + release order.
    let ctx = Context::new(1, 77);
    let req = ComputeCapacity(16);
    let log = Arc::new(RefCell::new(Vec::new()));
    let mut reg = Registry {{
        compute: vec![
            make_entry(Queue {{
                qid: 10, owner: Some(1), channel_index: 0, active_len: 1, replays: 0,
                configuring: false, last_published: 100, sustained: true, reusable: false,
                needs_fresh_backing: false, pool_vacant: false, pool_vm: 77, pool_cap: 16,
                has_ring: true, context: None, exit_cookie: Some(0xabc), exit_acked: false,
                unpublished: false, released_log: log.clone(),
            }}),
            make_entry(Queue {{
                qid: 11, owner: Some(1), channel_index: 1, active_len: 1, replays: 0,
                configuring: false, last_published: 101, sustained: false, reusable: false,
                needs_fresh_backing: false, pool_vacant: false, pool_vm: 77, pool_cap: 16,
                has_ring: true, context: None, exit_cookie: Some(0xabc), exit_acked: false,
                unpublished: true, released_log: log.clone(),
            }}),
        ],
        qids: Qids {{ fit: true, cancelled: Vec::new() }},
        admission: Admission {{ queues: 1 }},
        teardown: Teardown {{ releasing_id: None, render_acked: false }},
        build_room_noted: 0,
    }};
    assert_eq!(reg.compute_channels_of(1), (2, 0b11));
    assert_eq!(reg.next_sibling_index(1, 3), Some(2));
    assert!(reg.wants_compute_sibling(1, &ctx, 3, req));

    // When surplus QIDs are exhausted, a reusable queue must provide the sibling.
    reg.qids.fit = false;
    assert!(!reg.wants_compute_sibling(1, &ctx, 3, req));
    // An idle reusable queue still holding another VM's pool counts: it is given back and
    // re-armed for this VM (no lockout once earlier clients took every QID).
    reg.compute.push(make_entry(Queue {{
        qid: 13, owner: None, channel_index: 0, active_len: 0, replays: 0,
        configuring: false, last_published: 10, sustained: false, reusable: true,
        needs_fresh_backing: false, pool_vacant: false, pool_vm: 99, pool_cap: 16,
        has_ring: true, context: None, exit_cookie: None, exit_acked: false,
        unpublished: false, released_log: log.clone(),
    }}));
    assert!(reg.wants_compute_sibling(1, &ctx, 3, req));
    assert_eq!(reg.sibling_source(1, &ctx, req), Some(SiblingSource::Foreign(13)));
    // So does a vacant one, before it.
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().pool_vacant = true;
    assert_eq!(reg.sibling_source(1, &ctx, req), Some(SiblingSource::Vacant(13)));
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().pool_vacant = false;
    // A queue with an owner never does.
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().reusable = false;
    assert!(!reg.wants_compute_sibling(1, &ctx, 3, req));
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().reusable = true;
    // Same-VM fitting populated reusable queue DOES count, first.
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().pool_vm = 77;
    assert!(reg.wants_compute_sibling(1, &ctx, 3, req));
    assert_eq!(reg.sibling_source(1, &ctx, req), Some(SiblingSource::Warm(13)));
    // Undersized same-VM pool does NOT count.
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().pool_cap = 8;
    assert!(!reg.wants_compute_sibling(1, &ctx, 3, req));
    reg.compute.pop();
    reg.qids.fit = true;

    // If a bound channel has pending replays, refuse new siblings.
    reg.compute[1].as_mut().unwrap().queue.as_deref_mut().unwrap().replays = 1;
    assert!(!reg.wants_compute_sibling(1, &ctx, 3, req));
    reg.compute[1].as_mut().unwrap().queue.as_deref_mut().unwrap().replays = 0;

    // Add 3rd channel (index 2): cap reached, wants_compute_sibling becomes false.
    reg.compute.push(make_entry(Queue {{
        qid: 12, owner: Some(1), channel_index: 2, active_len: 0, replays: 0,
        configuring: false, last_published: 50, sustained: false, reusable: false,
        needs_fresh_backing: false, pool_vacant: false, pool_vm: 77, pool_cap: 16,
        has_ring: true, context: None, exit_cookie: Some(0xabc), exit_acked: false,
        unpublished: false, released_log: log.clone(),
    }}));
    assert_eq!(reg.next_sibling_index(1, 3), None);
    assert!(!reg.wants_compute_sibling(1, &ctx, 3, req));

    // Independent packet picks least-loaded idle sibling (qid 12).
    let indep = Packet {{ order: Order {{ wait_through: [None, None] }}, completion: Completion {{ sources: None }} }};
    assert_eq!(reg.select_compute_channel(1, &indep), Some(12));

    // When all 3 have equal active_len=1, oldest last_published (qid 12, lp=50) wins.
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().active_len = 1;
    assert_eq!(reg.select_compute_channel(1, &indep), Some(12));

    // Dependent packet when only ONE channel is active stays on that sole active channel.
    reg.compute[0].as_mut().unwrap().queue.as_deref_mut().unwrap().active_len = 0;
    reg.compute[2].as_mut().unwrap().queue.as_deref_mut().unwrap().active_len = 0;
    let dep = Packet {{ order: Order {{ wait_through: [None, Some(42)] }}, completion: Completion {{ sources: None }} }};
    assert_eq!(reg.select_compute_channel(1, &dep), Some(11));

    // 5. Multi-channel exit acknowledgment must mark ALL channels sharing the cookie.
    reg.acknowledge_compute_exit(0xabc);
    for slot in 0..3 {{
        assert!(reg.compute[slot].as_ref().unwrap().queue.as_deref().unwrap().exit_acked,
            "channel slot {{}} did not receive shared exit cookie ack", slot);
    }}
    assert!(!reg.teardown.render_acked);

    // 6. Sibling-before-primary release order.
    reg.release_compute(1).unwrap();
    let entries = log.borrow();
    assert_eq!(entries.len(), 3);
    assert_ne!(entries[0].1, 0, "first release must be a sibling");
    assert_ne!(entries[1].1, 0, "second release must be a sibling");
    assert_eq!(entries[2].1, 0, "primary must be released last");
    assert_eq!(reg.qids.cancelled, vec![11]);
    assert!(reg.compute[1].is_none());

    println!("PASS g17-m5-sibling-compute: all checks passed");
}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutation', choices=[
        'fixed-object-ids',
        'primary-before-sibling',
        'exit-ack-stops-first',
        'direct-allows-multi-barrier',
        'direct-blocks-sibling',
        'direct-locks-sibling-when-idle',
        'direct-skips-multi',
        'same-qid-pipelined',
    ])
    args = ap.parse_args()

    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-sib-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        cc = subprocess.run([rustc, '--edition=2021', '-O', str(rs), '-o', str(exe)],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            sys.stderr.write(cc.stderr)
            return cc.returncode
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
