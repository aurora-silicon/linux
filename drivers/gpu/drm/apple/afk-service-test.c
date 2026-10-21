// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"
#include "dptx-service.h"

static const struct apple_epic_service_ops reusable_ops = {
	.name = "reusable",
	.reusable = true,
};

static const struct apple_epic_service_ops permanent_ops = {
	.name = "permanent",
};

static int afk_service_test_init(struct kunit *test)
{
	struct apple_epic_service *service;

	service = kunit_kzalloc(test, sizeof(*service), GFP_KERNEL);
	if (!service)
		return -ENOMEM;
	spin_lock_init(&service->lock);
	test->priv = service;
	return 0;
}

static void afk_test_retire(struct apple_epic_service *service)
{
	unsigned long flags;

	spin_lock_irqsave(&service->lock, flags);
	service->enabled = false;
	service->torndown = true;
	spin_unlock_irqrestore(&service->lock, flags);
}

static void afk_service_many_generations_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;
	unsigned int i;

	/* Repeated unplug/replug must not consume an unbounded slot budget. */
	for (i = 0; i < 4 * AFK_MAX_CHANNEL; i++) {
		KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, i));
		KUNIT_EXPECT_EQ(test, service->channel, i);
		KUNIT_EXPECT_TRUE(test, afk_service_matches(service, i));
		afk_test_retire(service);
	}
}

static void afk_service_reader_pin_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	KUNIT_ASSERT_PTR_EQ(test, afk_service_get(service), service); /* owner */
	KUNIT_ASSERT_PTR_EQ(test, afk_service_get(service), service); /* reader */
	afk_test_retire(service);
	afk_service_put(service); /* owner detaches while EDID is being read */
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_EQ(test, service->channel, 1U);
	KUNIT_EXPECT_PTR_EQ(test, afk_service_get(service), NULL);
	afk_service_put(service);
	KUNIT_EXPECT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void afk_service_pending_reply_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	set_bit(3, service->cmd_map);
	afk_test_retire(service);
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_TRUE(test, afk_service_matches(service, 1));
	KUNIT_EXPECT_FALSE(test, afk_service_matches(service, 2));
	/* A timed-out command retains its DMA and slot until the late reply. */
	service->cmds[3].free_on_ack = true;
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	clear_bit(3, service->cmd_map);
	KUNIT_EXPECT_FALSE(test, afk_service_matches(service, 1));
	KUNIT_EXPECT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void afk_service_completed_reader_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	KUNIT_ASSERT_PTR_EQ(test, afk_service_get(service), service);
	set_bit(0, service->cmd_map);
	afk_test_retire(service);
	/* Reply arrived, but the waiting thread has not consumed it yet. */
	service->cmds[0].done = true;
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	clear_bit(0, service->cmd_map);
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	afk_service_put(service);
	KUNIT_EXPECT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void afk_service_close_pin_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_EXPECT_PTR_EQ(test, afk_service_pin(NULL), NULL);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	KUNIT_ASSERT_PTR_EQ(test, afk_service_get(service), service); /* owner */
	afk_test_retire(service);
	/* A close issued after teardown still pins the owner's service. */
	KUNIT_ASSERT_PTR_EQ(test, afk_service_pin(service), service);
	afk_service_put(service); /* owner detaches */
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_EQ(test, service->channel, 1U);
	afk_service_put(service);
	KUNIT_EXPECT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void afk_service_tag_sequence_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	service->cmd_tag = 0xa9;
	service->cookie = test;
	afk_test_retire(service);
	KUNIT_EXPECT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_EQ(test, service->cmd_tag, (u8)0xa9);
	KUNIT_EXPECT_PTR_EQ(test, service->cookie, NULL);
}

static void afk_service_unconverted_owner_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &permanent_ops, 1));
	afk_test_retire(service);
	KUNIT_EXPECT_FALSE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static int afk_test_prepare(struct apple_epic_service *service,
			    const char *name, const char *class, s64 unit)
{
	return 0;
}

static const struct apple_epic_service_ops prepared_ops = {
	.name = "prepared",
	.prepare = afk_test_prepare,
	.teardown = dptx_service_unbind,
};

static void afk_service_admission_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &prepared_ops, 1));
	/* Neither callback dispatch nor command callers can observe preparation. */
	KUNIT_EXPECT_FALSE(test, afk_service_matches(service, 1));
	KUNIT_EXPECT_PTR_EQ(test, afk_service_get(service), NULL);
	/* A rejected announcement or failed OPEN consumes no service slot. */
	afk_service_discard(service);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &prepared_ops, 2));
	/* Cookie-less services are valid: admission does not test cookie != NULL. */
	afk_service_publish(service);
	KUNIT_EXPECT_TRUE(test, afk_service_matches(service, 2));
	KUNIT_ASSERT_PTR_EQ(test, afk_service_get(service), service);
	afk_service_put(service);
}

static void dptx_service_identity_test(struct kunit *test)
{
	const char *name = "dcpdptx-port-epic";
	const char *class = "AppleDCPDPTXRemotePort";

	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, 3, true, 3, 1), 0);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, 0, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, -1, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, 3, true, 3, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(NULL, class, 3, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, NULL, 3, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port("wrong", class, 3, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, "wrong", 3, true, 3, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, 1, false, 3, 2), 1);
	KUNIT_EXPECT_EQ(test, dptx_service_port(name, class, 2, false, 3, 2), -EINVAL);
}

