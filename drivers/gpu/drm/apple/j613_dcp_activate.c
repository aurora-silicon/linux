// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Coldplug of the exact J613 25G83 display and its same-boot PMP supplier. */
#include <linux/bitfield.h>
#include <linux/sizes.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#include "dcp.h"
#include "j613-25g83.h"
#include "../../../pmdomain/apple/pmp-report-validation.h"

static unsigned int display_clock_hz;
static struct of_changeset gate;

unsigned int apple_j613_25g83_clock_hz(void)
{
	return display_clock_hz;
}

/* These are DT domain nodes, carrying the PMGR offsets, not copied registers. */
static int j613_power_ready(void)
{
	static const char *const labels[] = {
		"disp_sys", "disp_fe", "disp_cpu", "pmp", "pms_sram",
	};
	static const u32 offsets[] = { 0x1d8, 0x208, 0x10000, 0x450, 0x458 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(labels); i++) {
		struct device_node *np, *found = NULL;
		const char *label;
		void __iomem *pmgr;
		struct resource res;
		u32 offset, state;

		for_each_compatible_node(np, NULL, "apple,t8122-pmgr-pwrstate") {
			if (of_property_read_string(np, "label", &label) ||
			    strcmp(label, labels[i]))
				continue;
			if (found) {
				of_node_put(found);
				of_node_put(np);
				return -EINVAL;
			}
			found = of_node_get(np);
		}
		if (!found)
			return -ENODEV;
		if (of_property_read_u32(found, "reg", &offset) || offset != offsets[i] ||
		    of_address_to_resource(found->parent, 0, &res) ||
		    resource_size(&res) < 4 || offset > resource_size(&res) - 4) {
			of_node_put(found);
			return -EINVAL;
		}
		/* The register range is described by the parent syscon. */
		pmgr = of_iomap(found->parent, 0);
		of_node_put(found);
		if (!pmgr)
			return -ENOMEM;
		state = readl(pmgr + offset);
		iounmap(pmgr);
		if (FIELD_GET(GENMASK(7, 4), state) != 15)
			return -EBUSY;
	}
	return 0;
}

/* The SRAM image and integer bootargs must match this boot's nub. */
static int j613_pmp_image(struct device_node *pmp)
{
	void __iomem *sram, *asc, *ptd;
	struct resource res;
	uuid_t image, expected;
	u32 offset, size;
	u8 *args;
	unsigned int pos;
	int ret = -EINVAL;

	if (uuid_parse(J613_25G83_PMP_UUID, &expected) ||
	    of_address_to_resource(pmp, 0, &res))
		return -EINVAL;
	sram = of_iomap(pmp, 0);
	if (!sram)
		return -ENOMEM;
	if (resource_size(&res) < 0x234)
		goto out;
	for (pos = 0; pos < sizeof(image.b); pos += 4)
		put_unaligned_le32(readl(sram + 0x214 + pos), image.b + pos);
	offset = readl(sram + 0x22c);
	size = readl(sram + 0x230);
	if (!uuid_equal(&image, &expected) || !size || size > SZ_4K ||
	    size > resource_size(&res) || offset > resource_size(&res) - size || offset & 3)
		goto out;
	args = kmalloc(size, GFP_KERNEL);
	if (!args) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy_fromio(args, sram + offset, size);
	ret = apple_pmp_bootargs_valid(args, size) ? 0 : -EINVAL;
	kfree(args);
out:
	iounmap(sram);
	if (ret)
		return ret;
	asc = of_iomap(pmp, 1);
	if (!asc)
		return -ENOMEM;
	/* A new RTKit session starts only the halted, matching PMP image. */
	ret = readl(asc + 0x44) & BIT(4) ? -EBUSY : 0;
	iounmap(asc);
	if (ret)
		return ret;
	ptd = ioremap_np(0x2d03c0000ULL, 0x14000);
	if (!ptd)
		return -ENOMEM;
	ret = readq(ptd + 0x10) || readq(ptd + 0x1000) ? -EBUSY : 0;
	iounmap(ptd);
	return ret;
}

