// SPDX-License-Identifier: GPL-2.0
/*
 * CLx support
 *
 * Copyright (C) 2020 - 2023, Intel Corporation
 * Authors: Gil Fine <gil.fine@intel.com>
 *	    Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "tb.h"

static bool clx_enabled = true;
module_param_named(clx, clx_enabled, bool, 0444);
MODULE_PARM_DESC(clx, "allow low power states on the high-speed lanes (default: true)");

static const char *clx_name(unsigned int clx)
{
	switch (clx) {
	case TB_CL0S | TB_CL1 | TB_CL2:
		return "CL0s/CL1/CL2";
	case TB_CL1 | TB_CL2:
		return "CL1/CL2";
	case TB_CL0S | TB_CL2:
		return "CL0s/CL2";
	case TB_CL0S | TB_CL1:
		return "CL0s/CL1";
	case TB_CL0S:
		return "CL0s";
	case 0:
		return "disabled";
	default:
		return "unknown";
	}
}

static int tb_port_pm_secondary_set(struct tb_port *port, bool secondary)
{
	u32 phy;
	int ret;

	ret = tb_port_read(port, &phy, TB_CFG_PORT,
			   port->cap_phy + LANE_ADP_CS_1, 1);
	if (ret)
		return ret;

	if (secondary)
		phy |= LANE_ADP_CS_1_PMS;
	else
		phy &= ~LANE_ADP_CS_1_PMS;

	return tb_port_write(port, &phy, TB_CFG_PORT,
			     port->cap_phy + LANE_ADP_CS_1, 1);
}

static int tb_port_pm_secondary_enable(struct tb_port *port)
{
	return tb_port_pm_secondary_set(port, true);
}

static int tb_port_pm_secondary_disable(struct tb_port *port)
{
	return tb_port_pm_secondary_set(port, false);
}

/* Called for USB4 or Titan Ridge routers only */
static bool tb_port_clx_supported(struct tb_port *port, unsigned int clx)
{
	u32 val, mask = 0;
	bool ret;

	/* Don't enable CLx in case of two single-lane links */
	if (!port->bonded && port->dual_link_port)
		return false;

	/* Don't enable CLx in case of inter-domain link */
	if (port->xdomain)
		return false;

	if (tb_switch_is_usb4(port->sw)) {
		if (!usb4_port_clx_supported(port))
			return false;
	} else if (!tb_lc_is_clx_supported(port)) {
		return false;
	}

	if (clx & TB_CL0S)
		mask |= LANE_ADP_CS_0_CL0S_SUPPORT;
	if (clx & TB_CL1)
		mask |= LANE_ADP_CS_0_CL1_SUPPORT;
	if (clx & TB_CL2)
		mask |= LANE_ADP_CS_0_CL2_SUPPORT;

	ret = tb_port_read(port, &val, TB_CFG_PORT,
			   port->cap_phy + LANE_ADP_CS_0, 1);
	if (ret)
		return false;

	return !!(val & mask);
}

static int tb_port_clx_set(struct tb_port *port, unsigned int clx, bool enable)
{
	u32 phy, mask = 0;
	int ret;

	if (clx & TB_CL0S)
		mask |= LANE_ADP_CS_1_CL0S_ENABLE;
	if (clx & TB_CL1)
		mask |= LANE_ADP_CS_1_CL1_ENABLE;
	if (clx & TB_CL2)
		mask |= LANE_ADP_CS_1_CL2_ENABLE;

	if (!mask)
		return -EOPNOTSUPP;

	ret = tb_port_read(port, &phy, TB_CFG_PORT,
			   port->cap_phy + LANE_ADP_CS_1, 1);
	if (ret)
		return ret;

	if (enable)
		phy |= mask;
	else
		phy &= ~mask;

	return tb_port_write(port, &phy, TB_CFG_PORT,
			     port->cap_phy + LANE_ADP_CS_1, 1);
}

static int tb_port_clx_disable(struct tb_port *port, unsigned int clx)
{
	return tb_port_clx_set(port, clx, false);
}

static int tb_port_clx_enable(struct tb_port *port, unsigned int clx)
{
	return tb_port_clx_set(port, clx, true);
}

