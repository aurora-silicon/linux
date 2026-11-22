// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the DP855 read-only downstream I2C adapter. A fake register
 * file stands in for the SPI register interface, so no hardware is needed.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#define FAKE_MAX_LOG	64

struct dp855_fake_write {
	u16 reg;
	u8 len;
	u8 buf[8];
};

struct dp855_fake {
	u8 ack;
	u8 state;
	u8 status;
	u8 rdata[DP855_MAX_DATA];
	u16 fail_write_reg;
	unsigned int ack_reads;
	unsigned int state_reads;
	unsigned int status_reads;
	unsigned int rdata_reads;
	unsigned int nreads;
	unsigned int nw;
	struct dp855_fake_write w[FAKE_MAX_LOG];
};

static int fake_read(void *ctx, u16 reg, u8 *buf, size_t len)
{
	struct dp855_fake *f = ctx;

	f->nreads++;
	memset(buf, 0, len);
	switch (reg) {
	case DP855_REG_ACCESS_ACK:
		f->ack_reads++;
		buf[0] = f->ack;
		break;
	case DP855_REG_I2C_STATE:
		f->state_reads++;
		buf[0] = f->state;
		break;
	case DP855_REG_I2C_STATUS:
		f->status_reads++;
		buf[0] = f->status;
		break;
	case DP855_REG_I2C_RDATA:
		f->rdata_reads++;
		memcpy(buf, f->rdata, len);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fake_write(void *ctx, u16 reg, const u8 *buf, size_t len)
{
	struct dp855_fake *f = ctx;
	struct dp855_fake_write *w;

	if (f->nw == FAKE_MAX_LOG || len > sizeof(w->buf))
		return -ENOSPC;
	w = &f->w[f->nw++];
	w->reg = reg;
	w->len = len;
	memcpy(w->buf, buf, len);

	return reg == f->fail_write_reg ? -EIO : 0;
}

static const struct dp855_regio fake_regio = {
	.read = fake_read,
	.write = fake_write,
};

static struct dp855 *dp855_test_init(struct kunit *test, struct dp855_fake **fakep)
{
	struct dp855_fake *f;
	struct dp855 *dp;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	dp = kunit_kzalloc(test, sizeof(*dp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dp);

	dp->dev = kunit_device_register(test, "dp855-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dp->dev);
	mutex_init(&dp->lock);
	dp->io = &fake_regio;
	dp->io_ctx = f;
	dp->adap.algo = &dp855_i2c_algo;
	i2c_set_adapdata(&dp->adap, dp);
	dp->allow[dp->nallow++] = 0x70;

	f->ack = DP855_ACCESS_ACK_GRANTED;
	f->status = DP855_I2C_STATUS_DONE;
	*fakep = f;

	return dp;
}

static void expect_write(struct kunit *test, const struct dp855_fake *f,
			 unsigned int i, u16 reg, const u8 *buf, u8 len)
{
	KUNIT_ASSERT_LT(test, i, f->nw);
	KUNIT_EXPECT_EQ(test, f->w[i].reg, reg);
	KUNIT_EXPECT_EQ(test, f->w[i].len, len);
	KUNIT_EXPECT_MEMEQ(test, f->w[i].buf, buf, len);
}

static void expect_released(struct kunit *test, const struct dp855_fake *f)
{
	static const u8 rel = DP855_ACCESS_RELEASE;

	KUNIT_ASSERT_GT(test, f->nw, 0);
	expect_write(test, f, f->nw - 1, DP855_REG_ACCESS_REQ, &rel, 1);
}

static void dp855_test_pack_request(struct kunit *test)
{
	/* read 1 byte from 0x70, no register pointer */
	KUNIT_EXPECT_EQ(test, dp855_pack_request(true, 0x70, 0, 0, 0, 1),
			0x000000e000000001ULL);
	/* read 1 byte from 0x70 at register 0, one-byte pointer */
	KUNIT_EXPECT_EQ(test, dp855_pack_request(true, 0x70, 1, 0, 0, 1),
			0x000000e000000003ULL);
	/* read 16 bytes from 0x70 at register 0x1234, two-byte pointer */
	KUNIT_EXPECT_EQ(test, dp855_pack_request(true, 0x70, 2, 0x1234, 0, 16),
			0x001234e00001e005ULL);
	/* write 16 bytes to 0x7f, no pointer: write length field only */
	KUNIT_EXPECT_EQ(test, dp855_pack_request(false, 0x7f, 0, 0, 16, 0),
			0x000000fe00000078ULL);
	/* pointer value is ignored when there is no pointer */
	KUNIT_EXPECT_EQ(test, dp855_pack_request(true, 0x72, 0, 0xffff, 0, 1),
			0x000000e400000001ULL);
}

static void dp855_test_spi_frames(struct kunit *test)
{
	static const u8 rd[] = { 0x20, 0x3b, 0x9b, 0, 0, 0, 0, 0, 0, 0 };
	static const u8 wr[] = { 0x00, 0x3b, 0x92, 0x0c };
	static const u8 payload = DP855_ACCESS_ACQUIRE;
	u8 tx[DP855_SPI_BUF_LEN];

	memset(tx, 0xa5, sizeof(tx));
	dp855_spi_read_cmd(tx, DP855_REG_ACCESS_ACK, 1);
	KUNIT_EXPECT_MEMEQ(test, tx, rd, sizeof(rd));
	KUNIT_EXPECT_EQ(test, DP855_SPI_READ_DATA_OFF, 9);

	dp855_spi_write_cmd(tx, DP855_REG_ACCESS_REQ, &payload, 1);
	KUNIT_EXPECT_MEMEQ(test, tx, wr, sizeof(wr));
}

static void dp855_test_plain_read(struct kunit *test)
{
	const u8 acquire = DP855_ACCESS_ACQUIRE, one = 1, zero = 0;
	const u8 ff = 0xff, prep = DP855_I2C_CMD_PREPARE;
	const u8 start = DP855_I2C_CMD_START;
	static const u8 ctrl[] = { 0x0e, 0x20, 0x40, 0x00 };
	static const u8 req[] = { 0x01, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00 };
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);
	u8 val = 0;
	struct i2c_msg msg = { .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = &val };

	f->rdata[0] = 0x1d;
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, &msg, 1), 1);
	KUNIT_EXPECT_EQ(test, val, 0x1d);

	KUNIT_ASSERT_EQ(test, f->nw, 10);
	expect_write(test, f, 0, DP855_REG_ACCESS_REQ, &acquire, 1);
	expect_write(test, f, 1, DP855_REG_I2C_ENABLE, &one, 1);
	expect_write(test, f, 2, DP855_REG_I2C_AUX_STATUS, &zero, 1);
	expect_write(test, f, 3, DP855_REG_I2C_STATE, &ff, 1);
	expect_write(test, f, 4, DP855_REG_I2C_STATUS, &ff, 1);
	expect_write(test, f, 5, DP855_REG_I2C_CMD, &prep, 1);
	expect_write(test, f, 6, DP855_REG_I2C_CTRL, ctrl, sizeof(ctrl));
	expect_write(test, f, 7, DP855_REG_I2C_REQUEST, req, sizeof(req));
	expect_write(test, f, 8, DP855_REG_I2C_CMD, &start, 1);
	expect_released(test, f);
	KUNIT_EXPECT_EQ(test, f->rdata_reads, 1);
	KUNIT_EXPECT_EQ(test, dp->stats.xfer_ok, 1);
	KUNIT_EXPECT_EQ(test, dp->stats.lock_ok, 1);
}

static void dp855_test_pointer_read(struct kunit *test)
{
	static const u8 req[] = { 0x03, 0xe0, 0x01, 0x00, 0xe0, 0x05, 0x00, 0x00 };
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);
	u8 ptr = 0x05, val[16] = { };
	struct i2c_msg msgs[] = {
		{ .addr = 0x70, .len = 1, .buf = &ptr },
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 16, .buf = val },
	};
	unsigned int i;

	for (i = 0; i < 16; i++)
		f->rdata[i] = i + 1;
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, msgs, 2), 2);
	KUNIT_EXPECT_MEMEQ(test, val, f->rdata, 16);
	KUNIT_ASSERT_EQ(test, f->nw, 10);
	expect_write(test, f, 7, DP855_REG_I2C_REQUEST, req, sizeof(req));
	expect_released(test, f);
}

