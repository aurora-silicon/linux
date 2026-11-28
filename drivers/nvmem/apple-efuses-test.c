// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple SoC eFuse driver: the nvmem read callback's
 * word reads, offsets and sizes against the fuse range, and reads through
 * a real nvmem device registered with the driver's callback, including
 * bit-field cells as the PHY calibration users read them.
 *
 * The fuse range is kernel memory, so the tests need no Apple hardware.
 *
 * Included by apple-efuses.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/nvmem-consumer.h>
#include <linux/unaligned.h>

/* Fuse range of the fake block, in words. */
#define APPLE_EFUSES_TEST_WORDS		16
#define APPLE_EFUSES_TEST_SIZE		(APPLE_EFUSES_TEST_WORDS * sizeof(u32))
/* The fake block is followed by guard words a read must never reach. */
#define APPLE_EFUSES_TEST_GUARD		4
/* Destination bytes a read has not written. */
#define APPLE_EFUSES_TEST_POISON	0x5a

struct apple_efuses_test {
	struct apple_efuses_priv priv;
	u32 *fuses;
	struct device *dev;
};

static u32 apple_efuses_test_word(unsigned int i)
{
	return 0x10203040 + i * 0x01010101;
}

static int apple_efuses_test_init(struct kunit *test)
{
	struct apple_efuses_test *ctx;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	ctx->fuses = kunit_kcalloc(test, APPLE_EFUSES_TEST_WORDS + APPLE_EFUSES_TEST_GUARD,
				   sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->fuses);
	for (i = 0; i < APPLE_EFUSES_TEST_WORDS; i++)
		ctx->fuses[i] = cpu_to_le32(apple_efuses_test_word(i));
	for (; i < APPLE_EFUSES_TEST_WORDS + APPLE_EFUSES_TEST_GUARD; i++)
		ctx->fuses[i] = cpu_to_le32(0xbad0bad0);
	ctx->priv.fuses = (void __iomem __force *)ctx->fuses;

	return 0;
}

/* One word at offset 0. */
static void apple_efuses_test_read_word(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u32 val = 0;

	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, 0, &val, sizeof(val)), 0);
	KUNIT_EXPECT_EQ(test, val, apple_efuses_test_word(0));
}

/* Each word at its own offset. */
static void apple_efuses_test_read_each_offset(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	unsigned int i;
	u32 val;

	for (i = 0; i < APPLE_EFUSES_TEST_WORDS; i++) {
		val = 0;
		KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, i * sizeof(u32),
							&val, sizeof(val)), 0);
		KUNIT_EXPECT_EQ_MSG(test, val, apple_efuses_test_word(i), "word %u", i);
	}
}

/* A multi-word read from an offset walks consecutive words. */
static void apple_efuses_test_read_run(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u32 val[5];
	unsigned int i;

	memset(val, APPLE_EFUSES_TEST_POISON, sizeof(val));
	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, 3 * sizeof(u32), val,
						sizeof(val)), 0);
	for (i = 0; i < ARRAY_SIZE(val); i++)
		KUNIT_EXPECT_EQ_MSG(test, val[i], apple_efuses_test_word(3 + i), "word %u", i);
}

/* The whole range in one read, ending exactly at its last word. */
static void apple_efuses_test_read_all(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u32 val[APPLE_EFUSES_TEST_WORDS + 1];
	unsigned int i;

	memset(val, APPLE_EFUSES_TEST_POISON, sizeof(val));
	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, 0, val,
						APPLE_EFUSES_TEST_SIZE), 0);
	for (i = 0; i < APPLE_EFUSES_TEST_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, val[i], apple_efuses_test_word(i), "word %u", i);
	/* Nothing written past the requested size. */
	KUNIT_EXPECT_EQ(test, val[APPLE_EFUSES_TEST_WORDS], 0x5a5a5a5aU);
}

/* The last word of the range alone. */
static void apple_efuses_test_read_last(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u32 val = 0;

	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, APPLE_EFUSES_TEST_SIZE - 4,
						&val, sizeof(val)), 0);
	KUNIT_EXPECT_EQ(test, val, apple_efuses_test_word(APPLE_EFUSES_TEST_WORDS - 1));
}

