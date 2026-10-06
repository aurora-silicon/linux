// SPDX-License-Identifier: GPL-2.0-only
/* Included by type1 so the tests exercise the actual accounting helpers. */
#include <kunit/test.h>
#include <linux/pgalloc.h>

struct vfio_account_mm {
	struct mm_struct *mm;
	bool live;
};

static void vfio_account_mm_free(void *data)
{
	struct vfio_account_mm *ctx = data;

	if (ctx->live)
		mmput(ctx->mm);
	mmdrop(ctx->mm);
}

static struct vfio_account_mm *vfio_account_mm_new(struct kunit *test,
						 unsigned int shift)
{
	struct vfio_account_mm *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	ctx->mm = mm_alloc();
	if (!ctx->mm)
		return NULL;
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (shift != PAGE_SHIFT) {
		pgd_t *old = ctx->mm->pgd, *pgd;
		unsigned long flag = shift == 12 ? MMCF_USER_4K :
				     shift == 14 ? MMCF_USER_16K : MMCF_USER_64K;

		ctx->mm->context.flags |= flag;
		pgd = pgd_alloc(ctx->mm);
		ctx->mm->context.flags &= ~flag;
		if (!pgd) {
			mmput(ctx->mm);
			return NULL;
		}
		pgd_free(ctx->mm, old);
		ctx->mm->pgd = pgd;
		ctx->mm->context.flags |= flag;
	}
#endif
	ctx->live = true;
	mmgrab(ctx->mm);
	if (kunit_add_action_or_reset(test, vfio_account_mm_free, ctx))
		return NULL;
	return ctx;
}

static void vfio_account_dma_free(void *data)
{
	struct vfio_dma *dma = data;

	/* Cleanup must also work with the negative-control implementation. */
	if (dma->locked_vm)
		mm_lock_acct(dma->task, dma->mm, true, -dma->locked_vm);
	mmdrop(dma->mm);
	put_task_struct(dma->task);
}

static struct vfio_dma *vfio_account_dma_new(struct kunit *test,
					    struct vfio_account_mm *ctx)
{
	struct vfio_dma *dma = kunit_kzalloc(test, sizeof(*dma), GFP_KERNEL);

	if (!dma)
		return NULL;
	dma->mm = ctx->mm;
	mmgrab(dma->mm);
	dma->task = get_task_struct(current->group_leader);
	dma->lock_cap = true;
	if (kunit_add_action_or_reset(test, vfio_account_dma_free, dma))
		return NULL;
	return dma;
}

static void vfio_account_exit_test(struct kunit *test)
{
	unsigned int shifts[] = { PAGE_SHIFT,
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
		12, 14, 16,
#endif
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
		struct vfio_account_mm *ctx;
		struct vfio_dma *dma;

		if (i && shifts[i] == PAGE_SHIFT)
			continue;
		ctx = vfio_account_mm_new(test, shifts[i]);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_EQ(test, mm_page_shift(ctx->mm), shifts[i]);
		dma = vfio_account_dma_new(test, ctx);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_EQ(test, vfio_lock_acct(dma, 2, true), 0);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(ctx->mm), 2UL);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 2UL * PAGE_SIZE);
		ctx->live = false;
		mmput(ctx->mm);
		KUNIT_ASSERT_EQ(test, atomic_read(&ctx->mm->mm_users), 0);
		KUNIT_EXPECT_EQ(test, vfio_lock_acct(dma, 1, true), -ESRCH);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		KUNIT_EXPECT_EQ(test, vfio_lock_acct(dma, -1, true), 0);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(ctx->mm), 1UL);
		KUNIT_EXPECT_EQ(test, vfio_lock_acct(dma, -1, true), 0);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->mm->mm_users), 0);
		kunit_info(test, "VFIO %uK user/%luK native dead-mm refunds checked",
			   1U << (shifts[i] - 10), PAGE_SIZE >> 10);
	}
}

static void vfio_account_owner_test(struct kunit *test)
{
	unsigned int shifts[] = { PAGE_SHIFT,
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
		12, 14, 16,
#endif
	};

	for (unsigned int reverse = 0; reverse < 2; reverse++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
			struct vfio_account_mm *old, *next;
			struct vfio_dma *dma;

			if ((i || reverse) && shifts[i] == PAGE_SHIFT)
				continue;
			old = vfio_account_mm_new(test, reverse ? shifts[i] : PAGE_SHIFT);
			next = vfio_account_mm_new(test, reverse ? PAGE_SHIFT : shifts[i]);
			KUNIT_ASSERT_NOT_NULL(test, old);
			KUNIT_ASSERT_NOT_NULL(test, next);
			dma = vfio_account_dma_new(test, old);
			KUNIT_ASSERT_NOT_NULL(test, dma);
			KUNIT_ASSERT_EQ(test, vfio_lock_acct(dma, 3, true), 0);
			old->live = false;
			mmput(old->mm);
			KUNIT_ASSERT_EQ(test, atomic_read(&old->mm->mm_users), 0);
			if (task_rlimit(dma->task, RLIMIT_MEMLOCK) != RLIM_INFINITY) {
				unsigned long budget = task_rlimit(dma->task, RLIMIT_MEMLOCK) >>
						       MM_ACCOUNT_SHIFT;

				/* Model a recipient whose existing pins consume its budget. */
				next->mm->locked_vm = budget;
				KUNIT_EXPECT_EQ(test,
					vfio_change_dma_mm(dma, dma->task, next->mm, false),
					-ENOMEM);
				KUNIT_EXPECT_PTR_EQ(test, dma->mm, old->mm);
				KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(old->mm), 3UL);
				KUNIT_EXPECT_EQ(test, next->mm->locked_vm, budget);
				next->mm->locked_vm = 0;
			}
			KUNIT_ASSERT_EQ(test,
				vfio_change_dma_mm(dma, dma->task, next->mm, true), 0);
			KUNIT_EXPECT_PTR_EQ(test, dma->mm, next->mm);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(old->mm), 0UL);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), 3UL);
			KUNIT_EXPECT_EQ(test, dma->locked_vm, 3UL);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(next->mm), 3UL * PAGE_SIZE);
			/* Same-mm ownership is idempotent; it must not double the charge. */
			KUNIT_ASSERT_EQ(test,
				vfio_change_dma_mm(dma, dma->task, next->mm, true), 0);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), 3UL);
			KUNIT_ASSERT_EQ(test, vfio_lock_acct(dma, -3, true), 0);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(next->mm), 0UL);
			kunit_info(test, "VFIO dead %uK mm -> %uK mm ownership refund checked",
				   1U << (mm_page_shift(old->mm) - 10),
				   1U << (mm_page_shift(next->mm) - 10));
		}
	}
}

static struct kunit_case vfio_account_cases[] = {
	KUNIT_CASE(vfio_account_exit_test),
	KUNIT_CASE(vfio_account_owner_test),
	{}
};

static struct kunit_suite vfio_account_suite = {
	.name = "vfio-type1-account",
	.test_cases = vfio_account_cases,
};

kunit_test_suite(vfio_account_suite);
