// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the DCP serialized dictionary parsers that the other parser
 * suites do not call: display metadata, DPTX capabilities, system log events,
 * timing catalogs, panel EDID properties and audio elements. Firmware hands
 * these blobs to the driver, so beyond the accepted shapes every parser must
 * fail on every truncation of a valid blob instead of reading past its end.
 *
 * All values are synthetic.
 */

#include <kunit/test.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
#include <sound/pcm.h>
#endif

#include "dcp-peer-v27.h"
#include "parser.h"

/* Tag: size in bits 0-23, type in 24-28, padding in 29-30, last in 31. */
#define PB_DICT		1
#define PB_ARRAY	2
#define PB_INT64	4
#define PB_STRING	9
#define PB_BLOB		10
#define PB_BOOL		11
#define PB_CAP		8192

struct pb {
	struct kunit *test;
	u8 *data;
	u32 len;
};

static struct pb *pb_new(struct kunit *test, bool header)
{
	struct pb *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, b);
	b->data = kunit_kzalloc(test, PB_CAP, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b->data);
	b->test = test;
	if (header) {
		put_unaligned_le32(0xd3, b->data);
		b->len = 4;
	}
	return b;
}

static void pb_raw(struct pb *b, const void *bytes, u32 n)
{
	KUNIT_ASSERT_LE(b->test, b->len + n, PB_CAP);
	memcpy(b->data + b->len, bytes, n);
	b->len += n;
}

static void pb_word(struct pb *b, u32 word)
{
	b->len = round_up(b->len, 4);
	KUNIT_ASSERT_LE(b->test, b->len + 4, PB_CAP);
	put_unaligned_le32(word, b->data + b->len);
	b->len += 4;
}

static void pb_tag(struct pb *b, u32 type, u32 size)
{
	pb_word(b, size | type << 24);
}

static void pb_str(struct pb *b, const char *s)
{
	pb_tag(b, PB_STRING, strlen(s));
	pb_raw(b, s, strlen(s));
}

static void pb_int(struct pb *b, s64 value)
{
	u8 raw[8];

	pb_tag(b, PB_INT64, 0);
	put_unaligned_le64(value, raw);
	pb_raw(b, raw, sizeof(raw));
}

static void pb_kint(struct pb *b, const char *key, s64 value)
{
	pb_str(b, key);
	pb_int(b, value);
}

static void pb_kbool(struct pb *b, const char *key, bool value)
{
	pb_str(b, key);
	pb_tag(b, PB_BOOL, value);
}

static void pb_kstr(struct pb *b, const char *key, const char *value)
{
	pb_str(b, key);
	pb_str(b, value);
}

typedef int (*pb_parse_fn)(const u8 *data, size_t len, void *priv);

/*
 * Every strict prefix of @b must fail. The bytes after the prefix stay in
 * place and complete a valid blob, so a parser that reads past its length
 * succeeds instead. Each prefix also runs from an exact-size allocation,
 * which KASAN checks when enabled.
 */
static void pb_expect_prefixes_fail(struct pb *b, pb_parse_fn fn, void *priv)
{
	struct kunit *test = b->test;
	u32 len;

	for (len = 0; len < b->len; len++) {
		u8 *copy;

		KUNIT_EXPECT_LT_MSG(test, fn(b->data, len, priv), 0,
				    "prefix %u of %u", len, b->len);
		if (!len)
			continue;
		copy = kmemdup(b->data, len, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, copy);
		KUNIT_EXPECT_LT_MSG(test, fn(copy, len, priv), 0,
				    "exact prefix %u of %u", len, b->len);
		kfree(copy);
	}
}

/* A parser that reports an error leaves its output alone. */
static void pb_expect_untouched(struct kunit *test, const void *out, size_t size)
{
	KUNIT_EXPECT_NULL(test, memchr_inv(out, 0xa5, size));
}

/* Serialized framing */

static bool pb_complete(struct pb *b)
{
	return dcp_property_complete(b->data, b->len, b->len);
}

static void parser_bounds_framing_test(struct kunit *test)
{
	struct pb *b;

	/* The last-element flag in bit 31 is accepted. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_str(b, "Key");
	pb_word(b, PB_INT64 << 24 | BIT(31));
	pb_raw(b, "\0\0\0\0\0\0\0\0", 8);
	KUNIT_EXPECT_TRUE(test, pb_complete(b));

	/* Padding bits 29 and 30 must be clear. */
	b = pb_new(test, true);
	pb_word(b, PB_BOOL << 24 | BIT(29));
	KUNIT_EXPECT_FALSE(test, pb_complete(b));
	b = pb_new(test, true);
	pb_word(b, PB_BOOL << 24 | BIT(30));
	KUNIT_EXPECT_FALSE(test, pb_complete(b));

	/* Types the serializer does not define. */
	b = pb_new(test, true);
	pb_tag(b, 3, 0);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));
	b = pb_new(test, true);
	pb_tag(b, 0, 0);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));

	/* An integer is eight bytes whatever its tag size says. */
	b = pb_new(test, true);
	pb_tag(b, PB_INT64, 4);
	pb_raw(b, "\1\0\0\0", 4);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));

	/* A string or blob longer than the bytes left. */
	b = pb_new(test, true);
	pb_tag(b, PB_STRING, 5);
	pb_raw(b, "abcd", 4);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));
	b = pb_new(test, true);
	pb_tag(b, PB_BLOB, 0xffffff);
	pb_raw(b, "abcd", 4);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));

	/* The largest dictionary count, doubled for its keys, still ends at the blob. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 0xffffff);
	pb_kint(b, "Key", 1);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));
}

static void pb_nest(struct pb *b, unsigned int depth)
{
	unsigned int i;

	for (i = 0; i < depth; i++)
		pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_BOOL, 1);
}

/* The skipper bounds its recursion at 32 levels: 31 containers fit. */
static void parser_bounds_depth_test(struct kunit *test)
{
	struct pb *b;

	b = pb_new(test, true);
	pb_nest(b, 31);
	KUNIT_EXPECT_TRUE(test, pb_complete(b));

	b = pb_new(test, true);
	pb_nest(b, 32);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));

	b = pb_new(test, true);
	pb_nest(b, 1000);
	KUNIT_EXPECT_FALSE(test, pb_complete(b));
}

