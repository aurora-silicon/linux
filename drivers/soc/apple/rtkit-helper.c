// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Generic RTKit helper coprocessor
 * Copyright The Asahi Linux Contributors
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/rtkit-helper.h>
#include <linux/string.h>
#include <linux/suspend.h>

#define APPLE_ASC_CPU_CONTROL		0x44
#define APPLE_ASC_CPU_CONTROL_RUN	BIT(4)

/* A running MTP session described by the boot stage that started it. */
#define APPLE_RTKIT_HELPER_HANDOFF	"mtp-live-v1"
#define APPLE_RTKIT_HELPER_MAX_BUFFERS	4
#define APPLE_RTKIT_HELPER_BUFFER_CELLS	5

struct apple_rtkit_helper {
	struct device *dev;
	struct apple_rtkit *rtk;
	/* Inherited from the boot stage; it cannot be restarted. */
	bool adopted;
	/* The producer already began the J713 DockChannel input session. */
	bool preserved;
	/*
	 * The adopted session has a buffer inventory: requests for buffers
	 * it does not list must not replace storage the firmware still owns.
	 */
	bool strict_buffers;

	void __iomem *asc_base;

	struct resource *sram;
	void __iomem *sram_base;
};

bool apple_rtkit_helper_preserves_input(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	return helper && helper->preserved;
}
EXPORT_SYMBOL_GPL(apple_rtkit_helper_preserves_input);

static int apple_rtkit_helper_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_rtkit_helper *helper = cookie;
	struct resource res = {
		.start = bfr->iova,
		.end = bfr->iova + bfr->size - 1,
		.name = "rtkit_map",
	};

	/*
	 * An adopted session must keep the buffers negotiated by its first host.
	 * Exact repeats are handled by RTKit before reaching this callback. A new
	 * allocation, or an unrecorded request from a legacy producer, does not
	 * authorize replacing storage the firmware may still use.
	 */
	if (helper->adopted && helper->strict_buffers)
		return -ENODATA;

	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(helper->dev, bfr->size,
						    &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		return 0;
	}

	if (!helper->sram) {
		dev_err(helper->dev,
			"RTKit buffer request with no SRAM region: %pR\n", &res);
		return -EFAULT;
	}

	res.flags = helper->sram->flags;

	if (res.end < res.start || !resource_contains(helper->sram, &res)) {
		dev_err(helper->dev,
			"RTKit buffer request outside SRAM region: %pR\n", &res);
		return -EFAULT;
	}

	bfr->iomem = helper->sram_base + (res.start - helper->sram->start);
	bfr->is_mapped = true;

	return 0;
}

static void apple_rtkit_helper_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	// no-op
}

static const struct apple_rtkit_ops apple_rtkit_helper_ops = {
	.shmem_setup = apple_rtkit_helper_shmem_setup,
	.shmem_destroy = apple_rtkit_helper_shmem_destroy,
};

static void apple_rtkit_helper_release(void *rtk)
{
	apple_rtkit_free_retaining_buffers(rtk);
}

/*
 * MTP firmware cannot always be restarted once it has been taken to QUIESCED
 * or SLEEP, so a boot stage that used it hands the session over running. It
 * reserves the buffers the firmware holds and maps them 1:1 through the
 * DART. Only adopt a session it described whose CPU still runs.
 */
