// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Stockholm NFC controller and embedded Secure Element over SPMI
 *
 * Stockholm is an NCI controller with an embedded Secure Element. On the
 * Macs it sits on its own SPMI bus and is enabled through an SMC GPIO (VEN).
 * The Mac parts report no RF interfaces and no listen-mode routing in
 * CORE_INIT, so there is no NFC reader to offer the NFC core: the controller
 * only fronts the Secure Element. This driver checks that the follower is
 * present, reads the controller identity once and then holds the controller
 * in its deepest power state (VEN low) for the life of the system.
 *
 * Copyright The Aurora Silicon Contributors
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/spmi.h>
#include <linux/unaligned.h>

#include <net/nfc/nci.h>

#define STOCKHOLM_FIFO_CHUNK		8
#define STOCKHOLM_NCI_TIMEOUT_MS	500

#define STOCKHOLM_ERROR_STATUS0_REG	0x4
#define STOCKHOLM_ERROR_STATUS1_REG	0x7
#define STOCKHOLM_ERROR_STATUS2_REG	0x8

/* NCI 1.x CORE_INIT_RSP: status, 4 feature bytes, RF interface count. */
#define STOCKHOLM_INIT_RSP_RF_COUNT	5
/* After the RF list: connections, routing (2), control size, large (2). */
#define STOCKHOLM_INIT_RSP_TAIL		6
#define STOCKHOLM_INIT_RSP_MANUF_LEN	5

/* Transport parameters of one controller model. */
struct stockholm_model {
	u16 tx_reg;		/* non-incrementing NCI transmit FIFO */
	u16 rx_reg;		/* non-incrementing NCI receive FIFO */
	u16 id_reg;		/* follower ID, side-effect-free read */
	u16 irq_reg;		/* follower IRQ base, side-effect-free read */
	u16 standby_reg;	/* follower virtual command register */
	u8 follower_id;		/* expected value of id_reg */
	u8 irq_base;		/* expected value of irq_reg */
	u8 standby_off;
	u32 startup_delay_us;
};

static const struct stockholm_model stockholm_model96 = {
	.tx_reg = 0x1000,
	.rx_reg = 0x80,
	.id_reg = 0x100,
	.irq_reg = 0x105,
	.standby_reg = 0x0,
	.follower_id = 0x19,
	.irq_base = 0xe0,
	.standby_off = 0x14,
	.startup_delay_us = 10000,
};

struct stockholm {
	struct spmi_device *sdev;
	const struct stockholm_model *model;
	struct gpio_desc *enable_gpio;
	int data_irq;
	int error_irq;

	/* Serialises FIFO access and the awaited-response state below. */
	struct mutex io_lock;
	struct completion rx_done;
	bool rx_waiting;
	u8 rx_hdr[2];		/* awaited packet: MT/GID and OID bytes */
	u8 rx_len;
	u8 rx_payload[NCI_MAX_PAYLOAD_SIZE];
	u8 pkt[NCI_CTRL_HDR_SIZE + NCI_MAX_PAYLOAD_SIZE];
};

