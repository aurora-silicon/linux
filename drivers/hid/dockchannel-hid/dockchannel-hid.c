/*
 * SPDX-License-Identifier: GPL-2.0 OR MIT
 *
 * Apple DockChannel HID transport driver
 *
 * Copyright The Asahi Linux Contributors
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/hid.h>
#include <linux/mfd/macsmc.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/soc/apple/dockchannel.h>
#include <linux/soc/apple/rtkit-helper.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/of.h>
#include "../hid-ids.h"
#include "dockchannel-hid-protocol.h"

#define COMMAND_TIMEOUT_MS 1000
#define START_TIMEOUT_MS 2000

#define MAX_INTERFACES 16

/* Complete frame, including the largest aligned body and its checksum. */
#define MAX_PKT_SIZE (8 + 0xfffc + 4)
#define DCHID_RING_SIZE SZ_2M
#define DCHID_RING_HEADER_SIZE 8

#define DCHID_CHANNEL_NOTIFY 0x00
#define DCHID_CHANNEL_CMD 0x11
#define DCHID_CHANNEL_REPORT 0x12

struct dchid_hdr {
	u8 hdr_len;
	u8 channel;
	u16 length;
	u8 seq;
	u8 iface;
	u16 pad;
} __packed;

#define IFACE_COMM 0

#define FLAGS_GROUP GENMASK(7, 6)
#define FLAGS_REQ GENMASK(5, 0)

#define REQ_SET_REPORT 0
#define REQ_GET_REPORT 1

struct dchid_subhdr {
	u8 flags;
	u8 unk;
	u16 length;
	u32 retcode;
} __packed;

#define EVENT_GPIO_CMD	0xa0
#define EVENT_INIT	0xf0
#define EVENT_READY	0xf1

struct dchid_init_hdr {
	u8 type;
	u8 unk1;
	u8 unk2;
	u8 iface;
	char name[16];
	u8 more_packets;
	u8 unkpad;
} __packed;

#define INIT_HID_DESCRIPTOR	0
#define INIT_GPIO_REQUEST	1
#define INIT_TERMINATOR		2
#define INIT_PRODUCT_NAME	7

#define CMD_REGISTER_RING 0x91
#define CMD_SEND_FIRMWARE 0x95
#define CMD_ENABLE_INTERFACE 0xb4
#define CMD_ACK_GPIO_CMD 0xa1

struct dchid_init_block_hdr {
	u16 type;
	u16 length;
} __packed;

#define MAX_GPIO_NAME 32

struct dchid_gpio_request {
	u16 unk;
	u16 id;
	char name[MAX_GPIO_NAME];
} __packed;

struct dchid_gpio_cmd {
	u8 type;
	u8 iface;
	u8 gpio;
	u8 unk;
	u8 cmd;
} __packed;

struct dchid_gpio_ack {
	u8 type;
	u32 retcode;
	u8 cmd[];
} __packed;

/* Multi-touch report 0x75: byte 23 bit 0 is the firmware's button state. */
#define MT_REPORT_ID		0x75
#define MT_REPORT_BUTTON	23

#define STM_REPORT_ID		0x10
#define STM_REPORT_SERIAL	0x11
#define STM_REPORT_KEYBTYPE	0x14

struct dchid_stm_id {
	u8 unk;
	u16 vendor_id;
	u16 product_id;
	u16 version_number;
	u8 unk2;
	u8 unk3;
	u8 keyboard_type;
	u8 serial_length;
	/* Serial follows, but we grab it with a different report. */
} __packed;

#define FW_MAGIC 0x46444948
#define FW_VER 1

struct fw_header {
	u32 magic;
	u32 version;
	u32 hdr_length;
	u32 data_length;
	u32 iface_offset;
} __packed;

struct dchid_work {
	struct work_struct work;
	struct dchid_iface *iface;

	struct dchid_hdr hdr;
	u8 data[];
};

struct dchid_iface {
	struct dockchannel_hid *dchid;
	struct hid_device *hid;
	struct workqueue_struct *wq;

	bool creating;
	struct work_struct create_work;

	int index;
	const char *name;
	struct device_node *of_node;

	uint8_t tx_seq;
	bool deferred;
	bool starting;
	bool open;
	struct completion ready;

	void *hid_desc;
	size_t hid_desc_len;

	struct gpio_desc *gpio;
	char gpio_name[MAX_GPIO_NAME];
	int gpio_id;
	bool gpio_announced;

	struct mutex out_mutex;
	u32 out_flags;
	int out_report;
	u32 retcode;
	void *resp_buf;
	size_t resp_size;
	struct completion out_complete;

	u32 keyboard_layout_id;
	u32 power_method;
	bool firmware_managed_reset;
};

struct dockchannel_hid {
	struct device *dev;
	struct dockchannel *dc;
	struct device_link *helper_link;
	struct apple_smc *afe_smc;
	bool stopping;
	bool inherited_mtp;
	/* Between the system sleep suspend and resume callbacks */
	bool suspended;

	/* Held from probe on when the multi-touch interface uses power method 2 */
	struct gpio_desc *afe_reset;

	struct mutex ring_mutex;
	bool use_ring;
	bool ring_registered;
	int ring_error;
	void *ring;
	dma_addr_t ring_dma;
	u32 ring_read;
	bool id_ready;
	struct dchid_stm_id device_id;
	char serial[64];

	struct dchid_iface *comm;
	struct dchid_iface *ifaces[MAX_INTERFACES];

	u8 pkt_buf[MAX_PKT_SIZE];

	/* Workqueue to asynchronously create HID devices */
	struct workqueue_struct *new_iface_wq;
};

static ssize_t apple_layout_id_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct dchid_iface *iface = hdev->driver_data;

	return scnprintf(buf, PAGE_SIZE, "%d\n", iface->keyboard_layout_id);
}

static DEVICE_ATTR_RO(apple_layout_id);

static struct dchid_iface *
dchid_get_interface(struct dockchannel_hid *dchid, int index, const char *name)
{
	u32 power_method = DCHID_POWER_METHOD_1;
	bool firmware_managed_reset = false;
	struct device_node *of_node = NULL;
	struct dchid_iface *iface;

	if (READ_ONCE(dchid->stopping))
		return NULL;

	if (index >= MAX_INTERFACES) {
		dev_err(dchid->dev, "Interface index %d out of range\n", index);
		return NULL;
	}

	if (dchid->ifaces[index])
		return dchid->ifaces[index];

	/* Comm is not a HID subdevice */
	if (strcmp(name, "comm")) {
		of_node = of_get_child_by_name(dchid->dev->of_node, name);
		if (!of_node) {
			dev_dbg(dchid->dev, "No OF node for subdevice %s, ignoring\n", name);
			return NULL;
		}

		of_property_read_u32(of_node, "apple,power-method", &power_method);
		firmware_managed_reset =
			of_property_read_bool(of_node, "apple,firmware-managed-reset");
		if (firmware_managed_reset &&
		    (strcmp(name, "multi-touch") || power_method != DCHID_POWER_METHOD_2)) {
			dev_err(dchid->dev, "Firmware-managed reset is not supported for %s\n",
				name);
			goto err_put_node;
		}
		if (power_method != DCHID_POWER_METHOD_1 &&
		    power_method != DCHID_POWER_METHOD_2) {
			dev_err(dchid->dev, "Unsupported power method %u for %s\n",
				power_method, name);
			goto err_put_node;
		}
		if (power_method == DCHID_POWER_METHOD_2 &&
		    !firmware_managed_reset && !dchid->afe_reset) {
			dev_err(dchid->dev, "Power method 2 for %s needs the AFE reset line\n",
				name);
			goto err_put_node;
		}
	}

	iface = devm_kzalloc(dchid->dev, sizeof(struct dchid_iface), GFP_KERNEL);
	if (!iface)
		goto err_put_node;

	iface->index = index;
	iface->name = devm_kstrdup(dchid->dev, name, GFP_KERNEL);
	if (!iface->name)
		goto err_put_node;
	iface->dchid = dchid;
	iface->of_node = of_node;
	iface->power_method = power_method;
	iface->firmware_managed_reset = firmware_managed_reset;
	iface->out_report = -1;
	init_completion(&iface->out_complete);
	init_completion(&iface->ready);
	mutex_init(&iface->out_mutex);
	iface->wq = alloc_ordered_workqueue("dchid-%s", WQ_MEM_RECLAIM, iface->name);
	if (!iface->wq)
		goto err_put_node;

	dchid->ifaces[index] = iface;
	return iface;

err_put_node:
	of_node_put(of_node);
	return NULL;
}

