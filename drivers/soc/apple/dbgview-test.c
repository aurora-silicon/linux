// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SoC read-only debug register views.
 *
 * The view under test is a struct dbgview whose register block is a buffer
 * of kernel memory and whose device is a KUnit device with runtime PM and no
 * callbacks. The tests call the driver's own read and snapshot handlers; the
 * read handler copies to a KUnit user mapping. No device is probed and no
 * MMIO is touched.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/pm_runtime.h>
#include <asm/arch_timer.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "dbgview-internal.h"

#define DBGVIEW_TEST_USER_SIZE	(3 * PAGE_SIZE)
#define DBGVIEW_TEST_SEQ_SIZE	(8 * 1024)

struct dbgview_test_ctx {
	struct dbgview dv;
	struct file file;
	__le32 *regs;
	__le32 *copy;
	char __user *ubuf;
	u8 *kbuf;
	char *seq_buf;
};

static u32 dbgview_test_word(size_t i)
{
	return 0xd0000000 | (i * 4);
}

static struct dbgview_test_ctx *dbgview_test_setup(struct kunit *test,
						   resource_size_t size,
						   bool rpm)
{
	struct dbgview_test_ctx *ctx;
	unsigned long uaddr;
	struct device *dev;
	size_t i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	dev = kunit_device_register(test, "apple-dbgview-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	if (rpm) {
		pm_runtime_no_callbacks(dev);
		KUNIT_ASSERT_EQ(test, devm_pm_runtime_enable(dev), 0);
	}

	/* One spare word past the end to catch a read beyond the block. */
	ctx->regs = kunit_kzalloc(test, size + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->regs);
	ctx->copy = kunit_kzalloc(test, size + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->copy);
	for (i = 0; i < size / 4 + 1; i++)
		ctx->regs[i] = cpu_to_le32(dbgview_test_word(i));
	memcpy(ctx->copy, ctx->regs, size + 4);

	ctx->dv.dev = dev;
	ctx->dv.base = (void __iomem __force *)ctx->regs;
	ctx->dv.size = size;
	ctx->file.private_data = &ctx->dv;

	uaddr = kunit_vm_mmap(test, NULL, 0, DBGVIEW_TEST_USER_SIZE,
			      PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, uaddr, 0, "no user mapping");
	KUNIT_ASSERT_LT(test, uaddr, (unsigned long)TASK_SIZE);
	ctx->ubuf = (char __user *)uaddr;

	ctx->kbuf = kunit_kzalloc(test, DBGVIEW_TEST_USER_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->kbuf);

	return ctx;
}

/* Fill the user buffer with a marker so that untouched bytes show up. */
static void dbgview_test_poison_user(struct kunit *test,
				     struct dbgview_test_ctx *ctx)
{
	memset(ctx->kbuf, 0xee, DBGVIEW_TEST_USER_SIZE);
	KUNIT_ASSERT_EQ(test, copy_to_user(ctx->ubuf, ctx->kbuf,
					   DBGVIEW_TEST_USER_SIZE), 0);
}

static void dbgview_test_fetch_user(struct kunit *test,
				    struct dbgview_test_ctx *ctx)
{
	KUNIT_ASSERT_EQ(test, copy_from_user(ctx->kbuf, ctx->ubuf,
					     DBGVIEW_TEST_USER_SIZE), 0);
}

static ssize_t dbgview_test_read(struct dbgview_test_ctx *ctx, size_t count,
				 loff_t *pos)
{
	return dbgview_regs_read(&ctx->file, ctx->ubuf, count, pos);
}

static int dbgview_test_usage(struct dbgview_test_ctx *ctx)
{
	return atomic_read(&ctx->dv.dev->power.usage_count);
}

/* Bytes [0, len) of the user buffer are the registers from @pos on. */
static void dbgview_test_compare_words(struct kunit *test, const u8 *buf,
				       size_t pos, size_t len)
{
	const u32 *words = (const u32 *)buf;
	size_t i;

	for (i = 0; i < len / 4; i++)
		KUNIT_EXPECT_EQ(test, words[i], dbgview_test_word(pos / 4 + i));
}

