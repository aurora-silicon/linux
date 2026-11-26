// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/irq_work.h>
#include <linux/irqflags.h>
#include <linux/llist.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include "sync_scope.h"
#include "sync_input.h"

int asahi_g17_completion_fence_status(struct dma_fence *fence)
{
	/* These private fences have no signaled callback. A pending flag is
	 * sufficient to keep the frontier member; a concurrent signal can only
	 * defer pruning until the next submission. Read terminal errors through
	 * the normal locked API after observing the signalled flag.
	 */
	if (!dma_fence_test_signaled_flag(fence))
		return 0;
	return dma_fence_get_status(fence);
}

struct asahi_g17_deferred {
	struct llist_node ready;
	struct irq_work work;
	void (*complete)(struct asahi_g17_deferred *node);
};

struct asahi_g17_scope;
struct asahi_g17_scope_callback {
	struct dma_fence_cb callback;
	struct asahi_g17_scope *scope;
};

struct asahi_g17_scope {
	struct dma_fence base;
	atomic_t pending;
	atomic_t compute_waited;
	bool compute_only;
	struct dma_fence *children[2];
	struct asahi_g17_scope_callback callbacks[2];
	struct asahi_g17_deferred deferred;
	/* Identifies the exact anchor point this scope replaced. */
	u64 anchor_context;
	u64 anchor_point;
};

static const struct dma_fence_ops scope_ops;
static DEFINE_PER_CPU(struct llist_head *, active_scopes);
/* Suppresses internal scope chaining from counting as a host compute waiter. */
static DEFINE_PER_CPU(struct dma_fence *, scope_chaining_fence);
#define SCOPE_ATOMIC_BATCH 64

bool asahi_g17_is_scope(struct dma_fence *fence)
{
	return fence && rcu_access_pointer(fence->ops) == &scope_ops;
}

static struct asahi_g17_scope *to_scope(struct dma_fence *fence)
{
	return container_of(fence, struct asahi_g17_scope, base);
}

static const char *scope_driver_name(struct dma_fence *fence)
{
	return "aurora";
}

static const char *scope_timeline_name(struct dma_fence *fence)
{
	return "g17-queue-scope";
}

static bool scope_signaled(struct dma_fence *fence)
{
	struct asahi_g17_scope *scope = to_scope(fence);
	if (atomic_read_acquire(&scope->pending))
		return false;
	cmpxchg(&fence->error, 1, 0);
	return true;
}

static bool scope_enable_signaling(struct dma_fence *fence)
{
	struct asahi_g17_scope *scope = to_scope(fence);

	/* Child callbacks are already armed; no recursive callback installation. */
	if (scope_signaled(fence))
		return false;
	if (scope->compute_only &&
	    __this_cpu_read(scope_chaining_fence) != fence &&
	    !atomic_xchg(&scope->compute_waited, 1))
		asahi_g17_note_compute_waiter();
	return true;
}

static void scope_unwait(struct asahi_g17_scope *scope)
{
	if (atomic_xchg(&scope->compute_waited, 0))
		asahi_g17_unnote_compute_waiter();
}

unsigned int asahi_g17_scope_children(struct dma_fence *fence,
				     struct dma_fence *children[2])
{
	struct asahi_g17_scope *scope = to_scope(fence);
	unsigned long flags;
	unsigned int i, count = 0;

