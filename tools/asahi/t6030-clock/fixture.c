/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define FIELD_PREP(mask, v) (((u32)(v) << __builtin_ctz(mask)) & (mask))
#define JOIN_(a, b) a##b
#define JOIN(a, b) JOIN_(a, b)
#define guard(x) __attribute__((unused)) void *JOIN(guard_, __LINE__) =
#define lockdep_assert_held(x) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define EXPORT_SYMBOL_GPL(x)
#define APPLE_ATCPHY_MODE_USB4 1
#define APPLE_ATCPHY_MODE_TBT 2
#define symbol_get(fn) (&fn)
#define symbol_put(fn) ((void)0)
/* REGISTER_DEFINES */
struct atcphy_hw {
	enum atcphy_generation gen;
};
struct apple_atcphy {
	const struct atcphy_hw *hw;
	int lock, mode, dp_link_rate;
	void *dev;
	struct {
		u8 *core;
	} regs;
	bool tunnel_clock_on;
	u8 tunnel_rate, tunnel_users, tunnel_dpin_rate[2];
	unsigned tunnel_source[2];
};
struct phy {
	const void *ops;
	struct apple_atcphy *data;
};
struct apple_dcp;
struct apple_dcp_typec_route {
	struct apple_dcp *dcp;
	unsigned mux_index, tunnel_dpin;
	u64 tunnel_generation;
	bool xbar_up;
	void *active_xbar;
};
struct apple_dcp {
	int tb_lock;
	void *dev;
	bool dptx_tunnel, tb_clock_ok;
	unsigned dptx_dfp_port;
	u64 tb_generation;
	struct apple_dcp_typec_route *active_typec_route;
	struct {
		bool active, xbar_up, clock_ok;
		u64 generation;
		void *xbar;
	} split;
};
static int apple_atc_dp_phy_ops;
static unsigned writes, polls, apb_requests, stop_requests, crossbar_stops;
static uint64_t faults;
static u8 *registers;
static struct apple_atcphy *running_phy;
static struct apple_atcphy *phy_get_drvdata(struct phy *p)
{
	return p->data;
}
static bool of_machine_is_compatible(const char *s)
{
	return !strcmp(s, "apple,t6030");
}
static void atcphy_restore_after_pd_off(struct apple_atcphy *p)
{
}
static void udelay(unsigned us)
{
}
static u32 readl(const void *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}
static void writel(u32 v, void *p)
{
	if ((u8 *)p == registers + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0) {
		assert(!(v & DPTX_PCLK2_ENABLE) || (v & DPTX_PCLK1_ENABLE));
		if (running_phy && (running_phy->tunnel_users & BIT(1)) &&
		    running_phy->tunnel_rate)
			assert(v & DPTX_PCLK1_ENABLE);
	}
	writes++;
	memcpy(p, &v, 4);
}
static void core_clear32(struct apple_atcphy *p, unsigned off, u32 mask)
{
	writel(readl(p->regs.core + off) & ~mask, p->regs.core + off);
}
static void core_set32(struct apple_atcphy *p, unsigned off, u32 mask)
{
	writel(readl(p->regs.core + off) | mask, p->regs.core + off);
}
static void core_mask32(struct apple_atcphy *p, unsigned off, u32 mask, u32 v)
{
	writel((readl(p->regs.core + off) & ~mask) | (v & mask),
	       p->regs.core + off);
}
static int apple_atc_dp_tunnel_rate(struct phy *p, unsigned dpin, u8 rate)
{
	abort();
}
static int dcp_dpxbar_link(void *xbar, bool up)
{
	assert(!up);
	crossbar_stops++;
	return 0;
}
static int model_poll(void *address, u32 *value)
{
	unsigned i = polls++, off = (u8 *)address - registers;
	if (faults & (UINT64_C(1) << i))
		return -ETIMEDOUT;
	if (off == AUS_COMMON_DIG_RCAL1)
		*value = AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
	else if (off == AUSPLL_APB_CMD_OVERRIDE) {
		u32 cmd = readl(address);
		if (cmd & BIT(0)) {
			*value = cmd | BIT(1);
			apb_requests++;
		} else {
			*value = cmd & ~BIT(1);
			if ((cmd & 0x10000018) == 0x10000018) {
				u32 zero = 0;
				memcpy(registers + T8122_DP_PCLK_STATUS, &zero,
				       4);
				stop_requests++;
			}
		}
	} else {
		assert(off == T8122_DP_PCLK_STATUS);
		*value = readl(address);
	}
	if (off == AUSPLL_APB_CMD_OVERRIDE && (*value & BIT(0))) {
		u32 lock = ACIOPHY_AUSPLL_LOCK;
		memcpy(registers + T8122_DP_PCLK_STATUS, &lock, 4);
	}
	memcpy(address, value, 4);
	return 0;
}
#define readl_poll_timeout(address, value, condition, interval, timeout) \
	({                                                               \
		int err = model_poll((address), &(value));               \
		assert(err || (condition));                              \
		err;                                                     \
	})
