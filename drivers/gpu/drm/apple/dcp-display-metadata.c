// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/err.h>
#include <linux/int_log.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>

#include "dcp-display-metadata.h"
#include "parser.h"

static int dcp_metadata_timing(struct detailed_timing *timing,
			       const struct drm_display_mode *mode,
			       const struct dcp_display_metadata *metadata)
{
	struct detailed_pixel_timing *pixel = &timing->data.pixel_data;
	u32 hblank, vblank, hoff, voff, hsync, vsync;
	u32 width = metadata->width_cm * 10, height = metadata->height_cm * 10;
	u32 hflags = mode->flags & (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC);
	u32 vflags = mode->flags & (DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC);

	if (mode->clock <= 0 || mode->clock > 655350 || mode->clock % 10 ||
	    mode->hdisplay <= 0 || mode->vdisplay <= 0 ||
	    mode->hdisplay > 4095 || mode->vdisplay > 4095 ||
	    mode->htotal <= mode->hdisplay || mode->vtotal <= mode->vdisplay ||
	    mode->hsync_start < mode->hdisplay || mode->hsync_end <= mode->hsync_start ||
	    mode->hsync_end > mode->htotal || mode->vsync_start < mode->vdisplay ||
	    mode->vsync_end <= mode->vsync_start || mode->vsync_end > mode->vtotal ||
	    (hflags != DRM_MODE_FLAG_PHSYNC && hflags != DRM_MODE_FLAG_NHSYNC) ||
	    (vflags != DRM_MODE_FLAG_PVSYNC && vflags != DRM_MODE_FLAG_NVSYNC) ||
	    (mode->flags & ~(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			     DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC)))
		return -EINVAL;
	hblank = mode->htotal - mode->hdisplay;
	vblank = mode->vtotal - mode->vdisplay;
	hoff = mode->hsync_start - mode->hdisplay;
	voff = mode->vsync_start - mode->vdisplay;
	hsync = mode->hsync_end - mode->hsync_start;
	vsync = mode->vsync_end - mode->vsync_start;
	if (hblank > 4095 || vblank > 4095 || hoff > 1023 || hsync > 1023 ||
	    voff > 63 || vsync > 63 || width > 4095 || height > 4095)
		return -EINVAL;

	timing->pixel_clock = cpu_to_le16(mode->clock / 10);
	pixel->hactive_lo = mode->hdisplay;
	pixel->hblank_lo = hblank;
	pixel->hactive_hblank_hi = (mode->hdisplay >> 8) << 4 | hblank >> 8;
	pixel->vactive_lo = mode->vdisplay;
	pixel->vblank_lo = vblank;
	pixel->vactive_vblank_hi = (mode->vdisplay >> 8) << 4 | vblank >> 8;
	pixel->hsync_offset_lo = hoff;
	pixel->hsync_pulse_width_lo = hsync;
	pixel->vsync_offset_pulse_width_lo = (voff & 15) << 4 | (vsync & 15);
	pixel->hsync_vsync_offset_pulse_width_hi = (hoff >> 8) << 6 | (hsync >> 8) << 4 |
						 (voff >> 4) << 2 | vsync >> 4;
	pixel->width_mm_lo = width;
	pixel->height_mm_lo = height;
	pixel->width_height_mm_hi = (width >> 8) << 4 | height >> 8;
	pixel->misc = DRM_EDID_PT_SEPARATE_SYNC;
	if (hflags == DRM_MODE_FLAG_PHSYNC)
		pixel->misc |= DRM_EDID_PT_HSYNC_POSITIVE;
	if (vflags == DRM_MODE_FLAG_PVSYNC)
		pixel->misc |= DRM_EDID_PT_VSYNC_POSITIVE;
	return 0;
}

static int dcp_metadata_text(struct detailed_timing *timing, u8 type, const char *text)
{
	size_t length = strnlen(text, 14);
	u8 *out = timing->data.other_data.data.str.str;

	if (!length || length > 13)
		return -EINVAL;
	for (size_t i = 0; i < length; i++)
		if (text[i] < 0x20 || text[i] > 0x7e)
			return -EINVAL;
	timing->data.other_data.type = type;
	memset(out, ' ', 13);
	memcpy(out, text, length);
	if (length < 13)
		out[length] = '\n';
	return 0;
}

static s64 dcp_metadata_side(const u32 *a, const u32 *b, const u32 *point)
{
	return ((s64)b[0] - a[0]) * ((s64)point[1] - a[1]) -
	       ((s64)b[1] - a[1]) * ((s64)point[0] - a[0]);
}

