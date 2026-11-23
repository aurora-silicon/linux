// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SoC read-only register views.
 *
 * The register windows under test are zeroed buffers of kernel memory that
 * stand in for the mapped DT reg entries of a struct apple_regview. The tests
 * call the driver's own bounds check and seq_file show routine on the
 * driver's own register lists. No device is probed and no MMIO is touched.
 */
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "regview-internal.h"

#define REGVIEW_TEST_MAX_REGS	4
#define REGVIEW_TEST_SEQ_SIZE	(16 * 1024)

struct regview_test_ctx {
	struct apple_regview rv;
	struct apple_regview_map maps[REGVIEW_TEST_MAX_REGS];
	void *buf[REGVIEW_TEST_MAX_REGS];
	char *seq_buf;
	size_t seq_len;
};

KUNIT_DEFINE_ACTION_WRAPPER(regview_test_vfree, vfree, const void *);

static const struct apple_regview_hw *regview_test_hw(const char *compatible)
{
	const struct of_device_id *id;

	for (id = apple_regview_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, compatible))
			return id->data;
	return NULL;
}

/* Bytes of reg entry @reg that the list of @hw reaches. */
static u64 regview_test_extent(const struct apple_regview_hw *hw,
			       unsigned int reg)
{
	u64 end = 0;
	unsigned int i;

	for (i = 0; i < hw->nr_ranges; i++) {
		const struct apple_regview_range *r = &hw->ranges[i];
		u32 stride = r->stride ? r->stride : 4;
		u64 last;

		if (r->reg != reg)
			continue;
		last = r->offset + (u64)(r->count - 1) * stride + 4;
		end = max(end, last);
	}

	return end;
}

/* Back every reg entry of @hw with a buffer of the size it needs plus @extra. */
static struct regview_test_ctx *regview_test_setup(struct kunit *test,
						   const struct apple_regview_hw *hw,
						   s64 extra)
{
	struct regview_test_ctx *ctx;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, hw);
	KUNIT_ASSERT_LE(test, hw->nr_regs, REGVIEW_TEST_MAX_REGS);

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	for (i = 0; i < hw->nr_regs; i++) {
		u64 size = regview_test_extent(hw, i);

		/* An entry no range uses still gets a word of its own. */
		size = max_t(u64, size, 4);
		ctx->buf[i] = vzalloc(size + max_t(s64, extra, 0) + 4);
		KUNIT_ASSERT_NOT_NULL(test, ctx->buf[i]);
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test, regview_test_vfree,
							  ctx->buf[i]));
		ctx->maps[i].base = (void __iomem __force *)ctx->buf[i];
		ctx->maps[i].size = size + extra;
	}

	ctx->rv.hw = hw;
	ctx->rv.maps = ctx->maps;

	return ctx;
}

/* A value that names the reg entry and offset it was read from. */
static u32 regview_test_tag(unsigned int reg, u32 off)
{
	return 0xa0000000 | (reg << 24) | off;
}

static void regview_test_fill(struct regview_test_ctx *ctx)
{
	const struct apple_regview_hw *hw = ctx->rv.hw;
	unsigned int i;

	for (i = 0; i < hw->nr_regs; i++) {
		__le32 *w = ctx->buf[i];
		u64 off;

		for (off = 0; off + 4 <= ctx->maps[i].size; off += 4)
			w[off / 4] = cpu_to_le32(regview_test_tag(i, off));
	}
}

static void regview_test_show(struct kunit *test, struct regview_test_ctx *ctx)
{
	struct seq_file s = { };

	ctx->seq_buf = kunit_kzalloc(test, REGVIEW_TEST_SEQ_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->seq_buf);

	s.buf = ctx->seq_buf;
	s.size = REGVIEW_TEST_SEQ_SIZE - 1;
	s.private = &ctx->rv;

	KUNIT_ASSERT_EQ(test, apple_regview_show(&s, NULL), 0);
	KUNIT_ASSERT_FALSE(test, seq_has_overflowed(&s));
	ctx->seq_len = s.count;
	ctx->seq_buf[s.count] = '\0';
}

static bool regview_test_has(const struct regview_test_ctx *ctx, const char *want)
{
	return strstr(ctx->seq_buf, want);
}

static bool regview_test_starts(const struct regview_test_ctx *ctx, const char *want)
{
	return str_has_prefix(ctx->seq_buf, want);
}

