// SPDX-License-Identifier: GPL-2.0-only
/* Fault-injected register model; no hardware accesses. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctz(mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define max(a, b) ((a) > (b) ? (a) : (b))
/* The MMIO mock records write order synchronously. */
#define mb() \
	do { \
	} while (0)
static u32 regs[0x9000 / 4];
static unsigned int reads, writes, drop, busy, flushes, enables,
	partition_reads;
static unsigned int trace[4096], ntrace;
static unsigned int offset(const void *p)
{
	uintptr_t off = (uintptr_t)p - (uintptr_t)regs;

	assert(off < sizeof(regs) && !(off & 3));
	return off;
}
static u32 readl(const void *p)
{
	unsigned int off = offset(p);

	reads++;
	assert(off != 0x100 && off != 0x104 && off < 0x6000);
	if (off >= 0x800 && off < 0x840)
		partition_reads++;
	if (off == 0x80 && busy == 1)
		return BIT(31);
	if (off == 0x604 && busy == 2)
		return 1;
	if (off == 0x230 && busy == 3)
		return 1;
	return regs[off / 4];
}
static void writel(u32 value, void *p)
{
	unsigned int off = offset(p);

	writes++;
	assert(ntrace < ARRAY_SIZE(trace));
	trace[ntrace++] = off;
	assert(off != 0x100 && off != 0x104 && off != 0x200 && off != 0x208);
	assert(off != 0x700 && off != 0x704 && off != 0x720);
	assert(!(off >= 0x500 && off <= 0x548));
	if (off == drop)
		return;
	if (off == 0x4060) {
		/* Only implemented mask bits retain the write. */
		regs[off / 4] = value & 0xff;
	} else if (off >= 0xc20 && off < 0xc40) {
		assert(value && !(value & (value - 1)));
		regs[(off - 0x20) / 4] &= ~value;
	} else if (off == 0x80) {
		assert(value == 0xa000 || value == 0x8000);
		assert(!regs[0xc00 / 4] && !regs[0xc1c / 4]);
		flushes++;
	} else {
		if (off >= 0xc00 && off < 0xc20) {
			assert(flushes && !regs[0x4060 / 4]);
			enables++;
		}
		regs[off / 4] = value;
	}
}
#define readl_poll_timeout_atomic(addr, value, condition, delay, timeout) \
	({                                                                \
		(void)(delay);                                            \
		(void)(timeout);                                          \
		int result = -ETIMEDOUT;                                  \
		for (unsigned int tries = 0; tries < 4; tries++) {        \
			value = readl(addr);                              \
			if (condition) {                                  \
				result = 0;                               \
				break;                                    \
			}                                                 \
		}                                                         \
		result;                                                   \
	})

/* ACTUAL_HEADERS */

