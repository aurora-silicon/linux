#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host contract and compiled Rust tests for capacity-derived single-pair M5 render depth."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / "drivers" / "gpu" / "drm" / "asahi" / "g17"


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def extract_block(source: str, signature: str) -> str:
    assert source.count(signature) == 1, f"expected 1 match for {signature!r}"
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth, end = 1, open_brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_capacity_predicate_not_chip_allowlist() -> None:
    src = read("drivers/gpu/drm/asahi/g17/pairs.rs")
    m = re.search(
        r"pub\(crate\)\s+const\s+fn\s+render_depth_for\(([^)]*)\)\s*->\s*u8\s*\{([^}]*)\}",
        src,
        re.S,
    )
    require(m is not None, "render_depth_for() missing in pairs.rs")
    body = m.group(2)
    require("pairs_per_owner == 1" in body, "render_depth_for must gate on single-pair queues")
    require("scratch_slots >= RENDER_DEPTH_MAX as usize" in body,
            "render_depth_for must derive depth from scratch_slots >= RENDER_DEPTH_MAX")
    for chip in ("J714", "J813", "J716", "J775", "T6050", "M5"):
        require(chip not in body, f"render_depth_for must not hardcode chip name {chip}")


def test_scheduler_and_admission_use_dynamic_depth() -> None:
    job_src = read("drivers/gpu/drm/asahi/g17/job.rs")
    require("fn render_depth(&self) -> u32" in job_src, "Backend::render_depth missing")
    require("backend.render_depth().checked_mul(backend.render_pairs())" in job_src,
            "Scheduler::new must scale render credits by backend.render_depth()")

    adm_src = read("drivers/gpu/drm/asahi/g17/queue/admission.rs")
    require("render_depth: u8" in adm_src, "admission::State::render_depth missing")
    require("self.refs[*slot] < self.render_depth" in adm_src,
            "acquire_for must check self.render_depth")
    require("self.refs[slot] < self.render_depth" in adm_src,
            "usable must check self.render_depth")
    require("with_waiters_and_depth(" in adm_src,
            "admission::Pool::with_waiters_and_depth missing")

    rt_src = read("drivers/gpu/drm/asahi/g17/runtime.rs")
    require("admission::Pool::with_waiters_and_depth(" in rt_src,
            "Registry::with_waiters must pass device render depth to admission::Pool")
    require("render_depth: super::pairs::render_depth_for_device(" in rt_src,
            "Backend::new must sample render_depth_for_device")

    mod_src = read("drivers/gpu/drm/asahi/g17/mod.rs")
    require(mod_src.count("recovery::State::with_forgive_limit(") == 2,
            "both boot() and boot_m5() must pass active render depth to recovery::State")
    require("pairs::render_depth_for_device(shared.variant.m5_commands)" in mod_src,
            "recovery::State must derive forgive_limit from pairs::render_depth_for_device()")


def test_memory_target_and_retirement_support_64_records() -> None:
    mem_src = read("drivers/gpu/drm/asahi/g17/queue/render/memory.rs")
    require("pub(super) const RECORDS_MAX: usize = queues::RENDER_DEPTH_MAX as usize;" in mem_src,
            "RECORDS_MAX missing in memory.rs")
    require("pub(super) const GRAPH_SIZE_MAX: usize = 0x20000;" in mem_src,
            "GRAPH_SIZE_MAX must cover 64 announce records at 0x1b000 + 64 * 0x80")
    require("pub(super) const CONFIG_LARGE: [usize; 2] = [0, 0x8000];" in mem_src,
            "CONFIG_LARGE must separate stage 0 and stage 1 by 0x8000 for 64 * 0x180")
    require("assert!(ANNOUNCE[1] + RECORDS_MAX * ANNOUNCE_STRIDE <= GRAPH_SIZE_MAX);" in mem_src,
            "static assert for 64 announce records missing")
    require("assert!(CONFIG_LARGE[1] + RECORDS_MAX * CONFIG_STRIDE <= CONFIG_SIZE_LARGE);" in mem_src,
            "static assert for 64 config records missing")
    require("assert!(RECORDS_MAX * M5_USAGE_PAIR_BYTES <= mmu::UAT_PGSZ);" in mem_src,
            "static assert for 64 M5 usage records missing")
    require("assert!(RECORDS_MAX * 32 <= mmu::UAT_PGSZ);" in mem_src,
            "static assert for 64 M5 timestamp records missing")

    pub_src = read("drivers/gpu/drm/asahi/g17/queue/render/publication.rs")
    require("graph.config_offset(stage, ordinal)" in pub_src,
            "publication::Lane must use graph.config_offset()")
    require("graph.announce_offset(stage, ordinal)" in pub_src,
            "publication::Lane must use graph.announce_offset()")

    tgt_src = read("drivers/gpu/drm/asahi/g17/buffer/target.rs")
    require("const DEPTH_MAX: usize = queues::RENDER_DEPTH_MAX as usize;" in tgt_src,
            "target.rs must size slots/owners to DEPTH_MAX")
    require("ordinal as usize % self.depth" in tgt_src,
            "target.rs must index slots modulo self.depth")

    ret_src = read("drivers/gpu/drm/asahi/g17/queue/render/retirement.rs")
    require("pub(crate) fn with_capacity(capacity: usize) -> Result<Self>" in ret_src,
            "Tracker::with_capacity missing")
    require("entries: KVec<Option<T>>" in ret_src,
            "Tracker must store entries in KVec to keep Pair stack frames small")


def test_compact_window_accounts_for_depth() -> None:
    sub_src = read("drivers/gpu/drm/asahi/g17/queue/render/m5_render_submission.rs")
    require("crate::g17::pairs::render_depth_for(" in sub_src,
            "m5_render_submission::pair_sizes must use pairs::render_depth_for()")
    cmp_src = read("drivers/gpu/drm/asahi/g17/buffer/compact.rs")
    require("PairObject::SlotState => (self.depth, self.state)," in cmp_src,
            "PairSizes::of must multiply SlotState by self.depth")


