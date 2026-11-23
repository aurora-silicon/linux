/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PEER_V13_5_H
#define APPLE_DCP_PEER_V13_5_H

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "dptx-caps.h"

/* The 13.5 TimingElements entry and A359 use the same physical timing. */
#define T602X_TIMING_SIZE	0x50
#define T602X_CLOCK_OFFSET	0x44

/* Merge timing and color identifiers use a separate firmware namespace. */
#define DCP_SPLIT_MODE_SUFFIX	800U

static inline u32 dcp_t602x_wire_clock(const u8 *timing, size_t size)
{
	return size == T602X_TIMING_SIZE ? get_unaligned_le32(timing + T602X_CLOCK_OFFSET) : 0;
}

/*
 * The M2 Max 13.5 firmware reports width and active-pixel limits in its
 * rejection log. With one engine and the AW2725DF, its catalogue offered
 * physical SDR RGB through 1360 MHz. This is a fallback for paired RGB.
 */
static inline struct dcp_dptx_caps dcp_t602x_limits(const struct dcp_dptx_caps *caps)
{
	if (caps->width && caps->total_rate && caps->active_rate)
		return *caps;

	return (struct dcp_dptx_caps){
		.width = 6144,
		.total_rate = 1360000000,
		.active_rate = 1274019840,
	};
}

static inline bool dcp_t602x_rate_fits(struct dcp_dptx_caps caps, u16 width, u16 height,
				      u16 htotal, u16 vtotal, u64 clock, unsigned int pipes)
{
	u64 active = (u64)pipes * caps.active_rate;

	if (!pipes || pipes > 2 || !width || !height || width > htotal || height > vtotal ||
	    !clock || clock > U32_MAX || width > (u64)pipes * caps.width ||
	    clock > (u64)pipes * caps.total_rate)
		return false;

	/* In the product comparison active < clock <= U32_MAX. */
	return clock <= active || clock * width * height <= active * htotal * vtotal;
}

static inline bool dcp_t602x_mode_fits(struct dcp_dptx_caps caps, u16 width, u16 height,
				      u16 htotal, u16 vtotal, int clock_khz, unsigned int pipes)
{
	return clock_khz > 0 && dcp_t602x_rate_fits(caps, width, height, htotal, vtotal,
						   (u64)clock_khz * 1000, pipes);
}

/* Equal halves must preserve every horizontal boundary. */
static inline bool dcp_t602x_even_horizontal(u16 active, u16 sync_start,
					     u16 sync_end, u16 total)
{
	return !((active | sync_start | sync_end | total) & 1);
}

static inline bool dcp_t602x_is_peer(const struct device_node *source,
				     const struct device_node *peer)
{
	u32 handle;

	if (!source || !peer || source == peer || !peer->phandle)
		return false;
	of_property_for_each_u32(source, "apple,scanout-peers", handle)
		if (handle == peer->phandle)
			return true;
	return false;
}

/*
 * Match the requested outputs to distinct physical CRTC bits. Each T602x die
 * can have at most two pairs. Backtracking uses explicit indices and no
 * recursion. On failure the caller discards the scratch choices.
 */
static inline int dcp_t602x_match(const u32 *options, u32 *choice, unsigned int count)
{
	u32 used = 0;
	unsigned int i = 0;

	while (i < count) {
		u32 previous = choice[i];
		u32 candidates;

		used &= ~previous;
		candidates = options[i] & ~used;
		if (previous)
			candidates &= ~(previous | (previous - 1));
		if (!candidates) {
			choice[i] = 0;
			if (!i)
				return -ENOSPC;
			i--;
			continue;
		}
		choice[i] = BIT(__ffs(candidates));
		used |= choice[i++];
	}
	return 0;
}

struct dcp_t602x_assignment {
	u64 generation;
	u32 peer_index;
	bool checked;
};

static inline bool dcp_t602x_peer_matches(const struct dcp_t602x_assignment *assignment,
					  u32 peer_index, u64 generation)
{
	return assignment->peer_index && assignment->peer_index == peer_index &&
	       assignment->generation == generation;
}

/*
 * DPTX SetTiledDisplayHints on T602x, firmware 13.5.
 * The first word is the request's flags. The status follows the
 * allocation, separately from the width and pixel rate in the request.
 */
struct dcp_display_hints_v13_5 {
	u8 hints[0x60];
	struct {
		__le32 primary_id;
		__le32 peer_id;
		u8 extra_pipes;
		u8 flags;
		u8 pad[2];
		__le32 max_width;
		__le32 max_rate;
	} __packed allocation;
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
	memset(&r->allocation, 0, sizeof(r->allocation));
	r->status = 0;
	return 0;
}

#endif
