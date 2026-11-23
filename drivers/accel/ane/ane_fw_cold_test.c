// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/soc/apple/clpc.h>

#include "ane_fw_cold.h"
#include "ane_fw_sleep.h"

static const struct ane_fw_platform ane_cold_t8140 = { .dart_revision = 0x203 };
static const struct ane_fw_platform ane_cold_t6050 = { .dart_revision = 0x204 };

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

	KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, false));
	/*
	 * ANE_SYS is no marker: it may read on while genpd reports off, and
	 * generic DART runtime PM powers it off again.
	 */
	s.sys = 0x2300;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	/* Each SoC's ANE DARTs report their own revision; another one is refused. */
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t6050, true));
	for (unsigned int i = 0; i < ANE_FW_DARTS; i++)
		s.dart[i].version = 0x2a2a0204;
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, &ane_cold_t6050, true));
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_engine_restartable(&s, &ane_cold_t6050));
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
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_valid(&s, &ane_cold_t8140));
	s = ane_cold_fixture();
	s.cpu = 0x0f0003ff;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_pmgr_valid(&s, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_gate_valid(&s));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_engine_valid(&s, &ane_cold_t8140));
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
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	}
	for (unsigned int i = 0; i < 8; i++) {
		s = original;
		s.scratch[i] = 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	}
	s = original;
	s.rvbar++;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	s = original;
	s.ce_state = 1;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
	s = original;
	s.ce_error = BIT_ULL(63);
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
}

static void ane_cold_dart_test(struct kunit *test)
{
	struct ane_fw_cold_state s, original = ane_cold_fixture();

	for (unsigned int i = 0; i < ANE_FW_DARTS; i++) {
		s = original;
		s.dart[i].version++;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
		s = original;
		s.dart[i].protect |= BIT(0);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
		s = original;
		s.dart[i].lock |= BIT(2);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
		s = original;
		s.dart[i].error = 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
		/* The bound DART driver enables all streams; that is not a refusal. */
		for (unsigned int j = 0; j < 16; j++) {
			s = original;
			s.dart[i].enabled[j] = U32_MAX;
			KUNIT_EXPECT_TRUE(test, ane_fw_cold_valid(&s, &ane_cold_t8140, true));
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
	/* A backend that cycles the engine lets suspend through, not hibernation. */
	KUNIT_EXPECT_FALSE(test, ane_fw_sleep_refused_cycle(PM_SUSPEND_PREPARE, true, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_sleep_refused_cycle(PM_HIBERNATION_PREPARE, true, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_sleep_refused_cycle(PM_RESTORE_PREPARE, true, true));
	KUNIT_EXPECT_TRUE(test, ane_fw_sleep_refused_cycle(PM_SUSPEND_PREPARE, true, false));
	KUNIT_EXPECT_FALSE(test, ane_fw_sleep_refused_cycle(PM_HIBERNATION_PREPARE, false, true));
	/* The device callback backstop: refuse with running firmware only. */
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_backstop(true), -EBUSY);
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_backstop(false), 0);
}

/*
 * The cold cycle after our own power-down: the C4 state (ANE_SYS kept on,
 * CPU stopped: status 0x62, CE 0x5, DAPF kept) restarts like reset values;
 * a DART fault or lock does not.
 */
static void ane_cold_restartable_test(struct kunit *test)
{
	struct ane_fw_cold_state s, original = ane_cold_fixture();

	KUNIT_EXPECT_TRUE(test, ane_fw_cold_engine_restartable(&original, &ane_cold_t8140));
	s = original;
	s.status = 0x62;
	s.ce_state = 5;
	s.scratch[6] = 9;
	KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_valid(&s, &ane_cold_t8140));
	KUNIT_EXPECT_TRUE(test, ane_fw_cold_engine_restartable(&s, &ane_cold_t8140));
	for (unsigned int i = 0; i < ANE_FW_DARTS; i++) {
		s = original;
		s.dart[i].error = 1;
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_restartable(&s, &ane_cold_t8140));
		s = original;
		s.dart[i].lock |= BIT(2);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_restartable(&s, &ane_cold_t8140));
		s = original;
		s.dart[i].protect |= BIT(0);
		KUNIT_EXPECT_FALSE(test, ane_fw_cold_engine_restartable(&s, &ane_cold_t8140));
	}
}

