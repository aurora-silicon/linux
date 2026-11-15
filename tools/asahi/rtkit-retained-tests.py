#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the retained-wake endpoint map reply and IOP power acknowledgment."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/soc/apple/rtkit.c'


def function(source, name):
    match = re.search(r'^(?:static )?(?:void|int|long) ' + name + r'\(', source, re.M)
    assert match, name
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def constants(source):
    out = []
    for line in source.splitlines():
        if re.match(r'#define APPLE_RTKIT_MGMT_(TYPE|EPMAP_\w+|PWR_STATE)\b', line):
            out.append(line)
    enum = re.search(r'enum \{\n\tAPPLE_RTKIT_MGMT_HELLO = 1,.*?\n\};', source, re.S)
    states = re.search(r'enum \{\n\tAPPLE_RTKIT_PWR_STATE_OFF.*?\n\};', source, re.S)
    assert enum and states
    return '\n'.join(out) + '\n' + enum.group(0) + '\n' + states.group(0) + '\n'


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzll(m))
#define FIELD_PREP(m,v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define APPLE_RTKIT_MAX_ENDPOINTS 256
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct apple_rtkit {
	void *dev;
	unsigned long started[APPLE_RTKIT_MAX_ENDPOINTS / 64];
	bool retained_wake, retained_on_sent;
	int boot_result;
	unsigned int iop_power_state;
	int epmap_completion, iop_pwr_ack_completion;
};
static bool test_bit(unsigned n, const unsigned long *b) { return b[n / 64] >> (n % 64) & 1; }
static void set_bit(unsigned n, unsigned long *b) { b[n / 64] |= 1UL << (n % 64); }
static void complete_all(int *c) { (*c)++; }
static unsigned sent;
static u8 sent_type;
static u64 sent_data;
static int apple_rtkit_management_send(struct apple_rtkit *rtk, u8 type, u64 data)
{
	(void)rtk; sent++; sent_type = type; sent_data = data; return 0;
}
/* A fatal signal is pending for every killable wait. */
#define ERESTARTSYS 512
#define READ_ONCE(x) (x)
enum { PM_SUSPEND_ON, PM_SUSPEND_TO_IDLE, PM_SUSPEND_STANDBY, PM_SUSPEND_MEM };
static int pm_suspend_target_state;
struct completion { int unused; };
static unsigned waited_uninterruptible, waited_killable;
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long t)
{
	(void)c; waited_uninterruptible++; return t;
}
static long wait_for_completion_killable_timeout(struct completion *c, unsigned long t)
{
	(void)c; (void)t; waited_killable++; return -ERESTARTSYS;
}
'''

MAIN = r'''
static struct apple_rtkit fresh(void)
{
	struct apple_rtkit r = { .retained_wake = true, .boot_result = -EINPROGRESS };

	/* Management, syslog, crashlog, debug, ioreport, oslog, tracekit, DCP 0x37. */
	for (unsigned ep = 0; ep < 11; ep++)
		if ((0x51dUL >> ep) & 1)
			set_bit(ep, r.started);
	set_bit(0x37, r.started);
	sent = 0;
	return r;
}

