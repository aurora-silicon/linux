// SPDX-License-Identifier: GPL-2.0

#include <linux/suspend.h>

__rust_helper bool rust_helper_pm_suspend_via_firmware(void)
{
	return pm_suspend_via_firmware();
}

__rust_helper bool rust_helper_pm_resume_via_firmware(void)
{
	return pm_resume_via_firmware();
}

/* An inline without CONFIG_HIBERNATION, so Rust reaches it here. */
__rust_helper bool rust_helper_pm_hibernate_is_restored(void)
{
	return pm_hibernate_is_restored();
}
