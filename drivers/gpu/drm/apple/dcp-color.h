/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Output colour mode selection for Apple DCP external displays.
 *
 * Maps standard DRM connector properties (HDR_OUTPUT_METADATA EOTF,
 * Colorspace, color format, Broadcast RGB, max bpc) onto the per-timing
 * ColorModes catalogue reported by DCP firmware.
 */
#ifndef __APPLE_DCP_COLOR_H__
#define __APPLE_DCP_COLOR_H__

#include <linux/errno.h>
#include <linux/hdmi.h>
#include <linux/types.h>
#include <drm/drm_connector.h>

#include "parser.h"

struct dcp_color_request {
	u8 eotf;		/* enum hdmi_eotf */
	u8 colorspace;		/* enum drm_colorspace */
	u8 color_format;	/* enum drm_connector_color_format */
	u8 broadcast_rgb;	/* enum drm_hdmi_broadcast_rgb */
	u8 max_bpc;		/* 0 (uncapped) or 8..12 */
	bool allow_hdr;
	bool allow_dsc;
	bool allow_unsafe_sdr;
	bool default_rgb;
};

static inline bool dcp_color_request_is_default(const struct dcp_color_request *req)
{
	return req->color_format == DRM_CONNECTOR_COLOR_FORMAT_AUTO &&
	       req->broadcast_rgb == DRM_HDMI_BROADCAST_RGB_AUTO &&
	       (!req->max_bpc || req->max_bpc >= 12);
}

static inline bool dcp_color_format_is_ycbcr422(u8 format)
{
	return format == DCP_COLOR_FORMAT_YCBCR422 ||
	       format == DCP_COLOR_FORMAT_YCBCR422_DP ||
	       format == DCP_COLOR_FORMAT_YCBCR422_HDMI;
}

static inline u8 dcp_color_entry_format(const struct dcp_color_entry *e)
{
	if (e->flags & DCP_COLOR_FLAG_DOWNSTREAM_420)
		return DCP_COLOR_FORMAT_YCBCR420;
	return e->format;
}

static inline u8 dcp_color_wire_bpp(u8 format, u8 depth)
{
	if (format == DCP_COLOR_FORMAT_YCBCR420)
		return (depth * 3) / 2;
	if (dcp_color_format_is_ycbcr422(format))
		return depth * 2;
	return depth * 3;
}

static inline bool dcp_color_format_matches(u8 fmt, u8 req_fmt)
{
	switch (req_fmt) {
	case DRM_CONNECTOR_COLOR_FORMAT_AUTO:
		return fmt == DCP_COLOR_FORMAT_RGB ||
		       fmt == DCP_COLOR_FORMAT_YCBCR444 ||
		       dcp_color_format_is_ycbcr422(fmt) ||
		       fmt == DCP_COLOR_FORMAT_YCBCR420;
	case DRM_CONNECTOR_COLOR_FORMAT_RGB444:
		return fmt == DCP_COLOR_FORMAT_RGB;
	case DRM_CONNECTOR_COLOR_FORMAT_YCBCR444:
		return fmt == DCP_COLOR_FORMAT_YCBCR444;
	case DRM_CONNECTOR_COLOR_FORMAT_YCBCR422:
		return dcp_color_format_is_ycbcr422(fmt);
	case DRM_CONNECTOR_COLOR_FORMAT_YCBCR420:
		return fmt == DCP_COLOR_FORMAT_YCBCR420;
	default:
		return false;
	}
}

static inline bool dcp_color_eotf_matches(u8 eotf, const struct dcp_color_request *req)
{
	switch (req->eotf) {
	case HDMI_EOTF_TRADITIONAL_GAMMA_SDR:
		return eotf == DCP_EOTF_SDR_GAMMA;
	case HDMI_EOTF_TRADITIONAL_GAMMA_HDR:
		return req->allow_hdr && eotf == DCP_EOTF_HDR_GAMMA;
	case HDMI_EOTF_SMPTE_ST2084:
		return req->allow_hdr && eotf == DCP_EOTF_ST_2084;
	case HDMI_EOTF_BT_2100_HLG:
		return req->allow_hdr && eotf == DCP_EOTF_BT_2100;
	default:
		return false;
	}
}

static inline bool dcp_color_range_matches(u8 range, u8 broadcast_rgb)
{
	switch (broadcast_rgb) {
	case DRM_HDMI_BROADCAST_RGB_AUTO:
		return true;
	case DRM_HDMI_BROADCAST_RGB_FULL:
		return range == DCP_COLOR_YCBCR_RANGE_FULL;
	case DRM_HDMI_BROADCAST_RGB_LIMITED:
		return range == DCP_COLOR_YCBCR_RANGE_LIMITED;
	default:
		return false;
	}
}

