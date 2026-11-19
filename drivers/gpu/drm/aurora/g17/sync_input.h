/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AURORA_G17_SYNC_INPUT_H
#define AURORA_G17_SYNC_INPUT_H

#include <linux/types.h>

struct dma_fence;

struct aurora_g17_fence_leaf {
	struct dma_fence *fence;
	int status;
};

int aurora_g17_collect_fences(struct dma_fence *head,
			   struct aurora_g17_fence_leaf *out, size_t capacity);
void aurora_g17_put_fence(struct dma_fence *fence);
struct dma_fence *aurora_g17_merge_fences(struct dma_fence *const *fences, size_t count);

#endif
