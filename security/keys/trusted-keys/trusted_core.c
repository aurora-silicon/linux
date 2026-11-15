// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2010 IBM Corporation
 * Copyright (c) 2019-2021, Linaro Limited
 *
 * See Documentation/security/keys/trusted-encrypted.rst
 */

#include <keys/user-type.h>
#include <keys/trusted-type.h>
#include <keys/trusted_tee.h>
#include <keys/trusted_caam.h>
#include <keys/trusted_dcp.h>
#include <keys/trusted_tpm.h>
#include <keys/trusted_pkwm.h>
#include <kunit/visibility.h>
#include <linux/capability.h>
#include <linux/err.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/key-type.h>
#include <linux/module.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/static_call.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static char *trusted_rng = "default";
module_param_named(rng, trusted_rng, charp, 0);
MODULE_PARM_DESC(rng, "Select trusted key RNG");

#ifdef CONFIG_TRUSTED_KEYS_DEBUG
bool trusted_debug;
module_param_named(debug, trusted_debug, bool, 0);
MODULE_PARM_DESC(debug, "Enable trusted keys debug traces (default: 0)");
#endif

static char *trusted_key_source;
module_param_named(source, trusted_key_source, charp, 0);
MODULE_PARM_DESC(source, "Select trusted keys source (tpm, tee, caam, dcp or pkwm)");

static const struct trusted_key_source trusted_key_sources[] = {
#if defined(CONFIG_TRUSTED_KEYS_TPM)
	{ "tpm", &trusted_key_tpm_ops },
#endif
#if defined(CONFIG_TRUSTED_KEYS_TEE)
	{ "tee", &trusted_key_tee_ops },
#endif
#if defined(CONFIG_TRUSTED_KEYS_CAAM)
	{ "caam", &trusted_key_caam_ops },
#endif
#if defined(CONFIG_TRUSTED_KEYS_DCP)
	{ "dcp", &dcp_trusted_key_ops },
#endif
#if defined(CONFIG_TRUSTED_KEYS_PKWM)
	{ "pkwm", &pkwm_trusted_key_ops },
#endif
};

DEFINE_STATIC_CALL_NULL(trusted_key_seal, *trusted_key_sources[0].ops->seal);
DEFINE_STATIC_CALL_NULL(trusted_key_unseal,
			*trusted_key_sources[0].ops->unseal);
DEFINE_STATIC_CALL_NULL(trusted_key_get_random,
			*trusted_key_sources[0].ops->get_random);
static void (*trusted_key_exit)(void);
static unsigned char migratable __ro_after_init;

static DEFINE_MUTEX(trusted_key_source_lock);
static bool trusted_key_source_active;
static struct trusted_key_source *trusted_key_source_registered;
/*
 * trusted_key_exit and migratable belong to the built-in source and only
 * init_trusted() sets them; migratable is read-only after init. A source
 * registered later keeps its migration policy here, -1 while there is none.
 */
static int trusted_key_source_migratable = -1;
/* trusted_key_core_init() has run once, from init_trusted(). */
static bool trusted_key_core_done;

/* Defined below; registration needs it before its definition. */
static int kernel_get_random(unsigned char *key, size_t key_len);

/*
 * Dispatch targets for when no source is registered. The call sites use
 * static_call() directly, so the pointers must always be valid code; a NULL
 * target would fault instead of returning an error.
 */
static int trusted_key_absent_seal(struct trusted_key_payload *p, char *datablob)
{
	return -ENODEV;
}

static int trusted_key_absent_unseal(struct trusted_key_payload *p, char *datablob)
{
	return -ENODEV;
}

static int trusted_key_absent_get_random(unsigned char *key, size_t key_len)
{
	return -ENODEV;
}

/**
 * register_trusted_key_source - offer a trust source from a module
 * @src: the source. Must outlive its registration.
 *
 * Returns 0 on success, -EBUSY if a source is already active, -ENODEV if
 * trusted.source= names a different one, or the source's own init() error.
 */
