// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * H16/H17 facts from the 2026-10-31 H16/H17 specification, sections 3-7,
 * 12 and 15, and the H16 cold-boot facts, round 5. Metadata preflight
 * performs no MMIO, DMA mapping or power access.
 */
#include <linux/ioport.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "ane_fw_platform.h"
#include "ane_h16_boot_data.h"

static char *h16_stage;
module_param(h16_stage, charp, 0444);
MODULE_PARM_DESC(h16_stage, "H16 opt-in stage: preflight, map, boot, startup, program; unset refuses");

static const char * const ane_h16_reg_names[] = {
	"engine", "pmgr", "aux-0", "aux-1", "aux-2", "aux-3", "aux-4",
};

static const u32 ane_h16_reg_sizes[] = {
	0x2000000, 0x18000, 0x4000, 0x30000, 0xff4000, 0x2000000, 0x4000,
};

/* Offsets in reg[1]; genpd, never the ANE driver, owns transitions. */
static const u32 ane_h16_power_words[] = {
	0x3c0, 0xc008, 0xc000, 0xc010, 0xc018,
};

const struct ane_fw_platform ane_h16_platform = {
	.h16_boot = &ane_h16_build_26a428,
	/* The DT compatible is t6041; the physical M4 Pro is T6040. */
	.soc = 0x6040,
	.chip_family = 16,
	.ane_type = 0x110,
	.hwx_subtype = 7,
	.task_generation = 17,
	.interrupt_count = 1,
	/* J616s IRQ 1054 drains replies and events from all F2H channels. */
	.irq_role = ANE_FW_IRQ_CHANNELS,
	.owner = ANE_FW_OWNER_AP,
	.reg_names = ane_h16_reg_names,
	.reg_sizes = ane_h16_reg_sizes,
	.reg_count = ARRAY_SIZE(ane_h16_reg_names),
	.wrapper = 0x1600000,
	.mailbox = 0x1608000,
	.scratch = 0x1880020,
	.doorbell = 0x1884000,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.power_words = ane_h16_power_words,
	.power_count = ARRAY_SIZE(ane_h16_power_words),
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0) | BIT(11),
	.firmware_preloaded = true,
	.safe_reset = false,
};

const struct ane_fw_platform ane_h17_platform = {
	.soc = 0x8140,
	.chip_family = 17,
	.ane_type = 0x200,
	.hwx_subtype = 9,
	.task_generation = 19,
	.interrupt_count = 2,
	.owner = ANE_FW_OWNER_EXCLAVE,
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0) | BIT(11),
	.firmware_preloaded = true,
	.safe_reset = false,
	/* No H16 register-map inference is executable on H17. */
};

static int ane_fw_preload_check(struct device *dev,
				const struct ane_fw_platform *soc)
{
	struct device_node *np = dev->of_node;
	struct device_node *reserved __free(device_node) =
		of_find_node_by_path("/reserved-memory");
	u64 phys[2], size[2], dva[2];
	unsigned int i;

	if (!reserved ||
	    of_property_count_u32_elems(np, "memory-region") != 2 ||
	    of_property_count_strings(np, "memory-region-names") != 2 ||
	    of_property_count_u64_elems(np, "apple,firmware-iovas") != 2 ||
	    of_property_read_u64_array(np, "apple,firmware-iovas", dva, 2))
		return -ENODATA;

	for (i = 0; i < 2; i++) {
		struct device_node *mem;
		struct reserved_mem *rmem;
		const char *name;
		bool valid;

		if (of_property_read_string_index(np, "memory-region-names", i,
						  &name) ||
		    strcmp(name, i ? "firmware-data" : "firmware-text"))
			return -EINVAL;
		mem = of_parse_phandle(np, "memory-region", i);
		if (!mem)
			return -EINVAL;
		rmem = of_reserved_mem_lookup(mem);
		valid = rmem && mem->parent == reserved &&
			of_device_is_available(mem) &&
			of_property_read_bool(mem, "no-map") &&
			!of_property_read_bool(mem, "reusable");
		if (valid) {
			phys[i] = rmem->base;
			size[i] = rmem->size;
		}
		of_node_put(mem);
		if (!valid)
			return -EINVAL;
	}

	return ane_fw_preload_ranges_valid(soc, phys, size, dva) ? 0 : -EINVAL;
}

int ane_fw_platform_preflight(struct platform_device *pdev,
			      const struct ane_fw_platform *soc)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	u32 ane_type;
	unsigned int i;
	int ret;

	/* No backend has acquired ownership, even if DT omits the marker.
	 * Check before resources, firmware, power, IOMMU or engine access.
	 */
	if (soc->owner == ANE_FW_OWNER_EXCLAVE) {
		dev_info(dev, "H17 ANE: Linux exclave ownership handoff unavailable; refusing probe\n");
		return -EOPNOTSUPP;
	}
	if (of_property_read_bool(np, "apple,exclave-assigned")) {
		dev_info(dev, "ANE: exclave-assigned device has no Linux ownership provider\n");
		return -EOPNOTSUPP;
	}

	if (!of_property_read_bool(np, "apple,firmware-preloaded") ||
	    of_property_read_u32(np, "apple,ane-type", &ane_type) ||
	    ane_type != soc->ane_type)
		return dev_err_probe(dev, -EINVAL, "H16 firmware identity missing or invalid\n");

	if (of_property_count_strings(np, "reg-names") != soc->reg_count ||
	    platform_get_resource(pdev, IORESOURCE_MEM, soc->reg_count) ||
	    of_irq_count(np) != soc->interrupt_count)
		return dev_err_probe(dev, -EINVAL, "H16 resource count mismatch\n");

	for (i = 0; i < soc->reg_count; i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   soc->reg_names[i]);
		if (!res || resource_size(res) != soc->reg_sizes[i])
			return dev_err_probe(dev, -EINVAL, "H16 resource geometry mismatch\n");
	}

	ret = ane_fw_preload_check(dev, soc);
	if (ret)
		return dev_err_probe(dev, ret,
			"H16 requires bootloader-reserved no-map TEXT/DATA preload ranges\n");

	ret = ane_h16_stage_parse(h16_stage);
	if (ret < 0)
		return ret;
	if (!ret) {
		dev_info(dev, "H16 ANE: staged backend requires h16_stage opt-in; refusing before power access\n");
		return -EOPNOTSUPP;
	}
	if (!soc->h16_boot || of_property_count_u64_elems(np, "apple,dapf-ranges") != 8)
		return -ENODATA;
	return 0;
}

int ane_fw_platform_probe(struct platform_device *pdev, const struct ane_fw_platform *soc)
{
	int ret = ane_fw_platform_preflight(pdev, soc);

	if (ret)
		return ret;
	return ane_h16_backend_probe(pdev, soc, ane_h16_stage_parse(h16_stage));
}

void ane_fw_platform_remove(struct platform_device *pdev)
{
	ane_h16_backend_remove(pdev);
}