/*
 * The callback copies whole words only: a tail shorter than a word is not
 * read or written, so the destination never overflows. The nvmem core
 * rounds cell and sysfs reads to the 4-byte word size before it calls in.
 */
static void apple_efuses_test_partial_tail(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u8 val[12];
	u32 word;

	memset(val, APPLE_EFUSES_TEST_POISON, sizeof(val));
	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, 8, val, 6), 0);
	memcpy(&word, val, sizeof(word));
	KUNIT_EXPECT_EQ(test, word, apple_efuses_test_word(2));
	KUNIT_EXPECT_EQ(test, val[4], APPLE_EFUSES_TEST_POISON);
	KUNIT_EXPECT_EQ(test, val[5], APPLE_EFUSES_TEST_POISON);
	KUNIT_EXPECT_EQ(test, val[6], APPLE_EFUSES_TEST_POISON);
}

/* Reads of zero or fewer than four bytes write nothing and succeed. */
static void apple_efuses_test_short_reads(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	size_t bytes;
	u8 val[4];

	for (bytes = 0; bytes < sizeof(u32); bytes++) {
		memset(val, APPLE_EFUSES_TEST_POISON, sizeof(val));
		KUNIT_EXPECT_EQ(test, apple_efuses_read(&ctx->priv, 0, val, bytes), 0);
		KUNIT_EXPECT_MEMEQ(test, val, "\x5a\x5a\x5a\x5a", sizeof(val));
	}
}

/* The fuses are read, never written. */
static void apple_efuses_test_source_untouched(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	u32 val[APPLE_EFUSES_TEST_WORDS];
	unsigned int i;

	KUNIT_ASSERT_EQ(test, apple_efuses_read(&ctx->priv, 0, val, sizeof(val)), 0);
	for (i = 0; i < APPLE_EFUSES_TEST_WORDS; i++)
		KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->fuses[i]), apple_efuses_test_word(i));
	for (; i < APPLE_EFUSES_TEST_WORDS + APPLE_EFUSES_TEST_GUARD; i++)
		KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->fuses[i]), 0xbad0bad0U);
}

static struct nvmem_device *apple_efuses_test_register(struct kunit *test,
						       struct apple_efuses_test *ctx)
{
	/* as apple_efuses_probe(), with the fake range as the resource */
	struct nvmem_config config = {
		.add_legacy_fixed_of_cells = true,
		.read_only = true,
		.reg_read = apple_efuses_read,
		.stride = sizeof(u32),
		.word_size = sizeof(u32),
		.name = "apple_efuses_nvmem_test",
		.id = NVMEM_DEVID_AUTO,
		.root_only = true,
	};
	struct nvmem_device *nvmem;

	ctx->dev = kunit_device_register(test, "apple-efuses-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);

	config.dev = ctx->dev;
	config.priv = &ctx->priv;
	config.size = APPLE_EFUSES_TEST_SIZE;

	nvmem = devm_nvmem_register(ctx->dev, &config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nvmem);

	return nvmem;
}

/* A registered device reports the fuse range and reads it through the driver. */
static void apple_efuses_test_nvmem_read(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	u32 val[3];

	KUNIT_EXPECT_EQ(test, nvmem_dev_size(nvmem), APPLE_EFUSES_TEST_SIZE);

	KUNIT_ASSERT_EQ(test, nvmem_device_read(nvmem, 0x14, sizeof(val), val),
			(int)sizeof(val));
	KUNIT_EXPECT_EQ(test, val[0], apple_efuses_test_word(5));
	KUNIT_EXPECT_EQ(test, val[1], apple_efuses_test_word(6));
	KUNIT_EXPECT_EQ(test, val[2], apple_efuses_test_word(7));

	KUNIT_ASSERT_EQ(test, nvmem_device_read(nvmem, APPLE_EFUSES_TEST_SIZE - 4, 4, val), 4);
	KUNIT_EXPECT_EQ(test, val[0], apple_efuses_test_word(APPLE_EFUSES_TEST_WORDS - 1));
}

/* The device is read-only: a write is refused and leaves the fuses alone. */
static void apple_efuses_test_nvmem_read_only(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	u32 val = 0x12345678;

	KUNIT_EXPECT_LT(test, nvmem_device_write(nvmem, 0, sizeof(val), &val), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->fuses[0]), apple_efuses_test_word(0));
}