/* RVBAR: latched at TEXT is ready, unlatched is written, foreign refused. */
static void ane_cold_rvbar_test(struct kunit *test)
{
	const u64 text = 0x10000000000ULL;

	KUNIT_EXPECT_EQ(test, ane_fw_rvbar_check(text | 1, text), ANE_FW_RVBAR_LATCHED);
	KUNIT_EXPECT_EQ(test, ane_fw_rvbar_check(0, text), ANE_FW_RVBAR_WRITABLE);
	KUNIT_EXPECT_EQ(test, ane_fw_rvbar_check(text, text), ANE_FW_RVBAR_WRITABLE);
	KUNIT_EXPECT_EQ(test, ane_fw_rvbar_check(1, text), ANE_FW_RVBAR_FOREIGN);
	KUNIT_EXPECT_EQ(test, ane_fw_rvbar_check(0x20000000001ULL, text), ANE_FW_RVBAR_FOREIGN);
}

/* The boot/transport configs intentionally leave the floor driver disabled. */
static void ane_cold_no_clpc_test(struct kunit *test)
{
#if !IS_REACHABLE(CONFIG_APPLE_CLPC)
	struct apple_clpc_ane *ane = NULL;
	struct device dev = {};

	KUNIT_EXPECT_EQ(test, apple_clpc_ane_get(&dev, &ane), 0);
	KUNIT_EXPECT_PTR_EQ(test, ane, NULL);
	apple_clpc_ane_set_active(ane, true);
	apple_clpc_ane_set_active(ane, false);
	apple_clpc_ane_put(ane);
#else
	kunit_skip(test, "CLPC enabled; absence is covered by the CLPC suite");
#endif
}

/*
 * Recovery of a hung engine reuses its memory only with the whole proof: our
 * power-down began, ANE_SYS then went off as PMGR reads it, nothing is held
 * again, and the ANE CPU and sub-block read off now.
 */
static void ane_cold_hung_gone_test(struct kunit *test)
{
	const struct ane_fw_hung_proof proven = { .down = true, .sys_off = true };
	struct ane_fw_cold_state off = ane_cold_fixture(), s;
	struct ane_fw_hung_proof p;

	KUNIT_EXPECT_TRUE(test, ane_fw_sys_off(0x200));
	KUNIT_EXPECT_TRUE(test, ane_fw_sys_off(0x20f));
	KUNIT_EXPECT_FALSE(test, ane_fw_sys_off(0x23ff));
	KUNIT_EXPECT_FALSE(test, ane_fw_sys_off(0x10));

	KUNIT_EXPECT_TRUE(test, ane_fw_hung_gone(&proven, &off));
	/* No power-down of ours, so an OFF proves nothing about the hung firmware. */
	p = proven;
	p.down = false;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&p, &off));
	/* ANE_SYS never reported off (runtime DARTs held it, or no genpd notifier). */
	p = proven;
	p.sys_off = false;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&p, &off));
	/* Powered again, or a reference still held. */
	p = proven;
	p.held = true;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&p, &off));
	/* The ANE CPU power state targets or reads on. */
	s = off;
	s.cpu |= 0xf;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&proven, &s));
	s = off;
	s.cpu |= 0xf0;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&proven, &s));
	/* A sub-block word left on, or BUSY latched as a stuck power-down leaves it. */
	s = off;
	s.subblock = 0x3ff;
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&proven, &s));
	s = off;
	s.subblock = 0x300 | BIT(11);
	KUNIT_EXPECT_FALSE(test, ane_fw_hung_gone(&proven, &s));
}

