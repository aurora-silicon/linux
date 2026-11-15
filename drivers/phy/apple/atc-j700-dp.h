/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Copyright 2026 Aurora Silicon */
#ifndef APPLE_ATC_J700_DP_H
#define APPLE_ATC_J700_DP_H

/* Masks are the union of writes in the public J700 DP start/voltage/stop paths. */
struct j700_usb_field {
	u32 offset, mask;
};

static const struct j700_usb_field j700_usb_fields[] = {
	{ 0x0008, 0x00015540 },
	{ 0x000c, 0x00000055 },
	{ 0x010c, 0x00000555 },
	{ 0x0200, 0x0000000f },
	{ 0x9160, 0x00000001 },
	{ 0x91d4, 0x00000003 },
	{ 0x91dc, 0x00018000 },
	{ 0x9210, 0x0000c00f },
	{ 0x9244, 0x0007ffff },
	{ 0x9248, 0x10000000 },
	{ 0xb030, 0x00000c00 },
	{ 0xb064, 0x0000fc0c },
	{ 0xb068, 0x000001e0 },
	{ 0xb06c, 0x0001ee00 },
	{ 0xb0a4, 0x00000003 },
	{ 0xb0a8, 0x00ff8000 },
	{ 0xb0d4, 0x001fff80 },
	{ 0xb0d8, 0x00000007 },
	{ 0xb0dc, 0x00000018 },
	{ 0xc050, 0x0000c00f },
	{ 0xc0d0, 0x0017ffff },
	{ 0xc0d4, 0x10000000 },
	{ 0xd004, 0x33000000 },
	{ 0xd00c, 0x07000000 },
	{ 0xd010, 0x00000600 },
	{ 0xd01c, 0x00c00000 },
	{ 0xd044, 0xffffffff },
};

static_assert(ARRAY_SIZE(j700_usb_fields) == 27, "USB context storage size");

static void j700_usb_field(unsigned int index, unsigned int lane, u32 *offset, u32 *mask)
{
	*offset = j700_usb_fields[index].offset;
	*mask = j700_usb_fields[index].mask;
	if (index < 4)
		*mask <<= index == 3 ? 4 * lane : lane;
	else
		*offset += 0x7000 * lane;
}

/* Capture before the first DP tunable or idle write; never replace a retained context. */
static int j700_usb_capture(struct apple_atcphy *p, enum atcphy_mode mode)
{
	const struct atcphy_mode_configuration *cfg = atcphy_get_mode_config(p, mode);

	if (!p->hw->has_t8140_dp || !p->fixed_usb2)
		return 0;
	if (p->usb_off_pending ||
	    p->pipe_state != ATCPHY_PIPEHANDLER_STATE_DUMMY || p->pipe_pending)
		return -EBUSY;
	for (unsigned int lane = 0; lane < 2; lane++) {
		if (!cfg->dp_lane[lane] || (p->dp_usb_context_valid & BIT(lane)))
			continue;
		for (unsigned int i = 0; i < ARRAY_SIZE(j700_usb_fields); i++) {
			u32 offset, mask;

			j700_usb_field(i, lane, &offset, &mask);
			p->dp_usb_context[lane][i] = core_read32(p, offset) & mask;
		}
		p->dp_usb_context_valid |= BIT(lane);
	}
	return 0;
}

/* Same control write/delay/poll sequence, retaining its final sample for diagnostics. */
static int j700_usb_field_update(struct apple_atcphy *p, u32 offset, u32 mask,
				 u32 value, u32 *observed)
{
	core_mask32(p, offset, mask, value);
	usleep_range(10, 20);
	return readl_poll_timeout_atomic(p->regs.core + offset, *observed,
					(*observed & mask) == value, 1, 100);
}

