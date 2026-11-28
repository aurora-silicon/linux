#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the SMC GPIO interrupt path of drivers/gpio/gpio-macsmc.c on the host.

The event handler, the irqchip callbacks and the key helper are cut out of the
driver and built against a fixture that records every SMC write and every
nested interrupt. The cases pin that:
  - only SMC_EV_GPIO events are handled; the line comes from bits 15:8, an out
    of range line is refused, and every handled event is acknowledged on its
    own key (gPxx, CMD_IRQ_ACK | 1), mapped or not;
  - each IRQ type maps to the SMC's mode, an unsupported line refuses a type;
  - the bus unlock writes the mode, then the enable, each only when it differs
    from what the SMC was last given; a failed write is retried at the next
    unlock; the first unlock always states the enable.

--mutation NAME rebuilds with one known defect, and the run must then fail:
  no-ack       the event is never acknowledged
  stale-mode   the mode bookkeeping of the earlier Asahi version (written every time)
  any-event    the event type is not checked
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/gpio/gpio-macsmc.c"
FUNCS = ["macsmc_gpio_key", "macsmc_gpio_event", "macsmc_gpio_irq_mask", "macsmc_gpio_irq_unmask",
         "macsmc_gpio_irq_set_type", "macsmc_gpio_irq_bus_lock", "macsmc_gpio_irq_bus_sync_unlock"]
MUTATIONS = {
    "no-ack": ("\tkey = macsmc_gpio_key(smcgp->base_key, offset);\n\tif (apple_smc_write_u32(smcgp->smc, key, CMD_IRQ_ACK | 1) < 0)",
               "\tkey = macsmc_gpio_key(smcgp->base_key, offset);\n\tif ((void)key, 0)"),
    "stale-mode": ("\t\telse\n\t\t\tsmcgp->irq_mode_hw[hwirq] = mode;\n",
                   "\t\telse\n\t\t\tmode = smcgp->irq_mode_hw[hwirq];\n"),
    "any-event": ("\tif ((event >> 16) != SMC_EV_GPIO)\n\t\treturn NOTIFY_DONE;\n", ""),
}

