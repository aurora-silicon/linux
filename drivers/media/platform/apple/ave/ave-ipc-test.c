// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the AVE IPC parsers and bounds checks: everything the
 * firmware writes into the shared window (hello words, channel tables, ring
 * slots, acks, messages, shared-memory requests) must stay inside it.
 *
 * Included at the end of ave-ipc.c to reach its static helpers. The shared
 * window and the mailbox are plain kernel memory; no hardware is touched.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <kunit/device.h>
#include <kunit/test.h>

#define T_FW_BASE 0x100000000ULL
/* Large enough to hold the whole 6070 command area. */
#define T_IPC_BYTES 0x160000
/* Bytes past ipc.size kept as a canary: nothing may write there. */
#define T_CANARY_BYTES SZ_16K
#define T_CANARY 0xa5

struct t_ipc {
	struct ave_dev *ave;
	struct ave_fw_transport *t;
	struct ave_fw_abi *abi;
	struct ave_fw_layout *layout;
};

/* A device with a private copy of @base's layout, ABI and transport. */
static void t_ipc_init(struct kunit *test, struct t_ipc *ti,
		       const struct ave_fw_layout *base, size_t ipc_bytes)
{
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	static atomic_t seq = ATOMIC_INIT(0);
	char name[32];

	KUNIT_ASSERT_NOT_NULL(test, ave);
	ti->ave = ave;
	ti->t = kunit_kmalloc(test, sizeof(*ti->t), GFP_KERNEL);
	ti->abi = kunit_kmalloc(test, sizeof(*ti->abi), GFP_KERNEL);
	ti->layout = kunit_kmalloc(test, sizeof(*ti->layout), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, ti->t && ti->abi && ti->layout);
	*ti->layout = *base;
	*ti->abi = *base->abi;
	*ti->t = *base->abi->transport;
	ti->abi->transport = ti->t;
	ti->layout->abi = ti->abi;
	ave->layout = ti->layout;
	ave->variant = &ave_t6000;

	/* Several devices per case; names must be unique. */
	snprintf(name, sizeof(name), "apple-ave-ipc-%d", atomic_inc_return(&seq));
	ave->dev = kunit_device_register(test, name);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ave->dev));
	ave->ipc.cpu = kunit_kzalloc(test, ipc_bytes + T_CANARY_BYTES,
				     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ave->ipc.cpu);
	memset(ave->ipc.cpu + ipc_bytes, T_CANARY, T_CANARY_BYTES);
	ave->ipc.size = ipc_bytes;
	ave->ipc_fw_base = T_FW_BASE;
	ave->mbox = (void __iomem __force *)kunit_kzalloc(test, SZ_4K,
							  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)ave->mbox);
	spin_lock_init(&ave->irq_lock);
	init_waitqueue_head(&ave->irq_wait);
	ave->fw_state = AVE_FW_RUNNING;
}

static void t_canary_intact(struct kunit *test, struct ave_dev *ave)
{
	KUNIT_EXPECT_NULL(test, memchr_inv(ave->ipc.cpu + ave->ipc.size,
					   T_CANARY, T_CANARY_BYTES));
}

static __le64 *t_slot(struct ave_dev *ave, const struct ave_ring *r,
		      unsigned int n)
{
	return ave->ipc.cpu + r->offset + n * AVE_RING_SLOT_BYTES;
}

/* Lay out one ring as ave_ipc_parse_channels() leaves it. */
static void t_ring(struct ave_dev *ave, unsigned int id, u32 offset,
		   u32 slots, u32 doorbell, bool to_host)
{
	struct ave_ring *r = &ave->ring[id];

	*r = (struct ave_ring){
		.mode = to_host ? 0 : 1,
		.doorbell = doorbell,
		.slots = slots,
		.offset = AVE_IPC_CHANNEL_OFF + offset,
		.consumer = to_host ? 0 : -1,
		.producer = to_host ? -1 : 0,
	};
	/*
	 * Bit 0 set: a host-to-firmware slot is free for the host, a
	 * firmware-to-host slot still belongs to the firmware.
	 */
	for (unsigned int n = 0; n < slots; n++)
		*t_slot(ave, r, n) = cpu_to_le64(1);
}

/* IO and IO_T2H with four slots each, as on 6070. */
static void t_rings(struct ave_dev *ave)
{
	memset(ave->ring, 0, sizeof(ave->ring));
	t_ring(ave, AVE_RING_IO, 0x800, 4, 1, false);
	t_ring(ave, AVE_RING_T2H, 0xa00, 4, 3, true);
}

/* Play the firmware: post @addr/@len to the host in ring slot @n. */
static void t_fw_post(struct ave_dev *ave, unsigned int id, unsigned int n,
		      u64 addr, u64 len)
{
	__le64 *slot = t_slot(ave, &ave->ring[id], n);

	slot[1] = cpu_to_le64(len);
	slot[2] = 0;
	/* Host-to-firmware slots are acked by setting bit 0, others by clearing it. */
	slot[0] = cpu_to_le64(addr | ave->ring[id].mode);
}

static void t_ring_indices_valid(struct kunit *test, const struct ave_ring *r)
{
	KUNIT_EXPECT_GE(test, r->consumer, -1);
	KUNIT_EXPECT_LT(test, r->consumer, (int)r->slots);
	KUNIT_EXPECT_GE(test, r->producer, -1);
	KUNIT_EXPECT_LT(test, r->producer, (int)r->slots);
}

/* 9003 hello: the descriptor area and the heap are both bounded. */
static void ave_ipc_test_hello_9003(struct kunit *test)
{
	const struct ave_fw_transport *t = ave_abi_9003.transport;
	u32 min = t->descriptors * AVE_CHAN_DESC_STRIDE;
	u32 max = AVE_IPC_HANDSHAKE_OFF - AVE_IPC_CHANNEL_OFF;
	u32 hello[4] = { t->hello[0], min, t->hello[2], SZ_1M };

	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), 0);
	hello[1] = min - 1;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[1] = max;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), 0);
	/* A channel area running into the handshake block. */
	hello[1] = max + 1;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[1] = U32_MAX;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[1] = min;
	/* 9003 always asks for a heap, never more than 4 MiB. */
	hello[3] = 0;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[3] = 4 * SZ_1M;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), 0);
	hello[3] = 4 * SZ_1M + 1;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[3] = U32_MAX;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[3] = SZ_1M;
	hello[0] = t->hello[0] + 1;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
	hello[0] = t->hello[0];
	hello[2] = 0;
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
}

