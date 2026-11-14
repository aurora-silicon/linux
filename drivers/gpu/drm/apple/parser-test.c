// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_edid.h>
#include <drm/drm_property.h>
#include <drm/drm_atomic_state_helper.h>

#include "dcp.h"
#include <drm/drm_edid.h>
#include <linux/unaligned.h>

#include "connector.h"
#include "dcp-internal.h"
#include "dcp-property.h"
#include "iomfb_internal.h"
#include "parser.h"
#include "dcp-panel-adt.h"
#include "dcp-panel-test.h"

static struct device parser_device = {
	.init_name = "apple-display-parser-test",
};

static struct apple_dcp parser_dcp = {
	.dev = &parser_device,
};

/* The probe owns the metadata lock beside the mode catalogue. */
static void parser_modes_init(struct apple_dcp *dcp)
{
	dcp_modes_init(dcp);
	spin_lock_init(&dcp->metadata.lock);
}

struct parser_blob {
	u8 data[8192];
	u32 size;
};

struct parser_record {
	s64 id, score, color_score, color_id;
	s64 active, total, porch, sync, rate, horizontal_rate;
	u32 missing, horizontal_missing, color_missing, second_color_missing;
	s64 first_format;
	bool bad_score_type;
	bool vrr;
	bool vrr_zero;
	bool dsc_required;
};

enum parser_missing {
	MISSING_SCORE = BIT(0),
	MISSING_ACTIVE = BIT(1),
	MISSING_TOTAL = BIT(2),
	MISSING_PORCH = BIT(3),
	MISSING_SYNC = BIT(4),
	MISSING_RATE = BIT(5),
	MISSING_COLORS = BIT(6),
	MISSING_COLOR_SCORE = BIT(0),
	MISSING_COLOR_DEPTH = BIT(1),
};

static void blob_tag(struct parser_blob *blob, u32 type, u32 count)
{
	blob->size = ALIGN(blob->size, 4);
	put_unaligned_le32(count | type << 24, blob->data + blob->size);
	blob->size += 4;
}

static void blob_key(struct parser_blob *blob, const char *key)
{
	size_t length = strlen(key);

	blob_tag(blob, 9, length);
	memcpy(blob->data + blob->size, key, length);
	blob->size += length;
}

static void blob_int(struct parser_blob *blob, const char *key, s64 value)
{
	blob_key(blob, key);
	blob_tag(blob, 4, 0);
	put_unaligned_le64(value, blob->data + blob->size);
	blob->size += 8;
}

static void blob_bool(struct parser_blob *blob, const char *key, bool value)
{
	blob_key(blob, key);
	blob_tag(blob, 11, value);
}

static void blob_dimension(struct parser_blob *blob, const char *key,
			   const struct parser_record *record, bool vertical)
{
	u32 missing = vertical ? record->missing >> 1 & 0x1f : record->horizontal_missing;

	blob_key(blob, key);
	blob_tag(blob, 1, 5 - hweight32(missing));
	if (!(missing & BIT(0)))
		blob_int(blob, "Active", vertical ? record->active : 1920);
	if (!(missing & BIT(1)))
		blob_int(blob, "Total", vertical ? record->total : 2200);
	if (!(missing & BIT(2)))
		blob_int(blob, "FrontPorch", vertical ? record->porch : 88);
	if (!(missing & BIT(3)))
		blob_int(blob, "SyncWidth", vertical ? record->sync : 44);
	if (!(missing & BIT(4)))
		blob_int(blob, "PreciseSyncRate",
			 vertical ? record->rate : record->horizontal_rate);
}

static void blob_color(struct parser_blob *blob, s64 id, s64 score,
		       u32 missing, s64 format)
{
	blob_tag(blob, 1, 8 - hweight32(missing));
	blob_bool(blob, "IsVirtual", false);
	blob_int(blob, "ID", id);
	if (!(missing & MISSING_COLOR_SCORE))
		blob_int(blob, "Score", score);
	if (!(missing & MISSING_COLOR_DEPTH))
		blob_int(blob, "Depth", 8);
	blob_int(blob, "Colorimetry", DCP_COLORIMETRY_RGB);
	blob_int(blob, "DynamicRange", DCP_COLOR_YCBCR_RANGE_FULL);
	blob_int(blob, "EOTF", DCP_EOTF_SDR_GAMMA);
	blob_int(blob, "PixelEncoding", format);
}

static void blob_record(struct parser_blob *blob, const struct parser_record *record)
{
	unsigned int count = 6 + (record->vrr ? 2 : 0) + record->dsc_required;

	count -= !!(record->missing & MISSING_SCORE);
	count -= !!(record->missing & MISSING_COLORS);
	blob_tag(blob, 1, count);
	blob_bool(blob, "IsVirtual", false);
	blob_int(blob, "ID", record->id);
	if (!(record->missing & MISSING_SCORE)) {
		if (record->bad_score_type) {
			blob_key(blob, "Score");
			blob_tag(blob, 9, 1);
			blob->data[blob->size++] = 'x';
		} else {
			blob_int(blob, "Score", record->score);
		}
	}
	blob_dimension(blob, "HorizontalAttributes", record, false);
	blob_dimension(blob, "VerticalAttributes", record, true);
	if (!(record->missing & MISSING_COLORS)) {
		blob_key(blob, "ColorModes");
		blob_tag(blob, 2, 2);
		blob_color(blob, record->color_id, record->color_score,
			   record->color_missing, record->first_format);
		blob_color(blob, 101, record->color_score,
			   record->second_color_missing, DCP_COLOR_FORMAT_RGB);
	}
	if (record->dsc_required) {
		blob_key(blob, "DSCRequiredColorElementIDs");
		blob_tag(blob, 2, 2);
		blob_tag(blob, 4, 0);
		put_unaligned_le64(100, blob->data + blob->size);
		blob->size += 8;
		blob_tag(blob, 4, 0);
		put_unaligned_le64(101, blob->data + blob->size);
		blob->size += 8;
	}
	if (record->vrr) {
		blob_int(blob, "MinimumVariableRefreshRate", record->vrr_zero ? 0 : 48 << 16);
		blob_int(blob, "MaximumVariableRefreshRate", record->vrr_zero ? 0 : 120 << 16);
	}
}

static struct parser_record valid_record(s64 id, s64 score)
{
	return (struct parser_record) {
		.id = id, .score = score, .color_score = 10, .color_id = 100,
		.active = 1080, .total = 1125, .porch = 4, .sync = 5,
		.rate = 60 << 16, .horizontal_rate = (675U << 16) / 10,
	};
}

static struct parser_blob *make_blob(struct kunit *test,
				     const struct parser_record *records, unsigned int count)
{
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	unsigned int i;

	if (!blob)
		return NULL;
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 2, count);
	for (i = 0; i < count; i++)
		blob_record(blob, &records[i]);
	return blob;
}

static struct dcp_display_mode *parse_blob(struct parser_blob *blob,
					   unsigned int *count, unsigned int notch, bool internal)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(blob->data, blob->size, &ctx);

	if (ret)
		return ERR_PTR(ret);
	ctx.dcp = &parser_dcp;
	return enumerate_modes(&ctx, count, 500, 300, notch, internal, false);
}

static struct dcp_display_mode *parse_blob_tiled(struct parser_blob *blob,
						 unsigned int *count, u32 width, u32 height)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(blob->data, blob->size, &ctx);

	if (ret)
		return ERR_PTR(ret);
	ctx.dcp = &parser_dcp;
	ctx.tiled_width = width;
	ctx.tiled_height = height;
	return enumerate_modes(&ctx, count, 500, 300, 0, false, false);
}

static void parser_tiled_size_keeps_dsc_modes(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count;

	record.dsc_required = true;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);

	modes = parse_blob_tiled(blob, &count, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_FALSE(test, modes[0].sdr_usable);
	kfree(modes);

	modes = parse_blob_tiled(blob, &count, 3840, 2160);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_FALSE(test, modes[0].sdr_usable);
	kfree(modes);

	modes = parse_blob_tiled(blob, &count, 1920, 1080);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_TRUE(test, modes[0].sdr_usable);
	kfree(modes);
}

static void parser_score_presence(struct kunit *test)
{
	struct parser_record records[] = { valid_record(7, 100), valid_record(8, 0) };
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count;

	records[1].missing = MISSING_SCORE;
	blob = make_blob(test, records, ARRAY_SIZE(records));
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].timing_mode_id, 7U);
	KUNIT_EXPECT_TRUE(test, modes[0].mode.type & DRM_MODE_TYPE_PREFERRED);
	kfree(modes);

	records[0].missing = MISSING_SCORE;
	records[1] = valid_record(8, 0);
	blob = make_blob(test, records, ARRAY_SIZE(records));
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].timing_mode_id, 8U);
	kfree(modes);
}

static void parser_legacy_ties(struct kunit *test)
{
	struct parser_record records[] = { valid_record(7, 50), valid_record(8, 50) };
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, internal;

	for (internal = 0; internal < 2; internal++) {
		blob = make_blob(test, records, ARRAY_SIZE(records));
		KUNIT_ASSERT_NOT_NULL(test, blob);
		modes = parse_blob(blob, &count, 0, internal);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_EXPECT_EQ(test, count, 2U);
		KUNIT_EXPECT_TRUE(test, modes[0].mode.type & DRM_MODE_TYPE_PREFERRED);
		KUNIT_EXPECT_FALSE(test, modes[1].mode.type & DRM_MODE_TYPE_PREFERRED);
		KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, 100U);
		kfree(modes);
	}
	records[1].score = 51;
	blob = make_blob(test, records, ARRAY_SIZE(records));
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_FALSE(test, modes[0].mode.type & DRM_MODE_TYPE_PREFERRED);
	KUNIT_EXPECT_TRUE(test, modes[1].mode.type & DRM_MODE_TYPE_PREFERRED);
	kfree(modes);
}

