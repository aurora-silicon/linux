// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(void)(lock);          \
		(flags) = 0;           \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(lock);               \
		(void)(flags);              \
	} while (0)
struct apple_dart_sleep {
	int result;
};
#define APPLE_DART_WRITER_COUNTED 1
#define APPLE_DART_WRITER_CLOSED 2
struct apple_dart_cio {
	void *owner;
	unsigned int closed_writers;
	bool tlb_stale;
};
struct apple_dart {
	bool managed_sids, sleep_closed;
	unsigned int sleep_writers;
	int managed_lock;
	struct apple_dart_sleep *sleep_record;
	struct apple_dart_cio *cio;
	bool cio_active;
};
/* ACTUAL_FUNCTIONS */
int main(void)
{
	struct apple_dart_sleep s = { 0 };
	struct apple_dart d = { .managed_sids = true, .sleep_record = &s };
	struct apple_dart_cio cio = { 0 };
	int owner, gate;

	assert(apple_dart_sleep_writer_get(&d) == APPLE_DART_WRITER_COUNTED);
	/* nested invalidation cannot deadlock */
	assert(apple_dart_sleep_writer_get(&d) == APPLE_DART_WRITER_COUNTED);
	assert(apple_dart_sleep_close(&d) == -EBUSY && !d.sleep_closed);
	apple_dart_sleep_writer_put(&d, APPLE_DART_WRITER_COUNTED);
	assert(apple_dart_sleep_close(&d) == -EBUSY);
	apple_dart_sleep_writer_put(&d, APPLE_DART_WRITER_COUNTED);
	assert(!apple_dart_sleep_close(&d));
	assert(apple_dart_sleep_writer_get(&d) == -EBUSY &&
	       s.result == -EUCLEAN && !d.sleep_writers);
	d.sleep_writers = UINT_MAX;
	d.sleep_closed = false;
	assert(apple_dart_sleep_writer_get(&d) == -EOVERFLOW);
	/* Not gated at all: no lock, nothing counted. */
	d = (struct apple_dart){ 0 };
	gate = apple_dart_sleep_writer_get(&d);
	assert(!gate);
	apple_dart_sleep_writer_put(&d, gate);
	assert(!d.sleep_writers);
	/* A host-owned DART with no owner bound yet is not gated either. */
	d = (struct apple_dart){ .cio = &cio };
	gate = apple_dart_sleep_writer_get(&d);
	assert(!gate && !d.sleep_writers);
	apple_dart_sleep_writer_put(&d, gate);
	assert(!d.sleep_writers);
	/* Bound: counted while open, memory-only while closed, never refused. */
	cio.owner = &owner;
	gate = apple_dart_sleep_writer_get(&d);
	assert(gate == APPLE_DART_WRITER_COUNTED && d.sleep_writers == 1);
	assert(apple_dart_sleep_close(&d) == -EBUSY);
	apple_dart_sleep_writer_put(&d, gate);
	assert(!apple_dart_sleep_close(&d) && !cio.tlb_stale);
	gate = apple_dart_sleep_writer_get(&d);
	assert(gate == APPLE_DART_WRITER_CLOSED && !d.sleep_writers && cio.tlb_stale &&
	       cio.closed_writers == 1);
	apple_dart_sleep_writer_put(&d, gate);
	assert(!d.sleep_writers && !cio.closed_writers);
	/* Closed for good, its owner gone: still memory-only, never its registers. */
	cio.owner = NULL;
	gate = apple_dart_sleep_writer_get(&d);
	assert(gate == APPLE_DART_WRITER_CLOSED && cio.closed_writers == 1);
	apple_dart_sleep_writer_put(&d, gate);
	assert(!cio.closed_writers);
	puts("DART writer gate: nested/in-flight/closed/legacy/CIO invariants PASS");
}
