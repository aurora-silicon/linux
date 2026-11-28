// SPDX-License-Identifier: GPL-2.0
/*
 * Included by apple.c: KUnit tests for requests whose NVMMU TCB the
 * hardware did not invalidate.
 */
#include <kunit/test.h>

/* A controller whose NVMMU registers are plain memory. */
static struct apple_nvme *apple_nvme_test_anv(struct kunit *test,
					      const struct apple_nvme_hw *hw)
{
	struct apple_nvme *anv = kunit_kzalloc(test, sizeof(*anv), GFP_KERNEL);
	void *regs = kunit_kzalloc(test, APPLE_NVMMU_TCB_STAT + sizeof(u32),
				   GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, anv);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	anv->hw = hw;
	anv->mmio_nvmmu = (__force void __iomem *)regs;
	anv->adminq.is_adminq = true;
	spin_lock_init(&anv->lock);
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
	struct apple_nvme *anv = apple_nvme_test_anv(test, &apple_nvme_t6050_hw);

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
	struct apple_nvme *anv = apple_nvme_test_anv(test, &apple_nvme_t6050_hw);
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
	struct apple_nvme *anv = apple_nvme_test_anv(test, &apple_nvme_t6050_hw);
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
 * The tests below drive the driver's completion queue poll, cancel and
 * stop accounting on real blk-mq requests. The I/O queue's completion
 * queue, TCBs and doorbell are plain memory; the requests carry no data.
 */
struct apple_nvme_test_ctx {
	struct apple_nvme *anv;
	struct request_queue *q;
	/* Tags of the requests that ended. */
	unsigned long ended;
};

static void apple_nvme_test_free_set(void *set)
{
	blk_mq_free_tag_set(set);
}

static void apple_nvme_test_free_queue(void *q)
{
	blk_mq_destroy_queue(q);
	blk_put_queue(q);
}

/* Lets a test that failed end its requests, so that its queue goes away. */
static void apple_nvme_test_release_all(void *anv)
{
	apple_nvme_count_stop(anv);
	apple_nvme_cancel_tagsets(anv);
}

static void apple_nvme_test_add_set(struct kunit *test,
				    struct blk_mq_tag_set *set,
				    struct apple_nvme_queue *q)
{
	int ret;

	set->ops = &apple_nvme_mq_admin_ops;
	set->nr_hw_queues = 1;
	set->queue_depth = 4;
	/* Only aborted requests time out during a test. */
	set->timeout = 3600 * HZ;
	set->numa_node = NUMA_NO_NODE;
	set->cmd_size = sizeof(struct apple_nvme_iod);
	set->driver_data = q;
	KUNIT_ASSERT_EQ(test, blk_mq_alloc_tag_set(set), 0);
	ret = kunit_add_action_or_reset(test, apple_nvme_test_free_set, set);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static struct apple_nvme_test_ctx *
apple_nvme_test_ctx(struct kunit *test, const struct apple_nvme_hw *hw)
{
	struct apple_nvme_test_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx),
							 GFP_KERNEL);
	struct apple_nvme_queue *ioq;
	struct apple_nvme *anv;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	anv = apple_nvme_test_anv(test, hw);
	ctx->anv = anv;
	/*
	 * An aborted request goes to the timeout handler, which would read
	 * registers that are not there; this makes it re-arm the timer.
	 */
	anv->quarantined = true;

	ioq = &anv->ioq;
	ioq->cqes = kunit_kcalloc(test, apple_nvme_queue_depth(ioq),
				  sizeof(*ioq->cqes), GFP_KERNEL);
	ioq->tcbs = kunit_kcalloc(test, hw->max_queue_depth,
				  sizeof(*ioq->tcbs), GFP_KERNEL);
	ioq->cq_db = (__force u32 __iomem *)kunit_kzalloc(test, sizeof(u32),
							   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ioq->cqes);
	KUNIT_ASSERT_NOT_NULL(test, ioq->tcbs);
	KUNIT_ASSERT_NOT_NULL(test, (__force void *)ioq->cq_db);
	ioq->cq_phase = 1;
	/* The admin queue has nothing to reap: its CQE is never pending. */
	anv->adminq.cqes = kunit_kzalloc(test, sizeof(*anv->adminq.cqes),
					 GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, anv->adminq.cqes);
	anv->adminq.cq_phase = 1;

	apple_nvme_test_add_set(test, &anv->admin_tagset, &anv->adminq);
	apple_nvme_test_add_set(test, &anv->tagset, ioq);
	anv->ctrl.admin_tagset = &anv->admin_tagset;
	anv->ctrl.tagset = &anv->tagset;

	ctx->q = blk_mq_alloc_queue(&anv->tagset, NULL, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->q));
	ret = kunit_add_action_or_reset(test, apple_nvme_test_free_queue,
					ctx->q);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, apple_nvme_test_release_all, anv);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return ctx;
}