static void blob_sdr_color(struct parser_blob *blob, s64 id, s64 score,
			   s64 colorimetry)
{
	blob_tag(blob, 1, 8);
	blob_bool(blob, "IsVirtual", false);
	blob_int(blob, "ID", id);
	blob_int(blob, "Score", score);
	blob_int(blob, "Depth", 10);
	blob_int(blob, "Colorimetry", colorimetry);
	blob_int(blob, "DynamicRange", DCP_COLOR_YCBCR_RANGE_FULL);
	blob_int(blob, "EOTF", DCP_EOTF_SDR_GAMMA);
	blob_int(blob, "PixelEncoding", DCP_COLOR_FORMAT_RGB);
}

/*
 * One timing whose BT.2020 SDR element outscores its Default RGB one, as on
 * a firmware 27 external: only a firmware 27 external picks Default RGB.
 */
static void parser_sdr_default_rgb(struct kunit *test)
{
	struct parser_record record = valid_record(7, 100);
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_display_mode *modes;
	struct dcp_parse_ctx ctx;
	unsigned int count;

	KUNIT_ASSERT_NOT_NULL(test, blob);
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 2, 1);
	blob_tag(blob, 1, 6);
	blob_bool(blob, "IsVirtual", false);
	blob_int(blob, "ID", record.id);
	blob_int(blob, "Score", record.score);
	blob_dimension(blob, "HorizontalAttributes", &record, false);
	blob_dimension(blob, "VerticalAttributes", &record, true);
	blob_key(blob, "ColorModes");
	blob_tag(blob, 2, 2);
	blob_sdr_color(blob, 101, 20, DCP_COLORIMETRY_BT2020_RGB);
	blob_sdr_color(blob, 102, 10, DCP_COLORIMETRY_RGB);

	for (int fw27 = 0; fw27 < 2; fw27++) {
		KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
		ctx.dcp = &parser_dcp;
		ctx.sdr_default_rgb = fw27;
		modes = enumerate_modes(&ctx, &count, 500, 300, 0, false, false);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_ASSERT_EQ(test, count, 1U);
		KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, fw27 ? 102U : 101U);
		kfree(modes);
	}
}

static void parser_color_presence(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, missing;

	for (missing = MISSING_COLOR_SCORE; missing <= MISSING_COLOR_DEPTH; missing <<= 1) {
		record.color_missing = missing;
		blob = make_blob(test, &record, 1);
		KUNIT_ASSERT_NOT_NULL(test, blob);
		modes = parse_blob(blob, &count, 0, false);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_EXPECT_EQ(test, count, 1U);
		KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, 101U);
		kfree(modes);
	}
	record.color_missing = 0;
	record.first_format = DCP_COLOR_FORMAT_YCBCR420;
	record.second_color_missing = MISSING_COLOR_SCORE;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, 100U);
	KUNIT_EXPECT_EQ(test, modes[0].sdr_rgb.score, -1LL);
	kfree(modes);
	record.missing = MISSING_COLORS;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 0U);
	kfree(modes);
}

static void parser_geometry(struct kunit *test)
{
	struct parser_record record;
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, bit;

	for (bit = MISSING_ACTIVE; bit <= MISSING_RATE; bit <<= 1) {
		record = valid_record(7, 10);
		record.missing = bit;
		blob = make_blob(test, &record, 1);
		KUNIT_ASSERT_NOT_NULL(test, blob);
		modes = parse_blob(blob, &count, 0, false);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_EXPECT_EQ(test, count, 0U);
		kfree(modes);
	}
}

static void parser_horizontal_rate(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, bit;

	record.horizontal_missing = BIT(4);
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].mode.clock, 148500);
	kfree(modes);
	record.horizontal_missing = 0;
	record.horizontal_rate = 0;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].mode.clock, 148500);
	kfree(modes);
	record.horizontal_rate = -1;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 0U);
	kfree(modes);
	record.horizontal_rate = (675U << 16) / 10;
	for (bit = BIT(0); bit <= BIT(3); bit <<= 1) {
		record.horizontal_missing = bit;
		blob = make_blob(test, &record, 1);
		KUNIT_ASSERT_NOT_NULL(test, blob);
		modes = parse_blob(blob, &count, 0, false);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_EXPECT_EQ(test, count, 0U);
		kfree(modes);
	}
}

static void parser_bad_score(struct kunit *test)
{
	struct parser_record records[] = { valid_record(7, 10), valid_record(8, 20) };
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count;

	records[0].bad_score_type = true;
	blob = make_blob(test, records, ARRAY_SIZE(records));
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].timing_mode_id, 8U);
	kfree(modes);
	records[0].bad_score_type = false;
	records[0].score = -1;
	blob = make_blob(test, records, ARRAY_SIZE(records));
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].timing_mode_id, 8U);
	kfree(modes);
}

static void parser_bad_geometry(struct kunit *test)
{
	static const s64 values[][5] = {
		{ 0, 1125, 4, 5, 60 << 16 },
		{ -1, 1125, 4, 5, 60 << 16 },
		{ 1200, 1125, 4, 5, 60 << 16 },
		{ 1080, 1125, -1, 5, 60 << 16 },
		{ 1080, 1125, 4, -1, 60 << 16 },
		{ 1080, 1125, 4, 5, 0 },
		{ 1080, 1125, 4, 5, 1 },
		{ 65536, 65536, 4, 5, 60 << 16 },
		{ 1080, 65535, 4, 5, U32_MAX },
	};
	struct parser_record record;
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, i;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		record = valid_record(7, 10);
		record.active = values[i][0];
		record.total = values[i][1];
		record.porch = values[i][2];
		record.sync = values[i][3];
		record.rate = values[i][4];
		blob = make_blob(test, &record, 1);
		KUNIT_ASSERT_NOT_NULL(test, blob);
		modes = parse_blob(blob, &count, 0, false);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
		KUNIT_EXPECT_EQ(test, count, 0U);
		kfree(modes);
	}
}

static void parser_notch_vrr(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count;

	record.rate = 120 << 16;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 1080, true);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 0U);
	kfree(modes);
	modes = parse_blob(blob, &count, 32, true);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, modes[0].mode.vdisplay, 1048U);
	KUNIT_EXPECT_TRUE(test, modes[0].vrr);
	KUNIT_EXPECT_EQ(test, modes[0].min_vrr, 24U << 16);
	kfree(modes);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_FALSE(test, modes[0].vrr);
	kfree(modes);
	record.vrr = true;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_EXPECT_EQ(test, modes[0].min_vrr, 48U << 16);
	KUNIT_EXPECT_EQ(test, modes[0].max_vrr, 120U << 16);
	kfree(modes);
}

static void parser_vrr_empty_range(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct dcp_display_mode *modes;
	struct parser_blob *blob;
	unsigned int count, desktop;
	struct dcp_parse_ctx ctx;

	/* Desktop firmware can publish both fields as zero placeholders. */
	record.rate = 120 << 16;
	record.vrr = true;
	record.vrr_zero = true;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	modes = parse_blob(blob, &count, 0, true);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_FALSE(test, modes[0].vrr);
	KUNIT_EXPECT_EQ(test, modes[0].min_vrr, 0U);
	KUNIT_EXPECT_EQ(test, modes[0].max_vrr, 0U);
	kfree(modes);

	/* A display without the internal ProMotion policy stays fixed. */
	modes = parse_blob(blob, &count, 0, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_FALSE(test, modes[0].vrr);
	KUNIT_EXPECT_EQ(test, modes[0].min_vrr, 0U);
	KUNIT_EXPECT_EQ(test, modes[0].max_vrr, 0U);
	kfree(modes);
	/* The catalog owner enables this convention only for desktop firmware. */
	for (desktop = 0; desktop < 2; desktop++) {
		struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
		u64 generation;

		KUNIT_ASSERT_NOT_NULL(test, dcp);
		parser_modes_init(dcp);
		dcp->fw_compat = desktop ? DCP_FIRMWARE_V_26_6_DESKTOP :
			DCP_FIRMWARE_V_13_5;
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
		ctx.dcp = &parser_dcp;
		generation = dcp_modes_transfer_begin(dcp);
		KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, generation), 0);
		KUNIT_ASSERT_EQ(test, dcp->nr_modes, 1U);
		KUNIT_EXPECT_EQ(test, dcp->modes[0].vrr, !!desktop);
		KUNIT_EXPECT_EQ(test, dcp->modes[0].min_vrr, desktop ? 24U << 16 : 0U);
		KUNIT_EXPECT_EQ(test, dcp->modes[0].max_vrr, desktop ? 120U << 16 : 0U);
		kfree(dcp->modes);
	}
}

static void parser_replacement(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_display_mode selected, *previous;
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;
	struct drm_display_mode requested;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	parser_modes_init(dcp);
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	requested = dcp->modes[0].mode;
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, &selected));
	KUNIT_EXPECT_EQ(test, selected.timing_mode_id, 7U);
	previous = dcp->modes;
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size - 1, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_LT(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, previous);
	KUNIT_EXPECT_EQ(test, dcp->nr_modes, 1U);
	record.id = 8;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	KUNIT_EXPECT_EQ(test, dcp->modes[0].timing_mode_id, 8U);
	KUNIT_EXPECT_EQ(test, selected.timing_mode_id, 7U);
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, &selected));
	KUNIT_EXPECT_EQ(test, selected.timing_mode_id, 8U);
	requested.clock++;
	KUNIT_EXPECT_FALSE(test, lookup_mode(dcp, &requested, NULL));
	kfree(dcp->modes);
}

/*
 * Described again after a withdrawal it was kept connected through, the same
 * display keeps its EDID; any other replacement, or a placeholder being
 * retried, drops it.
 */
static void parser_held_keeps_edid(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_connector *connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL);
	struct platform_device *pipeline = kunit_kzalloc(test, sizeof(*pipeline), GFP_KERNEL);
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;
	unsigned int pass;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	KUNIT_ASSERT_NOT_NULL(test, pipeline);
	parser_modes_init(dcp);
	apple_connector_edid_init(connector);
	apple_connector_set_pipeline(connector, pipeline);
	dcp->connector = connector;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	/* Pass 0: not held. Pass 1: held. Pass 2: held, placeholder retried. */
	for (pass = 0; pass < 3; pass++) {
		/* The cached EDID is dropped by marking it stale for get_modes(). */
		WRITE_ONCE(connector->edid_invalidation_pending, false);
		atomic_set(&dcp->external_held, pass != 0);
		dcp->placeholder_retried = pass == 2;
		KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
		ctx.dcp = &parser_dcp;
		KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx,
							dcp_modes_transfer_begin(dcp)), 0);
		if (pass == 1)
			KUNIT_EXPECT_FALSE(test, connector->edid_invalidation_pending);
		else
			KUNIT_EXPECT_TRUE(test, connector->edid_invalidation_pending);
	}
	kfree(dcp->modes);
}

