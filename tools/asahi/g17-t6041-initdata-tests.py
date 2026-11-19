#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Build T6041 initialization data and compare it with captures when given.

The 32-core 25G83 configuration is also built from the operating points the loader publishes
from the J616C's ADT, and must give the contents that unit ran with."""

import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

DRIVER = Path(__file__).resolve().parents[2] / "drivers/gpu/drm/asahi"


def item(source, start):
    """Extract an item and its attributes."""
    at = source.index(start)
    line = source.rfind("\n", 0, at) + 1
    while True:
        previous = source.rfind("\n", 0, line - 1) + 1
        if line == 0 or not source[previous:line].lstrip().startswith("#["):
            break
        line = previous
    depth = 0
    end = at
    while True:
        c = source[end]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0 and c == "}" and not re.match(
                r"(pub\(crate\) )?(const|static) (?!fn )", source[at:]
            ):
                return source[line : end + 1]
        elif c == ";" and depth == 0:
            return source[line : end + 1]
        end += 1


HEADER = r"""
#![allow(dead_code, unused_imports, unused_variables, clippy::all)]
use core::mem::size_of;
#[derive(Debug)]
pub struct Error(i32);
pub const EINVAL: Error = Error(-22);
pub const EOVERFLOW: Error = Error(-75);
impl From<core::num::TryFromIntError> for Error {
    fn from(_: core::num::TryFromIntError) -> Self { EINVAL }
}
pub type Result<T = ()> = core::result::Result<T, Error>;
pub unsafe trait Zeroable {}
macro_rules! static_assert { ($e:expr $(,)?) => { const _: () = assert!($e); }; }
pub fn clear<T: Zeroable>(object: &mut T) {
    unsafe { core::ptr::write_bytes(object as *mut T, 0, 1) }
}
const PAGE_SIZE: usize = 0x4000;
"""

INITDATA_ITEMS = [
    "pub(crate) const REGISTER_MAP_SLOTS",
    "pub(crate) struct RegisterWindow",
    "const RESERVED_SLOT_UNK_20",
    "pub(crate) struct RegisterMapEntry",
    "unsafe impl Zeroable for RegisterMapEntry",
    "impl RegisterMapEntry",
    "pub(crate) const PERF_STATES",
    "const PERF_LANES",
    "pub(crate) const OPP_CELLS",
    "pub(crate) struct Opp ",
    "pub(crate) struct PerfConfig",
    "pub(crate) struct PerfGroup",
    "unsafe impl Zeroable for PerfGroup",
    "struct PerfState",
    "impl PerfGroup",
    "fn perf_states",
    "pub(crate) struct CompletionRingDescriptor",
    "unsafe impl Zeroable for CompletionRingDescriptor",
    "const COMPLETION_RING_DESCRIPTORS",
    "pub(crate) const COMPLETION_RING_INDEX",
    "const COMPLETION_RING_ENTRIES",
    "const COMPLETION_RING_ENTRY_SIZE",
    "pub(crate) struct CompletionRing ",
    "pub(crate) struct HwDataArgs",
    "type Words",
    "fn fill_words",
    "pub(crate) const CONTEXT_SWITCH_TIMER_MASK",
    "pub(crate) const CONTEXT_SWITCH_TIMER_MULTIPLIER",
    "pub(crate) struct RegionCConfig",
    "pub(crate) struct RegionC ",
    "unsafe impl Zeroable for RegionC ",
    "impl RegionC ",
]

SOC_ITEMS = [
    "pub(crate) const CHIP_ID",
    "pub(crate) const REGISTER_WINDOWS",
    "pub(crate) const PERF",
    "pub(crate) const OPPS",
    "const REGION_C",
]

