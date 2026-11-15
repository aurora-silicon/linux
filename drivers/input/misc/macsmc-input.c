// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC input event driver
 * Copyright The Asahi Linux Contributors
 *
 * This driver exposes HID events from the SMC as an input device.
 * This includes the lid open/close and power button notifications.
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/suspend.h>

#define LID_WAKE_SETTLE_MS_J716S	2000

static int lid_wake_settle_ms = -1;
module_param(lid_wake_settle_ms, int, 0444);
MODULE_PARM_DESC(lid_wake_settle_ms,
		 "A lid-open report this many ms or less after system sleep starts does not wake the system (0 = always wake, default: 2000 on the J716s, 0 elsewhere)");

/**
 * struct macsmc_input
 * @dev: Underlying struct device for the input sub-device
 * @smc: Pointer to apple_smc struct of the mfd parent
 * @input: Allocated input_dev; devres managed
 * @nb: Notifier block used for incoming events from SMC (e.g. button pressed down)
 * @pm_nb: Notifier for the whole suspend window, including early aborts
 * @lid_lock: Serialises lid reports, the notifier and device sleep callbacks
 * @lid_settle_ms: Early lid-open confirmation window for affected boards
 * @wakeup_mode: Power/lid events are wake events during system sleep
 * @lid_held: A lid-open report was held until a full resume
 * @sleep_start: CLOCK_BOOTTIME at the start of the current sleep attempt
 * @dark: A dark wake must recheck the lid before sleeping again
 */
struct macsmc_input {
	struct device *dev;
	struct apple_smc *smc;
	struct input_dev *input;
	struct notifier_block nb;
	struct notifier_block pm_nb;
	unsigned int lid_settle_ms;
	struct mutex lid_lock; /* protects wakeup_mode, lid_held, sleep_start and dark */
	bool wakeup_mode;
	bool lid_held;
	ktime_t sleep_start;
	bool dark;
};

#define SMC_EV_BTN 0x7201
#define SMC_EV_LID 0x7203

#define BTN_POWER		0x01 /* power button on e.g. Mac Mini chasis pressed */
#define BTN_TOUCHID		0x06 /* combined TouchID / power button on MacBooks pressed */
#define BTN_POWER_HELD_SHORT	0xfe /* power button briefly held down */
#define BTN_POWER_HELD_LONG	0x00 /* power button held down; sent just before forced poweroff */

static void macsmc_input_event_button(struct macsmc_input *smcin, unsigned long event)
{
	bool wakeup_mode = READ_ONCE(smcin->wakeup_mode);
	u8 button = (event >> 8) & 0xff;
	u8 state = !!(event & 0xff);

	switch (button) {
	case BTN_POWER:
	case BTN_TOUCHID:
		pm_wakeup_dev_event(smcin->dev, 0, (wakeup_mode && state));
		/*
		 * Suppress KEY_POWER reports when suspended to avoid powering down
		 * immediately after waking from s2idle.
		 * */
		if (wakeup_mode)
			return;

		input_report_key(smcin->input, KEY_POWER, state);
		input_sync(smcin->input);
		break;
	case BTN_POWER_HELD_SHORT: /* power button held down; ignore */
		break;
	case BTN_POWER_HELD_LONG:
		/*
		 * If we get here the power button has been held down for a while and
		 * we have about 4 seconds before forced power-off is triggered by SMC.
		 * Try to do an emergency shutdown to make sure the NVMe cache is
		 * flushed. macOS actually does this by panicing (!)...
		 */
		if (state) {
			dev_crit(smcin->dev, "Triggering forced shutdown!\n");
			if (kernel_can_power_off())
				kernel_power_off();
			else /* Missing macsmc-reboot driver? */
				kernel_restart("SMC power button triggered restart");
		}
		break;
	default:
		dev_warn(smcin->dev, "Unknown SMC button event: %04lx\n", event & 0xffff);
	}
}

/*
 * On the J716s the SMC has reported the lid open (event 0x72030001, the same
 * bytes as a real opening) while the lid stayed shut, about 230 ms after the
 * close that started the suspend and in step with the trackpad coprocessor
 * moving its AFE to "Dependent Mode". Taken at face value, that report ends
 * the suspend the close started. So in the sleep window an open report wakes
 * the system only if the SMC's lid key agrees and the sleep has run for
 * longer than lid_settle_ms, which is nonzero only where this was seen.
 * Called with lid_lock held.
 */
