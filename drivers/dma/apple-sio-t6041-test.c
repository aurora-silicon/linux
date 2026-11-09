// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by apple-sio.c: the T6041 SIO boot contract. */
#include <kunit/test.h>

struct sio_t6041_test {
	struct device dev;
	struct sio_data sio;
	u32 keys[32];
	u32 values[32];
	unsigned int count;
	u32 fail_key;
};

#define T6041_WIN	0x10000118000ULL	/* a window the loader mapped */

/* What a T6041-aware loader passes: J616s has no optional tables. */
static const u32 sio_t6041_loader[] = {
	0x1e, 0x10000118, 0x1f, 0x30,
	0x0f, 0x1000011c, 0x10, 0x1e000,
	0x30d, 0x1000013c, 0x30e, 0x4000,
	0x1a, 0x10000140, 0x1b, 0x40,
	0x22, 0x10000141, 0x23, 0xd8,
	0x1c, 0x10000142, 0x1d, 0x48,
};

/* The firmware's order, the scalar after the register tables, before the shims. */
static const u32 sio_t6041_expected[][2] = {
	{ 0x0f, 0x1000011c }, { 0x10, 0x1e000 },
	{ 0x1a, 0x10000140 }, { 0x1b, 0x40 },
	{ 0x1e, 0x10000118 }, { 0x1f, 0x30 },
	{ 0x2a, 0 },
	{ 0x22, 0x10000141 }, { 0x23, 0xd8 },
	{ 0x1c, 0x10000142 }, { 0x1d, 0x48 },
	{ 0x30d, 0x1000013c }, { 0x30e, 0x4000 },
};

static const struct sio_t6030_window sio_t6041_windows[] = {
	{ T6041_WIN, 0x2d000 },
};

static struct sio_t6041_test *sio_t6041_alloc(struct kunit *test)
{
	struct sio_t6041_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->sio.dev = &t->dev;
	t->sio.hw = &sio_hw_t6041;
	t->fail_key = U32_MAX;
	return t;
}

static int sio_t6041_set_param(struct sio_data *sio, u32 key, u32 val)
{
	struct sio_t6041_test *t = container_of(sio, struct sio_t6041_test, sio);

	if (t->count >= ARRAY_SIZE(t->keys))
		return -E2BIG;
	t->keys[t->count] = key;
	t->values[t->count++] = val;
	return key == t->fail_key ? -ETIME : 0;
}

static int sio_t6041_run(struct sio_t6041_test *t, const u32 *params, unsigned int n)
{
	u32 addr[ARRAY_SIZE(sio_t6041_params)] = {};
	u32 size[ARRAY_SIZE(sio_t6041_params)] = {};
	int err;

	t->count = 0;
	err = sio_t6041_collect(params, n, sio_t6041_windows, ARRAY_SIZE(sio_t6041_windows),
				addr, size);
	if (err)
		return err;
	return sio_t6041_send_params(&t->sio, addr, size, sio_t6041_set_param);
}

static void sio_t6041_order_test(struct kunit *test)
{
	struct sio_t6041_test *t = sio_t6041_alloc(test);
	u32 params[ARRAY_SIZE(sio_t6041_loader)];
	unsigned int i, n = ARRAY_SIZE(sio_t6041_loader), rev;

	for (rev = 0; rev < 2; rev++) {
		for (i = 0; i < n; i += 2) {
			unsigned int src = rev ? n - i - 2 : i;

			params[i] = sio_t6041_loader[src];
			params[i + 1] = sio_t6041_loader[src + 1];
		}
		KUNIT_ASSERT_EQ(test, sio_t6041_run(t, params, n), 0);
		KUNIT_ASSERT_EQ(test, t->count, ARRAY_SIZE(sio_t6041_expected));
		for (i = 0; i < t->count; i++) {
			KUNIT_EXPECT_EQ(test, t->keys[i], sio_t6041_expected[i][0]);
			KUNIT_EXPECT_EQ(test, t->values[i], sio_t6041_expected[i][1]);
		}
	}
}

static void sio_t6041_optional_test(struct kunit *test)
{
	struct sio_t6041_test *t = sio_t6041_alloc(test);
	u32 params[ARRAY_SIZE(sio_t6041_loader) + 8];
	unsigned int n = ARRAY_SIZE(sio_t6041_loader);

	memcpy(params, sio_t6041_loader, sizeof(sio_t6041_loader));
	/* SIO registers, then misc tunables: both go before the scalar, in table order */
	params[n++] = 0x28; params[n++] = 0x10000143;
	params[n++] = 0x29; params[n++] = 0x18;
	params[n++] = 0x20; params[n++] = 0x10000144;
	params[n++] = 0x21; params[n++] = 0x30;
	KUNIT_ASSERT_EQ(test, sio_t6041_run(t, params, n), 0);
	KUNIT_ASSERT_EQ(test, t->count, ARRAY_SIZE(sio_t6041_expected) + 4);
	KUNIT_EXPECT_EQ(test, t->keys[6], 0x20U);
	KUNIT_EXPECT_EQ(test, t->keys[7], 0x21U);
	KUNIT_EXPECT_EQ(test, t->keys[8], 0x28U);
	KUNIT_EXPECT_EQ(test, t->values[9], 0x18U);
	KUNIT_EXPECT_EQ(test, t->keys[10], 0x2aU);
	KUNIT_EXPECT_EQ(test, t->keys[11], 0x22U);
	/* Half an optional table */
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n - 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, t->count, 0U);
}

