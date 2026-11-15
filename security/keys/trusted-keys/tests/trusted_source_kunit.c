// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for trusted-key sources registered at runtime, through
 * register_trusted_key_source() and unregister_trusted_key_source(), before
 * and after the core's own init.
 *
 * The source under test is synthetic: it touches no hardware and seals
 * nothing for real. A test is skipped when another source holds the slot.
 *
 * Copyright 2026 Ryan Murray
 */

#include <keys/trusted-type.h>
#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/hex.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/string.h>

#define FAKE_NAME "trusted_source_kunit"
#define FAKE_BUILTIN_NAME "trusted_source_kunit_builtin"

struct trusted_source_fake {
	struct trusted_key_ops ops;
	struct trusted_key_source src;
	int init_ret;
	int inits, exits, seals, unseals;
	/* A built-in source for trusted_key_core_init(). */
	struct trusted_key_ops builtin_ops;
	struct trusted_key_source builtin;
	int builtin_ret;
	int builtin_inits;
};

/* The ops have no context argument; the tests run one at a time. */
static struct trusted_source_fake *fake;

static int fake_init(void)
{
	fake->inits++;
	return fake->init_ret;
}

static void fake_exit(void)
{
	fake->exits++;
}

/* A built-in source that finds no device, as each of them does on Apple. */
static int fake_builtin_init(void)
{
	fake->builtin_inits++;
	return fake->builtin_ret;
}

/* Not a cipher: an invertible stand-in, so that unseal can be checked. */
static int fake_seal(struct trusted_key_payload *p, char *datablob)
{
	unsigned int i;

	for (i = 0; i < p->key_len; i++)
		p->blob[i] = p->key[i] ^ 0x5a;
	p->blob_len = p->key_len;
	fake->seals++;
	return 0;
}

static int fake_unseal(struct trusted_key_payload *p, char *datablob)
{
	unsigned int i;

	if (p->blob_len < MIN_KEY_SIZE || p->blob_len > MAX_KEY_SIZE)
		return -EINVAL;
	for (i = 0; i < p->blob_len; i++)
		p->key[i] = p->blob[i] ^ 0x5a;
	p->key_len = p->blob_len;
	fake->unseals++;
	return 0;
}

static int fake_get_random(unsigned char *key, size_t key_len)
{
	size_t i;

	for (i = 0; i < key_len; i++)
		key[i] = i;
	return key_len;
}

KUNIT_DEFINE_ACTION_WRAPPER(trusted_source_withdraw,
			    unregister_trusted_key_source,
			    struct trusted_key_source *);
KUNIT_DEFINE_ACTION_WRAPPER(trusted_source_key_put, key_put, struct key *);

