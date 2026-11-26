// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the RTKit crashlog parser. Included by rtkit-crashlog.c.
 *
 * apple_rtkit_crashlog_locate() is checked for the offsets it returns. The
 * dump tests place each log flush against the end of a vmalloc() area, so a
 * read past the log runs into the guard page and fails the case.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/vmalloc.h>

#define CL_TEST_AREA PAGE_SIZE
#define CL_TEST_ID_OUTER FOURCC('A', 'O', 'P', ' ')
#define CL_TEST_ID_INNER FOURCC('t', 's', 'k', '1')
#define CL_TEST_SECTION 16

struct cl_test_ctx {
	struct apple_rtkit *rtk;
	u8 *area;
};

static int cl_test_init(struct kunit *test)
{
	struct cl_test_ctx *ctx;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->rtk = kunit_kzalloc(test, sizeof(*ctx->rtk), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->rtk);
	dev = kunit_device_register(test, "apple-rtkit-crashlog-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ctx->rtk->dev = dev;
	ctx->area = vzalloc(CL_TEST_AREA);
	KUNIT_ASSERT_NOT_NULL(test, ctx->area);
	test->priv = ctx;
	return 0;
}

static void cl_test_exit(struct kunit *test)
{
	struct cl_test_ctx *ctx = test->priv;

	vfree(ctx->area);
}

/* A buffer of @size bytes that ends where the vmalloc guard page begins. */
static u8 *cl_test_tail(struct kunit *test, size_t size)
{
	struct cl_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_LE(test, size, CL_TEST_AREA);
	memset(ctx->area, 0, CL_TEST_AREA);
	return ctx->area + CL_TEST_AREA - size;
}

static void cl_test_put32(u8 *p, u32 v)
{
	memcpy(p, &v, sizeof(v));
}

static void cl_test_put64(u8 *p, u64 v)
{
	memcpy(p, &v, sizeof(v));
}

static void cl_test_log(u8 *p, u32 size, u32 flags)
{
	struct apple_rtkit_crashlog_header h = {
		.fourcc = APPLE_RTKIT_CRASHLOG_HEADER_FOURCC,
		.size = size,
		.flags = flags,
	};

	memcpy(p, &h, sizeof(h));
}

static void cl_test_container(u8 *p, u32 size, u32 current_id, u32 count)
{
	struct apple_rtkit_crashlog_container c = {
		.fourcc = APPLE_RTKIT_CRASHLOG_CONTAINER_FOURCC,
		.version = APPLE_RTKIT_CRASHLOG_CONTAINER_VERSION << 8,
		.size = size,
		.current_id = current_id,
		.count = count,
	};

	memcpy(p, &c, sizeof(c));
}

static void cl_test_record(u8 *container, u32 i, u64 id, u32 offset)
{
	struct apple_rtkit_crashlog_record r = {
		.id = id,
		.offset = offset,
	};

	memcpy(container + sizeof(struct apple_rtkit_crashlog_container) + i * sizeof(r),
	       &r, sizeof(r));
}

/* A section header: its fourcc and its size, which counts the header. */
static u8 *cl_test_section(u8 *p, u32 fourcc, u32 size)
{
	cl_test_put32(p, fourcc);
	cl_test_put32(p + 12, size);
	return p + size;
}

static int cl_test_locate(const u8 *bfr, size_t size, size_t *offset, u32 *id,
			  unsigned int *depth)
{
	*offset = ~0;
	*id = ~0;
	*depth = ~0;
	return apple_rtkit_crashlog_locate(bfr, size, offset, id, depth);
}

static void cl_test_locate_bare(struct kunit *test)
{
	u8 *bfr = cl_test_tail(test, 0x100);
	unsigned int depth;
	size_t offset;
	u32 id;

	cl_test_log(bfr, 0x100, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), 0);
	KUNIT_EXPECT_EQ(test, offset, 0);
	KUNIT_EXPECT_EQ(test, id, 0);
	KUNIT_EXPECT_EQ(test, depth, 0);

	/* A bare log is taken as it is, even marked as still being written. */
	cl_test_log(bfr, 0x40, APPLE_RTKIT_CRASHLOG_INCOMPLETE);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), 0);
}

static void cl_test_locate_bare_invalid(struct kunit *test)
{
	u8 *bfr = cl_test_tail(test, 0x100);
	unsigned int depth;
	size_t offset;
	u32 id;

	cl_test_log(bfr, 0x101, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_log(bfr, sizeof(struct apple_rtkit_crashlog_header) - 1, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_put32(bfr, FOURCC('C', 'L', 'H', 'F'));
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);

	/* Shorter than a header: nothing is read past the end. */
	bfr = cl_test_tail(test, sizeof(struct apple_rtkit_crashlog_header) - 1);
	cl_test_put32(bfr, APPLE_RTKIT_CRASHLOG_HEADER_FOURCC);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x1f, &offset, &id, &depth), -EINVAL);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0, &offset, &id, &depth), -EINVAL);
}

