// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/string.h>
#include <linux/slab.h>

#if IS_ENABLED(CONFIG_DRM_APPLE_NEO_AUDIO)
#include <sound/pcm.h> // for sound format masks
#endif

#include "parser.h"
#include "trace.h"

#define DCP_PARSE_HEADER 0xd3

enum neo_dcp_parse_type {
	DCP_TYPE_DICTIONARY = 1,
	DCP_TYPE_ARRAY = 2,
	DCP_TYPE_INT64 = 4,
	DCP_TYPE_STRING = 9,
	DCP_TYPE_BLOB = 10,
	DCP_TYPE_BOOL = 11
};

struct neo_dcp_parse_tag {
	unsigned int size : 24;
	enum neo_dcp_parse_type type : 5;
	unsigned int padding : 2;
	bool last : 1;
} __packed;

static const void *neo_parse_bytes(struct neo_dcp_parse_ctx *ctx, size_t count)
{
	const void *ptr;

	if (ctx->pos > ctx->len || count > ctx->len - ctx->pos)
		return ERR_PTR(-EINVAL);

	ptr = ctx->blob + ctx->pos;
	ctx->pos += count;
	return ptr;
}

static const u32 *neo_parse_u32(struct neo_dcp_parse_ctx *ctx)
{
	return neo_parse_bytes(ctx, sizeof(u32));
}

static const struct neo_dcp_parse_tag *neo_parse_tag(struct neo_dcp_parse_ctx *ctx)
{
	const struct neo_dcp_parse_tag *tag;

	/* Align to 32-bits */
	if (ctx->pos > U32_MAX - 3)
		return ERR_PTR(-EINVAL);
	ctx->pos = round_up(ctx->pos, 4);

	tag = neo_parse_bytes(ctx, sizeof(struct neo_dcp_parse_tag));

	if (IS_ERR(tag))
		return tag;

	if (tag->padding)
		return ERR_PTR(-EINVAL);

	return tag;
}

static const struct neo_dcp_parse_tag *neo_parse_tag_of_type(struct neo_dcp_parse_ctx *ctx,
					       enum neo_dcp_parse_type type)
{
	const struct neo_dcp_parse_tag *tag = neo_parse_tag(ctx);

	if (IS_ERR(tag))
		return tag;

	if (tag->type != type)
		return ERR_PTR(-EINVAL);

	return tag;
}

/* Bound stack use when ignoring nested firmware dictionaries. */
#define DCP_PARSE_MAX_DEPTH 32

static int skip_value(struct neo_dcp_parse_ctx *handle, unsigned int depth)
{
	const struct neo_dcp_parse_tag *tag;
	const void *data;
	unsigned int count, i;
	int ret;

	if (depth >= DCP_PARSE_MAX_DEPTH)
		return -EINVAL;

	tag = neo_parse_tag(handle);
	if (IS_ERR(tag))
		return PTR_ERR(tag);

	switch (tag->type) {
	case DCP_TYPE_DICTIONARY:
	case DCP_TYPE_ARRAY:
		count = tag->size;
		if (tag->type == DCP_TYPE_DICTIONARY)
			count *= 2;

		for (i = 0; i < count; ++i) {
			ret = skip_value(handle, depth + 1);
			if (ret)
				return ret;
		}
		return 0;

	case DCP_TYPE_INT64:
		data = neo_parse_bytes(handle, sizeof(s64));
		break;

	case DCP_TYPE_STRING:
	case DCP_TYPE_BLOB:
		data = neo_parse_bytes(handle, tag->size);
		break;

	case DCP_TYPE_BOOL:
		return 0;

	default:
		return -EINVAL;
	}

	return IS_ERR(data) ? PTR_ERR(data) : 0;
}

static int skip(struct neo_dcp_parse_ctx *handle)
{
	return skip_value(handle, 0);
}

#if IS_ENABLED(CONFIG_DRM_APPLE_NEO_AUDIO)
static int skip_pair(struct neo_dcp_parse_ctx *handle)
{
	int ret;

	ret = skip(handle);
	if (ret)
		return ret;

	return skip(handle);
}

static bool consume_string(struct neo_dcp_parse_ctx *ctx, const char *specimen)
{
	struct neo_dcp_parse_ctx probe = *ctx;
	const struct neo_dcp_parse_tag *tag;
	const char *key;

	tag = neo_parse_tag_of_type(&probe, DCP_TYPE_STRING);
	if (IS_ERR(tag) || tag->size != strlen(specimen))
		return false;

	key = neo_parse_bytes(&probe, tag->size);
	if (IS_ERR(key) || memcmp(key, specimen, tag->size))
		return false;

	*ctx = probe;
	return true;
}
#endif