int register_trusted_key_source(struct trusted_key_source *src)
{
	int (*get_random)(unsigned char *key, size_t key_len);
	int ret;

	if (!src || !src->name || !src->ops || !src->ops->init ||
	    !src->ops->seal || !src->ops->unseal)
		return -EINVAL;

	/* An explicit trusted.source= names one source; honour it. */
	if (trusted_key_source && strcmp(trusted_key_source, src->name))
		return -ENODEV;

	mutex_lock(&trusted_key_source_lock);

	if (trusted_key_source_active) {
		mutex_unlock(&trusted_key_source_lock);
		return -EBUSY;
	}

	get_random = src->ops->get_random;
	if (trusted_rng && strcmp(trusted_rng, "default")) {
		if (!strcmp(trusted_rng, "kernel")) {
			get_random = kernel_get_random;
		} else if (strcmp(trusted_rng, src->name) || !get_random) {
			mutex_unlock(&trusted_key_source_lock);
			return -EINVAL;
		}
	}
	if (!get_random)
		get_random = kernel_get_random;

	ret = src->ops->init();
	if (ret) {
		mutex_unlock(&trusted_key_source_lock);
		return ret;
	}

	static_call_update(trusted_key_seal, src->ops->seal);
	static_call_update(trusted_key_unseal, src->ops->unseal);
	static_call_update(trusted_key_get_random, get_random);
	WRITE_ONCE(trusted_key_source_migratable, src->ops->migratable);
	trusted_key_source_active = true;
	trusted_key_source_registered = src;

	mutex_unlock(&trusted_key_source_lock);
	pr_info("trusted_key: source '%s' registered\n", src->name);
	return 0;
}
EXPORT_SYMBOL_GPL(register_trusted_key_source);

/**
 * unregister_trusted_key_source - withdraw a previously registered source
 * @src: the source that was registered.
 *
 * Keys sealed by @src stay in the keyring and stop being usable, which is the
 * intended outcome: the thing that could unseal them is gone.
 */
void unregister_trusted_key_source(struct trusted_key_source *src)
{
	mutex_lock(&trusted_key_source_lock);

	/* Only the source that registered may withdraw itself. */
	if (!trusted_key_source_active || src != trusted_key_source_registered) {
		mutex_unlock(&trusted_key_source_lock);
		return;
	}

	static_call_update(trusted_key_seal, trusted_key_absent_seal);
	static_call_update(trusted_key_unseal, trusted_key_absent_unseal);
	static_call_update(trusted_key_get_random, trusted_key_absent_get_random);

	if (src->ops->exit)
		src->ops->exit();
	WRITE_ONCE(trusted_key_source_migratable, -1);
	trusted_key_source_active = false;
	trusted_key_source_registered = NULL;

	mutex_unlock(&trusted_key_source_lock);
	pr_info("trusted_key: source '%s' unregistered\n",
		src && src->name ? src->name : "?");
}
EXPORT_SYMBOL_GPL(unregister_trusted_key_source);

enum {
	Opt_err,
	Opt_new, Opt_import, Opt_load, Opt_update,
};

static const match_table_t key_tokens = {
	{Opt_new, "new"},
	{Opt_import, "import"},
	{Opt_load, "load"},
	{Opt_update, "update"},
	{Opt_err, NULL}
};

/*
 * datablob_parse - parse the keyctl data and fill in the
 *                  payload structure
 *
 * On success returns 0, otherwise -EINVAL.
 */
