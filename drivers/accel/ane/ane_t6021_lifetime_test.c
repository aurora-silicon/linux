// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_exec_timing.h"
#include "ane_t6021_lifetime.h"

static void mring_classify_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0, 0, 0),
			ANE_T6021_MRING_IDLE);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0, SZ_1M, 0x45585050),
			ANE_T6021_MRING_MALLOC);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0, SZ_16K, U32_MAX),
			ANE_T6021_MRING_MALLOC);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0, SZ_16K, 1ULL << 32),
			ANE_T6021_MRING_BAD);
	/* A FREE gives back an IOVA in the header with an empty body. */
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0xfbff8000, 0, 0),
			ANE_T6021_MRING_FREE);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0xfbff8000, SZ_16K, 0),
			ANE_T6021_MRING_BAD);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0xfbff8000, 0, 1),
			ANE_T6021_MRING_BAD);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0xfbff8002, 0, 0),
			ANE_T6021_MRING_BAD);
	/* Answered slots carry bit 0 whatever the body holds. */
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(1, 0, 0),
			ANE_T6021_MRING_ANSWERED);
	KUNIT_EXPECT_EQ(test, ane_t6021_mring_classify(0xfbff8001, 0, 7),
			ANE_T6021_MRING_ANSWERED);
}

static void legtab_reuse_test(struct kunit *test)
{
	struct ane_legacy_buffer t[4] = {};
	u8 mem[4];
	u32 used = 0;
	int i, slot;

	for (i = 0; i < 4; i++) {
		slot = ane_t6021_legtab_pick(t, used, 4);
		KUNIT_ASSERT_EQ(test, slot, i);
		t[slot] = (struct ane_legacy_buffer) {
			.cpu = &mem[i], .dma = 0x100000 + i * SZ_16K, .size = SZ_16K,
		};
		used++;
	}
	KUNIT_EXPECT_EQ(test, ane_t6021_legtab_pick(t, used, 4), -1);
	/* A FREE of slot 2 makes it the next slot taken; the table does not grow. */
	slot = ane_t6021_legtab_find(t, used, 0x100000 + 2 * SZ_16K, NULL);
	KUNIT_ASSERT_EQ(test, slot, 2);
	memset(&t[slot], 0, sizeof(t[slot]));
	KUNIT_EXPECT_EQ(test, ane_t6021_legtab_find(t, used, 0x100000 + 2 * SZ_16K,
						    NULL), -1);
	KUNIT_EXPECT_EQ(test, ane_t6021_legtab_pick(t, used, 4), 2);
	/* The driver's own command buffer never matches a FREE. */
	KUNIT_EXPECT_EQ(test, ane_t6021_legtab_find(t, used, 0x100000, &t[0]), -1);
	KUNIT_EXPECT_EQ(test, ane_t6021_legtab_find(t, used, 0x100000, NULL), 0);
	/* A freed slot has size 0, so no address lookup can land in it. */
	KUNIT_EXPECT_EQ(test, t[2].size, (size_t)0);
}

static void put32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static void put_task(u8 *td, size_t at, u32 words, u32 w2)
{
	td[at + 2] = words;
	td[at + 3] = words >> 8;
	put32(td + at + 8, w2);
}

static void td_flags_test(struct kunit *test)
{
	u8 td[0x200] = {};

	/* One task: opens and closes the program. */
	put_task(td, 0x10, 0x20, 0x2a);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x90), 0);
	/* Two and three tasks: 0x08 first, 0 between, 0x22 last. */
	put_task(td, 0x10, 0x20, 0x08);
	put_task(td, 0x90, 0x20, 0x22);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x110), 0);
	put_task(td, 0x90, 0x20, 0);
	put_task(td, 0x110, 0x20, 0x22);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x190), 0);
	/* The wedging form: every task opens and closes. */
	put_task(td, 0x10, 0x20, 0x2a);
	put_task(td, 0x90, 0x20, 0x2a);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x110), -EINVAL);
	/* Not closed, closed early, opened twice. */
	put_task(td, 0x10, 0x20, 0x08);
	put_task(td, 0x90, 0x20, 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x110), -EINVAL);
	put_task(td, 0x90, 0x20, 0x22);
	put_task(td, 0x110, 0x20, 0x22);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x190), -EINVAL);
	put_task(td, 0x90, 0x20, 0x08);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x190), -EINVAL);
	/* A lone task without both bits. */
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x90), -EINVAL);
	/* A stream that does not parse is left to the firmware. */
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 0x14), 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_td_flags_check(td, 2), 0);
}