/* A bad hello is refused before the mailbox or the window are touched. */
static void ave_ipc_test_negotiate_bad_hello(struct kunit *test)
{
	struct t_ipc ti;
	u32 hello[4];

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	memcpy(hello, ti.t->hello, sizeof(hello));
	hello[1]++;
	KUNIT_EXPECT_EQ(test, ave_ipc_negotiate(ti.ave, hello), -EPROTO);
	KUNIT_EXPECT_EQ(test, readl(ti.ave->mbox + AVE_MBOX_DOORBELL), 0U);
	KUNIT_EXPECT_EQ(test, ti.ave->channel_bytes, 0U);
	/* A heap larger than 4 MiB is not allocated either. */
	memcpy(hello, ti.t->hello, sizeof(hello));
	hello[3] = 4 * SZ_1M + 1;
	KUNIT_EXPECT_EQ(test, ave_ipc_negotiate(ti.ave, hello), -EPROTO);
	KUNIT_EXPECT_NULL(test, ti.ave->heap.cpu);
}

/*
 * The fake mailbox answers with the words the host wrote, so the IPC window
 * address the "firmware" reports is ipc.dma. It must be 16 KiB aligned and
 * the whole window must fit below 2^64.
 */
static void ave_ipc_test_negotiate_window(struct kunit *test)
{
	static const u64 bad[] = {
		0, T_FW_BASE + SZ_4K, 0xffffffffffffc000ULL,
	};
	struct t_ipc ti;
	u8 *hs;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	hs = ti.ave->ipc.cpu + AVE_IPC_HANDSHAKE_OFF;
	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		ti.ave->ipc.dma = bad[i];
		memset(ti.ave->ipc.cpu + AVE_IPC_CHANNEL_OFF, T_CANARY, 0x100);
		KUNIT_EXPECT_EQ(test, ave_ipc_negotiate(ti.ave, ti.t->hello),
				-EPROTO);
		/* Nothing was cleared or handed to the firmware. */
		KUNIT_EXPECT_EQ(test,
				*(u8 *)(ti.ave->ipc.cpu + AVE_IPC_CHANNEL_OFF),
				T_CANARY);
	}

	/*
	 * A good window gets the handshake block, but the echoed handshake
	 * address is not the channel area: refused before any channel is parsed.
	 */
	ti.ave->ipc.dma = T_FW_BASE;
	KUNIT_EXPECT_EQ(test, ave_ipc_negotiate(ti.ave, ti.t->hello), -EPROTO);
	KUNIT_EXPECT_EQ(test, ti.ave->ipc_fw_base, T_FW_BASE);
	KUNIT_EXPECT_EQ(test, ti.ave->channel_bytes, ti.t->hello[1]);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(hs + AVE_HS_CHANNELS),
			T_FW_BASE + AVE_IPC_CHANNEL_OFF);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(hs + AVE_HS_DEVICE_TYPE),
			ave_t6000.device_type);
	KUNIT_EXPECT_EQ(test, ti.ave->ring[AVE_RING_IO].slots, 0U);
	t_canary_intact(test, ti.ave);
}

/* A two-descriptor (9003-style) table: IO then IO_T2H. */
#define T_CHAN_BYTES 0x1000

static u8 *t_desc(struct ave_dev *ave, unsigned int i)
{
	return ave->ipc.cpu + AVE_IPC_CHANNEL_OFF + i * AVE_CHAN_DESC_STRIDE;
}

static void t_write_desc(struct ave_dev *ave, unsigned int i, const char *name,
			 u32 dir, u32 doorbell, u32 slots, u64 addr)
{
	u8 *desc = t_desc(ave, i);

	memset(desc, 0, AVE_CHAN_DESC_STRIDE);
	strscpy(desc, name, AVE_CHAN_DESC_NAME_LEN);
	put_unaligned_le32(dir, desc + AVE_CHAN_DESC_DIR);
	put_unaligned_le32(doorbell, desc + AVE_CHAN_DESC_IRQ);
	put_unaligned_le32(slots, desc + AVE_CHAN_DESC_SLOTS);
	put_unaligned_le64(addr, desc + AVE_CHAN_DESC_ADDR);
}

static u64 t_chan_addr(u32 off)
{
	return T_FW_BASE + AVE_IPC_CHANNEL_OFF + off;
}

static void t_table_9003(struct ave_dev *ave)
{
	t_write_desc(ave, 0, "IO", 0, 1, 8, t_chan_addr(0x200));
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8, t_chan_addr(0x400));
}

static void t_channels_init(struct kunit *test, struct t_ipc *ti)
{
	t_ipc_init(test, ti, &ave_layout_9003_78_0, T_IPC_BYTES);
	ti->ave->channel_bytes = T_CHAN_BYTES;
	t_table_9003(ti->ave);
	KUNIT_ASSERT_EQ(test, ave_ipc_parse_channels(ti->ave), 0);
}

static void ave_ipc_test_channels_9003(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;

	t_channels_init(test, &ti);
	ave = ti.ave;
	KUNIT_EXPECT_STREQ(test, ave->ring[AVE_RING_IO].name, "IO");
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].offset,
			AVE_IPC_CHANNEL_OFF + 0x200);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].mode, 1U);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].producer, 0);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].consumer, -1);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_T2H].mode, 0U);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_T2H].producer, -1);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_T2H].consumer, 0);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_SMALLOC].slots, 0U);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_LOG].slots, 0U);
	for (unsigned int n = 0; n < 8; n++)
		KUNIT_EXPECT_EQ(test,
				le64_to_cpu(*t_slot(ave, &ave->ring[AVE_RING_IO], n)),
				1ULL);
	/* The slot after the ring is not handed over. */
	KUNIT_EXPECT_EQ(test,
			le64_to_cpu(*t_slot(ave, &ave->ring[AVE_RING_IO], 8)),
			0ULL);

	/* The last ring may end exactly at the end of the channel area. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(T_CHAN_BYTES - 8 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	/* Doorbell 31 is the last mailbox bit. */
	t_write_desc(ave, 1, "IO_T2H", 1, 31, 8, t_chan_addr(0x400));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_T2H].doorbell, 31U);
	/* Order does not matter on a table with no fixed indices. */
	t_write_desc(ave, 0, "IO_T2H", 1, 3, 8, t_chan_addr(0x400));
	t_write_desc(ave, 1, "IO", 0, 1, 8, t_chan_addr(0x200));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].offset,
			AVE_IPC_CHANNEL_OFF + 0x200);
}