static void parser_bounds_array_count_test(struct kunit *test)
{
	struct dcp_parse_ctx ctx;
	struct pb *b;
	u32 count = 0xa5a5a5a5;

	b = pb_new(test, true);
	pb_tag(b, PB_ARRAY, 3);
	pb_int(b, 1);
	pb_int(b, 2);
	pb_int(b, 3);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_array_count(&ctx, &count), 0);
	KUNIT_EXPECT_EQ(test, count, 3);
	/* The cursor is not consumed. */
	KUNIT_EXPECT_EQ(test, ctx.pos, 4);

	/* Only the tag is read: a count beyond the blob is the caller's to bound. */
	b = pb_new(test, true);
	pb_tag(b, PB_ARRAY, 0xffffff);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_array_count(&ctx, &count), 0);
	KUNIT_EXPECT_EQ(test, count, 0xffffff);

	count = 0xa5a5a5a5;
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_array_count(&ctx, &count), -EINVAL);
	KUNIT_ASSERT_EQ(test, parse(b->data, 6, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_array_count(&ctx, &count), -EINVAL);
	KUNIT_EXPECT_EQ(test, count, 0xa5a5a5a5);
}

/* DisplayAttributes */

static void pb_point(struct pb *b, const char *key, s64 x, s64 y, s64 gamma)
{
	pb_str(b, key);
	pb_tag(b, PB_DICT, gamma ? 3 : 2);
	pb_kint(b, "X", x);
	pb_kint(b, "Y", y);
	if (gamma)
		pb_kint(b, "Gamma", gamma);
}

static void pb_product(struct pb *b, const char *manufacturer, s64 product,
		       const char *name)
{
	pb_tag(b, PB_DICT, 7);
	pb_kstr(b, "ManufacturerID", manufacturer);
	pb_kint(b, "ProductID", product);
	pb_kstr(b, "ProductName", name);
	pb_kint(b, "SerialNumber", 7);
	pb_kint(b, "WeekOfManufacture", 10);
	pb_kint(b, "YearOfManufacture", 2024);
	pb_kstr(b, "AlphanumericSerialNumber", "SN0001");
}

static struct pb *pb_display_attributes(struct kunit *test)
{
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_DICT, 10);
	pb_kint(b, "MaxHorizontalImageSize", 60);
	pb_kint(b, "MaxVerticalImageSize", 34);
	pb_str(b, "Chromaticity");
	pb_tag(b, PB_DICT, 3);
	pb_point(b, "Red", 42000, 21000, 0);
	pb_point(b, "Green", 20000, 45000, 0);
	pb_point(b, "Blue", 10000, 4000, 0);
	pb_point(b, "DefaultWhitePoint", 20500, 21500, 144179);
	pb_kbool(b, "SupportsPQEOTF", true);
	pb_kbool(b, "SupportsHDRStaticMetadataType1", true);
	pb_kint(b, "SupportsBT2020RGB", 1);
	pb_str(b, "Luminance");
	pb_tag(b, PB_DICT, 3);
	pb_kint(b, "Max", 600 << 16);
	pb_kint(b, "MaxFrameAverage", 400 << 16);
	pb_kint(b, "Min", 1 << 12);
	pb_kstr(b, "Unknown", "ignored");
	pb_str(b, "ProductAttributes");
	pb_product(b, "TST", 0x1234, "Synthetic");
	return b;
}

static int pb_run_display_metadata(const u8 *data, size_t len, void *priv)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(data, len, &ctx);

	return ret ?: parse_display_metadata(&ctx, priv);
}

static void parser_bounds_metadata_test(struct kunit *test)
{
	struct pb *b = pb_display_attributes(test);
	struct dcp_display_metadata md;

	memset(&md, 0xa5, sizeof(md));
	KUNIT_ASSERT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), 0);
	KUNIT_EXPECT_EQ(test, md.width_cm, 60);
	KUNIT_EXPECT_EQ(test, md.height_cm, 34);
	KUNIT_EXPECT_EQ(test, md.chromaticity[0][0], 42000);
	KUNIT_EXPECT_EQ(test, md.chromaticity[1][1], 45000);
	KUNIT_EXPECT_EQ(test, md.chromaticity[2][1], 4000);
	KUNIT_EXPECT_EQ(test, md.white[0], 20500);
	KUNIT_EXPECT_EQ(test, md.white[1], 21500);
	KUNIT_EXPECT_EQ(test, md.gamma, 144179);
	KUNIT_EXPECT_STREQ(test, md.manufacturer, "TST");
	KUNIT_EXPECT_EQ(test, md.product, 0x1234);
	KUNIT_EXPECT_STREQ(test, md.name, "Synthetic");
	KUNIT_EXPECT_STREQ(test, md.serial_text, "SN0001");
	KUNIT_EXPECT_EQ(test, md.serial, 7);
	KUNIT_EXPECT_EQ(test, md.week, 10);
	KUNIT_EXPECT_EQ(test, md.year, 2024);
	KUNIT_EXPECT_TRUE(test, md.pq);
	KUNIT_EXPECT_TRUE(test, md.static_metadata_type1);
	KUNIT_EXPECT_TRUE(test, md.bt2020_rgb);
	KUNIT_EXPECT_EQ(test, md.max_luminance, 600 << 16);
	KUNIT_EXPECT_EQ(test, md.max_fall, 400 << 16);
	KUNIT_EXPECT_EQ(test, md.min_luminance, 1 << 12);
	KUNIT_EXPECT_EQ(test, md.defaults, 0);
}

static void parser_bounds_metadata_truncated_test(struct kunit *test)
{
	struct pb *b = pb_display_attributes(test);
	struct dcp_display_metadata md;

	memset(&md, 0xa5, sizeof(md));
	pb_expect_prefixes_fail(b, pb_run_display_metadata, &md);
	pb_expect_untouched(test, &md, sizeof(md));
}

/* Optional values out of range fall back to EDID defaults; identity is required. */
static void parser_bounds_metadata_defaults_test(struct kunit *test)
{
	struct dcp_display_metadata md;
	struct pb *b;

	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 6);
	pb_kint(b, "MaxHorizontalImageSize", 256);
	pb_kint(b, "MaxVerticalImageSize", 34);
	/* Primaries without a white point are no colour description. */
	pb_str(b, "Chromaticity");
	pb_tag(b, PB_DICT, 3);
	pb_point(b, "Red", 42000, 21000, 0);
	pb_point(b, "Green", 20000, 45000, 0);
	pb_point(b, "Blue", 10000, 4000, 0);
	/* A minimum at the maximum is no luminance range. */
	pb_str(b, "Luminance");
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "Max", 100);
	pb_kint(b, "Min", 100);
	pb_kstr(b, "SupportsBT2020RGB", "yes");
	pb_str(b, "ProductAttributes");
	pb_tag(b, PB_DICT, 2);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 1);

	KUNIT_ASSERT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), 0);
	KUNIT_EXPECT_EQ(test, md.width_cm, 0);
	KUNIT_EXPECT_EQ(test, md.height_cm, 0);
	KUNIT_EXPECT_EQ(test, md.chromaticity[0][0], 0);
	KUNIT_EXPECT_EQ(test, md.max_luminance, 0);
	KUNIT_EXPECT_FALSE(test, md.bt2020_rgb);
	KUNIT_EXPECT_EQ(test, md.defaults,
			DCP_METADATA_DEFAULT_SIZE | DCP_METADATA_DEFAULT_COLOR |
			DCP_METADATA_DEFAULT_NAME | DCP_METADATA_DEFAULT_DATE);

	/* No ProductAttributes at all. */
	memset(&md, 0xa5, sizeof(md));
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "MaxHorizontalImageSize", 60);
	pb_kint(b, "MaxVerticalImageSize", 34);
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);
	pb_expect_untouched(test, &md, sizeof(md));
}

