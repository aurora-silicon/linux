// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Parade DP855 panel timing controller: read-only downstream I2C adapter
 *
 * The DP855 is controlled over SPI with 16-bit register addresses. Inside
 * its register space sits an I2C transaction engine that reaches the panel
 * management devices behind the controller. The engine is shared with the
 * display firmware, which arbitrates it with an access lock register.
 *
 * This driver only reads. It accepts a single read message, or a one-byte
 * register pointer write immediately followed by a read from the same
 * address, for the 7-bit addresses of the enabled device tree children of
 * its "i2c" node. Everything else fails before any SPI traffic. A refused or
 * timed-out lock is an ordinary transfer failure; the controller is never
 * reset and the lock is never forced.
 *
 * Copyright The Aurora Silicon Contributors
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/* SPI framing: one command byte, a big-endian register address, payload. */
#define DP855_SPI_CMD_WRITE		0x00
#define DP855_SPI_CMD_READ		0x20
#define DP855_SPI_HDR_LEN		3
/* A read clocks six pad bytes after the address; data starts at byte 9. */
#define DP855_SPI_READ_PAD		6
#define DP855_SPI_READ_DATA_OFF		(DP855_SPI_HDR_LEN + DP855_SPI_READ_PAD)

/* Largest staged payload this driver moves in one engine transaction. */
#define DP855_MAX_DATA			16
#define DP855_SPI_BUF_LEN		(DP855_SPI_READ_DATA_OFF + DP855_MAX_DATA)

/* I2C engine registers */
#define DP855_REG_I2C_ENABLE		0x3800
#define   DP855_I2C_ENABLE		BIT(0)
#define DP855_REG_I2C_REQUEST		0x3804	/* 64-bit request word */
#define DP855_REG_I2C_CMD		0x380c
#define   DP855_I2C_CMD_START		0x01
#define   DP855_I2C_CMD_PREPARE		0x02
#define DP855_REG_I2C_STATE		0x3810
#define   DP855_I2C_STATE_BUSY		BIT(2)
#define   DP855_I2C_STATE_CLEAR_ALL	0xff
#define DP855_REG_I2C_STATUS		0x3900
#define   DP855_I2C_STATUS_DONE		BIT(0)
#define   DP855_I2C_STATUS_ERR		GENMASK(2, 1)
#define   DP855_I2C_STATUS_CLEAR_ALL	0xff
#define DP855_REG_I2C_AUX_STATUS	0x3904
#define DP855_REG_I2C_WDATA		0x3a00	/* outbound staging, 16 bytes */
#define DP855_REG_I2C_RDATA		0x3a10	/* inbound staging, 16 bytes */
#define DP855_REG_I2C_CTRL		0x3b1c	/* 32-bit control word */
#define DP855_REG_ACCESS_REQ		0x3b92
#define   DP855_ACCESS_ACQUIRE		0x0c	/* request the downstream engine */
#define   DP855_ACCESS_RELEASE		0x00
#define DP855_REG_ACCESS_ACK		0x3b9b
#define   DP855_ACCESS_ACK_MASK		GENMASK(1, 0)
#define   DP855_ACCESS_ACK_PENDING	0
#define   DP855_ACCESS_ACK_GRANTED	1
#define   DP855_ACCESS_ACK_REFUSED	3

/*
 * Control word. The engine's base configuration sets bits 1-3, 13 and 22;
 * their individual functions are not documented, so they are named by
 * position. Bit 4 marks a transaction that is continued by another chunk.
 * This driver never chunks, so it always leaves bit 4 clear.
 */
#define DP855_I2C_CTRL_CFG_1_3		GENMASK(3, 1)
#define DP855_I2C_CTRL_CONTINUE		BIT(4)
#define DP855_I2C_CTRL_CFG_13		BIT(13)
#define DP855_I2C_CTRL_CFG_22		BIT(22)
#define DP855_I2C_CTRL_BASE		(DP855_I2C_CTRL_CFG_1_3 | \
					 DP855_I2C_CTRL_CFG_13 | \
					 DP855_I2C_CTRL_CFG_22)

