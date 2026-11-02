/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_JPEG_HW_H
#define APPLE_JPEG_HW_H

#include <linux/limits.h>
#include <linux/types.h>

/* Public MIT-licensed m1n1 register map: proxyclient/m1n1/hw/jpeg.py. */
#define APPLE_JPEG_HWREV		0x188
#define APPLE_JPEG_REG_SIZE	0x4000
#define APPLE_JPEG_DMA_BASE	0x10000000000ULL
#define APPLE_JPEG_DMA_SIZE	0x100000000ULL

struct apple_jpeg_variant {
	const u32 *hwrevs;
	size_t num_hwrevs;
};

/*
 * Empty until each SoC's revision and reset/IRQ behaviour are qualified.
 * Neither an ADT hw-type nor m1n1's M1 Max revision qualifies another SoC.
 */
static const struct apple_jpeg_variant apple_jpeg_t6041 = { };
static const struct apple_jpeg_variant apple_jpeg_t8140 = { };

static inline bool
apple_jpeg_hwrev_supported(const struct apple_jpeg_variant *variant, u32 hwrev)
{
	size_t i;

	if (!variant || !variant->hwrevs || !hwrev || hwrev == U32_MAX)
		return false;

	for (i = 0; i < variant->num_hwrevs; i++)
		if (variant->hwrevs[i] == hwrev)
			return true;

	return false;
}

#endif /* APPLE_JPEG_HW_H */
