// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"

static const struct neo_apple_epic_service_ops reusable_ops = {
	.name = "reusable",
	.reusable = true,
};

static const struct neo_apple_epic_service_ops permanent_ops = {
	.name = "permanent",
};

static int neo_afk_service_test_init(struct kunit *test)
{
	struct neo_apple_epic_service *service;

	service = kunit_kzalloc(test, sizeof(*service), GFP_KERNEL);
	if (!service)
		return -ENOMEM;
	spin_lock_init(&service->lock);
	test->priv = service;
	return 0;
}

static void neo_afk_test_retire(struct neo_apple_epic_service *service)
{
	unsigned long flags;

	spin_lock_irqsave(&service->lock, flags);
	service->enabled = false;
	service->torndown = true;
	spin_unlock_irqrestore(&service->lock, flags);
}

static void neo_afk_service_many_generations_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;
	unsigned int i;

	/* Repeated unplug/replug must not consume an unbounded slot budget. */
	for (i = 0; i < 4 * AFK_MAX_CHANNEL; i++) {
		KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, i));
		KUNIT_EXPECT_EQ(test, service->channel, i);
		KUNIT_EXPECT_TRUE(test, neo_afk_service_matches(service, i));
		neo_afk_test_retire(service);
	}
}

static void neo_afk_service_reader_pin_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 1));
	KUNIT_ASSERT_PTR_EQ(test, neo_afk_service_get(service), service); /* owner */
	KUNIT_ASSERT_PTR_EQ(test, neo_afk_service_get(service), service); /* reader */
	neo_afk_test_retire(service);
	neo_afk_service_put(service); /* owner detaches while EDID is being read */
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_EQ(test, service->channel, 1U);
	KUNIT_EXPECT_PTR_EQ(test, neo_afk_service_get(service), NULL);
	neo_afk_service_put(service);
	KUNIT_EXPECT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void neo_afk_service_pending_reply_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 1));
	set_bit(3, service->cmd_map);
	neo_afk_test_retire(service);
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_TRUE(test, neo_afk_service_matches(service, 1));
	KUNIT_EXPECT_FALSE(test, neo_afk_service_matches(service, 2));
	/* A timed-out command retains its DMA and slot until the late reply. */
	service->cmds[3].free_on_ack = true;
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	clear_bit(3, service->cmd_map);
	KUNIT_EXPECT_FALSE(test, neo_afk_service_matches(service, 1));
	KUNIT_EXPECT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void neo_afk_service_completed_reader_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 1));
	KUNIT_ASSERT_PTR_EQ(test, neo_afk_service_get(service), service);
	set_bit(0, service->cmd_map);
	neo_afk_test_retire(service);
	/* Reply arrived, but the waiting thread has not consumed it yet. */
	service->cmds[0].done = true;
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	clear_bit(0, service->cmd_map);
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	neo_afk_service_put(service);
	KUNIT_EXPECT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static void neo_afk_service_tag_sequence_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 1));
	service->cmd_tag = 0xa9;
	service->cookie = test;
	neo_afk_test_retire(service);
	KUNIT_EXPECT_TRUE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
	KUNIT_EXPECT_EQ(test, service->cmd_tag, (u8)0xa9);
	KUNIT_EXPECT_PTR_EQ(test, service->cookie, NULL);
}

static void neo_afk_service_unconverted_owner_test(struct kunit *test)
{
	struct neo_apple_epic_service *service = test->priv;

	KUNIT_ASSERT_TRUE(test, neo_afk_service_reinit(service, NULL, &permanent_ops, 1));
	neo_afk_test_retire(service);
	KUNIT_EXPECT_FALSE(test, neo_afk_service_reinit(service, NULL, &reusable_ops, 2));
}

static struct kunit_case neo_afk_service_cases[] = {
	KUNIT_CASE(neo_afk_service_many_generations_test),
	KUNIT_CASE(neo_afk_service_reader_pin_test),
	KUNIT_CASE(neo_afk_service_pending_reply_test),
	KUNIT_CASE(neo_afk_service_completed_reader_test),
	KUNIT_CASE(neo_afk_service_tag_sequence_test),
	KUNIT_CASE(neo_afk_service_unconverted_owner_test),
	{ }
};

static struct kunit_suite neo_afk_service_suite = {
	.name = "apple-afk-service-lifetime",
	.init = neo_afk_service_test_init,
	.test_cases = neo_afk_service_cases,
};

kunit_test_suite(neo_afk_service_suite);
MODULE_LICENSE("Dual MIT/GPL");
