// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define guard(type) (void)
#define EXPORT_SYMBOL_GPL(name)
#define ATCPHY_GENERATION_T8132 4
#define ATCPHY_PIPEHANDLER_STATE_DUMMY 0
enum atcphy_mode {
	APPLE_ATCPHY_MODE_OFF,
	APPLE_ATCPHY_MODE_USB3,
	APPLE_ATCPHY_MODE_TBT,
	APPLE_ATCPHY_MODE_USB4
};
/* ACTUAL_DEFINES */
/* ACTUAL_STATE */
struct hw {
	unsigned int gen;
};
struct apple_atcphy {
	struct hw *hw;
	int lock;
	bool t8132_common_owned, cio_tunables, t8132_failed, mode_dirty,
		pipe_pending;
	bool swap_lanes, host_active, tunnel_clock_on, tunnel_ack_stale, pd_was_off,
		t8132_suspended;
	unsigned int pipe_state;
	enum atcphy_mode mode;
	struct atcphy_cio_sleep cio_sleep;
	struct {
		void *core;
	} regs;
	u8 tunnel_rate, tunnel_users, tunnel_dpin_rate[2];
	unsigned int tunnel_source[2];
};
static int apple_atc_usb3_phy_ops, wrong_ops;
struct phy {
	const void *ops;
	struct apple_atcphy *data;
};
static struct apple_atcphy *phy_get_drvdata(struct phy *phy)
{
	return phy->data;
}
#define PM_SUSPEND_MEM 3
static int pm_suspend_target_state = PM_SUSPEND_MEM;
static bool atcphy_mode_is_cio(enum atcphy_mode mode)
{
	return mode == APPLE_ATCPHY_MODE_TBT || mode == APPLE_ATCPHY_MODE_USB4;
}
static u32 regs[0x21000 / 4];
static unsigned int writes, reads, config, polls, fail_poll, stop_calls,
	start_calls;
static u32 stat_high, stat_low;
static bool fail_stop, fail_start, stop_failed;
#define dev_err(...) ((void)0)
static unsigned int trace[128], ntrace;
static u32 readl(void *p)
{
	unsigned int off = (uintptr_t)p - (uintptr_t)regs;

	reads++;
	assert(off < sizeof(regs) && !(off & 3));
	if (off == ATCPHY_POWER_STAT)
		return ((regs[ATCPHY_POWER_CTRL / 4] & 3) | stat_high) &
		       ~stat_low;
	return regs[off / 4];
}
static void writel(u32 value, void *p)
{
	unsigned int off = (uintptr_t)p - (uintptr_t)regs;

	writes++;
	assert(off == ATCPHY_POWER_CTRL || off == ATCPHY_MISC ||
	       off == ACIOPHY_LANE_MODE_T8122 || off == ACIOPHY_CROSSBAR_T8122);
	assert(ntrace < 128);
	trace[ntrace++] = off;
	/* The always-on reset must never be cleared by CIO entry. */
	if (off == ATCPHY_MISC)
		assert(value & ATCPHY_MISC_RESET_N);
	regs[off / 4] = value;
}
static u32 core_read32(struct apple_atcphy *p, unsigned int off)
{
	return readl(p->regs.core + off);
}
static void core_set32(struct apple_atcphy *p, unsigned int off, u32 bits)
{
	writel(core_read32(p, off) | bits, p->regs.core + off);
}
static void core_clear32(struct apple_atcphy *p, unsigned int off, u32 bits)
{
	writel(core_read32(p, off) & ~bits, p->regs.core + off);
}
static void core_mask32(struct apple_atcphy *p, unsigned int off, u32 mask,
			u32 value)
{
	writel((core_read32(p, off) & ~mask) | value, p->regs.core + off);
}
static void udelay(unsigned int us)
{
	assert(us == 1 || us == 2);
}
#define readl_poll_timeout_atomic(addr, value, condition, delay, timeout) \
	({                                                                \
		value = readl(addr);                                      \
		polls++;                                                  \
		assert(timeout == 500000 && delay == 1);                  \
		polls == fail_poll ? -ETIMEDOUT :                         \
		(condition)	   ? 0 :                                  \
				     -ETIMEDOUT;                          \
	})
