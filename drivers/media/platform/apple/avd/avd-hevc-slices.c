// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include "avd-hevc-fp.h"

/*
 * GStreamer tiles the OUTPUT buffer with whole NALs: bit_size includes the
 * header and data_byte_offset is NAL-relative. FFmpeg v4l2request supplies
 * absolute data_byte_offset and bit_size describes only the entropy data.
 * Check both complete tilings, with wide arithmetic, before choosing one.
 */
int avd_hevc_slice_ranges(const struct v4l2_ctrl_hevc_slice_params *sl,
			 unsigned int count, u32 payload,
			 struct avd_hevc_slice_range *ranges)
{
	u64 nal_end = 0, data_end = 0;
	bool nal = true, absolute = true;
	unsigned int i;

	if (!count || !payload)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		u64 bytes = ((u64)sl[i].bit_size + 7) / 8;
		u64 start = sl[i].data_byte_offset;

		if (!bytes || start >= bytes || nal_end + bytes > payload)
			nal = false;
		nal_end += bytes;
		if (!bytes || start < data_end || start + bytes > payload)
			absolute = false;
		data_end = start + bytes;
	}
	/* Whole-NAL bit_size values must account for the entire buffer. */
	nal &= nal_end == payload;
	absolute &= data_end == payload;
	if (!nal && !absolute)
		return -EINVAL;

	nal_end = 0;
	for (i = 0; i < count; i++) {
		u32 bytes = ((u64)sl[i].bit_size + 7) / 8;
		u32 off = sl[i].data_byte_offset;

		/* An ambiguous description is safe only if both ranges agree. */
		if (nal && absolute && (nal_end || off))
			return -EINVAL;
		ranges[i].offset = nal ? nal_end + off : off;
		ranges[i].size = nal ? bytes - off : bytes;
		nal_end += bytes;
	}
	return 0;
}
