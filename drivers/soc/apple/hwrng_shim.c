/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */
/*
 * hwrng core shim: struct hwrng lives here where the C compiler owns its
 * layout, and Rust sees an opaque pointer plus four entry points.
 */

#include <linux/hw_random.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "shim.h"

struct sep_hwrng {
	struct hwrng rng;
	/* Opaque Rust-side context, handed back to the read callback. */
	void *ctx;
	int (*read)(void *ctx, void *data, size_t max, bool wait);
};

static int sep_hwrng_read(struct hwrng *rng, void *data, size_t max,
				 bool wait)
{
	struct sep_hwrng *h =
		container_of(rng, struct sep_hwrng, rng);

	return h->read(h->ctx, data, max, wait);
}

void *sep_hwrng_alloc(void)
{
	return kzalloc(sizeof(struct sep_hwrng), GFP_KERNEL);
}

/* Must not be called while registered. */
void sep_hwrng_free(void *mem)
{
	kfree(mem);
}

/*
 * @name must outlive the registration; the Rust side passes a &'static CStr.
 * @quality is bits of entropy per 1024 bits of input; 0 takes the core default,
 * which is also 1024.
 */
int sep_hwrng_register(void *mem, const char *name,
			      unsigned short quality, void *ctx,
			      int (*read)(void *ctx, void *data, size_t max,
					  bool wait))
{
	struct sep_hwrng *h = mem;

	h->ctx = ctx;
	h->read = read;
	h->rng.name = name;
	h->rng.read = sep_hwrng_read;
	h->rng.quality = quality;

	return hwrng_register(&h->rng);
}

/*
 * Blocks until the core is done with the device, so any read in flight must
 * return promptly: the Rust side sets its shutdown flag and wakes its waiters
 * before calling this.
 */
void sep_hwrng_unregister(void *mem)
{
	struct sep_hwrng *h = mem;

	hwrng_unregister(&h->rng);
}