static bool macsmc_input_lid_wake_ok(struct macsmc_input *smcin, unsigned long event)
{
	s64 ms = ktime_ms_delta(ktime_get_boottime(), smcin->sleep_start);
	u8 closed;
	int ret;

	ret = apple_smc_read_u8(smcin->smc, SMC_KEY(MSLD), &closed);
	if (ret < 0) {
		dev_warn(smcin->dev, "lid open %08lx at +%lld ms in sleep, MSLD read failed: %d\n",
			 event, ms, ret);
		closed = 0;
	}

	if (closed) {
		dev_info(smcin->dev, "lid open %08lx at +%lld ms in sleep, MSLD=%u: ignored\n",
			 event, ms, closed);
		return false;
	}

	if (smcin->lid_settle_ms && ms <= smcin->lid_settle_ms) {
		dev_info(smcin->dev, "lid open %08lx at +%lld ms in sleep, MSLD=0, within %u ms: no wakeup\n",
			 event, ms, smcin->lid_settle_ms);
		return false;
	}

	dev_info(smcin->dev, "lid open %08lx at +%lld ms in sleep, MSLD=0: wakeup\n", event, ms);
	return true;
}

static void macsmc_input_event_lid(struct macsmc_input *smcin, unsigned long event)
{
	u8 lid_state = !!((event >> 8) & 0xff);

	guard(mutex)(&smcin->lid_lock);

	if (smcin->wakeup_mode && !lid_state &&
	    !macsmc_input_lid_wake_ok(smcin, event)) {
		/* SW_LID stays closed; complete() resynchronises it from MSLD. */
		smcin->lid_held = true;
		return;
	}

	pm_wakeup_dev_event(smcin->dev, 0, (smcin->wakeup_mode && !lid_state));
	/* Through a sleep, .complete reports where the lid ended up. */
	if (smcin->wakeup_mode)
		return;

	input_report_switch(smcin->input, SW_LID, lid_state);
	input_sync(smcin->input);
}

static int macsmc_input_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct macsmc_input *smcin = container_of(nb, struct macsmc_input, nb);
	u16 type = event >> 16;

	switch (type) {
	case SMC_EV_BTN:
		macsmc_input_event_button(smcin, event);
		return NOTIFY_OK;
	case SMC_EV_LID:
		macsmc_input_event_lid(smcin, event);
		return NOTIFY_OK;
	default:
		/* SMC event meant for another driver */
		return NOTIFY_DONE;
	}
}

/*
 * A suspend's first steps run before .prepare, and the SMC reports the lid and the button as
 * plain events until its AP idle notice. Treat them as wakes from the suspend's start, so a lid
 * opened or a button pressed while the machine goes down ends the suspend, as on macOS.
 */
static int macsmc_input_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct macsmc_input *smcin = container_of(nb, struct macsmc_input, pm_nb);

	guard(mutex)(&smcin->lid_lock);
	if (action == PM_SUSPEND_PREPARE) {
		smcin->sleep_start = ktime_get_boottime();
		smcin->lid_held = false;
		WRITE_ONCE(smcin->wakeup_mode, true);
	} else if (action == PM_POST_SUSPEND) {
		u8 val;

		WRITE_ONCE(smcin->wakeup_mode, false);
		smcin->dark = false;
		smcin->lid_held = false;
		/* A suspend that ended before .prepare held its lid events back too. */
		if (apple_smc_key_exists(smcin->smc, SMC_KEY(MSLD)) &&
		    apple_smc_read_u8(smcin->smc, SMC_KEY(MSLD), &val) >= 0) {
			input_report_switch(smcin->input, SW_LID, val);
			input_sync(smcin->input);
		}
	}

	return NOTIFY_DONE;
}

static void macsmc_input_unregister_pm_notifier(void *data)
{
	struct macsmc_input *smcin = data;

	unregister_pm_notifier(&smcin->pm_nb);
}

static void macsmc_input_unregister_notifier(void *data)
{
	struct macsmc_input *smcin = data;

	blocking_notifier_chain_unregister(&smcin->smc->event_handlers, &smcin->nb);
}