static inline bool dcp_color_space_matches(u8 colorimetry,
					   const struct dcp_color_request *req)
{
	bool hdr = req->eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR;

	switch (req->colorspace) {
	case DRM_MODE_COLORIMETRY_DEFAULT:
		if (hdr)
			return colorimetry == DCP_COLORIMETRY_BT2020_RGB ||
			       colorimetry == DCP_COLORIMETRY_BT2020_YCC ||
			       colorimetry == DCP_COLORIMETRY_BT2020_CYCC;
		return true;
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
		if (colorimetry == DCP_COLORIMETRY_BT2020_RGB)
			return true;
		/*
		 * Compositors (e.g. Mutter) request BT2020_RGB with AUTO format
		 * for HDR even on timings where only YCbCr BT.2020 is safe.
		 */
		return hdr &&
		       req->color_format == DRM_CONNECTOR_COLOR_FORMAT_AUTO &&
		       colorimetry == DCP_COLORIMETRY_BT2020_YCC;
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
		return colorimetry == DCP_COLORIMETRY_BT2020_YCC;
	case DRM_MODE_COLORIMETRY_BT2020_CYCC:
		return colorimetry == DCP_COLORIMETRY_BT2020_CYCC;
	case DRM_MODE_COLORIMETRY_BT709_YCC:
		return colorimetry == DCP_COLORIMETRY_BT709;
	case DRM_MODE_COLORIMETRY_SMPTE_170M_YCC:
	case DRM_MODE_COLORIMETRY_BT601_YCC:
		return colorimetry == DCP_COLORIMETRY_BT601;
	case DRM_MODE_COLORIMETRY_XVYCC_601:
		return colorimetry == DCP_COLORIMETRY_XVYCC_601;
	case DRM_MODE_COLORIMETRY_XVYCC_709:
		return colorimetry == DCP_COLORIMETRY_XVYCC_709;
	case DRM_MODE_COLORIMETRY_SYCC_601:
		return colorimetry == DCP_COLORIMETRY_SYCC_601;
	case DRM_MODE_COLORIMETRY_OPYCC_601:
		return colorimetry == DCP_COLORIMETRY_ADOBE_YCC_601;
	case DRM_MODE_COLORIMETRY_OPRGB:
		return colorimetry == DCP_COLORIMETRY_ADOBE_RGB;
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65:
		return colorimetry == DCP_COLORIMETRY_DCI_P3_RGB_D65;
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER:
		return colorimetry == DCP_COLORIMETRY_DCI_P3_RGB_THEATER;
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED:
		return colorimetry == DCP_COLORIMETRY_SCRGB_FIXED;
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT:
		return colorimetry == DCP_COLORIMETRY_SCRGB;
	default:
		return false;
	}
}

static inline u32 dcp_color_entry_rank(const struct dcp_color_entry *e,
				       const struct dcp_color_request *req)
{
	u32 fmt_rank, space_rank, depth_rank, range_rank, dsc_rank;
	bool hdr = req->eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
	u8 fmt = dcp_color_entry_format(e);

	switch (fmt) {
	case DCP_COLOR_FORMAT_RGB:
		fmt_rank = 6;
		break;
	case DCP_COLOR_FORMAT_YCBCR444:
		fmt_rank = 5;
		break;
	case DCP_COLOR_FORMAT_YCBCR422:
		fmt_rank = 4;
		break;
	case DCP_COLOR_FORMAT_YCBCR422_DP:
		fmt_rank = 3;
		break;
	case DCP_COLOR_FORMAT_YCBCR422_HDMI:
		fmt_rank = 2;
		break;
	case DCP_COLOR_FORMAT_YCBCR420:
		fmt_rank = 1;
		break;
	default:
		fmt_rank = 0;
		break;
	}

	if (hdr) {
		if (e->colorimetry == DCP_COLORIMETRY_BT2020_RGB)
			space_rank = 3;
		else if (e->colorimetry == DCP_COLORIMETRY_BT2020_YCC)
			space_rank = 2;
		else
			space_rank = 1;
	} else if (req->colorspace == DRM_MODE_COLORIMETRY_DEFAULT) {
		if (fmt == DCP_COLOR_FORMAT_RGB) {
			if (req->default_rgb && e->colorimetry == DCP_COLORIMETRY_RGB)
				space_rank = 4;
			else if (e->colorimetry == DCP_COLORIMETRY_SRGB ||
				 e->colorimetry == DCP_COLORIMETRY_BT709)
				space_rank = 3;
			else if (e->colorimetry == DCP_COLORIMETRY_RGB)
				space_rank = 2;
			else
				space_rank = 1;
		} else {
			if (e->colorimetry == DCP_COLORIMETRY_BT709)
				space_rank = 3;
			else if (e->colorimetry == DCP_COLORIMETRY_BT601)
				space_rank = 2;
			else
				space_rank = 1;
		}
	} else {
		space_rank = 1;
	}

	if (req->eotf == HDMI_EOTF_SMPTE_ST2084 &&
	    req->color_format == DRM_CONNECTOR_COLOR_FORMAT_AUTO) {
		/* Match pq_mode_rank(): 10 bpc fits HDR framebuffers directly. */
		depth_rank = e->depth == 10 ? 3 : (e->depth == 12 ? 2 : 1);
	} else {
		depth_rank = e->depth;
	}

	if (fmt == DCP_COLOR_FORMAT_RGB)
		range_rank = e->range == DCP_COLOR_YCBCR_RANGE_FULL ? 2 : 1;
	else
		range_rank = e->range == DCP_COLOR_YCBCR_RANGE_LIMITED ? 2 : 1;

	dsc_rank = (e->flags & DCP_COLOR_FLAG_DSC) ? 0 : 1;

	return (fmt_rank << 16) | (space_rank << 12) | (depth_rank << 6) |
	       (range_rank << 2) | dsc_rank;
}

