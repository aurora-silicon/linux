#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check Apple SoC Watchdog probe, ops, PM, sleep handoff and T6050 wake WDT helpers.

The full production driver body from drivers/watchdog/apple_wdt.c (ops, probe,
suspend, resume, and sleep restore) and the T6050 commit/wake WDT helpers from
drivers/soc/apple/sleep-t6050.h are compiled unchanged under ASan and UBSan
against an in-memory MMIO register model with write-order and barrier tracking.

- Without a "reconfig" MMIO resource (legacy wdt-version 2): probe adopts
  WD1_CTRL RESET_EN, start/stop/ping/set_timeout/get_timeleft/suspend/resume/
  restart drive WD1 unchanged and leave WD2 untouched.
- With a "reconfig" MMIO resource (simple-reconfig-wdog-support):
  - Probe rejects undersized (< 4 B) reconfig resources with -EINVAL.
  - Probe registers the sleep handoff before touching watchdog registers so a
    registration failure (-EBUSY) writes zero registers, and devm unregisters
    the watchdog before unregistering the sleep handoff.
  - Probe clears any pre-armed WD1_CTRL and unifies pre-probe takeover: any
    armed WD1 (RESET_EN) or WD2 (IRQ_EN or RESET_EN) sets WDOG_HW_RUNNING
    before the first WD2 write and re-arms WD2 safely through apple_wdt_wd2_arm().
  - start/stop/ping/set_timeout/get_timeleft/restart drive WD2 bark-IRQ with
    clock-derived reboot delay, write-order and readl() barrier checks, and
    underflow guard.
  - resume without apple_sleep reprograms reconfig, WD2_BITE and WD2_BARK even
    if register state was lost across suspend.
  - suspend/ping/resume during firmware-driven apple_sleep leave sleep's WD2
    safety net intact only for the device owning the sleep slot; non-matching
    devices keep operating normally.
  - kdump probe skips takeover (both WD1 and WD2) and arms WD2 at max_hw_heartbeat_ms.
  - T6050 sleep commit/wake WDT helpers save wdt-aux + AON_WDT_SLEEP before
    t6050_seq(T6050_SEQ_WDT) and restore it on wake after CTL=0, W1C IRQ_STATUS
    clear, and a readl() barrier, followed by a readl() barrier before re-arming.

--old-ref REV compiles and runs apple_wdt.c from REV and expects failure.
--mutation NAME mutates the driver/helper source and expects failure.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/watchdog/apple_wdt.c'
SLEEP_T6050 = 'drivers/soc/apple/sleep-t6050.h'


def show(ref, path):
    if not ref:
        return (ROOT / path).read_text()
    return subprocess.run(['git', '-C', str(ROOT), 'show', f'{ref}:{path}'], check=True,
                          capture_output=True, text=True).stdout


def extract_body(source):
    start = source.index('#define APPLE_WDT_WD0_CUR_TIME')
    end = source.index('static DEFINE_SIMPLE_DEV_PM_OPS')
    return source[start:end]


def extract_t6050_wdt_helpers(source):
    start = source.index('static void t6050_sleep_commit_wdt(')
    end = source.index('/*\n * The commit window,')
    return source[start:end]


