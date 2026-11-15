#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise M4's production recovery decisions and owner ledger without a GPU."""

from common import kernel_root, output_dir, rustc
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = kernel_root()
production = root / 'drivers/gpu/drm/asahi/g16'
output = args.output or output_dir()
output.mkdir(parents=True, exist_ok=True)

harness = r'''
#![allow(dead_code)]
#[path = "__ROOT__/pools/lifetime.rs"] mod lifetime;
#[path = "__ROOT__/recovery/logic.rs"] mod logic;

#[cfg(test)] mod integration {
    use super::lifetime::{Counts, Lifetime};
    use super::logic::{self, Retirement};

    // This is the retirement policy used by poll.rs: an affected fence does not
    // erase the independently authenticated owner receipt. No synthetic receipt
    // is generated here; the hardware poller remains responsible for that proof.
    fn finish(owners: &mut Lifetime, unit: usize, receipt: Retirement, affected: bool) -> bool {
        assert!(receipt.complete());
        if receipt.owner_retired() {
            assert!(owners.retire(unit));
        } else {
            owners.quarantine(unit);
        }
        receipt.job_failed(affected)
    }

    #[test]
    fn ten_thousand_affected_full_receipts_preserve_displacement() {
        let mut owners = Lifetime::new();
        for number in 0..10_000 {
            let unit = number % 2;
            assert!(owners.claim(unit));
            assert!(owners.publish(unit));
            assert!(finish(&mut owners, unit, Retirement::Full, true));
            assert!(!owners.exhausted());
            assert_eq!(owners.counts().quarantined, 0);
        }
        assert_eq!(owners.counts().retired, 2);
        assert_eq!(owners.counts().reusable, 1);
    }

    #[test]
    fn actual_abort_never_supplies_a_displacement_receipt() {
        let mut owners = Lifetime::new();
        assert!(owners.claim(0));
        assert!(owners.publish(0));
        assert!(!finish(&mut owners, 0, Retirement::Full, false));
        assert!(owners.claim(1));
        assert!(owners.publish(1));
        assert!(finish(&mut owners, 1, Retirement::Aborted, false));
        for _ in 0..10_000 {
            assert!(!owners.claim(0));
            assert!(!owners.claim(1));
            assert!(owners.exhausted());
        }
        assert_eq!(owners.counts(), Counts {
            retired: 1, quarantined: 1, ..Counts::default()
        });
    }

    #[test]
    fn exhausted_compute_ledger_does_not_poison_render_ledger() {
        let mut compute = Lifetime::new();
        let mut render = Lifetime::new();
        assert!(compute.claim(0));
        assert!(compute.publish(0));
        finish(&mut compute, 0, Retirement::Aborted, true);
        // The remaining fresh owner can run once, but cannot make the aborted
        // one reusable. Class exhaustion must remain explicit after this point.
        assert!(compute.claim(1));
        assert!(compute.publish(1));
        finish(&mut compute, 1, Retirement::Full, false);
        assert!(compute.exhausted());
        for number in 0..10_000 {
            let unit = number % 2;
            assert!(render.claim(unit));
            assert!(render.publish(unit));
            assert!(!finish(&mut render, unit, Retirement::Full, false));
            assert!(!render.exhausted());
        }
        assert_eq!(compute.counts().quarantined, 1);
        assert_eq!(render.counts().quarantined, 0);
    }

    #[test]
    fn two_aborts_stay_bounded_and_cannot_be_reset_by_preparation_cancel() {
        let mut owners = Lifetime::new();
        for unit in 0..2 {
            assert!(owners.claim(unit));
            assert!(owners.publish(unit));
            finish(&mut owners, unit, Retirement::Aborted, true);
            owners.cancel(unit);
        }
        assert!(owners.exhausted());
        assert_eq!(owners.counts().quarantined, 2);
        assert_eq!(owners.counts().reusable, 0);
    }

    #[test]
    fn repeated_handshakes_and_storm_limit_preserve_owner_accounting() {
        let mut owners = Lifetime::new();
        let (mut window, mut count) = (1, 0);
        for number in 0..logic::WINDOW_LIMIT {
            assert!(logic::admit(100, &mut window, &mut count));
            assert_eq!(logic::action(1, false, false), logic::RecoveryAction::MarkAndAcknowledge);
            assert_eq!(logic::action(2, true, true), logic::RecoveryAction::Wait);
            assert_eq!(logic::action(3, true, true), logic::RecoveryAction::Resume);
            let unit = number as usize % 2;
            assert!(owners.claim(unit));
            assert!(owners.publish(unit));
            assert!(finish(&mut owners, unit, Retirement::Full, true));
        }
        let before = owners.counts();
        assert!(!logic::admit(101, &mut window, &mut count));
        assert_eq!(owners.counts(), before);
        assert_eq!(before.quarantined, 0);
        assert!(logic::admit(logic::WINDOW_NS + 2, &mut window, &mut count));
    }
}
'''.replace('__ROOT__', str(production))

source = output / 'recovery-host.rs'
source.write_text(harness)
log = ''
for command in [
    rustc() + [ '--edition=2021', '--test', str(source), '-o', str(output / 'recovery-host')],
    [str(output / 'recovery-host'), '--nocapture'],
]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (output / 'recovery-host.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)

tests = int(re.search(r'test result: ok\. (\d+) passed;', log)[1])
(output / 'recovery-host.json').write_text(json.dumps({
    'result': 'pass',
    'tests': tests,
    'scope': 'Production recovery decisions and owner ledger; CPU-only tests, no hardware recovery qualification.',
    'known_limit': 'A genuine aborted owner remains quarantined until reboot; two-owner class capacity is bounded.',
    'source': {
        name: hashlib.sha256((production / name).read_bytes()).hexdigest()
        for name in ['recovery/logic.rs', 'pools/lifetime.rs', 'poll.rs', 'recovery.rs']
    },
}, indent=2) + '\n')
