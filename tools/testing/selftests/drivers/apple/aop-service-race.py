#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real AOP admission and device-core attach/retire functions.

Only allocation, PM, bus callbacks and device storage are host fixtures. The
admission methods, device lock helpers, probe admission, attach and retirement
functions are compiled directly from the production sources.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def extract(source, name):
    declaration = re.search(
        r"(?m)^(?:[ \t]*(?:pub )?(?:unsafe )?fn |"
        r"(?:static )?(?:int |void |const char \*))" + re.escape(name) + r"\(", source)
    if declaration is None:
        raise ValueError("missing definition: " + name)
    start, at = declaration.start(), declaration.end()
    opening = source.index("{", at)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def structure(source, name):
    start = source.index("pub struct " + name + " {")
    end = source.index("\n}", start) + 2
    return "#[repr(C)]\n" + source[start:end]


def main():
    cc, rustc = os.environ.get("CC", "cc"), os.environ.get("RUSTC", "rustc")
    if not shutil.which(cc) or not shutil.which(rustc):
        print("SKIP: host C and Rust compilers required")
        return 4
    core = (ROOT / "drivers/base/dd.c").read_text()
    methods = (ROOT / "rust/kernel/soc/apple/aop.rs").read_text()
    gate = methods[methods.index("impl ServiceGate {"):methods.index("/// Stable child-owned")]
    gate = "\n".join(extract(gate, name) for name in ("acquire", "ready", "close"))
    context = extract(methods[methods.index("impl ServiceContext {"):], "acquire")
    lookup = extract(methods, "from_child")
    audio_lookup = extract(methods, "audio_from_child")
    abi_start = methods.index("pub enum ServiceABI {")
    abi_end = methods.index("\n}", abi_start) + 2
    abi = "#[derive(Clone, Copy, PartialEq, Eq)]\n" + methods[abi_start:abi_end]
    abi += "\nimpl ServiceABI {\n" + extract(methods, "publishes_children") + "\n}"
    payload = "\n".join(structure(methods, name) for name in (
        "EPICService", "ServicePlatformData"))
    functions = "\n".join(extract(core, name) for name in (
        "__device_prepare_driver_override", "__device_install_driver_override",
        "__device_driver_lock", "__device_driver_unlock", "__driver_probe_device",
        "device_driver_attach", "__device_retire_driver"))
    c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define scoped_guard(kind, lock) for (int once = 1; once; once = 0)