static void sio_t6041_reject_test(struct kunit *test)
{
	struct sio_t6041_test *t = sio_t6041_alloc(test);
	u32 params[ARRAY_SIZE(sio_t6041_loader) + 2];
	unsigned int n = ARRAY_SIZE(sio_t6041_loader), i;
	/* Each required size, one off: protocol-9 shims, eight device types... */
	static const u32 wrong[][2] = {
		{ 0x23, 0xa0 }, { 0x23, 6 * 32 }, { 0x1d, 0x40 }, { 0x1b, 0x50 },
		{ 0x10, 0x1d000 }, { 0x1f, 0x18 }, { 0x30e, 0x8000 },
	};
	/* Keys the loader must not pass: arrays, the scalar, unknown */
	static const u32 foreign[] = { 0x0b, 0x0c, 0x01, 0x02, 0x2a, 0x1234, 0x10b };

	for (i = 0; i < ARRAY_SIZE(wrong); i++) {
		unsigned int j;

		memcpy(params, sio_t6041_loader, sizeof(sio_t6041_loader));
		for (j = 0; j < n; j += 2)
			if (params[j] == wrong[i][0])
				params[j + 1] = wrong[i][1];
		KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n), -EINVAL);
	}
	for (i = 0; i < ARRAY_SIZE(foreign); i++) {
		memcpy(params, sio_t6041_loader, sizeof(sio_t6041_loader));
		params[n] = foreign[i];
		params[n + 1] = 0x10000145;
		KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n + 2), -EINVAL);
	}
	/* A duplicate, a missing required table, an odd count, nothing */
	memcpy(params, sio_t6041_loader, sizeof(sio_t6041_loader));
	params[n] = 0x1e;
	params[n + 1] = 0x10000118;
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n + 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n - 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n - 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, 0), -EINVAL);
	/* Outside the mapped window: below it, and running past its end */
	params[1] = 0x10000100;
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n), -EINVAL);
	params[1] = 0x10000143;	/* 0x30 bytes from the window's last page: fits */
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n), 0);
	params[5] = 0x10000130;	/* the panic buffer would end past the window */
	KUNIT_EXPECT_EQ(test, sio_t6041_run(t, params, n), -EINVAL);
	KUNIT_EXPECT_EQ(test, t->count, 0U);
}

static void sio_t6041_ack_failure_test(struct kunit *test)
{
	struct sio_t6041_test *t = sio_t6041_alloc(test);
	unsigned int fail;

	for (fail = 0; fail < ARRAY_SIZE(sio_t6041_expected); fail++) {
		t->fail_key = sio_t6041_expected[fail][0];
		KUNIT_EXPECT_EQ(test, sio_t6041_run(t, sio_t6041_loader,
						    ARRAY_SIZE(sio_t6041_loader)), -ETIME);
		/* Nothing follows a refused parameter, a table's size included. */
		KUNIT_EXPECT_EQ(test, t->count, fail + 1);
	}
}

static void sio_t6041_arrays_test(struct kunit *test)
{
	struct sio_t6041_test *t = sio_t6041_alloc(test);
	static const u32 keys[] = { 1, 2, 0x0b, 0x0c };
	static const u32 values[] = { 0x10200000, 0xaaa, 0x10300000, 0x1cc0 };
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sio_t6041_arrays.segments * sio_t6041_arrays.segment_size,
			0x7ff8U);
	KUNIT_EXPECT_EQ(test, sio_t6041_arrays.responses * sio_t6041_arrays.response_size,
			0x1cc00U);
	/* Every endpoint and tag has its response: index endpoint * 32 + tag - 1. */
	KUNIT_EXPECT_LT(test, (NCHANNELS_MAX - 1) * 32 + SIO_NTAGS - 2,
			(int)sio_t6041_arrays.responses);
	t->sio.arrays = &sio_t6041_arrays;
	t->sio.shmem_iova = 0x10200000000ULL;
	t->sio.responses_iova = 0x10300000000ULL;
	KUNIT_ASSERT_EQ(test, sio_send_v9_shmem(&t->sio, sio_t6041_set_param), 0);
	KUNIT_ASSERT_EQ(test, t->count, ARRAY_SIZE(keys));
	for (i = 0; i < t->count; i++) {
		KUNIT_EXPECT_EQ(test, t->keys[i], keys[i]);
		KUNIT_EXPECT_EQ(test, t->values[i], values[i]);
	}
	/* The response count's ACK is the last step: a refusal there fails the boot. */
	t->count = 0;
	t->fail_key = 0x0c;
	KUNIT_EXPECT_EQ(test, sio_send_v9_shmem(&t->sio, sio_t6041_set_param), -ETIME);
	KUNIT_EXPECT_EQ(test, t->count, 4U);
}