static int atc_t8122_tunnel_stop(struct apple_atcphy *p)
{
	stop_calls++;
	assert(p->cio_sleep.prepared && !p->cio_sleep.changed);
	if (fail_stop) {
		/* As the real one: the gates are already off, the rate is gone. */
		stop_failed = true;
		p->tunnel_rate = 0;
		return -EIO;
	}
	p->tunnel_clock_on = false;
	p->tunnel_users = p->tunnel_rate = 0;
	return 0;
}
static int apple_atc_tunnel_pclk(struct apple_atcphy *p, unsigned int source,
				 unsigned int dpin)
{
	(void)p;
	assert(source == 7 + dpin);
	return dpin;
}
static int atc_t8122_tunnel_start(struct apple_atcphy *p, unsigned int clock,
				  u8 rate)
{
	assert(clock < 2 && rate == 0x1e);
	/* After the power cycle, or straight after a stop that failed. */
	assert((config && regs[ATCPHY_POWER_CTRL / 4] == 0x1b) ||
	       (stop_failed && !p->cio_sleep.changed && !p->tunnel_ack_stale));
	start_calls++;
	if (fail_start) {
		/* As the real one: a failed start stops the clock, dropping its owners. */
		p->tunnel_users = 0;
		p->tunnel_dpin_rate[0] = p->tunnel_dpin_rate[1] = 0;
		return -EIO;
	}
	/* The first start after the power cycle releases the stale ACK. */
	p->tunnel_ack_stale = false;
	p->tunnel_clock_on = true;
	p->tunnel_rate = rate;
	return 0;
}
static void atcphy_t8132_cio_config(struct apple_atcphy *p, bool keep_swap)
{
	(void)p;
	assert(keep_swap && regs[ATCPHY_POWER_CTRL / 4] == 0xb);
	assert(!p->cio_sleep.restored);
	config++;
	regs[ACIOPHY_LANE_MODE_T8122 / 4] = 0xdead;
	regs[ACIOPHY_CROSSBAR_T8122 / 4] = 0xbeef;
}
/* ACTUAL_FUNCTIONS */
static struct hw hw = { .gen = ATCPHY_GENERATION_T8132 };
static struct apple_atcphy atc;
static struct phy phy = { .ops = &apple_atc_usb3_phy_ops, .data = &atc };
static void reset(bool swap, unsigned int users)
{
	memset(regs, 0, sizeof(regs));
	writes = reads = config = polls = fail_poll = stop_calls = start_calls =
		ntrace = 0;
	stat_high = stat_low = 0;
	fail_stop = fail_start = stop_failed = false;
	pm_suspend_target_state = PM_SUSPEND_MEM;
	phy.ops = &apple_atc_usb3_phy_ops;
	atc = (struct apple_atcphy){ .hw = &hw,
				     .t8132_common_owned = true,
				     .cio_tunables = true,
				     .mode = APPLE_ATCPHY_MODE_USB4,
				     .swap_lanes = swap,
				     .regs.core = regs,
				     .tunnel_users = users,
				     .tunnel_clock_on = !!users,
				     .tunnel_rate = users ? 0x1e : 0,
				     .tunnel_dpin_rate = { 0x1e, 0x1e },
				     .tunnel_source = { 7, 8 } };
	regs[ATCPHY_POWER_CTRL / 4] = 0x1b;
	regs[ATCPHY_MISC / 4] = ATCPHY_MISC_RESET_N |
				(swap ? ATCPHY_MISC_LANE_SWAP : 0);
	regs[ACIOPHY_LANE_MODE_T8122 / 4] = 0x1000;
	regs[ACIOPHY_CROSSBAR_T8122 / 4] = 0x21000 | swap;
}
static void check_on(unsigned int users, bool swap)
{
	assert(atc.cio_sleep.restored && !atc.pd_was_off);
	assert(regs[ATCPHY_POWER_CTRL / 4] == 0x1b);
	assert(regs[ATCPHY_MISC / 4] == (1U | (swap ? BIT(2) : 0)));
	assert(regs[ACIOPHY_LANE_MODE_T8122 / 4] == 0x1000);
	assert(regs[ACIOPHY_CROSSBAR_T8122 / 4] == (0x21000U | swap));
	assert(atc.tunnel_users == users && atc.tunnel_clock_on == !!users);
	/* With no clock user the release stays pending for a later start. */
	assert(atc.tunnel_ack_stale == !users);
	assert(atc.t8132_common_owned && atc.mode == APPLE_ATCPHY_MODE_USB4 &&
	       atc.swap_lanes == swap);
	assert(trace[ntrace - 1] == ATCPHY_POWER_CTRL);
}
int main(void)
{
	unsigned int cases = 0;

	for (unsigned int swap = 0; swap < 2; swap++)
		for (unsigned int users = 0; users < 4; users++) {
			reset(swap, users);
			for (unsigned int cycle = 0; cycle < 2; cycle++) {
				assert(!apple_atcphy_cio_prepare(&phy) &&
				       atc.cio_sleep.prepared);
				assert(apple_atcphy_cio_prepare(&phy) ==
				       -EBUSY);
				assert(!apple_atcphy_cio_off(&phy) &&
				       atc.cio_sleep.off);
				assert(regs[ATCPHY_POWER_CTRL / 4] ==
				       ATCPHY_POWER_CLAMP_EN);
				assert(apple_atcphy_cio_finish(&phy) ==
				       -EUCLEAN);
				/* The power-loss case, followed by a same-power aborted cycle. */
				if (!cycle)
					memset(regs, 0, sizeof(regs));
				atc.pd_was_off = !cycle;
				assert(!apple_atcphy_cio_on(&phy));
				check_on(users, swap);
				assert(apple_atcphy_cio_off(&phy) == -EBUSY);
				unsigned int before = writes;

				assert(!apple_atcphy_cio_on(&phy) &&
				       writes == before);
				assert(!apple_atcphy_cio_finish(&phy) &&
				       !atc.cio_sleep.prepared);
				assert(apple_atcphy_cio_on(&phy) == -EUCLEAN);
				cases++;
			}
		}
	for (unsigned int poll = 1; poll <= 4; poll++) {
		reset(false, 3);
		assert(!apple_atcphy_cio_prepare(&phy));
		fail_poll = poll;
		if (poll <= 2) {
			assert(apple_atcphy_cio_off(&phy) == -ETIMEDOUT &&
			       !atc.cio_sleep.off);
		} else {
			assert(!apple_atcphy_cio_off(&phy));
			assert(apple_atcphy_cio_on(&phy) == -ETIMEDOUT &&
			       !atc.cio_sleep.restored);
		}
		assert(apple_atcphy_cio_finish(&phy) == -EUCLEAN);
		fail_poll = 0;
		assert(!apple_atcphy_cio_on(&phy));
		check_on(3, false);
		assert(!apple_atcphy_cio_finish(&phy));
		cases++;
	}
	for (unsigned int bad = 0; bad < 8; bad++) {
		reset(false, 0);
		if (bad == 0)
			atc.t8132_common_owned = false;
		if (bad == 1)
			atc.cio_tunables = false;
		if (bad == 2)
			atc.t8132_failed = true;
		if (bad == 3)
			atc.mode_dirty = true;
		if (bad == 4)
			atc.pipe_pending = true;
		if (bad == 5)
			atc.mode = APPLE_ATCPHY_MODE_USB3;
		if (bad == 6)
			phy.ops = &wrong_ops;
		if (bad == 7)
			pm_suspend_target_state = 0;
		assert(apple_atcphy_cio_prepare(&phy) < 0 && !reads && !writes);
		cases++;
	}
	for (unsigned int bit = 0; bit < 2; bit++) {
		reset(false, 0);
		assert(!apple_atcphy_cio_prepare(&phy));
		stat_high = BIT(bit);
		assert(apple_atcphy_cio_off(&phy) == -ETIMEDOUT &&
		       !atc.cio_sleep.off);
		stat_high = 0;
		assert(!apple_atcphy_cio_on(&phy));
		assert(!apple_atcphy_cio_finish(&phy));
		cases++;
		reset(false, 0);
		assert(!apple_atcphy_cio_prepare(&phy));
		assert(!apple_atcphy_cio_off(&phy));
		stat_low = BIT(bit);
		assert(apple_atcphy_cio_on(&phy) == -ETIMEDOUT &&
		       !atc.cio_sleep.restored);
		stat_low = 0;
		assert(!apple_atcphy_cio_on(&phy));
		assert(!apple_atcphy_cio_finish(&phy));
		cases++;
	}
	for (unsigned int bad = 0; bad < 5; bad++) {
		reset(false, 0);
		assert(!apple_atcphy_cio_prepare(&phy));
		if (bad == 0)
			atc.host_active = true;
		if (bad == 1)
			atc.pipe_state = 1;
		if (bad == 2)
			atc.swap_lanes = true;
		if (bad == 3)
			atc.mode_dirty = true;
		if (bad == 4)
			atc.tunnel_clock_on = true;
		assert(apple_atcphy_cio_off(&phy) == -EBUSY && !writes &&
		       !atc.cio_sleep.changed);
		assert(!apple_atcphy_cio_finish(&phy));
		cases++;
	}
	reset(false, 3);
	assert(!apple_atcphy_cio_prepare(&phy));
	fail_stop = true;
	/* Nothing was powered down: the sleep is refused and the PHY stays usable. */
	assert(apple_atcphy_cio_off(&phy) == -EIO && !writes &&
	       !atc.t8132_failed && !atc.cio_sleep.changed);
	/* The DP inputs got their clocks back. */
	assert(start_calls == 2 && atc.tunnel_clock_on && atc.tunnel_rate == 0x1e &&
	       atc.tunnel_users == 3);
	assert(!apple_atcphy_cio_on(&phy) && !writes);
	assert(!apple_atcphy_cio_finish(&phy));
	cases++;
	/* A clock that cannot start again either: still a clean refusal. */
	reset(false, 3);
	assert(!apple_atcphy_cio_prepare(&phy));
	fail_stop = fail_start = true;
	assert(apple_atcphy_cio_off(&phy) == -EIO && !writes &&
	       !atc.t8132_failed && !atc.cio_sleep.changed && start_calls == 1 &&
	       atc.tunnel_clock_on && !atc.tunnel_rate);
	/* Its DP owners are kept, so their release still stops the clock. */
	assert(atc.tunnel_users == 3 && atc.tunnel_dpin_rate[0] == 0x1e &&
	       atc.tunnel_dpin_rate[1] == 0x1e && atc.tunnel_source[1] == 8);
	assert(!apple_atcphy_cio_on(&phy) && !apple_atcphy_cio_finish(&phy));
	cases++;
	reset(false, 3);
	assert(!apple_atcphy_cio_prepare(&phy));
	assert(!apple_atcphy_cio_off(&phy));
	fail_start = true;
	assert(apple_atcphy_cio_on(&phy) == -EIO && !atc.cio_sleep.restored &&
	       atc.t8132_failed);
	assert(apple_atcphy_cio_finish(&phy) == -EUCLEAN);
	cases++;
	reset(false, 0);
	assert(!apple_atcphy_cio_prepare(&phy));
	assert(!apple_atcphy_cio_on(&phy) && !apple_atcphy_cio_finish(&phy) &&
	       !reads && !writes);
	cases++;
	printf("PASS %u CIO PHY sleep cases\n", cases);
}