static void dp855_test_rejects(struct kunit *test)
{
	u8 b[17] = { }, p2[2] = { };
	struct i2c_msg wr[] = { { .addr = 0x70, .len = 1, .buf = b } };
	struct i2c_msg rd17[] = { { .addr = 0x70, .flags = I2C_M_RD, .len = 17, .buf = b } };
	struct i2c_msg rd0[] = { { .addr = 0x70, .flags = I2C_M_RD, .len = 0, .buf = b } };
	struct i2c_msg ten[] = {
		{ .addr = 0x70, .flags = I2C_M_RD | I2C_M_TEN, .len = 1, .buf = b },
	};
	struct i2c_msg other[] = { { .addr = 0x72, .flags = I2C_M_RD, .len = 1, .buf = b } };
	struct i2c_msg pmic[] = { { .addr = 0x74, .flags = I2C_M_RD, .len = 1, .buf = b } };
	struct i2c_msg wp[] = { { .addr = 0x71, .flags = I2C_M_RD, .len = 1, .buf = b } };
	struct i2c_msg ptr2[] = {
		{ .addr = 0x70, .len = 2, .buf = p2 },
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = b },
	};
	struct i2c_msg split[] = {
		{ .addr = 0x70, .len = 1, .buf = p2 },
		{ .addr = 0x72, .flags = I2C_M_RD, .len = 1, .buf = b },
	};
	struct i2c_msg rdrd[] = {
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = p2 },
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = b },
	};
	struct i2c_msg wrwr[] = {
		{ .addr = 0x70, .len = 1, .buf = p2 },
		{ .addr = 0x70, .len = 1, .buf = b },
	};
	struct i2c_msg three[] = {
		{ .addr = 0x70, .len = 1, .buf = p2 },
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = b },
		{ .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = b },
	};
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	/* Allowlisting a reserved address must not make it reachable. */
	dp->allow[dp->nallow++] = 0x74;
	dp->allow[dp->nallow++] = 0x71;

	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, wr, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, rd17, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, rd0, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, ten, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, other, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, pmic, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, wp, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, ptr2, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, split, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, rdrd, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, wrwr, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dp855_i2c_xfer(&dp->adap, three, 3), -EOPNOTSUPP);

	/* None of them reached the controller. */
	KUNIT_EXPECT_EQ(test, f->nw, 0);
	KUNIT_EXPECT_EQ(test, f->nreads, 0);
	KUNIT_EXPECT_EQ(test, dp->stats.rejected, 12);
}