static void parser_bounds_metadata_keys_test(struct kunit *test)
{
	struct dcp_display_metadata md;
	char key[80];
	struct pb *b;

	memset(&md, 0xa5, sizeof(md));

	/* A repeated key. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kint(b, "MaxHorizontalImageSize", 60);
	pb_kint(b, "MaxHorizontalImageSize", 61);
	pb_str(b, "ProductAttributes");
	pb_product(b, "TST", 1, "Synthetic");
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	/* A key with a control character. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "Odd\nKey", 1);
	pb_str(b, "ProductAttributes");
	pb_product(b, "TST", 1, "Synthetic");
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	/* An empty key and one longer than the key buffer. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "", 1);
	pb_str(b, "ProductAttributes");
	pb_product(b, "TST", 1, "Synthetic");
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	memset(key, 'K', sizeof(key));
	key[64] = 0;
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, key, 1);
	pb_str(b, "ProductAttributes");
	pb_product(b, "TST", 1, "Synthetic");
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	/* A key of the wrong type. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_int(b, 1);
	pb_int(b, 1);
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	/* An array where the dictionary goes. */
	b = pb_new(test, true);
	pb_tag(b, PB_ARRAY, 0);
	KUNIT_EXPECT_EQ(test, pb_run_display_metadata(b->data, b->len, &md), -EINVAL);

	pb_expect_untouched(test, &md, sizeof(md));
}

static int pb_run_panel_product(const u8 *data, size_t len, void *priv)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(data, len, &ctx);

	return ret ?: parse_panel_product_metadata(&ctx, priv);
}

static void parser_bounds_product_test(struct kunit *test)
{
	struct dcp_display_metadata md;
	char label[257];
	struct pb *b;

	/* A panel label is cut to a descriptor; its date is not read. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 4);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 0xffff);
	pb_kstr(b, "ProductName", "Synthetic Panel Label");
	pb_kstr(b, "WeekOfManufacture", "not a number");
	KUNIT_ASSERT_EQ(test, pb_run_panel_product(b->data, b->len, &md), 0);
	KUNIT_EXPECT_STREQ(test, md.manufacturer, "TST");
	KUNIT_EXPECT_EQ(test, md.product, 0xffff);
	KUNIT_EXPECT_STREQ(test, md.name, "Synthetic Pan");
	KUNIT_EXPECT_EQ(test, md.week, 0);

	/* A blank label reads as absent. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 1);
	pb_kstr(b, "ProductName", "    ");
	KUNIT_ASSERT_EQ(test, pb_run_panel_product(b->data, b->len, &md), 0);
	KUNIT_EXPECT_EQ(test, md.name[0], 0);

	/* The legacy ID alone gives the manufacturer: TST = 20 << 10 | 19 << 5 | 20. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "LegacyManufacturerID", 20 << 10 | 19 << 5 | 20);
	pb_kint(b, "ProductID", 1);
	KUNIT_ASSERT_EQ(test, pb_run_panel_product(b->data, b->len, &md), 0);
	KUNIT_EXPECT_STREQ(test, md.manufacturer, "TST");

	memset(&md, 0xa5, sizeof(md));

	/* A legacy ID that disagrees with the manufacturer. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "LegacyManufacturerID", 1 << 10 | 1 << 5 | 1);
	pb_kint(b, "ProductID", 1);
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);

	/* Not three capitals; a product beyond 16 bits; no product. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kstr(b, "ManufacturerID", "tst");
	pb_kint(b, "ProductID", 1);
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kstr(b, "ManufacturerID", "TSTX");
	pb_kint(b, "ProductID", 1);
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 0x10000);
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kstr(b, "ManufacturerID", "TST");
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);

	/* A label longer than 255 bytes or with a control character. */
	memset(label, 'L', sizeof(label));
	label[256] = 0;
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 1);
	pb_kstr(b, "ProductName", label);
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kstr(b, "ManufacturerID", "TST");
	pb_kint(b, "ProductID", 1);
	pb_kstr(b, "ProductName", "Tab\there");
	KUNIT_EXPECT_EQ(test, pb_run_panel_product(b->data, b->len, &md), -EINVAL);

	pb_expect_untouched(test, &md, sizeof(md));

	b = pb_new(test, true);
	pb_product(b, "TST", 1, "Synthetic");
	pb_expect_prefixes_fail(b, pb_run_panel_product, &md);
	pb_expect_untouched(test, &md, sizeof(md));
}

/* DPTX port capabilities */

static int pb_run_dptx_caps(const u8 *data, size_t len, void *priv)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(data, len, &ctx);

	return ret ?: parse_dptx_caps(&ctx, priv);
}

static void parser_bounds_dptx_caps_test(struct kunit *test)
{
	struct dcp_dptx_caps caps;
	struct pb *b;

	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "MaxPixelWidth", 5120);
	pb_str(b, "Nested");
	pb_tag(b, PB_ARRAY, 2);
	pb_tag(b, PB_DICT, 0);
	pb_int(b, 3);
	pb_kint(b, "MaxTotalPixelRate", 1000000);
	pb_kint(b, "MaxActivePixelRate", U32_MAX);
	KUNIT_ASSERT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), 0);
	KUNIT_EXPECT_EQ(test, caps.width, 5120);
	KUNIT_EXPECT_EQ(test, caps.total_rate, 1000000);
	KUNIT_EXPECT_EQ(test, caps.active_rate, U32_MAX);

	memset(&caps, 0xa5, sizeof(caps));
	pb_expect_prefixes_fail(b, pb_run_dptx_caps, &caps);

	/* A missing limit. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "MaxPixelWidth", 5120);
	pb_kint(b, "MaxTotalPixelRate", 1000000);
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -ENOENT);

	/* A repeated limit. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "MaxPixelWidth", 5120);
	pb_kint(b, "MaxPixelWidth", 4096);
	pb_kint(b, "MaxTotalPixelRate", 1000000);
	pb_kint(b, "MaxActivePixelRate", 1000000);
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -EINVAL);

	/* Zero, beyond 32 bits, negative, or not an integer. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kint(b, "MaxPixelWidth", 0);
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kint(b, "MaxTotalPixelRate", (s64)U32_MAX + 1);
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kint(b, "MaxActivePixelRate", -1);
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kstr(b, "MaxPixelWidth", "5120");
	KUNIT_EXPECT_EQ(test, pb_run_dptx_caps(b->data, b->len, &caps), -EINVAL);

	pb_expect_untouched(test, &caps, sizeof(caps));
}

/* System log brightness events */

