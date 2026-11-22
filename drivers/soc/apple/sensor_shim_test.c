// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* KUnit tests for the fingerprint sensor's system sleep. No hardware is touched. */

#include <kunit/test.h>
#include <kunit/test-bug.h>

struct sep_fake_hw {
	/* 's' for a send, '0' and '1' for power writes, in order. */
	char calls[8];
	unsigned int ncalls;
	u8 sent[16];
	size_t sent_len;
	int send_rc;
	int power_rc;
};

static struct sep_fake_hw *sep_fake(void)
{
	return kunit_get_current_test()->priv;
}

static void sep_fake_note(struct sep_fake_hw *f, char call)
{
	if (f->ncalls < ARRAY_SIZE(f->calls))
		f->calls[f->ncalls++] = call;
}

static int sep_fake_send(const void *tx, size_t len)
{
	struct sep_fake_hw *f = sep_fake();

	sep_fake_note(f, 's');
	f->sent_len = min(len, sizeof(f->sent));
	memcpy(f->sent, tx, f->sent_len);
	return f->send_rc;
}

static int sep_fake_set_power(int on)
{
	struct sep_fake_hw *f = sep_fake();

	sep_fake_note(f, on ? '1' : '0');
	return f->power_rc;
}

static const struct sep_sensor_hw sep_fake_hw = {
	.set_power = sep_fake_set_power,
	.send = sep_fake_send,
};

static int sep_sensor_test_init(struct kunit *test)
{
	test->priv = kunit_kzalloc(test, sizeof(struct sep_fake_hw), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, test->priv);
	return 0;
}

static void sep_sensor_sleep_sends_the_command_then_cuts_power(struct kunit *test)
{
	struct sep_fake_hw *f = test->priv;
	struct sep_sensor_lifetime sensor = { .powered = true };

	KUNIT_EXPECT_EQ(test, sep_sensor_sleep_locked(&sensor, &sep_fake_hw), 0);
	KUNIT_ASSERT_EQ(test, f->ncalls, 2U);
	KUNIT_EXPECT_MEMEQ(test, f->calls, "s0", 2);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(sep_sensor_cmd_sleep));
	KUNIT_EXPECT_MEMEQ(test, f->sent, sep_sensor_cmd_sleep, sizeof(sep_sensor_cmd_sleep));
	KUNIT_EXPECT_FALSE(test, sensor.powered);
	KUNIT_EXPECT_TRUE(test, sensor.asleep);
}

static void sep_sensor_sleep_leaves_an_unpowered_sensor_alone(struct kunit *test)
{
	struct sep_fake_hw *f = test->priv;
	struct sep_sensor_lifetime sensor = { .powered = false };

	KUNIT_EXPECT_EQ(test, sep_sensor_sleep_locked(&sensor, &sep_fake_hw), 0);
	KUNIT_EXPECT_EQ(test, f->ncalls, 0U);
	KUNIT_EXPECT_FALSE(test, sensor.powered);
	KUNIT_EXPECT_TRUE(test, sensor.asleep);
}

static void sep_sensor_sleep_cuts_power_when_the_command_fails(struct kunit *test)
{
	struct sep_fake_hw *f = test->priv;
	struct sep_sensor_lifetime sensor = { .powered = true };

	f->send_rc = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, sep_sensor_sleep_locked(&sensor, &sep_fake_hw), 0);
	KUNIT_ASSERT_EQ(test, f->ncalls, 2U);
	KUNIT_EXPECT_MEMEQ(test, f->calls, "s0", 2);
	KUNIT_EXPECT_FALSE(test, sensor.powered);
}

static void sep_sensor_sleep_counts_a_failed_power_off_as_on(struct kunit *test)
{
	struct sep_fake_hw *f = test->priv;
	struct sep_sensor_lifetime sensor = { .powered = true };

	f->power_rc = -EIO;
	KUNIT_EXPECT_EQ(test, sep_sensor_sleep_locked(&sensor, &sep_fake_hw), -EIO);
	KUNIT_EXPECT_TRUE(test, sensor.powered);
	KUNIT_EXPECT_TRUE(test, sensor.asleep);
}

