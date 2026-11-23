// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple SoC platform error reporting driver
 *
 * Copyright The Aurora Silicon Contributors
 *
 * Included at the end of apple_soc_edac.c. The status registers are a fake
 * write-one-to-clear register block behind the driver's static-stubbed
 * accessors, so no MMIO is touched and every access is bounds checked.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "edac_module.h"
#include "apple_soc_edac-internal.h"

#define EDAC_TEST_WORDS		64
/* Not a valid interrupt: the IRQ core ignores masks and unmasks of it. */
#define EDAC_TEST_IRQ		INT_MAX

struct edac_test_fake {
	u32 regs[EDAC_TEST_WORDS];
	/* Bits that stay set after an acknowledge. */
	u32 sticky[EDAC_TEST_WORDS];
	unsigned int acks[EDAC_TEST_WORDS];
	unsigned int reads;
	unsigned int writes;
	unsigned int stray;
};

struct edac_test_ctx {
	struct device *dev;
	struct edac_test_fake fake;
	struct apple_soc_edac edac;
	struct apple_soc_edac_irq irq;
	void __iomem *status_regs[T6041_NI_REGS];
};

static int edac_test_word(struct edac_test_fake *fake, void __iomem *reg)
{
	uintptr_t off = (uintptr_t)(void __force *)reg - (uintptr_t)fake->regs;

	if (off % 4 || off >= sizeof(fake->regs)) {
		fake->stray++;
		return -1;
	}

	return off / 4;
}

static u32 edac_test_read(void __iomem *reg)
{
	struct kunit *test = kunit_get_current_test();
	struct edac_test_ctx *ctx = test->priv;
	int word = edac_test_word(&ctx->fake, reg);

	if (word < 0)
		return 0;
	ctx->fake.reads++;
	return ctx->fake.regs[word];
}

static void edac_test_write(void __iomem *reg, u32 val)
{
	struct kunit *test = kunit_get_current_test();
	struct edac_test_ctx *ctx = test->priv;
	int word = edac_test_word(&ctx->fake, reg);

	if (word < 0)
		return;
	ctx->fake.writes++;
	ctx->fake.acks[word]++;
	ctx->fake.regs[word] = (ctx->fake.regs[word] & ~val) |
			       ctx->fake.sticky[word];
}

static void edac_test_free_dci(void *data)
{
	edac_device_free_ctl_info(data);
}

static void edac_test_cancel(void *data)
{
	struct apple_soc_edac_irq *irq_data = data;

	cancel_delayed_work_sync(&irq_data->rearm);
}

/*
 * Set up one error line of @hw the way probe does, with the T6041 status
 * words and the NI status block of the line in the fake register block.
 */