static u32 dchid_checksum(void *p, size_t length)
{
	u32 sum = 0;

	while (length >= 4) {
		sum += get_unaligned_le32(p);
		p += 4;
		length -= 4;
	}

	WARN_ON_ONCE(length);
	return sum;
}

static bool dchid_verify_checksum(struct dchid_hdr *hdr, void *data, u32 *checksum)
{
	if (!IS_ALIGNED(hdr->length, sizeof(u32))) {
		*checksum = 0;
		return false;
	}

	*checksum = dchid_checksum(hdr, sizeof(*hdr));
	*checksum += dchid_checksum(data, hdr->length + sizeof(u32));
	return *checksum == U32_MAX;
}

static int dchid_send(struct dchid_iface *iface, u32 flags, void *msg, size_t size)
{
	u32 checksum = 0xffffffff;
	size_t wsize = round_down(size, 4);
	size_t tsize = size - wsize;
	int ret;
	struct {
		struct dchid_hdr hdr;
		struct dchid_subhdr sub;
	} __packed h;

	/* Both length fields are 16 bits wide. */
	if (size > U16_MAX - sizeof(h.sub) - 3)
		return -EMSGSIZE;

	memset(&h, 0, sizeof(h));
	h.hdr.hdr_len = sizeof(h.hdr);
	h.hdr.channel = DCHID_CHANNEL_CMD;
	h.hdr.length = round_up(size, 4) + sizeof(h.sub);
	h.hdr.seq = iface->tx_seq;
	h.hdr.iface = iface->index;
	h.sub.flags = flags;
	h.sub.length = size;

	ret = dockchannel_send(iface->dchid->dc, &h, sizeof(h));
	if (ret < 0)
		return ret;
	checksum -= dchid_checksum(&h, sizeof(h));

	ret = dockchannel_send(iface->dchid->dc, msg, wsize);
	if (ret < 0)
		return ret;
	checksum -= dchid_checksum(msg, wsize);

	if (tsize) {
		u8 tail[4] = {0, 0, 0, 0};

		memcpy(tail, msg + wsize, tsize);
		ret = dockchannel_send(iface->dchid->dc, tail, sizeof(tail));
		if (ret < 0)
			return ret;
		checksum -= dchid_checksum(tail, sizeof(tail));
	}

	ret = dockchannel_send(iface->dchid->dc, &checksum, sizeof(checksum));
	if (ret < 0)
		return ret;

	return 0;
}

static int dchid_cmd(struct dchid_iface *iface, u32 type, u32 req,
		     void *data, size_t size, void *resp_buf, size_t resp_size)
{
	int ret;
	int report_id = *(u8*)data;

	mutex_lock(&iface->out_mutex);

	WARN_ON(iface->out_report != -1);
	iface->out_report = report_id;
	iface->out_flags = FIELD_PREP(FLAGS_GROUP, type) | FIELD_PREP(FLAGS_REQ, req);
	iface->resp_buf = resp_buf;
	iface->resp_size = resp_size;
	reinit_completion(&iface->out_complete);

	ret = dchid_send(iface, iface->out_flags, data, size);
	if (ret < 0)
		goto done;

	if (!wait_for_completion_timeout(&iface->out_complete, msecs_to_jiffies(COMMAND_TIMEOUT_MS))) {
		dev_err(iface->dchid->dev, "output report 0x%x to iface  %d (%s) timed out\n",
			report_id, iface->index, iface->name);
		ret = -ETIMEDOUT;
		goto done;
	}

	ret = iface->resp_size;
	if (iface->retcode) {
		dev_err(iface->dchid->dev,
			"output report 0x%x to iface %d (%s) failed with err 0x%x\n",
			report_id, iface->index, iface->name, iface->retcode);
		ret = -EIO;
	}

done:
	iface->tx_seq++;
	iface->out_report = -1;
	iface->out_flags = 0;
	iface->resp_buf = NULL;
	iface->resp_size = 0;
	mutex_unlock(&iface->out_mutex);
	return ret;
}

static int dchid_comm_cmd(struct dockchannel_hid *dchid, void *cmd, size_t size)
{
	return dchid_cmd(dchid->comm, HID_FEATURE_REPORT, REQ_SET_REPORT, cmd, size, NULL, 0);
}

static int dchid_enable_interface(struct dchid_iface *iface)
{
	u8 msg[] = { CMD_ENABLE_INTERFACE, iface->index };

	return dchid_comm_cmd(iface->dchid, msg, sizeof(msg));
}

static int dchid_reset_interface(struct dchid_iface *iface, int state)
{
	u8 msg[DCHID_PM1_CMD_SIZE];
	u8 msg2[DCHID_PM2_CMD_SIZE];
	int ret;

	if (!iface->dchid->use_ring) {
		dchid_pm1_command(iface->index, state, msg);
		return dchid_comm_cmd(iface->dchid, msg, sizeof(msg));
	}

	/*
	 * T8132 boot capture: state 0 then 2, each with byte 4 equal to 0
	 * then 1. This is only the observed provisioning sequence, not a
	 * contract for suspend, wake, or arbitrary interface power changes.
	 */
	dchid_pm2_command(iface->index, state, false, msg2);
	ret = dchid_comm_cmd(iface->dchid, msg2, sizeof(msg2));
	if (ret < 0)
		return ret;

#if IS_REACHABLE(CONFIG_MFD_MACSMC)
	if (iface->dchid->afe_smc && iface->index == 1) {
		/*
		 * J616s macOS power sequence: gp1c high + 50 ms when turning
		 * off, low with no added delay when turning on. These are
		 * pKW4 function payloads, not gpio-macsmc CMD_OUTPUT values.
		 * The host operation sits between WillChange and HasChanged.
		 */
		__le32 value = cpu_to_le32(0x00010000 | (state == 0));

		ret = apple_smc_write(iface->dchid->afe_smc, SMC_KEY(gp1c),
				      &value, sizeof(value));
		if (ret < 0) {
			dev_err(iface->dchid->dev,
				"AFE power sequence failed: %d; reboot required\n", ret);
			return ret;
		}
		if (state == 0)
			msleep(50);
	}
#endif
	dchid_pm2_command(iface->index, state, true, msg2);
	return dchid_comm_cmd(iface->dchid, msg2, sizeof(msg2));
}

