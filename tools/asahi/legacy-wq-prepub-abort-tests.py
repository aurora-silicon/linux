#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for legacy AGX WorkQueue/QueueJob pre-publication abort ordering.

Extracts and compiles the production ordering state machine and queue abort
paths from:
  - drivers/gpu/drm/asahi/event.rs (`EventValue`)
  - drivers/gpu/drm/asahi/workqueue.rs (`MAX_JOB_SLOTS`, `can_submit_inline`,
    `WorkQueueOrder`, plus exact `Job`/`JobSubmission`/`WorkQueue` transitions)
  - drivers/gpu/drm/asahi/queue/mod.rs (`TvbShared`, `TvbJob`, `init_was_lost`,
    `QueueJob::commit`, `record_barrier_wait`, `barrier_dependency_lost`,
    `abort_subqueues`, and `QueueJob::run` pre-publication failure handling)
  - drivers/gpu/drm/asahi/queue/render.rs (`scene.rebind() || init_slot` return)

Verifies all scenarios for aurora-silicon/linux#2546 and kills each negative
control mutation with a Rust assertion panic (exit 101).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'


def extract_item(source, signature, attributes=False):
    """Extract a braced Rust item starting at `signature` through its matching `}`."""
    assert source.count(signature) == 1, f'expected 1 occurrence of {signature!r}, got {source.count(signature)}'
    start = source.index(signature)
    if attributes:
        while True:
            line = source.rfind('\n', 0, start - 1) + 1
            stripped = source[line:start].strip()
            if not (stripped.startswith('#[') or stripped.startswith('///') or stripped.startswith('//')):
                break
            start = line
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        ch = source[end]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        end += 1
    return source[start:end]


def extract_line(source, prefix):
    matches = [ln for ln in source.splitlines() if ln.strip().startswith(prefix)]
    assert len(matches) == 1, f'expected 1 line starting with {prefix!r}, got {len(matches)}'
    return matches[0]


def strip_ver_attrs(text):
    text = re.sub(r'#\[cfg\(CONFIG_[^)]+\)\]\s*pub\(crate\) fn (?:raw|current_noncoherent)\b[\s\S]*?\n    \}', '', text)
    text = re.sub(r'#\[cfg\(CONFIG_KUNIT\)\]\s*', '', text)
    return text