/* A fake firmware. Records every command class and fails on demand. */
struct fake_fw {
	struct kunit *test;
	struct ane_t6021_progtab *t;
	unsigned int unloads, releases, loads, creates, restarts;
	unsigned int terminates;
	int fail_unload, fail_restart;
	bool restart_in_unload;
	int sec_refs;
	u32 next_fw;
};

static const struct ane_t6021_progtab_ops fake_ops;

static int fake_unload(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;

	fw->unloads++;
	if (p->fw_proc_id != ANE_T6021_NO_ID)
		fw->terminates++;
	if (fw->restart_in_unload) {
		ane_t6021_progtab_restarted(fw->t, &fake_ops, fw);
		return -ETIMEDOUT;
	}
	return fw->fail_unload;
}

static void fake_release(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;
	unsigned int i;

	fw->releases++;
	for (i = 0; i < ANE_T6021_PROG_SECS; i++)
		if (p->sec[i])
			fw->sec_refs--;
}

static int fake_load(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;

	fw->loads++;
	p->fw_prog_id = fw->next_fw++;
	return 0;
}

static int fake_create(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;

	fw->creates++;
	p->fw_proc_id = fw->next_fw++;
	return 0;
}

static int fake_restart(void *ctx)
{
	struct fake_fw *fw = ctx;

	fw->restarts++;
	if (fw->fail_restart)
		return fw->fail_restart;
	ane_t6021_progtab_restarted(fw->t, &fake_ops, fw);
	return 0;
}

static const struct ane_t6021_progtab_ops fake_ops = {
	.unload = fake_unload,
	.release = fake_release,
	.load = fake_load,
	.create = fake_create,
	.restart = fake_restart,
};

static struct ane_t6021_progtab *new_table(struct kunit *test,
					   struct fake_fw *fw)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	fw->test = test;
	fw->t = t;
	fw->next_fw = 100;
	return t;
}

static struct ane_t6021_fdprogs *new_fd(struct kunit *test)
{
	struct ane_t6021_fdprogs *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	return f;
}

/* What the driver's LOAD ioctl does for a new program. */
static struct ane_t6021_prog *load(struct ane_t6021_progtab *t,
				   struct fake_fw *fw, u32 tag, u32 id)
{
	u8 digest[32] = {};
	struct ane_t6021_prog *p;

	digest[0] = tag;
	digest[1] = tag >> 8;
	if (ane_t6021_progtab_make_room(t, NULL, &fake_ops, fw))
		return NULL;
	p = ane_t6021_progtab_reserve(t, digest);
	if (!p)
		return NULL;
	p->sec[0] = fw;
	p->sec[6] = fw;
	fw->sec_refs += 2;
	fake_load(fw, p);
	ane_t6021_progtab_commit(t, p, id, p->fw_prog_id);
	return p;
}

/* LOAD, PROC_CREATE and one EXEC from a fresh file. */
static struct ane_t6021_prog *open_run(struct kunit *test,
				       struct ane_t6021_progtab *t,
				       struct fake_fw *fw,
				       struct ane_t6021_fdprogs *f, u32 tag)
{
	u8 digest[32] = {};
	struct ane_t6021_prog *p;