static enum rq_end_io_ret apple_nvme_test_end_io(struct request *req,
						 blk_status_t status,
						 const struct io_comp_batch *iob)
{
	struct apple_nvme_test_ctx *ctx = req->end_io_data;

	ctx->ended |= BIT(req->tag);
	return RQ_END_IO_FREE;
}

/* An I/O command submitted to the controller. */
static struct request *apple_nvme_test_start(struct kunit *test,
					     struct apple_nvme_test_ctx *ctx)
{
	struct request *req;

	req = blk_mq_alloc_request(ctx->q, REQ_OP_DRV_IN, BLK_MQ_REQ_NOWAIT);
	KUNIT_ASSERT_FALSE(test, IS_ERR(req));
	req->rq_flags |= RQF_QUIET;
	/* Fail it when cancelled instead of requeueing it. */
	req->cmd_flags |= REQ_FAILFAST_DRIVER;
	req->end_io = apple_nvme_test_end_io;
	req->end_io_data = ctx;
	nvme_req(req)->cmd->common.command_id = nvme_cid(req);
	blk_mq_start_request(req);
	return req;
}

/* The controller posts a completion for req at the CQ head. */
static void apple_nvme_test_post(struct apple_nvme_queue *q, struct request *req)
{
	struct nvme_completion *cqe = &q->cqes[q->cq_head];

	cqe->command_id = nvme_cid(req);
	cqe->status = cpu_to_le16(q->cq_phase);
}

static bool apple_nvme_test_stale(struct request *req)
{
	return ((struct apple_nvme_iod *)blk_mq_rq_to_pdu(req))->tcb_stale;
}

/*
 * A disable that stopped the controller reaps its last completions before
 * it counts the stop. A TCB that fails to invalidate in that poll is fenced
 * by the stop, so the cancel that follows releases its request along with
 * those that failed before and with the rest.
 */
static void apple_nvme_test_final_poll_fenced(struct kunit *test)
{
	struct apple_nvme_test_ctx *ctx = apple_nvme_test_ctx(test,
							      &apple_nvme_t6050_hw);
	struct apple_nvme *anv = ctx->anv;
	unsigned int stops = atomic_read(&anv->stops);
	struct request *a, *b, *c;
	struct apple_nvme_iod *iod;
	unsigned long all;

	a = apple_nvme_test_start(test, ctx);
	b = apple_nvme_test_start(test, ctx);
	c = apple_nvme_test_start(test, ctx);
	all = BIT(a->tag) | BIT(b->tag) | BIT(c->tag);
	iod = blk_mq_rq_to_pdu(b);
	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 1);

	/* a fails while the controller runs: it stays in flight. */
	apple_nvme_test_post(&anv->ioq, a);
	apple_nvme_handle_cq(&anv->ioq, true);
	KUNIT_EXPECT_TRUE(test, apple_nvme_test_stale(a));
	KUNIT_EXPECT_EQ(test, blk_mq_rq_state(a), MQ_RQ_IN_FLIGHT);
	KUNIT_EXPECT_EQ(test, ctx->ended, 0UL);

	/*
	 * b fails in the poll that ends the disable, once CC.EN is clear.
	 * Its iod stays with the tag set after it ends, as read below.
	 */
	apple_nvme_test_post(&anv->ioq, b);
	KUNIT_EXPECT_NE(test, apple_nvme_test_reg(anv, APPLE_NVMMU_TCB_INVAL),
			nvme_cid(b));
	apple_nvme_reap_and_cancel(anv, true);
	KUNIT_EXPECT_EQ(test, apple_nvme_test_reg(anv, APPLE_NVMMU_TCB_INVAL),
			iod->cmd.common.command_id);
	KUNIT_EXPECT_TRUE(test, iod->tcb_stale);
	KUNIT_EXPECT_EQ(test, iod->tcb_stale_stops, stops);
	KUNIT_EXPECT_EQ(test, (unsigned int)atomic_read(&anv->stops), stops + 1);
	KUNIT_EXPECT_EQ(test, ctx->ended, all);
	KUNIT_EXPECT_FALSE(test, apple_nvme_has_stale(anv));
}