/* Every malformed descriptor the firmware could write is refused. */
static void ave_ipc_test_channels_malformed(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	u8 *desc;

	t_channels_init(test, &ti);
	ave = ti.ave;
	desc = t_desc(ave, 0);

	/* A name with no terminating NUL in its 64 bytes. */
	memset(desc, 'A', AVE_CHAN_DESC_NAME_LEN);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	/* A two-channel table has no room for unknown channels. */
	t_write_desc(ave, 0, "DEBUG", 0, 1, 8, t_chan_addr(0x200));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	/* Wrong direction. */
	put_unaligned_le32(1, desc + AVE_CHAN_DESC_DIR);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(2, desc + AVE_CHAN_DESC_DIR);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	/* Doorbells outside the 32-bit doorbell register, or none for IO. */
	put_unaligned_le32(32, desc + AVE_CHAN_DESC_IRQ);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(U32_MAX, desc + AVE_CHAN_DESC_IRQ);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(0, desc + AVE_CHAN_DESC_IRQ);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	/* No slots. */
	put_unaligned_le32(0, desc + AVE_CHAN_DESC_SLOTS);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* More slots than the channel area holds (slots * 64 wraps in u32). */
	put_unaligned_le32(T_CHAN_BYTES / AVE_RING_SLOT_BYTES + 1,
			   desc + AVE_CHAN_DESC_SLOTS);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(0x04000000, desc + AVE_CHAN_DESC_SLOTS);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(U32_MAX, desc + AVE_CHAN_DESC_SLOTS);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	/* Two IO channels and no IO_T2H. */
	t_write_desc(ave, 1, "IO", 0, 1, 8, t_chan_addr(0x400));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* Two IO_T2H channels and no IO. */
	t_write_desc(ave, 0, "IO_T2H", 1, 3, 8, t_chan_addr(0x200));
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8, t_chan_addr(0x400));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_9003(ave);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
}

/* Ring addresses must land inside the channel area, past the descriptors. */
static void ave_ipc_test_channels_out_of_range(struct kunit *test)
{
	static const u64 bad[] = {
		/* Below the window, and far below it (u64 wrap). */
		T_FW_BASE - AVE_RING_SLOT_BYTES,
		0,
		/* Not slot aligned. */
		T_FW_BASE + AVE_IPC_CHANNEL_OFF + 0x208,
		/* On top of the descriptor table. */
		T_FW_BASE + AVE_IPC_CHANNEL_OFF,
		T_FW_BASE + AVE_IPC_CHANNEL_OFF + 2 * AVE_CHAN_DESC_STRIDE -
			AVE_RING_SLOT_BYTES,
		/* One slot past the end of the channel area. */
		T_FW_BASE + AVE_IPC_CHANNEL_OFF + T_CHAN_BYTES -
			7 * AVE_RING_SLOT_BYTES,
		/* Inside the window but past the channel area. */
		T_FW_BASE + AVE_IPC_HANDSHAKE_OFF,
		/* Past the window, and at the top of the address space. */
		T_FW_BASE + T_IPC_BYTES,
		U64_MAX & ~(u64)(AVE_RING_SLOT_BYTES - 1),
	};
	struct ave_dev *ave;
	struct t_ipc ti;

	t_channels_init(test, &ti);
	ave = ti.ave;
	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		for (unsigned int d = 0; d < 2; d++) {
			t_table_9003(ave);
			put_unaligned_le64(bad[i], t_desc(ave, d) + AVE_CHAN_DESC_ADDR);
			KUNIT_EXPECT_EQ_MSG(test, ave_ipc_parse_channels(ave), -EPROTO,
					    "descriptor %u at %#llx", d, bad[i]);
		}
	}
	t_canary_intact(test, ave);
}

/* Rings may touch but not share a slot. */
static void ave_ipc_test_channels_overlap(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;

	t_channels_init(test, &ti);
	ave = ti.ave;
	/* IO_T2H starting in IO's last slot. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(0x200 + 7 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* Same place. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8, t_chan_addr(0x200));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* A one-slot ring inside the other. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 1,
		     t_chan_addr(0x200 + 3 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* Back to back, either way round. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(0x200 + 8 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	/* IO further up, so that IO_T2H can sit right below it. */
	t_write_desc(ave, 0, "IO", 0, 1, 8, t_chan_addr(0x600));
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(0x600 - 8 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	/* IO_T2H ending in IO's first slot. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(0x600 - 7 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* IO_T2H starting in IO's last slot. */
	t_write_desc(ave, 1, "IO_T2H", 1, 3, 8,
		     t_chan_addr(0x600 + 7 * AVE_RING_SLOT_BYTES));
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
}

/* The 6070 table: unknown channels are skipped but still bounds checked. */
static const struct {
	const char *name;
	u32 dir, doorbell, slots, offset;
} t_6070_desc[] = {
	{ "TERMINAL", 2, 0, 512, 0x700 }, { "IO", 0, 1, 32, 0x8700 },
	{ "DEBUG", 0, 1, 8, 0x8f00 },	  { "BUF_H2T", 0, 2, 1, 0x9100 },
	{ "BUF_T2H", 1, 3, 1, 0x9140 },	  { "SHAREDMALLOC", 1, 3, 8, 0x9180 },
	{ "IO_T2H", 1, 3, 32, 0x9380 },
};

static void t_table_6070(struct ave_dev *ave)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(t_6070_desc); i++)
		t_write_desc(ave, i, t_6070_desc[i].name, t_6070_desc[i].dir,
			     t_6070_desc[i].doorbell, t_6070_desc[i].slots,
			     t_chan_addr(t_6070_desc[i].offset));
}

