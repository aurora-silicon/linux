// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

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

/* A fake firmware: records every unload and release, can fail on demand. */
struct fake_fw {
	unsigned int unloads, releases;
	int fail_unload;
	int sec_refs;
};

static int fake_unload(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;

	fw->unloads++;
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

static const struct ane_t6021_progtab_ops fake_ops = {
	.unload = fake_unload,
	.release = fake_release,
};

static struct ane_t6021_prog *load(struct ane_t6021_progtab *t,
				   struct fake_fw *fw, u8 tag, u32 id)
{
	u8 digest[32];
	struct ane_t6021_prog *p;

	memset(digest, tag, sizeof(digest));
	p = ane_t6021_progtab_reserve(t, digest);
	if (!p)
		return NULL;
	p->sec[0] = fw;
	p->sec[6] = fw;
	fw->sec_refs += 2;
	ane_t6021_progtab_commit(p, id, id + 100);
	return p;
}

static void progtab_shared_unload_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct ane_t6021_fdprogs *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	struct fake_fw fw = {};
	struct ane_t6021_prog *p, *q;
	u8 digest[32];

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	p = load(t, &fw, 1, 10);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	/* A second load of identical sections finds the same program. */
	memset(digest, 1, sizeof(digest));
	q = ane_t6021_progtab_find(t, digest);
	KUNIT_ASSERT_PTR_EQ(test, q, p);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, b, q), 0);
	/* Taking the same reference twice from one file counts once. */
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, b, q), 0);
	KUNIT_EXPECT_EQ(test, p->users, 2U);
	KUNIT_EXPECT_TRUE(test, ane_t6021_fdprogs_has(b, 10));

	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 0U);
	KUNIT_EXPECT_EQ(test, p->users, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 2);

	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, b, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.releases, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_find(t, digest));
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_by_id(t, 10));
	/* The slot is reused by the next program. */
	q = load(t, &fw, 2, 11);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	KUNIT_EXPECT_EQ(test, t->hi, 1U);
}

static void progtab_failed_unload_holds_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct fake_fw fw = { .fail_unload = -ETIMEDOUT };
	struct ane_t6021_prog *p, *q;
	unsigned int live, held;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	p = load(t, &fw, 3, 20);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 1U);
	/* The firmware may still read the sections: nothing is released. */
	KUNIT_EXPECT_EQ(test, fw.releases, 0U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 2);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_HELD);
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_by_id(t, 20));
	ane_t6021_progtab_count(t, &live, &held);
	KUNIT_EXPECT_EQ(test, live, 0U);
	KUNIT_EXPECT_EQ(test, held, 1U);
	/* A held slot is never reused before a restart. */
	q = load(t, &fw, 4, 21);
	KUNIT_EXPECT_PTR_NE(test, q, p);
	KUNIT_EXPECT_EQ(test, t->hi, 2U);
	/* A verified restart releases LIVE and HELD programs alike. */
	ane_t6021_progtab_reset(t, &fake_ops, &fw);
	KUNIT_EXPECT_EQ(test, fw.releases, 2U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_EQ(test, t->hi, 0U);
}

static void progtab_load_failure_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct ane_t6021_prog *p;
	u8 digest[32];

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	memset(digest, 5, sizeof(digest));
	p = ane_t6021_progtab_reserve(t, digest);
	KUNIT_ASSERT_NOT_NULL(test, p);
	/* An uncommitted program is invisible and cannot be referenced. */
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_find(t, digest));
	KUNIT_EXPECT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), -ESTALE);
	KUNIT_EXPECT_EQ(test, a->n, 0U);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_HELD);
}

static void progtab_restart_stale_ids_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct ane_t6021_fdprogs *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	struct fake_fw fw = {};
	struct ane_t6021_prog *p, *q;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	p = load(t, &fw, 6, 30);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	ane_t6021_progtab_reset(t, &fake_ops, &fw);
	/* Another file loads after the restart and reuses the slot. */
	q = load(t, &fw, 7, 31);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, b, q), 0);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	/* The old file's stale ID neither reaches nor drops the new program. */
	KUNIT_EXPECT_FALSE(test, ane_t6021_fdprogs_has(a, 31));
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_by_id(t, 30));
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 0U);
	KUNIT_EXPECT_EQ(test, q->users, 1U);
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, b, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
}