static struct edac_test_ctx *edac_test_line(struct kunit *test,
					    const struct apple_soc_edac_hw *hw,
					    unsigned int index)
{
	struct edac_test_ctx *ctx = test->priv;
	struct apple_soc_edac_irq *irq_data = &ctx->irq;
	struct device *dev = ctx->dev;
	unsigned int i;

	/* A previous line of the same case: its EDAC device name is reused. */
	if (ctx->edac.dci) {
		kunit_release_action(test, edac_test_cancel, irq_data);
		kunit_release_action(test, edac_test_free_dci, ctx->edac.dci);
	}
	memset(&ctx->fake, 0, sizeof(ctx->fake));
	memset(&ctx->edac, 0, sizeof(ctx->edac));
	memset(irq_data, 0, sizeof(*irq_data));
	ctx->edac.dev = dev;
	ctx->edac.hw = hw;
	ctx->edac.irqs = irq_data;
	ctx->edac.dci = edac_device_alloc_ctl_info(0, "apple_soc_edac_kunit",
						   1, "platform", 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, ctx->edac.dci);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test,
							   edac_test_free_dci,
							   ctx->edac.dci));
	ctx->edac.dci->dev = dev;
	ctx->edac.dci->ctl_name = APPLE_SOC_EDAC_NAME;
	ctx->edac.dci->dev_name = dev_name(dev);
	ctx->edac.dci->log_ue = 0;
	ctx->edac.dci->panic_on_ue = 0;

	/* T6041 line 45 + n reads its status word at fake word n. */
	for (i = 0; i < T6041_NI_REGS; i++)
		ctx->status_regs[i] = (void __iomem __force *)
			((uintptr_t)&ctx->fake.regs[i] - T6041_NI_STATUS);
	if (hw->num_status_regs)
		ctx->edac.status_regs = ctx->status_regs;

	irq_data->edac = &ctx->edac;
	irq_data->source = apple_soc_edac_source(hw, index);
	KUNIT_ASSERT_NOT_NULL(test, irq_data->source);
	irq_data->index = index;
	irq_data->irq = EDAC_TEST_IRQ;
	irq_data->backoff_ms = APPLE_SOC_EDAC_BACKOFF_MS;
	irq_data->armed_jiffies = jiffies;
	INIT_DELAYED_WORK(&irq_data->rearm, apple_soc_edac_rearm);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test,
							   edac_test_cancel,
							   irq_data));

	/* An NI's status word n is fake word n. */
	for (i = 0; i < hw->nni; i++) {
		if (hw->ni[i].index != index)
			continue;
		irq_data->ni = &hw->ni[i];
		irq_data->ni_base = (void __iomem __force *)
			((uintptr_t)ctx->fake.regs - hw->ni[i].status);
	}

	return ctx;
}

static u32 edac_test_ue(struct edac_test_ctx *ctx)
{
	return ctx->edac.dci->counters.ue_count;
}

/* Run the IRQ thread; returns the rearm delay in jiffies, 0 if none. */
static unsigned long edac_test_fire(struct kunit *test,
				    struct edac_test_ctx *ctx,
				    unsigned int expect_backoff_ms)
{
	struct apple_soc_edac_irq *irq_data = &ctx->irq;
	unsigned long before, after, expires, delay;

	before = jiffies;
	KUNIT_EXPECT_EQ(test, IRQ_HANDLED,
			apple_soc_edac_irq_thread(irq_data->irq, irq_data));
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
	after = jiffies;

	if (!delayed_work_pending(&irq_data->rearm)) {
		KUNIT_EXPECT_EQ(test, 0U, expect_backoff_ms);
		return 0;
	}

	expires = irq_data->rearm.timer.expires;
	cancel_delayed_work_sync(&irq_data->rearm);
	delay = msecs_to_jiffies(expect_backoff_ms);
	KUNIT_EXPECT_NE(test, 0U, expect_backoff_ms);
	KUNIT_EXPECT_TRUE(test, time_after_eq(expires, before + delay));
	KUNIT_EXPECT_TRUE(test, time_before_eq(expires, after + delay));

	return expires - before;
}

static int edac_test_init(struct kunit *test)
{
	struct edac_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->dev = kunit_device_register(test, "apple-soc-edac-kunit");
	if (IS_ERR(ctx->dev))
		return PTR_ERR(ctx->dev);
	kunit_activate_static_stub(test, apple_soc_edac_read, edac_test_read);
	kunit_activate_static_stub(test, apple_soc_edac_write, edac_test_write);

	return 0;
}

static const char *const edac_test_error_compatibles[] = {
	"apple,t6000-error-handler",
	"apple,t6041-error-handler", "apple,t8112-error-handler",
	"apple,t8140-error-handler",
};

static const char *const edac_test_event_compatibles[] = {
	"apple,t6000-event-log",
	"apple,t6041-event-log", "apple,t8112-event-log",
	"apple,t8140-event-log",
};

static const struct apple_soc_edac_hw *edac_test_hw(struct kunit *test,
						     const char *compatible)
{
	const struct of_device_id *id;

	for (id = apple_soc_edac_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, compatible)) {
			KUNIT_ASSERT_NOT_NULL(test, id->data);
			return id->data;
		}

	KUNIT_FAIL_AND_ABORT(test, "no match data for %s", compatible);
	return NULL;
}

