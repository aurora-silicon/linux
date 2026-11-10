// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/sched.h>

#include "kexec_internal.h"

struct kexec_block_test_state {
	unsigned int acquisitions;
	struct kexec_blocker blocker;
};

static void kexec_block_test_cleanup(void *data)
{
	struct kexec_block_test_state *state = data;

	while (state->acquisitions) {
		kexec_unblock(&state->blocker);
		state->acquisitions--;
	}
}

static int kexec_block_test_init(struct kunit *test)
{
	struct kexec_block_test_state *state;
	bool blocked;

	if (!kexec_test_load_limits_unlimited())
		kunit_skip(test, "a finite load-attempt budget is configured");
	/* Never disturb a real device's retained-memory acquisition. */
	if (!kexec_trylock())
		kunit_skip(test, "a kexec operation is in progress");
	blocked = kexec_blocked();
	kexec_unlock();
	if (blocked)
		kunit_skip(test, "a device already blocks kexec");
	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->blocker.reason = "KUnit retained-memory guard";
	test->priv = state;
	return kunit_add_action_or_reset(test, kexec_block_test_cleanup, state);
}

static void kexec_test_acquire(struct kunit *test)
{
	struct kexec_block_test_state *state = test->priv;
	int ret = kexec_block(&state->blocker);

	KUNIT_ASSERT_EQ(test, ret, 0);
	state->acquisitions++;
}

static void kexec_test_release(struct kunit *test)
{
	struct kexec_block_test_state *state = test->priv;

	KUNIT_ASSERT_GT(test, state->acquisitions, 0);
	kexec_unblock(&state->blocker);
	state->acquisitions--;
}

static void kexec_test_expect_blocked(struct kunit *test, bool expected)
{
	bool blocked;

	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	blocked = kexec_blocked();
	kexec_unlock();
	KUNIT_EXPECT_EQ(test, blocked, expected);
}

static void kexec_block_balanced_test(struct kunit *test)
{
	kexec_test_acquire(test);
	kexec_test_expect_blocked(test, true);
	kexec_test_release(test);
	kexec_test_expect_blocked(test, false);
}

static void kexec_block_nested_test(struct kunit *test)
{
	kexec_test_acquire(test);
	kexec_test_acquire(test);
	kexec_test_release(test);
	kexec_test_expect_blocked(test, true);
	kexec_test_release(test);
	kexec_test_expect_blocked(test, false);
}

static void kexec_block_busy_test(struct kunit *test)
{
	struct kexec_block_test_state *state = test->priv;
	int ret;

	kexec_test_acquire(test);
	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	ret = kexec_block(&state->blocker);
	kexec_unlock();
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	/* A failed acquisition must not leave an extra reference behind. */
	kexec_test_release(test);
	kexec_test_expect_blocked(test, false);
}

static void kexec_block_execution_test(struct kunit *test)
{
	bool image_loaded;

	kexec_test_acquire(test);
	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	image_loaded = !!kexec_image;
	kexec_unlock();
	/*
	 * New loads are blocked during this test. Require an empty image so
	 * even a broken execution guard cannot transition to another kernel.
	 * The blocker must take precedence over the no-image -EINVAL result.
	 */
	if (image_loaded)
		kunit_skip(test, "a normal kexec image is already loaded");
	KUNIT_EXPECT_EQ(test, kernel_kexec(), -EOPNOTSUPP);
	kexec_test_release(test);
}

static void kexec_test_require_empty_images(struct kunit *test)
{
	bool loaded;

	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	loaded = kexec_image || kexec_crash_image;
	kexec_unlock();
	/* The test guard prevents new loads; never unload a real image. */
	if (loaded)
		kunit_skip(test, "a normal or crash image is already loaded");
}

