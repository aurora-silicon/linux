#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the card-reader port decision of drivers/pci/controller/pcie-apple.c on the host.

apple_pcie_sd_work() is cut out of the driver and built against a fixture with the card-detect
level and the port's state, and counters for the power-down and power-up paths. Every
combination is checked: the port goes down only with the slot empty, comes up for a card, and is
left alone when it is already where it should be or when the card-detect line cannot be read.

--mutation NAME rebuilds with one known defect, and the run must then fail:
  empty-kept-up   an empty slot does not power the port down
  cd-inverted     the card-detect level is read the wrong way round
  read-error      a failed card-detect read counts as an empty slot
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/pci/controller/pcie-apple.c"
MUTATIONS = {
    "empty-kept-up": ("\toff = !present;\n", "\toff = false;\n"),
    "cd-inverted": ("\toff = !present;\n", "\toff = present;\n"),
    "read-error": ("\tif (present < 0)\n\t\treturn;\n", "\tif (present < 0)\n\t\tpresent = 0;\n"),
}

FIXTURE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define READ_ONCE(x) (x)
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
static struct delayed_work *to_delayed_work(struct work_struct *w) { return container_of(w, struct delayed_work, work); }
struct gpio_desc { int level; };
struct apple_pcie { int unused; };
struct apple_pcie_port { struct apple_pcie *pcie; struct gpio_desc *card_detect; struct delayed_work sd_work; bool sd_off; };
static int downs, ups;
static int gpiod_get_value_cansleep(struct gpio_desc *d) { return d->level; }
static void apple_pcie_sd_power_down(struct apple_pcie *pcie, struct apple_pcie_port *port) { downs++; port->sd_off = true; }
static int apple_pcie_sd_power_up(struct apple_pcie *pcie, struct apple_pcie_port *port) { ups++; port->sd_off = false; return 0; }
"""

TESTS = r"""
int main(void)
{
	static struct apple_pcie pcie; static struct gpio_desc cd; static struct apple_pcie_port port;
	/* level: 1 card, 0 empty, -5 read error; state -> expected downs, ups */
	const struct { int level; bool off; int down, up; } c[] = {
		{ 0, false, 1, 0 },	/* empty, up -> down */
		{ 0, true, 0, 0 },	/* already down */
		{ 1, true, 0, 1 },	/* card in while down -> up */
		{ 1, false, 0, 0 },	/* card, up: nothing */
		{ -5, false, 0, 0 },	/* unreadable line: leave it */
		{ -5, true, 0, 0 },
	};
	port.pcie = &pcie; port.card_detect = &cd;
	for (unsigned int i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		downs = ups = 0; cd.level = c[i].level; port.sd_off = c[i].off;
		apple_pcie_sd_work(&port.sd_work.work);
		if (downs != c[i].down || ups != c[i].up) {
			printf("case %u: level %d off %d -> down %d up %d\n", i, c[i].level, c[i].off,
			       downs, ups);
			assert(0);
		}
	}
	puts("pcie sd-off decision: 6 cases passed");
	return 0;
}
"""


def cut(text, name):
    m = re.search(r"\n(static[^\n;]*\b%s\(.*?\n\}\n)" % re.escape(name), text, re.S)
    assert m, name
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=None)
    ap.add_argument("--mutation", choices=sorted(MUTATIONS))
    args = ap.parse_args()
    body = cut(DRIVER.read_text(), "apple_pcie_sd_work")
    if args.mutation:
        a, b = MUTATIONS[args.mutation]
        assert body.count(a) == 1, args.mutation
        body = body.replace(a, b)
    with tempfile.TemporaryDirectory(prefix="pcie-sd-off-") as tmp:
        out = Path(args.out) if args.out else Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        src = out / "sd-off.c"
        src.write_text(FIXTURE + body + TESTS)
        exe = out / "sd-off"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        str(src), "-o", str(exe)], check=True)
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
