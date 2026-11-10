// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <kunit/test.h>

#include "apple_mcc_pmu.h"

static void apple_mcc_t8112_encoding_test(struct kunit *test)
{
	u64 agent = T8112_AGENT(
		T8112_REQUESTER(T8112_REQ_GRP_CPU, T8112_REQ_MEMBER_PCPU), 4);
	u64 read = t8112_control_word(agent, false);
	u64 write = t8112_control_word(agent, true);

	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_REQUESTER_GROUP, agent),
			(u64)T8112_REQ_GRP_CPU);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_REQUESTER_MEMBER, agent),
			(u64)T8112_REQ_MEMBER_PCPU);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_REQUESTER_MASK, agent),
			(u64)T8112_REQUESTER_RANGE(4));
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_DIRECTION, read),
			(u64)T8112_DIRECTION_READ_BYTES);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_DIRECTION, write),
			(u64)T8112_DIRECTION_WRITE_BYTES);
	KUNIT_EXPECT_TRUE(test, read & T8112_SEL_VALID);
	KUNIT_EXPECT_TRUE(test, read & T8112_SEL_FAMILY);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_SEL_SUBREQUEST, read), 0ULL);
}

static void apple_mcc_t6000_encoding_test(struct kunit *test)
{
	u64 agent = T6000_AGENT(T6000_REQUESTER(T6000_REQ_GRP_PCPU0,
						T6000_REQ_MEMBER_BASE),
				T6000_REQ_RANGE_4);
	u64 read = t6000_amcc_control_word(agent, false);
	u64 write = t6000_amcc_control_word(agent, true);

	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_SEL_REQUESTER_GROUP, read),
			(u64)T6000_REQ_GRP_PCPU0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_SEL_REQUESTER_MEMBER, read),
			(u64)T6000_REQ_MEMBER_BASE);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_SEL_REQUESTER_WILDCARDS, read),
			(u64)T6000_REQ_RANGE_4);
	KUNIT_EXPECT_TRUE(test, read & T6000_SEL_REQUESTER_MATCH);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_AMCC_SEL_EVENT_BANK, read),
			(u64)T6000_AMCC_BANK_DIRECT_BYTES);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_AMCC_SEL_EVENT_CODE, read),
			(u64)(T6000_AMCC_TXN_BIDIRECTIONAL |
			      T6000_AMCC_TXN_READ_ONLY));
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_AMCC_SEL_EVENT_BANK, write),
			(u64)(T6000_AMCC_BANK_DIRECT_BYTES |
			      T6000_AMCC_BANK_WRITE));
	KUNIT_EXPECT_EQ(test, FIELD_GET(T6000_AMCC_SEL_EVENT_CODE, write),
			(u64)(T6000_AMCC_TXN_BIDIRECTIONAL |
			      T6000_AMCC_TXN_WRITE_ONLY));
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(T6000_AMCC_SEL_COUNTER_CLASS, read),
			(u64)T6000_AMCC_COUNTER_PROGRAMMABLE);
	KUNIT_EXPECT_EQ(test,
			t6000_amcc_transaction_mask(false) &
				T6000_AMCC_TXN_WRITE_ONLY,
			(u16)0);
	KUNIT_EXPECT_EQ(test,
			t6000_amcc_transaction_mask(true) &
				T6000_AMCC_TXN_READ_ONLY,
			(u16)0);
	KUNIT_EXPECT_EQ(test,
			t6000_amcc_transaction_mask(false) &
				T6000_AMCC_TXN_BIDIRECTIONAL,
			(u16)T6000_AMCC_TXN_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test,
			t6000_amcc_transaction_mask(true) &
				T6000_AMCC_TXN_BIDIRECTIONAL,
			(u16)T6000_AMCC_TXN_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(T6000_DCS_SEL_EVENT,
				  t6000_dcs_control_word(agent, false)),
			(u64)T6000_DCS_EVENT_READ_BYTES);
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(T6000_DCS_SEL_EVENT,
				  t6000_dcs_control_word(agent, true)),
			(u64)T6000_DCS_EVENT_WRITE_BYTES);
}

static void apple_mcc_command_encoding_test(struct kunit *test)
{
	u32 desc0 = t8112_write_desc0();
	u32 desc1 = t8112_write_desc1();
	u32 desc3 = t8112_write_desc3();

	KUNIT_EXPECT_TRUE(test, desc0 & T8112_WR_DESC0_ENABLE);
	KUNIT_EXPECT_TRUE(test, desc0 & T8112_WR_DESC0_REQUESTER_FILTER);
	KUNIT_EXPECT_EQ(test, FIELD_GET(T8112_WR_DESC1_EVENT_CLASS, desc1),
			(u32)FIELD_MAX(T8112_WR_DESC1_EVENT_CLASS));
	KUNIT_EXPECT_TRUE(test, desc1 & T8112_WR_DESC1_WRITE_BYTES);
	KUNIT_EXPECT_EQ(test, desc3, (u32)T8112_WR_DESC3_REQUESTER_HI_MASK);
	KUNIT_EXPECT_EQ(test,
			FIELD_GET(T8112_WR_GLOBAL_EVENT_CLASS,
				  t8112_write_global_event_class()),
			(u32)FIELD_MAX(T8112_WR_GLOBAL_EVENT_CLASS));
	KUNIT_EXPECT_TRUE(test,
			  t6000_amcc_commit_word() & T6000_AMCC_CMD_APPLY);
	KUNIT_EXPECT_TRUE(test,
			  t6000_amcc_commit_word() & T6000_AMCC_CMD_CLEAR);
}

static struct kunit_case apple_mcc_pmu_test_cases[] = {
	KUNIT_CASE(apple_mcc_t8112_encoding_test),
	KUNIT_CASE(apple_mcc_t6000_encoding_test),
	KUNIT_CASE(apple_mcc_command_encoding_test),
	{}
};

static struct kunit_suite apple_mcc_pmu_test_suite = {
	.name = "apple-mcc-pmu-encoding",
	.test_cases = apple_mcc_pmu_test_cases,
};

kunit_test_suite(apple_mcc_pmu_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