def test_compiled_recovery_storm_forgive_budgets(mutation: str | None) -> None:
    """Compile production pairs::render_depth_for and recovery::safety::Storm on host."""
    pairs_src = read("drivers/gpu/drm/asahi/g17/pairs.rs")
    safety_src = read("drivers/gpu/drm/asahi/g17/recovery/safety.rs")

    if mutation == "global-forgive-64":
        old = "self.forgiven >= self.forgive_limit"
        require(safety_src.count(old) == 1, "mutation target missing in safety.rs")
        safety_src = safety_src.replace(old, "self.forgiven >= crate::hw::t8140::queues::RENDER_DEPTH_MAX")
    elif mutation == "m5-stuck-32":
        old = "RENDER_DEPTH_MAX\n    } else {"
        require(pairs_src.count(old) == 1, "mutation target missing in pairs.rs")
        pairs_src = pairs_src.replace(old, "RENDER_DEPTH\n    } else {")

    depth_fn = extract_block(pairs_src, "pub(crate) const fn render_depth_for(")
    storm_struct = extract_block(safety_src, "pub(super) struct Storm")
    storm_impl = extract_block(safety_src, "impl Storm")

    harness = f"""
#![allow(dead_code)]
mod hw {{
    pub mod t8140 {{
        pub mod queues {{
            pub const RENDER_DEPTH: u8 = 32;
            pub const RENDER_DEPTH_MAX: u8 = 64;
        }}
    }}
}}
use hw::t8140::queues::{{RENDER_DEPTH, RENDER_DEPTH_MAX}};

{depth_fn}

mod safety {{
    use crate::hw::t8140::queues::RENDER_DEPTH;
    const WINDOW_NS: u64 = 30_000_000_000;
    const LIMIT: u8 = 8;
    const DEFAULT_FORGIVE_LIMIT: u8 = RENDER_DEPTH;

    {storm_struct}
    {storm_impl}
}}
use safety::Storm;
const WINDOW_NS: u64 = 30_000_000_000;
const LIMIT: u8 = 8;

fn check_budget(limit: u8) {{
    let mut storm = Storm::with_forgive_limit(limit);
    for i in 0..limit {{
        assert!(!storm.note(0), "forgiven restart {{i}} of {{limit}} must not stop");
        assert!(storm.forgive(), "restart {{i}} of {{limit}} must be forgiven");
    }}
    // Final un-forgiven event after budget exhaustion.
    assert!(!storm.note(0), "first un-forgiven restart after {{limit}} must not stop yet");
    assert!(!storm.forgive(), "restart beyond {{limit}} must not be forgiven");
    for i in 2..LIMIT {{
        assert!(!storm.note(0), "un-forgiven restart {{i}} must not stop yet");
    }}
    // Reaching LIMIT latches the stop permanently, including across WINDOW_NS.
    assert!(storm.note(0), "LIMIT-th un-forgiven restart must latch stop");
    assert!(!storm.forgive(), "latched stop cannot be forgiven");
    assert!(storm.note(WINDOW_NS), "latched stop must stay latched past WINDOW_NS");
    assert!(!storm.forgive(), "latched stop must stay unforgiven past WINDOW_NS");

    // Before latching, a new window resets forgiveness.
    let mut fresh = Storm::with_forgive_limit(limit);
    for _ in 0..limit {{
        assert!(!fresh.note(0));
        assert!(fresh.forgive());
    }}
    assert!(!fresh.note(WINDOW_NS));
    assert!(fresh.forgive());
}}

fn main() {{
    let dual_pair = render_depth_for(64, 2);
    let small_scratch = render_depth_for(32, 1);
    let single_pair_m5 = render_depth_for(64, 1);
    assert_eq!(dual_pair, 32, "dual-pair devices must stay at depth 32");
    assert_eq!(small_scratch, 32, "devices with <64 scratch slots must stay at depth 32");
    assert_eq!(single_pair_m5, 64, "single-pair 64-scratch devices must expand to depth 64");

    // Default Storm::new() stays at 32.
    let mut default_storm = Storm::new();
    for _ in 0..32 {{
        assert!(!default_storm.note(0));
        assert!(default_storm.forgive());
    }}
    assert!(!default_storm.note(0));
    assert!(!default_storm.forgive(), "default Storm must cap forgiveness at 32");

    check_budget(dual_pair);
    check_budget(single_pair_m5);
    println!("PASS compiled render depth and Storm forgiveness budgets (32 & 64)");
}}
"""
    with tempfile.TemporaryDirectory(prefix="g17-render-depth-") as tmp:
        src = Path(tmp) / "test.rs"
        bin_path = Path(tmp) / "test"
        src.write_text(harness, encoding="utf-8")
        subprocess.run(
            [os.environ.get("RUSTC", "rustc"), "--edition=2021", str(src), "-o", str(bin_path)],
            check=True,
        )
        res = subprocess.run([str(bin_path)])
        if res.returncode != 0:
            raise SystemExit(res.returncode)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutation", choices=("global-forgive-64", "m5-stuck-32"))
    args = ap.parse_args()

    tests = [
        test_capacity_predicate_not_chip_allowlist,
        test_scheduler_and_admission_use_dynamic_depth,
        test_memory_target_and_retirement_support_64_records,
        test_compact_window_accounts_for_depth,
    ]
    for fn in tests:
        fn()
    test_compiled_recovery_storm_forgive_budgets(args.mutation)
    print(f"g17-render-depth-tests: {len(tests) + 1}/{len(tests) + 1} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