/* Register the fake, or skip if another source owns or claims the slot. */
static void fake_register(struct kunit *test)
{
	int ret = register_trusted_key_source(&fake->src);

	if (ret == -EBUSY || ret == -ENODEV)
		kunit_skip(test, "another trusted-key source holds the slot (%d)",
			   ret);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

/*
 * Run the core's init with the built-in fake, or skip, giving the reason, if
 * the fake is not tried: a source holds the slot, or trusted.source= or
 * trusted.rng= rules the fake out.
 */
static int fake_core_init(struct kunit *test)
{
	int ret = trusted_key_core_init(&fake->builtin, 1);

	if (fake->builtin_inits)
		return ret;

	/* The only error before a source's init(). */
	if (ret == -EINVAL)
		kunit_skip(test, "trusted.rng= rules out the built-in source");
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = register_trusted_key_source(&fake->src);
	if (!ret)
		unregister_trusted_key_source(&fake->src);
	if (ret == -EBUSY)
		kunit_skip(test, "another trusted-key source holds the slot");
	kunit_skip(test, "trusted.source= rules out the built-in source");
	return 0;
}

/* Instantiate a trusted key from @cmd; it is put when the test ends. */
static struct key *fake_key(struct kunit *test, const char *cmd)
{
	struct key *key;
	int ret;

	key = key_alloc(&key_type_trusted, FAKE_NAME, GLOBAL_ROOT_UID,
			GLOBAL_ROOT_GID, current_cred(), KEY_POS_ALL,
			KEY_ALLOC_NOT_IN_QUOTA, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, key);

	ret = key_instantiate_and_link(key, cmd, strlen(cmd), NULL, NULL);
	if (ret) {
		key_put(key);
		return ERR_PTR(ret);
	}
	ret = kunit_add_action_or_reset(test, trusted_source_key_put, key);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return key;
}

static unsigned char fake_key_migratable(struct key *key)
{
	struct trusted_key_payload *p;
	unsigned char migratable;

	down_read(&key->sem);
	p = dereference_key_locked(key);
	migratable = p->migratable;
	up_read(&key->sem);
	return migratable;
}

/*
 * Built in, KUnit runs before mark_readonly(), while read-only-after-init
 * data can still be written. Loaded as a module after boot, as apple_sep
 * is, every registration in this suite runs with that protection on.
 */
static void trusted_source_after_mark_readonly(struct kunit *test)
{
	int i;

	if (system_state < SYSTEM_RUNNING)
		kunit_skip(test, "built in: runs before mark_readonly()");

	for (i = 0; i < 2; i++) {
		fake_register(test);
		KUNIT_EXPECT_FALSE(test, IS_ERR(fake_key(test, "new 32")));
		unregister_trusted_key_source(&fake->src);
	}
	KUNIT_EXPECT_EQ(test, fake->inits, 2);
	KUNIT_EXPECT_EQ(test, fake->exits, 2);
	KUNIT_EXPECT_EQ(test, fake->seals, 2);
}

/*
 * A source module that is built in, as apple_sep can be, registers from its
 * probe at device_initcall, before init_trusted() runs at late_initcall.
 * The core then leaves it the slot and the dispatch.
 */
static void trusted_source_before_core_init(struct kunit *test)
{
	struct key *key;

	fake->ops.migratable = 1;
	fake_register(test);

	KUNIT_EXPECT_EQ(test, trusted_key_core_init(&fake->builtin, 1), 0);
	KUNIT_EXPECT_EQ(test, fake->builtin_inits, 0);

	key = fake_key(test, "new 32");
	KUNIT_ASSERT_FALSE(test, IS_ERR(key));
	KUNIT_EXPECT_EQ(test, fake->seals, 1);
	KUNIT_EXPECT_EQ(test, fake_key_migratable(key), 1);

	/* The source still holds the slot, so it can withdraw. */
	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, "new 32")), -ENODEV);
	KUNIT_EXPECT_EQ(test, fake->seals, 1);
}

/*
 * A source module registers after the core's init found no built-in
 * source, as a modular apple_sep does.
 */
static void trusted_source_after_core_init(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, fake_core_init(test), 0);

	/* With no source, the absent stubs answer. */
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, "new 32")), -ENODEV);

	fake_register(test);
	KUNIT_EXPECT_FALSE(test, IS_ERR(fake_key(test, "new 32")));
	KUNIT_EXPECT_EQ(test, fake->seals, 1);

	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->inits, 1);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);
	KUNIT_EXPECT_EQ(test, fake->builtin_inits, 1);
}

/*
 * After the core's init, a built-in source is not given the slot, even one
 * that initialises: nothing could withdraw it, and this one is gone once the
 * test ends. The slot stays free for a source module.
 */
static void trusted_source_builtin_after_core_init(struct kunit *test)
{
	fake->builtin_ret = 0;
	KUNIT_EXPECT_EQ(test, fake_core_init(test), -EBUSY);
	KUNIT_EXPECT_EQ(test, fake->builtin_inits, 1);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, "new 32")), -ENODEV);
	KUNIT_EXPECT_EQ(test, fake->seals, 0);

	/* The fake was tried, so no source held the slot: no skip here. */
	KUNIT_ASSERT_EQ(test, register_trusted_key_source(&fake->src), 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR(fake_key(test, "new 32")));
	KUNIT_EXPECT_EQ(test, fake->seals, 1);
	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->exits, 2);
}

