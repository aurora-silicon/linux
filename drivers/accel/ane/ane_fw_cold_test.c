// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_fw_cold.h"

static struct ane_fw_cold_state ane_cold_fixture(void)
{
	struct ane_fw_cold_state s = {
		.sys = 0x23ff, .cpu = 0x0f000300, .subblock = 0x300,
		.gate_request = 3, .rvbar = 0x10000000001, .status = 0x6a,
	};

	for (unsigned int i = 0; i < ANE_FW_DARTS; i++) {
		s.dart[i].version = 0x2a2a0203;
		s.dart[i].protect = i == 2 ? 0 : 2;
		s.dart[i].unprotect = i == 2 ? 0 : 2;
		s.dart[i].lock = i == 2 ? 0 : 2;
	}
	return s;
}

static void ane_cold_evidence_test(struct kunit *test)
{
	struct ane_fw_cold_state s = ane_cold_fixture();

	KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, true));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, false));
	/* Physical ANE_SYS on is the measured cold state, despite genpd off. */
	s.sys = 0x2300;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
}

static void ane_cold_warm_test(struct kunit *test)
{
	struct ane_fw_cold_state s, original = ane_cold_fixture();
	u32 *word;
	static const size_t offsets[] = {
		offsetof(struct ane_fw_cold_state, cpu),
		offsetof(struct ane_fw_cold_state, subblock),
		offsetof(struct ane_fw_cold_state, gate_request),
		offsetof(struct ane_fw_cold_state, gate_ack),
		offsetof(struct ane_fw_cold_state, control),
		offsetof(struct ane_fw_cold_state, status),
		offsetof(struct ane_fw_cold_state, pending),
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
		s = original;
		word = (u32 *)((u8 *)&s + offsets[i]);
		*word ^= 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
	}
	for (unsigned int i = 0; i < 8; i++) {
		s = original;
		s.scratch[i] = 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
	}
	s = original;
	s.rvbar++;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
	s = original;
	s.ce_state = 1;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
	s = original;
	s.ce_error = BIT_ULL(63);
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
}

static void ane_cold_dart_test(struct kunit *test)
{
	struct ane_fw_cold_state s, original = ane_cold_fixture();

	for (unsigned int i = 0; i < ANE_FW_DARTS; i++) {
		s = original;
		s.dart[i].version++;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
		s = original;
		s.dart[i].protect |= BIT(0);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
		s = original;
		s.dart[i].lock |= BIT(2);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
		s = original;
		s.dart[i].error = 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
		for (unsigned int j = 0; j < 16; j++) {
			s = original;
			s.dart[i].enabled[j] = BIT(31);
			KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, true));
		}
	}
}

static void ane_cold_command_policy_test(struct kunit *test)
{
	static const u16 forbidden[] = { 0x27, 0x28, 0x30, 0x31, 0x34 };
	static const u16 permitted[] = { 0, 3, 4, 0x22, 0x2d, 0x2f, 0x200, 0x202, 0x20c };

	for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++)
		KUNIT_EXPECT_TRUE(test, ane_fw_command_denied(forbidden[i]));
	for (unsigned int i = 0; i < ARRAY_SIZE(permitted); i++)
		KUNIT_EXPECT_FALSE(test, ane_fw_command_denied(permitted[i]));
}

static struct kunit_case ane_cold_cases[] = {
	KUNIT_CASE(ane_cold_evidence_test),
	KUNIT_CASE(ane_cold_warm_test),
	KUNIT_CASE(ane_cold_dart_test),
	KUNIT_CASE(ane_cold_command_policy_test),
	{}
};

static struct kunit_suite ane_cold_suite = {
	.name = "ane-fw-cold",
	.test_cases = ane_cold_cases,
};

kunit_test_suite(ane_cold_suite);

MODULE_DESCRIPTION("KUnit tests for cold ANE acquisition and H17 program policy");
MODULE_LICENSE("Dual MIT/GPL");