	spin_lock_irqsave(&fence->inline_lock, flags);
	if (!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		for (i = 0; i < ARRAY_SIZE(scope->children); ++i)
			if (scope->children[i])
				children[count++] = dma_fence_get(scope->children[i]);
	spin_unlock_irqrestore(&fence->inline_lock, flags);
	return count;
}

static void scope_release_completed(struct asahi_g17_scope *scope)
{
	struct dma_fence *children[2];
	unsigned long flags;
	unsigned int i;

	/* Readers take references under this lock. Signalled scopes need no history. */
	spin_lock_irqsave(&scope->base.inline_lock, flags);
	for (i = 0; i < ARRAY_SIZE(children); ++i) {
		children[i] = scope->children[i];
		scope->children[i] = NULL;
	}
	spin_unlock_irqrestore(&scope->base.inline_lock, flags);
	for (i = 0; i < ARRAY_SIZE(children); ++i)
		dma_fence_put(children[i]);
	dma_fence_put(&scope->base);
}

static void scope_complete_node(struct asahi_g17_deferred *d)
{
	struct asahi_g17_scope *scope =
		container_of(d, struct asahi_g17_scope, deferred);
	cmpxchg(&scope->base.error, 1, 0);
	dma_fence_signal(&scope->base);
	scope_unwait(scope);
	scope_release_completed(scope);
}

static void scope_work(struct irq_work *work)
{
	struct asahi_g17_deferred *d =
		container_of(work, struct asahi_g17_deferred, work);
	struct asahi_g17_scope *scope =
		container_of(d, struct asahi_g17_scope, deferred);
	cmpxchg(&scope->base.error, 1, 0);
	asahi_g17_signal_fence(&scope->base);
	scope_unwait(scope);
	scope_release_completed(scope);
}

static void scope_defer_list(struct llist_node *nodes)
{
	struct llist_node *node, *next;
	llist_for_each_safe(node, next, nodes) {
		struct asahi_g17_deferred *d =
			container_of(node, struct asahi_g17_deferred, ready);
		/* Transfer the ready-list reference to the irq_work callback. */
		irq_work_queue(&d->work);
	}
}

void asahi_g17_signal_fence(struct dma_fence *fence)
{
	struct llist_head ready = LLIST_HEAD_INIT(ready);
	struct llist_node *nodes, *node, *next;
	unsigned long flags;
	unsigned int completed = 0;

	/* Fence callbacks are atomic. Keep the local ready list pinned while
	 * signaling, then finish ready scopes/barriers after the source fence is
	 * unlocked. New ready nodes append to this list instead of recursing or
	 * sending IPIs.
	 */
	preempt_disable();
	if (__this_cpu_read(active_scopes)) {
		dma_fence_signal(fence);
		preempt_enable();
		return;
	}
	__this_cpu_write(active_scopes, &ready);
	dma_fence_signal(fence);
	for (;;) {
		/* An interrupt must not append after the final empty test. */
		local_irq_save(flags);
		nodes = llist_del_all(&ready);
		if (!nodes)
			__this_cpu_write(active_scopes, NULL);
		local_irq_restore(flags);
		if (!nodes)
			break;
		llist_for_each_safe(node, next, nodes) {
			struct asahi_g17_deferred *d;
			if (completed == SCOPE_ATOMIC_BATCH) {
				struct llist_node *more;
				local_irq_save(flags);
				more = llist_del_all(&ready);
				__this_cpu_write(active_scopes, NULL);
				local_irq_restore(flags);
				preempt_enable();
				scope_defer_list(node);
				scope_defer_list(more);
				return;
			}
			d = container_of(node, struct asahi_g17_deferred, ready);
			d->complete(d);
			++completed;
		}
	}
	preempt_enable();
}

static void scope_arrived(struct asahi_g17_scope *scope, int error)
{
	if (error < 0)
		cmpxchg(&scope->base.error, 1, error);
	if (atomic_dec_and_test(&scope->pending)) {
		struct llist_head *ready;
		dma_fence_get(&scope->base);
		preempt_disable();
		ready = __this_cpu_read(active_scopes);
		if (ready)
			llist_add(&scope->deferred.ready, ready);
		preempt_enable();
		/* Foreign fence signalers have no driver completion pass to drain us. */
		if (!ready)
			irq_work_queue(&scope->deferred.work);
	}
}

static void scope_callback(struct dma_fence *child, struct dma_fence_cb *callback)
{
	struct asahi_g17_scope_callback *cb =
		container_of(callback, typeof(*cb), callback);
	scope_arrived(cb->scope, child->error);
	dma_fence_put(&cb->scope->base);
}

static void scope_release(struct dma_fence *fence)
{
	struct asahi_g17_scope *scope = to_scope(fence);
	unsigned int i;

	scope_unwait(scope);
	for (i = 0; i < ARRAY_SIZE(scope->children); ++i)
		dma_fence_put(scope->children[i]);
	dma_fence_free(fence);
}

static void scope_set_deadline(struct dma_fence *fence, ktime_t deadline)
{
	struct asahi_g17_fence_leaf *leaves;
	int count, i;

	if (dma_fence_is_signaled(fence))
		return;
	/* A deadline is an advisory hint. Keep its traversal bounded and iterative,
	 * including when a foreign timeline boundary contains earlier scopes.
	 */
	leaves = kmalloc_array(128, sizeof(*leaves), GFP_ATOMIC | __GFP_NOWARN);
	if (!leaves)
		return;
	count = asahi_g17_collect_fences(fence, leaves, 128);
	for (i = 0; i < count; ++i) {
		if (!asahi_g17_is_scope(leaves[i].fence))
			dma_fence_set_deadline(leaves[i].fence, deadline);
		dma_fence_put(leaves[i].fence);
	}
	kfree(leaves);
}

static const struct dma_fence_ops scope_ops = {
	.get_driver_name = scope_driver_name,
	.get_timeline_name = scope_timeline_name,
	.enable_signaling = scope_enable_signaling,
	.signaled = scope_signaled,
	.release = scope_release,
	.set_deadline = scope_set_deadline,
};

void asahi_g17_scope_bind(struct dma_fence *fence, struct dma_fence *root)
{
	struct asahi_g17_scope *scope = to_scope(fence);
	/* A concurrent raw syncobj reset is invalid for the private queue anchor,
	 * but must not turn a hostile userspace race into a kernel dereference.
	 */
	if (!root || dma_fence_chain_contained(root) != fence)
		return;
	/* Written once before the submit returns; an unmatched snapshot takes fallback. */
	WRITE_ONCE(scope->anchor_point, root->seqno);
	WRITE_ONCE(scope->anchor_context, root->context);
}

struct dma_fence *asahi_g17_scope_create(struct dma_fence *previous,
					 struct dma_fence *submitted, u64 point,
					 bool compute_only)
{
	struct dma_fence *contained = previous ? dma_fence_chain_contained(previous) : NULL;
	struct dma_fence *parent = NULL;
	struct asahi_g17_scope *scope, *prior = NULL;
	u64 context;
	unsigned int i;

	if (asahi_g17_is_scope(contained)) {
		prior = to_scope(contained);
		if (READ_ONCE(prior->anchor_context) != previous->context ||
		    READ_ONCE(prior->anchor_point) != previous->seqno ||
		    point <= contained->seqno)
			prior = NULL;
	}
	if (prior) {
		parent = dma_fence_get(contained);
		context = contained->context;
	} else {
		/* Empty submissions or external edits may insert a foreign timeline point.
		 * Flatten that boundary once; the ordinary path retains just the prior scope.
		 */
		if (previous) {
			parent = dma_fence_unwrap_merge(previous);
			if (!parent)
				return NULL;
		}
		context = dma_fence_context_alloc(1);
	}
	scope = kzalloc_obj(*scope);
	if (!scope) {
		dma_fence_put(parent);
		return NULL;
	}
	dma_fence_init64(&scope->base, &scope_ops, NULL, context, point);
	init_irq_work(&scope->deferred.work, scope_work);
	scope->deferred.complete = scope_complete_node;
	scope->compute_only = compute_only;
	scope->children[0] = parent;
	scope->children[1] = dma_fence_get(submitted);
	/* The constructor guard prevents completion while callbacks are being installed. */
	atomic_set(&scope->pending, 3);
	scope->base.error = 1;
	for (i = 0; i < ARRAY_SIZE(scope->children); ++i) {
		struct dma_fence *child = scope->children[i];
		int err;

		if (!child) {
			scope_arrived(scope, 0);
			continue;
		}
		scope->callbacks[i].scope = scope;
		dma_fence_get(&scope->base);
		preempt_disable();
		__this_cpu_write(scope_chaining_fence, child);
		err = dma_fence_add_callback(child, &scope->callbacks[i].callback,
					     scope_callback);
		__this_cpu_write(scope_chaining_fence, NULL);
		preempt_enable();
		if (err) {
			scope_arrived(scope, child->error);
			dma_fence_put(&scope->base);
		}
	}
	scope_arrived(scope, 0);
	return &scope->base;
}

/*
 * Variable-fan-in barrier fence that signals once every child has signalled,
 * joining the per-CPU active_scopes drain list when triggered from
 * asahi_g17_signal_fence() so completion runs inline without queueing irq_work.
 */
struct asahi_g17_barrier;
struct asahi_g17_barrier_entry {
	struct dma_fence *child;
	struct dma_fence_cb cb;
	struct asahi_g17_barrier *barrier;
};

struct asahi_g17_barrier {
	struct dma_fence base;
	atomic_t pending;
	struct asahi_g17_deferred deferred;
	unsigned int num_children;
	struct asahi_g17_barrier_entry entries[];
};

static struct asahi_g17_barrier *to_barrier(struct dma_fence *fence)
{
	return container_of(fence, struct asahi_g17_barrier, base);
}

static const char *barrier_driver_name(struct dma_fence *fence)
{
	return "aurora";
}

static const char *barrier_timeline_name(struct dma_fence *fence)
{
	return "g17-dep-barrier";
}

static bool barrier_signaled(struct dma_fence *fence)
{
	struct asahi_g17_barrier *b = to_barrier(fence);

	if (atomic_read_acquire(&b->pending))
		return false;
	cmpxchg(&fence->error, 1, 0);
	return true;
}

static bool barrier_enable_signaling(struct dma_fence *fence)
{
	return !barrier_signaled(fence);
}

static void barrier_drop_children(struct asahi_g17_barrier *b)
{
	struct dma_fence *batch[16];
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < b->num_children; ) {
		unsigned int n = 0;

		spin_lock_irqsave(&b->base.inline_lock, flags);
		while (i < b->num_children && n < ARRAY_SIZE(batch)) {
			batch[n] = b->entries[i].child;
			b->entries[i].child = NULL;
			if (batch[n])
				++n;
			++i;
		}
		spin_unlock_irqrestore(&b->base.inline_lock, flags);
		while (n)
			dma_fence_put(batch[--n]);
	}
}