static int tb_port_clx(struct tb_port *port)
{
	u32 val;
	int ret;

	if (!tb_port_clx_supported(port, TB_CL0S | TB_CL1 | TB_CL2))
		return 0;

	ret = tb_port_read(port, &val, TB_CFG_PORT,
			   port->cap_phy + LANE_ADP_CS_1, 1);
	if (ret)
		return ret;

	if (val & LANE_ADP_CS_1_CL0S_ENABLE)
		ret |= TB_CL0S;
	if (val & LANE_ADP_CS_1_CL1_ENABLE)
		ret |= TB_CL1;
	if (val & LANE_ADP_CS_1_CL2_ENABLE)
		ret |= TB_CL2;

	return ret;
}

/**
 * tb_port_clx_is_enabled() - Is given CL state enabled
 * @port: USB4 port to check
 * @clx: Mask of CL states to check
 *
 * Return: %true if any of the given CL states is enabled for @port,
 * %false otherwise.
 */
bool tb_port_clx_is_enabled(struct tb_port *port, unsigned int clx)
{
	return !!(tb_port_clx(port) & clx);
}

/**
 * tb_switch_clx_is_supported() - Is CLx supported on this type of router
 * @sw: The router to check CLx support for
 *
 * Return: %true if CLx is supported, %false otherwise.
 */
static bool tb_switch_clx_is_supported(const struct tb_switch *sw)
{
	if (!clx_enabled)
		return false;

	if (sw->quirks & QUIRK_NO_CLX)
		return false;

	/*
	 * CLx is not enabled and validated on Intel USB4 platforms
	 * before Alder Lake.
	 */
	if (tb_switch_is_tiger_lake(sw))
		return false;

	return tb_switch_is_usb4(sw) || tb_switch_is_titan_ridge(sw);
}

/**
 * tb_switch_clx_init() - Initialize router CL states
 * @sw: Router
 *
 * Can be called for any router. Initializes the current CL state by
 * reading it from the hardware.
 *
 * Return: %0 on success, negative errno otherwise.
 */
int tb_switch_clx_init(struct tb_switch *sw)
{
	struct tb_port *up, *down;
	unsigned int clx, tmp;

	if (tb_switch_is_icm(sw))
		return 0;

	if (!tb_route(sw))
		return 0;

	if (!tb_switch_clx_is_supported(sw))
		return 0;

	up = tb_upstream_port(sw);
	down = tb_switch_downstream_port(sw);

	clx = tb_port_clx(up);
	tmp = tb_port_clx(down);
	if (clx != tmp)
		tb_sw_warn(sw, "CLx: inconsistent configuration %#x != %#x\n",
			   clx, tmp);

	tb_sw_dbg(sw, "CLx: current mode: %s\n", clx_name(clx));

	sw->clx = clx;
	return 0;
}

static int tb_switch_pm_secondary_resolve(struct tb_switch *sw)
{
	struct tb_port *up, *down;
	int ret;

	if (!tb_route(sw))
		return 0;

	up = tb_upstream_port(sw);
	down = tb_switch_downstream_port(sw);
	ret = tb_port_pm_secondary_enable(up);
	if (ret)
		return ret;

	return tb_port_pm_secondary_disable(down);
}

/**
 * tb_switch_clx_pm_roles() - Give a link its PM roles without its CL states
 * @sw: Device router whose link to its parent router is set
 *
 * TEST (T6050, J714s): the roles tb_switch_clx_enable() gives a link before it enables its
 * CL states: the router's upstream lane adapter PM secondary, the parent's lane adapter PM
 * primary. For a host whose routers stay out of CL states (%QUIRK_NO_CLX) but sleep in
 * place with the roles macOS gives them. The CL states are not touched.
 *
 * Return: %0 on success, negative errno otherwise.
 */
int tb_switch_clx_pm_roles(struct tb_switch *sw)
{
	return tb_switch_pm_secondary_resolve(sw);
}

#define TB_CLX_MACOS_ENABLES	(LANE_ADP_CS_1_CL0S_ENABLE | LANE_ADP_CS_1_CL1_ENABLE | \
				 LANE_ADP_CS_1_CL2_ENABLE)

