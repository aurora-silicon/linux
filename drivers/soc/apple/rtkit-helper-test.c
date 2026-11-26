// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple RTKit helper co-processor driver.
 *
 * Included at the end of rtkit-helper.c. The tests call the helper's own
 * shared memory callbacks, its buffer inventory parser and its sleep
 * callbacks directly. The RTKit AP power notice is a static stub, the RTKit
 * state the helper reads is a zeroed struct apple_rtkit, and the ASC and SRAM
 * registers are kernel memory. No hardware is touched.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/sizes.h>

#include "rtkit-internal.h"

/* The RTKit power states the helper's checks look at (rtkit.c). */
#define RTKH_TEST_PWR_IDLE	0x201
#define RTKH_TEST_PWR_ON	0x20

/* An SRAM region well inside the 44 bit DMA mask. */
#define RTKH_TEST_SRAM_START	0x210000000ULL
#define RTKH_TEST_SRAM_SIZE	0x10000
#define RTKH_TEST_MAX_CALLS	8

struct rtkh_test {
	struct apple_rtkit_helper helper;
	struct apple_rtkit rtk;
	struct device *dev;
	struct resource sram;
	void *sram_mem;
	u32 *asc;

	struct device_node np;
	struct property compat;
	struct property inventory;
	__be32 cells[2 * APPLE_RTKIT_HELPER_MAX_BUFFERS * APPLE_RTKIT_HELPER_BUFFER_CELLS];

	/* The AP power notices the helper sent, and what each one returns. */
	unsigned int ncalls;
	bool sent_on[RTKH_TEST_MAX_CALLS];
	int ret[RTKH_TEST_MAX_CALLS];

	unsigned int saved_flags;
	suspend_state_t saved_target;
};

static int rtkh_stub_send_ap_power(struct apple_rtkit_helper *helper, bool on)
{
	struct kunit *test = kunit_get_current_test();
	struct rtkh_test *ctx = test->priv;
	unsigned int i = ctx->ncalls++;
	int ret;

	KUNIT_ASSERT_PTR_EQ(test, helper, &ctx->helper);
	KUNIT_ASSERT_LT(test, i, RTKH_TEST_MAX_CALLS);
	ctx->sent_on[i] = on;
	ret = ctx->ret[i];
	/* An answered notice moves RTKit's AP state, as its ACK handler does. */
	if (!ret)
		ctx->rtk.ap_power_state = on ? RTKH_TEST_PWR_ON : RTKH_TEST_PWR_IDLE;
	return ret;
}

static void rtkh_set_run(struct rtkh_test *ctx, bool run)
{
	ctx->asc[APPLE_ASC_CPU_CONTROL / 4] = run ? APPLE_ASC_CPU_CONTROL_RUN : 0;
}

