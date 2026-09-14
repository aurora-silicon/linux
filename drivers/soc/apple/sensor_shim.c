/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */
/* Fingerprint sensor SPI shim: moves bytes over the bus and toggles power. */

#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include "shim.h"

/*
 * 8 MHz, SPI mode 2 (CPOL=1 CPHA=0), 8-bit words. Mode 3 returns sixteen zero
 * bytes, else identical. Must agree with the node's spi-cpol / absent spi-cpha,
 * which is what spi_setup() applies.
 */
#define SEP_SENSOR_HZ		8000000
#define SEP_SENSOR_BITS		8
static unsigned int sep_sensor_mode = SPI_MODE_2;
module_param_named(sensor_spi_mode, sep_sensor_mode, uint, 0444);
MODULE_PARM_DESC(sensor_spi_mode,
		 "SPI mode for the sensor: 2 (CPOL=1 CPHA=0, the default and the only mode this sensor answers under) or 3 (CPOL=1 CPHA=1, reads all-zero). For comparison only.");
#define SEP_SENSOR_MODE		sep_sensor_mode

/* Chip-select setup and hold, in ns. */
#define SEP_SENSOR_CS_NS		20

#define SEP_SENSOR_OFF_DELAY_MS	10
#define SEP_SENSOR_ON_DELAY_MS	7

static struct spi_device *sep_spi;
static struct gpio_desc *sep_power;
static bool sep_registered;

/*
 * Power line resolved by DT node path, not gpiochip index: registration order
 * shifts, so an index would silently pick the wrong chip.
 */
#define SEP_SENSOR_GPIO_NODE		"/soc/pinctrl@39b028000"
#define SEP_SENSOR_GPIO_LINE		122

#define SEP_POWER_NONE		0
#define SEP_POWER_NODE_PROPERTY	1
#define SEP_POWER_CHIP_LINE		2

static int sep_power_source;

/*
 * Takes the power line as an output driven low: the power cycle begins with an
 * off phase, so the line must be actively driven off, not merely read as low.
 */
static void sep_acquire_power(struct spi_device *spi)
{
	struct device_node *np;
	struct gpio_device *gdev;
	struct gpio_chip *gc;

	sep_power = gpiod_get_index(&spi->dev, NULL, 0, GPIOD_OUT_LOW);
	if (!IS_ERR(sep_power)) {
		sep_power_source = SEP_POWER_NODE_PROPERTY;
		dev_info(&spi->dev,
			 "sep sensor: power line from the device node, driven low\n");
		return;
	}
	sep_power = NULL;

	/*
	 * The fallback below is the j414s power line; a sibling board's sensor sits
	 * on a different pin, so only j414s may use it. Every other machine must
	 * describe the power GPIO in its device node (the gpiod_get_index path).
	 */
	if (!of_machine_is_compatible("apple,j414s")) {
		dev_warn(&spi->dev,
			 "sep sensor: no power GPIO in the device node; describe gpios in DT\n");
		return;
	}

	np = of_find_node_by_path(SEP_SENSOR_GPIO_NODE);
	if (!np) {
		dev_warn(&spi->dev,
			 "sep sensor: no DT node at %s, no power line\n",
			 SEP_SENSOR_GPIO_NODE);
		return;
	}

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
	of_node_put(np);
	if (!gdev) {
		dev_warn(&spi->dev,
			 "sep sensor: %s has no registered GPIO device\n",
			 SEP_SENSOR_GPIO_NODE);
		return;
	}

	gc = gpio_device_get_chip(gdev);
	if (gc)
		sep_power = gpiochip_request_own_desc(gc,
							 SEP_SENSOR_GPIO_LINE,
							 "apple-mesa-power",
							 GPIO_LOOKUP_FLAGS_DEFAULT,
							 GPIOD_OUT_LOW);
	if (!gc || IS_ERR(sep_power)) {
		sep_power = NULL;
		dev_warn(&spi->dev,
			 "sep sensor: could not take line %d on %s (%s)\n",
			 SEP_SENSOR_GPIO_LINE, SEP_SENSOR_GPIO_NODE,
			 gpio_device_get_label(gdev));
		gpio_device_put(gdev);
		return;
	}

	sep_power_source = SEP_POWER_CHIP_LINE;
	dev_info(&spi->dev,
		 "sep sensor: power line %s line %d (%s), driven low\n",
		 SEP_SENSOR_GPIO_NODE, SEP_SENSOR_GPIO_LINE,
		 gpio_device_get_label(gdev));
	gpio_device_put(gdev);
}

