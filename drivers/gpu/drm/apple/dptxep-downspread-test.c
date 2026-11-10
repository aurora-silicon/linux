// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dptxep.c: the native (AFK v2) down-spread calls answer with
 * the PHY's capability and applied state, keep the fields they do not own
 * as received, and never publish a setting the PHY refused.
 */
#include <kunit/device.h>
#include <kunit/test.h>

struct dptx_ssc_test {
	struct apple_dcp dcp;
	struct apple_dcp_afkep ep;
	struct apple_epic_service service;
	struct dptx_port port;
	struct phy phy;
	struct phy_ops ops;
	bool phy_ssc;
	int configure_ret;
	unsigned int configures;
	struct phy_configure_opts_dp last;
};

static struct dptx_ssc_test *dptx_ssc_current;

static int dptx_ssc_validate(struct phy *phy, enum phy_mode mode, int submode,
			     union phy_configure_opts *opts)
{
	opts->dp.lanes = 4;
	opts->dp.ssc = dptx_ssc_current->phy_ssc;
	return 0;
}

static int dptx_ssc_configure(struct phy *phy, union phy_configure_opts *opts)
{
	dptx_ssc_current->configures++;
	dptx_ssc_current->last = opts->dp;
	return dptx_ssc_current->configure_ret;
}

static struct dptx_ssc_test *dptx_ssc_setup(struct kunit *test, bool with_phy, bool phy_ssc)
{
	struct dptx_ssc_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dcp.dev = kunit_device_register(test, "dptx-ssc-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t->dcp.dev));
	t->ep.dcp = &t->dcp;
	t->ep.wire_v2 = true;
	t->service.ep = &t->ep;
	t->service.cookie = &t->port;
	t->ops.validate = dptx_ssc_validate;
	t->ops.configure = dptx_ssc_configure;
	t->phy.ops = &t->ops;
	mutex_init(&t->phy.mutex);
	if (with_phy)
		t->port.atcphy = &t->phy;
	t->phy_ssc = phy_ssc;
	dptx_ssc_current = t;
	return t;
}

/* Every byte set, so a field the handler does not own shows if it moved. */
static void dptx_ssc_fill(void *buf, size_t len, u8 byte)
{
	memset(buf, byte, len);
}

static u64 dptx_ssc_call(struct kunit *test, struct dptx_ssc_test *t, u32 cmd, u64 want,
			 u32 *status)
{
	struct dptxport_apcall_downspread_v2 q, r;

	dptx_ssc_fill(&q, sizeof(q), 0x5a);
	q.enabled = cpu_to_le64(want);
	dptx_ssc_fill(&r, sizeof(r), 0xa5);
	KUNIT_ASSERT_EQ(test, dptxport_v2_group_call(&t->service, 0, cmd, &q, sizeof(q),
						     &r, sizeof(r)), 0);
	KUNIT_EXPECT_MEMEQ(test, r.rsvd0, q.rsvd0, sizeof(q.rsvd0));
	KUNIT_EXPECT_MEMEQ(test, r.rsvd1, q.rsvd1, sizeof(q.rsvd1));
	*status = le32_to_cpu(r.status);
	return le64_to_cpu(r.enabled);
}

static void dptx_ssc_supports(struct kunit *test, struct dptx_ssc_test *t, u8 expect)
{
	struct dptxport_apcall_supports_downspread_v2 q, r;

	dptx_ssc_fill(&q, sizeof(q), 0x5a);
	q.qualifier = 1;
	dptx_ssc_fill(&r, sizeof(r), 0xa5);
	KUNIT_ASSERT_EQ(test, dptxport_v2_group_call(&t->service, 0,
						     DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD,
						     &q, sizeof(q), &r, sizeof(r)), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(r.status), 0);
	KUNIT_EXPECT_EQ(test, r.supported, expect);
	KUNIT_EXPECT_EQ(test, r.qualifier, q.qualifier);
	KUNIT_EXPECT_MEMEQ(test, r.rsvd0, q.rsvd0, sizeof(q.rsvd0));
	KUNIT_EXPECT_MEMEQ(test, r.rsvd1, q.rsvd1, sizeof(q.rsvd1));
	KUNIT_EXPECT_MEMEQ(test, r.rsvd2, q.rsvd2, sizeof(q.rsvd2));
}

static void dptx_ssc_no_phy_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, false, true);
	u32 status;

	dptx_ssc_supports(test, t, 0);
	/* Enabling what the route lacks is refused and changes nothing. */
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD, 1, &status), 0);
	KUNIT_EXPECT_EQ(test, status, 1);
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_GET_DOWN_SPREAD, 0, &status), 0);
	KUNIT_EXPECT_EQ(test, status, 0);
	/* Disabling is always possible. */
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD, 0, &status), 0);
	KUNIT_EXPECT_EQ(test, status, 0);
	KUNIT_EXPECT_EQ(test, t->configures, 0);
}