static int rtkh_test_init(struct kunit *test)
{
	struct rtkh_test *ctx;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	dev = kunit_device_register(test, "rtkit-helper-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ctx->dev = dev;
	dev->coherent_dma_mask = DMA_BIT_MASK(44);
	dev->dma_mask = &dev->coherent_dma_mask;
	/* Coherent: dma-direct hands out pages of the linear map. */
	dev_set_dma_coherent(dev);

	ctx->asc = kunit_kzalloc(test, APPLE_ASC_CPU_CONTROL + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->asc);
	ctx->sram_mem = kunit_kzalloc(test, RTKH_TEST_SRAM_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->sram_mem);
	ctx->sram = DEFINE_RES_MEM_NAMED(RTKH_TEST_SRAM_START,
					 RTKH_TEST_SRAM_SIZE, "sram");

	ctx->rtk.dev = dev;
	ctx->rtk.iop_power_state = RTKH_TEST_PWR_ON;
	ctx->rtk.ap_power_state = RTKH_TEST_PWR_ON;

	ctx->helper.dev = dev;
	ctx->helper.rtk = &ctx->rtk;
	ctx->helper.asc_base = (__force void __iomem *)ctx->asc;
	ctx->helper.sram = &ctx->sram;
	ctx->helper.sram_base = (__force void __iomem *)ctx->sram_mem;
	dev_set_drvdata(dev, &ctx->helper);

	ctx->saved_flags = pm_suspend_global_flags;
	ctx->saved_target = pm_suspend_target_state;
	pm_suspend_clear_flags();

	kunit_activate_static_stub(test, apple_rtkit_helper_send_ap_power,
				   rtkh_stub_send_ap_power);
	return 0;
}

static void rtkh_test_exit(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;

	/* The node is the test's: the device must not put it. */
	if (ctx->dev)
		ctx->dev->of_node = NULL;
	pm_suspend_global_flags = ctx->saved_flags;
	pm_suspend_target_state = ctx->saved_target;
}

/* shmem_setup / shmem_destroy */

static int rtkh_setup(struct rtkh_test *ctx, struct apple_rtkit_shmem *bfr,
		      u64 iova, size_t size)
{
	memset(bfr, 0, sizeof(*bfr));
	bfr->iova = iova;
	bfr->size = size;
	return apple_rtkit_helper_shmem_setup(&ctx->helper, bfr);
}

static void rtkh_expect_refused(struct kunit *test, struct apple_rtkit_shmem *bfr,
				int ret, int err)
{
	KUNIT_EXPECT_EQ(test, ret, err);
	KUNIT_EXPECT_NULL(test, bfr->buffer);
	KUNIT_EXPECT_NULL(test, bfr->iomem);
	KUNIT_EXPECT_FALSE(test, bfr->is_mapped);
}

static void rtkh_shmem_dma_alloc(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, 0, 0x4000), 0);
	KUNIT_ASSERT_NOT_NULL(test, bfr.buffer);
	KUNIT_EXPECT_NE(test, bfr.iova, 0);
	KUNIT_EXPECT_EQ(test, bfr.iova & (PAGE_SIZE - 1), 0);
	KUNIT_EXPECT_LE(test, bfr.iova + bfr.size - 1, DMA_BIT_MASK(44));
	KUNIT_EXPECT_NULL(test, bfr.iomem);
	KUNIT_EXPECT_FALSE(test, bfr.is_mapped);
	KUNIT_EXPECT_EQ(test, bfr.size, 0x4000);
	memset(bfr.buffer, 0xa5, bfr.size);

	dma_free_coherent(ctx->dev, bfr.size, bfr.buffer, bfr.iova);
}

static void rtkh_shmem_sram_inside(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START + 0x1000,
					 0x2000), 0);
	KUNIT_EXPECT_TRUE(test, bfr.is_mapped);
	KUNIT_EXPECT_NULL(test, bfr.buffer);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem, ctx->sram_mem + 0x1000);
	KUNIT_EXPECT_EQ(test, bfr.iova, RTKH_TEST_SRAM_START + 0x1000);
	KUNIT_EXPECT_EQ(test, bfr.size, 0x2000);
}

static void rtkh_shmem_sram_whole(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START,
					 RTKH_TEST_SRAM_SIZE), 0);
	KUNIT_EXPECT_TRUE(test, bfr.is_mapped);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem, ctx->sram_mem);
}

/* Byte granular and unaligned: the SRAM offset is the IOVA's, not rounded. */
static void rtkh_shmem_sram_unaligned_edges(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START + 0x123,
					 0x10), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem, ctx->sram_mem + 0x123);

	/* The very last byte of the region. */
	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, ctx->sram.end, 1), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem,
			    ctx->sram_mem + RTKH_TEST_SRAM_SIZE - 1);

	/* Ends exactly at the end of the region. */
	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, ctx->sram.end + 1 - 0x801,
					 0x801), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem,
			    ctx->sram_mem + RTKH_TEST_SRAM_SIZE - 0x801);
}

