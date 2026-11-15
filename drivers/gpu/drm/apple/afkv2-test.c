// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"
#include "afkv2-codec.h"

static void afkv2_frame_bounds_test(struct kunit *test)
{
	u8 record[AFKV2_HEADER_SIZE + 8] = {};
	struct afkv2_frame frame;
	size_t size;

	for (size = 0; size < AFKV2_HEADER_SIZE; size++)
		KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, size, false, &frame),
				-EINVAL);
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(NULL, sizeof(record), false, &frame),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, SIZE_MAX, false, &frame),
			-E2BIG);
	afkv2_put32(record + 4, U32_MAX);
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, sizeof(record), false, &frame),
			-EINVAL);
	afkv2_put32(record + 4, 15);
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, sizeof(record), true, &frame),
			-EINVAL);
	afkv2_put32(record + 4, 16);
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, sizeof(record), false, &frame),
			-EINVAL);
	KUNIT_ASSERT_EQ(test, afkv2_decode_frame(record, sizeof(record), true, &frame), 0);
	KUNIT_EXPECT_EQ(test, frame.payload_size, 0U);
	KUNIT_EXPECT_EQ(test, frame.padding_size, 8U);
	afkv2_put32(record + 4, sizeof(record) - 8);
	KUNIT_ASSERT_EQ(test, afkv2_decode_frame(record, sizeof(record), false, &frame), 0);
	KUNIT_EXPECT_EQ(test, frame.payload_size, 8U);
	KUNIT_EXPECT_PTR_EQ(test, frame.payload, record + AFKV2_HEADER_SIZE);
	record[17] = AFKV2_RESPONSE + 1;
	KUNIT_EXPECT_EQ(test, afkv2_decode_frame(record, sizeof(record), false, &frame),
			-EPROTO);
}

static void afkv2_command_bounds_test(struct kunit *test)
{
	u8 payload[AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE] = { 1 };
	struct afkv2_command command;
	size_t size;

	for (size = 0; size < sizeof(payload); size++)
		KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, size, &command), -EINVAL);
	KUNIT_ASSERT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command), 0);
	afkv2_put64(payload + 8, U64_MAX);
	afkv2_put32(payload + 24, 2);
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command),
			-EINVAL);
	afkv2_put32(payload + 24, 1);
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command), 0);
	afkv2_put64(payload + 8, 0);
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command),
			-EINVAL);
	afkv2_put64(payload + 8, 0x1000);
	afkv2_put32(payload + 24, AFKV2_MAX_PAYLOAD + 1);
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command),
			-EINVAL);
	payload[0] = 2;
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command),
			-EINVAL);
	payload[0] = 0;
	payload[2] = 1;
	KUNIT_EXPECT_EQ(test, afkv2_decode_command(payload, sizeof(payload), &command),
			-EINVAL);
}

static void afkv2_owned_span_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, afkv2_owned_span(0x1000, 0x100, 0x1000, 0x100));
	KUNIT_EXPECT_TRUE(test, afkv2_owned_span(0x1000, 0x100, 0x10ff, 1));
	KUNIT_EXPECT_FALSE(test, afkv2_owned_span(0x1000, 0x100, 0xfff, 1));
	KUNIT_EXPECT_FALSE(test, afkv2_owned_span(0x1000, 0x100, 0x10ff, 2));
	KUNIT_EXPECT_FALSE(test, afkv2_owned_span(0x1000, 0x100, 0x1000, SIZE_MAX));
	KUNIT_EXPECT_FALSE(test, afkv2_owned_span(U64_MAX, 2, U64_MAX, 1));
	KUNIT_EXPECT_TRUE(test, afkv2_owned_span(U64_MAX, 1, U64_MAX, 1));
}

static void afkv2_service_reply_bounds_test(struct kunit *test)
{
	u8 reply[68] = {};
	size_t size = SIZE_MAX;
	unsigned int len;

	afkv2_put16(reply + 2, 3);
	afkv2_put32(reply + 4, 8);
	afkv2_put32(reply + 8, 4);
	afkv2_put32(reply + 12, 0x69706378);
	for (len = 0; len < 64; len++)
		KUNIT_EXPECT_EQ(test, afkv2_service_reply_size(reply, len, 3, 8, 4, &size),
				-EPROTO);
	KUNIT_ASSERT_EQ(test, afkv2_service_reply_size(reply, sizeof(reply), 3, 8, 4,
						       &size), 0);
	KUNIT_EXPECT_EQ(test, size, 4U);
	KUNIT_EXPECT_EQ(test, afkv2_service_reply_size(reply, sizeof(reply), 3, 8, 3,
						       &size), -EMSGSIZE);
	afkv2_put32(reply + 8, U32_MAX);
	KUNIT_EXPECT_EQ(test, afkv2_service_reply_size(reply, sizeof(reply), 3, 8,
						       SIZE_MAX, &size), -EMSGSIZE);
}