static void cl_test_locate_container(struct kunit *test)
{
	u8 *bfr = cl_test_tail(test, 0x200);
	unsigned int depth;
	size_t offset;
	u32 id;

	cl_test_container(bfr, 0x200, CL_TEST_ID_OUTER, 2);
	cl_test_record(bfr, 0, FOURCC('o', 't', 'h', 'r'), 0x80);
	cl_test_record(bfr, 1, CL_TEST_ID_OUTER, 0x100);
	cl_test_log(bfr + 0x80, 0x40, 0);
	cl_test_log(bfr + 0x100, 0x100, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x200, &offset, &id, &depth), 0);
	KUNIT_EXPECT_EQ(test, offset, 0x100);
	KUNIT_EXPECT_EQ(test, id, CL_TEST_ID_OUTER);
	KUNIT_EXPECT_EQ(test, depth, 1);

	/* The selected log may fill the rest of the buffer, but no more. */
	cl_test_log(bfr + 0x100, 0x101, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x200, &offset, &id, &depth), -EINVAL);

	/* A log still being written is retried later. */
	cl_test_log(bfr + 0x100, 0x100, APPLE_RTKIT_CRASHLOG_INCOMPLETE);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x200, &offset, &id, &depth), -EAGAIN);
}

static void cl_test_locate_nested(struct kunit *test)
{
	u8 *bfr = cl_test_tail(test, 0x400);
	unsigned int depth;
	size_t offset;
	u32 id;

	/* Per-coprocessor, then per-task: offsets are from each container. */
	cl_test_container(bfr, 0x400, CL_TEST_ID_OUTER, 1);
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x100);
	cl_test_container(bfr + 0x100, 0x300, CL_TEST_ID_INNER, 2);
	cl_test_record(bfr + 0x100, 0, FOURCC('t', 's', 'k', '0'), 0x80);
	cl_test_record(bfr + 0x100, 1, CL_TEST_ID_INNER, 0x200);
	cl_test_log(bfr + 0x300, 0x100, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x400, &offset, &id, &depth), 0);
	KUNIT_EXPECT_EQ(test, offset, 0x300);
	KUNIT_EXPECT_EQ(test, id, CL_TEST_ID_INNER);
	KUNIT_EXPECT_EQ(test, depth, 2);

	/*
	 * The inner log's size is checked against what follows it, not against
	 * the whole buffer: 0x180 fits the buffer but not the space left.
	 */
	cl_test_log(bfr + 0x300, 0x180, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x400, &offset, &id, &depth), -EINVAL);

	/* The same for an inner container claiming more than is left. */
	cl_test_log(bfr + 0x300, 0x100, 0);
	cl_test_container(bfr + 0x100, 0x301, CL_TEST_ID_INNER, 2);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x400, &offset, &id, &depth), -EINVAL);
}

static void cl_test_locate_bad_container(struct kunit *test)
{
	u8 *bfr = cl_test_tail(test, 0x100);
	unsigned int depth;
	size_t offset;
	u32 id;

#define CL_TEST_RESET() do {						\
		cl_test_container(bfr, 0x100, CL_TEST_ID_OUTER, 1);	\
		cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x80);		\
		cl_test_log(bfr + 0x80, 0x80, 0);			\
	} while (0)

	CL_TEST_RESET();
	KUNIT_ASSERT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), 0);

	/* Another container version. */
	cl_test_put32(bfr + 4, 3 << 8);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	CL_TEST_RESET();
	/* Larger than the buffer. */
	cl_test_container(bfr, 0x101, CL_TEST_ID_OUTER, 1);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	/* No records, or more than the buffer can hold. */
	cl_test_container(bfr, 0x100, CL_TEST_ID_OUTER, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_container(bfr, 0x100, CL_TEST_ID_OUTER, (0x100 - 0x14) / 0x18 + 1);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	/* No record has the current ID. */
	cl_test_container(bfr, 0x100, CL_TEST_ID_INNER, 1);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	/* A record pointing back into its container, at its end, or past it. */
	CL_TEST_RESET();
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x13);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x100);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0xffffffff);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
	/* A record leading to too little for a header. */
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0xf0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, 0x100, &offset, &id, &depth), -EINVAL);
#undef CL_TEST_RESET
}