/* Caller must free the result */
static char *neo_parse_string(struct neo_dcp_parse_ctx *handle)
{
	const struct neo_dcp_parse_tag *tag = neo_parse_tag_of_type(handle, DCP_TYPE_STRING);
	const char *in;
	char *out;

	if (IS_ERR(tag))
		return (void *)tag;

	in = neo_parse_bytes(handle, tag->size);
	if (IS_ERR(in))
		return (void *)in;

	out = kmalloc(tag->size + 1, GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);

	memcpy(out, in, tag->size);
	out[tag->size] = '\0';
	return out;
}

static int neo_parse_int_bound(struct neo_dcp_parse_ctx *handle, s64 *value, s64 min, s64 max)
{
	const void *tag = neo_parse_tag_of_type(handle, DCP_TYPE_INT64);
	const void *in;

	if (IS_ERR(tag))
		return PTR_ERR(tag);

	in = neo_parse_bytes(handle, sizeof(s64));

	if (IS_ERR(in))
		return PTR_ERR(in);

	memcpy(value, in, sizeof(*value));

	if (*value < min || *value > max)
		return -EINVAL;

	return 0;
}

static int neo_parse_int(struct neo_dcp_parse_ctx *handle, s64 *value)
{
	return neo_parse_int_bound(handle, value, S64_MIN, S64_MAX);
}

static int neo_parse_bool(struct neo_dcp_parse_ctx *handle, bool *b)
{
	const struct neo_dcp_parse_tag *tag = neo_parse_tag_of_type(handle, DCP_TYPE_BOOL);

	if (IS_ERR(tag))
		return PTR_ERR(tag);

	*b = !!tag->size;
	return 0;
}

#if IS_ENABLED(CONFIG_DRM_APPLE_NEO_AUDIO)
static int neo_parse_blob(struct neo_dcp_parse_ctx *handle, size_t size, u8 const **blob)
{
	const struct neo_dcp_parse_tag *tag = neo_parse_tag_of_type(handle, DCP_TYPE_BLOB);
	const u8 *out;

	if (IS_ERR(tag))
		return PTR_ERR(tag);

	if (tag->size < size)
		return -EINVAL;

	out = neo_parse_bytes(handle, tag->size);

	if (IS_ERR(out))
		return PTR_ERR(out);

	*blob = out;
	return 0;
}
#endif

struct iterator {
	struct neo_dcp_parse_ctx *handle;
	u32 idx, len;
};

static int iterator_begin(struct neo_dcp_parse_ctx *handle, struct iterator *it,
			  bool dict)
{
	const struct neo_dcp_parse_tag *tag;
	enum neo_dcp_parse_type type = dict ? DCP_TYPE_DICTIONARY : DCP_TYPE_ARRAY;

	*it = (struct iterator) {
		.handle = handle,
		.idx = 0
	};

	tag = neo_parse_tag_of_type(it->handle, type);
	if (IS_ERR(tag))
		return PTR_ERR(tag);

	it->len = tag->size;
	return 0;
}

int neo_parse(const void *blob, size_t size, struct neo_dcp_parse_ctx *ctx)
{
	const u32 *header;

	if (size > U32_MAX)
		return -EINVAL;

	*ctx = (struct neo_dcp_parse_ctx) {
		.blob = blob,
		.len = size,
		.pos = 0,
	};

	header = neo_parse_u32(ctx);
	if (IS_ERR(header))
		return PTR_ERR(header);

	if (*header != DCP_PARSE_HEADER)
		return -EINVAL;

	return 0;
}

