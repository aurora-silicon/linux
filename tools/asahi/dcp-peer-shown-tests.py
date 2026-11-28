#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Paired firmware-27 completion shown-state and concurrent unbind retirement tests."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'drivers/gpu/drm/apple/dcp-peer-frames-v27.h').read_text()
H17P = (ROOT / 'drivers/gpu/drm/apple/iomfb_h17p.h').read_text()
V26 = (ROOT / 'drivers/gpu/drm/apple/dcp-peer-v26_6.inc').read_text()
RUNTIME = (ROOT / 'drivers/gpu/drm/apple/dcp-peer-runtime-v27.inc').read_text()
SHOWN = '''	*shown = s->have_shown[0] && s->shown[0] == done &&
		 s->have_shown[1] && s->shown[1] == done;
'''
assert HEADER.count(SHOWN) == 1
if '--always-shown' in sys.argv:
    HEADER = HEADER.replace(SHOWN, '\t*shown = true;\n')
if '--owner-only' in sys.argv:
    HEADER = HEADER.replace(SHOWN, '\t*shown = s->have_shown[0] && s->shown[0] == done;\n')


def extract_fn(src: str, name: str) -> str:
    idx = src.index(name + '(')
    start = src.rfind('\n', 0, idx) + 1
    brace = src.index('{', idx)
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == '{':
            depth += 1
        elif src[pos] == '}':
            depth -= 1
            if depth == 0:
                return src[start:pos + 1]
    raise AssertionError(f'unterminated {name}')


h17p_slice = H17P[H17P.index('#define IOMFB_DROPPED_MAX'):H17P.index('\n/* The H17P swap boundary')]
forget_fn = extract_fn(V26, 'dcp_peer_forget')
sync_fn = extract_fn(V26, 'dcp_peer_retire_sync')
record_fn = extract_fn(RUNTIME, 'dcp_peer_record_retired_v27')
transition_end_fn = extract_fn(RUNTIME, 'dcp_peer_transition_end')
stop_once_fn = extract_fn(RUNTIME, 'dcp_peer_stop_v27_once')
stop_fn = extract_fn(RUNTIME, 'dcp_peer_stop_v27')
stop_wait_fn = extract_fn(RUNTIME, 'dcp_peer_stop_wait_v27')

if '--drop-retire-sync' in sys.argv:
    sync_fn = 'static void dcp_peer_retire_sync(void) { (void)down_write; (void)up_write; }\n'

TEST = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef uint32_t u32; typedef int32_t s32;

#define DCP_FIRMWARE_V_27_0 27
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
static void (*g_acquire_hook)(const volatile void *ptr);
#define smp_load_acquire(p) ({ \
	typeof(*(p)) _v; \
	if (g_acquire_hook) \
		g_acquire_hook((const volatile void *)(p)); \
	_v = __atomic_load_n((p), __ATOMIC_ACQUIRE); \
	_v; \
})
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define min_t(t, a, b) ({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })

static unsigned long mock_jiffies_ms(void)
{
	struct timespec ts;

	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}
#define jiffies mock_jiffies_ms()

struct device_mock {
	pthread_mutex_t m;
};

static bool device_trylock(struct device_mock *d)
{
	return pthread_mutex_trylock(&d->m) == 0;
}

static void device_unlock(struct device_mock *d)
{
	assert(pthread_mutex_unlock(&d->m) == 0);
}

struct wq_mock {
	pthread_mutex_t m;
	pthread_cond_t cv;
};

static struct wq_mock dcp_peer_stop_wq = { .m = PTHREAD_MUTEX_INITIALIZER };
static atomic_uint g_wait_calls;

static void init_wq_monotonic(struct wq_mock *wq)
{
	pthread_condattr_t attr;

	assert(pthread_condattr_init(&attr) == 0);
	assert(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0);
	assert(pthread_cond_init(&wq->cv, &attr) == 0);
	assert(pthread_condattr_destroy(&attr) == 0);
}

static void wake_up_all(struct wq_mock *wq)
{
	assert(pthread_mutex_lock(&wq->m) == 0);
	assert(pthread_cond_broadcast(&wq->cv) == 0);
	assert(pthread_mutex_unlock(&wq->m) == 0);
}

