/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_PM_H_
#define AVD_PM_H_

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include "avd-queue.h"

#define AVD_PERF_FRAME_PIXELS		2200000ULL
#define AVD_PERF_WINDOW_MS		250ULL
#define AVD_PERF_ENTER_PX_PER_MS	180000ULL
#define AVD_PERF_EXIT_PX_PER_MS		135000ULL
#define AVD_PERF_HOLD_MS		500ULL
/*
 * Tier-1 backends expose 15 FIFO slots (7 on tier-0) and single-stream 1080p
 * players often decode-ahead 2-3 frames; engage on >= 4 frames in flight.
 */
#define AVD_PERF_INFLIGHT_HIGH		4U

struct avd_dma_release;

struct avd_pm_perf_ops {
	int (*get)(struct avd_dev *avd);
	void (*put)(struct avd_dev *avd, bool sync);
	void (*suspend)(struct avd_dev *avd);
};

struct avd_queue_pm_ops {
	/* Snapshot under the queue lock: all FREE, no context draining. */
	bool (*idle)(struct avd_dev *avd);
	/* Scheduling is paused; finish/retire all accepted frames. May sleep. */
	int (*drain)(struct avd_dev *avd);
	/* Kick prepared RESERVED entries only, respecting dependencies. */
	void (*resubmit)(struct avd_dev *avd);
};

struct avd_deferred_unmap {
	struct list_head node;
	void (*unmap)(struct avd_dev *avd, struct avd_deferred_unmap *unmap);
};

struct avd_pm {
	/* Serializes DMA releases against DART activation and firmware boot. */
	struct mutex lock;
	struct list_head deferred_unmaps;
	bool dart_active;
	/* Empty DART domain remains attached while resting or retired. */
	bool dma_parked;
	int irq;
	bool irq_enabled;
	/*
	 * hw_lock guards hw_on and irq_gen. The threaded handler touches the
	 * decoder only under hw_lock with hw_on set and an unchanged irq_gen,
	 * so turning the IRQ off never has to wait for the handler thread
	 * (which may itself be waiting for queue_mutex).
	 */
	/* A mutex: decoder MMIO is never done with interrupts off. */
	struct mutex hw_lock;
	bool hw_on;
	unsigned int irq_gen;
	void __iomem *piodma;
	phys_addr_t sram_phys;
	wait_queue_head_t idle_wait;
	/* Protected by queue_lock; counts drain operations, not stopped streams. */
	unsigned int draining;
	const struct avd_queue_pm_ops *queue;
	/* Optional "perf" floor domain, engaged only under high decode demand. */
	struct device *perf_dev;
	const struct avd_pm_perf_ops *perf_ops;
	/* Serializes perf-floor demand state and votes. */
	struct mutex perf_lock;
	bool perf_held;
	bool perf_rate_high;
	bool perf_window_valid;
	bool perf_window_multi;
	const void *perf_window_stream;
	u64 perf_window_start_ns;
	u64 perf_window_pixels;
	u64 perf_hold_until_ns;
	u64 (*clock_ns)(void);
};

/* The caller holds the queue lock, including the draining-context count. */
bool avd_pm_can_suspend(const struct avd_frame *frames, unsigned int draining);

void avd_pm_init(struct avd_dev *avd, const struct avd_backend_ops *backend,
		 const struct avd_queue_pm_ops *queue);
int avd_pm_attach_perf(struct avd_dev *avd, unsigned int index);
void avd_pm_perf_frame_start(struct avd_dev *avd, const void *stream_id,
			     u32 width, u32 height, unsigned int inflight,
			     unsigned int concurrent_streams);
void avd_pm_perf_idle(struct avd_dev *avd);
void avd_pm_perf_suspend(struct avd_dev *avd);
void avd_pm_enable(struct avd_dev *avd);
int avd_pm_map_piodma(struct avd_dev *avd);
void avd_pm_prepare_piodma(struct avd_dev *avd);
/* After queue cleanup: release deferred DMA mappings before the final off. */
void avd_pm_disable(struct avd_dev *avd);
/* Called in process context, after the last frame has retired its mapping. */
void avd_pm_unmap(struct avd_dev *avd, struct avd_deferred_unmap *unmap);
int avd_pm_power_on(struct avd_dev *avd);
int avd_pm_power_off(struct avd_dev *avd);
/* Caller holds a PM reference and queue_mutex; leaves the host IRQ closed. */
int avd_pm_recover(struct avd_dev *avd);
/* Stop firmware and leave DMA blocked; same caller/IRQ contract as recover. */
int avd_pm_park(struct avd_dev *avd);
/* After recovery has invalidated every previously firmware-owned slot. */
void avd_pm_enable_irq(struct avd_dev *avd);
irqreturn_t avd_pm_irq(struct avd_dev *avd, unsigned int gen);
void avd_pm_drain_begin(struct avd_dev *avd);
void avd_pm_drain_end(struct avd_dev *avd);
void avd_pm_queue_changed(struct avd_dev *avd);
extern const struct avd_queue_pm_ops avd_pm_queue_ops;

/* Backends may share these when suspend loses all firmware context. */
int avd_pm_backend_suspend(struct avd_dev *avd);
int avd_pm_backend_resume(struct avd_dev *avd);
extern const struct dev_pm_ops avd_pm_ops;

#endif /* AVD_PM_H_ */