static struct pb *pb_mnits_event(struct kunit *test, const char *event, s64 mnits)
{
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_DICT, 5);
	pb_kstr(b, "logEvent", event);
	pb_kint(b, "timestamp", 1234);
	pb_kint(b, "iDAC", 56);
	pb_str(b, "extra");
	pb_tag(b, PB_DICT, 1);
	pb_kbool(b, "x", false);
	pb_kint(b, "mNits", mnits);
	return b;
}

static int pb_run_mnits(const u8 *data, size_t len, void *priv)
{
	struct dcp_parse_ctx ctx;
	int ret = parse(data, len, &ctx);

	return ret ?: parse_system_log_mnits(&ctx, priv);
}

static void parser_bounds_mnits_test(struct kunit *test)
{
	struct dcp_system_ev_mnits entry;
	struct pb *b;

	b = pb_mnits_event(test, "Display (Event Forward)", 500000);
	KUNIT_ASSERT_EQ(test, pb_run_mnits(b->data, b->len, &entry), 0);
	KUNIT_EXPECT_EQ(test, entry.millinits, 500000);
	KUNIT_EXPECT_EQ(test, entry.idac, 56);
	KUNIT_EXPECT_EQ(test, entry.timestamp, 1234);

	memset(&entry, 0xa5, sizeof(entry));
	pb_expect_prefixes_fail(b, pb_run_mnits, &entry);

	b = pb_mnits_event(test, "Display (Event Forward) ", 500000);
	KUNIT_EXPECT_EQ(test, pb_run_mnits(b->data, b->len, &entry), -EINVAL);
	b = pb_mnits_event(test, "Display (Event Forward)", -1);
	KUNIT_EXPECT_EQ(test, pb_run_mnits(b->data, b->len, &entry), -EINVAL);
	b = pb_mnits_event(test, "Display (Event Forward)", (s64)U32_MAX + 1);
	KUNIT_EXPECT_EQ(test, pb_run_mnits(b->data, b->len, &entry), -ERANGE);

	/* No timestamp. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 3);
	pb_kstr(b, "logEvent", "Display (Event Forward)");
	pb_kint(b, "iDAC", 56);
	pb_kint(b, "mNits", 1);
	KUNIT_EXPECT_EQ(test, pb_run_mnits(b->data, b->len, &entry), -EINVAL);

	pb_expect_untouched(test, &entry, sizeof(entry));
}

static int pb_run_mnits_pairs(const u8 *data, size_t len, void *priv)
{
	return parse_system_log_mnits_pairs(data, len, priv);
}

/* Header-less key/value pairs, read up to the first one that does not parse. */
static void parser_bounds_mnits_pairs_test(struct kunit *test)
{
	struct dcp_system_ev_mnits entry;
	struct pb *b;

	b = pb_new(test, false);
	pb_kint(b, "timestamp", 99);
	pb_kint(b, "iDAC", -5);
	pb_kstr(b, "other", "value");
	pb_kint(b, "mNits", 250);
	KUNIT_ASSERT_EQ(test, pb_run_mnits_pairs(b->data, b->len, &entry), 0);
	KUNIT_EXPECT_EQ(test, entry.millinits, 250);
	KUNIT_EXPECT_EQ(test, entry.idac, 0);
	KUNIT_EXPECT_EQ(test, entry.timestamp, 99);

	/* mNits comes last, so no prefix holds all of it. */
	memset(&entry, 0xa5, sizeof(entry));
	pb_expect_prefixes_fail(b, pb_run_mnits_pairs, &entry);
	pb_expect_untouched(test, &entry, sizeof(entry));

	/* A key whose length runs past the end stops the walk before mNits. */
	b = pb_new(test, false);
	pb_tag(b, PB_STRING, 0x1000);
	pb_raw(b, "mNits", 5);
	pb_kint(b, "mNits", 250);
	KUNIT_EXPECT_EQ(test, pb_run_mnits_pairs(b->data, b->len, &entry), -EINVAL);

	/* A value of the wrong type stops it too. */
	b = pb_new(test, false);
	pb_kstr(b, "iDAC", "56");
	pb_kint(b, "mNits", 250);
	KUNIT_EXPECT_EQ(test, pb_run_mnits_pairs(b->data, b->len, &entry), -EINVAL);

	b = pb_new(test, false);
	pb_kint(b, "mNits", (s64)U32_MAX + 1);
	KUNIT_EXPECT_EQ(test, pb_run_mnits_pairs(b->data, b->len, &entry), -EINVAL);
	pb_expect_untouched(test, &entry, sizeof(entry));
}

/* TimingElements catalogs */

static void pb_dimension(struct pb *b, const char *key, s64 active, s64 total)
{
	pb_str(b, key);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "Active", active);
	pb_kint(b, "Total", total);
	pb_kint(b, "FrontPorch", 8);
	pb_kint(b, "SyncWidth", 4);
}

static struct pb *pb_timing_ids(struct kunit *test)
{
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_ARRAY, 4);
	pb_tag(b, PB_DICT, 3);
	pb_kint(b, "ID", 3);
	pb_dimension(b, "HorizontalAttributes", 1920, 2200);
	pb_dimension(b, "VerticalAttributes", 1080, 1125);
	/* Malformed entries are skipped: an ID of the wrong type, one beyond 32 bits. */
	pb_tag(b, PB_DICT, 1);
	pb_kstr(b, "ID", "4");
	pb_tag(b, PB_DICT, 1);
	pb_kint(b, "ID", (s64)U32_MAX + 5);
	/* No size: zero. */
	pb_tag(b, PB_DICT, 2);
	pb_kbool(b, "IsPreferred", true);
	pb_kint(b, "ID", 9);
	return b;
}

static int pb_run_timing_ids(const u8 *data, size_t len, void *priv)
{
	return dcp_parse_timing_ids(data, len, priv, 4);
}

