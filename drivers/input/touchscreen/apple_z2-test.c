// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Apple Z2 touchscreen driver
 *
 * This file is included at the end of apple_z2.c so that it can call the
 * driver's static functions. The device is replaced by a fake SPI controller
 * registered on a KUnit device: its transfer_one() plays the touch controller
 * (interrupt data command, packet read, firmware blobs). Touch reports end up
 * in a real registered input device and an input handler captures the events.
 * Firmware images are synthetic and handed to the upload as struct firmware,
 * so no file system is needed.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/property.h>
#include <linux/sizes.h>

/* Size of the receive buffer that apple_z2_probe() allocates */
#define Z2_TEST_RX_SIZE		APPLE_Z2_RX_BUF_SIZE
/*
 * The test's receive buffer is much larger than the driver's, so that a read
 * with a hostile length stays inside the allocation and can be checked.
 */
#define Z2_TEST_RX_ALLOC	(SZ_64K + SZ_4K)
#define Z2_TEST_MSG_ALLOC	SZ_8K
/* Bytes in front of the touch report in an interrupt data packet */
#define Z2_TEST_PKT_HDR		5
#define Z2_TEST_FINGER_LEN	30
#define Z2_TEST_MAX_EVENTS	4096
#define Z2_TEST_MAX_XFERS	32
#define Z2_TEST_TX_LOG		64
#define Z2_TEST_STATE_LIFTED	7

struct z2_test_event {
	u16 type;
	u16 code;
	s32 value;
};

struct z2_test_xfer {
	unsigned int len;
	u8 bpw;
	bool has_tx;
	bool has_rx;
	u8 tx[Z2_TEST_TX_LOG];
};

struct z2_test_cfg {
	u32 size_x;
	u32 size_y;
	bool invert_x;
	bool invert_y;
	bool swap_x_y;
	const u8 *cal;
	size_t cal_len;
};

struct z2_test_finger {
	u8 key;
	u8 state;
	u16 x;
	u16 y;
	u16 tool_major;
	u16 tool_minor;
	u16 orientation;
	u16 touch_major;
	u16 touch_minor;
};

struct z2_test_ctx {
	struct apple_z2 z2;
	struct input_handler handler;
	struct input_handle handle;
	struct software_node swnode;
	struct property_entry props[8];

	/* Touch report under construction */
	u8 *msg;

	/* Captured input events */
	struct z2_test_event *events;
	unsigned int nevents;

	/* Fake device: log of the transfers it has seen */
	struct z2_test_xfer xfers[Z2_TEST_MAX_XFERS];
	unsigned int nxfers;
	size_t max_rx_len;
	/* Fail the transfer with this index, -1 for none */
	int fail_xfer;
	/* Fake device: pending interrupt data packet */
	bool pkt_ready;
	u16 pkt_wire_len;
	u8 *pkt;
	size_t pkt_len;
};

static const struct z2_test_cfg z2_test_cfg_plain = {
	.size_x = 2000,
	.size_y = 600,
};

/* The fake touch controller */
static int z2_test_transfer_one(struct spi_controller *ctlr,
				struct spi_device *spi,
				struct spi_transfer *xfer)
{
	struct z2_test_ctx *ctx = spi_controller_get_devdata(ctlr);
	unsigned int idx = ctx->nxfers++;
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;

	if (idx < Z2_TEST_MAX_XFERS) {
		struct z2_test_xfer *rec = &ctx->xfers[idx];

		rec->len = xfer->len;
		rec->bpw = xfer->bits_per_word;
		rec->has_tx = tx;
		rec->has_rx = rx;
		if (tx)
			memcpy(rec->tx, tx, min(xfer->len, Z2_TEST_TX_LOG));
	}

	if (ctx->fail_xfer >= 0 && idx == ctx->fail_xfer)
		return -EIO;

	if (tx && rx) {
		/* Interrupt data command: answer with the pending length */
		memset(rx, 0, xfer->len);
		if (tx[0] == APPLE_Z2_CMD_READ_INTERRUPT_DATA &&
		    ctx->pkt_ready && xfer->len >= 3) {
			rx[0] = APPLE_Z2_REPLY_INTERRUPT_DATA;
			put_unaligned_le16(ctx->pkt_wire_len, rx + 1);
		}
	} else if (rx) {
		/* Packet read: only the packet's own bytes are written */
		ctx->max_rx_len = max(ctx->max_rx_len, xfer->len);
		memcpy(rx, ctx->pkt,
		       min3((size_t)xfer->len, ctx->pkt_len,
			    (size_t)Z2_TEST_RX_ALLOC));
		ctx->pkt_ready = false;
	} else if (tx && xfer->len == 2) {
		/* Firmware blob acknowledge: the device raises its interrupt */
		complete(&ctx->z2.boot_irq);
	}

	return 0;
}

static void z2_test_event(struct input_handle *handle, unsigned int type,
			  unsigned int code, int value)
{
	struct z2_test_ctx *ctx = handle->handler->private;

	if (ctx->nevents < Z2_TEST_MAX_EVENTS) {
		ctx->events[ctx->nevents].type = type;
		ctx->events[ctx->nevents].code = code;
		ctx->events[ctx->nevents].value = value;
	}
	ctx->nevents++;
}

static int z2_test_connect(struct input_handler *handler,
			   struct input_dev *dev,
			   const struct input_device_id *id)
{
	struct z2_test_ctx *ctx = handler->private;
	int error;

	if (dev != ctx->z2.input_dev)
		return -ENODEV;

	ctx->handle.dev = dev;
	ctx->handle.handler = handler;
	ctx->handle.name = "apple-z2-kunit";

	error = input_register_handle(&ctx->handle);
	if (error)
		return error;

	error = input_open_device(&ctx->handle);
	if (error)
		input_unregister_handle(&ctx->handle);

	return error;
}

static void z2_test_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
}

