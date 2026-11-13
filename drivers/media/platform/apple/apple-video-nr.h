/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The Apple memory-to-memory engines (AVD, AVE, JPEG, ProRes, scaler)
 * register their video nodes from APPLE_M2M_VIDEO_NR up. The low numbers
 * stay with capture devices, so the camera (apple-isp asks for video0)
 * keeps /dev/video0 whichever driver probes first.
 */
#ifndef _APPLE_VIDEO_NR_H
#define _APPLE_VIDEO_NR_H

#define APPLE_M2M_VIDEO_NR	10

#endif /* _APPLE_VIDEO_NR_H */