static int datablob_parse(char **datablob, struct trusted_key_payload *p)
{
	substring_t args[MAX_OPT_ARGS];
	long keylen;
	int ret = -EINVAL;
	int key_cmd;
	char *c;

	/* main command */
	c = strsep(datablob, " \t");
	if (!c)
		return -EINVAL;
	key_cmd = match_token(c, key_tokens, args);
	switch (key_cmd) {
	case Opt_new:
		/* first argument is key size */
		c = strsep(datablob, " \t");
		if (!c)
			return -EINVAL;
		ret = kstrtol(c, 10, &keylen);
		if (ret < 0 || keylen < MIN_KEY_SIZE || keylen > MAX_KEY_SIZE)
			return -EINVAL;
		p->key_len = keylen;
		ret = Opt_new;
		break;
	case Opt_import:
		c = strsep(datablob, " \t");
		if (!c || strlen(c) % 2)
			return -EINVAL;
		keylen = strlen(c) / 2;
		if (keylen < MIN_KEY_SIZE || keylen > MAX_KEY_SIZE)
			return -EINVAL;
		ret = hex2bin(p->key, c, keylen);
		if (ret < 0)
			return -EINVAL;
		p->key_len = keylen;
		ret = Opt_import;
		break;
	case Opt_load:
		/* first argument is sealed blob */
		c = strsep(datablob, " \t");
		if (!c)
			return -EINVAL;
		p->blob_len = strlen(c) / 2;
		if (p->blob_len > MAX_BLOB_SIZE)
			return -EINVAL;
		ret = hex2bin(p->blob, c, p->blob_len);
		if (ret < 0)
			return -EINVAL;
		ret = Opt_load;
		break;
	case Opt_update:
		ret = Opt_update;
		break;
	case Opt_err:
		return -EINVAL;
	}
	return ret;
}

/* The migration policy of the source that seals new keys. */
static unsigned char trusted_key_migratable(void)
{
	int m = READ_ONCE(trusted_key_source_migratable);

	return m < 0 ? migratable : m;
}

static struct trusted_key_payload *trusted_payload_alloc(struct key *key)
{
	struct trusted_key_payload *p = NULL;
	int ret;

	ret = key_payload_reserve(key, sizeof(*p));
	if (ret < 0)
		goto err;
	p = kzalloc_obj(*p);
	if (!p)
		goto err;

	p->migratable = trusted_key_migratable();
err:
	return p;
}

/*
 * trusted_instantiate - create a new trusted key
 *
 * Unseal an existing trusted blob or, for a new key, get a
 * random key, then seal and create a trusted key-type key,
 * adding it to the specified keyring.
 *
 * On success, return 0. Otherwise return errno.
 */
static int trusted_instantiate(struct key *key,
			       struct key_preparsed_payload *prep)
{
	struct trusted_key_payload *payload = NULL;
	size_t datalen = prep->datalen;
	char *datablob, *orig_datablob;
	int ret = 0;
	int key_cmd;
	size_t key_len;

	if (datalen == 0 || datalen > 32767 || !prep->data)
		return -EINVAL;

	orig_datablob = datablob = kmalloc(datalen + 1, GFP_KERNEL);
	if (!datablob)
		return -ENOMEM;
	memcpy(datablob, prep->data, datalen);
	datablob[datalen] = '\0';

	payload = trusted_payload_alloc(key);
	if (!payload) {
		ret = -ENOMEM;
		goto out;
	}

	key_cmd = datablob_parse(&datablob, payload);
	if (key_cmd < 0) {
		ret = key_cmd;
		goto out;
	}

	dump_payload(payload);

	switch (key_cmd) {
	case Opt_load:
		ret = static_call(trusted_key_unseal)(payload, datablob);
		dump_payload(payload);
		if (ret < 0)
			pr_info("key_unseal failed (%d)\n", ret);
		break;
	case Opt_new:
		key_len = payload->key_len;
		ret = static_call(trusted_key_get_random)(payload->key,
							  key_len);
		if (ret < 0)
			goto out;

		if (ret != key_len) {
			pr_info("key_create failed (%d)\n", ret);
			ret = -EIO;
			goto out;
		}

		ret = static_call(trusted_key_seal)(payload, datablob);
		if (ret < 0)
			pr_info("key_seal failed (%d)\n", ret);
		break;
	case Opt_import:
		ret = static_call(trusted_key_seal)(payload, datablob);
		if (ret < 0)
			pr_info("key_seal failed (%d)\n", ret);
		break;
	default:
		ret = -EINVAL;
	}
out:
	kfree_sensitive(orig_datablob);
	if (!ret)
		rcu_assign_keypointer(key, payload);
	else
		kfree_sensitive(payload);
	return ret;
}