#define dev_dbg(...) ((void)0)
#define dev_err_probe(dev, err, msg) (err)
#define pm_runtime_get_suppliers(dev) ((void)0)
#define pm_runtime_get_sync(dev) ((void)0)
#define pm_runtime_barrier(dev) ((void)0)
#define pm_request_idle(dev) ((void)0)
#define pm_runtime_put(dev) ((void)0)
#define pm_runtime_put_suppliers(dev) ((void)0)
struct bus_type { bool need_parent_lock; };
struct device_driver { int unused; };
struct device_private { bool dead; };
struct device {
    pthread_mutex_t lock;
    struct { const char *name; int lock; } driver_override;
    struct device *parent;
    struct bus_type *bus;
    struct device_private *p;
    const struct device_driver *driver;
    void *context;
    bool registered, can_match;
    int released;
    atomic_bool notify_retire;
};
static bool initcall_debug;
static struct bus_type bus; /* Platform bus does not take parent lock. */
static const struct device_driver driver;
extern int fixture_probe(void *context);
extern void fixture_retire_wait(void *context);
static void device_lock(struct device *d)
{
    if (atomic_exchange(&d->notify_retire, false))
        fixture_retire_wait(d->context);
    assert(!pthread_mutex_lock(&d->lock));
}
static void device_unlock(struct device *d) { assert(!pthread_mutex_unlock(&d->lock)); }
static bool device_is_registered(struct device *d) { return d->registered; }
static bool dev_ready_to_probe(struct device *d) { (void)d; return true; }
static int really_probe(struct device *d, const struct device_driver *drv)
{
    int ret = fixture_probe(d->context);
    if (!ret) d->driver = drv;
    return ret < 0 ? -ret : ret;
}
static int really_probe_debug(struct device *d, const struct device_driver *drv)
{ return really_probe(d, drv); }
static char *strnchr(const char *s, size_t len, int ch) { return (char *)memchr(s, ch, len); }
static char *kstrndup(const char *s, size_t len, int flags)
{
    (void)flags;
    char *copy = malloc(len + 1); assert(copy);
    memcpy(copy, s, len); copy[len] = 0; return copy;
}
static void kfree(const void *p) { free((void *)p); }
static void __device_release_driver(struct device *d, struct device *parent)
{
    (void)parent;
    if (d->driver) { d->driver = NULL; d->released++; }
}
FUNCTIONS
struct device *fixture_device(void *context)
{
    struct device *d = calloc(1, sizeof(*d)); assert(d);
    d->p = calloc(1, sizeof(*d->p)); assert(d->p);
    assert(!pthread_mutex_init(&d->lock, NULL));
    d->bus = &bus; d->context = context; d->registered = true;
    return d;
}
bool fixture_match(struct device *d)
{ return !d->driver_override.name || !strcmp(d->driver_override.name, "aop-child"); }
int fixture_attach(struct device *d) { return device_driver_attach(&driver, d); }
void fixture_retire(struct device *d)
{
    const char *name = __device_prepare_driver_override("apple-aop-retired", 17);
    assert(!IS_ERR(name));
    atomic_store(&d->notify_retire, true);
    __device_retire_driver(d, name);
}
bool fixture_bound(struct device *d) { return d->driver != NULL; }
int fixture_released(struct device *d) { return d->released; }
void fixture_unregister(struct device *d)
{
    device_lock(d); d->p->dead = true; d->registered = false;
    __device_release_driver(d, NULL); device_unlock(d);
}
void fixture_destroy(struct device *d)
{
    kfree(d->driver_override.name); free(d->p);
    assert(!pthread_mutex_destroy(&d->lock)); free(d);
}
'''.replace("FUNCTIONS", functions)
    rust = r'''
use std::{ffi::c_void, sync::{Arc, Mutex as StdMutex, Condvar,
    atomic::{AtomicUsize, Ordering}}, thread};
