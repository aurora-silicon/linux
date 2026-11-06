// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Stockholm NFC controller transport over SPMI
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spmi.h>

#include <net/nfc/nci_core.h>

#define STOCKHOLM_NFC_PROTOCOLS	(NFC_PROTO_JEWEL_MASK | \
				 NFC_PROTO_MIFARE_MASK | \
				 NFC_PROTO_FELICA_MASK | \
				 NFC_PROTO_ISO14443_MASK | \
				 NFC_PROTO_ISO14443_B_MASK | \
				 NFC_PROTO_ISO15693_MASK | \
				 NFC_PROTO_NFC_DEP_MASK)

#define STOCKHOLM_ERROR_STATUS0_REG	0x4
#define STOCKHOLM_ERROR_STATUS1_REG	0x7
#define STOCKHOLM_ERROR_STATUS2_REG	0x8

/* Transport parameters of one controller model. */
struct stockholm_model {
	u16 tx_reg;		/* non-incrementing NCI transmit FIFO */
	u16 rx_reg;		/* non-incrementing NCI receive FIFO */
	u16 id_reg;		/* follower ID, side-effect-free read */
	u16 irq_reg;		/* follower IRQ offset, side-effect-free read */
	u16 standby_reg;	/* follower virtual command register */
	u8 rx_trailer_len;	/* bytes drained after the NCI packet */
	u8 follower_id;		/* expected value of id_reg */
	u8 irq_offset;		/* expected base of the follower's IRQ block */
	u8 standby_on;
	u8 standby_off;
	u32 startup_delay_us;
};

static const struct stockholm_model stockholm_model96 = {
	.tx_reg = 0x1000,
	.rx_reg = 0x80,
	.id_reg = 0x100,
	.irq_reg = 0x105,
	.standby_reg = 0x0,
	.rx_trailer_len = 0,
	.follower_id = 0x19,
	.irq_offset = 0xe0,
	.standby_on = 0x15,
	.standby_off = 0x14,
	.startup_delay_us = 10000,
};

struct stockholm {
	struct spmi_device *sdev;
	struct nci_dev *ndev;
	struct gpio_desc *enable_gpio;
	struct mutex io_lock;
	u16 tx_reg;
	u16 rx_reg;
	u16 standby_reg;
	u8 rx_trailer_len;
	u8 standby_on;
	u8 standby_off;
	u32 startup_delay_us;
	int data_irq;
	int wake_irq;
	int error_irq;
	bool powered;
};