static const struct input_device_id z2_test_ids[] = {
	{
		/* connect() picks the test's own device */
		.flags = INPUT_DEVICE_ID_MATCH_BUS,
		.bustype = BUS_SPI,
	},
	{ }
};

KUNIT_DEFINE_ACTION_WRAPPER(z2_test_unregister_input, input_unregister_device,
			    struct input_dev *);
KUNIT_DEFINE_ACTION_WRAPPER(z2_test_unregister_handler,
			    input_unregister_handler, struct input_handler *);

/*
 * Build what apple_z2_probe() builds, minus the hardware: an SPI device on a
 * fake controller, the driver state and the registered input device.
 */
static struct z2_test_ctx *z2_test_setup(struct kunit *test,
					 const struct z2_test_cfg *cfg)
{
	struct spi_board_info info = { .modalias = "apple-z2-kunit" };
	struct z2_test_ctx *ctx;
	struct spi_controller *ctlr;
	struct spi_device *spi;
	struct input_dev *input;
	struct device *parent;
	unsigned int nprops = 0;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->fail_xfer = -1;
	ctx->events = kunit_kcalloc(test, Z2_TEST_MAX_EVENTS,
				    sizeof(*ctx->events), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->events);
	ctx->msg = kunit_kzalloc(test, Z2_TEST_MSG_ALLOC, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->msg);
	ctx->pkt = kunit_kzalloc(test, Z2_TEST_RX_ALLOC, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->pkt);
	ctx->z2.tx_buf = kunit_kzalloc(test,
				       sizeof(struct apple_z2_read_interrupt_cmd),
				       GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->z2.tx_buf);
	ctx->z2.rx_buf = kunit_kzalloc(test, Z2_TEST_RX_ALLOC, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->z2.rx_buf);
	init_completion(&ctx->z2.boot_irq);

	ctx->props[nprops++] = PROPERTY_ENTRY_U32("touchscreen-size-x",
						  cfg->size_x);
	ctx->props[nprops++] = PROPERTY_ENTRY_U32("touchscreen-size-y",
						  cfg->size_y);
	if (cfg->invert_x)
		ctx->props[nprops++] =
			PROPERTY_ENTRY_BOOL("touchscreen-inverted-x");
	if (cfg->invert_y)
		ctx->props[nprops++] =
			PROPERTY_ENTRY_BOOL("touchscreen-inverted-y");
	if (cfg->swap_x_y)
		ctx->props[nprops++] =
			PROPERTY_ENTRY_BOOL("touchscreen-swapped-x-y");
	if (cfg->cal)
		ctx->props[nprops++] =
			PROPERTY_ENTRY_U8_ARRAY_LEN(CAL_PROP_NAME, cfg->cal,
						    cfg->cal_len);
	ctx->swnode.properties = ctx->props;

	parent = kunit_device_register(test, "apple-z2-kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);

	ctlr = devm_spi_alloc_host(parent, 0);
	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	spi_controller_set_devdata(ctlr, ctx);
	ctlr->num_chipselect = 1;
	ctlr->transfer_one = z2_test_transfer_one;
	KUNIT_ASSERT_EQ(test, devm_spi_register_controller(parent, ctlr), 0);

	info.max_speed_hz = 1000000;
	info.swnode = &ctx->swnode;
	spi = spi_new_device(ctlr, &info);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	ctx->z2.spidev = spi;

	input = input_allocate_device();
	KUNIT_ASSERT_NOT_NULL(test, input);
	ctx->z2.input_dev = input;
	input->name = "Apple Z2 KUnit";
	input->phys = "apple_z2";
	input->id.bustype = BUS_SPI;
	input->dev.parent = &spi->dev;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	touchscreen_parse_properties(input, true, &ctx->z2.props);
	input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0, 65535, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0, 65535, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 65535, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, 65535, 0, 0);
	input_set_abs_params(input, ABS_MT_ORIENTATION, -32768, 32767, 0, 0);
	if (input_mt_init_slots(input, 256, INPUT_MT_DIRECT) ||
	    input_register_device(input)) {
		input_free_device(input);
		KUNIT_FAIL_AND_ABORT(test, "unable to set up the input device");
	}
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  z2_test_unregister_input,
						  input), 0);

	ctx->handler.private = ctx;
	ctx->handler.event = z2_test_event;
	ctx->handler.connect = z2_test_connect;
	ctx->handler.disconnect = z2_test_disconnect;
	ctx->handler.name = "apple-z2-kunit";
	ctx->handler.id_table = z2_test_ids;
	KUNIT_ASSERT_EQ(test, input_register_handler(&ctx->handler), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  z2_test_unregister_handler,
						  &ctx->handler), 0);
	KUNIT_ASSERT_PTR_EQ(test, ctx->handle.dev, input);

	return ctx;
}

static void z2_test_clear_log(struct z2_test_ctx *ctx)
{
	ctx->nevents = 0;
	ctx->nxfers = 0;
	ctx->max_rx_len = 0;
}

static unsigned int z2_test_count_events(struct z2_test_ctx *ctx, u16 type,
					 u16 code)
{
	unsigned int i, n = 0;

	for (i = 0; i < min(ctx->nevents, Z2_TEST_MAX_EVENTS); i++)
		if (ctx->events[i].type == type && ctx->events[i].code == code)
			n++;

	return n;
}

static bool z2_test_has_event(struct z2_test_ctx *ctx, u16 type, u16 code,
			      s32 value)
{
	unsigned int i;

	for (i = 0; i < min(ctx->nevents, Z2_TEST_MAX_EVENTS); i++)
		if (ctx->events[i].type == type &&
		    ctx->events[i].code == code &&
		    ctx->events[i].value == value)
			return true;

	return false;
}

/* The slot that tracks the finger with this key, or -1 if it is not down */
static int z2_test_slot_of(struct z2_test_ctx *ctx, int key)
{
	struct input_mt *mt = ctx->z2.input_dev->mt;
	int i;

	for (i = 0; i < mt->num_slots; i++)
		if (input_mt_is_active(&mt->slots[i]) &&
		    mt->slots[i].key == key)
			return i;

	return -1;
}

