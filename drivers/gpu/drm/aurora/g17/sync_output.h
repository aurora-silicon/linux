/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AURORA_G17_SYNC_OUTPUT_H
#define AURORA_G17_SYNC_OUTPUT_H
#include <linux/types.h>

struct dma_fence;
struct drm_syncobj;
struct aurora_g17_signal_bundle;
struct aurora_g17_signal_spec {
	struct drm_syncobj *syncobj;
	u64 point;
	u32 timeline;
};

struct aurora_g17_signal_bundle *
aurora_g17_signals_new(const struct aurora_g17_signal_spec *specs,
		     size_t count, size_t anchor, int *error);
void aurora_g17_signals_free(struct aurora_g17_signal_bundle *bundle);
int aurora_g17_signals_publish(struct aurora_g17_signal_bundle *bundle,
			      struct dma_fence *submitted);
void aurora_g17_signals_fail(struct aurora_g17_signal_bundle *bundle,
			    struct dma_fence *failure);
#endif