static int apple_rtkit_helper_live(struct apple_rtkit_helper *helper)
{
	const char *handoff;
	int ret;

	if (!of_property_present(helper->dev->of_node, "apple,rtkit-handoff"))
		return 0;
	ret = of_property_read_string(helper->dev->of_node, "apple,rtkit-handoff",
				      &handoff);
	if (ret)
		return ret;

	if (strcmp(handoff, APPLE_RTKIT_HELPER_HANDOFF)) {
		dev_warn(helper->dev, "unknown RTKit handoff \"%s\"\n", handoff);
		return -EINVAL;
	}

	if (!(readl_relaxed(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
	      APPLE_ASC_CPU_CONTROL_RUN)) {
		dev_warn(helper->dev, "RTKit handoff without a running CPU\n");
		return -ENODEV;
	}

	return 1;
}

static bool apple_rtkit_helper_reserved(struct device_node *np,
					const struct resource *buffer)
{
	struct device_node *mem;
	int i, count;

	count = of_count_phandle_with_args(np, "memory-region", NULL);
	for (i = 0; i < count; i++) {
		struct reserved_mem *rmem;
		bool contains = false;

		mem = of_parse_phandle(np, "memory-region", i);
		if (!mem)
			return false;
		rmem = of_reserved_mem_lookup(mem);
		if (rmem && of_property_present(mem, "no-map") && rmem->size &&
		    buffer->start >= rmem->base &&
		    resource_size(buffer) <= rmem->size &&
		    buffer->start - rmem->base <= rmem->size - resource_size(buffer))
			contains = true;
		of_node_put(mem);
		if (contains)
			return true;
	}
	return false;
}

static int apple_rtkit_helper_inherit_buffers(
	struct apple_rtkit_helper *helper,
	struct apple_rtkit_inherited_buffer *buffers)
{
	struct device_node *np = helper->dev->of_node;
	struct property *inventory;
	u32 cells[APPLE_RTKIT_HELPER_MAX_BUFFERS * APPLE_RTKIT_HELPER_BUFFER_CELLS];
	int count, i, length, ret;

	/* T8140 producers predating the buffer inventory remain compatible. */
	inventory = of_find_property(np, "apple,rtkit-buffers", &length);
	if (!inventory) {
		if (of_device_is_compatible(np, "apple,t8132-mtp") ||
		    (of_machine_is_compatible("apple,j614s") &&
		     of_device_is_compatible(np, "apple,t6041-mtp")))
			return -EINVAL;
		return 0;
	}
	if (!length)
		return 0;
	count = of_property_count_u32_elems(np, "apple,rtkit-buffers");
	if (count < 0 || count > ARRAY_SIZE(cells) ||
	    count % APPLE_RTKIT_HELPER_BUFFER_CELLS)
		return -EINVAL;
	if (!count)
		return 0;
	ret = of_property_read_u32_array(np, "apple,rtkit-buffers", cells, count);
	if (ret)
		return ret;

	count /= APPLE_RTKIT_HELPER_BUFFER_CELLS;
	for (i = 0; i < count; i++) {
		u32 *tuple = &cells[i * APPLE_RTKIT_HELPER_BUFFER_CELLS];
		struct apple_rtkit_shmem *b = &buffers[i].shmem;
		struct resource res = { .flags = IORESOURCE_MEM };

		/* These are the system endpoints with shared buffers in RTKit. */
		if ((tuple[0] != 1 && tuple[0] != 2 && tuple[0] != 4 && tuple[0] != 8) ||
		    !tuple[3] || tuple[4] > 1)
			return -EINVAL;
		buffers[i].endpoint = tuple[0];
		b->iova = ((u64)tuple[1] << 32) | tuple[2];
		b->size = tuple[3];
		b->is_mapped = tuple[4];
		res.start = b->iova;
		res.end = res.start + b->size - 1;
		if (res.end < res.start || res.end > DMA_BIT_MASK(44))
			return -EINVAL;

		if (helper->sram && resource_contains(helper->sram, &res)) {
			b->iomem = helper->sram_base + res.start - helper->sram->start;
		} else {
			/* DVA == PA is part of this retained-bypass handoff contract. */
			if (!apple_rtkit_helper_reserved(np, &res))
				return -EINVAL;
			b->iomem = devm_ioremap(helper->dev, res.start, resource_size(&res));
			if (!b->iomem)
				return -ENOMEM;
		}
	}
	return count;
}

static int apple_rtkit_helper_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_rtkit_helper *helper;
	struct apple_rtkit_inherited_buffer buffers[APPLE_RTKIT_HELPER_MAX_BUFFERS] = {};
	int count = 0, live, ret;

	/* 44 bits for addresses in standard RTKit requests */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(44));
	if (ret)
		return ret;

	helper = devm_kzalloc(dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return -ENOMEM;

	helper->dev = dev;
	platform_set_drvdata(pdev, helper);

	helper->asc_base = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(helper->asc_base))
		return PTR_ERR(helper->asc_base);

	helper->sram = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (helper->sram) {
		helper->sram_base = devm_ioremap_resource(dev, helper->sram);
		if (IS_ERR(helper->sram_base))
			return dev_err_probe(dev, PTR_ERR(helper->sram_base),
					"Failed to map SRAM region\n");
	}

	/* Reject an unknown retained-session contract instead of cold-starting it. */
	live = apple_rtkit_helper_live(helper);
	if (live < 0)
		return dev_err_probe(dev, live, "Invalid live RTKit handoff\n");
	if (live) {
		count = apple_rtkit_helper_inherit_buffers(helper, buffers);
		if (count < 0)
			return dev_err_probe(dev, count, "Invalid inherited RTKit buffers\n");
		if ((of_machine_is_compatible("apple,j713") &&
		     of_device_is_compatible(dev->of_node, "apple,t8132-mtp")) ||
		    (of_machine_is_compatible("apple,j614s") &&
		     of_device_is_compatible(dev->of_node, "apple,t6041-mtp")))
			helper->preserved = true;
	}

	if (!live && of_machine_is_compatible("apple,j713") &&
	    of_machine_is_compatible("apple,t8132")) {
		struct property *prop;
		u32 inherited;
		int len;

		prop = of_find_property(dev->of_node,
				"linux-enablement-mac,rtkit-inherited", &len);
		if (prop) {
			if (!len) {
				inherited = 1;
			} else if (len == sizeof(inherited)) {
				ret = of_property_read_u32(dev->of_node,
					"linux-enablement-mac,rtkit-inherited",
					&inherited);
				if (ret || inherited > 1)
					return dev_err_probe(dev, -EINVAL,
						"Invalid inherited helper state\n");
			} else {
				return dev_err_probe(dev, -EINVAL,
						"Invalid inherited helper length\n");
			}
			if (inherited) {
				if (!(readl(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
				      APPLE_ASC_CPU_CONTROL_RUN))
					return dev_err_probe(dev, -ENODEV,
						"Inherited helper is stopped\n");
				helper->preserved = true;
				live = 1;
				dev_warn(dev,
					 "Legacy MTP handoff: servicing notifications without a buffer inventory\n");
			}
		}
	}

	if (live) {
		/*
		 * Set before RX starts: with an inventory, or on T8132, no new
		 * buffer allocation is safe here. Producers that predate the
		 * inventory (T8140) keep mapping IOVAs inside the SRAM region.
		 */
		helper->adopted = true;
		helper->strict_buffers = count > 0 ||
			of_device_is_compatible(dev->of_node, "apple,t8132-mtp") ||
			(of_machine_is_compatible("apple,j614s") &&
			 of_device_is_compatible(dev->of_node, "apple,t6041-mtp"));
		helper->rtk = apple_rtkit_init_adopted_with_buffers(
			dev, helper, NULL, 0, &apple_rtkit_helper_ops, buffers, count);
		if (IS_ERR(helper->rtk))
			return dev_err_probe(dev, PTR_ERR(helper->rtk),
					     "Failed to adopt RTKit\n");

		ret = devm_add_action_or_reset(dev, apple_rtkit_helper_release,
					       helper->rtk);
		if (ret)
			return ret;

		/* There is no safe module-unload boundary for the retained session. */
		__module_get(THIS_MODULE);
		dev_info(dev, "adopted the running RTKit session (%d retained buffers)\n", count);
		return 0;
	}

	helper->rtk =
		devm_apple_rtkit_init(dev, helper, NULL, 0, &apple_rtkit_helper_ops);
	if (IS_ERR(helper->rtk))
		return dev_err_probe(dev, PTR_ERR(helper->rtk),
				     "Failed to initialize RTKit\n");

	writel_relaxed(APPLE_ASC_CPU_CONTROL_RUN,
		       helper->asc_base + APPLE_ASC_CPU_CONTROL);

	/* Works for both wake and boot */
	if (of_device_is_compatible(dev->of_node, "apple,t8142-mtp"))
		ret = apple_rtkit_wake_early_ap(helper->rtk);
	else
		ret = apple_rtkit_wake(helper->rtk);
	if (ret != 0)
		return dev_err_probe(dev, ret, "Failed to wake up coprocessor\n");

	return 0;
}

