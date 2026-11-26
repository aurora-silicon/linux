// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by afk.c to exercise the EPIC receive handlers. */
#include <kunit/device.h>
#include <kunit/test.h>

static int afk_parser_test_init(struct kunit *test)
{
	struct apple_dcp_afkep *ep;
	struct apple_dcp *dcp;

	ep = kunit_kzalloc(test, sizeof(*ep), GFP_KERNEL);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	if (!ep || !dcp)
		return -ENOMEM;
	dcp->dev = kunit_device_register(test, "afk-parser");
	if (IS_ERR(dcp->dev))
		return PTR_ERR(dcp->dev);
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	afk_ep_init_state(ep);
	ep->dcp = dcp;
	ep->num_channels = 1;
	ep->services[0].channel = 1;
	ep->services[0].enabled = true;
	test->priv = ep;
	return 0;
}

static void afk_compact_header_test(struct kunit *test)
{
	u8 record[sizeof(struct epic_hdr) + sizeof(struct epic_sub_hdr_h17p)] = {};
	unsigned int len;

	for (len = 0; len < sizeof(record); len++)
		KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(NULL, len));
	put_unaligned_le32(sizeof(record) - 8, record + 4);
	KUNIT_EXPECT_TRUE(test, afk_validate_h17p_header(record, sizeof(record)));
	KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(record, sizeof(record) + 1));
	put_unaligned_le32(U32_MAX, record + 4);
	KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(record, sizeof(record)));
}

static void afk_short_epic_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	unsigned int len;

	for (len = 0; len < sizeof(struct epic_hdr) + sizeof(struct epic_sub_hdr_h17p); len++)
		afk_recv_handle(ep, 0, EPIC_TYPE_NOTIFY, NULL, len);
	for (len = 0; len < 32; len++)
		afk_recv_handle_init(ep, 2, NULL, len);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);
}

static void afk_short_reply_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct epic_cmd_info *pending = &ep->services[0].cmds[0];
	unsigned int len;

	set_bit(0, ep->services[0].cmd_map);
	pending->tag = 0x200;
	for (len = 0; len < sizeof(struct epic_cmd); len++) {
		afk_recv_handle_reply(ep, 1, 0x200, NULL, len);
		KUNIT_EXPECT_FALSE(test, pending->done);
	}
}

static void afk_reply_index_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct epic_cmd_info *pending = &ep->services[0].cmds[0];
	struct epic_cmd reply = { .retcode = cpu_to_le32(7) };

	set_bit(0, ep->services[0].cmd_map);
	pending->tag = 0x200;
	afk_recv_handle_reply(ep, 1, MAX_PENDING_CMDS, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x2ff, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x300, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 2, 0x200, NULL, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x200, &reply, sizeof(reply));
	KUNIT_EXPECT_TRUE(test, pending->done);
	KUNIT_EXPECT_EQ(test, pending->retcode, 7U);
	reply.retcode = cpu_to_le32(9);
	afk_recv_handle_reply(ep, 1, 0x200, &reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test, pending->retcode, 7U);
}

static void afk_serialized_report_test(struct kunit *test)
{
	u8 dictionary[8] = {};
	unsigned int length;

	put_unaligned_le32(0xd3, dictionary);
	put_unaligned_le32(1U << 24, dictionary + 4);
	for (length = 0; length < sizeof(dictionary); length++)
		KUNIT_EXPECT_FALSE(test, afk_compact_report_dictionary(dictionary, length));
	KUNIT_EXPECT_TRUE(test, afk_compact_report_dictionary(dictionary, sizeof(dictionary)));
	put_unaligned_le32(0, dictionary);
	KUNIT_EXPECT_FALSE(test, afk_compact_report_dictionary(dictionary, sizeof(dictionary)));
}

struct compact_call_report_capture {
	unsigned int count;
	size_t size;
	u16 group;
	u32 command;
	u32 magic;
	u16 pa;
};