static void edac_test_expect_source(struct kunit *test,
				    const struct apple_soc_edac_hw *hw,
				    unsigned int index, const char *expect)
{
	const char *name = apple_soc_edac_source(hw, index);

	if (!expect) {
		KUNIT_EXPECT_NULL_MSG(test, name, "index %u", index);
		return;
	}
	KUNIT_EXPECT_NOT_NULL_MSG(test, name, "index %u", index);
	if (name)
		KUNIT_EXPECT_STREQ_MSG(test, name, expect, "index %u", index);
}

/* Every T6041 line maps to its range name, gaps and the tail to none. */
static void edac_test_source_t6041(struct kunit *test)
{
	static const struct {
		unsigned int index;
		const char *name;
	} cases[] = {
		{ 0, "sep" }, { 8, "sep" }, { 9, "pmgr" }, { 10, NULL },
		{ 25, NULL }, { 26, "sram" }, { 28, "sram" }, { 29, NULL },
		{ 44, NULL }, { 45, "ni0" }, { 47, "ni0" }, { 48, "ni1" },
		{ 51, "ni1" }, { 52, "ni2" }, { 56, "ni3" }, { 63, "ni3" },
		{ 64, "ni4" }, { 66, "ni5" }, { 68, "ni6" }, { 69, "ni6" },
		{ 70, NULL }, { 88, NULL }, { 89, NULL }, { 255, NULL },
		{ UINT_MAX, NULL },
	};
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		edac_test_expect_source(test, hw, cases[i].index, cases[i].name);
}

/* Range edges of every SoC: first and last line in, neighbours per table. */
static void edac_test_source_edges(struct kunit *test)
{
	unsigned int h, r, i, claimed;

	for (h = 0; h < ARRAY_SIZE(edac_test_error_compatibles); h++) {
		const struct apple_soc_edac_hw *hw =
			edac_test_hw(test, edac_test_error_compatibles[h]);

		claimed = 0;
		for (i = 0; i < hw->num_irqs; i++)
			claimed += !!apple_soc_edac_source(hw, i);

		for (r = 0; r < hw->nranges; r++) {
			const struct apple_soc_edac_range *range = &hw->ranges[r];
			unsigned int last = range->first + range->count - 1;

			edac_test_expect_source(test, hw, range->first,
						range->name);
			edac_test_expect_source(test, hw, last, range->name);
			claimed -= range->count;
		}
		/* Lines outside every range are left unclaimed. */
		KUNIT_EXPECT_EQ_MSG(test, claimed, 0U, "hw %u", h);
		KUNIT_EXPECT_NULL(test, apple_soc_edac_source(hw, hw->num_irqs));
	}
}

/* The ranges fit the interrupt count, ascend and do not overlap. */
static void edac_test_tables(struct kunit *test)
{
	unsigned int h, r, end;

	for (h = 0; h < ARRAY_SIZE(edac_test_error_compatibles); h++) {
		const struct apple_soc_edac_hw *hw =
			edac_test_hw(test, edac_test_error_compatibles[h]);

		KUNIT_EXPECT_FALSE(test, hw->event_log);
		KUNIT_EXPECT_GT(test, hw->nranges, (size_t)0);
		end = 0;
		for (r = 0; r < hw->nranges; r++) {
			const struct apple_soc_edac_range *range = &hw->ranges[r];

			KUNIT_EXPECT_NOT_NULL(test, range->name);
			KUNIT_EXPECT_GT(test, range->count, 0);
			KUNIT_EXPECT_GE_MSG(test, (unsigned int)range->first, end,
					    "hw %u range %u", h, r);
			end = range->first + range->count;
			KUNIT_EXPECT_LE_MSG(test, end, hw->num_irqs,
					    "hw %u range %u", h, r);
		}
	}

	for (h = 0; h < ARRAY_SIZE(edac_test_event_compatibles); h++) {
		const struct apple_soc_edac_hw *hw =
			edac_test_hw(test, edac_test_event_compatibles[h]);

		KUNIT_EXPECT_TRUE(test, hw->event_log);
		KUNIT_EXPECT_EQ(test, hw->nranges, (size_t)0);
		KUNIT_EXPECT_EQ(test, hw->nni, (size_t)0);
	}
}