static void parser_attachment_admission(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_display_mode selected, *previous;
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;
	struct drm_display_mode requested;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	parser_modes_init(dcp);
	spin_lock_init(&dcp->mode_state.lock);
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	requested = dcp->modes[0].mode;
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, &selected));
	previous = dcp->modes;
	dcp_modes_begin_attachment(dcp);
	KUNIT_EXPECT_FALSE(test, lookup_mode(dcp, &requested, NULL));
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size - 1, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_LT(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, previous);
	KUNIT_EXPECT_FALSE(test, lookup_mode(dcp, &requested, NULL));
	record.id = 8;
	record.active = 720;
	record.total = 750;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	KUNIT_EXPECT_FALSE(test, lookup_mode(dcp, &requested, NULL));
	requested = dcp->modes[0].mode;
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, &selected));
	KUNIT_EXPECT_EQ(test, selected.timing_mode_id, 8U);
	/* Link recovery changes modeset state, never catalog admission. */
	dcp_mode_invalidate(&dcp->mode_state);
	dcp_mode_hotplug(&dcp->mode_state, false, NULL);
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, &selected));
	KUNIT_EXPECT_EQ(test, selected.timing_mode_id, 8U);
	kfree(dcp->modes);
}

static void parser_stale_transfer(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;
	struct drm_display_mode requested;
	u64 old_generation, current_generation;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	parser_modes_init(dcp);
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	old_generation = dcp_modes_transfer_begin(dcp);
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, old_generation), 0);
	requested = dcp->modes[0].mode;

	/* An old transfer cannot admit a catalog on a new attachment. */
	dcp_modes_begin_attachment(dcp);
	current_generation = dcp_modes_transfer_begin(dcp);
	KUNIT_ASSERT_NE(test, old_generation, current_generation);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, old_generation), -ESTALE);
	KUNIT_EXPECT_FALSE(test, lookup_mode(dcp, &requested, NULL));

	record.id = 8;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, current_generation), 0);
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, NULL));
	KUNIT_EXPECT_EQ(test, dcp->modes[0].timing_mode_id, 8U);

	/* A late old transfer cannot overwrite an already admitted new one. */
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, old_generation), -ESTALE);
	KUNIT_EXPECT_EQ(test, dcp->modes[0].timing_mode_id, 8U);
	KUNIT_EXPECT_TRUE(test, lookup_mode(dcp, &requested, NULL));
	kfree(dcp->modes);
}

struct parser_catalog_observer {
	struct apple_connector connector;
	unsigned int notifications;
	unsigned int reasons;
	bool locked;
	struct apple_dcp *dcp;
};

static void parser_catalog_ready(struct work_struct *work)
{
	struct apple_connector *connector = container_of(work, struct apple_connector, hotplug_wq);
	struct parser_catalog_observer *observer =
		container_of(connector, struct parser_catalog_observer, connector);

	observer->notifications++;
	observer->reasons = atomic_xchg(&connector->hotplug_reasons, 0);
	observer->locked = mutex_is_locked(&observer->dcp->modes_lock);
}

static void parser_route_catalog_handoff(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct parser_catalog_observer *observer =
		kunit_kzalloc(test, sizeof(*observer), GFP_KERNEL);
	struct apple_connector *connector = observer ? &observer->connector : NULL;
	struct apple_connector *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct platform_device *pipeline = kunit_kzalloc(test, sizeof(*pipeline), GFP_KERNEL);
	struct dcp_display_mode *copy;
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;
	unsigned int count;
	u64 generation;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	KUNIT_ASSERT_NOT_NULL(test, other);
	KUNIT_ASSERT_NOT_NULL(test, pipeline);
	parser_modes_init(dcp);
	dcp->dev = &pipeline->dev;
	connector->dcp = pipeline;
	observer->dcp = dcp;
	INIT_WORK(&connector->hotplug_wq, parser_catalog_ready);
	apple_connector_edid_init(connector);
	apple_connector_edid_init(other);
	dcp->connector = connector;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, dcp_modes_transfer_begin(dcp)), 0);
	copy = dcp_modes_dup(dcp, other, &count, &generation);
	KUNIT_EXPECT_EQ(test, PTR_ERR(copy), (long)-ENODATA);
	copy = dcp_modes_dup(dcp, connector, &count, &generation);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, copy);
	KUNIT_EXPECT_EQ(test, count, 1U);
	dcp_modes_begin_attachment(dcp);
	dcp_modes_adopt(dcp, connector, generation, copy, count);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);

	copy = kmemdup(dcp->modes, sizeof(*copy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	generation = dcp_modes_transfer_begin(dcp);
	dcp_modes_adopt(dcp, other, generation, copy, 1);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);
	copy = kmemdup(dcp->modes, sizeof(*copy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	dcp_modes_adopt(dcp, connector, generation, copy, 1);
	KUNIT_EXPECT_TRUE(test, dcp->modes_admitted);
	KUNIT_EXPECT_TRUE(test, dcp->modes_provisional);

	/* Stale and rejected replacement cannot announce readiness. */
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, generation - 1), -ESTALE);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 0U);
	record.score = -1;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, generation), -EINVAL);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 0U);
	record.score = 10;
	record.id = 9;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, generation), 0);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 1U);
	KUNIT_EXPECT_NE(test, observer->reasons, 0U);
	KUNIT_EXPECT_FALSE(test, observer->locked);
	KUNIT_EXPECT_FALSE(test, dcp->modes_provisional);
	KUNIT_EXPECT_EQ(test, dcp->modes[0].timing_mode_id, 9U);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, generation), 0);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 1U);
	/* A provisional transition for a connector owned elsewhere sends no READY. */
	connector->dcp = NULL;
	dcp->modes_provisional = true;
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, generation), 0);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 1U);
	connector->dcp = pipeline;
	/* A copy arriving after the destination's catalog cannot replace its IDs. */
	copy = kmemdup(dcp->modes, sizeof(*copy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	copy->timing_mode_id = 7;
	dcp_modes_adopt(dcp, connector, generation, copy, 1);
	KUNIT_EXPECT_EQ(test, dcp->modes[0].timing_mode_id, 9U);
	kfree(dcp->modes);
}

static const struct drm_connector_funcs parser_route_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void parser_route_programming_guard(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_crtc *crtc = kunit_kzalloc(test, sizeof(*crtc), GFP_KERNEL);
	struct platform_device *pipeline = kunit_kzalloc(test, sizeof(*pipeline), GFP_KERNEL);
	struct platform_device *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct apple_connector *connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL);
	struct drm_connector_state conn_state = {};
	struct drm_crtc_state crtc_state = {};
	struct __drm_crtcs_state crtcs[1] = { { .new_state = &crtc_state } };
	struct __drm_connnectors_state connections[1] = {};
	struct drm_atomic_commit state = {
		.connectors = connections, .num_connector = 1, .crtcs = crtcs,
	};
	struct drm_encoder encoder = {};
	struct device *dev = drm_kunit_helper_alloc_device(test);
	struct drm_device *drm;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	drm = __drm_kunit_helper_alloc_drm_device(test, dev, sizeof(*drm), 0,
						  DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, crtc);
	KUNIT_ASSERT_NOT_NULL(test, pipeline);
	KUNIT_ASSERT_NOT_NULL(test, other);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	ret = drmm_connector_init(drm, &connector->base, &parser_route_connector_funcs,
				  DRM_MODE_CONNECTOR_DisplayPort, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	crtc->base.dev = drm;
	drm_atomic_helper_connector_reset(&connector->base);
	KUNIT_ASSERT_NOT_NULL(test, connector->base.state);
	connector->base.state->crtc = &crtc->base;
	crtc_state.connector_mask = drm_connector_mask(&connector->base);
	parser_modes_init(dcp);
	dcp->modes_admitted = true;
	dcp->fw_compat = DCP_FIRMWARE_V_13_5;
	dcp->nr_typec_routes = 1;
	dcp->connector = connector;
	crtc->dcp = pipeline;
	platform_set_drvdata(pipeline, dcp);
	connector->port_encoder = &encoder;
	connector->dcp = other;
	connector->connected = true;
	conn_state.crtc = &crtc->base;
	connections[0].ptr = &connector->base;
	connections[0].new_state = &conn_state;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, false));
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	connector->dcp = pipeline;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_can_retrain(&crtc->base, connector));
	connector->dcp = other;
	platform_set_drvdata(other, dcp);
	KUNIT_EXPECT_FALSE(test, dcp_crtc_can_retrain(&crtc->base, connector));
	connector->dcp = pipeline;
	dcp->modes_provisional = true;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, false));
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	dcp->modes_provisional = false;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	/* A plane-only commit omits connector entries but keeps its complete mask. */
	state.num_connector = 0;
	crtc_state.connector_mask = BIT_ULL(1);
	dcp->active_typec_route = &dcp->typec_routes[0];
	dcp->typec_crtc_off = true;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_needs_route_start(dcp));
	dcp->typec_follow_start = true;
	dcp->typec_generation = 5;
	dcp->typec_follow_gen = 5;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_needs_route_start(dcp));
	dcp->typec_follow_gen = 4;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_needs_route_start(dcp));
	dcp->typec_follow_gen = 5;
	/* A moved Thunderbolt tunnel starts its link as a direct route does. */
	dcp->active_typec_route->tunnel = true;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_needs_route_start(dcp));
	dcp->active_typec_route->tunnel = false;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	crtc_state.connector_mask = 0;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	crtc_state.connector_mask = BIT_ULL(0);
	dcp->modes_admitted = false;
	dcp->typec_follow_start = false;
	/* Paired reattachment keeps baseline power-on while lookup rejects modes. */
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	dcp->typec_follow_start = true;
	/* A current handoff must acquire its own admitted destination catalog. */
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	dcp->modes_admitted = true;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	dcp->active_typec_route = NULL;
	dcp->typec_follow_start = false;
	connector->dcp = other;
	KUNIT_EXPECT_FALSE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	connector->dcp = pipeline;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	state.num_connector = 1;
	/* An unrouted disconnected resume keeps its existing deferred path. */
	connector->dcp = NULL;
	connector->connected = false;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, false));
	/* Native/14.7 continues to use its existing atomic machinery. */
	dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	connector->dcp = other;
	KUNIT_EXPECT_TRUE(test, dcp_crtc_route_ready(&crtc->base, &state, true));
	connector->base.state->crtc = NULL;
}