static void dbgview_test_expect_words(struct kunit *test,
				      struct dbgview_test_ctx *ctx,
				      loff_t pos, size_t len)
{
	dbgview_test_fetch_user(test, ctx);
	dbgview_test_compare_words(test, ctx->kbuf, pos, len);
	/* nothing past the returned length was written */
	KUNIT_EXPECT_EQ(test, ctx->kbuf[len], 0xee);
}

static void dbgview_test_read_whole(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x100, &pos), 0x40);
	KUNIT_EXPECT_EQ(test, pos, 0x40);
	dbgview_test_expect_words(test, ctx, 0, 0x40);
}

static void dbgview_test_read_offset(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 8;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 8, &pos), 8);
	KUNIT_EXPECT_EQ(test, pos, 16);
	dbgview_test_expect_words(test, ctx, 8, 8);
	KUNIT_EXPECT_EQ(test, *(u32 *)ctx->kbuf, dbgview_test_word(2));
}

/* At or beyond the end of the block a read returns 0 and copies nothing. */
static void dbgview_test_read_eof(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0x40;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x10, &pos), 0);
	KUNIT_EXPECT_EQ(test, pos, 0x40);
	pos = 0x1000;
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x10, &pos), 0);
	KUNIT_EXPECT_EQ(test, pos, 0x1000);
	dbgview_test_fetch_user(test, ctx);
	KUNIT_EXPECT_EQ(test, ctx->kbuf[0], 0xee);
}

/* Only whole, aligned words are read. */
static void dbgview_test_read_unaligned_pos(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos;

	for (pos = 1; pos < 4; pos++) {
		loff_t p = pos;

		KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x10, &p), -EINVAL);
		KUNIT_EXPECT_EQ(test, p, pos);
	}
	pos = -4;
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x10, &pos), -EINVAL);
	KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
}

static void dbgview_test_read_short_count(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0;
	size_t count;

	for (count = 0; count < 4; count++) {
		KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, count, &pos), -EINVAL);
		KUNIT_EXPECT_EQ(test, pos, 0);
	}

	/* A count that is not a whole number of words is rounded down. */
	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 7, &pos), 4);
	KUNIT_EXPECT_EQ(test, pos, 4);
	dbgview_test_expect_words(test, ctx, 0, 4);
}

/* A read near the end stops at the end of the block. */
static void dbgview_test_read_clamped(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0x3c;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x10, &pos), 4);
	KUNIT_EXPECT_EQ(test, pos, 0x40);
	dbgview_test_expect_words(test, ctx, 0x3c, 4);
	KUNIT_EXPECT_EQ(test, *(u32 *)ctx->kbuf, dbgview_test_word(0xf));
}

/* One read never covers more than a page. */
static void dbgview_test_read_page_cap(struct kunit *test)
{
	resource_size_t size = 2 * PAGE_SIZE + 0x40;
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, size, true);
	loff_t pos = 0;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, DBGVIEW_TEST_USER_SIZE, &pos),
			PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, pos, PAGE_SIZE);
	dbgview_test_expect_words(test, ctx, 0, PAGE_SIZE);
}

/* Reading on until EOF returns every word once and nothing else. */
static void dbgview_test_read_sequential(struct kunit *test)
{
	resource_size_t size = 2 * PAGE_SIZE + 0x40;
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, size, true);
	u8 *all = kunit_kzalloc(test, size, GFP_KERNEL);
	unsigned int reads = 0;
	loff_t pos = 0;
	ssize_t ret;

	KUNIT_ASSERT_NOT_NULL(test, all);
	while ((ret = dbgview_test_read(ctx, 0x1000, &pos)) > 0) {
		KUNIT_ASSERT_LE(test, pos, size);
		KUNIT_ASSERT_EQ(test, copy_from_user(all + pos - ret, ctx->ubuf, ret), 0);
		KUNIT_ASSERT_LT(test, ++reads, 1000);
	}
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, pos, size);
	dbgview_test_compare_words(test, all, 0, size);
}

/* Every read takes and drops one runtime PM reference. */
static void dbgview_test_read_rpm_balanced(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0;
	int i;

	for (i = 0; i < 4; i++) {
		pos = 0;
		KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x40, &pos), 0x40);
		KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
	}
}