/* Keys are sealed and unsealed by the source until it withdraws. */
static void trusted_source_dispatch(struct kunit *test)
{
	struct trusted_key_payload *p;
	unsigned char *plain;
	unsigned int len;
	struct key *key;
	char *load;

	plain = kunit_kzalloc(test, MAX_KEY_SIZE, GFP_KERNEL);
	load = kunit_kzalloc(test, sizeof("load ") + 2 * MAX_BLOB_SIZE,
			     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	KUNIT_ASSERT_NOT_NULL(test, load);

	fake_register(test);
	KUNIT_EXPECT_EQ(test, fake->inits, 1);

	key = fake_key(test, "new 32");
	KUNIT_ASSERT_FALSE(test, IS_ERR(key));
	KUNIT_EXPECT_EQ(test, fake->seals, 1);

	down_read(&key->sem);
	p = dereference_key_locked(key);
	len = min_t(unsigned int, p->key_len, MAX_KEY_SIZE);
	memcpy(plain, p->key, len);
	memcpy(load, "load ", 5);
	bin2hex(load + 5, p->blob,
		min_t(unsigned int, p->blob_len, MAX_BLOB_SIZE));
	up_read(&key->sem);
	KUNIT_EXPECT_EQ(test, len, 32U);

	key = fake_key(test, load);
	KUNIT_ASSERT_FALSE(test, IS_ERR(key));
	KUNIT_EXPECT_EQ(test, fake->unseals, 1);

	down_read(&key->sem);
	p = dereference_key_locked(key);
	KUNIT_EXPECT_EQ(test, p->key_len, len);
	KUNIT_EXPECT_MEMEQ(test, p->key, plain, len);
	up_read(&key->sem);

	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);

	/* Dispatch is back on the absent stubs, not on the departed source. */
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, "new 32")), -ENODEV);
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, load)), -ENODEV);
	KUNIT_EXPECT_EQ(test, fake->seals, 1);
	KUNIT_EXPECT_EQ(test, fake->unseals, 1);
}

/* Each registration brings its own policy; a withdrawn one leaves none. */
static void trusted_source_migratable(struct kunit *test)
{
	static const unsigned char policy[] = { 1, 0, 1 };
	struct key *key;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(policy); i++) {
		fake->ops.migratable = policy[i];
		fake_register(test);

		key = fake_key(test, "new 32");
		KUNIT_ASSERT_FALSE(test, IS_ERR(key));
		KUNIT_EXPECT_EQ(test, fake_key_migratable(key), policy[i]);

		/* Only a migratable key may be resealed. */
		KUNIT_EXPECT_EQ(test, key_update(make_key_ref(key, true),
						 "update", 6),
				policy[i] ? 0 : -EPERM);
		KUNIT_EXPECT_EQ(test, fake_key_migratable(key), policy[i]);

		unregister_trusted_key_source(&fake->src);
	}
	KUNIT_EXPECT_EQ(test, fake->seals, 3 + 2);
}

/* A second source is refused, and cannot withdraw the first. */
static void trusted_source_exclusive(struct kunit *test)
{
	struct trusted_key_source other = {
		.name = FAKE_NAME,
		.ops = &fake->ops,
	};

	fake_register(test);
	KUNIT_EXPECT_EQ(test, register_trusted_key_source(&other), -EBUSY);
	KUNIT_EXPECT_EQ(test, register_trusted_key_source(&fake->src), -EBUSY);
	KUNIT_EXPECT_EQ(test, fake->inits, 1);

	unregister_trusted_key_source(&other);
	KUNIT_EXPECT_EQ(test, fake->exits, 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR(fake_key(test, "new 32")));
	KUNIT_EXPECT_EQ(test, fake->seals, 1);

	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);
}