def apply_mutation(body, t6050_helpers, mutation):
    if not mutation:
        return body, t6050_helpers
    if mutation == 'ignore-sleep-handoff':
        needle = 'if (wdt->reconfig && apple_sleep_wdt_active(wdd->parent))\n\t\treturn 0;'
        if needle not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        return body.replace(needle, 'if (0)\n\t\treturn 0;'), t6050_helpers
    if mutation == 'no-bark-in-start':
        needle = '\twritel_relaxed(bark_ticks, wdt->regs + APPLE_WDT_WD2_BARK_TIME);'
        if needle not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        return body.replace(needle, '\t/* omitted BARK write */'), t6050_helpers
    if mutation == 'no-status-clear':
        needle = '\twritel_relaxed(APPLE_WDT_CTRL_IRQ_STATUS,\n\t\t       wdt->regs + APPLE_WDT_WD2_CTRL);'
        if needle not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        return body.replace(needle, '\t/* omitted IRQ_STATUS clear */'), t6050_helpers
    if mutation == 'reconfig-before-bark':
        old = ('\twritel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CTRL);\n'
               '\twritel_relaxed(U32_MAX, wdt->regs + APPLE_WDT_WD2_BITE_TIME);\n'
               '\twritel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CUR_TIME);\n'
               '\twritel_relaxed(bark_ticks, wdt->regs + APPLE_WDT_WD2_BARK_TIME);\n'
               '\twritel_relaxed(APPLE_WDT_CTRL_IRQ_STATUS,\n'
               '\t\t       wdt->regs + APPLE_WDT_WD2_CTRL);\n'
               '\t(void)readl(wdt->regs + APPLE_WDT_WD2_CTRL);\n'
               '\tapple_wdt_enable_reconfig(wdt);')
        if old not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        new = ('\tapple_wdt_enable_reconfig(wdt);\n'
               '\twritel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CTRL);\n'
               '\twritel_relaxed(U32_MAX, wdt->regs + APPLE_WDT_WD2_BITE_TIME);\n'
               '\twritel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CUR_TIME);\n'
               '\twritel_relaxed(bark_ticks, wdt->regs + APPLE_WDT_WD2_BARK_TIME);\n'
               '\twritel_relaxed(APPLE_WDT_CTRL_IRQ_STATUS,\n'
               '\t\t       wdt->regs + APPLE_WDT_WD2_CTRL);\n'
               '\t(void)readl(wdt->regs + APPLE_WDT_WD2_CTRL);')
        return body.replace(old, new), t6050_helpers
    if mutation == 'register-after-arm':
        reg_block = ('\t\tret = apple_sleep_wdt_register(dev, wdt_res,\n'
                     '\t\t\t\t\t       apple_wdt_sleep_restore, wdt);\n'
                     '\t\tif (ret)\n'
                     '\t\t\treturn ret;\n'
                     '\t\tret = devm_add_action_or_reset(dev, apple_wdt_sleep_unregister, dev);\n'
                     '\t\tif (ret)\n'
                     '\t\t\treturn ret;\n')
        tail = '\treturn devm_watchdog_register_device(dev, &wdt->wdd);'
        if reg_block not in body or tail not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        mut = body.replace(reg_block, '')
        mut = mut.replace(tail,
                          '\tret = devm_watchdog_register_device(dev, &wdt->wdd);\n'
                          '\tif (ret)\n\t\treturn ret;\n'
                          '\tif (wdt->reconfig) {\n' + reg_block + '\t}\n\treturn 0;')
        return mut, t6050_helpers
    if mutation == 'hwrunning-after-arm':
        old = ('\t\t\tset_bit(WDOG_HW_RUNNING, &wdt->wdd.status);\n'
               '\t\t\tapple_wdt_wd2_arm(wdt,\n'
               '\t\t\t\t\t  apple_wdt_timeout_ticks(wdt, wdt->wdd.timeout));')
        if old not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        new = ('\t\t\tapple_wdt_wd2_arm(wdt,\n'
               '\t\t\t\t\t  apple_wdt_timeout_ticks(wdt, wdt->wdd.timeout));\n'
               '\t\t\tset_bit(WDOG_HW_RUNNING, &wdt->wdd.status);')
        return body.replace(old, new), t6050_helpers
    if mutation == 'no-wd1-adopt':
        old = ('\t\tarmed = (wd1_ctrl & APPLE_WDT_CTRL_RESET_EN) ||\n'
               '\t\t\t(wd2_ctrl & (APPLE_WDT_CTRL_IRQ_EN | APPLE_WDT_CTRL_RESET_EN));')
        if old not in body:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        new = '\t\tarmed = !!(wd2_ctrl & (APPLE_WDT_CTRL_IRQ_EN | APPLE_WDT_CTRL_RESET_EN));'
        return body.replace(old, new), t6050_helpers
    if mutation == 'no-t6050-wake-restore':
        old = ('\tif (s->aon_wdt_marked) {\n'
               '\t\twritel(s->aon_wdt_saved, s->wdt_aux + AON_WDT_SLEEP);\n'
               '\t\t(void)readl(s->wdt_aux + AON_WDT_SLEEP);\n'
               '\t\ts->aon_wdt_marked = false;\n'
               '\t}')
        if old not in t6050_helpers:
            raise SystemExit(f'mutation {mutation}: target pattern not found')
        return body, t6050_helpers.replace(old, '\t/* omitted wake restore */')
    raise SystemExit(f'unknown mutation: {mutation}')


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef uintptr_t resource_size_t;

#define BIT(x) (1u << (x))
#define GENMASK(h, l) (((~0u) << (l)) & (~0u >> (31 - (h))))
#define U32_MAX 0xffffffffu
#define MSEC_PER_SEC 1000u
#define __iomem
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define IORESOURCE_MEM 0x00000200u
#define WATCHDOG_NOWAYOUT 0
#define WDIOF_MAGICCLOSE 0x0100u
#define WDIOF_KEEPALIVEPING 0x8000u
#define WDIOF_SETTIMEOUT 0x0080u

#define WDOG_ACTIVE 0
#define WDOG_HW_RUNNING 1

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return dividend / divisor;
}

static inline void set_bit(unsigned int nr, volatile unsigned long *addr)
{
	*addr |= (1UL << nr);
}

static inline bool test_bit(unsigned int nr, const volatile unsigned long *addr)
{
	return (*addr & (1UL << nr)) != 0;
}

static inline bool IS_ERR(const void *ptr)
{
	return (uintptr_t)ptr >= (uintptr_t)-4095L;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long)(intptr_t)ptr;
}

struct device {
	void *drvdata;
};

struct platform_device {
	struct device dev;
};

struct resource {
	resource_size_t start;
	resource_size_t end;
};

static inline resource_size_t resource_size(const struct resource *res)
{
	return res->end - res->start + 1;
}

struct clk {
	unsigned long rate;
};

struct watchdog_device;

struct watchdog_ops {
	void *owner;
	int (*start)(struct watchdog_device *);
	int (*stop)(struct watchdog_device *);
	int (*ping)(struct watchdog_device *);
	int (*set_timeout)(struct watchdog_device *, unsigned int);
	unsigned int (*get_timeleft)(struct watchdog_device *);
	int (*restart)(struct watchdog_device *, unsigned long, void *);
};

struct watchdog_info {
	const char *identity;
	unsigned int options;
};

struct watchdog_device {
	struct device *parent;
	const struct watchdog_ops *ops;
	const struct watchdog_info *info;
	unsigned int timeout;
	unsigned int max_hw_heartbeat_ms;
	unsigned long status;
};

