// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * G16 (T8132, M4) helpers that have no Rust bindings: static inlines,
 * macros and THIS_MODULE. Built only with CONFIG_DRM_ASAHI_G16.
 */
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-display.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/soc/apple/pmp-telemetry.h>
#include <linux/thermal.h>

bool asahi_g16_board(void);
int asahi_g16_hold_power(struct device *dev);
void asahi_g16_module_get(void);
bool asahi_g16_ram_disjoint(u64 start, u64 size);
void __iomem *asahi_g16_ioremap_wc(u64 start, size_t size);
bool asahi_g16_dma_unsupported(struct device *dev);

/* Thermal-framework callbacks only publish requests. The Rust GPU owner
 * samples the PMP provider and shared cooling policy under its device mutex;
 * thermal callbacks never enter the GPU lock.
 */
struct asahi_g16_cooling {
	atomic_t state;
	struct apple_pmp_gpu *pmp;
	struct device_node *pmp_node;
	struct apple_pmp_cooling policy;
	struct thermal_cooling_device *cdev;
};

struct asahi_g16_cooling *asahi_g16_cooling_register(struct device *dev);
void asahi_g16_cooling_unregister(struct asahi_g16_cooling *cooling);
int asahi_g16_cooling_sample(struct asahi_g16_cooling *cooling, u64 now,
			    long *temperature, u32 *governor, u32 *power,
			    u32 *backstop);

static int asahi_g16_cooling_max(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	*state = 8;
	return 0;
}

static int asahi_g16_cooling_get(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	struct asahi_g16_cooling *cooling = cdev->devdata;

	*state = atomic_read(&cooling->state);
	return 0;
}

static int asahi_g16_cooling_set(struct thermal_cooling_device *cdev,
				unsigned long state)
{
	struct asahi_g16_cooling *cooling = cdev->devdata;

	if (state > 8)
		return -EINVAL;
	atomic_set(&cooling->state, state);
	return 0;
}

static const struct thermal_cooling_device_ops asahi_g16_cooling_ops = {
	.get_max_state = asahi_g16_cooling_max,
	.get_cur_state = asahi_g16_cooling_get,
	.set_cur_state = asahi_g16_cooling_set,
};

struct asahi_g16_cooling *asahi_g16_cooling_register(struct device *dev)
{
	struct asahi_g16_cooling *cooling;
	int ret;

	cooling = kzalloc_obj(*cooling);
	if (!cooling)
		return ERR_PTR(-ENOMEM);
	atomic_set(&cooling->state, 8);
	apple_pmp_cooling_init(&cooling->policy);
	cooling->pmp_node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	if (!cooling->pmp_node) {
		ret = -EINVAL;
		goto free;
	}
	cooling->pmp = apple_pmp_gpu_get(dev);
	if (IS_ERR(cooling->pmp)) {
		ret = PTR_ERR(cooling->pmp);
		goto put_node;
	}
	cooling->cdev = thermal_of_cooling_device_register(dev->of_node, 0,
							  "asahi-g16g", cooling,
							  &asahi_g16_cooling_ops);
	if (IS_ERR(cooling->cdev)) {
		ret = PTR_ERR(cooling->cdev);
		apple_pmp_gpu_put(cooling->pmp);
		goto put_node;
	}
	return cooling;
put_node:
	of_node_put(cooling->pmp_node);
free:
	kfree(cooling);
	return ERR_PTR(ret);
}

void asahi_g16_cooling_unregister(struct asahi_g16_cooling *cooling)
{
	thermal_cooling_device_unregister(cooling->cdev);
	apple_pmp_gpu_put(cooling->pmp);
	of_node_put(cooling->pmp_node);
	kfree(cooling);
}

/* Serialized by the GPU owner. Provider lifetime and MMIO access stay with
 * PMP: this consumer owns no SRAM mapping and cannot restart its firmware.
 */
