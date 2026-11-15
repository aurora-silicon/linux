// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-sio.c: the T6030 SIO boot-table check.
 *
 * sio_t6030_build() lays the tables out the way the boot loader does (m1n1's add_fwdata(): a
 * table shares the previous mapping while it fits in that mapping's 16K, the mappings start at
 * IOVA 0x30000 and end up 16K-aligned). It builds both a T6030-aware loader's protocol-10 tables
 * and an earlier loader's.
 */
#include <kunit/of.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>

#define SIO_TEST_PARAMS 32

struct sio_t6030_layout {
	u32 params[SIO_TEST_PARAMS];
	unsigned int nparams;
	struct sio_t6030_window windows[SIO_T6030_WINDOWS_MAX];
	unsigned int nwindows;
};

static void sio_t6030_add(struct sio_t6030_layout *l, u32 key, u32 size)
{
	struct sio_t6030_window *last = l->nwindows ? &l->windows[l->nwindows - 1] : NULL;
	u64 iova;

	if (last && ALIGN(last->size, SZ_16K) >= last->size + size) {
		iova = last->start + last->size;
		last->size = ALIGN(last->size + size, SZ_4K);
	} else {
		iova = last ? ALIGN(last->start + last->size, SZ_16K) : 0x30000;
		l->windows[l->nwindows].start = iova;
		l->windows[l->nwindows].size = ALIGN(size, SZ_4K);
		l->nwindows++;
	}
	l->params[l->nparams++] = key;
	l->params[l->nparams++] = iova >> 12;
	l->params[l->nparams++] = key + 1;
	l->params[l->nparams++] = size;
}

/* shims and types: the DMA shim and device-type table sizes this loader builds */
static void sio_t6030_build(struct sio_t6030_layout *l, u32 shims, u32 types)
{
	unsigned int i;

	memset(l, 0, sizeof(*l));
	sio_t6030_add(l, 0x1e, 0x300);	/* tunables, from the ADT */
	sio_t6030_add(l, 0x0b, 0x1b80);
	sio_t6030_add(l, 0x0f, 0x1e000);	/* panic buffer */
	sio_t6030_add(l, 0x30d, 0x4000);
	sio_t6030_add(l, 0x1a, 4 * 16);	/* map ranges */
	sio_t6030_add(l, 0x22, shims);
	sio_t6030_add(l, 0x1c, types);
	for (i = 0; i < l->nwindows; i++)
		l->windows[i].size = ALIGN(l->windows[i].size, SZ_16K);
}

static int sio_t6030_check(const struct sio_t6030_layout *l)
{
	return sio_t6030_check_params(l->params, l->nparams, l->windows, l->nwindows);
}

static u32 *sio_t6030_value(struct sio_t6030_layout *l, u32 key)
{
	unsigned int i;

	for (i = 0; i < l->nparams; i += 2)
		if (l->params[i] == key)
			return &l->params[i + 1];
	return NULL;
}

/* A T6030-aware loader's tables (six 36-byte shims, nine types) are admitted. */
static void sio_t6030_protocol10_test(struct kunit *test)
{
	struct sio_t6030_layout l;

	sio_t6030_build(&l, 6 * 36, 9 * 8);
	KUNIT_EXPECT_EQ(test, l.nwindows, 4U);
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), 0);
}

/* An earlier loader's tables (five 32-byte shims, eight types) are refused. */
static void sio_t6030_earlier_loader_test(struct kunit *test)
{
	struct sio_t6030_layout l;

	sio_t6030_build(&l, 5 * 32, 8 * 8);
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* Either table alone of the earlier size refuses too */
	sio_t6030_build(&l, 5 * 32, 9 * 8);
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	sio_t6030_build(&l, 6 * 36, 8 * 8);
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
}

/* Other record counts, such as a later protocol's, are refused. */
static void sio_t6030_wrong_counts_test(struct kunit *test)
{
	static const u32 sizes[][2] = {
		{ 5 * 36, 9 * 8 }, { 7 * 36, 9 * 8 }, { 6 * 32, 9 * 8 }, { 6 * 40, 9 * 8 },
		{ 6 * 36, 10 * 8 }, { 6 * 36, 9 * 16 }, { 0, 9 * 8 }, { 6 * 36, 0 },
	};
	struct sio_t6030_layout l;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		sio_t6030_build(&l, sizes[i][0], sizes[i][1]);
		KUNIT_EXPECT_EQ_MSG(test, sio_t6030_check(&l), -EINVAL, "case %u", i);
	}
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	*sio_t6030_value(&l, 0x1b) = 5 * 16;	/* map ranges */
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
}