#define wait_event_timeout(wq, cond, timeout_ms) ({ \
	struct timespec _ts; \
	unsigned long _rem = (timeout_ms); \
	int _rc = 0; \
	bool _ok; \
	atomic_fetch_add(&g_wait_calls, 1); \
	assert(clock_gettime(CLOCK_MONOTONIC, &_ts) == 0); \
	_ts.tv_sec += _rem / 1000; \
	_ts.tv_nsec += (_rem % 1000) * 1000000L; \
	if (_ts.tv_nsec >= 1000000000L) { \
		_ts.tv_sec++; \
		_ts.tv_nsec -= 1000000000L; \
	} \
	assert(pthread_mutex_lock(&(wq).m) == 0); \
	while (!(cond) && _rc != ETIMEDOUT) \
		_rc = pthread_cond_timedwait(&(wq).cv, &(wq).m, &_ts); \
	_ok = (cond); \
	assert(pthread_mutex_unlock(&(wq).m) == 0); \
	_ok ? 1L : 0L; \
})

struct mutex_mock {
	pthread_mutex_t m;
	bool is_peer_lock;
};

struct rwsem_mock {
	pthread_rwlock_t rw;
};

static struct mutex_mock dcp_peer_lock = { .m = PTHREAD_MUTEX_INITIALIZER, .is_peer_lock = true };
static struct rwsem_mock dcp_peer_retire_rwsem = { .rw = PTHREAD_RWLOCK_INITIALIZER };

static _Thread_local bool tls_holding_peer_lock;
static void (*pre_iomfb_lock_hook)(struct mutex_mock *m);

static void mutex_lock_impl(struct mutex_mock *m)
{
	if (m->is_peer_lock) {
		assert(!tls_holding_peer_lock);
	} else {
		/* Preserve iomfb.lock -> dcp_peer_lock order (never invert). */
		assert(!tls_holding_peer_lock);
		if (pre_iomfb_lock_hook)
			pre_iomfb_lock_hook(m);
	}
	assert(pthread_mutex_lock(&m->m) == 0);
	if (m->is_peer_lock)
		tls_holding_peer_lock = true;
}

static void mutex_unlock_impl(struct mutex_mock *m)
{
	if (m->is_peer_lock) {
		assert(tls_holding_peer_lock);
		tls_holding_peer_lock = false;
	}
	assert(pthread_mutex_unlock(&m->m) == 0);
}

static void down_read(struct rwsem_mock *r) { assert(pthread_rwlock_rdlock(&r->rw) == 0); }
static void up_read(struct rwsem_mock *r) { assert(pthread_rwlock_unlock(&r->rw) == 0); }
static void down_write(struct rwsem_mock *r) { assert(pthread_rwlock_wrlock(&r->rw) == 0); }
static void up_write(struct rwsem_mock *r) { assert(pthread_rwlock_unlock(&r->rw) == 0); }

#define _CAT2(a, b) a##b
#define _CAT(a, b) _CAT2(a, b)

struct rwsem_read_guard { struct rwsem_mock *lock; };
static void rwsem_read_unlock_guard(struct rwsem_read_guard *g)
{
	if (g->lock)
		up_read(g->lock);
}
#define guard_rwsem_read(sem_ptr) \
	struct rwsem_read_guard _CAT(__rg_, __LINE__) \
		__attribute__((cleanup(rwsem_read_unlock_guard))) = \
		(down_read(sem_ptr), (struct rwsem_read_guard){ (sem_ptr) })

struct mutex_guard { struct mutex_mock *lock; bool done; };
static void mutex_unlock_guard(struct mutex_guard *g)
{
	if (g->lock)
		mutex_unlock_impl(g->lock);
}
#define guard_mutex(m_ptr) \
	struct mutex_guard _CAT(__mg_, __LINE__) \
		__attribute__((cleanup(mutex_unlock_guard))) = \
		(mutex_lock_impl(m_ptr), (struct mutex_guard){ (m_ptr), false })

