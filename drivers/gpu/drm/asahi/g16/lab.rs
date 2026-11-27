// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! LAB ONLY (not for merge): runtime experiment knobs for linux#3349.
//!
//! `aurora-g16/lab` prints the knobs and counters and accepts `<index> <value>` writes. Every
//! knob defaults to zero, which keeps the behaviour of the branch this sits on.

use core::fmt;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use kernel::{prelude::*, uaccess::UserSliceReader};

/// Cross-queue waits at `SUBMIT`: 0 resolve in firmware as before; 1 never resolve in firmware
/// (every cross-queue wait waits on the host); 2 wait on the host when a waited job has a compute
/// command, and resolve render-only producers in firmware as before; 3 resolve in firmware as before,
/// but a render's barrier on a compute pipe waits on that pipe's service stamp, not its fw_stamp.
pub(crate) const FW_WAITS: usize = 0;
pub(crate) const KNOBS: usize = 4;

static KNOB: [AtomicU32; KNOBS] = [const { AtomicU32::new(0) }; KNOBS];

/// Submissions whose resolvable cross-queue waits the knob sent to the host.
pub(crate) static HOST_FORCED: AtomicU64 = AtomicU64::new(0);
/// Submissions with cross-queue waits resolved in firmware.
pub(crate) static FW_RESOLVED: AtomicU64 = AtomicU64::new(0);
/// Render passes whose compute barriers waited on a service stamp (knob 0 = 3).
pub(crate) static SERVICE_WAITS: AtomicU64 = AtomicU64::new(0);

pub(crate) fn knob(i: usize) -> u32 {
    KNOB[i].load(Ordering::Relaxed)
}

pub(crate) fn show_knobs(_: &(), f: &mut fmt::Formatter<'_>) -> fmt::Result {
    for (i, k) in KNOB.iter().enumerate() {
        writeln!(f, "{} {}", i, k.load(Ordering::Relaxed))?;
    }
    writeln!(
        f,
        "host_forced {} fw_resolved {} service_waits {}",
        HOST_FORCED.load(Ordering::Relaxed),
        FW_RESOLVED.load(Ordering::Relaxed),
        SERVICE_WAITS.load(Ordering::Relaxed)
    )
}

pub(crate) fn write_knobs(_: &(), reader: &mut UserSliceReader) -> Result {
    let mut buf = [0u8; 64];
    let n = reader.len().min(buf.len());
    reader.read_slice(&mut buf[..n])?;
    let text = core::str::from_utf8(&buf[..n]).map_err(|_| EINVAL)?.trim();
    let mut it = text.split_whitespace();
    let i: usize = it.next().ok_or(EINVAL)?.parse().map_err(|_| EINVAL)?;
    let v: u32 = it.next().ok_or(EINVAL)?.parse().map_err(|_| EINVAL)?;
    KNOB.get(i).ok_or(EINVAL)?.store(v, Ordering::Relaxed);
    Ok(())
}
