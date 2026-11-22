#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production apple_clpc_ane_cpu_opp() over a fake device tree.

The CLPC node's apple,ane-cpu-opp names an OPP of the CPU cluster whose
frequency sets the Neural Engine's speed. The parser must return a CPU that
uses the OPP's table and the OPP's frequency in kHz, -ENOENT without the
property, -ENODEV for an OPP no CPU uses, one outside a table or one without
an opp-hz a frequency QoS request can hold (1 kHz to S32_MAX kHz), and put
every node it got.

--mutation names a change to the production source that the matrix must kill.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(os.environ.get('CLPC_ROOT', Path(__file__).resolve().parents[2]))

MUTATIONS = {
    # Take any CPU, not one that uses the OPP's table.
    'any-cpu': ('match = cpu_table == table;', 'match = true;'),
    # Report hertz, not kilohertz.
    'hz': ('*khz = div_u64(hz, 1000);', '*khz = hz;'),
    # Leak the OPP node.
    'leak': ('\tof_node_put(opp);\n\treturn found;', '\treturn found;'),
    # Bound the floor by the u32 range, which a negative s32 request passes.
    'u32': ('div_u64(hz, 1000) > S32_MAX', 'div_u64(hz, 1000) > UINT_MAX'),
    # Accept an OPP outside any table.
    'no-parent': ('if (!table || of_property_read_u64', 'if (of_property_read_u64'),
}


def item(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define ENOENT 2
#define ENODEV 19
#define UINT_MAX 0xffffffffu
#define S32_MAX 0x7fffffff
#define NR_CPUS 11
static u64 div_u64(u64 a, unsigned int b) { return a / b; }
struct device_node { const char *name; struct device_node *parent, *opp_ref, *table_ref;
	int has_hz; u64 hz; int refs; };
static int n_get, n_put;
static struct device_node *get(struct device_node *np) { if (np) { np->refs++; n_get++; } return np; }
static void of_node_put(struct device_node *np) { if (np) { np->refs--; n_put++; } }
static struct device_node *of_parse_phandle(const struct device_node *np, const char *prop, int i)
{
	assert(i == 0);
	if (!strcmp(prop, "apple,ane-cpu-opp")) return get(np->opp_ref);
	if (!strcmp(prop, "operating-points-v2")) return get(np->table_ref);
	assert(0); return 0;
}
static struct device_node *of_get_parent(const struct device_node *np) { return get(np->parent); }
static int of_property_read_u64(const struct device_node *np, const char *prop, u64 *v)
{
	assert(!strcmp(prop, "opp-hz"));
	if (!np->has_hz) return -22;
	*v = np->hz; return 0;
}
static struct device_node *cpus[NR_CPUS];
static struct device_node *of_cpu_device_node_get(int cpu) { return get(cpus[cpu]); }
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < NR_CPUS; (cpu)++)

PARSER

int main(void)
{
	struct device_node etab = { "e-opp" }, ptab = { "p-opp" }, other = { "gpu-opp" };
	struct device_node popp = { "opp-2616000000", &ptab, 0, 0, 1, 2616000000ull };
	struct device_node eopp = { "opp-912000000", &etab, 0, 0, 1, 912000000ull };
	struct device_node gopp = { "opp", &other, 0, 0, 1, 1000000000ull };
	struct device_node nohz = { "opp-nohz", &ptab, 0, 0, 0, 0 };
	struct device_node tiny = { "opp-tiny", &ptab, 0, 0, 1, 999 };
	struct device_node huge = { "opp-huge", &ptab, 0, 0, 1, 5000000000000ull };
	/* Above S32_MAX kHz but within u32 kHz: a negative s32 request. */
	struct device_node s32 = { "opp-s32", &ptab, 0, 0, 1, (S32_MAX + 1ull) * 1000 };
	struct device_node top = { "opp-top", &ptab, 0, 0, 1, S32_MAX * 1000ull + 999 };
	struct device_node low = { "opp-low", &ptab, 0, 0, 1, 1000 };
	struct device_node orphan = { "opp-orphan", 0, 0, 0, 1, 2616000000ull };
	struct device_node cpu[NR_CPUS];
	struct device_node clpc = { "clpc" };
	unsigned int khz;
	int i, r;

	memset(cpu, 0, sizeof(cpu));
	/* T8132: CPUs 0-5 E, 6-9 P; CPU 7 has no node, CPU 10 no OPP table. */
	for (i = 0; i < NR_CPUS; i++) {
		cpu[i].name = "cpu";
		cpu[i].table_ref = i < 6 ? &etab : i < 10 ? &ptab : 0;
		cpus[i] = i == 7 ? 0 : &cpu[i];
	}

	khz = 7; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r == -ENOENT && khz == 7);

	clpc.opp_ref = &popp; khz = 0; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r >= 6 && r != 7 && cpu[r].table_ref == &ptab);
	assert(khz == 2616000);

	clpc.opp_ref = &eopp; khz = 0; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r >= 0 && r < 6 && khz == 912000);

	/* The bounds: 1 kHz and S32_MAX kHz are both held. */
	clpc.opp_ref = &low; khz = 0; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r >= 6 && r < 10 && khz == 1);
	clpc.opp_ref = &top; khz = 0; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r >= 6 && r < 10 && khz == S32_MAX);

	for (i = 0; i < 6; i++) {
		struct device_node *bad[] = { &gopp, &nohz, &tiny, &huge, &s32, &orphan };
		clpc.opp_ref = bad[i]; khz = 7; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
		assert(r == -ENODEV && khz == 7);
	}

	/* A table no CPU uses, with the CPUs' nodes all missing. */
	for (i = 0; i < NR_CPUS; i++) cpus[i] = 0;
	clpc.opp_ref = &popp; r = apple_clpc_ane_cpu_opp(&clpc, &khz);
	assert(r == -ENODEV);

	assert(n_get == n_put);
	printf("PASS apple,ane-cpu-opp: CPU from the OPP's table, kHz from opp-hz, refusals, %d gets put\n", n_get);
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    source = (ROOT / 'drivers/soc/apple/clpc.c').read_text()
    fn = item(source, 'static int apple_clpc_ane_cpu_opp(')
    fn = fn.replace('const struct device_node *np', 'struct device_node *np')
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert fn.count(old) == 1, args.mutation
        fn = fn.replace(old, new)
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 't.c'
        c.write_text(HARNESS.replace('PARSER', fn))
        exe = Path(tmp) / 't'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror', '-fsanitize=address,undefined',
                        '-o', str(exe), str(c)], check=True)
        res = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(res.stdout)
        sys.stderr.write(res.stderr)
        return res.returncode


if __name__ == '__main__':
    sys.exit(main())
