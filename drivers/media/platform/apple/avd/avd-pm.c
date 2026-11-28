// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "avd.h"

/* [C] Keep Linux's existing default until the idle-timer capture is available. */
#define AVD_AUTOSUSPEND_MS 100
#define AVD_PERF_AUTOSUSPEND_MS 100
#define AVD_PERF_WINDOW_NS (AVD_PERF_WINDOW_MS * NSEC_PER_MSEC)
#define AVD_PERF_WINDOW_ENTER_PIXELS \
	(AVD_PERF_ENTER_PX_PER_MS * AVD_PERF_WINDOW_MS)
#define AVD_PERF_HOLD_NS (AVD_PERF_HOLD_MS * NSEC_PER_MSEC)
#define AVD_DRAIN_TIMEOUT_MS 5000
#define AVD_PIODMA_SRAM_BASE 0x24

void avd_pm_enable_irq(struct avd_dev *avd)
{
	if (avd->pm.irq >= 0 && !avd->pm.irq_enabled) {
		mutex_lock(&avd->pm.hw_lock);
		avd->pm.hw_on = true;
		mutex_unlock(&avd->pm.hw_lock);
		enable_irq(avd->pm.irq);
		avd->pm.irq_enabled = true;
	}
}

/*
 * Callers may hold queue_mutex, which the threaded handler takes first, so
 * never wait for the handler here (disable_irq() would deadlock). Instead
 * close hardware access under hw_lock: a handler already touching the
 * decoder finishes before this returns, and any later one sees hw_on clear
 * or a new irq_gen and leaves the hardware alone.
 */
static void avd_pm_disable_irq(struct avd_dev *avd)
{
	if (avd->pm.irq >= 0 && avd->pm.irq_enabled) {
		mutex_lock(&avd->pm.hw_lock);
		avd->pm.hw_on = false;
		avd->pm.irq_gen++;
		mutex_unlock(&avd->pm.hw_lock);
		disable_irq_nosync(avd->pm.irq);
		avd->pm.irq_enabled = false;
	}
}

/* Run the backend's interrupt handler only against a live firmware instance. */
irqreturn_t avd_pm_irq(struct avd_dev *avd, unsigned int gen)
{
	irqreturn_t ret = IRQ_HANDLED;

	/* Caller holds queue_mutex; hw_lock holders never wait for it. */
	mutex_lock(&avd->pm.hw_lock);
	if (avd->pm.hw_on && gen == avd->pm.irq_gen)
		ret = avd->backend->irq(avd);
	mutex_unlock(&avd->pm.hw_lock);
	return ret;
}

struct avd_dma_release {
	struct avd_deferred_unmap unmap;
	void *cpu;
	dma_addr_t addr;
	size_t size;
};

static void avd_dma_unmap(struct avd_dev *avd,
			  struct avd_deferred_unmap *unmap)
{
	struct avd_dma_release *release;

	release = container_of(unmap, struct avd_dma_release, unmap);
	dma_free_coherent(avd->dev, release->size, release->cpu, release->addr);
	kfree(release);
}

int avd_buf_alloc(struct avd_dev *avd, struct avd_buf *buf, size_t size)
{
	struct avd_dma_release *release;

	avd_buf_free(avd, buf);
	if (!size)
		return -ENOMEM;

	/* Allocate the release record now: freeing must never need to allocate. */
	release = kzalloc_obj(*release);
	if (!release)
		return -ENOMEM;
	INIT_LIST_HEAD(&release->unmap.node);
	release->unmap.unmap = avd_dma_unmap;
	release->size = size;
	release->cpu = dma_alloc_coherent(avd->dev, size, &release->addr, GFP_KERNEL);
	if (!release->cpu) {
		kfree(release);
		return -ENOMEM;
	}

	/* Payload length, not an allocation rounding, is used by codec packers. */
	buf->cpu = release->cpu;
	buf->addr = release->addr;
	buf->size = size;
	buf->release = release;
	return 0;
}

void avd_buf_free(struct avd_dev *avd, struct avd_buf *buf)
{
	if (buf->release)
		avd_pm_unmap(avd, &buf->release->unmap);
	/* The record outlives the context when unmapping has to be deferred. */
	memset(buf, 0, sizeof(*buf));
}