/* T6041: exactly the NI lines 45-69 own one status aperture each. */
static void edac_test_t6041_status_lines(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	unsigned int i;

	KUNIT_EXPECT_EQ(test, hw->num_status_regs, T6041_NI_REGS);
	for (i = 45; i < 45 + T6041_NI_REGS; i++) {
		const char *name = apple_soc_edac_source(hw, i);

		KUNIT_ASSERT_NOT_NULL(test, name);
		KUNIT_EXPECT_EQ_MSG(test, strncmp(name, "ni", 2), 0, "index %u", i);
	}
	KUNIT_EXPECT_NULL(test, apple_soc_edac_source(hw, 44));
	KUNIT_EXPECT_NULL(test, apple_soc_edac_source(hw, 45 + T6041_NI_REGS));
}

/* T8140 NI blocks sit on claimed NI lines with a valid aperture and words. */
static void edac_test_t8140_ni_table(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	unsigned int i, j;

	KUNIT_EXPECT_EQ(test, hw->nni, (size_t)6);
	for (i = 0; i < hw->nni; i++) {
		const struct apple_soc_edac_ni *ni = &hw->ni[i];
		const char *name = apple_soc_edac_source(hw, ni->index);

		KUNIT_ASSERT_NOT_NULL(test, name);
		KUNIT_EXPECT_EQ(test, strncmp(name, "ni", 2), 0);
		KUNIT_EXPECT_NE(test, ni->words, 0U);
		KUNIT_EXPECT_EQ(test, ni->status % 4, 0);
		/* Interrupt n is described by the n-th interrupt-less reg. */
		KUNIT_EXPECT_GE(test, ni->reg, 9);
		for (j = 0; j < i; j++) {
			KUNIT_EXPECT_NE(test, ni->index, hw->ni[j].index);
			KUNIT_EXPECT_NE(test, ni->reg, hw->ni[j].reg);
		}
	}
	/* The NSI and PIO lines have no status block. */
	for (i = 24; i < 26; i++)
		for (j = 0; j < hw->nni; j++)
			KUNIT_EXPECT_NE(test, (unsigned int)hw->ni[j].index, i);
}

static void edac_test_event_log_source(struct kunit *test)
{
	unsigned int h;

	for (h = 0; h < ARRAY_SIZE(edac_test_event_compatibles); h++) {
		const struct apple_soc_edac_hw *hw =
			edac_test_hw(test, edac_test_event_compatibles[h]);

		edac_test_expect_source(test, hw, 0, "event-log");
		edac_test_expect_source(test, hw, hw->num_irqs - 1, "event-log");
		edac_test_expect_source(test, hw, hw->num_irqs, "event-log");
	}
}

static void edac_test_of_match(struct kunit *test)
{
	const struct of_device_id *id;
	unsigned int n = 0;

	for (id = apple_soc_edac_of_match; id->compatible[0]; id++) {
		const struct apple_soc_edac_hw *hw = id->data;

		KUNIT_ASSERT_NOT_NULL(test, hw);
		KUNIT_EXPECT_GT(test, hw->num_irqs, 0U);
		KUNIT_EXPECT_EQ_MSG(test, hw->event_log,
				    !!strstr(id->compatible, "-event-log"),
				    "%s", id->compatible);
		n++;
	}
	KUNIT_EXPECT_EQ(test, n, 8U);
}

