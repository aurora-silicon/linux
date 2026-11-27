// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dpavservep.c: which announced AV services the DP/HDMI AV
 * client admits. dcpavserv_identity() runs on a fake service, endpoint and
 * DCP; each row is one announcement (name, class, unit, channel) on one
 * endpoint and wire protocol.
 */
#include <kunit/test.h>

enum dpavid_role {
	DPAVID_LEGACY,	/* classic AFK wire */
	DPAVID_V2,	/* AFKV2 wire */
	DPAVID_EXT26,	/* AFKV2 wire, native26 external role */
};

struct dpavid_case {
	const char *desc;
	enum dpavid_role role;
	u32 endpoint;
	const char *name;
	const char *class;
	s64 unit;
	u32 channel;
	bool admitted;
};

#define DPAVID_NAME "dcpav-service-epic"

static const struct dpavid_case dpavid_cases[] = {
	/* The service name gates every role. */
	{ "legacy: no name", DPAVID_LEGACY, DPAVSERV_ENDPOINT, NULL, NULL, 0, 1, false },
	{ "legacy: other name", DPAVID_LEGACY, DPAVSERV_ENDPOINT, "dcpav-service",
	  NULL, 0, 1, false },
	{ "v2: other name", DPAVID_V2, DPAVSERV_ENDPOINT_V2, "dcpdp-service-epic",
	  "DCPDP13Service", 3, 3, false },
	{ "ext26: other name", DPAVID_EXT26, DPAVSERV_ENDPOINT, "dcpdp-service-epic",
	  "DCPDPService", 0, 1, false },

	/* Classic wire: unit 0 only, class and channel not consulted. */
	{ "legacy: unit 0", DPAVID_LEGACY, DPAVSERV_ENDPOINT, DPAVID_NAME, NULL, 0, 0, true },
	{ "legacy: unit 0, any class", DPAVID_LEGACY, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDPService", 0, 9, true },
	{ "legacy: unit 1", DPAVID_LEGACY, DPAVSERV_ENDPOINT, DPAVID_NAME, NULL, 1, 1, false },

	/* AFKV2 on the V2 endpoint: interface-id is the unit, equal to the channel. */
	{ "v2 ep 0x2a: DP2HDMI unit == channel", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "AppleDCPDP2HDMI", 3, 3, true },
	{ "v2 ep 0x2a: DP13 unit == channel", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDP13Service", 0xffff, 0xffff, true },
	{ "v2 ep 0x2a: unit != channel", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDP13Service", 2, 3, false },
	{ "v2 ep 0x2a: unit 0", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDP13Service", 0, 3, false },
	{ "v2 ep 0x2a: channel 0", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDP13Service", 0, 0, false },
	{ "v2 ep 0x2a: channel over u16", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDP13Service", 0x10000, 0x10000, false },
	{ "v2 ep 0x2a: no class", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  NULL, 3, 3, false },
	{ "v2 ep 0x2a: DCPDPService class", DPAVID_V2, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDPService", 3, 3, false },

	/*
	 * AFKV2 on the classic DPAV endpoint: the MCDP29xx converters and the
	 * M3/M4 Airs' generic DCPDP13Service, with EPICUnit 0 and a bounded
	 * dynamic interface-id. AppleDCPDP2HDMI and the other classes stay refused.
	 */
	{ "v2 ep 0x28: DP13 unit 0", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 0, 7, true },
	{ "v2 ep 0x28: DP2HDMI unit 0", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "AppleDCPDP2HDMI", 0, 1, false },
	{ "v2 ep 0x28: MCDP29XX unit 0", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "AppleDCPMCDP29XX", 0, 5, true },
	{ "v2 ep 0x28: MCDP2900 unit 0", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "AppleDCPMCDP2900", 0, 1, true },
	{ "v2 ep 0x28: MCDP2900 unit 1", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "AppleDCPMCDP2900", 1, 1, false },
	{ "v2 ep 0x28: other MCDP part", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "AppleDCPMCDP2800", 0, 1, false },
	{ "v2 ep 0x28: unit 0, channel at u16 max", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 0, 0xffff, true },
	{ "v2 ep 0x28: unit == channel", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 7, 7, false },
	{ "v2 ep 0x28: channel 0", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 0, 0, false },
	{ "v2 ep 0x28: channel over u16", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 0, 0x10000, false },
	{ "v2 ep 0x28: no class", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  NULL, 0, 7, false },
	{ "v2 ep 0x28: DCPDPService class", DPAVID_V2, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDPService", 0, 7, false },

	/* AFKV2 on any other endpoint. */
	{ "v2 other endpoint", DPAVID_V2, 0x29, DPAVID_NAME, "DCPDP13Service", 0, 7, false },

	/* native26: DCPDPService, unit 0, nonzero channel, classic endpoint only. */
	{ "ext26: DCPDPService unit 0", DPAVID_EXT26, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDPService", 0, 1, true },
	{ "ext26: unit 1", DPAVID_EXT26, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDPService", 1, 1, false },
	{ "ext26: channel 0", DPAVID_EXT26, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDPService", 0, 0, false },
	{ "ext26: no class", DPAVID_EXT26, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  NULL, 0, 1, false },
	{ "ext26: DP13 class", DPAVID_EXT26, DPAVSERV_ENDPOINT, DPAVID_NAME,
	  "DCPDP13Service", 0, 1, false },
	{ "ext26: V2 endpoint", DPAVID_EXT26, DPAVSERV_ENDPOINT_V2, DPAVID_NAME,
	  "DCPDPService", 0, 1, false },
};

static void dpavid_case_desc(const struct dpavid_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(dpavid, dpavid_cases, dpavid_case_desc);

static void dpavid_admission(struct kunit *test)
{
	const struct dpavid_case *c = test->param_value;
	struct apple_epic_service *svc;
	struct apple_dcp_afkep *ep;
	struct apple_dcp *dcp;

	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	ep = kunit_kzalloc(test, sizeof(*ep), GFP_KERNEL);
	svc = kunit_kzalloc(test, sizeof(*svc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, ep);
	KUNIT_ASSERT_NOT_NULL(test, svc);

	if (c->role == DPAVID_EXT26)
		dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_J613_EXT26;
	ep->dcp = dcp;
	ep->endpoint = c->endpoint;
	ep->wire_v2 = c->role != DPAVID_LEGACY;
	svc->ep = ep;
	svc->channel = c->channel;

	KUNIT_EXPECT_EQ(test, dcp_is_ext26(dcp), c->role == DPAVID_EXT26);
	KUNIT_EXPECT_EQ(test, dcpavserv_identity(svc, c->name, c->class, c->unit),
			c->admitted);
}

static struct kunit_case dpavid_test_cases[] = {
	KUNIT_CASE_PARAM(dpavid_admission, dpavid_gen_params),
	{}
};

static struct kunit_suite dpavid_suite = {
	.name = "apple-dcpavserv-identity",
	.test_cases = dpavid_test_cases,
};

kunit_test_suite(dpavid_suite);