static void sep_release_power(void)
{
	if (!sep_power)
		return;
	gpiod_set_value_cansleep(sep_power, 0);
	if (sep_power_source == SEP_POWER_CHIP_LINE)
		gpiochip_free_own_desc(sep_power);
	else
		gpiod_put(sep_power);
	sep_power = NULL;
	sep_power_source = SEP_POWER_NONE;
}

#define SEP_CS_TIMING_SOFTWARE	0
#define SEP_CS_TIMING_HOOK_BYPASSED	1
#define SEP_CS_TIMING_HARDWARE	2

static int sep_cs_timing_mode;

/*
 * Programs the 20 ns chip-select setup/hold and applies mode/speed via
 * spi_setup(), which reaches the controller's set_cs_timing hook.
 *
 * This sensor needs hardware timing. The hook runs only for a native chip
 * select; with a GPIO chip select the core silently emulates the delays in
 * software, which this sensor rejects but which looks like success.
 */
static int sep_apply_cs_timing(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	int rc;

	if (!ctlr->set_cs_timing)
		sep_cs_timing_mode = SEP_CS_TIMING_SOFTWARE;
	else if (spi_get_csgpiod(spi, 0))
		sep_cs_timing_mode = SEP_CS_TIMING_HOOK_BYPASSED;
	else
		sep_cs_timing_mode = SEP_CS_TIMING_HARDWARE;

	/*
	 * Hold the bus lock: these fields are read when a transfer asserts chip
	 * select. spi_setup() takes the controller io_mutex, not the bus lock,
	 * so no deadlock.
	 */
	rc = spi_bus_lock(ctlr);
	if (rc)
		return rc;

	spi->mode = SEP_SENSOR_MODE;
	spi->bits_per_word = SEP_SENSOR_BITS;
	spi->max_speed_hz = SEP_SENSOR_HZ;
	spi->cs_setup.value = SEP_SENSOR_CS_NS;
	spi->cs_setup.unit = SPI_DELAY_UNIT_NSECS;
	spi->cs_hold.value = SEP_SENSOR_CS_NS;
	spi->cs_hold.unit = SPI_DELAY_UNIT_NSECS;

	rc = spi_setup(spi);

	spi_bus_unlock(ctlr);

	if (rc)
		return rc;

	switch (sep_cs_timing_mode) {
	case SEP_CS_TIMING_HARDWARE:
		dev_info(&spi->dev,
			 "sep sensor: CS timing %u ns programmed in hardware\n",
			 SEP_SENSOR_CS_NS);
		break;
	case SEP_CS_TIMING_HOOK_BYPASSED:
		dev_warn(&spi->dev,
			 "sep sensor: GPIO chip select, CS timing emulated in software (sensor needs hardware timing)\n");
		break;
	default:
		dev_warn(&spi->dev,
			 "sep sensor: no set_cs_timing hook, %u ns emulated in software (sensor needs hardware timing)\n",
			 SEP_SENSOR_CS_NS);
		break;
	}
	return 0;
}

static int sep_sensor_probe(struct spi_device *spi)
{
	int rc;

	rc = sep_apply_cs_timing(spi);
	if (rc)
		return rc;

	sep_spi = spi;
	sep_acquire_power(spi);
	/* Read the mode back: a mode that did not take must not be logged as
	 * the one requested. */
	dev_info(&spi->dev,
		 "sep sensor: bound, %u Hz, mode %u (CPOL=%d CPHA=%d)\n",
		 spi->max_speed_hz,
		 (unsigned int)(spi->mode & (SPI_CPOL | SPI_CPHA)),
		 !!(spi->mode & SPI_CPOL), !!(spi->mode & SPI_CPHA));
	return 0;
}

static void sep_sensor_remove(struct spi_device *spi)
{
	sep_release_power();
	sep_spi = NULL;
}

/*
 * Its own compatible, deliberately not spidev's: capture must stay in the
 * kernel, and a spidev node would leak an image to userspace. No
 * MODULE_DEVICE_TABLE, so udev cannot auto-load this module.
 */
static const struct of_device_id sep_sensor_of_match[] = {
	{ .compatible = "apple,mesa-fingerprint" },
	{ }
};

/*
 * Legacy ID table only to silence the SPI core's "no spi_device_id" note. No
 * MODULE_DEVICE_TABLE: it quiets a message, it does not advertise a binding.
 */
