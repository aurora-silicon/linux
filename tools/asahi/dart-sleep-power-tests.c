// SPDX-License-Identifier: GPL-2.0-only
/* Actual native DART code; the model drops registers on controller power loss. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & ((~0U) >> (31 - (h))))
#define BITS_TO_U32(n) (((n) + 31) / 32)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
/* Single-threaded fixture: model publication without CPU barriers. */
#define smp_store_release(p, v) (*(p) = (v))
#define dma_wmb() ((void)0)
/* The MMIO mock records write order synchronously. */
#define mb() ((void)0)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define FIELD_GET(mask, v) (((v) & (mask)) >> __builtin_ctz(mask))
#define DART_PARAMS2 4
#define DART_T8110_PARAMS2_READ_PORTS GENMASK(18, 16)
#define DART_T8110_PARAMS2_WRITE_PORTS GENMASK(22, 20)
#define DART_T8110_OUTSTANDING(n) (0x230 + 4 * (n))
#define DART_T8110_IDLE_TIMEOUT_US 1000
#define DART_MAX_STREAMS 32
#define DART_MAX_TTBR 4
#define DART_T8110 2
#define DART_T8110_PROTECT 0x200
#define DART_T8110_PROTECT_LOCK 0x208
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)
#define DART_T8110_V2_ERROR_MASK 0x4060
#define DART_T8110_DISABLE_STREAMS 0xc20
#define APPLE_PS_PROT_SLICE(n) (0x100 + 0x40 * (n))
#define DART_TCR(d, sid) ((void)(d), 0x1000 + 4 * (sid))
#define DART_TTBR(d, sid, idx) ((void)(d), 0x1400 + 4 * (sid) + 4 * (idx))
struct device {
	int unused;
};
struct regmap {
	u32 word;
};
struct apple_dart_hw {
	unsigned int type, ttbr_count, enable_streams;
};
struct apple_dart {
	struct device *dev;
	struct apple_dart_hw *hw;
	void *regs;
	unsigned int num_streams, version, managed_sids;
	bool sleep_closed, commands_gated;
};
struct apple_dart_sleep {
	void *private;
	bool active, sealed, verified;
	u64 generation;
	int result;
};
static u32 regs[0x10000 / 4];
static bool critical;
static unsigned int writes, enabled_at, last_translation, flush_at;
static unsigned int drop_offset;
static unsigned int offset(const void *p)
{
	return (const char *)p - (const char *)regs;
}
static u32 readl(const void *p)
{
	assert(offset(p) < sizeof(regs));
	return *(const u32 *)p;
}
static void writel(u32 v, void *p)
{
	unsigned int off = offset(p);

	assert(off < sizeof(regs));

	writes++;
	if (off == drop_offset)
		return;
	if (off >= 0xc20 && off < 0xc24) {
		regs[0xc00 / 4] &= ~v;
		return;
	}
	if (off == 0xc00) {
		regs[off / 4] |= v;
		if (v)
			enabled_at = writes;
		return;
	}
	if (off == 0x4060) {
		regs[off / 4] = v & 1;
		return;
	}
	if (off >= 0x1000 && off < 0x1500)
		last_translation = writes;
	if (off == 0x80)
		flush_at = writes;
	regs[off / 4] = v;
}
#define readl_poll_timeout_atomic(p, v, cond, delay, timeout)      \
	({                                                         \
		(void)(delay);                                     \
		(void)(timeout);                                   \
		int ret = -ETIMEDOUT;                              \
		for (unsigned int tries = 0; tries < 4; tries++) { \
			v = readl(p);                              \
			if (cond) {                                \
				ret = 0;                           \
				break;                             \
			}                                          \
		}                                                  \
		ret;                                               \
	})
static int regmap_read(struct regmap *m, u32 off, u32 *v)
{
	assert(off == APPLE_PS_PROT_SLICE(28));
	*v = m->word;
	return 0;
}
static int regmap_update_bits(struct regmap *m, u32 off, u32 mask, u32 v)
{
	assert(off == APPLE_PS_PROT_SLICE(28));
	m->word = (m->word & ~mask) | (v & mask);
	return 0;
}
static u32 apple_dart_readl(struct apple_dart *d, u32 off)
{
	return readl(d->regs + off);
}
static void apple_dart_writel(struct apple_dart *d, u32 v, u32 off)
{
	writel(v, d->regs + off);
}
static bool apple_dart_fault_pending(struct apple_dart *d)
{
	(void)d;
	return regs[0x4000 / 4] || (regs[0x704 / 4] & 0x777);
}

