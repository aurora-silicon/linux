#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for mixed-engine compute_direct() render-prefix resolution.

Verifies:
1. `Context::render_active` RAII accounting with `Active::drop()` balances across
   push, failed push, retirement, and drop.
2. `Backend::compute_direct()` no longer bails out unconditionally on
   `self.context.render_seen()`.
3. When `render_active() == 0`, `compute_direct()` skips `self.shared.state.lock()`
   completely; when `render_active() != 0` and `wait_through[0].is_some()`, it takes
   `self.shared.state.lock()` briefly to resolve `firmware.queues.render_prefix()`
   (checking `lease.is_current()`, `!firmware.recovery.pending()`, and
   `!firmware.queues.render.replay_parked()`), drops the device mutex before taking
   `target_cell.lock()` / `queue.publish()`, and merges the render frontier into
   `direct_frontier`.
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


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        if source.count(part) < 1:
            return False
        idx = source.find(part, at + 1)
        if idx < 0:
            return False
        at = idx
    return True


def build_harness(mutation=None):
    runtime_src = (G17 / 'runtime.rs').read_text()
    render_src = (G17 / 'queue/render.rs').read_text()
    context_src = (G17 / 'context.rs').read_text()

    direct_fn = item(runtime_src, 'fn compute_direct(')
    drop_impl = item(render_src, 'impl Drop for Active')
    push_snippet_ok = 'packet.context.inc_render_active();\n        self.active\n            .push(Active {' in render_src
    context_ok = (
        'render_active: AtomicU32,' in context_src
        and 'pub(crate) fn render_active(&self) -> u32' in context_src
        and 'pub(crate) fn inc_render_active(&self)' in context_src
        and 'pub(crate) fn dec_render_active(&self)' in context_src
    )

    if mutation == 'bail-on-render-seen':
        direct_fn = direct_fn.replace(
            'if (has_sources && !m5)\n            || bound_count == 0',
            'if (has_sources && !m5)\n            || self.context.render_seen()\n            || bound_count == 0',
            1,
        )
    elif mutation == 'skip-render-prefix':
        direct_fn = direct_fn.replace(
            'firmware.queues.render_prefix(self.owner, packet)',
            'Ok(super::dependency::Frontier::new())',
            1,
        )
    elif mutation == 'ignore-replay-parked':
        direct_fn = direct_fn.replace(
            '|| firmware.queues.render.replay_parked()',
            '',
            1,
        )
    elif mutation == 'drop-active-dec':
        drop_impl = drop_impl.replace('self.packet.context.dec_render_active();', '', 1)

    no_render_seen_bail = '|| self.context.render_seen()' not in direct_fn
    prefix_contract_ok = ordered_ok(
        direct_fn,
        'packet.check_firmware_producers()',
        'self.context.render_seen()',
        'packet.order.wait_through[0].is_some()',
        'self.context.render_active() != 0',
        'let state = self.shared.state.lock();',
        '!lease.is_current()',
        'firmware.recovery.pending()',
        'firmware.queues.render.replay_parked()',
        'firmware.queues.render_prefix(self.owner, packet)',
        'direct_frontier.insert(0, *dep)',
        'let mut queue = target_cell.lock()?;',
        'queue.publish(',
        'direct_frontier.as_slice(),',
    )
    # Ensure state.lock() is dropped before target_cell.lock().
    lock_scope_ok = (
        direct_fn.index('let state = self.shared.state.lock();')
        < direct_fn.index('for dep in render_frontier.as_slice()')
        < direct_fn.index('let mut queue = target_cell.lock()?;')
    )
    drop_dec_ok = 'self.packet.context.dec_render_active();' in drop_impl

    return f'''
use std::sync::{{Arc, atomic::{{AtomicU32, Ordering}}}};

struct Context {{
    render_active: AtomicU32,
}}

impl Context {{
    fn new() -> Arc<Self> {{ Arc::new(Self {{ render_active: AtomicU32::new(0) }}) }}
    fn render_active(&self) -> u32 {{ self.render_active.load(Ordering::Acquire) }}
    fn inc_render_active(&self) {{ self.render_active.fetch_add(1, Ordering::AcqRel); }}
    fn dec_render_active(&self) {{ self.render_active.fetch_sub(1, Ordering::AcqRel); }}
}}

struct Packet {{ context: Arc<Context> }}
struct Active {{ packet: Arc<Packet> }}
{drop_impl}

struct Tracker {{
    cap: usize,
    items: Vec<Active>,
}}

impl Tracker {{
    fn new(cap: usize) -> Self {{ Self {{ cap, items: Vec::new() }} }}
    fn push(&mut self, a: Active) -> Result<(), ()> {{
        if self.items.len() >= self.cap {{
            return Err(());
        }}
        self.items.push(a);
        Ok(())
    }}
    fn pop(&mut self) -> Option<Active> {{
        if self.items.is_empty() {{ None }} else {{ Some(self.items.remove(0)) }}
    }}
}}

fn main() {{
    assert!({str(context_ok).lower()}, "Context must expose atomic render_active counter");
    assert!({str(push_snippet_ok).lower()}, "Pair::publish must increment render_active before pushing Active");
    assert!({str(no_render_seen_bail).lower()}, "compute_direct must not bail unconditionally on render_seen()");
    assert!({str(prefix_contract_ok).lower()}, "compute_direct must resolve active render prefix and check recovery/replay");
    assert!({str(lock_scope_ok).lower()}, "compute_direct must drop device mutex before locking compute cell");
    assert!({str(drop_dec_ok).lower()}, "Active::drop must decrement render_active");

    let ctx = Context::new();
    let pkt = Arc::new(Packet {{ context: ctx.clone() }});
    let mut tracker = Tracker::new(2);

    // Push two active renders.
    for _ in 0..2 {{
        pkt.context.inc_render_active();
        tracker.push(Active {{ packet: pkt.clone() }}).unwrap();
    }}
    assert_eq!(ctx.render_active(), 2);

    // Failed push drops Active immediately, leaving render_active balanced.
    pkt.context.inc_render_active();
    assert!(tracker.push(Active {{ packet: pkt.clone() }}).is_err());
    assert_eq!(ctx.render_active(), 2);

    // Retiring one drops it to 1; dropping the tracker drops it to 0.
    drop(tracker.pop());
    assert_eq!(ctx.render_active(), 1);
    drop(tracker);
    assert_eq!(ctx.render_active(), 0);

    println!("PASS g17-direct-compute-render-prefix-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=[
        'bail-on-render-seen',
        'skip-render-prefix',
        'ignore-replay-parked',
        'drop-active-dec',
    ])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-dc-rprefix-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
