/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCPEXT_MODE_H__
#define __APPLE_DCPEXT_MODE_H__
/* Focused 4K candidate: keep geometry consistent across firmware, DMA and KMS.
 * The separately retained 1080p kernel remains the working fallback.
 */
#define DCPEXT_WIDTH 3840
#define DCPEXT_HEIGHT 2160
#define DCPEXT_STRIDE (DCPEXT_WIDTH * 4)
#define DCPEXT_MAX_BUFFER_SIZE (64U * 1024 * 1024)
#endif