type Result<T> = std::result::Result<T, i32>;
const EPROBE_DEFER: i32 = -517;
const ENODEV: i32 = -19;
struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> {
    fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() }
}
pub trait AOP: Send + Sync {}
struct Provider(Arc<AtomicUsize>);
impl AOP for Provider {}
impl Drop for Provider {
    fn drop(&mut self) { self.0.fetch_add(1, Ordering::SeqCst); }
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum ServiceAdmission { Probing, Ready, Closed }
struct ServiceGate { admission: Mutex<ServiceAdmission> }
impl ServiceGate {
    fn new() -> Arc<Self> {
        Arc::new(Self { admission: Mutex(StdMutex::new(ServiceAdmission::Probing)) })
    }
    GATE
}
pub struct ServiceContext { provider: Arc<dyn AOP>, gate: Arc<ServiceGate> }
impl ServiceContext { CONTEXT }
ABI
PAYLOAD
struct DeviceRaw { platform_data: *const c_void }
pub struct Device(DeviceRaw);
impl Device { fn as_raw(&self) -> *const DeviceRaw { &self.0 } }
impl dyn AOP { LOOKUP AUDIO_LOOKUP }
struct ProbeState { entered: bool, finish: bool, retiring: bool }
struct Fixture {
    context: ServiceContext,
    payload: ServicePlatformData,
    child: Device,
    state: StdMutex<ProbeState>,
    changed: Condvar,
    hold: bool,
    lookups: AtomicUsize,
}
impl Fixture {
    fn new(hold: bool) -> (Box<Self>, Arc<dyn AOP>, Arc<AtomicUsize>) {
        let drops = Arc::new(AtomicUsize::new(0));
        let parent: Arc<dyn AOP> = Arc::new(Provider(drops.clone()));
        let mut fixture = Box::new(Self {
            context: ServiceContext { provider: parent.clone(), gate: ServiceGate::new() },
            payload: ServicePlatformData {
                service: EPICService { channel: 0, endpoint: 0x22 },
                context: std::ptr::null(),
                abi: ServiceABI::Legacy,
            },
            child: Device(DeviceRaw { platform_data: std::ptr::null() }),
            state: StdMutex::new(ProbeState { entered: false, finish: false, retiring: false }),
            changed: Condvar::new(), hold, lookups: AtomicUsize::new(0),
        });
        fixture.payload.context = &fixture.context;
        fixture.child.0.platform_data = (&fixture.payload as *const ServicePlatformData).cast();
        (fixture, parent, drops)
    }
}
#[no_mangle]
unsafe extern "C" fn fixture_probe(context: *mut c_void) -> i32 {
    let fixture = unsafe { &*context.cast::<Fixture>() };
    fixture.lookups.fetch_add(1, Ordering::SeqCst);
    let provider = match unsafe { <dyn AOP>::from_child(&fixture.child) } {
        Ok(p) => p, Err(e) => return e,
    };
    if fixture.hold {
        let mut state = fixture.state.lock().unwrap();
        state.entered = true; fixture.changed.notify_all();
        while !state.finish { state = fixture.changed.wait(state).unwrap(); }
    }
    drop(provider);
    0
}
#[no_mangle]
unsafe extern "C" fn fixture_retire_wait(context: *mut c_void) {
    let fixture = unsafe { &*context.cast::<Fixture>() };
    if fixture.hold {
        let mut state = fixture.state.lock().unwrap();
        state.retiring = true; fixture.changed.notify_all();
    }
}
unsafe extern "C" {
    fn fixture_device(context: *mut c_void) -> *mut c_void;
    fn fixture_match(device: *mut c_void) -> bool;
    fn fixture_attach(device: *mut c_void) -> i32;
    fn fixture_retire(device: *mut c_void);
    fn fixture_bound(device: *mut c_void) -> bool;
    fn fixture_released(device: *mut c_void) -> i32;
    fn fixture_unregister(device: *mut c_void);
    fn fixture_destroy(device: *mut c_void);
}

#[test]
fn probing_defers_then_ready_and_closed_context_owns_provider() {
    let (fixture, parent, drops) = Fixture::new(false);
    unsafe {
        let dev = fixture_device((&*fixture as *const Fixture).cast_mut().cast());
        assert_eq!(fixture_attach(dev), -11); // Real attach translates DEFER to EAGAIN.
        fixture.context.gate.ready();
        let owned = fixture.context.acquire().unwrap();
        drop(parent); // Driver-data ownership can go away without a borrowed pointer.
        assert_eq!(drops.load(Ordering::SeqCst), 0);
        fixture.context.gate.close();
        fixture.context.gate.ready(); // A late startup completion cannot reopen it.
        assert!(matches!(fixture.context.acquire(), Err(ENODEV)));
        drop(owned);
        fixture_unregister(dev); fixture_destroy(dev);
    }
    drop(fixture);
    assert_eq!(drops.load(Ordering::SeqCst), 1);
}

#[test]
fn matched_before_override_still_probes_but_closed_gate_rejects_after_reload() {
    let (fixture, parent, drops) = Fixture::new(false);
    fixture.context.gate.ready();
    unsafe {
        let dev = fixture_device((&*fixture as *const Fixture).cast_mut().cast());
        assert!(fixture_match(dev)); // bus.c match can precede attach's lock.
        fixture.context.gate.close();
        fixture_retire(dev);
        assert!(!fixture_match(dev));
        drop(parent); // Simulate parent drvdata release/module-local latch loss.
        let attempts = fixture.lookups.load(Ordering::SeqCst);
        assert_eq!(fixture_attach(dev), ENODEV); // Real attach has no rematch.
        assert_eq!(fixture.lookups.load(Ordering::SeqCst), attempts + 1);
        assert!(!fixture_bound(dev));
        assert_eq!(drops.load(Ordering::SeqCst), 0); // Retained context still owns it.
        fixture_unregister(dev); fixture_destroy(dev);
    }
    drop(fixture);
    assert_eq!(drops.load(Ordering::SeqCst), 1);
}

#[test]
fn admitted_probe_holds_owned_provider_and_retirement_waits_then_unbinds() {
    let (fixture, parent, drops) = Fixture::new(true);
    fixture.context.gate.ready();
    unsafe {
        let dev = fixture_device((&*fixture as *const Fixture).cast_mut().cast());
        let address = dev as usize;
        let probing = thread::spawn(move || fixture_attach(address as *mut c_void));
        let mut state = fixture.state.lock().unwrap();
        while !state.entered { state = fixture.changed.wait(state).unwrap(); }
        fixture.context.gate.close();
        drop(parent);
        assert_eq!(drops.load(Ordering::SeqCst), 0);
        let done = Arc::new(AtomicUsize::new(0));
        let completed = done.clone();
        let retiring = thread::spawn(move || {
            fixture_retire(address as *mut c_void);
            completed.store(1, Ordering::SeqCst);
        });
        while !state.retiring { state = fixture.changed.wait(state).unwrap(); }
        assert_eq!(done.load(Ordering::SeqCst), 0);
        state.finish = true; fixture.changed.notify_all(); drop(state);
        assert_eq!(probing.join().unwrap(), 0);
        retiring.join().unwrap();
        assert!(!fixture_bound(dev)); assert_eq!(fixture_released(dev), 1);
        assert_eq!(fixture_attach(dev), ENODEV);
        fixture_unregister(dev); fixture_destroy(dev);
    }
    drop(fixture);
    assert_eq!(drops.load(Ordering::SeqCst), 1);
}

#[test]
fn close_serializes_lookup_and_unregistered_child_never_dereferences_context() {
    let (fixture, parent, drops) = Fixture::new(false);
    fixture.context.gate.ready();
    let admission = fixture.context.gate.admission.lock();
    let gate = fixture.context.gate.clone();
    let closing = thread::spawn(move || gate.close());
    // The same lock used by acquire keeps close waiting until an owned clone
    // has been made. This reference remains valid after parent drvdata drops.
    let pending = fixture.context.provider.clone();
    drop(admission); closing.join().unwrap();
    assert!(matches!(fixture.context.acquire(), Err(ENODEV)));
    unsafe {
        let dev = fixture_device((&*fixture as *const Fixture).cast_mut().cast());
        fixture_unregister(dev);
        drop(parent); drop(fixture); drop(pending);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        assert_eq!(fixture_attach(dev), ENODEV); // Real __driver_probe_device guard.
        fixture_destroy(dev);
    }
}

#[test]
fn audio_abi_is_checked_before_provider_admission_or_any_configuration_call() {
    for provided in [ServiceABI::ControlOnly, ServiceABI::Legacy, ServiceABI::T8140, ServiceABI::J616sJack] {
        let (mut fixture, parent, drops) = Fixture::new(false);
        fixture.payload.abi = provided;
        fixture.context.gate.ready();
        assert_eq!(provided.publishes_children(), provided != ServiceABI::ControlOnly);
        for requested in [ServiceABI::ControlOnly, ServiceABI::Legacy, ServiceABI::T8140, ServiceABI::J616sJack] {
            let lookup = unsafe { <dyn AOP>::audio_from_child(&fixture.child, requested) };
            if requested != ServiceABI::ControlOnly && requested == provided {
                drop(lookup.unwrap());
            } else {
                assert!(matches!(lookup, Err(ENODEV)));
            }
        }
        fixture.context.gate.close();
        assert!(matches!(unsafe {
            <dyn AOP>::audio_from_child(&fixture.child, provided)
        }, Err(ENODEV)));
        drop(parent); drop(fixture);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
    }
}
'''.replace("GATE", gate).replace("CONTEXT", context).replace("PAYLOAD", payload).replace(
        "AUDIO_LOOKUP", audio_lookup).replace("LOOKUP", lookup).replace("ABI\n", abi + "\n")
    with tempfile.TemporaryDirectory(prefix="aop-service-race-") as directory:
        directory = Path(directory)
        cfile, obj = directory / "core.c", directory / "core.o"
        rs, binary = directory / "fixtures.rs", directory / "fixtures"
        cfile.write_text(c)
        rs.write_text(rust)
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-c", str(cfile), "-o", str(obj)], check=True)
        subprocess.run([rustc, "--edition=2021", "--test", str(rs),
                        "-C", "link-arg=" + str(obj), "-C", "link-arg=-pthread",
                        "-o", str(binary)], check=True)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