/*
 * TEST (T6050, J714s): macOS's CL-state writes on the link from the host router to the dock
 * (runs mxt6 and mxt6b: the dock's enumeration after each of four wakes, and four sleep
 * entries), each a read-modify-write of one lane adapter's LANE_ADP_CS_1: the host's end
 * first for a CL state, the dock's first for the roles. Once the link has its PM roles,
 * macOS enables CL0s on both ends, then CL1 and CL2 on both ends, and gives the roles again.
 * Its sleep entry gives the roles, disables CL1 and CL2 on both ends, then CL0s, and gives
 * the roles once more before the display tunnels go. The host router's firmware logs its CL
 * configuration change at each of the host's four writes and at no other. The two halves
 * follow each other here, as at macOS's enumeration after its second wake (on, then off
 * again within 7 ms for a width change): the link ends with its CL states off, as macOS
 * leaves it for the sleep.
 */
static const struct tb_clx_macos_step {
	bool host;	/* the parent's lane adapter, else the router's upstream one */
	bool roles;	/* the PM role: the router's end secondary, the parent's primary */
	bool on;
	u32 bits;
} tb_clx_macos_steps[] = {
	{ .host = true, .on = true, .bits = LANE_ADP_CS_1_CL0S_ENABLE },
	{ .on = true, .bits = LANE_ADP_CS_1_CL0S_ENABLE },
	{ .host = true, .on = true, .bits = LANE_ADP_CS_1_CL1_ENABLE | LANE_ADP_CS_1_CL2_ENABLE },
	{ .on = true, .bits = LANE_ADP_CS_1_CL1_ENABLE | LANE_ADP_CS_1_CL2_ENABLE },
	{ .roles = true },
	{ .host = true, .roles = true },
	{ .host = true, .bits = LANE_ADP_CS_1_CL1_ENABLE | LANE_ADP_CS_1_CL2_ENABLE },
	{ .bits = LANE_ADP_CS_1_CL1_ENABLE | LANE_ADP_CS_1_CL2_ENABLE },
	{ .host = true, .bits = LANE_ADP_CS_1_CL0S_ENABLE },
	{ .bits = LANE_ADP_CS_1_CL0S_ENABLE },
	{ .roles = true },
	{ .host = true, .roles = true },
};

/* What macOS reads before its writes: the USB4 port's CL support, then CL0s, CL1 and CL2. */
static bool tb_port_clx_macos_supported(struct tb_port *port)
{
	const u32 all = LANE_ADP_CS_0_CL0S_SUPPORT | LANE_ADP_CS_0_CL1_SUPPORT |
			LANE_ADP_CS_0_CL2_SUPPORT;
	u32 val;

	if (!port->cap_phy || !port->cap_usb4 || !usb4_port_clx_supported(port))
		return false;
	if (tb_port_read(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_0, 1))
		return false;
	return (val & all) == all;
}

/* After a failure: no CL state is left enabled on an end that can still be reached. */
static void tb_port_clx_macos_clear(struct tb_port *port)
{
	u32 val;

	if (tb_port_read(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_1, 1) ||
	    !(val & TB_CLX_MACOS_ENABLES))
		return;
	val &= ~TB_CLX_MACOS_ENABLES;
	tb_port_write(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_1, 1);
}

/*
 * TEST (sv6u4, J714s): the last cycle's writes, each with its lane adapter's word before and
 * after, and the host router firmware's "CLX CFG: clx_en changed" line taken for each of the
 * host's CL writes. apple.c hands each such line over as it is logged (tb_clx_test_fw_log());
 * the firmware logs it while it takes the write (mxt6b: before the write's reply), so it is
 * matched to the earliest host CL write sent whose CL bits changed that way and that has no
 * line yet. One record, for the last cycle; the line counters run from the boot.
 */
static const char * const tb_clx_test_names[] = {
	"host CL0s on", "device CL0s on", "host CL1+CL2 on", "device CL1+CL2 on",
	"device PM secondary", "host PM primary", "host CL1+CL2 off", "device CL1+CL2 off",
	"host CL0s off", "device CL0s off", "device PM secondary", "host PM primary",
};