#define guard(kind) _CAT(guard_, kind)
#define scoped_guard(kind, lock_ptr) \
	for (struct _CAT(kind, _guard) _CAT(__sg_, __LINE__) \
		     __attribute__((cleanup(_CAT(kind, _unlock_guard)))) = \
		     (_CAT(kind, _lock_impl)(lock_ptr), \
		      (struct _CAT(kind, _guard)){ (lock_ptr), false }); \
	     !_CAT(__sg_, __LINE__).done; _CAT(__sg_, __LINE__).done = true)

#define rwsem_read_lock_impl down_read

''' + HEADER.replace('#include <linux/types.h>', '') + '\n' + h17p_slice + r'''

struct dcp_peer_tiling {
	bool active;
};

struct apple_dcp {
	struct device_mock *dev;
	int fw_compat;
	struct apple_dcp *peer_reserved;
	struct apple_dcp *peer_owner;
	struct dcp_peer_tiling peer_tiling;
	struct dcp_peer_frames_v27 peer_frames;
	bool peer_unbinding;
	bool peer_transition_active;
	u32 peer_transition_seq;
	struct {
		struct mutex_mock lock;
		bool stopped;
		bool peer_paused;
	} iomfb;
	struct dcp_present_state_h17p present_state_h17p;
};

static bool peer_scanout = true;

static void (*g_quiesce_hook)(struct apple_dcp *dcp, struct apple_dcp *peer, int *ret);
static int g_settle_ret;
static atomic_uint g_quiesce_calls;

static void iomfb_pause_peer(struct apple_dcp *dcp, bool pause)
{
	WRITE_ONCE(dcp->iomfb.peer_paused, pause);
}

static int iomfb_queue_settle(struct apple_dcp *dcp, unsigned long timeout)
{
	(void)dcp;
	(void)timeout;
	return g_settle_ret;
}

static bool iomfb_retire_stalled_present_h17p(struct apple_dcp *dcp)
{
	(void)dcp;
	return false;
}

static bool dcp_peer_transport_idle(struct apple_dcp *dcp)
{
	return !READ_ONCE(dcp->peer_unbinding);
}

static int dcp_peer_probe_quiesce(struct apple_dcp *dcp, struct apple_dcp *peer,
				  bool invalidate_mode)
{
	int ret = 0;

	(void)invalidate_mode;
	atomic_fetch_add(&g_quiesce_calls, 1);
	if (g_quiesce_hook)
		g_quiesce_hook(dcp, peer, &ret);
	return ret;
}

''' + forget_fn + '\n' + sync_fn + '\n' + record_fn + '\n' + transition_end_fn + '\n' + stop_once_fn + '\n' + stop_fn + '\n' + stop_wait_fn + r'''

static struct apple_dcp *make_dcp(void)
{
	struct apple_dcp *d = calloc(1, sizeof(*d));

	assert(d);
	d->dev = calloc(1, sizeof(*d->dev));
	assert(d->dev);
	assert(pthread_mutex_init(&d->dev->m, NULL) == 0);
	assert(pthread_mutex_init(&d->iomfb.lock.m, NULL) == 0);
	d->fw_compat = DCP_FIRMWARE_V_27_0;
	return d;
}

static void free_dcp(struct apple_dcp *d)
{
	pthread_mutex_destroy(&d->iomfb.lock.m);
	pthread_mutex_destroy(&d->dev->m);
	free(d->dev);
	free(d);
}

static struct apple_dcp *g_hook_peer;
static struct apple_dcp *g_hook_other;

static void hook_clear_on_lock(struct mutex_mock *m)
{
	if (m == &g_hook_peer->iomfb.lock) {
		pre_iomfb_lock_hook = NULL;
		dcp_peer_forget(g_hook_peer);
	}
}

static void hook_reassign_on_lock(struct mutex_mock *m)
{
	if (m == &g_hook_peer->iomfb.lock) {
		pre_iomfb_lock_hook = NULL;
		dcp_peer_forget(g_hook_peer);
		mutex_lock_impl(&dcp_peer_lock);
		g_hook_peer->peer_unbinding = false;
		g_hook_other->peer_reserved = g_hook_peer;
		g_hook_peer->peer_owner = g_hook_other;
		mutex_unlock_impl(&dcp_peer_lock);
	}
}

