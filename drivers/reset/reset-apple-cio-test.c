// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple CIO reset controller
 *
 * The CIO request register is a regmap whose read and write callbacks model
 * the request/busy handshake described at the top of reset-apple-cio.c, so
 * the real deassert sequences run without touching MMIO. A small device tree
 * fragment under a disabled container exercises the pmgr-child probe path,
 * the reset core's translation of reset ids and its refusal of ids past
 * nr_resets.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/of_platform.h>
#include <linux/reset.h>

/* Phandles well above the ones QEMU and the DT compilers hand out. */
#define CIO_TEST_PH_PROBE	0x00c10001
#define CIO_TEST_PH_CORE	0x00c10002
#define CIO_TEST_PH_NOREG	0x00c10003
#define CIO_TEST_T6000_REG	0x1a034

/*
 * One register with up to two timed transitions: after @left reads the value
 * becomes @next, then after @left2 more reads it becomes @next2. A write runs
 * the model's response to the request that was stored.
 */
struct cio_model {
	u32 reg;
	u32 val;
	unsigned int left, left2;
	u32 next, next2;
	bool stuck;		/* the current value never changes */
	void (*respond)(struct cio_model *m, u32 req);
	int write_err;
	unsigned int busy_reads;	/* settle delay the response uses */
	unsigned int reads, writes, stray;
	u32 last_write;
	u32 val_at_write;	/* register value seen by the write */
};

static int cio_model_read(void *ctx, unsigned int reg, unsigned int *val)
{
	struct cio_model *m = ctx;

	if (reg != m->reg) {
		m->stray++;
		*val = 0;
		return 0;
	}
	m->reads++;
	*val = m->val;
	if (m->stuck || !m->left)
		return 0;
	if (--m->left == 0) {
		m->val = m->next;
		m->left = m->left2;
		m->next = m->next2;
		m->left2 = 0;
	}
	return 0;
}

static int cio_model_write(void *ctx, unsigned int reg, unsigned int val)
{
	struct cio_model *m = ctx;

	if (reg != m->reg) {
		m->stray++;
		return 0;
	}
	if (m->write_err)
		return m->write_err;
	m->writes++;
	m->last_write = val;
	m->val_at_write = m->val;
	m->stuck = false;
	m->left2 = 0;
	if (m->respond)
		m->respond(m, val);
	return 0;
}

/* t8103: request clears itself, busy while it runs, then only done is set. */
static void cio_t8103_respond(struct cio_model *m, u32 req)
{
	m->val = req | T8103_CIO_CTRL_INIT_BUSY;
	m->left = m->busy_reads;
	m->next = T8103_CIO_CTRL_INIT_DONE;
}

/* t6000/t8142: request bit N raises its busy bit, both clear when done. */
static void cio_t6000_respond(struct cio_model *m, u32 req)
{
	u32 busy = req << 16;

	m->val |= req | busy;
	m->left = m->busy_reads;
	m->next = m->val & ~(req | busy);
}

static void cio_t8142_respond(struct cio_model *m, u32 req)
{
	u32 busy = req << 1;

	m->val |= req | busy;
	m->left = m->busy_reads;
	m->next = m->val & ~(req | busy);
}

/* t6050: request alone, then request and done, then both clear. */
static void cio_t6050_respond(struct cio_model *m, u32 req)
{
	m->val |= req;
	m->left = m->busy_reads;
	m->next = m->val | (req << 1);
	m->left2 = m->busy_reads;
	m->next2 = m->val & ~(req | (req << 1));
}

static const struct regmap_config cio_test_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.reg_read = cio_model_read,
	.reg_write = cio_model_write,
};

/* Model behind the syscon regmap of the DT fixture; reset by every case. */
static struct cio_model cio_test_dt_model;
static struct regmap *cio_test_dt_regmap;
static struct of_changeset cio_test_cs;
static struct device_node *cio_test_pmgr, *cio_test_probe_np, *cio_test_core_np,
	*cio_test_noreg_np, *cio_test_consumer;

struct cio_test {
	struct cio_model m;
	struct device *dev;
	struct apple_cio_reset priv;
	struct platform_device *pdev;
};

