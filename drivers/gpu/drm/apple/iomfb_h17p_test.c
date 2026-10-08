// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "iomfb_h17p.h"

static void h17p_surface_wire_test(struct kunit *test)
{
	struct dcp_swap_submit_req_h17p *request;
	struct dcp_present_h17p *wire;
	struct dcp_surface *surface;
	u64 iova = 0x123456789aULL;

	request = kunit_kzalloc(test, sizeof(*request), GFP_KERNEL);
	wire = kunit_kmalloc(test, sizeof(*wire), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, request);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	surface = &request->surf[0].base;
	memset(wire, 0xa5, sizeof(*wire));
	surface->is_premultiplied = 1;
	surface->plane_cnt = 1;
	surface->plane_cnt2 = 1;
	surface->format = DCP_FORMAT_BGRA;
	surface->xfer_func = DCP_XFER_FUNC_SDR;
	surface->colorspace = DCP_COLORSPACE_NATIVE;
	surface->stride = 10176;
	surface->width = 2408;
	surface->height = 1506;
	surface->buf_size = 10176 * 1506;
	surface->planes[0].width = 2408;
	surface->planes[0].height = 1506;
	surface->planes[0].stride = 10176;
	surface->planes[0].size = 10176 * 1506;
	request->surf_iova[0] = iova;
	request->surf_null[0] = false;
	request->surf_null[1] = true;
	request->surf_null[2] = true;
	request->surf_null[3] = true;

	iomfb_serialize_present_h17p(wire, request);

	KUNIT_EXPECT_MEMEQ(test, wire->swap, &request->swap, sizeof(wire->swap));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x593),
			DCP_FORMAT_BGRA);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x59d), 10176U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5a9), 2408U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5ad), 1506U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5b1),
			10176U * 1506U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5e1), 2408U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5e5), 1506U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5f1), 10176U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)wire + 0x5f5),
			10176U * 1506U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64((u8 *)wire + 0xe38), iova);
	KUNIT_EXPECT_EQ(test, wire->tail[sizeof(wire->tail) - 2], 0U);
	KUNIT_EXPECT_EQ(test, wire->tail[sizeof(wire->tail) - 1], 0U);
}

static void h17p_opaque_x_parameter_test(struct kunit *test)
{
	struct dcp_apply_property_h17p property = dcp_opaque_x_property_h17p();

	KUNIT_EXPECT_EQ(test, property.property, 0x49U);
	KUNIT_EXPECT_EQ(test, property.value, 0U);
}

static void h17p_callback_bounds_test(struct kunit *test)
{
	/* Every installed handler is reachable without failing the session. */
	KUNIT_EXPECT_EQ(test, iomfb_h17p_first_unbounded_callback(), -1);

	/* Startup observations stay exact. */
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(114, 0x1044, 0x1004));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(114, 0x1044, 0x1008));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(590, 0x734, 0x0));

	/* Runtime callbacks must hold what their handlers read and write. */
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(209, 0x0, 0x8));
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(209, 0x4, 0xc));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(209, 0x0, 0x4));
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(593, 0x4, 0x0));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(593, 0x2, 0x0));
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(594, 0x4, 0x0));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(594, 0x0, 0x0));
	KUNIT_EXPECT_TRUE(test, iomfb_validate_callback_h17p(585, 0xe4, 0xe0));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(585, 0xe0, 0xe0));

	/* Tags without a handler or out of range never validate. */
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(5, 0x0, 0x0));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(-1, 0x0, 0x0));
	KUNIT_EXPECT_FALSE(test, iomfb_validate_callback_h17p(IOMFB_MAX_CB, 0x0, 0x0));
}

static void h17p_present_abort_test(struct kunit *test)
{
	struct dcp_present_state_h17p state = {};

	/* An abort after acceptance ends the present without a completion. */
	KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(&state, 7));
	KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(&state, 7, true));
	KUNIT_EXPECT_FALSE(test, dcp_present_abort_h17p(&state, 6));
	KUNIT_EXPECT_TRUE(test, state.pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_was_aborted_h17p(&state, 7));
	KUNIT_EXPECT_TRUE(test, dcp_present_abort_h17p(&state, 7));
	KUNIT_EXPECT_FALSE(test, state.pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_complete_h17p(&state, 7));
	/* A late completion for it is recognisable as stale, not unexpected. */
	KUNIT_EXPECT_TRUE(test, dcp_present_was_aborted_h17p(&state, 7));
	KUNIT_EXPECT_FALSE(test, dcp_present_was_aborted_h17p(&state, 6));
	KUNIT_EXPECT_FALSE(test, dcp_present_abort_h17p(&state, 7));

	/* An abort before the submit reply leaves the reply to finish it. */
	KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(&state, 8));
	KUNIT_EXPECT_TRUE(test, dcp_present_abort_h17p(&state, 8));
	KUNIT_EXPECT_TRUE(test, state.pending);
	KUNIT_EXPECT_TRUE(test, state.aborted);
	KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(&state, 8, true));
	KUNIT_EXPECT_FALSE(test, state.pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_complete_h17p(&state, 8));

	/* The next present starts clean and completes normally. */
	KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(&state, 9));
	KUNIT_EXPECT_FALSE(test, state.aborted);
	KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(&state, 9, true));
	KUNIT_EXPECT_TRUE(test, state.pending);
	/* A stale completion for 8 neither matches nor disturbs present 9. */
	KUNIT_EXPECT_FALSE(test, dcp_present_complete_h17p(&state, 8));
	KUNIT_EXPECT_TRUE(test, dcp_present_was_aborted_h17p(&state, 8));
	KUNIT_EXPECT_TRUE(test, state.pending);
	KUNIT_EXPECT_TRUE(test, dcp_present_complete_h17p(&state, 9));
	KUNIT_EXPECT_FALSE(test, state.pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_was_aborted_h17p(&state, 9));
}

static struct kunit_case h17p_present_cases[] = {
	KUNIT_CASE(h17p_surface_wire_test),
	KUNIT_CASE(h17p_opaque_x_parameter_test),
	KUNIT_CASE(h17p_callback_bounds_test),
	KUNIT_CASE(h17p_present_abort_test),
	{}
};

static struct kunit_suite h17p_present_suite = {
	.name = "apple-dcp-h17p-present",
	.test_cases = h17p_present_cases,
};

kunit_test_suite(h17p_present_suite);

MODULE_LICENSE("Dual MIT/GPL");