static void dptx_service_duplicate_close_test(struct kunit *test)
{
	struct apple_epic_service *first = test->priv;
	struct apple_epic_service *next;
	struct dptx_port port = {};

	next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, next);
	spin_lock_init(&next->lock);
	init_completion(&port.enable_completion);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(first, NULL, &prepared_ops, 1));
	KUNIT_ASSERT_EQ(test, dptx_service_bind(first, &port, 3), 0);
	/* An observer during preparation/OPEN sees no available port or owner. */
	KUNIT_EXPECT_FALSE(test, dptx_service_available(&port));
	KUNIT_EXPECT_PTR_EQ(test, port.service, NULL);
	KUNIT_EXPECT_PTR_EQ(test, first->cookie, &port);
	/* Failed OPEN does not publish anything and permits a fresh attempt. */
	afk_service_discard(first);
	KUNIT_EXPECT_FALSE(test, port.enabled);
	KUNIT_EXPECT_PTR_EQ(test, port.service, NULL);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(first, NULL, &prepared_ops, 1));
	KUNIT_ASSERT_EQ(test, dptx_service_bind(first, &port, 3), 0);
	afk_service_publish(first);
	KUNIT_EXPECT_FALSE(test, port.enabled);
	dptx_service_publish(first);
	KUNIT_EXPECT_TRUE(test, dptx_service_available(&port));
	KUNIT_EXPECT_PTR_EQ(test, port.service->cookie, &port);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(next, NULL, &prepared_ops, 2));
	KUNIT_EXPECT_EQ(test, dptx_service_bind(next, &port, 3), -EBUSY);
	afk_service_discard(next);
	KUNIT_EXPECT_TRUE(test, port.enabled);
	KUNIT_EXPECT_PTR_EQ(test, port.service, first);
	/* CLOSE invalidates a connected session without publishing a replacement. */
	port.connected = true;
	afk_test_retire(first);
	dptx_service_unbind(first);
	KUNIT_EXPECT_FALSE(test, port.enabled);
	KUNIT_EXPECT_FALSE(test, port.connected);
	KUNIT_EXPECT_PTR_EQ(test, first->cookie, &port);
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(next, NULL, &prepared_ops, 3));
	KUNIT_EXPECT_EQ(test, dptx_service_bind(next, &port, 3), -EBUSY);
	afk_service_discard(next);
	/* Old work between validate/connect/request still names the old session. */
	KUNIT_EXPECT_PTR_EQ(test, port.service, first);
	KUNIT_EXPECT_PTR_EQ(test, afk_service_get(port.service), NULL);
	KUNIT_EXPECT_FALSE(test, port.enabled);
}

static void afk_service_retired_channel_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	/* A slot that never served the channel does not know it. */
	KUNIT_EXPECT_FALSE(test, afk_service_retired(service, 1));
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 1));
	/* A live service is found by afk_service_matches(), not here. */
	KUNIT_EXPECT_FALSE(test, afk_service_retired(service, 1));
	/* Retired locally (display unplugged) before the co-processor closes it. */
	afk_test_retire(service);
	KUNIT_EXPECT_FALSE(test, afk_service_matches(service, 1));
	KUNIT_EXPECT_TRUE(test, afk_service_retired(service, 1));
	KUNIT_EXPECT_FALSE(test, afk_service_retired(service, 2));
	/* Reusing the slot forgets the old channel. */
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_FALSE(test, afk_service_retired(service, 1));
	KUNIT_EXPECT_FALSE(test, afk_service_retired(service, 2));
}

static void afk_service_declined_channel_test(struct kunit *test)
{
	struct apple_epic_service *service = test->priv;

	/* An announcement whose preparation we declined is closed later too. */
	KUNIT_ASSERT_TRUE(test, afk_service_reinit(service, NULL, &prepared_ops, 3));
	afk_service_discard(service);
	KUNIT_EXPECT_FALSE(test, afk_service_matches(service, 3));
	KUNIT_EXPECT_TRUE(test, afk_service_retired(service, 3));
}

static struct kunit_case afk_service_cases[] = {
	KUNIT_CASE(afk_service_admission_test),
	KUNIT_CASE(dptx_service_identity_test),
	KUNIT_CASE(dptx_service_duplicate_close_test),
	KUNIT_CASE(afk_service_many_generations_test),
	KUNIT_CASE(afk_service_reader_pin_test),
	KUNIT_CASE(afk_service_pending_reply_test),
	KUNIT_CASE(afk_service_completed_reader_test),
	KUNIT_CASE(afk_service_close_pin_test),
	KUNIT_CASE(afk_service_tag_sequence_test),
	KUNIT_CASE(afk_service_unconverted_owner_test),
	KUNIT_CASE(afk_service_retired_channel_test),
	KUNIT_CASE(afk_service_declined_channel_test),
	{ }
};

static struct kunit_suite afk_service_suite = {
	.name = "apple-afk-service-lifetime",
	.init = afk_service_test_init,
	.test_cases = afk_service_cases,
};

kunit_test_suite(afk_service_suite);
MODULE_DESCRIPTION("KUnit tests for the Apple AFK EPIC service lifetime");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("Dual MIT/GPL");