static void sep_sensor_requests_wait_for_the_resume(struct kunit *test)
{
	struct sep_sensor_lifetime sensor = { .powered = true };
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_EXPECT_EQ(test, sep_sensor_awake_locked(&sensor), 0);
	sep_sensor_sleep_locked(&sensor, &sep_fake_hw);
	KUNIT_EXPECT_EQ(test, sep_sensor_awake_locked(&sensor), -ESHUTDOWN);

	dev_set_drvdata(dev, &sensor);
	KUNIT_EXPECT_EQ(test, sep_sensor_resume(dev), 0);
	KUNIT_EXPECT_EQ(test, sep_sensor_awake_locked(&sensor), 0);
	/* The resume powers nothing: the next bring-up does. */
	KUNIT_EXPECT_FALSE(test, sensor.powered);
	KUNIT_EXPECT_EQ(test, ((struct sep_fake_hw *)test->priv)->ncalls, 2U);
}

static void sep_sensor_power_writes_are_tracked(struct kunit *test)
{
	struct sep_fake_hw *f = test->priv;
	struct sep_sensor_lifetime sensor = {};

	KUNIT_EXPECT_EQ(test, sep_sensor_write_power(&sensor, &sep_fake_hw, 1), 0);
	KUNIT_EXPECT_TRUE(test, sensor.powered);
	KUNIT_EXPECT_EQ(test, sep_sensor_write_power(&sensor, &sep_fake_hw, 0), 0);
	KUNIT_EXPECT_FALSE(test, sensor.powered);

	f->power_rc = -EIO;
	KUNIT_EXPECT_EQ(test, sep_sensor_write_power(&sensor, &sep_fake_hw, 1), -EIO);
	KUNIT_EXPECT_TRUE(test, sensor.powered);
	sensor.powered = false;
	KUNIT_EXPECT_EQ(test, sep_sensor_write_power(&sensor, &sep_fake_hw, 0), -EIO);
	KUNIT_EXPECT_TRUE(test, sensor.powered);
}

/* Framed like the driver's other commands: 0x80, opcode, then the length at 3. */
static void sep_sensor_sleep_command_is_framed(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sizeof(sep_sensor_cmd_sleep), (size_t)7);
	KUNIT_EXPECT_EQ(test, sep_sensor_cmd_sleep[0], (u8)0x80);
	KUNIT_EXPECT_EQ(test, sep_sensor_cmd_sleep[1], (u8)0x31);
	KUNIT_EXPECT_EQ(test, (size_t)get_unaligned_le16(&sep_sensor_cmd_sleep[3]),
			sizeof(sep_sensor_cmd_sleep));
}

static struct kunit_case sep_sensor_sleep_cases[] = {
	KUNIT_CASE(sep_sensor_sleep_sends_the_command_then_cuts_power),
	KUNIT_CASE(sep_sensor_sleep_leaves_an_unpowered_sensor_alone),
	KUNIT_CASE(sep_sensor_sleep_cuts_power_when_the_command_fails),
	KUNIT_CASE(sep_sensor_sleep_counts_a_failed_power_off_as_on),
	KUNIT_CASE(sep_sensor_requests_wait_for_the_resume),
	KUNIT_CASE(sep_sensor_power_writes_are_tracked),
	KUNIT_CASE(sep_sensor_sleep_command_is_framed),
	{}
};

static struct kunit_suite sep_sensor_sleep_suite = {
	.name = "apple_sep_sensor_sleep",
	.init = sep_sensor_test_init,
	.test_cases = sep_sensor_sleep_cases,
};

kunit_test_suite(sep_sensor_sleep_suite);