/* If the block cannot be powered, nothing is read or copied. */
static void dbgview_test_read_rpm_error(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, false);
	loff_t pos = 0;

	dbgview_test_poison_user(test, ctx);
	KUNIT_EXPECT_EQ(test, dbgview_test_read(ctx, 0x40, &pos), -EACCES);
	KUNIT_EXPECT_EQ(test, pos, 0);
	KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
	dbgview_test_fetch_user(test, ctx);
	KUNIT_EXPECT_EQ(test, ctx->kbuf[0], 0xee);
}

/* A bad user buffer fails the read and leaves the position alone. */
static void dbgview_test_read_efault(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, true);
	loff_t pos = 0;

	KUNIT_EXPECT_EQ(test, dbgview_regs_read(&ctx->file, (char __user *)NULL,
						0x40, &pos), -EFAULT);
	KUNIT_EXPECT_EQ(test, pos, 0);
	KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
}

/* Reads never change the block, nor the word past its end. */
static void dbgview_test_read_never_writes(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x80, true);
	loff_t pos = 0;

	while (dbgview_test_read(ctx, 0x14, &pos) > 0)
		;
	KUNIT_EXPECT_MEMEQ(test, ctx->regs, ctx->copy, 0x80 + 4);
}

static bool dbgview_test_has(const struct dbgview_test_ctx *ctx, const char *want)
{
	return strstr(ctx->seq_buf, want);
}

static bool dbgview_test_starts(const struct dbgview_test_ctx *ctx, const char *want)
{
	return str_has_prefix(ctx->seq_buf, want);
}

static unsigned int dbgview_test_lines(const char *buf)
{
	unsigned int n = 0;

	for (; *buf; buf++)
		if (*buf == '\n')
			n++;
	return n;
}

static int dbgview_test_snapshot(struct kunit *test,
				 struct dbgview_test_ctx *ctx, unsigned int bytes)
{
	struct seq_file s = { };
	int ret;

	ctx->seq_buf = kunit_kzalloc(test, DBGVIEW_TEST_SEQ_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->seq_buf);
	s.buf = ctx->seq_buf;
	s.size = DBGVIEW_TEST_SEQ_SIZE - 1;
	s.private = &ctx->dv;

	ret = dbgview_snapshot(&s, bytes);
	KUNIT_EXPECT_FALSE(test, seq_has_overflowed(&s));
	ctx->seq_buf[s.count] = '\0';
	return ret;
}

/* Counter, one line per word, counter, frequency. */
static void dbgview_test_snapshot_format(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x100, true);
	unsigned long long before, after;
	unsigned int frq;
	char line[32];
	int n;
	const char *p;
	size_t i;

	KUNIT_ASSERT_EQ(test, dbgview_test_snapshot(test, ctx, 0x40), 0);

	KUNIT_ASSERT_EQ(test, dbgview_test_lines(ctx->seq_buf), 16 + 3);
	n = sscanf(ctx->seq_buf, "cntvct_before %16llx\n", &before);
	KUNIT_ASSERT_EQ(test, n, 1);
	KUNIT_EXPECT_EQ(test, ctx->seq_buf[strlen("cntvct_before ") + 16], '\n');

	p = strchr(ctx->seq_buf, '\n') + 1;
	for (i = 0; i < 16; i++) {
		snprintf(line, sizeof(line), "%04zx %08x\n", i * 4,
			 dbgview_test_word(i));
		KUNIT_EXPECT_TRUE_MSG(test, str_has_prefix(p, line), "%s", line);
		p = strchr(p, '\n') + 1;
	}
	n = sscanf(p, "cntvct_after %16llx\n", &after);
	KUNIT_ASSERT_EQ(test, n, 1);
	p = strchr(p, '\n') + 1;
	n = sscanf(p, "cntfrq %u\n", &frq);
	KUNIT_ASSERT_EQ(test, n, 1);

	KUNIT_EXPECT_LE(test, before, after);
	KUNIT_EXPECT_NE(test, before, 0);
	KUNIT_EXPECT_EQ(test, frq, arch_timer_get_cntfrq());
	KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
}