#define REC_S(x) ((u64)(x) * NSEC_PER_SEC)
#define REC_MS(x) ((u64)(x) * NSEC_PER_MSEC)

/* Ask at @now and, when admitted, start the attempt then (as the recover work does). */
static enum ane_fw_recover_verdict rec_try(struct ane_fw_recover_limit *l,
					   const struct ane_fw_recover_policy *p,
					   u64 now, u64 *wait)
{
	enum ane_fw_recover_verdict v = ane_fw_recover_admit(l, p, now, wait);

	if (v == ANE_FW_RECOVER_NOW)
		ane_fw_recover_note(l, now);
	return v;
}

/*
 * A storm of hangs (0.6 s after each recovery): with the policy (5 in 60 s,
 * 1 s backoff) the recoveries spread out to 0, 1, 3, 7 and 15 s, and the
 * sixth hang stops them until a system sleep resets the limit.
 */
static void ane_cold_recover_storm_test(struct kunit *test)
{
	const struct ane_fw_recover_policy p = {
		.burst = 5, .window_ns = REC_S(60), .backoff_ns = REC_S(1),
	};
	static const u64 expect[] = { 0, 1, 3, 7, 15 };
	struct ane_fw_recover_limit l = {};
	u64 now = REC_S(100), wait, started[ARRAY_SIZE(expect)];
	unsigned int done = 0, refused = 0;

	/* Hang 0.6 s after each recovery; a refused one is asked again when told. */
	while (done < ARRAY_SIZE(expect)) {
		enum ane_fw_recover_verdict v = rec_try(&l, &p, now, &wait);

		KUNIT_ASSERT_NE(test, v, ANE_FW_RECOVER_STOP);
		if (v == ANE_FW_RECOVER_LATER) {
			KUNIT_ASSERT_GT(test, wait, 0ULL);
			refused++;
			now += wait;
			continue;
		}
		started[done++] = now;
		now += REC_MS(600);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(expect); i++)
		KUNIT_EXPECT_EQ(test, started[i] - started[0], REC_S(expect[i]));
	KUNIT_EXPECT_EQ(test, refused, 4U);
	/* The sixth hang within the minute trips the limit, and it stays tripped. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_STOP);
	KUNIT_EXPECT_TRUE(test, l.tripped);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + REC_S(3600), &wait), ANE_FW_RECOVER_STOP);
	KUNIT_EXPECT_EQ(test, wait, 0ULL);
	/* A system sleep clears it: the next one runs at once. */
	ane_fw_recover_reset(&l);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + REC_S(3601), &wait), ANE_FW_RECOVER_NOW);
}