#ifdef CONFIG_KEXEC
static void kexec_block_legacy_load_test(struct kunit *test)
{
	/* Bad alignment also makes a missing guard fail safely in validation. */
	struct kexec_segment segment = { .mem = 1, .memsz = PAGE_SIZE };
	int ret;

	kexec_test_acquire(test);
	kexec_test_require_empty_images(test);
	KUNIT_EXPECT_EQ(test, do_kexec_load(0, 1, &segment, 0), -EOPNOTSUPP);
	/* A crash kernel only passes the guard without CMA crash memory. */
	ret = do_kexec_load(0, 1, &segment, KEXEC_ON_CRASH);
	if (crashk_cma_cnt)
		KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	else
		KUNIT_EXPECT_EQ(test, ret, -EADDRNOTAVAIL);
	/* Unload an empty slot through the actual loader while still blocked. */
	KUNIT_EXPECT_EQ(test, do_kexec_load(0, 0, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, do_kexec_load(0, 0, NULL, KEXEC_ON_CRASH), 0);
	kexec_test_release(test);
}
#endif

#ifdef CONFIG_KEXEC_FILE
static void kexec_block_file_load_test(struct kunit *test)
{
	int ret;

	if (!current->files)
		kunit_skip(test, "no file table for safe invalid-fd validation");
	kexec_test_acquire(test);
	kexec_test_require_empty_images(test);
	/* Invalid fds cannot create an image even if the veto regresses. */
	ret = do_kexec_file_load(-1, -1, 0, NULL, 0);
	if (ret == -EPERM)
		kunit_skip(test, "file loads disabled by permissions or attempt limit");
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	/* Past the guard, the invalid fd stops a crash load before any image. */
	KUNIT_EXPECT_EQ(test,
			do_kexec_file_load(-1, -1, 0, NULL, KEXEC_FILE_ON_CRASH),
			crashk_cma_cnt ? -EOPNOTSUPP : -EBADF);
	KUNIT_EXPECT_EQ(test, do_kexec_file_load(-1, -1, 0, NULL, KEXEC_FILE_UNLOAD), 0);
	KUNIT_EXPECT_EQ(test,
			do_kexec_file_load(-1, -1, 0, NULL,
					   KEXEC_FILE_UNLOAD | KEXEC_FILE_ON_CRASH), 0);
	kexec_test_release(test);
}
#endif

static void kexec_block_crash_policy_test(struct kunit *test)
{
	bool empty_allowed, loaded_allowed, blocked;

	kexec_test_acquire(test);
	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	/* Exercise the real crash selector without installing any fake image. */
	empty_allowed = kexec_crash_image_allowed(false);
	loaded_allowed = kexec_crash_image_allowed(true);
	kexec_unlock();
	KUNIT_EXPECT_FALSE(test, empty_allowed);
	/* Retained memory never overlaps a memblock crash reservation. */
	KUNIT_EXPECT_EQ(test, loaded_allowed, !crashk_cma_cnt);
	kexec_test_release(test);
	KUNIT_ASSERT_TRUE(test, kexec_trylock());
	/* A real guard may have appeared since this case's initial skip check. */
	blocked = kexec_blocked();
	empty_allowed = kexec_crash_image_allowed(false);
	loaded_allowed = kexec_crash_image_allowed(true);
	kexec_unlock();
	KUNIT_EXPECT_FALSE(test, empty_allowed);
	KUNIT_EXPECT_EQ(test, loaded_allowed, !(blocked && crashk_cma_cnt));
}

static struct kunit_case kexec_block_test_cases[] = {
	KUNIT_CASE(kexec_block_balanced_test),
	KUNIT_CASE(kexec_block_nested_test),
	KUNIT_CASE(kexec_block_busy_test),
	KUNIT_CASE(kexec_block_execution_test),
#ifdef CONFIG_KEXEC
	KUNIT_CASE(kexec_block_legacy_load_test),
#endif
#ifdef CONFIG_KEXEC_FILE
	KUNIT_CASE(kexec_block_file_load_test),
#endif
	KUNIT_CASE(kexec_block_crash_policy_test),
	{}
};

static struct kunit_suite kexec_block_test_suite = {
	.name = "kexec-block",
	.init = kexec_block_test_init,
	.test_cases = kexec_block_test_cases,
};

kunit_test_suite(kexec_block_test_suite);
