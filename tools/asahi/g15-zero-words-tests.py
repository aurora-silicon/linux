#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Test the actual J516C record builder's narrow zero-word exemption.

Uses synthetic public data and temporary host copies of production Rust modules.
No GPU, firmware file, private fixture, kernel build or hardware access. The
golden T6030 inputs below are from SiliconForest 9f09c85f8d3b, except the full
hw.rs file refreshed at 709f0f08316a for the reviewed shared PB allocator changes.
Its T6030 configuration module is independently pinned to the original bytes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


G15 = "drivers/gpu/drm/asahi/g15"
GOLDEN_FILES = {
    "board/data.rs": "7c5ff1077ca3296cbfc9e7144294162b6dea78a3774c38eeed4b0f59a14d7a5c",
    "board/template.rs": "286872f5474a67534f214e83e659253299f85143f3dc59035fa80894022e2549",
    "hw.rs": "e4619590e670ed88d2a9bb76b03853081c615d3d0ad1c6a68321ce8e615ef8a0",
    "power/idle.rs": "4928ecf7699b3ca85ff5143eb6e996efd40d47321e29a238a02c8873bc9f102c",
}
GOLDEN_FUNCTIONS = {
    ("hw.rs", "pub(crate) mod t6030 {"): "0079ae08fbe0e51cbf14e48711981a81048d1a1d3fc5495ab8d5c376283a760c",
    ("initdata.rs", "fn templates("): "c3115c9926d3d7d93317cb8510e93b04f30b0c256811d7137568e822dd93b8ee",
    ("initdata.rs", "pub(crate) fn prepare_board("): "c59f968f125397efc63f75a49d3a8543c15c1fd4d50362f15683563398e64bc6",
    ("board.rs", "pub(crate) fn from_adt("): "cecd0b28db5bd94d3b75c1bbb5f35204e8bf0699f74e5b3d08228c59025f6ac2",
}


