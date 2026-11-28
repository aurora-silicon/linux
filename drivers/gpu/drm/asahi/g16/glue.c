// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * G16 (T8132, M4) helpers that have no Rust bindings: static inlines
 * and macros. The module pin is retain.c's. Built only with CONFIG_DRM_ASAHI_G16.
 */
#include <linux/device.h>
#include <asm/arch_timer.h>
#include <linux/atomic.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/apple/clpc.h>
#include <linux/soc/apple/display.h>
#include <linux/soc/apple/pmp-display.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/soc/apple/pmp-telemetry.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include "glue.h"

bool asahi_g16_board(void);
int asahi_g16_hold_power(struct device *dev);
bool asahi_g16_ram_disjoint(u64 start, u64 size);
void __iomem *asahi_g16_ioremap_wc(u64 start, size_t size);
bool asahi_g16_dma_unsupported(struct device *dev);

/* Thermal-framework callbacks only publish requests. The Rust GPU owner
 * samples the PMP provider (when apple,pmp is present) or cached SoC die
 * thermal-zone readings (otherwise) under its device mutex; thermal callbacks
 * never enter the GPU lock and SMC/zone polling runs outside it.
 */
#define ASAHI_G16_ZONE_POLL_FAST_MS	250
#define ASAHI_G16_ZONE_POLL_IDLE_MS	1000
#define ASAHI_G16_ZONE_STALE_NS		3000000000ULL

static const char * const asahi_g16_die_zones[] = {
	"macsmc_soc_die",
	"apple_pmp_hotspot",
};

struct asahi_g16_zone_trips {
	long release_mc;
	long hot_mc;
	long critical_mc;
	long max_passive_mc;
	bool have_hot;
	bool have_critical;
};

struct asahi_g16_cooling {
	atomic_t state;
	struct apple_pmp_gpu *pmp;
	struct device_node *pmp_node;
	struct apple_pmp_cooling policy;
	struct thermal_cooling_device *cdev;
	/* The last reading from a sensor that was valid, and when. */
	u64 valid_ns;
	long valid_mc;
	/* Cached thermal-zone telemetry when no apple,pmp node is present. */
	spinlock_t zone_lock;
	struct delayed_work zone_poll;
	atomic_t zone_active;
	atomic_t stopping;
	u64 zone_seq;
	u64 zone_ns;
	long zone_mc;
	long zone_release_mc;
	long zone_hot_mc;
	long zone_critical_mc;
	bool zone_limits_valid;
	int zone_err;
};

struct asahi_g16_cooling *asahi_g16_cooling_register(struct device *dev);
void asahi_g16_cooling_unregister(struct asahi_g16_cooling *cooling);
int asahi_g16_cooling_sample(struct asahi_g16_cooling *cooling, u64 now, bool idle,
			     long *temperature, u32 *governor, u32 *power,
			     u32 *backstop, u64 *zone_seq, long *release_mc,
			     long *hot_mc, long *critical_mc,
			     bool *limits_valid);
int asahi_g16_clpc_gpu_get(struct device *dev, struct apple_clpc_gpu **out);
void asahi_g16_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active);
void asahi_g16_clpc_gpu_put(struct apple_clpc_gpu *gpu);

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

static int asahi_g16_zone_trip_cb(struct thermal_trip *trip, void *data)
{
	struct asahi_g16_zone_trips *t = data;
	long temp = trip->temperature;
	long hyst = trip->hysteresis;

	if (temp == THERMAL_TEMP_INVALID || temp <= 0)
		return 0;

	switch (trip->type) {
	case THERMAL_TRIP_PASSIVE:
		if (temp > t->max_passive_mc)
			t->max_passive_mc = temp;
		fallthrough;
	case THERMAL_TRIP_ACTIVE:
		if (!t->have_hot || temp < t->hot_mc) {
			t->hot_mc = temp;
			t->release_mc = (hyst >= 0 && hyst < temp) ?
					temp - hyst : 0;
			t->have_hot = true;
		}
		break;
	case THERMAL_TRIP_HOT:
	case THERMAL_TRIP_CRITICAL:
		if (!t->have_critical || temp < t->critical_mc) {
			t->critical_mc = temp;
			t->have_critical = true;
		}
		break;
	default:
		break;
	}
	return 0;
}

