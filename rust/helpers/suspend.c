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