/* Restore only DP-owned bits, with the PIPE parked and the DP PMA idle. */
static int j700_usb_restore(struct apple_atcphy *p, enum atcphy_mode mode)
{
	const struct atcphy_mode_configuration *cfg = atcphy_get_mode_config(p, mode);
	const unsigned int power_order[] = { 2, 1, 0, 3 };
	const unsigned int receiver_order[] = { 4, 0, 6, 2 };
	int ret;

	if (!p->hw->has_t8140_dp || !p->fixed_usb2)
		return 0;
	if (p->usb_off_pending && mode != APPLE_ATCPHY_MODE_OFF)
		return -EBUSY;
	if (p->pipe_state != ATCPHY_PIPEHANDLER_STATE_DUMMY || p->pipe_pending)
		return -EBUSY;
	if (p->dp_owned_pmas || p->dp_idle_pmas)
		return -EBUSY;
	if (mode == APPLE_ATCPHY_MODE_OFF && p->usb_off_drained)
		return 0;
	for (unsigned int lane = 0; lane < 2; lane++) {
		if ((mode != APPLE_ATCPHY_MODE_OFF &&
		     cfg->lane_mode[lane] != ACIOPHY_LANE_MODE_USB3) ||
		    !(p->dp_usb_context_valid & BIT(lane)))
			continue;
		if (p->dp_usb_context_dirty & BIT(lane)) {
			for (unsigned int i = 0; i < ARRAY_SIZE(j700_usb_fields); i++) {
				u32 offset, mask, observed, value = p->dp_usb_context[lane][i];

				if (i == 1 || i == 3)
					continue;
				j700_usb_field(i, lane, &offset, &mask);
				core_mask32(p, offset, mask, value);
				observed = core_read32(p, offset) & mask;
				if (observed != value) {
					dev_dbg(p->dev, "USB restore stage=static pma=%u reg=%#x mask=%#x expected=%#x observed=%#x\n",
						lane, offset, mask, value, observed);
					return -EIO;
				}
			}
		}
		/* Preserve the public PMA power bit order, using saved values. */
		for (unsigned int i = 0; i < ARRAY_SIZE(power_order); i++) {
			u32 mask = BIT(4 * lane + power_order[i]);
			u32 value = p->dp_usb_context[lane][3] & mask, observed;

			ret = j700_usb_field_update(p, 0x200, mask, value, &observed);
			if (ret) {
				dev_dbg(p->dev, "USB restore stage=power pma=%u reg=0x200 mask=%#x expected=%#x observed=%#x ret=%d\n",
					lane, mask, value, observed & mask, ret);
				return ret;
			}
		}
		/* Restore receiver enables, then release its two idle controls. */
		for (unsigned int i = 0; i < ARRAY_SIZE(receiver_order); i++) {
			u32 mask = BIT(lane + receiver_order[i]);
			u32 value = p->dp_usb_context[lane][1] & mask, observed;

			ret = j700_usb_field_update(p, 0xc, mask, value, &observed);
			if (ret) {
				dev_dbg(p->dev, "USB restore stage=receiver pma=%u reg=0xc mask=%#x expected=%#x observed=%#x ret=%d\n",
					lane, mask, value, observed & mask, ret);
				return ret;
			}
		}
		/* OFF keeps the shadow until domain shutdown succeeds. */
		if (mode != APPLE_ATCPHY_MODE_OFF) {
			p->dp_usb_context_dirty &= ~BIT(lane);
			p->dp_usb_context_valid &= ~BIT(lane);
		}
	}
	return 0;
}

/* Retry only the idle operations claimed by an incomplete route change. */
static int j700_dp_idle_pending(struct apple_atcphy *p)
{
	int ret;

	for (unsigned int lane = 0; lane < 2; lane++) {
		if (!(p->dp_idle_pmas & BIT(lane)))
			continue;
		ret = j700_pma_idle(p, lane);
		if (ret)
			return ret;
		p->dp_idle_pmas &= ~BIT(lane);
	}
	return 0;
}

/* Preserve the upper routing state established by the platform firmware. */
static int j700_dp_route(struct apple_atcphy *p, bool four_lanes)
{
	u32 protocols = four_lanes ? 0x492 : (p->swap_lanes ? 0x252 : 0x489);

	core_mask32(p, 0x60, 0xfff, protocols);
	if (four_lanes) {
		core_mask32(p, 0x64, 0x1f, 0x14);
		core_mask32(p, 0x64, 0x3ffe0, p->swap_lanes ? 0x100 : 0x22000);
	} else {
		core_mask32(p, 0x64, 0x1ffff, 0x110 | p->swap_lanes);
	}
	for (unsigned int lane = 0; lane < 2; lane++) {
		if (!four_lanes && lane != !p->swap_lanes)
			continue;
		p->dp_idle_pmas |= BIT(lane);
	}
	return j700_dp_idle_pending(p);
}

