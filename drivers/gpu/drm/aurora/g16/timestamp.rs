// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! CPU-retired G16 timestamp buffers.
//!
//! Firmware writes only internal command ticks. After complete command retirement the host
//! converts those ticks and writes the bound user destinations through retained CPU mappings.
//! These buffers have no firmware alias and consume no global UAT address space.

use core::ops::Range;
use core::sync::atomic::Ordering;
use kernel::prelude::*;

use super::Gpu;
use crate::{gem, timestamp::Buffer};

pub(crate) fn map(gpu: &Gpu, bo: gem::ObjectRef, range: Range<usize>) -> Result<Buffer> {
    if gpu.shared.crashed.load(Ordering::Acquire) {
        return Err(EIO);
    }
    Buffer::cpu(bo, range)
}