static void trusted_rcu_free(struct rcu_head *rcu)
{
	struct trusted_key_payload *p;

	p = container_of(rcu, struct trusted_key_payload, rcu);
	kfree_sensitive(p);
}

/*
 * trusted_update - reseal an existing key with new PCR values
 */
static int trusted_update(struct key *key, struct key_preparsed_payload *prep)
{
	struct trusted_key_payload *p;
	struct trusted_key_payload *new_p;
	size_t datalen = prep->datalen;
	char *datablob, *orig_datablob;
	int ret = 0;

	if (key_is_negative(key))
		return -ENOKEY;
	p = key->payload.data[0];
	if (!p->migratable)
		return -EPERM;
	if (datalen == 0 || datalen > 32767 || !prep->data)
		return -EINVAL;

	orig_datablob = datablob = kmalloc(datalen + 1, GFP_KERNEL);
	if (!datablob)
		return -ENOMEM;

	new_p = trusted_payload_alloc(key);
	if (!new_p) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(datablob, prep->data, datalen);
	datablob[datalen] = '\0';
	ret = datablob_parse(&datablob, new_p);
	if (ret != Opt_update) {
		ret = -EINVAL;
		kfree_sensitive(new_p);
		goto out;
	}

	/* copy old key values, and reseal with new pcrs */
	new_p->migratable = p->migratable;
	new_p->key_len = p->key_len;
	memcpy(new_p->key, p->key, p->key_len);
	dump_payload(p);
	dump_payload(new_p);

	ret = static_call(trusted_key_seal)(new_p, datablob);
	if (ret < 0) {
		pr_info("key_seal failed (%d)\n", ret);
		kfree_sensitive(new_p);
		goto out;
	}

	rcu_assign_keypointer(key, new_p);
	call_rcu(&p->rcu, trusted_rcu_free);
out:
	kfree_sensitive(orig_datablob);
	return ret;
}

/*
 * trusted_read - copy the sealed blob data to userspace in hex.
 * On success, return to userspace the trusted key datablob size.
 */
static long trusted_read(const struct key *key, char *buffer,
			 size_t buflen)
{
	const struct trusted_key_payload *p;
	char *bufp;
	int i;

	p = dereference_key_locked(key);
	if (!p)
		return -EINVAL;

	if (buffer && buflen >= 2 * p->blob_len) {
		bufp = buffer;
		for (i = 0; i < p->blob_len; i++)
			bufp = hex_byte_pack(bufp, p->blob[i]);
	}
	return 2 * p->blob_len;
}

/*
 * trusted_destroy - clear and free the key's payload
 */
static void trusted_destroy(struct key *key)
{
	kfree_sensitive(key->payload.data[0]);
}

struct key_type key_type_trusted = {
	.name = "trusted",
	.instantiate = trusted_instantiate,
	.update = trusted_update,
	.destroy = trusted_destroy,
	.describe = user_describe,
	.read = trusted_read,
};
EXPORT_SYMBOL_GPL(key_type_trusted);

static int kernel_get_random(unsigned char *key, size_t key_len)
{
	return get_random_bytes_wait(key, key_len) ?: key_len;
}