static int cio_test_init(struct kunit *test)
{
	struct cio_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev = kunit_device_register(test, "apple-cio-reset-test");
	if (IS_ERR(t->dev))
		return PTR_ERR(t->dev);
	t->priv.dev = t->dev;
	t->priv.regmap = devm_regmap_init(t->dev, NULL, &t->m, &cio_test_regmap_config);
	if (IS_ERR(t->priv.regmap))
		return PTR_ERR(t->priv.regmap);
	mutex_init(&t->priv.lock);
	t->priv.rcdev.ops = &apple_cio_reset_ops;
	t->m.busy_reads = 3;
	memset(&cio_test_dt_model, 0, sizeof(cio_test_dt_model));
	cio_test_dt_model.busy_reads = 3;
	test->priv = t;
	return 0;
}

static void cio_test_exit(struct kunit *test)
{
	struct cio_test *t = test->priv;

	if (t && t->pdev)
		of_platform_device_destroy(&t->pdev->dev, NULL);
}

static void cio_test_setup(struct cio_test *t, const struct apple_cio_reset_variant *v,
			   u32 offset, u32 reg,
			   void (*respond)(struct cio_model *m, u32 req))
{
	t->priv.variant = v;
	t->priv.offset = offset;
	t->priv.rcdev.nr_resets = v->nr_resets;
	t->m.reg = reg;
	t->m.respond = respond;
}

static int cio_test_deassert(struct cio_test *t, unsigned long id)
{
	return t->priv.rcdev.ops->deassert(&t->priv.rcdev, id);
}

static void cio_t8103_port_page_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	unsigned long id;

	for (id = 0; id < apple_t8103_cio_reset.nr_resets; id++) {
		cio_test_setup(t, &apple_t8103_cio_reset, 0x84000,
			       0x84000 + id * T8103_CIO_CTRL_STRIDE, cio_t8103_respond);
		t->m.val = 0;
		t->m.writes = 0;
		KUNIT_EXPECT_EQ(test, cio_test_deassert(t, id), 0);
		KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
		/* A plain store of the request, not a read-modify-write. */
		KUNIT_EXPECT_EQ(test, t->m.last_write, T8103_CIO_CTRL_INIT_REQ);
		KUNIT_EXPECT_EQ(test, t->m.val, T8103_CIO_CTRL_INIT_DONE);
		KUNIT_EXPECT_EQ(test, t->m.left, 0U);
	}
	KUNIT_EXPECT_EQ(test, t->m.stray, 0U);
}

static void cio_t8103_plain_store_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t8103_cio_reset, 0x84000, 0x84000, cio_t8103_respond);
	/* Unknown bits read back are not replayed into the request. */
	t->m.val = BIT(8) | BIT(31);
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.last_write, T8103_CIO_CTRL_INIT_REQ);
}

static void cio_t8103_waits_busy_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t8103_cio_reset, 0x84000, 0x84000, cio_t8103_respond);
	t->m.val = T8103_CIO_CTRL_INIT_BUSY;
	t->m.left = 4;
	t->m.next = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
	KUNIT_EXPECT_EQ(test, t->m.val_at_write & T8103_CIO_CTRL_INIT_BUSY, 0U);
	KUNIT_EXPECT_EQ(test, t->m.val, T8103_CIO_CTRL_INIT_DONE);
}

static void cio_t8103_busy_advisory_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	/* A reconfiguration that never finishes only warns before the request. */
	cio_test_setup(t, &apple_t8103_cio_reset, 0x84000, 0x84000, cio_t8103_respond);
	t->m.val = T8103_CIO_CTRL_INIT_BUSY;
	t->m.stuck = true;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
	KUNIT_EXPECT_EQ(test, t->m.val_at_write, T8103_CIO_CTRL_INIT_BUSY);
}

static void cio_t8103_timeout_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t8103_cio_reset, 0x84000, 0x84000, cio_t8103_respond);
	t->m.busy_reads = 0;	/* the response never settles */
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);

	/* Done must be the only bit set; anything else is not complete. */
	t->m.respond = NULL;
	t->m.val = T8103_CIO_CTRL_INIT_DONE | BIT(5);
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), -ETIMEDOUT);
}