	digest[0] = tag;
	digest[1] = tag >> 8;
	p = ane_t6021_progtab_find(t, digest);
	if (!p)
		p = load(t, fw, tag, 1000 + tag);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, f, p), 0);
	KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p, true, &fake_ops, fw), 0);
	KUNIT_EXPECT_TRUE(test, p->resident);
	KUNIT_EXPECT_NE(test, p->fw_proc_id, ANE_T6021_NO_ID);
	KUNIT_EXPECT_LE(test, ane_t6021_progtab_resident(t),
			(unsigned int)ANE_T6021_FW_RESIDENT_MAX);
	return p;
}

static void progtab_shared_unload_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test), *b = new_fd(test);
	struct ane_t6021_prog *p, *q;

	p = open_run(test, t, &fw, a, 1);
	/* A second load of identical sections finds the same program and
	 * process: no command reaches the firmware.
	 */
	q = open_run(test, t, &fw, b, 1);
	KUNIT_ASSERT_PTR_EQ(test, q, p);
	/* Taking the same reference twice from one file counts once. */
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, b, q), 0);
	KUNIT_EXPECT_EQ(test, p->users, 2U);
	KUNIT_EXPECT_EQ(test, fw.loads, 1U);
	KUNIT_EXPECT_EQ(test, fw.creates, 1U);

	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 0U);
	KUNIT_EXPECT_EQ(test, p->users, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 2);

	/* The program is unloaded at its last close. */
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, b, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.terminates, 1U);
	KUNIT_EXPECT_EQ(test, fw.releases, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_by_id(t, 1001));
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 0U);
	/* The slot is reused by the next program. */
	q = load(t, &fw, 2, 1002);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	KUNIT_EXPECT_EQ(test, t->hi, 1U);
}

static void multitask_unloaded_at_close_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test);
	struct ane_t6021_prog *p;
	struct ane_t6021_progtab_stats st;

	/* Any program with a process, whatever its task count, is terminated
	 * and destroyed at its last close; nothing stays cached.
	 */
	p = open_run(test, t, &fw, a, 3);
	KUNIT_EXPECT_TRUE(test, ane_t6021_prog_unloadable(p));
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.terminates, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	ane_t6021_progtab_count(t, &st);
	KUNIT_EXPECT_EQ(test, st.cached, 0U);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 0U);

	/* A program that never got a process is only destroyed. */
	p = load(t, &fw, 4, 1004);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 2U);
	KUNIT_EXPECT_EQ(test, fw.terminates, 1U);
}

static void progtab_failed_unload_holds_test(struct kunit *test)
{
	struct fake_fw fw = { .fail_unload = -ETIMEDOUT };
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test);
	struct ane_t6021_progtab_stats st;
	struct ane_t6021_prog *p, *q;

	p = open_run(test, t, &fw, a, 5);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 1U);
	/* The firmware may still read the sections: nothing is released. */
	KUNIT_EXPECT_EQ(test, fw.releases, 0U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 2);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_HELD);
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_by_id(t, 1005));
	ane_t6021_progtab_count(t, &st);
	KUNIT_EXPECT_EQ(test, st.live, 0U);
	KUNIT_EXPECT_EQ(test, st.held, 1U);
	/* A HELD program still counts against the firmware budget. */
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 1U);
	/* A held slot is never reused before a restart. */
	q = load(t, &fw, 6, 1006);
	KUNIT_EXPECT_PTR_NE(test, q, p);
	KUNIT_EXPECT_EQ(test, t->hi, 2U);
	/* A verified restart frees everything no file holds. */
	ane_t6021_progtab_restarted(t, &fake_ops, &fw);
	KUNIT_EXPECT_EQ(test, fw.releases, 2U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_EQ(test, t->hi, 0U);
	KUNIT_EXPECT_EQ(test, t->gen, 1U);
}

static void progtab_load_failure_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test);
	struct ane_t6021_prog *p;
	u8 digest[32];

	memset(digest, 5, sizeof(digest));
	p = ane_t6021_progtab_reserve(t, digest);
	KUNIT_ASSERT_NOT_NULL(test, p);
	/* An uncommitted program is invisible and cannot be referenced. */
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_find(t, digest));
	KUNIT_EXPECT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), -ESTALE);
	KUNIT_EXPECT_EQ(test, a->n, 0U);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_HELD);
	/* A restart frees the reservation, it never becomes LIVE. */
	ane_t6021_progtab_restarted(t, &fake_ops, &fw);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_FREE);
}