static int stockholm_fifo_read(struct stockholm *st, u8 *buf, size_t len)
{
	size_t chunk;
	int ret;

	while (len) {
		chunk = min_t(size_t, len, STOCKHOLM_FIFO_CHUNK);
		ret = spmi_ext_register_readl(st->sdev, st->model->rx_reg, buf,
					      chunk);
		if (ret)
			return ret;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int stockholm_fifo_write(struct stockholm *st, const u8 *buf, size_t len)
{
	size_t chunk;
	int ret;

	while (len) {
		chunk = min_t(size_t, len, STOCKHOLM_FIFO_CHUNK);
		ret = spmi_ext_register_writel(st->sdev, st->model->tx_reg,
					       buf, chunk);
		if (ret)
			return ret;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int stockholm_standby_off(struct stockholm *st)
{
	u8 value = st->model->standby_off;

	return spmi_ext_register_writel(st->sdev, st->model->standby_reg,
					&value, 1);
}

static int stockholm_power_on(struct stockholm *st)
{
	u32 delay = st->model->startup_delay_us;
	int ret;

	ret = gpiod_direction_output(st->enable_gpio, 1);
	if (ret)
		return ret;

	usleep_range(delay, delay + delay / 10);
	ret = stockholm_standby_off(st);
	if (ret)
		dev_dbg(&st->sdev->dev, "standby exit failed: %d\n", ret);

	return 0;
}

static int stockholm_park(struct stockholm *st)
{
	int ret;

	ret = stockholm_standby_off(st);
	if (ret)
		dev_dbg(&st->sdev->dev, "standby disable failed: %d\n", ret);

	return gpiod_direction_output(st->enable_gpio, 0);
}

static irqreturn_t stockholm_data_irq(int irq, void *data)
{
	struct stockholm *st = data;
	u8 *hdr = st->pkt;
	u8 plen = 0;
	int ret;

	mutex_lock(&st->io_lock);
	ret = stockholm_fifo_read(st, hdr, NCI_CTRL_HDR_SIZE);
	if (!ret) {
		plen = nci_plen(hdr);
		ret = stockholm_fifo_read(st, hdr + NCI_CTRL_HDR_SIZE, plen);
	}
	if (ret) {
		mutex_unlock(&st->io_lock);
		dev_err_ratelimited(&st->sdev->dev, "NCI receive failed: %d\n",
				    ret);
		return IRQ_HANDLED;
	}

	/* Ignore the packet boundary flag; match MT, GID and OID. */
	if (st->rx_waiting && (hdr[0] & 0xef) == st->rx_hdr[0] &&
	    (hdr[1] & 0x3f) == st->rx_hdr[1]) {
		memcpy(st->rx_payload, hdr + NCI_CTRL_HDR_SIZE, plen);
		st->rx_len = plen;
		st->rx_waiting = false;
		complete(&st->rx_done);
	} else {
		dev_dbg(&st->sdev->dev, "dropped NCI packet %02x %02x len %u\n",
			hdr[0], hdr[1], plen);
	}
	mutex_unlock(&st->io_lock);

	return IRQ_HANDLED;
}

static irqreturn_t stockholm_error_irq(int irq, void *data)
{
	struct stockholm *st = data;
	u8 status[3];
	int ret;

	mutex_lock(&st->io_lock);
	ret = spmi_ext_register_readl(st->sdev, STOCKHOLM_ERROR_STATUS0_REG,
				      &status[0], 1);
	if (!ret)
		ret = spmi_ext_register_readl(st->sdev,
					      STOCKHOLM_ERROR_STATUS1_REG,
					      &status[1], 1);
	if (!ret)
		ret = spmi_ext_register_readl(st->sdev,
					      STOCKHOLM_ERROR_STATUS2_REG,
					      &status[2], 1);
	mutex_unlock(&st->io_lock);

	if (ret)
		dev_err_ratelimited(&st->sdev->dev,
				    "transport error, status read failed: %d\n",
				    ret);
	else
		dev_err_ratelimited(&st->sdev->dev,
				    "transport error, status %02x/%02x/%02x\n",
				    status[0], status[1], status[2]);

	return IRQ_HANDLED;
}

/*
 * Send one NCI control command and wait for its response. Returns the
 * response payload length; the payload is left in st->rx_payload.
 */
static int stockholm_nci_cmd(struct stockholm *st, u16 opcode,
			     const u8 *payload, u8 plen)
{
	u8 cmd[NCI_CTRL_HDR_SIZE + 2] = {};
	unsigned long left;
	int ret;

	if (plen > sizeof(cmd) - NCI_CTRL_HDR_SIZE)
		return -EINVAL;

	cmd[0] = nci_opcode_gid(opcode);
	nci_mt_set(cmd, NCI_MT_CMD_PKT);
	cmd[1] = nci_opcode_oid(opcode);
	cmd[2] = plen;
	if (plen)
		memcpy(cmd + NCI_CTRL_HDR_SIZE, payload, plen);

	mutex_lock(&st->io_lock);
	reinit_completion(&st->rx_done);
	st->rx_hdr[0] = (NCI_MT_RSP_PKT << 5) | nci_opcode_gid(opcode);
	st->rx_hdr[1] = nci_opcode_oid(opcode);
	st->rx_waiting = true;
	ret = stockholm_fifo_write(st, cmd, NCI_CTRL_HDR_SIZE + plen);
	if (ret)
		st->rx_waiting = false;
	mutex_unlock(&st->io_lock);
	if (ret)
		return ret;

	left = msecs_to_jiffies(STOCKHOLM_NCI_TIMEOUT_MS);
	left = wait_for_completion_timeout(&st->rx_done, left);

	mutex_lock(&st->io_lock);
	if (!left && st->rx_waiting) {
		st->rx_waiting = false;
		ret = -ETIMEDOUT;
	} else {
		ret = st->rx_len;
	}
	mutex_unlock(&st->io_lock);

	return ret;
}

/* What CORE_RESET and CORE_INIT report about the controller. */
struct stockholm_ident {
	u8 nci_version;
	u8 manufacturer;
	u8 rf_interfaces;
	u32 firmware;
};

/* NCI 1.x CORE_RESET_RSP payload: status, NCI version, configuration status. */
static int stockholm_parse_reset_rsp(const u8 *rsp, int len,
				     struct stockholm_ident *id)
{
	if (len < 1 || rsp[0] != NCI_STATUS_OK)
		return -EIO;
	/* NCI 2.x answers with the status only and reports in a notification. */
	if (len != 3)
		return -EOPNOTSUPP;

	id->nci_version = rsp[1];
	return 0;
}

/*
 * NCI 1.x CORE_INIT_RSP payload: status, features (4), RF interface count and
 * list, logical connections, routing table size (2), control payload size,
 * large parameter size (2), manufacturer ID, manufacturer data (4).
 */
static int stockholm_parse_init_rsp(const u8 *rsp, int len,
				    struct stockholm_ident *id)
{
	unsigned int tail;

	if (len <= STOCKHOLM_INIT_RSP_RF_COUNT || rsp[0] != NCI_STATUS_OK)
		return -EIO;

	id->rf_interfaces = rsp[STOCKHOLM_INIT_RSP_RF_COUNT];
	tail = STOCKHOLM_INIT_RSP_RF_COUNT + 1 + id->rf_interfaces +
	       STOCKHOLM_INIT_RSP_TAIL;
	if (len < tail + STOCKHOLM_INIT_RSP_MANUF_LEN)
		return -EIO;

	id->manufacturer = rsp[tail];
	id->firmware = get_unaligned_le32(&rsp[tail + 1]);
	return 0;
}

/* CORE_RESET and CORE_INIT, then log what the controller reports. */
static int stockholm_identify(struct stockholm *st)
{
	u8 reset_type = NCI_RESET_TYPE_RESET_CONFIG;
	struct stockholm_ident id = {};
	int len, ret;

	len = stockholm_nci_cmd(st, NCI_OP_CORE_RESET_CMD, &reset_type, 1);
	if (len < 0)
		return len;
	ret = stockholm_parse_reset_rsp(st->rx_payload, len, &id);
	if (ret)
		return ret;

	len = stockholm_nci_cmd(st, NCI_OP_CORE_INIT_CMD, NULL, 0);
	if (len < 0)
		return len;
	ret = stockholm_parse_init_rsp(st->rx_payload, len, &id);
	if (ret)
		return ret;

	dev_info(&st->sdev->dev,
		 "NCI %u.%u controller, manufacturer %#04x, firmware %08x, %u RF interfaces; parked off\n",
		 id.nci_version >> 4, id.nci_version & 0xf, id.manufacturer,
		 id.firmware, id.rf_interfaces);

	return 0;
}

static int stockholm_check_follower(struct stockholm *st)
{
	struct device *dev = &st->sdev->dev;
	u8 value;
	int ret;

	ret = spmi_ext_register_readl(st->sdev, st->model->id_reg, &value, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read follower ID\n");
	if (value != st->model->follower_id)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected follower ID %#04x\n", value);

	ret = spmi_ext_register_readl(st->sdev, st->model->irq_reg, &value, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read IRQ base\n");
	if (value != st->model->irq_base)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected IRQ base %#04x\n", value);

	return 0;
}

static int stockholm_request_irq(struct stockholm *st, const char *name,
				 irq_handler_t thread_fn)
{
	struct device *dev = &st->sdev->dev;
	int irq, ret;

	irq = fwnode_irq_get_byname(dev_fwnode(dev), name);
	if (irq < 0)
		return dev_err_probe(dev, irq, "no %s interrupt\n", name);

	ret = devm_request_threaded_irq(dev, irq, NULL, thread_fn,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request %s IRQ\n",
				     name);

	return irq;
}

static int stockholm_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct stockholm *st;
	int ret, err;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->sdev = sdev;
	st->model = device_get_match_data(dev);
	if (!st->model)
		return -ENODEV;
	mutex_init(&st->io_lock);
	init_completion(&st->rx_done);

	st->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(st->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(st->enable_gpio),
				     "failed to get VEN GPIO\n");

	st->data_irq = stockholm_request_irq(st, "data-ready",
					     stockholm_data_irq);
	if (st->data_irq < 0)
		return st->data_irq;
	st->error_irq = stockholm_request_irq(st, "error",
					      stockholm_error_irq);
	if (st->error_irq < 0)
		return st->error_irq;

	ret = stockholm_power_on(st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to raise VEN\n");

	ret = stockholm_check_follower(st);
	if (!ret) {
		enable_irq(st->error_irq);
		enable_irq(st->data_irq);
		err = stockholm_identify(st);
		disable_irq(st->data_irq);
		disable_irq(st->error_irq);
		if (err)
			dev_warn(dev, "identity read failed (%d); parked off\n",
				 err);
	}

	err = stockholm_park(st);
	if (err)
		return dev_err_probe(dev, err, "failed to park VEN low\n");

	return ret;
}

static const struct of_device_id stockholm_of_match[] = {
	{ .compatible = "apple,stockholm-nci", .data = &stockholm_model96 },
	{}
};
MODULE_DEVICE_TABLE(of, stockholm_of_match);

static struct spmi_driver stockholm_driver = {
	.probe = stockholm_probe,
	.driver = {
		.name = "apple-stockholm",
		.of_match_table = stockholm_of_match,
	},
};
module_spmi_driver(stockholm_driver);

#if IS_ENABLED(CONFIG_NFC_APPLE_STOCKHOLM_KUNIT_TEST)
#include "apple-stockholm-test.c"
#endif

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("Apple Stockholm NFC controller and Secure Element (power and identity)");
MODULE_LICENSE("GPL");