static void cio_t6000_ports_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	unsigned long id;

	cio_test_setup(t, &apple_t6000_cio_reset, CIO_TEST_T6000_REG, CIO_TEST_T6000_REG,
		       cio_t6000_respond);
	for (id = 0; id < apple_t6000_cio_reset.nr_resets; id++) {
		t->m.writes = 0;
		KUNIT_EXPECT_EQ(test, cio_test_deassert(t, id), 0);
		KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
		KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(id));
		/* Returns only once request and busy cleared again. */
		KUNIT_EXPECT_EQ(test, t->m.val, 0U);
		KUNIT_EXPECT_EQ(test, t->m.left, 0U);
	}
	KUNIT_EXPECT_EQ(test, t->m.stray, 0U);
}

static void cio_t6000_own_busy_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t6000_cio_reset, CIO_TEST_T6000_REG, CIO_TEST_T6000_REG,
		       cio_t6000_respond);
	t->m.val = BIT(16 + 2);
	t->m.left = 4;
	t->m.next = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 2), 0);
	KUNIT_EXPECT_EQ(test, t->m.val_at_write, 0U);
	KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(2));
	KUNIT_EXPECT_EQ(test, t->m.val, 0U);
}

static void cio_t6000_other_busy_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	/* Another port's reconfiguration: store our request, keep its bits. */
	cio_test_setup(t, &apple_t6000_cio_reset, CIO_TEST_T6000_REG, CIO_TEST_T6000_REG,
		       cio_t6000_respond);
	t->m.val = BIT(16 + 3);
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(0));
	KUNIT_EXPECT_EQ(test, t->m.val_at_write, (u32)BIT(16 + 3));
	KUNIT_EXPECT_EQ(test, t->m.val, (u32)BIT(16 + 3));
}

static void cio_t6000_timeout_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t6000_cio_reset, CIO_TEST_T6000_REG, CIO_TEST_T6000_REG,
		       cio_t6000_respond);
	t->m.busy_reads = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 1), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
}

static void cio_t6000_write_error_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t6000_cio_reset, CIO_TEST_T6000_REG, CIO_TEST_T6000_REG,
		       cio_t6000_respond);
	t->m.write_err = -EIO;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 1), -EIO);
	KUNIT_EXPECT_EQ(test, t->m.writes, 0U);
	/* No polling for a request that was never stored. */
	KUNIT_EXPECT_EQ(test, t->m.reads, 1U);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&t->priv.lock));
}

static void cio_t8142_ports_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	unsigned long id;

	/* The variant's own register: the generic offset is not used. */
	cio_test_setup(t, &apple_t8142_cio_reset, 0x1000, T8142_CIO_CTRL, cio_t8142_respond);
	for (id = 0; id < apple_t8142_cio_reset.nr_resets; id++) {
		KUNIT_EXPECT_EQ(test, cio_test_deassert(t, id), 0);
		KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(2 * id));
		KUNIT_EXPECT_EQ(test, t->m.val, 0U);
		KUNIT_EXPECT_EQ(test, t->m.left, 0U);
	}
	KUNIT_EXPECT_EQ(test, t->m.writes, 4U);
	KUNIT_EXPECT_EQ(test, t->m.stray, 0U);
}

static void cio_t8142_serializes_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	/* Any port busy holds back a request for every port. */
	cio_test_setup(t, &apple_t8142_cio_reset, 0, T8142_CIO_CTRL, cio_t8142_respond);
	t->m.val = BIT(2 * 3) | BIT(2 * 3 + 1);
	t->m.left = 4;
	t->m.next = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.val_at_write, 0U);
	KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(0));
}

static void cio_t8142_refuses_busy_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t8142_cio_reset, 0, T8142_CIO_CTRL, cio_t8142_respond);
	t->m.val = BIT(2 * 1 + 1);
	t->m.stuck = true;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 2), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->m.writes, 0U);
	/* Request bits alone are not busy. */
	t->m.val = BIT(2 * 1);
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 2), 0);
	KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(2 * 2));
}