/* Validate the complete colour description before changing any EDID bytes. */
static int dcp_metadata_color(struct edid *edid, const struct dcp_display_metadata *metadata)
{
	u32 coordinate[8], gamma;

	if (metadata->gamma &&
	    (metadata->gamma < 65536 || metadata->gamma > (354U << 16) / 100))
		return -EINVAL;
	for (unsigned int i = 0; i < 8; i++) {
		u32 value = i < 6 ? metadata->chromaticity[i / 2][i % 2] : metadata->white[i - 6];

		if (!value || value >= 65536)
			return -EINVAL;
		coordinate[i] = DIV_ROUND_CLOSEST(value, 64);
		if (!coordinate[i] || coordinate[i] > 1023)
			return -EINVAL;
		if (i == 7 && coordinate[6] + coordinate[7] > 1024)
			return -EINVAL;
	}
	/* Degenerate primaries or a white point outside their triangle are not usable. */
	{
		s64 area = dcp_metadata_side(coordinate, coordinate + 2, coordinate + 4);

		if (!area)
			return -EINVAL;
		for (unsigned int i = 0; i < 3; i++) {
			s64 side = dcp_metadata_side(coordinate + 2 * i,
				coordinate + 2 * ((i + 1) % 3), coordinate + 6);

			if ((area > 0 && side < 0) || (area < 0 && side > 0))
				return -EINVAL;
		}
	}
	gamma = metadata->gamma ?
		DIV_ROUND_CLOSEST(metadata->gamma * 100, 65536) - 100 : 255;
	if (metadata->gamma && gamma > 254)
		return -EINVAL;

	edid->gamma = gamma;
	edid->red_green_lo = (coordinate[0] & 3) << 6 | (coordinate[1] & 3) << 4 |
			     (coordinate[2] & 3) << 2 | (coordinate[3] & 3);
	edid->blue_white_lo = (coordinate[4] & 3) << 6 | (coordinate[5] & 3) << 4 |
			      (coordinate[6] & 3) << 2 | (coordinate[7] & 3);
	edid->red_x = coordinate[0] >> 2;
	edid->red_y = coordinate[1] >> 2;
	edid->green_x = coordinate[2] >> 2;
	edid->green_y = coordinate[3] >> 2;
	edid->blue_x = coordinate[4] >> 2;
	edid->blue_y = coordinate[5] >> 2;
	edid->white_x = coordinate[6] >> 2;
	edid->white_y = coordinate[7] >> 2;
	return 0;
}