static inline bool watchdog_active(struct watchdog_device *wdd)
{
	return test_bit(WDOG_ACTIVE, &wdd->status);
}

static inline bool watchdog_hw_running(struct watchdog_device *wdd)
{
	return test_bit(WDOG_HW_RUNNING, &wdd->status);
}

/* Mock hardware + write log */
#define MMIO_WORDS 16
static u32 mmio_regs[MMIO_WORDS];
static u32 wdt_aux_regs[MMIO_WORDS];
#define reconfig_reg (wdt_aux_regs[0x2c / 4])

struct mmio_event {
	bool is_reconfig;
	u32 off;
	u32 val;
};

static struct mmio_event write_log[256];
static unsigned int write_log_len;

static unsigned int mdelay_total_ms;
static bool mock_has_reconfig;
static resource_size_t mock_reconfig_size = 4;
static struct resource mock_wdt_res;
static struct resource mock_reconfig_res;
static struct clk mock_clk = { .rate = 24000000UL };
static bool mock_kdump;
static unsigned int mock_warns;
static unsigned int mock_infos;
static unsigned int mock_dt_timeout;
static bool wdt_ctrl_read_since_write;
static bool reconfig_read_since_write;
static struct watchdog_device *registered_wdd;
static struct watchdog_device *current_probe_wdd;
static bool expect_hw_running_before_wd2_write;
static bool mock_sleep_active;
static int mock_sleep_reg_ret;
static struct device *mock_sleep_owner;
static const struct resource *mock_sleep_reg_res;
static void (*mock_sleep_restore)(void *);
static void *mock_sleep_data;
static void *mock_devm_alloc;

/* Devm LIFO stack to verify teardown order (watchdog unregister before sleep unregister) */
enum devm_kind { DEVM_ACTION = 1, DEVM_WDT = 2 };
struct devm_entry {
	enum devm_kind kind;
	void (*action)(void *);
	void *data;
};
static struct devm_entry devm_stack[16];
static unsigned int devm_stack_len;
static enum devm_kind teardown_order[16];
static unsigned int teardown_order_len;

static void record_write(const volatile void *addr, u32 val)
{
	assert(write_log_len < 256);
	if (addr == (const volatile void *)&reconfig_reg) {
		if ((val & GENMASK(5, 4)) == BIT(4)) {
			/*
			 * Routing WD2 bark to cold reset must never occur while
			 * IRQ_EN is set, IRQ_STATUS is latched, BITE is short,
			 * CUR >= BARK, or without a readl() barrier on the
			 * separate watchdog register block first.
			 */
			assert((mmio_regs[0x2c / 4] & (BIT(0) | BIT(1))) == 0);
			assert(mmio_regs[0x24 / 4] == U32_MAX);
			assert(mmio_regs[0x20 / 4] < mmio_regs[0x28 / 4]);
			assert(wdt_ctrl_read_since_write);
		}
		reconfig_read_since_write = false;
		write_log[write_log_len++] = (struct mmio_event){ true, 0x2c, val };
	} else if (addr >= (const volatile void *)wdt_aux_regs &&
		   addr < (const volatile void *)(wdt_aux_regs + MMIO_WORDS)) {
		uintptr_t base = (uintptr_t)wdt_aux_regs;
		uintptr_t p = (uintptr_t)addr;
		write_log[write_log_len++] = (struct mmio_event){ true, (u32)(p - base), val };
	} else {
		uintptr_t base = (uintptr_t)mmio_regs;
		uintptr_t p = (uintptr_t)addr;
		u32 off = (u32)(p - base);
		assert(p >= base && p < base + sizeof(mmio_regs));
		if (expect_hw_running_before_wd2_write && off >= 0x20 && off <= 0x2c)
			assert(current_probe_wdd && watchdog_hw_running(current_probe_wdd));
		/* Enabling WD2 IRQ_EN under simple-reconfig requires a readl(reconfig) first */
		if (off == 0x2c && (val & BIT(0)) && (reconfig_reg & GENMASK(5, 4)) == BIT(4))
			assert(reconfig_read_since_write);
		wdt_ctrl_read_since_write = false;
		write_log[write_log_len++] = (struct mmio_event){ false, off, val };
	}
}

static u32 readl_relaxed(const volatile void *addr)
{
	return *(const volatile u32 *)addr;
}

static u32 readl(const volatile void *addr)
{
	if (addr == (const volatile void *)&mmio_regs[0x2c / 4])
		wdt_ctrl_read_since_write = true;
	if (addr == (const volatile void *)&reconfig_reg)
		reconfig_read_since_write = true;
	return *(const volatile u32 *)addr;
}

static void writel_relaxed(u32 val, volatile void *addr)
{
	if (addr == (volatile void *)&mmio_regs[0x2c / 4]) {
		/* Model W1C semantics on APPLE_WDT_CTRL_IRQ_STATUS (BIT(1)) */
		u32 latched = mmio_regs[0x2c / 4] & BIT(1);
		if (val & BIT(1))
			latched = 0;
		mmio_regs[0x2c / 4] = (val & ~BIT(1)) | latched;
	} else {
		*(volatile u32 *)addr = val;
	}
	record_write(addr, val);
}

static void writel(u32 val, volatile void *addr)
{
	writel_relaxed(val, addr);
}

static void mdelay(unsigned int ms)
{
	mdelay_total_ms += ms;
}