static void ave_ipc_test_channels_6070_unknown(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	u8 *debug;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti.ave;
	ave->channel_bytes = ti.t->hello[1];
	t_table_6070(ave);
	KUNIT_ASSERT_EQ(test, ave_ipc_parse_channels(ave), 0);

	debug = t_desc(ave, 2);
	/* An unused channel pointing past the channel area. */
	put_unaligned_le64(t_chan_addr(ave->channel_bytes), debug + AVE_CHAN_DESC_ADDR);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* Or outside the window altogether. */
	put_unaligned_le64(T_FW_BASE + T_IPC_BYTES, debug + AVE_CHAN_DESC_ADDR);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	/* Or with an unterminated name. */
	t_table_6070(ave);
	memset(debug, 'D', AVE_CHAN_DESC_NAME_LEN);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_6070(ave);
	/* IO must be exactly where this firmware puts it. */
	put_unaligned_le64(t_chan_addr(0x8700 + AVE_RING_SLOT_BYTES),
			   t_desc(ave, 1) + AVE_CHAN_DESC_ADDR);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_6070(ave);
	put_unaligned_le32(2, t_desc(ave, 6) + AVE_CHAN_DESC_IRQ);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	t_table_6070(ave);
	/* With no shared-memory pool, SHAREDMALLOC is an unknown channel. */
	ti.t->smalloc_bytes = 0;
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), 0);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_SMALLOC].slots, 0U);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_LOG].slots, 512U);
	t_canary_intact(test, ave);
}

/* Host-to-firmware ring: fill, refuse when full, wrap, drain to empty. */
static void ave_ipc_test_ring_h2f(struct kunit *test)
{
	struct ave_ring *r;
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 addr;
	u32 len;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti.ave;
	t_rings(ave);
	r = &ave->ring[AVE_RING_IO];

	KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), -EAGAIN);
	for (unsigned int n = 0; n < 4; n++) {
		KUNIT_ASSERT_EQ(test, ave_ring_send(ave, r, 0x1000 * (n + 1), 0x40 + n), 0);
		KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, n)[0]),
				0x1000ULL * (n + 1));
		KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, n)[1]),
				0x40ULL + n);
		t_ring_indices_valid(test, r);
	}
	KUNIT_EXPECT_EQ(test, readl(ave->mbox + AVE_MBOX_DOORBELL), BIT(1));
	/* Full: a fifth command must not overwrite one the firmware owns. */
	KUNIT_EXPECT_EQ(test, r->producer, -1);
	KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, 0x9000, 0x99), -ENOSPC);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, 0)[0]), 0x1000ULL);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, 0)[1]), 0x40ULL);

	/* Not acked yet. */
	KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), -EAGAIN);
	/* The ack of slot 0 frees exactly that slot. */
	t_fw_post(ave, AVE_RING_IO, 0, 0x1000, 0);
	KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), 0);
	KUNIT_EXPECT_EQ(test, addr, 0x1000ULL);
	KUNIT_EXPECT_EQ(test, len, 0U);
	KUNIT_EXPECT_EQ(test, r->producer, 0);
	KUNIT_EXPECT_EQ(test, r->consumer, 1);
	/* Wrap into slot 0; the ring is full again. */
	KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, 0x5000, 0x44), 0);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, 0)[0]), 0x5000ULL);
	KUNIT_EXPECT_EQ(test, r->producer, -1);
	KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, 0x9000, 0x99), -ENOSPC);

	/* Acks come back in ring order 1, 2, 3, 0. */
	for (unsigned int k = 0; k < 4; k++) {
		unsigned int n = (k + 1) % 4;
		u64 want = n ? 0x1000ULL * (n + 1) : 0x5000ULL;

		t_fw_post(ave, AVE_RING_IO, n, want, 0);
		KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), 0);
		KUNIT_EXPECT_EQ(test, addr, want);
		t_ring_indices_valid(test, r);
	}
	/* Empty. */
	KUNIT_EXPECT_EQ(test, r->consumer, -1);
	KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), -EAGAIN);
	KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, 0x6000, 0x40), 0);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, 1)[0]), 0x6000ULL);
}

/* Firmware-to-host ring: the two low bits never reach the address. */
static void ave_ipc_test_ring_f2h(struct kunit *test)
{
	struct ave_ring *r;
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 addr;
	u32 len;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti.ave;
	t_rings(ave);
	r = &ave->ring[AVE_RING_T2H];

	/* Every slot still belongs to the firmware. */
	KUNIT_EXPECT_EQ(test, ave_ring_recv(ave, r, &addr, &len), -EAGAIN);
	KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, 0x1000, 0x40), -ENOSPC);
	for (unsigned int k = 0; k < 6; k++) {
		unsigned int n = k % 4;

		t_fw_post(ave, AVE_RING_T2H, n, 0x2000 * (k + 1) | 2, 0x40);
		KUNIT_ASSERT_EQ(test, ave_ring_recv(ave, r, &addr, &len), 0);
		KUNIT_EXPECT_EQ(test, addr, 0x2000ULL * (k + 1));
		KUNIT_EXPECT_EQ(test, len, 0x40U);
		/* Next to read is the following slot, next to give back this one. */
		KUNIT_EXPECT_EQ(test, r->consumer, (int)((n + 1) % 4));
		KUNIT_EXPECT_EQ(test, r->producer, (int)n);
		/* Give it back: bit 0 set hands the slot to the firmware. */
		KUNIT_ASSERT_EQ(test, ave_ring_send(ave, r, addr, len), 0);
		KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, n)[0]), addr | 1);
		KUNIT_EXPECT_EQ(test, r->consumer, (int)((n + 1) % 4));
		KUNIT_EXPECT_EQ(test, r->producer, -1);
		KUNIT_EXPECT_EQ(test, ave_ring_send(ave, r, addr, len), -ENOSPC);
	}
	KUNIT_EXPECT_EQ(test, readl(ave->mbox + AVE_MBOX_DOORBELL), BIT(3));
}