static unsigned int z2_test_active_slots(struct z2_test_ctx *ctx)
{
	struct input_mt *mt = ctx->z2.input_dev->mt;
	unsigned int i, n = 0;

	for (i = 0; i < mt->num_slots; i++)
		if (input_mt_is_active(&mt->slots[i]))
			n++;

	return n;
}

static int z2_test_slot_abs(struct z2_test_ctx *ctx, int slot,
			    unsigned int code)
{
	return input_mt_get_value(&ctx->z2.input_dev->mt->slots[slot], code);
}

/* Length of a touch report that holds exactly @nfingers finger records */
static size_t z2_test_msg_len(unsigned int nfingers)
{
	return APPLE_Z2_FINGERS_OFFSET + nfingers * Z2_TEST_FINGER_LEN;
}

static void z2_test_put_finger(struct z2_test_ctx *ctx, unsigned int idx,
			       const struct z2_test_finger *f)
{
	u8 *rec = ctx->msg + z2_test_msg_len(idx);

	memset(rec, 0, Z2_TEST_FINGER_LEN);
	rec[0] = f->key;
	rec[1] = f->state;
	put_unaligned_le16(f->x, rec + 4);
	put_unaligned_le16(f->y, rec + 6);
	put_unaligned_le16(f->tool_major, rec + 12);
	put_unaligned_le16(f->tool_minor, rec + 14);
	put_unaligned_le16(f->orientation, rec + 16);
	put_unaligned_le16(f->touch_major, rec + 18);
	put_unaligned_le16(f->touch_minor, rec + 20);
}

static void z2_test_put_touch(struct z2_test_ctx *ctx, unsigned int idx,
			      u8 key, u8 state, u16 x, u16 y)
{
	struct z2_test_finger f = {
		.key = key, .state = state, .x = x, .y = y,
	};

	z2_test_put_finger(ctx, idx, &f);
}

static void z2_test_parse(struct z2_test_ctx *ctx, unsigned int nfingers,
			  size_t msg_len)
{
	ctx->msg[APPLE_Z2_NUM_FINGERS_OFFSET] = nfingers;
	apple_z2_parse_touches(&ctx->z2, ctx->msg, msg_len);
}

/*
 * Make ctx->msg the fake device's next interrupt data packet. @wire_len is
 * the length field of the device's reply; the device then sends that length
 * plus eight, rounded down to a multiple of four.
 */
static void z2_test_queue_packet(struct z2_test_ctx *ctx,
				 unsigned int nfingers, u16 wire_len)
{
	ctx->msg[APPLE_Z2_NUM_FINGERS_OFFSET] = nfingers;
	memset(ctx->pkt, 0, Z2_TEST_RX_ALLOC);
	memcpy(ctx->pkt + Z2_TEST_PKT_HDR, ctx->msg, Z2_TEST_MSG_ALLOC);
	ctx->pkt_wire_len = wire_len;
	ctx->pkt_len = (wire_len + 8) & ~3;
	ctx->pkt_ready = true;
}

/* The smallest length field for a packet that holds @nfingers records */
static u16 z2_test_wire_len(unsigned int nfingers)
{
	return round_up(Z2_TEST_PKT_HDR + z2_test_msg_len(nfingers), 4) - 8;
}

/*
 * Touch report parser
 */

/* A finger record written byte by byte, to pin the record layout */
static void z2_parse_raw_layout(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	static const u8 rec[Z2_TEST_FINGER_LEN] = {
		[0] = 9,			/* finger key */
		[1] = APPLE_Z2_TOUCH_STARTED,
		[4] = 0x34, [5] = 0x02,		/* x = 0x234 */
		[6] = 0x21, [7] = 0x01,		/* y = 0x121 */
		[12] = 0x01, [13] = 0x10,	/* tool major = 0x1001 */
		[14] = 0x02, [15] = 0x20,	/* tool minor = 0x2002 */
		[16] = 0x03, [17] = 0x30,	/* orientation = 0x3003 */
		[18] = 0x04, [19] = 0x40,	/* touch major = 0x4004 */
		[20] = 0x05, [21] = 0x50,	/* touch minor = 0x5005 */
	};
	int slot;

	KUNIT_ASSERT_EQ(test, sizeof(struct apple_z2_finger),
			Z2_TEST_FINGER_LEN);
	ctx->msg[16] = 1;
	memcpy(ctx->msg + 24, rec, sizeof(rec));
	apple_z2_parse_touches(&ctx->z2, ctx->msg, 24 + sizeof(rec));

	slot = z2_test_slot_of(ctx, 9);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 1);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
			0x234);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
			0x121);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_X, 0x234));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_Y, 0x121));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_WIDTH_MAJOR, 0x1001));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_WIDTH_MINOR, 0x2002));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_ORIENTATION, 0x3003));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_TOUCH_MAJOR, 0x4004));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_TOUCH_MINOR, 0x5005));
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 1);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_KEY, BTN_TOUCH, 1));

	/* The frame is closed */
	KUNIT_ASSERT_GT(test, ctx->nevents, 0);
	KUNIT_EXPECT_EQ(test, ctx->events[ctx->nevents - 1].type, EV_SYN);
	KUNIT_EXPECT_EQ(test, ctx->events[ctx->nevents - 1].code, SYN_REPORT);
}