struct unbind_race_ctx {
	struct apple_dcp *owner;
	struct apple_dcp *peer;
	pthread_mutex_t sync_m;
	pthread_cond_t sync_cv;
	bool reader_paused;
	bool unbind_started;
	bool release_reader;
	atomic_bool peer_freed;
};

static struct unbind_race_ctx *g_race;

static void hook_pause_for_unbind(struct mutex_mock *m)
{
	if (m == &g_race->peer->iomfb.lock) {
		pre_iomfb_lock_hook = NULL;
		pthread_mutex_lock(&g_race->sync_m);
		g_race->reader_paused = true;
		pthread_cond_broadcast(&g_race->sync_cv);
		while (!g_race->release_reader)
			pthread_cond_wait(&g_race->sync_cv, &g_race->sync_m);
		pthread_mutex_unlock(&g_race->sync_m);
	}
}

static void *unbind_thread(void *arg)
{
	struct unbind_race_ctx *ctx = arg;

	pthread_mutex_lock(&ctx->sync_m);
	while (!ctx->reader_paused)
		pthread_cond_wait(&ctx->sync_cv, &ctx->sync_m);
	ctx->unbind_started = true;
	pthread_cond_broadcast(&ctx->sync_cv);
	pthread_mutex_unlock(&ctx->sync_m);

	dcp_peer_forget(ctx->peer);
	dcp_peer_retire_sync();
	atomic_store(&ctx->peer_freed, true);
	free_dcp(ctx->peer);
	return NULL;
}

static void *reader_thread(void *arg)
{
	struct unbind_race_ctx *ctx = arg;

	dcp_peer_record_retired_v27(ctx->owner, 45);
	return NULL;
}

static void test_record_retired_races(void)
{
	struct apple_dcp *owner = make_dcp();
	struct apple_dcp *other = make_dcp();
	struct apple_dcp *peer = make_dcp();

	/* Normal paired retire records on the reserved peer. */
	owner->peer_reserved = peer;
	peer->peer_owner = owner;
	dcp_peer_record_retired_v27(owner, 42);
	assert(dcp_present_was_retired_h17p(&peer->present_state_h17p, 42));

	/* Stale reservation cleared before peer->iomfb.lock is skipped. */
	g_hook_peer = peer;
	pre_iomfb_lock_hook = hook_clear_on_lock;
	dcp_peer_record_retired_v27(owner, 43);
	assert(!dcp_present_was_retired_h17p(&peer->present_state_h17p, 43));

	/* Reassigned peer to another owner before peer->iomfb.lock is skipped. */
	peer->peer_unbinding = false;
	owner->peer_reserved = peer;
	peer->peer_owner = owner;
	g_hook_other = other;
	pre_iomfb_lock_hook = hook_reassign_on_lock;
	dcp_peer_record_retired_v27(owner, 44);
	assert(!dcp_present_was_retired_h17p(&peer->present_state_h17p, 44));

	/* Concurrent unbind + free waits for dcp_peer_record_retired_v27 to finish. */
	dcp_peer_forget(peer);
	peer->peer_unbinding = false;
	owner->peer_reserved = peer;
	peer->peer_owner = owner;
	struct unbind_race_ctx ctx = {
		.owner = owner,
		.peer = peer,
		.sync_m = PTHREAD_MUTEX_INITIALIZER,
		.sync_cv = PTHREAD_COND_INITIALIZER,
	};
	g_race = &ctx;
	pre_iomfb_lock_hook = hook_pause_for_unbind;

	pthread_t r_tid, u_tid;
	assert(pthread_create(&r_tid, NULL, reader_thread, &ctx) == 0);
	assert(pthread_create(&u_tid, NULL, unbind_thread, &ctx) == 0);

	pthread_mutex_lock(&ctx.sync_m);
	while (!ctx.unbind_started)
		pthread_cond_wait(&ctx.sync_cv, &ctx.sync_m);
	pthread_mutex_unlock(&ctx.sync_m);

	/* Give unbind a chance to race past dcp_peer_retire_sync() if broken. */
	nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
	assert(!atomic_load(&ctx.peer_freed));

	pthread_mutex_lock(&ctx.sync_m);
	ctx.release_reader = true;
	pthread_cond_broadcast(&ctx.sync_cv);
	pthread_mutex_unlock(&ctx.sync_m);

	assert(pthread_join(r_tid, NULL) == 0);
	assert(pthread_join(u_tid, NULL) == 0);
	assert(atomic_load(&ctx.peer_freed));

	free_dcp(owner);
	free_dcp(other);
}