/* Request word fields */
#define DP855_REQ_READ			BIT_ULL(0)
#define DP855_REQ_OFFSET_BYTES		GENMASK_ULL(2, 1)
#define DP855_REQ_WRITE_LEN		GENMASK_ULL(12, 3)	/* length - 1 */
#define DP855_REQ_READ_LEN		GENMASK_ULL(22, 13)	/* length - 1 */
#define DP855_REQ_ADDR			GENMASK_ULL(39, 33)
#define DP855_REQ_OFFSET		GENMASK_ULL(55, 40)

/* Poll bounds: one read, then at most this many retries 100 us apart. */
#define DP855_POLL_DELAY_US		100
#define DP855_LOCK_POLL_RETRIES		30
#define DP855_DONE_POLL_RETRIES		100

#define DP855_MAX_CLIENTS		8

/*
 * Panel management endpoints behind the controller. These belong to the
 * display firmware and are never reachable through this adapter, whatever
 * the device tree says.
 */
static const u8 dp855_forbidden_addrs[] = {
	0x40,	/* panel source driver */
	0x4f,	/* panel PMIC write-protect endpoint */
	0x71,	/* source driver write-protect endpoint */
	0x74,	/* panel PMIC */
};

struct dp855_regio {
	int (*read)(void *ctx, u16 reg, u8 *buf, size_t len);
	int (*write)(void *ctx, u16 reg, const u8 *buf, size_t len);
};

struct dp855_stats {
	u64 lock_ok;
	u64 lock_refused;
	u64 lock_timeout;
	u64 lock_error;
	u64 xfer_ok;
	u64 xfer_nak;
	u64 xfer_timeout;
	u64 rejected;
	int last_error;
	u8 last_ack;
};

struct dp855 {
	struct device *dev;
	const struct dp855_regio *io;
	void *io_ctx;
	struct spi_device *spi;
	/* Serializes every access to the controller and the stats below. */
	struct mutex lock;
	bool suspended;
	struct dp855_stats stats;
	struct i2c_adapter adap;
	bool adap_registered;
	u8 allow[DP855_MAX_CLIENTS];
	unsigned int nallow;
	struct dentry *debugfs;

	u8 tx[DP855_SPI_BUF_LEN] ____cacheline_aligned;
	u8 rx[DP855_SPI_BUF_LEN] ____cacheline_aligned;
};

static u64 dp855_pack_request(bool read, u8 addr, unsigned int offset_bytes,
			      u16 offset, unsigned int wlen, unsigned int rlen)
{
	u64 req = 0;

	if (read)
		req |= DP855_REQ_READ;
	req |= FIELD_PREP(DP855_REQ_OFFSET_BYTES, offset_bytes);
	if (wlen)
		req |= FIELD_PREP(DP855_REQ_WRITE_LEN, wlen - 1);
	if (rlen)
		req |= FIELD_PREP(DP855_REQ_READ_LEN, rlen - 1);
	req |= FIELD_PREP(DP855_REQ_ADDR, addr);
	if (offset_bytes)
		req |= FIELD_PREP(DP855_REQ_OFFSET, offset);

	return req;
}

/* SPI register access */

static void dp855_spi_read_cmd(u8 *tx, u16 reg, size_t len)
{
	memset(tx, 0, DP855_SPI_READ_DATA_OFF + len);
	tx[0] = DP855_SPI_CMD_READ;
	put_unaligned_be16(reg, &tx[1]);
}

static void dp855_spi_write_cmd(u8 *tx, u16 reg, const u8 *buf, size_t len)
{
	tx[0] = DP855_SPI_CMD_WRITE;
	put_unaligned_be16(reg, &tx[1]);
	memcpy(&tx[DP855_SPI_HDR_LEN], buf, len);
}