static void z2_parse_touch_move_release(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	int slot, id;

	z2_test_put_touch(ctx, 0, 4, APPLE_Z2_TOUCH_STARTED, 100, 50);
	z2_test_parse(ctx, 1, z2_test_msg_len(1));
	slot = z2_test_slot_of(ctx, 4);
	KUNIT_ASSERT_GE(test, slot, 0);
	id = z2_test_slot_abs(ctx, slot, ABS_MT_TRACKING_ID);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_TRACKING_ID, id));

	/* A moving finger keeps its slot and its tracking id */
	z2_test_clear_log(ctx);
	z2_test_put_touch(ctx, 0, 4, APPLE_Z2_TOUCH_MOVED, 130, 60);
	z2_test_parse(ctx, 1, z2_test_msg_len(1));
	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 4), slot);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_TRACKING_ID),
			id);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 0);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_X, 130));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_Y, 60));
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_SYN, SYN_REPORT),
			1);

	/* Any other state lifts the finger; its position is not reported */
	z2_test_clear_log(ctx);
	z2_test_put_touch(ctx, 0, 4, Z2_TEST_STATE_LIFTED, 999, 99);
	z2_test_parse(ctx, 1, z2_test_msg_len(1));
	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 4), -1);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 0);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_TRACKING_ID, -1));
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_POSITION_X), 0);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_KEY, BTN_TOUCH, 0));
}

static void z2_parse_two_fingers(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	int slot_a, slot_b;

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 10, 20);
	z2_test_put_touch(ctx, 1, 200, APPLE_Z2_TOUCH_STARTED, 1500, 400);
	z2_test_parse(ctx, 2, z2_test_msg_len(2));

	slot_a = z2_test_slot_of(ctx, 1);
	slot_b = z2_test_slot_of(ctx, 200);
	KUNIT_ASSERT_GE(test, slot_a, 0);
	KUNIT_ASSERT_GE(test, slot_b, 0);
	KUNIT_EXPECT_NE(test, slot_a, slot_b);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 2);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot_a, ABS_MT_POSITION_X),
			10);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot_a, ABS_MT_POSITION_Y),
			20);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot_b, ABS_MT_POSITION_X),
			1500);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot_b, ABS_MT_POSITION_Y),
			400);
	KUNIT_EXPECT_NE(test,
			z2_test_slot_abs(ctx, slot_a, ABS_MT_TRACKING_ID),
			z2_test_slot_abs(ctx, slot_b, ABS_MT_TRACKING_ID));
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 2);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_SYN, SYN_REPORT),
			1);

	/* One finger lifts, the other stays where it is */
	z2_test_put_touch(ctx, 0, 1, Z2_TEST_STATE_LIFTED, 10, 20);
	z2_test_put_touch(ctx, 1, 200, APPLE_Z2_TOUCH_MOVED, 1500, 400);
	z2_test_parse(ctx, 2, z2_test_msg_len(2));
	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 1), -1);
	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 200), slot_b);
}

/* Only "started" and "moved" put a finger down */
static void z2_parse_other_states(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	unsigned int state;

	for (state = 0; state < 256; state++) {
		bool down = state == APPLE_Z2_TOUCH_STARTED ||
			    state == APPLE_Z2_TOUCH_MOVED;

		z2_test_put_touch(ctx, 0, 7, state, 300, 200);
		z2_test_parse(ctx, 1, z2_test_msg_len(1));
		KUNIT_EXPECT_EQ_MSG(test, z2_test_slot_of(ctx, 7) >= 0, down,
				    "state %u", state);
	}
}

/* A report too short to hold the finger count is ignored altogether */
static void z2_parse_short_msg(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	size_t len;

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 10, 20);
	for (len = 0; len <= APPLE_Z2_NUM_FINGERS_OFFSET; len++)
		z2_test_parse(ctx, 1, len);

	KUNIT_EXPECT_EQ(test, ctx->nevents, 0);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 0);
}

static void z2_parse_no_fingers(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	/* A record is present, but the count says there is none */
	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 10, 20);
	z2_test_parse(ctx, 0, z2_test_msg_len(1));

	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 0);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 0);
}

static void z2_parse_inverted(struct kunit *test)
{
	static const struct z2_test_cfg cfg = {
		.size_x = 2000, .size_y = 600,
		.invert_x = true, .invert_y = true,
	};
	struct z2_test_ctx *ctx = z2_test_setup(test, &cfg);
	int slot;

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 100, 50);
	z2_test_parse(ctx, 1, z2_test_msg_len(1));

	slot = z2_test_slot_of(ctx, 1);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
			1999 - 100);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
			599 - 50);
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_X, 1899));
	KUNIT_EXPECT_TRUE(test, z2_test_has_event(ctx, EV_ABS,
						  ABS_MT_POSITION_Y, 549));
}

static void z2_parse_swapped(struct kunit *test)
{
	static const struct z2_test_cfg cfg = {
		.size_x = 2000, .size_y = 600,
		.invert_y = true, .swap_x_y = true,
	};
	struct z2_test_ctx *ctx = z2_test_setup(test, &cfg);
	int slot;

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 100, 50);
	z2_test_parse(ctx, 1, z2_test_msg_len(1));

	/* The y axis is inverted first, then the axes trade places */
	slot = z2_test_slot_of(ctx, 1);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
			599 - 50);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
			100);
}

/* The largest count a report can carry, with all its records present */
static void z2_parse_255_fingers(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	unsigned int i;

	for (i = 0; i < 255; i++)
		z2_test_put_touch(ctx, i, i, APPLE_Z2_TOUCH_STARTED,
				  i + 1, i + 2);
	z2_test_parse(ctx, 255, z2_test_msg_len(255));

	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 255);
	for (i = 0; i < 255; i += 127) {
		int slot = z2_test_slot_of(ctx, i);

		KUNIT_ASSERT_GE(test, slot, 0);
		KUNIT_EXPECT_EQ(test,
				z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
				i + 1);
		KUNIT_EXPECT_EQ(test,
				z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
				i + 2);
	}
}

/*
 * The report says it has 255 fingers but is only long enough for three.
 * What lies behind the report (here: plausible finger records) is not part
 * of it and must not turn into touches.
 */
static void z2_parse_count_beyond_report(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	unsigned int i, beyond = 0;

	for (i = 0; i < 255; i++)
		z2_test_put_touch(ctx, i, i, APPLE_Z2_TOUCH_STARTED,
				  i + 1, i + 2);
	z2_test_parse(ctx, 255, z2_test_msg_len(3));

	for (i = 3; i < 255; i++)
		if (z2_test_slot_of(ctx, i) >= 0)
			beyond++;
	KUNIT_EXPECT_EQ(test, beyond, 0);
	/* ...while the three records the report holds are all reported */
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 3);
}

