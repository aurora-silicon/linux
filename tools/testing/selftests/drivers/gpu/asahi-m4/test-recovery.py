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
    // erase the independently authenticated owner receipt, and an aborted owner is
    // abandoned (its job fails, its storage stays retained, its unit serves other
    // storage). No synthetic receipt is generated here; the hardware poller remains
    // responsible for that proof.
    fn finish<const N: usize>(owners: &mut Lifetime<N>, unit: usize, receipt: Retirement, affected: bool) -> bool {
        assert!(receipt.complete());
        if receipt.owner_retired() {
            assert!(owners.retire(unit));
        } else {
            owners.abandon(unit);
        }
        receipt.job_failed(affected)
    }

    #[test]
    fn ten_thousand_affected_full_receipts_preserve_displacement() {
        let mut owners = Lifetime::<2>::new(false);
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
        let mut owners = Lifetime::<2>::new(false);
        assert!(owners.claim(0));
        assert!(owners.publish(0));
        assert!(!finish(&mut owners, 0, Retirement::Full, false));
        assert!(owners.claim(1));
        assert!(owners.publish(1));
        assert!(finish(&mut owners, 1, Retirement::Aborted, false));
        // The abort does not displace owner 0; only a later full retirement does.
        for _ in 0..10_000 {
            assert!(!owners.claim(0));
            assert!(!owners.exhausted());
        }
        assert_eq!(owners.counts(), Counts {
            fresh: 1, retired: 1, reusable: 1, abandoned: 1, ..Counts::default()
        });
        assert!(owners.claim(1));
        assert!(owners.publish(1));
        assert!(!finish(&mut owners, 1, Retirement::Full, false));
        assert!(owners.claim(0));
    }

    #[test]
    fn an_aborted_compute_owner_exhausts_neither_ledger() {
        let mut compute = Lifetime::<2>::new(false);
        let mut render = Lifetime::<2>::new(false);
        assert!(compute.claim(0));
        assert!(compute.publish(0));
        finish(&mut compute, 0, Retirement::Aborted, true);
        for number in 0..10_000 {
            let unit = number % 2;
            assert!(compute.claim(unit));
            assert!(compute.publish(unit));
            assert!(!finish(&mut compute, unit, Retirement::Full, false));
            assert!(!compute.exhausted());
            assert!(render.claim(unit));
            assert!(render.publish(unit));
            assert!(!finish(&mut render, unit, Retirement::Full, false));
            assert!(!render.exhausted());
        }
        assert_eq!(compute.counts().quarantined, 0);
        assert_eq!(compute.counts().abandoned, 1);
        assert_eq!(render.counts().abandoned, 0);
    }

    #[test]
    fn aborts_cannot_be_reset_by_preparation_cancel_and_leave_both_units_in_service() {
        let mut owners = Lifetime::<2>::new(false);
        for unit in 0..2 {
            assert!(owners.claim(unit));
            assert!(owners.publish(unit));
            finish(&mut owners, unit, Retirement::Aborted, true);
            owners.cancel(unit);
        }
        assert!(!owners.exhausted());
        assert_eq!(owners.counts().abandoned, 2);
        assert_eq!(owners.counts().reusable, 2);
        assert!(owners.claim(0));
        owners.cancel(0);
        assert!(owners.claim(0));
    }

    #[test]
    fn a_recovery_aborting_every_pipelined_unit_leaves_compute_in_service() {
        // Six units in flight together (the compute queue). Every third round a recovery
        // aborts every flight; compute stays available, with as many units as before.
        let mut owners = Lifetime::<6>::new(true);
        let mut aborted = 0;
        for round in 0..1_000 {
            let mut published = Vec::new();
            for unit in 0..6 {
                if owners.claim(unit) {
                    assert!(owners.publish(unit));
                    published.push(unit);
                }
            }
            assert!(published.len() >= 5, "round {round}: {published:?}");
            for &unit in &published {
                let receipt = if round % 3 == 0 {
                    aborted += 1;
                    Retirement::Aborted
                } else {
                    Retirement::Full
                };
                finish(&mut owners, unit, receipt, false);
            }
            assert!(!owners.exhausted());
        }
        assert_eq!(owners.counts().quarantined, 0);
        assert_eq!(owners.counts().abandoned, aborted);
    }

    #[test]
    fn repeated_handshakes_and_storm_limit_preserve_owner_accounting() {
        let mut owners = Lifetime::<2>::new(false);
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
    'known_limit': 'Only an owner whose storage cannot be trusted (corrupt identity, failed retirement) remains quarantined until reboot; an aborted owner is abandoned and its unit serves other storage.',
    'source': {
        name: hashlib.sha256((production / name).read_bytes()).hexdigest()
        for name in ['recovery/logic.rs', 'pools/lifetime.rs', 'poll.rs', 'recovery.rs']
    },
}, indent=2) + '\n')
