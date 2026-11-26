#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the firmware-27 late display stop's RTKit check and atomic idle against a fake mailbox.

The bodies of apple_rtkit_v27_check() and apple_rtkit_v27_idle_atomic() are compiled from
drivers/soc/apple/rtkit-v27-late.inc, the RTKit constants and apple_rtkit_oslog_owed_msg()
and notification classifier from rtkit.c. The fake firmware answers the AP and IOP power-state requests and can put
system notifications or application traffic into the receive queue at any point.

Cases: before the commit a pending message refuses (kept for the disarm); after the commit
the check leaves it in the mailbox and the stop completes; traffic during the stop's wait is
either acknowledged inline or deferred in order; unexpected management, crashlog and
application messages fail the stop; no answer still times out; a full FIFO still fails.

Mutations: --consume-committed (the committed check consumes and refuses as before),
--fail-foreign (the stop fails on any foreign message as before).
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--out')
ap.add_argument('--consume-committed', action='store_true')
ap.add_argument('--fail-foreign', action='store_true')
args = ap.parse_args()

root = Path(__file__).resolve().parents[2]
inc = (root / 'drivers/soc/apple/rtkit-v27-late.inc').read_text()
rtkit = (root / 'drivers/soc/apple/rtkit.c').read_text()


def function(src, sig):
    a = src.index(sig)
    a = src.rfind('\n', 0, a) + 1
    b = src.index('\n}', src.index('{', a)) + 2
    return src[a:b]


def scope_struct():
    a = inc.index('struct apple_rtkit_v27_scope {')
    return inc[a:inc.index('};', a) + 2]


consts = [line for line in (root / "drivers/soc/apple/rtkit-internal.h").read_text().splitlines()
          if line.startswith("#define APPLE_RTKIT_APP_ENDPOINT_START ")]
for name in ['APPLE_RTKIT_PWR_STATE_IDLE', 'APPLE_RTKIT_PWR_STATE_ON', 'APPLE_RTKIT_EP_MGMT',
             'APPLE_RTKIT_EP_CRASHLOG', 'APPLE_RTKIT_EP_SYSLOG', 'APPLE_RTKIT_EP_OSLOG',
             'APPLE_RTKIT_EP_IOREPORT',
             'APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE', 'APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK',
             'APPLE_RTKIT_MGMT_SET_AP_PWR_STATE', 'APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK',
             'APPLE_RTKIT_MGMT_IDLE_NOTIFY']:
    m = re.search(r'\b' + name + r' = (0x[0-9a-fA-F]+|[0-9]+),', rtkit)
    consts.append(f'#define {name} {m.group(1)}')
for name in ['APPLE_RTKIT_MGMT_TYPE', 'APPLE_RTKIT_MGMT_PWR_STATE', 'APPLE_RTKIT_SYSLOG_TYPE',
             'APPLE_RTKIT_SYSLOG_LOG', 'APPLE_RTKIT_OSLOG_TYPE', 'APPLE_RTKIT_OSLOG_LOG',
             'APPLE_RTKIT_IOREPORT_UNKNOWN1', 'APPLE_RTKIT_IOREPORT_UNKNOWN2']:
    m = re.search(r'#define ' + name + r' (.+)', rtkit)
    consts.append(f'#define {name} {m.group(1)}')

check = function(inc, 'int apple_rtkit_v27_check(')
idle = function(inc, 'int apple_rtkit_v27_idle_atomic(')
if args.consume_committed:
    old = '\tif (committed)\n\t\treturn 0;\n'
    assert check.count(old) == 1
    check = check.replace(old, '')
if args.fail_foreign:
    old = ('\t\t\t\t\t/* Other system messages are replayed on disarm. */\n'
           '\t\t\t\t\tkfifo_put(&s->deferred, msg);\n')
    assert idle.count(old) == 1
    idle = idle.replace(old, old + '\t\t\t\t\tret = -EPROTO;\n'
                                   '\t\t\t\t\tgoto poison;\n')