static void rtkh_shmem_sram_outside(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	/* One byte past the end. */
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, ctx->sram.end + 1 - 0x800, 0x801),
			    -EFAULT);
	/* Straddles the end. */
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, ctx->sram.end + 1 - 0x1000, 0x2000),
			    -EFAULT);
	/* Starts one byte before the region. */
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START - 1, 0x10),
			    -EFAULT);
	/* Right after it. */
	rtkh_expect_refused(test, &bfr, rtkh_setup(ctx, &bfr, ctx->sram.end + 1, 1),
			    -EFAULT);
	/* Somewhere else entirely, page aligned. */
	rtkh_expect_refused(test, &bfr, rtkh_setup(ctx, &bfr, 0x10000000, 0x4000),
			    -EFAULT);
}

static void rtkh_shmem_sram_zero_size(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START + 0x1000, 0),
			    -EFAULT);
}

static void rtkh_shmem_sram_huge(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START, SZ_1G),
			    -EFAULT);
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START,
				       RTKH_TEST_SRAM_SIZE + 1),
			    -EFAULT);
}

/* iova + size wraps: the wrapped end lands back inside the region. */
static void rtkh_shmem_sram_wrap(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START + 0x1000,
				       SIZE_MAX - 0x7ff),
			    -EFAULT);
	rtkh_expect_refused(test, &bfr, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START,
						   SIZE_MAX),
			    -EFAULT);
	rtkh_expect_refused(test, &bfr, rtkh_setup(ctx, &bfr, U64_MAX - 0xfff, 0x2000),
			    -EFAULT);
}

static void rtkh_shmem_no_sram(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	ctx->helper.sram = NULL;
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START, 0x1000),
			    -EFAULT);
}

/* A strict adopted session refuses every new buffer, allocated or placed. */
static void rtkh_shmem_adopted_strict(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	ctx->helper.adopted = true;
	ctx->helper.strict_buffers = true;
	rtkh_expect_refused(test, &bfr, rtkh_setup(ctx, &bfr, 0, 0x4000), -ENODATA);
	KUNIT_EXPECT_EQ(test, bfr.iova, 0);
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START, 0x1000),
			    -ENODATA);

	/* strict_buffers alone, without an adopted session, does not refuse. */
	ctx->helper.adopted = false;
	KUNIT_EXPECT_EQ(test, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START, 0x1000), 0);
}

/* A producer without an inventory (T8140) keeps mapping SRAM IOVAs. */
static void rtkh_shmem_adopted_legacy(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem bfr;

	ctx->helper.adopted = true;
	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &bfr, RTKH_TEST_SRAM_START + 0x4000,
					 0x4000), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)bfr.iomem, ctx->sram_mem + 0x4000);
	rtkh_expect_refused(test, &bfr,
			    rtkh_setup(ctx, &bfr, ctx->sram.end, 2), -EFAULT);
}

/* Setting up the same request twice, and destroying twice, is harmless. */
static void rtkh_shmem_double_setup_destroy(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_shmem a, b;

	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &a, RTKH_TEST_SRAM_START + 0x2000,
					 0x1000), 0);
	KUNIT_ASSERT_EQ(test, rtkh_setup(ctx, &b, RTKH_TEST_SRAM_START + 0x2000,
					 0x1000), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)a.iomem, (__force void *)b.iomem);

	/* Re-running setup on a mapped buffer re-maps it to the same place. */
	KUNIT_ASSERT_EQ(test, apple_rtkit_helper_shmem_setup(&ctx->helper, &a), 0);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)a.iomem, (__force void *)b.iomem);

	apple_rtkit_helper_shmem_destroy(&ctx->helper, &a);
	apple_rtkit_helper_shmem_destroy(&ctx->helper, &a);
	memset(&b, 0, sizeof(b));
	apple_rtkit_helper_shmem_destroy(&ctx->helper, &b);
}

