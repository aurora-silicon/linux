#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for where the G17 event work runs (`drivers/gpu/drm/asahi/g17/mod.rs`).

Compiles the real `Shared::new_events_queue()`, `Shared::queue_event_records()`,
`Shared::note_submitter()` and `Shared::submitter()` on the host against a mock
workqueue that records each queue's kind and every enqueue. A model of the
kernel's placement then says which CPU the woken worker runs on:

- a per-CPU queue runs `enqueue_on(cpu)` on `cpu` and `enqueue()` on the
  queuing CPU (`queue_work_on()` on a bound pool);
- an unbound queue only takes the affinity pod (`WQ_AFFN_CACHE`, the CPUs
  sharing an L2) from the CPU it is aimed at, and the scheduler wakes the
  worker on an idle CPU of that pod (`select_idle_sibling()`), the aimed CPU
  itself only when it is idle or the pod has no idle CPU.

The event work has the higher priority, so a worker woken on the CPU of a
running submitter preempts it. If that submitter holds a lock the pass takes
(a compute cell in the middle of a publication), the worker cannot spin on an
owner that is not running: it sleeps until the unlock and preempts again, two
context switches of the submitter.

`queue_event_records()` queues the event work where the completion is taken
when that CPU shares the last submitter's cluster (`Cpumask::cluster_of()`,
mocked from the test's topology), and aims it at the submitter otherwise.

Checks:
1. M5: its queue is unbound, reclaim-safe and high priority; a completion taken
   on the submitter's own CPU, on another cluster or on the other die runs in
   the submitter's pod, never preempting a submitter that is publishing; the
   worker follows a moved submitter; before the first publication the work is
   queued locally.
2. Other G17 parts: the queue is per-CPU, as `system_highpri_wq`; the event
   work runs on the CPU that queued it in the submitter's cluster, and on the
   submitter's CPU from another cluster.
3. Structure: `Gpu::new()` builds the queue with `new_events_queue(m5)`,
   `aurora_g17_events` is allocated only by `new_events_queue()`, and the
   schedulers' timeout work still uses that queue.

Mutations: `percpu-always` (a per-CPU queue on every part, as M5 had with the
per-CPU queue), `unbound-always`, `aim-local` (never aim at the submitter),
`record-never` (never record the submitter), `not-reclaim`, and `split-flag`
(the queue kind no longer follows `m5`).
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / 'drivers/gpu/drm/asahi/g17/mod.rs'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


UNBOUND = 'workqueue::OwnedQueue::new_unbound_reclaim(c"aurora_g17_events", true)'
PERCPU = 'workqueue::OwnedQueue::new_percpu_reclaim(c"aurora_g17_events", true)'
CONSTRUCT = 'let events = Arc::new(Shared::new_events_queue(m5)?, GFP_KERNEL)?;'


