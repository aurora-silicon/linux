// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Initrd mode parameter and the root-only completion trigger. */
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/sysfs.h>

#include "shim.h"

/* Rust's module! parameters currently have no sysfs getter. */
static unsigned char keystore_only;
module_param(keystore_only, byte, 0444);
MODULE_PARM_DESC(keystore_only,
	"Initrd attach: 1 = trusted keys only, 2 = also per-user key bags for a password unlock (under the xART guard too); defer persistent host state and Touch ID until complete_attach");

bool sep_keystore_only(void)
{
	return keystore_only != 0;
}

bool sep_keystore_user_bags(void)
{
	return keystore_only == 2;
}

/* One SEP per machine. device_remove_file drains callbacks before release. */
static void *sep_attach_ctx;
static int (*sep_attach_complete)(void *ctx);

static ssize_t complete_attach_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = sep_attach_complete(sep_attach_ctx);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(complete_attach);

int sep_complete_attach_register(struct device *dev, void *ctx,
				int (*complete)(void *ctx))
{
	sep_attach_ctx = ctx;
	sep_attach_complete = complete;
	return device_create_file(dev, &dev_attr_complete_attach);
}

void sep_complete_attach_unregister(struct device *dev)
{
	device_remove_file(dev, &dev_attr_complete_attach);
	sep_attach_ctx = NULL;
	sep_attach_complete = NULL;
}