/* Backoff doubling, its cap, the sliding window and the burst bounds. */
static void ane_cold_recover_backoff_test(struct kunit *test)
{
	struct ane_fw_recover_policy p = {
		.burst = 16, .window_ns = REC_S(10), .backoff_ns = REC_S(1),
	};
	struct ane_fw_recover_limit l = {};
	u64 now = REC_S(1), wait;

	/* First in the window: at once. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_EQ(test, wait, 0ULL);
	/* Second: 1 s after the first; asked 0.25 s after it, wait 0.75 s. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + REC_MS(250), &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_MS(750));
	now += REC_S(1);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	/* Third: 2 s after the second; fourth: 4 s; fifth: capped at the window. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_S(2));
	now += REC_S(2);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_S(4));
	now += REC_S(4);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	/* Started at 0, 1, 3 and 7 s; at 7 s all four are in the 10 s window. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_S(8));
	/* At 10 s the first has left the window: three recent, 4 s after the latest. */
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + REC_S(3), &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_S(1));
	/* At 11 s the first two have: two recent, 2 s after the latest, due. */
	now += REC_S(4);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_FALSE(test, l.tripped);
	/* Quiet for a whole window: no backoff at all. */
	now += REC_S(10);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);

	/* Delays never exceed the window, whatever the backoff. */
	ane_fw_recover_reset(&l);
	p.backoff_ns = REC_S(60);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_LATER);
	KUNIT_EXPECT_EQ(test, wait, REC_S(10));

	/* No backoff: only the burst limits. A burst of 0 counts as 1. */
	ane_fw_recover_reset(&l);
	p.backoff_ns = 0;
	p.burst = 0;
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + 1, &wait), ANE_FW_RECOVER_STOP);

	/* A burst above the ring is the ring: 16 at once, the 17th stops. */
	ane_fw_recover_reset(&l);
	p.burst = 100;
	for (unsigned int i = 0; i < ANE_FW_RECOVER_BURST_MAX; i++)
		KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + i, &wait), ANE_FW_RECOVER_NOW);
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now + 16, &wait), ANE_FW_RECOVER_STOP);

	/* A clock that went back (a stamp after now) is not counted as recent. */
	ane_fw_recover_reset(&l);
	p.burst = 2;
	p.backoff_ns = REC_S(1);
	ane_fw_recover_note(&l, now + REC_S(5));
	KUNIT_EXPECT_EQ(test, rec_try(&l, &p, now, &wait), ANE_FW_RECOVER_NOW);
}

/* A recovery keeps ANE_SYS off for its minimum, counted from genpd's OFF. */
static void ane_cold_recover_off_test(struct kunit *test)
{
	const u64 min = 100 * NSEC_PER_MSEC, off = 5 * NSEC_PER_SEC;

	/* The storm's recoveries powered up again within about a millisecond. */
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off + NSEC_PER_MSEC, min),
			99 * NSEC_PER_MSEC);
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off, min), min);
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off + min, min), 0ULL);
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off + NSEC_PER_SEC, min), 0ULL);
	/* No OFF recorded, or one in the future: wait the whole minimum. */
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(0, off, min), min);
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off - 1, min), min);
	/* No minimum: never wait. */
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(off, off, 0), 0ULL);
	KUNIT_EXPECT_EQ(test, ane_fw_recover_off_wait(0, off, 0), 0ULL);
}

/* A recovered engine takes requests only with everything reading healthy. */
static void ane_cold_recover_settled_test(struct kunit *test)
{
	const struct ane_fw_settled up = { .sys = 0x23ff, .cpu = 0x0f0003ff };
	struct ane_fw_settled s;

	KUNIT_EXPECT_TRUE(test, ane_fw_recover_settled(&up));
	/* ANE_SYS or the CPU not (yet) on, in target or actual state. */
	s = up;
	s.sys = 0x230f;
	KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
	s = up;
	s.sys = 0x23f0;
	KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
	s = up;
	s.cpu = 0x0f0003f0;
	KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
	s = up;
	s.cpu = 0x0f000300;
	KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
	/* A DART fault on any of the three. */
	for (unsigned int i = 0; i < ANE_FW_DARTS; i++) {
		s = up;
		s.dart_error[i] = 0x80000001;
		KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
	}
	/* The transport failed while serving the new firmware's posts. */
	s = up;
	s.transport_error = -ENOSPC;
	KUNIT_EXPECT_FALSE(test, ane_fw_recover_settled(&s));
}

static struct kunit_case ane_cold_cases[] = {
	KUNIT_CASE(ane_cold_recover_settled_test),
	KUNIT_CASE(ane_cold_recover_off_test),
	KUNIT_CASE(ane_cold_recover_storm_test),
	KUNIT_CASE(ane_cold_recover_backoff_test),
	KUNIT_CASE(ane_cold_hung_gone_test),
	KUNIT_CASE(ane_cold_no_clpc_test),
	KUNIT_CASE(ane_cold_restartable_test),
	KUNIT_CASE(ane_cold_rvbar_test),
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
