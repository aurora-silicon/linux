// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_h16_execute.h"

#define TEST_COMMAND_SIZE 0xcd8

static const struct ane_h16_request_key test_key = {
	.uuid = 0x8877665544332211ULL,
	.program = 0x10203040,
	.process = 0x50607080,
	.procedure = 0x90a0b0c0,
};

static struct ane_h16_execute_buffer test_buffer(void)
{
	return (struct ane_h16_execute_buffer) {
		.index = 7,
		.class = 1,
		.mapping_dva = 0x12300000000ULL,
		.mapping_size = 0x300000000ULL,
		.offset = 0x10000,
		.size = 0x200000000ULL,
	};
}

static void ane_h16_execute_layout_test(struct kunit *test)
{
	static const u8 header[] = {
		0, 0, 0, 0, 0x0c, 2, 0, 0,
		0x40, 0x30, 0x20, 0x10, 0x80, 0x70, 0x60, 0x50,
		0xc0, 0xb0, 0xa0, 0x90, 0, 0, 0, 0,
		5, 0, 0, 0, 0, 0, 0, 0,
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		1, 0, 0, 0,
	};
	static const u8 descriptor[] = {
		1, 0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 1, 0, 0x23, 1, 0, 0,
		0, 0, 0, 0, 2, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,
	};
	struct ane_h16_execute_buffer buffer = test_buffer();
	struct ane_h16_execute call = { test_key, &buffer, 1, NULL, 0 };
	u8 *storage = kunit_kmalloc(test, TEST_COMMAND_SIZE + 2, GFP_KERNEL);
	u8 *p;

	KUNIT_ASSERT_NOT_NULL(test, storage);
	memset(storage, 0xa5, TEST_COMMAND_SIZE + 2);
	p = storage + 1; /* Exercise unaligned output and both destination guards. */
	KUNIT_ASSERT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), 0);
	KUNIT_EXPECT_MEMEQ(test, p, header, sizeof(header));
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p + sizeof(header), 0, 0x60 - sizeof(header)), NULL);
	KUNIT_EXPECT_MEMEQ(test, p + 0x60, descriptor, sizeof(descriptor));
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p + 0x90, 0, 0xca8 - 0x90), NULL);
	KUNIT_EXPECT_EQ(test, storage[0], (u8)0xa5);
	KUNIT_EXPECT_EQ(test, p[0xca8], (u8)0xa5);
}

static void ane_h16_execute_counts_test(struct kunit *test)
{
	struct ane_h16_execute_buffer *buffers;
	struct ane_h16_execute call = { .key = test_key };
	u8 *p = kunit_kmalloc(test, TEST_COMMAND_SIZE, GFP_KERNEL);
	unsigned int n, i;

	buffers = kunit_kcalloc(test, 64, sizeof(*buffers), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_NOT_NULL(test, buffers);
	for (i = 0; i < 64; i++) {
		buffers[i] = test_buffer();
		buffers[i].index = i;
		buffers[i].class = 8; /* Numeric classes have no guessed semantic name. */
	}
	call.buffers = buffers;
	for (n = 1; n <= 64; n++) {
		size_t end = 0x60 + n * 0x30;

		call.buffer_count = n;
		memset(p, 0xa5, TEST_COMMAND_SIZE);
		KUNIT_ASSERT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + 0x28), n);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + end - 0x30 + 4), n - 1);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + end - 0x30 + 8), 8U);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p + end, 0, 0xca8 - end), NULL);
	}
}

