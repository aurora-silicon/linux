// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dpavservep.c: the DCPAV I2C (DDC) message layout and how the
 * DDC adapter maps I2C transfers onto it.
 */
#include <kunit/test.h>
#include <linux/i2c.h>

#include "../dcp-ddc.h"

/* A DDC/CI "get VCP 0x10" request: 51 82 01 10 ac, as ddcutil writes it. */
static void dpavddc_write_layout(struct kunit *test)
{
	static const u8 data[] = { 0x82, 0x01, 0x10, 0xac };
	u8 *msg = kunit_kzalloc(test, DCPAV_I2C_MSG_MAX, GFP_KERNEL);
	u8 expect[0x84] = {};
	int size;

	KUNIT_ASSERT_NOT_NULL(test, msg);
	memset(msg, 0xa5, DCPAV_I2C_MSG_MAX);
	size = dcpav_i2c_message(msg, false, 0x37, 0x51, data, sizeof(data));
	KUNIT_ASSERT_EQ(test, size, 0x84);
	expect[0x02] = 1;
	expect[0x04] = 0x0a;
	expect[0x08] = 0x44;
	memcpy(expect + 0x0c, "xcpi", 4);
	expect[0x50] = 0x37;
	expect[0x60] = 0x51;
	expect[0x70] = 4;
	memcpy(expect + 0x80, data, sizeof(data));
	KUNIT_EXPECT_MEMEQ(test, msg, expect, sizeof(expect));
}

/* The 12-byte DDC/CI reply read: chip, offset and length slots, no data. */
static void dpavddc_read_layout(struct kunit *test)
{
	u8 *msg = kunit_kzalloc(test, DCPAV_I2C_MSG_MAX, GFP_KERNEL);
	u8 buf[12];
	u8 expect[0x8c] = {};

	KUNIT_ASSERT_NOT_NULL(test, msg);
	memset(msg, 0xa5, DCPAV_I2C_MSG_MAX);
	KUNIT_ASSERT_EQ(test, dcpav_i2c_message(msg, true, 0x37, 0x51, buf, sizeof(buf)),
			0x8c);
	expect[0x02] = 1;
	expect[0x04] = 0x09;
	expect[0x08] = 0x4c;
	memcpy(expect + 0x0c, "xcpi", 4);
	expect[0x40] = 0x37;
	expect[0x50] = 0x51;
	expect[0x60] = 12;
	KUNIT_EXPECT_MEMEQ(test, msg, expect, sizeof(expect));

	/* Bounds: a read needs a length, nothing above 128 bytes, 7-bit chips. */
	KUNIT_EXPECT_EQ(test, dcpav_i2c_message(msg, true, 0x37, 0x51, buf, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_message(msg, false, 0x37, 0x51, buf, 129), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_message(msg, false, 0x80, 0x51, buf, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_message(msg, false, 0x50, 0x00, NULL, 0), 0x80);
}

static void dpavddc_result(struct kunit *test)
{
	static const u8 reply_data[12] = {
		0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x5a, 0x9a, 0x00,
	};
	u8 *reply = kunit_kzalloc(test, DCPAV_I2C_MSG_MAX, GFP_KERNEL);
	u8 out[12] = {};
	int size;

	KUNIT_ASSERT_NOT_NULL(test, reply);
	size = dcpav_i2c_message(reply, true, 0x37, 0x51, out, sizeof(out));
	memcpy(reply + DCPAV_I2C_DATA, reply_data, sizeof(reply_data));
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size, true, out, sizeof(out)), 0);
	KUNIT_EXPECT_MEMEQ(test, out, reply_data, sizeof(out));

	/* Transport errors pass through; a short or foreign reply is -EPROTO. */
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(-ETIMEDOUT, 0, reply, size, true, out, 12),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size - 1, true, out, 12), -EPROTO);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size, false, out, 0), -EPROTO);

	/* The firmware's status: the sink did not answer (0xe0114000). */
	put_unaligned_le32(0xe0114000, reply + DCPAV_I2C_R_STATUS);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size, true, out, 12), -EREMOTEIO);
	put_unaligned_le32(0, reply + DCPAV_I2C_R_STATUS);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 1, reply, size, true, out, 12), -EIO);

	/* A write's status sits in the first argument slot. */
	size = dcpav_i2c_message(reply, false, 0x37, 0x51, reply_data, 4);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size, false, NULL, 0), 0);
	put_unaligned_le32(0xe0114000, reply + DCPAV_I2C_W_STATUS);
	KUNIT_EXPECT_EQ(test, dcpav_i2c_result(0, 0, reply, size, false, NULL, 0),
			-EREMOTEIO);
}

