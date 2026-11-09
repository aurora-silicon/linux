#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the Apple core PMU event placement code against a sysreg model.

Checks the M4/A18 16-bit PMESR fields, the M1 8-bit fields, counter
affinity and that the private config_base flags stay clear of the event
field. Mutations replace one rule each and must be caught. The model does
not qualify counting, overflow or interrupts on hardware.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

MUTATIONS = {
    # The M1 layout on M4 cores: counters 3-5 and 7-9 never count.
    'm1-layout': ('shift = (index - 2) * 16;', 'shift = (index - 2) * 8;'),
    # Cycles allowed on a configurable counter, which counts nothing.
    'cycles-anywhere': ('\t\treturn BIT(0);\n', '\t\treturn BIT(0) | M4_PMU_CNTR_EVT;\n'),
    # Counters 8 and 9 exposed although they never count.
    'expose-8-9': ('#define M4_PMU_CNTR_EVT\t\t\tGENMASK(7, 2)', '#define M4_PMU_CNTR_EVT\t\t\tGENMASK(9, 2)'),
    # Filter flags back inside the 16-bit event field.
    'flags-in-event': ('M1_PMU_CFG_COUNT_USER\t\t\t\t\t= BIT(16),', 'M1_PMU_CFG_COUNT_USER\t\t\t\t\t= BIT(8),'),
}


def function(source, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{', source, re.M)
    if not match:
        raise ValueError('missing function: ' + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def block(source, start):
    i = source.index(start)
    return source[i:source.index('};', i) + 2]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) >> (63 - (h))) & ~((1UL << (l)) - 1))
enum { SYS_IMP_APL_PMESR0_EL1, SYS_IMP_APL_PMESR1_EL1 };
static u64 pmesr[2];
#define sysreg_clear_set_s(r, c, s) (pmesr[r] = (pmesr[r] & ~(u64)(c)) | (u64)(s))
#define M1_PMU_NR_COUNTERS 10
struct pmu_hw_events { unsigned long used_mask[1]; };
static bool test_and_set_bit(int nr, unsigned long *a)
{
	bool old = *a & BIT(nr);
	*a |= BIT(nr);
	return old;
}
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) < (size); (bit)++) if (*(addr) & BIT(bit))
'''

TESTS = r'''
static int place(struct pmu_hw_events *c, unsigned int ev)
{
	return __m1_pmu_get_event_idx(c, m4_pmu_event_affinity(ev));
}

int main(void)
{
	struct pmu_hw_events c = { { 0 } };
	int i;

	assert(!((u64)M1_PMU_CFG_COUNT_USER & M4_PMU_CFG_EVENT));
	assert(!((u64)M1_PMU_CFG_COUNT_KERNEL & M4_PMU_CFG_EVENT));
	assert(!((u64)M1_PMU_CFG_COUNT_HOST & M4_PMU_CFG_EVENT));
	assert(!((u64)M1_PMU_CFG_COUNT_GUEST & M4_PMU_CFG_EVENT));
	assert(M1_PMU_PERFCTR_LAST == 0xff);
	puts("PASS filter flags sit above the 16-bit M4 event field");

	for (i = 2; i <= 9; i++) {
		u64 want0 = 0x1111222233334444ULL, want1 = 0x5555666677778888ULL;

		pmesr[0] = want0;
		pmesr[1] = want1;
		__m4_pmu_configure_eventsel(i, 0x1ca);
		if (i <= 5)
			want0 = (want0 & ~(0xffffULL << ((i - 2) * 16))) | (0x1caULL << ((i - 2) * 16));
		else
			want1 = (want1 & ~(0xffffULL << ((i - 6) * 16))) | (0x1caULL << ((i - 6) * 16));
		assert(pmesr[0] == want0 && pmesr[1] == want1);
	}
	pmesr[0] = pmesr[1] = 7;
	__m4_pmu_configure_eventsel(0, 0x08);
	__m4_pmu_configure_eventsel(1, 0x08);
	assert(pmesr[0] == 7 && pmesr[1] == 7);
	puts("PASS M4 events land in one 16-bit field per counter; fixed counters untouched");

	pmesr[0] = pmesr[1] = 0;
	for (i = 2; i <= 9; i++)
		__m1_pmu_configure_eventsel(i, 0x80 + i);
	assert(pmesr[0] == 0x85848382ULL && pmesr[1] == 0x89888786ULL);
	puts("PASS M1 8-bit event fields unchanged");

	assert(place(&c, M4_PMU_PERFCTR_CORE_ACTIVE_CYCLE) == 0);
	assert(place(&c, M4_PMU_PERFCTR_CORE_ACTIVE_CYCLE) == -EAGAIN);
	assert(place(&c, M4_PMU_PERFCTR_INST_ALL) == 1);
	assert(place(&c, M4_PMU_PERFCTR_INST_ALL) == 2);
	for (i = 3; i <= 7; i++)
		assert(place(&c, 0x1ca) == i);
	assert(place(&c, M4_PMU_PERFCTR_INST_BRANCH) == -EAGAIN);
	assert(!(c.used_mask[0] & (BIT(8) | BIT(9))));
	puts("PASS cycles only on PMC0, instructions on PMC1 then 2-7, never 8 or 9");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    source = (ROOT / 'drivers/perf/apple_m1_cpu_pmu.c').read_text()
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert source.count(old) == 1, args.mutation
        source = source.replace(old, new)
    defines = '\n'.join(re.findall(r'^#define (?:M1_PMU_CFG_EVENT|M4_PMU_\w+)\b.*$', source, re.M))
    parts = [defines,
             block(source, 'enum m1_pmu_events {'),
             block(source, 'enum m4_pmu_events {')]
    parts += [function(source, n) for n in (
        'm4_pmu_event_affinity', '__m1_pmu_configure_eventsel',
        '__m4_pmu_configure_eventsel', '__m1_pmu_get_event_idx')]
    with tempfile.TemporaryDirectory(prefix='apple-m4-pmu-') as tmp:
        tmp = Path(tmp)
        cfile, exe = tmp / 'test.c', tmp / 'test'
        cfile.write_text(PRELUDE + '\n'.join(parts) + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        str(cfile), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout, end='')
        if result.returncode:
            raise SystemExit(result.stderr)


if __name__ == '__main__':
    main()