/**
 * trusted_key_core_init - give the source slot to a built-in source
 * @sources: the built-in sources, in order of preference
 * @n: the number of @sources
 *
 * A source module that is built in, such as apple_sep, can register from
 * its probe at device_initcall, before init_trusted() runs at
 * late_initcall. Such a source keeps the slot: the built-in sources are
 * not tried, and the dispatch stays with it. Otherwise the first built-in
 * source to initialise takes the slot, and with none the dispatch goes to
 * the absent stubs, so that a source module can register later.
 *
 * init_trusted() calls this once, with trusted_key_sources. The KUnit tests
 * call it later, with synthetic sources of their own. Only the first call
 * gives the slot to a built-in source: one that takes it is never withdrawn,
 * as unregister_trusted_key_source() withdraws only a registered source,
 * and a test's source is gone once the test ends.
 *
 * Return: 0, the error of trusted.rng or of a built-in source's init(), or
 * -EBUSY for a built-in source that initialises after the first call.
 */
VISIBLE_IF_KUNIT int
trusted_key_core_init(const struct trusted_key_source *sources, size_t n)
{
	int (*get_random)(unsigned char *key, size_t key_len);
	/* -ENODEV when the built-in array is empty or nothing matches, so a
	 * later module source can still register instead of hitting -EBUSY.
	 */
	int ret = -ENODEV;
	size_t i;

	mutex_lock(&trusted_key_source_lock);

	/* A source module that is built in may have registered first. */
	if (trusted_key_source_active) {
		ret = 0;
		goto out;
	}

	for (i = 0; i < n; i++) {
		if (trusted_key_source &&
		    strncmp(trusted_key_source, sources[i].name,
			    strlen(sources[i].name)))
			continue;

		/*
		 * We always support trusted.rng="kernel" and "default" as
		 * well as trusted.rng=$trusted.source if the trust source
		 * defines its own get_random callback.
		 */
		get_random = sources[i].ops->get_random;
		if (trusted_rng && strcmp(trusted_rng, "default")) {
			if (!strcmp(trusted_rng, "kernel")) {
				get_random = kernel_get_random;
			} else if (strcmp(trusted_rng, sources[i].name) ||
				   !get_random) {
				pr_warn("Unsupported RNG. Supported: kernel");
				if (get_random)
					pr_cont(", %s", sources[i].name);
				pr_cont(", default\n");
				ret = -EINVAL;
				goto out;
			}
		}

		if (!get_random)
			get_random = kernel_get_random;

		ret = sources[i].ops->init();
		if (!ret && trusted_key_core_done) {
			if (sources[i].ops->exit)
				sources[i].ops->exit();
			ret = -EBUSY;
			goto out;
		}
		if (!ret) {
			static_call_update(trusted_key_seal, sources[i].ops->seal);
			static_call_update(trusted_key_unseal, sources[i].ops->unseal);
			static_call_update(trusted_key_get_random, get_random);

			trusted_key_exit = sources[i].ops->exit;
			migratable = sources[i].ops->migratable;
		}

		if (!ret || ret != -ENODEV)
			break;
	}

	/*
	 * encrypted_keys.ko depends on successful load of this module even if
	 * trusted key implementation is not found.
	 */
	if (ret == -ENODEV) {
		/*
		 * No built-in source. Leave the dispatch pointing at code that
		 * fails rather than at NULL, so a seal attempted before any
		 * module registers returns an error instead of faulting.
		 */
		static_call_update(trusted_key_seal, trusted_key_absent_seal);
		static_call_update(trusted_key_unseal, trusted_key_absent_unseal);
		static_call_update(trusted_key_get_random,
				   trusted_key_absent_get_random);
		ret = 0;
		goto out;
	}

	if (!ret)
		trusted_key_source_active = true;
out:
	trusted_key_core_done = true;
	mutex_unlock(&trusted_key_source_lock);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(trusted_key_core_init);

static int __init init_trusted(void)
{
	return trusted_key_core_init(trusted_key_sources,
				     ARRAY_SIZE(trusted_key_sources));
}

static void __exit cleanup_trusted(void)
{
	if (trusted_key_exit)
		(*trusted_key_exit)();
}

late_initcall(init_trusted);
module_exit(cleanup_trusted);

MODULE_DESCRIPTION("Trusted Key type");
MODULE_LICENSE("GPL");