static void parser_route_invalid_own_mode(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_crtc *crtc = kunit_kzalloc(test, sizeof(*crtc), GFP_KERNEL);
	struct platform_device *pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	struct drm_crtc_state crtc_state = { .enable = true, .mode_changed = true };
	struct __drm_crtcs_state crtcs[1] = { { .new_state = &crtc_state } };
	struct drm_atomic_commit state = { .crtcs = crtcs };

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, crtc);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	parser_modes_init(dcp);
	dcp->fw_compat = DCP_FIRMWARE_V_13_5;
	dcp->nr_typec_routes = 1;
	crtc->dcp = pdev;
	platform_set_drvdata(pdev, dcp);
	/* No crossing selected: a fixed/paired output still needs its own mode. */
	KUNIT_EXPECT_TRUE(test, dcp_crtc_mode_fixup(&crtc->base, &crtc_state.mode, NULL));
	KUNIT_EXPECT_EQ(test, dcp_crtc_atomic_check(&crtc->base, &state), -EINVAL);
}

static void parser_connector_ownership(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_connector *fixed = kunit_kzalloc(test, sizeof(*fixed), GFP_KERNEL);
	struct apple_connector *borrowed = kunit_kzalloc(test, sizeof(*borrowed), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, fixed);
	KUNIT_ASSERT_NOT_NULL(test, borrowed);
	parser_modes_init(dcp);
	mutex_lock(&dcp->modes_lock);
	dcp->connector = borrowed;
	dcp->modes_admitted = true;
	/* Fixed outputs retain their pipeline even when probing is forced. */
	fixed->base.force = DRM_FORCE_ON;
	KUNIT_EXPECT_FALSE(test, dcp_modes_for_connector(dcp, fixed));
	KUNIT_EXPECT_TRUE(test, dcp_modes_for_connector(dcp, borrowed));
	dcp->connector = fixed;
	dcp->modes_admitted = false;
	KUNIT_EXPECT_FALSE(test, dcp_modes_for_connector(dcp, fixed));
	dcp->modes_admitted = true;
	KUNIT_EXPECT_TRUE(test, dcp_modes_for_connector(dcp, fixed));
	KUNIT_EXPECT_FALSE(test, dcp_modes_for_connector(dcp, borrowed));
	mutex_unlock(&dcp->modes_lock);
}

static void parser_route_retirement(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_connector *fixed = kunit_kzalloc(test, sizeof(*fixed), GFP_KERNEL);
	struct apple_dcp_typec_route *route = kunit_kzalloc(test, 1, GFP_KERNEL);
	struct apple_dcp_typec_route *other = kunit_kzalloc(test, 1, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, fixed);
	KUNIT_ASSERT_NOT_NULL(test, route);
	KUNIT_ASSERT_NOT_NULL(test, other);
	parser_modes_init(dcp);
	dcp->connector = fixed;
	dcp->active_typec_route = route;
	dcp->modes_admitted = true;
	KUNIT_EXPECT_FALSE(test, dcp_modes_end_typec(dcp, other));
	KUNIT_EXPECT_EQ(test, dcp_modes_transfer_begin(dcp), 0ULL);
	KUNIT_EXPECT_TRUE(test, dcp_modes_end_typec(dcp, route));
	KUNIT_EXPECT_EQ(test, dcp_modes_transfer_begin(dcp), 1ULL);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);
	dcp->active_typec_route = NULL;
	KUNIT_EXPECT_FALSE(test, dcp_modes_end_typec(dcp, route));
	KUNIT_EXPECT_EQ(test, dcp_modes_transfer_begin(dcp), 1ULL);
	dcp_modes_release(dcp);
}

static void parser_rejected_replacement(struct kunit *test)
{
	struct parser_record record = valid_record(7, 10);
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_display_mode *previous;
	struct dcp_parse_ctx ctx;
	struct parser_blob *blob;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	parser_modes_init(dcp);
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, 0), 0);
	previous = dcp->modes;
	record.missing = MISSING_SCORE;
	blob = make_blob(test, &record, 1);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, 0), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, previous);
	KUNIT_EXPECT_EQ(test, dcp->nr_modes, 1U);
	KUNIT_EXPECT_TRUE(test, dcp->modes_admitted);
	dcp_modes_begin_attachment(dcp);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_EXPECT_EQ(test, dcp_modes_replace(dcp, &ctx, 1), -EINVAL);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);
	blob = make_blob(test, &record, 0);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	KUNIT_ASSERT_EQ(test, dcp_modes_replace(dcp, &ctx, 1), 0);
	KUNIT_EXPECT_EQ(test, dcp->nr_modes, 0U);
	KUNIT_EXPECT_TRUE(test, dcp->modes_admitted);
	dcp_modes_release(dcp);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, NULL);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);
}

static void parser_attribute_epoch(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_parse_ctx ctx;
	u64 generation;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	parser_modes_init(dcp);
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 1, 3);
	blob_int(blob, "MaxHorizontalImageSize", 52);
	blob_int(blob, "MaxVerticalImageSize", 29);
	blob_bool(blob, "SupportsBacklightControl", true);
	generation = dcp_modes_transfer_begin(dcp);
	dcp_modes_begin_attachment(dcp);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_EXPECT_EQ(test, dcp_attributes_replace(dcp, &ctx, generation), -ESTALE);
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 0);
	KUNIT_EXPECT_FALSE(test, dcp->ext_backlight);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_ASSERT_EQ(test, dcp_attributes_replace(dcp, &ctx, 1), 0);
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 520);
	KUNIT_EXPECT_EQ(test, dcp->height_mm, 290);
	KUNIT_EXPECT_TRUE(test, dcp->ext_backlight);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_EXPECT_EQ(test, dcp_attributes_replace(dcp, &ctx, generation), -ESTALE);
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 520);
	dcp_modes_release(dcp);
}

static void parser_attributes_malformed(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_parse_ctx ctx;
	const s64 invalid[] = { -1, INT_MAX / 10 + 1, S64_MAX };
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	parser_modes_init(dcp);
	dcp->width_mm = 520;
	dcp->height_mm = 290;
	dcp->ext_backlight = true;
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 2, 0);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_EXPECT_LT(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	KUNIT_ASSERT_EQ(test, parse(blob->data, 4, &ctx), 0);
	KUNIT_EXPECT_LT(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	blob->size = 4;
	blob_tag(blob, 1, 1);
	blob_int(blob, "Unknown", 1);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size - 1, &ctx), 0);
	KUNIT_EXPECT_LT(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	for (i = 0; i < ARRAY_SIZE(invalid); ++i) {
		blob->size = 4;
		blob_tag(blob, 1, 1);
		blob_int(blob, "MaxHorizontalImageSize", invalid[i]);
		KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
		KUNIT_EXPECT_LT(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	}
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 520);
	KUNIT_EXPECT_EQ(test, dcp->height_mm, 290);
	KUNIT_EXPECT_TRUE(test, dcp->ext_backlight);
	dcp_modes_release(dcp);
}

static void parser_optional_flag_extent(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_parse_ctx ctx;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	parser_modes_init(dcp);
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 1, 3);
	blob_int(blob, "MaxHorizontalImageSize", 52);
	blob_int(blob, "MaxVerticalImageSize", 29);
	blob_bool(blob, "SupportsBacklightControl", true);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_ASSERT_EQ(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	KUNIT_EXPECT_TRUE(test, dcp->ext_backlight);

	blob->size = 4;
	blob_tag(blob, 1, 3);
	blob_int(blob, "MaxHorizontalImageSize", 60);
	blob_int(blob, "MaxVerticalImageSize", 40);
	blob_key(blob, "SupportsBacklightControl");
	blob_tag(blob, 9, 2);
	blob->data[blob->size++] = 'x';
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_EXPECT_LT(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 520);
	KUNIT_EXPECT_EQ(test, dcp->height_mm, 290);
	KUNIT_EXPECT_TRUE(test, dcp->ext_backlight);

	/* A complete value of another type keeps the optional false fallback. */
	blob->data[blob->size++] = 'y';
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_ASSERT_EQ(test, dcp_attributes_replace(dcp, &ctx, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp->width_mm, 600);
	KUNIT_EXPECT_EQ(test, dcp->height_mm, 400);
	KUNIT_EXPECT_FALSE(test, dcp->ext_backlight);
	dcp_modes_release(dcp);
}

static void parser_attribute_deferred(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct parser_catalog_observer *observer =
		kunit_kzalloc(test, sizeof(*observer), GFP_KERNEL);
	struct apple_connector *connector = observer ? &observer->connector : NULL;
	struct drm_device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_parse_ctx ctx;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_ASSERT_NOT_NULL(test, blob);
	parser_modes_init(dcp);
	observer->dcp = dcp;
	INIT_WORK(&connector->hotplug_wq, parser_catalog_ready);
	mutex_init(&dev->mode_config.mutex);
	connector->base.dev = dev;
	apple_connector_backlight_init(connector);
	dcp->connector = connector;
	dcp->panel.width_mm = 600;
	dcp->modes = kmalloc_obj(*dcp->modes);
	KUNIT_ASSERT_NOT_NULL(test, dcp->modes);
	dcp->nr_modes = 1;
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 1, 2);
	blob_int(blob, "MaxHorizontalImageSize", 52);
	blob_int(blob, "MaxVerticalImageSize", 29);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	enable_work(&dcp->dimensions_wq);
	/* RX must return while a probe holds the DRM lock awaiting a reply. */
	mutex_lock(&dev->mode_config.mutex);
	ret = dcp_attributes_replace(dcp, &ctx, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, dcp->modes[0].mode.width_mm, 520);
	mutex_unlock(&dev->mode_config.mutex);
	flush_work(&dcp->dimensions_wq);
	flush_work(&connector->hotplug_wq);
	KUNIT_EXPECT_EQ(test, observer->notifications, 1U);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 520U);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.height_mm, 290U);
	dcp_modes_release(dcp);
	cancel_work_sync(&connector->hotplug_wq);
	cancel_work_sync(&connector->bl_sync_wq);
	apple_connector_backlight_early_unregister(connector);
}