static bool asahi_g16_zone_limits_valid(struct asahi_g16_zone_trips *trips)
{
	if (!trips->have_critical && trips->have_hot &&
	    trips->max_passive_mc > trips->hot_mc) {
		trips->critical_mc = trips->max_passive_mc;
		trips->have_critical = true;
	}
	return trips->have_hot && trips->have_critical &&
	       trips->release_mc > 0 && trips->release_mc <= trips->hot_mc &&
	       trips->critical_mc > trips->hot_mc;
}

static void asahi_g16_zone_poll_fn(struct work_struct *work)
{
	struct asahi_g16_cooling *cooling =
		container_of(to_delayed_work(work), struct asahi_g16_cooling,
			     zone_poll);
	long hottest = 0;
	long release_mc = 0;
	long hot_mc = 0;
	long critical_mc = 0;
	bool found = false;
	bool have_limits = false;
	bool limits_valid = true;
	int ret = -ENODEV;
	unsigned int i;
	unsigned int delay_ms;

	if (atomic_read(&cooling->stopping))
		return;

	for (i = 0; i < ARRAY_SIZE(asahi_g16_die_zones); i++) {
		struct asahi_g16_zone_trips trips = {};
		int temp, err;

		err = thermal_zone_get_temp_and_trips(asahi_g16_die_zones[i],
						      &temp, asahi_g16_zone_trip_cb,
						      &trips);

		if (!err) {
			if (!found || temp > hottest)
				hottest = temp;
			found = true;

			if (!asahi_g16_zone_limits_valid(&trips)) {
				limits_valid = false;
			} else if (!have_limits) {
				release_mc = trips.release_mc;
				hot_mc = trips.hot_mc;
				critical_mc = trips.critical_mc;
				have_limits = true;
			} else {
				release_mc = min(release_mc, trips.release_mc);
				hot_mc = min(hot_mc, trips.hot_mc);
				critical_mc = min(critical_mc, trips.critical_mc);
			}
		} else if (err != -ENODEV) {
			ret = err;
			found = false;
			limits_valid = false;
			break;
		}
	}
	if (found)
		ret = 0;
	if (!found || !have_limits || !limits_valid || release_mc <= 0 ||
	    release_mc > hot_mc || critical_mc <= hot_mc)
		limits_valid = false;

	spin_lock(&cooling->zone_lock);
	cooling->zone_seq++;
	cooling->zone_ns = ktime_get_ns();
	cooling->zone_mc = found ? hottest : 0;
	cooling->zone_release_mc = limits_valid ? release_mc : 0;
	cooling->zone_hot_mc = limits_valid ? hot_mc : 0;
	cooling->zone_critical_mc = limits_valid ? critical_mc : 0;
	cooling->zone_limits_valid = limits_valid;
	cooling->zone_err = ret;
	spin_unlock(&cooling->zone_lock);

	if (atomic_read(&cooling->stopping))
		return;

	delay_ms = (ret || !limits_valid || atomic_read(&cooling->zone_active) ||
		    hottest >= release_mc) ?
			   ASAHI_G16_ZONE_POLL_FAST_MS :
			   ASAHI_G16_ZONE_POLL_IDLE_MS;
	queue_delayed_work(system_freezable_wq, &cooling->zone_poll,
			   msecs_to_jiffies(delay_ms));
}

struct asahi_g16_cooling *asahi_g16_cooling_register(struct device *dev)
{
	struct asahi_g16_cooling *cooling;
	int ret;

