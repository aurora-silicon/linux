/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PEER_V13_5_H
#define APPLE_DCP_PEER_V13_5_H

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * DPTX SetTiledDisplayHints on T602x, firmware 13.5.
 * The first word is the request's flags. The status follows the
 * allocation, separately from the width and pixel rate in the request.
 */
struct dcp_display_hints_v13_5 {
	u8 hints[0x60];
	u8 allocation[0x14];
	u8 reserved[0x0c];
	__le32 status;
	u8 tail[0x0c];
} __packed;

static_assert(sizeof(struct dcp_display_hints_v13_5) == 144);
static_assert(offsetof(struct dcp_display_hints_v13_5, allocation) == 0x60);
static_assert(offsetof(struct dcp_display_hints_v13_5, status) == 0x80);

static inline int dcp_no_peer_hints_v13_5(const struct dcp_display_hints_v13_5 *request,
					  void *reply, size_t reply_size)
{
	struct dcp_display_hints_v13_5 *r = reply;

	if (reply_size < sizeof(*r))
		return -EINVAL;

	memcpy(r, request, sizeof(*r));
	memset(r->allocation, 0, sizeof(r->allocation));
	r->status = 0;
	return 0;
}

#endif