static void parser_dimensions_lifetime(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_connector *connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL);
	struct drm_device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	parser_modes_init(dcp);
	mutex_init(&dev->mode_config.mutex);
	connector->base.dev = dev;
	dcp->connector = connector;
	dcp->width_mm = 520;
	dcp->height_mm = 290;
	KUNIT_EXPECT_FALSE(test, schedule_work(&dcp->dimensions_wq));
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 0U);
	enable_work(&dcp->dimensions_wq);
	schedule_work(&dcp->dimensions_wq);
	flush_work(&dcp->dimensions_wq);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 520U);
	disable_work_sync(&dcp->dimensions_wq);
	dcp->width_mm = 600;
	KUNIT_EXPECT_FALSE(test, schedule_work(&dcp->dimensions_wq));
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 520U);
	enable_work(&dcp->dimensions_wq);
	schedule_work(&dcp->dimensions_wq);
	flush_work(&dcp->dimensions_wq);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 600U);
	dcp_modes_release(dcp);
	KUNIT_EXPECT_FALSE(test, schedule_work(&dcp->dimensions_wq));
}

static void parser_catalog_release(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	parser_modes_init(dcp);
	dcp->modes = kmalloc_obj(*dcp->modes);
	KUNIT_ASSERT_NOT_NULL(test, dcp->modes);
	dcp->nr_modes = 1;
	dcp->modes_admitted = true;
	dcp_modes_release(dcp);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, NULL);
	KUNIT_EXPECT_EQ(test, dcp->nr_modes, 0U);
	KUNIT_EXPECT_FALSE(test, dcp->modes_admitted);
	dcp_modes_release(dcp);
	KUNIT_EXPECT_PTR_EQ(test, dcp->modes, NULL);
}

static void parser_property_bounds(struct kunit *test)
{
	char key[64];

	memset(key, 'x', sizeof(key));
	KUNIT_EXPECT_FALSE(test, dcp_property_key_valid(key, sizeof(key)));
	key[63] = '\0';
	KUNIT_EXPECT_TRUE(test, dcp_property_key_valid(key, sizeof(key)));

	KUNIT_EXPECT_FALSE(test, dcp_property_size_valid(0));
	KUNIT_EXPECT_TRUE(test, dcp_property_size_valid(SZ_16M));
	KUNIT_EXPECT_FALSE(test, dcp_property_size_valid(SZ_16M + 1));
	KUNIT_EXPECT_TRUE(test, dcp_property_chunk_valid(8192, 4096, 4096, 4096));
	KUNIT_EXPECT_FALSE(test, dcp_property_chunk_valid(8192, 0, 4097, 4096));
	KUNIT_EXPECT_FALSE(test, dcp_property_chunk_valid(8192, U32_MAX - 2, 4, 4096));
	KUNIT_EXPECT_FALSE(test, dcp_property_chunk_valid(8192, 8190, 4, 4096));
}

static void parser_route_holder_snapshot(struct kunit *test)
{
	struct apple_dcp *back = kunit_kzalloc(test, sizeof(*back), GFP_KERNEL);
	struct apple_connector *holder = kunit_kzalloc(test, sizeof(*holder), GFP_KERNEL);
	struct drm_crtc *target = kunit_kzalloc(test, sizeof(*target), GFP_KERNEL);
	struct drm_crtc *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct drm_connector_state live = { .crtc = target };
	struct drm_connector_state reserved = {};
	struct __drm_connnectors_state conns[1] = {};
	struct drm_atomic_commit state = { .connectors = conns, .num_connector = 1 };

	KUNIT_ASSERT_NOT_NULL(test, back);
	KUNIT_ASSERT_NOT_NULL(test, holder);
	KUNIT_ASSERT_NOT_NULL(test, target);
	KUNIT_ASSERT_NOT_NULL(test, other);
	holder->base.state = &live;
	/* An absent holder snapshot is refused even if live state looks inactive. */
	KUNIT_EXPECT_FALSE(test, dcp_typec_follow_holder_off(&state, target, holder, back));
	conns[0].new_state = &reserved;
	/* The reserved off holder is retained despite a future live assignment. */
	KUNIT_EXPECT_TRUE(test, dcp_typec_follow_holder_off(&state, target, holder, back));
	reserved.crtc = target;
	live.crtc = NULL;
	KUNIT_EXPECT_FALSE(test, dcp_typec_follow_holder_off(&state, target, holder, back));
	reserved.crtc = other;
	back->crtc = to_apple_crtc(other);
	KUNIT_EXPECT_TRUE(test, dcp_typec_follow_holder_off(&state, target, holder, back));
}

static void parser_route_prepare_scope(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	struct apple_dcp_typec_route *route = kunit_kzalloc(test, sizeof(*route), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_ASSERT_NOT_NULL(test, route);
	mutex_init(&dcp->modes_lock);
	dcp->dev = dev;
	dcp->fw_compat = DCP_FIRMWARE_V_13_5;
	dcp->nr_typec_routes = 1;
	dcp->active_typec_route = route;
	dcp->typec_crtc_off = true;
	/* No PHY would fail strict preparation; a paired wake must bypass it. */
	KUNIT_EXPECT_EQ(test, dcp_typec_prepare_route(dcp), 0);
	dcp->typec_follow_start = true;
	dcp->typec_follow_gen = dcp->typec_generation;
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->tb_lock);
	KUNIT_EXPECT_EQ(test, dcp_typec_prepare_route(dcp), -ENODEV);
	mutex_destroy(&dcp->tb_lock);
	mutex_destroy(&dcp->hpd_mutex);
	mutex_destroy(&dcp->modes_lock);
}

static void parser_panel_identity(struct kunit *test)
{
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);
	struct dcp_display_metadata metadata;
	struct dcp_parse_ctx ctx;
	const struct drm_edid *edid;
	const struct edid *raw;
	struct dcp_display_mode modes[2] = {};

	modes[0].mode = (struct drm_display_mode) {
		.clock = 540005, .hdisplay = 3456, .hsync_start = 3500,
		.hsync_end = 3550, .htotal = 4000, .vdisplay = 2160,
		.vsync_start = 2165, .vsync_end = 2170, .vtotal = 2250,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	};
	modes[1] = modes[0];
	modes[1].mode.clock = 1080005;
	modes[1].vrr = true;
	modes[1].min_vrr = 24U << 16;
	modes[1].max_vrr = 120U << 16;

	KUNIT_ASSERT_NOT_NULL(test, blob);
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 1, 1);
	blob_key(blob, "ProductAttributes");
	blob_tag(blob, 1, 4);
	blob_key(blob, "ManufacturerID");
	blob_key(blob, "TST");
	blob_key(blob, "ProductName");
	blob_key(blob, "Synthetic Internal Panel");
	blob_int(blob, "ProductID", 42);
	blob_int(blob, "SerialNumber", 0x12345678);
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_panel_metadata(&ctx, &metadata), 0);
	KUNIT_EXPECT_STREQ(test, metadata.name, "Synthetic Int");
	edid = dcp_panel_edid(&metadata, NULL, modes, ARRAY_SIZE(modes), 345, 223);
	KUNIT_ASSERT_FALSE(test, IS_ERR(edid));
	KUNIT_EXPECT_TRUE(test, drm_edid_valid(edid));
	raw = drm_edid_raw(edid);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(raw->product_id.product_code), 42);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(raw->product_id.serial_number), 0x12345678U);
	/* No colour target: nominal D65 white, no primaries, undefined gamma. */
	KUNIT_EXPECT_EQ(test, raw->gamma, 0xff);
	KUNIT_EXPECT_EQ(test, raw->red_x, 0);
	KUNIT_EXPECT_EQ(test, raw->green_y, 0);
	KUNIT_EXPECT_EQ(test, raw->white_x, 320 >> 2);
	KUNIT_EXPECT_EQ(test, raw->white_y, 337 >> 2);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(raw->detailed_timings[0].pixel_clock), 54001);
	KUNIT_EXPECT_EQ(test, raw->detailed_timings[3].data.other_data.data.range.min_vfreq, 24);
	KUNIT_EXPECT_EQ(test, raw->detailed_timings[3].data.other_data.data.range.max_vfreq, 120);
	KUNIT_EXPECT_EQ(test, modes[0].mode.clock, 540005);
	drm_edid_free(edid);
}

static const struct dcp_display_mode panel_color_mode = {
	.mode = {
		.clock = 540005, .hdisplay = 3456, .hsync_start = 3500,
		.hsync_end = 3550, .htotal = 4000, .vdisplay = 2160,
		.vsync_start = 2165, .vsync_end = 2170, .vtotal = 2250,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	},
};

