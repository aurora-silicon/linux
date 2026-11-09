// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define BITS_TO_U32(n) (((n) + 31) / 32)
#define DART_MAX_STREAMS 32
#define DART_MAX_TTBR 4
struct device {
	int unused;
};
struct dart_power_state {
	int unused;
}; /* Power replay is covered separately. */
struct hw_info {
	unsigned int ttbr_count;
	u32 lock, tcr, ttbr, ttbr_valid, tcr_4level, enable_streams;
};
struct apple_dart {
	struct device *dev;
	struct hw_info *hw;
	bool retained_mtp, four_level, managed_failed;
	unsigned int managed_sids, num_streams, version;
};
static u32 regs[0x5000 / 4];
static bool irqoff, fault;
static unsigned int logs;
static bool irqs_disabled(void)
{
	return irqoff;
}
static void dev_err(struct device *d, const char *fmt, ...)
{
	(void)d;
	(void)fmt;
	logs++;
}
static u32 apple_dart_readl(struct apple_dart *d, u32 off)
{
	(void)d;
	assert(off < sizeof(regs) && !(off & 3));
	return regs[off / 4];
}
static bool apple_dart_fault_pending(struct apple_dart *d)
{
	(void)d;
	return fault;
}

/* ACTUAL_PRODUCTION */
int main(void)
{
	struct hw_info hw = { .ttbr_count = 1,
			      .lock = DART_T8110_PROTECT,
			      .tcr = DART_T8110_TCR,
			      .ttbr = DART_T8110_TTBR,
			      .ttbr_valid = DART_T8110_TTBR_VALID,
			      .tcr_4level = DART_T8110_TCR_FOUR_LEVEL,
			      .enable_streams = DART_T8110_ENABLE_STREAMS };
	struct apple_dart d = { .hw = &hw,
				.four_level = true,
				.version = 0x0203,
				.managed_sids = 6,
				.num_streams = DART_MAX_STREAMS };
	struct apple_dart_sleep_state s = { .dart = &d };

	regs[hw.enable_streams / 4] = 6;
	for (unsigned int sid = 1; sid <= 2; sid++) {
		regs[DART_TCR(&d, sid) / 4] =
			9; /* Captured four-level translation. */
		regs[DART_TTBR(&d, sid, 0) / 4] = 0x12340001 + 4 * sid;
	}
	assert(apple_dart_sleep_snapshot(&s, false) == 0);
	assert(apple_dart_sleep_snapshot(&s, true) == 0);
	assert(logs == 0);
	/* Each retained value must stay exact, including the table-depth bit. */
	unsigned int changed[] = {
		DART_TCR(&d, 1),	 DART_TCR(&d, 2),
		DART_TTBR(&d, 1, 0),	 DART_TTBR(&d, 2, 0),
		DART_TCR(&d, 0),	 DART_TTBR(&d, 0, 0),
		hw.enable_streams,	 DART_T8110_PROTECT,
		DART_T8110_PROTECT_LOCK, DART_T8110_V2_ERROR_MASK
	};
	irqoff = true;
	for (unsigned int i = 0; i < ARRAY_SIZE(changed); i++) {
		regs[changed[i] / 4] ^= 8;
		assert(apple_dart_sleep_snapshot(&s, true) == -EUCLEAN);
		regs[changed[i] / 4] ^= 8;
		assert(logs == 0);
	}
	irqoff = false;
	d.four_level = false;
	assert(apple_dart_sleep_snapshot(&s, false) == -EUCLEAN);
	for (unsigned int sid = 1; sid <= 2; sid++)
		regs[DART_TCR(&d, sid) / 4] = 1;
	assert(apple_dart_sleep_snapshot(&s, false) == 0);
	d.four_level = true;
	assert(apple_dart_sleep_snapshot(&s, false) == 0);
	/* Disabled, bypass, remap, unknown bits, and missing roots still fail. */
	unsigned int invalid[] = {
		0, 8, 2, 3, 5, 7, 0xb, 0xd, 0x81, 0x101, 0x11
	};

	for (unsigned int sid = 1; sid <= 2; sid++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(invalid); i++) {
			regs[DART_TCR(&d, sid) / 4] = invalid[i];
			assert(apple_dart_sleep_snapshot(&s, false) ==
			       -EUCLEAN);
		}
		regs[DART_TCR(&d, sid) / 4] = 9;
		regs[DART_TTBR(&d, sid, 0) / 4] &= ~DART_T8110_TTBR_VALID;
		assert(apple_dart_sleep_snapshot(&s, false) == -EUCLEAN);
		regs[DART_TTBR(&d, sid, 0) / 4] |= DART_T8110_TTBR_VALID;
	}
	assert(apple_dart_sleep_snapshot(&s, false) == 0);
	for (unsigned int version = 0x0201; version <= 0x0203; version++) {
		unsigned int mask = version >= 0x0202 ?
					    DART_T8110_V2_ERROR_MASK :
					    DART_T8110_ERROR_MASK;
		unsigned int other = version >= 0x0202 ?
					     DART_T8110_ERROR_MASK :
					     DART_T8110_V2_ERROR_MASK;
		d.version = version;
		regs[mask / 4] = 1;
		assert(apple_dart_sleep_snapshot(&s, false) == -EUCLEAN);
		regs[mask / 4] = 0;
		regs[other / 4] = ~0U;
		assert(apple_dart_sleep_snapshot(&s, false) == 0);
		regs[other / 4] = 0;
	}
	regs[DART_T8110_PROTECT / 4] = DART_T8110_PROTECT_TTBR_TCR;
	assert(apple_dart_sleep_snapshot(&s, false) == -EUCLEAN);
	regs[DART_T8110_PROTECT / 4] = 0;
	fault = true;
	assert(apple_dart_sleep_snapshot(&s, false) == -EBUSY);
	fault = false;
	regs[DART_T8110_TLB_CMD / 4] = DART_T8110_TLB_CMD_BUSY;
	assert(apple_dart_sleep_snapshot(&s, false) == -EBUSY);
	regs[DART_T8110_TLB_CMD / 4] = 0;
	d.managed_failed = true;
	assert(apple_dart_sleep_snapshot(&s, false) == -EUCLEAN);
	d.managed_failed = false;
	assert(apple_dart_sleep_snapshot(&s, false) == 0);
	assert(apple_dart_sleep_snapshot(&s, true) == 0);
	puts("PASS: DART table depths, corrupt roots, faults and snapshot validation");
}
