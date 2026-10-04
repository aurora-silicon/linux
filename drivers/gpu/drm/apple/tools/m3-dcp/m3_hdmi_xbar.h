/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* M4 T602x sequencer adapted to captured J514S ATC3 routing. Native
 * 26A428 connect/up/down model confirms source8000 -> destination8030.
 * Status mirrors in that offline model do not prove real clock readiness.
 */
#ifndef M3_HDMI_DPXBAR_H
#define M3_HDMI_DPXBAR_H
/* Caller holds its device lock and a runtime PM reference throughout each
 * operation. selected records an attempted route until a verified idle state;
 * it is deliberately retained after uncertain writes or failed shutdown. */
struct m3_hdmi_dpxbar {
	void __iomem *regs;
	bool selected;
	/* J514S DCPEXT0 UFP endpoint0/port0 maps to selector0. */
	u32 selector;
};
static const u32 dpxbar_offsets[] = { 0, 4, 8, 0xc, 0x10, 0x14, 0x18,
	0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34,
	0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c };
static const u32 dpxbar_idle[] = { 0, 0x1ff, 0, 0, 0, 0x1ff, 0,
	0, 0, 0x111, 0, 0, 0, 0, 0, 0x1ff, 0, 0, 0x1ff, 0, 0, 0x111 };

static int m3_hdmi_dpxbar_idle(struct m3_hdmi_dpxbar *xbar)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(dpxbar_offsets); i++) {
		u32 value = readl(xbar->regs + dpxbar_offsets[i]);
		if (dpxbar_offsets[i] == 0x34 && value == 0x100)
			continue;
		if (value != dpxbar_idle[i]) {
			pr_err("m3_hdmi_dpxbar: idle mismatch offset=%#x value=%#x expected=%#x\n",
				dpxbar_offsets[i], value, dpxbar_idle[i]);
			return -EBUSY;
		}
	}
	return 0;
}

static void m3_hdmi_dpxbar_update(struct m3_hdmi_dpxbar *xbar, u32 offset, u32 clear, u32 set)
{
	writel((readl(xbar->regs + offset) & ~clear) | set,
	       xbar->regs + offset);
}

static int m3_hdmi_dpxbar_up(struct m3_hdmi_dpxbar *xbar)
{
	u32 selector = xbar->selector;
	u32 bit, shift;
	int ret;

	if (selector != 0 && selector != 2)
		return -EINVAL;
	bit = 1U << selector;
	shift = 2 * selector;
	ret = m3_hdmi_dpxbar_idle(xbar);
	if (ret)
		return ret;
	/* Caller already pins the powered PHY and published DART tables. */
	xbar->selected = true;
	m3_hdmi_dpxbar_update(xbar, 0x30, 0xf00000, selector << 20);
	m3_hdmi_dpxbar_update(xbar, 0x30, 0xf00, selector << 8);
	if (readl(xbar->regs + 0x30) != (selector << 20 | selector << 8))
		return -EIO;
	/* Native connection-up uses source bit 1 and physical-output bit 0x100.
	 * Clock class 1 follows native pclkForConnection for attributes 0x100.
	 */
	m3_hdmi_dpxbar_update(xbar, 4, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0x14, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0x24, 0x100, 0);
	udelay(1);
	if (readl(xbar->regs + 0x804) & bit ||
	    readl(xbar->regs + 0x810) & bit ||
	    readl(xbar->regs + 0x81c) & 0x100)
		return -ETIMEDOUT;
	m3_hdmi_dpxbar_update(xbar, 8, 0, bit);
	m3_hdmi_dpxbar_update(xbar, 0x18, 3U << shift, 1U << shift);
	m3_hdmi_dpxbar_update(xbar, 0x28, 0x300, 0x100);
	m3_hdmi_dpxbar_update(xbar, 0, 0, bit);
	m3_hdmi_dpxbar_update(xbar, 0xc, 0, bit);
	m3_hdmi_dpxbar_update(xbar, 0x1c, 0, 0x100);
	m3_hdmi_dpxbar_update(xbar, 0x34, 0, 0x100);
	m3_hdmi_dpxbar_update(xbar, 0x2c, 0, bit);
	pr_info("m3_hdmi_dpxbar: selector%u up mux=%#x source_clock=%#x output_clock=%#x\n",
		selector, readl(xbar->regs + 0x30), readl(xbar->regs),
		readl(xbar->regs + 0x1c));
	if (readl(xbar->regs) != bit || readl(xbar->regs + 4) != (0x1ff & ~bit) ||
	    readl(xbar->regs + 8) != bit || readl(xbar->regs + 0xc) != bit ||
	    readl(xbar->regs + 0x14) != (0x1ff & ~bit) ||
	    readl(xbar->regs + 0x18) != (1U << shift) ||
	    readl(xbar->regs + 0x1c) != 0x100 ||
	    readl(xbar->regs + 0x24) != 0x11 ||
	    readl(xbar->regs + 0x28) != 0x100 ||
	    readl(xbar->regs + 0x2c) != bit ||
	    readl(xbar->regs + 0x34) != 0x100)
		return -EIO;
	return 0;
}

static int m3_hdmi_dpxbar_down(struct m3_hdmi_dpxbar *xbar)
{
	u32 source, read_clock, output;
	u32 selector = xbar->selector;
	u32 bit, shift;
	if (!xbar->selected)
		return 0;
	if (selector != 0 && selector != 2)
		return -EINVAL;
	bit = 1U << selector;
	shift = 2 * selector;
	m3_hdmi_dpxbar_update(xbar, 0x2c, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0xc, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0x1c, 0x100, 0);
	udelay(1);
	source = readl(xbar->regs + 0x800);
	read_clock = readl(xbar->regs + 0x808);
	output = readl(xbar->regs + 0x814);
	/* Native takeConnectionDown logs these intermediate samples and
	 * continues with inverse-clock gates below (9135918..9135a08).
	 * They are not its completion condition. Require the full idle
	 * snapshot after those writes; never release on a stale final status.
	 */
	pr_info("m3_hdmi_dpxbar: intermediate stop status source=%#x read=%#x output=%#x\n",
		source, read_clock, output);
	m3_hdmi_dpxbar_update(xbar, 8, bit, 0);
	m3_hdmi_dpxbar_update(xbar, 0x18, 3U << shift, 0);
	m3_hdmi_dpxbar_update(xbar, 0x28, 0x300, 0);
	m3_hdmi_dpxbar_update(xbar, 4, 0, bit);
	m3_hdmi_dpxbar_update(xbar, 0x14, 0, bit);
	m3_hdmi_dpxbar_update(xbar, 0x24, 0, 0x100);
	/* Native takeConnectionDown leaves bit8 at0x34 set. Hardware in the
	 * retained trial ignores a zero write; do not require an unnecessary
	 * recovery for this native parked value. Clocks/status must be stopped,
	 * and subsequent RTKit AP+IOP quiescence still gates DART teardown.
	 * Restore the reversible selector fields after clocks stop.
	 */
	m3_hdmi_dpxbar_update(xbar, 0x30, 0xf00f00, 0);
	if (m3_hdmi_dpxbar_idle(xbar))
		return -EIO;
	xbar->selected = false;
	pr_info("m3_hdmi_dpxbar: clocks stopped and selector restored; native parked register0x34=%#x\n",
		readl(xbar->regs + 0x34));
	return 0;
}

#endif