def build_harness(mutation=None):
    src = MOD.read_text()

    if mutation == 'split-flag':
        src = replace_once(src, CONSTRUCT,
                           'let events = Arc::new(Shared::new_events_queue(false)?, GFP_KERNEL)?;')

    new_queue = item(src, 'fn new_events_queue(')
    queue_records = item(src, 'fn queue_event_records(')
    note = item(src, 'fn note_submitter(')
    submitter = item(src, 'fn submitter(')

    if mutation == 'percpu-always':
        # The code before this change: a per-CPU queue on every part.
        new_queue = replace_once(new_queue, UNBOUND, PERCPU)
    elif mutation == 'unbound-always':
        new_queue = replace_once(new_queue, PERCPU, UNBOUND)
    elif mutation == 'not-reclaim':
        new_queue = replace_once(new_queue, UNBOUND,
                                 'workqueue::OwnedQueue::new_unbound(c"aurora_g17_events", true)')
    elif mutation == 'aim-local':
        queue_records = replace_once(
            queue_records,
            'self.events.enqueue_on::<Arc<Shared>, EVENT_WORK>(cpu, self.clone())',
            'self.events.enqueue::<Arc<Shared>, EVENT_WORK>(self.clone())')
    elif mutation == 'record-never':
        note = replace_once(note, 'if self.submitter.load(Ordering::Relaxed) != cpu {', 'if false {')

    # Gpu::new() builds the queue from `m5`, the value the harness's `Shared::build()` passes.
    derive_ok = src.count(CONSTRUCT) == 1
    only_here = src.count('c"aurora_g17_events"') == new_queue.count('c"aurora_g17_events"') == 2
    fence_ok = 'self.events.clone()' in item(src, 'fn fence_queue(')

    return f'''
#![allow(dead_code)]
use std::sync::atomic::{{AtomicU32, Ordering}};
use std::sync::Arc;

type Result<T = (), E = ()> = core::result::Result<T, E>;

mod kernel {{
    pub mod cpumask {{
        use std::cell::RefCell;
        thread_local! {{ pub static CLUSTERS: RefCell<Vec<u32>> = const {{ RefCell::new(Vec::new()) }}; }}

        /// The CPUs of one cluster (from the test's topology; each CPU its own without one).
        pub struct Cpumask(Vec<u32>);

        impl Cpumask {{
            pub fn cluster_of(cpu: super::cpu::CpuId) -> Self {{
                let cpu = cpu.as_u32();
                CLUSTERS.with(|clusters| {{
                    let clusters = clusters.borrow();
                    match clusters.get(cpu as usize) {{
                        Some(&cluster) => Cpumask(
                            (0..clusters.len() as u32)
                                .filter(|&c| clusters[c as usize] == cluster)
                                .collect(),
                        ),
                        None => Cpumask(vec![cpu]),
                    }}
                }})
            }}
            pub fn test(&self, cpu: super::cpu::CpuId) -> bool {{
                self.0.contains(&cpu.as_u32())
            }}
        }}
    }}

    pub mod cpu {{
        use std::cell::Cell;
        thread_local! {{ pub static CURRENT: Cell<u32> = const {{ Cell::new(0) }}; }}
        const NR_CPU_IDS: u32 = 64;

        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub struct CpuId(u32);

        impl CpuId {{
            pub fn current() -> Self {{
                CpuId(CURRENT.with(|cpu| cpu.get()))
            }}
            pub fn from_u32(id: u32) -> Option<Self> {{
                (id < NR_CPU_IDS).then_some(CpuId(id))
            }}
            pub fn as_u32(self) -> u32 {{
                self.0
            }}
        }}

        pub fn run_on<R>(cpu: u32, f: impl FnOnce() -> R) -> R {{
            let previous = CURRENT.with(|current| current.replace(cpu));
            let result = f();
            CURRENT.with(|current| current.set(previous));
            result
        }}
    }}
}}

mod workqueue {{
    use super::Result;
    use std::cell::RefCell;

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub enum Call {{
        /// `queue_work()`: on the queuing CPU (`WORK_CPU_UNBOUND`).
        Local {{ queuer: u32 }},
        /// `queue_work_on(cpu)`.
        On {{ cpu: u32 }},
    }}

    pub struct OwnedQueue {{
        pub unbound: bool,
        pub highpri: bool,
        pub reclaim: bool,
        pub calls: RefCell<Vec<Call>>,
    }}

    impl OwnedQueue {{
        fn make(unbound: bool, highpri: bool, reclaim: bool) -> Result<Self> {{
            Ok(Self {{ unbound, highpri, reclaim, calls: RefCell::new(Vec::new()) }})
        }}
        pub fn new_unbound(_name: &core::ffi::CStr, highpri: bool) -> Result<Self> {{
            Self::make(true, highpri, false)
        }}
        pub fn new_unbound_reclaim(_name: &core::ffi::CStr, highpri: bool) -> Result<Self> {{
            Self::make(true, highpri, true)
        }}
        pub fn new_percpu_reclaim(_name: &core::ffi::CStr, highpri: bool) -> Result<Self> {{
            Self::make(false, highpri, true)
        }}
        pub fn enqueue<W, const ID: u64>(&self, _w: W) -> core::result::Result<(), W> {{
            let queuer = crate::kernel::cpu::CpuId::current().as_u32();
            self.calls.borrow_mut().push(Call::Local {{ queuer }});
            Ok(())
        }}
        pub fn enqueue_on<W, const ID: u64>(
            &self,
            cpu: crate::kernel::cpu::CpuId,
            _w: W,
        ) -> core::result::Result<(), W> {{
            self.calls.borrow_mut().push(Call::On {{ cpu: cpu.as_u32() }});
            Ok(())
        }}
    }}
}}

use workqueue::Call;

const EVENT_WORK: u64 = 0;

struct Shared {{
    submission_enabled: bool,
    events: Arc<workqueue::OwnedQueue>,
    submitter: AtomicU32,
}}

impl Shared {{
{new_queue}

{queue_records}

{note}

{submitter}
}}

impl Shared {{
    /// As `Gpu::new()` builds it: the queue kind from `m5`.
    fn build(m5: bool) -> Arc<Self> {{
        let events = Arc::new(Shared::new_events_queue(m5).expect("events queue"));
        Arc::new(Shared {{
            submission_enabled: true,
            events,
            submitter: AtomicU32::new(u32::MAX),
        }})
    }}

    /// The completion interrupt, taken on `cpu`, queues the event pass.
    fn complete_on(self: &Arc<Self>, cpu: u32) -> Call {{
        kernel::cpu::run_on(cpu, || self.queue_event_records());
        *self.events.calls.borrow().last().expect("the event work was queued")
    }}

    fn publish_on(&self, cpu: u32) {{
        kernel::cpu::run_on(cpu, || self.note_submitter());
    }}
}}

/// CPUs grouped by die and by cluster (the L2, the unbound queue's affinity pod).
struct Topology {{
    cluster: Vec<u32>,
    die: Vec<u32>,
}}

impl Topology {{
    fn new(dies: u32, clusters_per_die: u32, cpus_per_cluster: u32) -> Self {{
        let mut cluster = Vec::new();
        let mut die = Vec::new();
        for d in 0..dies {{
            for c in 0..clusters_per_die {{
                for _ in 0..cpus_per_cluster {{
                    cluster.push(d * clusters_per_die + c);
                    die.push(d);
                }}
            }}
        }}
        kernel::cpumask::CLUSTERS.with(|clusters| *clusters.borrow_mut() = cluster.clone());
        Self {{ cluster, die }}
    }}
    fn cpus(&self) -> u32 {{
        self.cluster.len() as u32
    }}
}}

/// Which CPU the woken event worker runs on.
fn worker_cpu(topo: &Topology, queue: &workqueue::OwnedQueue, call: Call, busy: &[bool]) -> u32 {{
    match (queue.unbound, call) {{
        (false, Call::On {{ cpu }}) => cpu,
        (false, Call::Local {{ queuer }}) => queuer,
        (true, call) => {{
            let aim = match call {{
                Call::On {{ cpu }} => cpu,
                Call::Local {{ queuer }} => queuer,
            }};
            if !busy[aim as usize] {{
                return aim;
            }}
            (0..topo.cpus())
                .find(|&c| topo.cluster[c as usize] == topo.cluster[aim as usize] && !busy[c as usize])
                .unwrap_or(aim)
        }}
    }}
}}

/// Context switches the completion costs a submitter publishing on `submitter` while
/// holding a lock the event pass takes.
fn submitter_switches(worker: u32, submitter: u32, busy: &[bool]) -> u32 {{
    if worker == submitter && busy[submitter as usize] {{ 2 }} else {{ 0 }}
}}

fn busy(topo: &Topology, cpus: &[u32]) -> Vec<bool> {{
    let mut busy = vec![false; topo.cpus() as usize];
    for &cpu in cpus {{
        busy[cpu as usize] = true;
    }}
    busy
}}

fn test_m5_queue_kind() {{
    let shared = Shared::build(true);
    assert!(shared.events.unbound, "M5 event work aimed at the submitter needs an unbound queue");
    assert!(shared.events.reclaim, "the event work signals job fences: WQ_MEM_RECLAIM");
    assert!(shared.events.highpri, "the event work keeps system_highpri_wq's priority");
}}

fn test_other_queue_kind() {{
    let shared = Shared::build(false);
    assert!(!shared.events.unbound, "other G17 parts keep a per-CPU event queue");
    assert!(shared.events.reclaim, "the event work signals job fences: WQ_MEM_RECLAIM");
    assert!(shared.events.highpri, "the event work keeps system_highpri_wq's priority");
}}

/// The measured case: the completion interrupt is taken on the CPU of the submitter, which
/// is in the middle of a publication.
fn test_m5_completion_on_submitter_cpu() {{
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    shared.publish_on(16);
    let busy = busy(&topo, &[16]);
    let call = shared.complete_on(16);
    assert_eq!(call, Call::Local {{ queuer: 16 }}, "the submitter's cluster queues the event work locally");
    let worker = worker_cpu(&topo, &shared.events, call, &busy);
    assert_ne!(worker, 16, "the event worker must not preempt the publishing submitter");
    assert_eq!(topo.cluster[worker as usize], topo.cluster[16], "the worker shares the submitter's L2");
    assert_eq!(submitter_switches(worker, 16, &busy), 0);
}}

fn test_m5_completion_on_other_cluster() {{
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    shared.publish_on(16);
    let busy = busy(&topo, &[16, 2]);
    let call = shared.complete_on(2);
    let worker = worker_cpu(&topo, &shared.events, call, &busy);
    assert_eq!(topo.cluster[worker as usize], topo.cluster[16],
               "the worker runs in the submitter's cluster, not the interrupt's");
    assert_ne!(worker, 16);
    assert_eq!(submitter_switches(worker, 16, &busy), 0);
}}

fn test_m5_two_dies() {{
    let topo = Topology::new(2, 2, 6);
    let shared = Shared::build(true);
    shared.publish_on(20);
    let busy = busy(&topo, &[20, 3]);
    let call = shared.complete_on(3);
    let worker = worker_cpu(&topo, &shared.events, call, &busy);
    assert_eq!(topo.die[worker as usize], topo.die[20], "completion processing stays on the submitter's die");
    assert_eq!(topo.cluster[worker as usize], topo.cluster[20]);
    assert_ne!(worker, 20);
}}

fn test_m5_idle_submitter() {{
    // A submitter waiting on a fence leaves its CPU idle: running there preempts nobody.
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    shared.publish_on(16);
    let busy = busy(&topo, &[]);
    let call = shared.complete_on(3);
    let worker = worker_cpu(&topo, &shared.events, call, &busy);
    assert_eq!(topo.cluster[worker as usize], topo.cluster[16]);
    assert_eq!(submitter_switches(worker, 16, &busy), 0);
}}

fn test_m5_submitter_moves() {{
    // Two submitters on different clusters: the event work follows the last one, and never
    // preempts either while it publishes.
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    shared.publish_on(16);
    shared.publish_on(7);
    let busy = busy(&topo, &[16, 7]);
    let call = shared.complete_on(16);
    assert_eq!(call, Call::On {{ cpu: 7 }});
    let worker = worker_cpu(&topo, &shared.events, call, &busy);
    assert_eq!(topo.cluster[worker as usize], topo.cluster[7]);
    assert_eq!(submitter_switches(worker, 7, &busy) + submitter_switches(worker, 16, &busy), 0);
}}

fn test_m5_before_first_publication() {{
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    assert!(shared.submitter().is_none());
    let call = shared.complete_on(4);
    assert_eq!(call, Call::Local {{ queuer: 4 }}, "nothing published yet: queued locally");
    let worker = worker_cpu(&topo, &shared.events, call, &busy(&topo, &[4]));
    assert_eq!(topo.cluster[worker as usize], topo.cluster[4]);
}}

fn test_m5_completion_stream() {{
    // Every completion of a deep queue arrives while the submitter publishes the next job.
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(true);
    let mut switches = 0;
    for job in 0..64u32 {{
        shared.publish_on(16);
        let busy = busy(&topo, &[16]);
        let irq = if job % 2 == 0 {{ 16 }} else {{ 1 }};
        let call = shared.complete_on(irq);
        let worker = worker_cpu(&topo, &shared.events, call, &busy);
        assert_eq!(topo.cluster[worker as usize], topo.cluster[16]);
        switches += submitter_switches(worker, 16, &busy);
    }}
    assert_eq!(switches, 0, "completions must not cost the submitter context switches");
}}

fn test_other_runs_on_queuer() {{
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(false);
    shared.publish_on(4);
    assert_eq!(shared.submitter().map(|cpu| cpu.as_u32()), Some(4), "every G17 records the submitter");
    let busy = busy(&topo, &[3, 4]);
    let call = shared.complete_on(3);
    assert_eq!(call, Call::Local {{ queuer: 3 }});
    assert_eq!(worker_cpu(&topo, &shared.events, call, &busy), 3,
               "in the submitter's cluster other G17 parts run the event work where it is queued");
}}

fn test_other_follows_submitter_cluster() {{
    let topo = Topology::new(1, 3, 6);
    let shared = Shared::build(false);
    shared.publish_on(16);
    let busy = busy(&topo, &[3]);
    let call = shared.complete_on(3);
    assert_eq!(call, Call::On {{ cpu: 16 }}, "from another cluster the event work is aimed at the submitter");
    assert_eq!(worker_cpu(&topo, &shared.events, call, &busy), 16);
}}

fn test_disabled_submission() {{
    let shared = Shared::build(true);
    let shared = Arc::new(Shared {{
        submission_enabled: false,
        events: shared.events.clone(),
        submitter: AtomicU32::new(u32::MAX),
    }});
    shared.publish_on(16);
    kernel::cpu::run_on(16, || shared.queue_event_records());
    assert!(shared.events.calls.borrow().is_empty(), "no event work without submission");
}}

fn main() {{
    assert!({str(derive_ok).lower()}, "Gpu::new() must build the events queue with new_events_queue(m5)");
    assert!({str(only_here).lower()}, "aurora_g17_events must be allocated only by Shared::new_events_queue()");
    assert!({str(fence_ok).lower()}, "the schedulers' timeout work must stay on the events queue");
    test_m5_completion_on_submitter_cpu();
    test_m5_completion_on_other_cluster();
    test_m5_two_dies();
    test_m5_idle_submitter();
    test_m5_submitter_moves();
    test_m5_before_first_publication();
    test_m5_completion_stream();
    test_other_runs_on_queuer();
    test_other_follows_submitter_cluster();
    test_disabled_submission();
    test_m5_queue_kind();
    test_other_queue_kind();
    println!("PASS g17-m5-event-placement-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--mutation',
        choices=[
            'percpu-always',
            'unbound-always',
            'aim-local',
            'record-never',
            'not-reclaim',
            'split-flag',
        ],
    )
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-event-placement-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '--edition', '2021', '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
