// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// THIS_MODULE for the once-per-boot module pin (retain.rs): a firmware
// session that is never freed keeps callbacks into this module.

#include <linux/module.h>
#include <linux/types.h>

bool asahi_module_try_get(void);
void asahi_module_put(void);

/*
 * Pin this module until reboot, unless its removal has begun: a deferred or
 * asynchronous probe can run after an ordinary rmmod has passed its reference
 * check, which __module_get() would not stop.
 */
bool asahi_module_try_get(void)
{
	return try_module_get(THIS_MODULE);
}

/* Drop a reference asahi_module_try_get() took. */
void asahi_module_put(void)
{
	module_put(THIS_MODULE);
}