static bool apple_dart_sleep_critical(void)
{
	return critical;
}
static bool irqs_disabled(void)
{
	return critical;
}
static bool in_interrupt(void)
{
	return false;
}
#include "apple-dart-power.h"
/* ACTUAL_STATE */
static int apple_dart_sleep_valid(struct apple_dart_sleep *r)
{
	struct apple_dart_sleep_state *s = r->private;

	return !r->active || s->record != r || s->generation != r->generation ?
		       -ESTALE :
		       r->result;
}
static int apple_dart_sleep_snapshot(struct apple_dart_sleep_state *s,
				     bool verify)
{
	assert(verify);
	for (unsigned int sid = 0; sid < s->dart->num_streams; sid++)
		if (regs[(0x1000 + 4 * sid) / 4] != s->tcr[sid] ||
		    regs[(0x1400 + 4 * sid) / 4] != s->ttbr[sid][0])
			return -EUCLEAN;
	return 0;
}
#define CONFIG_ARM64 1
#define USEC_PER_SEC 1000000
static u64 tick;
static bool stopped_counter, backward_counter;
static u64 arch_timer_read_counter(void)
{
	if (backward_counter)
		return tick--;
	return stopped_counter ? tick : (tick += 1000);
}
static u32 arch_timer_get_cntfrq(void)
{
	return 1000000;
}
static void cpu_relax(void)
{
}
/* ACTUAL_SPIN */
/* ACTUAL_FUNCTIONS */
static struct apple_dart_hw hw = { DART_T8110, 1, 0xc00 };
static struct apple_dart dart;
static struct apple_dart_sleep record;
static struct apple_dart_sleep_state state;
static struct regmap gapf;
static void fresh(void)
{
	memset(regs, 0, sizeof(regs));
	memset(&state, 0, sizeof(state));
	dart = (struct apple_dart){ .hw = &hw,
				    .regs = regs,
				    .num_streams = 19,
				    .version = 0x203,
				    .managed_sids = 6,
				    .sleep_closed = true };
	record = (struct apple_dart_sleep){ .private = &state,
					    .active = true,
					    .sealed = true,
					    .generation = 9 };
	state.dart = &dart;
	state.record = &record;
	state.generation = 9;
	state.gapf = &gapf;
	gapf.word = 0x812;
	state.enabled[0] = regs[0xc00 / 4] = 6 | BIT(16) | BIT(18);
	for (unsigned int sid = 0; sid < 19; sid++) {
		state.tcr[sid] = regs[(0x1000 + 4 * sid) / 4] =
			sid == 1 || sid == 2   ? 9 :
			sid == 16 || sid == 18 ? 2 :
						 0;
		state.ttbr[sid][0] = regs[(0x1400 + 4 * sid) / 4] =
			sid == 1 || sid == 2 ? 0x12340001 + sid * 4 : 0;
	}
	regs[0x210 / 4] = 0x123;
	regs[0x228 / 4] = 0x42;
	regs[0x22c / 4] = 0x4000000;
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++)
		regs[dart_power_tunables[i].offset / 4] =
			dart_power_tunables[i].mask & 0x81080809;
	regs[4 / 4] = 1U << 16;
	stopped_counter = backward_counter = false;
	tick = 0;
	critical = false;
	writes = enabled_at = last_translation = flush_at = 0;
	drop_offset = ~0U;
}
static void power_off(void)
{
	assert(!apple_dart_sleep_power_capture(&state));
	assert(!apple_dart_sleep_power_ready(&record));
	critical = false;
	assert(!apple_dart_sleep_power_off(&record));
	assert(dart.commands_gated && dart.sleep_closed && !record.verified);
	assert(!regs[0xc00 / 4] && gapf.word == 0x802);
	critical = false;
}
int main(void)
{
	fresh();
	assert(apple_dart_sleep_power_ready(&record) == -ENODATA);
	assert(!writes);
	fresh();
	assert(!apple_dart_sleep_power_capture(&state));
	critical = true;
	assert(apple_dart_sleep_power_off(&record) == -EPERM);
	assert(!writes);
	fresh();
	gapf.word &= ~BIT(1);
	assert(apple_dart_sleep_power_capture(&state) == -EPERM);
	assert(!writes);
	/* Trial246: active radio DART inherited GAPF bit1, with bit4 clear. */
	for (unsigned int extra = 0; extra <= 0x800; extra += 0x800) {
		fresh();
		gapf.word = BIT(1) | extra;
		assert(!apple_dart_sleep_power_capture(&state));
		assert(!writes);
		critical = false;
		assert(!apple_dart_sleep_power_off(&record));
		critical = false;
		assert(gapf.word == (BIT(1) | extra));
		memset(regs, 0, sizeof(regs));
		assert(!apple_dart_sleep_power_restore(&record));
		assert(gapf.word == (BIT(1) | extra));
		assert(record.verified && !dart.sleep_closed &&
		       !dart.commands_gated);
	}
	fresh();
	dart.version = 0x204;
	assert(apple_dart_sleep_power_capture(&state) == -EOPNOTSUPP);
	assert(!writes);
	fresh();
	power_off();
	memset(regs, 0, sizeof(regs));
	assert(!apple_dart_sleep_power_restore(&record));
	assert(record.verified && !dart.sleep_closed && !dart.commands_gated);
	assert(regs[0xc00 / 4] == (6 | BIT(16) | BIT(18)) &&
	       gapf.word == 0x812);
	assert(regs[0x104 / 4] == 0 && regs[0x4060 / 4] == 0);
	assert(enabled_at > flush_at && flush_at > last_translation);
	assert(regs[(0x1000 + 4 * 16) / 4] == 2 &&
	       regs[(0x1000 + 4 * 18) / 4] == 2);
	assert(apple_dart_sleep_power_restore(&record) == -EPERM);
	fresh();
	power_off();
	memset(regs, 0, sizeof(regs));
	drop_offset = 0x1408;
	assert(apple_dart_sleep_power_restore(&record) == -EIO);
	assert(!regs[0xc00 / 4] && dart.commands_gated && dart.sleep_closed &&
	       !record.verified);
	fresh();
	power_off();
	memset(regs, 0, sizeof(regs));
	regs[0x200 / 4] = regs[0x208 / 4] = 1;
	assert(apple_dart_sleep_power_restore(&record) == -EUCLEAN);
	assert(!regs[0xc00 / 4]);
	fresh();
	power_off();
	regs[0x4000 / 4] = 1;
	assert(apple_dart_sleep_power_restore(&record) == -EBUSY);
	assert(!regs[0xc00 / 4]);
	fresh();
	power_off();
	gapf.word &= ~BIT(1);
	writes = 0;
	assert(apple_dart_sleep_power_restore(&record) == -EPERM);
	assert(!writes);
	fresh();
	assert(!apple_dart_sleep_power_capture(&state));
	critical = false;
	regs[0x80 / 4] = BIT(31);
	assert(apple_dart_sleep_power_off(&record) == -ETIMEDOUT);
	assert(!dart.commands_gated && dart.sleep_closed && !record.verified);
	fresh();
	assert(!apple_dart_sleep_power_capture(&state));
	critical = false;
	regs[0x230 / 4] = 1;
	assert(apple_dart_sleep_power_off(&record) == -ETIMEDOUT);
	assert(!writes);
	fresh();
	assert(!apple_dart_sleep_power_capture(&state));
	critical = false;
	regs[4 / 4] = 0;
	assert(apple_dart_sleep_power_off(&record) == -EIO);
	assert(!writes);
	fresh();
	stopped_counter = true;
	regs[0x80 / 4] = BIT(31);
	assert(apple_sleep_spin(regs + 0x80 / 4, BIT(31), 0, 1000) ==
	       -ETIMEDOUT);
	fresh();
	tick = 10;
	backward_counter = true;
	assert(apple_sleep_spin(NULL, 0, 0, 1000) == -EIO);
	puts("PASS: DART power loss, static streams, restore ordering and failures");
}