static void ane_h16_custom_bar_test(struct kunit *test)
{
	static const u8 first_bar[] = {
		4, 3, 2, 1, 8, 7, 6, 5, 12, 11, 10, 9,
		0, 32, 0x34, 0x12, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
	};
	struct ane_h16_execute_buffer buffer = test_buffer();
	struct ane_h16_custom_bar bars[2] = {
		{ 0x01020304, 0x05060708, 0x090a0b0c, 0, 32, 0x1234,
		  0x1112131415161718ULL },
		{ 0x01020304, 0x05060708, 0x090a0b0c, 32, 32, 0, 0x2122232425262728ULL },
	};
	struct ane_h16_execute call = { test_key, &buffer, 1, bars, 0 };
	u8 *p = kunit_kmalloc(test, TEST_COMMAND_SIZE + 1, GFP_KERNEL);
	unsigned int n;

	KUNIT_ASSERT_NOT_NULL(test, p);
	for (n = 0; n <= 2; n++) {
		size_t length = 0xca8 + n * 0x18;

		call.bar_count = n;
		memset(p, 0xa5, TEST_COMMAND_SIZE + 1);
		KUNIT_ASSERT_EQ(test, ane_h16_execute_encode(p, length, &call), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + 0xca0), n);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + 0xca4), 0U);
		KUNIT_EXPECT_EQ(test, p[length], (u8)0xa5);
		if (n)
			KUNIT_EXPECT_MEMEQ(test, p + 0xca8, first_bar, sizeof(first_bar));
		if (n == 2) {
			KUNIT_EXPECT_EQ(test, p[0xccc], (u8)32);
			KUNIT_EXPECT_EQ(test, p[0xccd], (u8)32);
			KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0xcd0),
					0x2122232425262728ULL);
		}
	}
}

static void ane_h16_execute_rejections_test(struct kunit *test)
{
	struct ane_h16_execute_buffer buffer = test_buffer();
	struct ane_h16_execute call = { test_key, &buffer, 1, NULL, 0 };
	u8 *p = kunit_kmalloc(test, TEST_COMMAND_SIZE, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, p);
	memset(p, 0xa5, TEST_COMMAND_SIZE);
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(NULL, 0xca8, &call), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, NULL), -EINVAL);
	for (i = 0; i < 4; i++) {
		static const size_t lengths[] = { 0, 0xca7, 0xca9, 0xcc0 };

		KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, lengths[i], &call), -EINVAL);
	}
	call.buffer_count = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	call.buffer_count = 65;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	call.buffer_count = 1;
	call.buffers = NULL;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	call.buffers = &buffer;
	call.bar_count = 3;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcf0, &call), -EINVAL);
	call.bar_count = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcc0, &call), -EINVAL);
	call.bar_count = 0;
	buffer.size = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	buffer = test_buffer();
	buffer.offset = buffer.mapping_size + 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	buffer.offset = buffer.mapping_size;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	buffer = test_buffer();
	buffer.size = buffer.mapping_size - buffer.offset + 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	buffer = test_buffer();
	buffer.mapping_dva = U64_MAX - buffer.mapping_size + 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p, 0xa5, TEST_COMMAND_SIZE), NULL);
	/* The exact last byte of a mapping is valid. */
	buffer = test_buffer();
	buffer.offset = buffer.mapping_size - 1;
	buffer.size = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xca8, &call), 0);
}

static void ane_h16_bar_rejections_test(struct kunit *test)
{
	struct ane_h16_execute_buffer buffer = test_buffer();
	struct ane_h16_custom_bar bars[2] = { { .bit_width = 32 }, { .bit_width = 32 } };
	struct ane_h16_execute call = { test_key, &buffer, 1, bars, 2 };
	u8 *p = kunit_kmalloc(test, TEST_COMMAND_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, p);
	memset(p, 0xa5, TEST_COMMAND_SIZE);
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), -EINVAL);
	bars[1].bit_offset = 31;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), -EINVAL);
	bars[1].bit_offset = 33;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), -EINVAL);
	bars[1].bit_offset = 255;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), -EINVAL);
	bars[1].bit_offset = 32;
	bars[1].bit_width = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p, 0xa5, TEST_COMMAND_SIZE), NULL);
	bars[1].bit_width = 32;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), 0);
	bars[1].bit_offset = 0;
	bars[1].bar = 1; /* Distinct BARs do not overlap. */
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), 0);
	bars[1].bar = 0;
	bars[1].configuration = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_execute_encode(p, 0xcd8, &call), 0);
}