/* Malformed and truncated parameter lists are refused. */
static void sio_t6030_malformed_test(struct kunit *test)
{
	struct sio_t6030_layout l;
	unsigned int i;

	sio_t6030_build(&l, 6 * 36, 9 * 8);
	KUNIT_EXPECT_EQ(test, sio_t6030_check_params(l.params, 0, l.windows, l.nwindows), -EINVAL);
	KUNIT_EXPECT_EQ(test, sio_t6030_check_params(l.params, l.nparams - 1, l.windows,
						     l.nwindows), -EINVAL);
	/* Truncated after any pair: the last table's size is always lost */
	for (i = 2; i < l.nparams; i += 2)
		KUNIT_EXPECT_EQ_MSG(test, sio_t6030_check_params(l.params, i, l.windows,
								 l.nwindows),
				    -EINVAL, "cut at %u", i);
	/* A key twice, even with the same value, for a table or any other parameter */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams] = 0x22;
	l.params[l.nparams + 1] = *sio_t6030_value(&l, 0x22);
	l.nparams += 2;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams] = 0x1e;
	l.params[l.nparams + 1] = *sio_t6030_value(&l, 0x1e);
	l.nparams += 2;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* A table without its size, or its size without the table */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams - 2] = 0x40;	/* 0x1d replaced */
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams - 4] = 0x41;	/* 0x1c replaced */
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* More pairs than the controller takes */
	{
		static u32 many[2 * (SIO_T6030_PARAMS_MAX + 1)];

		sio_t6030_build(&l, 6 * 36, 9 * 8);
		memset(many, 0, sizeof(many));
		memcpy(many, l.params, l.nparams * sizeof(u32));
		for (i = l.nparams; i < ARRAY_SIZE(many); i += 2)
			many[i] = 0x1000 + i;
		KUNIT_EXPECT_EQ(test, sio_t6030_check_params(many, ARRAY_SIZE(many), l.windows,
							     l.nwindows), -EINVAL);
		KUNIT_EXPECT_EQ(test, sio_t6030_check_params(many, ARRAY_SIZE(many) - 2,
							     l.windows, l.nwindows), 0);
	}
}

/*
 * A key wider than 16 bits is refused: the firmware gets its low 16 bits as
 * the endpoint and parameter bytes, so it would overwrite a checked parameter.
 */
static void sio_t6030_key_alias_test(struct kunit *test)
{
	static const u32 aliases[] = { 0x1001a, 0xff00001a, 0x1000022, 0x10023, 0x1001c, 0x1001d };
	struct sio_t6030_layout l;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(aliases); i++) {
		u32 key = aliases[i], checked;

		/* A run-time value, as the driver has: FIELD_PREP() masks it */
		OPTIMIZER_HIDE_VAR(key);
		checked = key & 0xffff;
		/* The setup message sio_send_dt_params() sends is that of the checked key */
		KUNIT_EXPECT_EQ(test, FIELD_PREP(SIOMSG_PARAM, key & 0xff) |
				      FIELD_PREP(SIOMSG_EP, key >> 8),
				FIELD_PREP(SIOMSG_PARAM, checked & 0xff) |
				FIELD_PREP(SIOMSG_EP, checked >> 8));
		sio_t6030_build(&l, 6 * 36, 9 * 8);
		KUNIT_ASSERT_NOT_NULL(test, sio_t6030_value(&l, checked));
		l.params[l.nparams] = key;
		l.params[l.nparams + 1] = 0x80;
		l.nparams += 2;
		KUNIT_EXPECT_EQ_MSG(test, sio_t6030_check(&l), -EINVAL, "key %#x", key);
	}
	/* Wider than 16 bits but aliasing no listed key: refused all the same */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams] = 0x10099;
	l.params[l.nparams + 1] = 0;
	l.nparams += 2;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* Any 16-bit key is its own message, and is admitted */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.params[l.nparams] = 0xffff;
	l.params[l.nparams + 1] = 0;
	l.nparams += 2;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), 0);
}

