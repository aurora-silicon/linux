/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PEER_V27_H
#define APPLE_DCP_PEER_V27_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>

struct dcp_dptx_caps {
	u32 width, total_rate, active_rate;
};

/* Per-engine state used by native D114/D115 in a VFTG pair. This is not
 * display-wall emulation: membership is set only while a split mode is active.
 */
struct dcp_peer_tiling_v27 {
	u32 phase, modeset_state, x, y, width, height, display_id;
	bool member, vftg, primary, ready, mode_flag, display_id_valid;
	/* AP completion state, separate from transient firmware barriers. */
	bool modeset_complete;
};

static inline bool dcp_peer_tiling_get_v27(
	const struct dcp_peer_tiling_v27 *own,
	const struct dcp_peer_tiling_v27 *related, u32 event, u32 param, u32 *value)
{
	const struct dcp_peer_tiling_v27 *target = own;

	/* Native getter redirects event 2 only for an active pair. */
	if (event == 2 && own->member) {
		if (!related)
			return false;
		target = related;
	}
	switch (param) {
	case 1: *value = target->member; return true;
	case 2:
		if (event == 1 || target->member)
			*value = target->phase;
		return true;
	case 3: return true; /* Native non-wall case preserves the input. */
	case 4:
		if (event != 1 && !target->member)
			return true;
		if (!target->display_id_valid)
			return false;
		*value = target->display_id;
		return true;
	case 5: case 12: *value = target->x; return true;
	case 6:
		if (event == 1 || target->member)
			*value = target->ready;
		return true;
	case 8: *value = own->vftg; return true;
	case 9: *value = own->primary; return true;
	case 10:
		if (event == 1 || target->member)
			*value = target->modeset_state;
		return true;
	case 11: *value = 0; return true; /* This pair is not a display wall. */
	case 13: *value = target->y; return true;
	case 14: *value = target->mode_flag; return true;
	default: return false;
	}
}

static inline bool dcp_peer_tiling_set_v27(struct dcp_peer_tiling_v27 *own,
	struct dcp_peer_tiling_v27 *related, u32 event, u32 param, u32 value)
{
	/* Unlike the getter, the native setter always redirects event 2. */
	struct dcp_peer_tiling_v27 *target = event == 2 ? related : own;

	if (!target)
		return false;
	switch (param) {
	case 2: target->phase = value; return true;
	case 6: target->ready = !!value; return true;
	case 10: target->modeset_state = value; return true;
	default: return false;
	}
}

/* Native 26A428 AV display hints and display allocation records. */
struct dcp_display_hints_v27 {
	u8 hints[0x6c];
	u8 padding[4];
	__le32 primary_id, peer_id;
	u8 extra_pipes, flags;
	u8 allocation_reserved[22];
	__le32 status;
	u8 tail[12];
} __packed;

static_assert(sizeof(struct dcp_display_hints_v27) == 160);
static_assert(offsetof(struct dcp_display_hints_v27, primary_id) == 0x70);
static_assert(offsetof(struct dcp_display_hints_v27, status) == 0x90);

/* Extra pipe count, as the native 26A428 T602X display crossbar computes it.
 * Caps come from the live DPTX service, never from a forced clock limit.
 */
static inline u32 dcp_peer_extra_pipes(const u8 *hints,
				     const struct dcp_dptx_caps *caps)
{
	u32 width = get_unaligned_le32(hints + 0x38);
	u32 total = get_unaligned_le32(hints + 0x40);
	u32 active = get_unaligned_le32(hints + 0x44);
	u32 extra;

	if (!(hints[0] & 1) || !width || !total || !active ||
	    !caps->width || !caps->total_rate || !caps->active_rate)
		return 0;
	extra = (width - 1) / caps->width;
	extra = max(extra, (total - 1) / caps->total_rate);
	return max(extra, (active - 1) / caps->active_rate);
}

/* Peer validity rule of the native T8132 display crossbar, inherited by T6050.
 * Endpoint zero is the internal display; a peer must be on the same die.
 */
static inline bool dcp_peer_index_allowed(u32 primary, u32 peer)
{
	static const u8 masks[] = { 0, 0x1c, 0x18, 0x10, 0 };

	return primary < ARRAY_SIZE(masks) && peer < ARRAY_SIZE(masks) &&
	       (masks[primary] & BIT(peer));
}

#endif