static unsigned int regview_test_lines(const char *buf)
{
	unsigned int n = 0;

	for (; *buf; buf++)
		if (*buf == '\n')
			n++;
	return n;
}

static unsigned int regview_test_nr_listed(const struct apple_regview_hw *hw)
{
	unsigned int i, n = 0;

	for (i = 0; i < hw->nr_ranges; i++)
		n += hw->ranges[i].count;
	return n;
}

static void regview_test_stride_default(struct kunit *test)
{
	struct apple_regview_range r = { .stride = 0 };

	KUNIT_EXPECT_EQ(test, apple_regview_stride(&r), 4);
	r.stride = 0x10;
	KUNIT_EXPECT_EQ(test, apple_regview_stride(&r), 0x10);
	r.stride = MCC_PLANE_STRIDE;
	KUNIT_EXPECT_EQ(test, apple_regview_stride(&r), MCC_PLANE_STRIDE);
}

/* A run whose last word ends exactly at the end of the entry is accepted. */
static void regview_test_check_exact_fit(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0x10, 4, 0, "a" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 1, .nr_regs = 1,
	};
	struct apple_regview_map map = { .size = 0x20 };
	struct apple_regview rv = { .hw = &hw, .maps = &map };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), 0);
	map.size = 0x1000;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), 0);
}

/* One byte or one word short of the last register is refused. */
static void regview_test_check_one_past(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0x10, 4, 0, "a" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 1, .nr_regs = 1,
	};
	struct apple_regview_map map = { .size = 0x1c };
	struct apple_regview rv = { .hw = &hw, .maps = &map };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
	map.size = 0x1f;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
	map.size = 0;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

/* The stride, not 4, sets how far a strided run reaches. */
static void regview_test_check_strided(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "s" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 1, .nr_regs = 1,
	};
	const u64 need = MCC_PLANE_CACHE_STATUS + 3 * MCC_PLANE_STRIDE + 4;
	struct apple_regview_map map = { .size = need };
	struct apple_regview rv = { .hw = &hw, .maps = &map };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), 0);
	map.size = need - 4;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
	/* Large enough for four packed words but not for four planes. */
	map.size = MCC_PLANE_CACHE_STATUS + 16;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

/* A range naming a reg entry the node need not have is refused. */
static void regview_test_check_bad_reg(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0, 1, 0, "a" },
		{ 2, 0, 1, 0, "b" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 2, .nr_regs = 2,
	};
	struct apple_regview_map maps[3] = {
		{ .size = 0x100 }, { .size = 0x100 }, { .size = 0x100 },
	};
	struct apple_regview rv = { .hw = &hw, .maps = maps };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

static void regview_test_check_zero_count(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0, 0, 0, "empty" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 1, .nr_regs = 1,
	};
	struct apple_regview_map map = { .size = 0x100 };
	struct apple_regview rv = { .hw = &hw, .maps = &map };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

/* Offsets near 4 GiB must not wrap around into a small aperture. */
static void regview_test_check_no_wrap(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0xfffffffc, 2, 0, "wrap" },
	};
	static const struct apple_regview_range ranges_stride[] = {
		{ 0, 0x10, 3, 0x80000000, "wrap" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 1, .nr_regs = 1,
	};
	static const struct apple_regview_hw hw_stride = {
		.ranges = ranges_stride, .nr_ranges = 1, .nr_regs = 1,
	};
	struct apple_regview_map map = { .size = 0x1000 };
	struct apple_regview rv = { .hw = &hw, .maps = &map };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
	rv.hw = &hw_stride;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

/* Each range is checked against its own reg entry, every range is checked. */
static void regview_test_check_per_entry(struct kunit *test)
{
	static const struct apple_regview_range ranges[] = {
		{ 0, 0x0, 1, 0, "small" },
		{ 1, 0x800, 1, 0, "big" },
	};
	static const struct apple_regview_hw hw = {
		.ranges = ranges, .nr_ranges = 2, .nr_regs = 2,
	};
	struct apple_regview_map maps[2] = {
		{ .size = 0x1000 }, { .size = 0x804 },
	};
	struct apple_regview rv = { .hw = &hw, .maps = maps };

	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), 0);
	/* reg 1 too small even though reg 0 is large */
	maps[1].size = 0x800;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
	/* the last range of the list fails, the first one fits */
	maps[1].size = 0x804;
	maps[0].size = 0;
	KUNIT_EXPECT_EQ(test, apple_regview_check(&rv), -EINVAL);
}

