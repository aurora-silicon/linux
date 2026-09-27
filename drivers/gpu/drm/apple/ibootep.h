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

#endif