bool avd_pm_can_suspend(const struct avd_frame *frames, unsigned int draining)
{
	unsigned int i;

	if (draining)
		return false;

	for (i = 0; i < AVD_MAX_FRAMES; i++)
		if (frames[i].state != AVD_FRAME_FREE)
			return false;

	return true;
}

static bool avd_pm_queue_idle(struct avd_dev *avd)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&avd->queue_lock, flags);
	idle = avd_pm_can_suspend(avd->frames, avd->pm.draining);
	spin_unlock_irqrestore(&avd->queue_lock, flags);
	return idle;
}

void avd_pm_queue_changed(struct avd_dev *avd)
{
	wake_up_all(&avd->pm.idle_wait);
}

void avd_pm_drain_begin(struct avd_dev *avd)
{
	unsigned long flags;

	spin_lock_irqsave(&avd->queue_lock, flags);
	avd->pm.draining++;
	spin_unlock_irqrestore(&avd->queue_lock, flags);
}

void avd_pm_drain_end(struct avd_dev *avd)
{
	unsigned long flags;

	spin_lock_irqsave(&avd->queue_lock, flags);
	avd->pm.draining--;
	spin_unlock_irqrestore(&avd->queue_lock, flags);
	avd_pm_queue_changed(avd);
}

static void avd_pm_resubmit(struct avd_dev *avd)
{
	/* queue_mutex may be held by the caller of pm_runtime_resume_and_get(). */
	mod_delayed_work(system_dfl_wq, &avd->queue_work, 0);
}

static int avd_pm_queue_drain(struct avd_dev *avd)
{
	avd_pm_resubmit(avd);
	if (!wait_event_timeout(avd->pm.idle_wait, avd_pm_queue_idle(avd),
				msecs_to_jiffies(AVD_DRAIN_TIMEOUT_MS)))
		return -EBUSY;
	return 0;
}

const struct avd_queue_pm_ops avd_pm_queue_ops = {
	.idle = avd_pm_queue_idle,
	.drain = avd_pm_queue_drain,
	.resubmit = avd_pm_resubmit,
};

static int avd_pm_perf_get_pd(struct avd_dev *avd)
{
	return pm_runtime_resume_and_get(avd->pm.perf_dev);
}

static void avd_pm_perf_put_pd(struct avd_dev *avd, bool sync)
{
	if (sync) {
		pm_runtime_put_sync_suspend(avd->pm.perf_dev);
		return;
	}
	pm_runtime_mark_last_busy(avd->pm.perf_dev);
	pm_runtime_put_autosuspend(avd->pm.perf_dev);
}

static void avd_pm_perf_suspend_pd(struct avd_dev *avd)
{
	pm_runtime_suspend(avd->pm.perf_dev);
}

static const struct avd_pm_perf_ops avd_pm_perf_runtime_ops = {
	.get = avd_pm_perf_get_pd,
	.put = avd_pm_perf_put_pd,
	.suspend = avd_pm_perf_suspend_pd,
};

static u64 avd_pm_clock_ns(struct avd_dev *avd)
{
	return avd->pm.clock_ns ? avd->pm.clock_ns() : ktime_get_ns();
}

static void avd_pm_perf_reset_demand(struct avd_pm *pm)
{
	pm->perf_rate_high = false;
	pm->perf_window_valid = false;
	pm->perf_window_multi = false;
	pm->perf_window_stream = NULL;
	pm->perf_window_start_ns = 0;
	pm->perf_window_pixels = 0;
	pm->perf_hold_until_ns = 0;
}

static void avd_pm_detach_perf(void *data)
{
	struct avd_dev *avd = data;
	struct device *perf_dev = avd->pm.perf_dev;

	if (!perf_dev)
		return;

	avd_pm_perf_suspend(avd);
	mutex_lock(&avd->pm.perf_lock);
	WRITE_ONCE(avd->pm.perf_ops, NULL);
	avd->pm.perf_dev = NULL;
	mutex_unlock(&avd->pm.perf_lock);
	pm_runtime_dont_use_autosuspend(perf_dev);
	dev_pm_domain_detach(perf_dev, false);
}