static int dp855_spi_read(void *ctx, u16 reg, u8 *buf, size_t len)
{
	struct dp855 *dp = ctx;
	struct spi_transfer xfer = {
		.tx_buf = dp->tx,
		.rx_buf = dp->rx,
		.len = DP855_SPI_READ_DATA_OFF + len,
	};
	int ret;

	if (len > DP855_MAX_DATA)
		return -EINVAL;

	dp855_spi_read_cmd(dp->tx, reg, len);
	ret = spi_sync_transfer(dp->spi, &xfer, 1);
	if (ret)
		return ret;

	memcpy(buf, &dp->rx[DP855_SPI_READ_DATA_OFF], len);
	return 0;
}

static int dp855_spi_write(void *ctx, u16 reg, const u8 *buf, size_t len)
{
	struct dp855 *dp = ctx;
	struct spi_transfer xfer = {
		.tx_buf = dp->tx,
		.len = DP855_SPI_HDR_LEN + len,
	};

	if (len > DP855_MAX_DATA)
		return -EINVAL;

	dp855_spi_write_cmd(dp->tx, reg, buf, len);
	return spi_sync_transfer(dp->spi, &xfer, 1);
}

static const struct dp855_regio dp855_spi_regio = {
	.read = dp855_spi_read,
	.write = dp855_spi_write,
};

static int dp855_read8(struct dp855 *dp, u16 reg, u8 *val)
{
	return dp->io->read(dp->io_ctx, reg, val, 1);
}

static int dp855_write8(struct dp855 *dp, u16 reg, u8 val)
{
	return dp->io->write(dp->io_ctx, reg, &val, 1);
}

static int dp855_write32(struct dp855 *dp, u16 reg, u32 val)
{
	u8 buf[4];

	put_unaligned_le32(val, buf);
	return dp->io->write(dp->io_ctx, reg, buf, sizeof(buf));
}

static int dp855_write64(struct dp855 *dp, u16 reg, u64 val)
{
	u8 buf[8];

	put_unaligned_le64(val, buf);
	return dp->io->write(dp->io_ctx, reg, buf, sizeof(buf));
}

/* Access lock */

static int dp855_wait_ack(struct dp855 *dp)
{
	unsigned int i;
	int ret;
	u8 val;

	for (i = 0; i <= DP855_LOCK_POLL_RETRIES; i++) {
		if (i)
			fsleep(DP855_POLL_DELAY_US);
		ret = dp855_read8(dp, DP855_REG_ACCESS_ACK, &val);
		if (ret)
			return ret;
		dp->stats.last_ack = val;

		switch (FIELD_GET(DP855_ACCESS_ACK_MASK, val)) {
		case DP855_ACCESS_ACK_GRANTED:
			return 0;
		case DP855_ACCESS_ACK_REFUSED:
			dp->stats.lock_refused++;
			return -EBUSY;
		case DP855_ACCESS_ACK_PENDING:
			break;
		default:
			dp->stats.lock_error++;
			return -EIO;
		}
	}

	dp->stats.lock_timeout++;
	return -ETIMEDOUT;
}

static int dp855_wait_idle(struct dp855 *dp)
{
	unsigned int i;
	int ret;
	u8 val;

	for (i = 0; i <= DP855_LOCK_POLL_RETRIES; i++) {
		if (i)
			fsleep(DP855_POLL_DELAY_US);
		ret = dp855_read8(dp, DP855_REG_I2C_STATE, &val);
		if (ret)
			return ret;
		if (!(val & DP855_I2C_STATE_BUSY))
			return 0;
	}

	dp->stats.lock_timeout++;
	return -ETIMEDOUT;
}

static void dp855_release(struct dp855 *dp)
{
	int ret;

	ret = dp855_write8(dp, DP855_REG_ACCESS_REQ, DP855_ACCESS_RELEASE);
	if (ret)
		dev_warn_ratelimited(dp->dev, "access release failed: %d\n", ret);
}