static const struct edid *panel_color_edid(struct kunit *test,
					   const struct dcp_display_metadata *metadata,
					   const struct dcp_color_space *target)
{
	const struct drm_edid *edid;
	struct edid *copy;

	edid = dcp_panel_edid(metadata, target, &panel_color_mode, 1, 345, 223);
	KUNIT_ASSERT_FALSE(test, IS_ERR(edid));
	KUNIT_EXPECT_TRUE(test, drm_edid_valid(edid));
	copy = kunit_kzalloc(test, sizeof(*copy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	memcpy(copy, drm_edid_raw(edid), sizeof(*copy));
	drm_edid_free(edid);
	return copy;
}

/* 10-bit EDID chromaticity: high eight bits plus two low bits. */
static u32 panel_color_coord(const struct edid *raw, unsigned int i)
{
	const u8 hi[] = { raw->red_x, raw->red_y, raw->green_x, raw->green_y,
			  raw->blue_x, raw->blue_y, raw->white_x, raw->white_y };
	u8 lo = i < 4 ? raw->red_green_lo : raw->blue_white_lo;

	return hi[i] << 2 | (lo >> (6 - 2 * (i % 4)) & 3);
}

static void parser_panel_color_target(struct kunit *test)
{
	/* Display P3 and sRGB primaries, then D65, in units of 1/1024. */
	static const u32 p3[] = { 696, 328, 271, 707, 154, 61, 320, 337 };
	static const u32 srgb[] = { 655, 338, 307, 614, 154, 61, 320, 337 };
	struct dcp_display_metadata metadata = {
		.manufacturer = "APP", .product = 0x616, .name = "Apple Panel",
	};
	const struct edid *raw;

	KUNIT_EXPECT_TRUE(test, IS_ERR(dcp_color_space_lookup("adobe-rgb")));
	KUNIT_EXPECT_TRUE(test, IS_ERR(dcp_color_space_lookup("")));
	KUNIT_EXPECT_TRUE(test, IS_ERR(dcp_color_space_lookup(NULL)));

	/* No firmware chromaticity: the named target supplies P3, D65 and 2.2. */
	raw = panel_color_edid(test, &metadata, dcp_color_space_lookup("display-p3"));
	for (unsigned int i = 0; i < ARRAY_SIZE(p3); i++)
		KUNIT_EXPECT_EQ(test, panel_color_coord(raw, i), p3[i]);
	KUNIT_EXPECT_EQ(test, raw->gamma, 120);
	raw = panel_color_edid(test, &metadata, dcp_color_space_lookup("srgb"));
	for (unsigned int i = 0; i < ARRAY_SIZE(srgb); i++)
		KUNIT_EXPECT_EQ(test, panel_color_coord(raw, i), srgb[i]);
	KUNIT_EXPECT_EQ(test, raw->gamma, 120);

	/* Incomplete firmware colour data falls back to the target. */
	metadata.chromaticity[0][0] = 44608;
	raw = panel_color_edid(test, &metadata, dcp_color_space_lookup("display-p3"));
	KUNIT_EXPECT_EQ(test, panel_color_coord(raw, 1), p3[1]);

	/* Complete firmware chromaticities win over the target. */
	metadata = (struct dcp_display_metadata) {
		.manufacturer = "APP", .product = 0x616, .name = "Apple Panel",
		.chromaticity = { { 44608, 20288 }, { 16960, 40448 }, { 9344, 5184 } },
		.white = { 20544, 21568 },
	};
	raw = panel_color_edid(test, &metadata, dcp_color_space_lookup("display-p3"));
	KUNIT_EXPECT_EQ(test, panel_color_coord(raw, 0), 697U);
	KUNIT_EXPECT_EQ(test, panel_color_coord(raw, 1), 317U);
	KUNIT_EXPECT_EQ(test, panel_color_coord(raw, 6), 321U);
	KUNIT_EXPECT_EQ(test, raw->gamma, 0xff);
}

static size_t parser_adt_property(u8 *out, size_t capacity, const char *name,
				  const void *value, u32 size)
{
	size_t padded = ALIGN((size_t)size, 4);

	if (strnlen(name, 32) >= 32 || capacity < 36 + padded)
		return 0;
	memset(out, 0, 36 + padded);
	strscpy((char *)out, name, 32);
	put_unaligned_le32(size, out + 32);
	memcpy(out + 36, value, size);
	return 36 + padded;
}

static void parser_adt_synthetic_matrix(__le32 matrix[10], s32 offset)
{
	static const s32 identity[9] = {
		1 << 24, 0, 0, 0, 1 << 24, 0, 0, 0, 1 << 24,
	};

	matrix[0] = cpu_to_le32(1);
	for (unsigned int i = 0; i < ARRAY_SIZE(identity); i++)
		matrix[i + 1] = cpu_to_le32(identity[i]);
	/* A deliberately synthetic row-neutral correction, not panel data. */
	matrix[1] = cpu_to_le32((1 << 24) + offset);
	matrix[2] = cpu_to_le32(-offset);
}

static size_t parser_adt_color_fixture(u8 *out, size_t capacity,
				       const void *gamut, u32 gamut_size,
				       const void *matrix, u32 matrix_size)
{
	const char root[] = "device-tree", product[] = "product";
	size_t pos = 0, used;

	if (capacity < 8)
		return 0;
	put_unaligned_le32(1, out);
	put_unaligned_le32(1, out + 4);
	pos = 8;
	used = parser_adt_property(out + pos, capacity - pos, "name", root, sizeof(root));
	if (!used || capacity - pos - used < 8)
		return 0;
	pos += used;
	put_unaligned_le32(3, out + pos);
	put_unaligned_le32(0, out + pos + 4);
	pos += 8;
	used = parser_adt_property(out + pos, capacity - pos, "name", product,
				   sizeof(product));
	if (!used)
		return 0;
	pos += used;
	used = parser_adt_property(out + pos, capacity - pos, "artwork-display-gamut",
				   gamut, gamut_size);
	if (!used)
		return 0;
	pos += used;
	used = parser_adt_property(out + pos, capacity - pos,
				   "primary-calibration-matrix", matrix, matrix_size);
	return used ? pos + used : 0;
}

static void parser_panel_adt_color_target(struct kunit *test)
{
	static const u32 p3[] = { 696, 328, 271, 707, 154, 61, 320, 337 };
	static const u32 srgb[] = { 655, 338, 307, 614, 154, 61, 320, 337 };
	struct dcp_display_metadata metadata = {
		.manufacturer = "APP", .product = 0x616, .name = "Apple Panel",
	};
	const struct dcp_color_space *target;
	struct dcp_panel_adt panel = {};
	__le32 matrix[10];
	u8 adt[512];
	size_t size;
	const struct edid *raw;

	parser_adt_synthetic_matrix(matrix, 1 << 20);
	size = parser_adt_color_fixture(adt, sizeof(adt), "P3", sizeof("P3"),
					matrix, sizeof(matrix));
	KUNIT_ASSERT_GT(test, size, (size_t)0);
	KUNIT_ASSERT_EQ(test, dcp_panel_adt_parse(adt, size, &panel), 0);
	KUNIT_EXPECT_EQ(test, panel.gamut, DCP_PANEL_GAMUT_DISPLAY_P3);
	target = dcp_panel_gamut_color_space(panel.gamut);
	KUNIT_ASSERT_NOT_NULL(test, target);
	raw = panel_color_edid(test, &metadata, target);
	for (unsigned int i = 0; i < ARRAY_SIZE(p3); i++)
		KUNIT_EXPECT_EQ(test, panel_color_coord(raw, i), p3[i]);

	parser_adt_synthetic_matrix(matrix, -(1 << 19));
	size = parser_adt_color_fixture(adt, sizeof(adt), "sRGB", sizeof("sRGB"),
					matrix, sizeof(matrix));
	panel = (struct dcp_panel_adt) {};
	KUNIT_ASSERT_EQ(test, dcp_panel_adt_parse(adt, size, &panel), 0);
	KUNIT_EXPECT_EQ(test, panel.gamut, DCP_PANEL_GAMUT_SRGB);
	target = dcp_panel_gamut_color_space(panel.gamut);
	KUNIT_ASSERT_NOT_NULL(test, target);
	raw = panel_color_edid(test, &metadata, target);
	for (unsigned int i = 0; i < ARRAY_SIZE(srgb); i++)
		KUNIT_EXPECT_EQ(test, panel_color_coord(raw, i), srgb[i]);

	/* Unknown, malformed, short, wrong-version, unbounded, non-neutral or singular. */
	for (unsigned int fault = 0; fault < 7; fault++) {
		const char *name = fault == 0 ? "AdobeRGB" : "P3";
		u32 name_size = fault == 1 ? 2 : strlen(name) + 1;
		u32 matrix_size = fault == 2 ? sizeof(matrix) - 4 : sizeof(matrix);

		parser_adt_synthetic_matrix(matrix, 1 << 20);
		if (fault == 3)
			matrix[0] = cpu_to_le32(2);
		if (fault == 4) {
			matrix[1] = cpu_to_le32(5 << 24);
			matrix[2] = cpu_to_le32(-(4 << 24));
		}
		if (fault == 5)
			matrix[2] = 0;
		if (fault == 6) {
			matrix[4] = matrix[1];
			matrix[5] = matrix[2];
			matrix[6] = matrix[3];
		}
		size = parser_adt_color_fixture(adt, sizeof(adt), name, name_size,
						matrix, matrix_size);
		panel = (struct dcp_panel_adt) { .gamut = DCP_PANEL_GAMUT_DISPLAY_P3 };
		KUNIT_ASSERT_EQ(test, dcp_panel_adt_parse(adt, size, &panel), 0);
		KUNIT_EXPECT_EQ(test, panel.gamut, DCP_PANEL_GAMUT_UNSPECIFIED);
	}

	/* Every truncation is rejected transactionally. */
	parser_adt_synthetic_matrix(matrix, 1 << 20);
	size = parser_adt_color_fixture(adt, sizeof(adt), "P3", sizeof("P3"),
					matrix, sizeof(matrix));
	for (size_t cut = 0; cut < size; cut++) {
		struct dcp_panel_adt before;

		panel = (struct dcp_panel_adt) { .identity = { .manufacturer = "TST" } };
		before = panel;
		KUNIT_EXPECT_LT(test, dcp_panel_adt_parse(adt, cut, &panel), 0);
		KUNIT_EXPECT_MEMEQ(test, &panel, &before, sizeof(panel));
	}
}

static void parser_panel_identity_stable(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_panel_identity *id = kunit_kzalloc(test, sizeof(*id), GFP_KERNEL);
	struct dcp_display_metadata metadata = {
		.manufacturer = "TST", .name = "Synthetic", .product = 42, .serial = 1,
	};
	u64 generation;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, id);
	dcp->panel_identity = id;
	dcp->fw_compat = DCP_FIRMWARE_V_26_6_DESKTOP;
	dcp->panel.width_mm = 345;
	spin_lock_init(&dcp->metadata.lock);
	dcp_panel_metadata_replace(dcp, &metadata);
	generation = id->generation;
	KUNIT_EXPECT_TRUE(test, dcp_panel_metadata_current(dcp, generation));
	dcp_panel_metadata_replace(dcp, NULL);
	dcp_panel_metadata_replace(dcp, &metadata);
	{
		struct dcp_display_metadata partial = { .manufacturer = "TST", .product = 42 };

		dcp_panel_metadata_replace(dcp, &partial);
	}
	KUNIT_EXPECT_TRUE(test, dcp_panel_metadata_current(dcp, generation));
	KUNIT_EXPECT_EQ(test, id->metadata.serial, 1U);
	metadata.serial++;
	dcp_panel_metadata_replace(dcp, &metadata);
	KUNIT_EXPECT_FALSE(test, dcp_panel_metadata_current(dcp, generation));
	KUNIT_EXPECT_EQ(test, id->metadata.serial, 2U);
}

static void parser_identity_emit(void *data, const char *key, u32 len,
				 u32 type, u32 size, u32 depth)
{
	u32 *seen = data;

	(*seen)++;
}

static void parser_identity_bounds(struct kunit *test)
{
	u32 seen = 0;
	__le32 huge[] = { cpu_to_le32(0xd3), cpu_to_le32(0x01ffffff) };
	int ret;

	ret = parse_identity_keys(huge, sizeof(huge), parser_identity_emit, &seen);
	KUNIT_EXPECT_EQ(test, ret, -E2BIG);
	KUNIT_EXPECT_EQ(test, seen, 1U);
	for (u32 len = 0; len < sizeof(huge); len++) {
		ret = parse_identity_keys(huge, len, parser_identity_emit, &seen);
		KUNIT_EXPECT_LT(test, ret, 0);
	}
}

static void parser_adt_bounds(struct kunit *test)
{
	struct dcp_panel_adt panel = { .identity = { .manufacturer = "APP" } };
	struct dcp_panel_adt before = panel;
	__le32 huge[] = { cpu_to_le32(4097), 0 };

	KUNIT_EXPECT_EQ(test, dcp_panel_adt_parse(huge, sizeof(huge), &panel), -E2BIG);
	KUNIT_EXPECT_MEMEQ(test, &panel, &before, sizeof(panel));
	KUNIT_EXPECT_EQ(test, dcp_panel_adt_parse(NULL, 0, &panel), -EINVAL);
}

static void parser_panel_of_dimensions(struct kunit *test)
{
	__be32 width = cpu_to_be32(345), height = cpu_to_be32(223);
	__be32 adjusted = cpu_to_be32(216);
	struct property adj = { .name = "adj-height-mm", .length = 4, .value = &adjusted };
	struct property h = { .name = "height-mm", .length = 4, .value = &height };
	struct property w = { .name = "width-mm", .length = 4, .value = &width, .next = &h };
	struct device_node node = { .properties = &w };
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->notch_height = 74;
	/* A missing optional adj-height-mm must not erase the full height. */
	dcp_panel_read_dimensions(dcp, &node);
	KUNIT_EXPECT_EQ(test, dcp->panel.width_mm, 345U);
	KUNIT_EXPECT_EQ(test, dcp->panel.height_mm, 223U);
	h.next = &adj;
	dcp_panel_read_dimensions(dcp, &node);
	KUNIT_EXPECT_EQ(test, dcp->panel.height_mm, 216U);
	dcp->notch_height = 0;
	dcp_panel_read_dimensions(dcp, &node);
	KUNIT_EXPECT_EQ(test, dcp->panel.height_mm, 223U);
}

/* Publish a panel's ADT identity through dcp_get_modes() on a firmware profile. */
static void parser_panel_publication(struct kunit *test,
				     enum dcp_firmware_version fw_compat, bool desktop_state)
{
	const char *gamut = fw_compat == DCP_FIRMWARE_H17P ? "sRGB" : "P3";
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_desktop_state *desktop = kunit_kzalloc(test, sizeof(*desktop), GFP_KERNEL);
	struct dcp_panel_identity *id = kunit_kzalloc(test, sizeof(*id), GFP_KERNEL);
	struct platform_device *pipeline = kunit_kzalloc(test, sizeof(*pipeline), GFP_KERNEL);
	struct apple_connector *connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL);
	struct dcp_panel_adt panel = {
		.identity = { .manufacturer = "APP", .product = 0x616, .name = "Apple Panel" },
		.width_mm = 345, .height_mm = 216,
	};
	struct dcp_panel_adt before = panel;
	__le32 matrix[10];
	u8 adt[512];
	size_t adt_size;
	struct device *dev = drm_kunit_helper_alloc_device(test);
	struct drm_device *drm;
	struct drm_display_mode *published;
	struct dcp_display_mode mode = panel_test_mode;
	const struct edid *raw;
	int count, ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	drm = __drm_kunit_helper_alloc_drm_device(test, dev, sizeof(*drm), 0,
						  DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, desktop);
	KUNIT_ASSERT_NOT_NULL(test, id);
	KUNIT_ASSERT_NOT_NULL(test, pipeline);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	ret = drmm_connector_init(drm, &connector->base, &parser_route_connector_funcs,
				  DRM_MODE_CONNECTOR_eDP, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	parser_adt_synthetic_matrix(matrix, 1 << 20);
	adt_size = parser_adt_color_fixture(adt, sizeof(adt), gamut, strlen(gamut) + 1,
					    matrix, sizeof(matrix));
	KUNIT_ASSERT_GT(test, adt_size, (size_t)0);
	ret = dcp_panel_adt_parse(adt, adt_size, &panel);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* /product's target metadata is not panel identity; no panel node exists. */
	KUNIT_EXPECT_FALSE(test, panel.found);
	KUNIT_EXPECT_MEMEQ(test, &panel.identity, &before.identity, sizeof(panel.identity));
	KUNIT_EXPECT_EQ(test, panel.width_mm, before.width_mm);
	KUNIT_EXPECT_EQ(test, panel.height_mm, before.height_mm);
	parser_modes_init(dcp);
	mutex_init(&dcp->dcpavserv.lock);
	mutex_init(&dcp->hpd_mutex);
	dcp->dev = dev;
	dcp->desktop = desktop_state ? desktop : NULL;
	dcp->panel_identity = id;
	dcp->fw_compat = fw_compat;
	dcp->panel.width_mm = panel.width_mm;
	dcp->panel.height_mm = panel.height_mm;
	dcp->panel.color_target = dcp_panel_gamut_color_space(panel.gamut);
	id->adt = panel.identity;
	dcp->connector = connector;
	dcp->modes = &mode;
	dcp->nr_modes = 1;
	dcp->modes_admitted = true;
	connector->dcp = pipeline;
	connector->edid_live = true;
	platform_set_drvdata(pipeline, dcp);

	/* Probe through the real iomfb publication path and DRM blob allocator. */
	mutex_lock(&drm->mode_config.mutex);
	count = dcp_get_modes(&connector->base);
	mutex_unlock(&drm->mode_config.mutex);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_ASSERT_NOT_NULL(test, connector->base.edid_blob_ptr);
	KUNIT_EXPECT_EQ(test, connector->base.edid_blob_ptr->length, (size_t)128);
	KUNIT_EXPECT_TRUE(test, connector->drm_edid_firmware_derived);
	raw = connector->base.edid_blob_ptr->data;
	KUNIT_EXPECT_EQ(test, panel_color_coord(raw, 0),
			fw_compat == DCP_FIRMWARE_H17P ? 655U : 696U);
	KUNIT_EXPECT_EQ(test, raw->gamma, 120);
	KUNIT_EXPECT_MEMEQ(test, raw->detailed_timings[1].data.other_data.data.str.str,
			   "Apple Panel", 11);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.width_mm, 345U);
	KUNIT_EXPECT_EQ(test, connector->base.display_info.height_mm, 216U);
	KUNIT_EXPECT_MEMEQ(test, &mode, &panel_test_mode, sizeof(mode));
	KUNIT_ASSERT_FALSE(test, list_empty(&connector->base.probed_modes));
	published = list_first_entry(&connector->base.probed_modes, struct drm_display_mode, head);
	KUNIT_EXPECT_EQ(test, published->clock, 521065);
	KUNIT_EXPECT_EQ(test, published->vsync_start, 2278);
	KUNIT_EXPECT_EQ(test, published->vsync_end, 2360);
	list_del(&published->head);
	drm_mode_destroy(drm, published);
	/* The stack catalogue is not owned by dcp_modes_release(). */
	dcp->modes = NULL;
	dcp->nr_modes = 0;
	dcp_modes_release(dcp);
}