#define TB_CLX_TEST_STEPS	ARRAY_SIZE(tb_clx_macos_steps)
#define TB_CLX_TEST_FW(word)	FIELD_GET(TB_CLX_MACOS_ENABLES, (word))

struct tb_clx_test_step {
	u32 was, now;
	int ret;		/* -ENODATA: not reached */
	bool read, sent;
	ktime_t at, done;
	unsigned int line;	/* the firmware line taken for it, counted from the boot; 0: none */
	ktime_t line_at;
};

static DEFINE_SPINLOCK(tb_clx_test_lock);
static struct {
	void *next_caller, *caller;
	unsigned int count;	/* cycles since boot */
	u8 down_port, up_port;
	int ret;
	struct tb_clx_test_step step[TB_CLX_TEST_STEPS];
	unsigned int lines, unmatched;
	unsigned int last_from, last_to;
	ktime_t last_at;
} tb_clx_test;

/* TEST (sv6u4): who asks for the next cycle (tb_switch_cio_pm_roles()'s caller). */
void tb_clx_test_set_caller(void *caller)
{
	tb_clx_test.next_caller = caller;
}

static void tb_clx_test_begin(struct tb_switch *sw)
{
	unsigned long flags;
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(tb_clx_test_names) != TB_CLX_TEST_STEPS);
	spin_lock_irqsave(&tb_clx_test_lock, flags);
	tb_clx_test.caller = tb_clx_test.next_caller;
	tb_clx_test.next_caller = NULL;
	tb_clx_test.count++;
	tb_clx_test.down_port = tb_switch_downstream_port(sw)->port;
	tb_clx_test.up_port = tb_upstream_port(sw)->port;
	tb_clx_test.ret = 0;
	memset(tb_clx_test.step, 0, sizeof(tb_clx_test.step));
	for (i = 0; i < TB_CLX_TEST_STEPS; i++)
		tb_clx_test.step[i].ret = -ENODATA;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);
}

static void tb_clx_test_read(unsigned int i, u32 was)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	tb_clx_test.step[i].was = was;
	tb_clx_test.step[i].now = was;
	tb_clx_test.step[i].read = true;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);
}

static void tb_clx_test_send(unsigned int i, u32 now)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	tb_clx_test.step[i].now = now;
	tb_clx_test.step[i].at = ktime_get();
	tb_clx_test.step[i].sent = true;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);
}

static void tb_clx_test_sent(unsigned int i, int ret)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	tb_clx_test.step[i].done = ktime_get();
	tb_clx_test.step[i].ret = ret;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);
}

/**
 * tb_clx_test_fw_log() - TEST (sv6u4): a "CLX CFG: clx_en changed" line of the router firmware
 * @from: Its CL configuration before (bit 0 CL0s, 1 CL1, 2 CL2: the LANE_ADP_CS_1 bits)
 * @to: And after
 *
 * Called by apple.c's RTKit syslog hook for each such line of a T6050 host router.
 */
void tb_clx_test_fw_log(unsigned int from, unsigned int to)
{
	struct tb_clx_test_step st = {};
	unsigned int i, line, count;
	unsigned long flags;
	ktime_t now = ktime_get();
	int match = -1;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	line = ++tb_clx_test.lines;
	count = tb_clx_test.count;
	tb_clx_test.last_from = from;
	tb_clx_test.last_to = to;
	tb_clx_test.last_at = now;
	for (i = 0; i < TB_CLX_TEST_STEPS; i++) {
		struct tb_clx_test_step *s = &tb_clx_test.step[i];

		if (!tb_clx_macos_steps[i].host || tb_clx_macos_steps[i].roles || !s->sent ||
		    s->line || TB_CLX_TEST_FW(s->was) != from || TB_CLX_TEST_FW(s->now) != to)
			continue;
		s->line = line;
		s->line_at = now;
		st = *s;
		match = i;
		break;
	}
	if (match < 0)
		tb_clx_test.unmatched++;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);

	if (match < 0) {
		pr_info("thunderbolt: TEST sv6u4: router firmware \"CLX CFG: clx_en changed %#x --> %#x\" (line %u since boot) matches no host CL write of the last cycle (#%u) (macOS mxt6, mxt6b: one such line for each of the host's CL writes, none else)\n",
			from, to, line, count);
		return;
	}
	pr_info("thunderbolt: TEST sv6u4: router firmware \"CLX CFG: clx_en changed %#x --> %#x\" (line %u since boot) taken for write %d of CL cycle #%u (%s, LANE_ADP_CS_1 %#x -> %#x), %lld us after that write was sent (macOS mxt6, mxt6b: one such line for each of the host's CL writes, 43 to 59 us after it and before its reply)\n",
		from, to, line, match + 1, count, tb_clx_test_names[match], st.was, st.now,
		ktime_us_delta(now, st.at));
}
EXPORT_SYMBOL_FOR_MODULES(tb_clx_test_fw_log, "thunderbolt_apple");

