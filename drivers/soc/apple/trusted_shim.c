// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Dj */
/*
 * Apple SEP driver — trusted-key source shim (device-rooted sealing wired to
 * the kernel trusted-key framework).
 *
 * An ABI the Rust bindings do not cover. <keys/trusted-type.h>'s struct
 * trusted_key_payload is sized by MAX_KEY_SIZE / MAX_BLOB_SIZE and embeds an
 * rcu_head, so its layout moves with the kernel config; and
 * register_trusted_key_source / unregister_trusted_key_source, key_type_trusted
 * and register_key_type() are C symbols absent from the Rust bindings.
 *
 * No sealing policy lives here: this file owns the struct handling and the
 * registration only. init/seal/unseal/get_random/exit live in trusted.rs and
 * are reached through the callbacks stored below. No key material is touched.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/key-type.h>
#include <keys/trusted-type.h>

#include "shim.h"

/*
 * register_trusted_key_source / unregister_trusted_key_source are declared by
 * <keys/trusted-type.h> and exported by the trusted-key core. Not re-declared
 * here: restating them risks a conflicting type (unregister returns void, not
 * int). register validates src->{name,ops->{init,seal,unseal}}, honours
 * trusted.source=, returns -EBUSY if another source is active, calls
 * src->ops->init(), then wires the static calls; unregister resets those static
 * calls, runs src->ops->exit(), and clears the active source.
 */

/* Rust callbacks (trusted.rs); the sep_tk_*_fn types are declared in shim.h. */
static sep_tk_init_fn   rs_init;
static sep_tk_seal_fn   rs_seal;
static sep_tk_unseal_fn rs_unseal;
static sep_tk_random_fn rs_random;
static sep_tk_exit_fn   rs_exit;

/*
 * Trampolines for the framework's ops table, each forwarding to the stored Rust
 * callback. Keeping these pointers in C means no Rust symbol is exported by
 * name and the ops table stays a plain static initialiser.
 */
static int tk_init(void)
{
	return rs_init ? rs_init() : -ENODEV;
}

static int tk_seal(struct trusted_key_payload *p, char *datablob)
{
	return rs_seal ? rs_seal(p, datablob) : -ENODEV;
}

static int tk_unseal(struct trusted_key_payload *p, char *datablob)
{
	return rs_unseal ? rs_unseal(p, datablob) : -ENODEV;
}

static int tk_get_random(unsigned char *key, size_t key_len)
{
	return rs_random ? rs_random(key, key_len) : -ENODEV;
}

static void tk_exit(void)
{
	if (rs_exit)
		rs_exit();
}

/*
 * A writable array, not a string literal, so .name assigns cleanly whether the
 * framework declares it char * or const char *.
 */
static char sep_tk_name[] = "applesep";

static struct trusted_key_ops sep_tk_ops = {
	.migratable	= 0,
	.init		= tk_init,
	.seal		= tk_seal,
	.unseal		= tk_unseal,
	.get_random	= tk_get_random,
	.exit		= tk_exit,
};

static struct trusted_key_source sep_tk_source = {
	.name	= sep_tk_name,
	.ops	= &sep_tk_ops,
};

/*
 * Store the callbacks, then register. They must be in place first because
 * register_trusted_key_source() calls init() synchronously.
 */
int sep_tk_register(sep_tk_init_fn init, sep_tk_seal_fn seal,
		       sep_tk_unseal_fn unseal, sep_tk_random_fn random,
		       sep_tk_exit_fn exit)
{
	rs_init   = init;
	rs_seal   = seal;
	rs_unseal = unseal;
	rs_random = random;
	rs_exit   = exit;
	return register_trusted_key_source(&sep_tk_source);
}

void sep_tk_unregister(void)
{
	unregister_trusted_key_source(&sep_tk_source);
	rs_init   = NULL;
	rs_seal   = NULL;
	rs_unseal = NULL;
	rs_random = NULL;
	rs_exit   = NULL;
}

/*
 * Key-type lifecycle, owned by the source's init()/exit(), as the in-tree
 * trusted-key sources (dcp, pkwm, tpm) do.
 */
int sep_tk_register_key_type(void)
{
	return register_key_type(&key_type_trusted);
}

void sep_tk_unregister_key_type(void)
{
	unregister_key_type(&key_type_trusted);
}

/* Compile-time facts the Rust side bounds-checks against. */
size_t sep_tk_max_key_size(void)
{
	return MAX_KEY_SIZE;
}

size_t sep_tk_max_blob_size(void)
{
	return MAX_BLOB_SIZE;
}

/*
 * Payload accessors. The struct layout tracks MAX_KEY_SIZE / MAX_BLOB_SIZE, so
 * these wrappers keep the offsets in C where the header defines them.
 */
unsigned char *sep_tk_key_ptr(struct trusted_key_payload *p)
{
	return p->key;
}

unsigned int sep_tk_key_len(const struct trusted_key_payload *p)
{
	return p->key_len;
}

void sep_tk_set_key_len(struct trusted_key_payload *p, unsigned int n)
{
	p->key_len = n;
}

unsigned char *sep_tk_blob_ptr(struct trusted_key_payload *p)
{
	return p->blob;
}

unsigned int sep_tk_blob_len(const struct trusted_key_payload *p)
{
	return p->blob_len;
}

void sep_tk_set_blob_len(struct trusted_key_payload *p, unsigned int n)
{
	p->blob_len = n;
}