static int dp855_test_read1(struct dp855 *dp)
{
	u8 val;
	struct i2c_msg msg = { .addr = 0x70, .flags = I2C_M_RD, .len = 1, .buf = &val };

	return dp855_i2c_xfer(&dp->adap, &msg, 1);
}

static void dp855_test_lock_refused(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	f->ack = DP855_ACCESS_ACK_REFUSED;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->ack_reads, 1);
	KUNIT_EXPECT_EQ(test, f->nw, 2);
	expect_released(test, f);
	KUNIT_EXPECT_EQ(test, f->state_reads, 0);
	KUNIT_EXPECT_EQ(test, dp->stats.lock_refused, 1);
	KUNIT_EXPECT_EQ(test, dp->stats.last_ack, DP855_ACCESS_ACK_REFUSED);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_lock_timeout(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	f->ack = DP855_ACCESS_ACK_PENDING;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->ack_reads, 1 + DP855_LOCK_POLL_RETRIES);
	KUNIT_EXPECT_EQ(test, f->nw, 2);
	expect_released(test, f);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_lock_bad_ack(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	/* Only the upper bits are ignored; ack value 2 is not defined. */
	f->ack = 0xfc | 2;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -EIO);
	expect_released(test, f);

	/* Upper bits do not hide a grant. */
	f->ack = 0xfc | DP855_ACCESS_ACK_GRANTED;
	f->nw = 0;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), 1);
	expect_released(test, f);
}