static void parser_bounds_timing_ids_test(struct kunit *test)
{
	struct dcp_timing_id ids[4];
	struct pb *b = pb_timing_ids(test);

	memset(ids, 0xa5, sizeof(ids));
	KUNIT_ASSERT_EQ(test, dcp_parse_timing_ids(b->data, b->len, ids, 4), 2);
	KUNIT_EXPECT_EQ(test, ids[0].id, 3);
	KUNIT_EXPECT_EQ(test, ids[0].width, 1920);
	KUNIT_EXPECT_EQ(test, ids[0].height, 1080);
	KUNIT_EXPECT_EQ(test, ids[1].id, 9);
	KUNIT_EXPECT_EQ(test, ids[1].width, 0);
	KUNIT_EXPECT_EQ(test, ids[1].height, 0);
	pb_expect_untouched(test, &ids[2], 2 * sizeof(ids[0]));

	/* At most @max are written. */
	memset(ids, 0xa5, sizeof(ids));
	KUNIT_EXPECT_EQ(test, dcp_parse_timing_ids(b->data, b->len, ids, 1), 1);
	KUNIT_EXPECT_EQ(test, ids[0].id, 3);
	pb_expect_untouched(test, &ids[1], 3 * sizeof(ids[0]));
	KUNIT_EXPECT_EQ(test, dcp_parse_timing_ids(b->data, b->len, ids, 0), 0);

	/* Every truncation ends in a skip that cannot finish. */
	pb_expect_prefixes_fail(b, pb_run_timing_ids, ids);

	/* A dictionary is no catalog. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 0);
	KUNIT_EXPECT_EQ(test, dcp_parse_timing_ids(b->data, b->len, ids, 4), -EINVAL);
}

#define PB_MAX_COLORS 8

struct pb_colors {
	unsigned int count;
	struct dcp_color_element e[PB_MAX_COLORS];
};

static void pb_collect_color(void *data, const struct dcp_color_element *e)
{
	struct pb_colors *c = data;

	if (c->count < PB_MAX_COLORS)
		c->e[c->count] = *e;
	c->count++;
}

static void pb_color(struct pb *b, s64 id, s64 eotf)
{
	pb_tag(b, PB_DICT, 5);
	pb_kint(b, "ID", id);
	pb_kint(b, "Depth", 10);
	pb_kint(b, "EOTF", eotf);
	pb_kbool(b, "IsVirtual", false);
	pb_kint(b, "Score", 100 + id);
}

static struct pb *pb_timing_colors(struct kunit *test)
{
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_ARRAY, 3);
	/* A malformed entry that would match. */
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "ID", 5);
	pb_kstr(b, "IsInterlaced", "no");
	/* Another timing. */
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "ID", 4);
	pb_str(b, "ColorModes");
	pb_tag(b, PB_ARRAY, 1);
	pb_color(b, 99, 0);
	/* The timing, its colours before its ID. */
	pb_tag(b, PB_DICT, 5);
	pb_str(b, "ColorModes");
	pb_tag(b, PB_ARRAY, 2);
	pb_color(b, 10, 0);
	pb_color(b, 11, 2);
	pb_str(b, "UnsafeColorElementIDs");
	pb_tag(b, PB_ARRAY, 1);
	pb_int(b, 11);
	pb_str(b, "DSCRequiredColorElementIDs");
	pb_tag(b, PB_ARRAY, 1);
	pb_int(b, 10);
	pb_kbool(b, "IsInterlaced", true);
	pb_kint(b, "ID", 5);
	return b;
}

static int pb_run_timing_colors(const u8 *data, size_t len, void *priv)
{
	struct pb_colors *c = priv;

	c->count = 0;
	return dcp_parse_timing_colors(data, len, 5, pb_collect_color, c, NULL);
}

static void parser_bounds_timing_colors_test(struct kunit *test)
{
	struct pb *b = pb_timing_colors(test);
	struct pb_colors *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	bool interlaced = false;

	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_EQ(test, dcp_parse_timing_colors(b->data, b->len, 5, pb_collect_color,
						      c, &interlaced), 0);
	KUNIT_EXPECT_TRUE(test, interlaced);
	KUNIT_ASSERT_EQ(test, c->count, 2);
	KUNIT_EXPECT_EQ(test, c->e[0].id, 10);
	KUNIT_EXPECT_EQ(test, c->e[0].depth, 10);
	KUNIT_EXPECT_EQ(test, c->e[0].score, 110);
	KUNIT_EXPECT_EQ(test, c->e[0].range, -1);
	KUNIT_EXPECT_FALSE(test, c->e[0].is_virtual);
	KUNIT_EXPECT_FALSE(test, c->e[0].unsafe);
	KUNIT_EXPECT_TRUE(test, c->e[0].dsc_required);
	KUNIT_EXPECT_EQ(test, c->e[1].id, 11);
	KUNIT_EXPECT_EQ(test, c->e[1].eotf, 2);
	KUNIT_EXPECT_TRUE(test, c->e[1].unsafe);
	KUNIT_EXPECT_FALSE(test, c->e[1].dsc_required);

	c->count = 0;
	KUNIT_EXPECT_EQ(test, dcp_parse_timing_colors(b->data, b->len, 6, pb_collect_color,
						      c, NULL), -ENOENT);
	KUNIT_EXPECT_EQ(test, c->count, 0);

	pb_expect_prefixes_fail(b, pb_run_timing_colors, c);
}

/*
 * The restricted ID lists hold 64 entries each: further IDs are dropped, not
 * written over the neighbouring list.
 */
static void parser_bounds_timing_colors_lists_test(struct kunit *test)
{
	struct pb_colors *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	struct pb *b = pb_new(test, true);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, c);
	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "ID", 1);
	pb_str(b, "DSCRequiredColorElementIDs");
	pb_tag(b, PB_ARRAY, 1);
	pb_int(b, 10);
	pb_str(b, "UnsafeColorElementIDs");
	pb_tag(b, PB_ARRAY, 65);
	for (i = 0; i < 64; i++)
		pb_int(b, 1000 + i);
	pb_int(b, 11);
	pb_str(b, "ColorModes");
	pb_tag(b, PB_ARRAY, 2);
	pb_color(b, 10, 0);
	pb_color(b, 11, 0);

	KUNIT_ASSERT_EQ(test, dcp_parse_timing_colors(b->data, b->len, 1, pb_collect_color,
						      c, NULL), 0);
	KUNIT_ASSERT_EQ(test, c->count, 2);
	KUNIT_EXPECT_TRUE(test, c->e[0].dsc_required);
	KUNIT_EXPECT_FALSE(test, c->e[0].unsafe);
	KUNIT_EXPECT_FALSE(test, c->e[1].dsc_required);
	KUNIT_EXPECT_FALSE(test, c->e[1].unsafe);

	/* A list entry of the wrong type makes the entry malformed. */
	b = pb_new(test, true);
	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "ID", 1);
	pb_str(b, "UnsafeColorElementIDs");
	pb_tag(b, PB_ARRAY, 1);
	pb_str(b, "11");
	c->count = 0;
	KUNIT_EXPECT_EQ(test, dcp_parse_timing_colors(b->data, b->len, 1, pb_collect_color,
						      c, NULL), -ENOENT);
	KUNIT_EXPECT_EQ(test, c->count, 0);
}