static void rtkh_ops(struct kunit *test)
{
	KUNIT_EXPECT_PTR_EQ(test, apple_rtkit_helper_ops.shmem_setup,
			    &apple_rtkit_helper_shmem_setup);
	KUNIT_EXPECT_PTR_EQ(test, apple_rtkit_helper_ops.shmem_destroy,
			    &apple_rtkit_helper_shmem_destroy);
	/* RTKit refuses ops with both setup flavours; the helper has one. */
	KUNIT_EXPECT_NULL(test, apple_rtkit_helper_ops.shmem_setup_ep);
}

/* The buffer inventory of a handed over session (apple,rtkit-buffers) */

static void rtkh_node(struct rtkh_test *ctx, const char *compatible)
{
	ctx->np.full_name = "mtp";
	if (compatible) {
		ctx->compat.name = "compatible";
		ctx->compat.value = (void *)compatible;
		ctx->compat.length = strlen(compatible) + 1;
		ctx->np.properties = &ctx->compat;
	}
	ctx->dev->of_node = &ctx->np;
}

static void rtkh_inventory(struct rtkh_test *ctx, const u32 *cells, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n && i < ARRAY_SIZE(ctx->cells); i++)
		ctx->cells[i] = cpu_to_be32(cells[i]);
	ctx->inventory.name = "apple,rtkit-buffers";
	ctx->inventory.value = n ? ctx->cells : NULL;
	ctx->inventory.length = n * sizeof(__be32);
	ctx->inventory.next = ctx->np.properties;
	ctx->np.properties = &ctx->inventory;
}

#define RTKH_HI(a)	((u32)((u64)(a) >> 32))
#define RTKH_LO(a)	((u32)(a))

static int rtkh_inherit(struct rtkh_test *ctx, struct apple_rtkit_inherited_buffer *b)
{
	memset(b, 0, sizeof(*b) * APPLE_RTKIT_HELPER_MAX_BUFFERS);
	return apple_rtkit_helper_inherit_buffers(&ctx->helper, b);
}

static void rtkh_inherit_no_inventory(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];

	/* Older producers (T8140) hand over without an inventory. */
	rtkh_node(ctx, "apple,t8140-mtp");
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), 0);

	/* T8132 sessions must describe their buffers. */
	rtkh_node(ctx, "apple,t8132-mtp");
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);
}

static void rtkh_inherit_empty(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];

	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, NULL, 0);
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), 0);
}

static void rtkh_inherit_one_sram(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	const u64 iova = RTKH_TEST_SRAM_START + 0x1000;
	const u32 cells[] = { 2, RTKH_HI(iova), RTKH_LO(iova), 0x2000, 0 };

	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, ARRAY_SIZE(cells));
	KUNIT_ASSERT_EQ(test, rtkh_inherit(ctx, b), 1);
	KUNIT_EXPECT_EQ(test, b[0].endpoint, 2);
	KUNIT_EXPECT_EQ(test, b[0].shmem.iova, iova);
	KUNIT_EXPECT_EQ(test, b[0].shmem.size, 0x2000);
	KUNIT_EXPECT_FALSE(test, b[0].shmem.is_mapped);
	KUNIT_EXPECT_NULL(test, b[0].shmem.buffer);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)b[0].shmem.iomem,
			    ctx->sram_mem + 0x1000);
}

