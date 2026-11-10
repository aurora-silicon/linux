// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by dcp-smc.c to test the native AFK SMC message bridge. */

#include <kunit/test.h>

struct dcp_smc_test_ctx {
	struct apple_smc smc;
	struct dcp_desktop_state desktop;
	struct apple_dcp dcp;
	struct apple_dcp_afkep ep;
	struct apple_epic_service service;
	smc_key expected_key;
	u32 value;
	int result;
	unsigned int reads;
};

static int dcp_smc_test_read(struct apple_smc *smc, smc_key key,
			     void *data, size_t size)
{
	struct dcp_smc_test_ctx *ctx = container_of(smc, typeof(*ctx), smc);

	ctx->reads++;
	if (key != ctx->expected_key)
		return -EINVAL;
	memcpy(data, &ctx->value, min(size, sizeof(ctx->value)));
	return ctx->result;
}

static int dcp_smc_test_init(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->expected_key = SMC_KEY(TEST);
	ctx->value = 0x44332211;
	ctx->result = sizeof(ctx->value);
	ctx->desktop.smc = &ctx->smc;
	ctx->dcp.desktop = &ctx->desktop;
	ctx->ep.dcp = &ctx->dcp;
	ctx->service.ep = &ctx->ep;
	test->priv = ctx;
	kunit_activate_static_stub(test, dcp_smc_read_key, dcp_smc_test_read);
	return 0;
}

static void dcp_smc_afk_envelope_test(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx = test->priv;
	__le64 request = cpu_to_le64(FIELD_PREP(DCP_SMC_MSG, DCP_SMC_MSG_READ_KEY) |
		FIELD_PREP(DCP_SMC_SIZE, 4) |
		FIELD_PREP(DCP_SMC_DATA, ctx->expected_key));
	__le64 response = 0;

	KUNIT_ASSERT_EQ(test,
			dcp_smc_afk_call(&ctx->service, 9, &request,
					 sizeof(request), &response, sizeof(response)),
			0);
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(DCP_SMC_DATA, le64_to_cpu(response)),
			(u64)ctx->value);
	KUNIT_EXPECT_EQ(test,
			dcp_smc_afk_call(&ctx->service, 9, &request,
					 sizeof(request) - 1, &response,
					 sizeof(response)),
			-EMSGSIZE);
}

static void dcp_smc_read_message_test(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx = test->priv;
	u64 request = FIELD_PREP(DCP_SMC_MSG, DCP_SMC_MSG_READ_KEY) |
		FIELD_PREP(DCP_SMC_SIZE, sizeof(ctx->value)) |
		FIELD_PREP(DCP_SMC_ID, 7) |
		FIELD_PREP(DCP_SMC_DATA, ctx->expected_key);
	u64 response = U64_MAX;

	KUNIT_ASSERT_EQ(test, dcp_smc_handle_message(&ctx->smc, request, &response), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_RESULT, response), 0ULL);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_SIZE, response), 4ULL);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_ID, response), 7ULL);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_DATA, response), (u64)ctx->value);
	KUNIT_EXPECT_EQ(test, ctx->reads, 1U);
}

static void dcp_smc_read_failure_test(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx = test->priv;
	u64 request = FIELD_PREP(DCP_SMC_MSG, DCP_SMC_MSG_READ_KEY) |
		FIELD_PREP(DCP_SMC_SIZE, 4) |
		FIELD_PREP(DCP_SMC_ID, 3) |
		FIELD_PREP(DCP_SMC_DATA, ctx->expected_key);
	u64 response = U64_MAX;

	ctx->result = -EIO;
	KUNIT_ASSERT_EQ(test, dcp_smc_handle_message(&ctx->smc, request, &response), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_RESULT, response),
			(u64)DCP_SMC_RESULT_FAILED);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_SIZE, response), 0ULL);
	KUNIT_EXPECT_EQ(test, FIELD_GET(DCP_SMC_ID, response), 3ULL);
}

static void dcp_smc_reject_non_read_test(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx = test->priv;
	u64 response = 0;
	u64 request = FIELD_PREP(DCP_SMC_MSG, DCP_SMC_MSG_READ_KEY + 1) |
		FIELD_PREP(DCP_SMC_SIZE, 4) |
		FIELD_PREP(DCP_SMC_DATA, ctx->expected_key);

	KUNIT_EXPECT_EQ(test, dcp_smc_handle_message(&ctx->smc, request, &response),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->reads, 0U);
}

static void dcp_smc_reject_size_test(struct kunit *test)
{
	struct dcp_smc_test_ctx *ctx = test->priv;
	u64 response = 0;
	u64 request = FIELD_PREP(DCP_SMC_MSG, DCP_SMC_MSG_READ_KEY) |
		FIELD_PREP(DCP_SMC_DATA, ctx->expected_key);

	KUNIT_EXPECT_EQ(test, dcp_smc_handle_message(&ctx->smc, request, &response),
			-EMSGSIZE);
	request |= FIELD_PREP(DCP_SMC_SIZE, DCP_SMC_INLINE_MAX + 1);
	KUNIT_EXPECT_EQ(test, dcp_smc_handle_message(&ctx->smc, request, &response),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, ctx->reads, 0U);
}

static struct kunit_case dcp_smc_test_cases[] = {
	KUNIT_CASE(dcp_smc_afk_envelope_test),
	KUNIT_CASE(dcp_smc_read_message_test),
	KUNIT_CASE(dcp_smc_read_failure_test),
	KUNIT_CASE(dcp_smc_reject_non_read_test),
	KUNIT_CASE(dcp_smc_reject_size_test),
	{}
};

static struct kunit_suite dcp_smc_test_suite = {
	.name = "apple-dcp-smc-afk",
	.init = dcp_smc_test_init,
	.test_cases = dcp_smc_test_cases,
};

kunit_test_suite(dcp_smc_test_suite);