/* Panel EDID properties */

static void pb_edid(u8 *edid, unsigned int blocks)
{
	memset(edid, 0, blocks * 128);
	memcpy(edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
	edid[126] = blocks - 1;
}

static void pb_kedid(struct pb *b, const char *key, const u8 *edid, u32 size)
{
	pb_str(b, key);
	pb_tag(b, PB_BLOB, size);
	pb_raw(b, edid, size);
}

static void parser_bounds_edid_raw_test(struct kunit *test)
{
	u8 *edid = kunit_kzalloc(test, 4224, GFP_KERNEL);
	const u8 *found = NULL;
	size_t size = 0;

	KUNIT_ASSERT_NOT_NULL(test, edid);
	pb_edid(edid, 2);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 256, &found, &size), 0);
	KUNIT_EXPECT_PTR_EQ(test, found, (const u8 *)edid);
	KUNIT_EXPECT_EQ(test, size, 256);
	/* Trailing bytes after the extensions are not EDID. */
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 300, &found, &size), 0);
	KUNIT_EXPECT_EQ(test, size, 256);

	/* Extension blocks the buffer does not hold. */
	found = NULL;
	size = 0;
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 255, &found, &size), -EINVAL);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 128, &found, &size), -EINVAL);
	/* 33 blocks exceed the 4 KiB bound even when present. */
	pb_edid(edid, 33);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 4224, &found, &size), -EINVAL);
	pb_edid(edid, 32);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(edid, 4096, &found, &size), 0);
	KUNIT_EXPECT_EQ(test, size, 4096);

	/* Fewer than 128 bytes is no raw EDID, and no serialized blob either. */
	found = NULL;
	size = 0;
	pb_edid(edid, 1);
	KUNIT_EXPECT_LT(test, parse_panel_edid_data(edid, 127, &found, &size), 0);
	KUNIT_EXPECT_NULL(test, found);
	KUNIT_EXPECT_EQ(test, size, 0);
}

static int pb_run_edid(const u8 *data, size_t len, void *priv)
{
	const u8 *edid;
	size_t size;

	return parse_panel_edid_data(data, len, &edid, &size);
}

static void parser_bounds_edid_serialized_test(struct kunit *test)
{
	u8 *edid = kunit_kzalloc(test, 384, GFP_KERNEL);
	const u8 *found;
	unsigned int i;
	size_t size;
	struct pb *b;

	KUNIT_ASSERT_NOT_NULL(test, edid);
	pb_edid(edid, 2);

	/* A blob at the root. */
	b = pb_new(test, true);
	pb_tag(b, PB_BLOB, 256);
	pb_raw(b, edid, 256);
	KUNIT_ASSERT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), 0);
	KUNIT_EXPECT_PTR_EQ(test, found, (const u8 *)b->data + 8);
	KUNIT_EXPECT_EQ(test, size, 256);

	/* A named blob in a nested dictionary, after other values. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "Other", 1);
	pb_str(b, "Properties");
	pb_tag(b, PB_DICT, 2);
	pb_kedid(b, "NotEDID", edid, 128);
	pb_kedid(b, "IODisplayEDID", edid, 256);
	KUNIT_ASSERT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), 0);
	KUNIT_EXPECT_EQ(test, size, 256);
	KUNIT_EXPECT_EQ(test, memcmp(found, edid, 256), 0);
	pb_expect_prefixes_fail(b, pb_run_edid, NULL);

	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "edid", edid, 256);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), 0);

	/* No EDID key. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "EDIDs", edid, 256);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -ENOENT);

	/* Sizes that are no whole EDID, or disagree with its extension count. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "EDID", edid, 130);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "EDID", edid, 384);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -EINVAL);
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "EDID", edid + 1, 128);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -EINVAL);

	/* A blob size beyond the bytes present. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 1);
	pb_str(b, "EDID");
	pb_tag(b, PB_BLOB, 256);
	pb_raw(b, edid, 128);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -EINVAL);

	/* Deeper than four dictionaries. */
	b = pb_new(test, true);
	for (i = 0; i < 5; i++) {
		pb_tag(b, PB_DICT, 1);
		pb_str(b, "Nested");
	}
	pb_tag(b, PB_DICT, 1);
	pb_kedid(b, "EDID", edid, 128);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -E2BIG);

	/* More entries than the walk budget. */
	b = pb_new(test, true);
	pb_tag(b, PB_DICT, 64);
	for (i = 0; i < 64; i++)
		pb_kint(b, "N", i);
	KUNIT_EXPECT_EQ(test, parse_panel_edid_data(b->data, b->len, &found, &size), -E2BIG);
}

/* Mode ranking */

static void pb_mode(struct dcp_display_mode *m, int clock, u16 htotal, u16 vtotal,
		    bool preferred)
{
	memset(m, 0, sizeof(*m));
	m->mode.clock = clock;
	m->mode.htotal = htotal;
	m->mode.vtotal = vtotal;
	m->firmware_preferred = preferred;
}

static void parser_bounds_default_above_test(struct kunit *test)
{
	struct dcp_display_mode a, b;

	/* The higher refresh rate first, whatever the preference or score. */
	pb_mode(&a, 297000, 2200, 1125, false);
	pb_mode(&b, 148500, 2200, 1125, true);
	KUNIT_EXPECT_TRUE(test, dcp_mode_default_above(&a, 0, &b, 100));
	KUNIT_EXPECT_FALSE(test, dcp_mode_default_above(&b, 100, &a, 0));

	/* Same rate from different totals: IsPreferred, then the score. */
	pb_mode(&a, 297000, 4400, 1125, false);
	KUNIT_EXPECT_FALSE(test, dcp_mode_default_above(&a, 100, &b, 0));
	KUNIT_EXPECT_TRUE(test, dcp_mode_default_above(&b, 0, &a, 100));
	a.firmware_preferred = true;
	KUNIT_EXPECT_TRUE(test, dcp_mode_default_above(&a, 1, &b, 0));
	KUNIT_EXPECT_FALSE(test, dcp_mode_default_above(&a, 0, &b, 0));

	/* The cross products do not wrap at the largest timings. */
	pb_mode(&a, INT_MAX, U16_MAX, U16_MAX, false);
	pb_mode(&b, INT_MAX - 1, U16_MAX, U16_MAX, false);
	KUNIT_EXPECT_TRUE(test, dcp_mode_default_above(&a, 0, &b, 0));
	KUNIT_EXPECT_FALSE(test, dcp_mode_default_above(&b, 0, &a, 0));
}

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
/* Audio elements */