/* Each table must lie inside one mapped window; spans must not overflow. */
static void sio_t6030_spans_test(struct kunit *test)
{
	struct sio_t6030_layout l;

	sio_t6030_build(&l, 6 * 36, 9 * 8);
	KUNIT_EXPECT_EQ(test, sio_t6030_check_params(l.params, l.nparams, l.windows, 0), -EINVAL);
	/* The shims' page outside every window */
	*sio_t6030_value(&l, 0x22) = 0x80;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/*
	 * Ending exactly at the end of their window, and one byte past it. The window
	 * keeps the map ranges and the shims; the types get a window of their own.
	 */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.windows[3].size = 0x59000 - 0x58000 + 6 * 36;
	l.windows[4].start = 0x5a000;
	l.windows[4].size = SZ_16K;
	l.nwindows = 5;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), 0);
	l.windows[3].size--;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* Starting at the start of their window, and one byte before it */
	l.windows[3].size++;
	l.windows[3].start += SZ_4K;
	l.windows[3].size -= SZ_4K;
	*sio_t6030_value(&l, 0x1a) = 0x59;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), 0);
	l.windows[3].start += 1;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	/* Beyond the controller's 42-bit DMA range, or in a window that wraps around */
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	*sio_t6030_value(&l, 0x22) = U32_MAX;
	l.windows[3].start = 0;
	l.windows[3].size = U64_MAX;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
	sio_t6030_build(&l, 6 * 36, 9 * 8);
	l.windows[3].start = U64_MAX - 0xfff;
	l.windows[3].size = 0x2000;
	KUNIT_EXPECT_EQ(test, sio_t6030_check(&l), -EINVAL);
}

OF_OVERLAY_DECLARE(apple_sio_t6030_test);

/* The device-tree side: the parameters and windows a loader writes, read back. */
static void sio_t6030_device_tree_test(struct kunit *test)
{
	static const char *const nodes[][2] = {
		{ "/sio-t6030-test/sio-t6030-test-v10", "admitted" },
		{ "/sio-t6030-test/sio-t6030-test-earlier", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-unmapped", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-other-owner", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-no-params", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-odd", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-bad-window", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-owner-none", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-owner-unknown", "refused" },
		{ "/sio-t6030-test/sio-t6030-test-many-windows", "refused" },
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, apple_sio_t6030_test));
	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		struct device_node *np __free(device_node) = of_find_node_by_path(nodes[i][0]);
		int ret;

		KUNIT_ASSERT_NOT_NULL_MSG(test, np, "%s", nodes[i][0]);
		ret = sio_t6030_check_node(np);
		if (!strcmp(nodes[i][1], "admitted"))
			KUNIT_EXPECT_EQ_MSG(test, ret, 0, "%s", nodes[i][0]);
		else
			KUNIT_EXPECT_NE_MSG(test, ret, 0, "%s", nodes[i][0]);
	}
}

/*
 * The probe refuses an earlier loader's tables before it maps a register. The test devices
 * have no register range, so a probe that gets past the check stops there with -EINVAL.
 */
static void sio_t6030_probe_test(struct kunit *test)
{
	static const struct {
		const char *path;
		int err;
	} cases[] = {
		{ "/sio-t6030-test/sio-t6030-test-earlier", -ENODEV },
		{ "/sio-t6030-test/sio-t6030-test-v10", -EINVAL },
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, apple_sio_t6030_test));
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct platform_device *pdev;

		pdev = kunit_platform_device_alloc(test, "apple-sio-t6030-test", i);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
		/* The device's release drops this reference */
		pdev->dev.of_node = of_find_node_by_path(cases[i].path);
		KUNIT_ASSERT_NOT_NULL_MSG(test, pdev->dev.of_node, "%s", cases[i].path);
		KUNIT_EXPECT_EQ_MSG(test, sio_probe(pdev), cases[i].err, "%s", cases[i].path);
	}
}

static struct kunit_case sio_t6030_test_cases[] = {
	KUNIT_CASE(sio_t6030_protocol10_test),
	KUNIT_CASE(sio_t6030_earlier_loader_test),
	KUNIT_CASE(sio_t6030_wrong_counts_test),
	KUNIT_CASE(sio_t6030_malformed_test),
	KUNIT_CASE(sio_t6030_key_alias_test),
	KUNIT_CASE(sio_t6030_spans_test),
	KUNIT_CASE(sio_t6030_device_tree_test),
	KUNIT_CASE(sio_t6030_probe_test),
	{}
};

static struct kunit_suite sio_t6030_test_suite = {
	.name = "apple-sio-t6030",
	.test_cases = sio_t6030_test_cases,
};

kunit_test_suite(sio_t6030_test_suite);
