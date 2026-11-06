// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple SoC platform error reporting
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "edac_module.h"

#define APPLE_SOC_EDAC_NAME	"apple_soc_edac"

struct apple_soc_edac_range {
	u8 first;
	u8 count;
	const char *name;
};

struct apple_soc_edac_hw {
	const struct apple_soc_edac_range *ranges;
	size_t nranges;
	unsigned int num_irqs;
	bool event_log;
};

struct apple_soc_edac;

struct apple_soc_edac_irq {
	struct apple_soc_edac *edac;
	const char *source;
	unsigned int index;
	int irq;
};

struct apple_soc_edac {
	struct device *dev;
	struct edac_device_ctl_info *dci;
	const struct apple_soc_edac_hw *hw;
	struct apple_soc_edac_irq *irqs;
};

/* DCS IRQs drive a separate thermal loop and are intentionally omitted. */
static const struct apple_soc_edac_range t6041_error_ranges[] = {
	{ 0, 9, "sep" },
	{ 9, 1, "pmgr" },
	{ 10, 16, "amcc" },
	{ 26, 3, "sram" },
	{ 45, 3, "ni0" },
	{ 48, 4, "ni1" },
	{ 52, 4, "ni2" },
	{ 56, 8, "ni3" },
	{ 64, 2, "ni4" },
	{ 66, 2, "ni5" },
	{ 68, 2, "ni6" },
	{ 70, 7, "nsc" },
	{ 77, 12, "nsi" },
};

static const struct apple_soc_edac_range t8140_error_ranges[] = {
	{ 0, 4, "amcc" },
	{ 4, 1, "pmgr" },
	{ 5, 9, "sep" },
	{ 18, 1, "ni0" },
	{ 19, 1, "ni1" },
	{ 20, 1, "ni2" },
	{ 21, 1, "ni3" },
	{ 22, 1, "ni4" },
	{ 23, 1, "ni5" },
	{ 24, 1, "nsi" },
	{ 25, 1, "pio" },
};

static const struct apple_soc_edac_hw t6041_error_hw = {
	.ranges = t6041_error_ranges,
	.nranges = ARRAY_SIZE(t6041_error_ranges),
	.num_irqs = 89,
};

static const struct apple_soc_edac_hw t8140_error_hw = {
	.ranges = t8140_error_ranges,
	.nranges = ARRAY_SIZE(t8140_error_ranges),
	.num_irqs = 26,
};

static const struct apple_soc_edac_hw t6041_event_hw = {
	.num_irqs = 21,
	.event_log = true,
};

static const struct apple_soc_edac_hw t8140_event_hw = {
	.num_irqs = 9,
	.event_log = true,
};

static const char *apple_soc_edac_source(const struct apple_soc_edac_hw *hw,
					 unsigned int index)
{
	size_t i;

	if (hw->event_log)
		return "event-log";

	for (i = 0; i < hw->nranges; i++) {
		const struct apple_soc_edac_range *range = &hw->ranges[i];

		if (index >= range->first && index < range->first + range->count)
			return range->name;
	}

	return NULL;
}

static irqreturn_t apple_soc_edac_irq_thread(int irq, void *data)
{
	struct apple_soc_edac_irq *irq_data = data;
	struct apple_soc_edac *edac = irq_data->edac;
	char msg[64];

	/* Fatal sources do not return, but stop a second delivery while logging. */
	disable_irq_nosync(irq);
	snprintf(msg, sizeof(msg), "%s source %u (IRQ %d)", irq_data->source,
		 irq_data->index, irq);
	edac_device_handle_ue(edac->dci, 0, 0, msg);
	panic("Apple SoC fatal hardware error: %s", msg);

	return IRQ_HANDLED;
}

static int apple_soc_edac_probe(struct platform_device *pdev)
{
	const struct apple_soc_edac_hw *hw;
	struct apple_soc_edac *edac;
	unsigned int requested = 0;
	unsigned int i;
	int ret;

	hw = device_get_match_data(&pdev->dev);
	if (!hw)
		return -EINVAL;

	if (platform_irq_count(pdev) != hw->num_irqs)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "expected %u interrupts\n", hw->num_irqs);

	edac = devm_kzalloc(&pdev->dev, sizeof(*edac), GFP_KERNEL);
	if (!edac)
		return -ENOMEM;

	edac->irqs = devm_kcalloc(&pdev->dev, hw->num_irqs,
				   sizeof(*edac->irqs), GFP_KERNEL);
	if (!edac->irqs)
		return -ENOMEM;

	edac->dev = &pdev->dev;
	edac->hw = hw;
	edac->dci = edac_device_alloc_ctl_info(0, "apple_soc", 1,
					       "platform", 1, 0,
					       edac_device_alloc_index());
	if (!edac->dci)
		return -ENOMEM;

	edac->dci->dev = &pdev->dev;
	edac->dci->mod_name = KBUILD_MODNAME;
	edac->dci->ctl_name = APPLE_SOC_EDAC_NAME;
	edac->dci->dev_name = dev_name(&pdev->dev);
	platform_set_drvdata(pdev, edac);

	ret = edac_device_add_device(edac->dci);
	if (ret)
		goto err_free_dci;

	for (i = 0; i < hw->num_irqs; i++) {
		struct apple_soc_edac_irq *irq_data = &edac->irqs[i];
		const char *source = apple_soc_edac_source(hw, i);
		int irq;

		if (!source)
			continue;

		irq = platform_get_irq(pdev, i);
		if (irq < 0) {
			ret = irq;
			goto err_del_device;
		}

		irq_data->edac = edac;
		irq_data->source = source;
		irq_data->index = i;
		irq_data->irq = irq;

		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						 apple_soc_edac_irq_thread,
						 IRQF_ONESHOT | IRQF_NO_AUTOEN,
						 dev_name(&pdev->dev), irq_data);
		if (ret)
			goto err_del_device;

		requested++;
	}

	for (i = 0; i < hw->num_irqs; i++) {
		if (edac->irqs[i].source)
			enable_irq(edac->irqs[i].irq);
	}

	dev_info(&pdev->dev, "monitoring %u fatal error sources\n", requested);

	return 0;

err_del_device:
	edac_device_del_device(&pdev->dev);
err_free_dci:
	edac_device_free_ctl_info(edac->dci);

	return ret;
}

static void apple_soc_edac_remove(struct platform_device *pdev)
{
	struct apple_soc_edac *edac = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < edac->hw->num_irqs; i++) {
		if (edac->irqs[i].source)
			disable_irq(edac->irqs[i].irq);
	}

	edac_device_del_device(&pdev->dev);
	edac_device_free_ctl_info(edac->dci);
}

static const struct of_device_id apple_soc_edac_of_match[] = {
	{ .compatible = "apple,t6041-error-handler", .data = &t6041_error_hw },
	{ .compatible = "apple,t8140-error-handler", .data = &t8140_error_hw },
	{ .compatible = "apple,t6041-event-log", .data = &t6041_event_hw },
	{ .compatible = "apple,t8140-event-log", .data = &t8140_event_hw },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_soc_edac_of_match);

static struct platform_driver apple_soc_edac_driver = {
	.probe = apple_soc_edac_probe,
	.remove = apple_soc_edac_remove,
	.driver = {
		.name = APPLE_SOC_EDAC_NAME,
		.of_match_table = apple_soc_edac_of_match,
	},
};
module_platform_driver(apple_soc_edac_driver);

MODULE_AUTHOR("The Asahi Linux Contributors");
MODULE_DESCRIPTION("Apple SoC platform error reporting driver");
MODULE_LICENSE("GPL");