static void reset(unsigned int streams, bool partitions)
{
	memset(regs, 0, sizeof(regs));
	reads = writes = busy = flushes = enables = partition_reads = ntrace =
		0;
	drop = ~0U;
	regs[0] = partitions ? BIT(29) : 0;
	regs[1] = BIT(16) | BIT(20) | BIT(2);
	regs[2] = 0x203;
	regs[3] = streams | (16 << 16);
	regs[0x208 / 4] = BIT(3) | BIT(4);
	regs[0x210 / 4] = 1;
	regs[0x228 / 4] = 0x1122;
	regs[0x22c / 4] = 0x3344;
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++)
		regs[dart_power_tunables[i].offset / 4] =
			dart_power_tunables[i].mask;
	for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
		regs[dart_power_tz[i].offset / 4] = dart_power_tz[i].mask;
	for (unsigned int i = 0; i < 16; i++)
		regs[(0x800 + 4 * i) / 4] = 0xa0000000 | 0x20000 | (2 * i);
	for (unsigned int sid = 0; sid < streams; sid++) {
		regs[(0x1000 + 4 * sid) / 4] =
			(sid == 253 || sid == 255) ? BIT(8) : BIT(0);
		regs[(0x1400 + 4 * sid) / 4] =
			(sid == 253 || sid == 255) ? 0 : BIT(31) | sid;
		regs[(0xc00 + 4 * (sid / 32)) / 4] |= BIT(sid % 32);
	}
}
static void lose_power(void)
{
	u32 params[4], tz[DART_POWER_TZ_WORDS], lock = regs[0x208 / 4];

	memcpy(params, regs, sizeof(params));
	for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
		tz[i] = regs[dart_power_tz[i].offset / 4];
	memset(regs, 0, sizeof(regs));
	memcpy(regs, params, sizeof(params));
	for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
		regs[dart_power_tz[i].offset / 4] = tz[i];
	regs[0x208 / 4] = lock;
	flushes = enables = ntrace = 0;
}
static void check_restored(const struct dart_cio_power *s)
{
	for (unsigned int sid = 0; sid < s->streams; sid++) {
		assert(regs[(0x1000 + 4 * sid) / 4] == s->tcr[sid]);
		assert(regs[(0x1400 + 4 * sid) / 4] == s->ttbr[sid]);
	}
	for (unsigned int i = 0; i < DIV_ROUND_UP(s->streams, 32); i++)
		assert(regs[(0xc00 + 4 * i) / 4] == s->enabled[i]);
	for (unsigned int i = 0; i < s->partitions; i++)
		assert((regs[(0x800 + 4 * i) / 4] & DART_CIO_PARTITION_MASK) ==
		       (s->partition[i] & DART_CIO_PARTITION_MASK));
	unsigned int flush = 0, pair = 0, enable = 0;

	for (unsigned int i = 0; i < ntrace; i++) {
		if (trace[i] == 0x80)
			flush = i;
		if (trace[i] == 0x228)
			pair = i;
		if (trace[i] == 0xc00)
			enable = i;
	}
	assert(flush < enable && (!pair || (flush < pair && pair < enable)));
}
static int restore(struct dart_cio_power *s)
{
	ntrace = flushes = enables = 0;
	return dart_cio_restore(regs, s);
}
int main(void)
{
	struct dart_cio_power s = {};
	unsigned int cases = 0;
	/* The live T8132 PCIe-C bank reports 256 SIDs, not the allocator's 64. */
	reset(256, false);
	regs[0] = 0xae313020;
	regs[1] = 0x34110083;
	regs[2] = 0x2a2a0202;
	regs[3] = 0x00100100;
	regs[0x208 / 4] = 0;
	assert(dart_cio_capture(regs, &s, 64, false) == -EOPNOTSUPP && !writes);
	assert(!dart_cio_capture(regs, &s, 256, false) && !s.common.has_tz);
	assert(!dart_cio_disable(regs, &s));
	lose_power();
	assert(!restore(&s));
	check_restored(&s);
	cases++;
	for (unsigned int streams = 28; streams <= 256; streams += 228) {
		bool parts = streams == 28;

		for (unsigned int version = 0x202; version <= 0x203;
		     version++) {
			reset(streams, parts);
			regs[2] = version;
			assert(!dart_cio_capture(regs, &s, streams, parts) &&
			       s.valid && !writes);
			assert(partition_reads == (parts ? 16 : 0));
			assert(!dart_cio_disable(regs, &s));
			for (unsigned int i = 0; i < DIV_ROUND_UP(streams, 32);
			     i++)
				assert(!regs[(0xc00 + 4 * i) / 4]);
			lose_power();
			assert(!restore(&s));
			check_restored(&s);
			cases++;
			assert(!dart_cio_capture(regs, &s, streams, parts));
			assert(!dart_cio_disable(regs, &s));
			/* Abort without losing power also restores the saved context. */
			assert(!restore(&s));
			check_restored(&s);
			cases++;
		}
	}
	for (unsigned int i = 0; i < 8; i++) {
		reset(256, false);
		regs[(0x4000 + 4 * i) / 4] = BIT(i);
		assert(dart_cio_capture(regs, &s, 256, false) == -EIO &&
		       !writes && !s.valid);
		cases++;
	}
	for (unsigned int kind = 0; kind < 9; kind++) {
		reset(28, true);
		switch (kind) {
		case 0:
			regs[2] = 0x201;
			break;
		case 1:
			regs[2] = 0x204;
			break;
		case 2:
			regs[0] = 0;
			break;
		case 3:
			regs[3] = 28 | (15 << 16);
			break;
		case 4:
			regs[3] = 27 | (16 << 16);
			break;
		case 5:
			regs[0x704 / 4] = 0x100;
			break;
		case 6:
			regs[0x4060 / 4] = 1;
			break;
		case 7:
			regs[0x208 / 4] = 0;
			break;
		case 8:
			regs[0xc00 / 4] |= BIT(31);
			break;
		}
		assert(dart_cio_capture(regs, &s, 28, true) < 0 && !writes &&
		       !s.valid);
		cases++;
	}
	for (unsigned int kind = 1; kind <= 3; kind++) {
		reset(28, true);
		busy = kind;
		assert(dart_cio_capture(regs, &s, 28, true) == -ETIMEDOUT &&
		       !writes);
		cases++;
	}
	const unsigned int badwrites[] = { 0x210,
					   0x20c,
					   0x220,
					   0x224,
					   0x300,
					   0x308,
					   0x310,
					   0x800,
					   0x83c,
					   0x1000,
					   0x1000 + 4 * 255,
					   0x1400,
					   0x228,
					   0x22c,
					   0xc00,
					   0xc1c };
	for (unsigned int i = 0; i < ARRAY_SIZE(badwrites); i++) {
		reset(256, true);
		assert(!dart_cio_capture(regs, &s, 256, true));
		assert(!dart_cio_disable(regs, &s));
		lose_power();
		drop = badwrites[i];
		assert(restore(&s) == -EIO);
		assert(!regs[0xc00 / 4] && !regs[0xc1c / 4]);
		drop = ~0U;
		assert(!restore(&s));
		check_restored(&s);
		cases++;
	}
	for (unsigned int kind = 0; kind < 5; kind++) {
		reset(256, true);
		assert(!dart_cio_capture(regs, &s, 256, true));
		assert(!dart_cio_disable(regs, &s));
		lose_power();
		if (kind == 0)
			regs[0x208 / 4] ^= BIT(3);
		if (kind == 1)
			regs[0x500 / 4] ^= 1;
		if (kind == 2)
			regs[3] ^= 1;
		if (kind == 3)
			regs[0x4000 / 4] = 1;
		if (kind == 4) {
			regs[0x200 / 4] = 1;
			regs[0x208 / 4] |= 1;
		}
		assert(restore(&s) == -EUCLEAN && !enables);
		cases++;
	}
	reset(28, true);
	assert(!dart_cio_capture(regs, &s, 28, true));
	busy = 1;
	assert(dart_cio_disable(regs, &s) == -ETIMEDOUT);
	busy = 0;
	assert(!restore(&s));
	check_restored(&s);
	cases++;
	reset(28, true);
	regs[0xc00 / 4] &= ~BIT(3);
	assert(!dart_cio_capture(regs, &s, 28, true));
	regs[0xc00 / 4] |= BIT(3);
	unsigned int oldwrites = writes;

	assert(restore(&s) == -EUCLEAN && writes == oldwrites);
	cases++;
	s.valid = false;
	unsigned int before = reads + writes;

	assert(dart_cio_disable(regs, &s) == -ENODATA);
	assert(dart_cio_restore(regs, &s) == -ENODATA &&
	       reads + writes == before);
	cases++;
	printf("PASS %u CIO DART power cases\n", cases);
}