int main(void)
{
	struct apple_rtkit r = fresh();
	u64 map0 = FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE, 0) | 0x51d;
	u64 map1 = APPLE_RTKIT_MGMT_EPMAP_LAST | FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE, 1) |
		   0x00867f3f;

	/* Each part of the map: exactly the started endpoints, MORE until the last. */
	apple_rtkit_management_rx_epmap_retained(&r, map0, 0, 0x51d);
	assert(sent == 1 && sent_type == APPLE_RTKIT_MGMT_EPMAP_REPLY);
	assert(sent_data == (0x51d | APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE));
	assert(r.epmap_completion == 0 && r.boot_result == -EINPROGRESS);
	apple_rtkit_management_rx_epmap_retained(&r, map1, 1, 0x00867f3f);
	assert(sent == 2 && sent_data == (APPLE_RTKIT_MGMT_EPMAP_LAST |
					   FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE, 1) | BIT(0x37 - 32)));
	assert(r.epmap_completion == 1 && r.boot_result == 0);

	/* A started endpoint missing from the map fails the wake; nothing is sent. */
	r = fresh();
	apple_rtkit_management_rx_epmap_retained(&r, map1 & ~(u64)BIT(0x37 - 32), 1,
						 0x00867f3f & ~BIT(0x37 - 32));
	assert(sent == 0 && r.boot_result == -EPROTO && r.epmap_completion == 1);

	/* QUIESCED gets one ON request, then ON completes. */
	r = fresh();
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_QUIESCED);
	assert(sent == 1 && sent_type == APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE);
	assert(sent_data == APPLE_RTKIT_PWR_STATE_ON && r.iop_pwr_ack_completion == 0);
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_ON);
	assert(sent == 1 && r.iop_pwr_ack_completion == 1);
	assert(r.iop_power_state == APPLE_RTKIT_PWR_STATE_ON);

	/* A last part cannot echo endpoint 32 * base: bit 0 is also MORE. */
	r = fresh();
	set_bit(32, r.started);
	apple_rtkit_management_rx_epmap_retained(&r, map1, 1, 0x00867f3f);
	assert(sent == 0 && r.boot_result == -EPROTO && r.epmap_completion == 1);

	/*
	 * A second QUIESCED, or any other state, fails the wake, before the map
	 * or while the wake waits for IOP ON after it.
	 */
	r = fresh();
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_QUIESCED);
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_QUIESCED);
	assert(sent == 1 && r.boot_result == -EPROTO);
	assert(r.epmap_completion == 1 && r.iop_pwr_ack_completion == 1);
	r = fresh();
	apple_rtkit_management_rx_epmap_retained(&r, map0, 0, 0x51d);
	apple_rtkit_management_rx_epmap_retained(&r, map1, 1, 0x00867f3f);
	assert(r.epmap_completion == 1 && r.boot_result == 0);
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_IDLE);
	assert(r.boot_result == -EPROTO && r.iop_pwr_ack_completion == 1);

	/* Outside a retained wake every acknowledgment completes, as before. */
	r = fresh();
	r.retained_wake = false;
	apple_rtkit_management_rx_iop_pwr_ack(&r, APPLE_RTKIT_PWR_STATE_QUIESCED);
	assert(sent == 0 && r.iop_pwr_ack_completion == 1);
	assert(r.iop_power_state == APPLE_RTKIT_PWR_STATE_QUIESCED);

	/*
	 * A retained wake waits through a signal, and so does a wait in a system
	 * sleep; any other wait ends only for a fatal one, not the freezer's.
	 */
	struct completion c;
	r = fresh();
	assert(apple_rtkit_wait_timeout(&r, &c, 7) == 7);
	assert(waited_uninterruptible == 1 && waited_killable == 0);
	r.retained_wake = false;
	assert(apple_rtkit_wait_timeout(&r, &c, 7) == -ERESTARTSYS);
	assert(waited_uninterruptible == 1 && waited_killable == 1);
	pm_suspend_target_state = PM_SUSPEND_MEM;
	assert(apple_rtkit_wait_timeout(&r, &c, 7) == 7);
	assert(waited_uninterruptible == 2 && waited_killable == 1);
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
	assert(apple_rtkit_wait_timeout(&r, &c, 7) == -ERESTARTSYS);
	assert(waited_uninterruptible == 2 && waited_killable == 2);

	puts("PASS retained endpoint map replies, IOP acknowledgments and waits");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='keep build and result files here')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (ROOT / DRIVER).read_text()
    code = PREFIX + constants(source)
    code += function(source, 'apple_rtkit_management_rx_epmap_retained')
    code += function(source, 'apple_rtkit_management_rx_iop_pwr_ack')
    code += function(source, 'apple_rtkit_wait_timeout') + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        cfile, exe = out / 'retained.c', out / 'retained'
        cfile.write_text(code)
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cfile), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout or result.stderr, end='')
        return result.returncode

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='rtkit-retained-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
