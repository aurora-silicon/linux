// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/devcoredump.h>
#include <linux/vmalloc.h>

void asahi_save_rtkit_dump(struct device *dev, const void *data, size_t size);

/* RTKit owns its immutable shadow only during the crash callback. */
void asahi_save_rtkit_dump(struct device *dev, const void *data, size_t size)
{
	void *copy;

	if (!size)
		return;
	copy = vmalloc(size);
	if (copy) {
		memcpy(copy, data, size);
		dev_coredumpv(dev, copy, size, GFP_KERNEL);
	}
}
