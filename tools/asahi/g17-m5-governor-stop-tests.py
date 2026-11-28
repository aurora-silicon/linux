#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: the M5 thermal governor hands everything back once it may not govern.

Compiles production `Governor` and `CpuAdvice` from
drivers/gpu/drm/asahi/g17/thermal.rs, and `Firmware::governor_tick()`, its
budget write and the governor work item (`WorkItem<GOVERNOR_WORK>::run`) from
drivers/gpu/drm/asahi/g17/mod.rs, against a mock firmware, budget word and
actuators. Verifies:
1. A budget the controller lowered goes back to the calibrated maximum when
   the controller loses its target (the SoC die zone or its passive trip gone).
2. A failed write of that maximum is retried until it lands, also with the
   GPU idle and the fans already back.
3. A failed budget write while governing (here the write back to the maximum
   once the GPU went idle) is retried on the next step.
4. A stopped firmware (as left by a stop that keeps it for outstanding
   preparations) or a crashed one hands every actuator back at once, though
   its latched effort still reads as GPU activity, and the work then stops.

--mutation removes one of the fixes. --old-ref REV reads the sources from a
git revision instead of the tree.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THERMAL = 'drivers/gpu/drm/asahi/g17/thermal.rs'
MOD = 'drivers/gpu/drm/asahi/g17/mod.rs'