/* Every command buffer maps to its own offset and back; nothing else does. */
static void ave_ipc_test_buf_of(struct kunit *test)
{
	const struct ave_fw_layout *layouts[] = {
		&ave_layout_9003_78_0, &ave_layout_6070_11_1,
	};

	for (unsigned int l = 0; l < ARRAY_SIZE(layouts); l++) {
		const struct ave_fw_transport *t;
		struct ave_dev *ave;
		struct t_ipc ti;
		size_t max, prev_end = 0;
		int off;

		t_ipc_init(test, &ti, layouts[l], SZ_4K);
		ave = ti.ave;
		t = ti.t;
		for (unsigned int buf = 0; buf < AVE_BUF_COUNT; buf++) {
			off = ave_ipc_buf_offset(ave, buf, &max);
			KUNIT_EXPECT_GE(test, off, (int)t->command_offset);
			KUNIT_EXPECT_GE(test, (size_t)off, prev_end);
			KUNIT_EXPECT_LE(test, off + max, ave_ipc_command_end(ave));
			prev_end = off + max;
			KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, T_FW_BASE + off),
					(int)buf);
			/* Inside a buffer is not a buffer. */
			KUNIT_EXPECT_EQ(test,
					ave_ipc_buf_of(ave, T_FW_BASE + off + 4),
					-EPROTO);
		}
		KUNIT_EXPECT_EQ(test, prev_end, (size_t)ave_ipc_command_end(ave));
		KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, 0), -EPROTO);
		KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, T_FW_BASE - 1), -EPROTO);
		KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, T_FW_BASE), -EPROTO);
		KUNIT_EXPECT_EQ(test,
				ave_ipc_buf_of(ave, T_FW_BASE + t->command_offset - 4),
				-EPROTO);
		KUNIT_EXPECT_EQ(test,
				ave_ipc_buf_of(ave, T_FW_BASE + ave_ipc_command_end(ave)),
				-EPROTO);
		KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, U64_MAX), -EPROTO);
		/* A buffer offset with no window base under it. */
		KUNIT_EXPECT_EQ(test, ave_ipc_buf_of(ave, t->command_offset), -EPROTO);
	}
}

/* A command buffer is only handed out when it fits the window. */
static void ave_ipc_test_cmd_bounds(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	size_t max;
	int off;
	u8 *cmd;

	/* A window that ends half way into the first Process buffer. */
	t_ipc_init(test, &ti, &ave_layout_6070_11_1,
		   ave_layout_6070_11_1.abi->transport->process_offset + SZ_32K);
	ave = ti.ave;

	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_COUNT, 4));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, U32_MAX, 4));
	off = ave_ipc_buf_offset(ave, AVE_BUF_CMD, &max);
	memset(ave->ipc.cpu + off, 0x5a, 0x100);
	cmd = ave_ipc_cmd(ave, AVE_BUF_CMD, 0x80);
	KUNIT_ASSERT_PTR_EQ(test, (void *)cmd, ave->ipc.cpu + off);
	KUNIT_EXPECT_NULL(test, memchr_inv(cmd, 0, 0x80));
	KUNIT_EXPECT_EQ(test, cmd[0x80], 0x5a);
	KUNIT_EXPECT_NOT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_CMD, max));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_CMD, max + 1));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_SCALAR(0),
					    AVE_IPC_SCALAR_BYTES + 1));
	KUNIT_EXPECT_NOT_NULL(test,
			      ave_ipc_cmd(ave, AVE_BUF_SCALAR(AVE_IPC_SCALAR_BUFS - 1),
					  AVE_IPC_SCALAR_BYTES));
	/* A buffer the firmware still owns is not handed out. */
	ave->unacked = BIT(AVE_BUF_SCALAR(3));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_SCALAR(3), 4));
	ave->unacked = 0;
	/* Process buffer 0 fits up to the end of the window, not past it. */
	KUNIT_EXPECT_NOT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_PROC(0), SZ_32K));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_PROC(0), SZ_32K + 4));
	/* Process buffer 1 starts past the window. */
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_PROC(1), 4));
	KUNIT_EXPECT_NULL(test, ave_ipc_cmd(ave, AVE_BUF_COUNT - 1, 4));
	t_canary_intact(test, ave);

	/* Submit applies the same bounds before it reads the command. */
	mutex_init(&ave->hw_lock);
	mutex_lock(&ave->hw_lock);
	KUNIT_EXPECT_EQ(test, ave_ipc_submit(ave, AVE_BUF_PROC(0), SZ_32K + 4, 0, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ave_ipc_submit(ave, AVE_BUF_PROC(1), 0x40, 0, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ave_ipc_submit(ave, AVE_BUF_COUNT, 0x40, 0, false),
			-EINVAL);
	mutex_unlock(&ave->hw_lock);
	KUNIT_EXPECT_EQ(test, ave->unacked, 0U);
	KUNIT_EXPECT_EQ(test, ave->ring[AVE_RING_IO].slots, 0U);
}

/* Command acks: only the exact address of a buffer the firmware holds. */
static void ave_ipc_test_poll_acks(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 good;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti.ave;
	good = T_FW_BASE + ti.t->scalar_offset + 2 * AVE_IPC_SCALAR_BYTES;

	/* Nothing pending. */
	t_rings(ave);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), 0);

	/* A good ack clears exactly its buffer. */
	ave->unacked = BIT(AVE_BUF_SCALAR(2)) | BIT(AVE_BUF_CMD);
	KUNIT_ASSERT_EQ(test, ave_ring_send(ave, &ave->ring[AVE_RING_IO], good, 0x40), 0);
	t_fw_post(ave, AVE_RING_IO, 0, good, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), 1);
	KUNIT_EXPECT_EQ(test, ave->unacked, (u32)BIT(AVE_BUF_CMD));

	/* An ack with a payload. */
	t_rings(ave);
	ave->unacked = BIT(AVE_BUF_SCALAR(2));
	ave_ring_send(ave, &ave->ring[AVE_RING_IO], good, 0x40);
	t_fw_post(ave, AVE_RING_IO, 0, good, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	KUNIT_EXPECT_EQ(test, ave->unacked, (u32)BIT(AVE_BUF_SCALAR(2)));
	/* An ack inside a buffer. */
	t_rings(ave);
	ave_ring_send(ave, &ave->ring[AVE_RING_IO], good, 0x40);
	t_fw_post(ave, AVE_RING_IO, 0, good + 0x40, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	/* An ack outside the window. */
	t_rings(ave);
	ave_ring_send(ave, &ave->ring[AVE_RING_IO], good, 0x40);
	t_fw_post(ave, AVE_RING_IO, 0, good - T_FW_BASE - 0x1000000, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	/* An ack for a buffer the host never sent. */
	t_rings(ave);
	ave_ring_send(ave, &ave->ring[AVE_RING_IO], good, 0x40);
	t_fw_post(ave, AVE_RING_IO, 0,
		  T_FW_BASE + ti.t->scalar_offset + 3 * AVE_IPC_SCALAR_BYTES, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	KUNIT_EXPECT_EQ(test, ave->unacked, (u32)BIT(AVE_BUF_SCALAR(2)));
}

/* Play the firmware: a 6070 lifecycle reply at window offset @off. */
static void t_reply(struct t_ipc *ti, u32 off, enum ave_opcode op, u32 client,
		    u32 subtype, u32 status)
{
	u8 *msg = ti->ave->ipc.cpu + off;

	memset(msg, 0, 0x40);
	put_unaligned_le16(ti->abi->reply_id[op], msg);
	put_unaligned_le32(client, msg + 0x10);
	put_unaligned_le32(subtype, msg + 0x1c);
	put_unaligned_le32(status, msg + 0x38);
}

#define T_MSG_OFF AVE_IPC_SHARED_OFF
#define T_REPLY_OK 0x00ee0000

static void t_msg_init(struct kunit *test, struct t_ipc *ti)
{
	struct ave_dev *ave;

	t_ipc_init(test, ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti->ave;
	t_rings(ave);
	ave->want[AVE_OP_OPEN] = 1;
	ave->expected[AVE_OP_OPEN].client = 10;
	ave->expected[AVE_OP_OPEN].subtype = ti->abi->semantic[AVE_OP_OPEN];
	t_reply(ti, T_MSG_OFF, AVE_OP_OPEN, 10, ti->abi->semantic[AVE_OP_OPEN],
		T_REPLY_OK);
	memset(ave->msg, 0x5a, sizeof(ave->msg));
}

/* The message is copied only when it lies inside the window, outside the commands. */
static void ave_ipc_test_poll_msg_bounds(struct kunit *test)
{
	const struct ave_fw_transport *t = ave_layout_6070_11_1.abi->transport;
	static const struct {
		u64 off;
		u64 len;
	} bad[] = {
		/* Larger than the host's message buffer. */
		{ T_MSG_OFF, AVE_MSG_MAX + 1 },
		{ T_MSG_OFF, U32_MAX },
		/* Running off the end of the window. */
		{ T_IPC_BYTES - 0x20, 0x40 },
		{ T_IPC_BYTES, 0x40 },
		/* In, or straddling into or out of, the command buffers. */
		{ 0x40000, 0x40 },
		{ 0x40000 - 0x20, 0x40 },
		{ 0x78000 + AVE_IPC_PROC_BUFS * 0x10000 - 0x20, 0x40 },
	};
	struct t_ipc ti;

	/* The offsets above are the 6070 command area. */
	KUNIT_ASSERT_EQ(test, t->command_offset, 0x40000U);
	KUNIT_ASSERT_EQ(test, t->process_offset + AVE_IPC_PROC_BUFS * t->process_bytes,
			0x78000U + AVE_IPC_PROC_BUFS * 0x10000);
	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		t_msg_init(test, &ti);
		t_fw_post(ti.ave, AVE_RING_T2H, 0, T_FW_BASE + bad[i].off, bad[i].len);
		KUNIT_EXPECT_EQ_MSG(test, ave_ipc_poll_once(ti.ave), -EPROTO,
				    "message %#llx/%#llx", bad[i].off, bad[i].len);
		KUNIT_EXPECT_NULL(test, memchr_inv(ti.ave->msg, 0x5a, sizeof(ti.ave->msg)));
		KUNIT_EXPECT_EQ(test, ti.ave->want[AVE_OP_OPEN], 1);
	}
	/* Below the window, and an address that only wraps into it. */
	t_msg_init(test, &ti);
	t_fw_post(ti.ave, AVE_RING_T2H, 0, T_FW_BASE - 0x40, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ti.ave), -EPROTO);
	t_msg_init(test, &ti);
	t_fw_post(ti.ave, AVE_RING_T2H, 0, T_MSG_OFF - T_FW_BASE, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ti.ave), -EPROTO);
	KUNIT_EXPECT_NULL(test, memchr_inv(ti.ave->msg, 0x5a, sizeof(ti.ave->msg)));

	/* Right below the command area is fine. */
	t_msg_init(test, &ti);
	t_reply(&ti, 0x40000 - 0x40, AVE_OP_OPEN, 10,
		ti.abi->semantic[AVE_OP_OPEN], T_REPLY_OK);
	t_fw_post(ti.ave, AVE_RING_T2H, 0, T_FW_BASE + 0x40000 - 0x40, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ti.ave), 1);
	KUNIT_EXPECT_EQ(test, ti.ave->want[AVE_OP_OPEN], 0);
	/* So is right after it. */
	t_msg_init(test, &ti);
	t_reply(&ti, 0x78000 + AVE_IPC_PROC_BUFS * 0x10000, AVE_OP_OPEN, 10,
		ti.abi->semantic[AVE_OP_OPEN], T_REPLY_OK);
	t_fw_post(ti.ave, AVE_RING_T2H, 0,
		  T_FW_BASE + 0x78000 + AVE_IPC_PROC_BUFS * 0x10000, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ti.ave), 1);
	t_canary_intact(test, ti.ave);
}