/* The report ends before the first finger record */
static void z2_parse_count_without_records(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	size_t len;

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 10, 20);
	z2_test_put_touch(ctx, 1, 2, APPLE_Z2_TOUCH_STARTED, 30, 40);
	for (len = APPLE_Z2_NUM_FINGERS_OFFSET + 1;
	     len <= APPLE_Z2_FINGERS_OFFSET; len++)
		z2_test_parse(ctx, 2, len);

	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 0);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 0);
}

/* The report ends in the middle of the second finger record */
static void z2_parse_partial_record(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 10, 20);
	z2_test_put_touch(ctx, 1, 2, APPLE_Z2_TOUCH_STARTED, 30, 40);
	z2_test_parse(ctx, 2, z2_test_msg_len(2) - 1);

	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 2), -1);
	KUNIT_EXPECT_GE(test, z2_test_slot_of(ctx, 1), 0);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 1);
}

/*
 * Interrupt data packets
 */

static void z2_read_command_framing(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	static const u8 counters[] = { 1, 2, 1, 2 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		const struct z2_test_xfer *xfer = &ctx->xfers[0];

		z2_test_clear_log(ctx);
		KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);

		/* No data pending: the command is the only transfer */
		KUNIT_ASSERT_EQ(test, ctx->nxfers, 1);
		KUNIT_EXPECT_EQ(test, xfer->len, 16);
		KUNIT_EXPECT_TRUE(test, xfer->has_tx);
		KUNIT_EXPECT_TRUE(test, xfer->has_rx);
		KUNIT_EXPECT_EQ(test, xfer->tx[0], 0xeb);
		KUNIT_EXPECT_EQ(test, xfer->tx[1], counters[i]);
		KUNIT_EXPECT_TRUE(test, !memchr_inv(xfer->tx + 2, 0, 12));
		/* The checksum is the sum of the bytes in front of it */
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(xfer->tx + 14),
				0xeb + counters[i]);
	}
	KUNIT_EXPECT_EQ(test, ctx->nevents, 0);
}

/* The packet read asks for the reply's length plus 8, rounded down to 4 */
static void z2_read_length_rounding(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	static const struct {
		u16 wire_len;
		unsigned int read_len;
	} cases[] = {
		{ 0, 8 }, { 1, 8 }, { 3, 8 }, { 4, 12 }, { 52, 60 },
		{ 55, 60 }, { 56, 64 }, { 3991, 3996 }, { 3992, 4000 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		z2_test_clear_log(ctx);
		z2_test_queue_packet(ctx, 0, cases[i].wire_len);
		KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);

		KUNIT_ASSERT_EQ(test, ctx->nxfers, 2);
		KUNIT_EXPECT_FALSE(test, ctx->xfers[1].has_tx);
		KUNIT_EXPECT_TRUE(test, ctx->xfers[1].has_rx);
		KUNIT_EXPECT_EQ_MSG(test, ctx->xfers[1].len, cases[i].read_len,
				    "length field %u", cases[i].wire_len);
	}
}

/* The touch report starts five bytes into the packet */
static void z2_read_reports_touch(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	int slot;

	z2_test_put_touch(ctx, 0, 3, APPLE_Z2_TOUCH_STARTED, 700, 300);
	z2_test_put_touch(ctx, 1, 5, APPLE_Z2_TOUCH_STARTED, 800, 310);
	z2_test_queue_packet(ctx, 2, z2_test_wire_len(2));
	KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);

	KUNIT_EXPECT_EQ(test, ctx->nxfers, 2);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 2);
	slot = z2_test_slot_of(ctx, 3);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
			700);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
			300);
	slot = z2_test_slot_of(ctx, 5);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_X),
			800);
	KUNIT_EXPECT_EQ(test, z2_test_slot_abs(ctx, slot, ABS_MT_POSITION_Y),
			310);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_SYN, SYN_REPORT),
			1);
}

static void z2_read_spi_errors(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	z2_test_put_touch(ctx, 0, 3, APPLE_Z2_TOUCH_STARTED, 700, 300);

	/* The command fails: no packet read follows */
	z2_test_queue_packet(ctx, 1, z2_test_wire_len(1));
	ctx->fail_xfer = 0;
	KUNIT_EXPECT_EQ(test, apple_z2_read_packet(&ctx->z2), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 1);

	/* The packet read fails: whatever is in the buffer is not parsed */
	z2_test_clear_log(ctx);
	memcpy(ctx->z2.rx_buf, ctx->pkt, 64);
	ctx->fail_xfer = 1;
	KUNIT_EXPECT_EQ(test, apple_z2_read_packet(&ctx->z2), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 2);
	KUNIT_EXPECT_EQ(test, ctx->nevents, 0);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), 0);
}

/* The largest packet that fits the driver's receive buffer */
static void z2_read_largest_packet(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	unsigned int nfingers = (Z2_TEST_RX_SIZE - Z2_TEST_PKT_HDR -
				 APPLE_Z2_FINGERS_OFFSET) / Z2_TEST_FINGER_LEN;
	unsigned int i;

	for (i = 0; i < nfingers; i++)
		z2_test_put_touch(ctx, i, i, APPLE_Z2_TOUCH_STARTED,
				  i + 1, i + 2);
	z2_test_queue_packet(ctx, nfingers, Z2_TEST_RX_SIZE - 8);
	KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);

	KUNIT_EXPECT_EQ(test, ctx->max_rx_len, Z2_TEST_RX_SIZE);
	KUNIT_EXPECT_EQ(test, z2_test_active_slots(ctx), nfingers);
}

/*
 * The device's reply carries a 16-bit length. The driver must not read more
 * than its receive buffer holds, whatever the device says.
 */