static void *devm_kzalloc(struct device *dev, size_t size, int gfp)
{
	assert(!mock_devm_alloc);
	mock_devm_alloc = calloc(1, size);
	return mock_devm_alloc;
}

static void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev,
						    unsigned int index)
{
	assert(index == 0);
	return (void __iomem *)mmio_regs;
}

static void __iomem *devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
							    unsigned int index,
							    struct resource **res)
{
	assert(index == 0);
	mock_wdt_res.start = 0x28836c000ULL;
	mock_wdt_res.end = 0x28836c000ULL + 0x4000 - 1;
	if (res)
		*res = &mock_wdt_res;
	return (void __iomem *)mmio_regs;
}

static struct resource *platform_get_resource_byname(struct platform_device *pdev,
						     unsigned int type, const char *name)
{
	assert(type == IORESOURCE_MEM);
	if (mock_has_reconfig && strcmp(name, "reconfig") == 0) {
		mock_reconfig_res.start = (resource_size_t)(uintptr_t)&reconfig_reg;
		mock_reconfig_res.end = mock_reconfig_res.start + mock_reconfig_size - 1;
		return &mock_reconfig_res;
	}
	return NULL;
}

static void __iomem *devm_ioremap(struct device *dev, resource_size_t offset,
				  resource_size_t size)
{
	return (void __iomem *)(uintptr_t)offset;
}

static struct clk *devm_clk_get_enabled(struct device *dev, const char *id)
{
	return &mock_clk;
}

static unsigned long clk_get_rate(struct clk *clk)
{
	return clk->rate;
}

static void platform_set_drvdata(struct platform_device *pdev, void *data)
{
	pdev->dev.drvdata = data;
}

static void *dev_get_drvdata(struct device *dev)
{
	return dev->drvdata;
}

static bool is_kdump_kernel(void)
{
	return mock_kdump;
}

static void watchdog_set_nowayout(struct watchdog_device *wdd, bool nowayout) {}
static int watchdog_init_timeout(struct watchdog_device *wdd, unsigned int t, struct device *dev)
{
	current_probe_wdd = wdd;
	if (mock_dt_timeout)
		wdd->timeout = mock_dt_timeout;
	return 0;
}
static void watchdog_stop_on_unregister(struct watchdog_device *wdd) {}
static void watchdog_set_restart_priority(struct watchdog_device *wdd, int prio) {}

static int devm_watchdog_register_device(struct device *dev, struct watchdog_device *wdd)
{
	registered_wdd = wdd;
	assert(devm_stack_len < 16);
	devm_stack[devm_stack_len++] = (struct devm_entry){ DEVM_WDT, NULL, wdd };
	return 0;
}

static int devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data)
{
	assert(devm_stack_len < 16);
	devm_stack[devm_stack_len++] = (struct devm_entry){ DEVM_ACTION, action, data };
	return 0;
}

static int apple_sleep_wdt_register(struct device *dev, const struct resource *wdt,
				    void (*restore)(void *), void *data)
{
	if (mock_sleep_reg_ret)
		return mock_sleep_reg_ret;
	mock_sleep_owner = dev;
	mock_sleep_reg_res = wdt;
	mock_sleep_restore = restore;
	mock_sleep_data = data;
	return 0;
}

static void apple_sleep_wdt_unregister(struct device *dev)
{
	if (mock_sleep_owner == dev) {
		mock_sleep_owner = NULL;
		mock_sleep_reg_res = NULL;
		mock_sleep_restore = NULL;
		mock_sleep_data = NULL;
	}
}

static bool apple_sleep_wdt_active(struct device *dev)
{
	return mock_sleep_active && mock_sleep_owner == dev;
}

#define dev_warn(dev, fmt, ...) do { mock_warns++; } while (0)
#define dev_info(dev, fmt, ...) do { mock_infos++; } while (0)
'''

T6050_PRELUDE = r'''
/* Minimal harness for T6050 sleep commit/wake WDT helpers extracted from sleep-t6050.h */
#define WDT_WD0_CTL		0x0c
#define WDT_WD2_COUNT		0x20
#define WDT_WD2_ALARM		0x28
#define WDT_WD2_CTL		0x2c
#define WDT_WD2_CTL_IRQ_STATUS	BIT(1)
#define WDT_TICKS_PER_S		24000000u
#define WDT_SUSPEND_SECS	120u
#define T6050_WDT_COMMIT_SECS	13u
#define AON_AP_DEADLINE		0x08
#define AON_WDT_SLEEP		0x2c
#define T6050_SEQ_WDT		2
#define T6050_SEQS		6

struct t6050_ops {
	const u32 *op;
	unsigned int n;
};

struct apple_sleep_region_state {
	struct t6050_ops seq[T6050_SEQS];
};

struct apple_sleep {
	void __iomem *wdt;
	void __iomem *wdt_aux;
	struct apple_sleep_region_state *region;
	u32 aon_wdt_saved;
	bool aon_wdt_marked;
};

static bool mock_t6050_seq_saw_saved;
static u32 mock_t6050_seq_saved_val;
static bool mock_t6050_wake_armed_after_readback;

static void t6050_seq(struct apple_sleep *s, int which)
{
	assert(which == T6050_SEQ_WDT);
	if (s->region && s->region->seq[T6050_SEQ_WDT].n) {
		mock_t6050_seq_saw_saved = s->aon_wdt_marked;
		mock_t6050_seq_saved_val = s->aon_wdt_saved;
		writel(0x20u, s->wdt_aux + AON_WDT_SLEEP);
	}
}

