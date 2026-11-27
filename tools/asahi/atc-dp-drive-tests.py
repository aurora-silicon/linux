#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""How the ATC PHY answers a DP drive level request, and what the firmware-27 drive call does with it.

Builds the production atcphy_dpphy_configure() against stubs and runs it over
PHY generation, DP state and the request's set_voltages/set_rate/set_lanes. A
PHY without drive level control (the T8103 generation: T8103, T8112, T600x,
T602x) accepts a drive-only request and keeps the levels its tunables set, as
the T8132 branch does; a drive request together with a rate or lane change is
refused. The firmware-27 SetDriveSettings handler passes every PHY error back
to DCP (no errno is special), so a real failure still fails the call.

--mutation names a change to the production source that the matrix must kill.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

MUTATIONS = {
    # The PR's first version: a drive-only request answered -EOPNOTSUPP.
    'eopnotsupp': ('\t\treturn 0;\n\t}\n\tif (opts->set_voltages || opts->set_lanes)',
                   '\t\treturn -EOPNOTSUPP;\n\t}\n\tif (opts->set_voltages || opts->set_lanes)'),
    # Accept drive levels that come with a rate change.
    'accept-with-rate': ('opts->set_voltages && !opts->set_rate && !opts->set_lanes',
                         'opts->set_voltages && !opts->set_lanes'),
    # The pre-PR refusal of every drive request.
    'refuse-drive': ('opts->set_voltages && !opts->set_rate && !opts->set_lanes', '0'),
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation', choices=sorted(MUTATIONS))
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
atc_c = (root / 'drivers/phy/apple/atc.c').read_text()
drive_inc = (root / 'drivers/gpu/drm/apple/dptx-drive-v27.inc').read_text()


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


configure = function(atc_c, 'static int atcphy_dpphy_configure(struct phy *phy,')
# Selected by the PHY's generation and hardware flags, never by a compatible.
assert 'compatible' not in configure and 'of_machine' not in configure

# The firmware-27 drive call returns every PHY error to DCP unchanged.
drive = function(drive_inc, 'static int dptxport_call_drive_v27(')
call = drive[drive.index('ret = phy_configure(dptx->atcphy, &opts);'):]
assert re.match(r'ret = phy_configure\(dptx->atcphy, &opts\);\s*if \(ret\)\s*return ret;', call), \
    'dptxport_call_drive_v27() must pass every phy_configure() error back'
assert 'EOPNOTSUPP' not in drive

if args.mutation:
    old, new = MUTATIONS[args.mutation]
    assert configure.count(old) == 1, args.mutation
    configure = configure.replace(old, new)

program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned int u32;
enum atcphy_dp_link_rate { ATCPHY_DP_LINK_RATE_RBR, ATCPHY_DP_LINK_RATE_HBR, ATCPHY_DP_LINK_RATE_HBR2,
			   ATCPHY_DP_LINK_RATE_HBR3 };
enum { ATCPHY_GENERATION_T8103, ATCPHY_GENERATION_T8122, ATCPHY_GENERATION_T8132 };
enum { APPLE_ATCPHY_MODE_OFF, APPLE_ATCPHY_MODE_DP, APPLE_ATCPHY_MODE_USB3_DP };
struct phy_configure_opts_dp { u32 link_rate, lanes, voltage[4], pre[4]; bool set_rate, set_lanes, set_voltages; };
union phy_configure_opts { struct phy_configure_opts_dp dp; };
struct hw { int gen; bool has_t8140_dp, dp_t8122; };
struct apple_atcphy { const struct hw *hw; void *dev; int mode; int lock; };
struct phy { struct apple_atcphy *a; };
static struct apple_atcphy *phy_get_drvdata(struct phy *p) { return p->a; }
static bool dp_on;
static int calls_rate, calls_other, last_rate = -1;
static bool atcphy_t8132_dp_on(struct apple_atcphy *a) { return dp_on; }
static int j700_dp_configure(struct apple_atcphy *a, struct phy_configure_opts_dp *o) { calls_other++; return 0; }
static int atcphy_dpphy_configure_t8122(struct apple_atcphy *a, struct phy_configure_opts_dp *o) { calls_other++; return 0; }
static void atcphy_restore_after_pd_off(struct apple_atcphy *a) { }
static int atcphy_dp_set_rate_t8122(struct apple_atcphy *a, u32 r) { calls_other++; return 0; }
static int atcphy_rpm_get(struct apple_atcphy *a) { return 0; }
static void atcphy_rpm_put(struct apple_atcphy *a) { }
static int atcphy_dp_configure(struct apple_atcphy *a, enum atcphy_dp_link_rate lr) { calls_rate++; last_rate = lr; return 0; }
#define guard(x) (void)
#define dev_dbg(...) do { } while (0)
#define dev_err(...) do { } while (0)
''' + configure + r'''
int main(void)
{
	static const struct hw t8103 = { ATCPHY_GENERATION_T8103 };
	struct apple_atcphy a = { .hw = &t8103, .mode = APPLE_ATCPHY_MODE_DP };
	struct phy p = { &a };
	unsigned cases = 0;

	for (unsigned v = 0; v < 2; v++)
	for (unsigned r = 0; r < 2; r++)
	for (unsigned l = 0; l < 2; l++) {
		union phy_configure_opts o = { .dp = { .set_voltages = v, .set_rate = r, .set_lanes = l,
						       .link_rate = 5400, .voltage = { 2 }, .pre = { 1 } } };
		int ret;

		calls_rate = calls_other = 0;
		ret = atcphy_dpphy_configure(&p, &o);
		if (v && !r && !l)
			/* A drive-only request: accepted, the PHY is not touched. */
			assert(ret == 0 && !calls_rate && !calls_other);
		else if (v || l)
			/* Drive levels with a rate or lanes, or lanes at all: refused, nothing done. */
			assert(ret == -EINVAL && !calls_rate && !calls_other);
		else if (r)
			assert(ret == 0 && calls_rate == 1 && last_rate == ATCPHY_DP_LINK_RATE_HBR2);
		else
			assert(ret == 0 && !calls_rate && !calls_other);
		cases++;
	}
	/* An unknown rate is still refused. */
	union phy_configure_opts bad = { .dp = { .set_rate = true, .link_rate = 4321 } };
	assert(atcphy_dpphy_configure(&p, &bad) == -EINVAL);

	/* T8132 keeps its own answers: DP off is -EOPNOTSUPP, a drive-only request with DP on is 0. */
	static const struct hw t8132 = { ATCPHY_GENERATION_T8132 };
	a.hw = &t8132;
	union phy_configure_opts drv = { .dp = { .set_voltages = true } };
	dp_on = false;
	assert(atcphy_dpphy_configure(&p, &drv) == -EOPNOTSUPP);
	dp_on = true;
	assert(atcphy_dpphy_configure(&p, &drv) == 0);
	/* T8122-class PHYs go to their own handler. */
	static const struct hw t8122 = { ATCPHY_GENERATION_T8122, .dp_t8122 = true };
	a.hw = &t8122;
	calls_other = 0;
	assert(atcphy_dpphy_configure(&p, &drv) == 0 && calls_other == 1);
	printf("%u cases\n", cases);
	return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    src.write_text(program)
    exe = src.with_suffix('')
    subprocess.run(['cc', '-Werror', '-Wall', '-Wno-unused-function', '-Wno-unused-value',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)])
    if run.returncode:
        sys.exit(f'FAIL: the drive matrix exited {run.returncode}')
print('PASS ATC PHY without drive control accepts a drive-only DP request; firmware-27 drive call passes PHY errors back')