/* Register facts from the authorized J700 hardware audit. */
struct j700_dp_step {
	u32 offset, mask, value;
	u16 delay_ns;
};

static void j700_dp_steps(struct apple_atcphy *p, u32 base,
			  const struct j700_dp_step *steps, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		core_mask32(p, base + steps[i].offset, steps[i].mask, steps[i].value);
		if (steps[i].delay_ns)
			ndelay(steps[i].delay_ns);
	}
}

static int j700_dp_wait(struct apple_atcphy *p, u32 offset, u32 mask, bool set)
{
	u32 value;

	return readl_poll_timeout_atomic(p->regs.core + offset, value,
					!!(value & mask) == set, 1, 100);
}

static void j700_dp_power(struct apple_atcphy *p, unsigned int lane)
{
	unsigned int shift = 4 * lane;
	const u32 bits[] = { BIT(2), BIT(1), BIT(0), BIT(3) };

	for (unsigned int i = 0; i < ARRAY_SIZE(bits); i++) {
		u32 mask = bits[i] << shift;

		core_mask32(p, 0x200, mask, i == 2 ? 0 : mask);
		udelay(1);
	}
	for (unsigned int bank = 0; bank < 2; bank++) {
		core_clear32(p, 0xc, BIT(6 - 4 * bank + lane));
		udelay(1);
	}
}

static void j700_dp_output_power(struct apple_atcphy *p, unsigned int lane, bool tx)
{
	u32 reg = tx ? 0x10c : 0xc;

	for (unsigned int pair = 0; pair < 2; pair++) {
		core_set32(p, reg, BIT(4 - 4 * pair + lane));
		core_set32(p, reg, BIT(6 - 4 * pair + lane));
		udelay(1);
	}
	if (!tx) {
		for (unsigned int pair = 0; pair < 2; pair++) {
			core_set32(p, 0x8, BIT(10 - 4 * pair + lane));
			core_set32(p, 0x8, BIT(12 - 4 * pair + lane));
			udelay(1);
		}
	}
	reg = tx ? 0x10c : 0x8;
	core_clear32(p, reg, BIT(tx ? 8 : 14) << lane);
	core_set32(p, reg, BIT(tx ? 10 : 16) << lane);
	udelay(1);
}

static void j700_dp_equalize(struct apple_atcphy *p, unsigned int lane,
			     bool tx, u32 preset)
{
	u32 base = (tx ? 0xc000 : 0x9000) + lane * 0x7000;
	u32 swing = ((preset & 3) << 14) | ((preset >> 2) & 15);
	u32 taps = ((preset >> 6) & 0x3ffff) * 2 + 1;

	if (tx)
		core_set32(p, base + 0xd0, BIT(20));
	core_mask32(p, base + (tx ? 0x50 : 0x210), 0xc00f, swing);
	core_set32(p, base + (tx ? 0xd4 : 0x248), BIT(28));
	core_mask32(p, base + (tx ? 0xd0 : 0x244), 0x7ffff, taps);
}

static void j700_dp_transmitter(struct apple_atcphy *p, unsigned int lane, int rate)
{
	static const struct j700_dp_step start[] = {
		{ 0x44, ~0U, 0x0aafb800, 500 },
		{ 0x44, 0x1000000, 0x1000000, 250 },
		{ 0x44, 0x4000000, 0x4000000, 250 },
		{ 0x44, 0x7c0, 0x300, 250 },
		{ 0x44, 0x6c000, 0, 250 },
	};
	static const struct j700_dp_step divide[] = {
		{ 0x44, 0x100000, 0x100000, 250 },
		{ 0x44, 0x400000, 0x400000, 0 },
	};
	static const struct j700_dp_step finish[] = {
		{ 0x04, 0x03000000, 0x02000000, 0 },
		{ 0x04, 0x30000000, 0x30000000, 0 },
		{ 0x10, 0x600, 0x600, 0 },
		{ 0x1c, 0xc00000, 0x800000, 0 },
	};
	u32 base = 0xd000 + lane * 0x7000;

	j700_dp_output_power(p, lane, true);
	j700_dp_steps(p, base, start, ARRAY_SIZE(start));
	j700_dp_equalize(p, lane, true, 0);
	if (rate != ATCPHY_DP_LINK_RATE_HBR3)
		j700_dp_steps(p, base, divide, ARRAY_SIZE(divide));
	j700_dp_steps(p, base, finish, 2);
	core_mask32(p, base + 0xc, 0x7000000,
		    rate == ATCPHY_DP_LINK_RATE_RBR ? 0x5000000 : 0x4000000);
	j700_dp_steps(p, base, finish + 2, ARRAY_SIZE(finish) - 2);
}