static bool edac_test_t6041(struct edac_test_ctx *ctx, bool *error,
			    bool *cleared)
{
	return apple_soc_edac_t6041_status(&ctx->irq, error, cleared);
}

/* A T6041 NI word that reads zero is left alone. */
static void edac_test_t6041_status_zero(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 50);
	bool error = true, cleared = false;

	KUNIT_EXPECT_FALSE(test, edac_test_t6041(ctx, &error, &cleared));
	KUNIT_EXPECT_FALSE(test, error);
	KUNIT_EXPECT_TRUE(test, cleared);
	KUNIT_EXPECT_EQ(test, ctx->fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.writes, 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
}

/* Each bit is acknowledged; only bits 0-2 and 4-7 are platform errors. */
static void edac_test_t6041_status_mask(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 45);
	unsigned int bit;
	bool error, cleared;

	for (bit = 0; bit < 32; bit++) {
		ctx->fake.regs[0] = BIT(bit);
		KUNIT_EXPECT_TRUE(test, edac_test_t6041(ctx, &error, &cleared));
		KUNIT_EXPECT_EQ_MSG(test, error, bit < 8 && bit != 3, "bit %u", bit);
		KUNIT_EXPECT_TRUE(test, cleared);
		KUNIT_EXPECT_EQ(test, ctx->fake.regs[0], 0U);
	}
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[0], 32U);

	/* Bit 3 among higher bits is still no error. */
	ctx->fake.regs[0] = 0xffffff08;
	apple_soc_edac_t6041_status(&ctx->irq, &error, &cleared);
	KUNIT_EXPECT_FALSE(test, error);
	ctx->fake.regs[0] = 0xffffffff;
	apple_soc_edac_t6041_status(&ctx->irq, &error, &cleared);
	KUNIT_EXPECT_TRUE(test, error);
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
}

/* A word that reads back set after the acknowledge is reported. */
static void edac_test_t6041_status_sticky(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 60);
	bool error, cleared;

	ctx->fake.regs[15] = 0x11;
	ctx->fake.sticky[15] = 0x10;
	KUNIT_EXPECT_TRUE(test, edac_test_t6041(ctx, &error, &cleared));
	KUNIT_EXPECT_TRUE(test, error);
	KUNIT_EXPECT_FALSE(test, cleared);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[15], 0x10U);
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[15], 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.reads, 2U);
}

/* Line 45 + n uses aperture n, and lines outside 45-69 touch none. */
static void edac_test_t6041_status_index(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 45);
	static const unsigned int outside[] = { 0, 9, 26, 44, 70, 88 };
	bool error, cleared;
	unsigned int i;

	for (i = 0; i < T6041_NI_REGS; i++)
		ctx->fake.regs[i] = 0x1;

	for (i = 0; i < T6041_NI_REGS; i++) {
		ctx->irq.index = 45 + i;
		KUNIT_EXPECT_TRUE(test, edac_test_t6041(ctx, &error, &cleared));
		KUNIT_EXPECT_EQ_MSG(test, ctx->fake.acks[i], 1U, "line %u", 45 + i);
		KUNIT_EXPECT_EQ(test, ctx->fake.regs[i], 0U);
	}

	for (i = 0; i < T6041_NI_REGS; i++)
		ctx->fake.regs[i] = 0x1;
	ctx->fake.reads = 0;
	for (i = 0; i < ARRAY_SIZE(outside); i++) {
		ctx->irq.index = outside[i];
		KUNIT_EXPECT_FALSE(test, edac_test_t6041(ctx, &error, &cleared));
		KUNIT_EXPECT_FALSE(test, error);
		KUNIT_EXPECT_TRUE(test, cleared);
	}
	KUNIT_EXPECT_EQ(test, ctx->fake.reads, 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
}

