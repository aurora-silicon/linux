// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple Silicon hardware tunable library.
 *
 * The property a boot stage would pass comes from a device node built by
 * the test, and the registers the tunable is applied to are a buffer of
 * kernel memory. No device tree of the machine and no MMIO is touched.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/apple/tunable.h>

#define TUNABLE_TEST_PROP  "apple,tunable-test"
#define TUNABLE_TEST_NREGS 8

struct tunable_test {
	struct device *dev;
	struct device_node np;
	struct property prop;
	__le32 regs[TUNABLE_TEST_NREGS];
};

static int tunable_test_init(struct kunit *test)
{
	struct tunable_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dev = kunit_device_register(test, "tunable-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);
	test->priv = t;

	return 0;
}

/* Parse a property of len bytes holding the cells, for res_size bytes at start */
static struct apple_tunable *tunable_test_parse_at(struct kunit *test, const u32 *cells,
						   size_t len, resource_size_t start,
						   resource_size_t res_size)
{
	struct tunable_test *t = test->priv;
	struct resource res = DEFINE_RES_MEM(start, res_size);
	__be32 *value;
	size_t i;

	value = kunit_kzalloc(test, round_up(len, 4) + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, value);
	for (i = 0; i < DIV_ROUND_UP(len, 4); i++)
		value[i] = cpu_to_be32(cells[i]);

	t->prop.name = (char *)TUNABLE_TEST_PROP;
	t->prop.value = value;
	t->prop.length = len;
	t->np.properties = &t->prop;

	return devm_apple_tunable_parse(t->dev, &t->np, TUNABLE_TEST_PROP, &res);
}

static struct apple_tunable *tunable_test_parse(struct kunit *test, const u32 *cells,
						size_t len, resource_size_t res_size)
{
	return tunable_test_parse_at(test, cells, len, 0x2a0000000ULL, res_size);
}

static u32 tunable_test_rd(struct tunable_test *t, unsigned int reg)
{
	return le32_to_cpu(t->regs[reg / 4]);
}

static void tunable_test_apply(struct tunable_test *t, struct apple_tunable *tunable)
{
	apple_tunable_apply((void __iomem __force *)t->regs, tunable);
}

/* Each [offset, mask, value] triplet of big-endian cells becomes one entry */
static void tunable_test_parse_triplets(struct kunit *test)
{
	static const u32 cells[] = {
		0x00, 0x000000ff, 0x00000012,
		0x1c, 0xffff0000, 0xabcd0000,
		0x04, 0x80000001, 0x80000000,
	};
	struct apple_tunable *tunable;

	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);
	KUNIT_ASSERT_EQ(test, tunable->sz, 3);
	KUNIT_EXPECT_EQ(test, tunable->values[0].offset, 0x00);
	KUNIT_EXPECT_EQ(test, tunable->values[0].mask, 0x000000ff);
	KUNIT_EXPECT_EQ(test, tunable->values[0].value, 0x00000012);
	KUNIT_EXPECT_EQ(test, tunable->values[1].offset, 0x1c);
	KUNIT_EXPECT_EQ(test, tunable->values[1].mask, 0xffff0000);
	KUNIT_EXPECT_EQ(test, tunable->values[1].value, 0xabcd0000);
	KUNIT_EXPECT_EQ(test, tunable->values[2].offset, 0x04);
	KUNIT_EXPECT_EQ(test, tunable->values[2].mask, 0x80000001);
	KUNIT_EXPECT_EQ(test, tunable->values[2].value, 0x80000000);
}

static void tunable_test_parse_missing(struct kunit *test)
{
	static const u32 cells[] = { 0x00, 0xff, 0x12 };
	struct tunable_test *t = test->priv;
	struct resource res = DEFINE_RES_MEM(0x2a0000000ULL, 0x20);
	struct apple_tunable *tunable;

	/* A node without properties */
	tunable = devm_apple_tunable_parse(t->dev, &t->np, TUNABLE_TEST_PROP, &res);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -ENOENT);

	/* A node with another one */
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);
	tunable = devm_apple_tunable_parse(t->dev, &t->np, "apple,tunable-other", &res);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -ENOENT);
}

