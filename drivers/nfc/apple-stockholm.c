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
 * With the se_probe module parameter set, debugfs also offers a read-only
 * look at the Secure Element: NFCEE discovery and a fixed table of SELECT
 * and GET DATA commands. There is no path for any other command; see
 * stockholm_se_apdu_allowed().
 *
 * Copyright The Aurora Silicon Contributors
 */

#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spmi.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

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

/*
 * While a Secure Element probe runs, the receive handler also queues a copy
 * of every packet (notifications and data as well as responses) here.
 */
#define STOCKHOLM_TAP_SLOTS		16

struct stockholm_tap_pkt {
	u16 len;
	u8 data[NCI_CTRL_HDR_SIZE + NCI_MAX_PAYLOAD_SIZE];
};

struct stockholm_tap {
	unsigned int head;
	unsigned int count;
	unsigned int dropped;
	struct stockholm_tap_pkt pkt[STOCKHOLM_TAP_SLOTS];
};

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

/* M1/M2 generation (T6000, T8112): the legacy follower, same map. */
static const struct stockholm_model stockholm_model_b2 = {
	.tx_reg = 0x1000,
	.rx_reg = 0x80,
	.id_reg = 0x100,
	.irq_reg = 0x105,
	.standby_reg = 0x0,
	.follower_id = 0x1c,
	.irq_base = 0xa0,
	.standby_off = 0x14,
	/* Commands are not acknowledged until about 10 ms after VEN rises. */
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

	/* Read-only Secure Element probe (se_probe=1 only). */
	struct mutex se_lock;		/* one probe run at a time */
	spinlock_t tap_lock;		/* protects tap */
	wait_queue_head_t tap_wq;
	struct stockholm_tap *tap;	/* packet copies while a run is active */
	struct dentry *debugfs;
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

static void stockholm_tap_push(struct stockholm *st, const u8 *pkt, u16 len)
{
	struct stockholm_tap *tap;

	spin_lock(&st->tap_lock);
	tap = st->tap;
	if (tap && tap->count < STOCKHOLM_TAP_SLOTS) {
		struct stockholm_tap_pkt *slot;

		slot = &tap->pkt[(tap->head + tap->count) % STOCKHOLM_TAP_SLOTS];
		slot->len = len;
		memcpy(slot->data, pkt, len);
		tap->count++;
	} else if (tap) {
		tap->dropped++;
	}
	spin_unlock(&st->tap_lock);
	if (tap)
		wake_up(&st->tap_wq);
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

	stockholm_tap_push(st, hdr, NCI_CTRL_HDR_SIZE + plen);

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
	u8 cmd[NCI_CTRL_HDR_SIZE + 8] = {};
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

/* CORE_RESET and CORE_INIT. */
static int stockholm_read_ident(struct stockholm *st,
				struct stockholm_ident *id)
{
	u8 reset_type = NCI_RESET_TYPE_RESET_CONFIG;
	int len, ret;

	len = stockholm_nci_cmd(st, NCI_OP_CORE_RESET_CMD, &reset_type, 1);
	if (len < 0)
		return len;
	ret = stockholm_parse_reset_rsp(st->rx_payload, len, id);
	if (ret)
		return ret;

	len = stockholm_nci_cmd(st, NCI_OP_CORE_INIT_CMD, NULL, 0);
	if (len < 0)
		return len;
	return stockholm_parse_init_rsp(st->rx_payload, len, id);
}

/* CORE_RESET and CORE_INIT, then log what the controller reports. */
static int stockholm_identify(struct stockholm *st)
{
	struct stockholm_ident id = {};
	int ret;

	ret = stockholm_read_ident(st, &id);
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

/*
 * Read-only Secure Element probe
 *
 * The Secure Element sits behind the controller as an NFC Execution
 * Environment (NFCEE). With se_probe=1 the driver offers two debugfs files
 * that power the controller up, look, and park it again:
 *
 *   nfcee     NFCEE discovery only: which execution environments the
 *             controller reports and which protocols each one speaks.
 *   se_probe  the same, then, if one of them takes ISO 7816 APDUs directly,
 *             a logical connection to it and the fixed command table below.
 *
 * Only SELECT by name, GET DATA of three public tags and GET RESPONSE ever
 * reach the Secure Element. Commands come from the compiled-in table, never
 * from userspace, and stockholm_se_apdu_allowed() checks every command again
 * right before it is sent. Nothing here authenticates, installs, deletes,
 * stores, sets keys or changes life-cycle state: failed GlobalPlatform
 * authentication attempts can lock a card for good.
 *
 * An NFCEE that the probe enables is disabled again afterwards, so the
 * controller is left in the mode it was found in.
 */
static bool se_probe;
module_param(se_probe, bool, 0444);
MODULE_PARM_DESC(se_probe,
		 "Offer a debugfs read-only Secure Element probe (default: off)");

#define STOCKHOLM_SE_NFCEE_MAX		8
#define STOCKHOLM_SE_RX_MAX		1024
#define STOCKHOLM_SE_REPORT_SIZE	(32 * 1024)
#define STOCKHOLM_SE_DISCOVER_MS	1500
#define STOCKHOLM_SE_APDU_MS		3000
#define STOCKHOLM_SE_CREDIT_MS		1000
#define STOCKHOLM_SE_MAX_GET_RESPONSE	4

/* NCI 1.x NFCEE status in NFCEE_DISCOVER_NTF. */
#define STOCKHOLM_NFCEE_ENABLED		0x00
#define STOCKHOLM_NFCEE_DISABLED	0x01

/* NCI 1.x: an initial credit count of 0xff turns data flow control off. */
#define STOCKHOLM_NCI_NO_FLOW_CONTROL	0xff

struct stockholm_se_apdu {
	const char *name;
	u8 len;
	u8 apdu[24];
};

#define SE_SELECT(_name, ...)						\
	{ .name = _name,						\
	  .len = 6 + sizeof((u8[]){ __VA_ARGS__ }),			\
	  .apdu = { 0x00, 0xa4, 0x04, 0x00,				\
		    sizeof((u8[]){ __VA_ARGS__ }), __VA_ARGS__, 0x00 } }

#define SE_GET_DATA(_name, _p1, _p2)					\
	{ .name = _name, .len = 5, .apdu = { 0x80, 0xca, _p1, _p2, 0x00 } }

/*
 * The complete list of commands the probe sends, in order. GET DATA goes to
 * the application selected last, so it follows the card manager SELECT.
 */
static const struct stockholm_se_apdu stockholm_se_apdus[] = {
	/* Card manager (Issuer Security Domain): SELECT with no name. */
	{ .name = "isd-default", .len = 5,
	  .apdu = { 0x00, 0xa4, 0x04, 0x00, 0x00 } },
	SE_GET_DATA("cplc", 0x9f, 0x7f),
	SE_GET_DATA("card-data", 0x00, 0x66),
	SE_GET_DATA("key-info", 0x00, 0xe0),
	SE_SELECT("isd-gp", 0xa0, 0x00, 0x00, 0x01, 0x51, 0x00, 0x00, 0x00),
	SE_SELECT("isd-op", 0xa0, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00),
	SE_SELECT("gp-crs", 0xa0, 0x00, 0x00, 0x01, 0x51, 0x43, 0x52, 0x53,
		  0x00),
	/* "2PAY.SYS.DDF01" and "1PAY.SYS.DDF01" */
	SE_SELECT("ppse", 0x32, 0x50, 0x41, 0x59, 0x2e, 0x53, 0x59, 0x53,
		  0x2e, 0x44, 0x44, 0x46, 0x30, 0x31),
	SE_SELECT("pse", 0x31, 0x50, 0x41, 0x59, 0x2e, 0x53, 0x59, 0x53,
		  0x2e, 0x44, 0x44, 0x46, 0x30, 0x31),
	SE_SELECT("visa", 0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10),
	SE_SELECT("mastercard", 0xa0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10),
	SE_SELECT("maestro", 0xa0, 0x00, 0x00, 0x00, 0x04, 0x30, 0x60),
	SE_SELECT("amex", 0xa0, 0x00, 0x00, 0x00, 0x25, 0x01),
	SE_SELECT("discover", 0xa0, 0x00, 0x00, 0x01, 0x52, 0x30, 0x10),
	SE_SELECT("jcb", 0xa0, 0x00, 0x00, 0x00, 0x65, 0x10, 0x10),
	SE_SELECT("unionpay", 0xa0, 0x00, 0x00, 0x03, 0x33, 0x01, 0x01),
	SE_SELECT("interac", 0xa0, 0x00, 0x00, 0x02, 0x77, 0x10, 0x10),
	SE_SELECT("fido-u2f", 0xa0, 0x00, 0x00, 0x06, 0x47, 0x2f, 0x00, 0x01),
	SE_SELECT("piv", 0xa0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10,
		  0x00, 0x01, 0x00),
	SE_SELECT("openpgp", 0xd2, 0x76, 0x00, 0x01, 0x24, 0x01),
	SE_SELECT("ndef", 0xd2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01),
	/* Car Connectivity Consortium digital key framework, "CCCDKFv1" */
	SE_SELECT("ccc-digital-key", 0xa0, 0x00, 0x00, 0x08, 0x09, 0x43, 0x43,
		  0x43, 0x44, 0x4b, 0x46, 0x76, 0x31),
};

/*
 * The allowlist. Every command is checked here right before it reaches the
 * Secure Element, including the GET RESPONSE and corrected-length retries
 * the probe builds itself:
 *
 *   00 A4 04 00 [Lc name 00]	SELECT by DF name, first occurrence
 *   80 CA 9F 7F 00		GET DATA: Card Production Life Cycle
 *   80 CA 00 66 00		GET DATA: card data
 *   80 CA 00 E0 00		GET DATA: key information template
 *   00 C0 00 00 Le		GET RESPONSE
 *
 * Only the basic logical channel, no secure messaging, no chaining.
 */
static bool stockholm_se_apdu_allowed(const u8 *apdu, size_t len)
{
	u16 tag;

	if (len < 5)
		return false;

	switch (apdu[1]) {
	case 0xa4:	/* SELECT */
		if (apdu[0] != 0x00 || apdu[2] != 0x04 || apdu[3] != 0x00)
			return false;
		if (len == 5)
			return true;
		return apdu[4] >= 5 && apdu[4] <= 16 &&
		       len == 6 + (size_t)apdu[4];
	case 0xca:	/* GET DATA */
		if (apdu[0] != 0x80 || len != 5)
			return false;
		tag = get_unaligned_be16(&apdu[2]);
		return tag == 0x9f7f || tag == 0x0066 || tag == 0x00e0;
	case 0xc0:	/* GET RESPONSE */
		return apdu[0] == 0x00 && apdu[2] == 0x00 && apdu[3] == 0x00 &&
		       len == 5;
	default:
		return false;
	}
}

struct stockholm_se_run {
	struct stockholm *st;
	char *text;
	size_t len;

	struct stockholm_tap_pkt pkt;

	int nfcee_count;
	u8 nfcee_ntf_len[STOCKHOLM_SE_NFCEE_MAX];
	u8 nfcee_ntf[STOCKHOLM_SE_NFCEE_MAX][NCI_MAX_PAYLOAD_SIZE];

	u8 conn_id;
	bool conn_open;
	bool flow_control;
	u8 max_payload;
	unsigned int credits;

	bool rx_done;
	bool rx_overflow;
	size_t rx_len;
	u8 rx[STOCKHOLM_SE_RX_MAX];
};

static __printf(2, 3) void stockholm_se_print(struct stockholm_se_run *run,
					      const char *fmt, ...)
{
	va_list args;
	int n;

	if (run->len >= STOCKHOLM_SE_REPORT_SIZE - 1)
		return;
	va_start(args, fmt);
	n = vscnprintf(run->text + run->len, STOCKHOLM_SE_REPORT_SIZE - run->len,
		       fmt, args);
	va_end(args);
	run->len += n;
}

static void stockholm_se_print_hex(struct stockholm_se_run *run,
				   const u8 *buf, size_t len)
{
	size_t chunk;

	while (len) {
		chunk = min_t(size_t, len, 64);
		stockholm_se_print(run, "%*phN", (int)chunk, buf);
		buf += chunk;
		len -= chunk;
	}
}

static bool stockholm_tap_pending(struct stockholm *st)
{
	bool pending;

	spin_lock(&st->tap_lock);
	pending = st->tap && st->tap->count;
	spin_unlock(&st->tap_lock);

	return pending;
}

/* Take the oldest queued packet, waiting until the deadline for one. */
static int stockholm_tap_pop(struct stockholm *st, struct stockholm_tap_pkt *out,
			     unsigned long deadline)
{
	struct stockholm_tap *tap;
	long left;

	for (;;) {
		spin_lock(&st->tap_lock);
		tap = st->tap;
		if (tap->count) {
			*out = tap->pkt[tap->head];
			tap->head = (tap->head + 1) % STOCKHOLM_TAP_SLOTS;
			tap->count--;
			spin_unlock(&st->tap_lock);
			return 0;
		}
		spin_unlock(&st->tap_lock);

		left = (long)(deadline - jiffies);
		if (left <= 0)
			return -ETIMEDOUT;
		wait_event_timeout(st->tap_wq, stockholm_tap_pending(st), left);
	}
}

/* Account for one received packet: notifications and connection data. */
static void stockholm_se_handle(struct stockholm_se_run *run)
{
	const u8 *pkt = run->pkt.data;
	u8 plen = run->pkt.len - NCI_CTRL_HDR_SIZE;
	const u8 *payload = pkt + NCI_CTRL_HDR_SIZE;
	u16 op;
	int i;

	switch (nci_mt(pkt)) {
	case NCI_MT_DATA_PKT:
		if (!run->conn_open || nci_conn_id(pkt) != run->conn_id) {
			stockholm_se_print(run, "drop data conn=%u len=%u\n",
					   nci_conn_id(pkt), plen);
			return;
		}
		if (run->rx_len + plen > sizeof(run->rx)) {
			run->rx_overflow = true;
			plen = sizeof(run->rx) - run->rx_len;
		}
		memcpy(run->rx + run->rx_len, payload, plen);
		run->rx_len += plen;
		if (!nci_pbf(pkt))
			run->rx_done = true;
		return;

	case NCI_MT_NTF_PKT:
		op = nci_opcode_pack(pkt[0] & 0x0f, pkt[1] & 0x3f);
		break;

	default:
		/* Command responses are handled by stockholm_nci_cmd(). */
		return;
	}

	if (op == NCI_OP_CORE_CONN_CREDITS_NTF) {
		/* Entry count, then connection ID and credits per entry. */
		for (i = 0; plen && i < payload[0] && 2 + 2 * i < plen; i++)
			if (payload[1 + 2 * i] == run->conn_id)
				run->credits += payload[2 + 2 * i];
		return;
	}

	if (op == NCI_OP_NFCEE_DISCOVER_NTF &&
	    run->nfcee_count < STOCKHOLM_SE_NFCEE_MAX) {
		memcpy(run->nfcee_ntf[run->nfcee_count], payload, plen);
		run->nfcee_ntf_len[run->nfcee_count] = plen;
		run->nfcee_count++;
	}

	stockholm_se_print(run, "ntf gid=%u oid=%02x data=", pkt[0] & 0x0f,
			   pkt[1] & 0x3f);
	stockholm_se_print_hex(run, payload, plen);
	stockholm_se_print(run, "\n");
}

enum stockholm_se_wait {
	SE_WAIT_NFCEE,
	SE_WAIT_DATA,
	SE_WAIT_CREDIT,
};

static bool stockholm_se_done(struct stockholm_se_run *run,
			      enum stockholm_se_wait what, int nfcee_want)
{
	switch (what) {
	case SE_WAIT_NFCEE:
		return run->nfcee_count >= nfcee_want;
	case SE_WAIT_DATA:
		return run->rx_done;
	case SE_WAIT_CREDIT:
		return !run->flow_control || run->credits > 0;
	}
	return true;
}

/* Process queued packets until the condition holds or the time is up. */
static int stockholm_se_wait(struct stockholm_se_run *run,
			     enum stockholm_se_wait what, int nfcee_want,
			     unsigned int ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(ms);
	int ret;

	while (!stockholm_se_done(run, what, nfcee_want)) {
		ret = stockholm_tap_pop(run->st, &run->pkt, deadline);
		if (ret)
			return ret;
		stockholm_se_handle(run);
	}

	return 0;
}

/* Drain whatever the controller sent before the next step. */
static void stockholm_se_drain(struct stockholm_se_run *run)
{
	while (!stockholm_tap_pop(run->st, &run->pkt, jiffies))
		stockholm_se_handle(run);
}

/* One NCI command whose response starts with a status byte. */
static int stockholm_se_cmd(struct stockholm_se_run *run, const char *step,
			    u16 opcode, const u8 *payload, u8 plen)
{
	struct stockholm *st = run->st;
	int len;

	len = stockholm_nci_cmd(st, opcode, payload, plen);
	stockholm_se_print(run, "cmd %s ", step);
	if (len < 0) {
		stockholm_se_print(run, "error=%d\n", len);
		return len;
	}
	stockholm_se_print(run, "rsp=");
	stockholm_se_print_hex(run, st->rx_payload, len);
	stockholm_se_print(run, "\n");
	if (len < 1)
		return -EIO;
	if (st->rx_payload[0] != NCI_STATUS_OK)
		return -EREMOTEIO;

	return len;
}

/* Send one command APDU on the open connection and collect the reply. */
static int stockholm_se_transceive(struct stockholm_se_run *run,
				   const u8 *apdu, size_t len)
{
	struct stockholm *st = run->st;
	u8 pkt[NCI_DATA_HDR_SIZE + sizeof(stockholm_se_apdus[0].apdu)];
	int ret;

	/* The allowlist gate: nothing else ever reaches the Secure Element. */
	if (!stockholm_se_apdu_allowed(apdu, len) || len > sizeof(pkt) - 3) {
		dev_err(&st->sdev->dev, "refusing APDU %*ph\n",
			(int)min_t(size_t, len, 8), apdu);
		return -EPERM;
	}
	if (len > run->max_payload)
		return -EMSGSIZE;

	ret = stockholm_se_wait(run, SE_WAIT_CREDIT, 0, STOCKHOLM_SE_CREDIT_MS);
	if (ret)
		return ret;
	if (run->flow_control)
		run->credits--;

	pkt[0] = run->conn_id;		/* data packet, last segment */
	pkt[1] = 0;
	pkt[2] = len;
	memcpy(pkt + NCI_DATA_HDR_SIZE, apdu, len);

	run->rx_done = false;
	run->rx_overflow = false;
	run->rx_len = 0;

	mutex_lock(&st->io_lock);
	ret = stockholm_fifo_write(st, pkt, NCI_DATA_HDR_SIZE + len);
	mutex_unlock(&st->io_lock);
	if (ret)
		return ret;

	ret = stockholm_se_wait(run, SE_WAIT_DATA, 0, STOCKHOLM_SE_APDU_MS);
	if (ret)
		return ret;
	if (run->rx_overflow || run->rx_len < 2)
		return -EPROTO;

	return 0;
}

/*
 * One table entry: send it, follow 61xx with GET RESPONSE and 6Cxx with the
 * same command and the corrected length, and print the whole response.
 */
static int stockholm_se_exchange(struct stockholm_se_run *run,
				 const struct stockholm_se_apdu *cmd)
{
	u8 apdu[sizeof(cmd->apdu)];
	u8 *resp;
	size_t resp_len = 0;
	u8 sw1 = 0, sw2 = 0;
	int ret, i;

	resp = kmalloc(STOCKHOLM_SE_RX_MAX, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	memcpy(apdu, cmd->apdu, cmd->len);
	ret = stockholm_se_transceive(run, apdu, cmd->len);
	if (!ret && run->rx[run->rx_len - 2] == 0x6c) {
		/* Wrong Le: repeat the same command with the length given. */
		apdu[cmd->len - 1] = run->rx[run->rx_len - 1];
		ret = stockholm_se_transceive(run, apdu, cmd->len);
	}

	for (i = 0; !ret; i++) {
		size_t data = run->rx_len - 2;

		sw1 = run->rx[run->rx_len - 2];
		sw2 = run->rx[run->rx_len - 1];
		data = min_t(size_t, data, STOCKHOLM_SE_RX_MAX - resp_len);
		memcpy(resp + resp_len, run->rx, data);
		resp_len += data;

		if (sw1 != 0x61 || i >= STOCKHOLM_SE_MAX_GET_RESPONSE)
			break;

		/* More data waiting: GET RESPONSE. */
		apdu[0] = 0x00;
		apdu[1] = 0xc0;
		apdu[2] = 0x00;
		apdu[3] = 0x00;
		apdu[4] = sw2;
		ret = stockholm_se_transceive(run, apdu, 5);
	}

	stockholm_se_print(run, "apdu name=%s cmd=", cmd->name);
	stockholm_se_print_hex(run, cmd->apdu, cmd->len);
	if (ret) {
		stockholm_se_print(run, " error=%d\n", ret);
	} else {
		stockholm_se_print(run, " sw=%02x%02x resp=", sw1, sw2);
		stockholm_se_print_hex(run, resp, resp_len);
		stockholm_se_print(run, "\n");
	}
	kfree(resp);

	return ret;
}

/* NCI 1.x NFCEE_DISCOVER_NTF: ID, status, protocol count and list, TLVs. */
static bool stockholm_se_nfcee_has(const u8 *ntf, u8 len, u8 protocol)
{
	int i;

	if (len < 3)
		return false;
	for (i = 0; i < ntf[2] && 3 + i < len; i++)
		if (ntf[3 + i] == protocol)
			return true;

	return false;
}

static int stockholm_se_discover(struct stockholm_se_run *run)
{
	struct stockholm *st = run->st;
	u8 action = NCI_NFCEE_DISCOVERY_ACTION_ENABLE;
	int ret, want, wait;

	ret = stockholm_se_cmd(run, "nfcee-discover",
			       NCI_OP_NFCEE_DISCOVER_CMD, &action, 1);
	if (ret < 0)
		return ret;
	want = ret >= 2 ? st->rx_payload[1] : 0;

	wait = stockholm_se_wait(run, SE_WAIT_NFCEE, want,
				 STOCKHOLM_SE_DISCOVER_MS);
	stockholm_se_print(run, "nfcee expected=%d reported=%d%s\n", want,
			   run->nfcee_count, wait ? " (timed out)" : "");

	action = NCI_NFCEE_DISCOVERY_ACTION_DISABLE;
	ret = stockholm_se_cmd(run, "nfcee-discover-stop",
			       NCI_OP_NFCEE_DISCOVER_CMD, &action, 1);

	return ret < 0 ? ret : 0;
}

static int stockholm_se_apdus_run(struct stockholm_se_run *run)
{
	struct stockholm *st = run->st;
	u8 mode[2], conn[6], close;
	bool enabled = false;
	const u8 *ntf = NULL;
	unsigned int i;
	int ret, err;
	u8 id;

	for (i = 0; i < run->nfcee_count; i++) {
		if (stockholm_se_nfcee_has(run->nfcee_ntf[i],
					   run->nfcee_ntf_len[i],
					   NCI_NFCEE_INTERFACE_APDU)) {
			ntf = run->nfcee_ntf[i];
			break;
		}
	}
	if (!ntf) {
		stockholm_se_print(run, "apdu-nfcee none\n");
		return 0;
	}

	id = ntf[0];
	stockholm_se_print(run, "apdu-nfcee id=%02x status=%02x\n", id, ntf[1]);
	if (ntf[1] != STOCKHOLM_NFCEE_ENABLED &&
	    ntf[1] != STOCKHOLM_NFCEE_DISABLED)
		return -ENODEV;

	if (ntf[1] == STOCKHOLM_NFCEE_DISABLED) {
		mode[0] = id;
		mode[1] = NCI_NFCEE_ENABLE;
		ret = stockholm_se_cmd(run, "nfcee-enable",
				       NCI_OP_NFCEE_MODE_SET_CMD, mode, 2);
		if (ret < 0)
			return ret;
		enabled = true;
		stockholm_se_drain(run);
	}

	conn[0] = NCI_DESTINATION_NFCEE;
	conn[1] = 1;
	conn[2] = NCI_DESTINATION_SPECIFIC_PARAM_NFCEE_TYPE;
	conn[3] = 2;
	conn[4] = id;
	conn[5] = NCI_NFCEE_INTERFACE_APDU;
	ret = stockholm_se_cmd(run, "conn-create", NCI_OP_CORE_CONN_CREATE_CMD,
			       conn, sizeof(conn));
	if (ret >= 4) {
		/* status, max data payload, initial credits, connection ID */
		run->max_payload = st->rx_payload[1];
		run->flow_control = st->rx_payload[2] != STOCKHOLM_NCI_NO_FLOW_CONTROL;
		run->credits = run->flow_control ? st->rx_payload[2] : 0;
		run->conn_id = st->rx_payload[3] & 0x0f;
		run->conn_open = true;
		stockholm_se_print(run,
				   "conn id=%u max-payload=%u credits=%u%s\n",
				   run->conn_id, run->max_payload, run->credits,
				   run->flow_control ? "" : " (no flow control)");

		ret = 0;
		for (i = 0; i < ARRAY_SIZE(stockholm_se_apdus); i++) {
			err = stockholm_se_exchange(run, &stockholm_se_apdus[i]);
			/* A dead link fails every later command the same way. */
			if (err == -ETIMEDOUT || err == -EPERM) {
				ret = err;
				break;
			}
		}

		close = run->conn_id;
		err = stockholm_se_cmd(run, "conn-close",
				       NCI_OP_CORE_CONN_CLOSE_CMD, &close, 1);
		run->conn_open = false;
		if (err < 0 && !ret)
			ret = err;
		stockholm_se_drain(run);
	} else if (ret >= 0) {
		ret = -EPROTO;
	}

	if (enabled) {
		mode[0] = id;
		mode[1] = NCI_NFCEE_DISABLE;
		err = stockholm_se_cmd(run, "nfcee-restore-disabled",
				       NCI_OP_NFCEE_MODE_SET_CMD, mode, 2);
		if (err < 0 && !ret)
			ret = err;
		stockholm_se_drain(run);
	}

	return ret;
}

/* Power up, identify, look, park. Returns the text report. */
static void stockholm_se_run(struct stockholm_se_run *run, bool apdus)
{
	struct stockholm *st = run->st;
	struct stockholm_ident id = {};
	struct stockholm_tap *tap;
	int ret, err;

	tap = kzalloc_obj(*tap);
	if (!tap) {
		stockholm_se_print(run, "result error=%d\n", -ENOMEM);
		return;
	}

	stockholm_se_print(run, "stockholm-se-probe 1 mode=%s\n",
			   apdus ? "apdu" : "nfcee");

	spin_lock(&st->tap_lock);
	st->tap = tap;
	spin_unlock(&st->tap_lock);

	ret = stockholm_power_on(st);
	if (!ret)
		ret = stockholm_check_follower(st);
	if (ret)
		goto park;

	enable_irq(st->error_irq);
	enable_irq(st->data_irq);

	ret = stockholm_read_ident(st, &id);
	if (!ret) {
		stockholm_se_print(run,
				   "ident nci=%u.%u manufacturer=%02x firmware=%08x rf-interfaces=%u\n",
				   id.nci_version >> 4, id.nci_version & 0xf,
				   id.manufacturer, id.firmware,
				   id.rf_interfaces);
		stockholm_se_drain(run);
		ret = stockholm_se_discover(run);
	}
	if (!ret && apdus)
		ret = stockholm_se_apdus_run(run);

	stockholm_se_drain(run);
	disable_irq(st->data_irq);
	disable_irq(st->error_irq);

park:
	err = stockholm_park(st);
	if (err && !ret)
		ret = err;

	spin_lock(&st->tap_lock);
	if (tap->dropped)
		stockholm_se_print(run, "tap dropped=%u\n", tap->dropped);
	st->tap = NULL;
	spin_unlock(&st->tap_lock);
	kfree(tap);

	stockholm_se_print(run, "result %s=%d parked=%s\n",
			   ret ? "error" : "ok", ret, err ? "no" : "yes");
}

struct stockholm_se_report {
	size_t len;
	char text[];
};

static int stockholm_se_open_common(struct inode *inode, struct file *file,
				    bool apdus)
{
	struct stockholm *st = inode->i_private;
	struct stockholm_se_report *report;
	struct stockholm_se_run *run;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	run = kzalloc_obj(*run);
	report = kvzalloc(struct_size(report, text, STOCKHOLM_SE_REPORT_SIZE),
			  GFP_KERNEL);
	if (!run || !report) {
		kfree(run);
		kvfree(report);
		return -ENOMEM;
	}
	run->st = st;
	run->text = report->text;

	mutex_lock(&st->se_lock);
	stockholm_se_run(run, apdus);
	mutex_unlock(&st->se_lock);

	report->len = run->len;
	kfree_sensitive(run);
	file->private_data = report;

	return nonseekable_open(inode, file);
}

static int stockholm_se_nfcee_open(struct inode *inode, struct file *file)
{
	return stockholm_se_open_common(inode, file, false);
}

static int stockholm_se_probe_open(struct inode *inode, struct file *file)
{
	return stockholm_se_open_common(inode, file, true);
}

static ssize_t stockholm_se_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct stockholm_se_report *report = file->private_data;

	return simple_read_from_buffer(buf, count, ppos, report->text,
				       report->len);
}

static int stockholm_se_release(struct inode *inode, struct file *file)
{
	struct stockholm_se_report *report = file->private_data;

	/* The report holds card production data: wipe it. */
	kvfree_sensitive(report,
			 struct_size(report, text, STOCKHOLM_SE_REPORT_SIZE));
	return 0;
}

static const struct file_operations stockholm_se_nfcee_fops = {
	.owner = THIS_MODULE,
	.open = stockholm_se_nfcee_open,
	.read = stockholm_se_read,
	.release = stockholm_se_release,
};

static const struct file_operations stockholm_se_probe_fops = {
	.owner = THIS_MODULE,
	.open = stockholm_se_probe_open,
	.read = stockholm_se_read,
	.release = stockholm_se_release,
};

/* The command table, one APDU per line, without touching the hardware. */
static int stockholm_se_allowlist_show(struct seq_file *s, void *unused)
{
	const struct stockholm_se_apdu *cmd;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(stockholm_se_apdus); i++) {
		cmd = &stockholm_se_apdus[i];
		seq_printf(s, "%s %*phN\n", cmd->name, cmd->len, cmd->apdu);
	}
	seq_puts(s, "get-response 00c00000xx\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stockholm_se_allowlist);

static struct dentry *stockholm_debugfs_root;

static void stockholm_se_debugfs_remove(void *data)
{
	struct stockholm *st = data;

	debugfs_remove_recursive(st->debugfs);
}

static int stockholm_se_debugfs_init(struct stockholm *st)
{
	struct device *dev = &st->sdev->dev;

	if (!se_probe || IS_ERR_OR_NULL(stockholm_debugfs_root))
		return 0;

	st->debugfs = debugfs_create_dir(dev_name(dev), stockholm_debugfs_root);
	debugfs_create_file("nfcee", 0400, st->debugfs, st,
			    &stockholm_se_nfcee_fops);
	debugfs_create_file("se_probe", 0400, st->debugfs, st,
			    &stockholm_se_probe_fops);
	debugfs_create_file("apdu_allowlist", 0400, st->debugfs, NULL,
			    &stockholm_se_allowlist_fops);
	dev_info(dev, "read-only Secure Element probe enabled in debugfs\n");

	return devm_add_action_or_reset(dev, stockholm_se_debugfs_remove, st);
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
	mutex_init(&st->se_lock);
	spin_lock_init(&st->tap_lock);
	init_waitqueue_head(&st->tap_wq);

	st->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(st->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(st->enable_gpio),
				     "failed to get VEN GPIO\n");

	/*
	 * The T8112 AOP SPMI controller has no AP interrupt, so its followers
	 * have none either. Check that the controller answers and park it.
	 */
	if (!of_property_present(sdev->ctrl->dev.of_node, "interrupts")) {
		ret = stockholm_power_on(st);
		if (ret)
			return dev_err_probe(dev, ret, "failed to raise VEN\n");
		ret = stockholm_check_follower(st);
		err = stockholm_park(st);
		if (err)
			return dev_err_probe(dev, err, "failed to park VEN low\n");
		if (!ret)
			dev_info(dev, "follower %#04x present, no interrupt path; parked off\n",
				 st->model->follower_id);
		return ret;
	}

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
	if (ret)
		return ret;

	return stockholm_se_debugfs_init(st);
}

static const struct of_device_id stockholm_of_match[] = {
	{ .compatible = "apple,stockholm-nci", .data = &stockholm_model96 },
	{ .compatible = "apple,t6000-stockholm-spmi", .data = &stockholm_model_b2 },
	{ .compatible = "apple,t8112-stockholm-spmi", .data = &stockholm_model_b2 },
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

static int __init stockholm_init(void)
{
	int ret;

	if (se_probe)
		stockholm_debugfs_root = debugfs_create_dir("apple-stockholm",
							    NULL);
	ret = spmi_driver_register(&stockholm_driver);
	if (ret)
		debugfs_remove(stockholm_debugfs_root);

	return ret;
}
module_init(stockholm_init);

static void __exit stockholm_exit(void)
{
	spmi_driver_unregister(&stockholm_driver);
	debugfs_remove(stockholm_debugfs_root);
}
module_exit(stockholm_exit);

#if IS_ENABLED(CONFIG_NFC_APPLE_STOCKHOLM_KUNIT_TEST)
#include "apple-stockholm-test.c"
#endif

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("Apple Stockholm NFC controller and Secure Element (power and identity)");
MODULE_LICENSE("GPL");