static int neo_parse_dimension(struct neo_dcp_parse_ctx *handle, struct dimension *dim)
{
	struct iterator it;
	int ret = 0;

	*dim = (struct dimension) {
		.active = -1,
		.total = -1,
		.front_porch = -1,
		.sync_width = -1,
		.precise_sync_rate = -1,
	};

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		char *key = neo_parse_string(it.handle);

		if (IS_ERR(key))
			ret = PTR_ERR(key);
		else if (!strcmp(key, "Active"))
			ret = neo_parse_int_bound(it.handle, &dim->active, 0, U16_MAX);
		else if (!strcmp(key, "Total"))
			ret = neo_parse_int_bound(it.handle, &dim->total, 0, U16_MAX);
		else if (!strcmp(key, "FrontPorch"))
			ret = neo_parse_int_bound(it.handle, &dim->front_porch, 0, U16_MAX);
		else if (!strcmp(key, "SyncWidth"))
			ret = neo_parse_int_bound(it.handle, &dim->sync_width, 0, U16_MAX);
		else if (!strcmp(key, "PreciseSyncRate"))
			ret = neo_parse_int_bound(it.handle, &dim->precise_sync_rate, 1, U32_MAX);
		else
			ret = skip(it.handle);

		if (!IS_ERR_OR_NULL(key))
			kfree(key);

		if (ret)
			return ret;
	}

	if (dim->active <= 0 || dim->total < dim->active ||
	    dim->front_porch < 0 || dim->sync_width < 0 ||
	    dim->front_porch + dim->sync_width > dim->total - dim->active)
		return -EINVAL;

	return 0;
}

struct color_mode {
	s64 colorimetry;
	s64 depth;
	s64 dynamic_range;
	s64 eotf;
	s64 id;
	s64 pixel_encoding;
	s64 score;
};

static int fill_color_mode(struct neo_dcp_color_mode *color,
			   struct color_mode *cmode)
{
	if (cmode->id < 0 || cmode->id > U32_MAX)
		return -EINVAL;

	if (color->score >= cmode->score)
		return 0;

	if (cmode->colorimetry < 0 || cmode->colorimetry >= DCP_COLORIMETRY_COUNT)
		return -EINVAL;
	if (cmode->depth < 8 || cmode->depth > 12)
		return -EINVAL;
	if (cmode->dynamic_range < 0 || cmode->dynamic_range >= DCP_COLOR_YCBCR_RANGE_COUNT)
		return -EINVAL;
	if (cmode->eotf < 0 || cmode->eotf >= DCP_EOTF_COUNT)
		return -EINVAL;
	if (cmode->pixel_encoding < 0 || cmode->pixel_encoding >= DCP_COLOR_FORMAT_COUNT)
		return -EINVAL;

	color->score = cmode->score;
	color->id = cmode->id;
	color->eotf = cmode->eotf;
	color->format = cmode->pixel_encoding;
	color->colorimetry = cmode->colorimetry;
	color->range = cmode->dynamic_range;
	color->depth = cmode->depth;

	return 0;
}

static int neo_parse_color_modes(struct neo_dcp_parse_ctx *handle,
			     struct neo_dcp_display_mode *out)
{
	struct iterator outer_it;
	int ret = 0;
	out->sdr_444.score = -1;
	out->sdr_rgb.score = -1;
	out->sdr.score = -1;
	out->best.score = -1;

	ret = iterator_begin(handle, &outer_it, false);
	if (ret)
		return ret;

	for (; outer_it.idx < outer_it.len; ++outer_it.idx) {
		struct iterator it;
		bool is_virtual = true;
		struct color_mode cmode = {
			.colorimetry = -1,
			.depth = -1,
			.dynamic_range = -1,
			.eotf = -1,
			.id = -1,
			.pixel_encoding = -1,
			.score = -1,
		};

		ret = iterator_begin(handle, &it, true);
		if (ret)
			return ret;

		for (; it.idx < it.len; ++it.idx) {
			char *key = neo_parse_string(it.handle);

			if (IS_ERR(key))
				ret = PTR_ERR(key);
			else if (!strcmp(key, "Colorimetry"))
				ret = neo_parse_int(it.handle, &cmode.colorimetry);
			else if (!strcmp(key, "Depth"))
				ret = neo_parse_int(it.handle, &cmode.depth);
			else if (!strcmp(key, "DynamicRange"))
				ret = neo_parse_int(it.handle, &cmode.dynamic_range);
			else if (!strcmp(key, "EOTF"))
				ret = neo_parse_int(it.handle, &cmode.eotf);
			else if (!strcmp(key, "ID"))
				ret = neo_parse_int(it.handle, &cmode.id);
			else if (!strcmp(key, "IsVirtual"))
				ret = neo_parse_bool(it.handle, &is_virtual);
			else if (!strcmp(key, "PixelEncoding"))
				ret = neo_parse_int(it.handle, &cmode.pixel_encoding);
			else if (!strcmp(key, "Score"))
				ret = neo_parse_int(it.handle, &cmode.score);
			else
				ret = skip(it.handle);

			if (!IS_ERR_OR_NULL(key))
				kfree(key);

			if (ret)
				return ret;
		}

		/* Skip virtual or partial entries */
		if (is_virtual || cmode.score < 0 || cmode.id < 0)
			continue;

		trace_neo_iomfb_color_mode(handle->neo_dcp, cmode.id, cmode.score,
				       cmode.depth, cmode.colorimetry,
				       cmode.eotf, cmode.dynamic_range,
				       cmode.pixel_encoding);

		if (cmode.eotf == DCP_EOTF_SDR_GAMMA) {
			if (cmode.pixel_encoding == DCP_COLOR_FORMAT_RGB &&
				cmode.depth <= 10)
				fill_color_mode(&out->sdr_rgb, &cmode);
			else if (cmode.pixel_encoding == DCP_COLOR_FORMAT_YCBCR444 &&
				cmode.depth <= 10)
				fill_color_mode(&out->sdr_444, &cmode);
			fill_color_mode(&out->sdr, &cmode);
		}
		fill_color_mode(&out->best, &cmode);
	}

