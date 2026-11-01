// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by apple-sio.c: exercise protocol-9 publication and ACK failures. */
#include <kunit/test.h>

struct sio_boot_test {
	struct device dev;
	struct sio_data *sio;
	u32 keys[20];
	u32 values[20];
	unsigned int count;
	u32 fail_key;
};

/* The loader order, including its response-array parameters. */
static const u32 sio_boot_loader_params[] = {
	0x1e, 0x30, 0x1f, 0x30,
	0x0b, 0x31, 0x0c, 0x1b80,
	0x0f, 0x34, 0x10, 0x1e000,
	0x30d, 0x54, 0x30e, 0x4000,
	0x1a, 0x58, 0x1b, 0x40,
	0x22, 0x59, 0x23, 0xa0,
	0x1c, 0x5a, 0x1d, 0x40,
};

/* Protocol-9 control and performance order, independent of loader order. */
static const u32 sio_boot_expected_keys[] = {
	0x0f, 0x10, 0x1a, 0x1b, 0x1e, 0x1f,
	0x22, 0x23, 0x1c, 0x1d, 0x30d, 0x30e,
};

static struct sio_boot_test *sio_boot_test_alloc(struct kunit *test)
{
	struct sio_boot_test *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->sio = kunit_kzalloc(test, sizeof(*ctx->sio), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->sio);
	ctx->sio->dev = &ctx->dev;
	ctx->fail_key = U32_MAX;
	return ctx;
}

static int sio_boot_set_param(struct sio_data *sio, u32 key, u32 val)
{
	struct sio_boot_test *ctx = container_of(sio->dev, struct sio_boot_test, dev);

	if (ctx->count >= ARRAY_SIZE(ctx->keys))
		return -E2BIG;
	ctx->keys[ctx->count] = key;
	ctx->values[ctx->count++] = val;
	return key == ctx->fail_key ? -ETIME : 0;
}

static void sio_boot_params(__be32 *params, bool reverse)
{
	unsigned int i, n = ARRAY_SIZE(sio_boot_loader_params);

	for (i = 0; i < n; i += 2) {
		unsigned int src = reverse ? n - i - 2 : i;

		params[i] = cpu_to_be32(sio_boot_loader_params[src]);
		params[i + 1] = cpu_to_be32(sio_boot_loader_params[src + 1]);
	}
}

static void sio_boot_order_test(struct kunit *test)
{
	struct sio_boot_test *ctx = sio_boot_test_alloc(test);
	__be32 params[ARRAY_SIZE(sio_boot_loader_params)];
	unsigned int i, reverse;
	int err;

	for (reverse = 0; reverse < 2; reverse++) {
		ctx->count = 0;
		sio_boot_params(params, reverse);
		err = sio_send_v9_dt_params(ctx->sio, params, ARRAY_SIZE(params),
					    sio_boot_set_param);
		KUNIT_ASSERT_EQ(test, err, 0);
		KUNIT_ASSERT_EQ(test, ctx->count, ARRAY_SIZE(sio_boot_expected_keys));
		for (i = 0; i < ctx->count; i++)
			KUNIT_EXPECT_EQ(test, ctx->keys[i], sio_boot_expected_keys[i]);
		/* Reordering must preserve every loader-supplied control value. */
		for (i = 0; i < ctx->count; i++) {
			unsigned int j;

			for (j = 0; j < ARRAY_SIZE(sio_boot_loader_params); j += 2)
				if (sio_boot_loader_params[j] == ctx->keys[i])
					break;
			KUNIT_ASSERT_LT(test, j, ARRAY_SIZE(sio_boot_loader_params));
			KUNIT_EXPECT_EQ(test, ctx->values[i], sio_boot_loader_params[j + 1]);
		}
		KUNIT_EXPECT_EQ(test, ctx->values[7], 0xa0U);
		KUNIT_EXPECT_EQ(test, ctx->values[11], 0x4000U);
	}
}

static void sio_boot_optional_tables_test(struct kunit *test)
{
	struct sio_boot_test *ctx = sio_boot_test_alloc(test);
	__be32 params[ARRAY_SIZE(sio_boot_loader_params) + 8];
	unsigned int n = ARRAY_SIZE(sio_boot_loader_params);

	sio_boot_params(params, false);
	params[n++] = cpu_to_be32(0x24);
	params[n++] = cpu_to_be32(0x60);
	params[n++] = cpu_to_be32(0x25);
	params[n++] = cpu_to_be32(0x20);
	params[n++] = cpu_to_be32(0x20);
	params[n++] = cpu_to_be32(0x61);
	params[n++] = cpu_to_be32(0x21);
	params[n++] = cpu_to_be32(0x10);
	KUNIT_ASSERT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n,
						    sio_boot_set_param), 0);
	KUNIT_ASSERT_EQ(test, ctx->count, 16U);
	KUNIT_EXPECT_EQ(test, ctx->keys[6], 0x20U);
	KUNIT_EXPECT_EQ(test, ctx->keys[8], 0x24U);
	KUNIT_EXPECT_EQ(test, ctx->keys[10], 0x22U);
	KUNIT_EXPECT_EQ(test, ctx->keys[14], 0x30dU);
}