static void z2_read_oversize_length(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	static const u16 wire_lens[] = {
		Z2_TEST_RX_SIZE - 7, Z2_TEST_RX_SIZE - 4, Z2_TEST_RX_SIZE,
		8192, 0xfff7, 0xffff,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wire_lens); i++) {
		/* The length the driver reads, as apple_z2_read_packet() rounds it */
		size_t pkt_len = (wire_lens[i] + 8) & ~3;
		bool fits = pkt_len <= Z2_TEST_RX_SIZE;

		z2_test_clear_log(ctx);
		z2_test_queue_packet(ctx, 0, wire_lens[i]);
		KUNIT_EXPECT_EQ_MSG(test, apple_z2_read_packet(&ctx->z2),
				    fits ? 0 : -EMSGSIZE,
				    "length field %u", wire_lens[i]);
		KUNIT_EXPECT_LE_MSG(test, ctx->max_rx_len,
				    (size_t)Z2_TEST_RX_SIZE,
				    "length field %u", wire_lens[i]);
		/* A dropped packet is not parsed */
		if (!fits)
			KUNIT_EXPECT_EQ_MSG(test, ctx->nevents, 0,
					    "length field %u", wire_lens[i]);
	}
}

/*
 * A packet that is too short for its finger count must not report what an
 * earlier, longer packet left in the receive buffer.
 */
static void z2_read_stale_finger(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	/* Two fingers down */
	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_STARTED, 100, 50);
	z2_test_put_touch(ctx, 1, 2, APPLE_Z2_TOUCH_STARTED, 900, 300);
	z2_test_queue_packet(ctx, 2, z2_test_wire_len(2));
	KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);
	KUNIT_ASSERT_EQ(test, z2_test_active_slots(ctx), 2);

	/* Finger 2 lifts, in a packet with one record */
	z2_test_put_touch(ctx, 0, 2, Z2_TEST_STATE_LIFTED, 900, 300);
	z2_test_queue_packet(ctx, 1, z2_test_wire_len(1));
	KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);
	KUNIT_ASSERT_EQ(test, z2_test_slot_of(ctx, 2), -1);

	/* Finger 1 moves; the packet claims two records but holds one */
	z2_test_clear_log(ctx);
	z2_test_put_touch(ctx, 0, 1, APPLE_Z2_TOUCH_MOVED, 110, 55);
	z2_test_queue_packet(ctx, 2, z2_test_wire_len(1));
	KUNIT_ASSERT_EQ(test, apple_z2_read_packet(&ctx->z2), 0);

	KUNIT_EXPECT_EQ(test, z2_test_slot_of(ctx, 2), -1);
	KUNIT_EXPECT_EQ(test, z2_test_count_events(ctx, EV_ABS,
						   ABS_MT_TRACKING_ID), 0);
}

static void z2_irq_boot_and_data(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	/* Before the firmware runs, an interrupt only signals the boot */
	z2_test_put_touch(ctx, 0, 3, APPLE_Z2_TOUCH_STARTED, 700, 300);
	z2_test_queue_packet(ctx, 1, z2_test_wire_len(1));
	KUNIT_EXPECT_EQ(test, apple_z2_irq(0, &ctx->z2), IRQ_HANDLED);
	KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&ctx->z2.boot_irq));
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 0);
	KUNIT_EXPECT_EQ(test, ctx->nevents, 0);

	/* Afterwards it fetches a packet */
	ctx->z2.booted = true;
	KUNIT_EXPECT_EQ(test, apple_z2_irq(0, &ctx->z2), IRQ_HANDLED);
	KUNIT_EXPECT_FALSE(test, try_wait_for_completion(&ctx->z2.boot_irq));
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 2);
	KUNIT_EXPECT_GE(test, z2_test_slot_of(ctx, 3), 0);
}

/*
 * Firmware upload
 *
 * The images below are synthetic and go straight to
 * apple_z2_load_firmware(). Several entries share one image and differ in
 * the size they declare.
 */

static const __le32 z2_test_fw_good[] = {
	cpu_to_le32(APPLE_Z2_FW_MAGIC), cpu_to_le32(1),
	/* Six bytes for 8-bit transfer, then two bytes of padding */
	cpu_to_le32(LOAD_COMMAND_INIT_PAYLOAD), cpu_to_le32(6),
	cpu_to_le32(0x04030201), cpu_to_le32(0xeeee0605),
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB), cpu_to_le32(8),
	cpu_to_le32(0x14131211), cpu_to_le32(0x18171615),
	cpu_to_le32(LOAD_COMMAND_SEND_CALIBRATION), cpu_to_le32(0x00123456),
	/* Not part of the image except in the "trailing" entry */
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB),
};

static const __le32 z2_test_fw_bad_magic[] = {
	cpu_to_le32(APPLE_Z2_FW_MAGIC + 1), cpu_to_le32(1),
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB), cpu_to_le32(4),
	cpu_to_le32(0x04030201),
};

static const __le32 z2_test_fw_bad_version[] = {
	cpu_to_le32(APPLE_Z2_FW_MAGIC), cpu_to_le32(2),
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB), cpu_to_le32(4),
	cpu_to_le32(0x04030201),
};

static const __le32 z2_test_fw_bad_command[] = {
	cpu_to_le32(APPLE_Z2_FW_MAGIC), cpu_to_le32(1),
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB), cpu_to_le32(4),
	cpu_to_le32(0x04030201),
	cpu_to_le32(3), cpu_to_le32(4),
	cpu_to_le32(0x14131211),
};

static const __le32 z2_test_fw_huge_blob[] = {
	cpu_to_le32(APPLE_Z2_FW_MAGIC), cpu_to_le32(1),
	cpu_to_le32(LOAD_COMMAND_SEND_BLOB), cpu_to_le32(0xfffffffc),
	cpu_to_le32(0x04030201), cpu_to_le32(0x08070605),
};

struct z2_test_image {
	const char *name;
	const void *data;
	size_t size;
};