static int dchid_register_ring(struct dockchannel_hid *dchid)
{
	struct {
		u8 cmd;
		u8 reserved[2];
		u64 addr;
		u32 size;
	} __packed msg = {
		.cmd = CMD_REGISTER_RING,
		.addr = dchid->ring_dma,
		.size = DCHID_RING_SIZE,
	};
	/* Observed in both macOS and standalone T8132 bringup before 0x91. */
	u8 prepare[] = { 0xc1, 0x02 };
	int ret = 0;

	if (!dchid->use_ring)
		return 0;

	mutex_lock(&dchid->ring_mutex);
	ret = dchid->ring_error;
	if (!dchid->ring_registered) {
		ret = dchid_comm_cmd(dchid, prepare, sizeof(prepare));
		if (ret < 0) {
			mutex_unlock(&dchid->ring_mutex);
			return ret;
		}
		/* The registration ACK itself arrives through the ring. */
		dma_wmb();
		WRITE_ONCE(dchid->ring_registered, true);
		ret = dchid_comm_cmd(dchid, &msg, sizeof(msg));
		dchid->ring_error = ret;
	}
	mutex_unlock(&dchid->ring_mutex);
	return ret;
}

/*
 * Power method 2: announce the transition, drive the AFE reset line to the
 * new state and confirm the transition. The reset is asserted for a power-off
 * and held for 50 ms, and released for a power-on.
 */
static int dchid_pm2_set_power(struct dchid_iface *iface, u8 state)
{
	struct dockchannel_hid *dchid = iface->dchid;
	u8 msg[DCHID_PM2_CMD_SIZE];
	int ret;

	dchid_pm2_command(iface->index, state, false, msg);
	ret = dchid_comm_cmd(dchid, msg, sizeof(msg));
	if (ret < 0)
		return ret;

	if (iface->firmware_managed_reset) {
		/* Firmware owns the reset; allow its power-on transition to settle. */
		if (state == DCHID_POWER_STATE_ON)
			fsleep(50 * USEC_PER_MSEC);
	} else {
		ret = gpiod_set_value_cansleep(dchid->afe_reset,
					       state == DCHID_POWER_STATE_OFF);
		if (ret < 0)
			return ret;
		if (state == DCHID_POWER_STATE_OFF)
			fsleep(50 * USEC_PER_MSEC);
	}

	dchid_pm2_command(iface->index, state, true, msg);
	return dchid_comm_cmd(dchid, msg, sizeof(msg));
}

static int dchid_send_firmware(struct dchid_iface *iface, void *firmware, size_t size)
{
	struct {
		u8 cmd;
		u8 unk1;
		u8 unk2;
		u8 iface;
		u64 addr;
		u32 size;
	} __packed msg = {
		.cmd = CMD_SEND_FIRMWARE,
		.unk1 = 2,
		.unk2 = 0,
		.iface = iface->index,
		.size = size,
	};
	dma_addr_t addr;
	void *buf = dmam_alloc_coherent(iface->dchid->dev, size, &addr, GFP_KERNEL);

	if (IS_ERR_OR_NULL(buf))
		return buf ? PTR_ERR(buf) : -ENOMEM;

	msg.addr = addr;
	memcpy(buf, firmware, size);
	wmb();

	return dchid_comm_cmd(iface->dchid, &msg, sizeof(msg));
}

static int dchid_get_firmware(struct dchid_iface *iface, void **firmware, size_t *size)
{
	int ret;
	const char *fw_name;
	const struct firmware *fw;
	struct fw_header *hdr;
	u8 *fw_data;

	ret = of_property_read_string(iface->of_node, "firmware-name", &fw_name);
	if (ret) {
		/* Firmware is only for some devices */
		*firmware = NULL;
		*size = 0;
		return 0;
	}

	ret = request_firmware(&fw, fw_name, iface->dchid->dev);
	if (ret)
		return ret;

	if (fw->size < sizeof(*hdr)) {
		dev_warn(iface->dchid->dev, "%s: firmware too short for its header\n",
			 fw_name);
		ret = -EINVAL;
		goto done;
	}

	hdr = (struct fw_header *)fw->data;

	if (hdr->magic != FW_MAGIC || hdr->version != FW_VER ||
		hdr->hdr_length < sizeof(*hdr) || hdr->hdr_length > fw->size ||
		(hdr->hdr_length + (size_t)hdr->data_length) > fw->size ||
		hdr->iface_offset >= hdr->data_length) {
		dev_warn(iface->dchid->dev, "%s: invalid firmware header\n",
			 fw_name);
		ret = -EINVAL;
		goto done;
	}

	fw_data = devm_kmemdup(iface->dchid->dev, fw->data + hdr->hdr_length,
			       hdr->data_length, GFP_KERNEL);
	if (!fw_data) {
		ret = -ENOMEM;
		goto done;
	}

	if (hdr->iface_offset)
		fw_data[hdr->iface_offset] = iface->index;

	*firmware = fw_data;
	*size = hdr->data_length;

done:
	release_firmware(fw);
	return ret;
}

static int dchid_request_gpio(struct dchid_iface *iface)
{
	char prop_name[MAX_GPIO_NAME + 16];
	int ret;

	if (iface->firmware_managed_reset)
		return -EINVAL;

	if (iface->dchid->use_ring) {
		dev_err_once(iface->dchid->dev, "T8132 SMC reset GPIO operation is not established\n");
		return -EOPNOTSUPP;
	}

	if (iface->gpio)
		return 0;

	dev_dbg(iface->dchid->dev, "Requesting GPIO %s#%d: %s\n",
		iface->name, iface->gpio_id, iface->gpio_name);

	/* A power method 2 interface holds its reset line from probe on. */
	if (iface->power_method == DCHID_POWER_METHOD_2 &&
	    !strcmp(iface->gpio_name, "afe-reset")) {
		iface->gpio = iface->dchid->afe_reset;
		return 0;
	}

	snprintf(prop_name, sizeof(prop_name), "apple,%s", iface->gpio_name);

	iface->gpio = devm_gpiod_get_index(iface->dchid->dev, prop_name, 0, GPIOD_OUT_LOW);

	if (IS_ERR(iface->gpio)) {
		ret = PTR_ERR(iface->gpio);
		iface->gpio = NULL;
		dev_err(iface->dchid->dev, "Failed to request GPIO %s-gpios: %d\n",
			prop_name, ret);
		return ret;
	}

	return 0;
}

