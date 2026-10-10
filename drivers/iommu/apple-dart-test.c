// SPDX-License-Identifier: GPL-2.0-only
/* Included by apple-dart.c to exercise domain and command lifecycles. */
#include <kunit/device.h>
#include <kunit/test.h>

static void apple_dart_test_before_attach(struct kunit *test)
{
	struct apple_dart_master_cfg master = {};
	struct apple_dart_domain *domain;
	struct apple_dart *dart;
	struct io_pgtable_ops *ops;
	size_t mapped = 0;
	int ret;

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	domain = kunit_kzalloc(test, sizeof(*domain), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, domain);
	dart->dev = kunit_device_register(test, "apple-dart-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dart->dev);
	dart->hw = &apple_dart_hw_t8103;
	dart->pgsize = PAGE_SIZE;
	dart->ias = 32;
	dart->oas = 36;
	dart->dma_max = DMA_BIT_MASK(32);
	dart->num_streams = 16;
	dart->locked = true;
	dart->regs = (__force void __iomem *)kunit_kzalloc(test, SZ_16K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart->regs);
	spin_lock_init(&dart->lock);
	mutex_init(&domain->init_lock);
	master.stream_maps[0].dart = dart;
	__set_bit(0, master.stream_maps[0].sidmap);
	writel(dart->hw->ttbr_valid, dart->regs + DART_TTBR(dart, 0, 0));
	KUNIT_ASSERT_EQ(test, apple_dart_finalize_domain(domain, &master), 0);
	ops = domain->pgtbl_ops;
	pm_runtime_set_active(dart->dev);
	pm_runtime_enable(dart->dev);

	/* The IOMMU core installs firmware reservations before attach_dev(). */
	ret = apple_dart_map_pages(&domain->domain, 0, SZ_1M, PAGE_SIZE, 1,
				   IOMMU_READ | IOMMU_WRITE, GFP_KERNEL, &mapped);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, apple_dart_iotlb_sync_map(&domain->domain, 0, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, ops->iova_to_phys(ops, 0), (phys_addr_t)SZ_1M);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&domain->stream_maps[0].sidmap[0]), 0L);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND), 0U);

	/* Adding streams defers publication until attach maps the locked roots. */
	KUNIT_EXPECT_EQ(test, apple_dart_domain_add_streams(domain, &master), 0);
	KUNIT_EXPECT_EQ(test, apple_dart_iotlb_sync_map(&domain->domain, 0, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND),
			(u32)DART_T8020_STREAM_COMMAND_INVALIDATE);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TTBR(dart, 0, 0)),
			dart->hw->ttbr_valid);
	KUNIT_EXPECT_EQ(test, ops->iova_to_phys(ops, 0), (phys_addr_t)SZ_1M);

	pm_runtime_disable(dart->dev);
	free_io_pgtable_ops(ops);
}

static void apple_dart_test_locked_handoff(struct kunit *test)
{
	struct apple_dart_stream_map stream = {};
	struct io_pgtable_cfg cfg = {};
	struct apple_dart *dart;
	u64 live[] = { 0x1001, 0, 0x3001, 0 };
	u64 owned[ARRAY_SIZE(live)] = {};
	u64 ours[] = { 0x4001, 0x5001, 0, 0 };

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	dart->hw = &apple_dart_hw_t8103;
	dart->num_streams = 1;
	dart->pgsize = sizeof(live);
	spin_lock_init(&dart->lock);
	dart->locked_ttbr[0][0] = live;
	dart->locked_owned[0][0] = owned;
	stream.dart = dart;
	__set_bit(0, stream.sidmap);
	cfg.apple_dart_cfg.n_ttbrs = 1;
	cfg.apple_dart_cfg.ttbr[0] = ours;

	/* Pre-attach synchronization defers a root without its owner shadow. */
	dart->locked_owned[0][0] = NULL;
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, true), 0);
	KUNIT_EXPECT_EQ(test, live[0], 0x1001ULL);
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), -EIO);
	KUNIT_EXPECT_EQ(test, live[0], 0x1001ULL);
	dart->locked_owned[0][0] = owned;

	/* Legacy firmware mappings are rebuilt by the IOMMU core at handoff. */
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), 0);
	KUNIT_EXPECT_MEMEQ(test, live, ours, sizeof(live));
	KUNIT_EXPECT_MEMEQ(test, owned, ours, sizeof(owned));
	apple_dart_retire_root(live, owned, ARRAY_SIZE(live));
	KUNIT_EXPECT_EQ(test, live[0], 0ULL);
	KUNIT_EXPECT_EQ(test, live[1], 0ULL);

	/* Older T8110 instances also use the legacy display handoff. */
	dart->hw = &apple_dart_hw_t8110;
	dart->version = 0x0200;
	live[0] = 0x1001;
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), 0);
	KUNIT_EXPECT_MEMEQ(test, live, ours, sizeof(live));
}