/* Replies are matched to what the host waits for, and the slot handed back. */
static void ave_ipc_test_poll_dispatch(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	__le64 *slot;

	t_msg_init(test, &ti);
	ave = ti.ave;
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), 1);
	KUNIT_EXPECT_EQ(test, ave->want[AVE_OP_OPEN], 0);
	KUNIT_EXPECT_EQ(test, ave->reply_err, 0);
	KUNIT_EXPECT_EQ(test, ave->msg_len, 0x40U);
	KUNIT_EXPECT_MEMEQ(test, ave->msg, ave->ipc.cpu + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave->msg[0x40], 0x5a);
	slot = t_slot(ave, &ave->ring[AVE_RING_T2H], 0);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(slot[0]), (T_FW_BASE + T_MSG_OFF) | 1);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), 0);

	/* The same reply again: nobody waits for it. */
	t_fw_post(ave, AVE_RING_T2H, 1, T_FW_BASE + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);

	/* A reply for another client. */
	t_msg_init(test, &ti);
	ave = ti.ave;
	t_reply(&ti, T_MSG_OFF, AVE_OP_OPEN, 11, ti.abi->semantic[AVE_OP_OPEN],
		T_REPLY_OK);
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	KUNIT_EXPECT_EQ(test, ave->want[AVE_OP_OPEN], 1);

	/* A reply of the wrong length or with an unknown id. */
	t_msg_init(test, &ti);
	ave = ti.ave;
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x3c);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	t_msg_init(test, &ti);
	ave = ti.ave;
	put_unaligned_le16(0xffff, ave->ipc.cpu + T_MSG_OFF);
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
	KUNIT_EXPECT_EQ(test, ave->want[AVE_OP_OPEN], 1);

	/* A refusal is answered but recorded. */
	t_msg_init(test, &ti);
	ave = ti.ave;
	t_reply(&ti, T_MSG_OFF, AVE_OP_OPEN, 10, ti.abi->semantic[AVE_OP_OPEN], 1);
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x40);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), 1);
	KUNIT_EXPECT_EQ(test, ave->want[AVE_OP_OPEN], 0);
	KUNIT_EXPECT_EQ(test, ave->reply_err, -EIO);

	/* A frame result on a firmware build that cannot encode. */
	t_msg_init(test, &ti);
	ave = ti.ave;
	ti.layout->encode_ready = false;
	t_reply(&ti, T_MSG_OFF, AVE_OP_PROCESS, 10, 0, T_REPLY_OK);
	t_fw_post(ave, AVE_RING_T2H, 0, T_FW_BASE + T_MSG_OFF, 0x48);
	KUNIT_EXPECT_EQ(test, ave_ipc_poll_once(ave), -EPROTO);
}