static int dchid_start_interface(struct dchid_iface *iface)
{
	void *fw;
	size_t size;
	int ret;

	if (iface->starting) {
		dev_warn(iface->dchid->dev, "Interface %s is already starting\n", iface->name);
		return -EINPROGRESS;
	}

	dev_dbg(iface->dchid->dev, "Starting interface %s\n", iface->name);

	iface->starting = true;
	ret = dchid_register_ring(iface->dchid);
	if (ret < 0)
		goto err;

	/* Look to see if we need firmware */
	ret = dchid_get_firmware(iface, &fw, &size);
	if (ret < 0)
		goto err;

	/* T8132 startup does not use the older host GPIO reset. */
	if (!iface->dchid->use_ring && iface->gpio_announced) {
		ret = dchid_request_gpio(iface);
		if (ret < 0)
			goto err;
	}

	/* Only multi-touch has firmware */
	if (fw && size) {

		/* Send firmware to the device */
		dev_dbg(iface->dchid->dev, "Sending firmware for %s\n", iface->name);
		/* Retained MTP must see AFE reset throughout firmware upload. */
		if (iface->dchid->inherited_mtp &&
		    iface->power_method == DCHID_POWER_METHOD_2) {
			if (iface->firmware_managed_reset) {
				ret = -EINVAL;
				goto err;
			}
			ret = gpiod_set_value_cansleep(iface->dchid->afe_reset, 1);
			if (ret < 0)
				goto err;
			fsleep(50 * USEC_PER_MSEC);
		}
		ret = dchid_send_firmware(iface, fw, size);
		if (ret < 0) {
			dev_err(iface->dchid->dev, "Failed to send %s firmware\n", iface->name);
			goto err;
		}

		/* After loading firmware, multi-touch needs a reset */
		dev_dbg(iface->dchid->dev, "Resetting %s\n", iface->name);
		if (iface->power_method == DCHID_POWER_METHOD_2) {
			/* The retained session is already off while reset is asserted. */
			if (!iface->dchid->inherited_mtp) {
				ret = dchid_pm2_set_power(iface, DCHID_POWER_STATE_OFF);
				if (ret < 0)
					goto err;
			}
			ret = dchid_pm2_set_power(iface, DCHID_POWER_STATE_ON);
			if (ret < 0)
				goto err;
		} else {
			ret = dchid_reset_interface(iface, DCHID_POWER_STATE_OFF);
			/* After a failed AFE power-off the trackpad stays off: no retry. */
			if (ret >= 0 || !iface->dchid->afe_smc || iface->index != 1)
				dchid_reset_interface(iface, DCHID_POWER_STATE_ON);
		}
	}

	return 0;

err:
	iface->starting = false;
	return ret;
}

static int dchid_start(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	if (iface->keyboard_layout_id) {
		int ret = device_create_file(&hdev->dev, &dev_attr_apple_layout_id);
		if (ret) {
			dev_warn(iface->dchid->dev, "Failed to create apple_layout_id: %d\n", ret);
			iface->keyboard_layout_id = 0;
		}
	}

	return 0;
};

static void dchid_stop(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	if (iface->keyboard_layout_id)
		device_remove_file(&hdev->dev, &dev_attr_apple_layout_id);
}

static int dchid_open(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;
	int ret;

	if (READ_ONCE(iface->dchid->stopping))
		return -ESHUTDOWN;

	if (!completion_done(&iface->ready)) {
		ret = dchid_start_interface(iface);
		if (ret < 0)
			return ret;

		if (!wait_for_completion_timeout(&iface->ready, msecs_to_jiffies(START_TIMEOUT_MS))) {
			dev_err(iface->dchid->dev, "iface %s start timed out\n", iface->name);
			return -ETIMEDOUT;
		}
	}

	/* Pairs with the acquire in dchid_handle_report(); publishes iface->hid. */
	smp_store_release(&iface->open, true);
	return 0;
}

static void dchid_close(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	WRITE_ONCE(iface->open, false);
}

static int dchid_parse(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	return hid_parse_report(hdev, iface->hid_desc, iface->hid_desc_len);
}

/* Note: buf excludes report number! For ease of fetching strings/etc. */
static int dchid_get_report_cmd(struct dchid_iface *iface, u8 reportnum, void *buf, size_t len)
{
	int ret = dchid_cmd(iface, HID_FEATURE_REPORT, REQ_GET_REPORT, &reportnum, 1, buf, len);

	return ret <= 0 ? ret : ret - 1;
}

static int dchid_raw_request(struct hid_device *hdev,
				unsigned char reportnum, __u8 *buf, size_t len,
				unsigned char rtype, int reqtype)
{
	struct dchid_iface *iface = hdev->driver_data;

	if (READ_ONCE(iface->dchid->stopping))
		return -ESHUTDOWN;
	/* Fail fast: an answer that arrives in suspend-to-idle is not waited for. */
	if (READ_ONCE(iface->dchid->suspended))
		return -EBUSY;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		buf[0] = reportnum;
		return dchid_cmd(iface, rtype, REQ_GET_REPORT, &reportnum, 1, buf + 1, len - 1);
	case HID_REQ_SET_REPORT:
		return dchid_cmd(iface, rtype, REQ_SET_REPORT, buf, len, NULL, 0);
	default:
		return -EIO;
	}

	return 0;
}

static struct hid_ll_driver dchid_ll = {
	.start = &dchid_start,
	.stop = &dchid_stop,
	.open = &dchid_open,
	.close = &dchid_close,
	.parse = &dchid_parse,
	.raw_request = &dchid_raw_request,
};

static void dchid_create_interface_work(struct work_struct *ws)
{
	struct dchid_iface *iface = container_of(ws, struct dchid_iface, create_work);
	struct dockchannel_hid *dchid = iface->dchid;
	struct hid_device *hid;
	int ret;

	if (READ_ONCE(dchid->stopping))
		return;

	if (iface->hid) {
		dev_warn(dchid->dev, "Interface %s already created!\n",
			 iface->name);
		return;
	}

	dev_dbg(dchid->dev, "New interface %s\n", iface->name);

	/* Start the interface. This is not the entire init process, as firmware is loaded later on device open. */
	ret = dchid_enable_interface(iface);
	if (ret < 0) {
		dev_warn(dchid->dev, "Failed to enable %s: %d\n", iface->name, ret);
		return;
	}

	iface->deferred = false;

	hid = hid_allocate_device();
	if (IS_ERR(hid))
		return;

	snprintf(hid->name, sizeof(hid->name), "Apple MTP %s", iface->name);
	snprintf(hid->phys, sizeof(hid->phys), "%s.%d (%s)",
		 dev_name(dchid->dev), iface->index, iface->name);
	strscpy(hid->uniq, dchid->serial, sizeof(hid->uniq));

	hid->ll_driver = &dchid_ll;
	hid->bus = BUS_HOST;
	hid->vendor = dchid->device_id.vendor_id;
	hid->product = dchid->device_id.product_id;
	hid->version = dchid->device_id.version_number;
	hid->type = HID_TYPE_OTHER;
	if (!strcmp(iface->name, "multi-touch")) {
		hid->type = HID_TYPE_SPI_MOUSE;
	} else if (!strcmp(iface->name, "keyboard")) {
		u32 country_code = 0;

		hid->type = HID_TYPE_SPI_KEYBOARD;

		/*
		 * We have to get the country code from the device tree, since the
		 * device provides no reliable way to get this info.
		 */
		if (!of_property_read_u32(iface->of_node, "hid-country-code", &country_code))
			hid->country = country_code;

		of_property_read_u32(iface->of_node, "apple,keyboard-layout-id",
			&iface->keyboard_layout_id);
	}

	hid->dev.parent = iface->dchid->dev;
	hid->driver_data = iface;

	iface->hid = hid;

	ret = hid_add_device(hid);
	if (ret < 0) {
		iface->hid = NULL;
		hid_destroy_device(hid);
		dev_warn(iface->dchid->dev, "Failed to register hid device %s\n", iface->name);
	}
}

static int dchid_create_interface(struct dchid_iface *iface)
{
	if (READ_ONCE(iface->dchid->stopping))
		return -ESHUTDOWN;
	if (iface->creating)
		return -EBUSY;

	iface->creating = true;
	INIT_WORK(&iface->create_work, dchid_create_interface_work);
	return queue_work(iface->dchid->new_iface_wq, &iface->create_work);
}