static void ane_h16_test_event(u8 *p, u32 state)
{
	memset(p, 0, 0x60);
	put_unaligned_le32(0x12345678, p); /* A rolling word, not fe0N. */
	put_unaligned_le16(0x300, p + 4);
	put_unaligned_le64(test_key.uuid, p + 8);
	put_unaligned_le32(test_key.program, p + 0x10);
	put_unaligned_le32(test_key.process, p + 0x14);
	put_unaligned_le32(test_key.procedure, p + 0x18);
	put_unaligned_le32(state, p + 0x1c);
	put_unaligned_le32(10, p + 0x20);
	put_unaligned_le64(0x123456789abcdef0ULL, p + 0x28);
	memset(p + 0x31, 0xff, 0x2f); /* Diagnostics do not decide completion. */
}

static void ane_h16_event_correlation_test(struct kunit *test)
{
	static const int results[] = { 0, 1, 2, 0 };
	struct ane_h16_event e;
	u8 storage[0x61], *p = storage + 1;
	unsigned int state, n;

	for (state = 0; state < 4; state++) {
		ane_h16_test_event(p, state);
		KUNIT_ASSERT_EQ(test, ane_h16_event_decode(p, 0x60, 6, &e), 0);
		KUNIT_EXPECT_EQ(test, e.key.uuid, test_key.uuid);
		KUNIT_EXPECT_EQ(test, e.state, state);
		KUNIT_EXPECT_EQ(test, e.cpu_load, 10U);
		KUNIT_EXPECT_EQ(test, e.timestamp, 0x123456789abcdef0ULL);
		KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, &test_key), results[state]);
		for (n = 0; n < 4; n++) {
			put_unaligned_le32(0xfe00 + n, p);
			KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, &test_key),
					results[state]);
		}
	}
	ane_h16_test_event(p, 1);
	for (n = 0; n < 4; n++) {
		struct ane_h16_request_key other = test_key;

		switch (n) {
		case 0:
			other.uuid ^= BIT_ULL(63);
			break;
		case 1:
			other.program++;
			break;
		case 2:
			other.process++;
			break;
		case 3:
			other.procedure++;
			break;
		}
		KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, &other), -EPROTO);
	}
	p[0x30] = 1; /* ISP traffic cannot retire a general scheduler request. */
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, &test_key), -ENOMSG);
}

static void ane_h16_event_rejections_test(struct kunit *test)
{
	struct ane_h16_event e;
	u8 p[0x61];
	size_t n;

	ane_h16_test_event(p, 1);
	memset(&e, 0xa5, sizeof(e));
	for (n = 0; n < 0x60; n++)
		KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, n, 6, &e), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, sizeof(p), 6, &e), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, 0x60, 7, &e), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(NULL, 0x60, 6, &e), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, 0x60, 6, NULL), -EPROTO);
	put_unaligned_le16(0x20c, p + 4); /* An ACK is never a terminal event. */
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 7, &test_key), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, &test_key), -EPROTO);
	put_unaligned_le16(0x300, p + 4);
	put_unaligned_le16(0x100, p + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, 0x60, 6, &e), -EPROTO);
	put_unaligned_le16(0, p + 6);
	put_unaligned_le32(4, p + 0x1c);
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, 0x60, 6, &e), -EPROTO);
	put_unaligned_le32(1, p + 0x1c);
	p[0x30] = 2;
	KUNIT_EXPECT_EQ(test, ane_h16_event_decode(p, 0x60, 6, &e), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(p, 0x60, 6, NULL), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(&e, 0xa5, sizeof(e)), NULL);
}

static struct kunit_case ane_h16_execute_cases[] = {
	KUNIT_CASE(ane_h16_execute_layout_test),
	KUNIT_CASE(ane_h16_execute_counts_test),
	KUNIT_CASE(ane_h16_custom_bar_test),
	KUNIT_CASE(ane_h16_execute_rejections_test),
	KUNIT_CASE(ane_h16_bar_rejections_test),
	KUNIT_CASE(ane_h16_event_correlation_test),
	KUNIT_CASE(ane_h16_event_rejections_test),
	{}
};

static struct kunit_suite ane_h16_execute_suite = {
	.name = "ane-h16-execute",
	.test_cases = ane_h16_execute_cases,
};

kunit_test_suite(ane_h16_execute_suite);
MODULE_DESCRIPTION("KUnit tests for H16 execute serialization and event correlation");
MODULE_LICENSE("Dual MIT/GPL");