/* Build only a base block: the authoritative firmware mode list stays separate. */
static int dcp_metadata_block(struct edid *edid, const struct dcp_display_metadata *metadata,
			      const struct drm_display_mode *mode)
{
	u32 manufacturer = 0;
	u8 checksum = 0;
	int ret;

	if (!metadata->width_cm || metadata->width_cm > 255 ||
	    !metadata->height_cm || metadata->height_cm > 255 ||
	    metadata->product > U16_MAX || metadata->week > 53 ||
	    metadata->year < 1990 || metadata->year > 2245 ||
	    metadata->gamma < 65536 || metadata->gamma > (354U << 16) / 100 ||
	    strnlen(metadata->manufacturer, 4) != 3)
		return -EINVAL;
	for (unsigned int i = 0; i < 3; i++) {
		if (metadata->manufacturer[i] < 'A' || metadata->manufacturer[i] > 'Z')
			return -EINVAL;
		manufacturer = manufacturer << 5 | (metadata->manufacturer[i] - '@');
	}

	memset(edid, 0, sizeof(*edid));
	memcpy(edid->header, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
	edid->product_id.manufacturer_name = cpu_to_be16(manufacturer);
	edid->product_id.product_code = cpu_to_le16(metadata->product);
	edid->product_id.serial_number = cpu_to_le32(metadata->serial);
	edid->product_id.week_of_manufacture = metadata->week;
	edid->product_id.year_of_manufacture = metadata->year - 1990;
	edid->version = 1;
	edid->revision = 4;
	/* No claim about connector interface, bit depth, HDR, audio or VRR. */
	edid->input = DRM_EDID_INPUT_DIGITAL;
	edid->width_cm = metadata->width_cm;
	edid->height_cm = metadata->height_cm;
	ret = dcp_metadata_color(edid, metadata);
	if (ret)
		return ret;
	edid->features = DRM_EDID_FEATURE_PREFERRED_TIMING | DRM_EDID_FEATURE_RGB;
	memset(edid->standard_timings, 1, sizeof(edid->standard_timings));
	ret = dcp_metadata_timing(&edid->detailed_timings[0], mode, metadata);
	if (!ret)
		ret = dcp_metadata_text(&edid->detailed_timings[1], EDID_DETAIL_MONITOR_NAME,
					metadata->name);
	if (!ret)
		ret = dcp_metadata_text(&edid->detailed_timings[2], EDID_DETAIL_MONITOR_STRING,
					"DCP metadata");
	if (!ret)
		ret = dcp_metadata_text(&edid->detailed_timings[3],
					metadata->serial_text[0] ? EDID_DETAIL_MONITOR_SERIAL :
					EDID_DETAIL_MONITOR_STRING,
					metadata->serial_text[0] ?
					metadata->serial_text : "FW derived");
	if (ret)
		return ret;
	for (unsigned int i = 0; i < sizeof(*edid) - 1; i++)
		checksum += ((const u8 *)edid)[i];
	edid->checksum = -checksum;
	return 0;
}

/* CTA-861.3 luminance code value: 50 * 2^(cv / 32) cd/m2. */
static u8 dcp_metadata_luminance_code(u32 luminance)
{
	s64 log;

	if (luminance <= 50U << 16)
		return 0;
	log = (s64)intlog2(luminance) - intlog2(50U << 16);
	return min(DIV_ROUND_CLOSEST_ULL(32 * log, 1U << 24), 255ULL);
}

/*
 * A CTA-861 extension carrying what an HDR compositor looks for: the
 * colorimetry and HDR static metadata data blocks.  It describes only what
 * the driver can output, an SDR or a PQ BT.2020 signal, and the sink's
 * own luminance range.
 */
static void dcp_metadata_cta(u8 *cta, const struct dcp_display_metadata *metadata)
{
	u8 *db = cta + 4, checksum = 0;

	memset(cta, 0, EDID_LENGTH);
	cta[0] = 0x02;		/* CTA extension tag */
	cta[1] = 0x03;		/* revision 3 */
	if (metadata->bt2020_rgb) {
		*db++ = 7 << 5 | 3;	/* extended tag, three bytes */
		*db++ = 0x05;		/* colorimetry data block */
		*db++ = 0x80;		/* BT.2020 RGB */
		*db++ = 0;
	}
	*db++ = 7 << 5 | (metadata->max_luminance ? 6 : 3);
	*db++ = 0x06;			/* HDR static metadata data block */
	*db++ = 0x05;		/* traditional SDR and SMPTE ST 2084 */
	*db++ = metadata->static_metadata_type1;	/* static metadata type 1 */
	if (metadata->max_luminance) {
		unsigned long square, code;

		*db++ = dcp_metadata_luminance_code(metadata->max_luminance);
		*db++ = dcp_metadata_luminance_code(metadata->max_fall);
		/* min = max * (cv / 255)^2 / 100, cv rounded to the nearest */
		square = div_u64((u64)metadata->min_luminance * 100 * 255 * 255,
				 metadata->max_luminance);
		code = int_sqrt(square);
		if (square - code * code > code)
			code++;
		*db++ = min(code, 255UL);
	}
	cta[2] = db - cta;	/* no detailed timings follow */
	for (unsigned int i = 0; i < EDID_LENGTH - 1; i++)
		checksum += cta[i];
	cta[EDID_LENGTH - 1] = -checksum;
}

/* A base block, followed by the HDR capability when the firmware reports PQ. */
static const struct drm_edid *dcp_metadata_edid_alloc(const struct edid *base,
						      const struct dcp_display_metadata *metadata)
{
	struct {
		struct edid base;
		u8 cta[EDID_LENGTH];
	} block = { .base = *base };
	const struct drm_edid *edid;
	size_t size = sizeof(block.base);

	if (metadata->pq) {
		block.base.extensions = 1;
		block.base.checksum -= 1;
		dcp_metadata_cta(block.cta, metadata);
		size = sizeof(block);
	}
	edid = drm_edid_alloc(&block, size);
	if (!edid)
		return ERR_PTR(-ENOMEM);
	if (!drm_edid_valid(edid)) {
		drm_edid_free(edid);
		return ERR_PTR(-EINVAL);
	}
	return edid;
}

const struct drm_edid *dcp_metadata_edid(const struct dcp_display_metadata *metadata,
					 const struct drm_display_mode *mode)
{
	struct edid block;
	int ret = dcp_metadata_block(&block, metadata, mode);

	if (ret)
		return ERR_PTR(ret);
	return dcp_metadata_edid_alloc(&block, metadata);
}

/*
 * This identity-only DTD is not enumerated or programmed. Keep the DCP clock,
 * active area and blanking, but fit vertical sync placement into EDID's six-bit
 * fields. Notch hiding expands the real sync pulse beyond those fields; the
 * exact syncs remain in the authoritative TimingElements catalogue.
 */
static int dcp_panel_identity_timing(struct detailed_timing *timing,
				     const struct drm_display_mode *mode,
				     const struct dcp_display_metadata *metadata)
{
	struct drm_display_mode summary = *mode;

	/* Refuse invalid source geometry before reducing the EDID summary. */
	if (mode->vdisplay <= 0 || mode->vdisplay > 4095 ||
	    mode->vsync_start < mode->vdisplay || mode->vsync_end <= mode->vsync_start ||
	    mode->vsync_end > mode->vtotal)
		return -EINVAL;
	summary.vsync_start = mode->vdisplay + min(mode->vsync_start - mode->vdisplay, 63);
	summary.vsync_end = summary.vsync_start + min(mode->vsync_end - mode->vsync_start, 63);
	return dcp_metadata_timing(timing, &summary, metadata);
}

/* EDID 1.4 range offsets extend each one-byte limit by 255. */
static int dcp_panel_range_byte(u32 limit, u8 *out, u8 *offsets, u8 flag)
{
	if (!limit || limit > 510)
		return -ERANGE;
	if (limit > 255) {
		limit -= 255;
		*offsets |= flag;
	}
	*out = limit;
	return 0;
}

/*
 * Supply one representable DCP timing and its catalog limits for EDID
 * conformity. DRM still enumerates TimingElements, never these rounded bytes.
 */
const struct drm_edid *dcp_panel_edid(const struct dcp_display_metadata *metadata,
				      const struct dcp_display_mode *modes, unsigned int count,
				      u32 width_mm, u32 height_mm)
{
	struct edid block = {};
	struct dcp_display_metadata dimensions = *metadata;
	struct detailed_non_pixel *limits = &block.detailed_timings[3].data.other_data;
	struct detailed_data_monitor_range *range = &limits->data.range;
	char serial[14], name[14];
	u32 min_v = U32_MAX, max_v = 0, min_h = U32_MAX, max_h = 0, clock = 0;
	u16 manufacturer = 0;
	u8 sum = 0;
	bool timing = false, preferred = false;
	int ret;

	if (!modes || !count || strnlen(metadata->manufacturer, 4) != 3 ||
	    metadata->product > U16_MAX || !width_mm || width_mm > 2550 ||
	    !height_mm || height_mm > 2550)
		return ERR_PTR(-EINVAL);
	for (unsigned int i = 0; i < 3; i++) {
		if (metadata->manufacturer[i] < 'A' || metadata->manufacturer[i] > 'Z')
			return ERR_PTR(-EINVAL);
		manufacturer = manufacturer << 5 | (metadata->manufacturer[i] - '@');
	}
	dimensions.width_cm = DIV_ROUND_CLOSEST(width_mm, 10);
	dimensions.height_cm = DIV_ROUND_CLOSEST(height_mm, 10);
	for (unsigned int i = 0; i < count; i++) {
		const struct dcp_display_mode *entry = &modes[i];
		struct drm_display_mode mode = entry->mode;
		struct detailed_timing candidate = {};
		u32 low, high;
		u32 pixels;

		if (mode.clock <= 0 || mode.clock > 2550000 || mode.htotal <= 0 ||
		    mode.htotal > 65535 || mode.vtotal <= 0 || mode.vtotal > 65535)
			return ERR_PTR(-EINVAL);
		pixels = (u32)mode.htotal * mode.vtotal;
		low = div_u64((u64)mode.clock * 1000, pixels);
		high = DIV_ROUND_UP_ULL((u64)mode.clock * 1000, pixels);
		if (entry->vrr && entry->min_vrr && entry->min_vrr <= entry->max_vrr) {
			low = entry->min_vrr >> 16;
			high = DIV_ROUND_UP_ULL(entry->max_vrr, 1U << 16);
		}
		min_v = min(min_v, low);
		max_v = max(max_v, high);
		min_h = min(min_h, (u32)mode.clock / mode.htotal);
		max_h = max(max_h, DIV_ROUND_UP_ULL((u32)mode.clock, mode.htotal));
		clock = max_t(u32, clock, mode.clock);
		/* The EDID base timing clock has units of 10 kHz. */
		mode.clock = DIV_ROUND_CLOSEST(mode.clock, 10) * 10;
		ret = dcp_panel_identity_timing(&candidate, &mode, &dimensions);
		if (!ret && (!timing || (!preferred && (mode.type & DRM_MODE_TYPE_PREFERRED)))) {
			struct detailed_pixel_timing *pixel = &candidate.data.pixel_data;

			/* The base size is in cm, but the DTD preserves exact panel mm. */
			pixel->width_mm_lo = width_mm;
			pixel->height_mm_lo = height_mm;
			pixel->width_height_mm_hi = (width_mm >> 8) << 4 | height_mm >> 8;
			block.detailed_timings[0] = candidate;
			timing = true;
			preferred = mode.type & DRM_MODE_TYPE_PREFERRED;
		}
	}
	if (!timing || clock > 2550000)
		return ERR_PTR(-ERANGE);

	memcpy(block.header, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
	block.product_id.manufacturer_name = cpu_to_be16(manufacturer);
	block.product_id.product_code = cpu_to_le16(metadata->product);
	block.product_id.serial_number = cpu_to_le32(metadata->serial);
	/* J616 is a 2024 model; this is not a panel manufacture date. */
	block.product_id.week_of_manufacture = 0xff;
	block.product_id.year_of_manufacture = 2024 - 1990;
	block.version = 1;
	block.revision = 4;
	block.input = DRM_EDID_INPUT_DIGITAL;
	block.width_cm = dimensions.width_cm;
	block.height_cm = dimensions.height_cm;
	block.gamma = 0xff;
	/* Nominal D65 reference white, without calibrated primaries or gamma. */
	block.white_x = 320 >> 2;
	block.white_y = 337 >> 2;
	block.blue_white_lo = (320 & 3) << 2 | (337 & 3);
	/* Missing or malformed optional colour data leaves the identity usable. */
	dcp_metadata_color(&block, metadata);
	block.features = DRM_EDID_FEATURE_PREFERRED_TIMING;
	memset(block.standard_timings, 1, sizeof(block.standard_timings));
	snprintf(name, sizeof(name), "%.3s %04x", metadata->manufacturer, metadata->product);
	ret = dcp_metadata_text(&block.detailed_timings[1], EDID_DETAIL_MONITOR_NAME,
				metadata->name[0] ? metadata->name : name);
	if (ret)
		return ERR_PTR(ret);
	if (metadata->serial)
		snprintf(serial, sizeof(serial), "%08x", metadata->serial);
	else
		snprintf(serial, sizeof(serial), "unspecified");
	ret = dcp_metadata_text(&block.detailed_timings[2], EDID_DETAIL_MONITOR_SERIAL,
				metadata->serial_text[0] ? metadata->serial_text : serial);
	if (ret)
		return ERR_PTR(ret);
	limits->type = EDID_DETAIL_MONITOR_RANGE;
	ret = dcp_panel_range_byte(min_v, &range->min_vfreq, &limits->pad2,
				   DRM_EDID_RANGE_OFFSET_MIN_VFREQ);
	ret = ret ?: dcp_panel_range_byte(max_v, &range->max_vfreq, &limits->pad2,
					 DRM_EDID_RANGE_OFFSET_MAX_VFREQ);
	ret = ret ?: dcp_panel_range_byte(min_h, &range->min_hfreq_khz, &limits->pad2,
					 DRM_EDID_RANGE_OFFSET_MIN_HFREQ);
	ret = ret ?: dcp_panel_range_byte(max_h, &range->max_hfreq_khz, &limits->pad2,
					 DRM_EDID_RANGE_OFFSET_MAX_HFREQ);
	if (ret)
		return ERR_PTR(ret);
	range->pixel_clock_mhz = DIV_ROUND_UP_ULL(clock, 10000);
	range->flags = DRM_EDID_RANGE_LIMITS_ONLY_FLAG;
	memset(&range->formula, ' ', sizeof(range->formula));
	*((u8 *)&range->formula) = '\n';
	for (unsigned int i = 0; i < sizeof(block) - 1; i++)
		sum += ((const u8 *)&block)[i];
	block.checksum = -sum;
	return dcp_metadata_edid_alloc(&block, metadata);
}
