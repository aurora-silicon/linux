// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * G15 (T6030/T6031, M3 Pro/Max) helpers that have no Rust bindings: static inlines
 * and macros. The module pin is retain.c's. Built only with CONFIG_DRM_AURORA_G15.
 */
#include <crypto/sha2.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/platform_device.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/page-flags.h>
#include <linux/pfn.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/pmp.h>
#include <linux/soc/apple/display.h>

void aurora_g15_sha256(const u8 *data, size_t len, u8 *out);
bool aurora_g15_board(void);
bool aurora_g15_machine(const char *compatible);
void aurora_g15_hold_power(struct device *dev);
void aurora_g15_release_power(struct device *dev);
void aurora_g15_reprobe(void);
void aurora_g15_report_loader_gpu(void);
int aurora_g15_adt_phram(u64 *start, u64 *size);
bool aurora_g15_ram_disjoint(u64 start, u64 size);
bool aurora_g15_ram_within(u64 start, u64 size);
unsigned int aurora_g15_memory_mode(u64 start, u64 size);
void __iomem *aurora_g15_ioremap_wc(u64 start, size_t size);
bool aurora_g15_dma_coherent(struct device *dev);
int aurora_g15_pmp_prepare(struct device *dev);
int aurora_g15_display_prepare(struct device *dev);
int aurora_g15_pmp_get(struct device *dev);
int aurora_g15_pmp_ready(void);
void aurora_g15_pmp_put(struct device *dev);
bool aurora_g15_reserved_uat(u32 index, u64 start, u64 size);

/*
 * The boot loader enables the T6030/T6031 GPU node only when its
 * chosen.aurora,t603x-gpu setting is on; otherwise the node stays disabled,
 * the adapter never probes and nothing says why native G15 is absent. Name
 * the setting once, at module activation, on a machine whose /chosen has it
 * and whose GPU node is disabled. Nothing else changes.
 */
void aurora_g15_report_loader_gpu(void)
{
	static const struct {
		const char *prop;
		const char *compatible;
	} socs[] = {
		{ "aurora,t6031-gpu", "apple,agx-t6031" },
		{ "aurora,t6030-gpu", "apple,agx-t6030" },
	};
	struct device_node *chosen, *np;
	const char *value;
	unsigned int i;
	u32 cell;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return;
	for (i = 0; i < ARRAY_SIZE(socs); i++) {
		if (!of_property_present(chosen, socs[i].prop))
			continue;
		np = of_find_compatible_node(NULL, NULL, socs[i].compatible);
		if (np && !of_device_is_available(np)) {
			if (!of_property_read_string(chosen, socs[i].prop, &value))
				pr_info("aurora: G15: native G15 not started: the boot loader left the GPU node %pOF disabled (/chosen/%s = \"%s\")\n",
					np, socs[i].prop, value);
			else if (!of_property_read_u32(chosen, socs[i].prop, &cell))
				pr_info("aurora: G15: native G15 not started: the boot loader left the GPU node %pOF disabled (/chosen/%s = %u)\n",
					np, socs[i].prop, cell);
		}
		of_node_put(np);
	}
	of_node_put(chosen);
}

/*
 * The maintained aura handoff keeps these four UAT regions in the linear map.
 * Require the exact enabled reserved-memory owner, in addition to the ADT
 * bounds/size/disjointness and per-page PageReserved checks in admission.
 */
bool aurora_g15_reserved_uat(u32 index, u64 start, u64 size)
{
	static const char *const names[] = {
		"uat-ttbs",
		"uat-pagetables",
		"uat-handoff",
		"uat-pagetables-l2",
	};
	struct device_node *rmem, *child, *np = NULL;
	struct resource res;
	unsigned int count = 0;
	bool match;

	if (index >= ARRAY_SIZE(names))
		return false;

	/* Unit addresses belong to the DT layout, not the reservation name. */
	rmem = of_find_node_by_path("/reserved-memory");
	for_each_child_of_node(rmem, child) {
		if (!of_node_name_eq(child, names[index]))
			continue;
		count++;
		if (!np)
			np = of_node_get(child);
	}
	of_node_put(rmem);
	match = count == 1 && np && of_device_is_available(np) &&
		!of_property_read_bool(np, "reusable") &&
		!of_get_address(np, 1, NULL, NULL) &&
		!of_address_to_resource(np, 0, &res) &&
		res.start == start && resource_size(&res) == size;
	of_node_put(np);
	return match;
}