struct stop_race_ctx {
	struct apple_dcp *owner;
	unsigned int timeout_ms;
	int ret;
	atomic_bool done;
	pthread_mutex_t m;
	pthread_cond_t cv;
	bool in_quiesce;
	bool release_quiesce;
	int quiesce_ret;
	bool clear_active;
};

static struct stop_race_ctx *g_stop_ctx;

static void hook_quiesce_race(struct apple_dcp *dcp, struct apple_dcp *peer, int *ret)
{
	struct stop_race_ctx *ctx = g_stop_ctx;
	int call = atomic_load(&g_quiesce_calls);

	(void)peer;
	if (ctx && call == 1) {
		pthread_mutex_lock(&ctx->m);
		ctx->in_quiesce = true;
		pthread_cond_broadcast(&ctx->cv);
		while (!ctx->release_quiesce)
			pthread_cond_wait(&ctx->cv, &ctx->m);
		pthread_mutex_unlock(&ctx->m);
		if (ctx->clear_active)
			smp_store_release(&dcp->peer_frames.active, false);
		*ret = ctx->quiesce_ret;
	} else {
		smp_store_release(&dcp->peer_frames.active, false);
		*ret = 0;
	}
}

static void *stop_wait_worker(void *arg)
{
	struct stop_race_ctx *ctx = arg;

	ctx->ret = dcp_peer_stop_wait_v27(ctx->owner, ctx->timeout_ms);
	atomic_store(&ctx->done, true);
	return NULL;
}

struct split_modeset_ctx {
	struct apple_dcp *owner;
	pthread_mutex_t m;
	pthread_cond_t cv;
	bool dev_locked;
	bool modeset_started;
};

static struct split_modeset_ctx *g_split_ctx;

static void hook_modeset_between_acquire(const volatile void *ptr)
{
	struct split_modeset_ctx *ctx = g_split_ctx;

	if (!ctx || ptr != &ctx->owner->peer_frames.active)
		return;
	g_acquire_hook = NULL;
	mutex_lock_impl(&dcp_peer_lock);
	assert(!ctx->owner->peer_transition_active);
	assert(ctx->owner->peer_frames.active);
	smp_store_release(&ctx->owner->peer_transition_active, true);
	smp_store_release(&ctx->owner->peer_frames.active, false);
	mutex_unlock_impl(&dcp_peer_lock);

	pthread_mutex_lock(&ctx->m);
	ctx->modeset_started = true;
	pthread_cond_broadcast(&ctx->cv);
	pthread_mutex_unlock(&ctx->m);
}

static void *split_modeset_worker(void *arg)
{
	struct split_modeset_ctx *ctx = arg;

	assert(pthread_mutex_lock(&ctx->owner->dev->m) == 0);
	pthread_mutex_lock(&ctx->m);
	ctx->dev_locked = true;
	pthread_cond_broadcast(&ctx->cv);
	while (!ctx->modeset_started)
		pthread_cond_wait(&ctx->cv, &ctx->m);
	pthread_mutex_unlock(&ctx->m);
	assert(pthread_mutex_unlock(&ctx->owner->dev->m) == 0);

	nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
	mutex_lock_impl(&dcp_peer_lock);
	ctx->owner->peer_frames.active = true;
	mutex_unlock_impl(&dcp_peer_lock);
	dcp_peer_transition_end(ctx->owner);
	return NULL;
}