static void apple_sleep_set_wd2(struct apple_sleep *s, unsigned int secs)
{
	if (secs == WDT_SUSPEND_SECS) {
		/* Before re-arming during wake, status must be clear and reconfig read back */
		assert((mmio_regs[0x2c / 4] & BIT(1)) == 0);
		mock_t6050_wake_armed_after_readback = reconfig_read_since_write;
	}
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
}
'''

MAIN = r'''
static void reset_hw(bool has_reconfig, unsigned long clk_rate, bool kdump)
{
	memset(mmio_regs, 0, sizeof(mmio_regs));
	memset(wdt_aux_regs, 0, sizeof(wdt_aux_regs));
	write_log_len = 0;
	mdelay_total_ms = 0;
	mock_has_reconfig = has_reconfig;
	mock_reconfig_size = 4;
	mock_clk.rate = clk_rate;
	mock_kdump = kdump;
	mock_warns = 0;
	mock_infos = 0;
	mock_dt_timeout = 0;
	wdt_ctrl_read_since_write = false;
	reconfig_read_since_write = false;
	registered_wdd = NULL;
	current_probe_wdd = NULL;
	expect_hw_running_before_wd2_write = false;
	mock_sleep_active = false;
	mock_sleep_reg_ret = 0;
	mock_sleep_owner = NULL;
	mock_sleep_reg_res = NULL;
	mock_sleep_restore = NULL;
	mock_sleep_data = NULL;
	devm_stack_len = 0;
	teardown_order_len = 0;
}

static int find_write(bool is_reconfig, u32 off, u32 val, int from)
{
	for (int i = from; i < (int)write_log_len; i++) {
		if (write_log[i].is_reconfig == is_reconfig &&
		    write_log[i].off == off &&
		    write_log[i].val == val)
			return i;
	}
	return -1;
}

static void teardown_pdev(struct platform_device *pdev)
{
	teardown_order_len = 0;
	while (devm_stack_len > 0) {
		struct devm_entry e = devm_stack[--devm_stack_len];
		teardown_order[teardown_order_len++] = e.kind;
		if (e.kind == DEVM_ACTION && e.action)
			e.action(e.data);
	}
	free(mock_devm_alloc);
	mock_devm_alloc = NULL;
	pdev->dev.drvdata = NULL;
}