/* A property that is not whole triplets is refused */
static void tunable_test_parse_bad_length(struct kunit *test)
{
	static const u32 cells[] = { 0x00, 0xff, 0x12, 0x04, 0xff, 0x34 };
	static const size_t bad[] = { 1, 4, 8, 11, 13, 16, 20, 23 };
	struct apple_tunable *tunable;
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		tunable = tunable_test_parse(test, cells, bad[i], 0x20);
		KUNIT_EXPECT_EQ_MSG(test, PTR_ERR_OR_ZERO(tunable), -EINVAL,
				    "length %zu", bad[i]);
	}

	tunable = tunable_test_parse(test, cells, 12, 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);
	KUNIT_EXPECT_EQ(test, tunable->sz, 1);
	tunable = tunable_test_parse(test, cells, 24, 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);
	KUNIT_EXPECT_EQ(test, tunable->sz, 2);
}

/* An empty property is a tunable with nothing to apply */
static void tunable_test_parse_empty(struct kunit *test)
{
	static const u32 cells[] = { 0 };
	struct tunable_test *t = test->priv;
	struct apple_tunable *tunable;

	tunable = tunable_test_parse(test, cells, 0, 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);
	KUNIT_EXPECT_EQ(test, tunable->sz, 0);

	t->regs[0] = cpu_to_le32(0x12345678);
	tunable_test_apply(t, tunable);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0), 0x12345678);
}

/* A resource without room for one register takes no tunable */
static void tunable_test_parse_small_resource(struct kunit *test)
{
	static const u32 cells[] = { 0x00, 0xff, 0x12 };
	struct apple_tunable *tunable;
	resource_size_t size;

	for (size = 0; size < 4; size++) {
		tunable = tunable_test_parse(test, cells, sizeof(cells), size);
		KUNIT_EXPECT_EQ_MSG(test, PTR_ERR_OR_ZERO(tunable), -EINVAL,
				    "resource of %llu bytes", (u64)size);
		/* Not even an empty one */
		tunable = tunable_test_parse(test, cells, 0, size);
		KUNIT_EXPECT_EQ_MSG(test, PTR_ERR_OR_ZERO(tunable), -EINVAL,
				    "resource of %llu bytes", (u64)size);
	}

	tunable = tunable_test_parse(test, cells, sizeof(cells), 4);
	KUNIT_EXPECT_FALSE(test, IS_ERR(tunable));
}

/* Registers are 32 bits wide: an offset between two of them is refused */
static void tunable_test_parse_unaligned_offset(struct kunit *test)
{
	u32 cells[] = {
		0x00, 0xff, 0x12,
		0x00, 0xff, 0x34,
	};
	struct apple_tunable *tunable;
	u32 off;

	for (off = 1; off < 8; off++) {
		if (off == 4)
			continue;
		/* In the first triplet, then in the last */
		cells[0] = off;
		cells[3] = 0;
		tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
		KUNIT_EXPECT_EQ_MSG(test, PTR_ERR_OR_ZERO(tunable), -EINVAL,
				    "offset %u", off);
		cells[0] = 0;
		cells[3] = off;
		tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
		KUNIT_EXPECT_EQ_MSG(test, PTR_ERR_OR_ZERO(tunable), -EINVAL,
				    "offset %u", off);
	}
}

/* The last register of the resource is the last offset taken */
static void tunable_test_parse_offset_bounds(struct kunit *test)
{
	u32 cells[] = {
		0x00, 0xff, 0x12,
		0x00, 0xff, 0x34,
	};
	struct apple_tunable *tunable;

	cells[3] = 0x1c;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_EXPECT_FALSE(test, IS_ERR(tunable));

	cells[3] = 0x20;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);

	cells[3] = 0x24;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);

	cells[3] = 0xfffffffc;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);

	/* In the first triplet too */
	cells[0] = 0x20;
	cells[3] = 0;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);
	cells[0] = 0;

	/* A register that only starts inside the resource is outside */
	cells[3] = 0x1c;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x1f);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);
	cells[3] = 0x18;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x1f);
	KUNIT_EXPECT_FALSE(test, IS_ERR(tunable));

	/* One register */
	cells[3] = 0x04;
	tunable = tunable_test_parse(test, cells, sizeof(cells), 4);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);
}

