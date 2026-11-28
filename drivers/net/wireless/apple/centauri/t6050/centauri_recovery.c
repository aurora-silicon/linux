// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include "centauri_recovery.h"

bool centauri_control_idle_ready(bool event_valid, u32 stage_first,
				 u32 stage_second, bool ring_drained)
{
	if (!event_valid)
		return true;
	/* Settled: the control stage reads running twice in a row and the
	 * command ring has drained, so no slot is in flight from a pre-event
	 * command. */
	return stage_first == 2 && stage_first == stage_second && ring_drained;
}
EXPORT_SYMBOL_GPL(centauri_control_idle_ready);

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
		*phase = CEN_BOOT_REWAKE;
		return 0;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(centauri_t6050_boot_phase_for_control_stage);

#if IS_ENABLED(CONFIG_APPLE_CENTAURI_T6050)

#define CEN_TRANSPORT_RECOVER_MAX 3

static void centauri_t6050_transport_recover_worker(struct work_struct *work)
{
	struct centauri_t6050 *c =
		container_of(work, struct centauri_t6050, transport_recover_work);

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
	if (!c || READ_ONCE(c->removing) || READ_ONCE(c->pm_prepared) ||
	    READ_ONCE(c->firmware_sleep))
		return;
	if (READ_ONCE(c->transport_recover_count) >= CEN_TRANSPORT_RECOVER_MAX) {
		dev_err(&c->pdev->dev,
			"Wi-Fi transport recovery limit (%u) reached\n",
			CEN_TRANSPORT_RECOVER_MAX);
		return;
	}
	queue_work(system_freezable_wq, &c->transport_recover_work);
}
EXPORT_SYMBOL_GPL(centauri_t6050_request_transport_recover);

void centauri_t6050_schedule_transport_recover(struct centauri_t6050 *c)
{
	enum centauri_t6050_boot_phase phase;
	u32 control;
	int ret;

	if (!c || READ_ONCE(c->removing) || READ_ONCE(c->pm_prepared) ||
	    READ_ONCE(c->firmware_sleep))
		return;

	mutex_lock(&c->boot_lock);
	if (READ_ONCE(c->removing) || c->pm_prepared || c->firmware_sleep ||
	    c->boot_phase == CEN_BOOT_STOPPED || c->boot_phase == CEN_BOOT_FAILED)
		goto out_unlock;

	WRITE_ONCE(c->transport_recover_count, c->transport_recover_count + 1);
	control = readl(c->bar + 0x8000);
	ret = centauri_t6050_boot_phase_for_control_stage(control, &phase);
	if (ret) {
		dev_err(&c->pdev->dev,
			"Wi-Fi transport recovery unsupported (control stage=%#x): %d\n",
			control, ret);
		c->boot_phase = CEN_BOOT_FAILED;
		goto out_unlock;
	}
	if (phase == CEN_BOOT_REWAKE) {
		mutex_unlock(&c->boot_lock);
		centauri_t6050_request_rewake(c->pdev, -EIO, true);
		return;
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

#endif /* CONFIG_APPLE_CENTAURI_T6050 */

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

static void recover_phase_rewake23(struct kunit *test)
{
	enum centauri_t6050_boot_phase phase = CEN_BOOT_READY;

	KUNIT_ASSERT_EQ(test, centauri_t6050_boot_phase_for_control_stage(2, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase, CEN_BOOT_REWAKE);
	phase = CEN_BOOT_READY;
	KUNIT_ASSERT_EQ(test, centauri_t6050_boot_phase_for_control_stage(3, &phase), 0);
	KUNIT_EXPECT_EQ(test, phase, CEN_BOOT_REWAKE);
}

static void recover_phase_null(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			centauri_t6050_boot_phase_for_control_stage(1, NULL),
			-EINVAL);
}

static void control_idle_no_event(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  centauri_control_idle_ready(false, 0, 0, false));
}

static void control_idle_settled(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  centauri_control_idle_ready(true, 2, 2, true));
}

static void control_idle_unstable_stage(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   centauri_control_idle_ready(true, 2, 3, true));
}

static void control_idle_transitional_stage(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   centauri_control_idle_ready(true, 3, 3, true));
}

static void control_idle_slot_in_flight(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   centauri_control_idle_ready(true, 2, 2, false));
}

static struct kunit_case centauri_recovery_cases[] = {
	KUNIT_CASE(recover_phase_rom),
	KUNIT_CASE(recover_phase_ipc),
	KUNIT_CASE(recover_phase_cycle),
	KUNIT_CASE(recover_phase_rewake23),
	KUNIT_CASE(recover_phase_null),
	KUNIT_CASE(control_idle_no_event),
	KUNIT_CASE(control_idle_settled),
	KUNIT_CASE(control_idle_unstable_stage),
	KUNIT_CASE(control_idle_transitional_stage),
	KUNIT_CASE(control_idle_slot_in_flight),
	{}
};

static struct kunit_suite centauri_recovery_suite = {
	.name = "centauri_t6050_recovery",
	.test_cases = centauri_recovery_cases,
};
kunit_test_suite(centauri_recovery_suite);

#endif /* CONFIG_KUNIT */
