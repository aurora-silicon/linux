#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production scaler interrupt and watchdog paths against a modelled engine.

A failed job (a decompress error from malformed lossless input) leaves the
modelled engine's interrupt line asserted until the engine is reset, as the
hardware does: clearing ASC_IRQ_STATUS is not enough. A timed-out job leaves
the engine running on the job's buffers. The model records the engine state
whenever a buffer is handed back, and every case checks that no buffer goes
back while the engine may still use it or while the line is still asserted,
and that the engine takes the next job afterwards: interrupts enabled, no
reset pending, the interrupt line enabled again.

With --ref the functions come from drivers/media/platform/apple/scaler of
that revision instead (the negative control).
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/media/platform/apple/scaler'
FUNCTIONS = ('asc_read', 'asc_write', 'asc_hw_init', 'asc_reset_engine', 'asc_complete',
             'asc_finish', 'asc_recover', 'asc_abort', 'asc_irq', 'asc_watchdog')
# Older revisions have no reset helper and no abort path; newer ones no asc_recover().
OPTIONAL = {'asc_reset_engine', 'asc_complete', 'asc_abort', 'asc_recover'}
CASES = ('done', 'decompress-error', 'decompress-error-running', 'timeout-idle', 'timeout',
         'timeout-stuck', 'late-error', 'spurious')

PRELUDE = r'''
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned long resource_size_t;
#define __iomem
#define BIT(n) (1U << (n))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
typedef int irqreturn_t;
enum { IRQ_NONE, IRQ_HANDLED };
typedef struct { int unused; } spinlock_t;
struct mutex { int unused; };
struct device { int unused; };
struct dev_pm_domain_list;
struct reset_control { int unused; };
struct v4l2_device { int unused; };
struct video_device { int unused; };
struct v4l2_m2m_dev { int unused; };
struct v4l2_m2m_ctx { int unused; };
struct v4l2_fh { struct v4l2_m2m_ctx *m2m_ctx; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct apple_scaler_variant { int unused; };
struct asc_reg { u32 off, val; };
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
struct vb2_buffer { unsigned int num_planes; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; u32 sequence; };

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/*
 * The engine. Its interrupt line is asserted while an error is latched or an
 * enabled status bit is set. Only an engine reset (or the PMGR reset, or
 * power-off) clears a latched error; writing the status register does not.
 * stuck: 0 the engine reset stops it, 1 only the PMGR reset, 2 nothing.
 */
static char regs[0x28000];
static struct {
	bool running, latched, powered;
	u32 status, enable;
	int stuck, resets, pmgr_resets, power_offs, irq_depth, rpm, events;
	int reset_event, pmgr_event, off_event, irq_off_event, irq_on_event;
} hw;
static bool line(void) { return hw.latched || (hw.status & hw.enable); }
static u32 readl_relaxed(const volatile void *addr)
{
	switch ((const char *)addr - regs) {
	case ASC_IS_RUNNING: return hw.running;
	case ASC_IRQ_STATUS: return hw.status;
	case ASC_IRQ_ENABLE: return hw.enable;
	case ASC_DEBUG_STATUS: return 0x1401;
	}
	return 0;
}
static void writel_relaxed(u32 v, volatile void *addr)
{
	switch ((char *)addr - regs) {
	case ASC_IRQ_STATUS: hw.status &= ~v; break;
	case ASC_IRQ_ENABLE: hw.enable = v; break;
	case ASC_RESET:
		if (v & ASC_RESET_ENGINE) {
			hw.resets++;
			hw.reset_event = ++hw.events;
			if (hw.stuck < 1)
				hw.running = false;
			if (hw.stuck < 2) {
				hw.latched = false;
				hw.status = 0;
			}
		}
		break;
	}
}
#define readl_relaxed_poll_timeout(addr, val, cond, sleep_us, timeout_us) ({ \
	int __ret = -ETIMEDOUT; \
	for (int __i = 0; __i < 100; __i++) { \
		(val) = readl_relaxed(addr); \
		if (cond) { __ret = 0; break; } \
	} \
	__ret; })
static int reset_control_reset(struct reset_control *r)
{
	(void)r;
	hw.pmgr_resets++;
	hw.pmgr_event = ++hw.events;
	if (hw.stuck < 2) {
		hw.running = false;
		hw.latched = false;
		hw.status = 0;
	}
	return 0;
}
#define spin_lock(l) ((void)(l))
#define spin_unlock(l) ((void)(l))
#define spin_lock_irqsave(l, f) ((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#define dev_err(d, ...) ((void)(d), fprintf(stderr, __VA_ARGS__))
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_err
static int pm_runtime_get_if_active(struct device *d) { (void)d; if (!hw.powered) return 0; hw.rpm++; return 1; }
static void pm_runtime_put_noidle(struct device *d) { (void)d; hw.rpm--; }
static int autosuspend_puts;
static int pm_runtime_put_autosuspend(struct device *d) { (void)d; hw.rpm--; autosuspend_puts++; return 0; }
static int pm_runtime_put_sync_suspend(struct device *d)
{
	(void)d;
	hw.rpm--;
	if (!hw.rpm) {
		hw.powered = hw.running = hw.latched = false;
		hw.status = 0;
		hw.power_offs++;
		hw.off_event = ++hw.events;
	}
	return 0;
}
static bool cancel_delayed_work(struct delayed_work *w) { (void)w; return true; }
static void disable_irq(unsigned int irq) { (void)irq; hw.irq_depth++; hw.irq_off_event = ++hw.events; }
static void enable_irq(unsigned int irq) { (void)irq; hw.irq_depth--; hw.irq_on_event = ++hw.events; }

/* Buffers: what the engine was doing when each went back. */
static struct vb2_v4l2_buffer src_buf, dst_buf;
static struct vb2_v4l2_buffer *src_q, *dst_q;
static struct { int n; enum vb2_buffer_state state; bool running, line; int event; } done[4];
static int ndone, job_finishes;
static struct vb2_v4l2_buffer *v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *m) { struct vb2_v4l2_buffer *b = src_q; (void)m; src_q = NULL; return b; }
static struct vb2_v4l2_buffer *v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *m) { struct vb2_v4l2_buffer *b = dst_q; (void)m; dst_q = NULL; return b; }
static void vb2_set_plane_payload(struct vb2_buffer *b, unsigned int p, unsigned long n) { (void)b; (void)p; (void)n; }
static void v4l2_m2m_buf_copy_metadata(struct vb2_v4l2_buffer *s, struct vb2_v4l2_buffer *d) { (void)s; (void)d; }
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *b, enum vb2_buffer_state state)
{
	(void)b;
	if (ndone < 4)
		done[ndone] = (__typeof__(done[0])){ ndone, state, hw.running, line(), ++hw.events };
	ndone++;
}
static void v4l2_m2m_job_finish(struct v4l2_m2m_dev *m, struct v4l2_m2m_ctx *c) { (void)m; (void)c; job_finishes++; }
'''

