/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DPAVSERV_EDID_H
#define APPLE_DPAVSERV_EDID_H

#include "../afkv2-codec.h"

#define DPAV_EDID_HEADER_SIZE 48U
#define EDID_LEADING_DATA_SIZE 8U
#define EDID_BLOCK_SIZE 128U
#define EDID_MAX_SIZE 32768U
#define EDID_BUF_SIZE (EDID_LEADING_DATA_SIZE + EDID_MAX_SIZE)

/* Validate only bytes actually returned by firmware, never zero-fill padding. */
static inline int dpavserv_edid_size(const void *response, size_t received,
				     size_t *edid_size)
{
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	const u8 *p = response;
	u64 used;
	size_t length, offset, i;

	if (!response || !edid_size || received < DPAV_EDID_HEADER_SIZE)
		return -EIO;
	if (afkv2_get64(p) != EDID_BUF_SIZE)
		return -EIO;
	used = afkv2_get64(p + 32);
	/*
	 * No EDID bytes: the firmware has not read the sink for this instance
	 * yet. It publishes a new instance on resume before its link is back,
	 * and reads the sink again once the link is up.
	 */
	if (!used)
		return -ENODATA;
	if (used < EDID_LEADING_DATA_SIZE + EDID_BLOCK_SIZE || used > EDID_BUF_SIZE ||
	    used > received - DPAV_EDID_HEADER_SIZE)
		return -EIO;
	length = used - EDID_LEADING_DATA_SIZE;
	p += DPAV_EDID_HEADER_SIZE + EDID_LEADING_DATA_SIZE;
	if (length % EDID_BLOCK_SIZE || memcmp(p, header, sizeof(header)) ||
	    ((size_t)p[126] + 1) * EDID_BLOCK_SIZE > length)
		return -EIO;
	/* Permit extra whole blocks for HF-EEODB, as drm_edid supports it. */
	for (offset = 0; offset < length; offset += EDID_BLOCK_SIZE) {
		u8 sum = 0;

		for (i = 0; i < EDID_BLOCK_SIZE; i++)
			sum += p[offset + i];
		if (sum)
			return -EIO;
	}
	*edid_size = length;
	return 0;
}

#endif