/* Log lines: every slot is handed back, wherever the line claims to be. */
static void ave_ipc_test_drain_log(struct kunit *test)
{
	struct ave_ring *r;
	struct ave_dev *ave;
	struct t_ipc ti;

	t_ipc_init(test, &ti, &ave_layout_6070_11_1, T_IPC_BYTES);
	ave = ti.ave;
	memset(ave->ring, 0, sizeof(ave->ring));
	t_ring(ave, AVE_RING_LOG, 0x1000, 4, 0, true);
	r = &ave->ring[AVE_RING_LOG];
	memcpy(ave->ipc.cpu + T_MSG_OFF, "line\n", 5);
	memset(ave->ipc.cpu + T_IPC_BYTES - AVE_LOG_LINE_MAX, 'x',
	       AVE_LOG_LINE_MAX);

	/* Nothing written yet. */
	ave_ipc_drain_log(ave);
	KUNIT_EXPECT_EQ(test, r->consumer, 0);

	t_fw_post(ave, AVE_RING_LOG, 0, T_FW_BASE + T_MSG_OFF, 5);
	/* Running off the end of the window. */
	t_fw_post(ave, AVE_RING_LOG, 1, T_FW_BASE + T_IPC_BYTES - 4, 0x100);
	/* An oversized line at the very end: clamped, so it fits. */
	t_fw_post(ave, AVE_RING_LOG, 2,
		  T_FW_BASE + T_IPC_BYTES - AVE_LOG_LINE_MAX, U32_MAX);
	ave_ipc_drain_log(ave);
	for (unsigned int n = 0; n < 3; n++)
		KUNIT_EXPECT_EQ(test, le64_to_cpu(*t_slot(ave, r, n)),
				(u64)AVE_LOG_SLOT_FREE);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(*t_slot(ave, r, 3)), 1ULL);
	KUNIT_EXPECT_EQ(test, r->consumer, 3);

	/* Below the window, then a NULL line; the index wraps. */
	t_fw_post(ave, AVE_RING_LOG, 3, T_FW_BASE - 0x100, 0x10);
	t_fw_post(ave, AVE_RING_LOG, 0, 0, 0x10);
	ave_ipc_drain_log(ave);
	KUNIT_EXPECT_EQ(test, r->consumer, 1);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(*t_slot(ave, r, 3)),
			(u64)AVE_LOG_SLOT_FREE);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(*t_slot(ave, r, 0)),
			(u64)AVE_LOG_SLOT_FREE);

	/* A full ring is read once round, not forever. */
	for (unsigned int n = 0; n < 4; n++)
		t_fw_post(ave, AVE_RING_LOG, n, T_FW_BASE + T_MSG_OFF, 5);
	ave_ipc_drain_log(ave);
	KUNIT_EXPECT_EQ(test, r->consumer, 1);
	t_canary_intact(test, ave);

	/* No TERMINAL channel: nothing to read. */
	memset(ave->ring, 0, sizeof(ave->ring));
	ave_ipc_drain_log(ave);
}

/* A shared-memory pool of 48 granules, sized down for the test. */
#define T_POOL_OFF 0x20000
#define T_POOL_BYTES 0x30000

static void t_pool_init(struct kunit *test, struct t_ipc *ti)
{
	t_ipc_init(test, ti, &ave_layout_6070_11_1, T_POOL_OFF + T_POOL_BYTES);
	ti->t->smalloc_offset = T_POOL_OFF;
	ti->t->smalloc_bytes = T_POOL_BYTES;
}

/* Every block lies inside the pool, and only blocks lent can be returned. */
static void ave_ipc_test_smalloc_bounds(struct kunit *test)
{
	const u64 start = T_FW_BASE + T_POOL_OFF;
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 addr, a, b;

	t_pool_init(test, &ti);
	ave = ti.ave;
	memset(ave->ipc.cpu + T_POOL_OFF, 0x77, T_POOL_BYTES);
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, 0, &addr), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, T_POOL_BYTES + 1, &addr), -EINVAL);
	/* The whole pool in one block, zeroed, and not a byte more. */
	KUNIT_ASSERT_EQ(test, ave_smalloc_get(ave, T_POOL_BYTES, &addr), 0);
	KUNIT_EXPECT_EQ(test, addr, start);
	KUNIT_EXPECT_NULL(test, memchr_inv(ave->ipc.cpu + T_POOL_OFF, 0, T_POOL_BYTES));
	t_canary_intact(test, ave);
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, 1, &addr), -ENOMEM);

	/* Returns that are not a lent block. */
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start - AVE_SMALLOC_GRANULE), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start + T_POOL_BYTES), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, U64_MAX & ~(u64)(SZ_4K - 1)), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start + AVE_SMALLOC_GRANULE), -ENOENT);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start), 0);
	/* Twice. */
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, start), -ENOENT);

	/* Partial granules round up; the last block ends at the pool's end. */
	KUNIT_ASSERT_EQ(test, ave_smalloc_get(ave, 1, &a), 0);
	KUNIT_ASSERT_EQ(test, ave_smalloc_get(ave, T_POOL_BYTES - AVE_SMALLOC_GRANULE, &b), 0);
	KUNIT_EXPECT_EQ(test, a, start);
	KUNIT_EXPECT_EQ(test, b, start + AVE_SMALLOC_GRANULE);
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, 1, &addr), -ENOMEM);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, a), 0);
	KUNIT_ASSERT_EQ(test, ave_smalloc_get(ave, AVE_SMALLOC_GRANULE, &addr), 0);
	KUNIT_EXPECT_EQ(test, addr, start);
	t_canary_intact(test, ave);
}