def function(source, marker):
    start = source.index(marker)
    depth = 0
    for end in range(source.index("{", start), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError("unterminated function: " + marker)


CASES = r'''
#[cfg(test)]
mod zero_words_regressions {
    use super::*;

    // Frozen pre-5131caff FROM_FILE offsets from SiliconForest 9f09c85f8d3b.
    // This list is deliberately independent of both new lists.
    const OLD: [usize; 74] = [
        224,232,244,248,252,256,260,264,268,728,732,736,740,744,748,
        992,1000,1012,1016,1024,1036,1496,1500,1504,1508,1512,1516,
        4192,4196,4200,4204,4208,4212,4216,12504,12528,19492,20012,
        20016,23056,23060,27176,27180,45528,45532,45536,45540,45544,
        45548,45552,45556,45560,45564,45568,45572,45576,45580,45584,
        45588,45592,45596,45748,45836,45840,45844,45848,45900,45904,
        45908,45912,45932,45940,45948,45956,
    ];

    fn synthetic() -> (Vec<u8>, Vec<Opp>, [Opp; 1], [u32; PROPS.len()]) {
        let mut file = vec![0u8; SIZE];
        for (n, &at) in OLD.iter().enumerate() {
            put32(&mut file, at, 0x13570000 | (n as u32 + 1)).unwrap();
        }
        put32(&mut file, MAX_POWER, 54321).unwrap();
        for (n, key) in [b"RT0g", b"RT1g", b"RT2g", b"RT3g", b"RT0r", b"RT0f"].iter().enumerate() {
            put32(&mut file, SENSOR_ROWS + 12*n, u32::from_le_bytes(**key)).unwrap();
        }
        // Public, synthetic OPPs: state0 off; eight voltage groups, five paired.
        let mut main = vec![Opp {mhz:0, mv:[125;4], cells:4, mw:0}];
        for (mv, freqs) in [
            (600, &[300u32][..]), (650, &[500][..]), (700, &[700][..]),
            (750, &[800,900][..]), (800, &[950,1000][..]),
            (850, &[1050,1100][..]), (900, &[1150,1200][..]),
            (950, &[1250,1300][..]),
        ] {
            for &mhz in freqs {
                main.push(Opp {mhz, mv:[mv;4], cells:4, mw:mhz*40});
            }
        }
        let cs = [Opp {mhz:24, mv:[635,0,0,0], cells:1, mw:0}];
        let mut props = [1u32; PROPS.len()];
        props[PERIOD_MS] = 8;
        props[AVG_TC_MS] = 32;
        props[PPM_TC_MS] = 32;
        props[MIN_SRAM_UV] = 815000;
        props[CSAFR_MIN_SRAM_UV] = 815000;
        for at in [AVG_KI,AVG_KP,FD_KI,FD_KP,PERF_KI,PERF_KI2,PERF_KP,
                   PERF_KP2,PPM_KI,PPM_KP,PWR_KI,PWR_KP,SE_KI,SE_KI1,SE_KP,SE_KP1] {
            props[at] = 0x3f800000; // finite normal 1.0, not a calibration claim
        }
        (file, main, cs, props)
    }

    fn inputs<'a>(main: &'a [Opp], cs: &'a [Opp], props: [u32; PROPS.len()]) -> Inputs<'a> {
        Inputs {props, zones:&[], main, cs, afr:cs, slots:&[], empty_writable:&[],
            ttbat:0x100000, usc_base:0x1000000000, user_end:0x2000000000,
            timestamps:0xfffffc2ffc000000, max_power_mw:54321}
    }

    #[test]
    fn old74_is_exact_disjoint_partition() {
        assert_eq!(FROM_FILE.len(), 32);
        assert_eq!(UNNEEDED.len(), 42);
        let mut union = FROM_FILE.to_vec();
        union.extend_from_slice(&UNNEEDED);
        union.sort_unstable();
        assert!(union.windows(2).all(|w| w[0] < w[1]), "duplicate or overlapping offsets");
        assert!(union.iter().all(|&at| at % 4 == 0 && at + 4 <= SIZE));
        let mut old = OLD.to_vec(); old.sort_unstable();
        assert_eq!(union, old, "the exemption must not grow beyond old74");
    }

    #[test]
    fn actual_from_file_copies_only_retained32() {
        let (file, _, _, _) = synthetic();
        let mut out = vec![0u8; SIZE];
        from_file(&mut out, &file).unwrap();
        for at in (0..SIZE).step_by(4) {
            let expected = if FROM_FILE.contains(&at) {get32(&file, at).unwrap()} else {0};
            assert_eq!(get32(&out, at).unwrap(), expected, "copy offset +{at:#x}");
        }
    }

    #[test]
    fn actual_builder_leaves42_zero_and_preserves32() {
        let (file, main, cs, props) = synthetic();
        let mut out = vec![0u8; SIZE];
        from_file(&mut out, &file).unwrap();
        build(&mut out, &inputs(&main, &cs, props)).unwrap();
        for &at in UNNEEDED.iter() {
            assert_ne!(get32(&file, at).unwrap(), 0, "fixture must exercise a real difference");
            assert_eq!(get32(&out, at).unwrap(), 0, "built offset +{at:#x}");
        }
        for &at in FROM_FILE.iter() {
            assert_eq!(get32(&out, at).unwrap(), get32(&file, at).unwrap(), "retained +{at:#x}");
        }
        // Model a matching board file with nonzero legacy values only at the42.
        let mut matching = out.clone();
        for &at in UNNEEDED.iter() {
            put32(&mut matching, at, get32(&file, at).unwrap()).unwrap();
        }
        assert_eq!(cross_check(&out, &matching), None);
    }

    #[test]
    fn actual_cross_check_exempts_only_zero_at_exact42() {
        let mut built = vec![0u8; SIZE];
        let mut file = vec![0u8; SIZE];
        for at in (0..SIZE).step_by(4) {
            put32(&mut file, at, 0x12345678).unwrap();
            let expected = if UNNEEDED.contains(&at) {None} else {Some((at,0,0x12345678))};
            assert_eq!(cross_check(&built, &file), expected, "file mismatch +{at:#x}");
            // No exemption for a nonzero built mismatch, even at the42.
            put32(&mut built, at, 1).unwrap();
            assert_eq!(cross_check(&built, &file), Some((at,1,0x12345678)), "built mismatch +{at:#x}");
            put32(&mut built, at, 0).unwrap();
            put32(&mut file, at, 0).unwrap();
        }
    }
}
'''


MUTANTS = {
    "broad-cross-check-exemption": ("cross_check", "UNNEEDED.contains(&at)", "true"),
    "nonzero-built-exempted": ("cross_check", "b == 0 && UNNEEDED.contains(&at)", "UNNEEDED.contains(&at)"),
    "legacy-copy-reintroduced": ("from_file", "FROM_FILE.iter()", "FROM_FILE.iter().chain(UNNEEDED.iter())"),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--rustc", default=os.environ.get("HOSTRUSTC", "rustc"))
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    sources = {name: (root / G15 / "board" / name).read_bytes()
               for name in ("record.rs", "words.rs", "template.rs", "data.rs")}
    golden = {}
    for name, expected in GOLDEN_FILES.items():
        actual = sha((root / G15 / name).read_bytes())
        if actual != expected:
            raise SystemExit("T6030 golden file changed: " + name)
        golden[name] = actual
    for (name, marker), expected in GOLDEN_FUNCTIONS.items():
        actual = sha(function((root / G15 / name).read_text(), marker).encode())
        if actual != expected:
            raise SystemExit("T6030 golden path changed: " + marker)
        golden[name + ":" + marker] = actual

    if args.evidence:
        args.evidence.mkdir(parents=False, exist_ok=False)
    receipt = {"sources": {name: sha(data) for name, data in sources.items()},
               "t6030_golden_inputs": golden, "mutants": {},
               "harness_sha256": sha(Path(__file__).read_bytes()),
               "compiler": subprocess.check_output([args.rustc, "--version"], text=True).strip()}
    with tempfile.TemporaryDirectory(prefix="g15-zero-words-") as temp:
        temp = Path(temp)
        for name, data in sources.items():
            (temp / name).write_bytes(data)
        original = sources["record.rs"].decode()
        wrapper = "mod board {\n"
        for name in ("template", "data", "words", "record"):
            wrapper += f'#[path = "{temp / name}.rs"] pub(crate) mod {name};\n'
            if name == "template":
                wrapper += "pub(crate) use template::{Line, Template};\n"
        wrapper += "}\n"
        (temp / "host.rs").write_text(wrapper)

        def run(name, record):
            (temp / "record.rs").write_text(record + CASES)
            binary = temp / name
            command = [args.rustc, "--edition=2021", "--test", "-O", "-C", "overflow-checks=on",
                       str(temp / "host.rs"), "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            if args.evidence:
                (args.evidence / (name + "-compile.log")).write_text(compiled.stdout + compiled.stderr)
            if compiled.returncode:
                raise RuntimeError(name + " did not compile:\n" + compiled.stderr)
            tested = subprocess.run([str(binary)], capture_output=True, text=True)
            if args.evidence:
                (args.evidence / (name + "-test.log")).write_text(tested.stdout + tested.stderr)
            return tested

        actual = run("actual", original)
        if actual.returncode:
            raise SystemExit(actual.stdout + actual.stderr)
        receipt["test_summary"] = next(line for line in actual.stdout.splitlines()
                                       if line.startswith("test result:"))
        if args.mutations:
            for name, (func, before, after) in MUTANTS.items():
                scope = function(original, "pub(crate) fn " + func + "(")
                if scope.count(before) != 1:
                    raise RuntimeError("mutation site changed: " + name)
                changed = original.replace(scope, scope.replace(before, after, 1), 1)
                mutant = run(name, changed)
                if not mutant.returncode:
                    raise RuntimeError("mutant survived: " + name)
                regression = ("actual_from_file_copies_only_retained32" if func == "from_file"
                              else "actual_cross_check_exempts_only_zero_at_exact42")
                if regression + " ... FAILED" not in mutant.stdout:
                    raise RuntimeError("new regression did not catch mutant: " + name)
                receipt["mutants"][name] = "caught at runtime"
    receipt["source_files_unchanged"] = all(
        (root / G15 / "board" / name).read_bytes() == data for name, data in sources.items())
    if not receipt["source_files_unchanged"]:
        raise SystemExit("source changed during validation; rerun on a frozen candidate")
    receipt["limits"] = "Host synthetic record data only; two private hardware-fixture tests remain ignored."
    if args.evidence:
        for name, data in sources.items():
            (args.evidence / name).write_bytes(data)
        (args.evidence / "synthetic-tests.rs").write_text(CASES)
        (args.evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