static inline void dcp_color_mode_from_entry(struct dcp_color_mode *out,
					     const struct dcp_color_entry *e)
{
	out->score = 0;
	out->id = e->id;
	out->eotf = e->eotf;
	out->format = dcp_color_entry_format(e);
	out->colorimetry = e->colorimetry;
	out->range = e->range;
	out->depth = e->depth;
}

static inline int dcp_default_sdr_color(const struct dcp_display_mode *mode,
					struct dcp_color_mode *out)
{
	const struct dcp_color_mode *cmode = NULL;
	unsigned int i;

	if (mode->color_mode_id == mode->sdr_rgb.id && mode->sdr_rgb.score >= 0)
		cmode = &mode->sdr_rgb;
	else if (mode->color_mode_id == mode->sdr_444.id && mode->sdr_444.score >= 0)
		cmode = &mode->sdr_444;
	else if (mode->color_mode_id == mode->sdr.id && mode->sdr.score >= 0)
		cmode = &mode->sdr;
	else if (mode->color_mode_id == mode->best.id && mode->best.score >= 0)
		cmode = &mode->best;

	if (cmode) {
		*out = *cmode;
		for (i = 0; i < mode->nr_colors; i++) {
			if (mode->colors[i].id == out->id) {
				out->format = dcp_color_entry_format(&mode->colors[i]);
				break;
			}
		}
		return 0;
	}
	return -EINVAL;
}

static inline int dcp_select_color_mode(const struct dcp_display_mode *mode,
					const struct dcp_color_request *req,
					struct dcp_color_mode *out)
{
	u8 max_bpc = req->max_bpc ? req->max_bpc : 12;
	bool hdr = req->eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
	unsigned int pass, max_pass, i;
	const struct dcp_color_entry *best = NULL;
	u32 best_rank = 0;

	if (dcp_color_request_is_default(req)) {
		if (req->eotf == HDMI_EOTF_TRADITIONAL_GAMMA_SDR &&
		    req->colorspace == DRM_MODE_COLORIMETRY_DEFAULT)
			return dcp_default_sdr_color(mode, out);
		if (req->eotf == HDMI_EOTF_SMPTE_ST2084 &&
		    (req->colorspace == DRM_MODE_COLORIMETRY_DEFAULT ||
		     req->colorspace == DRM_MODE_COLORIMETRY_BT2020_RGB)) {
			if (!req->allow_hdr || mode->pq.score < 0)
				return -EINVAL;
			*out = mode->pq;
			return 0;
		}
	}

	if (hdr && !req->allow_hdr)
		return -EINVAL;

	max_pass = (req->allow_unsafe_sdr && !mode->sdr_usable && mode->sdr_unsafe) ? 2 : 1;
	for (pass = 0; pass < max_pass && !best; pass++) {
		bool allow_unsafe = pass > 0;

		for (i = 0; i < mode->nr_colors; i++) {
			const struct dcp_color_entry *e = &mode->colors[i];
			u8 fmt = dcp_color_entry_format(e);
			u32 rank;

			if ((e->flags & DCP_COLOR_FLAG_UNSAFE) && !allow_unsafe)
				continue;
			if ((e->flags & DCP_COLOR_FLAG_DSC) && hdr &&
			    (!req->allow_dsc || allow_unsafe))
				continue;
			if (allow_unsafe && hdr && mode->sdr_unsafe_max_bpp &&
			    dcp_color_wire_bpp(fmt, e->depth) > mode->sdr_unsafe_max_bpp)
				continue;
			if (e->depth > max_bpc)
				continue;
			if (!dcp_color_eotf_matches(e->eotf, req))
				continue;
			if (!dcp_color_format_matches(fmt, req->color_format))
				continue;
			if (!dcp_color_range_matches(e->range, req->broadcast_rgb))
				continue;
			if (!dcp_color_space_matches(e->colorimetry, req))
				continue;
			rank = dcp_color_entry_rank(e, req);
			/* Entries are sorted by descending firmware score: keep first on ties. */
			if (!best || rank > best_rank) {
				best = e;
				best_rank = rank;
			}
		}
	}

	if (!best)
		return -EINVAL;
	dcp_color_mode_from_entry(out, best);
	return 0;
}

#endif /* __APPLE_DCP_COLOR_H__ */
