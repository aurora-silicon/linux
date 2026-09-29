// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include "m3_dcpext_modes.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
#include <errno.h>
#include <string.h>
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
#endif

int m3_dcpext_surface_validate(u32 width, u32 height, u32 pitch,
			      u64 dva, u64 mapped_bytes)
{
	u64 bytes;

	/* Scope is the qualified external controller's linear SDR path. */
	if (!width || width > M3_DCPEXT_MAX_WIDTH || !height || height > M3_DCPEXT_MAX_HEIGHT ||
	    pitch < width * 4 || (pitch & 63) || !dva || (dva & 0x3fff))
		return -EINVAL;
	bytes = (u64)pitch * height;
	if (bytes > mapped_bytes || dva >= (1ULL << 36) ||
	    bytes > (1ULL << 36) - dva)
		return -ERANGE;
	return 0;
}

static int mode_geometry_validate(const struct m3_dcpext_mode *m, u32 *fps_16_16)
{
	u64 fps, total;

	if (!m || !fps_16_16 || !m->width ||
	    m->width > M3_DCPEXT_MAX_WIDTH || !m->height || m->height > M3_DCPEXT_MAX_HEIGHT || !m->clock_khz ||
	    m->htotal <= m->width || m->vtotal <= m->height ||
	    m->htotal > 65535 || m->vtotal > 65535)
		return -EINVAL;
	if (m->interlaced || m->doublescan)
		return -EOPNOTSUPP;
	total = (u64)m->htotal * m->vtotal;
	fps = ((u64)m->clock_khz * 1000 * 65536 + total / 2) / total;
	if (fps < 24 * 65536 || fps > 120 * 65536)
		return -ERANGE;
	*fps_16_16 = fps;
	return 0;
}

int m3_dcpext_mode_validate_link(const struct m3_dcpext_mode *m,
                               u32 payload_kbps, u32 *fps_16_16)
{
	int ret;

	if (!payload_kbps || payload_kbps > M3_DCPEXT_HBR3_4LANE_KBPS)
		return -EINVAL;
	ret = mode_geometry_validate(m, fps_16_16);
	if (ret)
		return ret;
	return (u64)m->clock_khz * 24 > payload_kbps ? -ERANGE : 0;
}

int m3_dcpext_native_mode_validate(const struct m3_dcpext_native_mode *m,
                                  u32 payload_kbps, u32 *fps_16_16)
{
	if (!m)
		return -EINVAL;
	if (!m->dsc)
		return m3_dcpext_mode_validate_link(&m->geometry, payload_kbps, fps_16_16);
	/* DSC admission is limited to the HDMI HBR3 route. The current firmware
	 * publication must explicitly require DSC and must NOT mark the color
	 * unsafe. DCP owns its encoder/PPS and the converter's DSC decompressor. */
	if (payload_kbps != M3_DCPEXT_HBR3_4LANE_KBPS)
		return -EOPNOTSUPP;
	return mode_geometry_validate(&m->geometry, fps_16_16);
}

int m3_dcpext_mode_validate(const struct m3_dcpext_mode *m, u32 *fps_16_16)
{
 return m3_dcpext_mode_validate_link(m, M3_DCPEXT_MAX_PAYLOAD_KBPS, fps_16_16);
}

int m3_dcpext_mode_select(const struct m3_dcpext_mode *m,
			 const u8 *timings, u32 count, u8 selected[24])
{
	const u8 *best = NULL;
	u64 fps, best_delta = ~(u64)0;

	int ret;
	u32 rate;
	bool ambiguous = false;

	if (!timings || !selected || count > 64)
		return -EINVAL;
	ret = m3_dcpext_mode_validate(m, &rate);
	if (ret)
		return ret;
	fps = rate;
	for (u32 i = 0; i < count; i++) {
		const u8 *r = timings + i * 24;
		u32 reported = get_unaligned_le32(r + 12);
		u64 delta = fps > reported ? fps - reported : reported - fps;

		if (get_unaligned_le32(r) != 1 ||
		    get_unaligned_le32(r + 4) != m->width ||
		    get_unaligned_le32(r + 8) != m->height ||
		    get_unaligned_le32(r + 16) || get_unaligned_le32(r + 20))
			continue;
		/* Captured DMT records use nominal integer rates: e.g. 800x600
		 * 60.316 Hz -> 60, 640x480 72.809 Hz -> 72. Prefer the closest
		 * reported rate; fractional 59.94 and 60 remain distinguishable.
		 */
		if (delta >= 65536 || delta > best_delta)
			continue;
		if (delta < best_delta)
			ambiguous = false;
		else if (best && memcmp(best, r, 24))
			ambiguous = true;
		best = r;
		best_delta = delta;
	}
	if (!best)
		return -ENODATA;
	if (ambiguous)
		return -ENOTUNIQ;
	memcpy(selected, best, 24);
	return 0;
}

