// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the "model name" that /proc/cpuinfo takes from the SMBIOS
 * Processor Information. Included from cpuinfo.c, so it can test its static
 * parser.
 */
#include <kunit/test.h>

/* A type 4 structure of @len formatted bytes, Version string number @version */
static size_t make_type4(u8 *buf, u8 len, u8 version, const char *strings, size_t slen)
{
	memset(buf, 0, len);
	buf[0] = DMI_ENTRY_PROCESSOR;
	buf[1] = len;
	if (len > 0x10)
		buf[0x10] = version;
	memcpy(buf + len, strings, slen);
	return len + slen;
}

static void parse(u8 len, u8 version, const char *strings, size_t slen, char *out, size_t size)
{
	u8 buf[128];

	make_type4(buf, len, version, strings, slen);
	dmi_processor_version((const struct dmi_header *)buf, out, size);
}

#define STRINGS(s) s, sizeof(s)

static void cpuinfo_model_name_first_string(struct kunit *test)
{
	char out[64];

	parse(0x30, 1, STRINGS("Apple M4 Pro\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "Apple M4 Pro");
}

static void cpuinfo_model_name_later_string(struct kunit *test)
{
	char out[64];

	/* socket, manufacturer, version: the version is string 3 */
	parse(0x30, 3, STRINGS("SoC\0Apple Inc.\0Apple A18 Pro\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "Apple A18 Pro");
}

static void cpuinfo_model_name_trimmed(struct kunit *test)
{
	char out[64];

	parse(0x30, 1, STRINGS("  Apple M2 \t\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "Apple M2");
}

static void cpuinfo_model_name_absent(struct kunit *test)
{
	char out[64] = "stale";

	/* no string number */
	parse(0x30, 0, STRINGS("Apple M1\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "");

	/* a string number past the strings there are */
	strscpy(out, "stale", sizeof(out));
	parse(0x30, 4, STRINGS("SoC\0Apple Inc.\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "");

	/* only blanks */
	strscpy(out, "stale", sizeof(out));
	parse(0x30, 1, STRINGS("   \0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "");

	/* a structure too short to have the Version field */
	strscpy(out, "stale", sizeof(out));
	parse(0x10, 1, STRINGS("Apple M1\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "");
}

static void cpuinfo_model_name_other_type(struct kunit *test)
{
	char out[64] = "stale";
	u8 buf[128];

	make_type4(buf, 0x30, 1, STRINGS("Apple M4 Pro\0\0"));
	buf[0] = 1;	/* System Information */
	dmi_processor_version((const struct dmi_header *)buf, out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "");
}

static void cpuinfo_model_name_truncated(struct kunit *test)
{
	char out[8];

	parse(0x30, 1, STRINGS("Apple M4 Max\0\0"), out, sizeof(out));
	KUNIT_EXPECT_STREQ(test, out, "Apple M");
}

static struct kunit_case cpuinfo_model_name_cases[] = {
	KUNIT_CASE(cpuinfo_model_name_first_string),
	KUNIT_CASE(cpuinfo_model_name_later_string),
	KUNIT_CASE(cpuinfo_model_name_trimmed),
	KUNIT_CASE(cpuinfo_model_name_absent),
	KUNIT_CASE(cpuinfo_model_name_other_type),
	KUNIT_CASE(cpuinfo_model_name_truncated),
	{}
};

static struct kunit_suite cpuinfo_model_name_suite = {
	.name = "arm64-cpuinfo-model-name",
	.test_cases = cpuinfo_model_name_cases,
};
kunit_test_suite(cpuinfo_model_name_suite);