static int compact_call_report_cb(struct apple_epic_service *service,
				  enum epic_subtype type,
				  const void *data, size_t data_size)
{
	struct compact_call_report_capture *cap = service->cookie;
	const struct epic_service_call *call = data;

	cap->count++;
	cap->size = data_size;
	if (data_size >= sizeof(*call) + sizeof(u16)) {
		cap->group = le16_to_cpu(call->group);
		cap->command = le32_to_cpu(call->command);
		cap->magic = le32_to_cpu(call->magic);
		cap->pa = get_unaligned_le16((const u8 *)(call + 1));
	}
	return 0;
}

static const struct apple_epic_service_ops compact_call_report_ops = {
	.name = "compact-report-test",
	.report = compact_call_report_cb,
};

static void afk_compact_service_call_report_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct compact_call_report_capture cap = {};
	u8 wire[16 + sizeof(struct epic_compact_desc) +
		sizeof(struct epic_service_call) + 0x50] = {};
	struct epic_compact_sub_hdr *sub = (void *)(wire + 8);
	struct epic_service_call *call;

	ep->services[0].ops = &compact_call_report_ops;
	ep->services[0].cookie = &cap;
	sub->category = 0;
	sub->type = EPIC_SUBTYPE_STD_SERVICE;

	/* The bare service-call header immediately follows the subheader. */
	call = (void *)(sub + 1);
	call->group = cpu_to_le16(2);
	call->command = cpu_to_le32(4);
	call->data_len = cpu_to_le32(0x50);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);
	put_unaligned_le16(0x2100, (u8 *)(call + 1));

	afk_recv_compact_message(ep, 1, wire,
				 16 + sizeof(*call) + 0x50);
	KUNIT_EXPECT_EQ(test, cap.count, 1U);
	KUNIT_EXPECT_EQ(test, cap.size, sizeof(*call) + 0x50);
	KUNIT_EXPECT_EQ(test, cap.magic, (u32)EPIC_SERVICE_CALL_MAGIC);
	KUNIT_EXPECT_EQ(test, cap.group, 2);
	KUNIT_EXPECT_EQ(test, cap.command, 4U);
	KUNIT_EXPECT_EQ(test, cap.pa, 0x2100);

	/* Descriptor-wrapped struct epic_service_call report still strips 8 bytes. */
	memset(wire + 16, 0, sizeof(wire) - 16);
	call = (void *)((u8 *)(sub + 1) + sizeof(struct epic_compact_desc));
	call->group = cpu_to_le16(2);
	call->command = cpu_to_le32(4);
	call->data_len = cpu_to_le32(0x50);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);
	put_unaligned_le16(0x3400, (u8 *)(call + 1));

	afk_recv_compact_message(ep, 1, wire, sizeof(wire));
	KUNIT_EXPECT_EQ(test, cap.count, 2U);
	KUNIT_EXPECT_EQ(test, cap.size, sizeof(*call) + 0x50);
	KUNIT_EXPECT_EQ(test, cap.magic, (u32)EPIC_SERVICE_CALL_MAGIC);
	KUNIT_EXPECT_EQ(test, cap.pa, 0x3400);
}

/*
 * Replies to firmware calls wait on the endpoint's transmit queue. While it
 * makes no progress, further calls are taken only as long as their replies
 * fit the room of one message.
 */
struct compact_reply_gate {
	struct work_struct work;
	struct completion open;
};

static void compact_reply_gate_fn(struct work_struct *work)
{
	struct compact_reply_gate *gate =
		container_of(work, struct compact_reply_gate, work);

	wait_for_completion(&gate->open);
}

static int compact_call_count_cb(struct apple_epic_service *service, u16 group,
				 u32 command, const void *data, size_t size,
				 void *reply, size_t room)
{
	unsigned int *count = service->cookie;

	(*count)++;
	return 0;
}

static const struct apple_epic_service_ops compact_call_count_ops = {
	.name = "compact-call-test",
	.call_v2 = compact_call_count_cb,
};