/* Called with dp->lock held. On failure the lock request is withdrawn. */
static int dp855_acquire(struct dp855 *dp)
{
	int ret;

	if (dp->suspended)
		return -EBUSY;

	ret = dp855_write8(dp, DP855_REG_ACCESS_REQ, DP855_ACCESS_ACQUIRE);
	if (ret)
		goto err;

	ret = dp855_wait_ack(dp);
	if (ret)
		goto err;

	ret = dp855_wait_idle(dp);
	if (ret)
		goto err;

	ret = dp855_write8(dp, DP855_REG_I2C_ENABLE, DP855_I2C_ENABLE);
	if (ret)
		goto err;

	ret = dp855_write8(dp, DP855_REG_I2C_AUX_STATUS, 0);
	if (ret)
		goto err;

	dp->stats.lock_ok++;
	return 0;

err:
	dp855_release(dp);
	dp->stats.last_error = ret;
	return ret;
}

/* I2C engine */

static int dp855_wait_done(struct dp855 *dp)
{
	unsigned int i;
	int ret;
	u8 val;

	for (i = 0; i <= DP855_DONE_POLL_RETRIES; i++) {
		if (i)
			fsleep(DP855_POLL_DELAY_US);
		ret = dp855_read8(dp, DP855_REG_I2C_STATUS, &val);
		if (ret)
			return ret;
		if (!(val & DP855_I2C_STATUS_DONE))
			continue;
		if (val & DP855_I2C_STATUS_ERR) {
			dp->stats.xfer_nak++;
			return -EREMOTEIO;
		}
		return 0;
	}

	dp->stats.xfer_timeout++;
	return -ETIMEDOUT;
}

/* Called with the access lock held. */
static int dp855_engine_read(struct dp855 *dp, u8 addr, unsigned int offset_bytes,
			     u16 offset, u8 *buf, unsigned int len)
{
	u64 req = dp855_pack_request(true, addr, offset_bytes, offset, 0, len);
	int ret;

	ret = dp855_write8(dp, DP855_REG_I2C_STATE, DP855_I2C_STATE_CLEAR_ALL);
	if (!ret)
		ret = dp855_write8(dp, DP855_REG_I2C_STATUS,
				   DP855_I2C_STATUS_CLEAR_ALL);
	if (!ret)
		ret = dp855_write8(dp, DP855_REG_I2C_CMD, DP855_I2C_CMD_PREPARE);
	if (!ret)
		ret = dp855_write32(dp, DP855_REG_I2C_CTRL, DP855_I2C_CTRL_BASE);
	if (!ret)
		ret = dp855_write64(dp, DP855_REG_I2C_REQUEST, req);
	if (!ret)
		ret = dp855_write8(dp, DP855_REG_I2C_CMD, DP855_I2C_CMD_START);
	if (!ret)
		ret = dp855_wait_done(dp);
	if (!ret)
		ret = dp->io->read(dp->io_ctx, DP855_REG_I2C_RDATA, buf, len);

	return ret;
}

static bool dp855_addr_allowed(const struct dp855 *dp, u16 addr)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dp855_forbidden_addrs); i++)
		if (addr == dp855_forbidden_addrs[i])
			return false;

	for (i = 0; i < dp->nallow; i++)
		if (addr == dp->allow[i])
			return true;

	return false;
}

static bool dp855_read_msg_ok(const struct i2c_msg *msg)
{
	return (msg->flags & I2C_M_RD) &&
	       !(msg->flags & (I2C_M_TEN | I2C_M_RECV_LEN)) &&
	       msg->len >= 1 && msg->len <= DP855_MAX_DATA;
}

/*
 * Accepts one read message, or a one-byte pointer write followed by a read
 * from the same address. Returns the register pointer width (0 or 1).
 */
static int dp855_check_msgs(const struct dp855 *dp, const struct i2c_msg *msgs,
			    int num)
{
	const struct i2c_msg *rd;
	int offset_bytes;

	if (num == 1) {
		rd = &msgs[0];
		offset_bytes = 0;
	} else if (num == 2) {
		if (msgs[0].flags & (I2C_M_RD | I2C_M_TEN) || msgs[0].len != 1 ||
		    msgs[0].addr != msgs[1].addr)
			return -EOPNOTSUPP;
		rd = &msgs[1];
		offset_bytes = 1;
	} else {
		return -EOPNOTSUPP;
	}

	if (!dp855_read_msg_ok(rd) || !dp855_addr_allowed(dp, rd->addr))
		return -EOPNOTSUPP;

	return offset_bytes;
}

