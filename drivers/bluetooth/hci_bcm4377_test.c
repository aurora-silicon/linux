// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the hci_bcm4377 ring bookkeeping. Built as part of
 * hci_bcm4377.c, so the tests reach its static helpers. They run without
 * the radio: BAR0 and the shared ring state are plain kernel memory.
 */
#include <kunit/test.h>

#define BCM4377_TEST_BAR0_SIZE 0x200

struct bcm4377_test_ctx {
	struct bcm4377_data *bcm4377;
	struct pci_dev *pdev;
};

static int bcm4377_test_init(struct kunit *test)
{
	struct bcm4377_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->pdev = kunit_kzalloc(test, sizeof(*ctx->pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->pdev);
	ctx->pdev->dev.init_name = "bcm4377-kunit";
	ctx->bcm4377 = kunit_kzalloc(test, sizeof(*ctx->bcm4377), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->bcm4377);
	ctx->bcm4377->pdev = ctx->pdev;
	ctx->bcm4377->ring_state =
		kunit_kzalloc(test, sizeof(*ctx->bcm4377->ring_state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->bcm4377->ring_state);
	ctx->bcm4377->bar0 = (void __iomem *)kunit_kzalloc(test, BCM4377_TEST_BAR0_SIZE,
							   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)ctx->bcm4377->bar0);
	test->priv = ctx;
	return 0;
}

/*
 * The slot the host fills comes from its own producer index: whatever the
 * device leaves in the shared head, the entry lands inside the ring.
 */
static void bcm4377_enqueue_uses_host_head_test(struct kunit *test)
{
	struct bcm4377_test_ctx *ctx = test->priv;
	struct bcm4377_data *bcm4377 = ctx->bcm4377;
	struct bcm4377_transfer_ring *ring = &bcm4377->hci_h2d_ring;
	const size_t slot = sizeof(struct bcm4377_xfer_ring_entry) + 16;
	const size_t guard = slot;
	struct bcm4377_xfer_ring_entry *entry;
	u8 data[8], *buf;
	size_t i;

	ring->ring_id = BCM4377_XFER_RING_HCI_H2D;
	ring->n_entries = 4;
	ring->payload_size = 16;
	ring->sync = true;
	spin_lock_init(&ring->lock);
	buf = kunit_kzalloc(test, ring->n_entries * slot + guard, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	ring->ring = buf;
	memset(data, 0x5a, sizeof(data));

	/* A head one past the last slot, as the device could write it. */
	bcm4377->ring_state->xfer_ring_head[ring->ring_id] = cpu_to_le16(4);
	KUNIT_ASSERT_EQ(test, bcm4377_enqueue(bcm4377, ring, data, sizeof(data), false), 0);

	for (i = 0; i < guard; i++)
		KUNIT_EXPECT_EQ(test, buf[ring->n_entries * slot + i], 0);
	entry = (void *)buf;
	KUNIT_EXPECT_EQ(test, le16_to_cpu(entry->len), sizeof(data));
	KUNIT_EXPECT_EQ(test, entry->flags, BCM4377_XFER_RING_FLAG_PAYLOAD_IN_FOOTER);
	KUNIT_EXPECT_EQ(test, ring->head, 1);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(bcm4377->ring_state->xfer_ring_head[ring->ring_id]), 1);

	/* A far-off value written later does not move the next slot either. */
	bcm4377->ring_state->xfer_ring_head[ring->ring_id] = cpu_to_le16(0xffff);
	KUNIT_ASSERT_EQ(test, bcm4377_enqueue(bcm4377, ring, data, sizeof(data), false), 0);
	entry = (void *)(buf + slot);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(entry->len), sizeof(data));
	KUNIT_EXPECT_EQ(test, ring->head, 2);
	for (i = 0; i < guard; i++)
		KUNIT_EXPECT_EQ(test, buf[ring->n_entries * slot + i], 0);
}

static struct kunit_case bcm4377_test_cases[] = {
	KUNIT_CASE(bcm4377_enqueue_uses_host_head_test),
	{}
};

static struct kunit_suite bcm4377_test_suite = {
	.name = "hci_bcm4377",
	.init = bcm4377_test_init,
	.test_cases = bcm4377_test_cases,
};

kunit_test_suite(bcm4377_test_suite);