/* Only the words in the NI's mask are read, and set ones are acknowledged. */
static void edac_test_ni_status_words(struct kunit *test)
{
	/* SoC NI 0: 0x1ffd7ff */
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 18);
	unsigned int bit;
	bool cleared = false;

	KUNIT_ASSERT_NOT_NULL(test, ctx->irq.ni);
	for (bit = 0; bit < 32; bit++)
		ctx->fake.regs[bit] = 0xa5a5a5a5;

	KUNIT_EXPECT_TRUE(test, apple_soc_edac_ni_status(&ctx->irq, &cleared));
	KUNIT_EXPECT_TRUE(test, cleared);
	for (bit = 0; bit < 32; bit++) {
		bool in = ctx->irq.ni->words & BIT(bit);

		KUNIT_EXPECT_EQ_MSG(test, ctx->fake.acks[bit], in ? 1U : 0U,
				    "word %u", bit);
		KUNIT_EXPECT_EQ_MSG(test, ctx->fake.regs[bit],
				    in ? 0U : 0xa5a5a5a5U, "word %u", bit);
	}
	KUNIT_EXPECT_EQ(test, ctx->fake.reads,
			2 * hweight32(ctx->irq.ni->words));
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
}

/* No word set: nothing found and nothing written. */
static void edac_test_ni_status_none(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 22);
	bool cleared = false;

	KUNIT_ASSERT_NOT_NULL(test, ctx->irq.ni);
	KUNIT_EXPECT_FALSE(test, apple_soc_edac_ni_status(&ctx->irq, &cleared));
	KUNIT_EXPECT_TRUE(test, cleared);
	KUNIT_EXPECT_EQ(test, ctx->fake.writes, 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.reads,
			hweight32(ctx->irq.ni->words));
}

/* One word of several staying set makes the NI not cleared. */
static void edac_test_ni_status_sticky(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 23);
	bool cleared = true;

	/* GFX NI: 0x3ff7ff, so words 0 and 21 are both in the mask. */
	ctx->fake.regs[0] = 0x2;
	ctx->fake.regs[21] = 0x80000001;
	ctx->fake.sticky[21] = 0x80000000;
	KUNIT_EXPECT_TRUE(test, apple_soc_edac_ni_status(&ctx->irq, &cleared));
	KUNIT_EXPECT_FALSE(test, cleared);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[0], 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[21], 0x80000000U);
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[0], 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[21], 1U);
}

/* Word 31 sits at status + 0x7c; word 11 of an NI without it is never read. */
static void edac_test_ni_status_top_word(struct kunit *test)
{
	static const struct apple_soc_edac_ni ni = { 18, 9, 0x100, BIT(31) };
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 18);
	bool cleared;

	ctx->irq.ni = &ni;
	ctx->irq.ni_base = (void __iomem __force *)
		((uintptr_t)ctx->fake.regs - ni.status);
	ctx->fake.regs[31] = 0xffffffff;
	ctx->fake.regs[30] = 0x1;
	KUNIT_EXPECT_TRUE(test, apple_soc_edac_ni_status(&ctx->irq, &cleared));
	KUNIT_EXPECT_TRUE(test, cleared);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[31], 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[30], 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.reads, 2U);

	/* Word 11 is outside SoC NI 0's mask. */
	ctx->irq.ni = &ctx->edac.hw->ni[0];
	ctx->irq.ni_base = (void __iomem __force *)
		((uintptr_t)ctx->fake.regs - ctx->edac.hw->ni[0].status);
	ctx->fake.regs[30] = 0;
	ctx->fake.regs[11] = 0x1;
	KUNIT_EXPECT_FALSE(test, apple_soc_edac_ni_status(&ctx->irq, &cleared));
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[11], 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.stray, 0U);
}

/* T6041 NI error acknowledged: one UE, the line stays unmasked. */
static void edac_test_irq_t6041_error(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 47);

	ctx->fake.regs[2] = 0x4;
	KUNIT_EXPECT_EQ(test, edac_test_fire(test, ctx, 0), 0UL);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 1U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.regs[2], 0U);
}