/* PRODUCTION_FUNCTIONS */
static int clock_call(struct phy *phy, unsigned source, unsigned dpin, u8 rate)
{
	struct apple_dcp d = { .dptx_tunnel = true,
			       .dptx_dfp_port = dpin + 1,
			       .tb_generation = 7 };
	struct apple_dcp_typec_route r = { .dcp = &d,
					   .mux_index = source,
					   .tunnel_dpin = dpin,
					   .tunnel_generation = 7 };
	d.active_typec_route = &r;
	int ret = dcp_tunnel_set_rate(&d, phy, 0, rate);
	assert(d.tb_clock_ok == (!ret && rate));
	return ret;
}
static u32 tx(void)
{
	return readl(registers + ACIOPHY_LANE_DP_CFG_BLK_TX_DP_CTRL0);
}
int main(int argc, char **argv)
{
	assert(argc == 2);
	const char *name = argv[1];
	for (unsigned source = 0; source < 5; source++)
		for (unsigned dpin = 0; dpin < 4; dpin++)
			assert(dcp_fabric_t6030_pclk(source, dpin) ==
			       apple_dp_tunnel_t6030_pclk(source, dpin));
	u8 regs[0x10000] = { 0 };
	registers = regs;
	struct atcphy_hw hw = { .gen = ATCPHY_GENERATION_T8122 };
	struct apple_atcphy d = { .hw = &hw,
				  .regs.core = regs,
				  .mode = APPLE_ATCPHY_MODE_USB4 };
	running_phy = &d;
	struct phy phy = { .ops = &apple_atc_dp_phy_ops, .data = &d };
	if (!strcmp(name, "wrong-generation")) {
		hw.gen = ATCPHY_GENERATION_T8103;
		assert(clock_call(&phy, 0, 0, 0x14) == -EOPNOTSUPP && !writes);
		return 0;
	}
	if (!strncmp(name, "fail-start-", 11)) {
		unsigned n = strtoul(name + 11, NULL, 10);
		faults = UINT64_C(1) << n;
		assert(clock_call(&phy, 0, 0, 0x14) == -ETIMEDOUT);
		assert(!d.tunnel_clock_on && !d.tunnel_users && !d.tunnel_rate);
		assert(!(tx() & T8122_PCLK_ENABLES));
		return 0;
	}
	if (!strcmp(name, "busy-mode")) {
		d.mode = 0;
		assert(clock_call(&phy, 0, 0, 0x14) == -EBUSY && !writes);
		return 0;
	}
	if (!strcmp(name, "busy-pll")) {
		writel(T8122_AUSPLL_PCLK_DRIVER, regs + AUSPLL_CLKOUT_MASTER);
		unsigned saved = writes;
		assert(clock_call(&phy, 0, 0, 0x14) == -EBUSY &&
		       writes == saved);
		return 0;
	}
	if (!strcmp(name, "invalid-source")) {
		assert(clock_call(&phy, 1, 0, 0x14) == -EOPNOTSUPP && !writes);
		return 0;
	}
	if (!strcmp(name, "invalid-rate")) {
		assert(clock_call(&phy, 0, 0, 0x12) == -EINVAL && !writes);
		return 0;
	}
	if (!strcmp(name, "failed-cleanup")) {
		faults = BIT(1) | BIT(2);
		assert(clock_call(&phy, 0, 0, 0x14) == -ETIMEDOUT);
		assert(d.tunnel_clock_on && !d.tunnel_rate &&
		       d.tunnel_users == 1);
		unsigned saved = writes;
		assert(clock_call(&phy, 2, 1, 0x14) == -EBUSY &&
		       writes == saved);
		faults = 0;
		assert(!clock_call(&phy, 0, 0, 0));
		assert(!d.tunnel_clock_on && !d.tunnel_users);
		return 0;
	}
	unsigned first = !strcmp(name, "reverse-start") ? 1 : 0,
		 second = 1 - first;
	assert(!clock_call(&phy, 2 * first, first, 0x14));
	assert(d.tunnel_users == BIT(first) && d.tunnel_clock_on);
	assert(tx() & DPTX_PCLK1_ENABLE);
	if (first)
		assert(tx() & DPTX_PCLK2_ENABLE);
	unsigned saved_poll = polls, saved_apb = apb_requests;
	assert(!clock_call(&phy, 2 * second, second, 0x1e));
	assert(d.tunnel_users == 3 && polls == saved_poll &&
	       apb_requests == saved_apb);
	assert((tx() & (DPTX_PCLK1_ENABLE | DPTX_PCLK2_ENABLE)) ==
	       (DPTX_PCLK1_ENABLE | DPTX_PCLK2_ENABLE));
	assert((tx() & DPTX_PCLK1_SELECT) ==
	       FIELD_PREP(DPTX_PCLK1_SELECT, first ? 0 : 1));
	assert((tx() & DPTX_PCLK2_SELECT) ==
	       FIELD_PREP(DPTX_PCLK2_SELECT, first ? 1 : 0));
	if (!strcmp(name, "same-rate")) {
		unsigned saved = writes;
		assert(!clock_call(&phy, 0, 0, d.tunnel_dpin_rate[0]) &&
		       writes == saved);
	}
	if (!strcmp(name, "stale-owner")) {
		unsigned saved = writes;
		assert(clock_call(&phy, 2, 0, 0x06) == -ESTALE &&
		       writes == saved);
		assert(clock_call(&phy, 2, 0, 0) == -ESTALE && writes == saved);
	}
	if (!strcmp(name, "same-source")) {
		assert(!clock_call(&phy, 2, 1, 0));
		unsigned saved = writes;
		assert(clock_call(&phy, 0, 1, 0x14) == -EBUSY &&
		       writes == saved);
		assert(!clock_call(&phy, 2, 1, 0x1e));
	}
	if (!strcmp(name, "retune-one")) {
		u32 other = tx() & DPTX_PCLK2_SELECT;
		assert(!clock_call(&phy, 0, 0, 0x06));
		assert((tx() & DPTX_PCLK1_SELECT) ==
		       FIELD_PREP(DPTX_PCLK1_SELECT, 4));
		assert((tx() & DPTX_PCLK2_SELECT) == other &&
		       polls == saved_poll);
	}
	unsigned released = !strcmp(name, "stop-second") ? 1 : 0,
		 remaining = 1 - released;
	assert(!clock_call(&phy, 2 * released, released, 0));
	assert(d.tunnel_clock_on && d.tunnel_users == BIT(remaining) &&
	       !stop_requests);
	assert(tx() & DPTX_PCLK1_ENABLE);
	assert(!!(tx() & DPTX_PCLK2_ENABLE) == !!remaining);
	unsigned saved = writes;
	assert(!clock_call(&phy, 2 * released, released, 0) && writes == saved);
	if (!strcmp(name, "failed-final-stop")) {
		faults = UINT64_C(1) << polls;
		assert(clock_call(&phy, 2 * remaining, remaining, 0) ==
		       -ETIMEDOUT);
		assert(d.tunnel_clock_on && !d.tunnel_rate &&
		       d.tunnel_users == BIT(remaining));
		saved = writes;
		assert(clock_call(&phy, 2 * released, released, 0x14) ==
			       -EBUSY &&
		       writes == saved);
		faults = 0;
	}
	assert(!clock_call(&phy, 2 * remaining, remaining, 0));
	assert(!d.tunnel_clock_on && !d.tunnel_users && !d.tunnel_rate &&
	       !(tx() & T8122_PCLK_ENABLES));
	saved = writes;
	assert(!clock_call(&phy, 2 * remaining, remaining, 0) &&
	       writes == saved);
	printf("PASS %s writes=%u polls=%u: two callbacks, independent selectors, shared PLL, stop ownership\n",
	       name, writes, polls);
	return 0;
}
