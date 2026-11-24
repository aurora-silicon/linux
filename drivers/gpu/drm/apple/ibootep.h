/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCP_IBOOTEP_H
#define _APPLE_DCP_IBOOTEP_H

#include <linux/types.h>

struct apple_dcp;

/* Read-only publication check; does not send firmware commands. */
bool ibootep_is_ready(struct apple_dcp *dcp);

/* Queue external HPD/timing/color queries; never call synchronously from AFK RX. */
int ibootep_query_modes(struct apple_dcp *dcp);

/* Explicit, one-shot external presentation, called from sleepable work outside
 * the AFK receive queue. Buffer: 3840x2160, stride 15360, bytes B,G,R,0xff.
 * Caller verifies both DART mappings and retains the buffer until reboot,
 * including after an error: commands may already have reached firmware.
 */
int ibootep_present_pattern(struct apple_dcp *dcp, u64 iova, size_t size, u32 stride);
/* Allow one more pattern request, for the retained buffer after a hotplug bounce. */
int ibootep_rearm_pattern(struct apple_dcp *dcp);

/* Submit a retained buffer without changing power/timing. Success follows
 * SwapWait for this swap, not merely the SetSwapEnd command acknowledgment.
 * Failure leaves DMA ownership uncertain: retain every submitted buffer.
 */
int ibootep_present_frame(struct apple_dcp *dcp, u64 iova, size_t size, u32 stride);

/*
 * The iBoot interface's display commands, for a DCP driven through it
 * (DCP_FIRMWARE_IBOOT) and for the T6030 external role. Sleepable callers
 * outside the AFK receive queue; the caller serializes them.
 */
struct iboot_timing_mode {
	__le32 valid;
	__le32 width;
	__le32 height;
	__le32 fps;		/* 16.16 */
	/* byte 0 bit 6: a standard mode the firmware adds on its own */
	u8 flags[8];
} __packed;

struct iboot_color_mode {
	__le32 valid;
	__le32 colorimetry;
	__le32 eotf;
	__le32 encoding;
	__le32 bpp;
	u8 pad[4];
} __packed;

int ibootep_set_power(struct apple_dcp *dcp, bool on);
/* 1 with a sink, 0 without; the counts of its timing and colour modes. */
int ibootep_get_hpd(struct apple_dcp *dcp, u32 *timings, u32 *colors);
/* The sink's mode lists, newly allocated; the caller frees them. */
int ibootep_get_timing_modes(struct apple_dcp *dcp,
			     struct iboot_timing_mode **modes,
			     unsigned int *count);
int ibootep_get_color_modes(struct apple_dcp *dcp,
			    struct iboot_color_mode **modes, unsigned int *count);
int ibootep_set_mode(struct apple_dcp *dcp, const struct iboot_timing_mode *timing,
		     const struct iboot_color_mode *color);
/* SET_SURFACE: scan out one linear BGRA plane at @iova. */
int ibootep_set_surface(struct apple_dcp *dcp, u64 iova, u32 width, u32 height,
			u32 stride);

#endif
