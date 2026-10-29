#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check production PHY sequences and crossbar failure admission."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from common import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - display register admission # SKIP cc unavailable")
        return 4
    phy = (ROOT / "drivers/phy/apple/atc-t6050-dp.c").read_text()
    xbar = (ROOT / "drivers/mux/apple-display-crossbar.c").read_text()
    data = (ROOT / "drivers/phy/apple/atc-t6050-dp-data.h").read_text()
    data += (ROOT / "drivers/phy/apple/atc-t6050-typec-data.h").read_text()
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
enum atc_operation { ATC_MASK, ATC_TUNABLE, ATC_DELAY, ATC_POLL, ATC_POWER, ATC_FIRMWARE };
struct atc_op { u8 op, bank; u16 offset; u32 mask, value, timeout; };
struct atc_rate_op { struct atc_op op; u8 rates; u32 values[4]; };
struct t6050_dp { void *banks[45]; unsigned long resources[45]; bool typec; };
static const struct { u8 bank; } tunable_names[] = {
    {41}, {40}, {43}, {42}, {20}, {29}, {18}, {27}, {8}, {5}, {10}
};
#define __iomem
#define MUX_MAX 3
typedef int spinlock_t;
struct mux_control_ops { int unused; };
static const struct mux_control_ops apple_dpxbar_ops, apple_dpxbar_t602x_ops, apple_dpxbar_t8142_ops;
''' + block(xbar, r"^struct apple_dpxbar_hw\b") + ';\n' + block(xbar, r"^struct apple_dpxbar\b") + r''';
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
    profiles = re.findall(r"^static const struct apple_dpxbar_hw (\w+) =", xbar, re.M)
    for profile in profiles:
        program += block(xbar, r"^static const struct apple_dpxbar_hw " + profile + r" =") + ";\n"
    start = xbar.index("static const u8 t8142_dest_shift")
    end = xbar.index("static int t8142_link_down", start)
    program += xbar[start:end]
    for name in ["down", "up"]:
        program += block(xbar, "^static int t8142_link_" + name + r"\(")
    program += r'''
int main(void) {
    struct t6050_dp dp = {.typec=true};
    const struct { const struct atc_op *s; size_t n; } sets[] = {
        {dp_init, ARRAY_SIZE(dp_init)}, {aux_on, ARRAY_SIZE(aux_on)},
        {aux_off, ARRAY_SIZE(aux_off)}, {dp_poweroff, ARRAY_SIZE(dp_poweroff)},
        {dp_link_off, ARRAY_SIZE(dp_link_off)},
        {dp_reverse_init, ARRAY_SIZE(dp_reverse_init)},
        {mf_normal_init, ARRAY_SIZE(mf_normal_init)},
        {mf_reverse_init, ARRAY_SIZE(mf_reverse_init)},
        {mf_poweroff, ARRAY_SIZE(mf_poweroff)},
        {dp_group0_off, ARRAY_SIZE(dp_group0_off)},
        {dp_group1_off, ARRAY_SIZE(dp_group1_off)}
    };
    for (int i = 0; i < 45; i++) { dp.banks[i] = (void *)1; dp.resources[i] = 0x4000; }
    for (int i = 0; i < ARRAY_SIZE(sets); i++)
        for (int j = 0; j < sets[i].n; j++)
            assert(atc_validate_step(&dp, &sets[i].s[j]) == 0);
    const struct { const struct atc_rate_op *s; size_t n; } rates[] = {
        {dp_link_on, ARRAY_SIZE(dp_link_on)},
        {dp_group0_on, ARRAY_SIZE(dp_group0_on)},
        {dp_group1_on, ARRAY_SIZE(dp_group1_on)}
    };
    for (int group = 0; group < ARRAY_SIZE(rates); group++)
        for (int i = 0; i < rates[group].n; i++)
            for (int r = 0; r < 4; r++) {
                struct atc_op op = rates[group].s[i].op;
                if (!(rates[group].s[i].rates & BIT(r))) continue;
                op.value = rates[group].s[i].values[r];
                assert(atc_validate_step(&dp, &op) == 0);
            }
    struct atc_op bad = {ATC_MASK, 2, 0x4000, 1, 1, 0};
    assert(atc_validate_step(&dp, &bad) == -EINVAL);
    bad.offset = 2;
    assert(atc_validate_step(&dp, &bad) == -EINVAL);
    bad = (struct atc_op){ATC_FIRMWARE, 0, 0, 0, 1, 0};
    dp.typec = false;
    assert(atc_validate_step(&dp, &bad) == -EINVAL);
    dp.typec = true;
    assert(atc_validate_step(&dp, &bad) == 0);
    bad.value = 2;
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
    checks = "".join(f"assert({profile}.n_mux && {profile}.n_mux <= "
                     "ARRAY_SIZE(((struct apple_dpxbar *)0)->selected_dispext));\n"
                     for profile in profiles)
    program = program.replace("int main(void) {", "int main(void) {\n" + checks)
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
