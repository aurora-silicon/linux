#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production task-local DMA-fence lockdep guard with and without LOCKDEP."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

root = kernel_root()
out = output_dir()
production = root / 'rust/kernel/dma_fence.rs'
source = production.read_text()
guard = source[source.index('/// A lockdep annotation'):source.index('mod private {')]
harness = r'''
#![allow(dead_code)]
mod bindings {
    use std::cell::{Cell,RefCell};
    std::thread_local! {
        static EVENTS:RefCell<Vec<(bool,bool)>> = const { RefCell::new(Vec::new()) };
        static HELD:Cell<bool> = const { Cell::new(false) };
    }
    pub fn events()->Vec<(bool,bool)> { EVENTS.with(|e|e.borrow().clone()) }
    pub unsafe fn dma_fence_begin_signalling()->bool {
        let cookie=HELD.with(|held|held.replace(true));
        EVENTS.with(|e|e.borrow_mut().push((true,cookie))); cookie
    }
    pub unsafe fn dma_fence_end_signalling(cookie:bool) {
        if !cookie { HELD.with(|held|held.set(false)); }
        EVENTS.with(|e|e.borrow_mut().push((false,cookie)));
    }
}
__GUARD__
#[test]
fn nested_sections_keep_their_own_cookie_and_close_in_order() {
    let outer=SignallingGuard::new(); let inner=SignallingGuard::new(); let deepest=SignallingGuard::new();
    drop(deepest); drop(inner); drop(outer);
    #[cfg(CONFIG_LOCKDEP)] assert_eq!(bindings::events(),[(true,false),(true,true),(true,true),(false,true),(false,true),(false,false)]);
    #[cfg(not(CONFIG_LOCKDEP))] assert!(bindings::events().is_empty());
}
#[test]
fn early_return_closes_the_annotation() {
    fn refuse()->Result<(),()> { let _section=SignallingGuard::new(); Err(()) }
    assert!(refuse().is_err());
    #[cfg(CONFIG_LOCKDEP)] assert_eq!(bindings::events(),[(true,false),(false,false)]);
    #[cfg(not(CONFIG_LOCKDEP))] assert!(bindings::events().is_empty());
}
'''.replace('__GUARD__', guard)
unit = out / 'fence-signalling.rs'
unit.write_text(harness)
logs = ''
for lockdep in [False, True]:
    binary = out / ('fence-signalling-' + str(int(lockdep)))
    cfg = ['--cfg', 'CONFIG_LOCKDEP'] if lockdep else []
    for command in [rustc() + ['--edition=2021', '--test', str(unit), '-o', str(binary)] + cfg,
                    [str(binary), '--nocapture']]:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs += result.stdout
        print(result.stdout, end='')
        (out / 'fence-signalling.log').write_text(logs)
        if result.returncode:
            raise SystemExit(result.returncode)
# The cookie belongs to a task: moving its production guard to another thread must fail.
not_send = out / 'fence-signalling-not-send.rs'
not_send.write_text(harness + '\nfn main(){ let guard=SignallingGuard::new(); std::thread::spawn(move||drop(guard)); }\n')
result = subprocess.run(rustc() + ['--edition=2021', str(not_send), '-o', str(out/'not-send')],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'fence-signalling-not-send.log').write_text(result.stdout)
assert result.returncode and 'E0277' in result.stdout and 'cannot be sent between threads safely' in result.stdout, result.stdout
(out / 'fence-signalling.json').write_text(json.dumps({
    'passed': True, 'tests': 4, 'negative_compile_checks': 1, 'cpu_only': True,
    'scope': 'Production RAII annotation compiled in both CONFIG_LOCKDEP variants with task-cookie mock; does not exercise kernel lockdep or GPU lock ordering.',
    'source_sha256': sha(production),
}, indent=2) + '\n')