	cooling = kzalloc_obj(*cooling);
	if (!cooling)
		return ERR_PTR(-ENOMEM);
	apple_pmp_cooling_init(&cooling->policy);
	spin_lock_init(&cooling->zone_lock);
	INIT_DELAYED_WORK(&cooling->zone_poll, asahi_g16_zone_poll_fn);
	cooling->zone_err = -ENODATA;
	cooling->pmp_node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	/* A disabled PMP node never probes, so it provides no telemetry. */
	if (cooling->pmp_node && !of_device_is_available(cooling->pmp_node)) {
		of_node_put(cooling->pmp_node);
		cooling->pmp_node = NULL;
	}
	if (cooling->pmp_node) {
		atomic_set(&cooling->state, 8);
		cooling->pmp = apple_pmp_gpu_get(dev);
		if (IS_ERR(cooling->pmp)) {
			ret = dev_err_probe(dev, PTR_ERR(cooling->pmp),
					    "PMP telemetry from %pOF unavailable\n",
					    cooling->pmp_node);
			goto put_node;
		}
	} else {
		/*
		 * Without an available apple,pmp supplier, thermal-zone
		 * telemetry drives the fail-closed backstop (starting at maximum
		 * cooling) while the cooling device remains available to thermal
		 * policy.
		 */
		atomic_set(&cooling->state, 0);
	}
	cooling->cdev = thermal_of_cooling_device_register(dev->of_node, 0,
							  "aurora-g16g", cooling,
							  &asahi_g16_cooling_ops);
	if (IS_ERR(cooling->cdev)) {
		ret = PTR_ERR(cooling->cdev);
		if (cooling->pmp)
			apple_pmp_gpu_put(cooling->pmp);
		goto put_node;
	}
	if (!cooling->pmp_node)
		queue_delayed_work(system_freezable_wq, &cooling->zone_poll, 0);
	return cooling;
put_node:
	of_node_put(cooling->pmp_node);
	kfree(cooling);
	return ERR_PTR(ret);
}

void asahi_g16_cooling_unregister(struct asahi_g16_cooling *cooling)
{
	atomic_set(&cooling->stopping, 1);
	if (!cooling->pmp_node)
		cancel_delayed_work_sync(&cooling->zone_poll);
	thermal_cooling_device_unregister(cooling->cdev);
	if (cooling->pmp)
		apple_pmp_gpu_put(cooling->pmp);
	of_node_put(cooling->pmp_node);
	kfree(cooling);
}

bool asahi_g16_cooling_has_pmp(const struct asahi_g16_cooling *cooling)
{
	return cooling->pmp_node != NULL;
}

/* Serialized by the GPU owner. Provider lifetime and MMIO access stay with
 * PMP: this consumer owns no SRAM mapping and cannot restart its firmware.
 * An idle GPU samples about twice a second and makes no heat, so with nothing
 * in flight the last cool reading stands for as long as the sensors stay dark.
 * Without apple,pmp, return the latest cached SoC die thermal-zone reading and
 * fail closed whenever no fresh reading is available.
 */
int asahi_g16_cooling_sample(struct asahi_g16_cooling *cooling, u64 now, bool idle,
			     long *temperature, u32 *governor, u32 *power,
			     u32 *backstop, u64 *zone_seq, long *release_mc,
			     long *hot_mc, long *critical_mc,
			     bool *limits_valid)
{
	int ret;

	*temperature = 0;
	*zone_seq = 0;
	*release_mc = 0;
	*hot_mc = 0;
	*critical_mc = 0;
	*limits_valid = false;
	if (!cooling->pmp_node) {
		u64 seq, zone_ns;
		long zone_mc, rel, hot, crit;
		bool valid;
		int zone_err;

		atomic_set(&cooling->zone_active, !idle);
		spin_lock(&cooling->zone_lock);
		seq = cooling->zone_seq;
		zone_ns = cooling->zone_ns;
		zone_mc = cooling->zone_mc;
		rel = cooling->zone_release_mc;
		hot = cooling->zone_hot_mc;
		crit = cooling->zone_critical_mc;
		valid = cooling->zone_limits_valid;
		zone_err = cooling->zone_err;
		spin_unlock(&cooling->zone_lock);

		*zone_seq = seq;
		if (!zone_ns)
			ret = -ENODATA;
		else if (zone_err)
			ret = zone_err;
		else if (now > zone_ns && now - zone_ns > ASAHI_G16_ZONE_STALE_NS)
			ret = -ETIMEDOUT;
		else {
			*temperature = zone_mc;
			*release_mc = rel;
			*hot_mc = hot;
			*critical_mc = crit;
			*limits_valid = valid;
			ret = 0;
		}
		*governor = atomic_read(&cooling->state);
		*power = 0;
		*backstop = 8;
		return ret;
	}

	ret = apple_pmp_telemetry_get_hotspot(cooling->pmp_node, temperature);
	if (!ret) {
		cooling->valid_ns = now;
		cooling->valid_mc = *temperature;
	} else if (ret == -ENODATA &&
		   apple_pmp_hold_nodata(cooling->valid_ns, cooling->valid_mc,
					 now, idle)) {
		*temperature = cooling->valid_mc;
		/* Idle makes no heat: the 2 s with work in flight start with the work. */
		if (idle && cooling->valid_mc < APPLE_PMP_GATED_HOLD_MC)
			cooling->valid_ns = now;
		ret = 0;
	}
	apple_pmp_cooling_sample(&cooling->policy, now, *temperature, ret);
	*governor = atomic_read(&cooling->state);
	*power = apple_pmp_gpu_power_state(cooling->pmp);
	apple_pmp_cooling_observe(&cooling->policy, now, *governor);
	*backstop = apple_pmp_cooling_level(&cooling->policy);
	return ret;
}

