/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_SDR_EDID_H
#define APPLE_DCP_SDR_EDID_H

#include <linux/types.h>
#include <linux/string.h>

#define DCP_EDID_BLOCK_SIZE 128
#define DCP_EDID_EXT_COUNT 126
#define DCP_EDID_CHECKSUM 127
#define DCP_EDID_CHROMATICITY 25
#define DCP_EDID_CHROMATICITY_SIZE 10
#define DCP_EDID_CTA 0x02
#define DCP_CTA_AUDIO 1
#define DCP_CTA_VIDEO 2
#define DCP_CTA_SPEAKERS 4

static inline void dcp_edid_checksum(u8 *block)
{
	u8 sum = 0;
	unsigned int i;

	for (i = 0; i < DCP_EDID_CHECKSUM; i++)
		sum += block[i];
	block[DCP_EDID_CHECKSUM] = -sum;
}

/*
 * The native26 primary accepts XR24 only, with an sRGB output contract.
 * Keep sink identity, timings and CTA audio, but do not export sink colour
 * capabilities as pipeline capabilities. The raw firmware EDID stays cached.
 * Unknown extensions (including DisplayID colour blocks) are not a proven
 * part of that contract. Modes continue to come from the firmware catalogue.
 * @size is the validated, whole-block allocation size; the result fits in it.
 */
static inline size_t dcp_edid_sdr(u8 *edid, size_t size)
{
	size_t offset, output = DCP_EDID_BLOCK_SIZE;
	u8 count;

	if (size < DCP_EDID_BLOCK_SIZE || size % DCP_EDID_BLOCK_SIZE)
		return 0;
	count = edid[DCP_EDID_EXT_COUNT];
	if ((size_t)(count + 1) * DCP_EDID_BLOCK_SIZE > size)
		return 0;
	memset(edid + DCP_EDID_CHROMATICITY, 0, DCP_EDID_CHROMATICITY_SIZE);
	/* EDID 1.4: eight bits per colour; do not inherit sink deep colour. */
	if (edid[18] == 1 && edid[19] >= 4 && (edid[20] & 0x80))
		edid[20] = (edid[20] & ~0x70) | 0x20;
	for (offset = DCP_EDID_BLOCK_SIZE;
	     offset < (size_t)(count + 1) * DCP_EDID_BLOCK_SIZE;
	     offset += DCP_EDID_BLOCK_SIZE) {
		u8 block[DCP_EDID_BLOCK_SIZE] = { DCP_EDID_CTA, 3 };
		const u8 *src = edid + offset;
		unsigned int pos, end = src[2], dest = 4;

		if (src[0] != DCP_EDID_CTA || end < 4 || end > 127)
			continue;
		/* Keep basic audio and native DTD count, but no YCbCr flags. */
		block[3] = src[3] & 0x4f;
		for (pos = 4; pos < end;) {
			unsigned int length = (src[pos] & 0x1f) + 1;
			unsigned int tag = src[pos] >> 5;

			if (pos + length > end)
				break;
			/* Vendor/extended blocks can describe HDR or wide gamut. */
			if (tag == DCP_CTA_AUDIO || tag == DCP_CTA_VIDEO ||
			    tag == DCP_CTA_SPEAKERS) {
				memcpy(block + dest, src + pos, length);
				dest += length;
			}
			pos += length;
		}
		block[2] = dest;
		memcpy(block + dest, src + end, DCP_EDID_CHECKSUM - end);
		dcp_edid_checksum(block);
		memcpy(edid + output, block, sizeof(block));
		output += sizeof(block);
	}
	edid[DCP_EDID_EXT_COUNT] = output / DCP_EDID_BLOCK_SIZE - 1;
	dcp_edid_checksum(edid);
	return output;
}

#endif