/* The snapshot covers snap_bytes, rounded down to words, within the block. */
static void dbgview_test_snapshot_sizes(struct kunit *test)
{
	static const struct {
		resource_size_t size;
		unsigned int snap;
		unsigned int words;
	} cases[] = {
		{ 0x100, 0x40, 16 },
		{ 0x10, 0x40, 4 },	/* clamped to the block */
		{ 0x100, 8, 2 },
		{ 0x100, 6, 1 },	/* partial word dropped */
		{ 0x100, 3, 0 },
		{ 0x100, 0, 0 },
		{ 0x100, 0x100, 64 },
		{ 0x40, 0xffffffff, 16 },
	};
	char last[32];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct dbgview_test_ctx *ctx =
			dbgview_test_setup(test, cases[i].size, true);

		KUNIT_ASSERT_EQ(test, dbgview_test_snapshot(test, ctx, cases[i].snap), 0);
		KUNIT_EXPECT_EQ_MSG(test, dbgview_test_lines(ctx->seq_buf),
				    cases[i].words + 3, "case %zu", i);
		if (cases[i].words) {
			snprintf(last, sizeof(last), "\n%04x %08x\ncntvct_after ",
				 (cases[i].words - 1) * 4,
				 dbgview_test_word(cases[i].words - 1));
			KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(ctx->seq_buf, last),
						  "case %zu", i);
		} else {
			KUNIT_EXPECT_TRUE(test, dbgview_test_has(ctx, "\ncntvct_after "));
			KUNIT_EXPECT_TRUE(test, dbgview_test_starts(ctx, "cntvct_before "));
			KUNIT_EXPECT_NULL(test, strstr(ctx->seq_buf, "\n0000 "));
		}
		KUNIT_EXPECT_MEMEQ(test, ctx->regs, ctx->copy, cases[i].size + 4);
		kunit_device_unregister(test, ctx->dv.dev);
	}
}

static void dbgview_test_snapshot_rpm_error(struct kunit *test)
{
	struct dbgview_test_ctx *ctx = dbgview_test_setup(test, 0x40, false);

	KUNIT_EXPECT_EQ(test, dbgview_test_snapshot(test, ctx, 0x40), -EACCES);
	KUNIT_EXPECT_EQ(test, ctx->seq_buf[0], '\0');
	KUNIT_EXPECT_EQ(test, dbgview_test_usage(ctx), 0);
}

/* Neither file has a write method. */
static void dbgview_test_fops_read_only(struct kunit *test)
{
	const struct file_operations *snapshot = dbgview_test_snapshot_fops();

	KUNIT_EXPECT_NULL(test, dbgview_regs_fops.write);
	KUNIT_EXPECT_NULL(test, dbgview_regs_fops.write_iter);
	KUNIT_EXPECT_TRUE(test, dbgview_regs_fops.read == dbgview_regs_read);
	KUNIT_EXPECT_NULL(test, snapshot->write);
	KUNIT_EXPECT_NULL(test, snapshot->write_iter);
}

static struct kunit_case dbgview_test_cases[] = {
	KUNIT_CASE(dbgview_test_read_whole),
	KUNIT_CASE(dbgview_test_read_offset),
	KUNIT_CASE(dbgview_test_read_eof),
	KUNIT_CASE(dbgview_test_read_unaligned_pos),
	KUNIT_CASE(dbgview_test_read_short_count),
	KUNIT_CASE(dbgview_test_read_clamped),
	KUNIT_CASE(dbgview_test_read_page_cap),
	KUNIT_CASE(dbgview_test_read_sequential),
	KUNIT_CASE(dbgview_test_read_rpm_balanced),
	KUNIT_CASE(dbgview_test_read_rpm_error),
	KUNIT_CASE(dbgview_test_read_efault),
	KUNIT_CASE(dbgview_test_read_never_writes),
	KUNIT_CASE(dbgview_test_snapshot_format),
	KUNIT_CASE(dbgview_test_snapshot_sizes),
	KUNIT_CASE(dbgview_test_snapshot_rpm_error),
	KUNIT_CASE(dbgview_test_fops_read_only),
	{}
};

static struct kunit_suite dbgview_test_suite = {
	.name = "apple-dbgview",
	.test_cases = dbgview_test_cases,
};

kunit_test_suite(dbgview_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