FIXTURE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint32_t smc_key; typedef unsigned long irq_hw_number_t;
#define MAX_GPIO 64
#define BITS_PER_LONG (8 * sizeof(long))
#define DECLARE_BITMAP(n, b) unsigned long n[((b) + BITS_PER_LONG - 1) / BITS_PER_LONG]
static void set_bit(unsigned long nr, unsigned long *a) { a[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG); }
static void clear_bit(unsigned long nr, unsigned long *a) { a[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG)); }
static bool test_bit(unsigned long nr, const unsigned long *a) { return a[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG) & 1; }
static void assign_bit(unsigned long nr, unsigned long *a, bool v) { if (v) set_bit(nr, a); else clear_bit(nr, a); }
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#include <stddef.h>
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define NOTIFY_BAD 0x8002
#define EINVAL 22
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQ_TYPE_EDGE_BOTH 3
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW 8
#define IRQ_TYPE_SENSE_MASK 0xf
#define CMD_IRQ_ENABLE (4 << 24)
#define CMD_IRQ_ACK (5 << 24)
#define CMD_IRQ_MODE (6 << 24)
#define IRQ_MODE_HIGH 0
#define IRQ_MODE_LOW 1
#define IRQ_MODE_RISING 2
#define IRQ_MODE_FALLING 3
#define IRQ_MODE_BOTH 4
#define SMC_EV_GPIO 0x7202
#define IRQ_MODE_UNSET 0xff
static const char hex_asc[] = "0123456789abcdef";
#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); };
struct mutex { int held; };
struct apple_smc { int unused; };
struct irq_domain { int unused; };
struct gpio_chip { struct { struct irq_domain *domain; } irq; };
struct irq_data { void *chip_data; irq_hw_number_t hwirq; };
struct macsmc_gpio {
	void *dev; struct apple_smc *smc; struct gpio_chip gc; smc_key base_key;
	struct notifier_block nb; struct mutex irq_lock;
	DECLARE_BITMAP(irq_supported, MAX_GPIO);
	DECLARE_BITMAP(irq_enable_want, MAX_GPIO);
	DECLARE_BITMAP(irq_enable_hw, MAX_GPIO);
	DECLARE_BITMAP(irq_enable_known, MAX_GPIO);
	u8 irq_mode_want[MAX_GPIO];
	u8 irq_mode_hw[MAX_GPIO];
};
/* The fixture's world */
static struct macsmc_gpio *the; static struct apple_smc smc; static struct irq_domain dom;
static bool mapped[MAX_GPIO]; static int nested[8]; static int nnested;
static struct { smc_key key; u32 val; } wr[32]; static int nwr; static int fail_next_write;
static int enabled_calls, disabled_calls;
static int apple_smc_write_u32(struct apple_smc *s, smc_key key, u32 val)
{ assert(s == &smc); if (fail_next_write) { fail_next_write = 0; return -5; } assert(nwr < 32); wr[nwr].key = key; wr[nwr].val = val; nwr++; return 0; }
static int irq_find_mapping(struct irq_domain *d, unsigned int hw) { assert(d == &dom); return hw < MAX_GPIO && mapped[hw] ? 100 + hw : 0; }
static void handle_nested_irq(int irq) { assert(nnested < 8); nested[nnested++] = irq; }
static void *irq_data_get_irq_chip_data(struct irq_data *d) { return d->chip_data; }
static irq_hw_number_t irqd_to_hwirq(struct irq_data *d) { return d->hwirq; }
static void *gpiochip_get_data(struct gpio_chip *gc) { assert(gc == &the->gc); return the; }
static void gpiochip_enable_irq(struct gpio_chip *gc, irq_hw_number_t h) { enabled_calls++; }
static void gpiochip_disable_irq(struct gpio_chip *gc, irq_hw_number_t h) { disabled_calls++; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = 0; }
"""

TESTS = r"""
static smc_key KEY(const char *s) { return (smc_key)s[0] << 24 | (smc_key)s[1] << 16 | (smc_key)s[2] << 8 | (smc_key)s[3]; }
static void reset(struct macsmc_gpio *g)
{
	memset(g, 0, sizeof(*g)); g->smc = &smc; g->gc.irq.domain = &dom; g->base_key = KEY("gP\0\0");
	for (int i = 0; i < MAX_GPIO; i++) g->irq_mode_want[i] = g->irq_mode_hw[i] = IRQ_MODE_UNSET;
	set_bit(9, g->irq_supported); set_bit(0x19, g->irq_supported);
	memset(mapped, 0, sizeof(mapped)); nnested = nwr = fail_next_write = 0;
}
static void sync(struct macsmc_gpio *g, unsigned int line)
{ struct irq_data d = { &g->gc, line }; macsmc_gpio_irq_bus_lock(&d); macsmc_gpio_irq_bus_sync_unlock(&d); }
static int settype(struct macsmc_gpio *g, unsigned int line, unsigned int t)
{ struct irq_data d = { &g->gc, line }; return macsmc_gpio_irq_set_type(&d, t); }
static void unmask(struct macsmc_gpio *g, unsigned int l) { struct irq_data d = { &g->gc, l }; macsmc_gpio_irq_unmask(&d); }
static void mask(struct macsmc_gpio *g, unsigned int l) { struct irq_data d = { &g->gc, l }; macsmc_gpio_irq_mask(&d); }
int main(void)
{
	static struct macsmc_gpio g; the = &g;

	/* key helper: line 0x19 is gP19, line 9 is gP09 */
	reset(&g); assert((smc_key)macsmc_gpio_key(g.base_key, 0x19) == KEY("gP19")); assert((smc_key)macsmc_gpio_key(g.base_key, 9) == KEY("gP09"));

	/* 1. a GPIO event for a mapped line: one nested interrupt, then the ack on its key */
	reset(&g); mapped[9] = true;
	assert(macsmc_gpio_event(&g.nb, (unsigned long)SMC_EV_GPIO << 16 | 9 << 8, NULL) == NOTIFY_OK);
	assert(nnested == 1 && nested[0] == 109);
	assert(nwr == 1 && wr[0].key == KEY("gP09") && wr[0].val == (CMD_IRQ_ACK | 1));
	/* 2. another event type: not ours, nothing touched */
	reset(&g); mapped[9] = true;
	assert(macsmc_gpio_event(&g.nb, 0x7201UL << 16 | 9 << 8, NULL) == NOTIFY_DONE && !nnested && !nwr);
	/* 3. a line out of range: refused, nothing touched */
	reset(&g);
	assert(macsmc_gpio_event(&g.nb, (unsigned long)SMC_EV_GPIO << 16 | 0x50 << 8, NULL) == NOTIFY_BAD && !nnested && !nwr);
	/* 4. an unmapped line: no interrupt, but still acknowledged (or the SMC holds the next one) */
	reset(&g);
	assert(macsmc_gpio_event(&g.nb, (unsigned long)SMC_EV_GPIO << 16 | 9 << 8, NULL) == NOTIFY_OK && !nnested);
	assert(nwr == 1 && wr[0].val == (CMD_IRQ_ACK | 1));

	/* 5. types: each maps, an unsupported line or an unknown type is refused */
	reset(&g);
	const unsigned int t[5] = { IRQ_TYPE_LEVEL_HIGH, IRQ_TYPE_LEVEL_LOW, IRQ_TYPE_EDGE_RISING, IRQ_TYPE_EDGE_FALLING, IRQ_TYPE_EDGE_BOTH };
	const unsigned int m[5] = { IRQ_MODE_HIGH, IRQ_MODE_LOW, IRQ_MODE_RISING, IRQ_MODE_FALLING, IRQ_MODE_BOTH };
	for (int i = 0; i < 5; i++) { assert(!settype(&g, 9, t[i])); assert(g.irq_mode_want[9] == m[i]); }
	assert(settype(&g, 10, IRQ_TYPE_EDGE_BOTH) == -EINVAL);
	assert(settype(&g, 9, 0) == -EINVAL);

	/* 6. the bus: mode then enable, once; nothing more while nothing changes */
	reset(&g);
	assert(!settype(&g, 9, IRQ_TYPE_EDGE_BOTH)); unmask(&g, 9); sync(&g, 9);
	assert(nwr == 2 && wr[0].key == KEY("gP09") && wr[0].val == (CMD_IRQ_MODE | IRQ_MODE_BOTH) && wr[1].val == (CMD_IRQ_ENABLE | 1));
	nwr = 0; sync(&g, 9); assert(nwr == 0);
	assert(!settype(&g, 9, IRQ_TYPE_EDGE_BOTH)); sync(&g, 9); assert(nwr == 0);
	mask(&g, 9); sync(&g, 9); assert(nwr == 1 && wr[0].val == (CMD_IRQ_ENABLE | 0));
	nwr = 0; assert(!settype(&g, 9, IRQ_TYPE_LEVEL_HIGH)); sync(&g, 9); assert(nwr == 1 && wr[0].val == (CMD_IRQ_MODE | IRQ_MODE_HIGH));
	/* 7. a failed write is retried at the next unlock */
	reset(&g); assert(!settype(&g, 9, IRQ_TYPE_EDGE_RISING)); unmask(&g, 9);
	fail_next_write = 1; sync(&g, 9); assert(nwr == 1 && wr[0].val == (CMD_IRQ_ENABLE | 1));
	nwr = 0; sync(&g, 9); assert(nwr == 1 && wr[0].val == (CMD_IRQ_MODE | IRQ_MODE_RISING));
	/* 8. the first unlock of a masked line states the enable (0), then never again */
	reset(&g); sync(&g, 0x19); assert(nwr == 1 && wr[0].key == KEY("gP19") && wr[0].val == (CMD_IRQ_ENABLE | 0));
	nwr = 0; sync(&g, 0x19); assert(nwr == 0);
	assert(!g.irq_lock.held);
	puts("macsmc gpio irq: 8 cases passed");
	return 0;
}
"""


def cut(text, name):
    m = re.search(r"\n(static[^\n;]*\b%s\(.*?\n\}\n)" % re.escape(name), text, re.S)
    assert m, name
    return m.group(1)


def build(mutation):
    text = DRIVER.read_text()
    body = "\n".join(cut(text, f) for f in FUNCS)
    if mutation:
        a, b = MUTATIONS[mutation]
        assert body.count(a) == 1, mutation
        body = body.replace(a, b)
    return FIXTURE + body + TESTS


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=None)
    ap.add_argument("--mutation", choices=sorted(MUTATIONS))
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="macsmc-irq-") as tmp:
        out = Path(args.out) if args.out else Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        src = out / "macsmc-irq.c"
        src.write_text(build(args.mutation))
        exe = out / "macsmc-irq"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-Wno-unused-function", str(src), "-o", str(exe)], check=True)
        rc = subprocess.run([str(exe)]).returncode
    if args.mutation:
        if rc == 0:
            print("mutation %s survived" % args.mutation)
            return 1
        print("mutation %s killed" % args.mutation)
        return 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