	return 0;
}

/*
 * Calculate the pixel clock for a mode given the 16:16 fixed-point refresh
 * rate. The pixel clock is the refresh rate times the pixel count. DRM
 * specifies the clock in kHz. The intermediate result may overflow a u32, so
 * use a u64 where required.
 */
static u64 calculate_clock(struct dimension *horiz, struct dimension *vert)
{
	u32 pixels = horiz->total * vert->total;
	u64 clock = mul_u32_u32(pixels, vert->precise_sync_rate);

	return DIV_ROUND_CLOSEST_ULL(clock >> 16, 1000);
}

static int neo_parse_mode(struct neo_dcp_parse_ctx *handle,
		      struct neo_dcp_display_mode *out, s64 *score, int width_mm,
		      int height_mm, unsigned notch_height, bool internal)
{
	int ret = 0;
	struct iterator it;
	struct dimension horiz = { .active = -1 }, vert = { .active = -1 };
	s64 min_vrr = 0, max_vrr = 0;
	u64 clock;
	s64 id = -1;
	s64 best_color_mode = -1;
	bool is_virtual = false;
	struct drm_display_mode *mode = &out->mode;

	*out = (struct neo_dcp_display_mode) {
		.sdr_rgb.score = -1,
		.sdr_444.score = -1,
		.sdr.score = -1,
		.best.score = -1,
	};
	*score = -1;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		char *key = neo_parse_string(it.handle);

		if (IS_ERR(key))
			ret = PTR_ERR(key);
		else if (is_virtual)
			ret = skip(it.handle);
		else if (!strcmp(key, "HorizontalAttributes"))
			ret = neo_parse_dimension(it.handle, &horiz);
		else if (!strcmp(key, "VerticalAttributes"))
			ret = neo_parse_dimension(it.handle, &vert);
		else if (!strcmp(key, "MinimumVariableRefreshRate"))
			ret = neo_parse_int_bound(it.handle, &min_vrr, 0, U32_MAX);
		else if (!strcmp(key, "MaximumVariableRefreshRate"))
			ret = neo_parse_int_bound(it.handle, &max_vrr, 0, U32_MAX);
		else if (!strcmp(key, "ColorModes"))
			ret = neo_parse_color_modes(it.handle, out);
		else if (!strcmp(key, "ID"))
			ret = neo_parse_int_bound(it.handle, &id, 0, U32_MAX);
		else if (!strcmp(key, "IsVirtual"))
			ret = neo_parse_bool(it.handle, &is_virtual);
		else if (!strcmp(key, "Score"))
			ret = neo_parse_int_bound(it.handle, score, 0, S64_MAX);
		else
			ret = skip(it.handle);

		if (!IS_ERR_OR_NULL(key))
			kfree(key);

		if (ret) {
			trace_neo_iomfb_parse_mode_fail(id, &horiz, &vert, best_color_mode, is_virtual, *score);
			return ret;
		}
	}
	if (out->sdr_rgb.score >= 0)
		best_color_mode = out->sdr_rgb.id;
	else if (out->sdr_444.score >= 0)
		best_color_mode = out->sdr_444.id;
	else if (out->sdr.score >= 0)
		best_color_mode = out->sdr.id;
	else if (out->best.score >= 0)
		best_color_mode = out->best.id;

	trace_neo_iomfb_parse_mode_success(id, &horiz, &vert, best_color_mode,
				       is_virtual, *score);

	/*
	 * Reject modes without valid color mode.
	 */
	if (best_color_mode < 0 || id < 0 || *score < 0 ||
	    horiz.active <= 0 || vert.active <= notch_height ||
	    vert.precise_sync_rate <= 0)
		return -EINVAL;

	/*
	 * We need to skip virtual modes. In some cases, virtual modes are "too
	 * big" for the monitor and can cause breakage. It is unclear why the
	 * DCP reports these modes at all. Treat as a recoverable error.
	 */
	if (is_virtual)
		return -EINVAL;

	/*
	 * An internal ProMotion panel carries no EDID or DisplayID, so DCP
	 * reports no adaptive-sync range for it. Assume the ProMotion floor of
	 * 24 Hz up to whatever rate the mode itself advertises.
	 */
	if (internal && vert.precise_sync_rate >> 16 == 120) {
		out->min_vrr = 24 << 16;
		out->max_vrr = vert.precise_sync_rate;
		out->vrr = true;
	}

	/* Refresh rates are reported by DCP as 16.16 fixed-point Hz. */
	if (min_vrr && max_vrr > min_vrr) {
		out->min_vrr = min_vrr;
		out->max_vrr = max_vrr;
		out->vrr = true;
	}

	clock = calculate_clock(&horiz, &vert);
	if (!clock || clock > INT_MAX)
		return -EINVAL;

	vert.active -= notch_height;
	vert.sync_width += notch_height;

	/* From here we must succeed. Start filling out the mode. */
	*mode = (struct drm_display_mode) {
		.type = DRM_MODE_TYPE_DRIVER,
		.clock = clock,

		.vdisplay = vert.active,
		.vsync_start = vert.active + vert.front_porch,
		.vsync_end = vert.active + vert.front_porch + vert.sync_width,
		.vtotal = vert.total,

		.hdisplay = horiz.active,
		.hsync_start = horiz.active + horiz.front_porch,
		.hsync_end = horiz.active + horiz.front_porch +
			     horiz.sync_width,
		.htotal = horiz.total,

		.width_mm = width_mm,
		.height_mm = height_mm,
	};

	drm_mode_set_name(mode);

	out->timing_mode_id = id;
	out->color_mode_id = best_color_mode;

	trace_neo_iomfb_timing_mode(handle->neo_dcp, id, *score, horiz.active,
				vert.active, vert.precise_sync_rate,
				best_color_mode);

	return 0;
}