static size_t tb_clx_test_steps(char *buf, size_t size, const struct tb_clx_test_step *step,
				unsigned int first, unsigned int last)
{
	size_t at = 0;
	unsigned int i;

	for (i = first; i <= last; i++) {
		const struct tb_clx_test_step *s = &step[i];

		if (!s->read)
			at += scnprintf(buf + at, size - at, "%s%u %s not reached", i > first ? ", " : "",
					i + 1, tb_clx_test_names[i]);
		else if (!s->sent)
			at += scnprintf(buf + at, size - at, "%s%u %s read %#x, not written",
					i > first ? ", " : "", i + 1, tb_clx_test_names[i], s->was);
		else
			at += scnprintf(buf + at, size - at, "%s%u %s %#x -> %#x (%d)",
					i > first ? ", " : "", i + 1, tb_clx_test_names[i], s->was,
					s->now, s->ret);
	}
	return at;
}

static void tb_clx_test_done(struct tb_switch *sw, int ret, unsigned int stop)
{
	struct tb_clx_test_step step[TB_CLX_TEST_STEPS];
	unsigned int i, lines = 0, count, host = 0, dev = 0;
	char on[384], off[384], end[96] = "";
	unsigned long flags;
	u8 down_port, up_port;
	s64 on_us = -1;
	void *caller;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	tb_clx_test.ret = ret;
	memcpy(step, tb_clx_test.step, sizeof(step));
	caller = tb_clx_test.caller;
	count = tb_clx_test.count;
	down_port = tb_clx_test.down_port;
	up_port = tb_clx_test.up_port;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);

	for (i = 0; i < TB_CLX_TEST_STEPS; i++)
		lines += !!step[i].line;
	/* Write 10 is the last disable; the last pair's reads are writes 11 and 12's. */
	if (step[0].sent && step[9].sent && !step[9].ret)
		on_us = ktime_us_delta(step[9].done, step[0].at);
	if (step[10].read)
		dev = FIELD_GET(LANE_ADP_CS_1_ADAPTER_STATE_MASK, step[10].was);
	if (step[11].read)
		host = FIELD_GET(LANE_ADP_CS_1_ADAPTER_STATE_MASK, step[11].was);
	tb_clx_test_steps(on, sizeof(on), step, 0, 5);
	tb_clx_test_steps(off, sizeof(off), step, 6, 11);
	if (ret)
		scnprintf(end, sizeof(end), ", stopped at write %u, CL states cleared where set",
			  stop + 1);
	tb_sw_info(sw, "TEST sv6u4: link %llx:%u <-> %llx:%u CL cycle #%u by %pS: writes 1 to 6, LANE_ADP_CS_1 before -> after (write): %s (macOS, the dock's enumeration after each wake, mxt6 W1 +0.6649 to +0.6678, W2 +0.6451 to +0.6479; its symmetric words, W1: host 0x0822001e -> 0x0822041e, dock 0x4822001e -> 0x4822041e, host -> 0x08221c1e, dock -> 0x48221c1e, then the roles written as read, dock 0x4c221c1e, host 0x10221c1e)\n",
		   tb_route(tb_switch_parent(sw)), down_port, tb_route(sw), up_port, count, caller,
		   on);
	tb_sw_info(sw, "TEST sv6u4: CL cycle #%u (%d%s): writes 7 to 12: %s; CL states on %lld us, write 1 sent to write 10 done (macOS: from each enumeration to its sleep entry; on and off again only at W2, 6.9 ms in mxt6, about 12 ms in mxt6b); adapter states at the last pair: host %u, device router %u (2 CL0, 0 disabled, 1 training, 7 unplugged); firmware lines taken so far: %u of the host's 4 CL writes (macOS sleep entry, mxt6 C1 -0.5928 to -0.5877: host 0x08221c1e -> 0x0822041e, dock -> 0x4822041e, host -> 0x0822001e, dock -> 0x4822001e, then the roles)\n",
		   count, ret, end, off, on_us, host, dev, lines);
}