static int dp855_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			  int num)
{
	struct dp855 *dp = i2c_get_adapdata(adap);
	const struct i2c_msg *rd = &msgs[num - 1];
	u8 buf[DP855_MAX_DATA];
	int offset_bytes, ret;

	offset_bytes = dp855_check_msgs(dp, msgs, num);
	if (offset_bytes < 0) {
		mutex_lock(&dp->lock);
		dp->stats.rejected++;
		mutex_unlock(&dp->lock);
		return offset_bytes;
	}

	mutex_lock(&dp->lock);

	ret = dp855_acquire(dp);
	if (ret)
		goto out;

	ret = dp855_engine_read(dp, rd->addr, offset_bytes,
				offset_bytes ? msgs[0].buf[0] : 0, buf, rd->len);
	dp855_release(dp);

	if (ret) {
		dp->stats.last_error = ret;
		goto out;
	}

	memcpy(rd->buf, buf, rd->len);
	dp->stats.xfer_ok++;
	ret = num;
out:
	mutex_unlock(&dp->lock);
	return ret;
}

static u32 dp855_i2c_func(struct i2c_adapter *adap)
{
	/* Writes other than a one-byte register pointer are refused. */
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_READ_BYTE |
	       I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_READ_WORD_DATA |
	       I2C_FUNC_SMBUS_READ_I2C_BLOCK;
}

static const struct i2c_algorithm dp855_i2c_algo = {
	.xfer = dp855_i2c_xfer,
	.functionality = dp855_i2c_func,
};

static const struct i2c_adapter_quirks dp855_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = 2,
	.max_read_len = DP855_MAX_DATA,
	.max_comb_1st_msg_len = 1,
	.max_comb_2nd_msg_len = DP855_MAX_DATA,
};

/* debugfs: lock statistics and a bounded acquire/release exercise */