#define Z2_TEST_FW(_name, _image, _size) \
	{ "apple-z2-kunit/" _name ".bin", (_image), (_size) }

static const struct z2_test_image z2_test_fw_table[] = {
	Z2_TEST_FW("good", z2_test_fw_good, 48),
	Z2_TEST_FW("header-only", z2_test_fw_good, 8),
	Z2_TEST_FW("short-header", z2_test_fw_good, 4),
	Z2_TEST_FW("unpadded-end", z2_test_fw_good, 22),
	Z2_TEST_FW("cut-command", z2_test_fw_good, 28),
	Z2_TEST_FW("cut-blob", z2_test_fw_good, 20),
	Z2_TEST_FW("cut-calibration", z2_test_fw_good, 44),
	Z2_TEST_FW("trailing", z2_test_fw_good, 52),
	Z2_TEST_FW("bad-magic", z2_test_fw_bad_magic,
		   sizeof(z2_test_fw_bad_magic)),
	Z2_TEST_FW("bad-version", z2_test_fw_bad_version,
		   sizeof(z2_test_fw_bad_version)),
	Z2_TEST_FW("bad-command", z2_test_fw_bad_command,
		   sizeof(z2_test_fw_bad_command)),
	Z2_TEST_FW("huge-blob", z2_test_fw_huge_blob,
		   sizeof(z2_test_fw_huge_blob)),
};

static const u8 z2_test_cal[] = {
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0xff,
};

/* The blob the driver builds from z2_test_cal for address 0x00123456 */
static const u8 z2_test_cal_blob[] = {
	0x01, 0x30,			/* blob command */
	0x02, 0x00,			/* length in 32-bit words */
	0x56, 0x34, 0x12, 0x00,		/* address */
	0x9e, 0x00,			/* sum of the six bytes before */
	0x00, 0x00,			/* padding */
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0xff,
	0xbf, 0x02, 0x00, 0x00,		/* sum of the calibration bytes */
};

static const struct z2_test_cfg z2_test_cfg_cal = {
	.size_x = 2000,
	.size_y = 600,
	.cal = z2_test_cal,
	.cal_len = sizeof(z2_test_cal),
};

/* Upload one of the images above, as apple_z2_upload_firmware() would */
static int z2_test_upload(struct z2_test_ctx *ctx, const char *name)
{
	struct firmware fw = {};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(z2_test_fw_table); i++) {
		if (strcmp(z2_test_fw_table[i].name, name))
			continue;
		fw.data = z2_test_fw_table[i].data;
		fw.size = z2_test_fw_table[i].size;
		return apple_z2_load_firmware(&ctx->z2, &fw);
	}

	/* Not one of ours: let the firmware loader fail to find it */
	ctx->z2.fw_name = name;
	return apple_z2_upload_firmware(&ctx->z2);
}

/* A firmware blob transfer and the acknowledge that follows it */
static void z2_test_expect_blob(struct kunit *test, struct z2_test_ctx *ctx,
				unsigned int idx, const void *data,
				unsigned int len, unsigned int bpw)
{
	static const u8 ack[] = { 0x1a, 0xa1 };
	const struct z2_test_xfer *xfer = &ctx->xfers[idx];

	KUNIT_ASSERT_LT(test, idx + 1, min(ctx->nxfers, Z2_TEST_MAX_XFERS));
	KUNIT_ASSERT_LE(test, len, Z2_TEST_TX_LOG);
	KUNIT_EXPECT_EQ(test, xfer->len, len);
	KUNIT_EXPECT_EQ(test, xfer->bpw, bpw);
	KUNIT_EXPECT_TRUE(test, xfer->has_tx);
	KUNIT_EXPECT_FALSE(test, xfer->has_rx);
	KUNIT_EXPECT_MEMEQ(test, xfer->tx, data, len);

	xfer++;
	KUNIT_EXPECT_EQ(test, xfer->len, sizeof(ack));
	KUNIT_EXPECT_EQ(test, xfer->bpw, 8);
	KUNIT_EXPECT_TRUE(test, xfer->has_tx);
	KUNIT_EXPECT_FALSE(test, xfer->has_rx);
	KUNIT_EXPECT_MEMEQ(test, xfer->tx, ack, sizeof(ack));
}

static void z2_fw_upload(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_cal);

	KUNIT_ASSERT_EQ(test, z2_test_upload(ctx, "apple-z2-kunit/good.bin"),
			0);
	KUNIT_EXPECT_TRUE(test, ctx->z2.booted);

	/* Three blobs with their acknowledges, then a first packet poll */
	KUNIT_ASSERT_EQ(test, ctx->nxfers, 7);
	z2_test_expect_blob(test, ctx, 0, &z2_test_fw_good[4], 6, 8);
	z2_test_expect_blob(test, ctx, 2, &z2_test_fw_good[8], 8, 16);
	z2_test_expect_blob(test, ctx, 4, z2_test_cal_blob,
			    sizeof(z2_test_cal_blob), 16);
	KUNIT_EXPECT_EQ(test, ctx->xfers[6].len, 16);
	KUNIT_EXPECT_EQ(test, ctx->xfers[6].tx[0], 0xeb);
}

/* Without a calibration property the calibration command is skipped */
static void z2_fw_no_calibration(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);
	size_t size = 0;

	KUNIT_EXPECT_NULL(test, apple_z2_build_cal_blob(&ctx->z2, 0x123456,
							&size));

	KUNIT_ASSERT_EQ(test, z2_test_upload(ctx, "apple-z2-kunit/good.bin"),
			0);
	KUNIT_EXPECT_TRUE(test, ctx->z2.booted);
	KUNIT_ASSERT_EQ(test, ctx->nxfers, 5);
	z2_test_expect_blob(test, ctx, 0, &z2_test_fw_good[4], 6, 8);
	z2_test_expect_blob(test, ctx, 2, &z2_test_fw_good[8], 8, 16);
	KUNIT_EXPECT_EQ(test, ctx->xfers[4].tx[0], 0xeb);
}

