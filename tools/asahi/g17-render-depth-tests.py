#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host contract tests for capacity-derived single-pair M5 render depth."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / "drivers" / "gpu" / "drm" / "asahi" / "g17"


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


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


def main() -> int:
    tests = [
        test_capacity_predicate_not_chip_allowlist,
        test_scheduler_and_admission_use_dynamic_depth,
        test_memory_target_and_retirement_support_64_records,
        test_compact_window_accounts_for_depth,
    ]
    for fn in tests:
        fn()
    print(f"g17-render-depth-tests: {len(tests)}/{len(tests)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