T6041_ITEMS = SOC_ITEMS + [
    "pub(crate) const REGISTER_WINDOWS_25G83",
    "const RESERVED_SLOTS",
    "const MAX_POWER_MW",
    "const CHIP: ",
    "const CHIP_25G83: ",
    "const HW_DATA_WORDS: ",
    "const HW_DATA_WORDS_25G83: ",
    "const POWER_CONFIG_WORDS",
]

J616C_ITEMS = [
    "const REGISTER_WINDOWS: [",
    "const PERF: PerfConfig",
    "const MAX_POWER_MW",
    "const REGION_C",
    "const AON_TABLE",
    "const CHIP: ",
    "const POWER_CONFIG_WORDS",
]

T6041_OPP_HZ = [0, 338, 618, 796, 924, 952, 1056, 1062, 1182, 1182, 1312, 1242, 1380, 1326,
                1470, 1578]
T6041_OPP_MV = [
    [125, 600, 665, 705, 725, 770, 770, 810, 810, 860, 860, 900, 900, 940, 940, 1020],
    [125, 600, 665, 705, 725, 770, 770, 810, 810, 860, 860, 895, 895, 925, 925, 1020],
    [125, 600, 660, 700, 715, 770, 770, 805, 805, 860, 860, 900, 900, 940, 940, 1020],
    [125, 600, 660, 705, 730, 780, 780, 810, 810, 860, 860, 900, 900, 940, 940, 1020],
]
T6041_AUX = [(24, 585000), (338, 585000), (618, 635000), (796, 685000), (992, 720000),
             (1188, 770000), (1380, 855000), (1470, 885000)]

# The 32-core J616C's ADT /arm-io/sgx: perf-states (one table per cluster) and cs-perf-states
# (afr-perf-states is the same), as the loader publishes them in the device tree.
J616C_OPP_HZ = [0, 338, 618, 796, 924, 952, 1056, 1062, 1182, 1182, 1312, 1242, 1380, 1326,
                1470, 1578]
J616C_OPP_MV = [
    [125, 600, 650, 695, 720, 770, 770, 815, 815, 870, 870, 910, 910, 950, 950, 1030],
    [125, 595, 645, 690, 720, 760, 760, 805, 805, 850, 850, 890, 890, 930, 930, 1025],
    [125, 615, 660, 705, 725, 775, 775, 805, 805, 855, 855, 895, 895, 935, 935, 1030],
    [125, 600, 660, 700, 715, 770, 770, 805, 805, 860, 860, 900, 900, 940, 940, 1020],
]
J616C_AUX = [(24, 585000), (338, 585000), (618, 630000), (796, 680000), (992, 720000),
             (1188, 770000), (1380, 865000), (1470, 900000)]


def opps(name, hz, mv):
    rows = []
    for index, freq in enumerate(hz):
        cells = [m[index] * 1000 for m in mv] + [0] * (16 - len(mv))
        rows.append(
            "Opp { freq_hz: %d, volt_uv: [%s], cells: %d }"
            % (freq * 1_000_000, ", ".join(map(str, cells)), len(mv))
        )
    return "const %s: [Opp; %d] = [%s];\n" % (name, len(rows), ",\n".join(rows))


def aux(name, rows):
    body = ",\n".join(
        "Opp { freq_hz: %d, volt_uv: [%d, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0], cells: 1 }"
        % (mhz * 1_000_000, uv)
        for mhz, uv in rows
    )
    return "const %s: [Opp; %d] = [%s];\n" % (name, len(rows), body)