static void sio_boot_bad_params_test(struct kunit *test)
{
	struct sio_boot_test *ctx = sio_boot_test_alloc(test);
	__be32 params[ARRAY_SIZE(sio_boot_loader_params) + 2];
	unsigned int n = ARRAY_SIZE(sio_boot_loader_params);

	sio_boot_params(params, false);
	KUNIT_EXPECT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n - 1,
						    sio_boot_set_param), -EINVAL);
	/* A missing required pair, an unknown key, a duplicate, and half a table. */
	KUNIT_EXPECT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n - 2,
						    sio_boot_set_param), -EINVAL);
	params[n] = cpu_to_be32(0x1234);
	params[n + 1] = 0;
	KUNIT_EXPECT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n + 2,
						    sio_boot_set_param), -EINVAL);
	params[n] = cpu_to_be32(0x22);
	KUNIT_EXPECT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n + 2,
						    sio_boot_set_param), -EINVAL);
	params[n] = cpu_to_be32(0x20);
	KUNIT_EXPECT_EQ(test, sio_send_v9_dt_params(ctx->sio, params, n + 2,
						    sio_boot_set_param), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->count, 0U);
}

static void sio_boot_control_ack_failure_test(struct kunit *test)
{
	struct sio_boot_test *ctx = sio_boot_test_alloc(test);
	__be32 params[ARRAY_SIZE(sio_boot_loader_params)];
	unsigned int fail;
	int err;

	sio_boot_params(params, false);
	for (fail = 0; fail < ARRAY_SIZE(sio_boot_expected_keys); fail++) {
		ctx->count = 0;
		ctx->fail_key = sio_boot_expected_keys[fail];
		err = sio_send_v9_dt_params(ctx->sio, params, ARRAY_SIZE(params),
					    sio_boot_set_param);
		KUNIT_EXPECT_EQ(test, err, -ETIME);
		/* No dependent table follows a failed ACK, including shim size. */
		KUNIT_ASSERT_EQ(test, ctx->count, fail + 1);
		KUNIT_EXPECT_EQ(test, ctx->keys[fail], ctx->fail_key);
	}
}

static void sio_boot_dma_arrays_test(struct kunit *test)
{
	struct sio_boot_test *ctx = sio_boot_test_alloc(test);
	const u32 keys[] = { 1, 2, 0x0b, 0x0c };
	const u32 values[] = { 0x1234, 0xaaa, 0x5678, 0x1b80 };
	unsigned int i, fail;

	ctx->sio->shmem_iova = 0x1234000;
	ctx->sio->responses_iova = 0x5678000;
	KUNIT_ASSERT_EQ(test, sio_send_v9_shmem(ctx->sio, sio_boot_set_param), 0);
	KUNIT_ASSERT_EQ(test, ctx->count, ARRAY_SIZE(keys));
	for (i = 0; i < ctx->count; i++) {
		KUNIT_EXPECT_EQ(test, ctx->keys[i], keys[i]);
		KUNIT_EXPECT_EQ(test, ctx->values[i], values[i]);
	}
	/* Every failed ACK stops publication, including the segment count. */
	for (fail = 0; fail < ARRAY_SIZE(keys); fail++) {
		ctx->count = 0;
		ctx->fail_key = keys[fail];
		KUNIT_EXPECT_EQ(test, sio_send_v9_shmem(ctx->sio, sio_boot_set_param), -ETIME);
		KUNIT_EXPECT_EQ(test, ctx->count, fail + 1);
	}
}

static struct kunit_case sio_boot_test_cases[] = {
	KUNIT_CASE(sio_boot_order_test),
	KUNIT_CASE(sio_boot_optional_tables_test),
	KUNIT_CASE(sio_boot_bad_params_test),
	KUNIT_CASE(sio_boot_control_ack_failure_test),
	KUNIT_CASE(sio_boot_dma_arrays_test),
	{}
};

static struct kunit_suite sio_boot_test_suite = {
	.name = "apple-sio-boot",
	.test_cases = sio_boot_test_cases,
};

kunit_test_suite(sio_boot_test_suite);
