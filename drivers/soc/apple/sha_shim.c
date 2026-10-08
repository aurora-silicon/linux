/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */
/* SHA-256 shim: hashes two caller-chosen segments; no protocol logic here. */

#include <crypto/hash.h>
#include <linux/err.h>
#include <linux/types.h>

#include "shim.h"

/*
 * Hashes `a` then `b`, writes 32 bytes to `out`. Allocates a transform per call
 * and may sleep. Returns 0 or a negative errno; `out` invalid on failure.
 */
int sep_sha256(const void *a, size_t alen, const void *b, size_t blen,
		      unsigned char *out)
{
	struct crypto_shash *tfm;
	int rc;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;

		rc = crypto_shash_init(desc);
		if (!rc && alen)
			rc = crypto_shash_update(desc, a, alen);
		if (!rc && blen)
			rc = crypto_shash_update(desc, b, blen);
		if (!rc)
			rc = crypto_shash_final(desc, out);

		/* The digest is over a request that may contain a secret. */
		shash_desc_zero(desc);
	}

	crypto_free_shash(tfm);
	return rc;
}