/*
 * Keep a single PMP RTKit owner. The maintained bridge serializes requests,
 * waits for supplier readiness and acknowledgement, and latches lost replies.
 * The GPU retains both its module and supplier dependency until reboot.
 */
int aurora_g15_pmp_prepare(struct device *dev)
{
	int ret;

	if (!aurora_g15_board() || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, "apple,agx-t6030"))
		return -ENODEV;
	/* DISP ACK alone does not prove the native DRM component set is up. */
	/* The direct import waits for the display module to become LIVE; a
	 * symbol_get during its probe could defer without any post-LIVE retry.
	 */
	ret = apple_drm_t6030_link_device(dev);
	if (ret)
		return ret;
	return apple_pmp_link_device(dev);
}

/*
 * T6031 has no PMP vote, but its first firmware start follows the native
 * display as T6030's does: defer until that display is up. The display can
 * be off for this boot (no handoff, or not requested), so say once why. An
 * enabled display that is not published yet is said once too: if it never
 * comes up (its DCP refused or failed to bind), that line is why the GPU
 * stays off.
 */
int aurora_g15_display_prepare(struct device *dev)
{
	struct device_node *np;
	int ret;

	if (!dev->of_node || !of_device_is_compatible(dev->of_node, "apple,agx-t6031"))
		return -ENODEV;
	ret = apple_drm_t6031_link_device(dev);
	if (ret != -EPROBE_DEFER)
		return ret;
	np = of_find_compatible_node(NULL, NULL, "apple,t6031-display-subsystem");
	if (!np || !of_device_is_available(np))
		dev_info_once(dev, "G15: waiting for the T6031 native display, which this boot did not enable (it needs the boot loader's display handoff, and apple_t6031_display.enable=1 except on the J516C)\n");
	else
		dev_info_once(dev, "G15: waiting for the T6031 native display to come up: the GPU firmware starts only after the display is published\n");
	of_node_put(np);
	return ret;
}

int aurora_g15_pmp_get(struct device *dev)
{
	if (!aurora_g15_board() || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, "apple,agx-t6030"))
		return -ENODEV;
	return apple_pmp_set_device_power(0x0f, 5, 1);

}

int aurora_g15_pmp_ready(void)
{
	return apple_pmp_t6030_gpu_ready();
}

/*
 * Give back the vote of aurora_g15_pmp_get() after a start that failed before
 * the firmware was touched. The full M3 stack's J514S bridge has no release:
 * its vote stays, as before.
 */
void aurora_g15_pmp_put(struct device *dev)
{
	int ret = apple_pmp_set_device_power(0x0f, 5, 0);

	if (ret)
		dev_warn(dev, "G15: PMP AGX vote release failed: %d\n", ret);

}

/* SHA-256 of the firmware TEXT and the board templates. */
void aurora_g15_sha256(const u8 *data, size_t len, u8 *out)
{
	sha256(data, len, out);
}

/* Whether this is a J514S/J516S T6030 board. */
bool aurora_g15_board(void)
{
	return of_machine_is_compatible("apple,t6030") &&
	       (of_machine_is_compatible("apple,j514s") ||
		of_machine_is_compatible("apple,j516s"));
}

/* Whether the machine carries this compatible (of_machine_is_compatible()). */
bool aurora_g15_machine(const char *compatible)
{
	return of_machine_is_compatible(compatible) > 0;
}

/*
 * Keep the GFX domain powered for the whole (retained) session, like the
 * firmware session itself; released only by a start that failed before the
 * firmware was touched (aurora_g15_release_power()).
 */
void aurora_g15_hold_power(struct device *dev)
{
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
}