static void rtkh_inherit_max(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	const u64 s = RTKH_TEST_SRAM_START;
	const u32 cells[] = {
		1, RTKH_HI(s), RTKH_LO(s), 0x1000, 0,
		2, RTKH_HI(s + 0x1000), RTKH_LO(s + 0x1000), 0x1000, 1,
		4, RTKH_HI(s + 0x2000), RTKH_LO(s + 0x2000), 0x123, 0,
		8, RTKH_HI(s + 0xf000), RTKH_LO(s + 0xf000), 0x1000, 1,
	};
	static const u8 eps[] = { 1, 2, 4, 8 };
	unsigned int i;

	rtkh_node(ctx, "apple,t8142-mtp");
	rtkh_inventory(ctx, cells, ARRAY_SIZE(cells));
	KUNIT_ASSERT_EQ(test, rtkh_inherit(ctx, b), 4);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, b[i].endpoint, eps[i]);
		KUNIT_EXPECT_EQ(test, b[i].shmem.iova, ((u64)cells[i * 5 + 1] << 32) |
				cells[i * 5 + 2]);
		KUNIT_EXPECT_EQ(test, b[i].shmem.size, cells[i * 5 + 3]);
		KUNIT_EXPECT_EQ(test, b[i].shmem.is_mapped, !!cells[i * 5 + 4]);
		KUNIT_EXPECT_PTR_EQ(test, (__force void *)b[i].shmem.iomem,
				    ctx->sram_mem + (b[i].shmem.iova - s));
	}
}

static void rtkh_inherit_bad_count(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	u32 cells[ARRAY_SIZE(ctx->cells)];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cells); i += 5) {
		cells[i] = 1;
		cells[i + 1] = RTKH_HI(RTKH_TEST_SRAM_START);
		cells[i + 2] = RTKH_LO(RTKH_TEST_SRAM_START);
		cells[i + 3] = 0x1000;
		cells[i + 4] = 0;
	}

	/* Five buffers: one more than the session may hold. */
	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, 5 * (APPLE_RTKIT_HELPER_MAX_BUFFERS + 1));
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);

	/* A partial tuple. */
	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, 7);
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);

	/* Not even a whole cell. */
	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, 1);
	ctx->inventory.length = 3;
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);
}

static void rtkh_inherit_one_bad(struct kunit *test, u32 ep, u64 iova, u32 size,
				 u32 mapped)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	const u32 cells[] = { ep, RTKH_HI(iova), RTKH_LO(iova), size, mapped };

	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, ARRAY_SIZE(cells));
	KUNIT_EXPECT_EQ_MSG(test, rtkh_inherit(ctx, b), -EINVAL,
			    "ep %u iova %#llx size %#x mapped %u", ep, iova, size,
			    mapped);
}

static void rtkh_inherit_bad_tuple(struct kunit *test)
{
	const u64 s = RTKH_TEST_SRAM_START;

	/* Not a system endpoint with a shared buffer. */
	rtkh_inherit_one_bad(test, 0, s, 0x1000, 0);
	rtkh_inherit_one_bad(test, 3, s, 0x1000, 0);
	rtkh_inherit_one_bad(test, 7, s, 0x1000, 0);
	rtkh_inherit_one_bad(test, 0x20, s, 0x1000, 0);
	/* No size. */
	rtkh_inherit_one_bad(test, 2, s, 0, 0);
	/* is_mapped is a flag. */
	rtkh_inherit_one_bad(test, 2, s, 0x1000, 2);
	/* Wraps past the top of the address space. */
	rtkh_inherit_one_bad(test, 2, U64_MAX - 0xfff, 0x2000, 0);
	/* Above the 44 bit DMA mask. */
	rtkh_inherit_one_bad(test, 2, BIT_ULL(44), 0x1000, 0);
	/* Outside SRAM, and no reserved memory region describes it. */
	rtkh_inherit_one_bad(test, 2, 0x10000000, 0x4000, 0);
	rtkh_inherit_one_bad(test, 2, s + RTKH_TEST_SRAM_SIZE - 0x1000, 0x2000, 0);
}

/* One bad tuple refuses the whole inventory. */
static void rtkh_inherit_bad_second(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	const u64 s = RTKH_TEST_SRAM_START;
	const u32 cells[] = {
		2, RTKH_HI(s), RTKH_LO(s), 0x1000, 0,
		4, RTKH_HI(s + 0x1000), RTKH_LO(s + 0x1000), 0x1000, 3,
	};

	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, cells, ARRAY_SIZE(cells));
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);
}

