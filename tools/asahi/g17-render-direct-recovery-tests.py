#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host lifecycle test for render_direct() recovery crossing publication and doorbell contract.

Compiles production `render_doorbell_message()` (from `runtime/render.rs`) and
`finish_publication()` (from `runtime/backend.rs`), and drives the real
post-publication recovery check extracted from `render_direct()`
(`runtime/render/direct.rs`) through a full lifecycle harness:
1. Recovery starts while a render pair is lent and published (same generation,
   `recovery_state != 0`): `render_direct()` returns `Ok(())`, `return_render()`
   returns the published pair before `lease` drops, `queue_events()` runs,
   `finish_publication()` does NOT fail the unrelated client's VM, and the
   subsequent recovery pass settles the pair exactly once.
2. Generation changes mid-publication (`port.generation() != generation`):
   `render_direct()` returns `Err(EIO)` and `finish_publication()` fails the VM.
3. Shared `render_doorbell_message()` contract: M5 (!J813_FORMS) uses pipe 2,
   J813 (J813_FORMS) uses pipe 0, non-M5 uses pipe 0, and both render publishers
   delegate to `render_doorbell_message()`.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build_harness(mutation=None):
    render_src = (G17 / 'runtime/render.rs').read_text()
    direct_src = (G17 / 'runtime/render/direct.rs').read_text()
    backend_src = (G17 / 'runtime/backend.rs').read_text()

    doorbell_fn = item(render_src, 'fn render_doorbell_message(')
    direct_fn = item(direct_src, 'pub(in crate::g17::runtime) fn render_direct(')
    finish_fn = item(backend_src, 'pub(super) fn finish_publication(').replace('pub(super) ', '')

    if mutation == 'wrong-j813-pipe':
        doorbell_fn = doorbell_fn.replace(
            'if m5 && !crate::g17::J813_FORMS.load(core::sync::atomic::Ordering::Relaxed)',
            'if m5',
            1,
        )

    # Extract the exact post-publication check block from render_direct().
    check_start = direct_fn.index('result?;')
    check_end = direct_fn.index('Ok(())\n        })();', check_start) + len('Ok(())')
    post_publish = direct_fn[check_start:check_end]

    if mutation == 'recovery-state-eio':
        post_publish = post_publish.replace(
            'if port.recovery_state()? != 0 {\n                recovery_started = true;\n            }',
            'if port.recovery_state()? != 0 {\n                return Err(EIO);\n            }',
            1,
        )
    elif mutation == 'drop-generation-check':
        post_publish = post_publish.replace(
            'if port.generation() != generation {\n                return Err(EIO);\n            }',
            '',
            1,
        )

    # Verify both locked and direct render publishers share render_doorbell_message()
    # and that render_direct() returns the pair and drops lease before queueing events.
    shared_contract = (
        'notify(render_doorbell_message(m5, priority))' in render_src
        and 'self.port.notify(super::render_doorbell_message(m5, priority))' in direct_src
        and direct_src.count('J813_FORMS') == 0
    )
    order_tokens = [
        'firmware.return_render(slot, &mut loan, activation, packet)',
        'self.shared.note_changed();',
        'drop(lease);',
        'if recovery_started {\n            self.shared.queue_events();\n        }',
    ]
    order_ok = True
    pos = -1
    for tok in order_tokens:
        if direct_fn.count(tok) != 1 or direct_fn.index(tok) <= pos:
            order_ok = False
            break
        pos = direct_fn.index(tok)

    doorbell_host = doorbell_fn.replace('crate::g17::', 'g17_stub::')

    return f'''
use std::cell::Cell;
use std::sync::{{Arc, atomic::{{AtomicBool, AtomicUsize, Ordering}}}};

const EIO: i32 = -5;
const ETIMEDOUT: i32 = -110;
const ENODEV: i32 = -19;
type Result<T = ()> = std::result::Result<T, i32>;
type Fence = u8;

mod g17_stub {{
    use std::sync::atomic::AtomicBool;
    pub const MSG_TYPE_SHIFT: u32 = 48;
    pub static J813_FORMS: AtomicBool = AtomicBool::new(false);
}}

{doorbell_host}

#[derive(PartialEq, Eq)]
enum Engine {{ Render, Compute }}

struct Status(Cell<i32>);
impl Status {{ fn get(&self) -> i32 {{ self.0.get() }} }}

struct Completion {{
    status: Status,
    unpublished_fails: Cell<usize>,
}}
impl Completion {{
    fn status(&self) -> &Status {{ &self.status }}
    fn fail_unpublished(&self, _: i32) {{ self.unpublished_fails.set(self.unpublished_fails.get() + 1); }}
    fn scheduler_fence(&self) -> Fence {{ 1 }}
}}

struct Packet {{
    published: Cell<bool>,
    completion: Arc<Completion>,
}}
impl Packet {{
    fn is_published(&self) -> bool {{ self.published.get() }}
    fn engine(&self) -> Engine {{ Engine::Render }}
}}

struct DeferredBatch {{ finished: usize }}
impl DeferredBatch {{ fn finish(&mut self) {{ self.finished += 1; }} }}

mod job {{
    pub trait Backend {{
        fn fail_vm(&self, error: i32);
    }}
}}

struct Port {{
    gen: Cell<u64>,
    rec_state: Cell<u32>,
}}
impl Port {{
    fn generation(&self) -> u64 {{ self.gen.get() }}
    fn recovery_state(&self) -> Result<u32> {{ Ok(self.rec_state.get()) }}
}}

struct RecoveryHarness {{
    vm_failed: Cell<bool>,
    events_queued: Cell<usize>,
    pair_returned_before_lease_drop: Cell<bool>,
    lease_held: Cell<bool>,
    pair_in_registry: Cell<bool>,
    settlements: Cell<usize>,
    stop_called: Cell<bool>,
}}

impl job::Backend for RecoveryHarness {{
    fn fail_vm(&self, _error: i32) {{
        self.vm_failed.set(true);
    }}
}}

impl RecoveryHarness {{
    fn stop_after_error(&self) {{ self.stop_called.set(true); }}
    fn rollback_unpublished_compute(&self) -> Result {{ Ok(()) }}

    {finish_fn}

    fn run_direct_publish_lifecycle(&self, port: &Port, generation: u64, packet: &Arc<Packet>) -> Result<Fence> {{
        self.lease_held.set(true);
        self.pair_in_registry.set(false);
        let mut recovery_started = false;
        let result: Result = (|| {{
            let result: Result = Ok(());
            packet.published.set(true);
            {post_publish}
        }})();
        // return_render() places the published pair back while lease is still held.
        self.pair_in_registry.set(true);
        self.pair_returned_before_lease_drop.set(self.lease_held.get());
        self.lease_held.set(false);
        if recovery_started {{
            self.events_queued.set(self.events_queued.get() + 1);
        }}
        let mut deferred = DeferredBatch {{ finished: 0 }};
        self.finish_publication(packet, result, &mut deferred)
    }}

    fn run_recovery_pass(&self, port: &Port) {{
        // Recovery drains preparations (lease must already be dropped) and settles the returned pair once.
        assert!(!self.lease_held.get(), "recovery must wait for lease drop");
        if self.pair_in_registry.get() && port.recovery_state().unwrap() != 0 {{
            self.settlements.set(self.settlements.get() + 1);
            port.gen.set(port.gen.get() + 1);
            port.rec_state.set(0);
            self.pair_in_registry.set(false);
        }}
    }}
}}

fn new_packet() -> Arc<Packet> {{
    Arc::new(Packet {{
        published: Cell::new(false),
        completion: Arc::new(Completion {{
            status: Status(Cell::new(0)),
            unpublished_fails: Cell::new(0),
        }}),
    }})
}}

fn new_harness() -> RecoveryHarness {{
    RecoveryHarness {{
        vm_failed: Cell::new(false),
        events_queued: Cell::new(0),
        pair_returned_before_lease_drop: Cell::new(false),
        lease_held: Cell::new(false),
        pair_in_registry: Cell::new(false),
        settlements: Cell::new(0),
        stop_called: Cell::new(false),
    }}
}}

fn main() {{
    assert!({str(shared_contract).lower()}, "both render publishers must share render_doorbell_message");
    assert!({str(order_ok).lower()}, "render_direct must return pair before dropping lease and queueing events");

    // 1. Doorbell pipe contract.
    g17_stub::J813_FORMS.store(false, Ordering::Relaxed);
    assert_eq!(render_doorbell_message(false, 2) & 3, 0);
    assert_eq!(render_doorbell_message(true, 2) & 3, 2);
    assert_eq!((render_doorbell_message(true, 3) >> 2) & 3, 3);
    g17_stub::J813_FORMS.store(true, Ordering::Relaxed);
    assert_eq!(render_doorbell_message(true, 2) & 3, 0);
    g17_stub::J813_FORMS.store(false, Ordering::Relaxed);

    // 2. Recovery crosses publication (same generation, recovery_state != 0):
    //    unrelated client keeps its VM, pair is returned before lease drop, and settles once.
    let h = new_harness();
    let port = Port {{ gen: Cell::new(7), rec_state: Cell::new(1) }};
    let pkt = new_packet();
    let fence = h.run_direct_publish_lifecycle(&port, 7, &pkt).unwrap();
    assert_eq!(fence, 1);
    assert!(!h.vm_failed.get(), "unrelated client VM must survive recovery crossing publication");
    assert!(h.pair_returned_before_lease_drop.get());
    assert_eq!(h.events_queued.get(), 1);
    h.run_recovery_pass(&port);
    h.run_recovery_pass(&port);
    assert_eq!(h.settlements.get(), 1, "published pair must settle exactly once");

    // 3. Generation changed mid-publication: fails with EIO and fails VM.
    let h2 = new_harness();
    let port2 = Port {{ gen: Cell::new(8), rec_state: Cell::new(0) }};
    let pkt2 = new_packet();
    assert_eq!(h2.run_direct_publish_lifecycle(&port2, 7, &pkt2).err(), Some(EIO));
    assert!(h2.vm_failed.get(), "generation mismatch must fail VM");

    println!("PASS g17-render-direct-recovery-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['recovery-state-eio',
                                               'drop-generation-check',
                                               'wrong-j813-pipe'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-rd-rec-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