static void apple_dart_test_firmware_roots(struct kunit *test)
{
	struct apple_dart_stream_map stream = {};
	struct io_pgtable_cfg cfg = {};
	struct apple_dart *dart;
	u64 live[] = { 0x1001, 0, 0, 0 };
	u64 owned[ARRAY_SIZE(live)] = {};
	u64 ours[] = { 0, 0x2001, 0, 0 };

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	dart->hw = &apple_dart_hw_t8110;
	dart->version = 0x0202;
	dart->num_streams = 1;
	dart->pgsize = sizeof(live);
	spin_lock_init(&dart->lock);
	dart->locked_ttbr[0][0] = live;
	dart->locked_owned[0][0] = owned;
	stream.dart = dart;
	__set_bit(0, stream.sidmap);
	cfg.apple_dart_cfg.n_ttbrs = 1;
	cfg.apple_dart_cfg.ttbr[0] = ours;

	KUNIT_ASSERT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), 0);
	KUNIT_EXPECT_EQ(test, live[0], 0x1001ULL);
	KUNIT_EXPECT_EQ(test, live[1], 0x2001ULL);
	ours[0] = 0x3001;
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), -EBUSY);
	dart->version = 0x0203;
	KUNIT_EXPECT_EQ(test, apple_dart_hw_sync_locked(&cfg, &stream, false), -EBUSY);
	KUNIT_EXPECT_EQ(test, live[0], 0x1001ULL);
	/* Teardown must preserve a root which firmware has since replaced. */
	live[1] = 0x4001;
	apple_dart_retire_root(live, owned, ARRAY_SIZE(live));
	KUNIT_EXPECT_EQ(test, live[0], 0x1001ULL);
	KUNIT_EXPECT_EQ(test, live[1], 0x4001ULL);
}