/*
 * Every built-in list fits the smallest apertures that hold it and is refused
 * as soon as any entry it uses is a word smaller.
 */
static void regview_test_tables_fit(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_regview_of_match; id->compatible[0]; id++) {
		const struct apple_regview_hw *hw = id->data;
		struct regview_test_ctx *ctx = regview_test_setup(test, hw, 0);
		unsigned int i;

		KUNIT_EXPECT_EQ_MSG(test, apple_regview_check(&ctx->rv), 0,
				    "%s", id->compatible);
		for (i = 0; i < hw->nr_regs; i++) {
			if (!regview_test_extent(hw, i))
				continue;
			ctx->maps[i].size -= 4;
			KUNIT_EXPECT_EQ_MSG(test, apple_regview_check(&ctx->rv),
					    -EINVAL, "%s reg %u", id->compatible, i);
			ctx->maps[i].size += 4;
		}
	}
}

/* Lists are word aligned and name each register at most once. */
static void regview_test_tables_sane(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_regview_of_match; id->compatible[0]; id++) {
		const struct apple_regview_hw *hw = id->data;
		unsigned int i, j, a, b;

		KUNIT_ASSERT_NOT_NULL(test, hw);
		KUNIT_EXPECT_GT(test, hw->nr_ranges, 0);
		KUNIT_EXPECT_GT(test, hw->nr_regs, 0);

		for (i = 0; i < hw->nr_ranges; i++) {
			const struct apple_regview_range *r = &hw->ranges[i];
			u32 sr = apple_regview_stride(r);

			KUNIT_EXPECT_LT_MSG(test, r->reg, hw->nr_regs, "%s %s",
					    id->compatible, r->name);
			KUNIT_EXPECT_GT(test, r->count, 0);
			KUNIT_EXPECT_EQ(test, r->offset % 4, 0);
			KUNIT_EXPECT_EQ(test, sr % 4, 0);
			KUNIT_EXPECT_NOT_NULL(test, r->name);

			for (j = i + 1; j < hw->nr_ranges; j++) {
				const struct apple_regview_range *q = &hw->ranges[j];
				u32 sq = apple_regview_stride(q);

				if (q->reg != r->reg)
					continue;
				for (a = 0; a < r->count; a++) {
					u32 ra = r->offset + a * sr;

					for (b = 0; b < q->count; b++)
						KUNIT_EXPECT_NE_MSG(test, ra, q->offset + b * sq,
								    "%s: %s[%u] and %s[%u]",
								    id->compatible, r->name,
								    a, q->name, b);
				}
			}
		}
	}
}

/* The number of registers each block exposes, as its comment describes. */
static void regview_test_tables_counts(struct kunit *test)
{
	static const struct {
		const char *compatible;
		unsigned int regs;
		unsigned int nr_regs;
	} want[] = {
		/* 4 regions x start/end/enable + 4 planes */
		{ "apple,t8112-mcc", 16, 2 },
		/* the same on controller 0, plus 4 planes on the other three */
		{ "apple,t6000-mcc", 28, 4 },
		/* ten bus fault and seven queue fault counters */
		{ "apple,t8112-spmi-sideband", 17, 1 },
		/* sixteen fault counters of the primary bus */
		{ "apple,t6000-spmi-monitor", 16, 2 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(want); i++) {
		const struct apple_regview_hw *hw = regview_test_hw(want[i].compatible);

		KUNIT_ASSERT_NOT_NULL_MSG(test, hw, "%s", want[i].compatible);
		KUNIT_EXPECT_EQ_MSG(test, regview_test_nr_listed(hw), want[i].regs,
				    "%s", want[i].compatible);
		KUNIT_EXPECT_EQ_MSG(test, hw->nr_regs, want[i].nr_regs,
				    "%s", want[i].compatible);
	}
}

/*
 * Reading the SPMI monitor fault queue (reg[0]) pops an entry, so its list
 * must only name the counter window.
 */
static void regview_test_spmi_monitor_no_queue(struct kunit *test)
{
	const struct apple_regview_hw *hw =
		regview_test_hw("apple,t6000-spmi-monitor");
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, hw);
	for (i = 0; i < hw->nr_ranges; i++)
		KUNIT_EXPECT_EQ_MSG(test, hw->ranges[i].reg, SPMI_MON_COUNTERS,
				    "%s", hw->ranges[i].name);
}

