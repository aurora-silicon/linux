/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef __APPLE_IOMFB_V14_7_BOARD_H__
#define __APPLE_IOMFB_V14_7_BOARD_H__

#include <linux/types.h>

/*
 * What differs between the internal panels the 14.x IOMFB drives. A record is
 * selected by the DCP's compatible and the compatible of its panel node, as
 * the device tree describes the hardware, never by the machine. The T6030
 * record holds the values the IOMFB code was written and tested with (J514S,
 * J516S panels); another record enables only what was tested with its panel
 * and firmware image, and anything else fails closed. External processors
 * have no board record.
 */
struct dcp_v14_board {
	/* In log messages. */
	const char *name;
	/* The debugfs status file. */
	const char *debugfs;
	const char *dcp_compatible;
	/* The compatible of the DCP's "panel" child; NULL for any panel. */
	const char *panel;
	/* The firmware image whose layouts were tested on this board. */
	const char *firmware_uuid;
	/* Set to <1> by the boot loader on the DCP, display and PIODMA nodes. */
	const char *handoff;
	/* Native panel size, notch rows included; 0: any, from the boot framebuffer. */
	u32 panel_width, panel_height;
	/* The panel may have a 120 Hz timing besides 60 Hz. */
	bool promotion;
	/*
	 * Log what the first boots of a new board are judged by, not only with
	 * debugging on: every panel timing the firmware offers, and the display
	 * clock it is told.
	 */
	bool log_bringup;
	/*
	 * The colour matrix setter and getter of this board's firmware image;
	 * 0: the matrix is not sent. Method numbers differ between firmware
	 * releases, so they are only set where tested.
	 */
	u32 ctm_set, ctm_get;
	/*
	 * The panel's swaps carry the constant timestamp
	 * DCP_V14_SWAP_TIMESTAMP_VALUE in slots 0, 1 and 6. With zero
	 * timestamps this board's firmware completes at most one swap per
	 * 16.7 ms, so a 120 Hz panel presents at 60 fps.
	 */
	bool swap_timestamps;
	/* This panel and firmware image take a level block in their swaps. */
	bool panel_nits;
};

/* Whether the DCP node or its panel node (as @ctx gives them) has @compat. */
typedef bool (*dcp_v14_compat_fn)(const void *ctx, const char *compat);

/*
 * The board record of a DCP. NULL if there is none: *@dcp_known then says
 * whether a record names the DCP's compatible (but no record is for this
 * panel) or none does (not an internal 14.x DCP).
 */
const struct dcp_v14_board *dcp_v14_board_select(dcp_v14_compat_fn dcp_is,
						 dcp_v14_compat_fn panel_is,
						 const void *ctx, bool *dcp_known);

/*
 * Whether @board runs a DCP with the firmware image @uuid (NULL: none given):
 * only the image whose IOMFB layouts were validated for it.
 */
bool dcp_v14_board_admits(const struct dcp_v14_board *board, const char *uuid);

/*
 * Whether a swap carries timestamps: only the internal panel of a board whose
 * record asks for them. External processors (@external, no record) and every
 * other board send zero timestamps.
 */
bool dcp_v14_board_stamps_swaps(const struct dcp_v14_board *board, bool external);

#endif /* __APPLE_IOMFB_V14_7_BOARD_H__ */