static void afk_compact_call_reply_budget_test(struct kunit *test)
{
	/* Four calls of this size take all the room; the timestamp and sub-header precede. */
	const size_t payload_size = AFK_COMPACT_MESSAGE_MAX / 4;
	const size_t wire_size = 16 + payload_size;
	struct apple_dcp_afkep *ep = test->priv;
	struct compact_reply_gate gate;
	struct epic_compact_sub_hdr *sub;
	struct epic_service_call *call;
	unsigned int handled = 0, i;
	u8 *wire;

	wire = kunit_kzalloc(test, wire_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	sub = (void *)(wire + 8);
	sub->category = 1;
	sub->type = EPIC_SUBTYPE_STD_SERVICE;
	call = (void *)((u8 *)(sub + 1) + sizeof(struct epic_compact_desc));
	call->command = cpu_to_le32(4);
	call->data_len = cpu_to_le32(payload_size - sizeof(struct epic_compact_desc) -
				     sizeof(*call));
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);

	ep->services[0].ops = &compact_call_count_ops;
	ep->services[0].cookie = &handled;
	ep->tx_wq = alloc_ordered_workqueue("afk-parser-test-tx", 0);
	KUNIT_ASSERT_NOT_NULL(test, ep->tx_wq);

	/* The queue is ordered: nothing behind the gate runs until it opens. */
	INIT_WORK_ONSTACK(&gate.work, compact_reply_gate_fn);
	init_completion(&gate.open);
	queue_work(ep->tx_wq, &gate.work);

	/* A call left without a reply must hold off a coordinated sleep. */
	ep->dcp->v27_sleep_transport = true;
	for (i = 0; i < 4; i++)
		afk_recv_compact_message(ep, 1, wire, wire_size);
	KUNIT_EXPECT_FALSE(test, ep->sleep_poisoned);
	afk_recv_compact_message(ep, 1, wire, wire_size);
	KUNIT_EXPECT_EQ(test, handled, 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&ep->sleep_jobs), 4);
	KUNIT_EXPECT_TRUE(test, ep->sleep_poisoned);

	/* A small call no longer fits either. */
	call->data_len = cpu_to_le32(0);
	afk_recv_compact_message(ep, 1, wire,
				 16 + sizeof(struct epic_compact_desc) + sizeof(*call));
	KUNIT_EXPECT_EQ(test, handled, 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&ep->sleep_jobs), 4);

	/* The replies fail without a transmit ring, which frees their room. */
	complete(&gate.open);
	flush_workqueue(ep->tx_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&ep->sleep_jobs), 0);

	afk_recv_compact_message(ep, 1, wire,
				 16 + sizeof(struct epic_compact_desc) + sizeof(*call));
	KUNIT_EXPECT_EQ(test, handled, 5U);

	destroy_workqueue(ep->tx_wq);
	ep->tx_wq = NULL;
	destroy_work_on_stack(&gate.work);
	KUNIT_EXPECT_EQ(test, atomic_read(&ep->sleep_jobs), 0);
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8152)
#include "t8152_display.h"

/*
 * A T8152 DPTX call gets as much reply room as its request has, which the
 * firmware sizes. A reply too short for the status word is refused, and
 * nothing is written past its room.
 */
