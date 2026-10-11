#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise generation-4 FIFO recovery using the production function."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - SPMI FIFO recovery # SKIP cc unavailable")
        return 4
    source = (ROOT / "drivers/spmi/spmi-apple-controller.c").read_text()
    start = source.index("static int apple_spmi_recover(")
    end = source.index("static int spmi_raw_cmd(", start)
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define SPMI_ACT_FIFO_FLUSH 1
#define SPMI_ACT_REG 0xa4
#define SPMI_IRQ_NOTIFY 256
struct hw { bool gen4; u32 status, reply, rx_empty; };
struct apple_spmi { unsigned char *regs; const struct hw *hw; };
static unsigned char regs[0x800];
static u32 status, enabled, reset;
static int reads, acks, poll_result;
static u32 readl(void *p) {
    unsigned long off = (unsigned char *)p - regs;
    if (off == 0) return enabled;
    if (off == 0x200) return status;
    if (off == 0x220) reads++;
    return 0;
}
static void writel(u32 v, void *p) {
    unsigned long off = (unsigned char *)p - regs;
    if (off == 0) enabled = v;
    if (off == 4) reset = v;
}
static int poll(u32 *v) {
    *v = 0;
    if (!poll_result) status = BIT(14) | BIT(30);
    return poll_result;
}
#define readl_poll_timeout(addr, val, cond, delay, timeout) poll(&(val))
static void apple_spmi_irq_ack_raw(struct apple_spmi *s, u32 irq) { acks++; }
''' + source[start:end] + r'''
int main(void) {
    struct hw hw = {true, 0x200, 0x220, BIT(30)};
    struct apple_spmi s = {regs, &hw};
    enabled = 7; status = BIT(14) | BIT(30);
    assert(apple_spmi_recover(&s) == 0);
    assert(enabled == 7 && reset == 0 && acks == 1);
    status = 0; reads = 0;
    assert(apple_spmi_recover(&s) == -EIO);
    assert(enabled == 7 && reads == 512 && acks == 2);
    status = BIT(30); poll_result = -ETIMEDOUT;
    assert(apple_spmi_recover(&s) == -ETIMEDOUT);
    assert(enabled == 7 && reset == 1 && acks == 3);
    status = BIT(30); poll_result = 0;
    assert(apple_spmi_recover(&s) == 0 && enabled == 7);
    return 0;
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - SPMI FIFO recovery")
        return 1
    print("ok 1 - SPMI FIFO recovery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