static void cio_t6050_sequence_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	unsigned long id;

	cio_test_setup(t, &apple_t6050_cio_reset, 0, 0x60, cio_t6050_respond);
	for (id = 0; id < apple_t6050_cio_reset.nr_resets; id++) {
		t->m.reads = 0;
		KUNIT_EXPECT_EQ(test, cio_test_deassert(t, id), 0);
		KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(2 * id));
		/* Both transitions were waited for: request alone, then done. */
		KUNIT_EXPECT_EQ(test, t->m.val, 0U);
		KUNIT_EXPECT_EQ(test, t->m.left, 0U);
		KUNIT_EXPECT_GE(test, t->m.reads, 1U + 2 * t->m.busy_reads);
	}
	KUNIT_EXPECT_EQ(test, t->m.stray, 0U);
}

static void cio_t6050_port_mask_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	cio_test_setup(t, &apple_t6050_cio_reset, 0, 0x60, cio_t6050_respond);
	/* Own request still pending from before: wait for it to clear. */
	t->m.val = BIT(2 * 1) | BIT(2 * 1 + 1);
	t->m.left = 3;
	t->m.next = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 1), 0);
	KUNIT_EXPECT_EQ(test, t->m.val_at_write, 0U);

	/* Another port's bits do not hold the request back. */
	t->m.val = BIT(2 * 3) | BIT(2 * 3 + 1);
	t->m.writes = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), 0);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
	KUNIT_EXPECT_EQ(test, t->m.val, (u32)(BIT(2 * 3) | BIT(2 * 3 + 1)));
}

static void cio_t6050_no_done_test(struct kunit *test)
{
	struct cio_test *t = test->priv;

	/* The request is never taken: done never shows up. */
	cio_test_setup(t, &apple_t6050_cio_reset, 0, 0x60, cio_t6050_respond);
	t->m.busy_reads = 0;
	KUNIT_EXPECT_EQ(test, cio_test_deassert(t, 0), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
	KUNIT_EXPECT_EQ(test, t->m.val, (u32)BIT(0));
}

static void cio_match_table_test(struct kunit *test)
{
	static const struct {
		const char *compatible;
		const struct apple_cio_reset_variant *v;
	} rows[] = {
		{ "apple,t6050-cio-reset", &apple_t6050_cio_reset },
		{ "apple,t8142-cio-reset", &apple_t8142_cio_reset },
		{ "apple,t8103-cio-reset", &apple_t8103_cio_reset },
		{ "apple,t6000-cio-reset", &apple_t6000_cio_reset },
		{ "apple,t6030-cio-reset", &apple_t6000_cio_reset },
	};
	const struct of_device_id *id;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		for (id = apple_cio_reset_match; id->compatible[0]; id++)
			if (!strcmp(id->compatible, rows[i].compatible))
				break;
		KUNIT_EXPECT_PTR_EQ_MSG(test, id->data, rows[i].v, "%s", rows[i].compatible);
	}
	KUNIT_EXPECT_EQ(test, apple_t8103_cio_reset.nr_resets, 2U);
	KUNIT_EXPECT_TRUE(test, apple_t6000_cio_reset.pmgr_child);
	KUNIT_EXPECT_FALSE(test, apple_t8142_cio_reset.pmgr_child);
	KUNIT_EXPECT_FALSE(test, apple_t6050_cio_reset.pmgr_child);
	KUNIT_EXPECT_EQ(test, apple_t6050_cio_reset.ctrl, 0x60U);
	/* Only deassert: asserting needs the power domain cut. */
	KUNIT_EXPECT_NULL(test, apple_cio_reset_ops.assert);
	KUNIT_EXPECT_NULL(test, apple_cio_reset_ops.reset);
}

/* resets = <&probe 0>, <&probe 1>, <&probe 4>, <&core 3>, <&core 4>, <&noreg 0> */
static void cio_core_xlate_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	struct reset_control *rc;

	cio_test_setup(t, &apple_t8142_cio_reset, 0, T8142_CIO_CTRL, cio_t8142_respond);
	t->priv.rcdev.of_node = cio_test_core_np;
	t->priv.rcdev.dev = t->dev;
	KUNIT_ASSERT_EQ(test, devm_reset_controller_register(t->dev, &t->priv.rcdev), 0);

	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 3);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rc), "get: %ld", PTR_ERR(rc));
	KUNIT_EXPECT_EQ(test, reset_control_deassert(rc), 0);
	KUNIT_EXPECT_EQ(test, t->m.last_write, (u32)BIT(2 * 3));
	KUNIT_EXPECT_EQ(test, t->m.val, 0U);
	/* No .assert op: an exclusive assert cannot be honoured. */
	KUNIT_EXPECT_EQ(test, reset_control_assert(rc), -ENOTSUPP);
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
	reset_control_put(rc);

	/* Id 4 is past the four ports. */
	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 4);
	KUNIT_EXPECT_PTR_EQ(test, rc, ERR_PTR(-EINVAL));
	KUNIT_EXPECT_EQ(test, t->m.writes, 1U);
}