/*
 * Give the hold of aurora_g15_hold_power() back: the start failed before
 * anything firmware-visible was written, so the session is not retained.
 * Only where the AGX vote is given back as well (aurora_g15_pmp_put()): the
 * full M3 stack's J514S bridge keeps its vote, and with it the hold stays.
 */
void aurora_g15_release_power(struct device *dev)
{
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

}

/*
 * The boot loader's phram "adt" export: the first /reserved-memory child
 * compatible with "phram" and labelled "adt".
 */
int aurora_g15_adt_phram(u64 *start, u64 *size)
{
	struct device_node *rmem, *np;
	struct resource res;
	int ret = -ENODEV;

	rmem = of_find_node_by_path("/reserved-memory");
	for_each_child_of_node(rmem, np) {
		const char *label;

		if (of_device_is_compatible(np, "phram") &&
		    !of_property_read_string(np, "label", &label) && !strcmp(label, "adt") &&
		    !of_address_to_resource(np, 0, &res)) {
			of_node_put(np);
			ret = 0;
			break;
		}
	}
	of_node_put(rmem);
	if (ret)
		return ret;
	*start = res.start;
	*size = resource_size(&res);
	return 0;
}

/* The range is disjoint from System RAM (ADT region admission). */
bool aurora_g15_ram_disjoint(u64 start, u64 size)
{
	return region_intersects(start, size, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) ==
	       REGION_DISJOINT;
}

/*
 * Whether DMA mappings of the device skip cache maintenance. The G15 firmware
 * objects rely on the DMA mapping cleaning their pages to the point of
 * coherency, so the device must not be DMA-coherent.
 */
bool aurora_g15_dma_coherent(struct device *dev)
{
	return dev_is_dma_coherent(dev);
}

/*
 * Every page must be valid, System RAM and still reserved from the allocator.
 * REGION_INTERSECTS alone also accepts RAM mixed with undefined memory holes.
 */
bool aurora_g15_ram_within(u64 start, u64 size)
{
	u64 end, pa;

	if (!size || !IS_ALIGNED(start, PAGE_SIZE) ||
	    !IS_ALIGNED(size, PAGE_SIZE) || check_add_overflow(start, size, &end))
		return false;
	for (pa = start; pa < end; pa += PAGE_SIZE) {
		unsigned long pfn = PHYS_PFN(pa);

		if (!pfn_valid(pfn) || !page_is_ram(pfn) ||
		    !PageReserved(pfn_to_page(pfn)))
			return false;
	}
	return true;
}

/* One classification for every adopted-UAT and firmware-region CPU mapping. */
unsigned int aurora_g15_memory_mode(u64 start, u64 size)
{
	u64 end;

	if (!size || !IS_ALIGNED(start, PAGE_SIZE) ||
	    !IS_ALIGNED(size, PAGE_SIZE) || check_add_overflow(start, size, &end))
		return 0;
	if (aurora_g15_ram_within(start, size))
		return MEMREMAP_WB;
	if (aurora_g15_ram_disjoint(start, size))
		return MEMREMAP_WC;
	return 0;
}

/* The inherited RTKit buffer inside firmware DATA. */
void __iomem *aurora_g15_ioremap_wc(u64 start, size_t size)
{
	return ioremap_wc(start, size);
}

/*
 * Called only by the final infallible DriverModule initializer, after both
 * platform registrations have completed. driver_find() returns a borrowed
 * pointer, not an owned reference. The pinned registration is still owned by
 * DriverModule throughout this call; module exit cannot race its initialization.
 * No put_driver() is appropriate. A failed attach must not fail module init:
 * another device in this pass may already have started retained firmware.
 */
void aurora_g15_reprobe(void)
{
	const struct device_driver *drv;
	int ret;

	drv = driver_find("aurora-g15", &platform_bus_type);
	if (!drv) {
		pr_err("aurora-g15: registered adapter not found for activation\n");
		return;
	}
	ret = driver_attach(drv);
	if (ret)
		pr_err("aurora-g15: activation attach failed: %d\n", ret);
}