static void progtab_restart_keeps_ids_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test), *b = new_fd(test);
	struct ane_t6021_prog *p, *z;
	u32 fw_prog, fw_proc;

	p = open_run(test, t, &fw, a, 7);
	/* A second program, closed again: it is unloaded at once. */
	z = open_run(test, t, &fw, b, 8);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, b, &fake_ops, &fw), 0U);
	fw_prog = p->fw_prog_id;
	fw_proc = p->fw_proc_id;

	KUNIT_ASSERT_EQ(test, fake_restart(&fw), 0);
	/* The held program keeps its kernel ID and sections, not its
	 * firmware IDs.
	 */
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_by_id(t, 1007), p);
	KUNIT_EXPECT_FALSE(test, p->resident);
	KUNIT_EXPECT_EQ(test, p->fw_prog_id, ANE_T6021_NO_ID);
	KUNIT_EXPECT_EQ(test, p->fw_proc_id, ANE_T6021_NO_ID);
	KUNIT_EXPECT_EQ(test, z->state, ANE_T6021_PROG_FREE);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 2);
	KUNIT_EXPECT_TRUE(test, ane_t6021_fdprogs_has(a, 1007));
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 0U);

	/* The next use loads it and creates its process again. */
	KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p, true, &fake_ops, &fw), 0);
	KUNIT_EXPECT_TRUE(test, p->resident);
	KUNIT_EXPECT_NE(test, p->fw_prog_id, fw_prog);
	KUNIT_EXPECT_NE(test, p->fw_proc_id, fw_proc);
	KUNIT_EXPECT_EQ(test, fw.loads, 3U);
	KUNIT_EXPECT_EQ(test, fw.creates, 3U);
	/* A program without a file reference is not loaded again. */
	z->state = ANE_T6021_PROG_LIVE;
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_ensure(t, z, true, &fake_ops, &fw),
			-ESTALE);
	z->state = ANE_T6021_PROG_FREE;
}

static void budget_eviction_order_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *f[ANE_T6021_FW_RESIDENT_MAX + 1];
	struct ane_t6021_prog *p[ANE_T6021_FW_RESIDENT_MAX + 1];
	unsigned int i;

	for (i = 0; i <= ANE_T6021_FW_RESIDENT_MAX; i++)
		f[i] = new_fd(test);
	/* Eight programs, each held open by its own file. */
	for (i = 0; i < ANE_T6021_FW_RESIDENT_MAX; i++)
		p[i] = open_run(test, t, &fw, f[i], 10 + i);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t),
			(unsigned int)ANE_T6021_FW_RESIDENT_MAX);
	/* Using 0 again makes 1 the least recently used. */
	KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p[0], true, &fake_ops, &fw), 0);
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_victim(t, NULL), p[1]);
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_victim(t, p[1]), p[2]);
	/* A ninth program unloads 1 for room; no restart. */
	p[8] = open_run(test, t, &fw, f[8], 30);
	KUNIT_EXPECT_EQ(test, fw.restarts, 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.terminates, 1U);
	KUNIT_EXPECT_FALSE(test, p[1]->resident);
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_by_id(t, 1011), p[1]);
	/* Its file uses it again: 2 goes, 1 loads with a new process. */
	KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p[1], true, &fake_ops, &fw), 0);
	KUNIT_EXPECT_FALSE(test, p[2]->resident);
	KUNIT_EXPECT_TRUE(test, p[1]->resident);
	KUNIT_EXPECT_EQ(test, fw.restarts, 0U);
	/* HELD programs cannot be given back; when all residents are HELD the
	 * firmware is restarted, and the held programs files still hold come
	 * back as non-resident LIVE programs.
	 */
	for (i = 0; i <= ANE_T6021_FW_RESIDENT_MAX; i++)
		if (p[i]->resident)
			p[i]->state = ANE_T6021_PROG_HELD;
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_victim(t, NULL));
	KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p[2], true, &fake_ops, &fw), 0);
	KUNIT_EXPECT_EQ(test, fw.restarts, 1U);
	KUNIT_EXPECT_EQ(test, t->restarts, 1U);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 1U);
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_by_id(t, 1010), p[0]);
	KUNIT_EXPECT_FALSE(test, p[0]->resident);
	/* A failed restart reaches the caller and changes nothing. */
	for (i = 0; i <= ANE_T6021_FW_RESIDENT_MAX; i++)
		KUNIT_ASSERT_EQ(test, ane_t6021_progtab_ensure(t, p[i], true,
							       &fake_ops, &fw), 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t),
			(unsigned int)ANE_T6021_FW_RESIDENT_MAX);
	/* Nine programs, eight resident: the one left out is p[0]. */
	KUNIT_EXPECT_FALSE(test, p[0]->resident);
	for (i = 1; i <= ANE_T6021_FW_RESIDENT_MAX; i++)
		p[i]->state = ANE_T6021_PROG_HELD;
	fw.fail_restart = -EBUSY;
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_ensure(t, p[0], true, &fake_ops, &fw),
			-EBUSY);
	KUNIT_EXPECT_FALSE(test, p[0]->resident);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t),
			(unsigned int)ANE_T6021_FW_RESIDENT_MAX);
}

