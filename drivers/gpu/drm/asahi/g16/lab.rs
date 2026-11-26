// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! LAB ONLY (not for merge): a scheduling trace and runtime experiment knobs.
//!
//! `aurora-g16/lab` reads the knobs and accepts `<index> <value>` writes, or `r` to clear the
//! trace. `aurora-g16/trace` prints the trace records, oldest first. Every knob defaults to
//! zero, which keeps origin/main behaviour.

use core::fmt;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use kernel::{prelude::*, uaccess::UserSliceReader};

/// Knob indices.
pub(crate) const TRACE: usize = 0;
pub(crate) const NO_PREV_BARRIER: usize = 4;
pub(crate) const OWN_HOLD: usize = 5;
pub(crate) const CROSS_HOLD: usize = 6;
pub(crate) const FS_KICK: usize = 7;
/// Emit the same-ring FS(N-1) -> FS(N) barrier ahead of every pipelined pass's own TA wait.
pub(crate) const FS_SERIAL: usize = 8;
/// Retire a render or compute flight with later flights of its class behind it without the
/// shared notifier stamps, which firmware writes only for the threshold (newest) draw.
/// 1: render and compute; 2: render only; 3: render, and compute once the next compute flight
/// on the same pipe has reached its stamp too.
pub(crate) const BEHIND_NO_NOTIFIER: usize = 9;
/// Repoll after at most this many us while more than one flight is in flight (0: default).
pub(crate) const BEHIND_REPOLL_US: usize = 10;
/// Hold the submit records of work published while work is in flight and the firmware has taken
/// every posted record, until every posted record's work has finished; then post all held records
/// and ring once.
pub(crate) const DEFER_RECORDS: usize = 11;
/// Post records of work published behind in-flight work of its class with event 0.
pub(crate) const BEHIND_EVENT0: usize = 12;
/// Point a render flight's TA/FS work at a second, ASC-cached render notifier page (as macOS
/// 26B123 maps its notifier records, 0x443) with the TA/FS service stamps in the queues' separate
/// uncached user-stamp cells; its receipt reads those cells and not the records' context words.
/// Takes effect per flight (decided at its first pass); change it only while the GPU is idle.
pub(crate) const RENDER_NCACHE: usize = 15;
pub(crate) const KNOBS: usize = 16;

static KNOB: [AtomicU32; KNOBS] = [const { AtomicU32::new(0) }; KNOBS];

const LEN: usize = 8192;
static RECS: [[AtomicU64; 6]; LEN] = [const { [const { AtomicU64::new(0) }; 6] }; LEN];
static HEAD: AtomicU64 = AtomicU64::new(0);

/// Last stage stamps seen by the `FS_KICK = 2` poll nudge: TA, FS, compute pipe 0.
pub(crate) static LAST_STAMP: [AtomicU32; 3] = [const { AtomicU32::new(0) }; 3];

pub(crate) fn knob(i: usize) -> u32 {
    KNOB[i].load(Ordering::Relaxed)
}

pub(crate) fn on() -> bool {
    knob(TRACE) != 0
}

/// Records `w` when tracing is on.
pub(crate) fn rec(w: [u64; 6]) {
    if !on() {
        return;
    }
    let i = (HEAD.fetch_add(1, Ordering::Relaxed) as usize) % LEN;
    for (cell, v) in RECS[i].iter().zip(w) {
        cell.store(v, Ordering::Relaxed);
    }
}

pub(crate) fn show_knobs(_: &(), f: &mut fmt::Formatter<'_>) -> fmt::Result {
    for (i, k) in KNOB.iter().enumerate() {
        writeln!(f, "{} {}", i, k.load(Ordering::Relaxed))?;
    }
    Ok(())
}

pub(crate) fn write_knobs(_: &(), reader: &mut UserSliceReader) -> Result {
    let mut buf = [0u8; 64];
    let n = reader.len().min(buf.len());
    reader.read_slice(&mut buf[..n])?;
    let text = core::str::from_utf8(&buf[..n]).map_err(|_| EINVAL)?.trim();
    if text == "r" {
        HEAD.store(0, Ordering::Relaxed);
        return Ok(());
    }
    let mut it = text.split_whitespace();
    let i: usize = it.next().ok_or(EINVAL)?.parse().map_err(|_| EINVAL)?;
    let v: u32 = it.next().ok_or(EINVAL)?.parse().map_err(|_| EINVAL)?;
    KNOB.get(i).ok_or(EINVAL)?.store(v, Ordering::Relaxed);
    Ok(())
}

pub(crate) fn show_trace(_: &(), f: &mut fmt::Formatter<'_>) -> fmt::Result {
    let head = HEAD.load(Ordering::Relaxed) as usize;
    let start = head.saturating_sub(LEN);
    for n in start..head {
        let r = &RECS[n % LEN];
        writeln!(
            f,
            "{:x} {} {} {} {} {}",
            r[0].load(Ordering::Relaxed),
            r[1].load(Ordering::Relaxed),
            r[2].load(Ordering::Relaxed),
            r[3].load(Ordering::Relaxed),
            r[4].load(Ordering::Relaxed),
            r[5].load(Ordering::Relaxed)
        )?;
    }
    Ok(())
}
