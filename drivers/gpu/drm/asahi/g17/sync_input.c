// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include "sync_input.h"
#include "sync_scope.h"

static int collect_leaf(struct dma_fence *leaf,
			struct asahi_g17_fence_leaf *out, size_t capacity,
			size_t *count, bool coalesce_contexts)
{
	int status = dma_fence_get_status(leaf);
	size_t i;

	/* Already-signalled non-error leaves need no further wait or barrier. */
	if (status > 0)
		return 0;

	for (i = 0; i < *count; ++i) {
		if (out[i].fence == leaf) {
			if (status < 0 && out[i].status >= 0)
				out[i].status = status;
			return 0;
		}
		/*
		 * Coalesce healthy unsignaled leaves sharing a fence context (such
		 * as multiple points of one timeline chain), keeping only the
		 * latest seqno. Failed leaves are never coalesced, so errors are
		 * preserved, and neither is context 0, which names no timeline.
		 */
		if (coalesce_contexts && status == 0 && leaf->context != 0 &&
		    out[i].status == 0 && out[i].fence->context == leaf->context) {
			if (dma_fence_is_later(leaf, out[i].fence)) {
				dma_fence_put(out[i].fence);
				out[i].fence = dma_fence_get(leaf);
			}
			return 0;
		}
	}

	if (*count == capacity)
		return -ENOSPC;
	out[*count].fence = dma_fence_get(leaf);
	out[*count].status = status;
	++*count;
	return 0;
}

/* Called without the device mutex: status polling may run fence callbacks. */
static int collect_fences(struct dma_fence *head,
			  struct asahi_g17_fence_leaf *out, size_t capacity,
			  bool coalesce_contexts)
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
			unsigned int n, i;
			int status = dma_fence_get_status(node);

			if (status > 0) {
				dma_fence_put(node);
				continue;
			}
			n = asahi_g17_scope_children(node, children);
			if (!n) {
				/* Preserve a failed scope as a failed leaf, never drop its error. */
				if (status < 0)
					error = collect_leaf(node, out, capacity, &count,
							     coalesce_contexts);
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
				error = collect_leaf(leaf, out, capacity, &count,
						     coalesce_contexts);
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

int asahi_g17_collect_fences(struct dma_fence *head,
			   struct asahi_g17_fence_leaf *out, size_t capacity)
{
	return collect_fences(head, out, capacity, false);
}

int asahi_g17_collect_fences_coalesced(struct dma_fence *head,
				     struct asahi_g17_fence_leaf *out,
				     size_t capacity)
{
	return collect_fences(head, out, capacity, true);
}

void asahi_g17_put_fence(struct dma_fence *fence)
{
	dma_fence_put(fence);
}

/*
 * One fence that signals once every one of @fences has: already-signalled
 * non-error members are skipped, small inputs deduplicate identical pointers
 * and same-context members (keeping the latest seqno), already-signalled
 * error status is preserved, and 2+ remaining fences join an inline-drained
 * asahi_g17_barrier rather than a sorted dma_fence_array. The fences stay
 * borrowed; returns a new reference, or NULL without memory.
 */
struct dma_fence *asahi_g17_merge_fences(struct dma_fence *const *fences, size_t count)
{
	enum { INLINE_CAP = 64, DEDUP_LIMIT = 16 };
	struct dma_fence *stack_buf[INLINE_CAP];
	struct dma_fence **unsignaled = stack_buf;
	struct dma_fence *first_err = NULL;
	struct dma_fence *merged;
	size_t i, j, n = 0;

	if (count > INLINE_CAP) {
		unsignaled = kmalloc_array(count, sizeof(*unsignaled), GFP_KERNEL);
		if (!unsignaled)
			return NULL;
	}
	for (i = 0; i < count; ++i) {
		struct dma_fence *f = fences[i];
		int status;
		bool dup = false;

		if (!f)
			continue;
		status = dma_fence_get_status(f);
		if (status > 0)
			continue;
		if (status < 0) {
			if (!first_err)
				first_err = f;
			continue;
		}
		if (n <= DEDUP_LIMIT) {
			for (j = 0; j < n; ++j) {
				if (unsignaled[j] == f) {
					dup = true;
					break;
				}
				if (f->context != 0 && unsignaled[j]->context == f->context) {
					if (dma_fence_is_later(f, unsignaled[j]))
						unsignaled[j] = f;
					dup = true;
					break;
				}
			}
		} else if (unsignaled[n - 1] == f) {
			dup = true;
		}
		if (!dup)
			unsignaled[n++] = f;
	}
	if (first_err)
		unsignaled[n++] = first_err;
	merged = asahi_g17_barrier_create(unsignaled, n);
	if (unsignaled != stack_buf)
		kfree(unsignaled);
	return merged;
}

/*
 * How @a orders against @b: 1 when it signals after it on their shared
 * context, 0 when not, -1 when they are on different contexts, which
 * dma_fence_is_later() would warn about.
 */
int asahi_g17_fence_order(struct dma_fence *a, struct dma_fence *b)
{
	if (a->context != b->context)
		return -1;
	return dma_fence_is_later(a, b);
}