static void j700_dp_receiver(struct apple_atcphy *p, unsigned int lane, int rate)
{
	static const struct j700_dp_step setup[] = {
		{ 0xa4, 3, 2, 0 }, { 0xa8, 0x18000, 0x10000, 0 },
		{ 0x6c, 0x18000, 0x10000, 0 },
		{ 0xd4, 0x1f1f80, 0x121180, 0 }, { 0x64, 0xfc00, 0xb800, 0 },
		{ 0xd8, 7, 4, 0 }, { 0xdc, 0x18, 0, 0 },
		{ 0xd4, 0xe000, 0xc000, 100 },
	};
	static const struct j700_dp_step settle[] = {
		{ 0x6c, 0x600, 0x400, 100 }, { 0xd4, 0xd0000, 0x50000, 0 },
		{ 0xd8, 3, 1, 500 }, { 0xd4, 0xc0000, 0, 200 },
		{ 0x68, 0x180, 0x180, 200 }, { 0x68, 0x60, 0x60, 200 },
		{ 0xd8, 3, 3, 1000 }, { 0xd8, 3, 2, 1000 },
		{ 0xd8, 3, 0, 750 }, { 0xa8, 0x60000, 0x60000, 1000 },
		{ 0x6c, 0x6000, 0x4000, 0 },
	};
	u32 top = 0x9000 + lane * 0x7000;
	u32 base = top + 0x2000;

	core_set32(p, top + 0x160, 1);
	core_set32(p, top + 0x1dc, 0x18000);
	j700_dp_steps(p, base, setup, ARRAY_SIZE(setup));
	j700_dp_output_power(p, lane, false);
	core_set32(p, base + 0x6c, BIT(11));
	core_mask32(p, top + 0x1d4, 3, 1);
	ndelay(10);
	core_set32(p, top + 0x1d4, 2);
	j700_dp_steps(p, base, settle, ARRAY_SIZE(settle));
	j700_dp_equalize(p, lane, false, 0x3f000);
	core_mask32(p, base + 0xa8, 0x380000,
		    rate == ATCPHY_DP_LINK_RATE_RBR ? 0x280000 : 0x200000);
	core_mask32(p, base + 0xa8, 0xc00000, 0x800000);
	ndelay(50);
	core_set32(p, base + 0x30, 0xc00);
	ndelay(100);
}