static void tb_clx_test_skip(struct tb_switch *sw, const char *why)
{
	tb_sw_info(sw, "TEST sv6u4: its link's CL states not cycled: %s\n", why);
}

/**
 * tb_clx_test_sleep_report() - TEST (sv6u4): the last cycle, behind the host's sleep request
 * @tb: Domain
 * @entry: When the routers' sleep frames began
 *
 * By then every firmware line of the sleep's prepare has been logged.
 */
void tb_clx_test_sleep_report(struct tb *tb, ktime_t entry)
{
	struct tb_clx_test_step step[TB_CLX_TEST_STEPS];
	unsigned int i, count, lines, unmatched, last_from, last_to;
	ktime_t last_at, end = 0;
	char fw[256], last[128];
	unsigned long flags;
	size_t at = 0;
	void *caller;
	int ret;

	spin_lock_irqsave(&tb_clx_test_lock, flags);
	memcpy(step, tb_clx_test.step, sizeof(step));
	caller = tb_clx_test.caller;
	count = tb_clx_test.count;
	ret = tb_clx_test.ret;
	lines = tb_clx_test.lines;
	unmatched = tb_clx_test.unmatched;
	last_from = tb_clx_test.last_from;
	last_to = tb_clx_test.last_to;
	last_at = tb_clx_test.last_at;
	spin_unlock_irqrestore(&tb_clx_test_lock, flags);

	if (!count) {
		tb_info(tb, "TEST sv6u4: the link's CL states were never cycled before the routers' sleep bits; %u firmware \"clx_en changed\" lines since boot (sv6u3: none)\n",
			lines);
		return;
	}
	for (i = 0; i < TB_CLX_TEST_STEPS; i++) {
		if (step[i].sent)
			end = step[i].done;
		if (!tb_clx_macos_steps[i].host || tb_clx_macos_steps[i].roles)
			continue;
		if (step[i].line)
			at += scnprintf(fw + at, sizeof(fw) - at, "%swrite %u %#lx --> %#lx (+%lld us)",
					at ? ", " : "", i + 1, FIELD_GET(TB_CLX_MACOS_ENABLES, step[i].was),
					FIELD_GET(TB_CLX_MACOS_ENABLES, step[i].now),
					ktime_us_delta(step[i].line_at, step[i].at));
		else
			at += scnprintf(fw + at, sizeof(fw) - at, "%swrite %u NONE", at ? ", " : "",
					i + 1);
	}
	if (lines)
		scnprintf(last, sizeof(last), "%#x --> %#x, %lld ms before the sleep entry", last_from,
			  last_to, ktime_ms_delta(entry, last_at));
	else
		scnprintf(last, sizeof(last), "none since boot");
	tb_info(tb, "TEST sv6u4: the link's CL states before the routers' sleep bits: last cycle #%u by %pS (%d), its last write done %lld ms before the sleep entry; router firmware lines taken for its host CL writes: %s; the firmware's last \"clx_en changed\": %s; %u such lines since boot, %u matching no write (macOS mxt6, mxt6b, four sleeps: its sleep entry's 0x7 --> 0x1 and 0x1 --> 0x0, the last 359 to 363 ms before the dock's sleep bit; sv6u3: no such line since boot)\n",
		count, caller, ret, end ? ktime_ms_delta(entry, end) : -1LL, fw, last, lines,
		unmatched);
}