/* The T8112 MCC planes live in reg[1]; reg[0] is never read. */
static void regview_test_t8112_mcc_layout(struct kunit *test)
{
	const struct apple_regview_hw *hw = regview_test_hw("apple,t8112-mcc");
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, hw);
	for (i = 0; i < hw->nr_ranges; i++)
		KUNIT_EXPECT_EQ(test, hw->ranges[i].reg, 1);
	KUNIT_EXPECT_EQ(test, regview_test_extent(hw, 1),
			MCC_PLANE_CACHE_STATUS + 3 * MCC_PLANE_STRIDE + 4);
}

/* Exact text of the sideband view: one line per register, in list order. */
static void regview_test_show_format(struct kunit *test)
{
	const struct apple_regview_hw *hw =
		regview_test_hw("apple,t8112-spmi-sideband");
	struct regview_test_ctx *ctx = regview_test_setup(test, hw, 0);
	const char *tail, *want;
	char line[64];
	unsigned int j;

	regview_test_fill(ctx);
	regview_test_show(test, ctx);

	KUNIT_EXPECT_EQ(test, regview_test_lines(ctx->seq_buf),
			regview_test_nr_listed(hw));
	want = "0+0x0000200 bus-fault-count[0]: 0xa0000200\n";
	KUNIT_EXPECT_TRUE(test, regview_test_starts(ctx, want));
	want = "\n0+0x0000224 bus-fault-count[9]: 0xa0000224\n"
	       "0+0x000025c queue-fault-count[0]: 0xa000025c\n";
	KUNIT_EXPECT_TRUE(test, regview_test_has(ctx, want));
	tail = "\n0+0x0000274 queue-fault-count[6]: 0xa0000274\n";
	KUNIT_ASSERT_GE(test, ctx->seq_len, strlen(tail));
	KUNIT_EXPECT_STREQ(test, ctx->seq_buf + ctx->seq_len - strlen(tail), tail);
	/* The words between the two runs are not shown. */
	KUNIT_EXPECT_NULL(test, strstr(ctx->seq_buf, "0x0000228"));
	KUNIT_EXPECT_NULL(test, strstr(ctx->seq_buf, "[10]"));

	for (j = 0; j < SPMI_SB_NR_BUS_FAULTS; j++) {
		snprintf(line, sizeof(line), "0+0x%07x bus-fault-count[%u]: 0x%08x\n",
			 SPMI_SB_BUS_FAULTS + 4 * j, j,
			 regview_test_tag(0, SPMI_SB_BUS_FAULTS + 4 * j));
		KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(ctx->seq_buf, line), "%s", line);
	}
}

/* Values are printed as full 8-digit words, including 0 and all-ones. */
static void regview_test_show_values(struct kunit *test)
{
	const struct apple_regview_hw *hw =
		regview_test_hw("apple,t8112-spmi-sideband");
	struct regview_test_ctx *ctx = regview_test_setup(test, hw, 0);
	const char *want;
	__le32 *w;

	w = ctx->buf[0];
	w[SPMI_SB_BUS_FAULTS / 4] = cpu_to_le32(U32_MAX);
	w[SPMI_SB_BUS_FAULTS / 4 + 1] = cpu_to_le32(1);
	regview_test_show(test, ctx);

	want = "0+0x0000200 bus-fault-count[0]: 0xffffffff\n"
	       "0+0x0000204 bus-fault-count[1]: 0x00000001\n"
	       "0+0x0000208 bus-fault-count[2]: 0x00000000\n";
	KUNIT_EXPECT_TRUE(test, regview_test_starts(ctx, want));
}

/* A strided run reads the word of each plane, not consecutive words. */
static void regview_test_show_strided(struct kunit *test)
{
	const struct apple_regview_hw *hw = regview_test_hw("apple,t8112-mcc");
	struct regview_test_ctx *ctx = regview_test_setup(test, hw, 0);
	const char *want;
	char line[64];
	unsigned int j;

	regview_test_fill(ctx);
	regview_test_show(test, ctx);

	KUNIT_EXPECT_EQ(test, regview_test_lines(ctx->seq_buf), 16);
	want = "1+0x00006a0 tz-start[0]: 0xa10006a0\n"
	       "1+0x00006b0 tz-start[1]: 0xa10006b0\n";
	KUNIT_EXPECT_TRUE(test, regview_test_starts(ctx, want));
	for (j = 0; j < 4; j++) {
		u32 off = MCC_PLANE_CACHE_STATUS + j * MCC_PLANE_STRIDE;

		snprintf(line, sizeof(line), "1+0x%07x cache-status[%u]: 0x%08x\n",
			 off, j, regview_test_tag(1, off));
		KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(ctx->seq_buf, line), "%s", line);
	}
	KUNIT_EXPECT_TRUE(test, regview_test_has(ctx, "1+0x00006a8 tz-enable[0]: 0xa10006a8\n"));
	KUNIT_EXPECT_TRUE(test, regview_test_has(ctx, "1+0x00c1c04 cache-status[3]: "));
}