int avd_pm_attach_perf(struct avd_dev *avd, unsigned int index)
{
	struct device *perf_dev;
	int ret;

	perf_dev = dev_pm_domain_attach_by_id(avd->dev, index);
	if (IS_ERR(perf_dev))
		return PTR_ERR(perf_dev);
	if (!perf_dev)
		return 0;

	pm_runtime_set_autosuspend_delay(perf_dev, AVD_PERF_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(perf_dev);

	mutex_lock(&avd->pm.perf_lock);
	avd->pm.perf_dev = perf_dev;
	WRITE_ONCE(avd->pm.perf_ops, &avd_pm_perf_runtime_ops);
	mutex_unlock(&avd->pm.perf_lock);

	ret = devm_add_action_or_reset(avd->dev, avd_pm_detach_perf, avd);
	if (ret)
		return ret;

	return 0;
}

/*
 * Engage the optional "perf" floor only when active decode demand requires it:
 * frames larger than 1080p, multiple concurrent active streams, queue backlog,
 * or sustained high pixel throughput with Schmitt-trigger hysteresis and a
 * hold-down window to prevent floor thrashing.
 */
void avd_pm_perf_frame_start(struct avd_dev *avd, const void *stream_id,
			     u32 width, u32 height, unsigned int inflight,
			     unsigned int concurrent_streams)
{
	const struct avd_pm_perf_ops *ops;
	struct avd_pm *pm = &avd->pm;
	u64 frame_pixels = (u64)width * height;
	u64 now_ns;
	bool high_demand, need_perf;

	if (!READ_ONCE(pm->perf_ops))
		return;

	mutex_lock(&pm->perf_lock);
	ops = pm->perf_ops;
	if (!ops)
		goto unlock;

	now_ns = avd_pm_clock_ns(avd);

	if (!pm->perf_window_valid || now_ns < pm->perf_window_start_ns) {
		pm->perf_window_valid = true;
		pm->perf_window_start_ns = now_ns;
		pm->perf_window_pixels = frame_pixels;
		pm->perf_window_stream = stream_id;
		pm->perf_window_multi = false;
		pm->perf_rate_high = false;
		pm->perf_hold_until_ns = 0;
	} else {
		u64 dt_ns = now_ns - pm->perf_window_start_ns;

		if (dt_ns >= 2 * AVD_PERF_WINDOW_NS) {
			pm->perf_rate_high = false;
			pm->perf_window_start_ns = now_ns;
			pm->perf_window_pixels = frame_pixels;
			pm->perf_window_stream = stream_id;
			pm->perf_window_multi = false;
		} else if (dt_ns >= AVD_PERF_WINDOW_NS) {
			u64 dt_ms = div_u64(dt_ns, NSEC_PER_MSEC);
			u64 px_per_ms = div_u64(pm->perf_window_pixels, dt_ms);

			if (px_per_ms >= AVD_PERF_ENTER_PX_PER_MS)
				pm->perf_rate_high = true;
			else if (px_per_ms < AVD_PERF_EXIT_PX_PER_MS)
				pm->perf_rate_high = false;

			pm->perf_window_start_ns = now_ns;
			pm->perf_window_pixels = frame_pixels;
			pm->perf_window_stream = stream_id;
			pm->perf_window_multi = false;
		} else {
			pm->perf_window_pixels += frame_pixels;
			if (pm->perf_window_stream && stream_id &&
			    pm->perf_window_stream != stream_id)
				pm->perf_window_multi = true;
			else if (!pm->perf_window_stream)
				pm->perf_window_stream = stream_id;
			if (pm->perf_window_pixels >= AVD_PERF_WINDOW_ENTER_PIXELS)
				pm->perf_rate_high = true;
		}
	}

	high_demand = frame_pixels > AVD_PERF_FRAME_PIXELS ||
		      concurrent_streams > 1 ||
		      pm->perf_window_multi ||
		      inflight >= AVD_PERF_INFLIGHT_HIGH ||
		      pm->perf_rate_high;

	if (high_demand)
		pm->perf_hold_until_ns = now_ns + AVD_PERF_HOLD_NS;

	need_perf = high_demand ||
		    (pm->perf_hold_until_ns && now_ns < pm->perf_hold_until_ns);

	if (need_perf) {
		/* Best effort: decode still completes without the floor. */
		if (!pm->perf_held && ops->get(avd) >= 0)
			pm->perf_held = true;
	} else {
		pm->perf_hold_until_ns = 0;
		if (pm->perf_held) {
			pm->perf_held = false;
			ops->put(avd, false);
		}
	}

unlock:
	mutex_unlock(&pm->perf_lock);
}

void avd_pm_perf_idle(struct avd_dev *avd)
{
	const struct avd_pm_perf_ops *ops;

	if (!READ_ONCE(avd->pm.perf_ops))
		return;

	mutex_lock(&avd->pm.perf_lock);
	ops = avd->pm.perf_ops;
	if (ops && avd->pm.perf_held) {
		avd->pm.perf_held = false;
		ops->put(avd, false);
	}
	mutex_unlock(&avd->pm.perf_lock);
}

void avd_pm_perf_suspend(struct avd_dev *avd)
{
	const struct avd_pm_perf_ops *ops;

	if (!READ_ONCE(avd->pm.perf_ops))
		return;

	mutex_lock(&avd->pm.perf_lock);
	ops = avd->pm.perf_ops;
	avd_pm_perf_reset_demand(&avd->pm);
	if (ops) {
		if (avd->pm.perf_held) {
			avd->pm.perf_held = false;
			ops->put(avd, true);
		} else if (ops->suspend) {
			ops->suspend(avd);
		}
	}
	mutex_unlock(&avd->pm.perf_lock);
}

void avd_pm_init(struct avd_dev *avd, const struct avd_backend_ops *backend,
		 const struct avd_queue_pm_ops *queue)
{
	mutex_init(&avd->pm.lock);
	mutex_init(&avd->pm.perf_lock);
	INIT_LIST_HEAD(&avd->pm.deferred_unmaps);
	init_waitqueue_head(&avd->pm.idle_wait);
	avd->pm.dart_active = false;
	avd->pm.irq = -1;
	avd->pm.irq_enabled = false;
	mutex_init(&avd->pm.hw_lock);
	avd->pm.hw_on = false;
	avd->pm.irq_gen = 0;
	avd->pm.perf_dev = NULL;
	avd->pm.perf_ops = NULL;
	avd->pm.perf_held = false;
	avd->pm.clock_ns = NULL;
	avd_pm_perf_reset_demand(&avd->pm);
	avd->backend = backend;
	avd->pm.queue = queue;
}

void avd_pm_enable(struct avd_dev *avd)
{
	pm_runtime_set_autosuspend_delay(avd->dev, AVD_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(avd->dev);
	pm_runtime_enable(avd->dev);
}

int avd_pm_map_piodma(struct avd_dev *avd)
{
	struct resource *piodma, *sram;

	piodma = platform_get_resource_byname(avd->pdev, IORESOURCE_MEM, "piodma");
	/* Old DTs support only the legacy transport; never invent a resource. */
	if (!piodma)
		return 0;
	sram = platform_get_resource_byname(avd->pdev, IORESOURCE_MEM, "sram");
	if (!sram || !IS_ALIGNED(sram->start, 16) ||
	    (sram->start >> 4) > U32_MAX ||
	    resource_size(piodma) < AVD_PIODMA_SRAM_BASE + sizeof(u32))
		return -EINVAL;
	avd->pm.piodma = devm_ioremap_resource(avd->dev, piodma);
	if (IS_ERR(avd->pm.piodma))
		return PTR_ERR(avd->pm.piodma);
	avd->pm.sram_phys = sram->start;
	if (of_count_phandle_with_args(avd->dev->of_node, "iommus", "#iommu-cells") < 2)
		dev_warn(avd->dev, "PIODMA transport requires decode and DMA IOMMU streams\n");
	return 0;
}

void avd_pm_prepare_piodma(struct avd_dev *avd)
{
	/* Public CM3 boot ABI: replay the SRAM base after every power loss/reset. */
	if (avd->pm.piodma)
		writel((u32)(avd->pm.sram_phys >> 4),
		       avd->pm.piodma + AVD_PIODMA_SRAM_BASE);
}

void avd_pm_disable(struct avd_dev *avd)
{
	int ret;

	/* Power suppliers before flushing frees queued by cleanup while idle. */
	ret = pm_runtime_resume_and_get(avd->dev);
	if (ret >= 0)
		ret = pm_runtime_put_sync_suspend(avd->dev);
	if (ret < 0)
		dev_warn(avd->dev, "final power transition failed: %d\n", ret);
	pm_runtime_disable(avd->dev);
	/* Also close the host IRQ if the final hardware transition failed. */
	avd_pm_disable_irq(avd);
	avd_pm_perf_suspend(avd);
	/* No queue_mutex is held here: let a woken handler thread finish. */
	if (avd->pm.irq >= 0)
		synchronize_irq(avd->pm.irq);
	pm_runtime_dont_use_autosuspend(avd->dev);
	/* Resume may have kicked the already-stopped queue. */
	cancel_delayed_work_sync(&avd->queue_work);
	WARN_ON(!list_empty(&avd->pm.deferred_unmaps));
}

/* pm.lock held; release records own their storage and may free themselves. */
static void avd_pm_drain_unmaps(struct avd_dev *avd)
{
	struct avd_deferred_unmap *unmap, *next;

	list_for_each_entry_safe(unmap, next, &avd->pm.deferred_unmaps, node) {
		list_del_init(&unmap->node);
		unmap->unmap(avd, unmap);
	}
}

void avd_pm_unmap(struct avd_dev *avd, struct avd_deferred_unmap *unmap)
{
	mutex_lock(&avd->pm.lock);
	if (avd->pm.dart_active)
		unmap->unmap(avd, unmap);
	else
		list_add_tail(&unmap->node, &avd->pm.deferred_unmaps);
	mutex_unlock(&avd->pm.lock);
}

int avd_pm_backend_suspend(struct avd_dev *avd)
{
	avd->backend->shutdown(avd);
	return 0;
}

int avd_pm_backend_resume(struct avd_dev *avd)
{
	/* Includes transport setup, ITCM verification/load and a new generation. */
	return avd_backend_boot(avd);
}

/* Caller holds a runtime PM reference and queue_mutex. */
int avd_pm_park(struct avd_dev *avd)
{
	int ret = 0;

	if (!avd->pm.dart_active) {
		avd_pm_perf_suspend(avd);
		return -EHOSTDOWN;
	}
	avd_pm_disable_irq(avd);
	dev_warn(avd->dev, "recover: stop firmware\n");
	avd->backend->shutdown(avd);
	if (avd->domain && !avd->pm.dma_parked) {
		if (!avd->empty_domain) {
			ret = -ENODEV;
			goto out_perf;
		}
		dev_warn(avd->dev, "recover: block DMA\n");
		ret = iommu_attach_device(avd->empty_domain, avd->dev);
		if (ret) {
			dev_err(avd->dev, "recover: DART empty attach failed: %d\n", ret);
			goto out_perf;
		}
		avd->pm.dma_parked = true;
		/* Outstanding transfers drain into faults before any reset/free. */
		usleep_range(5000, 6000);
	}
out_perf:
	/* Drop the perf floor only after firmware shutdown and DMA parking. */
	avd_pm_perf_suspend(avd);
	return ret;
}

int avd_pm_recover(struct avd_dev *avd)
{
	bool suspended;
	int ret;

	if (!avd->pm.dart_active) {
		dev_err(avd->dev, "recover: decoder DART inactive, not touching it\n");
		return -EHOSTDOWN;
	}
	/* The PM KUnit fixture has no struct device. */
	suspended = avd->dev && pm_runtime_suspended(avd->dev);
	dev_warn(avd->dev, "recover: start (runtime %s, irq %s)\n",
		 suspended ? "suspended" : "active",
		 avd->pm.irq_enabled ? "on" : "off");
	if (WARN_ONCE(suspended,
		      "avd: recovery with the decoder powered off\n"))
		return -EHOSTDOWN;
	ret = avd_pm_park(avd);
	if (ret)
		return ret;
	/*
	 * On the J616C the machine hangs right after this reset, while the
	 * decoder's DART domain is restored. The cause is not known yet, so
	 * stay parked: players fall back to software. Tracked in
	 * https://github.com/aurora-silicon/linux/issues/2450.
	 */
	if (avd->variant && avd->variant->quirks & AVD_QUIRK_NO_RECOVERY_RESET) {
		dev_err(avd->dev, "recover: staying parked, no reset on this SoC\n");
		return -EIO;
	}
	dev_warn(avd->dev, "recover: reset\n");
	ret = reset_control_reset(avd->rstc);
	if (ret) {
		/* A failed reset must never restore the decoder's DMA mappings. */
		dev_err(avd->dev, "recover: reset failed: %d\n", ret);
		return ret;
	}
	if (avd->pm.dma_parked) {
		dev_warn(avd->dev, "recover: restore DMA\n");
		iommu_detach_device(avd->empty_domain, avd->dev);
		if (iommu_get_domain_for_dev(avd->dev) != avd->domain) {
			dev_err(avd->dev, "recover: DART domain not restored\n");
			return -EIO;
		}
		avd->pm.dma_parked = false;
	}
	dev_warn(avd->dev, "recover: boot firmware\n");
	ret = avd_pm_backend_resume(avd);
	if (ret) {
		dev_err(avd->dev, "recover: boot failed: %d\n", ret);
		/* Boot may have started the engine: park it again before retiring. */
		avd_pm_park(avd);
		return ret;
	}
	dev_warn(avd->dev, "recover: done\n");
	return 0;
}

int avd_pm_power_on(struct avd_dev *avd)
{
	const struct avd_backend_ops *backend = avd->backend;
	int ret;

	/* genpd and the IOMMU suppliers resume before this runtime callback. */
	mutex_lock(&avd->pm.lock);
	avd->pm.dart_active = true;
	avd_pm_drain_unmaps(avd);
	mutex_unlock(&avd->pm.lock);

	/* Rest/retirement survives autosuspend: never boot with DMA parked. */
	if (avd->faulted || avd->rest_until || avd->pm.dma_parked)
		return 0;

	/* Do not hold pm.lock: boot may allocate or release DMA buffers. */
	ret = backend->resume ? backend->resume(avd) : avd_pm_backend_resume(avd);
	if (ret) {
		avd->backend->shutdown(avd);
		mutex_lock(&avd->pm.lock);
		avd->pm.dart_active = false;
		mutex_unlock(&avd->pm.lock);
		return ret;
	}
	avd_pm_enable_irq(avd);

	if (avd->pm.queue->resubmit)
		avd->pm.queue->resubmit(avd);

	return 0;
}

int avd_pm_power_off(struct avd_dev *avd)
{
	const struct avd_backend_ops *backend = avd->backend;
	int ret;

	/* PM references cover reservation through retirement, including drain. */
	if (!avd->pm.queue->idle(avd))
		return -EBUSY;

	if (backend->suspend && !avd->pm.dma_parked) {
		ret = backend->suspend(avd);
		if (ret)
			return ret;
	} else {
		backend->shutdown(avd);
	}
	avd_pm_disable_irq(avd);

	/* No more firmware DMA; suppliers stay on until this callback returns. */
	mutex_lock(&avd->pm.lock);
	avd_pm_drain_unmaps(avd);
	avd->pm.dart_active = false;
	mutex_unlock(&avd->pm.lock);
	avd_pm_perf_suspend(avd);
	return 0;
}

static int avd_runtime_resume(struct device *dev)
{
	return avd_pm_power_on(dev_get_drvdata(dev));
}

static int avd_runtime_suspend(struct device *dev)
{
	return avd_pm_power_off(dev_get_drvdata(dev));
}

static int avd_system_suspend(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);
	int ret;

	v4l2_m2m_suspend(avd->m2m_dev);
	ret = avd->pm.queue->drain(avd);
	if (!ret)
		ret = pm_runtime_force_suspend(dev);
	if (!ret)
		avd_pm_perf_suspend(avd);
	if (ret)
		v4l2_m2m_resume(avd->m2m_dev);
	return ret;
}

static int avd_system_resume(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;
	v4l2_m2m_resume(avd->m2m_dev);
	return 0;
}

const struct dev_pm_ops avd_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(avd_system_suspend, avd_system_resume)
	RUNTIME_PM_OPS(avd_runtime_suspend, avd_runtime_resume, NULL)
};
