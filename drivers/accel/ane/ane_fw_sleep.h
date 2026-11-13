/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_SLEEP_H__
#define __ANE_FW_SLEEP_H__

#include <linux/notifier.h>
#include <linux/suspend.h>

#define ANE_FW_SLEEP_REFUSED "ANE: system sleep not supported yet, suspend refused\n"

/*
 * System sleep while the H16/H17 firmware runs. Hibernation and restore are
 * always refused while the firmware is retained: the image would carry a
 * firmware state the restored kernel cannot match. Suspend passes only for a
 * backend that cycles the engine across it: the H17 backend (ane_fw_cold.c)
 * and the H16 backend (ane_h16_backend.c) power it off and, after resume,
 * boot the firmware again from the preload and replay every open program
 * behind its handle. A platform without a sleep path refuses suspend up
 * front, before any device suspends. Without retained firmware nothing is
 * refused.
 */
/*
 * A hibernation pm_test dry run, the only hibernation that reaches the
 * notifiers while hibernation_available() is false, makes no image and keeps
 * the power on. A backend that cycles the engine handles it as a suspend:
 * the engine is powered off for the freeze and booted again afterwards.
 */
static inline unsigned long ane_fw_sleep_dry_run_event(unsigned long event)
{
	if (hibernation_available())
		return event;
	if (event == PM_HIBERNATION_PREPARE)
		return PM_SUSPEND_PREPARE;
	if (event == PM_POST_HIBERNATION)
		return PM_POST_SUSPEND;
	return event;
}

static inline bool ane_fw_sleep_refused_cycle(unsigned long event, bool retained, bool cycles)
{
	if (!retained)
		return false;
	if (event == PM_HIBERNATION_PREPARE || event == PM_RESTORE_PREPARE)
		return true;
	return event == PM_SUSPEND_PREPARE && !cycles;
}

static inline bool ane_fw_sleep_refused(unsigned long event, bool retained)
{
	return ane_fw_sleep_refused_cycle(event, retained, false);
}

static inline int ane_fw_sleep_event(unsigned long event, bool retained)
{
	return ane_fw_sleep_refused(event, retained) ? NOTIFY_BAD : NOTIFY_DONE;
}

/*
 * The device suspend callback's answer: a backstop for firmware started
 * after the notifier ran, or a sleep path that skipped the notifiers.
 */
static inline int ane_fw_sleep_backstop(bool retained)
{
	return retained ? -EBUSY : 0;
}

#endif /* __ANE_FW_SLEEP_H__ */
