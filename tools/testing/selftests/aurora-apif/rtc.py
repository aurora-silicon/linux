#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the production PMU clock reader and alarm transaction."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from pmp import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - PMU RTC transactions # SKIP cc unavailable")
        return 4
    source = (ROOT / "drivers/rtc/rtc-apple-spmi-pmu.c").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
typedef uint64_t u64;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define RTC_SEC_SHIFT 15
#define ALARM_EVENT BIT(1)
#define ALARM_MONITOR BIT(0)
#define ALARM_ENABLE BIT(6)
#define ALARM_IRQ_MASK BIT(0)
struct nvmem_cell { int id; };
struct apple_pmu_rtc {
    struct nvmem_cell *counter, *offset, *ctrl, *irq_mask;
    bool alarm_fault;
};
static int unstable_counter, unstable_offset, fault, writes, reads;
static u8 values[2] = {ALARM_EVENT | ALARM_ENABLE | BIT(7), BIT(3)};
static int ids[8];
static u8 out[8];
static int apple_pmu_rtc_read_u48(struct nvmem_cell *c, u64 *v) {
    reads++;
    if (fault) return -EIO;
    if (c->id == 2) *v = 10ULL << 16 | (unstable_counter ? (u64)reads << 16 : 0);
    else *v = (1ULL << 48) - (2ULL << 15) + (unstable_offset ? reads : 0);
    return 0;
}
static int apple_pmu_rtc_read_cell(struct nvmem_cell *c, void *v, size_t n) {
    if (fault) return -EIO;
    *(u8 *)v = values[c->id];
    return 0;
}
static int apple_pmu_rtc_write_u8(struct nvmem_cell *c, u8 v) {
    ids[writes] = c->id; out[writes++] = v;
    return 0;
}
'''
    for name in ["ticks", "program"]:
        program += block(source, "^static int apple_pmu_rtc_" + name + r"\(")
    program += r'''
int main(void) {
    struct nvmem_cell counter = {2}, offset = {3}, ctrl = {0}, mask = {1};
    struct apple_pmu_rtc rtc = {&counter, &offset, &ctrl, &mask, false};
    u64 ticks, off;
    assert(apple_pmu_rtc_ticks(&rtc, &ticks, &off) == 0);
    assert((ticks & ((1ULL << 48) - 1)) == 8ULL << 15);
    unstable_counter = 1; reads = 0;
    assert(apple_pmu_rtc_ticks(&rtc, &ticks, &off) == -EIO && reads == 3);
    unstable_counter = 0; unstable_offset = 1;
    assert(apple_pmu_rtc_ticks(&rtc, &ticks, &off) == -EIO);
    unstable_offset = 0; fault = 1;
    assert(apple_pmu_rtc_program(&rtc, true) == -EIO && writes == 0);
    fault = 0;
    assert(apple_pmu_rtc_program(&rtc, true) == 0 && writes == 4);
    assert(ids[0] == 1 && out[0] == (BIT(3) | ALARM_IRQ_MASK));
    assert(ids[1] == 0 && !(out[1] & ALARM_ENABLE) && (out[1] & ALARM_EVENT));
    assert(ids[2] == 0 && (out[2] & ALARM_ENABLE) && !(out[2] & ALARM_EVENT));
    assert((out[2] & BIT(7)) && ids[3] == 1 && out[3] == BIT(3));
    rtc.alarm_fault = true;
    assert(apple_pmu_rtc_program(&rtc, true) == -EIO && writes == 4);
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - PMU RTC transactions")
        return 1
    print("ok 1 - PMU RTC transactions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