static void test_peer_stop_wait(void)
{
	struct apple_dcp *owner = make_dcp();
	struct apple_dcp *peer = make_dcp();

	init_wq_monotonic(&dcp_peer_stop_wq);
	g_quiesce_hook = hook_quiesce_race;

	/* 1. Concurrent paired stop waits once on dcp_peer_stop_wq (no polling). */
	owner->peer_reserved = peer;
	peer->peer_owner = owner;
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);

	struct stop_race_ctx first = {
		.owner = owner,
		.timeout_ms = 1000,
		.m = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
		.clear_active = true,
	};
	struct stop_race_ctx second = {
		.owner = owner,
		.timeout_ms = 1000,
	};
	g_stop_ctx = &first;

	pthread_t t1, t2;
	assert(pthread_create(&t1, NULL, stop_wait_worker, &first) == 0);
	pthread_mutex_lock(&first.m);
	while (!first.in_quiesce)
		pthread_cond_wait(&first.cv, &first.m);
	pthread_mutex_unlock(&first.m);

	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	nanosleep(&(struct timespec){ .tv_nsec = 30 * 1000 * 1000 }, NULL);
	assert(atomic_load(&g_wait_calls) == 1);

	pthread_mutex_lock(&first.m);
	first.release_quiesce = true;
	pthread_cond_broadcast(&first.cv);
	pthread_mutex_unlock(&first.m);

	assert(pthread_join(t1, NULL) == 0);
	assert(pthread_join(t2, NULL) == 0);
	assert(first.ret == 1);
	assert(second.ret == 0);
	assert(atomic_load(&g_quiesce_calls) == 1);
	assert(atomic_load(&g_wait_calls) == 1);

	/* 2. Failed in-flight stop wakes waiter, which retries and succeeds. */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	first = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
		.m = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
		.quiesce_ret = -EIO,
		.clear_active = false,
	};
	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
	};
	g_stop_ctx = &first;

	assert(pthread_create(&t1, NULL, stop_wait_worker, &first) == 0);
	pthread_mutex_lock(&first.m);
	while (!first.in_quiesce)
		pthread_cond_wait(&first.cv, &first.m);
	pthread_mutex_unlock(&first.m);

	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
	assert(atomic_load(&g_wait_calls) == 1);

	pthread_mutex_lock(&first.m);
	first.release_quiesce = true;
	pthread_cond_broadcast(&first.cv);
	pthread_mutex_unlock(&first.m);

	assert(pthread_join(t1, NULL) == 0);
	assert(pthread_join(t2, NULL) == 0);
	assert(first.ret == -EIO);
	assert(second.ret == 1);
	assert(atomic_load(&g_quiesce_calls) == 2);
	assert(atomic_load(&g_wait_calls) == 1);

	/* 3. Contended wait times out cleanly at deadline without extra retry. */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	first = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
		.m = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
		.clear_active = true,
	};
	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 25,
	};
	g_stop_ctx = &first;

	assert(pthread_create(&t1, NULL, stop_wait_worker, &first) == 0);
	pthread_mutex_lock(&first.m);
	while (!first.in_quiesce)
		pthread_cond_wait(&first.cv, &first.m);
	pthread_mutex_unlock(&first.m);

	unsigned long start_ms = mock_jiffies_ms();
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	assert(pthread_join(t2, NULL) == 0);
	unsigned long elapsed_ms = mock_jiffies_ms() - start_ms;
	assert(second.ret == -EBUSY);
	assert(elapsed_ms >= 20 && elapsed_ms < 80);
	assert(atomic_load(&g_wait_calls) == 1);

	pthread_mutex_lock(&first.m);
	first.release_quiesce = true;
	pthread_cond_broadcast(&first.cv);
	pthread_mutex_unlock(&first.m);
	assert(pthread_join(t1, NULL) == 0);
	assert(first.ret == 1);

	/* 4. Uncontended -EBUSY (held device lock) stops cleanly at deadline. */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	assert(pthread_mutex_lock(&owner->dev->m) == 0);
	start_ms = mock_jiffies_ms();
	int ret = dcp_peer_stop_wait_v27(owner, 25);
	elapsed_ms = mock_jiffies_ms() - start_ms;
	assert(pthread_mutex_unlock(&owner->dev->m) == 0);
	assert(ret == -EBUSY);
	assert(elapsed_ms >= 20 && elapsed_ms < 60);
	assert(atomic_load(&g_wait_calls) >= 1);

	/*
	 * 5. In-flight paired modeset clearing active mid-quiesce and setting it
	 * again on completion does not let a waiter bypass waiting or skip
	 * stopping the new pair (both when active clears mid-wait and when a
	 * waiter arrives while active is already false during the transition).
	 */
	g_stop_ctx = NULL;
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	mutex_lock_impl(&dcp_peer_lock);
	smp_store_release(&owner->peer_transition_active, true);
	mutex_unlock_impl(&dcp_peer_lock);

	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
	};
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	while (atomic_load(&g_wait_calls) < 1)
		nanosleep(&(struct timespec){ .tv_nsec = 1000 * 1000 }, NULL);

	mutex_lock_impl(&dcp_peer_lock);
	owner->peer_frames.active = false;
	mutex_unlock_impl(&dcp_peer_lock);
	wake_up_all(&dcp_peer_stop_wq);
	nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
	assert(!atomic_load(&second.done));

	mutex_lock_impl(&dcp_peer_lock);
	owner->peer_frames.active = true;
	mutex_unlock_impl(&dcp_peer_lock);
	dcp_peer_transition_end(owner);

	assert(pthread_join(t2, NULL) == 0);
	assert(second.ret == 1);
	assert(atomic_load(&g_quiesce_calls) == 1);
	assert(!owner->peer_frames.active);

	/* 5b. Waiter arriving while active == false mid-modeset waits and stops new pair. */
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	mutex_lock_impl(&dcp_peer_lock);
	owner->peer_frames.active = false;
	smp_store_release(&owner->peer_transition_active, true);
	mutex_unlock_impl(&dcp_peer_lock);

	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
	};
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	while (atomic_load(&g_wait_calls) < 1)
		nanosleep(&(struct timespec){ .tv_nsec = 1000 * 1000 }, NULL);
	nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
	assert(!atomic_load(&second.done));

	mutex_lock_impl(&dcp_peer_lock);
	owner->peer_frames.active = true;
	mutex_unlock_impl(&dcp_peer_lock);
	dcp_peer_transition_end(owner);

	assert(pthread_join(t2, NULL) == 0);
	assert(second.ret == 1);
	assert(atomic_load(&g_quiesce_calls) == 1);
	assert(!owner->peer_frames.active);

	/*
	 * 5c/5d. A modeset claiming transition and clearing peer_frames.active
	 * between reading peer_transition_active and peer_frames.active cannot
	 * cause a false-success 0 return at stop entry or retry exits; the stop
	 * waits for the modeset to finish and stops the new replacement pair.
	 */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	struct split_modeset_ctx split_entry = {
		.owner = owner,
		.m = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
	};
	g_split_ctx = &split_entry;
	g_acquire_hook = hook_modeset_between_acquire;
	ret = dcp_peer_stop_wait_v27(owner, 1000);
	g_acquire_hook = NULL;
	g_split_ctx = NULL;
	assert(!split_entry.modeset_started);
	assert(ret == 1);
	assert(atomic_load(&g_quiesce_calls) == 1);
	assert(!owner->peer_frames.active);

	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	struct split_modeset_ctx split_retry = {
		.owner = owner,
		.m = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
	};
	g_split_ctx = &split_retry;
	pthread_t m_tid;
	assert(pthread_create(&m_tid, NULL, split_modeset_worker, &split_retry) == 0);
	pthread_mutex_lock(&split_retry.m);
	while (!split_retry.dev_locked)
		pthread_cond_wait(&split_retry.cv, &split_retry.m);
	pthread_mutex_unlock(&split_retry.m);

	g_acquire_hook = hook_modeset_between_acquire;
	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 1000,
	};
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	assert(pthread_join(t2, NULL) == 0);
	assert(pthread_join(m_tid, NULL) == 0);
	g_acquire_hook = NULL;
	g_split_ctx = NULL;
	assert(split_retry.modeset_started);
	assert(second.ret == 1);
	assert(atomic_load(&g_quiesce_calls) == 1);
	assert(!owner->peer_frames.active);

	/*
	 * 6. Holding dcp_peer_lock across deadline expiry prevents both initial
	 * admission and retry admission after a prior -EBUSY from claiming the
	 * transition or running quiesce past the deadline.
	 */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 25,
	};
	mutex_lock_impl(&dcp_peer_lock);
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	nanosleep(&(struct timespec){ .tv_nsec = 40 * 1000 * 1000 }, NULL);
	mutex_unlock_impl(&dcp_peer_lock);
	assert(pthread_join(t2, NULL) == 0);
	assert(second.ret == -EBUSY);
	assert(atomic_load(&g_quiesce_calls) == 0);
	assert(!owner->peer_transition_active);
	assert(owner->peer_frames.active);

	/* 6b. Retry admission blocked on dcp_peer_lock across expiry is not admitted. */
	owner->peer_frames.active = true;
	atomic_store(&g_wait_calls, 0);
	atomic_store(&g_quiesce_calls, 0);
	second = (struct stop_race_ctx){
		.owner = owner,
		.timeout_ms = 30,
	};
	assert(pthread_mutex_lock(&owner->dev->m) == 0);
	assert(pthread_create(&t2, NULL, stop_wait_worker, &second) == 0);
	while (atomic_load(&g_wait_calls) < 1)
		nanosleep(&(struct timespec){ .tv_nsec = 1000 * 1000 }, NULL);
	mutex_lock_impl(&dcp_peer_lock);
	assert(pthread_mutex_unlock(&owner->dev->m) == 0);
	nanosleep(&(struct timespec){ .tv_nsec = 45 * 1000 * 1000 }, NULL);
	mutex_unlock_impl(&dcp_peer_lock);
	assert(pthread_join(t2, NULL) == 0);
	assert(second.ret == -EBUSY);
	assert(atomic_load(&g_quiesce_calls) == 0);
	assert(!owner->peer_transition_active);
	assert(owner->peer_frames.active);

	g_quiesce_hook = NULL;
	g_stop_ctx = NULL;
	free_dcp(owner);
	free_dcp(peer);
}

