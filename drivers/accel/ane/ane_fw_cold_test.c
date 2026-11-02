// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_fw_cold.h"
#include "ane_fw_sleep.h"

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
	/*
	 * ANE_SYS is no marker: it may read on while genpd reports off, and
	 * generic DART runtime PM powers it off again.
	 */
	s.sys = 0x2300;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, true));
}

/*
 * Each step judges only what its power state makes readable: the probe
 * step only PMGR words, so a warm engine cannot fail it, and the engine
 * step nothing in PMGR.
 */
static void ane_cold_step_test(struct kunit *test)
{
	struct ane_fw_cold_state s = ane_cold_fixture();

	s.control = 0x10;
	s.dart[0].enabled[0] = 1;
	s.gate_request = 0;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_pmgr_valid(&s, true));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_gate_valid(&s));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_valid(&s));
	s = ane_cold_fixture();
	s.cpu = 0x0f0003ff;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_pmgr_valid(&s, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_gate_valid(&s));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_engine_valid(&s));
	s = ane_cold_fixture();
	s.subblock = 0x3ff;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_pmgr_valid(&s, true));
	s = ane_cold_fixture();
	s.gate_ack = 1;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_pmgr_valid(&s, true));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_gate_valid(&s));
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
		/* The bound DART driver enables all streams; that is not a refusal. */
		for (unsigned int j = 0; j < 16; j++) {
			s = original;
			s.dart[i].enabled[j] = U32_MAX;
			KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, true));
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

/* The measured 26.6.2 layout: DATA ends 0x14000 short of the span. */
static void ane_cold_heap_tail_test(struct kunit *test)
{
	u64 dva[2] = { 0x10000000000ULL, 0x100000c0000ULL };
	u64 size[2] = { 0xc0000, 0x2ac000 };
	const u64 pool = 0x10000384000ULL;
	u64 tail = 0, tail_size = 0;

	KUNIT_ASSERT_EQ(test, ane_fw_heap_tail(dva, size, pool, &tail, &tail_size), 0);
	KUNIT_EXPECT_EQ(test, tail, 0x1000036c000ULL);
	KUNIT_EXPECT_EQ(test, tail_size, 0x14000ULL);
	/* The faulting writes of neo-run-0045B lie inside the tail. */
	KUNIT_EXPECT_LE(test, tail, 0x1000037c040ULL);
	KUNIT_EXPECT_GT(test, tail + tail_size, 0x1000037fc00ULL);
	/* DATA that ends the span leaves nothing to map. */
	size[1] = ANE_FW_SPAN - 0xc0000;
	KUNIT_ASSERT_EQ(test, ane_fw_heap_tail(dva, size, pool, &tail, &tail_size), 0);
	KUNIT_EXPECT_EQ(test, tail_size, 0ULL);
	/* DATA past the span, a partial page, overlap, or the pool: refused. */
	size[1] = ANE_FW_SPAN;
	KUNIT_EXPECT_EQ(test, ane_fw_heap_tail(dva, size, pool, &tail, &tail_size), -ERANGE);
	size[1] = 0x2ab000;
	KUNIT_EXPECT_EQ(test, ane_fw_heap_tail(dva, size, pool, &tail, &tail_size), -ERANGE);
	size[1] = 0x2ac000;
	dva[1] = 0x100000bc000ULL;
	KUNIT_EXPECT_EQ(test, ane_fw_heap_tail(dva, size, pool, &tail, &tail_size), -ERANGE);
	dva[1] = 0x100000c0000ULL;
	KUNIT_EXPECT_EQ(test, ane_fw_heap_tail(dva, size, 0x10000370000ULL, &tail,
					       &tail_size), -ERANGE);
}