static void z2_fw_cal_blob(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_cal);
	size_t size = 0;
	const u8 *blob;

	blob = apple_z2_build_cal_blob(&ctx->z2, 0x00123456, &size);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, blob);
	KUNIT_EXPECT_EQ(test, size, sizeof(z2_test_cal_blob));
	if (size == sizeof(z2_test_cal_blob))
		KUNIT_EXPECT_MEMEQ(test, blob, z2_test_cal_blob, size);
	kfree(blob);
}

/* Images that end cleanly, with and without commands */
static void z2_fw_clean_end(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	KUNIT_EXPECT_EQ(test,
			z2_test_upload(ctx, "apple-z2-kunit/header-only.bin"),
			0);
	KUNIT_EXPECT_TRUE(test, ctx->z2.booted);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 1);

	/* The last blob need not be padded */
	ctx->z2.booted = false;
	z2_test_clear_log(ctx);
	KUNIT_EXPECT_EQ(test,
			z2_test_upload(ctx, "apple-z2-kunit/unpadded-end.bin"),
			0);
	KUNIT_EXPECT_TRUE(test, ctx->z2.booted);
	KUNIT_ASSERT_EQ(test, ctx->nxfers, 3);
	z2_test_expect_blob(test, ctx, 0, &z2_test_fw_good[4], 6, 8);
}

struct z2_test_fw_case {
	const char *name;
	/* Transfers that happen before the image is found malformed */
	unsigned int nxfers;
};

static void z2_test_fw_case_desc(const struct z2_test_fw_case *c, char *desc)
{
	strscpy(desc, c->name + strlen("apple-z2-kunit/"),
		KUNIT_PARAM_DESC_SIZE);
}

static const struct z2_test_fw_case z2_test_fw_rejected[] = {
	{ "apple-z2-kunit/bad-magic.bin", 0 },
	{ "apple-z2-kunit/bad-version.bin", 0 },
	/* The image ends inside a command header */
	{ "apple-z2-kunit/cut-command.bin", 2 },
	/* The image ends inside a blob */
	{ "apple-z2-kunit/cut-blob.bin", 0 },
	/* The image ends in front of the calibration address */
	{ "apple-z2-kunit/cut-calibration.bin", 4 },
	/* Four stray bytes after the last command */
	{ "apple-z2-kunit/trailing.bin", 4 },
	{ "apple-z2-kunit/bad-command.bin", 2 },
	{ "apple-z2-kunit/huge-blob.bin", 0 },
};

KUNIT_ARRAY_PARAM(z2_fw_rejected, z2_test_fw_rejected, z2_test_fw_case_desc);

static void z2_fw_rejected(struct kunit *test)
{
	const struct z2_test_fw_case *c = test->param_value;
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	KUNIT_EXPECT_EQ(test, z2_test_upload(ctx, c->name), -EINVAL);
	KUNIT_EXPECT_FALSE(test, ctx->z2.booted);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, c->nxfers);
}

/* An image shorter than its own header has no magic and version to accept */
static void z2_fw_short_header(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	KUNIT_EXPECT_EQ(test,
			z2_test_upload(ctx, "apple-z2-kunit/short-header.bin"),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, ctx->z2.booted);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 0);
}

static void z2_fw_missing(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_plain);

	KUNIT_EXPECT_LT(test,
			z2_test_upload(ctx, "apple-z2-kunit/missing.bin"), 0);
	KUNIT_EXPECT_FALSE(test, ctx->z2.booted);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 0);
}

static void z2_fw_spi_error(struct kunit *test)
{
	struct z2_test_ctx *ctx = z2_test_setup(test, &z2_test_cfg_cal);

	/* The second blob fails: the upload stops there */
	ctx->fail_xfer = 2;
	KUNIT_EXPECT_EQ(test, z2_test_upload(ctx, "apple-z2-kunit/good.bin"),
			-EIO);
	KUNIT_EXPECT_FALSE(test, ctx->z2.booted);
	KUNIT_EXPECT_EQ(test, ctx->nxfers, 3);
}

static struct kunit_case apple_z2_test_cases[] = {
	KUNIT_CASE(z2_parse_raw_layout),
	KUNIT_CASE(z2_parse_touch_move_release),
	KUNIT_CASE(z2_parse_two_fingers),
	KUNIT_CASE(z2_parse_other_states),
	KUNIT_CASE(z2_parse_short_msg),
	KUNIT_CASE(z2_parse_no_fingers),
	KUNIT_CASE(z2_parse_inverted),
	KUNIT_CASE(z2_parse_swapped),
	KUNIT_CASE(z2_parse_255_fingers),
	KUNIT_CASE(z2_parse_count_beyond_report),
	KUNIT_CASE(z2_parse_count_without_records),
	KUNIT_CASE(z2_parse_partial_record),
	KUNIT_CASE(z2_read_command_framing),
	KUNIT_CASE(z2_read_length_rounding),
	KUNIT_CASE(z2_read_reports_touch),
	KUNIT_CASE(z2_read_spi_errors),
	KUNIT_CASE(z2_read_largest_packet),
	KUNIT_CASE(z2_read_oversize_length),
	KUNIT_CASE(z2_read_stale_finger),
	KUNIT_CASE(z2_irq_boot_and_data),
	KUNIT_CASE(z2_fw_upload),
	KUNIT_CASE(z2_fw_no_calibration),
	KUNIT_CASE(z2_fw_cal_blob),
	KUNIT_CASE(z2_fw_clean_end),
	KUNIT_CASE_PARAM(z2_fw_rejected, z2_fw_rejected_gen_params),
	KUNIT_CASE(z2_fw_short_header),
	KUNIT_CASE(z2_fw_missing),
	KUNIT_CASE(z2_fw_spi_error),
	{}
};

static struct kunit_suite apple_z2_test_suite = {
	.name = "apple-z2",
	.test_cases = apple_z2_test_cases,
};

kunit_test_suite(apple_z2_test_suite);
