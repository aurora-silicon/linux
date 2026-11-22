// SPDX-License-Identifier: GPL-2.0-only OR MIT
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "epic/dpavserv-edid.h"

static unsigned int assertions;
#define CHECK(x) do { assertions++; assert(x); } while (0)

static void checksum(u8 *block)
{
	u8 sum = 0;
	unsigned int i;

	block[127] = 0;
	for (i = 0; i < 127; i++)
		sum += block[i];
	block[127] = -sum;
}

int main(void)
{
	u8 command[192], packet[216], body[136] = {0};
	u8 edid[DPAV_EDID_HEADER_SIZE + 8 + 3 * 128] = {0};
	struct afkv2_frame frame;
	struct afkv2_command reply;
	size_t written, length = 0;
	u8 *base = edid + DPAV_EDID_HEADER_SIZE + 8;

	CHECK(!afkv2_encode_inline_command(command, sizeof(command), 0x91,
					   body, sizeof(body), 400));
	CHECK(command[0] == 0 && command[1] == 0x91);
	CHECK(afkv2_get32(command + 4) == 400);
	CHECK(!afkv2_encode_frame(packet, sizeof(packet), 5, 4, 0, 0xc0,
				  AFKV2_RESPONSE, 0, command, 8 + sizeof(body), &written));
	/* Response uses status in the request-capacity field. */
	afkv2_put32(packet + AFKV2_HEADER_SIZE + 4, 0);
	CHECK(!afkv2_decode_frame(packet, written, false, &frame));
	CHECK(!afkv2_decode_command(frame.payload, frame.payload_size, &reply));
	CHECK(!afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 135));
	CHECK(afkv2_check_inline_response(&frame, &reply, 5, 0xc0, 0x91, 136));
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x92, 136));
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc1, 0x91, 136));
	frame.category = AFKV2_COMMAND;
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	frame.category = AFKV2_RESPONSE;
	frame.token = 1;
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	frame.token = 0;
	/* R30 responses carry these opaque nonzero header fields. */
	frame.transport_flags = 0x18;
	frame.interface_flags = 0x0400;
	frame.reserved = 0x3607ebe0;
	CHECK(!afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	/* Context and bounds remain mandatory even with opaque header values. */
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x92, 136));
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 135));
	frame.transport_flags = 0;
	frame.interface_flags = 0;
	frame.reserved = 0;
	reply.oob = true;
	CHECK(afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	reply.oob = false;
	/* The original OOB matcher must continue rejecting inline replies. */
	CHECK(afkv2_check_response(&frame, &reply, 4, 0xc0, 0x91, 0, 136, 0, 0));
	reply.value = 7;
	CHECK(!afkv2_check_inline_response(&frame, &reply, 4, 0xc0, 0x91, 136));
	CHECK(reply.value != 0); /* Error status is preserved for the caller. */
	CHECK(afkv2_encode_inline_command(command, 7, 1, body, 1, 1));
	CHECK(afkv2_encode_inline_command(command, sizeof(command), 1, NULL, 1, 1));
	CHECK(afkv2_encode_inline_command(command, sizeof(command), 1, body, 1,
					  AFKV2_MAX_PAYLOAD + 1));

	afkv2_put16(body + 2, 1);
	afkv2_put32(body + 4, 7);
	afkv2_put32(body + 8, 72);
	afkv2_put32(body + 12, 0x69706378);
	CHECK(!afkv2_service_reply_size(body, 136, 1, 7, 72, &length) && length == 72);
	CHECK(afkv2_service_reply_size(body, 135, 1, 7, 72, &length));
	CHECK(afkv2_service_reply_size(body, 136, 1, 7, 71, &length));
	CHECK(afkv2_service_reply_size(body, 63, 1, 7, 72, &length));
	CHECK(afkv2_service_reply_size(body, 136, 2, 7, 72, &length));
	CHECK(afkv2_service_reply_size(body, 136, 1, 8, 72, &length));
	body[12] ^= 1;
	CHECK(afkv2_service_reply_size(body, 136, 1, 7, 72, &length));

	afkv2_put64(edid, EDID_BUF_SIZE);
	afkv2_put64(edid + 32, 8 + 128);
	memset(base + 1, 255, 6);
	base[18] = 1;
	base[19] = 4;
	checksum(base);
	CHECK(!dpavserv_edid_size(edid, 48 + 8 + 128, &length) && length == 128);
	CHECK(dpavserv_edid_size(edid, 47, &length));
	CHECK(dpavserv_edid_size(edid, 48 + 8 + 127, &length));
	base[0] = 1;
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	base[0] = 0;
	base[24] ^= 1;
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	base[24] ^= 1;
	base[126] = 1;
	checksum(base);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	afkv2_put64(edid + 32, 8 + 256);
	CHECK(!dpavserv_edid_size(edid, 48 + 8 + 256, &length) && length == 256);
	/* HF-EEODB can supply more complete blocks than base[126] declares. */
	base[126] = 0;
	checksum(base);
	CHECK(!dpavserv_edid_size(edid, sizeof(edid), &length) && length == 256);
	afkv2_put64(edid + 32, 8 + 255);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	afkv2_put64(edid + 32, EDID_BUF_SIZE + 1);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	afkv2_put64(edid + 32, 8 + 128);
	afkv2_put64(edid, EDID_BUF_SIZE - 1);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length));
	/* A reply without EDID bytes: not read from the sink yet. */
	afkv2_put64(edid, EDID_BUF_SIZE);
	afkv2_put64(edid + 32, 0);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length) == -ENODATA);
	afkv2_put64(edid + 32, 8);
	CHECK(dpavserv_edid_size(edid, sizeof(edid), &length) == -EIO);
	printf("PASS: inline AFKV2 and EDID validation (%u checks)\n", assertions);
	return 0;
}