static void parser_panel_j616s_publication(struct kunit *test)
{
	parser_panel_publication(test, DCP_FIRMWARE_V_26_6_DESKTOP, true);
}

/*
 * A laptop panel on the H17P (Neo) or an M1/M2 firmware has no desktop
 * state and no EDID transport: it still gets the ADT identity.
 */
static void parser_panel_h17p_publication(struct kunit *test)
{
	parser_panel_publication(test, DCP_FIRMWARE_H17P, false);
}

static void parser_panel_v13_5_publication(struct kunit *test)
{
	parser_panel_publication(test, DCP_FIRMWARE_V_13_5, false);
}

/* No internal panel identity: nothing is invented for the connector. */
static void parser_panel_identity_absent(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_display_metadata metadata = { .manufacturer = "TST", .product = 42 };
	u64 generation = 0;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	dcp->panel.width_mm = 300;
	spin_lock_init(&dcp->metadata.lock);
	dcp_panel_metadata_replace(dcp, &metadata);
	KUNIT_EXPECT_FALSE(test, dcp_panel_metadata_current(dcp, 0));
	KUNIT_EXPECT_EQ(test, PTR_ERR(dcp_panel_edid_snapshot(dcp, &panel_test_mode, 1,
							       &generation)), -ENODEV);
}

