#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for G14 firmware 27.2 non-blocking UAT binding under Runtime.state.

Extracts and compiles the production UAT binding, slot allocator, and G14 27.2
runtime state-machine paths from:
  - drivers/gpu/drm/asahi/slotalloc.rs (`SlotAllocator::{get,get_inner,try_get,
    get_inner_impl}`, `Drop for Guard`)
  - drivers/gpu/drm/asahi/mmu.rs (`Uat::{bind,try_bind,bind_inner}`,
    `Drop for VmBind`, `binding_wire_generation`, `application_ttb1`)
  - drivers/gpu/drm/asahi/g14_v27_2.rs (`COMMAND_TIMEOUT_MS`, `STOP_DRAIN_MS`,
    `RETIRE_WAIT_US`, `RESOURCE_RETRY_MS`, `PublishError`, `State` helpers,
    `Runtime::{submit,withdraw,fail_all,acknowledge,free_slots,try_bind_vm,
    handle_bind_busy,pump,publish_compute,publish_render,retire_slot,
    retire_render,advance,service,service_locked,sleep_prepare,sleep_complete,
    stop}`)

Verifies all positive scenarios and kills each negative-control mutation with
a Rust assertion panic (exit 101).
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


def strip_cfg_try_bind(text):
    return re.sub(r'#\[cfg\(.*\)\]\s*', '', text, count=1)