MAIN = r"""
fn build(
    chip_id: u32,
    windows: &[RegisterWindow],
    reserved: &[usize],
    perf: &PerfConfig,
    opps: &[Opp],
    aux: &[Opp],
    chip: &v25g83::Chip,
    region_c: &RegionCConfig,
    cores: u32,
) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    let args = HwDataArgs {
        chip_id,
        windows,
        reserved_slots: reserved,
        opps,
        perf,
        completion_rings: [
            CompletionRing { low_va: 0x70_03f0_0000, high_va: 0xffff_fc20_0000_0000 },
            CompletionRing { low_va: 0x70_03f2_8000, high_va: 0xffff_fc20_0002_8000 },
        ],
        qos_va: 0xffff_fc20_0004_0000,
    };
    let ext = v25g83::HwDataArgs25G83 { active_cores: cores, cs_opps: aux, afr_opps: aux, chip: Some(chip) };
    let mut hw: Box<v25g83::HwData25G83> = unsafe { Box::new(core::mem::zeroed()) };
    hw.init(&args, &ext).unwrap();
    let mut power: Box<v25g83::PowerConfig25G83> = unsafe { Box::new(core::mem::zeroed()) };
    power.init(0xffff_fc20_000d_3440, 0, 0, chip.max_power_mw, &chip.leakage);
    power.apply_chip(chip).unwrap();
    let mut region: Box<RegionC> = unsafe { Box::new(core::mem::zeroed()) };
    region.init(region_c);
    region.max_power_mw = region_c.max_power_mw;
    let hw_bytes = unsafe {
        core::slice::from_raw_parts((&*hw as *const v25g83::HwData25G83).cast::<u8>(),
                                    size_of::<v25g83::HwData25G83>())
    }.to_vec();
    let region_bytes = unsafe {
        core::slice::from_raw_parts((&*region as *const RegionC).cast::<u8>(), size_of::<RegionC>())
    }.to_vec();
    (hw_bytes, power.0.to_vec(), region_bytes)
}

fn main() {
    let out = std::env::args().nth(1).unwrap();
    let (hw, power, region) = build(t6041::CHIP_ID, &t6041::REGISTER_WINDOWS,
        &t6041::RESERVED_SLOTS, &t6041::PERF, &T6041_OPPS, &T6041_AUX, &t6041::CHIP,
        &t6041::REGION_C, 40);
    println!("T6041 initialization objects built");
    std::fs::write(format!("{}/t6041-hwdata.bin", out), &hw).unwrap();
    std::fs::write(format!("{}/t6041-power-config.bin", out), &power).unwrap();
    std::fs::write(format!("{}/t6041-region-c.bin", out), &region).unwrap();
    // 25G83: the same configuration with its own register map, and 0 at +0x2610.
    let (hw_g83, power_g83, region_g83) = build(t6041::CHIP_ID, &t6041::REGISTER_WINDOWS_25G83,
        &[], &t6041::PERF, &T6041_OPPS, &T6041_AUX, &t6041::CHIP_25G83, &t6041::REGION_C, 40);
    println!("T6041 25G83 initialization objects built");
    assert_eq!(power_g83, power, "the register map changes only the hardware data");
    assert_eq!(region_g83, region, "the register map changes only the hardware data");
    let map = 0x640..0x640 + REGISTER_MAP_SLOTS * core::mem::size_of::<RegisterMapEntry>();
    let outside = |a: &[u8], b: &[u8]| a.iter().zip(b).enumerate()
        .filter(|(i, (x, y))| x != y && !map.contains(i)).map(|(i, _)| i).collect::<Vec<_>>();
    assert_eq!(outside(&hw_g83, &hw), [0x2610],
               "the 25G83 hardware data differs only in its register map and +0x2610");
    assert_eq!((hw[0x2610], hw_g83[0x2610]), (1, 0), "+0x2610: 1 on 25F84, 0 on 25G83");
    std::fs::write(format!("{}/t6041-25g83-hwdata.bin", out), &hw_g83).unwrap();
    // The 32-core J616C on 25G83, from its operating points. What it ran with since October
    // 2026, hashed without the runtime words (FNV-1a 64), from a dump of 25 Oct 2026.
    let (hw32, power32, region32) = build(t6041::CHIP_ID, &t6041_25g83::REGISTER_WINDOWS, &[],
        &t6041_25g83::PERF, &J616C_OPPS, &J616C_AUX, &t6041_25g83::CHIP,
        &t6041_25g83::REGION_C, 32);
    println!("T6041 25G83 32-core initialization objects built");
    let fnv = |bytes: &[u8], skip: &[(usize, usize)]| {
        bytes.iter().enumerate().filter(|(i, _)| !skip.iter().any(|&(at, n)| (at..at + n).contains(i)))
            .fold(0xcbf2_9ce4_8422_2325u64, |h, (_, &b)| (h ^ u64::from(b)).wrapping_mul(0x100_0000_01b3))
    };
    assert_eq!(fnv(&hw32, &[(0x2620, 0x80), (0x26b8, 8)]), 0xd781_973f_9983_1e35,
               "the J616C hardware data");
    assert_eq!(fnv(&power32, &[(0x3b50, 8), (0x6318, 8), (0x63d4, 4)]), 0xc36d_7dba_a597_aac4,
               "the J616C power configuration");
    assert_eq!(fnv(&region32, &[]), 0x91a4_c670_17e3_0a24, "the J616C region C");
    let word = |at: usize| u32::from_le_bytes(power32[at..at + 4].try_into().unwrap());
    assert_eq!(word(0x9ec), (-1000.0 / t6041_25g83::MAX_POWER_MW as f32).to_bits());
    println!("T6041 25G83 32-core objects match the J616C's");
    std::fs::write(format!("{}/t6041-25g83-32-hwdata.bin", out), &hw32).unwrap();
    std::fs::write(format!("{}/t6041-25g83-32-power-config.bin", out), &power32).unwrap();
}
"""