static const struct spi_device_id sep_sensor_spi_ids[] = {
	{ "mesa-fingerprint", 0 },
	{ }
};

static struct spi_driver sep_sensor_driver = {
	.driver = {
		.name = "apple-mesa",
		.of_match_table = sep_sensor_of_match,
	},
	.id_table = sep_sensor_spi_ids,
	.probe = sep_sensor_probe,
	.remove = sep_sensor_remove,
};

/*
 * Registered before the device tree gains the sensor node, so the bind happens
 * when the SPI core's notifier creates the device.
 */
int sep_sensor_register(void)
{
	int rc;

	if (sep_registered)
		return 0;
	rc = spi_register_driver(&sep_sensor_driver);
	if (rc)
		return rc;
	sep_registered = true;
	return 0;
}

void sep_sensor_unregister(void)
{
	if (!sep_registered)
		return;
	spi_unregister_driver(&sep_sensor_driver);
	sep_registered = false;
}

int sep_sensor_bound(void)
{
	return sep_spi != NULL;
}

/* Whether those delays reach hardware, are emulated, or are silently bypassed. */
int sep_sensor_cs_timing_mode(void)
{
	return sep_cs_timing_mode;
}

/* Power cycle: off, 10 ms, on, 7 ms. -ENODEV if there is no power line. */
int sep_sensor_power_cycle(void)
{
	if (!sep_power)
		return -ENODEV;

	gpiod_set_value_cansleep(sep_power, 0);
	msleep(SEP_SENSOR_OFF_DELAY_MS);
	gpiod_set_value_cansleep(sep_power, 1);
	msleep(SEP_SENSOR_ON_DELAY_MS);
	return 0;
}

/* How the power line was obtained: none, the node's property, or chip+line. */
int sep_sensor_power_source(void)
{
	return sep_power_source;
}

int sep_sensor_power_line(void)
{
	if (!sep_power)
		return -ENODEV;
	return desc_to_gpio(sep_power);
}

/* Powers the sensor on/off, holding the hardware settling delay. */
int sep_sensor_power(int on)
{
	if (!sep_power)
		return -ENODEV;

	gpiod_set_value_cansleep(sep_power, on ? 1 : 0);
	if (on)
		msleep(SEP_SENSOR_ON_DELAY_MS);
	else
		msleep(SEP_SENSOR_OFF_DELAY_MS);
	return 0;
}

/*
 * One chip-select assertion, `len` clocks, full duplex. The caller supplies all
 * `len` transmit bytes (trailing ones as 0xff) rather than relying on the
 * controller running its transmit buffer dry.
 */
int sep_sensor_xfer(const void *tx, void *rx, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.speed_hz = SEP_SENSOR_HZ,
		.bits_per_word = SEP_SENSOR_BITS,
	};

	if (!sep_spi)
		return -ENODEV;
	if (!len)
		return -EINVAL;

	return spi_sync_transfer(sep_spi, &xfer, 1);
}

/*
 * One chip-select assertion, transmit only, rx_buf NULL (not a discard buffer).
 * A simultaneous receive changes the long transfer's pacing and the sensor
 * rejects the patch blob, so the receive must be absent entirely.
 */
int sep_sensor_xfer_tx(const void *tx, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = NULL,
		.len = len,
		.speed_hz = SEP_SENSOR_HZ,
		.bits_per_word = SEP_SENSOR_BITS,
	};

	if (!sep_spi)
		return -ENODEV;
	if (!len)
		return -EINVAL;

	return spi_sync_transfer(sep_spi, &xfer, 1);
}

/*
 * Two transfers in one chip-select assertion: command out, then read in.
 * Chip-select stays asserted because neither transfer sets cs_change.
 */
int sep_sensor_xfer2(const void *tx, size_t tx_len, void *rx, size_t rx_len)
{
	struct spi_transfer xfers[2] = {
		{
			.tx_buf = tx,
			.len = tx_len,
			.speed_hz = SEP_SENSOR_HZ,
			.bits_per_word = SEP_SENSOR_BITS,
		},
		{
			.rx_buf = rx,
			.len = rx_len,
			.speed_hz = SEP_SENSOR_HZ,
			.bits_per_word = SEP_SENSOR_BITS,
		},
	};

	if (!sep_spi)
		return -ENODEV;
	if (!tx_len || !rx_len)
		return -EINVAL;

	return spi_sync_transfer(sep_spi, xfers, 2);
}