static void pb_layout(struct pb *b, s64 nchans, const char * const *labels,
		      unsigned int nlabels)
{
	unsigned int i;

	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "ActiveChannelCount", nchans);
	pb_str(b, "ChannelLayout");
	pb_tag(b, PB_ARRAY, nlabels);
	for (i = 0; i < nlabels; i++)
		pb_str(b, labels[i]);
}

static const char * const pb_stereo[] = { "Front Left", "Front Right" };
static const char * const pb_surround[] = {
	"Front Left", "Front Right", "Front Center", "Low Frequency Effects",
	"Rear Left", "Rear Right", "Side Left", "Rear Center",
};

static void pb_avep(struct pb *b, s64 rate, s64 sample_size, const char *cookie_key,
		    u32 cookie_size)
{
	u8 cookie[DCP_SOUND_COOKIE_MAX + 4];
	unsigned int i;

	for (i = 0; i < sizeof(cookie); i++)
		cookie[i] = i + 1;
	pb_tag(b, PB_DICT, 5);
	pb_kint(b, "StreamSampleRate", rate);
	pb_kint(b, "SampleSize", sample_size);
	pb_kstr(b, "Other", "skipped");
	pb_str(b, "AudioChannelLayoutElements");
	pb_tag(b, PB_ARRAY, 2);
	pb_layout(b, 2, pb_stereo, ARRAY_SIZE(pb_stereo));
	pb_layout(b, 8, pb_surround, ARRAY_SIZE(pb_surround));
	pb_str(b, cookie_key);
	pb_tag(b, PB_BLOB, cookie_size);
	pb_raw(b, cookie, cookie_size);
}

static struct pb *pb_audio(struct kunit *test, u32 cookie_size, bool unmatched)
{
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_ARRAY, unmatched ? 3 : 2);
	pb_avep(b, 44100, 16, "ElementData", cookie_size);
	pb_avep(b, 48000, 24, "ElementData", cookie_size);
	/* A rate ALSA has no bit for and a sample size it has no format for. */
	if (unmatched)
		pb_avep(b, 12345, 18, "ElementData", cookie_size);
	return b;
}

static const struct dcp_sound_format_mask pb_any = {
	.formats = ~0ULL, .rates = ~0U, .nchans = ~0U,
};

/*
 * Channel counts the firmware reports but the u32 channel mask has no bit
 * for (32 and up, negative, wider than 32 bits) match nothing; they must not
 * wrap onto the bit of a small count (1 << 33 is 1 << 1 on arm64).
 */
static void parser_bounds_sound_wide_layouts_test(struct kunit *test)
{
	static const s64 counts[] = { 2, 31, 32, 33, 34, 63, 64, -1, -31, 0x100000001LL };
	struct dcp_sound_format_mask sieve = pb_any, hits;
	struct dcp_parse_ctx ctx;
	struct pb *b = pb_new(test, true);
	unsigned int i;

	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 3);
	pb_kint(b, "StreamSampleRate", 48000);
	pb_kint(b, "SampleSize", 16);
	pb_str(b, "AudioChannelLayoutElements");
	pb_tag(b, PB_ARRAY, ARRAY_SIZE(counts));
	for (i = 0; i < ARRAY_SIZE(counts); i++) {
		pb_tag(b, PB_DICT, 1);
		pb_kint(b, "ActiveChannelCount", counts[i]);
	}

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_constraints(&ctx, &sieve, &hits), 0);
	KUNIT_EXPECT_EQ(test, hits.nchans, BIT(2) | BIT(31));

	/* No layout has 0 or 1 channels: a sieve for those matches no element */
	sieve.nchans = BIT(0) | BIT(1);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_constraints(&ctx, &sieve, &hits), 0);
	KUNIT_EXPECT_EQ(test, hits.nchans, 0U);
	KUNIT_EXPECT_EQ(test, hits.rates, 0U);
}

/*
 * A count wider than 32 bits is not the low 32 bits of itself: a layout
 * claiming 0x100000008 channels must not be taken for the 8-channel one
 * and replace its channel map.
 */
static void parser_bounds_sound_wide_layout_map_test(struct kunit *test)
{
	struct dcp_sound_format_mask sieve = {
		.formats = SNDRV_PCM_FMTBIT_S16,
		.rates = SNDRV_PCM_RATE_48000,
		.nchans = BIT(8),
	};
	u8 data[DCP_SOUND_COOKIE_SIZE] = { 1 };
	struct snd_pcm_chmap_elem chmap = {};
	struct dcp_sound_cookie cookie = {};
	struct dcp_parse_ctx ctx;
	struct pb *b = pb_new(test, true);

	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "StreamSampleRate", 48000);
	pb_kint(b, "SampleSize", 16);
	pb_str(b, "AudioChannelLayoutElements");
	pb_tag(b, PB_ARRAY, 2);
	pb_layout(b, 8, pb_surround, ARRAY_SIZE(pb_surround));
	pb_layout(b, 0x100000008LL, pb_stereo, ARRAY_SIZE(pb_stereo));
	pb_str(b, "ElementData");
	pb_tag(b, PB_BLOB, sizeof(data));
	pb_raw(b, data, sizeof(data));

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), 1);
	KUNIT_EXPECT_EQ(test, chmap.channels, 8);
	KUNIT_EXPECT_EQ(test, chmap.map[3], SNDRV_CHMAP_LFE);
}

static void parser_bounds_sound_constraints_test(struct kunit *test)
{
	struct dcp_sound_format_mask sieve = pb_any, hits;
	struct dcp_parse_ctx ctx;
	struct pb *b = pb_audio(test, DCP_SOUND_COOKIE_SIZE, true);
	u32 len;

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_constraints(&ctx, &sieve, &hits), 0);
	KUNIT_EXPECT_EQ(test, hits.rates, SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000);
	KUNIT_EXPECT_EQ(test, hits.formats, SNDRV_PCM_FMTBIT_S16 | SNDRV_PCM_FMTBIT_S24);
	KUNIT_EXPECT_EQ(test, hits.nchans, BIT(2) | BIT(8));

	sieve.rates = SNDRV_PCM_RATE_48000;
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_constraints(&ctx, &sieve, &hits), 0);
	KUNIT_EXPECT_EQ(test, hits.rates, SNDRV_PCM_RATE_48000);
	KUNIT_EXPECT_EQ(test, hits.formats, SNDRV_PCM_FMTBIT_S24);

	/* The parser reports a truncation instead of the elements it did read. */
	sieve = pb_any;
	for (len = 4; len < b->len; len++) {
		KUNIT_ASSERT_EQ(test, parse(b->data, len, &ctx), 0);
		KUNIT_EXPECT_LT_MSG(test, parse_sound_constraints(&ctx, &sieve, &hits), 0,
				    "prefix %u of %u", len, b->len);
	}
}

