// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Byte-bounded storage for fully retired, displaced command owners.
//! Firmware's last owner and in-flight commands must never enter this cache.
use kernel::prelude::*;
use core::sync::atomic::{AtomicUsize, Ordering};

pub(crate) const CACHE_BYTES: usize = 1280 * 1024 * 1024;
pub(crate) const GLOBAL_CACHE_BYTES: usize = 2048 * 1024 * 1024;
static CACHED_BYTES: AtomicUsize = AtomicUsize::new(0);
static HITS: AtomicUsize = AtomicUsize::new(0);
static MISSES: AtomicUsize = AtomicUsize::new(0);
static PUTS: AtomicUsize = AtomicUsize::new(0);
static EVICTIONS: AtomicUsize = AtomicUsize::new(0);
static OVERSIZED: AtomicUsize = AtomicUsize::new(0);
static GLOBAL_REJECTS: AtomicUsize = AtomicUsize::new(0);

pub(crate) fn statistics() -> [usize; 7] {
    [&CACHED_BYTES, &HITS, &MISSES, &PUTS, &EVICTIONS, &OVERSIZED, &GLOBAL_REJECTS]
        .map(|v| v.load(Ordering::Relaxed))
}

fn reserve_bytes(bytes: usize) -> bool {
    CACHED_BYTES.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |current|
        (current <= GLOBAL_CACHE_BYTES - bytes).then_some(current + bytes)).is_ok()
}

pub(crate) struct OwnerCache<T> {
    entries: KVec<(usize, T)>,
    bytes: usize,
}

impl<T> OwnerCache<T> {
    pub(crate) fn new() -> Self {
        Self { entries: KVec::new(), bytes: 0 }
    }

    pub(crate) fn take(&mut self, matches: impl Fn(&T) -> bool) -> Option<T> {
        let Some(index) = self.entries.iter().position(|(_, owner)| matches(owner)) else {
            MISSES.fetch_add(1, Ordering::Relaxed);
            return None;
        };
        HITS.fetch_add(1, Ordering::Relaxed);
        let (bytes, owner) = self.entries.remove(index).ok()?;
        self.bytes -= bytes;
        CACHED_BYTES.fetch_sub(bytes, Ordering::Relaxed);
        Some(owner)
    }

    pub(crate) fn evict_one(&mut self) -> bool {
        let Ok((bytes, owner)) = self.entries.remove(0) else { return false; };
        self.bytes -= bytes;
        drop(owner);
        CACHED_BYTES.fetch_sub(bytes, Ordering::Relaxed);
        EVICTIONS.fetch_add(1, Ordering::Relaxed);
        true
    }

    pub(crate) fn trim_for(&mut self, bytes: usize) {
        if bytes > CACHE_BYTES { return; }
        while self.bytes > CACHE_BYTES - bytes && self.evict_one() {}
    }

    pub(crate) fn put(&mut self, owner: T, bytes: usize) {
        if bytes > CACHE_BYTES { OVERSIZED.fetch_add(1, Ordering::Relaxed); return; }
        self.trim_for(bytes);
        if !reserve_bytes(bytes) { GLOBAL_REJECTS.fetch_add(1, Ordering::Relaxed); return; }
        if self.entries.push((bytes, owner), GFP_KERNEL).is_ok() {
            self.bytes += bytes;
            PUTS.fetch_add(1, Ordering::Relaxed);
        } else {
            CACHED_BYTES.fetch_sub(bytes, Ordering::Relaxed);
        }
    }
}

impl<T> Drop for OwnerCache<T> {
    fn drop(&mut self) {
        while let Ok((_, owner)) = self.entries.remove(0) { drop(owner); }
        CACHED_BYTES.fetch_sub(self.bytes, Ordering::Relaxed);
    }
}

pub(crate) trait CacheControl: Clone {
    fn same(&self, other: &Self) -> bool;
    fn closed(&self) -> bool;
    fn trim_for(&self, bytes: usize);
    fn evict_one(&self) -> bool;
}

pub(crate) struct Registry<C> { entries: KVec<C> }
impl<C: CacheControl> Registry<C> {
    pub(crate) fn new() -> Self { Self { entries: KVec::new() } }
    pub(crate) fn prepare(&mut self, incoming: C, bytes: usize) -> bool {
        if bytes > CACHE_BYTES || incoming.closed() { return false; }
        for i in (0..self.entries.len()).rev() {
            if self.entries[i].closed() || self.entries[i].same(&incoming) {
                let _ = self.entries.remove(i);
            }
        }
        incoming.trim_for(bytes);
        if self.entries.push(incoming, GFP_KERNEL).is_err() { return false; }
        for cache in &self.entries {
            while CACHED_BYTES.load(Ordering::Relaxed) > GLOBAL_CACHE_BYTES - bytes {
                if !cache.evict_one() { break; }
            }
            if CACHED_BYTES.load(Ordering::Relaxed) <= GLOBAL_CACHE_BYTES - bytes {
                return true;
            }
        }
        false
    }
}
