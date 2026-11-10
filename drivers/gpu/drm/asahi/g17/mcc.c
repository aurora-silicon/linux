// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/device.h>
#include <linux/property.h>
#include <linux/soc/apple/mcc.h>
#include "mcc.h"

/* Positive means that this GPU has no memory-cache lifecycle provider. */
int asahi_mcc_release_parameter_buffer(void *consumer)
{
	struct device *dev = consumer;
	int ret;

	if (!device_property_present(dev, "apple,memory-cache-controller"))
		return 1;
	ret = apple_mcc_release_stream(
		dev, APPLE_MCC_STREAM_GPU_PARAMETER_BUFFER_WRITE);
	/* Stream metadata is optional compatibility data, as in apple-mcc. */
	if (ret == -ENOENT || ret == -EINVAL || ret == -EOPNOTSUPP ||
	    ret == -ENODEV)
		return 1;
	return ret;
}