MAIN = r'''
static struct v4l2_m2m_ctx m2m;
static struct apple_scaler asc;
static struct apple_scaler_ctx ctx;
static struct reset_control rst;

/* A job in flight: the engine powered and programmed, one runtime PM reference. */
static void start(bool running, u32 status, bool latched, int stuck)
{
	memset(&hw, 0, sizeof(hw));
	memset(done, 0, sizeof(done));
	ndone = job_finishes = autosuspend_puts = 0;
	memset(&asc, 0, sizeof(asc));
	memset(&ctx, 0, sizeof(ctx));
	asc.regs = regs;
	asc.regs_size = sizeof(regs);
	asc.rst = &rst;
	asc.irq = 42;
	ctx.asc = &asc;
	ctx.fh.m2m_ctx = &m2m;
	ctx.dst.num_planes = 1;
	src_buf.vb2_buf.num_planes = dst_buf.vb2_buf.num_planes = 1;
	src_q = &src_buf;
	dst_q = &dst_buf;
	asc.active = &ctx;
	hw.powered = true;
	hw.rpm = 1;
	hw.enable = ASC_IRQ_DONE | ASC_IRQ_ERRORS;
	hw.running = running;
	hw.status = status;
	hw.latched = latched;
	hw.stuck = stuck;
}

/* Both buffers went back with @state, while nothing used them. */
static void returned(enum vb2_buffer_state state)
{
	CHECK(ndone == 2);
	CHECK(job_finishes == 1);
	for (int i = 0; i < ndone && i < 2; i++) {
		CHECK(done[i].state == state);
		CHECK(!done[i].running);
		CHECK(!done[i].line);
	}
}

/* The engine takes the next job: no reset pending, interrupts on, line quiet and enabled. */
static void ready(void)
{
	CHECK(!asc.need_init);
	CHECK(!asc.active);
	CHECK(hw.enable == (ASC_IRQ_DONE | ASC_IRQ_ERRORS));
	CHECK(!line());
	CHECK(hw.irq_depth == 0);
	CHECK(hw.rpm == 0);
	CHECK(autosuspend_puts == 1);
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";

	if (!strcmp(c, "done")) {
		start(false, ASC_IRQ_DONE, false, 0);
		CHECK(asc_irq(asc.irq, &asc) == IRQ_HANDLED);
		returned(VB2_BUF_STATE_DONE);
		ready();
		CHECK(hw.resets == 0);
	} else if (!strcmp(c, "decompress-error") || !strcmp(c, "decompress-error-running")) {
		/* Malformed lossless input: the engine reports a decompress error. */
		start(!strcmp(c, "decompress-error-running"), ASC_IRQ_DECOMPRESS_ERROR, true, 0);
		CHECK(asc_irq(asc.irq, &asc) == IRQ_HANDLED);
		returned(VB2_BUF_STATE_ERROR);
		CHECK(hw.resets >= 1);
		for (int i = 0; i < ndone && i < 2; i++)
			CHECK(hw.reset_event && hw.reset_event < done[i].event);
		ready();
	} else if (!strcmp(c, "timeout-idle")) {
		/* The completion was lost; the engine is idle: its own reset will do. */
		start(false, 0, false, 0);
		asc_watchdog(&asc.watchdog.work);
		returned(VB2_BUF_STATE_ERROR);
		CHECK(hw.pmgr_resets == 0);
		for (int i = 0; i < ndone && i < 2; i++)
			CHECK(hw.reset_event && hw.reset_event < done[i].event);
		/* No interrupt handler runs during the reset. */
		CHECK(hw.irq_off_event && hw.irq_off_event < hw.reset_event);
		CHECK(hw.irq_on_event > hw.reset_event);
		ready();
	} else if (!strcmp(c, "timeout")) {
		/* Still running on the buffers: it never goes idle for the engine reset. */
		start(true, 0, false, 1);
		asc_watchdog(&asc.watchdog.work);
		returned(VB2_BUF_STATE_ERROR);
		CHECK(hw.pmgr_resets == 1);
		for (int i = 0; i < ndone && i < 2; i++)
			CHECK(hw.pmgr_event && hw.pmgr_event < done[i].event);
		CHECK(hw.irq_off_event && hw.irq_off_event < hw.pmgr_event);
		ready();
	} else if (!strcmp(c, "timeout-stuck")) {
		/* No reset stops it: powered off before the buffers go back. */
		start(true, 0, false, 2);
		asc_watchdog(&asc.watchdog.work);
		returned(VB2_BUF_STATE_ERROR);
		CHECK(hw.power_offs == 1);
		for (int i = 0; i < ndone && i < 2; i++)
			CHECK(hw.off_event && hw.off_event < done[i].event);
		CHECK(asc.need_init);
		CHECK(hw.rpm == 0);
		CHECK(autosuspend_puts == 0);
		CHECK(hw.irq_depth == 0);
	} else if (!strcmp(c, "late-error")) {
		/* The watchdog already took the job: nothing is handed back twice. */
		start(false, ASC_IRQ_READ_ERROR, false, 0);
		asc.active = NULL;
		CHECK(asc_irq(asc.irq, &asc) == IRQ_HANDLED);
		CHECK(ndone == 0 && job_finishes == 0);
		CHECK(hw.rpm == 1);
	} else if (!strcmp(c, "spurious")) {
		start(false, 0, false, 0);
		CHECK(asc_irq(asc.irq, &asc) == IRQ_NONE);
		CHECK(ndone == 0 && hw.rpm == 1);
	} else {
		printf("unknown case %s\n", c);
		return 2;
	}
	printf("%s: %s\n", c, failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
'''