static void apple_dart_test_gated_commands(struct kunit *test)
{
	struct apple_dart_stream_map stream = {};
	struct apple_dart *dart;
	u32 sentinel = 0x40;
	int ret;

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	dart->dev = kunit_device_register(test, "apple-dart-gated-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dart->dev);
	dev_set_drvdata(dart->dev, dart);
	dart->regs = (__force void __iomem *)kunit_kzalloc(test, SZ_16K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart->regs);
	dart->num_streams = 1;
	dart->pgsize = SZ_16K;
	dart->version = 0x0202;
	spin_lock_init(&dart->lock);
	stream.dart = dart;
	__set_bit(0, stream.sidmap);
	pm_runtime_set_active(dart->dev);
	pm_runtime_enable(dart->dev);

	writel(sentinel, dart->regs + DART_T8020_STREAM_SELECT);
	writel(sentinel, dart->regs + DART_T8020_STREAM_COMMAND);
	writel(sentinel, dart->regs + DART_T8110_TLB_CMD);
	writel(sentinel, dart->regs + DART_T8110_TLB_START);
	writel(sentinel, dart->regs + DART_T8110_TLB_END);
	apple_dart_quiesce_commands(dart->dev);

	/* Neither engine may touch its command registers after quiesce. */
	KUNIT_EXPECT_EQ(test, apple_dart_t8020_hw_invalidate_tlb(&stream), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_SELECT), sentinel);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND), sentinel);
	ret = apple_dart_t8110_hw_tlb_command_range(&stream,
						    DART_T8110_TLB_CMD_OP_FLUSH_SID,
						    true, 0, SZ_16K - 1);
	KUNIT_EXPECT_EQ(test, ret, -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8110_TLB_CMD), sentinel);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8110_TLB_START), sentinel);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8110_TLB_END), sentinel);

	/* A restored port may submit commands again. */
	apple_dart_resume_commands(dart->dev);
	KUNIT_EXPECT_EQ(test, apple_dart_t8020_hw_invalidate_tlb(&stream), 0);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND),
			(u32)DART_T8020_STREAM_COMMAND_INVALIDATE);
	KUNIT_EXPECT_EQ(test, apple_dart_t8110_hw_invalidate_tlb(&stream), 0);
	KUNIT_EXPECT_NE(test, readl(dart->regs + DART_T8110_TLB_CMD), sentinel);
	pm_runtime_disable(dart->dev);
}

static void apple_dart_test_resume_commands_failure(struct kunit *test)
{
	struct apple_dart *dart;

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	dart->dev = kunit_device_register(test, "apple-dart-resume-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dart->dev);
	dev_set_drvdata(dart->dev, dart);
	spin_lock_init(&dart->lock);
	pm_runtime_set_active(dart->dev);
	pm_runtime_enable(dart->dev);
	apple_dart_quiesce_commands(dart->dev);

	/* A prior runtime-PM error must leave commands gated and report failure. */
	scoped_guard(spinlock_irqsave, &dart->dev->power.lock)
		dart->dev->power.runtime_error = -EIO;
	KUNIT_EXPECT_EQ(test, apple_dart_resume_commands(dart->dev), -EINVAL);
	KUNIT_EXPECT_TRUE(test, dart->commands_gated);
	KUNIT_EXPECT_EQ(test, atomic_read(&dart->dev->power.usage_count), 0);

	/* Clearing the PM error permits a later retry without leaking a reference. */
	KUNIT_EXPECT_EQ(test, pm_runtime_set_active(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, apple_dart_resume_commands(dart->dev), 0);
	KUNIT_EXPECT_FALSE(test, dart->commands_gated);
	KUNIT_EXPECT_EQ(test, atomic_read(&dart->dev->power.usage_count), 0);
	pm_runtime_disable(dart->dev);
}

static struct apple_dart *apple_dart_test_tunnel(struct kunit *test)
{
	struct apple_dart *dart;

	dart = kunit_kzalloc(test, sizeof(*dart), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart);
	dart->dev = kunit_device_register(test, "apple-dart-tunnel-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dart->dev);
	dart->regs = (__force void __iomem *)kunit_kzalloc(test, SZ_16K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dart->regs);
	dart->hw = &apple_dart_hw_t8103_usb4;
	dart->num_streams = dart->hw->max_sid_count;
	dart->tunneled = true;
	spin_lock_init(&dart->lock);
	dev_set_drvdata(dart->dev, dart);
	return dart;
}

static void apple_dart_test_tunnel_restore(struct kunit *test)
{
	struct apple_dart *dart = apple_dart_test_tunnel(test);
	unsigned int sid, idx;
	u32 sentinel = 0x40;

	/* M1 tunnel DART nodes have no retained power-domain attachment. */
	KUNIT_ASSERT_FALSE(test, dart->power_retained);
	for (sid = 0; sid < dart->num_streams; sid++) {
		writel(0x80 + sid, dart->regs + DART_TCR(dart, sid));
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			writel(0x1000 + 0x10 * sid + idx,
			       dart->regs + DART_TTBR(dart, sid, idx));
	}
	KUNIT_ASSERT_EQ(test, apple_dart_save_tunnel_state(dart->dev), 0);
	memset_io(dart->regs, 0, SZ_16K);
	KUNIT_ASSERT_EQ(test, apple_dart_restore_tunnel_state(dart->dev), 0);
	for (sid = 0; sid < dart->num_streams; sid++) {
		KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TCR(dart, sid)),
				0x80 + sid);
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TTBR(dart, sid, idx)),
					0x1000 + 0x10 * sid + idx);
	}
	KUNIT_EXPECT_FALSE(test, dart->tunnel_state_saved);
	KUNIT_EXPECT_TRUE(test, dart->tunnel_state_restored);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_SELECT), U32_MAX);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_SELECT + 4), U32_MAX);

	/* Normal resume must not reset translations after the link is live. */
	writel(sentinel, dart->regs + DART_T8020_STREAM_COMMAND);
	KUNIT_EXPECT_EQ(test, apple_dart_resume(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND), sentinel);
	KUNIT_EXPECT_FALSE(test, dart->tunnel_state_restored);
	KUNIT_EXPECT_EQ(test, apple_dart_restore_tunnel_state(dart->dev), -EINVAL);
}