static void cio_probe_pmgr_child_test(struct kunit *test)
{
	struct cio_model *m = &cio_test_dt_model;
	struct cio_test *t = test->priv;
	struct reset_control *rc;

	m->reg = CIO_TEST_T6000_REG;
	m->respond = cio_t6000_respond;
	t->pdev = of_platform_device_create(cio_test_probe_np, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, t->pdev);
	KUNIT_ASSERT_TRUE(test, device_is_bound(&t->pdev->dev));

	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 1);
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(rc), "get: %ld", PTR_ERR(rc));
	KUNIT_EXPECT_EQ(test, reset_control_deassert(rc), 0);
	/* The offset came from "reg", the register map from the parent. */
	KUNIT_EXPECT_EQ(test, m->writes, 1U);
	KUNIT_EXPECT_EQ(test, m->last_write, (u32)BIT(1));
	KUNIT_EXPECT_EQ(test, m->val, 0U);
	KUNIT_EXPECT_EQ(test, m->stray, 0U);
	reset_control_put(rc);

	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(rc));
	KUNIT_EXPECT_EQ(test, reset_control_deassert(rc), 0);
	KUNIT_EXPECT_EQ(test, m->last_write, (u32)BIT(0));
	reset_control_put(rc);

	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 2);
	KUNIT_EXPECT_PTR_EQ(test, rc, ERR_PTR(-EINVAL));
	KUNIT_EXPECT_EQ(test, m->writes, 2U);
}

static void cio_probe_no_reg_test(struct kunit *test)
{
	struct cio_test *t = test->priv;
	struct reset_control *rc;

	t->pdev = of_platform_device_create(cio_test_noreg_np, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, t->pdev);
	KUNIT_EXPECT_FALSE(test, device_is_bound(&t->pdev->dev));
	/* No controller was registered for it. */
	rc = of_reset_control_get_exclusive_by_index(cio_test_consumer, 5);
	KUNIT_EXPECT_PTR_EQ(test, rc, ERR_PTR(-EPROBE_DEFER));
	KUNIT_EXPECT_EQ(test, cio_test_dt_model.writes, 0U);
}

static struct device_node *cio_test_node(struct device_node *parent, const char *name,
					 const char *compatible, u32 phandle)
{
	struct device_node *np = of_changeset_create_node(&cio_test_cs, parent, name);

	if (!np)
		return NULL;
	if (compatible && of_changeset_add_prop_string(&cio_test_cs, np, "compatible",
						       compatible))
		return NULL;
	if (phandle && (of_changeset_add_prop_u32(&cio_test_cs, np, "phandle", phandle) ||
			of_changeset_add_prop_u32(&cio_test_cs, np, "#reset-cells", 1)))
		return NULL;
	return np;
}