/* Bit 3 alone is acknowledged without an error count. */
static void edac_test_irq_t6041_bit3(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 69);

	ctx->fake.regs[24] = BIT(3);
	KUNIT_EXPECT_EQ(test, edac_test_fire(test, ctx, 0), 0UL);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 0U);
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[24], 1U);
}

/* An NI line with no status set is masked and backed off, not counted. */
static void edac_test_irq_t6041_empty(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 56);

	edac_test_fire(test, ctx, APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 0U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.writes, 0U);
}

/* An acknowledge that does not take counts once and backs off. */
static void edac_test_irq_t6041_sticky(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6041-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 64);

	ctx->fake.regs[19] = 0x1;
	ctx->fake.sticky[19] = 0x1;
	edac_test_fire(test, ctx, APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 1U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 1U);
}

/* Lines without a status contract count a UE and back off. */
static void edac_test_irq_no_status(struct kunit *test)
{
	static const struct {
		const char *compatible;
		unsigned int index;
	} lines[] = {
		{ "apple,t6041-error-handler", 3 },		/* sep */
		{ "apple,t6041-error-handler", 27 },	/* sram */
		{ "apple,t6000-error-handler", 33 },	/* ni0, no status on T6000 */
		{ "apple,t8112-error-handler", 2 },		/* sep */
		{ "apple,t8140-error-handler", 24 },	/* nsi */
		{ "apple,t6000-event-log", 0 },
		{ "apple,t8140-event-log", 8 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lines); i++) {
		const struct apple_soc_edac_hw *hw;
		struct edac_test_ctx *ctx;

		hw = edac_test_hw(test, lines[i].compatible);
		ctx = edac_test_line(test, hw, lines[i].index);
		KUNIT_EXPECT_NULL(test, ctx->irq.ni);
		edac_test_fire(test, ctx, APPLE_SOC_EDAC_BACKOFF_MS);
		KUNIT_EXPECT_EQ_MSG(test, edac_test_ue(ctx), 1U, "line %u", i);
		KUNIT_EXPECT_EQ(test, ctx->fake.reads, 0U);
		KUNIT_EXPECT_EQ(test, ctx->fake.writes, 0U);
	}
}

/* T8140 NI: acknowledged status is one UE without a mask. */
static void edac_test_irq_ni_error(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 21);

	ctx->fake.regs[24] = 0x100;
	KUNIT_EXPECT_EQ(test, edac_test_fire(test, ctx, 0), 0UL);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 1U);
	KUNIT_EXPECT_EQ(test, ctx->fake.acks[24], 1U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 0U);
}

/* T8140 NI asserted with no status: masked, no UE. Sticky: UE once + mask. */
static void edac_test_irq_ni_empty_sticky(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8140-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 19);

	edac_test_fire(test, ctx, APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 0U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 1U);

	ctx->fake.regs[3] = 0x8;
	ctx->fake.sticky[3] = 0x8;
	edac_test_fire(test, ctx, 2 * APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), 1U);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 2U);
}

/* The backoff doubles to its cap; after the retry limit the line stays off. */
static void edac_test_irq_backoff_limit(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6000-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 0);
	unsigned int i, expect = APPLE_SOC_EDAC_BACKOFF_MS;

	for (i = 0; i < APPLE_SOC_EDAC_MAX_RETRIES; i++) {
		/* Fires straight after its unmask: still asserted. */
		ctx->irq.armed_jiffies = jiffies;
		edac_test_fire(test, ctx, expect);
		expect = min(expect * 2, APPLE_SOC_EDAC_BACKOFF_MAX_MS);
		KUNIT_EXPECT_EQ(test, ctx->irq.backoff_ms, expect);
	}
	KUNIT_EXPECT_EQ(test, ctx->irq.backoff_ms, APPLE_SOC_EDAC_BACKOFF_MAX_MS);

	ctx->irq.armed_jiffies = jiffies;
	edac_test_fire(test, ctx, 0);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, APPLE_SOC_EDAC_MAX_RETRIES + 1);
	KUNIT_EXPECT_EQ(test, edac_test_ue(ctx), APPLE_SOC_EDAC_MAX_RETRIES + 1);
}