static void apple_dart_test_tunnel_reject_unsafe(struct kunit *test)
{
	struct apple_dart *dart = apple_dart_test_tunnel(test);
	u32 sentinel = 0x40;

	writel(sentinel, dart->regs + DART_T8020_STREAM_COMMAND);
	KUNIT_EXPECT_EQ(test, apple_dart_restore_tunnel_state(dart->dev), -EINVAL);
	dart->locked = true;
	KUNIT_EXPECT_EQ(test, apple_dart_save_tunnel_state(dart->dev), -EOPNOTSUPP);
	dart->locked = false;
	dart->commands_gated = true;
	KUNIT_EXPECT_EQ(test, apple_dart_save_tunnel_state(dart->dev), -EBUSY);
	KUNIT_EXPECT_FALSE(test, dart->tunnel_state_saved);
	dart->commands_gated = false;
	dart->hw = &apple_dart_hw_t8110;
	KUNIT_EXPECT_EQ(test, apple_dart_save_tunnel_state(dart->dev), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND), sentinel);
}

static int apple_dart_test_tlb_failure(struct apple_dart_stream_map *stream_map)
{
	return -EIO;
}

static void apple_dart_test_tunnel_reset_failure(struct kunit *test)
{
	struct apple_dart *dart = apple_dart_test_tunnel(test);
	struct apple_dart_hw hw = apple_dart_hw_t8103_usb4;

	hw.invalidate_tlb = apple_dart_test_tlb_failure;
	dart->hw = &hw;
	KUNIT_ASSERT_EQ(test, apple_dart_save_tunnel_state(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, apple_dart_restore_tunnel_state(dart->dev), -EIO);
	KUNIT_EXPECT_FALSE(test, dart->tunnel_state_restored);
	KUNIT_EXPECT_TRUE(test, dart->tunnel_state_saved);

	/* A later successful reset can still restore the saved mappings. */
	dart->hw = &apple_dart_hw_t8103_usb4;
	KUNIT_EXPECT_EQ(test, apple_dart_restore_tunnel_state(dart->dev), 0);
	KUNIT_EXPECT_TRUE(test, dart->tunnel_state_restored);
	KUNIT_EXPECT_EQ(test, apple_dart_suspend(dart->dev), 0);
	KUNIT_EXPECT_FALSE(test, dart->tunnel_state_restored);
}

static void apple_dart_test_failed_host_pm(struct kunit *test)
{
	struct apple_dart *dart = apple_dart_test_tunnel(test);
	u32 sentinel = 0x40;

	writel(0x80, dart->regs + DART_TCR(dart, 0));
	KUNIT_ASSERT_EQ(test, apple_dart_save_tunnel_state(dart->dev), 0);
	apple_dart_quiesce_commands(dart->dev);
	writel(sentinel, dart->regs + DART_T8020_STREAM_COMMAND);
	writel(sentinel, dart->regs + DART_TCR(dart, 0));

	/* PM still resumes dependents after the host's noirq callback fails. */
	KUNIT_EXPECT_EQ(test, apple_dart_resume(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_T8020_STREAM_COMMAND), sentinel);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TCR(dart, 0)), sentinel);
	KUNIT_EXPECT_TRUE(test, dart->commands_gated);

	/* Another sleep must neither access the failed port nor lose its snapshot. */
	KUNIT_EXPECT_EQ(test, apple_dart_suspend(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, dart->save_tcr[0], 0x80U);
	KUNIT_EXPECT_EQ(test, apple_dart_save_tunnel_state(dart->dev), -EBUSY);
	KUNIT_EXPECT_TRUE(test, dart->tunnel_state_saved);
	KUNIT_EXPECT_EQ(test, dart->save_tcr[0], 0x80U);
}

