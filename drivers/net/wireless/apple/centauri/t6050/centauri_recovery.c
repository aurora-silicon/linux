// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include "centauri_recovery.h"

s64 centauri_fw_event_scan_settle_remainder_ms(bool event_valid, ktime_t event_at,
					       ktime_t now, unsigned settle_ms)
{
	s64 elapsed;

	if (!event_valid || !settle_ms)
		return 0;
	elapsed = ktime_to_ms(ktime_sub(now, event_at));
	if (elapsed < 0)
		elapsed = 0;
	if (elapsed >= settle_ms)
		return 0;
	return settle_ms - elapsed;
}
EXPORT_SYMBOL_GPL(centauri_fw_event_scan_settle_remainder_ms);

int centauri_t6050_boot_phase_for_control_stage(u32 control_stage,
						enum centauri_t6050_boot_phase *phase)
{
	if (!phase)
		return -EINVAL;

	switch (control_stage) {
	case 0:
		*phase = CEN_BOOT_ROM;
		return 0;
	case 1:
		*phase = CEN_BOOT_IPC;
		return 0;
	case 4:
		*phase = CEN_BOOT_CYCLE;
		return 0;
	case 2:
	case 3:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(centauri_t6050_boot_phase_for_control_stage);

#define CEN_TRANSPORT_RECOVER_MAX 3

static void centauri_t6050_transport_recover_worker(struct work_struct *work)
{
	struct centauri_t6050 *c =
		container_of(work, struct centauri_t6050, transport_recover_work);

	c->transport_recover_count++;
	centauri_t6050_schedule_transport_recover(c);
}

void centauri_t6050_transport_recover_init(struct centauri_t6050 *c)
{
	INIT_WORK(&c->transport_recover_work, centauri_t6050_transport_recover_worker);
	c->transport_recover_count = 0;
}

void centauri_t6050_transport_recover_cancel(struct centauri_t6050 *c)
{
	cancel_work_sync(&c->transport_recover_work);
}

void centauri_t6050_request_transport_recover(struct centauri_t6050 *c)
{
	if (!c || READ_ONCE(c->removing))
		return;
	if (c->transport_recover_count >= CEN_TRANSPORT_RECOVER_MAX) {
		dev_err(&c->pdev->dev,
			"Wi-Fi transport recovery limit (%u) reached\n",
			CEN_TRANSPORT_RECOVER_MAX);
		return;
	}
	schedule_work(&c->transport_recover_work);
}
EXPORT_SYMBOL_GPL(centauri_t6050_request_transport_recover);

void centauri_t6050_schedule_transport_recover(struct centauri_t6050 *c)
{
	enum centauri_t6050_boot_phase phase;
	u32 control;
	int ret;

	if (!c || READ_ONCE(c->removing))
		return;

	mutex_lock(&c->boot_lock);
	if (c->boot_phase == CEN_BOOT_STOPPED || c->boot_phase == CEN_BOOT_FAILED)
		goto out_unlock;

	control = readl(c->bar + 0x8000);
	ret = centauri_t6050_boot_phase_for_control_stage(control, &phase);
	if (ret) {
		dev_err(&c->pdev->dev,
			"Wi-Fi transport recovery unsupported (control stage=%#x): %d\n",
			control, ret);
		c->boot_phase = CEN_BOOT_FAILED;
		goto out_unlock;
	}

	WRITE_ONCE(c->ready, false);
	centauri_t6050_free_ipc(c);
	if (phase == CEN_BOOT_ROM)
		c->boot_attempted = false;
	c->boot_phase = phase;
	mutex_unlock(&c->boot_lock);
	queue_delayed_work(system_long_wq, &c->boot_work, 0);
	return;

out_unlock:
	mutex_unlock(&c->boot_lock);
}
EXPORT_SYMBOL_GPL(centauri_t6050_schedule_transport_recover);

#if IS_ENABLED(CONFIG_APPLE_CENTAURI_T6050_RECOVERY_KUNIT)

#include <kunit/test.h>

static void recover_phase_rom(struct kunit *test)
{
	enum centauri_t6050_boot_phase phase = CEN_BOOT_FAILED;

	KUNIT_EXPECT_EQ(test, centauri_t6050_boot_phase_for_control_stage(0, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase, CEN_BOOT_ROM);
}

static void recover_phase_ipc(struct kunit *test)
{
	enum centauri_t6050_boot_phase phase = CEN_BOOT_FAILED;

	KUNIT_EXPECT_EQ(test, centauri_t6050_boot_phase_for_control_stage(1, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase, CEN_BOOT_IPC);
}

static void recover_phase_cycle(struct kunit *test)
{
	enum centauri_t6050_boot_phase phase = CEN_BOOT_FAILED;

	KUNIT_EXPECT_EQ(test, centauri_t6050_boot_phase_for_control_stage(4, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase, CEN_BOOT_CYCLE);
}

static void recover_phase_stop23(struct kunit *test)
{
	enum centauri_t6050_boot_phase phase = CEN_BOOT_READY;

	KUNIT_EXPECT_EQ(test,
			centauri_t6050_boot_phase_for_control_stage(2, &phase),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			centauri_t6050_boot_phase_for_control_stage(3, &phase),
			-EOPNOTSUPP);
}

static void recover_phase_null(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			centauri_t6050_boot_phase_for_control_stage(1, NULL),
			-EINVAL);
}

static void scan_settle_remainder_inside(struct kunit *test)
{
	ktime_t event = ktime_set(1000, 0);
	ktime_t now = ktime_add_ms(event, 2);

	KUNIT_EXPECT_EQ(test,
			centauri_fw_event_scan_settle_remainder_ms(true, event,
								   now, 15),
			13);
}

static void scan_settle_remainder_waits_out(struct kunit *test)
{
	ktime_t event = ktime_set(0, 0);
	ktime_t now = ktime_add_ms(event, 10);

	KUNIT_EXPECT_EQ(test,
			centauri_fw_event_scan_settle_remainder_ms(true, event,
								   now, 15),
			5);
}

static void scan_settle_remainder_outside(struct kunit *test)
{
	ktime_t event = ktime_set(0, 0);
	ktime_t now = ktime_add_ms(event, 15);

	KUNIT_EXPECT_EQ(test,
			centauri_fw_event_scan_settle_remainder_ms(true, event,
								   now, 15),
			0);
	now = ktime_add_ms(event, 20);
	KUNIT_EXPECT_EQ(test,
			centauri_fw_event_scan_settle_remainder_ms(true, event,
								   now, 15),
			0);
}

static void scan_settle_remainder_no_event(struct kunit *test)
{
	ktime_t event = ktime_set(0, 0);
	ktime_t now = ktime_add_ms(event, 2);

	KUNIT_EXPECT_EQ(test,
			centauri_fw_event_scan_settle_remainder_ms(false, event,
								   now, 15),
			0);
}

static struct kunit_case centauri_recovery_cases[] = {
	KUNIT_CASE(recover_phase_rom),
	KUNIT_CASE(recover_phase_ipc),
	KUNIT_CASE(recover_phase_cycle),
	KUNIT_CASE(recover_phase_stop23),
	KUNIT_CASE(recover_phase_null),
	KUNIT_CASE(scan_settle_remainder_inside),
	KUNIT_CASE(scan_settle_remainder_waits_out),
	KUNIT_CASE(scan_settle_remainder_outside),
	KUNIT_CASE(scan_settle_remainder_no_event),
	{}
};

static struct kunit_suite centauri_recovery_suite = {
	.name = "centauri_t6050_recovery",
	.test_cases = centauri_recovery_cases,
};
kunit_test_suite(centauri_recovery_suite);

#endif /* CONFIG_KUNIT */