struct neo_dcp_display_mode *neo_enumerate_modes(struct neo_dcp_parse_ctx *handle,
					 unsigned int *count, int width_mm,
					 int height_mm, unsigned notch_height,
					 bool internal)
{
	struct iterator it;
	int ret;
	struct neo_dcp_display_mode *mode, *modes;
	struct neo_dcp_display_mode *best_mode = NULL;
	s64 score, best_score = -1;

	ret = iterator_begin(handle, &it, false);

	if (ret)
		return ERR_PTR(ret);

	/* Start with a worst case allocation. */
	modes = kcalloc(it.len, sizeof(*modes), GFP_KERNEL);
	*count = 0;

	if (!modes)
		return ERR_PTR(-ENOMEM);

	for (; it.idx < it.len; ++it.idx) {
		struct neo_dcp_parse_ctx entry = *it.handle;

		mode = &modes[*count];
		ret = neo_parse_mode(it.handle, mode, &score, width_mm, height_mm,
				 notch_height, internal);

		/* Errors for a single mode are recoverable -- just skip it. */
		if (ret) {
			/* A failed parse can stop in the middle of the dictionary. */
			*it.handle = entry;
			ret = skip(it.handle);
			if (ret) {
				kfree(modes);
				return ERR_PTR(ret);
			}
			continue;
		}

		/* Process a successful mode */
		(*count)++;

		if (score > best_score) {
			best_score = score;
			best_mode = mode;
		}
	}

	if (best_mode != NULL)
		best_mode->mode.type |= DRM_MODE_TYPE_PREFERRED;

	return modes;
}

int neo_parse_display_attributes(struct neo_dcp_parse_ctx *handle, int *width_mm,
			     int *height_mm)
{
	int ret = 0;
	struct iterator it;
	s64 width_cm = 0, height_cm = 0;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		char *key = neo_parse_string(it.handle);

		if (IS_ERR(key))
			ret = PTR_ERR(key);
		else if (!strcmp(key, "MaxHorizontalImageSize"))
			ret = neo_parse_int(it.handle, &width_cm);
		else if (!strcmp(key, "MaxVerticalImageSize"))
			ret = neo_parse_int(it.handle, &height_cm);
		else
			ret = skip(it.handle);

		if (!IS_ERR_OR_NULL(key))
			kfree(key);

		if (ret)
			return ret;
	}

	/* 1cm = 10mm */
	*width_mm = 10 * width_cm;
	*height_mm = 10 * height_cm;

	return 0;
}

