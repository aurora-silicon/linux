#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check production PHY sequences and crossbar failure admission."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from pmp import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - display register admission # SKIP cc unavailable")
        return 4
    phy = (ROOT / "drivers/phy/apple/atc-t6050-dp.c").read_text()
    xbar = (ROOT / "drivers/mux/apple-display-crossbar.c").read_text()
    data = (ROOT / "drivers/phy/apple/atc-t6050-dp-data.h").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define T8142_MUX_MAX 5
#define resource_size(r) (*(r))
enum atc_operation { ATC_MASK, ATC_TUNABLE, ATC_DELAY, ATC_POLL, ATC_POWER };
struct atc_op { u8 op, bank; u16 offset; u32 mask, value, timeout; };
struct atc_rate_op { struct atc_op op; u8 rates; u32 values[4]; };
struct t6050_dp { void *banks[44]; unsigned long resources[44]; };
static const struct { u8 bank; } tunable_names[] = {
    {41}, {40}, {43}, {42}, {20}, {29}, {18}, {27}, {8}
};
struct apple_dpxbar {
    unsigned char *regs;
    int selected_dispext[5]; u8 pclk[5]; bool uhbr[5], enabled[5], faulted;
};
static int writes, stuck;
static u32 readl(void *p) { return stuck ? ~0U : 0; }
static void dpxbar_clear32(struct apple_dpxbar *x, u32 r, u32 v) { writes++; }
static void dpxbar_set32(struct apple_dpxbar *x, u32 r, u32 v) { writes++; }
static void dpxbar_mask32(struct apple_dpxbar *x, u32 r, u32 m, u32 v) { writes++; }
static void udelay(int v) {}
#define readl_poll_timeout_atomic(addr, value, cond, delay, timeout) \
    ((value = readl(addr)), (cond) ? 0 : -ETIMEDOUT)
''' + data
    program += block(phy, r"^static int atc_validate_step\(")
    start = xbar.index("static const u8 t8142_dest_shift")
    end = xbar.index("static int t8142_link_down", start)
    program += xbar[start:end]
    for name in ["down", "up"]:
        program += block(xbar, "^static int t8142_link_" + name + r"\(")
    program += r'''
int main(void) {
    struct t6050_dp dp = {0};
    const struct { const struct atc_op *s; size_t n; } sets[] = {
        {dp_init, ARRAY_SIZE(dp_init)}, {aux_on, ARRAY_SIZE(aux_on)},
        {aux_off, ARRAY_SIZE(aux_off)}, {dp_poweroff, ARRAY_SIZE(dp_poweroff)},
        {dp_link_off, ARRAY_SIZE(dp_link_off)}
    };
    for (int i = 0; i < 44; i++) { dp.banks[i] = (void *)1; dp.resources[i] = 0x4000; }
    for (int i = 0; i < ARRAY_SIZE(sets); i++)
        for (int j = 0; j < sets[i].n; j++)
            assert(atc_validate_step(&dp, &sets[i].s[j]) == 0);
    for (int i = 0; i < ARRAY_SIZE(dp_link_on); i++)
        for (int r = 0; r < 4; r++) {
            struct atc_op op = dp_link_on[i].op;
            if (!(dp_link_on[i].rates & BIT(r))) continue;
            op.value = dp_link_on[i].values[r];
            assert(atc_validate_step(&dp, &op) == 0);
        }
    struct atc_op bad = {ATC_MASK, 2, 0x4000, 1, 1, 0};
    assert(atc_validate_step(&dp, &bad) == -EINVAL);
    bad.offset = 2;
    assert(atc_validate_step(&dp, &bad) == -EINVAL);
    struct apple_dpxbar x = {0};
    unsigned char registers[0x830] = {0}; x.regs = registers;
    for (int i = 0; i < 5; i++) x.selected_dispext[i] = -1;
    assert(t8142_link_up(&x, 0) == -ENODEV && !writes);
    x.selected_dispext[0] = 0;
    assert(t8142_link_up(&x, 0) == -EINVAL && !writes);
    x.pclk[0] = 1; x.pclk[1] = 1; x.enabled[1] = true; x.uhbr[1] = true;
    assert(t8142_link_up(&x, 0) == -EBUSY && !writes);
    x.enabled[1] = false;
    assert(t8142_link_up(&x, 0) == 0 && x.enabled[0]);
    stuck = 1;
    assert(t8142_link_down(&x, 0) == -ETIMEDOUT && x.faulted && x.enabled[0]);
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - display register admission")
        return 1
    print("ok 1 - display register admission")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