static void apple_rtkit_helper_remove(struct platform_device *pdev)
{
	struct apple_rtkit_helper *helper = platform_get_drvdata(pdev);

	/* An inherited session would not come back from QUIESCED. */
	if (helper->adopted || helper->preserved)
		return;

	if (apple_rtkit_is_running(helper->rtk))
		apple_rtkit_quiesce(helper->rtk);

	writel_relaxed(0, helper->asc_base + APPLE_ASC_CPU_CONTROL);
}

static int __maybe_unused apple_rtkit_helper_suspend(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	/* Only suspend-to-idle has a verified live-session retention contract. */
	if (helper->adopted && pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		return -EBUSY;
	return 0;
}

static int __maybe_unused apple_rtkit_helper_freeze(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	/* A hibernation image cannot recreate the firmware's live DMA ownership. */
	return helper->adopted ? -EBUSY : 0;
}

static const struct dev_pm_ops apple_rtkit_helper_pm_ops = {
	.suspend = pm_sleep_ptr(apple_rtkit_helper_suspend),
	.freeze = pm_sleep_ptr(apple_rtkit_helper_freeze),
	.poweroff = pm_sleep_ptr(apple_rtkit_helper_freeze),
};

static const struct of_device_id apple_rtkit_helper_of_match[] = {
	{ .compatible = "apple,rtk-helper-asc4" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_rtkit_helper_of_match);

static struct platform_driver apple_rtkit_helper_driver = {
	.driver = {
		.name = "rtkit-helper",
		.of_match_table = apple_rtkit_helper_of_match,
		/* The live MTP input session is announced only once per boot. */
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&apple_rtkit_helper_pm_ops),
	},
	.probe = apple_rtkit_helper_probe,
	.remove = apple_rtkit_helper_remove,
};
module_platform_driver(apple_rtkit_helper_driver);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple RTKit helper driver");