static int cio_test_suite_init(struct kunit_suite *suite)
{
	static const u32 resets[] = {
		CIO_TEST_PH_PROBE, 0, CIO_TEST_PH_PROBE, 1, CIO_TEST_PH_PROBE, 4,
		CIO_TEST_PH_CORE, 3, CIO_TEST_PH_CORE, 4, CIO_TEST_PH_NOREG, 0,
	};
	struct device_node *box;
	int ret;

	if (!of_root)
		return -ENODEV;
	cio_test_dt_regmap = regmap_init(NULL, NULL, &cio_test_dt_model,
					 &cio_test_regmap_config);
	if (IS_ERR(cio_test_dt_regmap))
		return PTR_ERR(cio_test_dt_regmap);

	of_changeset_init(&cio_test_cs);
	/* Disabled container: nothing below it is populated on its own. */
	box = cio_test_node(of_root, "kunit-apple-cio-reset", NULL, 0);
	if (!box || of_changeset_add_prop_string(&cio_test_cs, box, "status", "disabled"))
		goto nomem;
	cio_test_pmgr = cio_test_node(box, "power-management", NULL, 0);
	cio_test_probe_np = cio_test_node(cio_test_pmgr, "cio-reset@1a034",
					  "apple,t6000-cio-reset", CIO_TEST_PH_PROBE);
	cio_test_noreg_np = cio_test_node(cio_test_pmgr, "cio-reset",
					  "apple,t6000-cio-reset", CIO_TEST_PH_NOREG);
	cio_test_core_np = cio_test_node(box, "core-reset", NULL, CIO_TEST_PH_CORE);
	cio_test_consumer = cio_test_node(box, "consumer", NULL, 0);
	if (!cio_test_pmgr || !cio_test_probe_np || !cio_test_noreg_np ||
	    !cio_test_core_np || !cio_test_consumer ||
	    of_changeset_add_prop_u32(&cio_test_cs, cio_test_probe_np, "reg",
				      CIO_TEST_T6000_REG) ||
	    of_changeset_add_prop_u32_array(&cio_test_cs, cio_test_consumer, "resets",
					    resets, ARRAY_SIZE(resets)))
		goto nomem;
	ret = of_changeset_apply(&cio_test_cs);
	if (ret)
		goto destroy;
	/*
	 * A changeset attaches a node before adding its properties, so the
	 * "phandle" property is not picked up by the attach: set it here.
	 */
	cio_test_probe_np->phandle = CIO_TEST_PH_PROBE;
	cio_test_noreg_np->phandle = CIO_TEST_PH_NOREG;
	cio_test_core_np->phandle = CIO_TEST_PH_CORE;
	/*
	 * A syscon entry cannot be removed, so the node it is keyed by keeps
	 * an extra reference and the register map stays: neither is freed.
	 */
	of_node_get(cio_test_pmgr);
	ret = of_syscon_register_regmap(cio_test_pmgr, cio_test_dt_regmap);
	if (ret)
		of_changeset_revert(&cio_test_cs);
	return ret;

nomem:
	ret = -ENOMEM;
destroy:
	of_changeset_destroy(&cio_test_cs);
	regmap_exit(cio_test_dt_regmap);
	return ret;
}

static void cio_test_suite_exit(struct kunit_suite *suite)
{
	of_changeset_revert(&cio_test_cs);
}

static struct kunit_case apple_cio_reset_test_cases[] = {
	KUNIT_CASE(cio_t8103_port_page_test),
	KUNIT_CASE(cio_t8103_plain_store_test),
	KUNIT_CASE(cio_t8103_waits_busy_test),
	KUNIT_CASE(cio_t8103_busy_advisory_test),
	KUNIT_CASE(cio_t8103_timeout_test),
	KUNIT_CASE(cio_t6000_ports_test),
	KUNIT_CASE(cio_t6000_own_busy_test),
	KUNIT_CASE(cio_t6000_other_busy_test),
	KUNIT_CASE(cio_t6000_timeout_test),
	KUNIT_CASE(cio_t6000_write_error_test),
	KUNIT_CASE(cio_t8142_ports_test),
	KUNIT_CASE(cio_t8142_serializes_test),
	KUNIT_CASE(cio_t8142_refuses_busy_test),
	KUNIT_CASE(cio_t6050_sequence_test),
	KUNIT_CASE(cio_t6050_port_mask_test),
	KUNIT_CASE(cio_t6050_no_done_test),
	KUNIT_CASE(cio_match_table_test),
	KUNIT_CASE(cio_core_xlate_test),
	KUNIT_CASE(cio_probe_pmgr_child_test),
	KUNIT_CASE(cio_probe_no_reg_test),
	{}
};

static struct kunit_suite apple_cio_reset_test_suite = {
	.name = "apple-cio-reset",
	.suite_init = cio_test_suite_init,
	.suite_exit = cio_test_suite_exit,
	.init = cio_test_init,
	.exit = cio_test_exit,
	.test_cases = apple_cio_reset_test_cases,
};

kunit_test_suite(apple_cio_reset_test_suite);
