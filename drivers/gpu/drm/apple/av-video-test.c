// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include "av-video.h"

static const struct hdr_output_metadata metadata = {
	.hdmi_metadata_type1 = {
		.eotf = 2,
		.display_primaries = { { 0x0201, 0x0403 }, { 0x0605, 0x0807 },
				       { 0x0a09, 0x0c0b } },
		.white_point = { 0x0e0d, 0x100f },
		.max_display_mastering_luminance = 0x1211,
		.min_display_mastering_luminance = 0x1413,
		.max_cll = 0x1615,
		.max_fall = 0x1817,
	},
};

static void av_hdr_object_test(struct kunit *test)
{
	static const u8 expected[] = {
		0xd3, 0, 0, 0, 2, 0, 0, 0x81,
		4, 0, 0, 9, 'T', 'y', 'p', 'e',
		12, 0, 0, 9, 'C', 'E', 'A', '-', '8', '6', '1', ':', 'T', 'y', 'p', '1',
		4, 0, 0, 9, 'D', 'a', 't', 'a',
		24, 0, 0, 0x8a,
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
	};
	u8 object[DCP_HDR_OBJECT_SIZE];

	KUNIT_ASSERT_EQ(test, dcp_hdr_encode(object, &metadata), 0);
	KUNIT_ASSERT_EQ(test, sizeof(object), sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, object, expected, sizeof(expected));
}

static void av_hdr_type_test(struct kunit *test)
{
	struct hdr_output_metadata invalid = metadata;
	u8 object[DCP_HDR_OBJECT_SIZE];

	invalid.metadata_type = 1;
	KUNIT_EXPECT_EQ(test, dcp_hdr_encode(object, &invalid), -EINVAL);
	invalid.metadata_type = 0;
	invalid.hdmi_metadata_type1.metadata_type = 1;
	KUNIT_EXPECT_EQ(test, dcp_hdr_encode(object, &invalid), -EINVAL);
}

static void av_hdr_envelope_test(struct kunit *test)
{
	u8 message[DCP_HDR_MESSAGE_SIZE];
	u8 expected[DCP_HDR_MESSAGE_SIZE] = {};

	/* Independent golden envelope, including every reserved byte. */
	expected[2] = 1;
	expected[4] = 0x15;
	expected[8] = 0x64;
	expected[12] = 0x78;
	expected[13] = 0x63;
	expected[14] = 0x70;
	expected[15] = 0x69;
	expected[0x50] = 68;
	KUNIT_ASSERT_EQ(test, dcp_hdr_message(message, &metadata), 0);
	KUNIT_EXPECT_MEMEQ(test, message, expected, 0x60);
	KUNIT_EXPECT_EQ(test, sizeof(message), (size_t)164);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(message + 0x60), (u32)0xd3);
	KUNIT_EXPECT_EQ(test, message[sizeof(message) - 1], (u8)24);
}

static void av_hdr_reply_test(struct kunit *test)
{
	u8 reply[DCP_HDR_MESSAGE_SIZE] = {};

	KUNIT_EXPECT_EQ(test, dcp_hdr_reply(-ETIMEDOUT, 0, reply, 0), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, dcp_hdr_reply(0, 1, reply, sizeof(reply)), -EIO);
	KUNIT_EXPECT_EQ(test, dcp_hdr_reply(0, 0, reply, 0x43), -EPROTO);
	/* A nonzero neighbouring field must not be mistaken for the status. */
	put_unaligned_le32(1, reply + 0x3c);
	put_unaligned_le32(1, reply + 0x44);
	KUNIT_EXPECT_EQ(test, dcp_hdr_reply(0, 0, reply, 0x44), 0);
	put_unaligned_le32(0x80000001, reply + 0x40);
	KUNIT_EXPECT_EQ(test, dcp_hdr_reply(0, 0, reply, sizeof(reply)), -EIO);
}

static struct kunit_case av_video_cases[] = {
	KUNIT_CASE(av_hdr_object_test),
	KUNIT_CASE(av_hdr_type_test),
	KUNIT_CASE(av_hdr_envelope_test),
	KUNIT_CASE(av_hdr_reply_test),
	{}
};

static struct kunit_suite av_video_suite = {
	.name = "apple-dcp-hdr",
	.test_cases = av_video_cases,
};
kunit_test_suite(av_video_suite);
MODULE_LICENSE("Dual MIT/GPL");