int main(void)
{
	struct platform_device pdev = {};
	struct device other_dev = {};
	struct watchdog_device *wdd;

	/* 1. Legacy WD1 path (no reconfig): unchanged behaviour, WD2 untouched */
	reset_hw(false, 24000000UL, false);
	mmio_regs[0x1c / 4] = BIT(2); /* WD1_CTRL = RESET_EN armed by bootloader */
	assert(apple_wdt_probe(&pdev) == 0);
	wdd = registered_wdd;
	assert(wdd != NULL);
	assert(watchdog_hw_running(wdd));
	assert(mmio_regs[0x14 / 4] == 30u * 24000000u);
	assert(mmio_regs[0x2c / 4] == 0);

	assert(wdd->ops->start(wdd) == 0);
	assert(mmio_regs[0x1c / 4] == BIT(2));
	assert(mmio_regs[0x2c / 4] == 0);

	mmio_regs[0x10 / 4] = 10u * 24000000u;
	assert(wdd->ops->get_timeleft(wdd) == 20u);
	assert(wdd->ops->ping(wdd) == 0);
	assert(mmio_regs[0x10 / 4] == 0);

	/* Underflow guard when counter reaches or passes bite (catches old driver) */
	mmio_regs[0x10 / 4] = 35u * 24000000u;
	assert(wdd->ops->get_timeleft(wdd) == 0);

	assert(wdd->ops->stop(wdd) == 0);
	assert(mmio_regs[0x1c / 4] == 0);

	assert(wdd->ops->restart(wdd, 0, NULL) == 0);
	assert(mmio_regs[0x1c / 4] == BIT(2));
	assert(mmio_regs[0x14 / 4] == 0);
	assert(mmio_regs[0x2c / 4] == 0);
	assert(mdelay_total_ms == 150);
	teardown_pdev(&pdev);

	/* 2. Simple-reconfig WD2: undersized reconfig resource rejected with 0 writes */
	reset_hw(true, 24000000UL, false);
	mock_reconfig_size = 2;
	assert(apple_wdt_probe(&pdev) == -EINVAL);
	assert(write_log_len == 0);
	teardown_pdev(&pdev);

	/* 2b. Failed sleep handoff registration returns -EBUSY with 0 register writes */
	reset_hw(true, 24000000UL, false);
	mock_sleep_reg_ret = -EBUSY;
	mmio_regs[0x1c / 4] = BIT(2);
	mmio_regs[0x2c / 4] = BIT(0);
	assert(apple_wdt_probe(&pdev) == -EBUSY);
	assert(write_log_len == 0);
	assert(registered_wdd == NULL);
	teardown_pdev(&pdev);

	/* 3. Simple-reconfig WD2: probe clears armed WD1 and safely takes over hazardous WD2 */
	reset_hw(true, 24000000UL, false);
	mock_dt_timeout = 20; /* verify watchdog_init_timeout() runs before takeover */
	expect_hw_running_before_wd2_write = true;
	reconfig_reg = 0x000000a5u; /* unrelated bits must be preserved */
	mmio_regs[0x1c / 4] = BIT(2); /* legacy WD1 armed before probe */
	mmio_regs[0x20 / 4] = 500u; /* hazardous: CUR >= BARK */
	mmio_regs[0x24 / 4] = 1000u; /* stale short WD2_BITE */
	mmio_regs[0x28 / 4] = 100u; /* stale short WD2_BARK */
	mmio_regs[0x2c / 4] = BIT(0) | BIT(1) | BIT(2); /* IRQ_EN + latched IRQ_STATUS + RESET_EN */
	assert(apple_wdt_probe(&pdev) == 0);
	expect_hw_running_before_wd2_write = false;
	wdd = registered_wdd;
	assert(mock_sleep_reg_res == &mock_wdt_res);
	assert(mock_infos == 2); /* both legacy WD1 and WD2 RESET_EN takeover logged */
	assert(mmio_regs[0x1c / 4] == 0); /* WD1 cleared */
	assert(watchdog_hw_running(wdd));
	assert(reconfig_reg == 0x00000095u);
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	assert(mmio_regs[0x28 / 4] == 20u * 24000000u);
	assert(mmio_regs[0x20 / 4] == 0);

	/* Verify unified arming write order: CTRL=0, BITE=U32_MAX, CUR=0, BARK, CTRL=IRQ_STATUS, reconfig, CTRL=IRQ_EN */
	{
		int i_stop = find_write(false, 0x2c, 0, 0);
		int i_bite = find_write(false, 0x24, U32_MAX, i_stop);
		int i_cur = find_write(false, 0x20, 0, i_bite);
		int i_bark = find_write(false, 0x28, 20u * 24000000u, i_cur);
		int i_clr = find_write(false, 0x2c, BIT(1), i_bark);
		int i_reconfig = find_write(true, 0x2c, 0x00000095u, i_clr);
		int i_en = find_write(false, 0x2c, BIT(0), i_reconfig);
		assert(i_stop >= 0 && i_bite > i_stop && i_cur > i_bite && i_bark > i_cur);
		assert(i_clr > i_bark && i_reconfig > i_clr && i_en > i_reconfig);
	}

	/* 4. WD2 start / ping / set_timeout / get_timeleft / underflow / stop */
	write_log_len = 0;
	assert(wdd->ops->set_timeout(wdd, 15) == 0);
	assert(mmio_regs[0x28 / 4] == 15u * 24000000u);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	{
		int i_cur = find_write(false, 0x20, 0, 0);
		int i_bark = find_write(false, 0x28, 15u * 24000000u, 0);
		assert(i_cur >= 0 && i_bark > i_cur);
	}

	mmio_regs[0x20 / 4] = 5u * 24000000u;
	assert(wdd->ops->get_timeleft(wdd) == 10u);
	assert(wdd->ops->ping(wdd) == 0);
	assert(mmio_regs[0x20 / 4] == 0);
	mmio_regs[0x20 / 4] = 20u * 24000000u;
	assert(wdd->ops->get_timeleft(wdd) == 0);

	/* 5. Suspend/resume WITHOUT apple_sleep: resume reprograms lost BARK and reconfig */
	set_bit(WDOG_ACTIVE, &wdd->status);
	assert(apple_wdt_suspend(&pdev.dev) == 0);
	assert(mmio_regs[0x2c / 4] == 0);
	/* Simulate power loss clearing BARK, BITE and reconfig across suspend */
	mmio_regs[0x24 / 4] = 0;
	mmio_regs[0x28 / 4] = 0;
	reconfig_reg = 0;
	write_log_len = 0;
	assert(apple_wdt_resume(&pdev.dev) == 0);
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x28 / 4] == 15u * 24000000u);
	assert(mmio_regs[0x20 / 4] == 0);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	{
		int i_stop = find_write(false, 0x2c, 0, 0);
		int i_bite = find_write(false, 0x24, U32_MAX, i_stop);
		int i_cur = find_write(false, 0x20, 0, i_bite);
		int i_bark = find_write(false, 0x28, 15u * 24000000u, i_cur);
		int i_clr = find_write(false, 0x2c, BIT(1), i_bark);
		int i_reconfig = find_write(true, 0x2c, BIT(4), i_clr);
		int i_en = find_write(false, 0x2c, BIT(0), i_reconfig);
		assert(i_stop >= 0 && i_bite > i_stop && i_cur > i_bite && i_bark > i_cur);
		assert(i_clr > i_bark && i_reconfig > i_clr && i_en > i_reconfig);
	}

	/* 6. Suspend/resume WITH apple_sleep active: sleep owns WD2 from .begin to .end */
	assert(mock_sleep_restore != NULL);
	mock_sleep_active = true;
	/* apple_sleep_begin() armed WD2 for 120 s */
	mmio_regs[0x20 / 4] = 1000u;
	mmio_regs[0x28 / 4] = 120u * 24000000u;
	mmio_regs[0x2c / 4] = BIT(0);
	assert(apple_wdt_suspend(&pdev.dev) == 0);
	/* Must NOT stop sleep's 120 s safety net */
	assert(mmio_regs[0x2c / 4] == BIT(0));
	assert(mmio_regs[0x28 / 4] == 120u * 24000000u);
	/* Keepalive ping during sleep must NOT zero sleep's counter */
	assert(wdd->ops->ping(wdd) == 0);
	assert(mmio_regs[0x20 / 4] == 1000u);
	/* Sleep commit switched reconfig to 0x20 and wake armed 120 s */
	reconfig_reg = 0x20u;
	assert(apple_wdt_resume(&pdev.dev) == 0);
	/* Must NOT clobber sleep's resume safety net before .end */
	assert(reconfig_reg == 0x20u);
	assert(mmio_regs[0x28 / 4] == 120u * 24000000u);
	/* apple_sleep_end() clears WD2_CTL and invokes restore callback */
	mmio_regs[0x2c / 4] = 0;
	mock_sleep_restore(mock_sleep_data);
	mock_sleep_active = false;
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x28 / 4] == 15u * 24000000u);
	assert(mmio_regs[0x20 / 4] == 0);
	assert(mmio_regs[0x2c / 4] == BIT(0));

	/* 6b. Per-device active(): a non-owner device still pings/suspends during sleep */
	mock_sleep_active = true;
	mock_sleep_owner = &other_dev;
	mmio_regs[0x20 / 4] = 1000u;
	assert(wdd->ops->ping(wdd) == 0);
	assert(mmio_regs[0x20 / 4] == 0);
	assert(apple_wdt_suspend(&pdev.dev) == 0);
	assert(mmio_regs[0x2c / 4] == 0);
	assert(apple_wdt_resume(&pdev.dev) == 0);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	mock_sleep_owner = &pdev.dev;
	mock_sleep_active = false;

	/* When watchdog is stopped, sleep .end restore must leave WD2 off */
	wdd->status = 0;
	assert(wdd->ops->stop(wdd) == 0);
	mock_sleep_active = true;
	mmio_regs[0x2c / 4] = 0;
	mock_sleep_restore(mock_sleep_data);
	mock_sleep_active = false;
	assert(mmio_regs[0x2c / 4] == 0);
	/* Stopped WD2: a counter below the bark is no time left */
	mmio_regs[0x20 / 4] = 5u * 24000000u;
	assert(wdd->ops->get_timeleft(wdd) == 0);
	mmio_regs[0x20 / 4] = 0;

	/* 7. Restart uses clock-derived bark delay (40 ms) and WD2, never WD1 */
	reconfig_reg = 0;
	mdelay_total_ms = 0;
	write_log_len = 0;
	assert(wdd->ops->restart(wdd, 0, NULL) == 0);
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x28 / 4] == 24000000u * 40u / 1000u);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	assert(mmio_regs[0x1c / 4] == 0);
	assert(mdelay_total_ms >= 200);
	{
		int i_stop = find_write(false, 0x2c, 0, 0);
		int i_bite = find_write(false, 0x24, U32_MAX, i_stop);
		int i_cur = find_write(false, 0x20, 0, i_bite);
		int i_bark = find_write(false, 0x28, 24000000u * 40u / 1000u, i_cur);
		int i_clr = find_write(false, 0x2c, BIT(1), i_bark);
		int i_reconfig = find_write(true, 0x2c, BIT(4), i_clr);
		int i_en = find_write(false, 0x2c, BIT(0), i_reconfig);
		assert(i_stop >= 0 && i_bite > i_stop && i_cur > i_bite && i_bark > i_cur);
		assert(i_clr > i_bark && i_reconfig > i_clr && i_en > i_reconfig);
	}
	teardown_pdev(&pdev);
	/* Verify devm unwinds watchdog before sleep handoff */
	assert(teardown_order_len == 2);
	assert(teardown_order[0] == DEVM_WDT);
	assert(teardown_order[1] == DEVM_ACTION);
	assert(mock_sleep_restore == NULL);

	/* 7b. Pre-probe legacy WD1 alone is adopted as WD2 simple-reconfig with HW_RUNNING */
	reset_hw(true, 24000000UL, false);
	expect_hw_running_before_wd2_write = true;
	mmio_regs[0x1c / 4] = BIT(2); /* published Aura trial-boot WD1 */
	assert(apple_wdt_probe(&pdev) == 0);
	expect_hw_running_before_wd2_write = false;
	wdd = registered_wdd;
	assert(mock_infos == 1);
	assert(mmio_regs[0x1c / 4] == 0);
	assert(watchdog_hw_running(wdd));
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x28 / 4] == 30u * 24000000u);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	teardown_pdev(&pdev);

	/* 7c. Pre-probe WD2 RESET_EN alone is adopted as WD2 simple-reconfig with HW_RUNNING */
	reset_hw(true, 24000000UL, false);
	expect_hw_running_before_wd2_write = true;
	mmio_regs[0x2c / 4] = BIT(2);
	assert(apple_wdt_probe(&pdev) == 0);
	expect_hw_running_before_wd2_write = false;
	wdd = registered_wdd;
	assert(mock_infos == 1);
	assert(watchdog_hw_running(wdd));
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x2c / 4] == BIT(0));
	teardown_pdev(&pdev);

	/* 7d. Idle pre-probe with only stale latched IRQ_STATUS clears status and stays stopped */
	reset_hw(true, 24000000UL, false);
	mmio_regs[0x2c / 4] = BIT(1);
	assert(apple_wdt_probe(&pdev) == 0);
	wdd = registered_wdd;
	assert(!watchdog_hw_running(wdd));
	assert(mmio_regs[0x2c / 4] == 0);
	assert(reconfig_reg == 0);
	teardown_pdev(&pdev);

	/* 8. Restart bark delay scales with clk_rate (non-24 MHz) */
	reset_hw(true, 12000000UL, false);
	assert(apple_wdt_probe(&pdev) == 0);
	wdd = registered_wdd;
	assert(wdd->ops->restart(wdd, 0, NULL) == 0);
	assert(mmio_regs[0x28 / 4] == 12000000u * 40u / 1000u);
	teardown_pdev(&pdev);

	/* 9. Crash kernel skips takeover (WD1 and WD2) and arms WD2 at max_hw_heartbeat_ms */
	reset_hw(true, 24000000UL, true);
	mmio_regs[0x1c / 4] = BIT(2);
	mmio_regs[0x2c / 4] = BIT(0);
	assert(apple_wdt_probe(&pdev) == 0);
	wdd = registered_wdd;
	assert(!watchdog_hw_running(wdd));
	assert(mmio_regs[0x1c / 4] == 0);
	assert(reconfig_reg == BIT(4));
	assert(mmio_regs[0x24 / 4] == U32_MAX);
	assert(mmio_regs[0x28 / 4] == (U32_MAX / 24000000u) * 24000000u);
	assert(mmio_regs[0x2c / 4] == BIT(0));
	teardown_pdev(&pdev);

