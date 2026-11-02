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
 * backend that cycles the engine across it (powers it off at prepare and
 * boots it again from the preload after resume, ane_fw_cold.c); otherwise it
 * is refused up front, before any device suspends. Without retained firmware
 * nothing is refused.
 */
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
