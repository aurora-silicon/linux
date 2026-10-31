// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_pending.h"
#include "ane_h16_resources.h"

static void make_event(u8 *bytes, const struct ane_h16_pending *request, u32 state)
{
	memset(bytes, 0, 0x60);
	put_unaligned_le32(0x12345678, bytes);
	put_unaligned_le16(ANE_H16_EVENT_REPORT, bytes + 4);
	put_unaligned_le64(request->key.uuid, bytes + 8);
	put_unaligned_le32(request->key.program, bytes + 0x10);
	put_unaligned_le32(request->key.process, bytes + 0x14);
	put_unaligned_le32(request->key.procedure, bytes + 0x18);
	put_unaligned_le32(state, bytes + 0x1c);
}

static void pending_sequence_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending a = { .key = { .program = 7, .process = 8, .procedure = 9 },
				     .deadline = 100 };
	struct ane_h16_pending b = { .key = a.key, .deadline = 100 };
	u8 event[0x60], ack[8] = {};
	u32 state;

	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &a, 0), 0);
	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &b, 0), 0);
	KUNIT_EXPECT_NE(test, a.key.uuid, b.key.uuid);
	/* A valid transport acknowledgment leaves both UUIDs pending. */
	put_unaligned_le16(ANE_H16_EXECUTE, ack + 4);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(0x10001, 0xca8, 0,
						  0x10000, 0xca8, ANE_H16_EXECUTE, ack, true), 0);
	KUNIT_EXPECT_TRUE(test, a.active);
	KUNIT_EXPECT_TRUE(test, b.active);
	for (state = 0; state <= 3; state += 3) {
		make_event(event, &a, state);
		KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1),
				ANE_H16_EVENT_PROGRESS);
		KUNIT_EXPECT_TRUE(test, a.active);
		KUNIT_EXPECT_FALSE(test, a.terminal);
	}
	make_event(event, &b, 1);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 2),
			ANE_H16_EVENT_COMPLETE);
	KUNIT_EXPECT_TRUE(test, b.terminal);
	KUNIT_EXPECT_EQ(test, b.status, 0);
	KUNIT_EXPECT_FALSE(test, b.active);
	KUNIT_EXPECT_TRUE(test, a.active);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 3), -ENOENT);
	make_event(event, &a, 1);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 4),
			ANE_H16_EVENT_COMPLETE);
	KUNIT_EXPECT_FALSE(test, table.quarantined);
}

static void pending_mismatched_ids_test(struct kunit *test)
{
	unsigned int offset;

	for (offset = 0x10; offset <= 0x18; offset += 4) {
		struct ane_h16_pending_table table = {};
		struct ane_h16_pending request = { .deadline = 100 };
		u8 event[0x60];

		KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
		make_event(event, &request, 1);
		put_unaligned_le32(1, event + offset);
		KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1), -EPROTO);
		KUNIT_EXPECT_TRUE(test, request.active);
		KUNIT_EXPECT_FALSE(test, request.terminal);
		KUNIT_EXPECT_TRUE(test, table.quarantined);
		KUNIT_EXPECT_EQ(test, table.error, -EPROTO);
		ane_h16_pending_abort(&table, &request, -EIO);
		KUNIT_EXPECT_FALSE(test, request.active);
	}
}

static void pending_malformed_test(struct kunit *test)
{
	unsigned int variant;

	for (variant = 0; variant < 9; variant++) {
		struct ane_h16_pending_table table = {};
		struct ane_h16_pending request = { .deadline = 100 };
		u8 event[0x60];
		unsigned int channel = 6;
		size_t length = sizeof(event);
		const void *bytes = event;

		KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
		make_event(event, &request, 1);
		switch (variant) {
		case 0:
			bytes = NULL;
			break;
		case 1:
			length--;
			break;
		case 2:
			length++;
			break;
		case 3:
			channel = 7;
			break;
		case 4:
			put_unaligned_le16(ANE_H16_EXECUTE, event + 4);
			break;
		case 5:
			event[6] = 1;
			break;
		case 6:
			event[7] = 1;
			break;
		case 7:
			put_unaligned_le32(4, event + 0x1c);
			break;
		case 8:
			event[0x30] = 2;
			break;
		}
		KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, bytes, length, channel, 1),
				-EPROTO);
		KUNIT_EXPECT_TRUE(test, request.active);
		KUNIT_EXPECT_TRUE(test, table.quarantined);
	}
}

static void pending_routing_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending request = { .deadline = 100 };
	u8 event[0x60];

	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
	make_event(event, &request, 1);
	event[0x30] = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1), -ENOMSG);
	event[0x30] = 0;
	put_unaligned_le64(request.key.uuid + 1, event + 8);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1), -ENOENT);
	memset(event, 0, sizeof(event));
	put_unaligned_le16(0x0107, event + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0xc, 6, 1),
			ANE_H16_EVENT_PROGRESS);
	KUNIT_EXPECT_TRUE(test, request.active);
	KUNIT_EXPECT_FALSE(test, table.quarantined);
	event[7] = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0xc, 6, 1), -EPROTO);
	KUNIT_EXPECT_TRUE(test, table.quarantined);
}

static void pending_failure_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending request = { .deadline = 100 };
	struct ane_h16_pending next = { .deadline = 100 };
	u8 event[0x60];

	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
	make_event(event, &request, 2);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1),
			ANE_H16_EVENT_FAILED);
	KUNIT_EXPECT_FALSE(test, request.active);
	KUNIT_EXPECT_TRUE(test, request.terminal);
	KUNIT_EXPECT_EQ(test, request.status, -EIO);
	KUNIT_EXPECT_TRUE(test, table.quarantined);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_insert(&table, &next, 2), -EIO);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 2), -EIO);
}