static void afkv2_response_bounds_test(struct kunit *test)
{
	struct afkv2_frame frame = { .category = AFKV2_RESPONSE, .interface = 4, .type = 9 };
	struct afkv2_command command = {
		.id = 0xf1, .oob = true, .rx_address = 0x1000, .rx_size = 64,
		.tx_address = 0x2000, .tx_size = 32,
	};

	KUNIT_EXPECT_EQ(test, afkv2_check_response(&frame, &command, 4, 9, 0xf1,
						   0x1000, 64, 0x2000, 32), 0);
	command.rx_size++;
	KUNIT_EXPECT_EQ(test, afkv2_check_response(&frame, &command, 4, 9, 0xf1,
						   0x1000, 64, 0x2000, 32), -EPROTO);
	command.rx_size--;
	command.id = 0x01;
	KUNIT_EXPECT_EQ(test, afkv2_check_response(&frame, &command, 4, 9, 0xf1,
						   0x1000, 64, 0x2000, 32), -EPROTO);
	command.id = 0xf1;
	command.rx_address++;
	KUNIT_EXPECT_EQ(test, afkv2_check_response(&frame, &command, 4, 9, 0xf1,
						   0x1000, 64, 0x2000, 32), -EPROTO);
}

static void afkv2_callback_bounds_test(struct kunit *test)
{
	struct afkv2_command command = {
		.oob = true, .rx_size = 64, .tx_size = 32, .value = 64,
	};
	size_t request = SIZE_MAX, response = SIZE_MAX;

	KUNIT_ASSERT_EQ(test, afkv2_callback_lengths(&command, 32, 256, &request, &response), 0);
	KUNIT_EXPECT_EQ(test, request, 32U);
	KUNIT_EXPECT_EQ(test, response, 64U);
	command.tx_size = 31;
	KUNIT_EXPECT_EQ(test, afkv2_callback_lengths(&command, 32, 256, &request, &response),
			-EINVAL);
	command.tx_size = 32;
	command.rx_size = 65;
	KUNIT_EXPECT_EQ(test, afkv2_callback_lengths(&command, 32, 256, &request, &response),
			-EINVAL);
	command.value = U32_MAX;
	command.rx_size = 257;
	KUNIT_EXPECT_EQ(test, afkv2_callback_lengths(&command, 32, 256, &request, &response),
			-EINVAL);
}

static void afkv2_inline_response_test(struct kunit *test)
{
	struct afkv2_frame frame = { .category = AFKV2_RESPONSE, .interface = 4, .type = 9 };
	struct afkv2_command command = { .id = 3, .inline_size = 64 };

	KUNIT_EXPECT_EQ(test, afkv2_check_inline_response(&frame, &command, 4, 9, 3, 64), 0);
	command.inline_size++;
	KUNIT_EXPECT_EQ(test, afkv2_check_inline_response(&frame, &command, 4, 9, 3, 64),
			-EPROTO);
	command.inline_size--;
	frame.token = 1;
	KUNIT_EXPECT_EQ(test, afkv2_check_inline_response(&frame, &command, 4, 9, 3, 64),
			-EPROTO);
}

static void afk_ring_payload_bounds_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0x40, 0x100, 0x1000));
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0xfc0, 0x30, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0xfc0, 0x31, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x1000, 0, 0x1000));
}

static struct kunit_case afkv2_parser_cases[] = {
	KUNIT_CASE(afk_ring_payload_bounds_test),
	KUNIT_CASE(afkv2_frame_bounds_test),
	KUNIT_CASE(afkv2_command_bounds_test),
	KUNIT_CASE(afkv2_owned_span_test),
	KUNIT_CASE(afkv2_service_reply_bounds_test),
	KUNIT_CASE(afkv2_response_bounds_test),
	KUNIT_CASE(afkv2_callback_bounds_test),
	KUNIT_CASE(afkv2_inline_response_test),
	{}
};

static struct kunit_suite afkv2_parser_suite = {
	.name = "apple-afkv2-parser",
	.test_cases = afkv2_parser_cases,
};

kunit_test_suite(afkv2_parser_suite);

MODULE_LICENSE("Dual MIT/GPL");
