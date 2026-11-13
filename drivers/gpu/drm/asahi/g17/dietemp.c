// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * G17 host die-temperature source. The PMP report driver owns the MMIO
 * mapping and admits only its documented running-status and fast-die word.
 */

#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-report.h>

struct asahi_g17_dietemp {
	struct device_node *report;
	unsigned int lane;
};

int asahi_g17_dietemp_get(struct device *dev, void **out)
{
	struct asahi_g17_dietemp *source;
	struct of_phandle_args args;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = of_parse_phandle_with_fixed_args(
		dev->of_node, "apple,fast-die-effort", 1, 0, &args);
	if (ret)
		return ret;

	source = kzalloc_obj(*source);
	if (!source) {
		of_node_put(args.np);
		return -ENOMEM;
	}
	source->report = args.np;
	source->lane = args.args[0];
	*out = source;
	return 0;
}

int asahi_g17_dietemp_sample(void *data, u8 *effort)
{
	struct asahi_g17_dietemp *source = data;

	if (!source || !effort)
		return -EINVAL;
	return apple_pmp_report_fast_die_effort(source->report, source->lane,
						 effort);
}

void asahi_g17_dietemp_put(void *data)
{
	struct asahi_g17_dietemp *source = data;

	if (!source)
		return;
	of_node_put(source->report);
	kfree(source);
}