/* Each range reads the window of its own reg entry. */
static void regview_test_show_multi_entry(struct kunit *test)
{
	const struct apple_regview_hw *hw = regview_test_hw("apple,t6000-mcc");
	struct regview_test_ctx *ctx = regview_test_setup(test, hw, 0);
	char line[64];
	unsigned int reg;

	regview_test_fill(ctx);
	regview_test_show(test, ctx);

	KUNIT_EXPECT_EQ(test, regview_test_lines(ctx->seq_buf), 28);
	for (reg = 0; reg < 4; reg++) {
		u32 off = MCC_PLANE_CACHE_STATUS + 2 * MCC_PLANE_STRIDE;

		snprintf(line, sizeof(line), "%u+0x%07x cache-status[2]: 0x%08x\n",
			 reg, off, regview_test_tag(reg, off));
		KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(ctx->seq_buf, line), "%s", line);
	}
	/* The carveouts are only listed for controller 0. */
	KUNIT_EXPECT_NULL(test, strstr(ctx->seq_buf, "1+0x00006a0"));
}

/* Showing a view leaves every byte of every window as it was. */
static void regview_test_show_never_writes(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_regview_of_match; id->compatible[0]; id++) {
		struct regview_test_ctx *ctx = regview_test_setup(test, id->data, 0x100);
		unsigned int i;

		regview_test_fill(ctx);
		for (i = 0; i < ctx->rv.hw->nr_regs; i++)
			memset(ctx->buf[i] + ctx->maps[i].size, 0x5a, 4);

		regview_test_show(test, ctx);

		for (i = 0; i < ctx->rv.hw->nr_regs; i++) {
			__le32 *w = ctx->buf[i];
			u64 off;
			u32 guard;

			for (off = 0; off + 4 <= ctx->maps[i].size; off += 4)
				if (le32_to_cpu(w[off / 4]) != regview_test_tag(i, off))
					break;
			KUNIT_EXPECT_EQ_MSG(test, off, (u64)ctx->maps[i].size,
					    "%s reg %u", id->compatible, i);
			memcpy(&guard, ctx->buf[i] + ctx->maps[i].size, 4);
			KUNIT_EXPECT_EQ(test, guard, 0x5a5a5a5a);
		}
	}
}

/* The debugfs file has no write method. */
static void regview_test_fops_read_only(struct kunit *test)
{
	const struct file_operations *fops = apple_regview_test_fops();

	KUNIT_EXPECT_NULL(test, fops->write);
	KUNIT_EXPECT_NULL(test, fops->write_iter);
	KUNIT_EXPECT_NOT_NULL(test, fops->read);
}

static struct kunit_case regview_test_cases[] = {
	KUNIT_CASE(regview_test_stride_default),
	KUNIT_CASE(regview_test_check_exact_fit),
	KUNIT_CASE(regview_test_check_one_past),
	KUNIT_CASE(regview_test_check_strided),
	KUNIT_CASE(regview_test_check_bad_reg),
	KUNIT_CASE(regview_test_check_zero_count),
	KUNIT_CASE(regview_test_check_no_wrap),
	KUNIT_CASE(regview_test_check_per_entry),
	KUNIT_CASE(regview_test_tables_fit),
	KUNIT_CASE(regview_test_tables_sane),
	KUNIT_CASE(regview_test_tables_counts),
	KUNIT_CASE(regview_test_spmi_monitor_no_queue),
	KUNIT_CASE(regview_test_t8112_mcc_layout),
	KUNIT_CASE(regview_test_show_format),
	KUNIT_CASE(regview_test_show_values),
	KUNIT_CASE(regview_test_show_strided),
	KUNIT_CASE(regview_test_show_multi_entry),
	KUNIT_CASE(regview_test_show_never_writes),
	KUNIT_CASE(regview_test_fops_read_only),
	{}
};

static struct kunit_suite regview_test_suite = {
	.name = "apple-regview",
	.test_cases = regview_test_cases,
};

kunit_test_suite(regview_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