/* OSSerialize tags follow the existing Asahi parser and m1n1 fw/common.py.
 * Bound every container and skip, including unknown fields. Keep this parser
 * independent of DRM so captured and malformed publications use the same code
 * in host sanitizer tests and in the driver. */
/* Every value consumes at least one four-byte tag. Bound traversal by the
 * publication size rather than an unrelated 65536-node ceiling: HDMI sinks
 * with many timing/color combinations can legitimately exceed that ceiling.
 * The byte, depth and per-container limits remain independently enforced. */
struct native_value {
	u32 type, count, begin, end;
};
struct native_blob {
	const u8 *data;
	u32 bytes;
};

static int native_value_read(const struct native_blob *b, u32 *pos,
			     u32 depth, u32 *budget, struct native_value *v)
{
	struct native_value child;
	u32 tag, children = 0, payload = 0;
	int ret;

	if (depth > 16 || !*budget || *pos > b->bytes)
		return -EINVAL;
	--*budget;
	*pos = (*pos + 3) & ~3U;
	if (*pos > b->bytes || b->bytes - *pos < 4)
		return -EINVAL;
	tag = get_unaligned_le32(b->data + *pos);
	*pos += 4;
	*v = (struct native_value) {
		.type = (tag >> 24) & 31, .count = tag & 0xffffff,
		.begin = *pos,
	};
	if (tag & 0x60000000)
		return -EINVAL;
	switch (v->type) {
	case 1:
	case 2:
		if (v->count > 4096)
			return -E2BIG;
		children = v->count * (v->type == 1 ? 2 : 1);
		break;
	case 4:
		if (v->count != 64)
			return -EINVAL;
		payload = 8;
		break;
	case 9:
	case 10:
		payload = v->count;
		break;
	case 11:
		if (v->count > 1)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	if (payload > b->bytes - *pos)
		return -EINVAL;
	*pos += payload;
	for (u32 i = 0; i < children; i++) {
		ret = native_value_read(b, pos, depth + 1, budget, &child);
		if (ret)
			return ret;
		if (v->type == 1 && !(i & 1) && child.type != 9)
			return -EINVAL;
	}
	v->end = *pos;
	return 0;
}

static int native_member(const struct native_blob *b,
			 const struct native_value *dict, const char *name,
			 struct native_value *out)
{
	struct native_value key, value;
	u32 pos = dict->begin, budget = b->bytes / 4;
	bool found = false;
	int ret;

	if (dict->type != 1)
		return -EINVAL;
	for (u32 i = 0; i < dict->count; i++) {
		ret = native_value_read(b, &pos, 0, &budget, &key);
		if (!ret)
			ret = native_value_read(b, &pos, 0, &budget, &value);
		if (ret || key.type != 9)
			return -EINVAL;
		if (key.count != strlen(name) ||
		    memcmp(b->data + key.begin, name, key.count))
			continue;
		if (found)
			return -EINVAL;
		*out = value;
		found = true;
	}
	return found ? 0 : -ENOENT;
}

static int native_uint(const struct native_blob *b, const struct native_value *d,
		       const char *name, u32 *out)
{
	struct native_value v;
	u64 number;
	int ret = native_member(b, d, name, &v);

	if (ret)
		return ret;
	if (v.type != 4)
		return -EINVAL;
	number = get_unaligned_le64(b->data + v.begin);
	if (number > 0xffffffffULL)
		return -ERANGE;
	*out = number;
	return 0;
}

static int native_bool(const struct native_blob *b, const struct native_value *d,
		       const char *name, bool *out)
{
	struct native_value v;
	int ret = native_member(b, d, name, &v);

	if (ret)
		return ret;
	if (v.type != 11)
		return -EINVAL;
	*out = v.count;
	return 0;
}

static int native_color_excluded(const struct native_blob *b,
				 const struct native_value *mode,
				 const char *name, u32 id)
{
	struct native_value array, value;
	u32 pos, budget = b->bytes / 4;
	int ret = native_member(b, mode, name, &array);

	if (ret || array.type != 2)
		return ret ?: -EINVAL;
	pos = array.begin;
	for (u32 i = 0; i < array.count; i++) {
		ret = native_value_read(b, &pos, 0, &budget, &value);
		if (ret || value.type != 4)
			return -EINVAL;
		if (get_unaligned_le64(b->data + value.begin) == id)
			return 1;
	}
	return 0;
}

static int native_color(const struct native_blob *b,
			const struct native_value *mode, u32 *id, bool allow_dsc, bool *dsc)
{
	struct native_value array, color;
	u32 pos, budget = b->bytes / 4, best_score = 0;
	bool found = false, best_rgb = false;
	int ret = native_member(b, mode, "ColorModes", &array);

	if (ret || array.type != 2)
		return -EINVAL;
	pos = array.begin;
	for (u32 i = 0; i < array.count; i++) {
		u32 candidate, score, depth, encoding, eotf, range, colorimetry;
		bool virtual, rgb, yuv;
		int compressed;

		ret = native_value_read(b, &pos, 0, &budget, &color);
		if (ret || native_bool(b, &color, "IsVirtual", &virtual))
			return -EINVAL;
		if (virtual)
			continue;
		if (native_uint(b, &color, "ID", &candidate) ||
		    native_uint(b, &color, "Score", &score) ||
		    native_uint(b, &color, "Depth", &depth) ||
		    native_uint(b, &color, "PixelEncoding", &encoding) ||
		    native_uint(b, &color, "EOTF", &eotf) ||
		    native_uint(b, &color, "Colorimetry", &colorimetry) ||
		    native_uint(b, &color, "DynamicRange", &range))
			return -EINVAL;
		/* DCP performs RGB framebuffer -> wire format conversion for A411.
		 * Enum values match the shared Asahi parser.h. Prefer RGB to retain
		 * text chroma; use BT.709 limited-range 4:2:2 only when needed. */
		rgb = !encoding && !range && (colorimetry == 10 || colorimetry == 16);
		yuv = encoding == 3 && range == 1 && colorimetry == 1;
		if (depth != 8 || eotf || (!rgb && !yuv))
			continue;
		ret = native_color_excluded(b, mode, "UnsafeColorElementIDs", candidate);
		if (ret < 0)
			return ret;
		if (ret)
			continue;
		compressed = native_color_excluded(b, mode, "DSCRequiredColorElementIDs", candidate);
		if (compressed < 0)
			return compressed;
		if (compressed && !allow_dsc)
			continue;
		if (!found || (rgb && !best_rgb) ||
		    (rgb == best_rgb && (score > best_score || (score == best_score && candidate < *id)))) {
			*id = candidate;
			*dsc = !!compressed;
			best_score = score;
			best_rgb = rgb;
			found = true;
		}
	}
	return found ? 0 : -EOPNOTSUPP;
}

static int native_dimension(const struct native_blob *b,
			    const struct native_value *mode, const char *name,
			    u32 *active, u32 *total, u32 *front, u32 *sync,
			    bool *positive, u32 *rate)
{
	struct native_value dimension;
	u32 back, polarity, repetition;
	int ret = native_member(b, mode, name, &dimension);

	if (ret || native_uint(b, &dimension, "Active", active) ||
	    native_uint(b, &dimension, "Total", total) ||
	    native_uint(b, &dimension, "FrontPorch", front) ||
	    native_uint(b, &dimension, "SyncWidth", sync) ||
	    native_uint(b, &dimension, "BackPorch", &back) ||
	    native_uint(b, &dimension, "SyncPolarity", &polarity) ||
	    native_uint(b, &dimension, "PixelRepetition", &repetition) ||
	    native_uint(b, &dimension, "PreciseSyncRate", rate))
		return -EINVAL;
	if (!*active || !*sync || !*total || *total > 65535 ||
	    (u64)*active + *front + *sync + back != *total || polarity > 1)
		return -EINVAL;
	if (repetition)
		return -EOPNOTSUPP;
	*positive = polarity;
	return 0;
}

/* 14.6 publishes IsSplit but omits pipe counts. Missing counts mean the
 * single pipe only for a non-split timing; malformed/present counts still fail. */
static int native_pipe_count(const struct native_blob *b,
                             const struct native_value *v, const char *key,
                             u32 *count)
{
 int ret = native_uint(b, v, key, count);
 if (ret == -ENOENT) { *count = 1; return 0; }
 return ret;
}

static int native_mode(const struct native_blob *b, const struct native_value *v,
		       struct m3_dcpext_native_mode *out, u32 payload_kbps, bool allow_dsc)
{
	struct m3_dcpext_mode *m = &out->geometry;
	u32 horizontal_rate, rate, pipes_h, pipes_v, ignored;
	bool virtual, interlaced, split;
	u64 clock;
	int ret;

	memset(out, 0, sizeof(*out));
	if (native_bool(b, v, "IsVirtual", &virtual))
		return -EINVAL;
	if (virtual)
		return -EOPNOTSUPP;
	if (native_bool(b, v, "IsInterlaced", &interlaced) ||
	    native_bool(b, v, "IsSplit", &split) ||
	    native_bool(b, v, "IsPreferred", &out->preferred) ||
	    native_pipe_count(b, v, "HorizontalPipeCount", &pipes_h) ||
	    native_pipe_count(b, v, "VerticalPipeCount", &pipes_v) ||
	    native_uint(b, v, "ID", &out->timing_id))
		return -EINVAL;
	if (interlaced || split || pipes_h != 1 || pipes_v != 1)
		return -EOPNOTSUPP;
	ret = native_dimension(b, v, "HorizontalAttributes", &m->width,
			       &m->htotal, &out->hfront, &out->hsync,
			       &out->hpositive, &horizontal_rate);
	if (!ret)
		ret = native_dimension(b, v, "VerticalAttributes", &m->height,
				       &m->vtotal, &out->vfront, &out->vsync,
				       &out->vpositive, &rate);
	if (ret)
		return ret;
	if (rate < 24 * 65536 || rate > 120 * 65536 ||
	    m->width > M3_DCPEXT_MAX_WIDTH || m->height > M3_DCPEXT_MAX_HEIGHT)
		return -EOPNOTSUPP;
	clock = (u64)m->htotal * m->vtotal * rate;
	m->clock_khz = (clock + 32768000) / 65536000;
	ret = native_color(b, v, &out->color_id, allow_dsc, &out->dsc);
	if (ret)
		return ret;
	ret = m3_dcpext_native_mode_validate(out, payload_kbps, &ignored);
	return ret ? -EOPNOTSUPP : 0;
}

int m3_dcpext_native_modes_parse_transport(const void *data, u32 bytes,
			       struct m3_dcpext_native_mode *modes,
			       u32 capacity, u32 *count, u32 payload_kbps, bool allow_dsc)
{
	struct native_blob blob = { .data = data, .bytes = bytes };
	struct native_value root, value;
	struct m3_dcpext_native_mode mode;
	u32 pos = 4, budget = bytes / 4, used = 0;
	int ret;

	if (!count)
		return -EINVAL;
	*count = 0;
	if (!payload_kbps || payload_kbps > M3_DCPEXT_HBR3_4LANE_KBPS ||
	    (allow_dsc && payload_kbps != M3_DCPEXT_HBR3_4LANE_KBPS) ||
	    !data || !modes || !capacity || capacity > M3_DCPEXT_NATIVE_MAX_MODES ||
	    bytes < 8 || bytes > M3_DCPEXT_MAX_PROPERTY_BYTES || get_unaligned_le32(data) != 0xd3)
		return -EINVAL;
	ret = native_value_read(&blob, &pos, 0, &budget, &root);
	if (ret || root.type != 2 || root.count > M3_DCPEXT_NATIVE_MAX_MODES ||
	    ((pos + 3) & ~3U) != bytes)
		return -EINVAL;
	pos = root.begin;
	budget = bytes / 4;
	for (u32 i = 0; i < root.count; i++) {
		ret = native_value_read(&blob, &pos, 0, &budget, &value);
		if (!ret)
			ret = native_mode(&blob, &value, &mode, payload_kbps, allow_dsc);
		if (ret == -EOPNOTSUPP)
			continue;
		if (ret)
			return ret;
		for (u32 j = 0; j < used; j++)
			if (modes[j].timing_id == mode.timing_id)
				return -EINVAL;
		if (used == capacity)
			return -ENOSPC;
		modes[used++] = mode;
	}
	*count = used;
	return 0;
}

int m3_dcpext_native_modes_parse_link(const void *data,u32 bytes,
 struct m3_dcpext_native_mode *modes,u32 capacity,u32 *count,u32 payload_kbps)
{return m3_dcpext_native_modes_parse_transport(data,bytes,modes,capacity,count,payload_kbps,false);}

int m3_dcpext_native_modes_parse(const void *data, u32 bytes,
                               struct m3_dcpext_native_mode *modes,
                               u32 capacity, u32 *count)
{
 return m3_dcpext_native_modes_parse_link(data, bytes, modes, capacity, count,
                                        M3_DCPEXT_MAX_PAYLOAD_KBPS);
}

int m3_dcpext_native_mode_select(const struct m3_dcpext_native_mode *requested,
				const struct m3_dcpext_native_mode *modes,
				u32 count, u32 *selected)
{
	const struct m3_dcpext_mode *g;
	u32 rate, best = 0, best_delta = ~0U;
	bool found = false, ambiguous = false;
	int ret;

	if (!requested || !modes || !selected || count > M3_DCPEXT_NATIVE_MAX_MODES)
		return -EINVAL;
	g = &requested->geometry;
	ret = mode_geometry_validate(g, &rate);
	if (ret)
		return ret;
	for (u32 i = 0; i < count; i++) {
		const struct m3_dcpext_native_mode *m = &modes[i];
		u32 delta, candidate_rate;

		if (m3_dcpext_native_mode_validate(m, M3_DCPEXT_HBR3_4LANE_KBPS, &candidate_rate) ||
		    g->width != m->geometry.width || g->height != m->geometry.height ||
		    g->htotal != m->geometry.htotal || g->vtotal != m->geometry.vtotal ||
		    requested->hfront != m->hfront || requested->hsync != m->hsync ||
		    requested->vfront != m->vfront || requested->vsync != m->vsync ||
		    requested->hpositive != m->hpositive || requested->vpositive != m->vpositive)
			continue;
		delta = rate > candidate_rate ? rate - candidate_rate : candidate_rate - rate;
		/* Native DMT publications round rates (800x600 says 60 rather than
		 * 60.317 Hz). Keep EDID's exact clock, match all porches/polarities,
		 * and choose the uniquely closest native rate within one Hz. */
		if (delta >= 65536 || delta > best_delta)
			continue;
		if (found && delta == best_delta)
			ambiguous = true;
		else
			ambiguous = false;
		best = i;
		best_delta = delta;
		found = true;
	}
	if (!found)
		return -ENODATA;
	if (ambiguous)
		return -ENOTUNIQ;
	*selected = best;
	return 0;
}

int m3_dcpext_native_dimensions(const void *data,u32 bytes,u32 *width_mm,u32 *height_mm)
{
 struct native_blob b={.data=data,.bytes=bytes};struct native_value root;
 u32 pos=4,budget=bytes/4,w,h;int ret;
 if(!data || bytes<8 || bytes>0x100000 || !width_mm || !height_mm || get_unaligned_le32(data)!=0xd3)return -EINVAL;
 ret=native_value_read(&b,&pos,0,&budget,&root);
 if(ret || root.type!=1 || ((pos+3)&~3U)!=bytes)return -EINVAL;
 if(native_uint(&b,&root,"MaxHorizontalImageSize",&w) ||
    native_uint(&b,&root,"MaxVerticalImageSize",&h) || !w || !h || w>255 || h>255)return -EINVAL;
 *width_mm=w*10;*height_mm=h*10;return 0;
}
