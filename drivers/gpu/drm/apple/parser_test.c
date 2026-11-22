// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "parser.h"

/* Serialized dictionary tags: size in bits 0-23, type in 24-28, last in 31. */
#define T_DICT 1
#define T_ARRAY 2
#define T_INT64 4
#define T_STRING 9
#define T_BOOL 11

struct blob {
	u8 data[512];
	size_t len;
};

static void blob_align(struct blob *b)
{
	b->len = round_up(b->len, 4);
}

static void blob_tag(struct blob *b, u32 type, u32 size)
{
	blob_align(b);
	put_unaligned_le32(size | type << 24, b->data + b->len);
	b->len += 4;
}

static void blob_init(struct blob *b)
{
	memset(b, 0, sizeof(*b));
	put_unaligned_le32(0xd3, b->data);
	b->len = 4;
}

static void blob_string(struct blob *b, const char *s)
{
	size_t n = strlen(s);

	blob_tag(b, T_STRING, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
}

static void blob_int(struct blob *b, s64 value)
{
	blob_tag(b, T_INT64, 64);
	put_unaligned_le64(value, b->data + b->len);
	b->len += 8;
}

static int parse_service_v27(struct kunit *test, const struct blob *b, bool h17p,
			     bool v27, const char **name, const char **class,
			     s64 *unit)
{
	struct dcp_parse_ctx ctx;

	KUNIT_ASSERT_EQ(test, parse(b->data, b->len, &ctx), 0);
	return parse_epic_service_init(&ctx, name, class, unit, h17p, v27);
}

static int parse_service(struct kunit *test, const struct blob *b, bool h17p,
			 const char **name, const char **class, s64 *unit)
{
	return parse_service_v27(test, b, h17p, false, name, class, unit);
}

static void parser_service_init_test(struct kunit *test)
{
	const char *name, *class;
	struct blob b;
	s64 unit = -1;

	blob_init(&b);
	blob_tag(&b, T_DICT, 4);
	blob_string(&b, "EPICName");
	blob_string(&b, "dcpav-service-epic");
	blob_string(&b, "Other");
	blob_tag(&b, T_BOOL, 1);
	blob_string(&b, "EPICProviderClass");
	blob_string(&b, "AppleDCPAVService");
	blob_string(&b, "EPICUnit");
	blob_int(&b, 2);

	KUNIT_ASSERT_EQ(test, parse_service(test, &b, false, &name, &class, &unit), 0);
	KUNIT_EXPECT_STREQ(test, name, "dcpav-service-epic");
	KUNIT_EXPECT_STREQ(test, class, "AppleDCPAVService");
	KUNIT_EXPECT_EQ(test, unit, 2);
	kfree(name);
	kfree(class);
}

static void parser_service_init_h17p_keys_test(struct kunit *test)
{
	const char *name, *class;
	struct blob b;
	s64 unit = -1;

	blob_init(&b);
	blob_tag(&b, T_DICT, 3);
	blob_string(&b, "compartment");
	blob_string(&b, "system");
	blob_string(&b, "name");
	blob_string(&b, "system");
	blob_string(&b, "interface-id");
	blob_int(&b, 1);

	KUNIT_EXPECT_EQ(test, parse_service(test, &b, false, &name, &class, &unit),
			-ENOENT);
	KUNIT_EXPECT_TRUE(test, IS_ERR(name));
	KUNIT_EXPECT_TRUE(test, IS_ERR(class));

	KUNIT_ASSERT_EQ(test, parse_service(test, &b, true, &name, &class, &unit), 0);
	KUNIT_EXPECT_STREQ(test, name, "system");
	KUNIT_EXPECT_STREQ(test, class, "system");
	KUNIT_EXPECT_EQ(test, unit, 1);
	kfree(name);
	kfree(class);
}

/* Firmware 27 takes EPICUnit over interface-id; H17P keeps the later key. */
static void parser_service_init_v27_unit_test(struct kunit *test)
{
	const char *name, *class;
	struct blob b;
	s64 unit = -1;

	blob_init(&b);
	blob_tag(&b, T_DICT, 4);
	blob_string(&b, "EPICName");
	blob_string(&b, "dcpdptx-port-epic");
	blob_string(&b, "EPICProviderClass");
	blob_string(&b, "AppleDCPDPTXRemotePort");
	blob_string(&b, "EPICUnit");
	blob_int(&b, 1);
	blob_string(&b, "interface-id");
	blob_int(&b, 3);

	KUNIT_ASSERT_EQ(test, parse_service_v27(test, &b, true, true, &name, &class,
						&unit), 0);
	KUNIT_EXPECT_EQ(test, unit, 1);
	kfree(name);
	kfree(class);

	KUNIT_ASSERT_EQ(test, parse_service(test, &b, true, &name, &class, &unit), 0);
	KUNIT_EXPECT_EQ(test, unit, 3);
	kfree(name);
	kfree(class);
}

static void parser_service_init_truncated_test(struct kunit *test)
{
	const char *name, *class;
	struct blob b;
	s64 unit;

	/* A string length running past the end of the blob. */
	blob_init(&b);
	blob_tag(&b, T_DICT, 3);
	blob_string(&b, "EPICName");
	blob_tag(&b, T_STRING, 0x100);
	memcpy(b.data + b.len, "dcp", 3);
	b.len += 3;

	KUNIT_EXPECT_LT(test, parse_service(test, &b, false, &name, &class, &unit), 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(name));
	KUNIT_EXPECT_TRUE(test, IS_ERR(class));

	/* A dictionary announcing more pairs than the blob holds. */
	blob_init(&b);
	blob_tag(&b, T_DICT, 0xffffff);
	blob_string(&b, "EPICName");
	blob_string(&b, "dcp");

	KUNIT_EXPECT_LT(test, parse_service(test, &b, false, &name, &class, &unit), 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(name));
	KUNIT_EXPECT_TRUE(test, IS_ERR(class));
}

static void parser_nesting_test(struct kunit *test)
{
	const char *name, *class;
	struct blob b;
	unsigned int i;
	s64 unit;

	/* An ignored value nested far deeper than any real dictionary. */
	blob_init(&b);
	blob_tag(&b, T_DICT, 4);
	blob_string(&b, "EPICName");
	blob_string(&b, "dcp");
	blob_string(&b, "EPICProviderClass");
	blob_string(&b, "dcp");
	blob_string(&b, "EPICUnit");
	blob_int(&b, 0);
	blob_string(&b, "Ignored");
	for (i = 0; i < 64; i++)
		blob_tag(&b, T_ARRAY, 1);
	blob_tag(&b, T_BOOL, 0);

	KUNIT_EXPECT_LT(test, parse_service(test, &b, false, &name, &class, &unit), 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(name));
	KUNIT_EXPECT_TRUE(test, IS_ERR(class));
}

static void parser_bad_header_test(struct kunit *test)
{
	struct dcp_parse_ctx ctx;
	struct blob b;

	blob_init(&b);
	KUNIT_EXPECT_EQ(test, parse(b.data, 3, &ctx), -EINVAL);
	put_unaligned_le32(0xd4, b.data);
	KUNIT_EXPECT_EQ(test, parse(b.data, b.len, &ctx), -EINVAL);
}

static void parser_mode_count_test(struct kunit *test)
{
	struct dcp_display_mode *modes;
	struct dcp_parse_ctx ctx;
	unsigned int count = 1;
	struct blob b;

	/* An element count that cannot be backed by the bytes present. */
	blob_init(&b);
	blob_tag(&b, T_ARRAY, 0xffffff);
	blob_tag(&b, T_DICT, 0);

	KUNIT_ASSERT_EQ(test, parse(b.data, b.len, &ctx), 0);
	modes = enumerate_modes(&ctx, &count, 0, 0, 0, false, false);
	KUNIT_EXPECT_PTR_EQ(test, modes, ERR_PTR(-EINVAL));

	/* Entries that are present but invalid are skipped, not fatal. */
	blob_init(&b);
	blob_tag(&b, T_ARRAY, 2);
	blob_tag(&b, T_DICT, 0);
	blob_tag(&b, T_DICT, 1);
	blob_string(&b, "ID");
	blob_int(&b, 1);

	KUNIT_ASSERT_EQ(test, parse(b.data, b.len, &ctx), 0);
	modes = enumerate_modes(&ctx, &count, 0, 0, 0, false, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(modes));
	KUNIT_EXPECT_EQ(test, count, 0);
	kfree(modes);
}

/* Build an array of @count empty dictionaries in a test allocation. */
static struct dcp_display_mode *parse_empty_modes(struct kunit *test,
						  unsigned int count,
						  unsigned int *found)
{
	size_t len = 8 + 4 * count;
	struct dcp_parse_ctx ctx;
	unsigned int i;
	u8 *data;

	data = kunit_kzalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	put_unaligned_le32(0xd3, data);
	put_unaligned_le32(count | T_ARRAY << 24, data + 4);
	for (i = 0; i < count; i++)
		put_unaligned_le32(T_DICT << 24, data + 8 + 4 * i);

	KUNIT_ASSERT_EQ(test, parse(data, len, &ctx), 0);
	return enumerate_modes(&ctx, found, 0, 0, 0, false, false);
}

static void parser_mode_cap_test(struct kunit *test)
{
	struct dcp_display_mode *modes;
	unsigned int count = 1;

	modes = parse_empty_modes(test, DCP_MAX_TIMING_MODES, &count);
	KUNIT_ASSERT_FALSE(test, IS_ERR(modes));
	KUNIT_EXPECT_EQ(test, count, 0);
	kfree(modes);

	modes = parse_empty_modes(test, DCP_MAX_TIMING_MODES + 1, &count);
	KUNIT_EXPECT_PTR_EQ(test, modes, ERR_PTR(-E2BIG));
}

static void parser_property_complete_test(struct kunit *test)
{
	struct blob b;

	blob_init(&b);
	blob_tag(&b, T_DICT, 1);
	blob_string(&b, "Key");
	blob_int(&b, 7);

	KUNIT_EXPECT_TRUE(test, dcp_property_complete(b.data, b.len, b.len));
	/* One unsent byte after the aligned object is tolerated. */
	KUNIT_EXPECT_TRUE(test, dcp_property_complete(b.data, b.len, b.len + 1));
	KUNIT_EXPECT_FALSE(test, dcp_property_complete(b.data, b.len, b.len + 4));
	KUNIT_EXPECT_FALSE(test, dcp_property_complete(b.data, b.len - 4, b.len));
	KUNIT_EXPECT_FALSE(test, dcp_property_complete(b.data, b.len + 4, b.len + 4));
	KUNIT_EXPECT_FALSE(test, dcp_property_complete(NULL, b.len, b.len));
}

static struct kunit_case parser_cases[] = {
	KUNIT_CASE(parser_service_init_test),
	KUNIT_CASE(parser_service_init_h17p_keys_test),
	KUNIT_CASE(parser_service_init_v27_unit_test),
	KUNIT_CASE(parser_service_init_truncated_test),
	KUNIT_CASE(parser_nesting_test),
	KUNIT_CASE(parser_bad_header_test),
	KUNIT_CASE(parser_mode_count_test),
	KUNIT_CASE(parser_mode_cap_test),
	KUNIT_CASE(parser_property_complete_test),
	{}
};

static struct kunit_suite parser_suite = {
	.name = "apple-dcp-parser",
	.test_cases = parser_cases,
};

kunit_test_suite(parser_suite);