/*
 * An 8-bit field at bit 16 of a fuse word: the core reads the whole word
 * through the driver, then shifts the field down to byte 0.
 */
static void apple_efuses_test_cell_byte(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	struct nvmem_cell_info info = {
		.name = "byte",
		.offset = 0x8,
		.bit_offset = 16,
		.nbits = 8,
	};
	u8 buf[4];

	memset(buf, APPLE_EFUSES_TEST_POISON, sizeof(buf));
	/* bytes = DIV_ROUND_UP(16 + 8, 8), read as one 4-byte word */
	KUNIT_ASSERT_EQ(test, nvmem_device_cell_read(nvmem, &info, buf), 3);
	KUNIT_EXPECT_EQ(test, buf[0], (u8)(apple_efuses_test_word(2) >> 16));
	KUNIT_EXPECT_EQ(test, buf[1], 0);
	KUNIT_EXPECT_EQ(test, buf[2], 0);
}

/* A bit field spanning two fuse words, as the PHY calibration cells use. */
static void apple_efuses_test_cell_bits(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	struct nvmem_cell_info info = {
		.name = "bits",
		.offset = 0x20,
		.bit_offset = 26,
		.nbits = 12,
	};
	u64 both;
	u8 buf[8];

	memset(buf, APPLE_EFUSES_TEST_POISON, sizeof(buf));
	/* bytes = DIV_ROUND_UP(26 + 12, 8), read as two 4-byte words */
	KUNIT_ASSERT_EQ(test, nvmem_device_cell_read(nvmem, &info, buf), 5);
	both = apple_efuses_test_word(8) | (u64)apple_efuses_test_word(9) << 32;
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(buf), (u16)((both >> 26) & GENMASK(11, 0)));
}

/* A cell in the last fuse word reads it without touching what follows. */
static void apple_efuses_test_cell_last_word(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	struct nvmem_cell_info info = {
		.name = "last",
		.offset = APPLE_EFUSES_TEST_SIZE - 4,
		.bytes = 4,
	};
	u32 val = 0;

	KUNIT_ASSERT_EQ(test, nvmem_device_cell_read(nvmem, &info, &val), 4);
	KUNIT_EXPECT_EQ(test, val, apple_efuses_test_word(APPLE_EFUSES_TEST_WORDS - 1));
}

/* The core refuses a cell that is not on the driver's 4-byte stride. */
static void apple_efuses_test_cell_unaligned(struct kunit *test)
{
	struct apple_efuses_test *ctx = test->priv;
	struct nvmem_device *nvmem = apple_efuses_test_register(test, ctx);
	struct nvmem_cell_info info = {
		.name = "unaligned",
		.offset = 0x6,
		.bytes = 2,
	};
	u16 val = 0;

	KUNIT_EXPECT_EQ(test, nvmem_device_cell_read(nvmem, &info, &val), -EINVAL);
}

static struct kunit_case apple_efuses_test_cases[] = {
	KUNIT_CASE(apple_efuses_test_read_word),
	KUNIT_CASE(apple_efuses_test_read_each_offset),
	KUNIT_CASE(apple_efuses_test_read_run),
	KUNIT_CASE(apple_efuses_test_read_all),
	KUNIT_CASE(apple_efuses_test_read_last),
	KUNIT_CASE(apple_efuses_test_partial_tail),
	KUNIT_CASE(apple_efuses_test_short_reads),
	KUNIT_CASE(apple_efuses_test_source_untouched),
	KUNIT_CASE(apple_efuses_test_nvmem_read),
	KUNIT_CASE(apple_efuses_test_nvmem_read_only),
	KUNIT_CASE(apple_efuses_test_cell_byte),
	KUNIT_CASE(apple_efuses_test_cell_bits),
	KUNIT_CASE(apple_efuses_test_cell_last_word),
	KUNIT_CASE(apple_efuses_test_cell_unaligned),
	{}
};

static struct kunit_suite apple_efuses_test_suite = {
	.name = "apple-efuses",
	.init = apple_efuses_test_init,
	.test_cases = apple_efuses_test_cases,
};

kunit_test_suite(apple_efuses_test_suite);