def function(text, name):
    # A definition: 'static', its type (maybe on a line of its own), the name.
    match = re.search(r'^static[^;{}()]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text, re.M)
    if not match:
        return None
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def structure(text, name):
    start = text.index('struct ' + name + ' {')
    return text[start:text.index('\n};', start) + 3]


def source(ref, name):
    if ref:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{ref}:{DRIVER}/{name}'],
                                       text=True)
    return (ROOT / DRIVER / name).read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ref', help='test the driver of this revision instead')
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    drv = source(args.ref, 'apple-scaler-drv.c')
    regs = re.sub(r'^#include.*$', '', source(args.ref, 'apple-scaler-regs.h'), flags=re.M)
    hw = source(args.ref, 'apple-scaler-hw.h')
    defines = [re.search(r'^#define ASC_JOB_MAX_WRITES\b.*$', hw, re.M).group(0),
               re.search(r'^#define ASC_IDLE_TIMEOUT_US\b.*$', drv, re.M).group(0)]
    bodies = []
    for name in FUNCTIONS:
        body = function(drv, name)
        if body is None and name not in OPTIONAL:
            print(f'FAIL line 0: {name} is missing')
            sys.exit(1)
        if body:
            bodies.append(body)
    # Every function first declared, so their order in the driver does not matter.
    prototypes = [re.match(r'[^{]*', b).group(0).rstrip() + ';' for b in bodies]
    code = (regs + PRELUDE + '\n'.join(defines) + '\n' + structure(drv, 'apple_scaler') + '\n' +
            structure(drv, 'apple_scaler_ctx') + '\n' + '\n'.join(prototypes) + '\n' +
            '\n'.join(bodies) + MAIN)

    failed = False
    with tempfile.TemporaryDirectory(prefix='scaler-recovery-') as tmp:
        src = Path(tmp) / 'production.c'
        binary = Path(tmp) / 'production'
        src.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-variable', '-fsanitize=address,undefined', str(src), '-o',
                        str(binary)], check=True)
        for case in args.case or CASES:
            run = subprocess.run([str(binary), case], capture_output=True, text=True)
            sys.stdout.write(run.stdout)
            if run.returncode:
                print(f'case {case}: exit {run.returncode}')
                failed = True
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
