/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* T8132 native radio DART power state. The caller owns power and excludes DMA. */
#ifndef __APPLE_DART_POWER_STATE_H
#define __APPLE_DART_POWER_STATE_H

#define DART_POWER_TUNABLES 6
#define DART_POWER_TZ_WORDS 11
#define DART_POWER_ERROR_PENDING 0x4000
#define DART_POWER_ERROR_MASK 0x4060

struct dart_power_state {
	u32 lock, diag, pair[2], version;
	u32 tunable[DART_POWER_TUNABLES];
	u32 tz[DART_POWER_TZ_WORDS];
	bool has_tz, valid;
};

static const struct {
	u32 offset, mask;
} dart_power_tunables[DART_POWER_TUNABLES] = {
	{ 0x20c, 0x80000121 }, { 0x220, 0x000f0f0f }, { 0x224, 0x00ffffff },
	{ 0x300, 0x00001f31 }, { 0x308, 0x3ffffffc }, { 0x310, 0x3ffffffc },
};

static const struct {
	u32 offset, mask;
} dart_power_tz[DART_POWER_TZ_WORDS] = {
	{ 0x500, 7 },	       { 0x504, 1 },	      { 0x508, 0x3ffffffc }, { 0x510, 0x3ffffffc },
	{ 0x518, 0x3ffffffc }, { 0x520, 0x3ffffffc }, { 0x528, 0x3ffffffc }, { 0x530, 0x3ffffffc },
	{ 0x538, 0x3ffffffc }, { 0x540, 0x3ffffffc }, { 0x548, 0x3ffffffc },
};

/* Only v2.2/v2.3, with one 32-stream error bitmap, are supported here. */
static bool dart_power_fault_pending(void __iomem *regs, const struct dart_power_state *s)
{
	return readl(regs + DART_POWER_ERROR_PENDING) ||
	       (s->version >= 0x202 && (readl(regs + 0x704) & 0x777));
}

/* Reserved mask bits need not read back as written; complete the posted write. */
static void dart_power_mask_errors(void __iomem *regs, bool mask)
{
	writel(mask ? ~0U : 0, regs + DART_POWER_ERROR_MASK);
	/* Finish the mask store before the ordering read and stream changes. */
	mb();
	readl(regs + DART_POWER_ERROR_MASK);
}

/* Configuration writes complete and are checked before enabling a stream. */
static int dart_power_write(void __iomem *regs, u32 offset, u32 value)
{
	writel(value, regs + offset);
	return readl(regs + offset) == value ? 0 : -EIO;
}

static int dart_power_config(void __iomem *regs, u32 offset, u32 mask, u32 saved, bool locked)
{
	u32 value = readl(regs + offset);

	if (!((value ^ saved) & mask))
		return 0;
	if (locked)
		return -EUCLEAN;
	return dart_power_write(regs, offset, (value & ~mask) | (saved & mask));
}

static int dart_power_tlb_idle(void __iomem *regs)
{
	u32 value;
	int ret;

	ret = readl_poll_timeout_atomic(regs + 0x80, value, !(value & BIT(31)), 1, 10000);
	return ret ?: readl_poll_timeout_atomic(regs + 0x604, value, !(value & BIT(0)), 1, 10000);
}

static int dart_power_flush(void __iomem *regs, bool off)
{
	int ret = dart_power_tlb_idle(regs);

	if (ret)
		return ret;
	writel(off ? 0xa000 : 0x8000, regs + 0x80);
	return dart_power_tlb_idle(regs);
}

#endif