static void dpavddc_plan(struct kunit *test)
{
	u8 wbuf[5] = { 0x51, 0x82, 0x01, 0x10, 0xac }, off = 0x80, rbuf[128];
	struct i2c_msg ddcci_w = { .addr = 0x37, .len = 5, .buf = wbuf };
	struct i2c_msg ddcci_r = { .addr = 0x37, .flags = I2C_M_RD, .len = 12, .buf = rbuf };
	struct i2c_msg edid[2] = {
		{ .addr = 0x50, .len = 1, .buf = &off },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 128, .buf = rbuf },
	};
	struct apple_ddc_xfer x;

	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&ddcci_w, 1, 0, &x), 0);
	KUNIT_EXPECT_FALSE(test, x.read);
	KUNIT_EXPECT_EQ(test, x.chip, 0x37);
	KUNIT_EXPECT_EQ(test, x.offset, 0x51);
	KUNIT_EXPECT_PTR_EQ(test, x.buf, &wbuf[1]);
	KUNIT_EXPECT_EQ(test, x.len, 4);

	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&ddcci_r, 1, 0, &x), 0);
	KUNIT_EXPECT_TRUE(test, x.read);
	KUNIT_EXPECT_EQ(test, x.offset, 0x51);
	KUNIT_EXPECT_EQ(test, x.len, 12);

	KUNIT_ASSERT_EQ(test, apple_ddc_plan(edid, 2, 0, &x), 0);
	KUNIT_EXPECT_TRUE(test, x.read);
	KUNIT_EXPECT_EQ(test, x.chip, 0x50);
	KUNIT_EXPECT_EQ(test, x.offset, 0x80);
	KUNIT_EXPECT_EQ(test, x.len, 128);

	/* What the service cannot carry. */
	edid[0].addr = 0x30;		/* E-DDC segment pointer */
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(edid, 2, 0, &x), -EOPNOTSUPP);
	edid[0].addr = 0x50;
	edid[1].addr = 0x37;		/* repeated start to another chip */
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(edid, 2, 0, &x), -EOPNOTSUPP);
	edid[1].addr = 0x50;
	edid[1].len = 129;
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(edid, 2, 0, &x), -EOPNOTSUPP);
	/* i2c-dev's separate EDID offset write, then a bare read at the counter. */
	edid[0].addr = 0x50;
	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&edid[0], 1, 0, &x), 0);
	KUNIT_EXPECT_TRUE(test, x.none);
	KUNIT_EXPECT_EQ(test, x.offset, 0x80);
	edid[1].len = 128;
	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&edid[1], 1, 0x80, &x), 0);
	KUNIT_EXPECT_FALSE(test, x.none);
	KUNIT_EXPECT_TRUE(test, x.read);
	KUNIT_EXPECT_EQ(test, x.chip, 0x50);
	KUNIT_EXPECT_EQ(test, x.offset, 0x80);
	KUNIT_EXPECT_EQ(test, x.len, 128);
	/* A DDC/CI write is sent, never taken as an offset. */
	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&ddcci_w, 1, 0x80, &x), 0);
	KUNIT_EXPECT_FALSE(test, x.none);
	/* A bare DDC/CI reply read keeps the host-address offset. */
	KUNIT_ASSERT_EQ(test, apple_ddc_plan(&ddcci_r, 1, 0x80, &x), 0);
	KUNIT_EXPECT_EQ(test, x.offset, 0x51);
	ddcci_r.addr = 0x30;		/* E-DDC segment pointer */
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(&ddcci_r, 1, 0, &x), -EOPNOTSUPP);
	ddcci_w.len = 0;		/* address-only probe */
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(&ddcci_w, 1, 0, &x), -EOPNOTSUPP);
	ddcci_w.len = 5;
	ddcci_w.flags = I2C_M_TEN;
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(&ddcci_w, 1, 0, &x), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_ddc_plan(&ddcci_w, 3, 0, &x), -EOPNOTSUPP);
}

static struct kunit_case dpavddc_test_cases[] = {
	KUNIT_CASE(dpavddc_write_layout),
	KUNIT_CASE(dpavddc_read_layout),
	KUNIT_CASE(dpavddc_result),
	KUNIT_CASE(dpavddc_plan),
	{}
};

static struct kunit_suite dpavddc_suite = {
	.name = "apple-dcpav-ddc",
	.test_cases = dpavddc_test_cases,
};

kunit_test_suite(dpavddc_suite);
