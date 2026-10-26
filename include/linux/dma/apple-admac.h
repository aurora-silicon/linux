/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_DMA_APPLE_ADMAC_H
#define __LINUX_DMA_APPLE_ADMAC_H

#include <linux/dmaengine.h>
#include <linux/errno.h>
#include <linux/kconfig.h>

/*
 * Optional, in-path playback processing. Install in process context after
 * terminate_sync(), before preparing a cyclic transaction. The ADMAC owns
 * private coherent copies; userspace never maps these copies. Callbacks run
 * under the channel's IRQ-safe lock and may not sleep. On a process failure,
 * no ticket may have been reserved. The context outlives detach/synchronize.
 */
struct apple_admac_playback_filter {
	const void *source;
	dma_addr_t source_dma;
	size_t buffer_bytes;
	size_t period_bytes;
	void *context;
	int (*process)(void *context, void *samples, size_t bytes, u64 *ticket);
	int (*retire)(void *context, u64 ticket);
	void (*fault)(void *context);
};

#if IS_REACHABLE(CONFIG_APPLE_ADMAC)
int apple_admac_set_playback_filter(struct dma_chan *chan,
			const struct apple_admac_playback_filter *filter);
#else
static inline int apple_admac_set_playback_filter(struct dma_chan *chan,
			const struct apple_admac_playback_filter *filter)
{
	return -EOPNOTSUPP;
}
#endif

#endif
