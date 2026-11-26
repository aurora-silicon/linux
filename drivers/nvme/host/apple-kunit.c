// SPDX-License-Identifier: GPL-2.0
/*
 * Included by apple.c: KUnit tests for requests whose NVMMU TCB the
 * hardware did not invalidate.
 */
#include <kunit/test.h>

/* A controller whose NVMMU registers are plain memory. */
static struct apple_nvme *apple_nvme_test_anv(struct kunit *test)
{
	struct apple_nvme *anv = kunit_kzalloc(test, sizeof(*anv), GFP_KERNEL);
	void *regs = kunit_kzalloc(test, APPLE_NVMMU_TCB_STAT + sizeof(u32),
				   GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, anv);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	anv->hw = &apple_nvme_t6050_hw;
	anv->mmio_nvmmu = (__force void __iomem *)regs;
	anv->adminq.is_adminq = true;
	return anv;
}

static u32 apple_nvme_test_reg(struct apple_nvme *anv, unsigned int off)
{
	return le32_to_cpup((__force __le32 *)(anv->mmio_nvmmu + off));
}

static void apple_nvme_test_set_reg(struct apple_nvme *anv, unsigned int off,
				    u32 val)
{
	*(__force __le32 *)(anv->mmio_nvmmu + off) = cpu_to_le32(val);
}

/* The invalidation writes the tag and reports a non-zero status as -EIO. */
static void apple_nvme_test_inval_status(struct kunit *test)
{
	struct apple_nvme *anv = apple_nvme_test_anv(test);

	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->ioq, 5), 0);
	KUNIT_EXPECT_EQ(test, apple_nvme_test_reg(anv, APPLE_NVMMU_TCB_INVAL), 5);

	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 1);
	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->adminq, 1), -EIO);
	KUNIT_EXPECT_EQ(test, apple_nvme_test_reg(anv, APPLE_NVMMU_TCB_INVAL), 1);

	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 0x80000000);
	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->ioq, 63), -EIO);

	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 0);
	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->ioq, 63), 0);
}

/* The fault attribute reports a failure even when the NVMMU succeeded. */
static void apple_nvme_test_inval_fault_injection(struct kunit *test)
{
#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
	struct apple_nvme *anv = apple_nvme_test_anv(test);
	struct fault_attr saved = apple_nvme_fail_inval;

	apple_nvme_fail_inval.probability = 100;
	apple_nvme_fail_inval.interval = 1;
	atomic_set(&apple_nvme_fail_inval.times, 1);
	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->ioq, 3), -EIO);
	/* One failure only, as configured. */
	KUNIT_EXPECT_EQ(test, apple_nvmmu_inval(&anv->ioq, 3), 0);
	apple_nvme_fail_inval = saved;
#else
	kunit_skip(test, "needs CONFIG_FAULT_INJECTION_DEBUG_FS");
#endif
}

/* A stale request is held until a controller stop completes after it. */
static void apple_nvme_test_stale_held_until_stop(struct kunit *test)
{
	struct apple_nvme *anv = apple_nvme_test_anv(test);
	struct apple_nvme_iod *iod = kunit_kzalloc(test, sizeof(*iod), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, iod);
	KUNIT_EXPECT_FALSE(test, apple_nvme_tcb_stale_held(anv, iod));

	iod->tcb_stale = true;
	iod->tcb_stale_stops = atomic_read(&anv->stops);
	KUNIT_EXPECT_TRUE(test, apple_nvme_tcb_stale_held(anv, iod));
	apple_nvme_count_stop(anv);
	KUNIT_EXPECT_FALSE(test, apple_nvme_tcb_stale_held(anv, iod));

	/* A stop that completed before the failure does not count. */
	iod->tcb_stale_stops = atomic_read(&anv->stops);
	KUNIT_EXPECT_TRUE(test, apple_nvme_tcb_stale_held(anv, iod));

	/* Nor does the counter wrapping around. */
	atomic_set(&anv->stops, -1);
	iod->tcb_stale_stops = atomic_read(&anv->stops);
	KUNIT_EXPECT_TRUE(test, apple_nvme_tcb_stale_held(anv, iod));
	apple_nvme_count_stop(anv);
	KUNIT_EXPECT_FALSE(test, apple_nvme_tcb_stale_held(anv, iod));
}

/*
 * Completing a held request (a cancel without a stop) leaves it alone: its
 * DMA stays mapped, its PRP lists and tag stay with it. The request below
 * has a segment and no DMA device, so an unmap or the end of the request
 * would not return.
 */
static void apple_nvme_test_complete_keeps_held(struct kunit *test)
{
	struct apple_nvme *anv = apple_nvme_test_anv(test);
	struct request *req;
	struct apple_nvme_iod *iod;

	req = kunit_kzalloc(test, sizeof(*req) + sizeof(*iod), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	iod = blk_mq_rq_to_pdu(req);
	iod->q = &anv->ioq;
	iod->tcb_stale = true;
	iod->tcb_stale_stops = atomic_read(&anv->stops);
	iod->npages = 1;
	iod->nents = 1;
	req->nr_phys_segments = 1;
	req->tag = 7;

	apple_nvme_complete_rq(req);
	KUNIT_EXPECT_TRUE(test, iod->tcb_stale);
	KUNIT_EXPECT_EQ(test, iod->npages, 1);
	KUNIT_EXPECT_EQ(test, iod->nents, 1);
}

static struct kunit_case apple_nvme_test_cases[] = {
	KUNIT_CASE(apple_nvme_test_inval_status),
	KUNIT_CASE(apple_nvme_test_inval_fault_injection),
	KUNIT_CASE(apple_nvme_test_stale_held_until_stop),
	KUNIT_CASE(apple_nvme_test_complete_keeps_held),
	{}
};

static struct kunit_suite apple_nvme_test_suite = {
	.name = "nvme-apple",
	.test_cases = apple_nvme_test_cases,
};
kunit_test_suite(apple_nvme_test_suite);
