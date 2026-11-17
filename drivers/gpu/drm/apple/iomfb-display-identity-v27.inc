/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Aurora Silicon */

struct dcp_display_product_request_v27 {
	u8 vendor[10];
	__le32 vendor_id;
	__le32 vendor_length;
	__le32 product_id;
	u8 vendor_id_null, product_id_null;
} __packed;

struct dcp_display_product_reply_v27 {
	u8 vendor[10];
	__le32 vendor_id;
	__le32 product_id;
	u8 success, padding;
} __packed;

static_assert(sizeof(struct dcp_display_product_request_v27) == 24);
static_assert(sizeof(struct dcp_display_product_reply_v27) == 20);
static_assert(offsetof(struct dcp_display_product_reply_v27, product_id) == 14);

struct dcp_display_identity_callback_v27 {
	void *output;
	enum dcp_context_id context;
};

static void display_identity_done_v27(struct apple_dcp *dcp, void *out, void *cookie)
{
	const struct dcp_display_product_reply_v27 *reply = out;
	struct dcp_display_identity_callback_v27 *cb = cookie;
	bool vendor_matches = reply && (!reply->vendor[0] ||
			      (!memcmp(reply->vendor, "APP", 3) && !reply->vendor[3]));
	bool display_x1 = reply && (reply->success & 1) && vendor_matches &&
			 (le32_to_cpu(reply->product_id) >> 1) == 0x5721;

	put_unaligned_le32(display_x1, cb->output);
	dcp_ack(dcp, cb->context);
	kfree(cb);
}

static bool trampoline_display_identity_v27(struct apple_dcp *dcp, int tag,
					   void *out, void *in)
{
	static const struct dcp_method_entry method = {
		.name = "display_product_id", .tag = {'A', '3', '7', '5'},
	};
	static const enum dcp_context_id contexts[] = {
		DCP_CONTEXT_CB, DCP_CONTEXT_ASYNC, DCP_CONTEXT_OOBCB, DCP_CONTEXT_OOBASYNC,
	};
	struct dcp_channel *channels[] = {
		&dcp->ch_cb, &dcp->ch_async, &dcp->ch_oobcb, &dcp->ch_oobasync,
	};
	struct dcp_display_product_request_v27 request = {
		.vendor_length = cpu_to_le32(10),
	};
	struct dcp_display_identity_callback_v27 *cb;
	int context = -1;

	trace_iomfb_callback(dcp, tag, __func__);

	for (unsigned int i = 0; i < ARRAY_SIZE(channels); i++) {
		struct dcp_channel *ch = channels[i];

		if (ch->depth && ch->output[ch->depth - 1] == out) {
			context = contexts[i];
			break;
		}
	}
	if (WARN_ON(context < 0))
		return true;
	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return true;
	cb->output = out;
	cb->context = context;
	if (dcp_push_nested(dcp, context, &method, sizeof(request),
			    sizeof(struct dcp_display_product_reply_v27), &request,
			    display_identity_done_v27, cb)) {
		kfree(cb);
		return true;
	}
	return false;
}