def program():
    initdata = (DRIVER / "g17/fw/initdata.rs").read_text()
    v25g83 = (DRIVER / "g17/fw/initdata/v25g83.rs").read_text()
    t6041 = (DRIVER / "hw/t6041.rs").read_text()
    v25g83 = re.sub(r"use super::\{[^}]*\};", "use super::*;", v25g83, count=1)
    v25g83 = v25g83.replace("use kernel::prelude::*;", "")
    float_source = (DRIVER / "float.rs").read_text().split("impl<'a> TryFrom<of::Property")[0]
    float_source = re.sub(r"use kernel::\{.*?\};", "use super::*;", float_source, flags=re.S)
    code = [HEADER, "mod float {\n" + float_source + "\n}\n"]
    code += [item(initdata, name) for name in INITDATA_ITEMS]
    common = (DRIVER / "hw/t604x.rs").read_text().replace("use crate::g17::fw::initdata::RegisterWindow;", "use super::RegisterWindow;")
    code.append("mod t604x {\n%s\n}\n" % common)
    code.append("pub mod v25g83 {\n%s\n}\n" % v25g83)
    j616c = (DRIVER / "hw/t6041_25g83.rs").read_text()
    for name, source, items in (("t6041", t6041, T6041_ITEMS), ("t6041_25g83", j616c, J616C_ITEMS)):
        body = "\n".join(item(source, i) for i in items)
        body = re.sub(r"(?m)^const ", "pub(crate) const ", body)
        code.append("pub mod %s {\nuse super::*;\nuse super::t604x::{instances, window};\n%s\n}\n" % (name, body))
    code.append(opps("T6041_OPPS", T6041_OPP_HZ, T6041_OPP_MV))
    code.append(aux("T6041_AUX", T6041_AUX))
    code.append(opps("J616C_OPPS", J616C_OPP_HZ, J616C_OPP_MV))
    code.append(aux("J616C_AUX", J616C_AUX))
    code.append(MAIN)
    return "\n".join(code)


def words(data):
    return struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])


def volatile(first, second):
    """Words that differ between two captures of the same object taken ~60 s apart: the
    firmware rewrites them while it runs."""
    a, b = Path(first).read_bytes(), Path(second).read_bytes()
    return [(4 * i, 4 * i + 3) for i, (x, y) in enumerate(zip(words(a), words(b))) if x != y]