/*
 * Pre-M4 with a controller that could not be stopped: the cancel leaves the
 * request whose TCB failed in flight, and returns, rather than complete it
 * and wait for it. Resetting ANS is the stop that releases it.
 */
static void apple_nvme_test_no_stop_keeps_in_flight(struct kunit *test)
{
	struct apple_nvme_test_ctx *ctx = apple_nvme_test_ctx(test,
							      &apple_nvme_t8103_hw);
	struct apple_nvme *anv = ctx->anv;
	struct request *a, *c;
	unsigned long ta, tc;

	a = apple_nvme_test_start(test, ctx);
	c = apple_nvme_test_start(test, ctx);
	ta = BIT(a->tag);
	tc = BIT(c->tag);
	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 1);
	apple_nvme_test_post(&anv->ioq, a);

	apple_nvme_reap_and_cancel(anv, false);
	KUNIT_EXPECT_TRUE(test, apple_nvme_test_stale(a));
	KUNIT_EXPECT_EQ(test, blk_mq_rq_state(a), MQ_RQ_IN_FLIGHT);
	KUNIT_EXPECT_EQ(test, ctx->ended, tc);
	KUNIT_EXPECT_TRUE(test, apple_nvme_has_stale(anv));

	/* Another cancel without a stop changes nothing. */
	apple_nvme_cancel_tagsets(anv);
	KUNIT_EXPECT_EQ(test, blk_mq_rq_state(a), MQ_RQ_IN_FLIGHT);
	KUNIT_EXPECT_EQ(test, ctx->ended, tc);

	apple_nvme_reset_fenced(anv);
	KUNIT_EXPECT_EQ(test, ctx->ended, ta | tc);
	KUNIT_EXPECT_FALSE(test, apple_nvme_has_stale(anv));
}

/* A second completion for a request already held does not complete it. */
static void apple_nvme_test_second_cqe_ignored(struct kunit *test)
{
	struct apple_nvme_test_ctx *ctx = apple_nvme_test_ctx(test,
							      &apple_nvme_t6050_hw);
	struct apple_nvme *anv = ctx->anv;
	struct request *a;
	unsigned long ta;

	a = apple_nvme_test_start(test, ctx);
	ta = BIT(a->tag);
	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 1);
	apple_nvme_test_post(&anv->ioq, a);
	apple_nvme_handle_cq(&anv->ioq, true);
	KUNIT_EXPECT_TRUE(test, apple_nvme_test_stale(a));

	apple_nvme_test_set_reg(anv, APPLE_NVMMU_TCB_STAT, 0);
	apple_nvme_test_post(&anv->ioq, a);
	apple_nvme_handle_cq(&anv->ioq, true);
	KUNIT_EXPECT_EQ(test, blk_mq_rq_state(a), MQ_RQ_IN_FLIGHT);
	KUNIT_EXPECT_EQ(test, ctx->ended, 0UL);

	apple_nvme_reap_and_cancel(anv, true);
	KUNIT_EXPECT_EQ(test, ctx->ended, ta);
}

static struct kunit_case apple_nvme_test_cases[] = {
	KUNIT_CASE(apple_nvme_test_inval_status),
	KUNIT_CASE(apple_nvme_test_inval_fault_injection),
	KUNIT_CASE(apple_nvme_test_stale_held_until_stop),
	KUNIT_CASE(apple_nvme_test_final_poll_fenced),
	KUNIT_CASE(apple_nvme_test_no_stop_keeps_in_flight),
	KUNIT_CASE(apple_nvme_test_second_cqe_ignored),
	{}
};

static struct kunit_suite apple_nvme_test_suite = {
	.name = "nvme-apple",
	.test_cases = apple_nvme_test_cases,
};
kunit_test_suite(apple_nvme_test_suite);