static void pending_timeout_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending request = { .deadline = 100 };
	u8 event[0x60];

	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
	make_event(event, &request, 1);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 100), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, request.status, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, table.error, -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, request.terminal);
	KUNIT_EXPECT_FALSE(test, request.active);
	KUNIT_EXPECT_TRUE(test, table.quarantined);
	/* Late success never makes output readable after timeout. */
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 101), -EIO);
}

static void pending_transport_failure_test(struct kunit *test)
{
	static const int errors[] = { -ETIMEDOUT, -EPROTO, -EREMOTEIO, -EIO };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		struct ane_h16_pending_table table = {};
		struct ane_h16_pending request = { .deadline = 100 };

		KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, 0), 0);
		ane_h16_pending_abort(&table, &request, errors[i]);
		KUNIT_EXPECT_FALSE(test, request.active);
		KUNIT_EXPECT_FALSE(test, request.terminal);
		KUNIT_EXPECT_EQ(test, request.status, errors[i]);
		KUNIT_EXPECT_TRUE(test, table.quarantined);
	}
}

static void pending_capacity_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending *requests;
	unsigned int i;

	requests = kunit_kcalloc(test, ANE_H16_PENDING_MAX + 1, sizeof(*requests), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, requests);
	for (i = 0; i <= ANE_H16_PENDING_MAX; i++)
		requests[i].deadline = 100;
	for (i = 0; i < ANE_H16_PENDING_MAX; i++) {
		KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &requests[i], 0), 0);
		KUNIT_EXPECT_EQ(test, requests[i].key.uuid, (u64)i + 1);
	}
	KUNIT_EXPECT_EQ(test, ane_h16_pending_insert(&table, &requests[i], 0), -ENOSPC);
	ane_h16_pending_remove(&table, &requests[0]);
	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &requests[i], 0), 0);
	KUNIT_EXPECT_EQ(test, requests[i].key.uuid, (u64)ANE_H16_PENDING_MAX + 1);
	ane_h16_pending_remove(&table, &requests[i]);
	table.next_uuid = U64_MAX;
	KUNIT_EXPECT_EQ(test, ane_h16_pending_insert(&table, &requests[i], 0), -EOVERFLOW);
}

static void pending_deadline_wrap_test(struct kunit *test)
{
	struct ane_h16_pending_table table = {};
	struct ane_h16_pending request = { .deadline = 3 };
	u8 event[0x60];

	KUNIT_ASSERT_EQ(test, ane_h16_pending_insert(&table, &request, ULONG_MAX - 4), 0);
	make_event(event, &request, 1);
	KUNIT_EXPECT_EQ(test, ane_h16_pending_dispatch(&table, event, 0x60, 6, 1),
			ANE_H16_EVENT_COMPLETE);
	request.deadline = 3;
	KUNIT_EXPECT_EQ(test, ane_h16_pending_insert(&table, &request, 3), -EINVAL);
}

static void resources_test(struct kunit *test)
{
	struct ane_h16_hwx hwx = { .surface_count = 2 };
	struct ane_h16_resource resources[2] = {};
	unsigned int i, variant;

	for (i = 0; i < 2; i++) {
		hwx.surfaces[i].section.vm = 0x30000000 + i * SZ_16K;
		hwx.surfaces[i].section.size = SZ_16K;
		hwx.surfaces[i].allocation = SZ_16K;
		hwx.surfaces[i].span = 256;
		hwx.surfaces[i].direction = i + 1;
		resources[i].hwx_vm = hwx.surfaces[i].section.vm;
		resources[i].permissions = i + 1;
		resources[i].buffer = (struct ane_h16_execute_buffer) {
			.index = i, .class = i, .mapping_dva = 0x100000000ULL + i * SZ_16K,
			.mapping_size = SZ_16K, .size = 256,
		};
	}
	KUNIT_EXPECT_EQ(test, ane_h16_resources_validate(&hwx, resources, 2), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_resources_validate(&hwx, resources, 1), -EINVAL);
	for (variant = 0; variant < 9; variant++) {
		struct ane_h16_resource bad[2];

		memcpy(bad, resources, sizeof(bad));
		switch (variant) {
		case 0:
			bad[0].permissions = 2;
			break;
		case 1:
			bad[0].hwx_vm++;
			break;
		case 2:
			bad[0].buffer.class = 8;
			break;
		case 3:
			bad[0].buffer.size++;
			break;
		case 4:
			bad[0].buffer.offset = 1;
			break;
		case 5:
			bad[0].buffer.mapping_dva = U64_MAX - SZ_16K + 1;
			break;
		case 6:
			bad[1].buffer.index = bad[0].buffer.index;
			break;
		case 7:
			bad[1].buffer.mapping_dva = bad[0].buffer.mapping_dva;
			break;
		case 8:
			bad[0].buffer.mapping_size--;
			break;
		}
		KUNIT_EXPECT_EQ(test, ane_h16_resources_validate(&hwx, bad, 2), -EINVAL);
	}
}

static struct kunit_case pending_cases[] = {
	KUNIT_CASE(pending_sequence_test),
	KUNIT_CASE(pending_mismatched_ids_test),
	KUNIT_CASE(pending_malformed_test),
	KUNIT_CASE(pending_routing_test),
	KUNIT_CASE(pending_failure_test),
	KUNIT_CASE(pending_timeout_test),
	KUNIT_CASE(pending_transport_failure_test),
	KUNIT_CASE(pending_capacity_test),
	KUNIT_CASE(pending_deadline_wrap_test),
	KUNIT_CASE(resources_test),
	{}
};

static struct kunit_suite pending_suite = {
	.name = "ane-h16-pending",
	.test_cases = pending_cases,
};

kunit_test_suite(pending_suite);
MODULE_LICENSE("Dual MIT/GPL");