/* Thirty distinct programs, each opened, run and closed, while one more is
 * held open throughout: everything unloads at close, the budget holds at
 * every step and no restart is ever needed.
 */
static void budget_sequence_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *f = new_fd(test), *keep = new_fd(test);
	unsigned int i;

	open_run(test, t, &fw, keep, 500);
	for (i = 0; i < 30; i++) {
		struct ane_t6021_prog *p = open_run(test, t, &fw, f, 600 + i);

		KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, f, &fake_ops, &fw),
				0U);
		KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_FREE);
		KUNIT_EXPECT_LE(test, ane_t6021_progtab_resident(t), 2U);
	}
	KUNIT_EXPECT_EQ(test, fw.restarts, 0U);
	KUNIT_EXPECT_EQ(test, fw.terminates, 30U);
	KUNIT_ASSERT_NOT_NULL(test, ane_t6021_progtab_by_id(t, 1500));
	KUNIT_EXPECT_TRUE(test, ane_t6021_progtab_by_id(t, 1500)->resident);
	ane_t6021_progtab_fd_close(t, keep, &fake_ops, &fw);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_resident(t), 0U);
}

static void progtab_capacity_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test), *b = new_fd(test);
	struct ane_t6021_prog *p;
	u8 digest[32];
	unsigned int i;

	/* Known programs are bounded by the firmware table, not the budget. */
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++) {
		p = load(t, &fw, i, 1000 + i);
		KUNIT_ASSERT_NOT_NULL(test, p);
		KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
		KUNIT_ASSERT_LE(test, ane_t6021_progtab_resident(t),
				(unsigned int)ANE_T6021_FW_RESIDENT_MAX);
	}
	memset(digest, 0xff, sizeof(digest));
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_reserve(t, digest));
	KUNIT_EXPECT_FALSE(test, ane_t6021_fdprogs_room(t, a));
	KUNIT_EXPECT_EQ(test, fw.restarts, 0U);
	/* Closing the file unloads the resident ones and frees every slot. */
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.releases, (unsigned int)ANE_T6021_MAX_PROGRAMS);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	p = ane_t6021_progtab_reserve(t, digest);
	KUNIT_EXPECT_PTR_EQ(test, p, &t->progs[0]);

	/* A full file list drops stale IDs before it refuses a reference. */
	b->n = ANE_T6021_MAX_PROGRAMS;
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++)
		b->ids[i] = 5000 + i;
	ane_t6021_progtab_commit(t, p, 9000, 1);
	KUNIT_EXPECT_EQ(test, ane_t6021_fdprogs_ref(t, b, p), 0);
	KUNIT_EXPECT_EQ(test, b->n, 1U);
	KUNIT_EXPECT_EQ(test, p->users, 1U);
}