/* Offsets count from the start of the resource, wherever that is */
static void tunable_test_parse_resource_start(struct kunit *test)
{
	static const u32 cells[] = { 0x1c, 0xff, 0x12 };
	struct apple_tunable *tunable;

	tunable = tunable_test_parse_at(test, cells, sizeof(cells), 0, 0x20);
	KUNIT_EXPECT_FALSE(test, IS_ERR(tunable));
	tunable = tunable_test_parse_at(test, cells, sizeof(cells), 0x10, 0x20);
	KUNIT_EXPECT_FALSE(test, IS_ERR(tunable));
	tunable = tunable_test_parse_at(test, cells, sizeof(cells), 0x10, 0x1c);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(tunable), -EINVAL);
}

/* Applying clears the mask's bits and sets the value's, elsewhere nothing */
static void tunable_test_apply_mask_value(struct kunit *test)
{
	static const u32 cells[] = {
		0x04, 0x00ff00ff, 0x00120034,
		0x1c, 0xffffffff, 0xdeadbeef,
		0x0c, 0x00000000, 0x00000000,
		0x10, 0xf0000000, 0x00000000,
	};
	struct tunable_test *t = test->priv;
	struct apple_tunable *tunable;
	int i;

	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);

	for (i = 0; i < TUNABLE_TEST_NREGS; i++)
		t->regs[i] = cpu_to_le32(0xa5a5a500 + i);
	tunable_test_apply(t, tunable);

	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x00), 0xa5a5a500);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x04), 0xa512a534);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x08), 0xa5a5a502);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x0c), 0xa5a5a503);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x10), 0x05a5a504);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x14), 0xa5a5a505);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x18), 0xa5a5a506);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x1c), 0xdeadbeef);

	/* Applying it again changes nothing */
	tunable_test_apply(t, tunable);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x04), 0xa512a534);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x10), 0x05a5a504);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x1c), 0xdeadbeef);
}

/* Entries are applied in the property's order, each on the last one's result */
static void tunable_test_apply_order(struct kunit *test)
{
	static const u32 cells[] = {
		0x08, 0x0000ffff, 0x00001111,
		0x08, 0x00000ff0, 0x00000220,
		0x08, 0xff000000, 0x33000000,
	};
	struct tunable_test *t = test->priv;
	struct apple_tunable *tunable;

	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);

	t->regs[2] = cpu_to_le32(0xffffffff);
	tunable_test_apply(t, tunable);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x08), 0x33ff1221);
}

/* The value's bits are set as given, the mask only says what to clear first */
static void tunable_test_apply_value_outside_mask(struct kunit *test)
{
	static const u32 cells[] = { 0x00, 0x0000000f, 0x00000105 };
	struct tunable_test *t = test->priv;
	struct apple_tunable *tunable;

	tunable = tunable_test_parse(test, cells, sizeof(cells), 0x20);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tunable);

	t->regs[0] = cpu_to_le32(0x0000f00a);
	tunable_test_apply(t, tunable);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x00), 0x0000f105);
}

/* A driver may apply a tunable that its device tree doesn't have */
static void tunable_test_apply_null(struct kunit *test)
{
	struct tunable_test *t = test->priv;

	t->regs[0] = cpu_to_le32(0x12345678);
	tunable_test_apply(t, NULL);
	KUNIT_EXPECT_EQ(test, tunable_test_rd(t, 0x00), 0x12345678);
}

static struct kunit_case tunable_test_cases[] = {
	KUNIT_CASE(tunable_test_parse_triplets),
	KUNIT_CASE(tunable_test_parse_missing),
	KUNIT_CASE(tunable_test_parse_bad_length),
	KUNIT_CASE(tunable_test_parse_empty),
	KUNIT_CASE(tunable_test_parse_small_resource),
	KUNIT_CASE(tunable_test_parse_unaligned_offset),
	KUNIT_CASE(tunable_test_parse_offset_bounds),
	KUNIT_CASE(tunable_test_parse_resource_start),
	KUNIT_CASE(tunable_test_apply_mask_value),
	KUNIT_CASE(tunable_test_apply_order),
	KUNIT_CASE(tunable_test_apply_value_outside_mask),
	KUNIT_CASE(tunable_test_apply_null),
	{}
};

static struct kunit_suite tunable_test_suite = {
	.name = "apple-tunable",
	.init = tunable_test_init,
	.test_cases = tunable_test_cases,
};
kunit_test_suite(tunable_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("KUnit tests for Apple Silicon hardware tunable support");