pre = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_PREP(m, v) ((((u64)(v)) << __builtin_ctzll(m)) & (m))
#define FIELD_GET(m, v) ((((u64)(v)) & (m)) >> __builtin_ctzll(m))
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
#define EXPORT_SYMBOL_GPL(x)
struct module;
struct apple_mbox_msg { u64 msg0; u32 msg1; };
#define FIFO_N 64
#define DECLARE_KFIFO(n, t, sz) struct { t buf[sz]; unsigned in, out; } n
#define INIT_KFIFO(f) ((f).in = (f).out = 0)
#define kfifo_is_full(f) ((f)->in - (f)->out == FIFO_N)
#define kfifo_len(f) ((f)->in - (f)->out)
#define kfifo_put(f, v) (kfifo_is_full(f) ? 0 : ((f)->buf[(f)->in++ % FIFO_N] = (v), 1))
#define kfifo_get(f, p) ((f)->in == (f)->out ? 0 : (*(p) = (f)->buf[(f)->out++ % FIFO_N], 1))
#define QN 256
struct apple_mbox {
	bool removing, active;
	struct apple_mbox_msg rx[QN]; unsigned rx_in, rx_out;
	struct apple_mbox_msg tx[QN]; unsigned tx_n;
	bool recv_enabled;
};
struct apple_rtkit {
	struct apple_mbox *mbox;
	bool crashed, shutting_down, strict_idle_poisoned, oslog_echo, silent;
	unsigned ap_power_state, iop_power_state;
	unsigned ticks, inject_at, injected;
	struct apple_mbox_msg inject[8]; unsigned ninject;
};
'''
consts_c = '\n'.join(consts) + '\n'
fake = r'''
static bool irqs_disabled(void) { return true; }
static struct apple_rtkit *cur;
static void push(struct apple_mbox *b, u32 ep, u64 msg0)
{
	b->rx[b->rx_in++ % QN] = (struct apple_mbox_msg){ .msg0 = msg0, .msg1 = ep };
}
static void udelay(unsigned us)
{
	(void)us;
	if (++cur->ticks == cur->inject_at)
		for (unsigned i = 0; i < cur->ninject; i++, cur->injected++)
			push(cur->mbox, cur->inject[i].msg1, cur->inject[i].msg0);
}
static bool apple_rtkit_v27_rx_drained(struct apple_rtkit *rtk) { return true; }
static void apple_mbox_set_recv_enabled(struct apple_mbox *b, bool on) { b->recv_enabled = on; }
static int apple_mbox_recv_nowait(struct apple_mbox *b, struct apple_mbox_msg *m)
{
	if (b->removing || !b->active)
		return -ENODEV;
	if (b->rx_in == b->rx_out)
		return 0;
	*m = b->rx[b->rx_out++ % QN];
	return 1;
}
/* The firmware: answers each power-state request, IOP idle followed by its notice. */
static int apple_mbox_send_nowait(struct apple_mbox *b, struct apple_mbox_msg m)
{
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, m.msg0);
	u64 st = FIELD_GET(APPLE_RTKIT_MGMT_PWR_STATE, m.msg0);

	b->tx[b->tx_n++] = m;
	if (cur->silent || m.msg1 != APPLE_RTKIT_EP_MGMT)
		return 0;
	if (type == APPLE_RTKIT_MGMT_SET_AP_PWR_STATE)
		push(b, 0, FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK) | st);
	if (type == APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE) {
		push(b, 0, FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK) | st);
		push(b, 0, FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_IDLE_NOTIFY));
	}
	return 0;
}
'''
owed = function(rtkit, 'static bool apple_rtkit_oslog_owed_msg(') + '\n' + \
    function(rtkit, 'static bool apple_rtkit_notification_msg(')

main = r'''
static struct apple_mbox mb;
static struct apple_rtkit rk;
static struct apple_rtkit_v27_scope sc;
static void reset(void)
{
	memset(&mb, 0, sizeof(mb)); memset(&rk, 0, sizeof(rk)); memset(&sc, 0, sizeof(sc));
	mb.active = mb.recv_enabled = true;
	rk.mbox = &mb; rk.ap_power_state = rk.iop_power_state = APPLE_RTKIT_PWR_STATE_ON;
	sc.rtk = &rk; sc.held = true; INIT_KFIFO(sc.deferred);
	cur = &rk;
}
#define SYSLOG_LINE (FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG) | 0x12)
#define APP_MSG 0x123456789aULL
static void stopped_ok(void)
{
	assert(sc.stopped && !sc.failed && !rk.strict_idle_poisoned && !mb.recv_enabled);
	assert(rk.ap_power_state == APPLE_RTKIT_PWR_STATE_IDLE);
	assert(rk.iop_power_state == APPLE_RTKIT_PWR_STATE_IDLE);
}
int main(void)
{
	struct apple_mbox_msg m;

	/* Before the commit: a pending log line refuses the sleep and is kept for the disarm. */
	reset(); push(&mb, APPLE_RTKIT_EP_SYSLOG, SYSLOG_LINE);
	assert(apple_rtkit_v27_check(&sc, false) == -EAGAIN);
	assert(kfifo_len(&sc.deferred) == 1 && mb.rx_in == mb.rx_out);
	reset();
	assert(apple_rtkit_v27_check(&sc, false) == 0 && apple_rtkit_v27_check(&sc, true) == 0);

	/* After the commit: the log line stays in the mailbox and the stop answers it. */
	reset(); push(&mb, APPLE_RTKIT_EP_SYSLOG, SYSLOG_LINE);
	assert(apple_rtkit_v27_check(&sc, true) == 0);
	assert(kfifo_len(&sc.deferred) == 0 && mb.rx_in != mb.rx_out);
	assert(apple_rtkit_v27_idle_atomic(&sc) == 0);
	stopped_ok();
	assert(!kfifo_get(&sc.deferred, &m));
	{
		unsigned echoes = 0;
		for (unsigned i = 0; i < mb.tx_n; i++)
			echoes += mb.tx[i].msg1 == APPLE_RTKIT_EP_SYSLOG &&
				  mb.tx[i].msg0 == SYSLOG_LINE;
		assert(echoes == 1);
	}

	/* Other system messages are retained in FIFO order. */
	reset(); rk.inject_at = 1; rk.ninject = 2;
	rk.inject[0] = (struct apple_mbox_msg){ .msg0 = 0, .msg1 = APPLE_RTKIT_EP_SYSLOG };
	rk.inject[1] = (struct apple_mbox_msg){ .msg0 = 0, .msg1 = APPLE_RTKIT_EP_IOREPORT };
	assert(apple_rtkit_v27_idle_atomic(&sc) == 0);
	stopped_ok(); assert(rk.injected == 2);
	assert(kfifo_get(&sc.deferred, &m) && m.msg1 == APPLE_RTKIT_EP_SYSLOG);
	assert(kfifo_get(&sc.deferred, &m) && m.msg1 == APPLE_RTKIT_EP_IOREPORT);
	assert(!kfifo_get(&sc.deferred, &m));

	/* Application RPCs invalidate the stop. */
	reset(); push(&mb, APPLE_RTKIT_APP_ENDPOINT_START, APP_MSG);
	assert(apple_rtkit_v27_idle_atomic(&sc) == -EPROTO);
	assert(sc.failed && !sc.stopped && rk.strict_idle_poisoned);

	/* An owed OSLOG counter is still echoed, not deferred. */
	reset(); rk.oslog_echo = true;
	push(&mb, APPLE_RTKIT_EP_OSLOG, FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE, APPLE_RTKIT_OSLOG_LOG) | 7);
	assert(apple_rtkit_v27_idle_atomic(&sc) == 0);
	stopped_ok(); assert(kfifo_len(&sc.deferred) == 0);
	{
		bool echoed = false;
		for (unsigned i = 0; i < mb.tx_n; i++)
			echoed |= mb.tx[i].msg1 == APPLE_RTKIT_EP_OSLOG;
		assert(echoed);
	}

	/* A management message off the handshake, or a crashlog: still no stop. */
	reset(); push(&mb, APPLE_RTKIT_EP_MGMT,
		FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK) |
		APPLE_RTKIT_PWR_STATE_ON);
	assert(apple_rtkit_v27_idle_atomic(&sc) == -EPROTO);
	assert(sc.failed && !sc.stopped && rk.strict_idle_poisoned && kfifo_len(&sc.deferred) == 1);
	reset(); push(&mb, APPLE_RTKIT_EP_CRASHLOG, 0x1ULL << 52);
	assert(apple_rtkit_v27_idle_atomic(&sc) == -EPROTO);
	assert(sc.failed && !sc.stopped && rk.strict_idle_poisoned);

	/* No answer: the 100 ms budget still ends in a failure. */
	reset(); rk.silent = true;
	assert(apple_rtkit_v27_idle_atomic(&sc) == -ETIMEDOUT);
	assert(sc.failed && !sc.stopped && rk.strict_idle_poisoned && mb.recv_enabled);

	/* A full FIFO: nothing more is consumed, the stop fails. */
	reset();
	for (unsigned i = 0; i < FIFO_N; i++)
		push(&mb, APPLE_RTKIT_EP_SYSLOG, 0);
	assert(apple_rtkit_v27_idle_atomic(&sc) == -ENOBUFS);
	assert(sc.failed && !sc.stopped && kfifo_is_full(&sc.deferred));
	assert(apple_rtkit_v27_check(&sc, true) == -EBUSY);
	return 0;
}
'''

code = pre + consts_c + scope_struct() + '\n' + fake + owed + '\n' + check + '\n' + idle + '\n' + main


def build_and_run(out):
    out.mkdir(parents=True, exist_ok=True)
    c = out / 'rtkit-v27-late.c'
    c.write_text(code)
    exe = out / 'rtkit-v27-late'
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)]).returncode


if args.out:
    rc = build_and_run(Path(args.out))
else:
    with tempfile.TemporaryDirectory() as t:
        rc = build_and_run(Path(t))
if rc:
    sys.exit(1)
print('PASS rtkit v27 late stop: committed check leaves a message to the stop, '
      'system traffic deferred in order, notification and OSLOG echoes, '
      'MGMT/crashlog/application/timeout/full FIFO fail')