static void progtab_restart_during_unload_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_fdprogs *a = new_fd(test);
	struct ane_t6021_prog *p, *q;

	p = open_run(test, t, &fw, a, 50);
	q = open_run(test, t, &fw, a, 51);
	/* The first unload fails and its recovery restarts the firmware.
	 * Nothing may be marked HELD afterwards, and the second program,
	 * no longer resident, needs no unload.
	 */
	fw.restart_in_unload = true;
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_NE(test, p->state, ANE_T6021_PROG_HELD);
	KUNIT_EXPECT_NE(test, q->state, ANE_T6021_PROG_HELD);
	KUNIT_EXPECT_EQ(test, t->gen, 1U);
}

static void progtab_cancel_test(struct kunit *test)
{
	struct fake_fw fw = {};
	struct ane_t6021_progtab *t = new_table(test, &fw);
	struct ane_t6021_prog *p;
	u8 digest[32];

	memset(digest, 10, sizeof(digest));
	p = ane_t6021_progtab_reserve(t, digest);
	KUNIT_ASSERT_NOT_NULL(test, p);
	p->sec[2] = &fw;
	fw.sec_refs = 1;
	/* A LOAD that was never sent gives its slot and references back. */
	ane_t6021_progtab_cancel(p, &fake_ops, &fw);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_FREE);
	KUNIT_EXPECT_PTR_EQ(test, ane_t6021_progtab_reserve(t, digest), p);
}

struct fake_cmds {
	u16 ops[4];
	u32 progs[4];
	u32 procs[4];
	unsigned int n;
	int fail_at;
};

static int fake_cmd(void *ctx, u16 opcode, u32 fw_prog, u32 fw_proc)
{
	struct fake_cmds *c = ctx;

	c->ops[c->n] = opcode;
	c->progs[c->n] = fw_prog;
	c->procs[c->n] = fw_proc;
	return ++c->n == c->fail_at ? -ETIMEDOUT : 0;
}

static void unload_order_test(struct kunit *test)
{
	struct ane_t6021_prog p = {
		.fw_prog_id = 4, .fw_proc_id = 9,
	};
	struct fake_cmds c = {};

	/* The process goes first: destroying a program with a process halts
	 * the firmware.
	 */
	KUNIT_EXPECT_EQ(test, ane_t6021_prog_unload_cmds(&p, fake_cmd, &c), 0);
	KUNIT_ASSERT_EQ(test, c.n, 2U);
	KUNIT_EXPECT_EQ(test, c.ops[0], ANE_T6021_CMD_TERMINATE_PROCESS);
	KUNIT_EXPECT_EQ(test, c.progs[0], 4U);
	KUNIT_EXPECT_EQ(test, c.procs[0], 9U);
	KUNIT_EXPECT_EQ(test, c.ops[1], ANE_T6021_CMD_DESTROY_PROGRAM);
	KUNIT_EXPECT_EQ(test, c.progs[1], 4U);

	/* No process: only the program is destroyed. */
	memset(&c, 0, sizeof(c));
	p.fw_proc_id = ANE_T6021_NO_ID;
	KUNIT_EXPECT_EQ(test, ane_t6021_prog_unload_cmds(&p, fake_cmd, &c), 0);
	KUNIT_ASSERT_EQ(test, c.n, 1U);
	KUNIT_EXPECT_EQ(test, c.ops[0], ANE_T6021_CMD_DESTROY_PROGRAM);

	/* A failed terminate stops the sequence. */
	memset(&c, 0, sizeof(c));
	c.fail_at = 1;
	p.fw_proc_id = 9;
	KUNIT_EXPECT_EQ(test, ane_t6021_prog_unload_cmds(&p, fake_cmd, &c),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, c.n, 1U);
}

/* The restart steps, recorded in order; a step fails when asked to. */
struct fake_restart {
	char log[8];
	unsigned int n;
	char fail;
};

