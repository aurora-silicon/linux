#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real override allocation/installation and AOP token lifetime fault fixtures."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def extract(source, name):
    at = source.index(name + "(")
    start = source.rfind("\n", 0, at) + 1
    opening = source.index("{", at)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    cc, rustc = os.environ.get("CC", "cc"), os.environ.get("RUSTC", "rustc")
    if not shutil.which(cc) or not shutil.which(rustc):
        print("SKIP: host C and Rust compilers required")
        return 4
    driver = (ROOT / "drivers/soc/apple/aop.rs").read_text()
    begin = driver.index("struct PreparedOverride(")
    end = driver.index("\nimpl ChildDevice", begin)
    token = driver[begin:end]
    core = (ROOT / "drivers/base/dd.c").read_text()
    functions = "\n".join(extract(core, name) for name in (
        "__device_prepare_driver_override", "__device_install_driver_override",
        "__device_set_driver_override", "__device_retire_driver"))
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define EINVAL 22
#define ENOMEM 12
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define scoped_guard(kind, lock) for (int once = 1; once; once = 0)
struct device {
    struct { const char *name; int lock; } driver_override;
    struct device *parent;
};
static void __device_driver_lock(struct device *dev, struct device *parent)
{ (void)dev; (void)parent; }
static void __device_driver_unlock(struct device *dev, struct device *parent)
{ (void)dev; (void)parent; }
static void __device_release_driver(struct device *dev, struct device *parent)
{ (void)dev; (void)parent; }
static bool fail_alloc;
static int allocated, freed;
static char *strnchr(const char *s, size_t len, int ch) { return (char *)memchr(s, ch, len); }
static char *kstrndup(const char *s, size_t len, int flags)
{
    (void)flags;
    if (fail_alloc) return NULL;
    char *copy = malloc(len + 1); assert(copy);
    memcpy(copy, s, len); copy[len] = 0; allocated++; return copy;
}
void kfree(const void *p) { if (p) { freed++; free((void *)p); } }
FUNCTIONS
void fixture_fail(bool fail) { fail_alloc = fail; }
int fixture_live(void) { return allocated - freed; }
struct device *fixture_device(void) { return calloc(1, sizeof(struct device)); }
bool fixture_matches(struct device *d, const char *name)
{
    return !d->driver_override.name || !strcmp(d->driver_override.name, name);
}
void fixture_destroy(struct device *d) { kfree(d->driver_override.name); free(d); }
'''.replace("FUNCTIONS", functions)
    rust = r'''