/**
 * tb_switch_clx_macos_cycle() - Enable a link's CL states and disable them again, as macOS
 * @sw: Device router whose link to its parent router is cycled
 *
 * TEST (T6050, J714s): the writes of tb_clx_macos_steps, in that order and with no wait
 * between them, so the CL states are on only for the time the writes take. Every access to
 * the router's end crosses the link, so a link that does not come back from a CL state fails
 * one of them. The last pair reads both ends after the disable and checks that each lane
 * adapter is still up, with its CL states off. On any failure the CL states are cleared
 * again on each end that still answers, the parent's first, and the cycle's error is
 * returned. The router's @clx is not changed: the link ends with its CL states off. Runs
 * under the domain lock.
 *
 * Return: %0 on success, also for a link that does not support all of CL0s, CL1 and CL2 or
 * with the CL states disabled by the module parameter; negative errno otherwise.
 */
int tb_switch_clx_macos_cycle(struct tb_switch *sw)
{
	struct tb_port *up, *down, *port;
	unsigned int i, state;
	u32 val;
	int ret = 0;

	if (!tb_route(sw))
		return 0;
	if (!clx_enabled) {
		tb_clx_test_skip(sw, "the clx module parameter is off");	/* TEST (sv6u4) */
		return 0;
	}
	up = tb_upstream_port(sw);
	down = tb_switch_downstream_port(sw);
	if (!tb_port_clx_macos_supported(down) || !tb_port_clx_macos_supported(up)) {
		/* TEST (sv6u4) */
		tb_clx_test_skip(sw, "a USB4 port without CL support, or a lane adapter without all of CL0s, CL1 and CL2 (macOS reads all supported on both ends)");
		return 0;
	}

	tb_clx_test_begin(sw);	/* TEST (sv6u4) */
	for (i = 0; i < ARRAY_SIZE(tb_clx_macos_steps); i++) {
		const struct tb_clx_macos_step *st = &tb_clx_macos_steps[i];

		port = st->host ? down : up;
		ret = tb_port_read(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_1, 1);
		if (ret)
			break;
		tb_clx_test_read(i, val);	/* TEST (sv6u4) */
		/* The last role pair: both ends must still be up, with their CL states off. */
		if (i + 2 >= ARRAY_SIZE(tb_clx_macos_steps)) {
			state = FIELD_GET(LANE_ADP_CS_1_ADAPTER_STATE_MASK, val);
			if (state < TB_PORT_UP || state > TB_PORT_CL2)
				ret = -ENOTCONN;
			else if (val & TB_CLX_MACOS_ENABLES)
				ret = -EIO;
			if (ret)
				break;
		}
		if (st->roles && st->host)
			val &= ~LANE_ADP_CS_1_PMS;
		else if (st->roles)
			val |= LANE_ADP_CS_1_PMS;
		else if (st->on)
			val |= st->bits;
		else
			val &= ~st->bits;
		tb_clx_test_send(i, val);	/* TEST (sv6u4) */
		ret = tb_port_write(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_1, 1);
		tb_clx_test_sent(i, ret);	/* TEST (sv6u4) */
		if (ret)
			break;
	}
	if (ret) {
		tb_port_clx_macos_clear(down);
		tb_port_clx_macos_clear(up);
	}
	tb_clx_test_done(sw, ret, i);	/* TEST (sv6u4) */
	return ret;
}

static int tb_switch_mask_clx_objections(struct tb_switch *sw)
{
	int up_port = sw->config.upstream_port_number;
	u32 offset, val[2], mask_obj, unmask_obj;
	int ret, i;

	/* Only Titan Ridge of pre-USB4 devices support CLx states */
	if (!tb_switch_is_titan_ridge(sw))
		return 0;

	if (!tb_route(sw))
		return 0;

	/*
	 * In Titan Ridge there are only 2 dual-lane Thunderbolt ports:
	 * Port A consists of lane adapters 1,2 and
	 * Port B consists of lane adapters 3,4
	 * If upstream port is A, (lanes are 1,2), we mask objections from
	 * port B (lanes 3,4) and unmask objections from Port A and vice-versa.
	 */
	if (up_port == 1) {
		mask_obj = TB_LOW_PWR_C0_PORT_B_MASK;
		unmask_obj = TB_LOW_PWR_C1_PORT_A_MASK;
		offset = TB_LOW_PWR_C1_CL1;
	} else {
		mask_obj = TB_LOW_PWR_C1_PORT_A_MASK;
		unmask_obj = TB_LOW_PWR_C0_PORT_B_MASK;
		offset = TB_LOW_PWR_C3_CL1;
	}

	ret = tb_sw_read(sw, &val, TB_CFG_SWITCH,
			 sw->cap_lp + offset, ARRAY_SIZE(val));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(val); i++) {
		val[i] |= mask_obj;
		val[i] &= ~unmask_obj;
	}

	return tb_sw_write(sw, &val, TB_CFG_SWITCH,
			   sw->cap_lp + offset, ARRAY_SIZE(val));
}

