#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for G14 firmware 27.2 pump() EBUSY retry and pre-publication VM isolation.

Extracts and compiles the production G14 27.2 runtime state-machine paths from:
  - drivers/gpu/drm/asahi/g14_v27_2.rs (`EP_DOORBELL`, `COMMAND_TIMEOUT_MS`,
    `STOP_DRAIN_MS`, `RETIRE_WAIT_US`, `RESOURCE_RETRY_MS`, `FirmwareWait`,
    `State` helpers including `fail_publication`, `Runtime::{healthy,submit,
    withdraw,fail_all,acknowledge,free_slots,handle_ring_busy,pump,
    publish_compute,publish_render,retire_slot,retire_render,advance,service,
    service_locked,sleep_prepare,sleep_complete,stop}`)

Verifies all positive scenarios and kills each negative-control mutation with
a Rust assertion panic (exit 101).
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'


def extract_item(source, signature, attributes=False):
    """Extract a braced Rust item starting at `signature` through its matching `}`."""
    assert source.count(signature) == 1, (
        f'expected 1 occurrence of {signature!r}, got {source.count(signature)}'
    )
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


def build_harness(mutation=None):
    g14_src = (ASAHI / 'g14_v27_2.rs').read_text()
    v27_2_src = (ASAHI / 'gpu/v27_2.rs').read_text()

    # Structural assertions on production sources.
    assert 'pub(crate) fn check_render(&self) -> Result' in v27_2_src
    assert 'pub(crate) fn check_compute(&self) -> Result' in v27_2_src
    assert 'Err(EBUSY) if state.running()' not in g14_src, (
        'dead Err(EBUSY) if state.running() branch remains in g14_v27_2.rs'
    )

    g14_consts = '\n'.join([
        extract_line(g14_src, 'const EP_DOORBELL:'),
        extract_line(g14_src, 'const COMMAND_TIMEOUT_MS:'),
        extract_line(g14_src, 'const STOP_DRAIN_MS:'),
        extract_line(g14_src, 'const RETIRE_WAIT_US:'),
        extract_line(g14_src, 'const RESOURCE_RETRY_MS:'),
        extract_line(g14_src, 'const EVENT_HISTORY:'),
    ])
    vm_failed_fn = extract_item(g14_src, 'fn vm_failed(packet: &Packet) -> bool {')
    fw_wait = '\n'.join([
        extract_item(g14_src, 'struct FirmwareWait {'),
        extract_item(g14_src, 'impl FirmwareWait {'),
    ])
    state_impl = extract_item(g14_src, 'impl State {\n    /// Stop publishing')
    rt_impl = extract_item(g14_src, 'impl Runtime {')
    rt_methods = {
        'healthy': extract_item(rt_impl, '    pub(crate) fn healthy(&self) -> bool {'),
        'submit': extract_item(rt_impl, '    pub(crate) fn submit(self: &Arc<Self>, packet: Arc<Packet>) -> Result {'),
        'withdraw': extract_item(rt_impl, '    pub(crate) fn withdraw(&self, packet: &Arc<Packet>) {'),
        'fail_all': extract_item(rt_impl, '    fn fail_all(&self, state: &mut State, error: Error) {'),
        'acknowledge': extract_item(rt_impl, '    fn acknowledge(&self, state: &mut State) {'),
        'free_slots': extract_item(rt_impl, '    fn free_slots(&self, state: &mut State, count: usize) -> Result<Option<[usize; 2]>> {'),
        'handle_ring_busy': extract_item(rt_impl, '    fn handle_ring_busy(&self, state: &mut State, packet: &Arc<Packet>, paired: bool) -> bool {'),
        'pump': extract_item(rt_impl, '    fn pump(&self, state: &mut State) {'),
        'publish_compute': extract_item(rt_impl, '    fn publish_compute('),
        'publish_render': extract_item(rt_impl, '    fn publish_render('),
        'retire_slot': extract_item(rt_impl, '    fn retire_slot(&self, state: &mut State, slot: usize) {'),
        'retire_render': extract_item(rt_impl, '    fn retire_render(&self, state: &mut State) -> Result {'),
        'advance': extract_item(rt_impl, '    fn advance(&self, state: &mut State) -> Result {'),
        'service': extract_item(rt_impl, '    fn service(&self) {'),
        'service_locked': extract_item(rt_impl, '    fn service_locked(&self, state: &mut State) {'),
        'sleep_prepare': extract_item(rt_impl, '    pub(crate) fn sleep_prepare(&self, deep: bool) -> Result {'),
        'sleep_complete': extract_item(rt_impl, '    pub(crate) fn sleep_complete(&self) {'),
        'stop': extract_item(rt_impl, '    pub(crate) fn stop(&self) {'),
    }

    # Verify publish_render checks ring room before calling queue.prepare().
    check_pos = rt_methods['publish_render'].index('check_render()')
    prep_pos = rt_methods['publish_render'].index('queue.prepare(')
    assert check_pos < prep_pos, 'publish_render must call check_render before queue.prepare'

    # Apply targeted negative-control mutations directly to extracted production code.
    if mutation == 'dead-render-ebusy':
        old = '                Err(EBUSY) => {\n                    if !self.handle_ring_busy(state, &packet, paired) {\n                        return;\n                    }\n                }'
        assert old in rt_methods['pump']
        rt_methods['pump'] = rt_methods['pump'].replace(
            old,
            '                Err(EBUSY) if state.running() => return,',
            1,
        )
    elif mutation == 'poison-vm-on-fail-pub':
        old = 'packet.fail_packet(err);'
        assert old in state_impl
        state_impl = state_impl.replace(old, 'packet.finish(Err(err));', 1)
    elif mutation == 'poison-vm-on-deferred-fail-pub':
        old = 'done.packet.fail_packet(err);'
        assert old in rt_methods['service_locked']
        rt_methods['service_locked'] = rt_methods['service_locked'].replace(
            old, 'done.packet.finish(Err(err));', 1
        )
    elif mutation == 'no-ebusy-retry-arm':
        old = '        self.notify.retry();\n        false'
        assert old in rt_methods['handle_ring_busy']
        rt_methods['handle_ring_busy'] = rt_methods['handle_ring_busy'].replace(
            old, '        false', 1
        )
    elif mutation == 'no-ebusy-timeout':
        old = 'if now - wait.since >= Delta::from_millis(COMMAND_TIMEOUT_MS) {\n            return Err(ETIMEDOUT);\n        }'
        assert old in fw_wait
        fw_wait = fw_wait.replace(old, 'let _ = now;', 1)
    elif mutation == 'keep-ebusy-wait-on-withdraw':
        old = '            if front_removed {\n                state.waiting_ring = None;\n            }'
        assert old in rt_methods['withdraw']
        rt_methods['withdraw'] = rt_methods['withdraw'].replace(old, '', 1)
    elif mutation == 'keep-ebusy-wait-on-sleep':
        assert 'state.waiting_ring = None;' in rt_methods['sleep_prepare']
        assert 'state.waiting_ring = None;' in rt_methods['sleep_complete']
        rt_methods['sleep_prepare'] = rt_methods['sleep_prepare'].replace(
            'state.waiting_ring = None;', ''
        )
        rt_methods['sleep_complete'] = rt_methods['sleep_complete'].replace(
            'state.waiting_ring = None;', ''
        )
    elif mutation == 'no-precheck-render-ring':
        old = '        state.root.check_render()?;\n'
        assert old in rt_methods['publish_render']
        rt_methods['publish_render'] = rt_methods['publish_render'].replace(old, '', 1)
    elif mutation is not None:
        raise ValueError(f'unknown mutation: {mutation}')

    rt_body = '\n'.join(rt_methods.values())

    return f'''
#![allow(warnings)]
use core::ops::Sub;
use core::sync::atomic::{{AtomicBool, AtomicI64, AtomicU32, AtomicU64, AtomicU8, AtomicUsize, Ordering}};
use std::sync::{{Arc, Condvar as StdCondvar, Mutex as StdMutex, MutexGuard as StdMutexGuard}};
use std::time::Duration;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Error(pub i32);
pub type Result<T = ()> = core::result::Result<T, Error>;

pub const EIO: Error = Error(-5);
pub const ENOMEM: Error = Error(-12);
pub const EBUSY: Error = Error(-16);
pub const ENODEV: Error = Error(-19);
pub const EINVAL: Error = Error(-22);
pub const ENOSPC: Error = Error(-28);
pub const EOVERFLOW: Error = Error(-75);
pub const ETIMEDOUT: Error = Error(-110);
pub const ECANCELED: Error = Error(-125);
pub const ENOTSUPP: Error = Error(-524);

pub struct GfpKernel;
pub const GFP_KERNEL: GfpKernel = GfpKernel;

pub mod kernel {{
    pub use crate::container_of;
    pub mod alloc {{
        use crate::{{Error, ENOMEM}};
        #[derive(Copy, Clone, Debug)]
        pub struct AllocError;
        impl From<AllocError> for Error {{
            fn from(_: AllocError) -> Self {{ ENOMEM }}
        }}
    }}
    pub mod error {{
        use crate::{{Error, Result}};
        pub fn to_result(v: i32) -> Result {{
            if v == 0 {{ Ok(()) }} else {{ Err(Error(v)) }}
        }}
    }}
    pub mod bindings {{
        pub struct delayed_work;
        pub unsafe fn flush_delayed_work(_: *mut delayed_work) {{}}
        pub unsafe fn apple_rtkit_poweroff(_: *mut core::ffi::c_void) -> i32 {{ 0 }}
    }}
}}

#[macro_export]
macro_rules! container_of {{
    ($ptr:expr, $ty:ty, $field:ident) => {{
        $ptr as *mut $ty
    }};
}}

macro_rules! dev_err {{ ($dev:expr, $($t:tt)*) => {{ {{ let _ = $dev; }} }}; }}
macro_rules! dev_warn {{ ($dev:expr, $($t:tt)*) => {{ {{ let _ = $dev; }} }}; }}
macro_rules! dev_info {{ ($dev:expr, $($t:tt)*) => {{ {{ let _ = $dev; }} }}; }}

static NOW_US: AtomicI64 = AtomicI64::new(1_000_000);

fn reset_clock() {{
    NOW_US.store(1_000_000, Ordering::SeqCst);
}}

fn advance_ms(ms: i64) {{
    NOW_US.fetch_add(ms * 1000, Ordering::SeqCst);
}}

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Delta(i64);
impl Delta {{
    pub fn from_millis(ms: i64) -> Self {{ Self(ms * 1000) }}
    pub fn from_micros(us: i64) -> Self {{ Self(us) }}
}}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Monotonic;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Instant<T>(i64, core::marker::PhantomData<T>);
impl<T> Instant<T> {{
    pub fn now() -> Self {{
        Self(NOW_US.load(Ordering::SeqCst), core::marker::PhantomData)
    }}
    pub fn elapsed(&self) -> Delta {{
        Delta(NOW_US.load(Ordering::SeqCst) - self.0)
    }}
}}
impl<T> Sub for Instant<T> {{
    type Output = Delta;
    fn sub(self, rhs: Self) -> Delta {{
        Delta(self.0 - rhs.0)
    }}
}}

fn msecs_to_jiffies(ms: u32) -> u64 {{ u64::from(ms) }}
fn fsleep(d: Delta) {{ NOW_US.fetch_add(d.0, Ordering::SeqCst); }}

pub struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> {{
    pub fn new(v: T) -> Self {{ Self(StdMutex::new(v)) }}
    pub fn lock(&self) -> StdMutexGuard<'_, T> {{ self.0.lock().unwrap() }}
}}

pub enum CondVarTimeoutResult {{
    Woken {{ jiffies: u64 }},
    TimedOut,
}}

pub struct CondVar {{
    cv: StdCondvar,
    notifies: AtomicUsize,
}}
impl CondVar {{
    pub fn new() -> Self {{
        Self {{ cv: StdCondvar::new(), notifies: AtomicUsize::new(0) }}
    }}
    pub fn notify_all(&self) {{
        self.notifies.fetch_add(1, Ordering::SeqCst);
        self.cv.notify_all();
    }}
    pub fn wait_timeout<'a, T>(&self, guard: &mut StdMutexGuard<'a, T>, jiffies: u64) -> CondVarTimeoutResult {{
        let taken = unsafe {{ core::ptr::read(guard) }};
        let (new_guard, res) = self.cv.wait_timeout(taken, Duration::from_millis(15)).unwrap();
        unsafe {{ core::ptr::write(guard, new_guard) }};
        if res.timed_out() {{
            advance_ms(jiffies as i64);
            CondVarTimeoutResult::TimedOut
        }} else {{
            CondVarTimeoutResult::Woken {{ jiffies }}
        }}
    }}
}}

pub struct KVec<T>(Vec<T>);
impl<T> KVec<T> {{
    pub fn new() -> Self {{ Self(Vec::new()) }}
    pub fn push(&mut self, v: T, _: GfpKernel) -> core::result::Result<(), kernel::alloc::AllocError> {{
        self.0.push(v);
        Ok(())
    }}
    pub fn pop(&mut self) -> Option<T> {{ self.0.pop() }}
    pub fn remove(&mut self, i: usize) -> Result<T> {{
        if i < self.0.len() {{ Ok(self.0.remove(i)) }} else {{ Err(EINVAL) }}
    }}
    pub fn len(&self) -> usize {{ self.0.len() }}
    pub fn is_empty(&self) -> bool {{ self.0.is_empty() }}
    pub fn first(&self) -> Option<&T> {{ self.0.first() }}
    pub fn iter(&self) -> core::slice::Iter<'_, T> {{ self.0.iter() }}
    pub fn iter_mut(&mut self) -> core::slice::IterMut<'_, T> {{ self.0.iter_mut() }}
}}
impl<T> core::ops::Index<usize> for KVec<T> {{
    type Output = T;
    fn index(&self, i: usize) -> &T {{ &self.0[i] }}
}}
impl<T> core::ops::IndexMut<usize> for KVec<T> {{
    fn index_mut(&mut self, i: usize) -> &mut T {{ &mut self.0[i] }}
}}

mod mmu {{
    use super::*;

    pub struct VmStatus {{
        pub recorded: StdMutex<Vec<Error>>,
    }}
    impl VmStatus {{
        pub fn new() -> Arc<Self> {{ Arc::new(Self {{ recorded: StdMutex::new(Vec::new()) }}) }}
        pub fn record(&self, err: Error) {{ self.recorded.lock().unwrap().push(err); }}
        pub fn record_if_device_loss(&self, err: Error) {{
            if err == EIO || err == ETIMEDOUT || err == ENODEV {{
                self.record(err);
            }}
        }}
        pub fn count(&self) -> usize {{ self.recorded.lock().unwrap().len() }}
        pub fn errors(&self) -> Vec<Error> {{ self.recorded.lock().unwrap().clone() }}
        pub fn get(&self) -> i32 {{
            self.recorded.lock().unwrap().first().map_or(0, |e| e.0)
        }}
    }}

    #[derive(Clone)]
    pub struct Vm {{
        pub id: u64,
        pub vm_status: Arc<VmStatus>,
    }}
    impl Vm {{
        pub fn new(id: u64) -> Self {{
            Self {{ id, vm_status: VmStatus::new() }}
        }}
        pub fn status(&self) -> Option<&Arc<VmStatus>> {{ Some(&self.vm_status) }}
    }}

    pub struct VmBind {{
        slot: u32,
        live: Arc<AtomicUsize>,
    }}
    impl VmBind {{
        pub fn slot(&self) -> u32 {{ self.slot }}
    }}
    impl Drop for VmBind {{
        fn drop(&mut self) {{
            self.live.fetch_sub(1, Ordering::SeqCst);
        }}
    }}

    pub struct Uat {{
        pub live_binds: Arc<AtomicUsize>,
        pub bind_calls: AtomicUsize,
    }}
    impl Uat {{
        pub fn new() -> Self {{
            Self {{
                live_binds: Arc::new(AtomicUsize::new(0)),
                bind_calls: AtomicUsize::new(0),
            }}
        }}
        pub fn bind(&self, vm: &Vm) -> Result<VmBind> {{
            self.bind_calls.fetch_add(1, Ordering::SeqCst);
            self.live_binds.fetch_add(1, Ordering::SeqCst);
            Ok(VmBind {{ slot: (vm.id as u32 % 63) + 1, live: self.live_binds.clone() }})
        }}
    }}
}}

mod event {{
    #[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
    pub struct EventValue(pub u32);
    impl EventValue {{
        pub fn next(self) -> Self {{ Self(self.0.wrapping_add(0x100)) }}
        pub fn raw(self) -> u32 {{ self.0 }}
    }}
}}

mod gpu {{
    use super::*;
    pub const POOL_SLOTS: usize = 8;
    pub const TILER: usize = 0;
    pub const FRAGMENT: usize = 1;
    pub const COMPUTE_KICK: u64 = 1;

    #[derive(Copy, Clone)]
    pub struct QueueInfo {{
        pub slot: u32,
        pub compute_state: u64,
    }}

    pub struct ComputeJob {{
        pub context: u32,
        pub sequence: u64,
        pub cdm_base: u64,
        pub cdm_end: u64,
        pub usc_base: u64,
        pub preempt: u64,
        pub timestamps: [u64; 2],
        pub helper: crate::m3::agx_compute::Helper,
        pub sampler_heap: u64,
        pub sampler_count: u32,
    }}

    pub struct CommandPool {{
        pub encode_calls: usize,
        pub mark_published_calls: usize,
        pub published_mask: u64,
        pub registered_mask: u64,
        pub fail_encode_on_call: Option<(usize, Error)>,
    }}
    impl CommandPool {{
        pub fn new() -> Self {{
            Self {{
                encode_calls: 0,
                mark_published_calls: 0,
                published_mask: 0,
                registered_mask: 0,
                fail_encode_on_call: None,
            }}
        }}
        pub fn encode(&mut self, slot: usize, _q: &QueueInfo, _stamp: u32, _job: &ComputeJob) -> Result<u64> {{
            self.encode_calls += 1;
            if let Some((n, err)) = self.fail_encode_on_call {{
                if self.encode_calls == n {{
                    return Err(err);
                }}
            }}
            Ok(0x1000 * (slot as u64 + 1))
        }}
        pub fn encode_with(
            &mut self,
            slot: usize,
            _context: u32,
            _sequence: u64,
            _stamp: u32,
            f: impl FnOnce(&mut [u8], &mut [u8], crate::render::Common) -> Result,
        ) -> Result<u64> {{
            self.encode_calls += 1;
            let mut cmd = [0u8; 0x1000];
            let mut stream = [0u8; 0x400];
            f(&mut cmd, &mut stream, crate::render::Common)?;
            Ok(0x2000 * (slot as u64 + 1))
        }}
        pub fn mark_published(&mut self, slot: usize) {{
            self.mark_published_calls += 1;
            self.published_mask ^= 1u64 << slot;
        }}
        pub fn retire(&self, _slot: usize) -> Result<bool> {{ Ok(true) }}
        pub fn registered(&self, slot: usize) -> Result<bool> {{
            Ok(self.registered_mask & (1u64 << slot) != 0)
        }}
        pub fn describe(&self, _slot: usize) -> Result<(u32, u32, u32)> {{ Ok((0, 0, 0)) }}
    }}

    pub struct Submission {{
        pub published: Arc<AtomicUsize>,
        pub announced: Option<Arc<AtomicBool>>,
        pub tgt: u32,
    }}
    impl Submission {{
        pub fn publish(self) {{
            self.published.fetch_add(1, Ordering::SeqCst);
            if let Some(a) = self.announced {{
                a.store(true, Ordering::Release);
            }}
        }}
        pub fn target(&self) -> u32 {{ self.tgt }}
    }}

    pub struct RootV27_2 {{
        pub compute_published: Arc<AtomicUsize>,
        pub render_published: Arc<AtomicUsize>,
        pub compute_retired: Arc<AtomicU32>,
        pub compute_stamp: Arc<AtomicU32>,
        pub render_done_flags: Arc<[AtomicBool; 2]>,
        pub render_stamps: Arc<[AtomicU32; 2]>,
        pub render_targets: [AtomicU32; 2],
        pub render_announced: [Arc<AtomicBool>; 2],
        pub check_compute_err: Option<Error>,
        pub check_render_err: Option<Error>,
        pub stage_compute_err: Option<Error>,
        pub stage_render_err: [Option<Error>; 2],
    }}
    impl RootV27_2 {{
        pub fn new() -> Self {{
            Self {{
                compute_published: Arc::new(AtomicUsize::new(0)),
                render_published: Arc::new(AtomicUsize::new(0)),
                compute_retired: Arc::new(AtomicU32::new(0)),
                compute_stamp: Arc::new(AtomicU32::new(0)),
                render_done_flags: Arc::new([AtomicBool::new(false), AtomicBool::new(false)]),
                render_stamps: Arc::new([AtomicU32::new(0), AtomicU32::new(0)]),
                render_targets: [AtomicU32::new(0), AtomicU32::new(0)],
                render_announced: [Arc::new(AtomicBool::new(false)), Arc::new(AtomicBool::new(false))],
                check_compute_err: None,
                check_render_err: None,
                stage_compute_err: None,
                stage_render_err: [None, None],
            }}
        }}
        pub fn drain(&self) -> Result {{ Ok(()) }}
        pub fn queue(&self) -> (QueueInfo, event::EventValue) {{
            (
                QueueInfo {{ slot: 0, compute_state: 0 }},
                event::EventValue(self.compute_stamp.load(Ordering::SeqCst)),
            )
        }}
        pub fn progress(&self) -> Result<(u32, u32)> {{
            Ok((
                self.compute_published.load(Ordering::SeqCst) as u32,
                self.compute_retired.load(Ordering::SeqCst),
            ))
        }}
        pub fn check_compute(&self) -> Result {{
            if let Some(err) = self.check_compute_err {{
                return Err(err);
            }}
            Ok(())
        }}
        pub fn check_render(&self) -> Result {{
            if let Some(err) = self.check_render_err {{
                return Err(err);
            }}
            Ok(())
        }}
        pub fn stage_compute(&self, _cmd: u64) -> Result<Submission> {{
            if let Some(err) = self.stage_compute_err {{
                return Err(err);
            }}
            Ok(Submission {{
                published: self.compute_published.clone(),
                announced: None,
                tgt: 1,
            }})
        }}
        pub fn render_queue(&self, dm: usize) -> (QueueInfo, event::EventValue) {{
            (
                QueueInfo {{ slot: dm as u32, compute_state: 0 }},
                event::EventValue(self.render_stamps[dm].load(Ordering::SeqCst)),
            )
        }}
        pub fn render_queue_announced(&self, dm: usize) -> bool {{
            self.render_announced[dm].load(Ordering::Acquire)
        }}
        pub fn render_kick(dm: usize) -> u64 {{ 10 + dm as u64 }}
        pub fn stage_render(&self, dm: usize, _work: &[u64]) -> Result<Submission> {{
            if let Some(err) = self.stage_render_err[dm] {{
                return Err(err);
            }}
            let tgt = self.render_targets[dm].fetch_add(1, Ordering::SeqCst) + 1;
            Ok(Submission {{
                published: self.render_published.clone(),
                announced: Some(self.render_announced[dm].clone()),
                tgt,
            }})
        }}
        pub fn render_done(&self, dm: usize, _target: u32, _stamp: event::EventValue) -> Result<bool> {{
            Ok(self.render_done_flags[dm].load(Ordering::SeqCst))
        }}
        pub fn render_retired(&self, dm: usize, _target: u32) -> Result<bool> {{
            Ok(self.render_done_flags[dm].load(Ordering::SeqCst))
        }}
        pub fn recovery_phase(&self) -> Result<u32> {{ Ok(0) }}
        pub fn firmware_running(&self) -> Result<u32> {{ Ok(0) }}
        pub fn log_timeout(&self, _dev: &()) {{}}
    }}

    pub struct GpuManagerG14V13_5 {{
        pub uat: mmu::Uat,
    }}
    impl GpuManagerG14V13_5 {{
        pub fn kernel_uat(&self) -> &mmu::Uat {{ &self.uat }}
    }}
}}

mod render {{
    use super::*;
    pub const INITBUF_AT: usize = 0xf00;
    pub const OWN_BARRIER_AT: usize = 0xf40;
    pub const OWN_BARRIER_SIZE: usize = 0x40;

    pub struct Common;
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub enum BufferOp {{
        Rebuild,
        Grow,
    }}
    pub struct Prep {{
        pub buffer_op: Option<BufferOp>,
        active_scenes: Arc<AtomicUsize>,
    }}
    impl Drop for Prep {{
        fn drop(&mut self) {{
            self.active_scenes.fetch_sub(1, Ordering::SeqCst);
        }}
    }}
    pub struct QueueRender {{
        pub prepare_calls: AtomicUsize,
        pub published_calls: AtomicUsize,
        pub active_scenes: Arc<AtomicUsize>,
        pub binding_initialized: AtomicBool,
        pub last_buffer_op: StdMutex<Option<Option<BufferOp>>>,
        pub fail_prepare: StdMutex<Option<Error>>,
    }}
    impl QueueRender {{
        pub fn new() -> Arc<Self> {{
            Arc::new(Self {{
                prepare_calls: AtomicUsize::new(0),
                published_calls: AtomicUsize::new(0),
                active_scenes: Arc::new(AtomicUsize::new(0)),
                binding_initialized: AtomicBool::new(false),
                last_buffer_op: StdMutex::new(None),
                fail_prepare: StdMutex::new(None),
            }})
        }}
        pub fn prepare(&self, _cmd: &crate::m3::agx_uapi::UapiRenderCommand, _usc: u64) -> Result<Prep> {{
            self.prepare_calls.fetch_add(1, Ordering::SeqCst);
            if let Some(err) = *self.fail_prepare.lock().unwrap() {{
                return Err(err);
            }}
            assert_eq!(
                self.active_scenes.fetch_add(1, Ordering::SeqCst),
                0,
                "previous Prep/Scene must have dropped before retry prepare()"
            );
            let op = if !self.binding_initialized.load(Ordering::SeqCst) {{
                Some(BufferOp::Rebuild)
            }} else {{
                None
            }};
            *self.last_buffer_op.lock().unwrap() = Some(op);
            Ok(Prep {{ buffer_op: op, active_scenes: self.active_scenes.clone() }})
        }}
        pub fn published(&self) {{
            self.published_calls.fetch_add(1, Ordering::SeqCst);
            self.binding_initialized.store(true, Ordering::SeqCst);
        }}
    }}

    pub fn encode_vertex(
        _cmd: &mut [u8], _stream: &mut [u8], _common: &Common, _tiler: &gpu::QueueInfo,
        _state: u64, _command: &crate::m3::agx_uapi::UapiRenderCommand, _scene: &Prep,
        _fs_stamp: u32, _fs_slot: u32, _ts: [u64; 2], _att: &(),
    ) -> Result {{ Ok(()) }}
    pub fn encode_buffer_op(_rec: &mut [u8], _common: &Common, _scene: &Prep, _op: BufferOp) {{}}
    pub fn encode_fragment(
        _cmd: &mut [u8], _stream: &mut [u8], _common: &Common, _frag: &gpu::QueueInfo,
        _state: u64, _command: &crate::m3::agx_uapi::UapiRenderCommand, _scene: &Prep,
        _ts: [u64; 2], _att: &(),
    ) -> Result {{ Ok(()) }}
    pub fn encode_own_barrier(_rec: &mut [u8], _tiler: &gpu::QueueInfo, _ta_s: u32, _fs_s: u32) {{}}
}}

mod m3 {{
    pub mod agx_compute {{
        #[derive(Copy, Clone, Default)]
        pub struct Helper;
        #[derive(Copy, Clone, Default)]
        pub struct Control {{
            pub base: u64,
            pub end: u64,
            pub usc_base: u64,
            pub helper: Helper,
            pub sampler_heap: u64,
            pub sampler_count: u32,
        }}
    }}
    pub mod agx_uapi {{
        #[derive(Copy, Clone, Default)]
        pub struct UapiRenderCommand;
    }}
}}

use gpu::{{CommandPool, ComputeJob, GpuManagerG14V13_5, QueueInfo, RootV27_2, FRAGMENT, POOL_SLOTS, TILER}};

#[derive(Copy, Clone)]
enum Command {{
    Compute(m3::agx_compute::Control),
    Render {{ command: m3::agx_uapi::UapiRenderCommand, usc: u64 }},
}}

struct Preempt(u64);
impl Preempt {{
    fn gpu_va(&self) -> GpuVa {{ GpuVa(self.0) }}
}}
struct GpuVa(u64);
impl GpuVa {{
    fn get(&self) -> u64 {{ self.0 }}
}}

struct Packet {{
    vm: mmu::Vm,
    commands: Vec<Command>,
    preempt: Option<Preempt>,
    render_queue: Option<Arc<render::QueueRender>>,
    render_attachments: Vec<[(); 2]>,
    published: AtomicBool,
    guard_released: AtomicBool,
    guard_pinned: AtomicBool,
    guard_quarantined: AtomicBool,
    finish_state: AtomicU8,
    result: StdMutex<Option<Result>>,
}}
impl Packet {{
    fn compute(vm: mmu::Vm, count: usize) -> Arc<Self> {{
        Arc::new(Self {{
            vm,
            commands: vec![Command::Compute(Default::default()); count],
            preempt: Some(Preempt(0xdead_0000)),
            render_queue: None,
            render_attachments: Vec::new(),
            published: AtomicBool::new(false),
            guard_released: AtomicBool::new(false),
            guard_pinned: AtomicBool::new(false),
            guard_quarantined: AtomicBool::new(false),
            finish_state: AtomicU8::new(0),
            result: StdMutex::new(None),
        }})
    }}
    fn compute_no_preempt(vm: mmu::Vm) -> Arc<Self> {{
        Arc::new(Self {{
            vm,
            commands: vec![Command::Compute(Default::default())],
            preempt: None,
            render_queue: None,
            render_attachments: Vec::new(),
            published: AtomicBool::new(false),
            guard_released: AtomicBool::new(false),
            guard_pinned: AtomicBool::new(false),
            guard_quarantined: AtomicBool::new(false),
            finish_state: AtomicU8::new(0),
            result: StdMutex::new(None),
        }})
    }}
    fn render(vm: mmu::Vm, rq: Arc<render::QueueRender>) -> Arc<Self> {{
        Arc::new(Self {{
            vm,
            commands: vec![Command::Render {{ command: Default::default(), usc: 0x1000 }}],
            preempt: None,
            render_queue: Some(rq),
            render_attachments: vec![[(), ()]],
            published: AtomicBool::new(false),
            guard_released: AtomicBool::new(false),
            guard_pinned: AtomicBool::new(false),
            guard_quarantined: AtomicBool::new(false),
            finish_state: AtomicU8::new(0),
            result: StdMutex::new(None),
        }})
    }}
    fn compute_then_render(vm: mmu::Vm, rq: Arc<render::QueueRender>) -> Arc<Self> {{
        Arc::new(Self {{
            vm,
            commands: vec![
                Command::Compute(Default::default()),
                Command::Render {{ command: Default::default(), usc: 0x1000 }},
            ],
            preempt: Some(Preempt(0xdead_0000)),
            render_queue: Some(rq),
            render_attachments: vec![[(), ()], [(), ()]],
            published: AtomicBool::new(false),
            guard_released: AtomicBool::new(false),
            guard_pinned: AtomicBool::new(false),
            guard_quarantined: AtomicBool::new(false),
            finish_state: AtomicU8::new(0),
            result: StdMutex::new(None),
        }})
    }}
    fn timestamp_addresses(&self, _idx: usize) -> [u64; 2] {{ [0, 0] }}
    fn render_timestamp_addresses(&self, _idx: usize) -> [[u64; 2]; 2] {{ [[0, 0], [0, 0]] }}
    fn mark_published(&self) {{ self.published.store(true, Ordering::SeqCst); }}
    fn release_guard(&self) {{
        if self.guard_pinned.load(Ordering::Acquire) {{
            return;
        }}
        self.guard_released.store(true, Ordering::SeqCst);
    }}
    fn retired(&self) {{ self.release_guard(); self.finish(Ok(())); }}
    fn finish(&self, res: Result) {{ self.complete(res, true); }}
    fn fail_packet(&self, err: Error) {{ self.complete(Err(err), false); }}
    fn complete(&self, res: Result, fail_vm: bool) {{
        const PENDING: u8 = 0;
        const PACKET_FAILED: u8 = 1;
        const TERMINAL: u8 = 2;
        let record_vm = |e: Error| {{
            if let Some(status) = self.vm.status() {{
                if fail_vm {{
                    status.record(e);
                }} else {{
                    status.record_if_device_loss(e);
                }}
            }}
        }};
        match res {{
            Ok(()) => {{
                if self.finish_state.swap(TERMINAL, Ordering::AcqRel) != PENDING {{
                    return;
                }}
            }}
            Err(e) if fail_vm => match self.finish_state.swap(TERMINAL, Ordering::AcqRel) {{
                PENDING => {{}}
                PACKET_FAILED => {{
                    if !self.guard_released.load(Ordering::SeqCst) {{
                        self.guard_pinned.store(true, Ordering::Release);
                        self.guard_quarantined.store(true, Ordering::SeqCst);
                    }}
                    record_vm(e);
                    return;
                }}
                _ => return,
            }},
            Err(_) => {{
                if self.finish_state.compare_exchange(PENDING, PACKET_FAILED, Ordering::AcqRel, Ordering::Acquire).is_err() {{
                    return;
                }}
            }}
        }}
        if let Err(err) = res {{
            if !self.guard_released.load(Ordering::SeqCst) {{
                if fail_vm {{
                    self.guard_pinned.store(true, Ordering::Release);
                }}
                self.guard_quarantined.store(true, Ordering::SeqCst);
            }}
            record_vm(err);
        }}
        *self.result.lock().unwrap() = Some(res);
    }}
    fn outcome(&self) -> Option<Result> {{ *self.result.lock().unwrap() }}
}}

struct Peer {{
    crashed: bool,
    kicks: Vec<u64>,
    send_err: Option<Error>,
}}
impl Peer {{
    fn new() -> Self {{ Self {{ crashed: false, kicks: Vec::new(), send_err: None }} }}
    fn as_mut(&mut self) -> &mut Self {{ self }}
    fn as_raw(&self) -> *mut core::ffi::c_void {{ core::ptr::null_mut() }}
    fn is_crashed(&self) -> bool {{ self.crashed }}
    fn send_message(&mut self, _ep: u8, msg: u64) -> Result {{
        if let Some(err) = self.send_err {{
            return Err(err);
        }}
        self.kicks.push(msg);
        Ok(())
    }}
}}

struct AscControl;
impl AscControl {{
    fn halt(&self) {{}}
}}

struct FaultStatus;
impl FaultStatus {{
    fn log(&self, _dev: &()) {{}}
}}

fn set_idle_policy(_root: &RootV27_2, _peer: &mut Peer, _allow_off: bool) -> Result {{ Ok(()) }}
fn wait(_root: &RootV27_2, _ms: i64, mut done: impl FnMut() -> Result<bool>) -> Result {{
    let _ = done()?;
    Ok(())
}}

struct Notify {{
    wakes: AtomicUsize,
    retries: AtomicUsize,
    detached: AtomicBool,
    messages: AtomicU64,
}}
impl Notify {{
    fn new() -> Arc<Self> {{
        Arc::new(Self {{
            wakes: AtomicUsize::new(0),
            retries: AtomicUsize::new(0),
            detached: AtomicBool::new(false),
            messages: AtomicU64::new(0),
        }})
    }}
    fn wake(&self) {{
        if !self.detached.load(Ordering::SeqCst) {{
            self.wakes.fetch_add(1, Ordering::SeqCst);
        }}
    }}
    fn retry(&self) {{
        if !self.detached.load(Ordering::SeqCst) {{
            self.retries.fetch_add(1, Ordering::SeqCst);
        }}
    }}
    fn detach(&self) -> Option<()> {{
        self.detached.store(true, Ordering::SeqCst);
        Some(())
    }}
}}

struct OwnedQueue;
impl OwnedQueue {{
    fn drain(&self) {{}}
}}

struct Work;
impl Work {{
    fn raw_get(_: *const ()) -> *mut () {{ core::ptr::null_mut() }}
}}
struct DelayedWork;
impl DelayedWork {{
    fn raw_as_work(_: &Self) -> *const () {{ core::ptr::null_mut() }}
}}

{g14_consts}
{fw_wait}
{vm_failed_fn}

mod g14_v27_2_abi {{
    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    pub enum TimeoutAction {{
        FailAll,
        Recover,
    }}
    pub fn command_timeout_action(phase: u32) -> TimeoutAction {{
        if phase == 1 {{ TimeoutAction::Recover }} else {{ TimeoutAction::FailAll }}
    }}
    pub fn guard_releasable(live: bool, kept: bool) -> bool {{
        !live && !kept
    }}
    #[derive(Default, Debug)]
    pub struct RecoveryLedger;
    impl RecoveryLedger {{
        pub fn report_answered(&mut self) -> bool {{ false }}
        pub fn last(&self) -> Option<u64> {{ None }}
    }}
}}

struct Inflight {{
    packet: Arc<Packet>,
    last: bool,
    publication_error: Option<Error>,
    stamp: event::EventValue,
    slot: usize,
    ordinal: u64,
    started: Instant<Monotonic>,
    doomed: Option<Error>,
    _bind: mmu::VmBind,
}}

struct RenderFlight {{
    packet: Arc<Packet>,
    last: bool,
    bind: mmu::VmBind,
    scene: render::Prep,
    slots: [usize; 2],
    stamps: [event::EventValue; 2],
    targets: [u32; 2],
    started: Instant<Monotonic>,
    doomed: Option<Error>,
}}

struct State {{
    peer: Peer,
    root: RootV27_2,
    pool: CommandPool,
    asc: AscControl,
    fault: FaultStatus,
    queue: QueueInfo,
    last: event::EventValue,
    sequence: u64,
    next_slot: usize,
    waiting_unlink: Option<FirmwareWait>,
    waiting_control: Option<FirmwareWait>,
    waiting_ring: Option<FirmwareWait>,
    published: u64,
    late: u64,
    busy: u64,
    skipped: u64,
    unacked: u64,
    deferred: u64,
    retired_total: u64,
    retired_raw: u32,
    capacity: u32,
    inflight: KVec<Inflight>,
    rendering: Option<RenderFlight>,
    pending: KVec<(Arc<Packet>, usize)>,
    accepting: bool,
    submitted: u64,
    retired: u64,
    firmware_error: Option<(Error, i32)>,
    ledger: g14_v27_2_abi::RecoveryLedger,
    events: [u32; EVENT_HISTORY],
    events_read: u64,
    kept: KVec<Arc<Packet>>,
    kept_compute: KVec<Inflight>,
    kept_renders: KVec<RenderFlight>,
    hardware_unreachable: bool,
}}

{state_impl}

impl State {{
    fn queue_stamp(&self) -> u32 {{
        self.root.queue().1.raw()
    }}
}}

struct Runtime {{
    dev: (),
    graph: Arc<GpuManagerG14V13_5>,
    healthy: AtomicBool,
    asleep: AtomicBool,
    deep_sleeping: AtomicBool,
    notify: Arc<Notify>,
    queue: OwnedQueue,
    state: Mutex<Option<State>>,
    idle: CondVar,
    retry: DelayedWork,
}}

impl Runtime {{
    fn new() -> Arc<Self> {{
        let root = RootV27_2::new();
        let (queue, last) = root.queue();
        Arc::new(Self {{
            dev: (),
            graph: Arc::new(GpuManagerG14V13_5 {{ uat: mmu::Uat::new() }}),
            healthy: AtomicBool::new(true),
            asleep: AtomicBool::new(false),
            deep_sleeping: AtomicBool::new(false),
            notify: Notify::new(),
            queue: OwnedQueue,
            state: Mutex::new(Some(State {{
                peer: Peer::new(),
                root,
                pool: CommandPool::new(),
                asc: AscControl,
                fault: FaultStatus,
                queue,
                last,
                sequence: 0,
                next_slot: 0,
                waiting_unlink: None,
                waiting_control: None,
                waiting_ring: None,
                published: 0,
                late: 0,
                busy: 0,
                skipped: 0,
                unacked: 0,
                deferred: 0,
                retired_total: 0,
                retired_raw: 0,
                capacity: 64,
                inflight: KVec::new(),
                rendering: None,
                pending: KVec::new(),
                accepting: true,
                submitted: 0,
                retired: 0,
                firmware_error: None,
                ledger: g14_v27_2_abi::RecoveryLedger::default(),
                events: [0; EVENT_HISTORY],
                events_read: 0,
                kept: KVec::new(),
                kept_compute: KVec::new(),
                kept_renders: KVec::new(),
                hardware_unreachable: false,
            }})),
            idle: CondVar::new(),
            retry: DelayedWork,
        }})
    }}

    fn process_events(&self, _state: &mut State) -> Result {{ Ok(()) }}
    fn fail_doomed(&self, _state: &mut State, _packet: &Arc<Packet>, _err: Error) {{}}
    fn keep<T>(&self, _list: &mut KVec<T>, _flight: T) {{}}
    fn recover(&self, _state: &mut State, _error: Error, _slot: i32) -> Result<bool> {{ Ok(false) }}
    fn log_silent_hang(&self, _state: &State, _work: &str) {{}}

{rt_body}
}}

fn test_render_transient_ebusy_precheck_retries_without_prepare_churn() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(1);
    let rq = render::QueueRender::new();
    let pkt = Packet::render(vm.clone(), rq.clone());

    // Both render queues were already announced by earlier renders; consumer cursor r lags.
    {{
        let mut g = rt.state.lock();
        let s = g.as_mut().unwrap();
        s.root.render_announced[TILER].store(true, Ordering::SeqCst);
        s.root.render_announced[FRAGMENT].store(true, Ordering::SeqCst);
        s.root.check_render_err = Some(EBUSY);
        s.root.stage_render_err[TILER] = Some(EBUSY);
    }}

    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(pkt.outcome().is_none(), "transient render EBUSY must NOT fail immediately");
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 0, "check_render must short-circuit before queue.prepare()");
        assert_eq!(s.pool.encode_calls, 0, "must not encode pool slots while ring is busy");
        assert_eq!(rt.graph.uat.bind_calls.load(Ordering::SeqCst), 0, "must not bind UAT while ring is busy");
        assert!(s.rendering.is_none());
        assert_eq!(s.pending.len(), 1);
        assert!(s.waiting_ring.is_some());
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 1, "transient EBUSY must arm delayed retry");
        assert_eq!(s.peer.kicks, vec![RootV27_2::render_kick(FRAGMENT), RootV27_2::render_kick(TILER)]);
    }}

    // Retry before RESOURCE_RETRY_MS elapses: no extra doorbell kicks, deadline intact, retry re-armed.
    let initial_since = rt.state.lock().as_ref().unwrap().waiting_ring.as_ref().unwrap().since;
    advance_ms(5);
    rt.service();
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(pkt.outcome().is_none());
        assert_eq!(s.waiting_ring.as_ref().unwrap().since, initial_since);
        assert_eq!(s.peer.kicks.len(), 2, "doorbell kicks must be rate-limited to RESOURCE_RETRY_MS");
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 2);
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 0);
    }}

    // Advance past RESOURCE_RETRY_MS: sends one more kick pair while still busy.
    advance_ms(RESOURCE_RETRY_MS as i64);
    rt.service();
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.peer.kicks.len(), 4);
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 3);
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 0);
    }}

    // Firmware advances consumer cursor r: next retry prepares and publishes render once.
    {{
        let mut g = rt.state.lock();
        let s = g.as_mut().unwrap();
        s.root.check_render_err = None;
        s.root.stage_render_err[TILER] = None;
    }}
    advance_ms(RESOURCE_RETRY_MS as i64);
    rt.service();
    let render_done = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 1);
        assert_eq!(rq.published_calls.load(Ordering::SeqCst), 1);
        assert!(s.rendering.is_some());
        assert!(s.pending.is_empty());
        assert!(s.waiting_ring.is_none());
        s.root.render_done_flags.clone()
    }};

    render_done[TILER].store(true, Ordering::SeqCst);
    render_done[FRAGMENT].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
    assert!(rt.healthy());
}}

fn test_render_transient_ebusy_after_prepare_unwinds_cleanly() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(2);
    let rq = render::QueueRender::new();
    let pkt = Packet::render(vm.clone(), rq.clone());

    // Pre-check passes, queue.prepare() succeeds (BufferOp::Rebuild), TA stages, then FS returns EBUSY.
    {{
        let mut g = rt.state.lock();
        let s = g.as_mut().unwrap();
        s.root.stage_render_err[FRAGMENT] = Some(EBUSY);
    }}

    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(pkt.outcome().is_none(), "post-prepare stage_render EBUSY must retry");
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 1);
        assert_eq!(rq.published_calls.load(Ordering::SeqCst), 0, "queue.published() must not run on EBUSY");
        assert_eq!(rq.active_scenes.load(Ordering::SeqCst), 0, "Prep/Scene must drop and unwind active_scenes");
        assert!(!rq.binding_initialized.load(Ordering::SeqCst), "binding must remain uninitialized for retry");
        assert_eq!(*rq.last_buffer_op.lock().unwrap(), Some(Some(render::BufferOp::Rebuild)));
        assert_eq!(rt.graph.uat.live_binds.load(Ordering::SeqCst), 0, "VmBind must drop on EBUSY");
        assert_eq!(s.pool.mark_published_calls, 0, "pool spare buffers must not advance on EBUSY");
        assert_eq!(s.busy, 0);
        assert_eq!(s.sequence, 0);
        assert_eq!(s.next_slot, 0);
        assert_eq!(s.submitted, 0);
        assert!(s.rendering.is_none());
        assert_eq!(s.pending.len(), 1);
        assert!(s.waiting_ring.is_some());
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 1);
    }}

    // Ring clears: retry re-prepares with BufferOp::Rebuild still preserved and succeeds.
    rt.state.lock().as_mut().unwrap().root.stage_render_err[FRAGMENT] = None;
    advance_ms(RESOURCE_RETRY_MS as i64);
    rt.service();
    let render_done = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 2);
        assert_eq!(rq.published_calls.load(Ordering::SeqCst), 1);
        assert_eq!(rq.active_scenes.load(Ordering::SeqCst), 1);
        assert!(rq.binding_initialized.load(Ordering::SeqCst));
        assert_eq!(*rq.last_buffer_op.lock().unwrap(), Some(Some(render::BufferOp::Rebuild)));
        assert!(s.rendering.is_some());
        assert!(s.pending.is_empty());
        assert!(s.waiting_ring.is_none());
        s.root.render_done_flags.clone()
    }};

    render_done[TILER].store(true, Ordering::SeqCst);
    render_done[FRAGMENT].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt.outcome(), Some(Ok(())));
    assert_eq!(rq.active_scenes.load(Ordering::SeqCst), 0);
    assert_eq!(vm.vm_status.count(), 0);
    assert!(rt.healthy());
}}

fn test_compute_transient_ebusy_idle_and_running() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(3);
    let pkt1 = Packet::compute(vm.clone(), 1);
    let pkt2 = Packet::compute(vm.clone(), 1);

    // Part A: !state.running() with compute ring EBUSY.
    rt.state.lock().as_mut().unwrap().root.check_compute_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt1.clone()), Ok(()));
    let since1 = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(pkt1.outcome().is_none(), "idle compute EBUSY must retry");
        assert_eq!(s.pool.encode_calls, 0);
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 1);
        assert_eq!(s.peer.kicks, vec![gpu::COMPUTE_KICK]);
        s.waiting_ring.as_ref().unwrap().since
    }};

    // Concurrent submit must NOT reset waiting_ring deadline.
    advance_ms(500);
    assert_eq!(rt.submit(pkt2.clone()), Ok(()));
    assert_eq!(
        rt.state.lock().as_ref().unwrap().waiting_ring.as_ref().unwrap().since,
        since1,
        "concurrent submit must not reset EBUSY wait deadline"
    );

    // Ring advances: both packets pipeline.
    rt.state.lock().as_mut().unwrap().root.check_compute_err = None;
    advance_ms(RESOURCE_RETRY_MS as i64);
    rt.service();
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 2);
        assert!(s.pending.is_empty());
        assert!(s.waiting_ring.is_none());
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};
    compute_stamp.store(0x200, Ordering::SeqCst);
    compute_retired.store(2, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt1.outcome(), Some(Ok(())));
    assert_eq!(pkt2.outcome(), Some(Ok(())));

    // Part B: state.running() == true when next compute hits EBUSY; retry MUST still arm
    // so if consumer lags past retirement without another IRQ, delayed service wakes and publishes.
    let pkt3 = Packet::compute(vm.clone(), 1);
    let pkt4 = Packet::compute(vm.clone(), 1);
    assert_eq!(rt.submit(pkt3.clone()), Ok(()));
    rt.state.lock().as_mut().unwrap().root.stage_compute_err = Some(EBUSY);
    let retries_before = rt.notify.retries.load(Ordering::SeqCst);
    assert_eq!(rt.submit(pkt4.clone()), Ok(()));
    assert!(rt.notify.retries.load(Ordering::SeqCst) > retries_before, "running compute EBUSY must still arm retry");
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_some());

    // pkt3 retires while ring consumer STILL lags -> pkt4 stays queued and re-arms retry.
    compute_stamp.store(0x300, Ordering::SeqCst);
    compute_retired.store(3, Ordering::SeqCst);
    let retries_before2 = rt.notify.retries.load(Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt3.outcome(), Some(Ok(())));
    assert!(pkt4.outcome().is_none());
    assert!(rt.notify.retries.load(Ordering::SeqCst) > retries_before2);

    // Ring clears -> pkt4 publishes and completes cleanly.
    rt.state.lock().as_mut().unwrap().root.stage_compute_err = None;
    advance_ms(RESOURCE_RETRY_MS as i64);
    rt.service();
    compute_stamp.store(0x400, Ordering::SeqCst);
    compute_retired.store(4, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt4.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
    assert!(rt.healthy());
}}

fn test_render_persistent_ebusy_timeout_leaves_vm_and_runtime_healthy() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(4);
    let rq = render::QueueRender::new();
    let pkt_r = Packet::render(vm.clone(), rq.clone());
    let pkt_c = Packet::compute(vm.clone(), 1);

    rt.state.lock().as_mut().unwrap().root.check_render_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt_r.clone()), Ok(()));
    advance_ms(100);
    assert_eq!(rt.submit(pkt_c.clone()), Ok(()));

    // One millisecond before deadline: still pending.
    advance_ms(COMMAND_TIMEOUT_MS - 101);
    rt.service();
    assert!(pkt_r.outcome().is_none(), "must not time out before COMMAND_TIMEOUT_MS");

    // At COMMAND_TIMEOUT_MS: render packet fails cleanly with EBUSY without poisoning VM or runtime,
    // and pump() advances to the following compute packet on the same VM.
    advance_ms(1);
    rt.service();
    assert_eq!(pkt_r.outcome(), Some(Err(EBUSY)));
    assert!(pkt_r.guard_released.load(Ordering::SeqCst));
    assert!(!pkt_r.guard_quarantined.load(Ordering::SeqCst));
    assert_eq!(vm.vm_status.count(), 0, "EBUSY timeout must not poison VmStatus");
    assert!(rt.healthy(), "EBUSY timeout must keep runtime healthy");

    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1, "subsequent compute packet on same VM must proceed");
        assert!(s.pending.is_empty());
        assert!(s.waiting_ring.is_none());
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_c.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
}}

fn test_compute_persistent_ebusy_timeout_leaves_vm_and_runtime_healthy() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(5);
    let rq = render::QueueRender::new();
    let pkt_c = Packet::compute(vm.clone(), 1);
    let pkt_r = Packet::render(vm.clone(), rq.clone());

    rt.state.lock().as_mut().unwrap().root.check_compute_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt_c.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_r.clone()), Ok(()));

    advance_ms(COMMAND_TIMEOUT_MS - 1);
    rt.service();
    assert!(pkt_c.outcome().is_none());

    advance_ms(1);
    rt.service();
    assert_eq!(pkt_c.outcome(), Some(Err(EBUSY)));
    assert!(pkt_c.guard_released.load(Ordering::SeqCst));
    assert_eq!(vm.vm_status.count(), 0);
    assert!(rt.healthy());

    let render_done = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(s.rendering.is_some(), "subsequent render packet on same VM must proceed");
        assert!(s.pending.is_empty());
        assert!(s.waiting_ring.is_none());
        s.root.render_done_flags.clone()
    }};
    render_done[TILER].store(true, Ordering::SeqCst);
    render_done[FRAGMENT].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_r.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
}}

fn test_pre_publication_errors_do_not_poison_vm_status() {{
    reset_clock();
    let rt = Runtime::new();
    let vm_a = mmu::Vm::new(6);
    let rq = render::QueueRender::new();

    // QueueRender::prepare() returns EINVAL -> fail_packet(EINVAL), VmStatus clean.
    *rq.fail_prepare.lock().unwrap() = Some(EINVAL);
    let pkt_bad_inval = Packet::render(vm_a.clone(), rq.clone());
    assert_eq!(rt.submit(pkt_bad_inval.clone()), Ok(()));
    assert_eq!(pkt_bad_inval.outcome(), Some(Err(EINVAL)));
    assert!(pkt_bad_inval.guard_released.load(Ordering::SeqCst));
    assert!(!pkt_bad_inval.guard_quarantined.load(Ordering::SeqCst));
    assert_eq!(vm_a.vm_status.count(), 0, "pre-publication EINVAL must NOT poison VmStatus");

    // QueueRender::prepare() returns ENOMEM -> fail_packet(ENOMEM), VmStatus clean.
    *rq.fail_prepare.lock().unwrap() = Some(ENOMEM);
    let pkt_bad_nomem = Packet::render(vm_a.clone(), rq.clone());
    assert_eq!(rt.submit(pkt_bad_nomem.clone()), Ok(()));
    assert_eq!(pkt_bad_nomem.outcome(), Some(Err(ENOMEM)));
    assert!(pkt_bad_nomem.guard_released.load(Ordering::SeqCst));
    assert_eq!(vm_a.vm_status.count(), 0, "pre-publication ENOMEM must NOT poison VmStatus");

    // Compute missing preempt -> fail_packet(EINVAL), VmStatus clean.
    let pkt_no_preempt = Packet::compute_no_preempt(vm_a.clone());
    assert_eq!(rt.submit(pkt_no_preempt.clone()), Ok(()));
    assert_eq!(pkt_no_preempt.outcome(), Some(Err(EINVAL)));
    assert!(pkt_no_preempt.guard_released.load(Ordering::SeqCst));
    assert_eq!(vm_a.vm_status.count(), 0);

    // Subsequent valid render on the SAME VM succeeds cleanly!
    *rq.fail_prepare.lock().unwrap() = None;
    let pkt_good = Packet::render(vm_a.clone(), rq.clone());
    assert_eq!(rt.submit(pkt_good.clone()), Ok(()));
    let render_done = rt.state.lock().as_ref().unwrap().root.render_done_flags.clone();
    render_done[TILER].store(true, Ordering::SeqCst);
    render_done[FRAGMENT].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_good.outcome(), Some(Ok(())));
    assert_eq!(vm_a.vm_status.count(), 0);
    assert!(rt.healthy());

    // Device-loss pre-publication error (EIO from corrupted ring geometry) still records on VmStatus.
    let vm_b = mmu::Vm::new(7);
    let rq_b = render::QueueRender::new();
    rt.state.lock().as_mut().unwrap().root.check_render_err = Some(EIO);
    let pkt_eio = Packet::render(vm_b.clone(), rq_b);
    assert_eq!(rt.submit(pkt_eio.clone()), Ok(()));
    assert_eq!(pkt_eio.outcome(), Some(Err(EIO)));
    assert!(pkt_eio.guard_released.load(Ordering::SeqCst));
    assert_eq!(vm_b.vm_status.errors(), vec![EIO], "device-loss EIO must still record on VmStatus");
}}

fn test_multi_command_packet_inflight_prefix_defers_fail_packet() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(8);
    // 3 compute commands: cmd 0 and cmd 1 publish, cmd 2 fails pre-publication with ENOMEM.
    let pkt = Packet::compute(vm.clone(), 3);
    rt.state.lock().as_mut().unwrap().pool.fail_encode_on_call = Some((3, ENOMEM));

    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 2, "cmd 0 and cmd 1 must remain in flight");
        assert!(s.pending.is_empty(), "packet must be removed from pending on fail_publication");
        assert!(s.inflight[0].publication_error.is_none());
        assert_eq!(s.inflight[1].publication_error, Some(ENOMEM));
        assert!(pkt.outcome().is_none(), "packet must not finish until published prefix retires");
        assert!(!pkt.guard_released.load(Ordering::SeqCst), "guard must remain held while prefix runs");
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};

    // Retire cmd 0: cmd 1 is still running -> guard stays held, packet still pending completion.
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert!(pkt.outcome().is_none());
    assert!(!pkt.guard_released.load(Ordering::SeqCst));
    assert_eq!(rt.state.lock().as_ref().unwrap().inflight.len(), 1);

    // Retire cmd 1 cleanly: guard releases and packet finishes via fail_packet(ENOMEM) without poisoning VM.
    compute_stamp.store(0x200, Ordering::SeqCst);
    compute_retired.store(2, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt.outcome(), Some(Err(ENOMEM)));
    assert!(pkt.guard_released.load(Ordering::SeqCst));
    assert!(!pkt.guard_quarantined.load(Ordering::SeqCst));
    assert_eq!(vm.vm_status.count(), 0, "clean prefix retirement + unpublished tail failure must NOT poison VmStatus");
    assert!(rt.healthy());

    // Subsequent packet from the same VM succeeds.
    rt.state.lock().as_mut().unwrap().pool.fail_encode_on_call = None;
    let pkt_next = Packet::compute(vm.clone(), 1);
    assert_eq!(rt.submit(pkt_next.clone()), Ok(()));
    compute_stamp.store(0x300, Ordering::SeqCst);
    compute_retired.store(3, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_next.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
}}

fn test_multi_command_packet_retired_prefix_then_tail_fails() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(9);
    let rq = render::QueueRender::new();
    *rq.fail_prepare.lock().unwrap() = Some(EINVAL);
    let pkt = Packet::compute_then_render(vm.clone(), rq.clone());

    // cmd 0 (compute) publishes; cmd 1 (render) waits for compute to retire.
    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1);
        assert_eq!(s.pending.len(), 1);
        assert_eq!(s.pending[0].1, 1);
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};

    // cmd 0 retires cleanly -> !state.owns(&pkt), then pump() attempts render cmd 1 which fails EINVAL.
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt.outcome(), Some(Err(EINVAL)));
    assert!(pkt.guard_released.load(Ordering::SeqCst));
    assert!(!pkt.guard_quarantined.load(Ordering::SeqCst));
    assert_eq!(vm.vm_status.count(), 0, "retired prefix + failed unpublished tail must NOT poison VmStatus");
    assert!(rt.healthy());
}}

fn test_multi_command_packet_inflight_prefix_hang_still_poisons_vm() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(10);
    let pkt = Packet::compute(vm.clone(), 2);
    rt.state.lock().as_mut().unwrap().pool.fail_encode_on_call = Some((2, ENOMEM));

    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    assert_eq!(rt.state.lock().as_ref().unwrap().inflight[0].publication_error, Some(ENOMEM));

    // cmd 0 hangs past COMMAND_TIMEOUT_MS -> real firmware timeout MUST poison VmStatus and quarantine guard.
    advance_ms(COMMAND_TIMEOUT_MS + 1);
    rt.service();
    assert!(!rt.healthy());
    assert_eq!(pkt.outcome(), Some(Err(ETIMEDOUT)));
    assert!(!pkt.guard_released.load(Ordering::SeqCst));
    assert!(pkt.guard_quarantined.load(Ordering::SeqCst));
    assert!(vm.vm_status.errors().contains(&ETIMEDOUT));
}}

fn test_withdraw_front_and_nonfront_during_ebusy_wait() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(11);
    let rq = render::QueueRender::new();
    let pkt_a = Packet::render(vm.clone(), rq.clone());
    let pkt_b = Packet::render(vm.clone(), rq.clone());
    let pkt_c = Packet::render(vm.clone(), rq.clone());

    rt.state.lock().as_mut().unwrap().root.check_render_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt_a.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_c.clone()), Ok(()));
    let initial_since = rt.state.lock().as_ref().unwrap().waiting_ring.as_ref().unwrap().since;

    // Non-front withdraw must NOT reset front packet's waiting_ring.
    advance_ms(1000);
    rt.withdraw(&pkt_c);
    assert!(pkt_c.guard_released.load(Ordering::SeqCst));
    assert_eq!(rt.state.lock().as_ref().unwrap().waiting_ring.as_ref().unwrap().since, initial_since);

    // Front withdraw at t=4000ms must clear waiting_ring and wake service for pkt_b.
    advance_ms(3000);
    let wakes_before = rt.notify.wakes.load(Ordering::SeqCst);
    rt.withdraw(&pkt_a);
    assert!(pkt_a.guard_released.load(Ordering::SeqCst));
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_none(), "withdrawing front must clear waiting_ring");
    assert!(rt.notify.wakes.load(Ordering::SeqCst) > wakes_before, "withdrawing front must wake service for successor");

    // Service runs at t=4000ms: pkt_b starts a fresh COMMAND_TIMEOUT_MS deadline (expires at t=9000ms).
    rt.service();
    assert!(pkt_b.outcome().is_none());
    advance_ms(2000); // t=6000ms: would expire if pkt_a's t=0 timestamp leaked to pkt_b!
    rt.service();
    assert!(pkt_b.outcome().is_none(), "successor packet must get its own fresh EBUSY deadline");

    // Withdrawing the last pending packet while idle clears waiting_ring and notifies idle.
    let notifies_before = rt.idle.notifies.load(Ordering::SeqCst);
    rt.withdraw(&pkt_b);
    assert!(rt.state.lock().as_ref().unwrap().pending.is_empty());
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_none());
    assert!(rt.idle.notifies.load(Ordering::SeqCst) > notifies_before);
}}

fn test_sleep_prepare_and_sleep_complete_during_ebusy_wait() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(12);
    let rq = render::QueueRender::new();
    let pkt = Packet::render(vm.clone(), rq.clone());

    rt.state.lock().as_mut().unwrap().root.check_render_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_some());

    // Shallow sleep_prepare succeeds immediately when !running and clears waiting_ring.
    assert_eq!(rt.sleep_prepare(false), Ok(()));
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_none());

    // Time spent asleep (60s) must neither arm retries nor expire the pending packet.
    let retries_before = rt.notify.retries.load(Ordering::SeqCst);
    advance_ms(60_000);
    rt.service();
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), retries_before);
    assert!(pkt.outcome().is_none());

    // On resume, packet starts a fresh retry window instead of timing out immediately.
    rt.sleep_complete();
    assert!(pkt.outcome().is_none(), "sleep duration must not trigger immediate EBUSY timeout");
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_some());
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), retries_before + 1);

    // Ring clears after resume -> packet publishes and completes cleanly.
    rt.state.lock().as_mut().unwrap().root.check_render_err = None;
    advance_ms(100);
    rt.service();
    let render_done = rt.state.lock().as_ref().unwrap().root.render_done_flags.clone();
    render_done[TILER].store(true, Ordering::SeqCst);
    render_done[FRAGMENT].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt.outcome(), Some(Ok(())));
    assert_eq!(vm.vm_status.count(), 0);
}}

fn test_stop_during_ebusy_wait_no_deadlock() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(13);
    let rq = render::QueueRender::new();
    let pkt = Packet::render(vm.clone(), rq);

    rt.state.lock().as_mut().unwrap().root.check_render_err = Some(EBUSY);
    assert_eq!(rt.submit(pkt.clone()), Ok(()));

    let (tx, rx) = std::sync::mpsc::channel();
    let rt_stop = rt.clone();
    std::thread::spawn(move || {{
        rt_stop.stop();
        let _ = tx.send(());
    }});
    rx.recv_timeout(Duration::from_millis(500))
        .expect("DEADLOCK: Runtime::stop() hung during EBUSY wait");
    assert!(!rt.healthy());
    assert!(rt.notify.detached.load(Ordering::SeqCst));
    assert_eq!(pkt.outcome(), Some(Err(ENODEV)));
    assert!(pkt.guard_released.load(Ordering::SeqCst));
}}

fn test_doorbell_kick_error_during_ebusy_fails_runtime() {{
    reset_clock();
    let rt = Runtime::new();
    let vm = mmu::Vm::new(14);
    let rq = render::QueueRender::new();
    let pkt = Packet::render(vm.clone(), rq);

    {{
        let mut g = rt.state.lock();
        let s = g.as_mut().unwrap();
        s.root.render_announced[TILER].store(true, Ordering::SeqCst);
        s.root.check_render_err = Some(EBUSY);
        s.peer.send_err = Some(EIO);
    }}
    assert_eq!(rt.submit(pkt.clone()), Ok(()));
    assert!(!rt.healthy());
    assert_eq!(pkt.outcome(), Some(Err(EIO)));
    assert!(rt.state.lock().as_ref().unwrap().waiting_ring.is_none());
}}

fn main() {{
    test_render_transient_ebusy_precheck_retries_without_prepare_churn();
    test_render_transient_ebusy_after_prepare_unwinds_cleanly();
    test_compute_transient_ebusy_idle_and_running();
    test_render_persistent_ebusy_timeout_leaves_vm_and_runtime_healthy();
    test_compute_persistent_ebusy_timeout_leaves_vm_and_runtime_healthy();
    test_pre_publication_errors_do_not_poison_vm_status();
    test_multi_command_packet_inflight_prefix_defers_fail_packet();
    test_multi_command_packet_retired_prefix_then_tail_fails();
    test_multi_command_packet_inflight_prefix_hang_still_poisons_vm();
    test_withdraw_front_and_nonfront_during_ebusy_wait();
    test_sleep_prepare_and_sleep_complete_during_ebusy_wait();
    test_stop_during_ebusy_wait_no_deadlock();
    test_doorbell_kick_error_during_ebusy_fails_runtime();
    println!("g14-v27-2-pump-ebusy-tests: all 13 cases passed");
}}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--out', type=Path, help='directory for generated Rust source and binary')
    ap.add_argument('--mutation', help='apply a negative-control mutation that must fail')
    args = ap.parse_args()

    src = build_harness(args.mutation)
    if args.out is not None:
        args.out.mkdir(parents=True, exist_ok=True)
        workdir = args.out
        tmp = None
    else:
        tmp = tempfile.TemporaryDirectory(prefix='g14-v27-2-pump-ebusy-')
        workdir = Path(tmp.name)

    try:
        rs = workdir / 'harness.rs'
        exe = workdir / 'harness'
        rs.write_text(src)
        rustc = os.environ.get('RUSTC', 'rustc')
        comp = subprocess.run(
            [rustc, '--edition=2021', str(rs), '-o', str(exe)],
            capture_output=True,
            text=True,
        )
        if comp.returncode != 0:
            sys.stderr.write(comp.stdout)
            sys.stderr.write(comp.stderr)
            return comp.returncode
        run = subprocess.run([str(exe)])
        return run.returncode
    finally:
        if tmp is not None:
            tmp.cleanup()


if __name__ == '__main__':
    sys.exit(main())