static int fake_step(struct fake_restart *r, char step)
{
	r->log[r->n++] = step;
	return r->fail == step ? -ETIMEDOUT : 0;
}

static int fr_power_down(void *ctx)
{
	return fake_step(ctx, 'p');
}

static int fr_wait_idle(void *ctx)
{
	return fake_step(ctx, 'w');
}

static int fr_halt(void *ctx)
{
	return fake_step(ctx, 'h');
}

static int fr_power_cycle(void *ctx)
{
	return fake_step(ctx, 'c');
}

static int fr_boot(void *ctx)
{
	return fake_step(ctx, 'b');
}

static const struct ane_t6021_restart_ops fr_ops = {
	.power_down = fr_power_down,
	.wait_idle = fr_wait_idle,
	.halt = fr_halt,
	.power_cycle = fr_power_cycle,
	.boot = fr_boot,
};

static void restart_stop_boundary_test(struct kunit *test)
{
	struct fake_restart r = {};
	static const char failures[] = "pwh";
	unsigned int i;

	/* System suspend stops here; its PM owner performs the power transition. */
	KUNIT_EXPECT_EQ(test, ane_t6021_restart_stop(&fr_ops, &r, true), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "pwh");
	memset(&r, 0, sizeof(r));
	KUNIT_EXPECT_EQ(test, ane_t6021_restart_stop(&fr_ops, &r, false), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "h");
	for (i = 0; i < sizeof(failures) - 1; i++) {
		memset(&r, 0, sizeof(r));
		r.fail = failures[i];
		KUNIT_EXPECT_EQ(test, ane_t6021_restart_stop(&fr_ops, &r, true),
				-ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, r.n, i + 1);
		KUNIT_EXPECT_NULL(test, strchr(r.log, 'c'));
		KUNIT_EXPECT_NULL(test, strchr(r.log, 'b'));
	}
}