static bool validate_mask(unsigned int clx)
{
	/* Previous states need to be enabled */
	if (clx & TB_CL1)
		return (clx & TB_CL0S) == TB_CL0S;
	return true;
}

/**
 * tb_switch_clx_enable() - Enable CLx on upstream port of specified router
 * @sw: Router to enable CLx for
 * @clx: The CLx state to enable
 *
 * CLx is enabled only if both sides of the link support CLx, and if both sides
 * of the link are not configured as two single lane links and only if the link
 * is not inter-domain link. The complete set of conditions is described in CM
 * Guide 1.0 section 8.1.
 *
 * Return: %0 on success, negative errno otherwise.
 */
int tb_switch_clx_enable(struct tb_switch *sw, unsigned int clx)
{
	bool up_clx_support, down_clx_support;
	struct tb_switch *parent_sw;
	struct tb_port *up, *down;
	int ret;

	if (!clx || sw->clx == clx)
		return 0;

	if (!validate_mask(clx))
		return -EINVAL;

	parent_sw = tb_switch_parent(sw);
	if (!parent_sw)
		return 0;

	if (!tb_switch_clx_is_supported(parent_sw) ||
	    !tb_switch_clx_is_supported(sw))
		return 0;

	/* Only support CL2 for v2 routers */
	if ((clx & TB_CL2) &&
	    (usb4_switch_version(parent_sw) < 2 ||
	     usb4_switch_version(sw) < 2))
		return -EOPNOTSUPP;

	ret = tb_switch_pm_secondary_resolve(sw);
	if (ret)
		return ret;

	up = tb_upstream_port(sw);
	down = tb_switch_downstream_port(sw);

	up_clx_support = tb_port_clx_supported(up, clx);
	down_clx_support = tb_port_clx_supported(down, clx);

	tb_port_dbg(up, "CLx: %s %ssupported\n", clx_name(clx),
		    up_clx_support ? "" : "not ");
	tb_port_dbg(down, "CLx: %s %ssupported\n", clx_name(clx),
		    down_clx_support ? "" : "not ");

	if (!up_clx_support || !down_clx_support)
		return -EOPNOTSUPP;

	ret = tb_port_clx_enable(up, clx);
	if (ret)
		return ret;

	ret = tb_port_clx_enable(down, clx);
	if (ret) {
		tb_port_clx_disable(up, clx);
		return ret;
	}

	ret = tb_switch_mask_clx_objections(sw);
	if (ret) {
		tb_port_clx_disable(up, clx);
		tb_port_clx_disable(down, clx);
		return ret;
	}

	sw->clx |= clx;

	tb_sw_dbg(sw, "CLx: %s enabled\n", clx_name(clx));
	return 0;
}

/**
 * tb_switch_clx_disable() - Disable CLx on upstream port of specified router
 * @sw: Router to disable CLx for
 *
 * Disables all CL states of the given router. Can be called on any
 * router and if the states were not enabled already does nothing.
 *
 * Return: CL states that were disabled or negative errno otherwise.
 */
int tb_switch_clx_disable(struct tb_switch *sw)
{
	unsigned int clx = sw->clx;
	struct tb_port *up, *down;
	int ret;

	if (!tb_switch_clx_is_supported(sw))
		return 0;

	if (!clx)
		return 0;

	if (sw->is_unplugged)
		return clx;

	up = tb_upstream_port(sw);
	down = tb_switch_downstream_port(sw);

	ret = tb_port_clx_disable(up, clx);
	if (ret)
		return ret;

	ret = tb_port_clx_disable(down, clx);
	if (ret)
		return ret;

	sw->clx = 0;

	tb_sw_dbg(sw, "CLx: %s disabled\n", clx_name(clx));
	return clx;
}