static int dp855_stats_show(struct seq_file *s, void *unused)
{
	struct dp855 *dp = s->private;

	mutex_lock(&dp->lock);
	seq_printf(s, "lock_ok %llu\nlock_refused %llu\nlock_timeout %llu\n",
		   dp->stats.lock_ok, dp->stats.lock_refused,
		   dp->stats.lock_timeout);
	seq_printf(s, "lock_error %llu\nxfer_ok %llu\nxfer_nak %llu\n",
		   dp->stats.lock_error, dp->stats.xfer_ok, dp->stats.xfer_nak);
	seq_printf(s, "xfer_timeout %llu\nrejected %llu\nlast_error %d\n",
		   dp->stats.xfer_timeout, dp->stats.rejected,
		   dp->stats.last_error);
	seq_printf(s, "last_ack 0x%02x\n", dp->stats.last_ack);
	mutex_unlock(&dp->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dp855_stats);

static int dp855_lock_cycles_set(void *data, u64 val)
{
	struct dp855 *dp = data;
	int ret = 0;
	u64 i;

	if (!val || val > 1000)
		return -EINVAL;

	for (i = 0; i < val && !ret; i++) {
		mutex_lock(&dp->lock);
		ret = dp855_acquire(dp);
		if (!ret)
			dp855_release(dp);
		mutex_unlock(&dp->lock);
	}

	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(dp855_lock_cycles_fops, NULL, dp855_lock_cycles_set,
			 "%llu\n");

/* Probe */

static int dp855_parse_clients(struct dp855 *dp, struct device_node *bus)
{
	struct device_node *child;
	u32 addr;

	for_each_available_child_of_node(bus, child) {
		if (of_property_read_u32(child, "reg", &addr) || addr > 0x7f) {
			dev_err(dp->dev, "%pOF: bad address\n", child);
			of_node_put(child);
			return -EINVAL;
		}
		if (dp->nallow == DP855_MAX_CLIENTS) {
			of_node_put(child);
			return -E2BIG;
		}
		dp->allow[dp->nallow++] = addr;
		if (!dp855_addr_allowed(dp, addr)) {
			dev_err(dp->dev, "%pOF: address 0x%02x is reserved\n",
				child, addr);
			of_node_put(child);
			return -EINVAL;
		}
	}

	return 0;
}

static int dp855_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct device_node *bus;
	struct dp855 *dp;
	int ret;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;

	dp->dev = dev;
	dp->spi = spi;
	dp->io = &dp855_spi_regio;
	dp->io_ctx = dp;
	ret = devm_mutex_init(dev, &dp->lock);
	if (ret)
		return ret;
	spi_set_drvdata(spi, dp);

	dp->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("stats", 0400, dp->debugfs, dp, &dp855_stats_fops);
	debugfs_create_file_unsafe("lock_cycles", 0200, dp->debugfs, dp,
				   &dp855_lock_cycles_fops);

	bus = of_get_available_child_by_name(dev->of_node, "i2c");
	if (!bus)
		return 0;

	ret = dp855_parse_clients(dp, bus);
	if (ret) {
		of_node_put(bus);
		goto err_debugfs;
	}

	dp->adap.owner = THIS_MODULE;
	dp->adap.algo = &dp855_i2c_algo;
	dp->adap.quirks = &dp855_i2c_quirks;
	dp->adap.dev.parent = dev;
	dp->adap.dev.of_node = bus;
	dp->adap.class = 0;
	strscpy(dp->adap.name, "dp855-i2c", sizeof(dp->adap.name));
	i2c_set_adapdata(&dp->adap, dp);

	ret = i2c_add_adapter(&dp->adap);
	of_node_put(bus);
	if (ret)
		goto err_debugfs;
	dp->adap_registered = true;

	dev_info(dev, "read-only downstream bus with %u client(s)\n",
		 dp->nallow);
	return 0;

err_debugfs:
	debugfs_remove_recursive(dp->debugfs);
	return ret;
}

static void dp855_remove(struct spi_device *spi)
{
	struct dp855 *dp = spi_get_drvdata(spi);

	if (dp->adap_registered)
		i2c_del_adapter(&dp->adap);
	debugfs_remove_recursive(dp->debugfs);
}

static int dp855_suspend(struct device *dev)
{
	struct dp855 *dp = dev_get_drvdata(dev);

	/* Waits for an in-flight transaction, which releases the lock. */
	if (dp->adap_registered)
		i2c_mark_adapter_suspended(&dp->adap);
	mutex_lock(&dp->lock);
	dp->suspended = true;
	mutex_unlock(&dp->lock);

	return 0;
}

static int dp855_resume(struct device *dev)
{
	struct dp855 *dp = dev_get_drvdata(dev);

	mutex_lock(&dp->lock);
	dp->suspended = false;
	mutex_unlock(&dp->lock);
	if (dp->adap_registered)
		i2c_mark_adapter_resumed(&dp->adap);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dp855_pm_ops, dp855_suspend, dp855_resume);

static const struct of_device_id dp855_of_match[] = {
	{ .compatible = "parade,dp855" },
	{ }
};
MODULE_DEVICE_TABLE(of, dp855_of_match);

static const struct spi_device_id dp855_spi_id[] = {
	{ "dp855" },
	{ }
};
MODULE_DEVICE_TABLE(spi, dp855_spi_id);

static struct spi_driver dp855_driver = {
	.driver = {
		.name = "i2c-dp855",
		.of_match_table = dp855_of_match,
		.pm = pm_sleep_ptr(&dp855_pm_ops),
	},
	.probe = dp855_probe,
	.remove = dp855_remove,
	.id_table = dp855_spi_id,
};
module_spi_driver(dp855_driver);

#if IS_ENABLED(CONFIG_I2C_DP855_KUNIT_TEST)
#include "i2c-dp855-test.c"
#endif

MODULE_DESCRIPTION("Parade DP855 read-only downstream I2C adapter");
MODULE_LICENSE("Dual MIT/GPL");