/* The 44 bit limit is inclusive: a buffer may end on its last byte. */
static void rtkh_inherit_dma_mask_edge(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct apple_rtkit_inherited_buffer b[APPLE_RTKIT_HELPER_MAX_BUFFERS];
	const u64 top = DMA_BIT_MASK(44) + 1;
	const u32 ok[] = { 8, RTKH_HI(top - 0x1000), RTKH_LO(top - 0x1000), 0x1000, 0 };
	const u32 over[] = { 8, RTKH_HI(top - 0x1000), RTKH_LO(top - 0x1000), 0x1001, 0 };

	ctx->sram = DEFINE_RES_MEM_NAMED(top - RTKH_TEST_SRAM_SIZE,
					 RTKH_TEST_SRAM_SIZE + 0x1000, "sram");

	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, ok, ARRAY_SIZE(ok));
	KUNIT_ASSERT_EQ(test, rtkh_inherit(ctx, b), 1);
	KUNIT_EXPECT_PTR_EQ(test, (__force void *)b[0].shmem.iomem,
			    ctx->sram_mem + RTKH_TEST_SRAM_SIZE - 0x1000);

	/* Inside this SRAM, but one byte past the DMA mask. */
	rtkh_node(ctx, "apple,t8132-mtp");
	rtkh_inventory(ctx, over, ARRAY_SIZE(over));
	KUNIT_EXPECT_EQ(test, rtkh_inherit(ctx, b), -EINVAL);
}

/* Sleep: the AP power notices */

static void rtkh_expect_calls(struct kunit *test, const bool *on, unsigned int n)
{
	struct rtkh_test *ctx = test->priv;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ctx->ncalls, n);
	for (i = 0; i < n; i++)
		KUNIT_EXPECT_EQ_MSG(test, ctx->sent_on[i], on[i], "notice %u", i);
}

/* A firmware sleep sends one idle notice and one AP on, whichever callback. */
static void rtkh_pm_fw_sleep_cycle(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true };

	pm_set_suspend_via_firmware();
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_TRUE(test, ctx->helper.ap_idle);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend_late(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 1);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);
	apple_rtkit_helper_complete(dev);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_unanswered);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_pending);
}

/* The M5 Pro's idle notice waits for .suspend_late. */
static void rtkh_pm_idle_late(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true };

	ctx->helper.idle_late = true;
	pm_set_suspend_via_firmware();
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend_late(dev), 0);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 1);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), 0);
	apple_rtkit_helper_complete(dev);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

/* A sleep the firmware does not finish tells the co-processor nothing. */
static void rtkh_pm_no_fw_no_notice(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;

	pm_suspend_target_state = PM_SUSPEND_MEM;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend_late(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
	apple_rtkit_helper_complete(dev);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_freeze(dev), 0);
}

/*
 * An unanswered idle notice fails .prepare; the PM notifier ends that sleep.
 * Still no answer: nothing is sent, and the next sleep is refused without a
 * second idle notice.
 */
static void rtkh_pm_unanswered_still_running(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false };

	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, ctx->helper.idle_unanswered);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);

	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_unanswered);
	KUNIT_EXPECT_TRUE(test, ctx->helper.idle_pending);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);

	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), -EBUSY);
	KUNIT_EXPECT_TRUE(test, ctx->helper.idle_pending);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

/* The answer came before the sleep ended: AP on goes out at its end. */
static void rtkh_pm_unanswered_late_answer(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true, false };

	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -ETIMEDOUT);
	ctx->rtk.ap_power_state = RTKH_TEST_PWR_IDLE;

	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_pending);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);
	KUNIT_EXPECT_EQ(test, ctx->rtk.ap_power_state, RTKH_TEST_PWR_ON);

	/* The next sleep starts clean. */
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_TRUE(test, ctx->helper.ap_idle);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

