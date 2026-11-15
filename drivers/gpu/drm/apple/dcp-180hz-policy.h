/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_180HZ_POLICY_H
#define APPLE_DCP_180HZ_POLICY_H

#include <linux/types.h>
#include <linux/string.h>
#include <drm/drm_modes.h>

/* Only this verified uncompressed configuration is admitted. */
static inline bool dcp_180hz_payload(u32 rate, u32 lanes, u32 clock)
{
	return rate == 0x1e && lanes == 2 && clock == 414090 &&
		(u64)clock * 24 <= (u64)8100000 * lanes * 8 / 10;
}

static inline bool dcp_180hz_dtd(const u8 *d, const struct drm_display_mode *m)
{
	int ha = d[2] | (d[4] & 0xf0) << 4;
	int hb = d[3] | (d[4] & 0x0f) << 8;
	int va = d[5] | (d[7] & 0xf0) << 4;
	int vb = d[6] | (d[7] & 0x0f) << 8;
	int hf = d[8] | (d[11] & 0xc0) << 2;
	int hs = d[9] | (d[11] & 0x30) << 4;
	int vf = (d[10] >> 4) | (d[11] & 0x0c) << 2;
	int vs = (d[10] & 0x0f) | (d[11] & 0x03) << 4;
	u32 flags = (d[17] & 2 ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
		    (d[17] & 4 ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);

	return ((d[0] | d[1] << 8) * 10 == m->clock) && m->clock == 414090 &&
		(d[17] & 0x98) == 0x18 && ha == 1920 && va == 1080 &&
		ha + hb == 2080 && va + vb == 1106 &&
		m->hdisplay == ha && m->htotal == ha + hb &&
		m->hsync_start == ha + hf && m->hsync_end == ha + hf + hs &&
		m->vdisplay == va && m->vtotal == va + vb &&
		m->vsync_start == va + vf && m->vsync_end == va + vf + vs &&
		m->flags == flags;
}

/* Bounded actual EDID only; no serial field is consulted. */
static inline bool dcp_180hz_edid(const u8 *p, size_t len,
				 const struct drm_display_mode *mode)
{
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	bool name = false, timing = false, forum = false;
	unsigned int names = 0, i, off, end;

	if (!p || !mode || len != 256 || memcmp(p, header, 8) || p[126] != 1 ||
	    p[8] != 0x04 || p[9] != 0x72 || p[10] != 0x4f || p[11] != 0x0b ||
	    p[18] != 1 || p[19] < 3 || p[19] > 4 || !(p[20] & 0x80))
		return false;
	for (off = 0; off < len; off += 128) {
		u8 sum = 0;

		for (i = 0; i < 128; i++)
			sum += p[off + i];
		if (sum)
			return false;
	}
	for (off = 54; off + 18 <= 126; off += 18) {
		const u8 *d = p + off;

		if (d[0] || d[1]) {
			timing |= dcp_180hz_dtd(d, mode);
		} else if (!d[2] && d[3] == 0xfc && !d[4]) {
			unsigned int n = 13;

			names++;
			while (n && (d[5 + n - 1] == ' ' || d[5 + n - 1] == '\n' ||
				     d[5 + n - 1] == 0))
				n--;
			name = (n == 8 && !memcmp(d + 5, "QG270 S3", 8)) ||
			       (n == 7 && !memcmp(d + 5, "QG270S3", 7));
		}
	}
	p += 128;
	if (p[0] != 2 || p[1] < 3 || p[2] < 4 || p[2] > 127)
		return false;
	end = p[2];
	for (off = 4; off < end;) {
		const u8 *db = p + off;
		unsigned int size = (db[0] & 31) + 1;

		if (size > end - off)
			return false;
		if ((db[0] >> 5) == 3 && size >= 4 &&
		    db[1] == 0xd8 && db[2] == 0x5d && db[3] == 0xc4) {
			/* HF-VSDB overrides the older HDMI VSDB clock, as drm_edid does. */
			if (forum || size < 8 || db[4] != 1 || !(db[6] & 0x80) ||
			    (u32)db[5] * 5000 < (u32)mode->clock)
				return false;
			forum = true;
		}
		off += size;
	}
	for (off = end; off + 18 <= 127; off += 18)
		if (p[off] || p[off + 1])
			timing |= dcp_180hz_dtd(p + off, mode);
	return name && names == 1 && timing && forum;
}

#endif