/* A failed init() leaves no source behind, and registration can retry. */
static void trusted_source_init_failure(struct kunit *test)
{
	int ret;

	fake->init_ret = -EIO;
	ret = register_trusted_key_source(&fake->src);
	if (ret == -EBUSY || ret == -ENODEV)
		kunit_skip(test, "another trusted-key source holds the slot (%d)",
			   ret);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake->inits, 1);
	KUNIT_EXPECT_EQ(test, fake->exits, 0);
	KUNIT_EXPECT_EQ(test, PTR_ERR(fake_key(test, "new 32")), -ENODEV);

	fake->init_ret = 0;
	fake_register(test);
	KUNIT_EXPECT_FALSE(test, IS_ERR(fake_key(test, "new 32")));
	unregister_trusted_key_source(&fake->src);
	KUNIT_EXPECT_EQ(test, fake->inits, 2);
	KUNIT_EXPECT_EQ(test, fake->exits, 1);
}

static void trusted_source_expect_invalid(struct kunit *test,
					  struct trusted_key_source *src)
{
	int ret = register_trusted_key_source(src);

	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	if (!ret)
		unregister_trusted_key_source(src);
}

/* Incomplete sources are refused before anything is called. */
static void trusted_source_invalid(struct kunit *test)
{
	struct trusted_key_ops no_seal = fake->ops;
	struct trusted_key_source no_name = { .ops = &fake->ops };
	struct trusted_key_source no_ops = { .name = FAKE_NAME };
	struct trusted_key_source bad_ops = {
		.name = FAKE_NAME,
		.ops = &no_seal,
	};

	no_seal.seal = NULL;
	trusted_source_expect_invalid(test, NULL);
	trusted_source_expect_invalid(test, &no_name);
	trusted_source_expect_invalid(test, &no_ops);
	trusted_source_expect_invalid(test, &bad_ops);
	KUNIT_EXPECT_EQ(test, fake->inits, 0);
}

static int trusted_source_init(struct kunit *test)
{
	fake = kunit_kzalloc(test, sizeof(*fake), GFP_KERNEL);
	if (!fake)
		return -ENOMEM;

	fake->ops.init = fake_init;
	fake->ops.seal = fake_seal;
	fake->ops.unseal = fake_unseal;
	fake->ops.get_random = fake_get_random;
	fake->ops.exit = fake_exit;
	fake->src.name = FAKE_NAME;
	fake->src.ops = &fake->ops;

	fake->builtin_ops = fake->ops;
	fake->builtin_ops.init = fake_builtin_init;
	fake->builtin_ret = -ENODEV;
	fake->builtin.name = FAKE_BUILTIN_NAME;
	fake->builtin.ops = &fake->builtin_ops;

	/* However the test ends; a no-op unless the fake is registered. */
	return kunit_add_action_or_reset(test, trusted_source_withdraw,
					 &fake->src);
}

static struct kunit_case trusted_source_cases[] = {
	KUNIT_CASE(trusted_source_after_mark_readonly),
	KUNIT_CASE(trusted_source_before_core_init),
	KUNIT_CASE(trusted_source_after_core_init),
	KUNIT_CASE(trusted_source_dispatch),
	KUNIT_CASE(trusted_source_migratable),
	KUNIT_CASE(trusted_source_exclusive),
	KUNIT_CASE(trusted_source_init_failure),
	KUNIT_CASE(trusted_source_invalid),
	/* Last, as a core without the check would leave the fake in the slot. */
	KUNIT_CASE(trusted_source_builtin_after_core_init),
	{}
};

static struct kunit_suite trusted_source_suite = {
	.name = "trusted_source",
	.init = trusted_source_init,
	.test_cases = trusted_source_cases,
};

kunit_test_suite(trusted_source_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for trusted-key sources registered at runtime");
MODULE_LICENSE("GPL");