static int stockholm_fifo_read(struct stockholm *st, u8 *buf, size_t len)
{
	size_t chunk;
	int ret;

	while (len) {
		chunk = min_t(size_t, len, 8);
		ret = spmi_ext_register_readl(st->sdev, st->rx_reg, buf, chunk);
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
		chunk = min_t(size_t, len, 8);
		ret = spmi_ext_register_writel(st->sdev, st->tx_reg, buf, chunk);
		if (ret)
			return ret;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int stockholm_set_standby(struct stockholm *st, bool standby)
{
	u8 value = standby ? st->standby_on : st->standby_off;

	return spmi_ext_register_writel(st->sdev, st->standby_reg, &value, 1);
}

static int stockholm_set_power(struct stockholm *st, bool on)
{
	int ret;

	if (st->powered == on)
		return 0;

	ret = gpiod_direction_output(st->enable_gpio, on);
	if (ret)
		return ret;

	st->powered = on;
	if (on) {
		usleep_range(st->startup_delay_us,
			     st->startup_delay_us + max_t(u32, 100, st->startup_delay_us / 10));
		ret = stockholm_set_standby(st, false);
		if (ret)
			dev_dbg(&st->sdev->dev, "standby exit failed: %d\n", ret);
	}

	return 0;
}

static int stockholm_nci_open(struct nci_dev *ndev)
{
	struct stockholm *st = nci_get_drvdata(ndev);
	int ret;

	mutex_lock(&st->io_lock);
	ret = stockholm_set_power(st, true);
	if (!ret)
		enable_irq(st->data_irq);
	mutex_unlock(&st->io_lock);

	return ret;
}

static int stockholm_nci_close(struct nci_dev *ndev)
{
	struct stockholm *st = nci_get_drvdata(ndev);
	int ret;

	disable_irq(st->data_irq);
	mutex_lock(&st->io_lock);
	ret = stockholm_set_standby(st, false);
	if (ret)
		dev_dbg(&st->sdev->dev, "standby disable failed: %d\n", ret);
	ret = stockholm_set_power(st, false);
	mutex_unlock(&st->io_lock);

	return ret;
}

static int stockholm_nci_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct stockholm *st = nci_get_drvdata(ndev);
	int ret;

	if (!st->powered) {
		kfree_skb(skb);
		return -EHOSTDOWN;
	}

	mutex_lock(&st->io_lock);
	ret = stockholm_fifo_write(st, skb->data, skb->len);
	mutex_unlock(&st->io_lock);

	if (ret)
		kfree_skb(skb);
	else
		consume_skb(skb);

	return ret;
}

static int stockholm_nci_post_setup(struct nci_dev *ndev)
{
	struct stockholm *st = nci_get_drvdata(ndev);

	dev_info(&st->sdev->dev,
		 "NCI %u.%u, manufacturer %#02x, firmware identity %#08x\n",
		 ndev->nci_ver >> 4, ndev->nci_ver & 0xf,
		 ndev->manufact_id, ndev->manufact_specific_info);

	return 0;
}

static const struct nci_ops stockholm_nci_ops = {
	.open = stockholm_nci_open,
	.close = stockholm_nci_close,
	.send = stockholm_nci_send,
	.post_setup = stockholm_nci_post_setup,
};

static irqreturn_t stockholm_data_irq_thread(int irq, void *data)
{
	struct stockholm *st = data;
	struct nci_ctrl_hdr hdr;
	struct sk_buff *skb = NULL;
	u8 trailer[2];
	size_t nci_len;
	int ret;

	mutex_lock(&st->io_lock);
	ret = stockholm_fifo_read(st, (u8 *)&hdr, sizeof(hdr));
	if (ret)
		goto out_unlock;

	nci_len = sizeof(hdr) + hdr.plen;
	skb = alloc_skb(nci_len, GFP_KERNEL);
	if (!skb) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	skb_put_data(skb, &hdr, sizeof(hdr));
	ret = stockholm_fifo_read(st, skb_put(skb, hdr.plen), hdr.plen);
	/* The two model-96 trailer bytes are not part of the NCI packet. */
	if (!ret && st->rx_trailer_len)
		ret = stockholm_fifo_read(st, trailer, st->rx_trailer_len);
	mutex_unlock(&st->io_lock);
	if (ret)
		goto out_free;

	nci_recv_frame(st->ndev, skb);
	return IRQ_HANDLED;

out_unlock:
	mutex_unlock(&st->io_lock);
out_free:
	if (!ret)
		ret = -EBADMSG;
	if (ret)
		dev_err_ratelimited(&st->sdev->dev, "NCI receive failed: %d\n", ret);
	if (skb)
		kfree_skb(skb);
	return IRQ_HANDLED;
}

static irqreturn_t stockholm_wake_irq_thread(int irq, void *data)
{
	struct stockholm *st = data;

	pm_wakeup_event(&st->sdev->dev, 0);
	return IRQ_HANDLED;
}

static irqreturn_t stockholm_error_irq_thread(int irq, void *data)
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
				    "SPMI transport error; status read failed: %d\n",
				    ret);
	else
		dev_err_ratelimited(&st->sdev->dev,
				    "SPMI transport error: status %#02x/%#02x/%#02x\n",
				    status[0], status[1], status[2]);
	return IRQ_HANDLED;
}

