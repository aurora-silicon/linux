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
struct apple_dart {
	bool managed_sids, sleep_closed;
	unsigned int sleep_writers;
	int managed_lock;
	struct apple_dart_sleep *sleep_record;
};
/* ACTUAL_FUNCTIONS */
int main(void)
{
	struct apple_dart_sleep s = { 0 };
	struct apple_dart d = { .managed_sids = true, .sleep_record = &s };

	assert(!apple_dart_sleep_writer_get(&d));
	assert(!apple_dart_sleep_writer_get(
		&d)); /* nested invalidation cannot deadlock */
	assert(apple_dart_sleep_close(&d) == -EBUSY && !d.sleep_closed);
	apple_dart_sleep_writer_put(&d);
	assert(apple_dart_sleep_close(&d) == -EBUSY);
	apple_dart_sleep_writer_put(&d);
	assert(!apple_dart_sleep_close(&d));
	assert(apple_dart_sleep_writer_get(&d) == -EBUSY &&
	       s.result == -EUCLEAN && !d.sleep_writers);
	d.sleep_writers = UINT_MAX;
	d.sleep_closed = false;
	assert(apple_dart_sleep_writer_get(&d) == -EOVERFLOW);
	d = (struct apple_dart){ 0 };
	assert(!apple_dart_sleep_writer_get(&d));
	apple_dart_sleep_writer_put(&d);
	assert(!d.sleep_writers);
	puts("DART writer gate: nested/in-flight/closed/legacy invariants PASS");
}