/* The hand-off that starts the engine at boot, and each thing that stops it. */
static void ane_cold_handoff_test(struct kunit *test)
{
	static const struct ane_fw_platform soc = {
		.preload_dva = { 0x10000000000ULL, 0x100000c0000ULL },
		.preload_size = { 0xc0000, 0x2ac000 },
	};
	static const struct ane_fw_cold_state cold = {
		.sys = 0x23ff, .cpu = 0x0f000300, .subblock = 0x300,
	};
	const struct ane_fw_handoff good = {
		.pmgr = &cold,
		.marker = ANE_FW_COLD_BOOT_EVIDENCE,
		.preloaded = true,
		.regions_ok = true,
		.phys = { 0x10000768000ULL, 0x100019a0000ULL },
		.size = { 0xc0000, 0x2ac000 },
		.dva = { 0x10000000000ULL, 0x100000c0000ULL },
		.text_ok = true,
	};
	struct ane_fw_handoff h;

	KUNIT_EXPECT_TRUE(test, ane_fw_cold_handoff_valid(&good, &soc));
	h = good;
	h.pmgr = NULL;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.marker = NULL;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h.marker = "raw-no-xnu-ane-untouched-v2";
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.preloaded = false;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	/* Disabled placeholders (an older loader). */
	h = good;
	h.regions_ok = false;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.size[1] = 0x2a8000;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.dva[0] = 0x10000004000ULL;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.phys[1] = h.phys[0];
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h = good;
	h.phys[0] += 0x1000;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	/* Another firmware build. */
	h = good;
	h.text_ok = false;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
}

/*
 * kexec copies /chosen, so a kernel after one that ran the ANE still sees
 * the raw-boot marker: the live PMGR state, not the marker, decides.
 */
static void ane_cold_handoff_warm_test(struct kunit *test)
{
	static const struct ane_fw_platform soc = {
		.preload_dva = { 0x10000000000ULL, 0x100000c0000ULL },
		.preload_size = { 0xc0000, 0x2ac000 },
	};
	struct ane_fw_cold_state warm = {
		.sys = 0x23ff, .cpu = 0x0f00030f, .subblock = 0x300,
	};
	struct ane_fw_handoff h = {
		.marker = ANE_FW_COLD_BOOT_EVIDENCE,
		.preloaded = true,
		.regions_ok = true,
		.phys = { 0x10000768000ULL, 0x100019a0000ULL },
		.size = { 0xc0000, 0x2ac000 },
		.dva = { 0x10000000000ULL, 0x100000c0000ULL },
		.text_ok = true,
		.pmgr = &warm,
	};

	/* Marker present, ANE CPU powered: warm. */
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_handoff_loader_ok(&h, &soc));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	/* Sub-block on. */
	warm.cpu = 0x0f000300;
	warm.subblock = 0x30f;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
	/* Cold PMGR but a kdump kernel. */
	warm.subblock = 0x300;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_handoff_valid(&h, &soc));
	h.crash_kernel = true;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_handoff_valid(&h, &soc));
}

/*
 * While the firmware is retained every sleep state is refused at prepare;
 * the matching post events and anything without retained firmware pass.
 */
static void ane_cold_sleep_test(struct kunit *test)
{
	static const unsigned long refused[] = {
		PM_SUSPEND_PREPARE, PM_HIBERNATION_PREPARE, PM_RESTORE_PREPARE,
	};
	static const unsigned long passed[] = {
		PM_POST_SUSPEND, PM_POST_HIBERNATION, PM_POST_RESTORE,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(refused); i++) {
		KUNIT_EXPECT_TRUE(test, ane_fw_sleep_refused(refused[i], true));
		KUNIT_EXPECT_EQ(test, ane_fw_sleep_event(refused[i], true), NOTIFY_BAD);
		KUNIT_EXPECT_EQ(test, ane_fw_sleep_event(refused[i], false), NOTIFY_DONE);
	}
	for (i = 0; i < ARRAY_SIZE(passed); i++) {
		KUNIT_EXPECT_EQ(test, ane_fw_sleep_event(passed[i], true), NOTIFY_DONE);
		KUNIT_EXPECT_EQ(test, ane_fw_sleep_event(passed[i], false), NOTIFY_DONE);
	}
}

static struct kunit_case ane_cold_cases[] = {
	KUNIT_CASE(ane_cold_sleep_test),
	KUNIT_CASE(ane_cold_handoff_warm_test),
	KUNIT_CASE(ane_cold_handoff_test),
	KUNIT_CASE(ane_cold_heap_tail_test),
	KUNIT_CASE(ane_cold_evidence_test),
	KUNIT_CASE(ane_cold_step_test),
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