def in_ranges(offset, ranges):
    return any(lo <= offset + 3 and offset <= hi for lo, hi in ranges)


def compress(diffs):
    """(offset, value) pairs to (offset, value, repeat) runs."""
    runs = []
    for offset, value in diffs:
        if runs and runs[-1][1] == value and runs[-1][0] + 4 * runs[-1][2] == offset:
            runs[-1][2] += 1
        else:
            runs.append([offset, value, 1])
    return runs


HW_RUNTIME = [(0xe88, 0xe8f), (0x2620, 0x269f), (0x26b8, 0x26bf)]
POWER_RUNTIME = [(0x3b50, 0x3b57), (0x6318, 0x631f), (0x63d4, 0x63d7)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mc", help="native configuration capture")
    parser.add_argument("--ha", help="native hardware-data capture")
    parser.add_argument("--ha-repeat", help="repeated hardware-data capture")
    parser.add_argument("--out")
    parser.add_argument("--overlay", action="store_true")
    args = parser.parse_args()
    captures = (args.mc, args.ha, args.ha_repeat)
    if any(captures) and not all(captures):
        parser.error("--mc, --ha and --ha-repeat go together")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "check.rs").write_text(program())
        subprocess.run(
            ["rustc", "--edition=2021", "-O", "-o", str(tmp / "check"), str(tmp / "check.rs")],
            check=True,
        )
        out = Path(args.out) if args.out else tmp
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(tmp / "check"), str(out)], check=True)
        hw = (out / "t6041-hwdata.bin").read_bytes()
        power = (out / "t6041-power-config.bin").read_bytes()

    if not args.mc:
        return
    mc = Path(args.mc).read_bytes()[:0x2718]
    ha = Path(args.ha).read_bytes()
    churn = volatile(args.ha, args.ha_repeat)
    failures = 0
    report = {}
    for name, ours, theirs, runtime, rewritten in (
        ("hwdata", hw, mc, HW_RUNTIME, []),
        ("power-config", power, ha, POWER_RUNTIME, churn),
    ):
        length = min(len(ours), len(theirs))
        ours_w, theirs_w = words(ours[:length]), words(theirs[:length])
        diffs, skipped = [], []
        for index, (a, b) in enumerate(zip(ours_w, theirs_w)):
            if a == b:
                continue
            offset = 4 * index
            if in_ranges(offset, runtime):
                skipped.append((offset, a, b, "host pointer/random"))
            else:
                diffs.append((offset, a, b))
        tail = [i for i, w in enumerate(words(theirs[length:])) if w]
        report[name] = (diffs, skipped, length, len(ours), len(theirs), tail)
        if args.overlay:
            print("// %s" % name)
            for offset, value, repeat in compress([(o, b) for o, a, b in diffs]):
                print("    (0x%x, 0x%x, %d)," % (offset, value, repeat))
            continue
        print("%s: compared 0x%x bytes (ours 0x%x, macOS 0x%x): %d words differ, %d skipped"
              % (name, length, len(ours), len(theirs), len(diffs), len(skipped)))
        moving = [i for i in range(length // 4) if in_ranges(4 * i, rewritten)]
        if moving:
            print("  %d of the compared words change while the firmware runs (macOS's first "
                  "capture is used)" % len(moving))
        for offset, a, b in diffs:
            print("  DIFF +0x%04x ours %#010x macOS %#010x" % (offset, a, b))
        for offset, a, b, why in skipped:
            print("  skip +0x%04x ours %#010x macOS %#010x (%s)" % (offset, a, b, why))
        if tail:
            print("  macOS has %d non-zero words past our object (firmware-rewritten: %s)"
                  % (len(tail), all(in_ranges(length + 4 * i, rewritten) for i in tail)))
        failures += len(diffs)
    if not args.overlay:
        print("RESULT: %s" % ("PASS" if failures == 0 else "%d differences" % failures))
        sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