int neo_parse_epic_service_init(struct neo_dcp_parse_ctx *handle, const char **name,
			    const char **class, s64 *unit, bool h17p_keys)
{
	int ret = 0;
	struct iterator it;
	bool parsed_unit = false;
	bool parsed_name = false;
	bool parsed_class = false;

	*name = ERR_PTR(-ENOENT);
	*class = ERR_PTR(-ENOENT);

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		char *key = neo_parse_string(it.handle);

		if (IS_ERR(key)) {
			ret = PTR_ERR(key);
			break;
		}

		/*
		 * H17P advertises its system service with lower-case keys --
		 * "compartment", "name", "interface-id" -- where earlier
		 * firmware uses EPICName/EPICProviderClass/EPICUnit.  Without
		 * them the service never registers, the firmware has no system
		 * service to ask and idles the panel, so no vblank and no swap
		 * ever completes.  Only H17P callers accept them.  If a
		 * dictionary carries both spellings, the later value wins and
		 * the earlier string is freed.
		 */
		if (!strcmp(key, "EPICName") ||
		    (h17p_keys && !strcmp(key, "compartment"))) {
			if (parsed_name)
				kfree(*name);
			*name = neo_parse_string(it.handle);
			if (IS_ERR(*name))
				ret = PTR_ERR(*name);
			else
				parsed_name = true;
		} else if (!strcmp(key, "EPICProviderClass") ||
			   (h17p_keys && !strcmp(key, "name"))) {
			if (parsed_class)
				kfree(*class);
			*class = neo_parse_string(it.handle);
			if (IS_ERR(*class))
				ret = PTR_ERR(*class);
			else
				parsed_class = true;
		} else if (!strcmp(key, "EPICUnit") ||
			   (h17p_keys && !strcmp(key, "interface-id"))) {
			ret = neo_parse_int(it.handle, unit);
			if (!ret)
				parsed_unit = true;
		} else {
			ret = skip(it.handle);
		}

		kfree(key);
		if (ret)
			break;
	}

	if (!parsed_unit || !parsed_name || !parsed_class)
		ret = -ENOENT;

	if (ret) {
		if (!IS_ERR(*name)) {
			kfree(*name);
			*name = ERR_PTR(ret);
		}
		if (!IS_ERR(*class)) {
			kfree(*class);
			*class = ERR_PTR(ret);
		}
	}

	return ret;
}

#if IS_ENABLED(CONFIG_DRM_APPLE_NEO_AUDIO)
static int neo_parse_sample_rate_bit(struct neo_dcp_parse_ctx *handle, unsigned int *ratebit)
{
	s64 rate;
	int ret = neo_parse_int(handle, &rate);

	if (ret)
		return ret;

	*ratebit = snd_pcm_rate_to_rate_bit(rate);
	if (*ratebit == SNDRV_PCM_RATE_KNOT) {
		/*
		 * The rate wasn't recognized, and unless we supply
		 * a supplementary constraint, the SNDRV_PCM_RATE_KNOT bit
		 * will allow any rate. So clear it.
		 */
		*ratebit = 0;
	}

	return 0;
}

static int neo_parse_sample_fmtbit(struct neo_dcp_parse_ctx *handle, u64 *fmtbit)
{
	s64 sample_size;
	int ret = neo_parse_int(handle, &sample_size);

	if (ret)
		return ret;

	switch (sample_size) {
	case 16:
		*fmtbit = SNDRV_PCM_FMTBIT_S16;
		break;
	case 20:
		*fmtbit = SNDRV_PCM_FMTBIT_S20;
		break;
	case 24:
		*fmtbit = SNDRV_PCM_FMTBIT_S24;
		break;
	case 32:
		*fmtbit = SNDRV_PCM_FMTBIT_S32;
		break;
	default:
		*fmtbit = 0;
		break;
	}

	return 0;
}