static void j700_dp_outputs_off(struct apple_atcphy *p, unsigned int lane)
{
	static const struct j700_dp_step tx[] = {
		{ 0x1c, 0xc00000, 0xc00000, 0 }, { 0x10, 0x600, 0x400, 0 },
		{ 0x04, 0x30000000, 0x20000000, 250 },
		{ 0x44, 0x100000, 0, 250 }, { 0x44, 0x400000, 0, 250 },
		{ 0x44, 0x4000000, 0, 250 }, { 0x44, 0x1000000, 0, 250 },
		{ 0x44, 0x1000, 0, 0 },
	};
	static const struct j700_dp_step rx[] = {
		{ 0x30, 0xc00, 0x800, 50 }, { 0xa8, 0xc00000, 0xc00000, 500 },
		{ 0xa8, 0x60000, 0x40000, 0 }, { 0x68, 0x60, 0x40, 200 },
		{ 0x68, 0x180, 0x100, 200 }, { 0xd4, 0x10000, 0, 100 },
		{ 0x6c, 0x600, 0x600, 0 },
	};
	const u32 tx_bits[] = { BIT(8), BIT(0), BIT(4) };
	const u32 rx_bits[] = { BIT(14), BIT(6), BIT(10) };

	j700_dp_steps(p, 0xd000 + lane * 0x7000, tx, ARRAY_SIZE(tx));
	for (unsigned int i = 0; i < ARRAY_SIZE(tx_bits); i++) {
		core_mask32(p, 0x10c, tx_bits[i] << lane, i ? 0 : tx_bits[i] << lane);
		udelay(1);
	}
	j700_dp_steps(p, 0xb000 + lane * 0x7000, rx, ARRAY_SIZE(rx));
	core_mask32(p, 0x91d4 + lane * 0x7000, 3, 1);
	core_clear32(p, 0xb06c + lane * 0x7000, BIT(11));
	for (unsigned int i = 0; i < ARRAY_SIZE(rx_bits); i++) {
		core_mask32(p, 0x8, rx_bits[i] << lane, i ? 0 : rx_bits[i] << lane);
		udelay(1);
	}
	for (unsigned int i = 0; i < 2; i++) {
		core_clear32(p, 0xc, BIT(4 * i + lane));
		udelay(1);
	}
}

static int j700_dp_stop(struct apple_atcphy *p)
{
	int ret;

	if (!p->dp_owned_pmas)
		return 0;
	core_set32(p, 0x7000, BIT(0));
	for (unsigned int i = 0; i < 2; i++)
		if (p->dp_owned_pmas & BIT(i))
			j700_dp_outputs_off(p, i);
	core_mask32(p, 0x7000, 0xc, 8);
	core_mask32(p, 0x2000, 0x1ffffff9, 0x10000001);
	ret = j700_dp_wait(p, 0x2000, BIT(1), true);
	if (ret)
		return ret;
	core_mask32(p, 0x2000, 0x1ffffff9, 0x10000018);
	ret = j700_dp_wait(p, 0x2000, BIT(1), false);
	if (!ret)
		ret = j700_dp_wait(p, 0x7034, BIT(3), false);
	if (ret)
		return ret;
	core_clear32(p, 0x2200, BIT(3));
	for (unsigned int i = 0; i < 2; i++) {
		if (!(p->dp_owned_pmas & BIT(i)))
			continue;
		ret = j700_pma_idle(p, i);
		if (ret)
			return ret;
	}
	p->dp_owned_pmas = 0;
	p->dp_link_rate = -1;
	dev_dbg(p->dev, "J700 DP main stopped\n");
	return 0;
}

static int j700_dp_start(struct apple_atcphy *p, unsigned int pmas, int rate, bool ssc)
{
	static const u32 parameters[4][4] = {
		{ 0x1e0e021c, 0, 0x156600, 0x15671b },
		{ 0x1e0e01c2, 0x07fffffe, 0x155200, 0x155316 },
		{ 0x1e0e01c2, 0x07fffffe, 0x554800, 0x554916 },
		{ 0x1e0e02a3, 0x0bff7ffc, 0x554810, 0x564922 },
	};
	int ret;

	if (!pmas || (pmas & ~3U) || rate < 0 || (unsigned int)rate >= ARRAY_SIZE(parameters))
		return -EINVAL;
	if (p->hw->has_t8140_dp && p->fixed_usb2) {
		if ((p->dp_usb_context_valid & pmas) != pmas)
			return -EIO;
		p->dp_usb_context_dirty |= pmas;
	}
	/* Claim before the first write: failed setup must still be cleaned up. */
	p->dp_owned_pmas = pmas;
	p->dp_link_rate = -1;
	for (unsigned int i = 0; i < 2; i++)
		if (pmas & BIT(i))
			j700_dp_power(p, i);
	core_clear32(p, 0x2234, 3);
	core_mask32(p, 0x7000, 0x1ff0, 0x490);
	for (unsigned int i = 0; i < 3; i++)
		writel(parameters[rate][i == 2 ? 2 + ssc : i], p->regs.core + 0x2080 + 4 * i);
	core_mask32(p, 0x2208, 0x1f0000, 0x70000);
	core_set32(p, 0x2218, 1);
	core_set32(p, 0x2200, BIT(3));
	core_mask32(p, 0x2000, 0x1ffffff9, 0x10000001);
	ret = j700_dp_wait(p, 0x2000, BIT(1), true);
	if (!ret)
		ret = j700_dp_wait(p, 0x7034, BIT(3), true);
	if (ret)
		return ret;
	core_mask32(p, 0x2000, 0x1ffffff9, 0x10014000);
	ret = j700_dp_wait(p, 0x2000, BIT(1), false);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < 2; i++)
		if (pmas & BIT(i))
			core_mask32(p, 0xb064 + i * 0x7000, 0xc, 8);
	core_set32(p, 0x7000, 0xc);
	udelay(1);
	for (unsigned int i = 0; i < 2; i++) {
		if (!(pmas & BIT(i)))
			continue;
		j700_dp_transmitter(p, i, rate);
		j700_dp_receiver(p, i, rate);
	}
	core_clear32(p, 0x7000, 1);
	p->dp_link_rate = rate;
	p->dp_spread_spectrum = ssc;
	dev_dbg(p->dev, "J700 DP main ready: PMAs=%u rate=%d ssc=%u\n", pmas, rate, ssc);
	return 0;
}