static void dchid_handle_descriptor(struct dchid_iface *iface, void *hid_desc, size_t desc_len)
{
	if (iface->hid) {
		dev_warn(iface->dchid->dev, "Tried to initialize already started interface %s!\n",
			 iface->name);
		return;
	}

	iface->hid_desc = devm_kmemdup(iface->dchid->dev, hid_desc, desc_len, GFP_KERNEL);
	if (!iface->hid_desc)
		return;

	iface->hid_desc_len = desc_len;
}

static void dchid_handle_ready(struct dockchannel_hid *dchid, void *data, size_t length)
{
	struct dchid_iface *iface;
	u8 *pkt = data;
	u8 index;
	int i, ret;

	if (length < 2) {
		dev_err(dchid->dev, "Bad length for ready message: %zu\n", length);
		return;
	}

	index = pkt[1];

	if (index >= MAX_INTERFACES) {
		dev_err(dchid->dev, "Got ready notification for bad iface %d\n", index);
		return;
	}

	iface = dchid->ifaces[index];
	if (!iface) {
		dev_err(dchid->dev, "Got ready notification for unknown iface %d\n", index);
		return;
	}

	dev_info(dchid->dev, "Interface %s is now ready\n", iface->name);
	complete_all(&iface->ready);

	/* When STM is ready, grab global device info */
	if (!strcmp(iface->name, "stm")) {
		ret = dchid_get_report_cmd(iface, STM_REPORT_ID, &dchid->device_id,
					   sizeof(dchid->device_id));
		if (ret < sizeof(dchid->device_id)) {
			dev_warn(iface->dchid->dev, "Failed to get device ID from STM!\n");
			/* Fake it and keep going. Things might still work... */
			memset(&dchid->device_id, 0, sizeof(dchid->device_id));
			dchid->device_id.vendor_id = HOST_VENDOR_ID_APPLE;
		}
		ret = dchid_get_report_cmd(iface, STM_REPORT_SERIAL, dchid->serial,
					   sizeof(dchid->serial) - 1);
		if (ret < 0) {
			dev_warn(iface->dchid->dev, "Failed to get serial from STM!\n");
			dchid->serial[0] = 0;
		}

		dchid->id_ready = true;
		for (i = 0; i < MAX_INTERFACES; i++) {
			if (!dchid->ifaces[i] || !dchid->ifaces[i]->deferred)
				continue;
			dchid_create_interface(dchid->ifaces[i]);
		}
	}
}

static void dchid_handle_init(struct dockchannel_hid *dchid, void *data, size_t length)
{
	struct dchid_init_hdr *hdr = data;
	struct dchid_iface *iface;
	struct dchid_init_block_hdr *blk;
	char name[sizeof(hdr->name) + 1];

	if (length < sizeof(*hdr))
		return;

	/* The name field is not necessarily NUL-terminated. */
	memcpy(name, hdr->name, sizeof(hdr->name));
	name[sizeof(hdr->name)] = '\0';
	iface = dchid_get_interface(dchid, hdr->iface, name);
	if (!iface)
		return;

	data += sizeof(*hdr);
	length -= sizeof(*hdr);

	while (length >= sizeof(*blk)) {
		blk = data;
		data += sizeof(*blk);
		length -= sizeof(*blk);

		if (blk->length > length)
			break;

		switch (blk->type) {
		case INIT_HID_DESCRIPTOR:
			dchid_handle_descriptor(iface, data, blk->length);
			break;

		case INIT_GPIO_REQUEST: {
			struct dchid_gpio_request *req = data;

			if (blk->length < sizeof(*req)) {
				dev_warn(dchid->dev, "Short GPIO request for %s\n",
					 iface->name);
				break;
			}

			if (iface->gpio_announced) {
				dev_err(dchid->dev,
					"Cannot request more than one GPIO per interface!\n");
				break;
			}

			strscpy(iface->gpio_name, req->name, MAX_GPIO_NAME);
			iface->gpio_id = req->id;
			iface->gpio_announced = true;
			break;
		}

		case INIT_TERMINATOR:
			break;

		case INIT_PRODUCT_NAME: {
			char *product = data;

			if (!blk->length || product[blk->length - 1] != 0) {
				dev_warn(dchid->dev, "Unterminated product name for %s\n",
					 iface->name);
			} else {
				dev_dbg(dchid->dev, "Product name for %s: %s\n",
					iface->name, product);
			}
			break;
		}

		default:
			dev_warn(dchid->dev, "Unknown init packet %d for %s\n",
				 blk->type, iface->name);
			break;
		}

		data += blk->length;
		length -= blk->length;

		if (blk->type == INIT_TERMINATOR)
			break;
	}

	if (hdr->more_packets || (dchid->use_ring && !iface->hid_desc))
		return;

	/* We need to enable STM first, since it'll give us the device IDs */
	if (iface->dchid->id_ready || !strcmp(iface->name, "stm")) {
		dchid_create_interface(iface);
	} else {
		iface->deferred = true;
	}
}

static void dchid_handle_gpio(struct dockchannel_hid *dchid, void *data, size_t length)
{
	struct dchid_gpio_cmd *cmd = data;
	struct dchid_iface *iface;
	u32 retcode = 0xe000f00d; /* Give it a random Apple-style error code */
	struct dchid_gpio_ack *ack;

	if (length < sizeof(*cmd))
		return;

	if (cmd->iface >= MAX_INTERFACES || !(iface = dchid->ifaces[cmd->iface])) {
		dev_err(dchid->dev, "Got GPIO command for bad inteface %d\n", cmd->iface);
		goto err;
	}

	if (!iface->gpio_announced) {
		dev_err(dchid->dev, "Got GPIO command for %s, which announced no GPIO\n",
			iface->name);
		goto err;
	}

	if (dchid_request_gpio(iface) < 0)
		goto err;

	if (!iface->gpio || cmd->gpio != iface->gpio_id) {
		dev_err(dchid->dev, "Got GPIO command for bad GPIO %s#%d\n",
			iface->name, cmd->gpio);
		goto err;
	}

	dev_dbg(dchid->dev, "GPIO command: %s#%d: %d\n", iface->name, cmd->gpio, cmd->cmd);

	switch (cmd->cmd) {
	case 3:
		/* Pulse.  */
		gpiod_set_value_cansleep(iface->gpio, 1);
		msleep(10); /* Random guess... */
		gpiod_set_value_cansleep(iface->gpio, 0);
		retcode = 0;
		break;
	default:
		dev_err(dchid->dev, "Unknown GPIO command %d\n", cmd->cmd	);
		break;
	}

err:
	/* Ack it */
	ack = kzalloc(sizeof(*ack) + length, GFP_KERNEL);
	if (!ack)
		return;

	ack->type = CMD_ACK_GPIO_CMD;
	ack->retcode = retcode;
	memcpy(ack->cmd, data, length);

	if (dchid_comm_cmd(dchid, ack, sizeof(*ack) + length) < 0)
		dev_err(dchid->dev, "Failed to ACK GPIO command\n");

	kfree(ack);
}

static void dchid_handle_event(struct dockchannel_hid *dchid, void *data, size_t length)
{
	u8 *p = data;

	if (!length)
		return;

	switch (*p) {
	case EVENT_INIT:
		dchid_handle_init(dchid, data, length);
		break;
	case EVENT_READY:
		dchid_handle_ready(dchid, data, length);
		break;
	case EVENT_GPIO_CMD:
		dchid_handle_gpio(dchid, data, length);
		break;
	}
}

