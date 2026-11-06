// SPDX-License-Identifier: GPL-2.0

#include <linux/iio/trigger.h>

__rust_helper struct iio_trigger *rust_helper_iio_trigger_get(struct iio_trigger *trig)
{
	return iio_trigger_get(trig);
}