static void cl_test_locate_depth(struct kunit *test)
{
	unsigned int max = APPLE_RTKIT_CRASHLOG_CONTAINER_MAX_DEPTH, depth, i;
	size_t len = 0x40 * (max + 2), offset;
	u8 *bfr = cl_test_tail(test, len);
	u32 id;

	/* @max containers, each leading 0x40 bytes on, then a log. */
	for (i = 0; i < max; i++) {
		cl_test_container(bfr + 0x40 * i, len - 0x40 * i, i + 1, 1);
		cl_test_record(bfr + 0x40 * i, 0, i + 1, 0x40);
	}
	cl_test_log(bfr + 0x40 * max, 0x40, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, len, &offset, &id, &depth), 0);
	KUNIT_EXPECT_EQ(test, depth, max);
	KUNIT_EXPECT_EQ(test, offset, 0x40 * max);
	KUNIT_EXPECT_EQ(test, id, max);

	/* One container more is too deep. */
	cl_test_container(bfr + 0x40 * max, len - 0x40 * max, max + 1, 1);
	cl_test_record(bfr + 0x40 * max, 0, max + 1, 0x40);
	cl_test_log(bfr + 0x40 * (max + 1), 0x40, 0);
	KUNIT_EXPECT_EQ(test, cl_test_locate(bfr, len, &offset, &id, &depth), -EINVAL);
}

/* Every decoder, ending with a footer. The dump must reach it in bounds. */
static void cl_test_dump_full(struct kunit *test)
{
	struct cl_test_ctx *ctx = test->priv;
	size_t len = 0x20 + 0x30 + 0x30 + 0x18 + (28 + 2 * 24) + (16 + 0x350) +
		     APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE + 0x30 + 0x10 + CL_TEST_SECTION;
	u8 *bfr = cl_test_tail(test, len), *p;

	cl_test_log(bfr, len, 0);
	p = bfr + 0x20;
	memcpy(p + 20, "panic: test\nsecond line", 23);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_STR, 0x30);
	memset(p + 16, 'v', 0x30 - 16);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_VERSION, 0x30);
	cl_test_put64(p + 16, 1234);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_TIME, 0x18);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_MBOX, 28 + 2 * 24);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_REGS, 16 + 0x350);
	cl_test_put32(p + APPLE_RTKIT_CRASHLOG_CL4_SIZE, APPLE_RTKIT_CRASHLOG_CL4_NOMINAL_SIZE);
	p = cl_test_section(p, APPLE_RTKIT_CRASHLOG_REGS_CL4,
			    APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE);
	p = cl_test_section(p, FOURCC('C', 'c', 's', 't'), 0x30);
	/* An empty unknown section: a header only. */
	p = cl_test_section(p, FOURCC('C', 'x', 'y', 'z'), 0x10);
	cl_test_section(p, APPLE_RTKIT_CRASHLOG_HEADER_FOURCC, CL_TEST_SECTION);
	KUNIT_ASSERT_PTR_EQ(test, p + CL_TEST_SECTION, bfr + len);

	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	KUNIT_SUCCEED(test);
}

/*
 * The last section of a log without a footer: its decoder could read 16
 * bytes past the log, so it is not decoded.
 */
static void cl_test_dump_no_footer(struct kunit *test)
{
	static const u32 fourccs[] = {
		APPLE_RTKIT_CRASHLOG_STR, APPLE_RTKIT_CRASHLOG_VERSION,
		APPLE_RTKIT_CRASHLOG_MBOX, APPLE_RTKIT_CRASHLOG_TIME,
		FOURCC('C', 'c', 's', 't'),
	};
	struct cl_test_ctx *ctx = test->priv;
	size_t len = 0x20 + 0x40;
	u8 *bfr;
	int i;

	for (i = 0; i < ARRAY_SIZE(fourccs); i++) {
		bfr = cl_test_tail(test, len);
		cl_test_log(bfr, len, 0);
		/* No newline and no terminator: Cstr must stop at the section. */
		memset(bfr + 0x20 + 16, 'x', 0x40 - 16);
		cl_test_section(bfr + 0x20, fourccs[i], 0x40);
		apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	}
	KUNIT_SUCCEED(test);
}