MUTATIONS = {
    'drop-stop-gate': (
        MOD,
        '        if !self.primary.running || power::Control::crashed(self) {\n'
        '            return None;\n'
        '        }\n',
        ''),
    'drop-ineligible-restore': (
        MOD,
        '            if let Some(budget) = self.governor.restore() {\n'
        '                self.publish_budget(budget);\n'
        '            }\n',
        ''),
    'drop-publish-retry': (
        MOD,
        '            self.governor.publish_failed();\n',
        ''),
}


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def read(path, old_ref):
    if old_ref:
        return subprocess.check_output(['git', 'show', f'{old_ref}:{path}'], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


HARNESS = r'''
#![allow(dead_code, unused, private_interfaces)]
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;

pub struct Mutex<T>(std::sync::Mutex<T>);
impl<T> Mutex<T> {
    pub fn new(value: T) -> Self { Self(std::sync::Mutex::new(value)) }
    pub fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() }
}

static NOW: AtomicU64 = AtomicU64::new(0);
fn now_ns() -> u64 { NOW.load(Ordering::Relaxed) }

pub mod thermal {
    use super::*;

    @THERMAL_CONSTS@

    /// The SoC die zone's passive trip (0: the zone or its trip is gone).
    pub static TARGET: AtomicI32 = AtomicI32::new(0);
    pub static HOTSPOT: AtomicI32 = AtomicI32::new(0);
    fn target_mc() -> Option<i32> {
        let target = TARGET.load(Ordering::Relaxed);
        (target > 0).then_some(target)
    }
    // m5_thermal_governor bit 0, on by default.
    pub(super) fn enabled() -> bool { target_mc().is_some() }
    pub(super) fn requested() -> bool { true }
    pub(crate) fn hotspot_temperature_mc() -> Option<i32> { Some(HOTSPOT.load(Ordering::Relaxed)) }

    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    @CPU_ADVICE@

    @GOVERNOR@

    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    pub enum Call { Apply(CpuAdvice, bool), Stop }
    pub struct Fans { pub(crate) boosted: bool }
    impl Fans { pub(crate) fn boosted(&self) -> bool { self.boosted } }
    pub struct Actuators { pub(crate) fans: Fans, pub(crate) calls: Vec<Call> }
    impl Actuators {
        pub(crate) fn new() -> Self { Self { fans: Fans { boosted: false }, calls: Vec::new() } }
        pub(crate) fn apply(&mut self, advice: CpuAdvice, fans: bool, _temp_mc: Option<i32>, _now_ns: u64) {
            self.calls.push(Call::Apply(advice, fans));
            self.fans.boosted = fans;
        }
        /// Everything back at once; the SMC takes the fans: nothing left pending.
        pub(crate) fn stop(&mut self) -> bool {
            self.calls.push(Call::Stop);
            self.fans.boosted = false;
            false
        }
    }
}

pub mod power {
    pub(crate) trait Control { fn crashed(&self) -> bool; }
}

pub struct DeviceShared { pub governor_activity_ns: AtomicU64, pub crashed: AtomicBool }
pub struct RoleState { pub shared: Arc<DeviceShared> }
pub struct Coprocessor { pub running: bool, pub state: Arc<RoleState> }
pub struct Effort { pub applied: u32 }
impl Effort { fn applied(&self) -> u32 { self.applied } }

/// Region C's budget word and the power-policy block.
pub struct Init { pub word: AtomicU32, pub fail: AtomicBool, pub power: AtomicU32 }
impl Init {
    fn m5_power_budget(&self) -> Result<u32, i32> { Ok(self.word.load(Ordering::Relaxed)) }
    fn m5_gpu_power_mw(&self) -> Result<u32, i32> { Ok(self.power.load(Ordering::Relaxed)) }
    fn m5_firmware_clamped(&self) -> Result<bool, i32> { Ok(false) }
    fn publish_m5_power_budget(&self, budget_mw: u32) -> Result<(), i32> {
        if self.fail.load(Ordering::Relaxed) {
            return Err(-5);
        }
        self.word.store(budget_mw, Ordering::Relaxed);
        Ok(())
    }
}

pub struct Firmware {
    pub primary: Coprocessor,
    pub effort: Effort,
    pub governor: thermal::Governor,
    pub init: Init,
}
impl Firmware {
    @GOVERNOR_TICK@
    @PUBLISH_BUDGET@
}
impl power::Control for Firmware {
    fn crashed(&self) -> bool { self.primary.state.shared.crashed.load(Ordering::Acquire) }
}

pub struct Shared {
    pub governor_armed: AtomicBool,
    pub state: Mutex<Option<Box<Firmware>>>,
    pub actuators: Mutex<thermal::Actuators>,
    pub requeued: AtomicU32,
}
impl Shared {
    fn queue_governor(self: &Arc<Self>) {
        self.governor_armed.store(true, Ordering::Release);
        self.requeued.fetch_add(1, Ordering::Relaxed);
    }
}

pub trait WorkItem<const ID: u64> {
    type Pointer;
    fn run(this: Self::Pointer);
}
const GOVERNOR_WORK: u64 = 6;

@WORK_ITEM@

#[cfg(test)]
mod tests {
    use super::*;
    use thermal::{Call, CpuAdvice, HOTSPOT, TARGET};

    const S: u64 = 1_000_000_000;
    const MAXIMUM: u32 = 60_000;

    fn device() -> Arc<Shared> {
        TARGET.store(100_000, Ordering::Relaxed);
        HOTSPOT.store(101_000, Ordering::Relaxed);
        let shared = Arc::new(DeviceShared {
            governor_activity_ns: AtomicU64::new(0), crashed: AtomicBool::new(false),
        });
        let firmware = Firmware {
            primary: Coprocessor { running: true, state: Arc::new(RoleState { shared }) },
            effort: Effort { applied: 0 },
            governor: thermal::Governor::new(),
            init: Init { word: AtomicU32::new(MAXIMUM), fail: AtomicBool::new(false), power: AtomicU32::new(45_000) },
        };
        Arc::new(Shared {
            governor_armed: AtomicBool::new(true),
            state: Mutex::new(Some(Box::new(firmware))),
            actuators: Mutex::new(thermal::Actuators::new()),
            requeued: AtomicU32::new(0),
        })
    }

    fn firmware<R>(device: &Arc<Shared>, f: impl FnOnce(&mut Firmware) -> R) -> R {
        f(device.state.lock().as_deref_mut().unwrap())
    }

    fn word(device: &Arc<Shared>) -> u32 {
        firmware(device, |fw| fw.init.word.load(Ordering::Relaxed))
    }

    /// One run of the governor work at `t` seconds; returns whether it queued itself again.
    fn step(device: &Arc<Shared>, t: u64) -> bool {
        NOW.store(t * S, Ordering::Relaxed);
        device.requeued.store(0, Ordering::Relaxed);
        <Shared as WorkItem<GOVERNOR_WORK>>::run(device.clone());
        device.requeued.load(Ordering::Relaxed) != 0
    }

    /// A step after the GPU accepted work at `t` seconds.
    fn busy(device: &Arc<Shared>, t: u64) -> bool {
        firmware(device, |fw| fw.primary.state.shared.governor_activity_ns.store(t * S, Ordering::Release));
        step(device, t)
    }

    /// A busy GPU above the target: the controller lowers the budget and boosts the fans.
    fn engage(device: &Arc<Shared>) -> u32 {
        for t in 1..=3 {
            assert!(busy(device, t));
        }
        let lowered = word(device);
        assert!(lowered < MAXIMUM, "the controller lowers the budget above the target ({lowered} mW)");
        assert!(device.actuators.lock().fans.boosted(), "the controller boosts the fans");
        lowered
    }

    #[test]
    fn a_lowered_budget_goes_back_when_the_target_goes() {
        let device = device();
        engage(&device);
        // The SoC die zone or its passive trip went away (the SMC hwmon cell unbound).
        TARGET.store(0, Ordering::Relaxed);
        assert!(busy(&device, 4));
        assert_eq!(word(&device), MAXIMUM, "the calibrated maximum goes back");
        // And stays: later steps leave the word alone.
        assert!(busy(&device, 5));
        assert_eq!(word(&device), MAXIMUM);
    }

    #[test]
    fn a_failed_write_of_the_maximum_is_retried_until_it_lands() {
        let device = device();
        let lowered = engage(&device);
        TARGET.store(0, Ordering::Relaxed);
        firmware(&device, |fw| fw.init.fail.store(true, Ordering::Relaxed));
        // The GPU went idle: only the owed write keeps the work running once the fans are back.
        assert!(step(&device, 10));
        assert!(step(&device, 11), "a failed write keeps the work running");
        assert!(!device.actuators.lock().fans.boosted());
        assert_eq!(word(&device), lowered);
        firmware(&device, |fw| fw.init.fail.store(false, Ordering::Relaxed));
        let again = step(&device, 12);
        assert_eq!(word(&device), MAXIMUM, "the retried write lands");
        assert!(!again, "the work stops once the write landed");
    }

    #[test]
    fn a_failed_write_while_governing_is_retried() {
        let device = device();
        let lowered = engage(&device);
        // The GPU went idle: the governor hands the maximum back, and that write fails.
        firmware(&device, |fw| fw.init.fail.store(true, Ordering::Relaxed));
        assert!(step(&device, 10), "a failed write keeps the work running");
        assert_eq!(word(&device), lowered);
        firmware(&device, |fw| fw.init.fail.store(false, Ordering::Relaxed));
        step(&device, 11);
        assert_eq!(word(&device), MAXIMUM, "the next step writes it again");
        assert!(!step(&device, 12), "the work stops once the write landed");
    }

    fn stop_hands_everything_back(stop: impl FnOnce(&mut Firmware)) {
        let device = device();
        // Submitted work holds the effort: the governor reads it as GPU activity.
        firmware(&device, |fw| fw.effort.applied = 0x10000);
        engage(&device);
        firmware(&device, stop);
        device.actuators.lock().calls.clear();
        assert!(!step(&device, 4), "the work stops once every actuator is back");
        assert_eq!(device.actuators.lock().calls, vec![Call::Stop]);
        assert!(!device.actuators.lock().fans.boosted());
        // A later run (an earlier queueing) changes nothing.
        assert!(!step(&device, 5));
        assert_eq!(device.actuators.lock().calls, vec![Call::Stop, Call::Stop]);
    }

    #[test]
    fn a_stopped_firmware_hands_every_actuator_back() {
        // stop_runtime() stopped the coprocessor but kept the firmware: outstanding
        // preparations did not drain.
        stop_hands_everything_back(|fw| fw.primary.running = false);
    }

    #[test]
    fn a_crashed_firmware_hands_every_actuator_back() {
        stop_hands_everything_back(|fw| fw.primary.state.shared.crashed.store(true, Ordering::Release));
    }
}
'''


def build(mutation, old_ref):
    thermal = read(THERMAL, old_ref)
    mod = read(MOD, old_ref)
    if mutation:
        path, old, new = MUTATIONS[mutation]
        assert path == MOD
        assert mod.count(old) == 1, f'mutation {mutation} does not apply'
        mod = mod.replace(old, new)
    consts = re.findall(r'^(?:pub\([a-z]+\) )?const [A-Z0-9_]+: (?:u32|u64|i32|i64|usize) = [^;]+;$',
                        thermal, re.M)
    publish = item(mod, '    fn publish_budget(&mut self') if 'fn publish_budget(&mut self' in mod else ''
    return (HARNESS
            .replace('@THERMAL_CONSTS@', '\n    '.join(consts))
            .replace('@CPU_ADVICE@', item(thermal, 'pub(crate) enum CpuAdvice {'))
            .replace('@GOVERNOR@', item(thermal, 'pub(crate) struct Governor {') + '\n'
                     + item(thermal, '\nimpl Governor {'))
            .replace('@PUBLISH_BUDGET@', publish)
            .replace('@GOVERNOR_TICK@', item(mod, '    fn governor_tick(&mut self'))
            .replace('@WORK_ITEM@', item(mod, 'impl WorkItem<GOVERNOR_WORK> for Shared {')))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    ap.add_argument('--old-ref', help='read the sources from this git revision')
    ap.add_argument('--out', type=Path, help='build directory (default: a temporary one)')
    args = ap.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix='g17-governor-stop-'))
    out.mkdir(parents=True, exist_ok=True)
    src, binary = out / 'governor_stop.rs', out / 'governor_stop'
    src.write_text(build(args.mutation, args.old_ref))
    rustc = os.environ.get('RUSTC', 'rustc')
    subprocess.run([rustc, '--edition=2021', '--test', str(src), '-o', str(binary)], check=True)
    res = subprocess.run([str(binary), '--test-threads=1'], capture_output=True, text=True)
    print(res.stdout, end='')
    print(res.stderr, end='', file=sys.stderr)
    return res.returncode


if __name__ == '__main__':
    sys.exit(main())