static void afk_t8152_dptx_short_reply_test(struct kunit *test)
{
	const size_t head = 16 + sizeof(struct epic_compact_desc) +
			    sizeof(struct epic_service_call);
	struct apple_dcp_afkep *ep = test->priv;
	struct apple_epic_service *service = &ep->services[0];
	struct epic_compact_sub_hdr *sub;
	struct epic_service_call *call;
	u8 request[sizeof(u32)] = {}, reply[8];
	size_t room, i;
	u8 *wire;

	service->ops = &t8152_dptx_port_ops;
	service->ep = ep;

	/* The handler itself, with the bytes after its room watched. */
	for (room = 0; room <= sizeof(u32); room++) {
		memset(reply, 0xa5, sizeof(reply));
		KUNIT_EXPECT_EQ_MSG(test,
				    service->ops->call_v2(service, 1, 0, request, room,
							  reply, room),
				    room < sizeof(u32) ? -EINVAL : 0, "room %zu", room);
		for (i = room; i < sizeof(reply); i++)
			KUNIT_EXPECT_EQ_MSG(test, reply[i], 0xa5,
					    "room %zu, byte %zu", room, i);
	}

	/* The whole call from the wire, its reply allocated to the request's size. */
	wire = kunit_kzalloc(test, head + sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);
	sub = (void *)(wire + 8);
	sub->category = 1;
	sub->type = EPIC_SUBTYPE_STD_SERVICE;
	call = (void *)((u8 *)(sub + 1) + sizeof(struct epic_compact_desc));
	call->group = cpu_to_le16(1);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);

	ep->tx_wq = alloc_ordered_workqueue("afk-parser-test-tx", 0);
	KUNIT_ASSERT_NOT_NULL(test, ep->tx_wq);
	for (room = 0; room <= sizeof(u32); room++) {
		call->data_len = cpu_to_le32(room);
		afk_recv_compact_message(ep, 1, wire, head + room);
		flush_workqueue(ep->tx_wq);
	}
	destroy_workqueue(ep->tx_wq);
	ep->tx_wq = NULL;
	KUNIT_EXPECT_EQ(test, atomic_read(&ep->sleep_jobs), 0);
}
#endif

/*
 * Slot reuse for re-announced compact services. The firmware re-announces a
 * service under a fresh interface on every DP relink without closing the old
 * one; afk_compact_slot() decides whether the new announcement supersedes the
 * enabled one it matches (reclaiming its slot) or is a sibling (a fresh slot).
 */
static const struct apple_epic_service_ops compact_singleton_ops = {
	.name = "singleton",
	.singleton = true,
};

static const struct apple_epic_service_ops compact_sibling_ops = {
	.name = "sibling",
};

/* Populate a returned slot as afk_recv_handle_compact_init() would. */
static void compact_slot_fill(struct apple_epic_service *service,
			      const struct apple_epic_service_ops *ops,
			      const char *name, s64 unit, u32 channel)
{
	service->ops = ops;
	strscpy(service->name, name, sizeof(service->name));
	service->unit = unit;
	service->channel = channel;
	service->enabled = true;
}

static void compact_slot_reset(struct apple_dcp_afkep *ep, u32 endpoint)
{
	u32 i;

	ep->endpoint = endpoint;
	ep->num_channels = 0;
	for (i = 0; i < AFK_MAX_CHANNEL; i++) {
		struct apple_epic_service *service = &ep->services[i];

		service->ops = NULL;
		service->enabled = false;
		service->users = 0;
		service->channel = 0;
		service->unit = 0;
		service->power_gen = 0;
		service->name[0] = '\0';
		bitmap_zero(service->cmd_map, MAX_PENDING_CMDS);
	}
	WRITE_ONCE(ep->dcp->afk_power_gen, 0);
}

static void afk_compact_singleton_replaces_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct apple_epic_service *first, *again;

	/* 0x2b (AV_AUDIO_ENDPOINT_DESKTOP) is not in the hotplug range. */
	compact_slot_reset(ep, 0x2b);

	first = afk_compact_slot(ep, &compact_singleton_ops, "singleton", 0);
	KUNIT_ASSERT_PTR_NE(test, first, NULL);
	compact_slot_fill(first, &compact_singleton_ops, "singleton", 0, 10);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);

	/* Re-announced in the same power generation: reuse and retire the slot. */
	again = afk_compact_slot(ep, &compact_singleton_ops, "singleton", 0);
	KUNIT_EXPECT_PTR_EQ(test, again, first);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);
	KUNIT_EXPECT_FALSE(test, again->enabled);
}