static struct {
	const char *label;
	u8 type;
} chan_position_names[] = {
	{ "Front Left", SNDRV_CHMAP_FL },
	{ "Front Right", SNDRV_CHMAP_FR },
	{ "Rear Left", SNDRV_CHMAP_RL },
	{ "Rear Right", SNDRV_CHMAP_RR },
	{ "Front Center", SNDRV_CHMAP_FC },
	{ "Low Frequency Effects", SNDRV_CHMAP_LFE },
	{ "Rear Center", SNDRV_CHMAP_RC },
	{ "Front Left Center", SNDRV_CHMAP_FLC },
	{ "Front Right Center", SNDRV_CHMAP_FRC },
	{ "Rear Left Center", SNDRV_CHMAP_RLC },
	{ "Rear Right Center", SNDRV_CHMAP_RRC },
	{ "Front Left Wide", SNDRV_CHMAP_FLW },
	{ "Front Right Wide", SNDRV_CHMAP_FRW },
	{ "Front Left High", SNDRV_CHMAP_FLH },
	{ "Front Center High", SNDRV_CHMAP_FCH },
	{ "Front Right High", SNDRV_CHMAP_FRH },
	{ "Top Center", SNDRV_CHMAP_TC },
};

static void append_chmap(struct snd_pcm_chmap_elem *chmap, u8 type)
{
	if (!chmap || chmap->channels >= ARRAY_SIZE(chmap->map))
		return;

	chmap->map[chmap->channels] = type;
	chmap->channels++;
}

static int neo_parse_chmap(struct neo_dcp_parse_ctx *handle, struct snd_pcm_chmap_elem *chmap)
{
	struct iterator it;
	int i, ret;

	if (!chmap)
		return skip(handle);

	chmap->channels = 0;

	ret = iterator_begin(handle, &it, false);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		for (i = 0; i < ARRAY_SIZE(chan_position_names); i++)
			if (consume_string(it.handle, chan_position_names[i].label))
				break;

		if (i == ARRAY_SIZE(chan_position_names)) {
			ret = skip(it.handle);
			if (ret)
				return ret;

			append_chmap(chmap, SNDRV_CHMAP_UNKNOWN);
			continue;
		}

		append_chmap(chmap, chan_position_names[i].type);
	}

	return 0;
}

static int neo_parse_chan_layout_element(struct neo_dcp_parse_ctx *handle,
				     unsigned int *nchans_out,
				     struct snd_pcm_chmap_elem *chmap)
{
	struct iterator it;
	int ret;
	s64 nchans = 0;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		if (consume_string(it.handle, "ActiveChannelCount"))
			ret = neo_parse_int(it.handle, &nchans);
		else if (consume_string(it.handle, "ChannelLayout"))
			ret = neo_parse_chmap(it.handle, chmap);
		else
			ret = skip_pair(it.handle);

		if (ret)
			return ret;
	}

	if (nchans_out)
		*nchans_out = nchans;

	return 0;
}

static int neo_parse_nchans_mask(struct neo_dcp_parse_ctx *handle, unsigned int *mask)
{
	struct iterator it;
	int ret;

	*mask = 0;

	ret = iterator_begin(handle, &it, false);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		int nchans;

		ret = neo_parse_chan_layout_element(it.handle, &nchans, NULL);
		if (ret)
			return ret;
		*mask |= 1 << nchans;
	}

	return 0;
}

static int neo_parse_avep_element(struct neo_dcp_parse_ctx *handle,
			      struct neo_dcp_sound_format_mask *sieve,
			      struct neo_dcp_sound_format_mask *hits)
{
	struct neo_dcp_sound_format_mask mask = {0, 0, 0};
	struct iterator it;
	int ret;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		if (consume_string(handle, "StreamSampleRate"))
			ret = neo_parse_sample_rate_bit(it.handle, &mask.rates);
		else if (consume_string(handle, "SampleSize"))
			ret = neo_parse_sample_fmtbit(it.handle, &mask.formats);
		else if (consume_string(handle, "AudioChannelLayoutElements"))
			ret = neo_parse_nchans_mask(it.handle, &mask.nchans);
		else
			ret = skip_pair(it.handle);

		if (ret)
			return ret;
	}

	trace_neo_avep_sound_mode(handle->neo_dcp, mask.rates, mask.formats, mask.nchans);

	if (!(mask.rates & sieve->rates) || !(mask.formats & sieve->formats) ||
		!(mask.nchans & sieve->nchans))
	    return 0;

	if (hits) {
		hits->rates |= mask.rates;
		hits->formats |= mask.formats;
		hits->nchans |= mask.nchans;
	}

	return 1;
}

