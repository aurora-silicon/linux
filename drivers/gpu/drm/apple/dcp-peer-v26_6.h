/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PEER_V26_6_H
#define APPLE_DCP_PEER_V26_6_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/unaligned.h>

struct dcp_peer_tiling {
	u32 phase, modeset_state, x, y, width, height, display_id;
	bool member, vftg, primary, ready, mode_flag, display_id_valid;
	bool modeset_complete;
};

static inline bool dcp_peer_tiling_get(
	const struct dcp_peer_tiling *own,
	const struct dcp_peer_tiling *related, u32 event, u32 param, u32 *value)
{
	const struct dcp_peer_tiling *target = own;

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
	case 3: return true;
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
	case 11: *value = 0; return true;
	case 13: *value = target->y; return true;
	case 14: *value = target->mode_flag; return true;
	default: return false;
	}
}

static inline bool dcp_peer_tiling_set(struct dcp_peer_tiling *own,
	struct dcp_peer_tiling *related, u32 event, u32 param, u32 value)
{
	struct dcp_peer_tiling *target = event == 2 ? related : own;

	if (!target)
		return false;
	switch (param) {
	case 2: target->phase = value; return true;
	case 6: target->ready = !!value; return true;
	case 10: target->modeset_state = value; return true;
	default: return false;
	}
}

/*
 * Display allocation hints — 160 bytes, layout shared with fw27.
 * The AP fills primary/peer endpoint IDs and extra-pipe count for a sink
 * wider than one pipe's maximum.
 */
struct dcp_display_hints {
	u8 hints[0x6c];
	u8 padding[4];
	/* The allocation the AP fills in, or clears when it allocates no peer. */
	struct_group_attr(allocation, __packed,
		__le32 primary_id, peer_id;
		u8 extra_pipes, flags;
		u8 allocation_reserved[22];
	);
	__le32 status;
	u8 tail[12];
} __packed;

static_assert(sizeof(struct dcp_display_hints) == 160);
static_assert(offsetof(struct dcp_display_hints, primary_id) == 0x70);
static_assert(offsetof(struct dcp_display_hints, status) == 0x90);
static_assert(sizeof_field(struct dcp_display_hints, allocation) == 32);

/* The widest mode one desktop 26.6 DISPEXT pipe drives (firmware: "max width: 6720"). */
#define DCP_DESKTOP_PIPE_MAX_WIDTH	6720

/*
 * Peer validity by firmware endpoint (apple,dcp-endpoint, the firmware's
 * Disp_Index).  Endpoint 0 is the internal display and never a peer; the
 * external pipes merge upwards only (the T8132 rule inherited by T6050):
 * dcpext0 (1) is the primary, dcpext1 (2) its peer.  The other direction
 * pairs, but the peer's half of the picture is noise.
 */
static inline bool dcp_peer_index_allowed_26_6(u32 primary, u32 peer)
{
	return primary && peer > primary;
}

#endif