static void dchid_handle_report(struct dchid_iface *iface, void *data, size_t length)
{
	/*
	 * An interface starts reporting as soon as it is enabled, which is
	 * before its HID device has been registered and opened. Until then
	 * there is nobody to deliver the reports to.
	 */
	if (!smp_load_acquire(&iface->open))
		return;

	hid_input_report(iface->hid, HID_INPUT_REPORT, data, length, 1);
}

static void dchid_packet_work(struct work_struct *ws)
{
	struct dchid_work *work = container_of(ws, struct dchid_work, work);
	struct dchid_subhdr *shdr = (void *)work->data;
	struct dockchannel_hid *dchid = work->iface->dchid;
	u8 *payload = work->data + sizeof(*shdr);
	int type;

	if (READ_ONCE(dchid->stopping))
		goto out;

	if (work->hdr.length < sizeof(*shdr)) {
		dev_err(dchid->dev, "Short packet (%u bytes) for iface %d\n",
			work->hdr.length, work->hdr.iface);
		goto out;
	}

	if (shdr->length + sizeof(*shdr) > work->hdr.length) {
		dev_err(dchid->dev, "Bad sub header length (%hu > %zu)\n",
			shdr->length, work->hdr.length - sizeof(*shdr));
		goto out;
	}

	type = FIELD_GET(FLAGS_GROUP, shdr->flags);

	switch (type) {
	case HID_INPUT_REPORT:
		if (work->hdr.iface == IFACE_COMM)
			dchid_handle_event(dchid, payload, shdr->length);
		else
			dchid_handle_report(work->iface, payload, shdr->length);
		break;
	default:
		dev_err(dchid->dev, "Received unknown packet type %d\n", type);
		break;
	}

out:
	kfree(work);
}

static void dchid_handle_ack(struct dchid_iface *iface, struct dchid_hdr *hdr, void *data)
{
	struct dchid_subhdr *shdr = (void *)data;
	u8 *payload = data + sizeof(*shdr);

	if (hdr->length < sizeof(*shdr)) {
		dev_err(iface->dchid->dev, "Short ACK packet (%u bytes)\n",
			hdr->length);
		return;
	}
	if (shdr->length + sizeof(*shdr) > hdr->length) {
		dev_err(iface->dchid->dev, "Bad sub header length (%hu > %zu)\n",
			shdr->length, hdr->length - sizeof(*shdr));
		return;
	}
	if (shdr->flags != iface->out_flags) {
		dev_err(iface->dchid->dev,
			"Received unexpected flags 0x%x on ACK channel (expected 0x%x)\n",
			shdr->flags, iface->out_flags);
		return;
	}

	if (shdr->length < 1) {
		dev_err(iface->dchid->dev, "Received length 0 output report ack\n");
		return;
	}
	if (iface->tx_seq != hdr->seq) {
		dev_err(iface->dchid->dev, "Received ACK with bad seq (expected %d, got %d)\n",
			iface->tx_seq, hdr->seq);
		return;
	}
	if (iface->out_report != payload[0]) {
		dev_err(iface->dchid->dev, "Received ACK with bad report (expected %d, got %d\n",
			iface->out_report, payload[0]);
		return;
	}

	/*
	 * Report the response length as the bytes actually delivered, so that
	 * a caller with a small buffer is not told about bytes that were never
	 * copied. The report ID byte counts as delivered.
	 */
	if (iface->resp_buf) {
		size_t copied = min((size_t)shdr->length - 1, iface->resp_size);

		if (copied)
			memcpy(iface->resp_buf, payload + 1, copied);
		iface->resp_size = copied + 1;
	} else {
		iface->resp_size = shdr->length;
	}

	iface->out_report = -1;
	iface->retcode = shdr->retcode;
	complete(&iface->out_complete);
}

/*
 * Whether a report should wake the system: any keyboard report, and a
 * multi-touch report only while the trackpad is clicked. Touches, the
 * accelerometer and the other interfaces do not wake it.
 */
static bool dchid_wake_report(struct dchid_iface *iface, void *data)
{
	struct dchid_subhdr *shdr = data;
	u8 *payload = data + sizeof(*shdr);

	if (FIELD_GET(FLAGS_GROUP, shdr->flags) != HID_INPUT_REPORT)
		return false;
	if (!strcmp(iface->name, "keyboard"))
		return shdr->length > 0;
	return !strcmp(iface->name, "multi-touch") &&
	       shdr->length > MT_REPORT_BUTTON && payload[0] == MT_REPORT_ID &&
	       (payload[MT_REPORT_BUTTON] & 1);
}

/* Return the complete frame size, or reject a lost framing boundary. */
static int dchid_frame_size(const struct dchid_hdr *hdr)
{
	size_t length = hdr->length;

	if (hdr->hdr_len != sizeof(*hdr) || !IS_ALIGNED(length, 4))
		return -EPROTO;
	return sizeof(*hdr) + length + sizeof(u32);
}

/*
 * Both receive transports pass complete frames here. Header bytes 6/7 and
 * application padding are opaque, checksum-covered data, not required zeroes.
 */
static int dchid_dispatch(struct dockchannel_hid *dchid, struct dchid_hdr *hdr,
			  void *data, bool fifo)
{
	struct dchid_iface *iface;
	struct dchid_work *work;
	u32 checksum;

	if (!dchid_verify_checksum(hdr, data, &checksum)) {
		dev_err(dchid->dev, "Checksum mismatch (iface %d): 0x%08x != 0xffffffff\n",
			hdr->iface, checksum);
		return 0;
	}

	if (dchid->use_ring && hdr->channel == DCHID_CHANNEL_NOTIFY) {
		/* A FIFO notification has no application subheader or report ID. */
		return fifo && !hdr->length && hdr->iface == IFACE_COMM;
	}

	if (hdr->iface >= MAX_INTERFACES || hdr->length < sizeof(struct dchid_subhdr))
		return 0;
	iface = dchid->ifaces[hdr->iface];
	if (!iface) {
		dev_err(dchid->dev, "Received packet for uninitialized iface %d\n", hdr->iface);
		return 0;
	}

	switch (hdr->channel) {
	case DCHID_CHANNEL_CMD:
		dchid_handle_ack(iface, hdr, data);
		return 0;
	case DCHID_CHANNEL_REPORT:
		/* pm_wakeup_hard_event() does nothing while wakeup is disabled. */
		if (READ_ONCE(dchid->suspended) &&
		    hdr->length >= sizeof(struct dchid_subhdr) +
				   ((struct dchid_subhdr *)data)->length &&
		    dchid_wake_report(iface, data))
			pm_wakeup_hard_event(dchid->dev);
		break;
	default:
		dev_warn(dchid->dev, "Unknown channel 0x%x, treating as report...\n",
			 hdr->channel);
		break;
	}

	work = kzalloc(sizeof(*work) + hdr->length, GFP_KERNEL);
	if (!work)
		return 0;
	work->hdr = *hdr;
	work->iface = iface;
	memcpy(work->data, data, hdr->length);
	INIT_WORK(&work->work, dchid_packet_work);
	queue_work(iface->wq, &work->work);
	return 0;
}

static void dchid_ring_copy(struct dockchannel_hid *dchid, void *dst,
			    u32 offset, size_t length)
{
	size_t first = min_t(size_t, length,
			     DCHID_RING_SIZE - DCHID_RING_HEADER_SIZE - offset);
	void *data = dchid->ring + DCHID_RING_HEADER_SIZE;

	memcpy(dst, data + offset, first);
	memcpy(dst + first, data, length - first);
}

