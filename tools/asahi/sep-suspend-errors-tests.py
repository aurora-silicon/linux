#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile actual SEP suspend/error paths with host-only dependency stubs.

Extract C prepare/wait/noirq and Rust wrappers/enter/leave/exit/restart verbatim.
Kernel poll macros are also extracted verbatim; virtual time replaces sleeping.
These controls test error classification and AP decisions, not firmware or MMIO
safety against autonomous late gating. No target access or kernel build occurs.
"""
import argparse
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^.*\b(?:fn|int(?: __maybe_unused)?) ' + name + r'\([^;]*?\)\s*(?:->[^\{]+)?\{',
                      source, re.M)
    if not match:
        raise AssertionError(name)
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def macro(source, name):
    return re.search(r'^#define ' + name + r'\([^\n]*\n(?:.*\\\n)*.*',
                     source, re.M).group()


C_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef long long ktime_t;
#define BIT(n) (1u << (n))
#define SEP_PS_DEV_DISABLE BIT(10)
#define GENMASK(h, l) (((~0u) << (l)) & (~0u >> (31 - (h))))
#define SEP_PS_ACTUAL GENMASK(7, 4)
#define USEC_PER_MSEC 1000
#define READ_ONCE(x) (x)
#define IS_ENABLED(x) (config_enabled)
#define CONFIG_APPLE_SOC_SLEEP 1
#define __maybe_unused
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct device_node { struct device_node *parent; };
struct device { struct device_node *of_node; };
struct resource { unsigned long start; };
struct regmap { int unused; };
struct apple_mbox_msg { u64 msg0; u32 msg1; };
struct apple_mbox {};
static struct device_node parent, node = { &parent };
static struct device device = { &node }, dart;
static struct regmap regmap;
static bool config_enabled, hyp, sep_pm_suspending, dart_present, map_ok;
static int domain_error, read_error, read_error_after, register_error, gate_after, reads;
static int actual_until;
static int puts, maps, unmaps, registrations, lookups;
static ktime_t now;
static bool (*sep_pm_told)(void);
static bool yes(void) { return true; }
static bool pm_suspend_via_firmware(void) { return true; }
static bool is_kernel_in_hyp_mode(void) { return hyp; }
static int sep_pm_domain(void *p, struct regmap **m, u32 *o) {
    lookups++; *m = &regmap; *o = 0; return domain_error;
}
static int regmap_read(struct regmap *m, u32 o, u32 *v) {
    ++reads; if (read_error && reads >= read_error_after) return read_error;
    *v = gate_after && reads >= gate_after ? SEP_PS_DEV_DISABLE : 0;
    if (reads < actual_until) *v |= SEP_PS_ACTUAL;
    return 0;
}
#define might_sleep_if(x) ((void)0)
#define barrier() ((void)0)
#define cpu_relax() ((void)0)
static ktime_t ktime_get(void) { return now; }
static ktime_t ktime_add_us(ktime_t t, u64 us) { return t + us; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
static void usleep_range(unsigned long a, unsigned long b) { now += b; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int i) {
    lookups++; return domain_error ? NULL : &node;
}
static int of_property_read_u32(struct device_node *n, const char *p, u32 *v) { *v = 0; return 0; }
static int of_address_to_resource(struct device_node *n, int i, struct resource *r) { r->start = 0; return 0; }
static void of_node_put(struct device_node *n) {}
static unsigned resource_size(struct resource *r) { return 4; }
static struct device *sep_pm_dart(struct device *d) { return dart_present ? &dart : NULL; }
static void *dev_get_drvdata(struct device *d) { return d; }
static void *ioremap(unsigned long start, unsigned size) { ++maps; return map_ok ? &regmap : NULL; }
static void iounmap(void *p) { ++unmaps; }
static void put_device(struct device *d) { ++puts; }
static int sep_pm_commit_tell(void *data) { assert(0 && "registration must not send"); return 0; }
static int apple_sleep_enclave_register(struct device *d, int (*tell)(void *), void *data) {
    ++registrations; return register_error;
}
static struct sep_pm_commit {
    struct device *dev, *dart; struct apple_mbox *mbox; void *ps;
    struct apple_mbox_msg request; u64 reply, reply_mask;
    bool registered, sent;
} sep_pm_commit;
void fixture(void) {
    memset(&sep_pm_commit, 0, sizeof(sep_pm_commit));
    config_enabled = hyp = sep_pm_suspending = dart_present = map_ok = true;
    domain_error = read_error = register_error = gate_after = reads = 0;
    puts = maps = unmaps = registrations = lookups = 0; now = 0; read_error_after = 1;
    actual_until = 0;
    sep_pm_told = yes;
}
void parameter(int which, int value) {
    switch (which) {
    case 0: config_enabled = value; break; case 1: hyp = value; break;
    case 2: sep_pm_suspending = value; break; case 3: dart_present = value; break;
    case 4: map_ok = value; break; case 5: domain_error = value; break;
    case 6: read_error = value; break; case 7: register_error = value; break;
    case 8: gate_after = value; break; case 9: sep_pm_commit.registered = value; break;
    case 10: read_error_after = value; break; case 11: actual_until = value; break;
    }
}
int counter(int which) {
    switch (which) { case 0: return puts; case 1: return maps;
    case 2: return unmaps; case 3: return registrations; case 4: return lookups;
    default: return reads; }
}
void *device_pointer(void) { return &device; }
'''

RUST_PREFIX = r'''
#![allow(dead_code, unused_imports, unused_variables)]
use std::cell::{Cell, RefCell, RefMut};
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
#[derive(Clone, Copy, Debug, PartialEq, Eq)] struct Error(i32);
impl Error { fn from_errno(n: i32) -> Self { assert!(n < 0); Self(n) } }
type Result<T = ()> = std::result::Result<T, Error>;
const EBUSY: Error = Error(-16); const ENODEV: Error = Error(-19);
const ENOMEM: Error = Error(-12); const EIO: Error = Error(-5);
const ENOENT: Error = Error(-2); const ETIMEDOUT: Error = Error(-110);
const EOPNOTSUPP: Error = Error(-95); const ENOSYS: Error = Error(-38);
macro_rules! dev_err { ($($a:tt)*) => {{}}; }
macro_rules! dev_warn { ($($a:tt)*) => {{}}; }
macro_rules! dev_dbg { ($($a:tt)*) => {{}}; }
macro_rules! dev_info { ($($a:tt)*) => {{}}; }
extern "C" {
    fn fixture(); fn parameter(which: i32, value: i32); fn counter(which: i32) -> i32;
    fn device_pointer() -> *mut c_void;
    fn noirq() -> i32;
}
thread_local! { static REAL: Cell<bool> = const { Cell::new(false) }; }
mod bindings { pub unsafe fn pm_resume_via_firmware() -> bool { super::REAL.with(|v| v.get()) } }
mod kernel {
    pub mod device {
        pub struct Device;
        impl Device { pub fn as_raw(&self) -> *mut std::ffi::c_void { unsafe { crate::device_pointer() } } }
    }
    pub mod bindings { #[allow(non_camel_case_types)] pub enum apple_mbox {} }
    pub mod soc { pub mod apple { pub mod mailbox {
        pub struct Message { pub msg0: u64, pub msg1: u32 }
    } } }
    pub mod time {
        pub struct Delta; impl Delta { pub fn from_millis(_: i64) -> Self { Self } }
        pub mod delay { pub fn fsleep(_: super::Delta) {} }
    }
}
mod proto {
    pub const EP_CONTROL: u32 = 0; pub const CONTROL_TIMEOUT_MS: u32 = 5000;
    pub const TAG_SLEEP: u32 = 0; pub const SLEEP_REPLY: u64 = 0;
    pub const SLEEP_REPLY_MASK: u64 = 0;
    pub fn op_sleep() {}
    pub fn encode_control(_: &(), _: u32) -> crate::kernel::soc::apple::mailbox::Message {
        crate::kernel::soc::apple::mailbox::Message { msg0: 0, msg1: 0 }
    }
}
mod control {
    pub fn wait_idle<F: FnMut() -> bool, D: FnMut(u32)>(_: u32, _: u32, mut f: F, _: D) -> bool { !f() }
}
struct Lock<T>(RefCell<T>);
impl<T> Lock<T> { fn lock(&self) -> RefMut<'_, T> { self.0.borrow_mut() } }
struct Control; impl Control { fn requests_in_flight(&self) -> bool { false } }
struct Mbox;
impl Mbox {
    fn as_raw(&self) -> *mut kernel::bindings::apple_mbox { std::ptr::null_mut() }
    fn set_recv_enabled(&self, _: bool) {}
}
const PHASE_READY: u32 = 1;
struct SepData {
    dev: kernel::device::Device, phase: AtomicU32, control: Lock<Control>, mbox: Lock<Option<Mbox>>,
    gate: Cell<pm::SleepGate>, held: Cell<bool>, probes: Cell<u32>, requests: Cell<u32>,
    kicks: Cell<u32>, releases: Cell<u32>, finish: Cell<Option<bool>>, reply: Cell<Result<()>>,
}
impl SepData {
    fn new() -> Self {
        Self { dev: kernel::device::Device, phase: AtomicU32::new(PHASE_READY),
            control: Lock(RefCell::new(Control)), mbox: Lock(RefCell::new(Some(Mbox))),
            gate: Cell::new(pm::SleepGate::Open), held: Cell::new(false),
            probes: Cell::new(0), requests: Cell::new(0), kicks: Cell::new(0), releases: Cell::new(0),
            finish: Cell::new(None), reply: Cell::new(Ok(())) }
    }
    fn endpoint_present(&self, _: u32) -> bool { true }
    fn sleep_gate(&self) -> pm::SleepGate { self.gate.get() }
    fn set_sleep_gate(&self, g: pm::SleepGate) { self.gate.set(g); }
    fn sleep_hold_rx(&self) { self.held.set(true); }
    fn sleep_release_rx(&self) { self.held.set(false); self.releases.set(self.releases.get()+1); }
    fn sleep_request(&self) -> Result<()> { self.requests.set(self.requests.get()+1); Ok(()) }
    fn sleep_probe(&self) -> Result<()> { self.probes.set(self.probes.get()+1); self.sleep_release_rx(); self.reply.get() }
    fn sleep_kick(&self) -> Result<()> { self.kicks.set(self.kicks.get()+1); self.sleep_release_rx(); self.reply.get() }
}
'''

RUST_TESTS = r'''
fn reset() { unsafe { fixture() }; REAL.with(|v| v.set(false)); pm::FINISH.with(|v|v.set(None)); }
fn param(n: i32, v: i32) { unsafe { parameter(n, v) } }
fn count(n: i32) -> i32 { unsafe { counter(n) } }
#[test]
fn registration_failures_refuse_unsent() {
    for (p,v,e) in [(7,-16,EBUSY),(4,0,ENOMEM),(3,0,ENODEV),(5,-2,ENOENT),(9,1,EBUSY),(7,-19,ENODEV)] {
        reset(); param(p,v); let s=SepData::new();
        assert_eq!(s.sleep_enter(),Err(e));
        assert_eq!(s.gate.get(),pm::SleepGate::Open);
        assert_eq!(s.requests.get(),0); assert!(!s.held.get());
        assert_eq!(count(0), if p==7 || p==4 {1} else {0});
        assert_eq!(count(2), if p==7 {1} else {0});
    }
}
#[test]
fn explicit_unsupported_keeps_legacy_path() {
    for p in [0,1,2] {
        reset(); param(p,0); param(5,-2); let s=SepData::new();
        assert_eq!(s.sleep_enter(),Ok(())); assert_eq!(s.requests.get(),1);
        assert_eq!(s.gate.get(),pm::SleepGate::Told); assert!(s.held.get());
        assert_eq!(count(4),0); assert_eq!(count(1),0);
    }
    for e in [-95,-38] {
        reset(); param(7,e); let s=SepData::new();
        assert_eq!(s.sleep_enter(),Ok(())); assert_eq!(s.requests.get(),1);
        assert_eq!(count(0),1); assert_eq!(count(2),1);
    }
}
#[test]
fn successful_registration_defers_and_unsent_refusal_reopens() {
    reset(); let s=SepData::new(); assert_eq!(s.sleep_enter(),Ok(()));
    assert_eq!(s.requests.get(),0); assert_eq!(s.gate.get(),pm::SleepGate::Draining);
    assert!(s.held.get()); assert_eq!(count(3),1);
    pm::FINISH.with(|v| v.set(Some(false))); s.sleep_exit();
    assert_eq!(s.gate.get(),pm::SleepGate::Open); assert!(!s.held.get());
    assert_eq!(s.probes.get()+s.kicks.get(),0);
}
#[test]
fn missing_mailbox_is_an_error() {
    reset(); let s=SepData::new(); *s.mbox.lock()=None;
    assert_eq!(s.sleep_enter(),Err(ENODEV)); assert_eq!(s.requests.get(),0);
    assert_eq!(s.gate.get(),pm::SleepGate::Open);
}
#[test]
fn observation_errors_never_probe_or_release_rx() {
    for (p,e,at) in [(5,-2,1),(5,-5,1),(6,-5,1),(6,-110,1),(6,-5,3),(6,-110,3)] {
        reset(); param(p,e); param(10,at); let s=SepData::new();
        s.gate.set(pm::SleepGate::Told); s.held.set(true); pm::FINISH.with(|v| v.set(Some(true)));
        assert_eq!(pm::wait_gated(&s.dev),Err(Error(e)));
        s.sleep_exit(); assert_eq!(s.gate.get(),pm::SleepGate::Lost);
        assert!(s.held.get()); assert_eq!(s.probes.get()+s.kicks.get()+s.releases.get(),0);
    }
}
#[test]
fn deadline_expiry_preserves_existing_probe_outcomes() {
    for reply in [Ok(()),Err(EIO)] {
        reset(); let s=SepData::new(); s.gate.set(pm::SleepGate::Told); s.held.set(true); s.reply.set(reply);
        assert_eq!(pm::wait_gated(&s.dev),Ok(false)); assert!(count(5)>1);
        s.sleep_exit(); assert_eq!(s.probes.get(),1); assert_eq!(s.kicks.get(),0);
        assert_eq!(s.gate.get(),if reply.is_ok() {pm::SleepGate::Open} else {pm::SleepGate::Lost});
    }
}
#[test]
fn observed_gate_starts_the_enclave_as_a_real_wake_does() {
    for reply in [Ok(()),Err(ETIMEDOUT)] {
        reset(); param(8,3); let s=SepData::new(); s.gate.set(pm::SleepGate::Told); s.held.set(true);
        s.reply.set(reply); pm::FINISH.with(|v| v.set(Some(true)));
        s.sleep_exit();
        assert_eq!(s.kicks.get(),1); assert_eq!(s.probes.get(),0);
        if reply.is_ok() {
            assert_eq!(s.gate.get(),pm::SleepGate::Open); assert!(!s.held.get());
        } else {
            assert_eq!(s.gate.get(),pm::SleepGate::Lost); assert!(s.held.get());
        }
    }
}
#[test]
fn a_gated_domain_still_on_is_never_started() {
    reset(); param(8,3); param(11,i32::MAX); let s=SepData::new();
    s.gate.set(pm::SleepGate::Told); s.held.set(true);
    assert_eq!(pm::wait_gated(&s.dev),Ok(true));
    let before=count(5); assert_eq!(pm::wait_off(&s.dev),Ok(false)); assert!(count(5)-before>1);
    s.sleep_exit(); assert_eq!(s.gate.get(),pm::SleepGate::Lost); assert!(s.held.get());
    assert_eq!(s.probes.get()+s.kicks.get()+s.releases.get(),0);
}
#[test]
fn the_domain_going_off_late_still_starts_it() {
    reset(); param(8,3); param(11,40); let s=SepData::new();
    s.gate.set(pm::SleepGate::Told); s.held.set(true);
    s.sleep_exit(); assert_eq!(s.kicks.get(),1); assert_eq!(s.gate.get(),pm::SleepGate::Open);
}
#[test]
fn off_wait_errors_never_start_it() {
    for (e,at) in [(-5,4),(-110,4)] {
        reset(); param(8,3); param(6,e); param(10,at); let s=SepData::new();
        s.gate.set(pm::SleepGate::Told); s.held.set(true);
        s.sleep_exit(); assert_eq!(s.gate.get(),pm::SleepGate::Lost); assert!(s.held.get());
        assert_eq!(s.probes.get()+s.kicks.get()+s.releases.get(),0);
    }
}
#[test]
fn the_final_read_after_the_deadline_decides() {
    reset(); let s=SepData::new(); assert_eq!(pm::wait_gated(&s.dev),Ok(false)); let n=count(5);
    // A gate first seen at the read after the deadline counts.
    reset(); param(8,n); assert_eq!(pm::wait_gated(&s.dev),Ok(true)); assert_eq!(count(5),n);
    // One read later it was never seen.
    reset(); param(8,n+1); assert_eq!(pm::wait_gated(&s.dev),Ok(false)); assert_eq!(count(5),n);
    // That last read failing is an error, not a deadline.
    for e in [-5,-110] {
        reset(); param(6,e); param(10,n); assert_eq!(pm::wait_gated(&s.dev),Err(Error(e)));
    }
    // The same for the domain going off.
    reset(); param(8,1); param(11,n); assert_eq!(pm::wait_off(&s.dev),Ok(true)); assert_eq!(count(5),n);
    reset(); param(8,1); param(11,n+1); assert_eq!(pm::wait_off(&s.dev),Ok(false));
    reset(); param(8,1); param(11,i32::MAX); param(6,-5); param(10,n);
    assert_eq!(pm::wait_off(&s.dev),Err(EIO));
}
#[test]
fn real_resume_and_lost_refusal_keep_existing_branches() {
    reset(); let s=SepData::new(); s.gate.set(pm::SleepGate::Lost); s.held.set(true);
    assert_eq!(s.sleep_enter(),Ok(())); s.sleep_exit(); assert_eq!(s.requests.get()+s.probes.get()+s.kicks.get(),0);
    REAL.with(|v| v.set(true)); s.sleep_exit(); assert_eq!(s.kicks.get(),1);
    assert_eq!(s.gate.get(),pm::SleepGate::Open);
}
#[test]
fn noirq_preserves_errno_contract_and_legacy_missing_domain() {
    for (p,v,expected) in [(8,1,0),(8,0,-110),(5,-2,0),(6,-5,-5),(6,-110,-110)] {
        reset(); param(p,v); assert_eq!(unsafe { noirq() },expected);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--contracts', type=Path)
    parser.add_argument('--mutation', choices=['arm-fallback', 'read-as-deadline', 'probe-on-error',
                                               'no-capability-check', 'stay-lost', 'start-while-on',
                                               'off-ignores-actual'])
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    args.out.mkdir(parents=True, exist_ok=True)
    contracts = args.contracts or args.source
    pm = (args.source / 'drivers/soc/apple/pm.rs').read_text()
    sep = (args.source / 'drivers/soc/apple/sep.rs').read_text()
    shim = (args.source / 'drivers/soc/apple/pm_shim.c').read_text()
    if args.mutation == 'arm-fallback':
        pm = pm.replace('_ => Err(err),', '_ => Ok(false),')
    if args.mutation == 'read-as-deadline':
        shim = shim.replace('if (ret)\n\t\treturn ret;\n\n\treturn !!poll_ret;', 'if (ret)\n\t\treturn 1;\n\n\treturn !!poll_ret;')
    if args.mutation == 'probe-on-error':
        sep = sep.replace('Err(e) => Err(e),', 'Err(_e) => self.sleep_probe(),')
    if args.mutation == 'stay-lost':
        sep = sep.replace('Ok(true) => self.sleep_restart(),', 'Ok(true) => Err(ETIMEDOUT),')
    if args.mutation == 'start-while-on':
        sep = sep.replace('if !pm::wait_off(&self.dev)? {', 'if false {')
    if args.mutation == 'off-ignores-actual':
        shim = shim.replace('((val & SEP_PS_DEV_DISABLE) && !(val & SEP_PS_ACTUAL))', '(val & SEP_PS_DEV_DISABLE)')
    if args.mutation == 'no-capability-check':
        shim = shim.replace(' || !IS_ENABLED(CONFIG_APPLE_SOC_SLEEP) ||\n\t    !is_kernel_in_hyp_mode()', '')
    iopoll = (contracts / 'include/linux/iopoll.h').read_text()
    regmap = (contracts / 'include/linux/regmap.h').read_text()
    c = C_PREFIX + '\n' + '\n'.join([macro(iopoll, 'poll_timeout_us'), macro(iopoll, 'read_poll_timeout'), macro(regmap, 'regmap_read_poll_timeout')])
    c += '\n' + '\n'.join(function(shim, n) for n in ['sep_pm_wait_gated', 'sep_pm_wait_off', 'sep_pm_commit_prepare', 'sep_pm_suspend_noirq'])
    c += '\nint noirq(void) { return sep_pm_suspend_noirq(&device); }\n'
    cpath, obj = args.out / 'control.c', args.out / 'control.o'
    cpath.write_text(c)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-c', str(cpath), '-o', str(obj)], check=True)
    rust = RUST_PREFIX + '\nmod pm {\nuse crate::*;\nconst GATE_TIMEOUT_MS:u32=5000;\n'
    rust += '''extern "C" {
        fn sep_pm_wait_gated(dev:*mut c_void, ms:u32)->c_int;
        fn sep_pm_wait_off(dev:*mut c_void, ms:u32)->c_int;
        fn sep_pm_commit_prepare(dev:*mut c_void, mbox:*mut c_void, a:u64, b:u32, c:u64, d:u64)->c_int;
    }
    pub fn dart_prepare(_: &kernel::device::Device)->Result<()> { Ok(()) }
    pub fn commit_finish(_: &kernel::device::Device)->Option<bool> { FINISH.with(|v|v.get()) }
    thread_local! { pub static FINISH:Cell<Option<bool>>=const { Cell::new(None) }; }
'''
    rust += pm[pm.index('#[derive(Clone, Copy, Debug, PartialEq, Eq)]'):pm.index('/// Whether the enclave gated')]
    rust += '\n'.join(function(pm, n) for n in ['wait_gated', 'wait_off', 'leave_for_commit']) + '\n}\nimpl SepData {\n'
    rust += '\n'.join(function(sep, n) for n in ['sleep_enter', 'sleep_leave_for_commit', 'sleep_exit', 'sleep_restart'])
    rust += '\n}\n' + RUST_TESTS
    rpath, binary = args.out / 'control.rs', args.out / 'control'
    rpath.write_text(rust)
    subprocess.run(['rustc', '--edition=2021', '--test', str(rpath), '-C', f'link-arg={obj.resolve()}', '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--test-threads=1'], check=False)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