static void sio_t6041_config_test(struct kunit *test)
{
	struct sio_shmem_chan_config cfg = {
		.datashape = 2, .timeout = 0, .fifo = 0x800,
		.threshold = sio_hw_t6041.fifo_threshold, .limit = sio_hw_t6041.cfg_word4,
	};
	/* macOS 26.6 DisplayPort audio records */
	static const u32 t6041[] = { 2, 0, 0x800, 0x600, 0x800, 0 };
	static const u32 t8140[] = { 2, 0, 0x800, 0x800, 0x800, 0 };
	u32 words[8];

	memset(words, 0xa5, sizeof(words));
	sio_write_config(&sio_hw_t6041, words, &cfg);
	KUNIT_EXPECT_MEMEQ(test, words, t6041, sizeof(t6041));
	KUNIT_EXPECT_EQ(test, words[6], 0xa5a5a5a5U);

	memset(words, 0xa5, sizeof(words));
	cfg.threshold = sio_hw_t8140.fifo_threshold;
	cfg.limit = sio_hw_t8140.cfg_word4;
	sio_write_config(&sio_hw_t8140, words, &cfg);
	KUNIT_EXPECT_MEMEQ(test, words, t8140, sizeof(t8140));
	KUNIT_EXPECT_EQ(test, words[6], 0xa5a5a5a5U);
	cfg.threshold = sio_hw_t6041.fifo_threshold;
	cfg.limit = sio_hw_t6041.cfg_word4;

	/* The other layouts keep their five words and leave the sixth alone. */
	memset(words, 0xa5, sizeof(words));
	cfg.threshold = sio_hw_t6030.fifo_threshold;
	cfg.limit = sio_hw_t6030.cfg_word4;
	sio_write_config(&sio_hw_t6030, words, &cfg);
	KUNIT_EXPECT_EQ(test, words[3], 0x600U);
	KUNIT_EXPECT_EQ(test, words[4], 0x800U);
	KUNIT_EXPECT_EQ(test, words[5], 0xa5a5a5a5U);
}

static void sio_t6041_match_test(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = sio_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, "apple,t6041-sio"))
			break;
	KUNIT_ASSERT_TRUE(test, id->compatible[0] != '\0');
	KUNIT_EXPECT_PTR_EQ(test, id->data, (const void *)&sio_hw_t6041);
	/* T8140 takes the T6041 tables, with its own channel record */
	for (id = sio_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, "apple,t8140-sio"))
			break;
	KUNIT_ASSERT_TRUE(test, id->compatible[0] != '\0');
	KUNIT_EXPECT_PTR_EQ(test, id->data, (const void *)&sio_hw_t8140);
	KUNIT_EXPECT_EQ(test, sio_hw_t8140.protocol, 10U);
	KUNIT_EXPECT_PTR_EQ(test, sio_hw_t8140.arrays, &sio_t6041_arrays);
	KUNIT_EXPECT_TRUE(test, sio_hw_t8140.check_boot_tables == sio_t6041_check_boot_tables);
	KUNIT_EXPECT_EQ(test, sio_hw_t6041.protocol, 10U);
	KUNIT_EXPECT_TRUE(test, sio_hw_t6041.clear_period_desc);
	KUNIT_EXPECT_PTR_EQ(test, sio_hw_t6041.arrays, &sio_t6041_arrays);
	/* Protocol 10 alone is T6030's too, which keeps its own contract. */
	KUNIT_EXPECT_EQ(test, sio_hw_t6030.protocol, 0U);
	KUNIT_EXPECT_NULL(test, sio_hw_t6030.arrays);
	/* The one-segment descriptor overlay: handle slot * 4, IOVA at +72, length at +80 */
	KUNIT_EXPECT_EQ(test, 56 + offsetof(struct sio_coproc_desc, flag), 60U);
	KUNIT_EXPECT_EQ(test, 56 + offsetof(struct sio_coproc_desc, iova), 72U);
	KUNIT_EXPECT_EQ(test, 56 + offsetof(struct sio_coproc_desc, size), 80U);
	KUNIT_EXPECT_EQ(test, sizeof(struct sio_coproc_desc), 4U * SIO_T6041_SEGMENT_SIZE);
}

static struct kunit_case sio_t6041_test_cases[] = {
	KUNIT_CASE(sio_t6041_order_test),
	KUNIT_CASE(sio_t6041_optional_test),
	KUNIT_CASE(sio_t6041_reject_test),
	KUNIT_CASE(sio_t6041_ack_failure_test),
	KUNIT_CASE(sio_t6041_arrays_test),
	KUNIT_CASE(sio_t6041_config_test),
	KUNIT_CASE(sio_t6041_match_test),
	{}
};

static struct kunit_suite sio_t6041_test_suite = {
	.name = "apple-sio-t6041",
	.test_cases = sio_t6041_test_cases,
};

kunit_test_suite(sio_t6041_test_suite);