static int dchid_drain_ring(struct dockchannel_hid *dchid)
{
	const u32 capacity = DCHID_RING_SIZE - DCHID_RING_HEADER_SIZE;
	u32 *indices = dchid->ring;
	u32 producer, consumer, available;
	int size;

	if (!READ_ONCE(dchid->ring_registered)) {
		/* The firmware can notify before it has the ring address: nothing to read. */
		dev_dbg(dchid->dev, "Ring notification before registration\n");
		return 0;
	}

	while (1) {
		producer = READ_ONCE(indices[0]);
		consumer = READ_ONCE(indices[1]);
		if (producer >= capacity || consumer != dchid->ring_read ||
		    !IS_ALIGNED(producer, 4))
			return -EPROTO;
		/* Read only the bytes published by this producer snapshot. */
		dma_rmb();
		available = producer >= consumer ? producer - consumer :
			    capacity - consumer + producer;
		if (available < sizeof(struct dchid_hdr))
			return 0;
		dchid_ring_copy(dchid, dchid->pkt_buf, consumer,
				sizeof(struct dchid_hdr));
		size = dchid_frame_size((void *)dchid->pkt_buf);
		if (size < 0)
			return size;
		if (available < size)
			return 0;
		dchid_ring_copy(dchid, dchid->pkt_buf, consumer, size);
		dchid_dispatch(dchid, (void *)dchid->pkt_buf,
			       dchid->pkt_buf + sizeof(struct dchid_hdr), false);
		consumer += size;
		if (consumer >= capacity)
			consumer -= capacity;
		dchid->ring_read = consumer;
		/* All frame reads must complete before the producer can reuse it. */
		mb();
		WRITE_ONCE(indices[1], consumer);
		cond_resched();
	}
	return 0;
}

static void dchid_handle_packet(void *cookie, size_t avail)
{
	struct dockchannel_hid *dchid = cookie;
	struct dchid_hdr hdr;
	int ret;

	if (READ_ONCE(dchid->stopping))
		return;

	if (dockchannel_recv(dchid->dc, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		dev_err(dchid->dev, "Read failed (header)\n");
		return;
	}
	if (hdr.hdr_len != sizeof(hdr)) {
		dev_err(dchid->dev, "Bad header length %d\n", hdr.hdr_len);
		goto done;
	}
	if (dockchannel_recv(dchid->dc, dchid->pkt_buf, hdr.length + 4) != hdr.length + 4) {
		dev_err(dchid->dev, "Read failed (body)\n");
		goto done;
	}

	if (dchid_dispatch(dchid, &hdr, dchid->pkt_buf, true)) {
		ret = dchid_drain_ring(dchid);
		if (ret < 0)
			dev_err(dchid->dev, "Receive ring failed: %d\n", ret);
	}

done:
	if (!READ_ONCE(dchid->stopping))
		dockchannel_await(dchid->dc, dchid_handle_packet, dchid,
				  sizeof(struct dchid_hdr));
}

/* AFE power through SMC key gp1c, for boards whose device tree links the SMC. */
static int dchid_bind_afe_smc(struct dockchannel_hid *dchid)
{
	struct device *dev = dchid->dev;
#if IS_REACHABLE(CONFIG_MFD_MACSMC)
	struct device_node *node;
	struct platform_device *pdev;
	struct device_link *link;
	struct apple_smc_key_info info;
	int ret;
#endif

	if (!of_find_property(dev->of_node, "apple,afe-smc", NULL))
		return 0;
	if (!of_device_is_compatible(dev->of_node, "apple,t6041-dockchannel-hid"))
		return -EINVAL;
#if !IS_REACHABLE(CONFIG_MFD_MACSMC)
	return -EOPNOTSUPP;
#else
	node = of_parse_phandle(dev->of_node, "apple,afe-smc", 0);
	if (!node)
		return -EINVAL;
	/* What macsmc binds to: its driver data is read as an apple_smc below */
	if (!of_device_is_compatible(node, "apple,t8103-smc") &&
	    !of_device_is_compatible(node, "apple,smc")) {
		of_node_put(node);
		return -EINVAL;
	}
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	link = device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&pdev->dev);
	if (!link)
		return -EINVAL;
	if (link->supplier->links.status != DL_DEV_DRIVER_BOUND)
		return -EPROBE_DEFER;
	dchid->afe_smc = dev_get_drvdata(link->supplier);
	if (!dchid->afe_smc)
		return -EPROBE_DEFER;
	ret = apple_smc_get_key_info(dchid->afe_smc, SMC_KEY(gp1c), &info);
	if (ret < 0)
		return ret;
	if (info.size != 4 || !(info.flags & APPLE_SMC_WRITABLE))
		return -EINVAL;
	dev_info(dev, "AFE power sequence via SMC gp1c\n");
	return 0;
#endif
}