static struct dcp_peer_frames_v27 paired(u32 first, u32 last)
{
	struct dcp_peer_frames_v27 s = { .active = true };

	for (u32 id = first; id <= last; id++)
		dcp_peer_frame_submit_v27(&s, id);
	return s;
}

int main(void)
{
	struct dcp_peer_frames_v27 s;
	u32 w = 0;
	bool shown = true;

	/* A new adaptive-sync minimum can reuse only the installed mode. */
	s = (struct dcp_peer_frames_v27){ .timing = 81, .color = 86, .active = true };
	assert(dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0, false));
	assert(!dcp_peer_vrr_only_v27(&s, 80, 86, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 85, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, true));
	s.failed = true;
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));
	s.failed = false;
	s.active = false;
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));

	/* First modeset of a pair: both engines display swap 5. */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 5, true, &w, &shown));
	assert(w == 5 && shown);

	/* The peer's half of swap 5 was not displayed. */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 5, false, &w, &shown));
	assert(w == 5 && !shown);

	/* Neither half displayed (the owner's completion came second). */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 1, 5, false, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 0, 5, false, &w, &shown));
	assert(w == 5 && !shown);

	/*
	 * Engines drift: the peer has shown 6 when the owner shows 5. The
	 * retired swap is 5, which the peer's shown 6 does not vouch for;
	 * swap 6 then retires shown.
	 */
	s = paired(5, 6);
	assert(!dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(w == 5 && !shown);
	assert(dcp_peer_frame_complete_v27(&s, 0, 6, true, &w, &shown));
	assert(w == 6 && shown);

	/* A displayed earlier swap does not mark a later, undisplayed one. */
	s = paired(5, 6);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(!dcp_peer_frame_complete_v27(&s, 0, 6, false, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	assert(w == 6 && !shown);

	/* A stale or duplicate completion is not a retirement. */
	assert(!dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	test_record_retired_races();
	test_peer_stop_wait();
	return 0;
}
'''

with tempfile.TemporaryDirectory() as t:
    src = Path(t) / 'peer.c'
    src.write_text(TEST)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', str(src), '-o',
                    str(src.with_suffix(''))], check=True)
    if subprocess.run([str(src.with_suffix(''))]).returncode:
        sys.exit(1)
print('PASS paired completion shown state and concurrent unbind / stale reservation tests')