static void dp855_test_busy_timeout(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	f->state = DP855_I2C_STATE_BUSY;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->state_reads, 1 + DP855_LOCK_POLL_RETRIES);
	KUNIT_EXPECT_EQ(test, f->nw, 2);
	expect_released(test, f);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_done_timeout(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	f->status = 0;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->status_reads, 1 + DP855_DONE_POLL_RETRIES);
	KUNIT_EXPECT_EQ(test, f->rdata_reads, 0);
	expect_released(test, f);
	KUNIT_EXPECT_EQ(test, dp->stats.xfer_timeout, 1);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_completion_errors(struct kunit *test)
{
	static const u8 errs[] = { 0x03, 0x05, 0x07 };
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(errs); i++) {
		f->status = errs[i];
		f->nw = 0;
		KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -EREMOTEIO);
		KUNIT_EXPECT_EQ(test, f->rdata_reads, 0);
		expect_released(test, f);
	}
	/* Error bits without completion keep polling until the bound. */
	f->status = 0x06;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_io_errors(struct kunit *test)
{
	static const u16 regs[] = {
		DP855_REG_I2C_ENABLE, DP855_REG_I2C_AUX_STATUS,
		DP855_REG_I2C_STATE, DP855_REG_I2C_CTRL,
		DP855_REG_I2C_REQUEST, DP855_REG_I2C_CMD,
	};
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		f->fail_write_reg = regs[i];
		f->nw = 0;
		KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -EIO);
		expect_released(test, f);
	}
	KUNIT_EXPECT_EQ(test, f->rdata_reads, 0);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&dp->lock));
}

static void dp855_test_suspended(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	dp->suspended = true;
	KUNIT_EXPECT_EQ(test, dp855_test_read1(dp), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->nw, 0);
	KUNIT_EXPECT_EQ(test, f->nreads, 0);
}

static void dp855_test_lock_cycles(struct kunit *test)
{
	struct dp855_fake *f;
	struct dp855 *dp = dp855_test_init(test, &f);

	KUNIT_EXPECT_EQ(test, dp855_lock_cycles_set(dp, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, dp855_lock_cycles_set(dp, 1001), -EINVAL);
	KUNIT_EXPECT_EQ(test, dp855_lock_cycles_set(dp, 5), 0);
	KUNIT_EXPECT_EQ(test, dp->stats.lock_ok, 5);
	/* acquire, enable, aux clear, release per cycle */
	KUNIT_EXPECT_EQ(test, f->nw, 20);
	expect_released(test, f);

	f->ack = DP855_ACCESS_ACK_REFUSED;
	KUNIT_EXPECT_EQ(test, dp855_lock_cycles_set(dp, 5), -EBUSY);
	KUNIT_EXPECT_EQ(test, dp->stats.lock_refused, 1);
	expect_released(test, f);
}

static struct kunit_case dp855_test_cases[] = {
	KUNIT_CASE(dp855_test_pack_request),
	KUNIT_CASE(dp855_test_spi_frames),
	KUNIT_CASE(dp855_test_plain_read),
	KUNIT_CASE(dp855_test_pointer_read),
	KUNIT_CASE(dp855_test_rejects),
	KUNIT_CASE(dp855_test_lock_refused),
	KUNIT_CASE(dp855_test_lock_timeout),
	KUNIT_CASE(dp855_test_lock_bad_ack),
	KUNIT_CASE(dp855_test_busy_timeout),
	KUNIT_CASE(dp855_test_done_timeout),
	KUNIT_CASE(dp855_test_completion_errors),
	KUNIT_CASE(dp855_test_io_errors),
	KUNIT_CASE(dp855_test_suspended),
	KUNIT_CASE(dp855_test_lock_cycles),
	{ }
};

static struct kunit_suite dp855_test_suite = {
	.name = "i2c-dp855",
	.test_cases = dp855_test_cases,
};

kunit_test_suite(dp855_test_suite);