/* A line that stayed quiet for a minute after its unmask starts over. */
static void edac_test_irq_backoff_reset(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t8112-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 1);

	ctx->irq.retries = APPLE_SOC_EDAC_MAX_RETRIES + 1;
	ctx->irq.backoff_ms = APPLE_SOC_EDAC_BACKOFF_MAX_MS;
	ctx->irq.armed_jiffies = jiffies -
		msecs_to_jiffies(APPLE_SOC_EDAC_BACKOFF_MAX_MS) - 1;
	edac_test_fire(test, ctx, APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 1U);
	KUNIT_EXPECT_EQ(test, ctx->irq.backoff_ms, 2 * APPLE_SOC_EDAC_BACKOFF_MS);

	/* Unmasked under a minute ago: still counting. */
	ctx->irq.armed_jiffies = jiffies -
		msecs_to_jiffies(APPLE_SOC_EDAC_BACKOFF_MAX_MS) + HZ;
	edac_test_fire(test, ctx, 2 * APPLE_SOC_EDAC_BACKOFF_MS);
	KUNIT_EXPECT_EQ(test, ctx->irq.retries, 2U);
}

/* The rearm work unmasks and restarts the quiet-period clock. */
static void edac_test_rearm(struct kunit *test)
{
	const struct apple_soc_edac_hw *hw =
		edac_test_hw(test, "apple,t6000-error-handler");
	struct edac_test_ctx *ctx = edac_test_line(test, hw, 12);
	unsigned long before;

	ctx->irq.armed_jiffies = jiffies - 10 * HZ;
	before = jiffies;
	apple_soc_edac_rearm(&ctx->irq.rearm.work);
	KUNIT_EXPECT_GE(test, ctx->irq.armed_jiffies, before);
	KUNIT_EXPECT_LE(test, ctx->irq.armed_jiffies, jiffies);
}

static struct kunit_case apple_soc_edac_test_cases[] = {
	KUNIT_CASE(edac_test_source_t6041),
	KUNIT_CASE(edac_test_source_edges),
	KUNIT_CASE(edac_test_tables),
	KUNIT_CASE(edac_test_t6041_status_lines),
	KUNIT_CASE(edac_test_t8140_ni_table),
	KUNIT_CASE(edac_test_event_log_source),
	KUNIT_CASE(edac_test_of_match),
	KUNIT_CASE(edac_test_t6041_status_zero),
	KUNIT_CASE(edac_test_t6041_status_mask),
	KUNIT_CASE(edac_test_t6041_status_sticky),
	KUNIT_CASE(edac_test_t6041_status_index),
	KUNIT_CASE(edac_test_ni_status_words),
	KUNIT_CASE(edac_test_ni_status_none),
	KUNIT_CASE(edac_test_ni_status_sticky),
	KUNIT_CASE(edac_test_ni_status_top_word),
	KUNIT_CASE(edac_test_irq_t6041_error),
	KUNIT_CASE(edac_test_irq_t6041_bit3),
	KUNIT_CASE(edac_test_irq_t6041_empty),
	KUNIT_CASE(edac_test_irq_t6041_sticky),
	KUNIT_CASE(edac_test_irq_no_status),
	KUNIT_CASE(edac_test_irq_ni_error),
	KUNIT_CASE(edac_test_irq_ni_empty_sticky),
	KUNIT_CASE(edac_test_irq_backoff_limit),
	KUNIT_CASE(edac_test_irq_backoff_reset),
	KUNIT_CASE(edac_test_rearm),
	{}
};

static struct kunit_suite apple_soc_edac_test_suite = {
	.name = "apple-soc-edac",
	.init = edac_test_init,
	.test_cases = apple_soc_edac_test_cases,
};

kunit_test_suite(apple_soc_edac_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