static void afk_compact_singleton_no_leak_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct apple_epic_service *service;
	unsigned int i;

	compact_slot_reset(ep, 0x2b);

	/* A relink across every suspend re-announces; the slot budget must hold. */
	for (i = 0; i < 4 * AFK_MAX_CHANNEL; i++) {
		service = afk_compact_slot(ep, &compact_singleton_ops, "singleton", 0);
		KUNIT_ASSERT_PTR_NE(test, service, NULL);
		compact_slot_fill(service, &compact_singleton_ops, "singleton", 0, i);
	}
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);
}

static void afk_compact_siblings_kept_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct apple_epic_service *first, *second;

	compact_slot_reset(ep, 0x2b);

	/* A non-singleton re-announced in one generation is a sibling. */
	first = afk_compact_slot(ep, &compact_sibling_ops, "sibling", -1);
	KUNIT_ASSERT_PTR_NE(test, first, NULL);
	compact_slot_fill(first, &compact_sibling_ops, "sibling", -1, 10);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);

	second = afk_compact_slot(ep, &compact_sibling_ops, "sibling", -1);
	KUNIT_ASSERT_PTR_NE(test, second, NULL);
	KUNIT_EXPECT_PTR_NE(test, second, first);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 2U);
	KUNIT_EXPECT_TRUE(test, first->enabled);
}

/* Named entries (exact or cut) beat "*", regardless of table order. */
static void afk_compact_cut_before_wildcard_test(struct kunit *test)
{
	static const struct apple_epic_service_ops ops[] = {
		{ .name = "dcpav-service-epic" },
		{
			.name = "dcpav-audio-interface-epic",
			.compact_full = "dcpav-audio-interface-epic:0",
		},
		{ .name = "*" },
		{},
	};
	static const struct apple_epic_service_ops wildcard_first_ops[] = {
		{ .name = "*" },
		{ .name = "dcpav-service-epic" },
		{
			.name = "dcpav-audio-interface-epic",
			.compact_full = "dcpav-audio-interface-epic:0",
		},
		{},
	};
	struct apple_dcp_afkep *ep = test->priv;

	ep->ops = ops;
	/* "dispext0:dcpav-audio-interface-epic:0", cut to its 31 characters */
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-audio-interface-"),
			    &ops[1]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "x:dcpav-audio-interface-epic:0"),
			    &ops[1]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-service-epic:0"),
			    &ops[0]);
	/* Anything else, cut or not, is the wildcard's. */
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "ext:dcpav-cec-interface-epic:0"),
			    &ops[2]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-video-interface-"),
			    &ops[2]);

	/* Listing "*" first still lets named entries win, exact or cut. */
	ep->ops = wildcard_first_ops;
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-service-epic:0"),
			    &wildcard_first_ops[1]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "x:dcpav-audio-interface-epic:0"),
			    &wildcard_first_ops[2]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-audio-interface-"),
			    &wildcard_first_ops[2]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "ext:dcpav-cec-interface-epic:0"),
			    &wildcard_first_ops[0]);
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_match_service(ep, "dispext0:dcpav-video-interface-"),
			    &wildcard_first_ops[0]);
}

static struct kunit_case afk_parser_cases[] = {
	KUNIT_CASE(afk_serialized_report_test),
	KUNIT_CASE(afk_compact_service_call_report_test),
	KUNIT_CASE(afk_compact_call_reply_budget_test),
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8152)
	KUNIT_CASE(afk_t8152_dptx_short_reply_test),
#endif
	KUNIT_CASE(afk_compact_header_test),
	KUNIT_CASE(afk_short_epic_test),
	KUNIT_CASE(afk_short_reply_test),
	KUNIT_CASE(afk_reply_index_test),
	KUNIT_CASE(afk_compact_singleton_replaces_test),
	KUNIT_CASE(afk_compact_singleton_no_leak_test),
	KUNIT_CASE(afk_compact_siblings_kept_test),
	KUNIT_CASE(afk_compact_cut_before_wildcard_test),
	{}
};

static struct kunit_suite afk_parser_suite = {
	.name = "apple-epic-parser",
	.init = afk_parser_test_init,
	.test_cases = afk_parser_cases,
};

kunit_test_suite(afk_parser_suite);
