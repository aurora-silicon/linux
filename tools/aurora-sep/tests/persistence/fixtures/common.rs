// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Host-only dependencies for unchanged production methods. No hardware or files.
#![allow(dead_code, non_upper_case_globals, non_snake_case, unused_imports)]
extern crate self as kernel;
use std::cell::{Cell, RefCell};
use std::ffi::CStr;
use std::ops::{Deref, DerefMut};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(i32);
impl Error {
    pub fn from_errno(n: i32) -> Self {
        assert!((-4095..0).contains(&n));
        Self(n)
    }
    pub unsafe fn from_errno_unchecked(n: i32) -> Self {
        Self(n)
    }
}
pub type Result<T = (), E = Error> = std::result::Result<T, E>;
pub const EPERM: Error = Error(-1);
pub const ENOENT: Error = Error(-2);
pub const EIO: Error = Error(-5);
pub const ENXIO: Error = Error(-6);
pub const ENOMEM: Error = Error(-12);
pub const EACCES: Error = Error(-13);
pub const EBUSY: Error = Error(-16);
pub const EEXIST: Error = Error(-17);
pub const ENOTDIR: Error = Error(-20);
pub const EINVAL: Error = Error(-22);
pub const EMFILE: Error = Error(-24);
pub const ENOSPC: Error = Error(-28);
pub const EROFS: Error = Error(-30);
pub const ENODATA: Error = Error(-61);
pub const ELOOP: Error = Error(-40);
pub const ENOTSYNC: Error = Error(-522);
pub const ENODEV: Error = Error(-19);
pub const GFP_KERNEL: u32 = 0;
pub trait AsCharPtr {
    fn as_char_ptr(&self) -> *const std::ffi::c_char;
}
impl AsCharPtr for CStr {
    fn as_char_ptr(&self) -> *const std::ffi::c_char {
        self.as_ptr()
    }
}
pub mod ffi {
    pub use std::ffi::*;
}
pub mod bindings {
    pub unsafe fn IS_ERR(p: *const std::ffi::c_void) -> bool {
        (p as usize) >= usize::MAX - 4094
    }
    pub unsafe fn PTR_ERR(p: *const std::ffi::c_void) -> isize {
        p as isize
    }
}
pub mod prelude {
    pub use crate::{
        AsCharPtr, Error, KVec, Result, EACCES, EBUSY, EEXIST, EINVAL, EIO, ELOOP, EMFILE,
        ENODATA, ENODEV, ENOENT, ENOMEM, ENOSPC, ENOTDIR, ENOTSYNC, ENXIO, EPERM, EROFS,
        GFP_KERNEL,
    };
    pub use std::ffi::{c_char, c_int, c_long, c_void, CStr};
}
pub struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    pub const fn new() -> Self {
        Self(Vec::new())
    }
    pub fn push(&mut self, value: T, _: u32) -> Result {
        self.0.push(value);
        Ok(())
    }
    pub fn with_capacity(n: usize, _: u32) -> Result<Self> {
        let mut v = Vec::new();
        v.try_reserve(n).map_err(|_| ENOMEM)?;
        Ok(Self(v))
    }
}
impl<T: Clone> KVec<T> {
    pub fn resize(&mut self, n: usize, v: T, _: u32) -> Result {
        self.0
            .try_reserve(n.saturating_sub(self.0.len()))
            .map_err(|_| ENOMEM)?;
        self.0.resize(n, v);
        Ok(())
    }
    pub fn extend_from_slice(&mut self, s: &[T], _: u32) -> Result {
        self.0.try_reserve(s.len()).map_err(|_| ENOMEM)?;
        self.0.extend_from_slice(s);
        Ok(())
    }
}
impl<T> Deref for KVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        &self.0
    }
}
impl<T> DerefMut for KVec<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        &mut self.0
    }
}
pub struct Secret(pub KVec<u8>);
impl Deref for Secret {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        &self.0
    }
}
impl Drop for Secret {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}
macro_rules! static_assert {
    ($v:expr) => {
        const _: () = assert!($v);
    };
}
macro_rules! dev_err { ($dev:expr, $($arg:tt)*) => { let _ = (&$dev, format_args!($($arg)*)); }; }
macro_rules! dev_warn { ($dev:expr, $($arg:tt)*) => { let _ = (&$dev, format_args!($($arg)*)); }; }
macro_rules! dev_err_ratelimited {
    ($dev:expr, $($arg:tt)*) => { let _ = (&$dev, format_args!($($arg)*)); };
}
macro_rules! dev_warn_ratelimited {
    ($dev:expr, $($arg:tt)*) => { let _ = (&$dev, format_args!($($arg)*)); };
}