static int j613_pmp_values(struct device_node *pmp)
{
	struct device_node *values = of_find_node_by_path("/chosen/asahi,t8122-pmp");
	struct property *prop, *copy;
	const char *uuid;
	const u8 *table;
	u32 value;
	int length, ret = -EINVAL;

	if (!values)
		return -ENODEV;
	if (of_property_read_u32(values, "apple,board-id", &value) ||
	    of_property_read_u32(values, "apple,dram-vendor-id", &value) ||
	    of_property_read_string(values, "apple,tunable-uuid", &uuid) ||
	    strcmp(uuid, J613_25G83_PMP_UUID))
		goto out;
	table = of_get_property(values, "apple,tunable-ptd-range", &length);
	if (!apple_pmp_ranges_valid(table, length, 0x1000, 0x10800, 0x1080))
		goto out;
	table = of_get_property(values, "apple,tunable-soc-device", &length);
	if (!table || length != 20 * 0x7c ||
	    get_unaligned_le32(table + 11 * 0x7c) != 8 ||
	    memcmp(table + 11 * 0x7c + 0x74, "DISP\0\0\0\0", 8))
		goto out;
	ret = j613_pmp_image(pmp);
	if (ret)
		goto out;
	for_each_property_of_node(values, prop) {
		if (strcmp(prop->name, "apple,board-id") &&
		    strcmp(prop->name, "apple,dram-vendor-id") &&
		    strcmp(prop->name, "apple,dram-capacity") &&
		    !strstarts(prop->name, "apple,tunable-"))
			continue;
		if (!prop->length || prop->length > SZ_64K ||
		    of_property_present(pmp, prop->name)) {
			ret = -EINVAL;
			break;
		}
		copy = kzalloc_obj(*copy);
		if (!copy) {
			ret = -ENOMEM;
			break;
		}
		copy->name = kstrdup(prop->name, GFP_KERNEL);
		copy->value = kmemdup(prop->value, prop->length, GFP_KERNEL);
		copy->length = prop->length;
		if (!copy->name || !copy->value) {
			kfree(copy->name);
			kfree(copy->value);
			kfree(copy);
			ret = -ENOMEM;
			break;
		}
		ret = of_changeset_add_property(&gate, pmp, copy);
		if (ret) {
			kfree(copy->name);
			kfree(copy->value);
			kfree(copy);
		}
		if (ret)
			break;
	}
out:
	of_node_put(values);
	return ret;
}

int __init apple_j613_25g83_coldplug(void)
{
	static const char *const paths[] = {
		"/soc/pmp-report@2d03c0000", "/soc/iommu@2d0300000",
		"/soc/mailbox@2d0c08000", "/soc/pmp@2d0500000",
		"/soc/iommu@28d30c000", "/soc/iommu@28d304000",
		"/soc/mailbox@28ec08000", "/soc/display-subsystem",
		"/soc/dcp@28ec00000/piodma", "/soc/dcp@28ec00000",
	};
	struct device_node *nodes[ARRAY_SIZE(paths)] = {}, *chosen;
	const char *uuid, *status;
	void __iomem *cpu;
	u32 profile, version[3], witness[5], markers[3];
	unsigned int i;
	int ret = -EINVAL;

	if (!of_machine_is_compatible("apple,j613"))
		return 0;
	nodes[9] = of_find_node_by_path(paths[9]);
	/* Ordinary 14.x J613 boot keeps the existing gate and report driver. */
	if (!nodes[9] || !of_property_present(nodes[9], "apple,j613-25g83-profile")) {
		ret = 0;
		goto out;
	}
	if (of_property_count_u32_elems(nodes[9], "apple,j613-25g83-profile") != 1 ||
	    of_property_read_u32(nodes[9], "apple,j613-25g83-profile", &profile) ||
	    of_property_count_u32_elems(nodes[9], "apple,firmware-compat") != 3 ||
	    of_property_read_u32_array(nodes[9], "apple,firmware-compat", version, 3) ||
	    of_property_read_string(nodes[9], "apple,firmware-uuid", &uuid) ||
	    !j613_25g83_identity(true, profile, version, 3, uuid))
		goto out;
	chosen = of_find_node_by_path("/chosen");
	ret = chosen && of_property_count_u32_elems(chosen,
		"apple,j613-25g83-display-clock-adt") == 5 ?
		of_property_read_u32_array(chosen, "apple,j613-25g83-display-clock-adt", witness, 5) : -EINVAL;
	of_node_put(chosen);
	if (ret || !j613_25g83_clock(witness, 5)) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		if (!nodes[i])
			nodes[i] = of_find_node_by_path(paths[i]);
		if (!nodes[i] || of_property_read_string(nodes[i], "status", &status) ||
		    strcmp(status, "disabled")) {
			ret = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < 3; i++) {
		if (of_property_count_u32_elems(nodes[7 + i], J613_25G83_HANDOFF) != 1 ||
		    of_property_read_u32(nodes[7 + i], J613_25G83_HANDOFF, &markers[i])) {
			ret = -EINVAL;
			goto out;
		}
	}
	if (!j613_25g83_mappings(markers, 3)) {
		ret = -EINVAL;
		goto out;
	}
	ret = j613_power_ready();
	if (ret)
		goto out;
	cpu = of_iomap(nodes[9], 0);
	if (!cpu) {
		ret = -ENOMEM;
		goto out;
	}
	ret = readl(cpu + 0x44) & BIT(4) ? 0 : -EBUSY;
	iounmap(cpu);
	if (ret)
		goto out;
	of_changeset_init(&gate);
	ret = j613_pmp_values(nodes[3]);
	for (i = 0; i < ARRAY_SIZE(nodes) && !ret; i++)
		ret = of_changeset_update_prop_string(&gate, nodes[i], "status", "okay");
	if (!ret) {
		display_clock_hz = witness[3];
		ret = of_changeset_apply(&gate);
	}
	if (ret) {
		display_clock_hz = 0;
		of_changeset_destroy(&gate);
	} else {
		display_clock_hz = witness[3];
		pr_info("J613/25G83 display handoff accepted; ADT clock %u Hz\n", display_clock_hz);
	}
out:
	for (i = 0; i < ARRAY_SIZE(nodes); i++)
		of_node_put(nodes[i]);
	return ret;
}
