// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Measurement only (`gpubench_opt` bit 8): where a job's completion path spends its time, from
//! its doorbell to its fence signal. Points are system counter reads; gpubench_opt.c aggregates
//! the deltas per engine (`gpubench_lat`). Only the event worker records, so the statistics have
//! a single writer.

use super::completion::{Completion, Deferred};
use core::sync::atomic::{AtomicU64, Ordering};

unsafe extern "C" {
    fn asahi_g17_lat_record(engine: u32, kind: u32, ns: i64);
}

const COMPUTE: usize = 0;
const RENDER: usize = 1;

// Deltas, in gpubench_opt.c's order.
const DOORBELL_IRQ: u32 = 0;
const IRQ_PASS: u32 = 1;
const PASS_LOCKED: u32 = 2;
const LOCKED_SIGNAL: u32 = 3;
const SIGNAL_CALL: u32 = 4;
const DOORBELL_SIGNAL: u32 = 5;
const DOORBELL_GPU_START: u32 = 6;
const GPU_BUSY: u32 = 7;
const GPU_END_IRQ: u32 = 8;
const SIGNAL_DOORBELL: u32 = 9;

/// Recorded for a delta whose points are missing or more than a second apart.
const MISSING: i64 = i64::MIN;
/// The GPU's command timestamp clock.
const GPU_HZ: u64 = crate::hw::t8140::resources::COMMAND_TIMESTAMP_HZ;
/// No gap recorded (see [`doorbell`]).
const NO_GAP: u64 = u64::MAX;

/// Counter at the first events message since the current pass began; zero for none.
static IRQ: AtomicU64 = AtomicU64::new(0);
/// Counter at the end of each engine's last fence signal; zero for none.
static LAST_SIGNAL: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];

fn counter() -> u64 {
    super::qos::clock().0
}

/// Nanoseconds from counter value `from` to `to`.
fn ns(from: u64, to: u64) -> i64 {
    let frequency = super::qos::clock().1;
    let delta = to.wrapping_sub(from) as i64;
    if frequency == 0 || delta.unsigned_abs() > frequency {
        return MISSING;
    }
    delta * 1_000_000_000 / frequency as i64
}

/// Nanoseconds from `from` to `to`, in GPU timestamp ticks.
fn ticks_ns(from: u64, to: u64) -> i64 {
    let delta = to.wrapping_sub(from) as i64;
    if from == 0 || to == 0 || delta.unsigned_abs() > GPU_HZ {
        return MISSING;
    }
    delta * 1_000_000_000 / GPU_HZ as i64
}

/// Counter value `host` as a GPU timestamp: the device's 24 MHz domain follows the system
/// counter. Zero if it does not convert.
fn gpu_ticks(host: u64) -> u64 {
    let (_, frequency) = super::qos::clock();
    super::qos::time_from_host(host, frequency).unwrap_or(0)
}

fn record(engine: usize, kind: u32, ns: i64) {
    // SAFETY: Plain values only.
    unsafe { asahi_g17_lat_record(engine as u32, kind, ns) };
}

fn engine(completion: &Completion) -> usize {
    if completion.is_compute() {
        COMPUTE
    } else {
        RENDER
    }
}

/// The doorbell of `completion`'s command is about to ring.
pub(super) fn doorbell(completion: &Completion) {
    let now = counter();
    let last = LAST_SIGNAL[engine(completion)].load(Ordering::Relaxed);
    let gap = if last != 0 { ns(last, now) } else { MISSING };
    let words = completion.latency();
    words[1].store(if gap == MISSING { NO_GAP } else { gap as u64 }, Ordering::Relaxed);
    words[0].store(now, Ordering::Relaxed);
}

/// An events message arrived. Only the first since the current pass began counts: it is the
/// one that queued the next pass.
pub(super) fn irq() {
    let _ = IRQ.compare_exchange(0, counter(), Ordering::Relaxed, Ordering::Relaxed);
}

/// The points of one event pass.
#[derive(Clone, Copy)]
pub(super) struct Pass {
    /// The events message this pass serves; zero if none queued it.
    irq: u64,
    start: u64,
    /// The pass has taken the device mutex once.
    locked: u64,
}

impl Pass {
    /// An event pass begins; `None` unless bit 8 is on.
    pub(super) fn begin() -> Option<Self> {
        if !super::opt(super::OPT_LATENCY) {
            return None;
        }
        let start = counter();
        Some(Self {
            irq: IRQ.swap(0, Ordering::Relaxed),
            start,
            locked: start,
        })
    }

    /// The pass has taken the device mutex once.
    pub(super) fn locked(self) -> Self {
        Self {
            locked: counter(),
            ..self
        }
    }
}

/// One retired job: its engine, doorbell counter, gap word and GPU start and end.
fn job(completion: &Completion, gpu: [u64; 2]) -> (usize, u64, u64, [u64; 2]) {
    let words = completion.latency();
    (
        engine(completion),
        words[0].load(Ordering::Relaxed),
        words[1].load(Ordering::Relaxed),
        gpu,
    )
}

/// Finishes `value` in the pass `pass`, recording a retired job's points.
pub(super) fn finish(pass: Pass, value: Deferred) {
    let retired = match &value {
        Deferred::RetiredCompute(completion, ticks, _) => Some(job(completion, [ticks[0], ticks[1]])),
        Deferred::Retired(completion, Ok(ticks)) => Some(if completion.is_compute() {
            job(completion, [ticks[0], ticks[1]])
        } else {
            job(completion, [ticks[0], ticks[3]])
        }),
        _ => None,
    };
    let Some((engine, doorbell, gap, gpu)) = retired else {
        value.finish();
        return;
    };
    let signal = counter();
    value.finish();
    let signalled = counter();
    LAST_SIGNAL[engine].store(signalled, Ordering::Relaxed);
    let known = |at: u64, delta: i64| if at == 0 { MISSING } else { delta };
    record(engine, DOORBELL_IRQ, known(doorbell, known(pass.irq, ns(doorbell, pass.irq))));
    record(engine, IRQ_PASS, known(pass.irq, ns(pass.irq, pass.start)));
    record(engine, PASS_LOCKED, ns(pass.start, pass.locked));
    record(engine, LOCKED_SIGNAL, ns(pass.locked, signal));
    record(engine, SIGNAL_CALL, ns(signal, signalled));
    record(engine, DOORBELL_SIGNAL, known(doorbell, ns(doorbell, signal)));
    let doorbell_ticks = if doorbell == 0 { 0 } else { gpu_ticks(doorbell) };
    record(engine, DOORBELL_GPU_START, ticks_ns(doorbell_ticks, gpu[0]));
    record(engine, GPU_BUSY, ticks_ns(gpu[0], gpu[1]));
    let irq_ticks = if pass.irq == 0 { 0 } else { gpu_ticks(pass.irq) };
    record(engine, GPU_END_IRQ, ticks_ns(gpu[1], irq_ticks));
    record(engine, SIGNAL_DOORBELL, if doorbell == 0 || gap == NO_GAP { MISSING } else { gap as i64 });
}
