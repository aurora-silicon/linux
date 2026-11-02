/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_SLEEP_H__
#define __ANE_FW_SLEEP_H__

#include <linux/notifier.h>
#include <linux/suspend.h>

#define ANE_FW_SLEEP_REFUSED "ANE: system sleep not supported yet, suspend refused\n"

/*
 * System sleep while the H16/H17 firmware runs. The firmware was started
 * from the bootloader's preload and cannot yet be stopped and brought back,
 * so while it is retained every sleep state is refused up front, before any
 * device suspends: suspend (including suspend-to-idle), hibernation and
 * restore. Without retained firmware nothing is refused.
 */
static inline bool ane_fw_sleep_refused(unsigned long event, bool retained)
{
	return retained && (event == PM_SUSPEND_PREPARE || event == PM_HIBERNATION_PREPARE ||
			    event == PM_RESTORE_PREPARE);
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