static void progtab_capacity_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct ane_t6021_fdprogs *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	struct fake_fw fw = {};
	struct ane_t6021_prog *p;
	u8 digest[32];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++) {
		memset(digest, 0, sizeof(digest));
		digest[0] = i;
		digest[1] = i >> 8;
		p = ane_t6021_progtab_reserve(t, digest);
		KUNIT_ASSERT_NOT_NULL(test, p);
		ane_t6021_progtab_commit(p, 1000 + i, i);
		KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	}
	memset(digest, 0xff, sizeof(digest));
	KUNIT_EXPECT_NULL(test, ane_t6021_progtab_reserve(t, digest));
	KUNIT_EXPECT_FALSE(test, ane_t6021_fdprogs_room(t, a));
	/* Closing the file unloads all of them and frees every slot. */
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &fake_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, (unsigned int)ANE_T6021_MAX_PROGRAMS);
	p = ane_t6021_progtab_reserve(t, digest);
	KUNIT_EXPECT_PTR_EQ(test, p, &t->progs[0]);

	/* A full file list drops stale IDs before it refuses a reference. */
	b->n = ANE_T6021_MAX_PROGRAMS;
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++)
		b->ids[i] = 5000 + i;
	ane_t6021_progtab_commit(p, 9000, 1);
	KUNIT_EXPECT_EQ(test, ane_t6021_fdprogs_ref(t, b, p), 0);
	KUNIT_EXPECT_EQ(test, b->n, 1U);
	KUNIT_EXPECT_EQ(test, p->users, 1U);
}

static struct ane_t6021_progtab *restart_table;

/* An unload whose failure made the driver restart the firmware. */
static int restart_unload(void *ctx, struct ane_t6021_prog *p)
{
	struct fake_fw *fw = ctx;

	fw->unloads++;
	ane_t6021_progtab_reset(restart_table, &fake_ops, fw);
	return -ETIMEDOUT;
}

static const struct ane_t6021_progtab_ops restart_ops = {
	.unload = restart_unload,
	.release = fake_release,
};

static void progtab_restart_during_unload_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct ane_t6021_fdprogs *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct fake_fw fw = {};
	struct ane_t6021_prog *p, *q;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, a);
	restart_table = t;
	p = load(t, &fw, 8, 40);
	q = load(t, &fw, 9, 41);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, p), 0);
	KUNIT_ASSERT_EQ(test, ane_t6021_fdprogs_ref(t, a, q), 0);
	/* The restart released both programs; nothing may be marked HELD
	 * afterwards and the second program needs no unload of its own.
	 */
	KUNIT_EXPECT_EQ(test, ane_t6021_progtab_fd_close(t, a, &restart_ops, &fw), 0U);
	KUNIT_EXPECT_EQ(test, fw.unloads, 1U);
	KUNIT_EXPECT_EQ(test, fw.releases, 2U);
	KUNIT_EXPECT_EQ(test, fw.sec_refs, 0);
	KUNIT_EXPECT_EQ(test, p->state, ANE_T6021_PROG_FREE);
	KUNIT_EXPECT_EQ(test, t->gen, 1U);
}

static void progtab_cancel_test(struct kunit *test)
{
	struct ane_t6021_progtab *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct fake_fw fw = {};
	struct ane_t6021_prog *p;
	u8 digest[32];

	KUNIT_ASSERT_NOT_NULL(test, t);
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

static struct kunit_case lifetime_cases[] = {
	KUNIT_CASE(mring_classify_test),
	KUNIT_CASE(legtab_reuse_test),
	KUNIT_CASE(progtab_shared_unload_test),
	KUNIT_CASE(progtab_failed_unload_holds_test),
	KUNIT_CASE(progtab_load_failure_test),
	KUNIT_CASE(progtab_restart_stale_ids_test),
	KUNIT_CASE(progtab_capacity_test),
	KUNIT_CASE(progtab_restart_during_unload_test),
	KUNIT_CASE(progtab_cancel_test),
	KUNIT_CASE(unload_order_test),
	{}
};

static struct kunit_suite lifetime_suite = {
	.name = "ane-t6021-lifetime",
	.test_cases = lifetime_cases,
};

kunit_test_suite(lifetime_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE H14 firmware allocation and program lifetime tests");