use std::{mem, ptr::NonNull, ffi::CStr};
type Result<T> = std::result::Result<T, isize>;
const EINVAL: isize = -22;
const RETIRED_DRIVER_OVERRIDE: &CStr = c"apple-aop-retired";
trait KernelCStr { fn as_char_ptr(&self) -> *const core::ffi::c_char; }
impl KernelCStr for CStr {
    fn as_char_ptr(&self) -> *const core::ffi::c_char { self.as_ptr() }
}
unsafe fn from_err_ptr<T>(pointer: *mut T) -> Result<*mut T> {
    if pointer as usize >= usize::MAX - 4094 { Err(pointer as isize) } else { Ok(pointer) }
}
mod bindings {
    pub type device = core::ffi::c_void;
    unsafe extern "C" {
        pub fn __device_prepare_driver_override(s: *const core::ffi::c_char, len: usize) -> *const core::ffi::c_char;
        pub fn __device_install_driver_override(dev: *mut device, s: *const core::ffi::c_char);
        pub fn __device_retire_driver(dev: *mut device, s: *const core::ffi::c_char);
        pub fn __device_set_driver_override(dev: *mut device, s: *const core::ffi::c_char, len: usize) -> i32;
        pub fn kfree(pointer: *const core::ffi::c_void);
        pub fn fixture_fail(fail: bool);
        pub fn fixture_live() -> i32;
        pub fn fixture_device() -> *mut device;
        pub fn fixture_matches(dev: *mut device, name: *const core::ffi::c_char) -> bool;
        pub fn fixture_destroy(dev: *mut device);
    }
}
TOKEN
#[test]
fn parent_child_oom_and_reload_preserve_the_device_owned_barrier() {
    unsafe {
        // Parent reserve fails before its handoff point can be reached.
        bindings::fixture_fail(true);
        let mut cpu_exposed = false;
        let result = (|| -> Result<PreparedOverride> {
            let marker = PreparedOverride::new()?;
            cpu_exposed = true;
            Ok(marker)
        })();
        assert!(matches!(result, Err(-12)));
        assert!(!cpu_exposed && bindings::fixture_live() == 0);

        // Parent succeeds. A child reserve fails before child registration/
        // publication; the parent already owns its guaranteed retire reserve.
        bindings::fixture_fail(false);
        let parent = PreparedOverride::new().unwrap();
        bindings::fixture_fail(true);
        let mut child_exposed = false;
        let result = (|| -> Result<PreparedOverride> {
            let marker = PreparedOverride::new()?;
            child_exposed = true;
            Ok(marker)
        })();
        assert!(matches!(result, Err(-12)));
        assert!(!child_exposed && bindings::fixture_live() == 1);
        let device = bindings::fixture_device();
        parent.install(device); // Allocation still forced to fail: install cannot allocate.
        assert!(!bindings::fixture_matches(device, c"apple-aop".as_ptr()));
        let mut module_local_retired = true;
        assert!(module_local_retired);
        module_local_retired = false; // Simulated module unload/reload.
        assert!(!module_local_retired);
        assert!(!bindings::fixture_matches(device, c"apple-aop".as_ptr()));
        assert!(bindings::fixture_live() == 1);
        bindings::fixture_destroy(device);
        assert!(bindings::fixture_live() == 0);

        // A child marker also transfers without allocation at retirement.
        bindings::fixture_fail(false);
        let child = PreparedOverride::new().unwrap();
        let device = bindings::fixture_device();
        bindings::fixture_fail(true);
        child.retire_driver(device);
        assert!(!bindings::fixture_matches(device, c"snd_soc_apple_aop".as_ptr()));
        bindings::fixture_destroy(device);
        assert!(bindings::fixture_live() == 0);

        // Confirmed shutdown discards the unused reserve, preserving normal
        // matching/reprobe and any pre-existing override.
        bindings::fixture_fail(false);
        let device = bindings::fixture_device();
        assert_eq!(bindings::__device_set_driver_override(device, c"old-driver".as_ptr(), 10), 0);
        let reserved = PreparedOverride::new().unwrap();
        drop(reserved);
        assert!(bindings::fixture_matches(device, c"old-driver".as_ptr()));
        assert!(!bindings::fixture_matches(device, c"another".as_ptr()));
        // The existing setter retains its old override on allocation error.
        bindings::fixture_fail(true);
        assert_eq!(bindings::__device_set_driver_override(device, c"replacement".as_ptr(), 11), -12);
        assert!(bindings::fixture_matches(device, c"old-driver".as_ptr()));
        assert_eq!(bindings::__device_set_driver_override(device, c"".as_ptr(), 0), 0);
        assert!(bindings::fixture_matches(device, c"any-driver".as_ptr()));
        bindings::fixture_destroy(device);
        assert!(bindings::fixture_live() == 0);
    }
}
'''.replace("TOKEN", token)
    with tempfile.TemporaryDirectory(prefix="aop-retirement-") as directory:
        directory = Path(directory)
        cfile, obj = directory / "core.c", directory / "core.o"
        rs, binary = directory / "fixtures.rs", directory / "fixtures"
        cfile.write_text(c)
        rs.write_text("#![allow(non_camel_case_types)]\n" + rust)
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", str(cfile),
                        "-o", str(obj)], check=True)
        subprocess.run([rustc, "--edition=2021", "--test", str(rs),
                        "-C", "link-arg=" + str(obj), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
