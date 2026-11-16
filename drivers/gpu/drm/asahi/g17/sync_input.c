// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include "sync_input.h"
#include "sync_scope.h"

static int collect_leaf(struct dma_fence *leaf,
			struct asahi_g17_fence_leaf *out, size_t capacity,
			size_t *count)
{
	if (*count == capacity)
		return -ENOSPC;
	out[*count].fence = dma_fence_get(leaf);
	out[*count].status = dma_fence_get_status(leaf);
	++*count;
	return 0;
}

/* Called without the device mutex: status polling may run fence callbacks. */
int asahi_g17_collect_fences(struct dma_fence *head,
			   struct asahi_g17_fence_leaf *out, size_t capacity)
{
	struct dma_fence *pending[128];
	size_t count = 0, depth = 1;
	int error = 0;

	pending[0] = dma_fence_get(head);
	while (depth && !error) {
		struct dma_fence *node = pending[--depth];
		struct dma_fence_unwrap cursor;
		struct dma_fence *leaf;

		if (asahi_g17_is_scope(node)) {
			struct dma_fence *children[2];
			unsigned int n = asahi_g17_scope_children(node, children), i;
			if (!n) {
				/* Preserve a failed scope as a failed leaf, never drop its error. */
				if (dma_fence_get_status(node) < 0)
					error = collect_leaf(node, out, capacity, &count);
			} else if (depth + n > ARRAY_SIZE(pending)) {
				for (i = 0; i < n; ++i)
					dma_fence_put(children[i]);
				error = -ENOSPC;
			} else {
				for (i = 0; i < n; ++i)
					pending[depth++] = children[i];
			}
			dma_fence_put(node);
			continue;
		}

		dma_fence_unwrap_for_each(leaf, &cursor, node) {
			if (asahi_g17_is_scope(leaf)) {
				if (depth == ARRAY_SIZE(pending))
					error = -ENOSPC;
				else
					pending[depth++] = dma_fence_get(leaf);
			} else if (dma_fence_is_container(leaf)) {
				error = -EOPNOTSUPP;
			} else {
				error = collect_leaf(leaf, out, capacity, &count);
			}
			if (error)
				break;
		}
		/* A bounded early exit still owns the iterator's current chain. */
		dma_fence_put(cursor.chain);
		dma_fence_put(node);
	}
	while (depth)
		dma_fence_put(pending[--depth]);
	if (error) {
		while (count)
			dma_fence_put(out[--count].fence);
		return error;
	}
	return count;
}

void asahi_g17_put_fence(struct dma_fence *fence)
{
	dma_fence_put(fence);
}

/*
 * One fence that signals once every one of @fences has: unwrapped, already
 * signalled members dropped and same-context members deduplicated. The
 * fences stay borrowed; returns a new reference, or NULL without memory.
 */
struct dma_fence *asahi_g17_merge_fences(struct dma_fence *const *fences, size_t count)
{
	struct dma_fence_unwrap *cursors;
	struct dma_fence *merged;

	cursors = kmalloc_array(count, sizeof(*cursors), GFP_KERNEL);
	if (!cursors)
		return NULL;
	merged = __dma_fence_unwrap_merge(count, (struct dma_fence **)fences, cursors);
	kfree(cursors);
	return merged;
}

/*
 * Measurement only: how a compute input from the consumer's own logical queue
 * is handled. 0: the general firmware-source path (the driver before the
 * own-queue change); 1: no lookup in the submit ioctl, found on the owner's
 * queue first at publication, with the dependency, loan and consumer mark
 * (the change); 2: as 1, but a source found there gets no dependency, loan or
 * consumer mark (the earlier ring-order form). Read per source, so arms may
 * alternate on one boot.
 */
static unsigned int gpubench_local_inputs;
module_param(gpubench_local_inputs, uint, 0644);
MODULE_PARM_DESC(gpubench_local_inputs, "Measurement: own-queue compute inputs (0 general, 1 own queue first, 2 ring order)");

unsigned int asahi_g17_local_inputs(void)
{
	return READ_ONCE(gpubench_local_inputs);
}

/*
 * Measurement only: M5 render submission changes, one bit each (see
 * sync_input.rs). Each bit is read where its change applies, so masks may
 * alternate on one boot; 0 is the driver without them.
 */
static unsigned int gpubench_render_submit;
module_param(gpubench_render_submit, uint, 0644);
MODULE_PARM_DESC(gpubench_render_submit, "Measurement: M5 render submission changes (bit mask, 0 none)");

unsigned int asahi_g17_render_submit(void)
{
	return READ_ONCE(gpubench_render_submit);
}

/* Measurement only: 0 is the held host-port-first path; 1 restores the
 * doorbell-first, unheld path. Change only between idle benchmark clients.
 */
static unsigned int gpubench_compute_kick;
module_param(gpubench_compute_kick, uint, 0644);
MODULE_PARM_DESC(gpubench_compute_kick, "Measurement: compute kick (0 held host-port first, 1 old doorbell first)");

unsigned int asahi_g17_compute_kick_mode(void)
{
	return READ_ONCE(gpubench_compute_kick);
}