static int macsmc_input_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_input *smcin;
	bool have_lid, have_power;
	int error;

	/* Bail early if this SMC neither supports power button nor lid events */
	have_lid = apple_smc_key_exists(smc, SMC_KEY(MSLD));
	have_power = apple_smc_key_exists(smc, SMC_KEY(bHLD));
	if (!have_lid && !have_power)
		return -ENODEV;

	smcin = devm_kzalloc(&pdev->dev, sizeof(*smcin), GFP_KERNEL);
	if (!smcin)
		return -ENOMEM;

	smcin->dev = &pdev->dev;
	smcin->smc = smc;
	platform_set_drvdata(pdev, smcin);

	error = devm_mutex_init(&pdev->dev, &smcin->lid_lock);
	if (error)
		return error;

	/* No OF node of its own: the false report has been seen on the J716s only. */
	if (lid_wake_settle_ms >= 0)
		smcin->lid_settle_ms = lid_wake_settle_ms;
	else if (of_machine_is_compatible("apple,j716s"))
		smcin->lid_settle_ms = LID_WAKE_SETTLE_MS_J716S;

	smcin->input = devm_input_allocate_device(&pdev->dev);
	if (!smcin->input)
		return -ENOMEM;

	smcin->input->phys = "macsmc-input (0)";
	smcin->input->name = "Apple SMC power/lid events";

	if (have_lid)
		input_set_capability(smcin->input, EV_SW, SW_LID);
	if (have_power)
		input_set_capability(smcin->input, EV_KEY, KEY_POWER);

	if (have_lid) {
		u8 val;

		error = apple_smc_read_u8(smc, SMC_KEY(MSLD), &val);
		if (error < 0)
			dev_warn(&pdev->dev, "Failed to read initial lid state\n");
		else
			input_report_switch(smcin->input, SW_LID, val);
	}

	if (have_power) {
		u32 val;

		error = apple_smc_read_u32(smc, SMC_KEY(bHLD), &val);
		if (error < 0)
			dev_warn(&pdev->dev, "Failed to read initial power button state\n");
		else
			input_report_key(smcin->input, KEY_POWER, val & 1);
	}

	error = input_register_device(smcin->input);
	if (error) {
		dev_err(&pdev->dev, "Failed to register input device: %d\n", error);
		return error;
	}

	input_sync(smcin->input);

	/*
	 * Register the wakeup cleanup before the notifier, so that removal
	 * drains the notifier callbacks first, while both the input device
	 * and the wakeup source are still alive.
	 */
	error = devm_device_init_wakeup(&pdev->dev);
	if (error)
		return error;

	smcin->nb.notifier_call = macsmc_input_event;
	error = blocking_notifier_chain_register(&smc->event_handlers, &smcin->nb);
	if (error)
		return error;

	/*
	 * The notifier block lives in devm-allocated memory and the callback
	 * reports to a devm-managed input device: take it off the SMC's chain
	 * before either goes away on unbind.
	 */
	error = devm_add_action_or_reset(&pdev->dev,
					 macsmc_input_unregister_notifier, smcin);
	if (error)
		return error;

	smcin->pm_nb.notifier_call = macsmc_input_pm_notify;
	error = register_pm_notifier(&smcin->pm_nb);
	if (error)
		return error;

	return devm_add_action_or_reset(&pdev->dev, macsmc_input_unregister_pm_notifier, smcin);
}

static int macsmc_input_pm_prepare(struct device *dev)
{
	struct macsmc_input *smcin = dev_get_drvdata(dev);
	u8 val;

	guard(mutex)(&smcin->lid_lock);
	smcin->sleep_start = ktime_get_boottime();
	smcin->lid_held = false;
	WRITE_ONCE(smcin->wakeup_mode, true);
	/* A lid opened as the dark wake ended may have sent its event already. */
	if (smcin->dark) {
		smcin->dark = false;
		if (apple_smc_key_exists(smcin->smc, SMC_KEY(MSLD)) &&
		    apple_smc_read_u8(smcin->smc, SMC_KEY(MSLD), &val) >= 0 && !val) {
			dev_info(dev, "the lid opened during a dark wake: not sleeping again\n");
			return -EBUSY;
		}
	}
	return 0;
}

static void macsmc_input_pm_complete(struct device *dev)
{
	struct macsmc_input *smcin = dev_get_drvdata(dev);
	u8 lid;
	bool held;
	int error;

	guard(mutex)(&smcin->lid_lock);
	/* Keep the wake window and held lid event through an automatic dark re-sleep. */
	if (pm_dark_wake()) {
		smcin->dark = apple_smc_read_u8(smcin->smc, SMC_KEY(MSLD), &lid) < 0 || lid;
		return;
	}
	WRITE_ONCE(smcin->wakeup_mode, false);
	held = smcin->lid_held;
	smcin->lid_held = false;

	/* Full resume reports the actual state, including an open held for confirmation. */
	if (apple_smc_key_exists(smcin->smc, SMC_KEY(MSLD))) {
		error = apple_smc_read_u8(smcin->smc, SMC_KEY(MSLD), &lid);
		if (error < 0) {
			dev_warn(dev, "Failed to read the lid state after sleep: %d\n", error);
			return;
		}
		if (held)
			dev_info(dev, "lid after sleep: MSLD=%u\n", lid);
		input_report_switch(smcin->input, SW_LID, !!lid);
		input_sync(smcin->input);
	}
}

static const struct dev_pm_ops macsmc_input_pm_ops = {
	.prepare = macsmc_input_pm_prepare,
	.complete = macsmc_input_pm_complete,
};

static struct platform_driver macsmc_input_driver = {
	.driver = {
		.name = "macsmc-input",
		.pm = &macsmc_input_pm_ops,
	},
	.probe = macsmc_input_probe,
};
module_platform_driver(macsmc_input_driver);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC input driver");
MODULE_ALIAS("platform:macsmc-input");