static int dockchannel_hid_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dockchannel_hid *dchid;
	struct device_node *child, *helper;
	struct fwnode_handle *stm;
	struct platform_device *helper_pdev;
	struct property *prop;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	dchid = devm_kzalloc(dev, sizeof(*dchid), GFP_KERNEL);
	if (!dchid) {
		return -ENOMEM;
	}

	dchid->dev = dev;
	stm = device_get_named_child_node(dev, "stm");
	dchid->id_ready = !stm || !fwnode_device_is_available(stm);
	fwnode_handle_put(stm);
	mutex_init(&dchid->ring_mutex);
	/* The M4 Pro (J616s) MTP uses the same 26.6.2 firmware transport as T8132. */
	dchid->use_ring = of_device_is_compatible(dev->of_node, "apple,t6041-dockchannel-hid");

	ret = dchid_bind_afe_smc(dchid);
	if (ret)
		return dev_err_probe(dev, ret, "AFE power sequence preflight failed\n");

	/*
	 * The STM interface reports the machine's vendor and product IDs,
	 * keyboard type and serial number, and the other interfaces wait for
	 * it. Firmware without an STM interface gets Apple's vendor ID and
	 * nothing else.
	 */
	if (of_property_read_bool(dev->of_node, "apple,no-stm")) {
		dchid->device_id.vendor_id = HOST_VENDOR_ID_APPLE;
		dchid->id_ready = true;
	}

	/*
	 * First make sure all the GPIOs are available, in cased we need to defer.
	 * This is necessary because MTP will request them by name later, and by then
	 * it's too late to defer the probe.
	 */

	for_each_child_of_node(dev->of_node, child) {
		for_each_property_of_node(child, prop) {
			size_t len = strlen(prop->name);
			struct gpio_desc *gpio;

			if (len < 12 || strncmp("apple,", prop->name, 6) ||
			    strcmp("-gpios", prop->name + len - 6))
				continue;

			gpio = fwnode_gpiod_get_index(&child->fwnode, prop->name, 0, GPIOD_ASIS,
						      prop->name);
			if (IS_ERR_OR_NULL(gpio)) {
				if (PTR_ERR(gpio) == -EPROBE_DEFER) {
					of_node_put(child);
					return -EPROBE_DEFER;
				}
			} else {
				gpiod_put(gpio);
			}
		}
	}

	/*
	 * With power method 2 the host drives the multi-touch AFE reset line
	 * during every power transition, so the line must be there before the
	 * coprocessor announces the interface. Request it now, while a
	 * -EPROBE_DEFER can still be honoured, and leave its level alone until
	 * the first transition has been announced.
	 */
	child = of_get_child_by_name(dev->of_node, "multi-touch");
	if (child) {
		u32 power_method = DCHID_POWER_METHOD_1;
		bool firmware_managed_reset;

		of_property_read_u32(child, "apple,power-method", &power_method);
		firmware_managed_reset =
			of_property_read_bool(child, "apple,firmware-managed-reset");
		of_node_put(child);
		if (firmware_managed_reset &&
		    (power_method != DCHID_POWER_METHOD_2 ||
		     of_property_present(dev->of_node, "apple,afe-reset-gpios")))
			return dev_err_probe(dev, -EINVAL, "Conflicting touch reset ownership\n");
		if (power_method == DCHID_POWER_METHOD_2 && !firmware_managed_reset) {
			dchid->afe_reset = devm_gpiod_get(dev, "apple,afe-reset",
							  GPIOD_ASIS);
			if (IS_ERR(dchid->afe_reset))
				return dev_err_probe(dev, PTR_ERR(dchid->afe_reset),
						     "Failed to request the AFE reset line\n");
		}
	}

	/*
	 * Make sure we also have the MTP coprocessor available, and
	 * defer probe if the helper hasn't probed yet.
	 */
	helper = of_parse_phandle(dev->of_node, "apple,helper-cpu", 0);
	if (!helper) {
		dev_err(dev, "Missing apple,helper-cpu property\n");
		return -EINVAL;
	}

	helper_pdev = of_find_device_by_node(helper);
	of_node_put(helper);
	if (!helper_pdev) {
		dev_err(dev, "Failed to find helper device\n");
		return -EINVAL;
	}

	dchid->helper_link = device_link_add(dev, &helper_pdev->dev,
					     DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&helper_pdev->dev);
	if (!dchid->helper_link) {
		dev_err(dev, "Failed to link to helper device\n");
		return -EINVAL;
	}

	if (dchid->helper_link->supplier->links.status != DL_DEV_DRIVER_BOUND)
		return -EPROBE_DEFER;
	dchid->inherited_mtp =
		apple_rtkit_helper_preserves_input(dchid->helper_link->supplier);

	/* Now it is safe to begin initializing */
	dchid->dc = dockchannel_init(pdev);
	if (IS_ERR_OR_NULL(dchid->dc)) {
		return PTR_ERR(dchid->dc);
	}

	/*
	 * A keypress or a trackpad click wakes the system. The coprocessor
	 * keeps running across suspend-to-idle and keeps sending through the
	 * FIFO, so received frames are classified (dchid_wake_report()) and
	 * other traffic does not wake it. Without a wakeup source the machine
	 * still works, it just cannot be woken from here.
	 */
	ret = dockchannel_init_wakeup(dchid->dc);
	if (ret)
		dev_warn(dev, "Failed to set up wakeup: %d\n", ret);

	if (dchid->use_ring) {
		dchid->ring = dmam_alloc_coherent(dev, DCHID_RING_SIZE, &dchid->ring_dma,
						  GFP_KERNEL);
		if (!dchid->ring)
			return -ENOMEM;
	}
	/*
	 * Freeze interface creation across system sleep so the HID devices
	 * stay stable while the suspend and resume callbacks walk them.
	 */
	dchid->new_iface_wq = alloc_ordered_workqueue("dchid-new", WQ_FREEZABLE);
	if (!dchid->new_iface_wq)
		return -ENOMEM;

	dchid->comm = dchid_get_interface(dchid, IFACE_COMM, "comm");
	if (!dchid->comm) {
		dev_err(dchid->dev, "Failed to initialize comm interface\n");
		destroy_workqueue(dchid->new_iface_wq);
		return -EIO;
	}

	dev_dbg(dchid->dev, "Initialized, awaiting packets\n");
	platform_set_drvdata(pdev, dchid);
	dockchannel_await(dchid->dc, dchid_handle_packet, dchid, sizeof(struct dchid_hdr));

	return 0;
}

static void dockchannel_hid_remove(struct platform_device *pdev)
{
	struct dockchannel_hid *dchid = platform_get_drvdata(pdev);
	int i;

	/*
	 * Stop the receive path first: no new packets, no new work, and a
	 * receive callback that is still running does not re-arm itself.
	 */
	WRITE_ONCE(dchid->stopping, true);
	dockchannel_await(dchid->dc, NULL, NULL, 0);

	/* Packet work can still queue interface creation; drain it first. */
	for (i = 0; i < MAX_INTERFACES; i++) {
		if (dchid->ifaces[i])
			destroy_workqueue(dchid->ifaces[i]->wq);
	}
	destroy_workqueue(dchid->new_iface_wq);

	for (i = 0; i < MAX_INTERFACES; i++) {
		struct dchid_iface *iface = dchid->ifaces[i];

		if (!iface)
			continue;
		if (iface->hid)
			hid_destroy_device(iface->hid);
		of_node_put(iface->of_node);
	}
}

/*
 * The coprocessor keeps running across suspend-to-idle: there is no known
 * sleep command for it, and its RTKit session and the trackpad AFE power
 * sequence are not restartable. Only mark the sleep window, in which input
 * may wake the system and requests from the HID core fail fast.
 */
static int dockchannel_hid_suspend(struct device *dev)
{
	struct dockchannel_hid *dchid = dev_get_drvdata(dev);
	int i, ret;

	/* HID drivers may still need the transport while preparing for sleep. */
	for (i = 0; i < MAX_INTERFACES; i++) {
		struct dchid_iface *iface = dchid->ifaces[i];

		if (!iface || !iface->hid)
			continue;
		ret = hid_driver_suspend(iface->hid, PMSG_SUSPEND);
		if (ret)
			goto unwind;
	}
	WRITE_ONCE(dchid->suspended, true);
	return 0;

unwind:
	/* PM does not call our resume when this callback fails. */
	while (--i >= 0) {
		struct dchid_iface *iface = dchid->ifaces[i];

		if (iface && iface->hid)
			hid_driver_resume(iface->hid);
	}
	return ret;
}

static int dockchannel_hid_resume(struct device *dev)
{
	struct dockchannel_hid *dchid = dev_get_drvdata(dev);
	int i, ret = 0;

	/* Allow feature requests again before notifying any HID driver. */
	WRITE_ONCE(dchid->suspended, false);
	for (i = 0; i < MAX_INTERFACES; i++) {
		struct dchid_iface *iface = dchid->ifaces[i];
		int err;

		if (!iface || !iface->hid)
			continue;
		err = hid_driver_resume(iface->hid);
		if (!ret)
			ret = err;
	}
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dockchannel_hid_pm_ops, dockchannel_hid_suspend,
				dockchannel_hid_resume);

static const struct of_device_id dockchannel_hid_of_match[] = {
	{ .compatible = "apple,dockchannel-hid" },
	{},
};
MODULE_DEVICE_TABLE(of, dockchannel_hid_of_match);
MODULE_FIRMWARE("apple/tpmtfw-*.bin");

static struct platform_driver dockchannel_hid_driver = {
	.driver = {
		.name = "dockchannel-hid",
		.of_match_table = dockchannel_hid_of_match,
		/* The coprocessor announces its interfaces only once per boot. */
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&dockchannel_hid_pm_ops),
	},
	.probe = dockchannel_hid_probe,
	.remove = dockchannel_hid_remove,
};
module_platform_driver(dockchannel_hid_driver);

MODULE_DESCRIPTION("Apple DockChannel HID transport driver");
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_HID_DOCKCHANNEL_PARSER_KUNIT_TEST)
#include "dockchannel-hid-parser-test.c"
#endif