/* A crashed co-processor is not running either: its idle is not pending. */
static void rtkh_pm_unanswered_crashed(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;

	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	ctx->ret[1] = -ENODEV;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -ETIMEDOUT);
	ctx->rtk.crashed = true;
	apple_rtkit_helper_complete(dev);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_pending);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 2);
	KUNIT_EXPECT_TRUE(test, ctx->sent_on[1]);
	/* A failed AP on is not an unanswered idle notice. */
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_unanswered);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);
}

/* The answer came after the sleep ended: the next sleep sends AP on first. */
static void rtkh_pm_pending_answered_later(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true, false, true };

	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -ETIMEDOUT);
	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_ASSERT_TRUE(test, ctx->helper.idle_pending);

	ctx->rtk.ap_power_state = RTKH_TEST_PWR_IDLE;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_pending);
	KUNIT_EXPECT_TRUE(test, ctx->helper.ap_idle);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

static void rtkh_pm_pending_ap_on_fails(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true };

	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	ctx->ret[1] = -EIO;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -ETIMEDOUT);
	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	ctx->rtk.ap_power_state = RTKH_TEST_PWR_IDLE;

	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), -EBUSY);
	KUNIT_EXPECT_TRUE(test, ctx->helper.idle_pending);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

/* .complete and the PM notifier both end a sleep: only once. */
static void rtkh_pm_complete_then_notify(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true };

	ctx->helper.idle_late = true;
	pm_set_suspend_via_firmware();
	ctx->ret[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend_late(dev), -ETIMEDOUT);
	ctx->rtk.ap_power_state = RTKH_TEST_PWR_IDLE;
	apple_rtkit_helper_complete(dev);
	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	apple_rtkit_helper_complete(dev);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

static void rtkh_pm_notify_other_events(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;

	ctx->helper.idle_unanswered = true;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb,
							   PM_SUSPEND_PREPARE, NULL),
			NOTIFY_DONE);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb,
							   PM_POST_HIBERNATION, NULL),
			NOTIFY_DONE);
	KUNIT_EXPECT_TRUE(test, ctx->helper.idle_unanswered);

	ctx->helper.idle_unanswered = false;
	apple_rtkit_helper_pm_notify(&ctx->helper.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 0);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_pending);
}

/* A failed AP on leaves nothing behind; the next sleep idles normally. */
static void rtkh_pm_ap_on_fails(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;
	static const bool want[] = { false, true, false };

	pm_set_suspend_via_firmware();
	ctx->ret[1] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
	KUNIT_EXPECT_FALSE(test, ctx->helper.ap_idle);
	KUNIT_EXPECT_FALSE(test, ctx->helper.idle_unanswered);
	apple_rtkit_helper_complete(dev);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_prepare(dev), 0);
	rtkh_expect_calls(test, want, ARRAY_SIZE(want));
}

/* An adopted session survives s2idle only with its CPU still running. */
static void rtkh_pm_adopted_s2idle(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;

	ctx->helper.adopted = true;
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	rtkh_set_run(ctx, true);
	dev->power.wakeup_path = false;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), 0);
	KUNIT_EXPECT_TRUE(test, device_awake_path(dev));
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);

	rtkh_set_run(ctx, false);
	dev->power.wakeup_path = false;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), -EHOSTDOWN);
	KUNIT_EXPECT_FALSE(test, device_awake_path(dev));
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 0);
}

static void rtkh_pm_adopted_refuses(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;
	struct device *dev = ctx->dev;

	rtkh_set_run(ctx, true);
	pm_suspend_target_state = PM_SUSPEND_MEM;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_freeze(dev), 0);

	ctx->helper.adopted = true;
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_suspend(dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_freeze(dev), -EBUSY);

	/* Not adopted: a stopped CPU does not matter on resume. */
	ctx->helper.adopted = false;
	rtkh_set_run(ctx, false);
	KUNIT_EXPECT_EQ(test, apple_rtkit_helper_resume(dev), 0);
}