int asahi_g16_cooling_sample(struct asahi_g16_cooling *cooling, u64 now,
			    long *temperature, u32 *governor, u32 *power,
			    u32 *backstop)
{
	int ret;

	*temperature = 0;
	ret = apple_pmp_telemetry_get_hotspot(cooling->pmp_node, temperature);
	apple_pmp_cooling_sample(&cooling->policy, now, *temperature, ret);
	*governor = atomic_read(&cooling->state);
	*power = apple_pmp_gpu_power_state(cooling->pmp);
	apple_pmp_cooling_observe(&cooling->policy, now, *governor);
	*backstop = apple_pmp_cooling_level(&cooling->policy);
	return ret;
}

/* Whether this is a qualified J713/T8132 board. */
bool asahi_g16_board(void)
{
	return apple_pmp_j713_board();
}

/*
 * Keep the GFX domain powered for the whole (retained) session. Never released,
 * like the firmware session itself.
 */
int asahi_g16_hold_power(struct device *dev)
{
	int ret;

	pm_runtime_get_noresume(dev);
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_enable(dev);
	return dev->pm_domain ? 0 : -EINVAL;
}

/* Pin this module until reboot. */
void asahi_g16_module_get(void)
{
	__module_get(THIS_MODULE);
}

/* The range is disjoint from System RAM (ADT region admission). */
bool asahi_g16_ram_disjoint(u64 start, u64 size)
{
	return region_intersects(start, size, IORESOURCE_SYSTEM_RAM,
				 IORES_DESC_NONE) == REGION_DISJOINT;
}

/*
 * Whether DMA mappings of the device skip cache maintenance. The G16 firmware
 * objects rely on the DMA mapping cleaning their pages to the point of
 * coherency, so the device must not be DMA-coherent.
 */
bool asahi_g16_dma_unsupported(struct device *dev)
{
	if (dev_is_dma_coherent(dev) || device_iommu_mapped(dev) ||
	    get_dma_ops(dev))
		return true;
#ifdef CONFIG_DMA_DECLARE_COHERENT
	if (dev->dma_mem)
		return true;
#endif
	return false;
}

/* The inherited RTKit buffer inside firmware DATA. */
void __iomem *asahi_g16_ioremap_wc(u64 start, size_t size)
{
	return ioremap_wc(start, size);
}

/* Resolve and validate a no-map reserved-memory phandle. This wraps kernel OF
 * helpers; all GPU handoff, size, ordering and overlap policy remains in Rust.
 */
int asahi_g16_reserved_region(struct device *dev, const char *name, u64 *start,
			      u64 *size);
int asahi_g16_reserved_region(struct device *dev, const char *name, u64 *start,
			      u64 *size)
{
	struct device_node *np;
	struct reserved_mem *rmem;
	struct resource res;
	int index, len, ret;

	index = of_property_match_string(dev->of_node, "memory-region-names",
					 name);
	if (index < 0)
		return -EINVAL;
	np = of_parse_phandle(dev->of_node, "memory-region", index);
	if (!np)
		return -EINVAL;
	rmem = of_reserved_mem_lookup(np);
	ret = !of_find_property(np, "no-map", &len) || len ||
			      of_property_read_bool(np, "reusable") ||
			      !of_device_is_available(np) ?
		      -EINVAL :
		      of_address_to_resource(np, 0, &res);
	if (!ret && (!rmem || rmem->base != res.start ||
		     rmem->size != resource_size(&res)))
		ret = -EINVAL;
	of_node_put(np);
	if (ret)
		return ret;
	*start = res.start;
	*size = resource_size(&res);
	return 0;
}

/* Read the first PMGR power-domain register. Policy belongs to the Rust owner. */
int asahi_g16_power_domain_state(struct device *dev, u32 *offset, u32 *state);
int asahi_g16_power_domain_state(struct device *dev, u32 *offset, u32 *state)
{
	struct device_node *np;
	struct regmap *pmgr;
	int ret;

	if (!dev->pm_domain)
		return -EINVAL;
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!np)
		return -EINVAL;
	ret = of_property_read_u32(np, "reg", offset);
	pmgr = np->parent ? syscon_node_to_regmap(np->parent) :
			    ERR_PTR(-EINVAL);
	of_node_put(np);
	if (ret)
		return ret;
	if (IS_ERR(pmgr))
		return PTR_ERR(pmgr);
	return regmap_read(pmgr, *offset, state);
}