/* The host tracks at most AVE_SMALLOC_MAX blocks, even with room left. */
static void ave_ipc_test_smalloc_slots(struct kunit *test)
{
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 addr, first = 0;

	t_pool_init(test, &ti);
	ave = ti.ave;
	KUNIT_ASSERT_GT(test, T_POOL_BYTES / AVE_SMALLOC_GRANULE, AVE_SMALLOC_MAX);
	for (unsigned int i = 0; i < AVE_SMALLOC_MAX; i++) {
		KUNIT_ASSERT_EQ(test, ave_smalloc_get(ave, 1, &addr), 0);
		KUNIT_EXPECT_EQ(test, addr, T_FW_BASE + T_POOL_OFF + i * AVE_SMALLOC_GRANULE);
		if (!i)
			first = addr;
	}
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, 1, &addr), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ave_smalloc_put(ave, first), 0);
	KUNIT_EXPECT_EQ(test, ave_smalloc_get(ave, 1, &addr), 0);
	KUNIT_EXPECT_EQ(test, addr, first);
}

/* The SHAREDMALLOC ring: unknown returns are answered, bad sizes stop serving. */
static void ave_ipc_test_serve_smalloc(struct kunit *test)
{
	struct ave_ring *r;
	struct ave_dev *ave;
	struct t_ipc ti;
	__le64 *slot;

	t_pool_init(test, &ti);
	ave = ti.ave;
	memset(ave->ring, 0, sizeof(ave->ring));
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), 0);
	t_ring(ave, AVE_RING_SMALLOC, 0x800, 4, 3, true);
	r = &ave->ring[AVE_RING_SMALLOC];

	/* A block the host never lent: answered with no size. */
	t_fw_post(ave, AVE_RING_SMALLOC, 0, T_FW_BASE + T_POOL_OFF + SZ_64K, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), 1);
	slot = t_slot(ave, r, 0);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(slot[0]), (T_FW_BASE + T_POOL_OFF + SZ_64K) | 1);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(slot[1]), 0ULL);
	/* An address outside the window is answered the same way. */
	t_fw_post(ave, AVE_RING_SMALLOC, 1, 0x1000, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), 1);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, r, 1)[1]), 0ULL);
	/* A request with no size, then one larger than the pool. */
	t_fw_post(ave, AVE_RING_SMALLOC, 2, 0, 0);
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), -EINVAL);
	t_ring(ave, AVE_RING_SMALLOC, 0x800, 4, 3, true);
	t_fw_post(ave, AVE_RING_SMALLOC, 0, 0, T_POOL_BYTES + 1);
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), -EINVAL);
	t_ring_indices_valid(test, r);
	t_canary_intact(test, ave);
}

/*
 * Request sizes near 4 GiB: rounding them up to granules must not wrap to
 * zero granules, which would lend nothing while telling the firmware it
 * has the whole size, at an address the next request gets again.
 */
static void ave_ipc_test_smalloc_huge(struct kunit *test)
{
	static const u32 huge[] = {
		U32_MAX - AVE_SMALLOC_GRANULE + 1,
		U32_MAX - AVE_SMALLOC_GRANULE + 2,
		U32_MAX,
	};
	struct ave_dev *ave;
	struct t_ipc ti;
	u64 addr;

	t_pool_init(test, &ti);
	ave = ti.ave;
	for (unsigned int i = 0; i < ARRAY_SIZE(huge); i++)
		KUNIT_EXPECT_EQ_MSG(test, ave_smalloc_get(ave, huge[i], &addr), -EINVAL,
				    "request of %#x bytes", huge[i]);

	/* Through the ring: the request must not be answered with a block. */
	memset(ave->ring, 0, sizeof(ave->ring));
	t_ring(ave, AVE_RING_SMALLOC, 0x800, 4, 3, true);
	t_fw_post(ave, AVE_RING_SMALLOC, 0, 0, U32_MAX);
	KUNIT_EXPECT_EQ(test, ave_ipc_serve_smalloc(ave), -EINVAL);
	KUNIT_EXPECT_EQ(test, le64_to_cpu(t_slot(ave, &ave->ring[AVE_RING_SMALLOC], 0)[0]),
			0ULL);
}

static struct kunit_case ave_ipc_test_cases[] = {
	KUNIT_CASE(ave_ipc_test_hello_9003),
	KUNIT_CASE(ave_ipc_test_negotiate_bad_hello),
	KUNIT_CASE(ave_ipc_test_negotiate_window),
	KUNIT_CASE(ave_ipc_test_channels_9003),
	KUNIT_CASE(ave_ipc_test_channels_malformed),
	KUNIT_CASE(ave_ipc_test_channels_out_of_range),
	KUNIT_CASE(ave_ipc_test_channels_overlap),
	KUNIT_CASE(ave_ipc_test_channels_6070_unknown),
	KUNIT_CASE(ave_ipc_test_ring_h2f),
	KUNIT_CASE(ave_ipc_test_ring_f2h),
	KUNIT_CASE(ave_ipc_test_buf_of),
	KUNIT_CASE(ave_ipc_test_cmd_bounds),
	KUNIT_CASE(ave_ipc_test_poll_acks),
	KUNIT_CASE(ave_ipc_test_poll_msg_bounds),
	KUNIT_CASE(ave_ipc_test_poll_dispatch),
	KUNIT_CASE(ave_ipc_test_drain_log),
	KUNIT_CASE(ave_ipc_test_smalloc_bounds),
	KUNIT_CASE(ave_ipc_test_smalloc_slots),
	KUNIT_CASE(ave_ipc_test_serve_smalloc),
	KUNIT_CASE(ave_ipc_test_smalloc_huge),
	{}
};

static struct kunit_suite ave_ipc_test_suite = {
	.name = "apple-ave-ipc",
	.test_cases = ave_ipc_test_cases,
};

kunit_test_suite(ave_ipc_test_suite);