static void barrier_complete_node(struct asahi_g17_deferred *d)
{
	struct asahi_g17_barrier *b =
		container_of(d, struct asahi_g17_barrier, deferred);
	cmpxchg(&b->base.error, 1, 0);
	dma_fence_signal(&b->base);
	barrier_drop_children(b);
	dma_fence_put(&b->base);
}

static void barrier_work(struct irq_work *work)
{
	struct asahi_g17_deferred *d =
		container_of(work, struct asahi_g17_deferred, work);
	struct asahi_g17_barrier *b =
		container_of(d, struct asahi_g17_barrier, deferred);
	cmpxchg(&b->base.error, 1, 0);
	asahi_g17_signal_fence(&b->base);
	barrier_drop_children(b);
	dma_fence_put(&b->base);
}

static void barrier_arrived(struct asahi_g17_barrier *b, int error)
{
	if (error < 0)
		cmpxchg(&b->base.error, 1, error);
	if (atomic_dec_and_test(&b->pending)) {
		struct llist_head *ready;
		dma_fence_get(&b->base);
		preempt_disable();
		ready = __this_cpu_read(active_scopes);
		if (ready)
			llist_add(&b->deferred.ready, ready);
		preempt_enable();
		if (!ready)
			irq_work_queue(&b->deferred.work);
	}
}