static void restart_state_machine_test(struct kunit *test)
{
	enum ane_t6021_restart_stage stage;
	struct fake_restart r = {};

	/* Planned: shut the firmware down, wait for WFI, stop, then power. */
	KUNIT_EXPECT_EQ(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "pwhcb");
	KUNIT_EXPECT_EQ(test, stage, ANE_T6021_RESTART_DONE);

	/* After a timeout the firmware cannot answer: no POWER_DOWN. */
	memset(&r, 0, sizeof(r));
	KUNIT_EXPECT_EQ(test, ane_t6021_restart_run(&fr_ops, &r, false, &stage), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "hcb");

	/* STOPPED not proven: the domains are never touched. */
	memset(&r, 0, sizeof(r));
	r.fail = 'h';
	KUNIT_EXPECT_NE(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "pwh");
	KUNIT_EXPECT_EQ(test, stage, ANE_T6021_RESTART_STOP);

	/* POWER_DOWN not accepted: not even the RUN clear is tried. */
	memset(&r, 0, sizeof(r));
	r.fail = 'p';
	KUNIT_EXPECT_NE(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "p");
	KUNIT_EXPECT_EQ(test, stage, ANE_T6021_RESTART_STOP);

	/* No WFI after POWER_DOWN: same. */
	memset(&r, 0, sizeof(r));
	r.fail = 'w';
	KUNIT_EXPECT_NE(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_STREQ(test, r.log, "pw");

	/* Later failures name their stage. */
	memset(&r, 0, sizeof(r));
	r.fail = 'c';
	KUNIT_EXPECT_NE(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_EQ(test, stage, ANE_T6021_RESTART_CYCLE);
	memset(&r, 0, sizeof(r));
	r.fail = 'b';
	KUNIT_EXPECT_NE(test, ane_t6021_restart_run(&fr_ops, &r, true, &stage), 0);
	KUNIT_EXPECT_EQ(test, stage, ANE_T6021_RESTART_BOOT);
}

/* The execute files' table: last, sum, minimum and maximum per entry. */
static void exec_timing_fold_test(struct kunit *test)
{
	static const char *const names[] = { "a", "b", "polls" };
	struct ane_xt_sample x = {};
	struct ane_xt_stats *st;
	struct ane_xt *xt;

	xt = kunit_kzalloc(test, sizeof(*xt), GFP_KERNEL);
	st = kunit_kzalloc(test, sizeof(*st), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, xt);
	KUNIT_ASSERT_NOT_NULL(test, st);
	ane_xt_init(xt, names, 2, ARRAY_SIZE(names));
	x.v[0] = 300;
	x.v[1] = 5;
	x.v[2] = 3;
	x.v[3] = 11;
	ane_xt_add(xt, &x);
	x.v[0] = 100;
	x.v[1] = 9;
	x.v[2] = 0;
	ane_xt_add(xt, &x);
	ane_xt_snapshot(xt, st, false);
	KUNIT_EXPECT_EQ(test, st->count, 2ULL);
	KUNIT_EXPECT_EQ(test, st->sum.v[0], 400ULL);
	KUNIT_EXPECT_EQ(test, st->min.v[0], 100ULL);
	KUNIT_EXPECT_EQ(test, st->max.v[0], 300ULL);
	KUNIT_EXPECT_EQ(test, st->last.v[0], 100ULL);
	KUNIT_EXPECT_EQ(test, st->min.v[1], 5ULL);
	KUNIT_EXPECT_EQ(test, st->max.v[1], 9ULL);
	/* A zero count is a real minimum, not an unset one. */
	KUNIT_EXPECT_EQ(test, st->min.v[2], 0ULL);
	KUNIT_EXPECT_EQ(test, st->max.v[2], 3ULL);
	/* Entries past n are never folded. */
	KUNIT_EXPECT_EQ(test, st->sum.v[3], 0ULL);

	/* A reset hands back what it cleared. */
	ane_xt_snapshot(xt, st, true);
	KUNIT_EXPECT_EQ(test, st->count, 2ULL);
	ane_xt_snapshot(xt, st, false);
	KUNIT_EXPECT_EQ(test, st->count, 0ULL);
	x.v[0] = 7;
	ane_xt_add(xt, &x);
	ane_xt_snapshot(xt, st, false);
	KUNIT_EXPECT_EQ(test, st->min.v[0], 7ULL);
	KUNIT_EXPECT_EQ(test, st->max.v[0], 7ULL);

	/* The sizes clamp to the sample. */
	ane_xt_init(xt, names, 99, 99);
	KUNIT_EXPECT_EQ(test, xt->n, (unsigned int)ANE_XT_MAX);
	KUNIT_EXPECT_EQ(test, xt->nns, (unsigned int)ANE_XT_MAX);
	ane_xt_init(xt, names, 3, 2);
	KUNIT_EXPECT_EQ(test, xt->nns, 2U);
}

static struct kunit_case lifetime_cases[] = {
	KUNIT_CASE(mring_classify_test),
	KUNIT_CASE(legtab_reuse_test),
	KUNIT_CASE(td_flags_test),
	KUNIT_CASE(progtab_shared_unload_test),
	KUNIT_CASE(multitask_unloaded_at_close_test),
	KUNIT_CASE(progtab_failed_unload_holds_test),
	KUNIT_CASE(progtab_load_failure_test),
	KUNIT_CASE(progtab_restart_keeps_ids_test),
	KUNIT_CASE(budget_eviction_order_test),
	KUNIT_CASE(budget_sequence_test),
	KUNIT_CASE(progtab_capacity_test),
	KUNIT_CASE(progtab_restart_during_unload_test),
	KUNIT_CASE(progtab_cancel_test),
	KUNIT_CASE(unload_order_test),
	KUNIT_CASE(restart_state_machine_test),
	KUNIT_CASE(restart_stop_boundary_test),
	KUNIT_CASE(exec_timing_fold_test),
	{}
};

static struct kunit_suite lifetime_suite = {
	.name = "ane-t6021-lifetime",
	.test_cases = lifetime_cases,
};

kunit_test_suite(lifetime_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE H14 firmware allocation and program lifetime tests");