static void parser_bounds_sound_mode_test(struct kunit *test)
{
	struct dcp_sound_format_mask sieve = {
		.formats = SNDRV_PCM_FMTBIT_S24,
		.rates = SNDRV_PCM_RATE_48000,
		.nchans = BIT(8),
	};
	struct snd_pcm_chmap_elem chmap = {};
	struct dcp_sound_cookie cookie = {};
	struct dcp_parse_ctx ctx;
	struct pb *b = pb_audio(test, DCP_SOUND_COOKIE_SIZE, true);
	u32 len;

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), 1);
	KUNIT_ASSERT_EQ(test, chmap.channels, 8);
	KUNIT_EXPECT_EQ(test, chmap.map[0], SNDRV_CHMAP_FL);
	KUNIT_EXPECT_EQ(test, chmap.map[3], SNDRV_CHMAP_LFE);
	/* A label the table does not name. */
	KUNIT_EXPECT_EQ(test, chmap.map[6], SNDRV_CHMAP_UNKNOWN);
	KUNIT_EXPECT_EQ(test, chmap.map[7], SNDRV_CHMAP_RC);
	KUNIT_EXPECT_EQ(test, cookie.data[0], 1);
	KUNIT_EXPECT_EQ(test, cookie.data[DCP_SOUND_COOKIE_SIZE - 1], DCP_SOUND_COOKIE_SIZE);

	/* No element matches. */
	sieve.rates = SNDRV_PCM_RATE_96000;
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), 0);
	sieve.rates = SNDRV_PCM_RATE_48000;

	/* Link data shorter than the service takes. */
	b = pb_audio(test, DCP_SOUND_COOKIE_SIZE - 1, true);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_EXPECT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), -EINVAL);

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_AUDIO)
	/* The 14.7 external role takes 32 bytes. */
	b = pb_audio(test, DCP_SOUND_COOKIE_SIZE, true);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	ctx.v14_7_audio = true;
	KUNIT_EXPECT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), -EINVAL);
	b = pb_audio(test, DCP_SOUND_COOKIE_SIZE_V14_7, true);
	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	ctx.v14_7_audio = true;
	KUNIT_ASSERT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), 1);
	KUNIT_EXPECT_EQ(test, cookie.data[DCP_SOUND_COOKIE_SIZE_V14_7 - 1],
			DCP_SOUND_COOKIE_SIZE_V14_7);
#endif

	/* The matching element is the last: no prefix holds all of it. */
	b = pb_audio(test, DCP_SOUND_COOKIE_SIZE, false);
	for (len = 4; len < b->len; len++) {
		KUNIT_ASSERT_EQ(test, parse(b->data, len, &ctx), 0);
		KUNIT_EXPECT_LT_MSG(test, parse_sound_mode(&ctx, &sieve, &chmap, &cookie), 0,
				    "prefix %u of %u", len, b->len);
	}
}

/* A layout with more labels than a channel map holds is cut at the map. */
static void parser_bounds_sound_chmap_test(struct kunit *test)
{
	struct dcp_sound_format_mask sieve = pb_any;
	struct snd_pcm_chmap_elem chmap = {};
	struct dcp_parse_ctx ctx;
	struct pb *b = pb_new(test, true);
	unsigned int i;

	sieve.nchans = BIT(2);
	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 4);
	pb_kint(b, "StreamSampleRate", 48000);
	pb_kint(b, "SampleSize", 24);
	pb_str(b, "AudioChannelLayoutElements");
	pb_tag(b, PB_ARRAY, 1);
	pb_tag(b, PB_DICT, 2);
	pb_kint(b, "ActiveChannelCount", 2);
	pb_str(b, "ChannelLayout");
	pb_tag(b, PB_ARRAY, ARRAY_SIZE(chmap.map) + 5);
	for (i = 0; i < ARRAY_SIZE(chmap.map) + 5; i++)
		pb_str(b, i & 1 ? "Front Right" : "Front Left");
	pb_str(b, "ElementData");
	pb_tag(b, PB_BLOB, DCP_SOUND_COOKIE_MAX);
	pb_raw(b, "abcdefghijklmnopqrstuvwxyz0123456789", DCP_SOUND_COOKIE_MAX);

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	KUNIT_ASSERT_EQ(test, parse_sound_mode(&ctx, &sieve, &chmap, NULL), 1);
	KUNIT_EXPECT_EQ(test, chmap.channels, ARRAY_SIZE(chmap.map));
	KUNIT_EXPECT_EQ(test, chmap.map[ARRAY_SIZE(chmap.map) - 1], SNDRV_CHMAP_FL);
}
#endif

static struct kunit_case parser_bounds_cases[] = {
	KUNIT_CASE(parser_bounds_framing_test),
	KUNIT_CASE(parser_bounds_depth_test),
	KUNIT_CASE(parser_bounds_array_count_test),
	KUNIT_CASE(parser_bounds_metadata_test),
	KUNIT_CASE(parser_bounds_metadata_truncated_test),
	KUNIT_CASE(parser_bounds_metadata_defaults_test),
	KUNIT_CASE(parser_bounds_metadata_keys_test),
	KUNIT_CASE(parser_bounds_product_test),
	KUNIT_CASE(parser_bounds_dptx_caps_test),
	KUNIT_CASE(parser_bounds_mnits_test),
	KUNIT_CASE(parser_bounds_mnits_pairs_test),
	KUNIT_CASE(parser_bounds_timing_ids_test),
	KUNIT_CASE(parser_bounds_timing_colors_test),
	KUNIT_CASE(parser_bounds_timing_colors_lists_test),
	KUNIT_CASE(parser_bounds_edid_raw_test),
	KUNIT_CASE(parser_bounds_edid_serialized_test),
	KUNIT_CASE(parser_bounds_default_above_test),
#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	KUNIT_CASE(parser_bounds_sound_constraints_test),
	KUNIT_CASE(parser_bounds_sound_wide_layouts_test),
	KUNIT_CASE(parser_bounds_sound_wide_layout_map_test),
	KUNIT_CASE(parser_bounds_sound_mode_test),
	KUNIT_CASE(parser_bounds_sound_chmap_test),
#endif
	{}
};

static struct kunit_suite parser_bounds_suite = {
	.name = "apple-dcp-parser-bounds",
	.test_cases = parser_bounds_cases,
};

kunit_test_suite(parser_bounds_suite);