static int stockholm_read_config(struct stockholm *st,
				 const struct stockholm_model *model)
{
	struct device *dev = &st->sdev->dev;
	u8 value;
	int ret;

	ret = spmi_ext_register_readl(st->sdev, model->id_reg, &value, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read follower ID\n");
	if (value != model->follower_id)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected follower ID %#02x\n", value);

	ret = spmi_ext_register_readl(st->sdev, model->irq_reg, &value, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read IRQ offset\n");
	if (value != model->irq_offset)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected IRQ offset %#02x\n", value);

	return 0;
}

static int stockholm_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	const struct stockholm_model *model;
	struct stockholm *st;
	int ven, ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->sdev = sdev;
	mutex_init(&st->io_lock);
	spmi_device_set_drvdata(sdev, st);

	model = device_get_match_data(dev);
	if (!model)
		return -ENODEV;
	st->tx_reg = model->tx_reg;
	st->rx_reg = model->rx_reg;
	st->standby_reg = model->standby_reg;
	st->rx_trailer_len = model->rx_trailer_len;
	st->standby_on = model->standby_on;
	st->standby_off = model->standby_off;
	st->startup_delay_us = model->startup_delay_us;
	st->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(st->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(st->enable_gpio),
				     "failed to get VEN GPIO\n");
	ven = gpiod_get_value_cansleep(st->enable_gpio);
	dev_dbg(dev, "VEN at probe: %d\n", ven);

	ret = stockholm_set_power(st, true);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable controller\n");

	ret = stockholm_read_config(st, model);
	if (ret)
		goto err_poweroff;

	st->ndev = nci_allocate_device(&stockholm_nci_ops,
					STOCKHOLM_NFC_PROTOCOLS, 0, 0);
	if (!st->ndev) {
		ret = -ENOMEM;
		goto err_poweroff;
	}
	nci_set_parent_dev(st->ndev, dev);
	nci_set_drvdata(st->ndev, st);

	st->data_irq = fwnode_irq_get_byname(dev_fwnode(dev), "data-ready");
	if (st->data_irq < 0) {
		ret = st->data_irq;
		goto err_free_nci;
	}
	ret = devm_request_threaded_irq(dev, st->data_irq, NULL,
					stockholm_data_irq_thread, IRQF_ONESHOT,
					dev_name(dev), st);
	if (ret)
		goto err_free_nci;
	disable_irq(st->data_irq);

	st->wake_irq = fwnode_irq_get_byname(dev_fwnode(dev), "host-wake");
	if (st->wake_irq >= 0) {
		ret = devm_request_threaded_irq(dev, st->wake_irq, NULL,
						stockholm_wake_irq_thread,
						IRQF_ONESHOT, dev_name(dev), st);
		if (ret)
			goto err_free_nci;
		device_init_wakeup(dev, true);
	}

	st->error_irq = fwnode_irq_get_byname(dev_fwnode(dev), "error");
	if (st->error_irq >= 0) {
		ret = devm_request_threaded_irq(dev, st->error_irq, NULL,
						stockholm_error_irq_thread,
						IRQF_ONESHOT, dev_name(dev), st);
		if (ret)
			goto err_free_nci;
	}

	ret = nci_register_device(st->ndev);
	if (ret)
		goto err_free_nci;

	ret = stockholm_set_standby(st, false);
	if (ret)
		dev_dbg(dev, "initial standby disable failed: %d\n", ret);
	ret = stockholm_set_power(st, false);
	if (ret)
		dev_warn(dev, "failed to park VEN low: %d\n", ret);
	else
		dev_info(dev, "registered NCI device and parked VEN low\n");

	return 0;

err_free_nci:
	nci_free_device(st->ndev);
err_poweroff:
	stockholm_set_power(st, false);
	return ret;
}

static void stockholm_remove(struct spmi_device *sdev)
{
	struct stockholm *st = spmi_device_get_drvdata(sdev);

	nci_unregister_device(st->ndev);
	nci_free_device(st->ndev);
	stockholm_set_power(st, false);
}

static void stockholm_shutdown(struct spmi_device *sdev)
{
	struct stockholm *st = spmi_device_get_drvdata(sdev);

	stockholm_set_power(st, false);
}

static const struct of_device_id stockholm_of_match[] = {
	{ .compatible = "apple,stockholm-nci", .data = &stockholm_model96 },
	{}
};
MODULE_DEVICE_TABLE(of, stockholm_of_match);

static struct spmi_driver stockholm_driver = {
	.probe = stockholm_probe,
	.remove = stockholm_remove,
	.shutdown = stockholm_shutdown,
	.driver = {
		.name = "apple-stockholm",
		.of_match_table = stockholm_of_match,
	},
};
module_spmi_driver(stockholm_driver);

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("Apple Stockholm NFC controller over SPMI");
MODULE_LICENSE("GPL");