static void barrier_callback(struct dma_fence *child, struct dma_fence_cb *callback)
{
	struct asahi_g17_barrier_entry *entry =
		container_of(callback, typeof(*entry), cb);
	struct asahi_g17_barrier *b = entry->barrier;
	barrier_arrived(b, child->error);
	dma_fence_put(&b->base);
}

static void barrier_release(struct dma_fence *fence)
{
	struct asahi_g17_barrier *b = to_barrier(fence);
	unsigned int i;

	for (i = 0; i < b->num_children; ++i)
		dma_fence_put(b->entries[i].child);
	dma_fence_free(fence);
}

static void barrier_set_deadline(struct dma_fence *fence, ktime_t deadline)
{
	struct asahi_g17_barrier *b = to_barrier(fence);
	struct dma_fence *batch[16];
	unsigned long flags;
	unsigned int i;

	if (dma_fence_is_signaled(fence))
		return;
	for (i = 0; i < b->num_children; ) {
		unsigned int n = 0;

		spin_lock_irqsave(&fence->inline_lock, flags);
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
			spin_unlock_irqrestore(&fence->inline_lock, flags);
			return;
		}
		while (i < b->num_children && n < ARRAY_SIZE(batch)) {
			if (b->entries[i].child)
				batch[n++] = dma_fence_get(b->entries[i].child);
			++i;
		}
		spin_unlock_irqrestore(&fence->inline_lock, flags);
		while (n) {
			struct dma_fence *c = batch[--n];
			dma_fence_set_deadline(c, deadline);
			dma_fence_put(c);
		}
	}
}

static const struct dma_fence_ops barrier_ops = {
	.get_driver_name = barrier_driver_name,
	.get_timeline_name = barrier_timeline_name,
	.enable_signaling = barrier_enable_signaling,
	.signaled = barrier_signaled,
	.release = barrier_release,
	.set_deadline = barrier_set_deadline,
};

struct dma_fence *asahi_g17_barrier_create(struct dma_fence *const *fences,
					   size_t count)
{
	struct asahi_g17_barrier *b;
	unsigned int i;

	if (count == 0)
		return dma_fence_get_stub();
	if (count == 1)
		return dma_fence_get(fences[0]);
	if (count > (size_t)INT_MAX - 1)
		return NULL;

	b = kzalloc(struct_size(b, entries, count), GFP_KERNEL);
	if (!b)
		return NULL;

	b->num_children = count;
	dma_fence_init64(&b->base, &barrier_ops, NULL,
			 dma_fence_context_alloc(1), 1);
	init_irq_work(&b->deferred.work, barrier_work);
	b->deferred.complete = barrier_complete_node;
	/* Constructor guard prevents completion while callbacks are being installed. */
	atomic_set(&b->pending, (int)count + 1);
	b->base.error = 1;

	for (i = 0; i < count; ++i) {
		struct dma_fence *child = dma_fence_get(fences[i]);
		b->entries[i].child = child;
		b->entries[i].barrier = b;
		dma_fence_get(&b->base);
		if (dma_fence_add_callback(child, &b->entries[i].cb, barrier_callback)) {
			barrier_arrived(b, child->error);
			dma_fence_put(&b->base);
		}
	}
	/* Release constructor guard inline without irq_work if all arrived. */
	if (atomic_dec_and_test(&b->pending)) {
		cmpxchg(&b->base.error, 1, 0);
		dma_fence_signal(&b->base);
		barrier_drop_children(b);
	}
	return &b->base;
}