static void rtkh_preserves_input(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;

	KUNIT_EXPECT_FALSE(test, apple_rtkit_helper_preserves_input(ctx->dev));
	ctx->helper.preserved = true;
	KUNIT_EXPECT_TRUE(test, apple_rtkit_helper_preserves_input(ctx->dev));
	dev_set_drvdata(ctx->dev, NULL);
	KUNIT_EXPECT_FALSE(test, apple_rtkit_helper_preserves_input(ctx->dev));
	dev_set_drvdata(ctx->dev, &ctx->helper);
}

static void rtkh_pm_register(struct kunit *test)
{
	struct rtkh_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, apple_rtkit_helper_pm_register(&ctx->helper), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->helper.pm_nb.notifier_call,
			    &apple_rtkit_helper_pm_notify);
	/* The device's devres unregisters it when the test ends. */
}

static struct kunit_case apple_rtkit_helper_test_cases[] = {
	KUNIT_CASE(rtkh_ops),
	KUNIT_CASE(rtkh_shmem_dma_alloc),
	KUNIT_CASE(rtkh_shmem_sram_inside),
	KUNIT_CASE(rtkh_shmem_sram_whole),
	KUNIT_CASE(rtkh_shmem_sram_unaligned_edges),
	KUNIT_CASE(rtkh_shmem_sram_outside),
	KUNIT_CASE(rtkh_shmem_sram_zero_size),
	KUNIT_CASE(rtkh_shmem_sram_huge),
	KUNIT_CASE(rtkh_shmem_sram_wrap),
	KUNIT_CASE(rtkh_shmem_no_sram),
	KUNIT_CASE(rtkh_shmem_adopted_strict),
	KUNIT_CASE(rtkh_shmem_adopted_legacy),
	KUNIT_CASE(rtkh_shmem_double_setup_destroy),
	KUNIT_CASE(rtkh_inherit_no_inventory),
	KUNIT_CASE(rtkh_inherit_empty),
	KUNIT_CASE(rtkh_inherit_one_sram),
	KUNIT_CASE(rtkh_inherit_max),
	KUNIT_CASE(rtkh_inherit_bad_count),
	KUNIT_CASE(rtkh_inherit_bad_tuple),
	KUNIT_CASE(rtkh_inherit_bad_second),
	KUNIT_CASE(rtkh_inherit_dma_mask_edge),
	KUNIT_CASE(rtkh_pm_fw_sleep_cycle),
	KUNIT_CASE(rtkh_pm_idle_late),
	KUNIT_CASE(rtkh_pm_no_fw_no_notice),
	KUNIT_CASE(rtkh_pm_unanswered_still_running),
	KUNIT_CASE(rtkh_pm_unanswered_late_answer),
	KUNIT_CASE(rtkh_pm_unanswered_crashed),
	KUNIT_CASE(rtkh_pm_pending_answered_later),
	KUNIT_CASE(rtkh_pm_pending_ap_on_fails),
	KUNIT_CASE(rtkh_pm_complete_then_notify),
	KUNIT_CASE(rtkh_pm_notify_other_events),
	KUNIT_CASE(rtkh_pm_ap_on_fails),
	KUNIT_CASE(rtkh_pm_adopted_s2idle),
	KUNIT_CASE(rtkh_pm_adopted_refuses),
	KUNIT_CASE(rtkh_preserves_input),
	KUNIT_CASE(rtkh_pm_register),
	{}
};

static struct kunit_suite apple_rtkit_helper_test_suite = {
	.name = "apple-rtkit-helper",
	.init = rtkh_test_init,
	.exit = rtkh_test_exit,
	.test_cases = apple_rtkit_helper_test_cases,
};

kunit_test_suite(apple_rtkit_helper_test_suite);
