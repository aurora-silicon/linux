// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Preserve the working display PMP protocol. The M3 GPU DT has no apple,pmp
 * property and uses its existing power domain; optional G16/PMP paths fail
 * closed instead of changing firmware behavior for this integration.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/soc/apple/pmp.h>
int apple_pmp_link_device(struct device *consumer)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(apple_pmp_link_device);
int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(apple_pmp_set_device_power);