static int neo_parse_mode_in_avep_element(struct neo_dcp_parse_ctx *handle,
				      unsigned int selected_nchans,
				      struct snd_pcm_chmap_elem *chmap,
				      struct neo_dcp_sound_cookie *cookie)
{
	struct iterator it;
	struct neo_dcp_parse_ctx save_handle;
	int ret;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		if (consume_string(it.handle, "AudioChannelLayoutElements")) {
			struct iterator inner_it;
			int nchans;

			ret = iterator_begin(it.handle, &inner_it, false);
			if (ret)
				return ret;

			for (; inner_it.idx < inner_it.len; ++inner_it.idx) {
				save_handle = *it.handle;
				ret = neo_parse_chan_layout_element(inner_it.handle,
								&nchans, NULL);
				if (ret)
					return ret;

				if (nchans != selected_nchans)
					continue;

				/*
				 * Now that we know this layout matches the
				 * selected channel number, reread the element
				 * and fill in the channel map.
				 */
				*inner_it.handle = save_handle;
				ret = neo_parse_chan_layout_element(inner_it.handle,
								NULL, chmap);
				if (ret)
					return ret;
			}
		} else if (consume_string(it.handle, "ElementData")) {
			const u8 *blob;

			ret = neo_parse_blob(it.handle, sizeof(*cookie), &blob);
			if (ret)
				return ret;

			if (cookie)
				memcpy(cookie, blob, sizeof(*cookie));
		} else {
			ret = skip_pair(it.handle);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int neo_parse_sound_constraints(struct neo_dcp_parse_ctx *handle,
			    struct neo_dcp_sound_format_mask *sieve,
			    struct neo_dcp_sound_format_mask *hits)
{
	int ret;
	struct iterator it;

	if (hits) {
		hits->rates = 0;
		hits->formats = 0;
		hits->nchans = 0;
	}

	ret = iterator_begin(handle, &it, false);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		ret = neo_parse_avep_element(it.handle, sieve, hits);

		if (ret < 0)
			return ret;
	}

	return 0;
}

int neo_parse_sound_mode(struct neo_dcp_parse_ctx *handle,
		     struct neo_dcp_sound_format_mask *sieve,
		     struct snd_pcm_chmap_elem *chmap,
		     struct neo_dcp_sound_cookie *cookie)
{
	struct neo_dcp_parse_ctx save_handle;
	struct iterator it;
	int ret;

	ret = iterator_begin(handle, &it, false);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		save_handle = *it.handle;
		ret = neo_parse_avep_element(it.handle, sieve, NULL);

		if (!ret)
			continue;

		if (ret < 0)
			return ret;

		ret = neo_parse_mode_in_avep_element(&save_handle, __ffs(sieve->nchans),
						 chmap, cookie);
		if (ret < 0)
			return ret;
		return 1;
	}

	return 0;
}
#endif

int neo_parse_system_log_mnits(struct neo_dcp_parse_ctx *handle, struct neo_dcp_system_ev_mnits *entry)
{
	struct iterator it;
	int ret;
	s64 mnits = -1;
	s64 idac = -1;
	s64 timestamp = -1;
	bool type_match = false;

	ret = iterator_begin(handle, &it, true);
	if (ret)
		return ret;

	for (; it.idx < it.len; ++it.idx) {
		char *key = neo_parse_string(it.handle);
		if (IS_ERR(key)) {
			ret = PTR_ERR(key);
		} else if (!strcmp(key, "mNits")) {
			ret = neo_parse_int(it.handle, &mnits);
		} else if (!strcmp(key, "iDAC")) {
			ret = neo_parse_int(it.handle, &idac);
		} else if (!strcmp(key, "logEvent")) {
			const char *value = neo_parse_string(it.handle);

			ret = IS_ERR(value) ? PTR_ERR(value) : 0;
			if (!ret) {
				type_match = strcmp(value, "Display (Event Forward)") == 0;
				kfree(value);
			}
		} else if (!strcmp(key, "timestamp")) {
			ret = neo_parse_int(it.handle, &timestamp);
		} else {
			ret = skip(it.handle);
		}

		if (!IS_ERR_OR_NULL(key))
			kfree(key);

		if (ret) {
			pr_err("dcp parser: failed to parse mNits sys event\n");
			return ret;
		}
	}

	if (!type_match ||  mnits < 0 || idac < 0 || timestamp < 0)
		return -EINVAL;
	if (mnits > U32_MAX)
		return -ERANGE;

	entry->millinits = mnits;
	entry->idac = idac;
	entry->timestamp = timestamp;

	return 0;
}