static void dptx_ssc_phy_without_ssc_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, true, false);

	dptx_ssc_supports(test, t, 0);
}

static void dptx_ssc_before_rate_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, true, true);
	u32 status;

	dptx_ssc_supports(test, t, 1);
	/* No rate yet: kept for the next SetLinkRate, the PHY is not touched. */
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD, 1, &status), 1);
	KUNIT_EXPECT_EQ(test, status, 0);
	KUNIT_EXPECT_EQ(test, t->configures, 0);
	KUNIT_EXPECT_TRUE(test, t->port.phy_ops.dp.ssc);
	KUNIT_EXPECT_FALSE(test, t->port.ssc_applied);
}

static void dptx_ssc_at_rate_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, true, true);
	u32 status;

	dptx_ssc_supports(test, t, 1);
	t->port.link_rate = LINK_RATE_HBR2;
	t->port.phy_ops.dp.link_rate = 5400;
	/* Any nonzero 64-bit value means on. */
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD,
					    0x100000000ULL, &status), 1);
	KUNIT_EXPECT_EQ(test, status, 0);
	KUNIT_EXPECT_EQ(test, t->configures, 1);
	KUNIT_EXPECT_TRUE(test, t->last.ssc);
	KUNIT_EXPECT_TRUE(test, t->last.set_rate);
	KUNIT_EXPECT_EQ(test, t->last.link_rate, 5400);
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_GET_DOWN_SPREAD, 0, &status), 1);
	/* The same state again does not restart the PHY. */
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD, 1, &status), 1);
	KUNIT_EXPECT_EQ(test, t->configures, 1);
}

static void dptx_ssc_phy_refuses_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, true, true);
	u32 status;

	dptx_ssc_supports(test, t, 1);
	t->port.link_rate = LINK_RATE_HBR2;
	t->port.phy_ops.dp.link_rate = 5400;
	t->configure_ret = -EIO;
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_SET_DOWN_SPREAD, 1, &status), 0);
	KUNIT_EXPECT_EQ(test, status, 1);
	KUNIT_EXPECT_FALSE(test, t->port.phy_ops.dp.ssc);
	KUNIT_EXPECT_EQ(test, dptx_ssc_call(test, t, DPTX_APCALL_GET_DOWN_SPREAD, 0, &status), 0);
}

static void dptx_ssc_short_test(struct kunit *test)
{
	struct dptx_ssc_test *t = dptx_ssc_setup(test, true, true);
	u8 q[48] = {}, r[48] = {};

	KUNIT_EXPECT_EQ(test, dptxport_v2_group_call(&t->service, 0,
						     DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD,
						     q, 32, r, 48), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptxport_v2_group_call(&t->service, 0,
						     DPTX_APCALL_SET_DOWN_SPREAD,
						     q, 31, r, 32), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptxport_v2_group_call(&t->service, 0,
						     DPTX_APCALL_GET_DOWN_SPREAD,
						     q, 32, r, 16), -EINVAL);
	KUNIT_EXPECT_EQ(test, t->configures, 0);
}

static struct kunit_case dptx_ssc_cases[] = {
	KUNIT_CASE(dptx_ssc_no_phy_test),
	KUNIT_CASE(dptx_ssc_phy_without_ssc_test),
	KUNIT_CASE(dptx_ssc_before_rate_test),
	KUNIT_CASE(dptx_ssc_at_rate_test),
	KUNIT_CASE(dptx_ssc_phy_refuses_test),
	KUNIT_CASE(dptx_ssc_short_test),
	{}
};

static struct kunit_suite dptx_ssc_suite = {
	.name = "apple-dptx-downspread",
	.test_cases = dptx_ssc_cases,
};
kunit_test_suite(dptx_ssc_suite);