def build_harness(mutation=None):
    event_src = (ASAHI / 'event.rs').read_text()
    wq_src = (ASAHI / 'workqueue.rs').read_text()
    queue_src = (ASAHI / 'queue/mod.rs').read_text()
    render_src = (ASAHI / 'queue/render.rs').read_text()

    # Structural assertions against the real production source files before extraction.
    wq_job_drop = extract_item(wq_src, 'impl Drop for Job::ver {')
    wq_sub_drop = extract_item(wq_src, "impl<'a> Drop for JobSubmission::ver<'a> {")
    wq_sub_run = extract_item(wq_src, 'pub(crate) fn run(mut self, channel: &mut channel::PipeChannel::ver) {')
    wq_job_submit = extract_item(wq_src, "pub(crate) fn submit(&mut self) -> Result<JobSubmission::ver<'_>> {")
    wq_event_info = extract_item(wq_src, 'pub(crate) fn event_info(&self) -> Option<QueueEventInfo::ver> {')
    q_job_impl = extract_item(queue_src, 'impl QueueJob::ver {')
    q_run = extract_item(queue_src, 'fn run(job: &mut Self) -> Result<Option<dma_fence::Fence>> {')
    q_submit = extract_item(queue_src, "fn submit<'drm>(")
    q_commit = extract_item(q_job_impl, 'fn commit(&mut self) -> Result {')
    q_abort_subqueues = extract_item(q_job_impl, 'fn abort_subqueues(&mut self) {')
    q_barrier_lost = extract_item(q_job_impl, 'fn barrier_dependency_lost(&self) -> bool {')
    q_record_wait = extract_item(q_job_impl, 'fn record_barrier_wait(&mut self, queue_idx: usize, event_seq: u64) {')
    r_submit = extract_item(render_src, 'pub(super) fn submit_render(')

    # Extract compiled production items.
    ev_struct = extract_line(event_src, 'pub(crate) struct EventValue(')
    ev_impl = strip_ver_attrs(extract_item(event_src, 'impl EventValue {'))
    ev_pord = extract_item(event_src, 'impl PartialOrd for EventValue {')
    ev_ord = extract_item(event_src, 'impl Ord for EventValue {')

    max_slots = extract_line(wq_src, 'const MAX_JOB_SLOTS:')
    wq_can_inline = extract_item(wq_src, '\nfn can_submit_inline(').lstrip('\n')
    wq_order_struct = extract_item(wq_src, 'struct WorkQueueOrder {', attributes=True)
    wq_order_impl = extract_item(wq_src, 'impl WorkQueueOrder {')

    tvb_shared = extract_item(queue_src, 'struct TvbShared {')
    tvb_job = extract_item(queue_src, 'struct TvbJob {')
    tvb_job_impl = extract_item(queue_src, 'impl TvbJob {')
    init_was_lost = extract_item(queue_src, 'fn init_was_lost(')

    # Mirror the exact production control-flow flags from the real methods
    # using whitespace-insensitive regexes so rustfmt cannot break the harness.
    job_drop_uses_abort = (
        re.search(r'\binner\s*\.\s*order\s*\.\s*abort_committed\s*\(', wq_job_drop) is not None
        and '.sub(' not in wq_job_drop
    )
    sub_drop_uses_abort = (
        re.search(r'\binner\s*\.\s*order\s*\.\s*abort_committed\s*\(', wq_sub_drop) is not None
        and 'last_submitted =' not in wq_sub_drop
        and '.sub(' not in wq_sub_drop
    )
    submit_defers_last_submitted = (
        'last_submitted =' not in wq_job_submit
        and re.search(r'\.\s*publish\s*\(', wq_sub_run) is not None
    )
    event_info_uses_wait_target = (
        re.search(
            r'\binner\s*\.\s*order\s*\.\s*wait_target\s*\(\s*inner\s*\.\s*pending\s*\.\s*is_empty\s*\(\s*\)\s*\)',
            wq_event_info,
        )
        is not None
    )
    m_frag_take = re.search(
        r'let\s+mut\s+frag_job\s*=\s*job\s*\.\s*sj_frag\s*\.\s*as_mut\s*\(\s*\)\s*\.\s*and_then\s*\(',
        q_run,
    )
    m_vtx_take = re.search(
        r'let\s+mut\s+vtx_job\s*=\s*job\s*\.\s*sj_vtx\s*\.\s*as_mut\s*\(\s*\)\s*\.\s*and_then\s*\(',
        q_run,
    )
    m_comp_take = re.search(
        r'let\s+mut\s+comp_job\s*=\s*job\s*\.\s*sj_comp\s*\.\s*as_mut\s*\(\s*\)\s*\.\s*and_then\s*\(',
        q_run,
    )
    m_first_submit = re.search(r'\bwqjob\s*\.\s*submit\s*\(\s*\)\s*\?', q_run)
    run_takes_all_jobs_upfront = (
        m_frag_take is not None
        and m_vtx_take is not None
        and m_comp_take is not None
        and m_first_submit is not None
        and m_comp_take.end() < m_first_submit.start()
        and re.search(r'\bjob\s*\.\s*abort_subqueues\s*\(\s*\)\s*;', q_run) is not None
    )
    run_checks_barrier = re.search(r'if\s+job\s*\.\s*barrier_dependency_lost\s*\(\s*\)', q_run) is not None
    commit_uncommits_on_err = re.search(r'\bsj\s*\.\s*uncommit\s*\(\s*\)\s*;', q_commit) is not None

    m_new_scene = re.search(r'\bbuffer\s*\.\s*new_scene\s*\(', r_submit)
    m_rebind_latch = re.search(
        r'if\s+scene\s*\.\s*rebind\s*\(\s*\)\s*\{\s*\*tvb_init_pending\s*=\s*true\s*;\s*\}',
        r_submit,
    )
    m_arc_scene = re.search(r'Arc\s*::\s*new\s*\(\s*scene\s*,', r_submit)
    render_latches_rebind_early = (
        m_new_scene is not None
        and m_rebind_latch is not None
        and m_arc_scene is not None
        and m_new_scene.end() < m_rebind_latch.start() < m_arc_scene.start()
    )

    m_push = re.search(r'\bjob\s*\.\s*push\s*\(\s*\)\s*;', q_submit)
    m_clear_pending = re.search(r'self\s*\.\s*tvb_init_pending\s*=\s*false\s*;', q_submit)
    render_reports_rebind_init = (
        re.search(r'let\s+emitted_init\s*=\s*scene\s*\.\s*rebind\s*\(\s*\)\s*\|\|\s*init_slot\s*;', r_submit)
        is not None
        and re.search(r'\bOk\s*\(\s*emitted_init\s*\)', r_submit) is not None
        and re.search(r'let\s+init_slot\s*=\s*!carried_init\s*&&\s*self\s*\.\s*tvb_init_pending\s*;', q_submit)
        is not None
        and re.search(r'&mut\s+self\s*\.\s*tvb_init_pending', q_submit) is not None
        and re.search(r'carried_init\s*=\s*true\s*;', q_submit) is not None
        and m_push is not None
        and m_clear_pending is not None
        and m_push.end() < m_clear_pending.start()
        and re.search(r'take\s*\(\s*&mut\s+self\s*\.\s*tvb_init_pending', q_submit) is None
    )

    barriers_record_cross_ioctl_only = (
        re.search(r'if\s+\*index\s*==\s*0\s*\{\s*job\s*\.\s*record_barrier_wait\s*\(', q_submit) is not None
        and re.search(r'if\s+from_prev\s*\{\s*job\s*\.\s*record_barrier_wait\s*\(', q_submit) is not None
        and len(re.findall(r'\brecord_barrier_wait\s*\(', q_submit)) == 2
    )

    # Host-compiled production method adaptations.
    host_q_commit = q_commit.replace('mod_dev_dbg!(self.dev, "QueueJob {}: Committing\\n", self.id);', '')
    host_q_record_wait = q_record_wait
    host_q_barrier_lost = q_barrier_lost
    host_q_abort_subqueues = q_abort_subqueues

    # Apply targeted negative-control mutations directly to the extracted production code.
    mut_old_job_drop = False
    mut_old_sub_drop = False
    mut_partial_take = False
    mut_omit_rebind_tvb = False
    mut_late_rebind_latch = False
    mut_record_intra_ioctl_barrier = False

    if mutation == 'old-job-drop-rollback':
        mut_old_job_drop = True
    elif mutation == 'old-submission-drop-last-submitted':
        mut_old_sub_drop = True
    elif mutation == 'omit-submit-seq-advance':
        old = 'if self.submit_seq == cmd_seq {\n            self.submit_seq += command_count;\n            true\n        } else {'
        assert old in wq_order_impl, 'abort_committed body changed'
        wq_order_impl = wq_order_impl.replace(
            old,
            'if self.submit_seq == cmd_seq {\n            let _ = command_count;\n            true\n        } else {',
            1,
        )
    elif mutation == 'omit-barrier-invalidation':
        old = 'frag_lost || comp_lost'
        assert old in host_q_barrier_lost, 'barrier_dependency_lost body changed'
        host_q_barrier_lost = host_q_barrier_lost.replace(old, 'let _ = (frag_lost, comp_lost); false', 1)
    elif mutation == 'stale-event-info-tail':
        old = (
            '        if self.submit_seq < self.commit_seq {\n'
            '            Some((tail, self.event_seq))\n'
            '        } else if !pending_empty {\n'
            '            Some((self.last_submitted?, self.published_event_seq))\n'
            '        } else {\n'
            '            None\n'
            '        }'
        )
        assert old in wq_order_impl, 'wait_target body changed'
        wq_order_impl = wq_order_impl.replace(
            old,
            '        let _ = pending_empty;\n        Some((tail, self.event_seq))',
            1,
        )
    elif mutation == 'partial-take-in-run':
        mut_partial_take = True
        host_q_abort_subqueues = 'fn abort_subqueues(&mut self) {}'
    elif mutation == 'omit-rebind-tvb-init':
        mut_omit_rebind_tvb = True
    elif mutation == 'late-rebind-latch':
        mut_late_rebind_latch = True
    elif mutation == 'record-intra-ioctl-barrier':
        mut_record_intra_ioctl_barrier = True

    return f'''
#![allow(warnings)]
use core::cmp;
use core::sync::atomic::{{AtomicBool, AtomicU64, Ordering}};
use std::sync::{{Arc, Mutex, MutexGuard}};

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Error(i32);
pub type Result<T = ()> = core::result::Result<T, Error>;
const EINVAL: Error = Error(-22);
const ENOMEM: Error = Error(-12);
const EBUSY: Error = Error(-16);
const ECANCELED: Error = Error(-125);
const ENODEV: Error = Error(-19);

#[derive(Eq, PartialEq, Copy, Clone, Debug)]
#[repr(transparent)]
{ev_struct}
{ev_impl}
{ev_pord}
{ev_ord}

impl EventValue {{
    fn sub_mut_legacy_bug(&mut self, val: u32) {{
        self.0 = self.0.wrapping_sub(val * 0x100);
    }}
}}

{max_slots}
{wq_can_inline}
{wq_order_struct}
{wq_order_impl}

{tvb_shared}
{tvb_job}
{tvb_job_impl}
{init_was_lost}

const SQ_RENDER: usize = 0;
const SQ_COMPUTE: usize = 1;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct QueueEventInfo {{
    slot: u32,
    value: EventValue,
    cmd_seq: u64,
    event_seq: u64,
}}

#[derive(Copy, Clone, Debug)]
struct BarrierCmd {{
    wait_slot: u32,
    wait_value: EventValue,
}}

#[derive(Clone, Debug)]
struct MockCmd {{
    value: EventValue,
    barrier: Option<BarrierCmd>,
    init_buffer: bool,
    uses_tvb: bool,
}}

struct EventSlotPool {{
    stamps: Vec<EventValue>,
    fw_stamps: Vec<EventValue>,
    in_use: Vec<bool>,
    next_slot: usize,
}}

impl EventSlotPool {{
    fn new(n: usize) -> Self {{
        let mut stamps = Vec::with_capacity(n);
        let mut fw_stamps = Vec::with_capacity(n);
        for slot in 0..n {{
            stamps.push(EventValue::from_raw((slot as u32) << 24));
            fw_stamps.push(EventValue::from_raw(0));
        }}
        Self {{
            stamps,
            fw_stamps,
            in_use: vec![false; n],
            next_slot: 0,
        }}
    }}

    fn acquire(&mut self, preferred: Option<u32>) -> (u32, EventValue) {{
        if let Some(s) = preferred {{
            if !self.in_use[s as usize] {{
                self.in_use[s as usize] = true;
                return (s, self.stamps[s as usize]);
            }}
        }}
        for off in 0..self.in_use.len() {{
            let idx = (self.next_slot + off) % self.in_use.len();
            if !self.in_use[idx] {{
                self.in_use[idx] = true;
                self.next_slot = (idx + 1) % self.in_use.len();
                return (idx as u32, self.stamps[idx]);
            }}
        }}
        panic!("out of event slots");
    }}

    fn release(&mut self, slot: u32) {{
        assert!(self.in_use[slot as usize]);
        self.in_use[slot as usize] = false;
    }}
}}

struct MockWorkQueueInner {{
    name: &'static str,
    pool: Arc<Mutex<EventSlotPool>>,
    last_slot: Option<u32>,
    event_slot: Option<u32>,
    order: WorkQueueOrder,
    pending: Vec<MockCmd>,
    ring_size: usize,
    fail_next_submit: Option<Error>,
    fail_next_commit: Option<Error>,
    published_batches: Vec<(u32, Vec<MockCmd>)>,
}}

struct MockWorkQueue {{
    inner: Mutex<MockWorkQueueInner>,
}}

impl MockWorkQueue {{
    fn new(name: &'static str, pool: Arc<Mutex<EventSlotPool>>) -> Arc<Self> {{
        Arc::new(Self {{
            inner: Mutex::new(MockWorkQueueInner {{
                name,
                pool,
                last_slot: None,
                event_slot: None,
                order: WorkQueueOrder::default(),
                pending: Vec::new(),
                ring_size: 0x500,
                fail_next_submit: None,
                fail_next_commit: None,
                published_batches: Vec::new(),
            }}),
        }})
    }}

    fn event_info(&self) -> Option<QueueEventInfo> {{
        let inner = self.inner.lock().unwrap();
        let slot = inner.event_slot?;
        let (value, event_seq) = inner.order.wait_target(inner.pending.is_empty())?;
        Some(QueueEventInfo {{
            slot,
            value,
            cmd_seq: inner.order.commit_seq,
            event_seq,
        }})
    }}

    fn has_published(&self, event_seq: u64) -> bool {{
        self.inner.lock().unwrap().order.has_published(event_seq)
    }}

    fn new_job(self: &Arc<Self>) -> MockJob {{
        let mut inner = self.inner.lock().unwrap();
        if inner.event_slot.is_none() {{
            let pref = inner.last_slot;
            let (slot, cur) = inner.pool.lock().unwrap().acquire(pref);
            inner.last_slot = Some(slot);
            inner.event_slot = Some(slot);
            inner.order.attach_event(cur);
        }}
        let (start_value, cmd_seq, event_seq) = inner.order.new_job();
        let slot = inner.event_slot.unwrap();
        MockJob {{
            wq: self.clone(),
            event_info: QueueEventInfo {{
                slot,
                value: start_value,
                cmd_seq,
                event_seq,
            }},
            start_value,
            pending: Vec::new(),
            event_count: 0,
            committed: false,
            submitted: false,
        }}
    }}

    fn signal(&self) {{
        let mut inner = self.inner.lock().unwrap();
        let Some(slot) = inner.event_slot else {{ return; }};
        let value = inner.pool.lock().unwrap().stamps[slot as usize];
        if let Some(lc) = inner.order.last_completed {{
            assert!(value >= lc, "stamp rolled back on {{}}", inner.name);
        }}
        inner.order.last_completed = Some(value);
        let mut done = 0usize;
        for cmd in &inner.pending {{
            if cmd.value <= value {{
                done += 1;
            }} else {{
                break;
            }}
        }}
        inner.pending.drain(..done);
        if inner.pending.is_empty() && inner.order.pending_jobs == 0 {{
            inner.pool.lock().unwrap().release(slot);
            inner.event_slot = None;
            inner.order.clear_event();
        }}
    }}
}}

struct MockJob {{
    wq: Arc<MockWorkQueue>,
    event_info: QueueEventInfo,
    start_value: EventValue,
    pending: Vec<MockCmd>,
    event_count: usize,
    committed: bool,
    submitted: bool,
}}

impl MockJob {{
    fn event_info(&self) -> QueueEventInfo {{
        let mut info = self.event_info;
        info.cmd_seq += self.pending.len() as u64;
        info.event_seq += self.event_count as u64;
        info
    }}

    fn next_seq(&mut self) {{
        self.event_count += 1;
        self.event_info.value.increment();
    }}

    fn add(&mut self, barrier: Option<BarrierCmd>, init_buffer: bool, uses_tvb: bool) {{
        assert!(!self.committed);
        let value = self.event_info.value.next();
        self.pending.push(MockCmd {{ value, barrier, init_buffer, uses_tvb }});
    }}

    fn commit(&mut self) -> Result {{
        if self.committed || self.pending.is_empty() {{
            return Err(EINVAL);
        }}
        let mut inner = self.wq.inner.lock().unwrap();
        if let Some(err) = inner.fail_next_commit.take() {{
            return Err(err);
        }}
        if inner.order.commit(
            self.start_value,
            self.event_info.value,
            self.pending.len() as u64,
            self.event_count as u64,
        ).is_err() {{
            return Err(EINVAL);
        }}
        self.committed = true;
        Ok(())
    }}

    fn uncommit(&mut self) {{
        if !self.committed || self.submitted {{
            return;
        }}
        let mut inner = self.wq.inner.lock().unwrap();
        if inner.order.uncommit(
            self.start_value,
            self.event_info.value,
            self.event_info.cmd_seq,
            self.pending.len() as u64,
            self.event_info.event_seq,
            self.event_count as u64,
        ) {{
            self.committed = false;
        }}
    }}

    fn can_submit_inline(&self) -> bool {{
        let inner = self.wq.inner.lock().unwrap();
        let free_space = inner.ring_size - inner.pending.len() - 1;
        can_submit_inline(
            inner.order.submit_seq,
            self.event_info.cmd_seq,
            inner.pending.len(),
            inner.order.free_slots(),
            self.event_count,
            free_space,
            self.pending.len(),
        )
    }}

    fn submit(&mut self) -> Result<MockJobSubmission<'_>> {{
        if !self.committed || self.submitted || self.pending.is_empty() {{
            return Err(EINVAL);
        }}
        let mut inner = self.wq.inner.lock().unwrap();
        if inner.order.submit_seq != self.event_info.cmd_seq {{
            return Err(EINVAL);
        }}
        if inner.order.commit_seq < self.event_info.cmd_seq + self.pending.len() as u64 {{
            return Err(EINVAL);
        }}
        if let Some(err) = inner.fail_next_submit.take() {{
            return Err(err);
        }}
        let command_count = self.pending.len();
        let free_space = inner.ring_size - inner.pending.len() - 1;
        if free_space <= command_count {{
            return Err(EBUSY);
        }}
        for cmd in self.pending.drain(..) {{
            inner.pending.push(cmd);
        }}
        self.submitted = true;
        Ok(MockJobSubmission {{
            inner: Some(inner),
            cmd_seq: self.event_info.cmd_seq,
            command_count,
            event_count: self.event_count,
            end_value: self.event_info.value,
            end_event_seq: self.event_info.event_seq + self.event_count as u64,
        }})
    }}
}}

impl Drop for MockJob {{
    fn drop(&mut self) {{
        let mut inner = self.wq.inner.lock().unwrap_or_else(|e| e.into_inner());
        if self.committed && !self.submitted {{
            if {str(mut_old_job_drop).lower()} {{
                // Mutant: legacy tail rollback without advancing submit_seq.
                if let Some(v) = inner.order.tail_value.as_mut() {{
                    v.sub_mut_legacy_bug(self.event_count as u32);
                }}
                inner.order.commit_seq -= self.pending.len() as u64;
                inner.order.event_seq -= self.event_count as u64;
            }} else {{
                inner.order.abort_committed(self.event_info.cmd_seq, self.pending.len() as u64);
            }}
        }}
        let pending_empty = inner.pending.is_empty();
        if inner.order.release_job(pending_empty) {{
            if let Some(slot) = inner.event_slot.take() {{
                inner.pool.lock().unwrap().release(slot);
            }}
        }}
    }}
}}

struct MockJobSubmission<'a> {{
    inner: Option<MutexGuard<'a, MockWorkQueueInner>>,
    cmd_seq: u64,
    command_count: usize,
    event_count: usize,
    end_value: EventValue,
    end_event_seq: u64,
}}

impl<'a> MockJobSubmission<'a> {{
    fn run(mut self) {{
        let mut inner = self.inner.take().unwrap();
        let command_count = self.command_count;
        let end_value = self.end_value;
        let end_event_seq = self.end_event_seq;
        core::mem::forget(self);

        let slot = inner.event_slot.expect("missing event");
        let start = inner.pending.len() - command_count;
        let batch = inner.pending[start..].to_vec();
        inner.published_batches.push((slot, batch));
        inner.order.publish(command_count as u64, end_value, end_event_seq);
    }}
}}

impl<'a> Drop for MockJobSubmission<'a> {{
    fn drop(&mut self) {{
        let inner = self.inner.as_mut().unwrap();
        let new_len = inner.pending.len() - self.command_count;
        inner.pending.truncate(new_len);
        if {str(mut_old_sub_drop).lower()} {{
            // Mutant: legacy JobSubmission::drop rolling back tail and setting last_submitted = tail.
            if let Some(v) = inner.order.tail_value.as_mut() {{
                v.sub_mut_legacy_bug(self.event_count as u32);
            }}
            let val = inner.order.tail_value;
            inner.order.commit_seq -= self.command_count as u64;
            inner.order.event_seq -= self.event_count as u64;
            inner.order.last_submitted = val;
        }} else {{
            inner.order.abort_committed(self.cmd_seq, self.command_count as u64);
        }}
    }}
}}

struct SubQueueJob {{
    wq: Arc<MockWorkQueue>,
    job: Option<MockJob>,
}}

impl SubQueueJob {{
    fn new(wq: Arc<MockWorkQueue>) -> Self {{ Self {{ wq, job: None }} }}
    fn get(&mut self) -> &mut MockJob {{
        if self.job.is_none() {{
            self.job = Some(self.wq.new_job());
        }}
        self.job.as_mut().unwrap()
    }}
    fn commit(&mut self) -> Result {{
        match self.job.as_mut() {{ Some(j) => j.commit(), None => Ok(()) }}
    }}
    fn uncommit(&mut self) {{
        if let Some(j) = self.job.as_mut() {{ j.uncommit(); }}
    }}
    fn can_submit_inline(&self) -> bool {{
        self.job.as_ref().map_or(true, |j| j.can_submit_inline())
    }}
}}

struct MockBufferInner {{
    has_token: bool,
    active_scenes: usize,
    evicted: bool,
}}

#[derive(Clone)]
struct MockBuffer {{
    inner: Arc<Mutex<MockBufferInner>>,
}}

struct MockScene {{
    buf: MockBuffer,
}}

impl Drop for MockScene {{
    fn drop(&mut self) {{
        let mut inner = self.buf.inner.lock().unwrap();
        inner.active_scenes -= 1;
    }}
}}

impl MockBuffer {{
    fn new() -> Self {{
        Self {{
            inner: Arc::new(Mutex::new(MockBufferInner {{
                has_token: false,
                active_scenes: 0,
                evicted: false,
            }})),
        }}
    }}

    /// Mirrors `Buffer::new_scene()` + `SlotAllocator`: records `last_token`
    /// when binding a fresh slot while `active_scenes == 0`, so dropping the
    /// scene after an error leaves `has_token = true` and hides `rebind()` from
    /// the next render unless the caller latched `tvb_init_pending`.
    fn new_scene(&self) -> (MockScene, bool) {{
        let mut inner = self.inner.lock().unwrap();
        let mut rebind = false;
        if inner.active_scenes == 0 && (!inner.has_token || inner.evicted) {{
            rebind = true;
            inner.has_token = true;
            inner.evicted = false;
        }}
        inner.active_scenes += 1;
        (MockScene {{ buf: self.clone() }}, rebind)
    }}

    fn evict(&self) {{
        let mut inner = self.inner.lock().unwrap();
        assert_eq!(inner.active_scenes, 0, "cannot evict active buffer slot");
        inner.evicted = true;
    }}

    fn active_scenes(&self) -> usize {{
        self.inner.lock().unwrap().active_scenes
    }}
}}

struct QueueJob {{
    id: u64,
    sj_vtx: Option<SubQueueJob>,
    sj_frag: Option<SubQueueJob>,
    sj_comp: Option<SubQueueJob>,
    inline_result: i32,
    tvb: Option<Arc<TvbJob>>,
    wait_frag_seq: Option<u64>,
    wait_comp_seq: Option<u64>,
    fail_start_op: Option<Error>,
    scenes: Vec<MockScene>,
}}

impl QueueJob {{
    {host_q_commit}
    {host_q_record_wait}
    {host_q_barrier_lost}
    {host_q_abort_subqueues}

    fn can_submit_inline(&self) -> bool {{
        self.sj_vtx.as_ref().map_or(true, |j| j.can_submit_inline())
            && self.sj_frag.as_ref().map_or(true, |j| j.can_submit_inline())
            && self.sj_comp.as_ref().map_or(true, |j| j.can_submit_inline())
    }}

    fn run(&mut self) -> Result {{
        if self.inline_result != 0 {{
            return if self.inline_result == 1 {{ Ok(()) }} else {{ Err(Error(self.inline_result)) }};
        }}
        if self.tvb.as_ref().is_some_and(|tvb| tvb.init_lost()) {{
            self.abort_subqueues();
            return Err(ECANCELED);
        }}
        if self.barrier_dependency_lost() {{
            self.abort_subqueues();
            if let Some(tvb) = self.tvb.as_ref() {{
                tvb.note_run_failed();
            }}
            return Err(ECANCELED);
        }}

        let ret: Result = (|| {{
            if let Some(err) = self.fail_start_op.take() {{
                return Err(err);
            }}
            if {str(mut_partial_take).lower()} {{
                // Mutant: Bug 3 legacy partial take() per subqueue.
                let mut frag_job = None;
                let mut frag_sub = None;
                if let Some(sj) = self.sj_frag.as_mut() {{
                    frag_job = sj.job.take();
                    if let Some(wqjob) = frag_job.as_mut() {{
                        frag_sub = Some(wqjob.submit()?);
                    }}
                }}
                let mut vtx_job = None;
                let mut vtx_sub = None;
                if let Some(sj) = self.sj_vtx.as_mut() {{
                    vtx_job = sj.job.take();
                    if let Some(wqjob) = vtx_job.as_mut() {{
                        vtx_sub = Some(wqjob.submit()?);
                    }}
                }}
                let mut comp_job = None;
                let mut comp_sub = None;
                if let Some(sj) = self.sj_comp.as_mut() {{
                    comp_job = sj.job.take();
                    if let Some(wqjob) = comp_job.as_mut() {{
                        comp_sub = Some(wqjob.submit()?);
                    }}
                }}
                if let Some(tvb) = self.tvb.as_ref() {{
                    tvb.published.store(true, Ordering::Release);
                }}
                frag_sub.map(MockJobSubmission::run);
                vtx_sub.map(MockJobSubmission::run);
                comp_sub.map(MockJobSubmission::run);
                drop(comp_job);
                drop(vtx_job);
                drop(frag_job);
                Ok(())
            }} else {{
                let mut frag_job = self.sj_frag.as_mut().and_then(|sj| sj.job.take());
                let mut vtx_job = self.sj_vtx.as_mut().and_then(|sj| sj.job.take());
                let mut comp_job = self.sj_comp.as_mut().and_then(|sj| sj.job.take());

                let frag_sub = if let Some(wqjob) = frag_job.as_mut() {{ Some(wqjob.submit()?) }} else {{ None }};
                let vtx_sub = if let Some(wqjob) = vtx_job.as_mut() {{ Some(wqjob.submit()?) }} else {{ None }};
                let comp_sub = if let Some(wqjob) = comp_job.as_mut() {{ Some(wqjob.submit()?) }} else {{ None }};

                if let Some(tvb) = self.tvb.as_ref() {{
                    tvb.published.store(true, Ordering::Release);
                }}
                frag_sub.map(MockJobSubmission::run);
                vtx_sub.map(MockJobSubmission::run);
                comp_sub.map(MockJobSubmission::run);
                drop(comp_job);
                drop(vtx_job);
                drop(frag_job);
                Ok(())
            }}
        }})();

        if ret.is_err() {{
            self.abort_subqueues();
            if let Some(tvb) = self.tvb.as_ref() {{
                tvb.note_run_failed();
            }}
        }}
        ret
    }}
}}

/// Simulated legacy G13/G14 firmware execution engine.
/// Executes published commands from vtx, frag, and comp queues in round-robin,
/// checking barriers (`fw_stamps[wait_slot] >= wait_value`), monotonic stamp
/// progression, and TVB slot initialization before any render tiles.
/// Panics on barrier deadlock or uninitialized TVB use!
struct FirmwareSim {{
    pool: Arc<Mutex<EventSlotPool>>,
    q_vtx: Arc<MockWorkQueue>,
    q_frag: Arc<MockWorkQueue>,
    q_comp: Arc<MockWorkQueue>,
    tvb_slot_initialized: bool,
}}

impl FirmwareSim {{
    fn new(
        pool: Arc<Mutex<EventSlotPool>>,
        q_vtx: Arc<MockWorkQueue>,
        q_frag: Arc<MockWorkQueue>,
        q_comp: Arc<MockWorkQueue>,
    ) -> Self {{
        Self {{
            pool,
            q_vtx,
            q_frag,
            q_comp,
            tvb_slot_initialized: false,
        }}
    }}

    fn drain_and_verify(&mut self) -> usize {{
        let mut streams: Vec<(Arc<MockWorkQueue>, Vec<(u32, MockCmd)>)> = Vec::new();
        for wq in [&self.q_vtx, &self.q_frag, &self.q_comp] {{
            let mut flat = Vec::new();
            let mut inner = wq.inner.lock().unwrap();
            for (slot, cmds) in inner.published_batches.drain(..) {{
                for c in cmds {{
                    flat.push((slot, c));
                }}
            }}
            streams.push((wq.clone(), flat));
        }}

        let mut executed = 0usize;
        loop {{
            let mut progressed = false;
            for (wq, cmds) in streams.iter_mut() {{
                if cmds.is_empty() {{
                    continue;
                }}
                let (slot, cmd) = &cmds[0];
                if let Some(b) = cmd.barrier {{
                    let cur = self.pool.lock().unwrap().fw_stamps[b.wait_slot as usize];
                    if cur < b.wait_value {{
                        continue; // Barrier waiting on another stream (or deadlocked!)
                    }}
                }}
                let (slot, cmd) = cmds.remove(0);
                if cmd.init_buffer {{
                    self.tvb_slot_initialized = true;
                }}
                if cmd.uses_tvb {{
                    assert!(
                        self.tvb_slot_initialized,
                        "FIRMWARE FAULT: render executed on uninitialized TVB buffer slot!"
                    );
                }}
                // Only Run* commands (not standalone barriers/init) retire their stamp,
                // or equivalently every command advances up to its stamp_value.
                if cmd.barrier.is_none() && !cmd.init_buffer {{
                    let mut p = self.pool.lock().unwrap();
                    let prev_fw = p.fw_stamps[slot as usize];
                    let prev_sh = p.stamps[slot as usize];
                    if prev_fw != EventValue::from_raw(0) {{
                        assert!(cmd.value > prev_fw, "non-monotonic fw_stamp {{:?}} <= {{:?}}", cmd.value, prev_fw);
                    }}
                    assert!(cmd.value > prev_sh, "non-monotonic stamp {{:?}} <= {{:?}}", cmd.value, prev_sh);
                    p.fw_stamps[slot as usize] = cmd.value;
                    p.stamps[slot as usize] = cmd.value;
                    drop(p);
                    wq.signal();
                }}
                executed += 1;
                progressed = true;
            }}
            if !progressed {{
                let stuck: Vec<_> = streams.iter().filter(|(_, c)| !c.is_empty()).collect();
                assert!(
                    stuck.is_empty(),
                    "FIRMWARE BARRIER DEADLOCK: {{}} workqueue stream(s) stuck waiting on unwritten stamp: {{:?}}",
                    stuck.len(),
                    stuck.iter().map(|(q, c)| (q.inner.lock().unwrap().name, &c[0])).collect::<Vec<_>>()
                );
                break;
            }}
        }}
        executed
    }}
}}

#[derive(Copy, Clone, Debug)]
enum IoctlCmd {{
    Render {{
        vdm_barrier: Option<usize>,
        frag_barrier_only: bool,
        cdm_barrier: Option<usize>,
        fail_after_new_scene: Option<Error>,
    }},
    Compute {{
        vdm_barrier: Option<usize>,
        cdm_barrier: Option<usize>,
    }},
}}

struct MockQueue {{
    pool: Arc<Mutex<EventSlotPool>>,
    q_vtx: Arc<MockWorkQueue>,
    q_frag: Arc<MockWorkQueue>,
    q_comp: Arc<MockWorkQueue>,
    buf: MockBuffer,
    tvb_shared: Arc<TvbShared>,
    tvb_seq: u64,
    tvb_init_seq: u64,
    tvb_init_pending: bool,
    next_id: u64,
}}

impl MockQueue {{
    fn new() -> Self {{
        let pool = Arc::new(Mutex::new(EventSlotPool::new(16)));
        Self {{
            q_vtx: MockWorkQueue::new("vtx", pool.clone()),
            q_frag: MockWorkQueue::new("frag", pool.clone()),
            q_comp: MockWorkQueue::new("comp", pool.clone()),
            buf: MockBuffer::new(),
            pool,
            tvb_shared: Arc::new(TvbShared {{ lost_init: AtomicU64::new(0) }}),
            tvb_seq: 0,
            tvb_init_seq: 0,
            tvb_init_pending: false,
            next_id: 1,
        }}
    }}

    fn sim(&self) -> FirmwareSim {{
        FirmwareSim::new(self.pool.clone(), self.q_vtx.clone(), self.q_frag.clone(), self.q_comp.clone())
    }}

    fn submit_render_step(
        buf: &MockBuffer,
        job: &mut QueueJob,
        init_slot: bool,
        tvb_init_pending: &mut bool,
        fail_after_new_scene: Option<Error>,
    ) -> Result<bool> {{
        let (scene, rebind) = buf.new_scene();
        if !{str(mut_late_rebind_latch).lower()}
            && !{str(mut_omit_rebind_tvb).lower()}
            && rebind
        {{
            *tvb_init_pending = true;
        }}
        if let Some(err) = fail_after_new_scene {{
            drop(scene);
            return Err(err);
        }}

        let ev_vtx = job.sj_vtx.as_mut().unwrap().get().event_info();
        let frag_job = job.sj_frag.as_mut().unwrap().get();
        // Internal frag barrier waiting on own vertex stamp.
        frag_job.add(Some(BarrierCmd {{ wait_slot: ev_vtx.slot, wait_value: ev_vtx.value.next() }}), false, false);
        frag_job.add(None, false, true);

        let emitted_init = rebind || init_slot;
        let vtx_job = job.sj_vtx.as_mut().unwrap().get();
        if emitted_init {{
            vtx_job.add(None, true, false);
        }}
        vtx_job.add(None, false, true);

        job.sj_vtx.as_mut().unwrap().get().next_seq();
        job.sj_frag.as_mut().unwrap().get().next_seq();
        job.scenes.push(scene);

        Ok(if {str(mut_omit_rebind_tvb).lower()} {{ init_slot }} else {{ emitted_init }})
    }}

    fn submit_ioctl(&mut self, cmds: &[IoctlCmd]) -> Result<QueueJob> {{
        let nr_render = cmds.iter().filter(|c| matches!(c, IoctlCmd::Render {{ .. }})).count();
        if nr_render > 0
            && init_was_lost(self.tvb_shared.lost_init.load(Ordering::Acquire), self.tvb_init_seq)
        {{
            self.tvb_init_pending = true;
        }}

        let mut events_render = vec![self.q_frag.event_info()];
        let mut events_comp = vec![self.q_comp.event_info()];
        let id = self.next_id;
        self.next_id += 1;

        let mut job = QueueJob {{
            id,
            sj_vtx: Some(SubQueueJob::new(self.q_vtx.clone())),
            sj_frag: Some(SubQueueJob::new(self.q_frag.clone())),
            sj_comp: Some(SubQueueJob::new(self.q_comp.clone())),
            inline_result: 0,
            tvb: None,
            wait_frag_seq: None,
            wait_comp_seq: None,
            fail_start_op: None,
            scenes: Vec::new(),
        }};

        let mut carried_init = false;
        for &cmd in cmds {{
            match cmd {{
                IoctlCmd::Render {{
                    vdm_barrier,
                    frag_barrier_only,
                    cdm_barrier,
                    fail_after_new_scene,
                }} => {{
                    if let Some(idx) = cdm_barrier {{
                        if let Some(ev) = *events_comp.get(idx).ok_or(EINVAL)? {{
                            job.sj_vtx.as_mut().unwrap().get().add(
                                Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                                false,
                                false,
                            );
                            if idx == 0 || {str(mut_record_intra_ioctl_barrier).lower()} {{
                                job.record_barrier_wait(SQ_COMPUTE, ev.event_seq);
                            }}
                        }}
                    }}
                    if let Some(idx) = vdm_barrier {{
                        if let Some(ev) = *events_render.get(idx).ok_or(EINVAL)? {{
                            let target = if frag_barrier_only {{
                                job.sj_frag.as_mut().unwrap().get()
                            }} else {{
                                job.sj_vtx.as_mut().unwrap().get()
                            }};
                            target.add(
                                Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                                false,
                                false,
                            );
                            if idx == 0 || {str(mut_record_intra_ioctl_barrier).lower()} {{
                                job.record_barrier_wait(SQ_RENDER, ev.event_seq);
                            }}
                        }}
                    }}

                    let init_slot = !carried_init && self.tvb_init_pending;
                    if Self::submit_render_step(
                        &self.buf,
                        &mut job,
                        init_slot,
                        &mut self.tvb_init_pending,
                        fail_after_new_scene,
                    )? {{
                        carried_init = true;
                        if {str(mut_late_rebind_latch).lower()} {{
                            self.tvb_init_pending = true;
                        }}
                    }}
                    events_render.push(Some(
                        job.sj_frag.as_ref().unwrap().job.as_ref().unwrap().event_info(),
                    ));
                }}
                IoctlCmd::Compute {{ vdm_barrier, cdm_barrier }} => {{
                    if let Some(idx) = vdm_barrier {{
                        if let Some(ev) = *events_render.get(idx).ok_or(EINVAL)? {{
                            job.sj_comp.as_mut().unwrap().get().add(
                                Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                                false,
                                false,
                            );
                            if idx == 0 || {str(mut_record_intra_ioctl_barrier).lower()} {{
                                job.record_barrier_wait(SQ_RENDER, ev.event_seq);
                            }}
                        }}
                    }}
                    if let Some(idx) = cdm_barrier {{
                        if let Some(ev) = *events_comp.get(idx).ok_or(EINVAL)? {{
                            job.sj_comp.as_mut().unwrap().get().add(
                                Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                                false,
                                false,
                            );
                            if idx == 0 || {str(mut_record_intra_ioctl_barrier).lower()} {{
                                job.record_barrier_wait(SQ_COMPUTE, ev.event_seq);
                            }}
                        }}
                    }}
                    let c = job.sj_comp.as_mut().unwrap().get();
                    c.add(None, false, false);
                    c.next_seq();
                    events_comp.push(Some(
                        job.sj_comp.as_ref().unwrap().job.as_ref().unwrap().event_info(),
                    ));
                }}
            }}
        }}

        let tvb = if nr_render > 0 {{
            let seq = self.tvb_seq + 1;
            let tvb = Arc::new(TvbJob {{
                shared: self.tvb_shared.clone(),
                seq,
                init_seq: if carried_init {{ seq }} else {{ self.tvb_init_seq }},
                published: AtomicBool::new(false),
            }});
            job.tvb = Some(tvb.clone());
            Some(tvb)
        }} else {{
            None
        }};

        self.finish_commit(&mut job, tvb)?;
        Ok(job)
    }}

    fn begin_render(
        &mut self,
        vdm_barrier: bool,
        frag_barrier_only: bool,
        cdm_barrier: bool,
        rebind: bool,
    ) -> (QueueJob, Option<Arc<TvbJob>>) {{
        if init_was_lost(self.tvb_shared.lost_init.load(Ordering::Acquire), self.tvb_init_seq) {{
            self.tvb_init_pending = true;
        }}
        let ev_render = self.q_frag.event_info();
        let ev_comp = self.q_comp.event_info();
        let id = self.next_id;
        self.next_id += 1;

        let mut job = QueueJob {{
            id,
            sj_vtx: Some(SubQueueJob::new(self.q_vtx.clone())),
            sj_frag: Some(SubQueueJob::new(self.q_frag.clone())),
            sj_comp: Some(SubQueueJob::new(self.q_comp.clone())),
            inline_result: 0,
            tvb: None,
            wait_frag_seq: None,
            wait_comp_seq: None,
            fail_start_op: None,
            scenes: Vec::new(),
        }};

        if cdm_barrier {{
            if let Some(ev) = ev_comp {{
                job.sj_vtx.as_mut().unwrap().get().add(
                    Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                    false,
                    false,
                );
                job.record_barrier_wait(SQ_COMPUTE, ev.event_seq);
            }}
        }}
        if vdm_barrier {{
            if let Some(ev) = ev_render {{
                let target = if frag_barrier_only {{
                    job.sj_frag.as_mut().unwrap().get()
                }} else {{
                    job.sj_vtx.as_mut().unwrap().get()
                }};
                target.add(Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}), false, false);
                job.record_barrier_wait(SQ_RENDER, ev.event_seq);
            }}
        }}

        let ev_vtx = job.sj_vtx.as_mut().unwrap().get().event_info();
        let frag_job = job.sj_frag.as_mut().unwrap().get();
        // Internal frag barrier waiting on own vertex stamp.
        frag_job.add(Some(BarrierCmd {{ wait_slot: ev_vtx.slot, wait_value: ev_vtx.value.next() }}), false, false);
        frag_job.add(None, false, true);

        // Match production queue/mod.rs: never clear `self.tvb_init_pending` here;
        // it is only cleared in `finish_commit()` after commit/push succeeds.
        let init_slot = self.tvb_init_pending;
        if rebind && !{str(mut_omit_rebind_tvb).lower()} {{
            self.tvb_init_pending = true;
        }}
        let emitted_init = rebind || init_slot;
        let vtx_job = job.sj_vtx.as_mut().unwrap().get();
        if emitted_init {{
            vtx_job.add(None, true, false);
        }}
        vtx_job.add(None, false, true);

        job.sj_vtx.as_mut().unwrap().get().next_seq();
        job.sj_frag.as_mut().unwrap().get().next_seq();

        let carried_init = if {str(mut_omit_rebind_tvb).lower()} {{ init_slot }} else {{ emitted_init }};
        let seq = self.tvb_seq + 1;
        let tvb = Arc::new(TvbJob {{
            shared: self.tvb_shared.clone(),
            seq,
            init_seq: if carried_init {{ seq }} else {{ self.tvb_init_seq }},
            published: AtomicBool::new(false),
        }});
        job.tvb = Some(tvb.clone());
        (job, Some(tvb))
    }}

    fn finish_commit(&mut self, job: &mut QueueJob, tvb: Option<Arc<TvbJob>>) -> Result {{
        job.commit()?;
        if let Some(tvb) = tvb {{
            self.tvb_seq = tvb.seq;
            if tvb.carries_init() {{
                self.tvb_init_pending = false;
                self.tvb_init_seq = tvb.seq;
            }}
        }}
        Ok(())
    }}

    fn build_and_commit_render(
        &mut self,
        vdm_barrier: bool,
        frag_barrier_only: bool,
        cdm_barrier: bool,
        rebind: bool,
    ) -> Result<QueueJob> {{
        let (mut job, tvb) = self.begin_render(vdm_barrier, frag_barrier_only, cdm_barrier, rebind);
        self.finish_commit(&mut job, tvb)?;
        Ok(job)
    }}

    fn build_and_commit_compute(&mut self, vdm_barrier: bool, cdm_barrier: bool) -> Result<QueueJob> {{
        let ev_render = self.q_frag.event_info();
        let ev_comp = self.q_comp.event_info();
        let id = self.next_id;
        self.next_id += 1;
        let mut job = QueueJob {{
            id,
            sj_vtx: Some(SubQueueJob::new(self.q_vtx.clone())),
            sj_frag: Some(SubQueueJob::new(self.q_frag.clone())),
            sj_comp: Some(SubQueueJob::new(self.q_comp.clone())),
            inline_result: 0,
            tvb: None,
            wait_frag_seq: None,
            wait_comp_seq: None,
            fail_start_op: None,
            scenes: Vec::new(),
        }};
        if vdm_barrier {{
            if let Some(ev) = ev_render {{
                job.sj_comp.as_mut().unwrap().get().add(
                    Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                    false,
                    false,
                );
                job.record_barrier_wait(SQ_RENDER, ev.event_seq);
            }}
        }}
        if cdm_barrier {{
            if let Some(ev) = ev_comp {{
                job.sj_comp.as_mut().unwrap().get().add(
                    Some(BarrierCmd {{ wait_slot: ev.slot, wait_value: ev.value }}),
                    false,
                    false,
                );
                job.record_barrier_wait(SQ_COMPUTE, ev.event_seq);
            }}
        }}
        let c = job.sj_comp.as_mut().unwrap().get();
        c.add(None, false, false);
        c.next_seq();
        job.commit()?;
        Ok(job)
    }}
}}

fn test_concurrent_commit_across_prepub_abort() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
    // J2 starts building (calls new_job on vtx & frag) BEFORE J1 fails.
    let (mut j2, tvb2) = q.begin_render(false, false, false, false);

    // J1 fails before publishing (ENOMEM in frag submit).
    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));
    drop(j1);

    // J2 MUST still commit cleanly (Bug 1: old Job::drop rolled back tail -> EINVAL).
    q.finish_commit(&mut j2, tvb2).expect("J2 commit must not fail out of order");
    assert_eq!(j2.run(), Ok(()));
    drop(j2);
    assert!(sim.drain_and_verify() > 0);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
}}

fn test_committed_barrier_waiters_cancel_instead_of_deadlocking() {{
    for frag_only in [false, true] {{
        let mut q = MockQueue::new();
        let mut sim = q.sim();
        sim.tvb_slot_initialized = true;

        // J1 commits, J2 (render with vdm_barrier=0) commits, J3 (compute with vdm_barrier=0) commits.
        let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
        let mut j2 = q.build_and_commit_render(true, frag_only, false, false).unwrap();
        let mut j3 = q.build_and_commit_compute(true, false).unwrap();

        q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
        assert_eq!(j1.run(), Err(ENOMEM));

        // J2 and J3 wait on J1's fragment stamp -> MUST fail with ECANCELED, NEVER publish!
        assert_eq!(j2.run(), Err(ECANCELED), "barrier waiter J2 must cancel (frag_only={{}})", frag_only);
        assert_eq!(j3.run(), Err(ECANCELED), "compute barrier waiter J3 must cancel");
        drop((j1, j2, j3));

        // Firmware sees zero deadlocked commands, and event slots are cleanly idle.
        assert_eq!(sim.drain_and_verify(), 0);
        assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
        assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
        assert!(q.q_comp.inner.lock().unwrap().event_slot.is_none());

        // Fresh submission after drain runs cleanly!
        let mut j4 = q.build_and_commit_render(true, frag_only, false, false).unwrap();
        assert_eq!(j4.run(), Ok(()));
        drop(j4);
        assert!(sim.drain_and_verify() > 0);
    }}
}}

fn test_job_submission_drop_preserves_last_submitted_and_free_slots() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    // Case A: J0 in flight on GPU, J1 and J2 committed. J1 succeeds frag.submit(), fails vtx.submit().
    let mut j0 = q.build_and_commit_render(false, false, false, false).unwrap();
    assert_eq!(j0.run(), Ok(()));
    let j0_ls = q.q_frag.inner.lock().unwrap().order.last_submitted;

    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
    let mut j2 = q.build_and_commit_render(false, false, false, false).unwrap();

    q.q_vtx.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));

    // Bug 2 check: q_frag.last_submitted MUST stay at J0 (NOT rolled-back tail), free_slots == 126.
    {{
        let fi = q.q_frag.inner.lock().unwrap();
        assert_eq!(fi.order.last_submitted, j0_ls, "JobSubmission::drop corrupted last_submitted");
        assert_eq!(fi.order.free_slots(), (MAX_JOB_SLOTS - 1) as usize, "free_slots corrupted");
        assert_eq!(fi.pending.len(), 2, "only J0 commands should remain pending");
    }}

    // J2 (no barrier on J1) runs cleanly while J0 is still in flight and J1 is NOT yet dropped!
    assert_eq!(j2.run(), Ok(()));
    drop((j0, j1, j2));
    assert!(sim.drain_and_verify() > 0);
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());

    // Case B: Empty queue (no J0), J1 and J2 committed. J1 fails in vtx.submit() after frag.submit().
    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
    let mut j2 = q.build_and_commit_render(false, false, false, false).unwrap();
    let initial_ls = q.q_frag.inner.lock().unwrap().order.last_submitted;
    q.q_vtx.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));
    {{
        let fi = q.q_frag.inner.lock().unwrap();
        assert_eq!(fi.order.last_submitted, initial_ls, "empty-queue JobSubmission::drop advanced last_submitted");
        assert_eq!(fi.order.free_slots(), MAX_JOB_SLOTS as usize, "empty-queue free_slots must stay MAX_JOB_SLOTS");
    }}
    assert_eq!(j2.run(), Ok(()));
    drop((j1, j2));
    assert!(sim.drain_and_verify() > 0);
}}

fn test_deferred_drop_with_inflight_j0_bug3() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    // J0 stays in flight (so drm_sched_get_finished_job would NOT free J1 before J2::run()!).
    let mut j0 = q.build_and_commit_render(false, false, false, false).unwrap();
    assert_eq!(j0.run(), Ok(()));

    // J1 fails at frag.submit() while J1 is kept alive (NOT dropped yet!).
    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
    let mut j2 = q.build_and_commit_render(false, false, false, false).unwrap();

    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));
    // DO NOT drop j1 yet! Bug 3 left j1.sj_vtx alive so q_vtx.submit_seq stayed behind!
    assert_eq!(j2.run(), Ok(()), "J2 must run cleanly even before J1 is dropped by free_job_cb");

    // Also test J3 failing at start_op() before taking any subqueue job, while kept alive!
    let mut j3 = q.build_and_commit_render(false, false, false, false).unwrap();
    j3.fail_start_op = Some(ENODEV);
    let mut j4 = q.build_and_commit_render(false, false, false, false).unwrap();
    assert_eq!(j3.run(), Err(ENODEV));
    assert_eq!(j4.run(), Ok(()), "J4 must run cleanly after J3 failed in start_op() before J3 drops");

    drop((j0, j1, j2, j3, j4));
    assert!(sim.drain_and_verify() > 0);
}}

fn test_fresh_submission_after_abort_while_j0_inflight_no_livelock() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    // J0 in flight on GPU.
    let mut j0 = q.build_and_commit_render(false, false, false, false).unwrap();
    assert_eq!(j0.run(), Ok(()));
    let j0_ev = q.q_frag.event_info().expect("J0 is live");

    // J1 commits, then aborts before publishing while J0 is STILL running.
    let mut j1 = q.build_and_commit_render(true, false, false, false).unwrap();
    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));
    drop(j1);

    // J2 arrives AFTER J1 aborted while J0 is STILL running, with vdm_barrier=0!
    // event_info() MUST point to live J0, NOT dead J1, so J2 does NOT livelock with ECANCELED!
    let ev_after_abort = q.q_frag.event_info().expect("J0 still in flight");
    assert_eq!(ev_after_abort.value, j0_ev.value, "event_info must point at live J0, not dead J1");
    assert_eq!(ev_after_abort.event_seq, j0_ev.event_seq);

    let mut j2 = q.build_and_commit_render(true, false, false, false).unwrap();
    assert_eq!(j2.run(), Ok(()), "J2 arriving after J1 abort while J0 runs must succeed (no livelock)");
    drop((j0, j2));
    assert!(sim.drain_and_verify() > 0);
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
}}

fn test_toctou_event_info_across_idle_reset() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    // J1 commits on idle queue.
    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();

    // Thread A samples event_info() BEFORE calling new_job()!
    let sampled_ev = q.q_frag.event_info().expect("J1 committed");

    // Thread B aborts and drops J1 -> queue becomes idle, event_slot resets to None!
    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));
    drop(j1);
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());

    // Thread A continues building J2 with the stale sampled_ev barrier (same slot, same value!).
    let mut j2 = QueueJob {{
        id: 99,
        sj_vtx: Some(SubQueueJob::new(q.q_vtx.clone())),
        sj_frag: Some(SubQueueJob::new(q.q_frag.clone())),
        sj_comp: Some(SubQueueJob::new(q.q_comp.clone())),
        inline_result: 0,
        tvb: None,
        wait_frag_seq: None,
        wait_comp_seq: None,
        fail_start_op: None,
        scenes: Vec::new(),
    }};
    j2.sj_vtx.as_mut().unwrap().get().add(
        Some(BarrierCmd {{ wait_slot: sampled_ev.slot, wait_value: sampled_ev.value }}),
        false,
        false,
    );
    j2.record_barrier_wait(SQ_RENDER, sampled_ev.event_seq);
    let ev_vtx = j2.sj_vtx.as_mut().unwrap().get().event_info();
    j2.sj_frag.as_mut().unwrap().get().add(
        Some(BarrierCmd {{ wait_slot: ev_vtx.slot, wait_value: ev_vtx.value.next() }}),
        false,
        false,
    );
    j2.sj_frag.as_mut().unwrap().get().add(None, false, true);
    j2.sj_vtx.as_mut().unwrap().get().add(None, false, true);
    j2.sj_vtx.as_mut().unwrap().get().next_seq();
    j2.sj_frag.as_mut().unwrap().get().next_seq();
    assert_eq!(j2.commit(), Ok(()));

    // J2 MUST cancel cleanly instead of self-deadlocking on its own stamp value!
    assert_eq!(j2.run(), Err(ECANCELED));
    drop(j2);
    assert_eq!(sim.drain_and_verify(), 0);
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
}}

fn test_inline_submit_failure_and_replay() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    let mut j1 = q.build_and_commit_render(false, false, false, false).unwrap();
    assert!(j1.can_submit_inline());

    // Inline submit fails at vtx.submit() after frag.submit() succeeded.
    q.q_vtx.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    let res = j1.run();
    assert_eq!(res, Err(ENOMEM));
    j1.inline_result = res.unwrap_err().0;

    // Scheduler replay consumes recorded error without double-aborting.
    assert_eq!(j1.run(), Err(ENOMEM));

    // Next job J2 submits and runs cleanly even BEFORE J1 is dropped!
    let mut j2 = q.build_and_commit_render(true, false, false, false).unwrap();
    assert_eq!(j2.run(), Ok(()));
    drop((j1, j2));
    assert!(sim.drain_and_verify() > 0);
}}

fn test_uncommitted_drop_and_partial_commit_uncommit() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    sim.tvb_slot_initialized = true;

    // Uncommitted drop resets idle queue cleanly.
    let (j_uncommitted, _tvb) = q.begin_render(false, false, false, false);
    drop(j_uncommitted);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());

    // Partial commit failure: vtx commits, frag fails -> vtx uncommits cleanly!
    q.q_frag.inner.lock().unwrap().fail_next_commit = Some(EINVAL);
    let (mut j_partial, _tvb) = q.begin_render(false, false, false, false);
    assert_eq!(j_partial.commit(), Err(EINVAL));
    drop(j_partial);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert_eq!(q.q_vtx.inner.lock().unwrap().order.commit_seq, 0);
    assert_eq!(q.q_vtx.inner.lock().unwrap().order.submit_seq, 0);

    // Next submission succeeds cleanly!
    let mut j_ok = q.build_and_commit_render(false, false, false, false).unwrap();
    assert_eq!(j_ok.run(), Ok(()));
    drop(j_ok);
    assert!(sim.drain_and_verify() > 0);
}}

fn test_rebind_tvb_init_loss_cancels_dependent_render() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    // Fresh buffer slot: firmware has NOT initialized the TVB slot yet!
    assert!(!sim.tvb_slot_initialized);

    // J1 has scene.rebind() == true (no TVB grow), so J1 carries InitBuffer.
    // J2 is built before J1 runs (so scene.rebind() == false, no InitBuffer, no explicit barrier).
    let mut j1 = q.build_and_commit_render(false, false, false, true).unwrap();
    let mut j2 = q.build_and_commit_render(false, false, false, false).unwrap();

    // J1 fails before publishing!
    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j1.run(), Err(ENOMEM));

    // J2 MUST cancel with ECANCELED instead of running on an uninitialized TVB slot!
    assert_eq!(j2.run(), Err(ECANCELED), "J2 relying on lost rebind InitBuffer must cancel");
    drop((j1, j2));
    assert_eq!(sim.drain_and_verify(), 0);

    // J3 arrives next: sees lost_init -> sets tvb_init_pending -> emits InitBuffer -> succeeds!
    let mut j3 = q.build_and_commit_render(true, false, false, false).unwrap();
    assert_eq!(j3.run(), Ok(()));
    drop(j3);
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);
}}

fn test_issue_2546_fault_injection_soak() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    let mut completed_jobs = 0usize;
    let mut failed_jobs = 0usize;

    // Simulate the #2546 stress mix: overlapping render+compute submissions with
    // periodic pre-publication ENOMEM faults on init-carrying and normal renders.
    for round in 0..40u64 {{
        let rebind = !sim.tvb_slot_initialized && round == 0;
        if round % 5 == 0 {{
            // Simulate TVB grow requiring a fresh slot init.
            q.tvb_init_pending = true;
            sim.tvb_slot_initialized = false;
        }}
        let mut j1 = q.build_and_commit_render(true, round % 2 == 0, false, rebind)
            .expect("J1 commit must never fail with EINVAL");
        let mut j2 = q.build_and_commit_render(round % 3 != 0, true, false, false)
            .expect("J2 commit must never fail with EINVAL");
        let mut j3 = q.build_and_commit_compute(round % 2 == 0, true)
            .expect("J3 commit must never fail with EINVAL");

        if round % 2 == 1 {{
            if round % 4 == 1 {{
                q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
            }} else {{
                q.q_vtx.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
            }}
        }}

        for job in [&mut j1, &mut j2, &mut j3] {{
            match job.run() {{
                Ok(()) => completed_jobs += 1,
                Err(e) => {{
                    assert!(e == ENOMEM || e == ECANCELED, "unexpected error {{:?}}", e);
                    failed_jobs += 1;
                }}
            }}
        }}
        drop((j1, j2, j3));
        sim.drain_and_verify();
    }}

    assert!(completed_jobs > 30, "expected healthy progress, got {{}}", completed_jobs);
    assert!(failed_jobs > 15, "expected injected failures, got {{}}", failed_jobs);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_comp.inner.lock().unwrap().event_slot.is_none());
}}

fn test_rebind_failure_mid_submit_render_preserves_tvb_init_pending() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();
    assert!(!sim.tvb_slot_initialized);

    // Case A: First render binds a fresh slot (`new_scene()` sets `last_token` and
    // returns `rebind == true`), then fails mid-`submit_render()` (e.g. invalid
    // `cmdbuf.samples` EINVAL or `Arc::new` ENOMEM) before `commit()`.
    let res = q.submit_ioctl(&[IoctlCmd::Render {{
        vdm_barrier: None,
        frag_barrier_only: false,
        cdm_barrier: None,
        fail_after_new_scene: Some(EINVAL),
    }}]);
    assert!(matches!(res, Err(EINVAL)));
    assert_eq!(q.buf.active_scenes(), 0, "failed render must drop its scene");
    assert!(
        q.tvb_init_pending,
        "rebind must remain pending after mid-submit_render() failure"
    );

    // Next valid render re-acquires the same slot with `rebind == false` because
    // `last_token` was already recorded; it MUST still emit InitBuffer via `tvb_init_pending`!
    let mut j_next = q
        .submit_ioctl(&[IoctlCmd::Render {{
            vdm_barrier: None,
            frag_barrier_only: false,
            cdm_barrier: None,
            fail_after_new_scene: None,
        }}])
        .expect("retry render must succeed");
    assert!(!q.tvb_init_pending, "tvb_init_pending clears once init-carrying job commits");
    assert_eq!(j_next.run(), Ok(()));
    drop(j_next);
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);
    assert_eq!(q.buf.active_scenes(), 0);

    // Case B: Slot evicted while idle -> mid-submit_render() ENOMEM -> next render still initializes.
    q.buf.evict();
    sim.tvb_slot_initialized = false;
    assert!(matches!(
        q.submit_ioctl(&[IoctlCmd::Render {{
            vdm_barrier: None,
            frag_barrier_only: false,
            cdm_barrier: None,
            fail_after_new_scene: Some(ENOMEM),
        }}]),
        Err(ENOMEM)
    ));
    assert!(q.tvb_init_pending);
    let mut j_evict = q
        .submit_ioctl(&[IoctlCmd::Render {{
            vdm_barrier: None,
            frag_barrier_only: false,
            cdm_barrier: None,
            fail_after_new_scene: None,
        }}])
        .unwrap();
    assert_eq!(j_evict.run(), Ok(()));
    drop(j_evict);
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);

    // Case C: Pre-existing `tvb_init_pending` (e.g. TVB grow) must survive:
    // (1) uncommitted begin_render() drop, (2) mid-submit_render() failure, and
    // (3) commit() failure, clearing only after finish_commit() succeeds.
    q.tvb_init_pending = true;
    sim.tvb_slot_initialized = false;

    let (j_uncommitted, _tvb) = q.begin_render(false, false, false, false);
    drop(j_uncommitted);
    assert!(q.tvb_init_pending, "uncommitted begin_render drop must not clear tvb_init_pending");

    assert!(matches!(
        q.submit_ioctl(&[IoctlCmd::Render {{
            vdm_barrier: None,
            frag_barrier_only: false,
            cdm_barrier: None,
            fail_after_new_scene: Some(ENOMEM),
        }}]),
        Err(ENOMEM)
    ));
    assert!(q.tvb_init_pending, "mid-submit_render error must not clear tvb_init_pending");

    q.q_frag.inner.lock().unwrap().fail_next_commit = Some(EINVAL);
    assert!(matches!(
        q.submit_ioctl(&[IoctlCmd::Render {{
            vdm_barrier: None,
            frag_barrier_only: false,
            cdm_barrier: None,
            fail_after_new_scene: None,
        }}]),
        Err(EINVAL)
    ));
    assert!(q.tvb_init_pending, "commit() failure must not clear tvb_init_pending");

    // Case D: Multi-render ioctl [r0, r1] emits InitBuffer ONCE (not twice on r1),
    // and preserves tvb_init_pending if r1 fails after r0 succeeded.
    let mut j_pair = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: None,
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1),
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
        ])
        .unwrap();
    let init_count = j_pair
        .sj_vtx
        .as_ref()
        .unwrap()
        .job
        .as_ref()
        .unwrap()
        .pending
        .iter()
        .filter(|c| c.init_buffer)
        .count();
    assert_eq!(init_count, 1, "multi-render ioctl must emit InitBuffer once, not redundantly on r1");
    assert!(!q.tvb_init_pending);
    assert_eq!(j_pair.run(), Ok(()));
    drop(j_pair);
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);

    // Multi-render ioctl after eviction where r0 rebinds and succeeds, then r1 fails mid-submit_render().
    q.buf.evict();
    sim.tvb_slot_initialized = false;
    assert!(matches!(
        q.submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: None,
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1),
                frag_barrier_only: true,
                cdm_barrier: None,
                fail_after_new_scene: Some(EINVAL),
            }},
        ]),
        Err(EINVAL)
    ));
    assert_eq!(q.buf.active_scenes(), 0);
    assert!(q.tvb_init_pending, "r1 failure after r0 rebind must preserve tvb_init_pending");

    // Subsequent multi-render ioctl initializes once on r0 and runs cleanly.
    let mut j_recover = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: None,
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1),
                frag_barrier_only: true,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
        ])
        .unwrap();
    let recover_init_count = j_recover
        .sj_vtx
        .as_ref()
        .unwrap()
        .job
        .as_ref()
        .unwrap()
        .pending
        .iter()
        .filter(|c| c.init_buffer)
        .count();
    assert_eq!(recover_init_count, 1, "recovery multi-render ioctl must emit InitBuffer exactly once");
    assert_eq!(j_recover.run(), Ok(()));
    drop(j_recover);
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
}}

fn test_multi_command_ioctl_intra_and_cross_barriers() {{
    let mut q = MockQueue::new();
    let mut sim = q.sim();

    // Case A: Idle queue with intra-ioctl barriers (`index > 0`) across [R0, C0, R1, C1].
    // Intra-ioctl barriers wait on earlier commands of the SAME QueueJob and MUST NOT
    // be recorded in wait_frag_seq/wait_comp_seq (otherwise J0 would self-cancel with ECANCELED!).
    let mut j0 = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: Some(0),
                frag_barrier_only: false,
                cdm_barrier: Some(0),
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(1), // intra-ioctl wait on R0's frag
                cdm_barrier: Some(0),
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1), // intra-ioctl frag-only wait on R0's frag
                frag_barrier_only: true,
                cdm_barrier: Some(1), // intra-ioctl wait on C0
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(2), // intra-ioctl wait on R1's frag
                cdm_barrier: Some(1), // intra-ioctl wait on C0
            }},
        ])
        .unwrap();
    assert_eq!(j0.wait_frag_seq, None, "idle queue intra-ioctl barriers must not record cross-ioctl frag wait");
    assert_eq!(j0.wait_comp_seq, None, "idle queue intra-ioctl barriers must not record cross-ioctl comp wait");
    assert_eq!(j0.run(), Ok(()), "multi-command ioctl must not self-cancel on intra-ioctl barriers");

    // Case B: While J0 is published and live in flight, J1 uses BOTH cross-ioctl (`index == 0`)
    // AND intra-ioctl (`index > 0`) barriers across [R0, C0, R1, C1].
    let j0_frag_seq = q.q_frag.event_info().expect("J0 frag live").event_seq;
    let j0_comp_seq = q.q_comp.event_info().expect("J0 comp live").event_seq;
    let mut j1 = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: Some(0), // cross-ioctl wait on J0 frag
                frag_barrier_only: false,
                cdm_barrier: Some(0), // cross-ioctl wait on J0 comp
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(1), // intra-ioctl wait on J1.R0 frag
                cdm_barrier: Some(0), // cross-ioctl wait on J0 comp
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1), // intra-ioctl frag-only wait on J1.R0 frag
                frag_barrier_only: true,
                cdm_barrier: Some(1), // intra-ioctl wait on J1.C0
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(2), // intra-ioctl wait on J1.R1 frag
                cdm_barrier: Some(1), // intra-ioctl wait on J1.C0
            }},
        ])
        .unwrap();
    assert_eq!(
        j1.wait_frag_seq,
        Some(j0_frag_seq),
        "only cross-ioctl index 0 frag barrier may be recorded"
    );
    assert_eq!(
        j1.wait_comp_seq,
        Some(j0_comp_seq),
        "only cross-ioctl index 0 comp barrier may be recorded"
    );
    assert_eq!(j1.run(), Ok(()), "J1 must run cleanly with both cross-ioctl and intra-ioctl barriers");
    drop((j0, j1));
    assert!(sim.drain_and_verify() > 0);
    assert!(sim.tvb_slot_initialized);

    // Case C: Prior job J2 commits, then multi-command J3 commits with BOTH cross-ioctl (`index == 0`)
    // and intra-ioctl (`index > 0`) barriers, then J2 aborts before publishing.
    // J3 MUST cancel with ECANCELED because its `index == 0` dependency never published.
    let mut j2 = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: None,
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(1),
                cdm_barrier: None,
            }},
        ])
        .unwrap();
    let mut j3 = q
        .submit_ioctl(&[
            IoctlCmd::Render {{
                vdm_barrier: Some(0), // cross-ioctl on J2 frag
                frag_barrier_only: true,
                cdm_barrier: Some(0), // cross-ioctl on J2 comp
                fail_after_new_scene: None,
            }},
            IoctlCmd::Render {{
                vdm_barrier: Some(1), // intra-ioctl on J3.R0 frag
                frag_barrier_only: false,
                cdm_barrier: None,
                fail_after_new_scene: None,
            }},
            IoctlCmd::Compute {{
                vdm_barrier: Some(2), // intra-ioctl on J3.R1 frag
                cdm_barrier: Some(0), // cross-ioctl on J2 comp
            }},
        ])
        .unwrap();
    q.q_frag.inner.lock().unwrap().fail_next_submit = Some(ENOMEM);
    assert_eq!(j2.run(), Err(ENOMEM));
    assert_eq!(j3.run(), Err(ECANCELED), "multi-command J3 waiting on aborted J2 must cancel");
    drop((j2, j3));
    assert_eq!(sim.drain_and_verify(), 0);
    assert!(q.q_vtx.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_frag.inner.lock().unwrap().event_slot.is_none());
    assert!(q.q_comp.inner.lock().unwrap().event_slot.is_none());
}}

fn main() {{
    assert!({str(job_drop_uses_abort).lower()}, "Job::drop must abort via WorkQueueOrder without tail .sub()");
    assert!({str(sub_drop_uses_abort).lower()}, "JobSubmission::drop must abort without clobbering last_submitted or tail");
    assert!({str(submit_defers_last_submitted).lower()}, "Job::submit must defer last_submitted update to JobSubmission::run()");
    assert!({str(event_info_uses_wait_target).lower()}, "WorkQueue::event_info must use WorkQueueOrder::wait_target()");
    assert!({str(run_takes_all_jobs_upfront).lower()}, "QueueJob::run must take all subqueue jobs before any submit() and abort on error");
    assert!({str(run_checks_barrier).lower()}, "QueueJob::run must cancel jobs whose barrier predecessor aborted");
    assert!({str(commit_uncommits_on_err).lower()}, "QueueJob::commit must uncommit earlier subqueues on partial commit failure");
    assert!({str(render_latches_rebind_early).lower()}, "submit_render must latch tvb_init_pending right after new_scene() before fallible steps");
    assert!({str(render_reports_rebind_init).lower()}, "submit_render must report scene.rebind() || init_slot and clear tvb_init_pending only after push()");
    assert!({str(barriers_record_cross_ioctl_only).lower()}, "Queue::submit must record only cross-ioctl index 0 barriers in wait_*_seq");

    test_concurrent_commit_across_prepub_abort();
    test_committed_barrier_waiters_cancel_instead_of_deadlocking();
    test_job_submission_drop_preserves_last_submitted_and_free_slots();
    test_deferred_drop_with_inflight_j0_bug3();
    test_fresh_submission_after_abort_while_j0_inflight_no_livelock();
    test_toctou_event_info_across_idle_reset();
    test_inline_submit_failure_and_replay();
    test_uncommitted_drop_and_partial_commit_uncommit();
    test_rebind_tvb_init_loss_cancels_dependent_render();
    test_rebind_failure_mid_submit_render_preserves_tvb_init_pending();
    test_multi_command_ioctl_intra_and_cross_barriers();
    test_issue_2546_fault_injection_soak();
    println!("PASS legacy-wq-prepub-abort-tests");
}}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        '--mutation',
        choices=[
            'old-job-drop-rollback',
            'old-submission-drop-last-submitted',
            'omit-submit-seq-advance',
            'omit-barrier-invalidation',
            'stale-event-info-tail',
            'partial-take-in-run',
            'omit-rebind-tvb-init',
            'late-rebind-latch',
            'record-intra-ioctl-barrier',
        ],
    )
    args = ap.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', os.environ.get('RUSTC', 'rustc'))
    with tempfile.TemporaryDirectory(prefix='legacy-wq-prepub-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '--edition=2021', '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