static void blob_timing_dimension(struct parser_blob *blob, const char *key,
				  s64 active, s64 total, s64 porch, s64 sync, s64 rate)
{
	blob_key(blob, key);
	blob_tag(blob, 1, 7);
	blob_int(blob, "Active", active);
	blob_int(blob, "Total", total);
	blob_int(blob, "FrontPorch", porch);
	blob_int(blob, "SyncWidth", sync);
	blob_int(blob, "PreciseSyncRate", rate);
	blob_int(blob, "SyncPolarity", 1);
	blob_int(blob, "PixelRepetition", 0);
}

/* A desktop 26.x sink timing: the keys the firmware-derived EDID needs. */
static struct parser_blob *make_timing_blob(struct kunit *test)
{
	struct parser_blob *blob = kunit_kzalloc(test, sizeof(*blob), GFP_KERNEL);

	if (!blob)
		return NULL;
	put_unaligned_le32(0xd3, blob->data);
	blob->size = 4;
	blob_tag(blob, 2, 1);
	blob_tag(blob, 1, 8);
	blob_bool(blob, "IsVirtual", false);
	blob_int(blob, "ID", 7);
	blob_int(blob, "Score", 10);
	blob_timing_dimension(blob, "HorizontalAttributes", 1920, 2200, 88, 44,
			      (675U << 16) / 10);
	blob_timing_dimension(blob, "VerticalAttributes", 1080, 1125, 4, 5, 60 << 16);
	blob_key(blob, "ColorModes");
	blob_tag(blob, 2, 1);
	blob_color(blob, 100, 10, 0, DCP_COLOR_FORMAT_RGB);
	blob_bool(blob, "IsPreferred", true);
	blob_bool(blob, "IsInterlaced", false);
	return blob;
}

static void parser_desktop_metadata_timing(struct kunit *test)
{
	const struct dcp_display_metadata metadata = {
		.width_cm = 60, .height_cm = 33,
		.chromaticity = { { 44608, 20288 }, { 16960, 40448 }, { 9344, 5184 } },
		.white = { 20544, 21568 }, .gamma = 144179,
		.product = 2895, .serial = 305419896, .week = 7, .year = 2013,
		.manufacturer = "ACR", .name = "QG270 S3", .serial_text = "ANON000000001",
	};
	struct parser_blob *blob = make_timing_blob(test);
	struct drm_display_mode preferred;
	struct dcp_display_mode *modes;
	const struct drm_edid *edid;
	const struct edid *raw;
	struct dcp_parse_ctx ctx;
	unsigned int count;

	KUNIT_ASSERT_NOT_NULL(test, blob);

	/* Other DCPs keep their modes and give no metadata timing. */
	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	modes = enumerate_modes(&ctx, &count, 600, 330, 0, false, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_TRUE(test, modes[0].preferred);
	KUNIT_EXPECT_FALSE(test, modes[0].metadata_timing_valid);
	KUNIT_EXPECT_EQ(test, modes[0].metadata_sync_flags, 0U);
	KUNIT_EXPECT_EQ(test, modes[0].mode.flags, 0U);
	kfree(modes);

	KUNIT_ASSERT_EQ(test, parse(blob->data, blob->size, &ctx), 0);
	ctx.dcp = &parser_dcp;
	ctx.metadata_timing = true;
	modes = enumerate_modes(&ctx, &count, 600, 330, 0, false, false);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, modes);
	KUNIT_ASSERT_EQ(test, count, 1U);
	KUNIT_EXPECT_TRUE(test, modes[0].preferred);
	KUNIT_EXPECT_TRUE(test, modes[0].mode.type & DRM_MODE_TYPE_PREFERRED);
	KUNIT_EXPECT_TRUE(test, modes[0].metadata_timing_valid);
	KUNIT_EXPECT_EQ(test, modes[0].metadata_sync_flags,
			(u32)(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC));
	/* The published mode is unchanged: only the EDID timing takes the polarity. */
	KUNIT_EXPECT_EQ(test, modes[0].mode.flags, 0U);
	KUNIT_EXPECT_EQ(test, modes[0].mode.clock, 148500);

	preferred = modes[0].mode;
	preferred.flags |= modes[0].metadata_sync_flags;
	kfree(modes);
	edid = dcp_metadata_edid(&metadata, &preferred);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, edid);
	raw = drm_edid_raw(edid);
	KUNIT_EXPECT_EQ(test, be16_to_cpu(raw->product_id.manufacturer_name),
			(u16)(('A' - '@') << 10 | ('C' - '@') << 5 | ('R' - '@')));
	KUNIT_EXPECT_EQ(test, le16_to_cpu(raw->product_id.product_code), 2895);
	KUNIT_EXPECT_EQ(test, raw->detailed_timings[1].data.other_data.type,
			EDID_DETAIL_MONITOR_NAME);
	KUNIT_EXPECT_MEMEQ(test, raw->detailed_timings[1].data.other_data.data.str.str,
			   "QG270 S3\n", 9);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(raw->detailed_timings[0].pixel_clock), 14850);
	drm_edid_free(edid);
}

/*
 * The M5 internal DCP (firmware 27) crashes on any set_digital_out_mode that
 * is not the 120 Hz boot timing (3024x1986@60 killed the co-processor).  The
 * connector must refuse every other mode of the internal panel.
 */
static void parser_internal_v27_only_120hz(struct kunit *test)
{
	struct dcp_display_mode catalog[2] = {};
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct platform_device *pipeline = kunit_kzalloc(test, sizeof(*pipeline), GFP_KERNEL);
	struct apple_connector *connector = kunit_kzalloc(test, sizeof(*connector), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, pipeline);
	KUNIT_ASSERT_NOT_NULL(test, connector);
	for (i = 0; i < 2; i++) {
		struct drm_display_mode *m = &catalog[i].mode;

		m->clock = i ? 523576 : 1047153;
		m->hdisplay = 3024; m->hsync_start = 3048;
		m->hsync_end = 3056; m->htotal = 3064;
		m->vdisplay = 1986; m->vsync_start = 2754;
		m->vsync_end = 2812; m->vtotal = 2848;
		m->type = DRM_MODE_TYPE_DRIVER | (i ? 0 : DRM_MODE_TYPE_PREFERRED);
		catalog[i].timing_mode_id = 2 + i;
	}
	parser_modes_init(dcp);
	mutex_init(&dcp->dcpavserv.lock);
	mutex_init(&dcp->hpd_mutex);
	dcp->fw_compat = DCP_FIRMWARE_V_27_0;
	dcp->hw.only_120hz_modes = true;
	dcp->connector = connector;
	dcp->modes = catalog;
	dcp->nr_modes = 2;
	dcp->modes_admitted = true;
	connector->dcp = pipeline;
	platform_set_drvdata(pipeline, dcp);

	KUNIT_EXPECT_EQ(test, dcp_mode_valid(&connector->base, &catalog[0].mode), MODE_OK);
	KUNIT_EXPECT_EQ(test, dcp_mode_valid(&connector->base, &catalog[1].mode), MODE_BAD);

	/* The T8142 Air internal DCP (60 Hz panel, firmware 27.0) and the T6050 dcpext keep every mode. */
	dcp->hw.only_120hz_modes = false;
	KUNIT_EXPECT_EQ(test, dcp_mode_valid(&connector->base, &catalog[0].mode), MODE_OK);
	KUNIT_EXPECT_EQ(test, dcp_mode_valid(&connector->base, &catalog[1].mode), MODE_OK);
	dcp->modes = NULL;
	dcp->nr_modes = 0;
	dcp_modes_release(dcp);
}

static struct kunit_case parser_cases[] = {
	KUNIT_CASE(parser_internal_v27_only_120hz),
	KUNIT_CASE(parser_panel_of_dimensions),
	KUNIT_CASE(parser_panel_j616s_publication),
	KUNIT_CASE(parser_panel_h17p_publication),
	KUNIT_CASE(parser_panel_v13_5_publication),
	KUNIT_CASE(parser_panel_identity_absent),
	KUNIT_CASE(parser_adt_bounds),
	KUNIT_CASE(parser_identity_bounds),
	KUNIT_CASE(parser_panel_identity_stable),
	KUNIT_CASE(parser_panel_identity),
	KUNIT_CASE(parser_panel_color_target),
	KUNIT_CASE(parser_panel_adt_color_target),
	KUNIT_CASE(parser_tiled_size_keeps_dsc_modes),
	KUNIT_CASE(parser_score_presence),
	KUNIT_CASE(parser_legacy_ties),
	KUNIT_CASE(parser_color_presence),
	KUNIT_CASE(parser_sdr_default_rgb),
	KUNIT_CASE(parser_geometry),
	KUNIT_CASE(parser_bad_geometry),
	KUNIT_CASE(parser_horizontal_rate),
	KUNIT_CASE(parser_bad_score),
	KUNIT_CASE(parser_notch_vrr),
	KUNIT_CASE(parser_vrr_empty_range),
	KUNIT_CASE(parser_replacement),
	KUNIT_CASE(parser_held_keeps_edid),
	KUNIT_CASE(parser_attachment_admission),
	KUNIT_CASE(parser_stale_transfer),
	KUNIT_CASE(parser_route_catalog_handoff),
	KUNIT_CASE(parser_route_programming_guard),
	KUNIT_CASE(parser_route_prepare_scope),
	KUNIT_CASE(parser_route_holder_snapshot),
	KUNIT_CASE(parser_route_invalid_own_mode),
	KUNIT_CASE(parser_connector_ownership),
	KUNIT_CASE(parser_route_retirement),
	KUNIT_CASE(parser_rejected_replacement),
	KUNIT_CASE(parser_attribute_epoch),
	KUNIT_CASE(parser_attributes_malformed),
	KUNIT_CASE(parser_optional_flag_extent),
	KUNIT_CASE(parser_attribute_deferred),
	KUNIT_CASE(parser_dimensions_lifetime),
	KUNIT_CASE(parser_catalog_release),
	KUNIT_CASE(parser_property_bounds),
	KUNIT_CASE(parser_desktop_metadata_timing),
	{}
};

static struct kunit_suite parser_suite = {
	.name = "apple-display-parser",
	.test_cases = parser_cases,
};

kunit_test_suite(parser_suite);