#ifndef SKIP_T6050_SLEEP_HELPERS
	/* 10. T6050 sleep commit/wake WDT helpers save and restore wdt-aux + AON_WDT_SLEEP */
	{
		struct apple_sleep_region_state reg = { .seq = { [T6050_SEQ_WDT] = { .n = 1 } } };
		struct apple_sleep s = {
			.wdt = (void __iomem *)mmio_regs,
			.wdt_aux = (void __iomem *)wdt_aux_regs,
			.region = &reg,
		};

		reset_hw(true, 24000000UL, false);
		reconfig_reg = 0x00000095u; /* 0x10 routing + unrelated bits */
		reconfig_read_since_write = true; /* already settled before commit */
		mmio_regs[0x24 / 4] = U32_MAX;
		mmio_regs[0x28 / 4] = 30u * 24000000u;
		mmio_regs[0x2c / 4] = BIT(0);
		mock_t6050_seq_saw_saved = false;
		mock_t6050_seq_saved_val = 0;

		t6050_sleep_commit_wdt(&s);
		assert(mock_t6050_seq_saw_saved);
		assert(mock_t6050_seq_saved_val == 0x00000095u);
		assert(s.aon_wdt_marked);
		assert(reconfig_reg == 0x20u);
		assert(mmio_regs[0x2c / 4] == BIT(0));
		assert(mmio_regs[0x28 / 4] == T6050_WDT_COMMIT_SECS * WDT_TICKS_PER_S);

		/* Wake: simulate latched IRQ_STATUS and verify CTL=0 -> clear -> readl -> restore -> readl -> set_wd2 */
		mmio_regs[0x2c / 4] = BIT(0) | BIT(1);
		write_log_len = 0;
		mock_t6050_wake_armed_after_readback = false;
		t6050_sleep_wake_wdt(&s);
		assert(!s.aon_wdt_marked);
		assert(reconfig_reg == 0x00000095u);
		assert(mock_t6050_wake_armed_after_readback);
		assert(mmio_regs[0x2c / 4] == BIT(0));
		assert(mmio_regs[0x28 / 4] == WDT_SUSPEND_SECS * WDT_TICKS_PER_S);
		{
			int i_stop = find_write(false, 0x2c, 0, 0);
			int i_clr = find_write(false, 0x2c, BIT(1), i_stop);
			int i_rest = find_write(true, 0x2c, 0x00000095u, i_clr);
			int i_arm = find_write(false, 0x2c, BIT(0), i_rest);
			assert(i_stop >= 0 && i_clr > i_stop && i_rest > i_clr && i_arm > i_rest);
		}

		/* When seq[T6050_SEQ_WDT] is empty, aon_wdt_marked stays false and wake leaves reconfig alone */
		reg.seq[T6050_SEQ_WDT].n = 0;
		reconfig_reg = 0x10u;
		reconfig_read_since_write = true;
		write_log_len = 0;
		t6050_sleep_commit_wdt(&s);
		assert(!s.aon_wdt_marked);
		write_log_len = 0;
		t6050_sleep_wake_wdt(&s);
		assert(find_write(true, 0x2c, 0x10u, 0) < 0);
		assert(reconfig_reg == 0x10u);
	}
#endif

	puts("PASS apple_wdt legacy WD1, simple-reconfig WD2, sleep handoff, and T6050 wake");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='keep build and result files here')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--old-ref', help='compile and run apple_wdt.c from this revision')
    parser.add_argument('--mutation', help='apply a named mutation and expect failure')
    args = parser.parse_args()

    source = show(args.old_ref, DRIVER)
    body = extract_body(source)
    if args.old_ref:
        t6050_part = '#define SKIP_T6050_SLEEP_HELPERS 1\n'
    else:
        t6050_src = show(None, SLEEP_T6050)
        t6050_helpers = extract_t6050_wdt_helpers(t6050_src)
        body, t6050_helpers = apply_mutation(body, t6050_helpers, args.mutation)
        t6050_part = T6050_PRELUDE + t6050_helpers

    code = PREFIX + body + t6050_part + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        cfile, exe = out / 'apple-wdt.c', out / 'apple-wdt'
        cfile.write_text(code)
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(cfile), '-o', str(exe)], check=True)
        res = subprocess.run([str(exe)], capture_output=True, text=True)
        print(res.stdout + res.stderr, end='')
        return res.returncode

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='apple-wdt-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