def build_harness(mutation=None):
    mmu_src = (ASAHI / 'mmu.rs').read_text()
    slot_src = (ASAHI / 'slotalloc.rs').read_text()
    g14_src = (ASAHI / 'g14_v27_2.rs').read_text()

    # Structural assertions on production sources.
    assert '#[cfg(any(CONFIG_DRM_ASAHI_G15, CONFIG_DRM_ASAHI_G14_V27_2))]' in mmu_src, (
        'Uat::try_bind must be gated on any(CONFIG_DRM_ASAHI_G15, CONFIG_DRM_ASAHI_G14_V27_2)'
    )
    assert '.bind(' not in g14_src, 'blocking .bind() call remains in g14_v27_2.rs'

    # Extract slotalloc items.
    slot_methods = '\n'.join([
        extract_item(slot_src, '    pub(crate) fn set_limit('),
        extract_item(slot_src, '    pub(crate) fn get('),
        extract_item(slot_src, '    pub(crate) fn get_inner('),
        extract_item(slot_src, '    pub(crate) fn try_get('),
        extract_item(slot_src, '    fn get_inner_impl('),
    ])
    slot_guard_drop = extract_item(slot_src, 'impl<T: SlotItem> Drop for Guard<T> {')

    # Extract mmu items.
    mmu_bind = extract_item(mmu_src, '    pub(crate) fn bind(&self, vm: &Vm)')
    mmu_try_bind = strip_cfg_try_bind(
        extract_item(mmu_src, '    pub(crate) fn try_bind(&self, vm: &Vm)', attributes=True)
    )
    mmu_bind_inner = extract_item(mmu_src, '    fn bind_inner(&self, vm: &Vm, wait: bool)')
    mmu_vmbind_drop = extract_item(mmu_src, 'impl Drop for VmBind {')
    mmu_wire_gen = extract_item(mmu_src, 'const fn binding_wire_generation(')
    mmu_app_ttb1 = extract_item(mmu_src, 'const fn application_ttb1(')

    # Extract g14_v27_2 items.
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
    publish_error = '\n'.join([
        extract_item(g14_src, 'enum PublishError {'),
        extract_item(g14_src, 'impl From<Error> for PublishError {'),
        extract_item(g14_src, 'impl From<kernel::alloc::AllocError> for PublishError {'),
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
        'try_bind_vm': extract_item(rt_impl, '    fn try_bind_vm(&self, vm: &mmu::Vm)'),
        'handle_bind_busy': extract_item(rt_impl, '    fn handle_bind_busy(&self, state: &mut State, packet: &Arc<Packet>) -> bool {'),
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

    # Verify publish_render binds before calling queue.prepare().
    bind_pos = rt_methods['publish_render'].index('try_bind_vm')
    prep_pos = rt_methods['publish_render'].index('queue.prepare(')
    assert bind_pos < prep_pos, 'publish_render must call try_bind_vm before queue.prepare'

    # Apply targeted negative-control mutations directly to extracted production code.
    if mutation == 'blocking-bind':
        old = 'self.graph.kernel_uat().try_bind(vm)'
        assert old in rt_methods['try_bind_vm']
        rt_methods['try_bind_vm'] = rt_methods['try_bind_vm'].replace(
            old, 'self.graph.kernel_uat().bind(vm)', 1
        )
    elif mutation == 'no-peer-try-bind':
        old = 'if self.inner.firmware.is_no_peer() {\n            return Err(ENOTSUPP);\n        }'
        assert old in mmu_bind_inner
        mmu_bind_inner = mmu_bind_inner.replace(old, '', 1)
        mmu_bind = mmu_bind.replace(
            'self.bind_inner(vm, true)',
            'if self.inner.firmware.is_no_peer() { return Err(ENOTSUPP); }\n        self.bind_inner(vm, true)',
            1,
        )
    elif mutation == 'fail-when-running':
        old = 'if state.running() {\n            state.waiting_bind = None;\n            return false;\n        }'
        assert old in rt_methods['handle_bind_busy']
        rt_methods['handle_bind_busy'] = rt_methods['handle_bind_busy'].replace(
            old,
            'if state.running() {\n            state.fail_bind_wait(packet);\n            return true;\n        }',
            1,
        )
    elif mutation == 'no-retry-arm':
        old = 'self.notify.retry();'
        assert old in rt_methods['handle_bind_busy']
        rt_methods['handle_bind_busy'] = rt_methods['handle_bind_busy'].replace(old, '', 1)
    elif mutation == 'no-bind-timeout':
        old = 'now - since >= Delta::from_millis(COMMAND_TIMEOUT_MS)'
        assert old in state_impl
        state_impl = state_impl.replace(old, 'let _ = (now, since); false', 1)
    elif mutation == 'poison-vm-on-timeout':
        old = 'packet.fail_packet(ENOSPC);'
        assert old in state_impl
        state_impl = state_impl.replace(old, 'packet.finish(Err(ENOSPC));', 1)
    elif mutation == 'prepare-before-bind':
        old = '        let bind = self.try_bind_vm(&packet.vm)?;\n        let scene = queue.prepare(command, usc)?;'
        assert old in rt_methods['publish_render']
        rt_methods['publish_render'] = rt_methods['publish_render'].replace(
            old,
            '        let scene = queue.prepare(command, usc)?;\n        let bind = self.try_bind_vm(&packet.vm)?;',
            1,
        )
    elif mutation == 'keep-waiting-bind-on-withdraw':
        old = '            if front_removed {\n                state.waiting_bind = None;\n            }'
        assert old in rt_methods['withdraw']
        rt_methods['withdraw'] = rt_methods['withdraw'].replace(old, '', 1)
    elif mutation == 'keep-waiting-bind-across-sleep':
        assert 'state.waiting_bind = None;' in rt_methods['sleep_prepare']
        assert 'state.waiting_bind = None;' in rt_methods['sleep_complete']
        rt_methods['sleep_prepare'] = rt_methods['sleep_prepare'].replace(
            'state.waiting_bind = None;', '',
        )
        rt_methods['sleep_complete'] = rt_methods['sleep_complete'].replace(
            'state.waiting_bind = None;', '',
        )
    elif mutation is not None:
        raise ValueError(f'unknown mutation: {mutation}')

    rt_body = '\n'.join(rt_methods.values())

    return f'''
#![allow(warnings)]
use core::num::NonZeroUsize;
use core::ops::Sub;
use core::sync::atomic::{{AtomicBool, AtomicI64, AtomicU32, AtomicU64, AtomicUsize, Ordering}};
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
pub const ERESTARTSYS: Error = Error(-512);
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
macro_rules! pr_err {{ ($($t:tt)*) => {{ () }}; }}
macro_rules! pr_warn {{ ($($t:tt)*) => {{ () }}; }}
macro_rules! pr_crit {{ ($($t:tt)*) => {{ () }}; }}
macro_rules! mod_pr_debug {{ ($($t:tt)*) => {{ () }}; }}

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
    pub fn notify_one(&self) {{
        self.notifies.fetch_add(1, Ordering::SeqCst);
        self.cv.notify_one();
    }}
    pub fn notify_all(&self) {{
        self.notifies.fetch_add(1, Ordering::SeqCst);
        self.cv.notify_all();
    }}
    pub fn wait_interruptible<'a, T>(&self, guard: &mut StdMutexGuard<'a, T>) -> bool {{
        // Real blocking wait (bounded to 5s only as process cleanup safety;
        // the test watchdog fails deadlocks in 400ms).
        let taken = unsafe {{ core::ptr::read(guard) }};
        let (new_guard, _) = self.cv.wait_timeout(taken, Duration::from_secs(5)).unwrap();
        unsafe {{ core::ptr::write(guard, new_guard) }};
        false
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

pub mod module_parameters {{
    use core::sync::atomic::{{AtomicU32, Ordering}};
    pub struct Param(pub AtomicU32);
    impl Param {{
        pub fn value(&self) -> u32 {{ self.0.load(Ordering::SeqCst) }}
        pub fn set(&self, v: u32) {{ self.0.store(v, Ordering::SeqCst); }}
    }}
    pub static robust_isolation: Param = Param(AtomicU32::new(0));
}}

mod mem {{
    pub fn sync() {{}}
    pub fn tlbi_asid(_: u8) {{}}
}}

mod slotalloc {{
    use super::*;

    pub trait SlotItem {{
        type Data;
        fn release(&mut self, _data: &mut Self::Data, _slot: u32) {{}}
    }}

    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    pub struct SlotToken {{
        time: u64,
        slot: u32,
    }}

    struct Entry<T> {{
        item: T,
        get_time: u64,
        drop_time: u64,
    }}

    struct SlotAllocatorInner<T: SlotItem> {{
        data: T::Data,
        slots: Vec<Option<Entry<T>>>,
        get_count: u64,
        drop_count: u64,
        slot_limit: usize,
    }}

    struct SlotAllocatorOuter<T: SlotItem> {{
        inner: Mutex<SlotAllocatorInner<T>>,
        cond: CondVar,
    }}

    pub struct SlotAllocator<T: SlotItem>(Arc<SlotAllocatorOuter<T>>);

    pub struct Guard<T: SlotItem> {{
        item: Option<T>,
        token: SlotToken,
        changed: bool,
        alloc: Arc<SlotAllocatorOuter<T>>,
    }}

    impl<T: SlotItem> Guard<T> {{
        pub fn slot(&self) -> u32 {{ self.token.slot }}
        pub fn token(&self) -> SlotToken {{ self.token }}
        pub fn changed(&self) -> bool {{ self.changed }}
    }}

    impl<T: SlotItem> SlotAllocator<T> {{
        pub fn new(num_slots: u32, data: T::Data, mut ctor: impl FnMut(&mut T::Data, u32) -> T) -> Self {{
            let mut slots = Vec::with_capacity(num_slots as usize);
            let mut data = data;
            for i in 0..num_slots {{
                let item = ctor(&mut data, i);
                slots.push(Some(Entry {{ item, get_time: 0, drop_time: 0 }}));
            }}
            Self(Arc::new(SlotAllocatorOuter {{
                inner: Mutex::new(SlotAllocatorInner {{
                    data,
                    slots,
                    get_count: 0,
                    drop_count: 0,
                    slot_limit: usize::MAX,
                }}),
                cond: CondVar::new(),
            }}))
        }}
{slot_methods}
    }}

{slot_guard_drop}
}}

mod mmu {{
    use super::*;
    use super::slotalloc::{{Guard, SlotAllocator, SlotItem, SlotToken}};

    const UAT_USER_CTX_START: usize = 1;
    const TTBR_VALID: u64 = 1;
    const TTBR_ASID_SHIFT: usize = 48;

    pub struct SlotItemUnit;
    impl SlotItem for SlotItemUnit {{
        type Data = ();
    }}

    #[derive(Clone, Copy, PartialEq, Eq)]
    pub enum UatFirmware {{
        Handoff,
        NoPeerRoots,
    }}
    impl UatFirmware {{
        pub const fn is_no_peer(self) -> bool {{
            matches!(self, Self::NoPeerRoots)
        }}
    }}

    pub struct SlotTTBS {{
        pub ttb0: AtomicU64,
        pub ttb1: AtomicU64,
    }}

    pub struct Handoff;
    impl Handoff {{
        pub fn current_slot(&self) -> Option<u32> {{ None }}
    }}

    pub struct UatShared {{
        pub ttbs: [SlotTTBS; 64],
        pub map_kernel_to_user: bool,
        pub kernel_ttb1: u64,
        pub binding_generations: [u64; 64],
        pub binding_published: [bool; 64],
        pub owned_user_roots: [u64; 64],
    }}
    impl UatShared {{
        pub fn ttbs(&self) -> &[SlotTTBS; 64] {{ &self.ttbs }}
        pub fn handoff(&self) -> Handoff {{ Handoff }}
        pub fn publish_slot(&self, _idx: usize) {{}}
    }}

    pub struct Tx<'a>(StdMutexGuard<'a, UatShared>);
    impl<'a> Tx<'a> {{
        pub fn shared_mut(&mut self) -> &mut UatShared {{ &mut self.0 }}
    }}

    pub struct UatInner {{
        pub firmware: UatFirmware,
        pub m4_adopted: bool,
        pub shared: Mutex<UatShared>,
    }}
    impl UatInner {{
        pub fn check_m4(&self) -> Result {{ Ok(()) }}
        pub fn lock_handoff(&self) -> Result<Tx<'_>> {{ Ok(Tx(self.shared.lock())) }}
    }}

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
        pub fn get(&self) -> i32 {{
            self.recorded.lock().unwrap().first().map_or(0, |e| e.0)
        }}
    }}

    pub struct VmBinding {{
        pub active_users: usize,
        pub bind_token: Option<SlotToken>,
        pub binding: Option<Guard<SlotItemUnit>>,
        pub generation: u8,
        pub ttb: u64,
    }}

    #[derive(Clone)]
    pub struct Vm {{
        pub id: u64,
        pub binding: Arc<Mutex<VmBinding>>,
        pub vm_status: Arc<VmStatus>,
    }}
    impl Vm {{
        pub fn new(id: u64) -> Self {{
            Self {{
                id,
                binding: Arc::new(Mutex::new(VmBinding {{
                    active_users: 0,
                    bind_token: None,
                    binding: None,
                    generation: 0,
                    ttb: 0x8000_0000 + id * 0x4000,
                }})),
                vm_status: VmStatus::new(),
            }}
        }}
        pub fn status(&self) -> Option<&Arc<VmStatus>> {{ Some(&self.vm_status) }}
    }}

    pub struct VmBind(pub Vm, pub u32);
    impl VmBind {{
        pub fn slot(&self) -> u32 {{ self.1 }}
    }}
{mmu_vmbind_drop}

{mmu_wire_gen}
{mmu_app_ttb1}

    pub struct Uat {{
        pub inner: Arc<UatInner>,
        pub slots: SlotAllocator<SlotItemUnit>,
    }}
    impl Uat {{
        pub fn new(num_slots: u32, firmware: UatFirmware) -> Self {{
            Self {{
                inner: Arc::new(UatInner {{
                    firmware,
                    m4_adopted: false,
                    shared: Mutex::new(UatShared {{
                        ttbs: core::array::from_fn(|_| SlotTTBS {{
                            ttb0: AtomicU64::new(0),
                            ttb1: AtomicU64::new(0),
                        }}),
                        map_kernel_to_user: true,
                        kernel_ttb1: 0x8_0000_0000,
                        binding_generations: [0; 64],
                        binding_published: [false; 64],
                        owned_user_roots: [0; 64],
                    }}),
                }}),
                slots: SlotAllocator::new(num_slots, (), |_, _| SlotItemUnit),
            }}
        }}
{mmu_bind}
{mmu_try_bind}
{mmu_bind_inner}
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
    }}
    impl CommandPool {{
        pub fn new() -> Self {{
            Self {{
                encode_calls: 0,
                mark_published_calls: 0,
                published_mask: 0,
                registered_mask: 0,
            }}
        }}
        pub fn encode(&mut self, slot: usize, _q: &QueueInfo, _stamp: u32, _job: &ComputeJob) -> Result<u64> {{
            self.encode_calls += 1;
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
        pub tgt: u32,
    }}
    impl Submission {{
        pub fn publish(self) {{
            self.published.fetch_add(1, Ordering::SeqCst);
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
        pub stage_compute_err: Option<Error>,
        pub stage_render_err: Option<Error>,
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
                stage_compute_err: None,
                stage_render_err: None,
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
        pub fn stage_compute(&self, _cmd: u64) -> Result<Submission> {{
            if let Some(err) = self.stage_compute_err {{
                return Err(err);
            }}
            Ok(Submission {{ published: self.compute_published.clone(), tgt: 1 }})
        }}
        pub fn render_queue(&self, dm: usize) -> (QueueInfo, event::EventValue) {{
            (
                QueueInfo {{ slot: dm as u32, compute_state: 0 }},
                event::EventValue(self.render_stamps[dm].load(Ordering::SeqCst)),
            )
        }}
        pub fn render_queue_announced(&self, _dm: usize) -> bool {{ false }}
        pub fn render_kick(dm: usize) -> u64 {{ 10 + dm as u64 }}
        pub fn stage_render(&self, dm: usize, _work: &[u64]) -> Result<Submission> {{
            if let Some(err) = self.stage_render_err {{
                return Err(err);
            }}
            let tgt = self.render_targets[dm].fetch_add(1, Ordering::SeqCst) + 1;
            Ok(Submission {{ published: self.render_published.clone(), tgt }})
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
    pub struct Prep {{
        pub buffer_op: Option<()>,
    }}
    pub struct QueueRender {{
        pub prepare_calls: AtomicUsize,
        pub published_calls: AtomicUsize,
        pub fail_prepare: StdMutex<Option<Error>>,
    }}
    impl QueueRender {{
        pub fn new() -> Arc<Self> {{
            Arc::new(Self {{
                prepare_calls: AtomicUsize::new(0),
                published_calls: AtomicUsize::new(0),
                fail_prepare: StdMutex::new(None),
            }})
        }}
        pub fn prepare(&self, _cmd: &crate::m3::agx_uapi::UapiRenderCommand, _usc: u64) -> Result<Prep> {{
            self.prepare_calls.fetch_add(1, Ordering::SeqCst);
            if let Some(err) = *self.fail_prepare.lock().unwrap() {{
                return Err(err);
            }}
            Ok(Prep {{ buffer_op: None }})
        }}
        pub fn published(&self) {{
            self.published_calls.fetch_add(1, Ordering::SeqCst);
        }}
    }}

    pub fn encode_vertex(
        _cmd: &mut [u8], _stream: &mut [u8], _common: &Common, _tiler: &gpu::QueueInfo,
        _state: u64, _command: &crate::m3::agx_uapi::UapiRenderCommand, _scene: &Prep,
        _fs_stamp: u32, _fs_slot: u32, _ts: [u64; 2], _att: &(),
    ) -> Result {{ Ok(()) }}
    pub fn encode_buffer_op(_rec: &mut [u8], _common: &Common, _scene: &Prep, _op: ()) {{}}
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
    guard_quarantined: AtomicBool,
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
            guard_quarantined: AtomicBool::new(false),
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
            guard_quarantined: AtomicBool::new(false),
            result: StdMutex::new(None),
        }})
    }}
    fn timestamp_addresses(&self, _idx: usize) -> [u64; 2] {{ [0, 0] }}
    fn render_timestamp_addresses(&self, _idx: usize) -> [[u64; 2]; 2] {{ [[0, 0], [0, 0]] }}
    fn mark_published(&self) {{ self.published.store(true, Ordering::SeqCst); }}
    fn release_guard(&self) {{ self.guard_released.store(true, Ordering::SeqCst); }}
    fn retired(&self) {{ self.release_guard(); self.finish(Ok(())); }}
    fn finish(&self, res: Result) {{ self.complete(res, true); }}
    fn fail_packet(&self, err: Error) {{ self.complete(Err(err), false); }}
    fn complete(&self, res: Result, fail_vm: bool) {{
        let mut slot = self.result.lock().unwrap();
        if slot.is_some() {{ return; }}
        if let Err(err) = res {{
            if !self.guard_released.load(Ordering::SeqCst) {{
                self.guard_quarantined.store(true, Ordering::SeqCst);
            }}
            if let Some(status) = self.vm.status() {{
                if fail_vm {{
                    status.record(err);
                }} else {{
                    status.record_if_device_loss(err);
                }}
            }}
        }}
        *slot = Some(res);
    }}
    fn outcome(&self) -> Option<Result> {{ *self.result.lock().unwrap() }}
}}

struct Peer {{
    crashed: bool,
    kicks: Vec<u64>,
}}
impl Peer {{
    fn new() -> Self {{ Self {{ crashed: false, kicks: Vec::new() }} }}
    fn as_mut(&mut self) -> &mut Self {{ self }}
    fn as_raw(&self) -> *mut core::ffi::c_void {{ core::ptr::null_mut() }}
    fn is_crashed(&self) -> bool {{ self.crashed }}
    fn send_message(&mut self, _ep: u8, msg: u64) -> Result {{
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
    fn raw_as_work(_: &Self) -> *const () {{ core::ptr::null() }}
}}

{g14_consts}
{fw_wait}
{publish_error}
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
    waiting_bind: Option<Instant<Monotonic>>,
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
    fn new(num_slots: u32, firmware: mmu::UatFirmware) -> Arc<Self> {{
        let root = RootV27_2::new();
        let (queue, last) = root.queue();
        Arc::new(Self {{
            dev: (),
            graph: Arc::new(GpuManagerG14V13_5 {{
                uat: mmu::Uat::new(num_slots, firmware),
            }}),
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
                waiting_bind: None,
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

fn test_try_bind_no_peer_guard() {{
    reset_clock();
    module_parameters::robust_isolation.set(0);
    let uat = mmu::Uat::new(4, mmu::UatFirmware::NoPeerRoots);
    let vm = mmu::Vm::new(1);
    assert_eq!(uat.bind(&vm).err(), Some(ENOTSUPP), "bind() must reject NoPeerRoots");
    assert_eq!(uat.try_bind(&vm).err(), Some(ENOTSUPP), "try_bind() must reject NoPeerRoots");
}}

fn test_robust_isolation_compute_concurrent_no_deadlock() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_a = mmu::Vm::new(1);
    let vm_b = mmu::Vm::new(2);
    let pkt_a = Packet::compute(vm_a.clone(), 1);
    let pkt_b = Packet::compute(vm_b.clone(), 1);

    // Client A takes the single robust_isolation UAT slot.
    assert_eq!(rt.submit(pkt_a.clone()), Ok(()));
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1);
        assert_eq!(s.pending.len(), 0);
        assert_eq!(s.submitted, 1);
        assert_eq!(s.sequence, 1);
        assert_eq!(s.next_slot, 1);
        assert_eq!(s.pool.encode_calls, 1);
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};

    // Client B submits on Thread 1 while Client A holds the only UAT slot.
    let (tx_b, rx_b) = std::sync::mpsc::channel();
    let rt_b = rt.clone();
    let pkt_b_t = pkt_b.clone();
    std::thread::spawn(move || {{
        let res = rt_b.submit(pkt_b_t);
        let _ = tx_b.send(res);
    }});

    // Under blocking bind(), Thread 1 stalls forever holding Runtime.state.
    let sub_res = rx_b
        .recv_timeout(Duration::from_millis(400))
        .expect("DEADLOCK: Client B submit() blocked under Runtime.state mutex");
    assert_eq!(sub_res, Ok(()));

    // Verify Client B stayed queued cleanly without mutating slots/counters/pool or failing.
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1, "only Client A should be in flight");
        assert_eq!(s.pending.len(), 1, "Client B must remain queued in pending");
        assert_eq!(s.submitted, 1, "Client B must not advance submitted counter");
        assert_eq!(s.sequence, 1, "Client B must not advance sequence");
        assert_eq!(s.next_slot, 1, "Client B must not advance next_slot");
        assert_eq!(s.pool.encode_calls, 1, "Client B must not encode before bind succeeds");
        assert_eq!(s.busy, 1, "only slot 0 should be busy");
        assert!(s.waiting_bind.is_none(), "waiting_bind must stay None while running");
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 0, "no retry timer needed while running");
        assert!(pkt_b.outcome().is_none(), "Client B must not fail while Client A is in flight");
    }}

    // Complete Client A on GPU and run Runtime::service() on Thread 2.
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    let (tx_s, rx_s) = std::sync::mpsc::channel();
    let rt_s = rt.clone();
    std::thread::spawn(move || {{
        rt_s.service();
        let _ = tx_s.send(());
    }});
    rx_s.recv_timeout(Duration::from_millis(400))
        .expect("DEADLOCK: Runtime::service() blocked acquiring Runtime.state");

    assert_eq!(pkt_a.outcome(), Some(Ok(())));
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1, "Client B should now be published");
        assert_eq!(s.pending.len(), 0, "pending queue should now be empty");
        assert_eq!(s.submitted, 2);
        assert_eq!(s.pool.encode_calls, 2);
    }}

    // Complete Client B on GPU and verify clean retirement.
    compute_stamp.store(0x200, Ordering::SeqCst);
    compute_retired.store(2, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_b.outcome(), Some(Ok(())));
    assert_eq!(vm_a.vm_status.count(), 0);
    assert_eq!(vm_b.vm_status.count(), 0);
    assert!(rt.healthy());
}}

fn test_same_vm_pipelining_under_robust_isolation() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_a = mmu::Vm::new(1);
    let vm_b = mmu::Vm::new(2);
    let pkt_a1 = Packet::compute(vm_a.clone(), 1);
    let pkt_a2 = Packet::compute(vm_a.clone(), 1);
    let pkt_b = Packet::compute(vm_b.clone(), 1);

    assert_eq!(rt.submit(pkt_a1.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_a2.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));

    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 2, "same-VM compute must pipeline under robust_isolation");
        assert_eq!(s.pending.len(), 1, "different-VM Client B waits in pending");
        assert_eq!(vm_a.binding.lock().active_users, 2);
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};

    // Retiring A1 drops active_users to 1; slot is still held by A2, B stays queued.
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_a1.outcome(), Some(Ok(())));
    assert!(pkt_b.outcome().is_none());
    assert_eq!(rt.state.lock().as_ref().unwrap().inflight.len(), 1);
    assert_eq!(rt.state.lock().as_ref().unwrap().pending.len(), 1);

    // Retiring A2 frees the UAT slot; service() -> pump() immediately publishes B.
    compute_stamp.store(0x200, Ordering::SeqCst);
    compute_retired.store(2, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_a2.outcome(), Some(Ok(())));
    assert_eq!(rt.state.lock().as_ref().unwrap().inflight.len(), 1);
    assert_eq!(rt.state.lock().as_ref().unwrap().pending.len(), 0);

    compute_stamp.store(0x300, Ordering::SeqCst);
    compute_retired.store(3, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_b.outcome(), Some(Ok(())));
}}

fn test_render_bind_busy_does_not_prepare_or_mutate() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_r = mmu::Vm::new(3);
    let rq = render::QueueRender::new();
    let pkt_r = Packet::render(vm_r.clone(), rq.clone());

    // External holder takes the single slot while runtime is idle.
    let ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_r.clone()), Ok(()));

    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 0, "queue.prepare() must not run on ENOSPC");
        assert_eq!(s.pool.encode_calls, 0, "render must not encode before bind succeeds");
        assert!(s.rendering.is_none());
        assert_eq!(s.pending.len(), 1);
        assert!(s.waiting_bind.is_some());
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 1);
    }}

    // Release external slot before timeout; delayed retry binds and publishes render once.
    advance_ms(RESOURCE_RETRY_MS as i64);
    drop(ext_bind);
    rt.service();
    let render_done = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(rq.prepare_calls.load(Ordering::SeqCst), 1);
        assert_eq!(rq.published_calls.load(Ordering::SeqCst), 1);
        assert!(s.rendering.is_some());
        assert!(s.pending.is_empty());
        assert!(s.waiting_bind.is_none());
        s.root.render_done_flags.clone()
    }};

    render_done[0].store(true, Ordering::SeqCst);
    render_done[1].store(true, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_r.outcome(), Some(Ok(())));
    assert_eq!(vm_r.vm_status.count(), 0);
}}

fn test_render_prepare_enospc_not_misclassified_as_bind_busy() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_r = mmu::Vm::new(4);
    let rq = render::QueueRender::new();
    *rq.fail_prepare.lock().unwrap() = Some(ENOSPC);
    let pkt_r = Packet::render(vm_r.clone(), rq.clone());

    assert_eq!(rt.submit(pkt_r.clone()), Ok(()));
    assert_eq!(pkt_r.outcome(), Some(Err(ENOSPC)), "allocator ENOSPC in prepare() must fail immediately");
    {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert!(s.pending.is_empty());
        assert!(s.waiting_bind.is_none());
        assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 0);
    }}
    // UAT slot must have been released when prepare() failed.
    let vm_other = mmu::Vm::new(5);
    assert!(rt.graph.kernel_uat().try_bind(&vm_other).is_ok());
}}

fn test_idle_external_hold_bounded_retry_recovers() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_b = mmu::Vm::new(10);
    let pkt_b = Packet::compute(vm_b.clone(), 1);

    let ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 1, "idle ENOSPC must arm retry");

    advance_ms(1000);
    rt.service();
    assert!(pkt_b.outcome().is_none());
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), 2, "retry must rearm before deadline");

    advance_ms(1000);
    drop(ext_bind);
    rt.service();
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1);
        assert!(s.pending.is_empty());
        assert!(s.waiting_bind.is_none());
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};

    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_b.outcome(), Some(Ok(())));
    assert_eq!(vm_b.vm_status.count(), 0);
    assert!(rt.healthy());
}}

fn test_idle_external_hold_deadline_fails_cleanly_without_poisoning() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_b = mmu::Vm::new(11);
    let pkt_b = Packet::compute(vm_b.clone(), 1);
    let pkt_ext = Packet::compute(vm_ext.clone(), 1);

    let _ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));
    advance_ms(100);
    assert_eq!(rt.submit(pkt_ext.clone()), Ok(()));

    // Just before deadline: still pending, no failure yet.
    advance_ms(COMMAND_TIMEOUT_MS - 101);
    rt.service();
    assert!(pkt_b.outcome().is_none(), "must not time out before COMMAND_TIMEOUT_MS");

    // At deadline: B fails cleanly with ENOSPC, VM status unpoisoned, runtime healthy,
    // and pump() advances to pkt_ext (whose VM already owns the slot).
    advance_ms(1);
    rt.service();
    assert_eq!(pkt_b.outcome(), Some(Err(ENOSPC)), "persistent external hold must fail packet with ENOSPC");
    assert!(pkt_b.guard_released.load(Ordering::SeqCst), "unpublished packet guard must be released");
    assert!(!pkt_b.guard_quarantined.load(Ordering::SeqCst), "unpublished packet guard must not be quarantined");
    assert_eq!(vm_b.vm_status.count(), 0, "innocent VM status must NOT be poisoned on ENOSPC timeout");
    assert!(rt.healthy(), "runtime must remain healthy after bind-slot timeout");

    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1, "subsequent packet on bound VM must proceed");
        assert!(s.pending.is_empty());
        assert!(s.waiting_bind.is_none());
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_ext.outcome(), Some(Ok(())));
}}

fn test_withdraw_front_and_nonfront_during_bind_wait() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_b = mmu::Vm::new(20);
    let vm_c = mmu::Vm::new(21);
    let vm_d = mmu::Vm::new(22);
    let pkt_b = Packet::compute(vm_b.clone(), 1);
    let pkt_c = Packet::compute(vm_c.clone(), 1);
    let pkt_d = Packet::compute(vm_d.clone(), 1);

    let _ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_c.clone()), Ok(()));
    assert_eq!(rt.submit(pkt_d.clone()), Ok(()));
    let initial_wait = rt.state.lock().as_ref().unwrap().waiting_bind;
    assert!(initial_wait.is_some());

    // Withdrawing a non-front packet must NOT reset front packet's waiting_bind.
    advance_ms(1000);
    rt.withdraw(&pkt_d);
    assert!(pkt_d.guard_released.load(Ordering::SeqCst));
    assert_eq!(rt.state.lock().as_ref().unwrap().waiting_bind, initial_wait);

    // Advance to 4000ms and withdraw front packet B: waiting_bind must clear and wake service.
    advance_ms(3000);
    let wakes_before = rt.notify.wakes.load(Ordering::SeqCst);
    rt.withdraw(&pkt_b);
    assert!(pkt_b.guard_released.load(Ordering::SeqCst));
    assert!(rt.state.lock().as_ref().unwrap().waiting_bind.is_none(), "withdrawing front must clear waiting_bind");
    assert!(rt.notify.wakes.load(Ordering::SeqCst) > wakes_before, "withdrawing front must wake service for successor");

    // Service runs at t=4000ms: C starts a fresh COMMAND_TIMEOUT_MS deadline (expires at t=9000ms).
    rt.service();
    assert!(pkt_c.outcome().is_none());
    advance_ms(2000); // t=6000ms: would have expired if B's t=0 timestamp leaked to C!
    rt.service();
    assert!(pkt_c.outcome().is_none(), "successor packet must get its own fresh retry deadline");

    // Withdrawing the last pending packet while idle clears waiting_bind and notifies idle.
    let notifies_before = rt.idle.notifies.load(Ordering::SeqCst);
    rt.withdraw(&pkt_c);
    assert!(rt.state.lock().as_ref().unwrap().pending.is_empty());
    assert!(rt.state.lock().as_ref().unwrap().waiting_bind.is_none());
    assert!(rt.idle.notifies.load(Ordering::SeqCst) > notifies_before);
}}

fn test_sleep_prepare_and_sleep_complete_during_bind_wait() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_b = mmu::Vm::new(30);
    let pkt_b = Packet::compute(vm_b.clone(), 1);

    let ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));
    assert!(rt.state.lock().as_ref().unwrap().waiting_bind.is_some());

    // Shallow sleep_prepare succeeds immediately when !running and clears waiting_bind.
    assert_eq!(rt.sleep_prepare(false), Ok(()));
    assert!(rt.state.lock().as_ref().unwrap().waiting_bind.is_none());

    // Time spent asleep (60s) must not arm retries or fail pending packet.
    let retries_before = rt.notify.retries.load(Ordering::SeqCst);
    advance_ms(60_000);
    rt.service();
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), retries_before);
    assert!(pkt_b.outcome().is_none());

    // On resume, packet gets a fresh retry window instead of timing out immediately.
    rt.sleep_complete();
    assert!(pkt_b.outcome().is_none(), "sleep duration must not trigger immediate bind-wait timeout");
    assert!(rt.state.lock().as_ref().unwrap().waiting_bind.is_some());
    assert_eq!(rt.notify.retries.load(Ordering::SeqCst), retries_before + 1);

    // Slot released after resume -> packet completes cleanly.
    advance_ms(100);
    drop(ext_bind);
    rt.service();
    let (compute_retired, compute_stamp) = {{
        let g = rt.state.lock();
        let s = g.as_ref().unwrap();
        assert_eq!(s.inflight.len(), 1);
        (s.root.compute_retired.clone(), s.root.compute_stamp.clone())
    }};
    compute_stamp.store(0x100, Ordering::SeqCst);
    compute_retired.store(1, Ordering::SeqCst);
    rt.service();
    assert_eq!(pkt_b.outcome(), Some(Ok(())));
}}

fn test_stop_during_bind_wait_no_deadlock() {{
    reset_clock();
    module_parameters::robust_isolation.set(1);
    let rt = Runtime::new(4, mmu::UatFirmware::Handoff);
    let vm_ext = mmu::Vm::new(99);
    let vm_b = mmu::Vm::new(40);
    let pkt_b = Packet::compute(vm_b.clone(), 1);

    let _ext_bind = rt.graph.kernel_uat().try_bind(&vm_ext).unwrap();
    assert_eq!(rt.submit(pkt_b.clone()), Ok(()));

    let (tx, rx) = std::sync::mpsc::channel();
    let rt_stop = rt.clone();
    std::thread::spawn(move || {{
        rt_stop.stop();
        let _ = tx.send(());
    }});
    rx.recv_timeout(Duration::from_millis(500))
        .expect("DEADLOCK: Runtime::stop() hung during bind wait");
    assert!(!rt.healthy());
    assert!(rt.notify.detached.load(Ordering::SeqCst));
    assert_eq!(pkt_b.outcome(), Some(Err(ENODEV)));
    assert!(pkt_b.guard_released.load(Ordering::SeqCst));
}}

fn main() {{
    test_try_bind_no_peer_guard();
    test_robust_isolation_compute_concurrent_no_deadlock();
    test_same_vm_pipelining_under_robust_isolation();
    test_render_bind_busy_does_not_prepare_or_mutate();
    test_render_prepare_enospc_not_misclassified_as_bind_busy();
    test_idle_external_hold_bounded_retry_recovers();
    test_idle_external_hold_deadline_fails_cleanly_without_poisoning();
    test_withdraw_front_and_nonfront_during_bind_wait();
    test_sleep_prepare_and_sleep_complete_during_bind_wait();
    test_stop_during_bind_wait_no_deadlock();
    println!("g14-v27-2-try-bind-tests: all 10 cases passed");
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
        tmp = tempfile.TemporaryDirectory(prefix='g14-v27-2-try-bind-')
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