static int j700_dp_configure(struct apple_atcphy *p, const struct phy_configure_opts_dp *o)
{
	const struct atcphy_mode_configuration *mode;
	unsigned int pmas, capacity, first, table;
	int rate = -1, ret;

	guard(mutex)(&p->lock);
	if ((o->set_rate && !o->link_rate) || (o->set_lanes && !o->lanes))
		return o->set_voltages ? -EINVAL : j700_dp_stop(p);
	/* The last completed mode cannot describe a partially changed lane route. */
	if (p->mode_dirty || p->pipe_pending || p->usb_off_pending)
		return -EBUSY;
	if (p->mode != APPLE_ATCPHY_MODE_DP && p->mode != APPLE_ATCPHY_MODE_USB3_DP)
		return -EINVAL;
	mode = atcphy_get_mode_config(p, p->mode);
	pmas = !!mode->dp_lane[0] | (!!mode->dp_lane[1] << 1);
	capacity = 2 * hweight32(pmas);
	if (o->lanes > capacity || (o->set_lanes && o->lanes != 1 &&
				    o->lanes != 2 && o->lanes != 4))
		return -EINVAL;
	if (!p->dp_presets_valid[0])
		return -ENOENT;
	if (o->set_voltages) {
		if (!o->lanes)
			return -EINVAL;
		for (unsigned int i = 0; i < o->lanes; i++)
			if (o->voltage[i] > 3 || o->pre[i] > 3)
				return -EINVAL;
	}
	if (o->set_rate) {
		const unsigned int rates[] = { 1620, 2700, 5400, 8100 };

		for (unsigned int i = 0; i < ARRAY_SIZE(rates); i++)
			if (o->link_rate == rates[i])
				rate = i;
		if (rate < 0)
			return -EINVAL;
		if (p->dp_link_rate != rate || p->dp_owned_pmas != pmas ||
		    p->dp_spread_spectrum != o->ssc) {
			ret = j700_dp_stop(p);
			if (ret)
				return ret;
			ret = j700_dp_start(p, pmas, rate, o->ssc);
			if (ret) {
				int cleanup = j700_dp_stop(p);

				dev_err(p->dev, "DP setup failed: %d, cleanup: %d\n", ret, cleanup);
				return ret;
			}
		}
	}
	if (!o->set_voltages)
		return 0;
	if (!p->dp_owned_pmas || p->dp_link_rate < 0)
		return -EINVAL;
	table = p->dp_link_rate + 1;
	if (!p->dp_presets_valid[table])
		table = 0;
	first = pmas == 3 ? !p->swap_lanes : (pmas == 2);
	for (unsigned int i = 0; i < o->lanes; i++)
		j700_dp_equalize(p, first ^ (i / 2), i % 2,
				 p->dp_presets[table][4 * o->voltage[i] + o->pre[i]]);
	return 0;
}

#endif