static irqreturn_t apple_dart_test_irq_handler(int irq, void *data)
{
	struct apple_dart *dart = data;

	writel(0x1234, dart->regs + DART_TCR(dart, 0));
	return IRQ_HANDLED;
}

static void apple_dart_test_gated_irq(struct kunit *test)
{
	struct apple_dart *dart = apple_dart_test_tunnel(test);
	struct apple_dart_hw hw = apple_dart_hw_t8103_usb4;

	hw.irq_handler = apple_dart_test_irq_handler;
	dart->hw = &hw;
	pm_runtime_set_active(dart->dev);
	pm_runtime_enable(dart->dev);
	apple_dart_quiesce_commands(dart->dev);
	KUNIT_EXPECT_EQ(test, apple_dart_irq(0, dart), IRQ_NONE);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TCR(dart, 0)), 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&dart->dev->power.usage_count), 0);

	KUNIT_ASSERT_EQ(test, apple_dart_resume_commands(dart->dev), 0);
	KUNIT_EXPECT_EQ(test, apple_dart_irq(0, dart), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TCR(dart, 0)), 0x1234U);
	KUNIT_EXPECT_EQ(test, atomic_read(&dart->dev->power.usage_count), 0);

	/* Failed runtime resume must not run the handler or leak its reference. */
	writel(0, dart->regs + DART_TCR(dart, 0));
	scoped_guard(spinlock_irqsave, &dart->dev->power.lock)
		dart->dev->power.runtime_error = -EIO;
	KUNIT_EXPECT_EQ(test, apple_dart_irq(0, dart), IRQ_NONE);
	KUNIT_EXPECT_EQ(test, readl(dart->regs + DART_TCR(dart, 0)), 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&dart->dev->power.usage_count), 0);
	KUNIT_EXPECT_EQ(test, pm_runtime_set_active(dart->dev), 0);
	pm_runtime_disable(dart->dev);
}

static struct kunit_case apple_dart_test_cases[] = {
	KUNIT_CASE(apple_dart_test_before_attach),
	KUNIT_CASE(apple_dart_test_locked_handoff),
	KUNIT_CASE(apple_dart_test_firmware_roots),
	KUNIT_CASE(apple_dart_test_gated_commands),
	KUNIT_CASE(apple_dart_test_resume_commands_failure),
	KUNIT_CASE(apple_dart_test_tunnel_restore),
	KUNIT_CASE(apple_dart_test_tunnel_reject_unsafe),
	KUNIT_CASE(apple_dart_test_tunnel_reset_failure),
	KUNIT_CASE(apple_dart_test_failed_host_pm),
	KUNIT_CASE(apple_dart_test_gated_irq),
	{}
};

static struct kunit_suite apple_dart_test_suite = {
	.name = "apple-dart",
	.test_cases = apple_dart_test_cases,
};

kunit_test_suite(apple_dart_test_suite);
