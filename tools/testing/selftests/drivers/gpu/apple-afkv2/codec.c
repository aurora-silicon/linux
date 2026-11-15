// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "afkv2-codec.h"

static unsigned int assertions;
#define CHECK(x)              \
	do {                  \
		assertions++; \
		assert(x);    \
	} while (0)

/* Invented fixtures with independently specified wire bytes; no device data. */
static const u8 open_fixture[24] = { 0x7b, 0, 0x34, 0x12, 0x10, 0, 0, 0,
				     0,	   0, 0,    0,	  0,	0, 0, 0,
				     0x12, 0, 1,    0,	  0,	0, 0, 0 };
static const u8 descriptor_fixture[32] = {
	1,    0xab, 0,	  0,	0x40, 0, 0,    0, 0x88, 0x77, 0x66,
	0x55, 0x44, 0x33, 0x22, 0x11, 8, 7,    6, 5,	4,    3,
	2,    1,    0x40, 0,	0,    0, 0x10, 0, 0,	0
};

int main(void)
{
	u8 data[128], command[32], unaligned[129], saved[128];
	struct afkv2_frame frame, reply;
	struct afkv2_command parsed, response;
	size_t written = 999;
	int ret;

	memset(data, 0xa5, sizeof(data));
	CHECK(!afkv2_encode_open(data, sizeof(data), 0x7b, 0x1234, &written));
	CHECK(written == sizeof(open_fixture));
	CHECK(!memcmp(data, open_fixture, sizeof(open_fixture)));
	CHECK(data[24] == 0xa5);
	CHECK(!afkv2_decode_frame(data, written, false, &frame));
	CHECK(frame.interface == 0x1234 && frame.type == AFKV2_OPEN);
	CHECK(frame.category == AFKV2_REPORT && frame.interface_flags == 1);
	CHECK(frame.payload_size == 0 && frame.padding_size == 0);
	memcpy(unaligned + 1, open_fixture, sizeof(open_fixture));
	CHECK(!afkv2_decode_frame(unaligned + 1, sizeof(open_fixture), false,
				  &frame));
	CHECK(afkv2_decode_frame(data, SIZE_MAX, true, &frame) == -E2BIG);
	CHECK(afkv2_decode_frame(NULL, 24, false, &frame) == -EINVAL);
	CHECK(afkv2_decode_frame(data, 24, false, NULL) == -EINVAL);
	for (size_t i = 0; i < 24; i++)
		CHECK(afkv2_decode_frame(data, i, false, &frame) == -EINVAL);
	CHECK(afkv2_decode_frame(data, 32, false, &frame) == -EINVAL);
	CHECK(!afkv2_decode_frame(data, 32, true, &frame) &&
	      frame.padding_size == 8);
	for (u32 i = 0; i < 16; i++) {
		afkv2_put32(data + 4, i);
		CHECK(afkv2_decode_frame(data, 24, false, &frame) == -EINVAL);
	}
	afkv2_put32(data + 4, 17);
	CHECK(afkv2_decode_frame(data, 24, false, &frame) == -EINVAL);
	afkv2_put32(data + 4, UINT32_MAX);
	CHECK(afkv2_decode_frame(data, 24, false, &frame) == -EINVAL);
	memcpy(data, open_fixture, sizeof(open_fixture));
	for (unsigned int category = 0; category < 256; category++) {
		data[17] = category;
		ret = afkv2_decode_frame(data, 24, false, &frame);
		CHECK(category <= 2 ? ret == 0 : ret == -EPROTO);
	}
	CHECK(!afkv2_encode_oob_command(command, sizeof(command), 0xab,
					0x1122334455667788ULL, 64,
					0x0102030405060708ULL, 16));
	CHECK(!memcmp(command, descriptor_fixture, sizeof(command)));
	CHECK(!afkv2_decode_command(command, sizeof(command), &parsed));
	CHECK(parsed.oob && parsed.id == 0xab && parsed.value == 64);
	CHECK(parsed.rx_address == 0x1122334455667788ULL &&
	      parsed.tx_size == 16);
	CHECK(parsed.inline_size == 0);
	for (size_t i = 0; i < sizeof(command); i++)
		CHECK(afkv2_decode_command(command, i, &response) == -EINVAL);
	command[0] = 2;
	CHECK(afkv2_decode_command(command, sizeof(command), &response) ==
	      -EINVAL);
	memcpy(command, descriptor_fixture, sizeof(command));
	command[2] = 1;
	CHECK(afkv2_decode_command(command, sizeof(command), &response) ==
	      -EINVAL);
	memcpy(command, descriptor_fixture, sizeof(command));
	CHECK(afkv2_encode_oob_command(command, 31, 0, 1, 1, 2, 1) == -ENOSPC);
	CHECK(afkv2_encode_oob_command(command, 32, 0, UINT64_MAX, 2, 0, 0) ==
	      -EINVAL);
	CHECK(afkv2_encode_oob_command(command, 32, 0, 1, AFKV2_MAX_PAYLOAD + 1,
				       0, 0) == -EINVAL);
	CHECK(afkv2_encode_oob_command(command, 32, 0, 1, 0, 0, 0) == -EINVAL);
	memcpy(command, descriptor_fixture, sizeof(command));
	CHECK(!afkv2_encode_frame(data, sizeof(data), 1, 0x1234,
				  0xaabbccddeeff0011ULL, 0xc0, AFKV2_COMMAND, 0,
				  command, sizeof(command), &written));
	CHECK(written == 56 && afkv2_get32(data + 4) == 48);
	CHECK(!afkv2_decode_frame(data, written, false, &frame));
	CHECK(frame.token == 0xaabbccddeeff0011ULL &&
	      frame.category == AFKV2_COMMAND);
	CHECK(!afkv2_encode_error_reply(unaligned, sizeof(unaligned), 2, &frame,
					&parsed, 0xe00002c7U, &written));
	CHECK(!afkv2_decode_frame(unaligned, written, false, &reply));
	CHECK(reply.token == frame.token && reply.interface == frame.interface);
	CHECK(reply.type == frame.type && reply.category == AFKV2_RESPONSE);
	CHECK(!afkv2_decode_command(reply.payload, reply.payload_size,
				    &response));
	CHECK(response.value == 0xe00002c7U && response.rx_size == 0 &&
	      response.tx_size == 0);
	CHECK(response.rx_address == parsed.rx_address &&
	      response.tx_address == parsed.tx_address);
	CHECK(!afkv2_check_response(&reply, &response, 0x1234, 0xc0, 0xab,
				    parsed.rx_address, 64, parsed.tx_address,
				    16));
	response.value = 0;
	response.rx_size = 64;
	response.tx_size = 16;
	CHECK(!afkv2_check_response(&reply, &response, 0x1234, 0xc0, 0xab,
				    parsed.rx_address, 64, parsed.tx_address,
				    16));
	response.rx_address++;
	CHECK(afkv2_check_response(&reply, &response, 0x1234, 0xc0, 0xab,
				   parsed.rx_address, 64, parsed.tx_address,
				   16) == -EPROTO);
	response.rx_address--;
	response.rx_size++;
	CHECK(afkv2_check_response(&reply, &response, 0x1234, 0xc0, 0xab,
				   parsed.rx_address, 64, parsed.tx_address,
				   16) == -EPROTO);
	response.rx_size--;
	CHECK(afkv2_check_response(&reply, &response, 0x1235, 0xc0, 0xab,
				   parsed.rx_address, 64, parsed.tx_address,
				   16) == -EPROTO);
	CHECK(afkv2_check_response(&reply, &response, 0x1234, 0xc1, 0xab,
				   parsed.rx_address, 64, parsed.tx_address,
				   16) == -EPROTO);
	CHECK(afkv2_check_response(&reply, &response, 0x1234, 0xc0, 0xaa,
				   parsed.rx_address, 64, parsed.tx_address,
				   16) == -EPROTO);
	memset(data, 0xa5, sizeof(data));
	memcpy(saved, data, sizeof(data));
	CHECK(afkv2_encode_open(data, 23, 0, 1, &written) == -ENOSPC);
	CHECK(!memcmp(data, saved, sizeof(data)));
	CHECK(afkv2_encode_open(data, sizeof(data), 0, 0, &written) == -EINVAL);
	CHECK(!memcmp(data, saved, sizeof(data)));
	CHECK(afkv2_encode_frame(data, sizeof(data), 0, 1, 0, 0, 0, 0, data,
				 SIZE_MAX, &written) == -E2BIG);
	CHECK(!memcmp(data, saved, sizeof(data)));
	u8 *large = malloc(AFKV2_HEADER_SIZE + AFKV2_MAX_PAYLOAD);

	CHECK(large);
	memset(large, 0x5a, AFKV2_HEADER_SIZE + AFKV2_MAX_PAYLOAD);
	CHECK(!afkv2_encode_frame(large, AFKV2_HEADER_SIZE + AFKV2_MAX_PAYLOAD,
				  0, 1, 0, 0xc0, 1, 0, large, AFKV2_MAX_PAYLOAD,
				  &written));
	CHECK(!afkv2_decode_frame(large, written, false, &frame));
	CHECK(frame.payload_size == AFKV2_MAX_PAYLOAD &&
	      frame.payload[0] == 0x5a);
	free(large);
	bool ids[256] = { 0 };

	for (unsigned int generation = 0; generation < 16; generation++) {
		for (unsigned int slot = 0; slot < 16; slot++) {
			u8 id = afkv2_command_id(generation, slot);

			CHECK(!ids[id]);
			ids[id] = true;
			CHECK((id & 15) == slot);
		}
	}
	CHECK(afkv2_command_id(16, 3) == afkv2_command_id(0, 3));
	CHECK(afkv2_owned_span(0x1000, 32, 0x1000, 32));
	CHECK(afkv2_owned_span(0x1000, 32, 0x101c, 4));
	CHECK(!afkv2_owned_span(0x1000, 32, 0x0fff, 1));
	CHECK(!afkv2_owned_span(0x1000, 32, 0x101d, 4));
	CHECK(!afkv2_owned_span(0x1000, 32, 0x1020, 1));
	CHECK(!afkv2_owned_span(0x1000, 0, 0x1000, 0));
	CHECK(afkv2_owned_span(UINT64_MAX - 15, 16, UINT64_MAX, 1));
	CHECK(!afkv2_owned_span(UINT64_MAX - 15, 17, UINT64_MAX, 1));
	struct afkv2_command callback = { .oob = true, .tx_size = 96,
		.rx_size = 144, .value = 144 };
	size_t input = 0, output = 0;

	CHECK(!afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	CHECK(input == 96 && output == 144);
	callback.value = 143;
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	callback.value = 2048;
	callback.rx_size = 2048;
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	callback.rx_size = 67;
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	callback.rx_size = 144;
	callback.tx_size = 67;
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	callback.oob = false;
	callback.inline_size = 96;
	CHECK(!afkv2_callback_lengths(&callback, 68, 1024, &input, &output));
	CHECK(input == 96 && output == 96);
	CHECK(afkv2_callback_lengths(NULL, 68, 1024, &input, &output));
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, NULL, &output));
	CHECK(afkv2_callback_lengths(&callback, 68, 1024, &input, NULL));
	CHECK(afkv2_callback_lengths(&callback, 1025, 1024, &input, &output));
	printf("PASS %u assertions\n", assertions);
	return 0;
}