static void cl_test_dump_bad_sizes(struct kunit *test)
{
	struct cl_test_ctx *ctx = test->priv;
	size_t len = 0x80;
	u8 *bfr = cl_test_tail(test, len);

	/* A log claiming more than the buffer. */
	cl_test_log(bfr, len + 0x1000, 0);
	cl_test_section(bfr + 0x20, APPLE_RTKIT_CRASHLOG_VERSION, len - 0x20 + 0x800);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);

	/* A section larger than what is left of the log. */
	cl_test_log(bfr, len, 0);
	cl_test_section(bfr + 0x20, APPLE_RTKIT_CRASHLOG_VERSION, len - 0x20 + 1);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);

	/* Sizes that would never advance or underflow a decoder. */
	cl_test_section(bfr + 0x20, APPLE_RTKIT_CRASHLOG_VERSION, 0);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	cl_test_section(bfr + 0x20, APPLE_RTKIT_CRASHLOG_VERSION, 15);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);

	/* A section header cut off by the end of the log. */
	bfr = cl_test_tail(test, 0x28);
	cl_test_log(bfr, 0x28, 0);
	cl_test_put32(bfr + 0x20, APPLE_RTKIT_CRASHLOG_VERSION);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, 0x28);

	/* Too small for a header. */
	apple_rtkit_crashlog_dump(ctx->rtk, cl_test_tail(test, 0x1f), 0x1f);
	KUNIT_SUCCEED(test);
}

/* Sections too small for their decoders are skipped, not decoded. */
static void cl_test_dump_small_sections(struct kunit *test)
{
	static const u32 fourccs[] = {
		APPLE_RTKIT_CRASHLOG_MBOX, APPLE_RTKIT_CRASHLOG_REGS,
		APPLE_RTKIT_CRASHLOG_REGS_CL4, APPLE_RTKIT_CRASHLOG_TIME,
	};
	struct cl_test_ctx *ctx = test->priv;
	size_t len = 0x20 + 0x10 + CL_TEST_SECTION;
	u8 *bfr;
	int i;

	for (i = 0; i < ARRAY_SIZE(fourccs); i++) {
		bfr = cl_test_tail(test, len);
		cl_test_log(bfr, len, 0);
		cl_test_section(bfr + 0x20, fourccs[i], 0x10);
		cl_test_section(bfr + 0x30, APPLE_RTKIT_CRASHLOG_HEADER_FOURCC,
				CL_TEST_SECTION);
		apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	}

	/* A Crl4 frame of another size is not decoded either. */
	len = 0x20 + APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE + CL_TEST_SECTION;
	bfr = cl_test_tail(test, len);
	cl_test_log(bfr, len, 0);
	cl_test_put32(bfr + 0x20 + APPLE_RTKIT_CRASHLOG_CL4_SIZE, 0xd0);
	cl_test_section(bfr + 0x20, APPLE_RTKIT_CRASHLOG_REGS_CL4,
			APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	KUNIT_SUCCEED(test);
}

static void cl_test_dump_container(struct kunit *test)
{
	struct cl_test_ctx *ctx = test->priv;
	size_t len = 0x100;
	u8 *bfr = cl_test_tail(test, len);

	/* The selected log ends the buffer; its last section has no footer. */
	cl_test_container(bfr, len, CL_TEST_ID_OUTER, 1);
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x80);
	cl_test_log(bfr + 0x80, 0x80, 0);
	memset(bfr + 0xa0 + 16, 'x', 0x60 - 16);
	cl_test_section(bfr + 0xa0, APPLE_RTKIT_CRASHLOG_VERSION, 0x60);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);

	/* A container whose record leads past the end is not followed. */
	cl_test_record(bfr, 0, CL_TEST_ID_OUTER, 0x1000);
	apple_rtkit_crashlog_dump(ctx->rtk, bfr, len);
	KUNIT_SUCCEED(test);
}

static struct kunit_case apple_rtkit_crashlog_test_cases[] = {
	KUNIT_CASE(cl_test_locate_bare),
	KUNIT_CASE(cl_test_locate_bare_invalid),
	KUNIT_CASE(cl_test_locate_container),
	KUNIT_CASE(cl_test_locate_nested),
	KUNIT_CASE(cl_test_locate_bad_container),
	KUNIT_CASE(cl_test_locate_depth),
	KUNIT_CASE(cl_test_dump_full),
	KUNIT_CASE(cl_test_dump_no_footer),
	KUNIT_CASE(cl_test_dump_bad_sizes),
	KUNIT_CASE(cl_test_dump_small_sections),
	KUNIT_CASE(cl_test_dump_container),
	{}
};

static struct kunit_suite apple_rtkit_crashlog_test_suite = {
	.name = "apple-rtkit-crashlog",
	.init = cl_test_init,
	.exit = cl_test_exit,
	.test_cases = apple_rtkit_crashlog_test_cases,
};

kunit_test_suite(apple_rtkit_crashlog_test_suite);
