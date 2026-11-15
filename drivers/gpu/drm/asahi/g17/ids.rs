// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Firmware-visible object generations shared by contexts, pools and render work.

use super::fw::ObjectIds;
use core::sync::atomic::{AtomicU32, Ordering};
use kernel::prelude::*;

/// Module-lifetime identities remain unique across retained graphs and device rebinds.
pub(crate) static OBJECTS: Counter = Counter::new();

pub(crate) struct Counter(AtomicU32);

impl Counter {
    const fn new() -> Self {
        Self(AtomicU32::new(1))
    }

    fn reserve(&self, count: u32) -> Result<u32> {
        let mut old = self.0.load(Ordering::Acquire);
        loop {
            let next = old.checked_add(count).ok_or(EOVERFLOW)?;
            match self
                .0
                .compare_exchange_weak(old, next, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => return Ok(old),
                Err(current) => old = current,
            }
        }
    }

    pub(crate) fn object(&self) -> Result<u32> {
        let id = self.reserve(1)?;
        if id == 0 {
            Err(EOVERFLOW)
        } else {
            Ok(id)
        }
    }

    /// Allocate consecutive current IDs atomically, fragment first and tiling
    /// second. Both name the same retained predecessor of their render graph.
    pub(crate) fn render_pair(&self, predecessor: u32) -> Result<(ObjectIds, ObjectIds)> {
        let fragment = self.reserve(2)?;
        if predecessor == 0 || fragment == 0 {
            return Err(EOVERFLOW);
        }
        Ok((
            ObjectIds {
                predecessor,
                current: fragment,
            },
            ObjectIds {
                predecessor,
                current: fragment + 1,
            },
        ))
    }
}