/*
 * genpd never turned the GFX domain's TVM gate off, so a sleep that lost it goes
 * unnoticed: the domain then doesn't come on. Enabled here if it's found clear.
 */
int asahi_g16_tvm_enable(struct device *dev);
int asahi_g16_tvm_enable(struct device *dev)
{
	struct device_node *gfx, *tvm;
	void __iomem *reg;
	u32 val;
	int ret = 0;

	gfx = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!gfx)
		return -EINVAL;
	tvm = of_parse_phandle(gfx, "power-domains", 0);
	of_node_put(gfx);
	if (!tvm)
		return -EINVAL;
	reg = of_iomap(tvm, 0);
	of_node_put(tvm);
	if (!reg)
		return -ENOMEM;
	val = readl(reg);
	dev_dbg(dev, "GFX TVM gate after the sleep: %#x\n", val);
	if (!(val & BIT(0))) {
		writel(val | BIT(0), reg);
		ret = readl_poll_timeout(reg, val, val & BIT(0), 1, 1000);
	}
	iounmap(reg);

	return ret;
}

/* Whether this T8132 board's retained PMP carries the GPU's AGX vote. */
bool asahi_g16_board(void)
{
	return apple_pmp_t8132_retained_board();
}

/*
 * The system counter frequency the boot loader programmed, in Hz: the clock the
 * ADT counts GPU power sample periods in.
 */
u32 asahi_g16_timebase_hz(void)
{
	return arch_timer_get_cntfrq();
}

/*
 * The first firmware start follows the native display the boot loader handed
 * over, as on J713 bring-up: defer until drm/apple has published it. A boot
 * without one (no internal panel, or none qualified) starts on the boot
 * framebuffer.
 */
int asahi_g16_display_prepare(struct device *dev, const char *display_compat)
{
	struct device_node *np;
	bool handed_over;
	int ret;

	if (!display_compat || !*display_compat)
		return -EINVAL;
	np = of_find_compatible_node(NULL, NULL, display_compat);
	handed_over = np && of_device_is_available(np);
	if (!handed_over) {
		of_node_put(np);
		dev_info_once(dev, "G16: no native display this boot; starting on the boot framebuffer\n");
		return 0;
	}
	ret = apple_drm_link_device(dev, np);
	of_node_put(np);
	if (ret == -EPROBE_DEFER)
		dev_info_once(dev, "G16: waiting for %s to register\n", display_compat);
	return ret;
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

/*
 * The CLPC holding the memory floor for GPU work is a performance aid: the GPU
 * works without one, only slower. A kernel without the CLPC runs without it;
 * one that has not probed yet defers the GPU, with the reason recorded.
 */
int asahi_g16_clpc_gpu_get(struct device *dev, struct apple_clpc_gpu **out)
{
	int ret = apple_clpc_gpu_get(dev, out);

	if (ret == -ENODEV) {
		dev_warn(dev, "no CLPC: GPU work gets no memory floor\n");
		*out = NULL;
		return 0;
	}
	if (ret)
		return dev_err_probe(dev, ret, "no CLPC for GPU work\n");
	return 0;
}

void asahi_g16_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active)
{
	apple_clpc_gpu_set_active(gpu, active);
}

void asahi_g16_clpc_gpu_put(struct apple_clpc_gpu *gpu)
{
	apple_clpc_gpu_put(gpu);
}
