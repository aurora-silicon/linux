// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The J873g HDMI port: the T8152's dedicated DisplayPort PHY in display
 * crossbar 0's lpdptx slot, converted by a PS190.
 *
 * Interim, until drivers/phy/apple/dptx.c has a T8152 type (its lanes are
 * 0x2800 apart from 0x4500) and the PS190's power through the SMC is
 * described as a supply: this programs the PHY and powers the converter
 * itself.
 *
 * When the bootloader brought HDMI up, this keeps its setup and reprograms
 * what a link training changes: the PLL for a new link rate and the active
 * lanes. Drive settings are only recorded, and there is no per-link route.
 * When it did not (no HDMI monitor at boot, or a USB-C display on the main
 * DCP), the HDMI port joins the shared USB-C ports: a DCP without a USB-C
 * display takes it on a plug and starts the PHY and the PS190 from cold.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/macsmc.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "dptxep.h"
#include "t8152_display.h"

#define HDMI_LANES		4
#define HDMI_LANE0		0x4500
#define HDMI_LANE_STRIDE	0x2800

struct t8152_hdmi {
	void __iomem *core;
	void __iomem *apb;
	/* AP DisplayPort AUX controller aux1, for the EDID */
	struct t8152_aux aux;
	/* The cold start of the HDMI port taken from the shared ports: */
	void __iomem *ps;	/* PMGR power state of the PHY */
	void __iomem *psd;	/* PMGR PSD service of the PHY */
	struct gpio_desc *hpd;
	struct device_node *smc_np;
	const __be32 *pcon, *tunable;
	int pcon_len, tunable_len;
	bool active;
};

/*
 * The DCP driving the HDMI port taken from the shared ports, if any, and its
 * HPD line, requested once for all the DCPs that may take the port.
 */
static DEFINE_MUTEX(hdmi_port_lock);
static struct device *hdmi_port_owner;
static struct gpio_desc *hdmi_port_hpd;
static unsigned int hdmi_port_users;

#define HDMI_PS_TARGET		GENMASK(3, 0)
#define HDMI_PS_ACTUAL		GENMASK(7, 4)
#define HDMI_PS_FLAGS		GENMASK(9, 8)	/* write-one-to-clear */
#define HDMI_PSD_STATE		0x80
#define HDMI_PSD_TARGET		0x90
#define HDMI_POWER_TIMEOUT_US	100000

/* APB 0x10 bits 3:0: the crossbar UFP feeding the PHY */
#define HDMI_SOURCE_SELECT	0x10
#define HDMI_SOURCE_AUX1	6	/* UFP endpoint of the aux1 controller */

static void hdmi_set_bits(struct t8152_hdmi *hdmi, u32 offset, u32 shift,
			  u32 mask, u32 value)
{
	u32 reg = readl(hdmi->core + offset);

	writel((reg & ~mask) | (value << shift), hdmi->core + offset);
}

static void hdmi_apb_update(struct t8152_hdmi *hdmi, u32 offset, u32 mask,
			    u32 value)
{
	u32 reg = readl(hdmi->apb + offset);

	writel((reg & ~mask) | value, hdmi->apb + offset);
}

/* The PLL configuration words for each link rate (DPCD units). */
static const struct {
	u32 rate;
	u32 w[22];
} hdmi_pll_configs[] = {
	{ LINK_RATE_RBR,
	  { 1, 0x21c, 0, 0, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x3c, 2, 1, 0, 0, 1, 0,
	    0, 0, 1, 3 } },
	{ LINK_RATE_HBR,
	  { 1, 0x1c2, 0x1fff, 0x3ffe, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x32, 2, 1, 0,
	    0, 1, 0, 0, 0, 1, 3 } },
	{ LINK_RATE_HBR2,
	  { 0, 0x1c2, 0x1fff, 0x3ffe, 1, 9, 1, 0, 0, 0x1a, 1, 0x32, 1, 0, 0, 1,
	    0, 0, 0, 1, 1, 3 } },
	{ LINK_RATE_HBR3,
	  { 0, 0x2a3, 0x2ffd, 0x3ffc, 1, 9, 1, 0, 0, 0x1a, 1, 0x5a, 1, 0, 0, 1,
	    0, 0, 0, 1, 1, 3 } },
};

/* A PLL command: command in 0x408[2:0], request bit 3, acknowledge bit 19. */
static int hdmi_pll_command(struct t8152_display *disp, struct t8152_hdmi *hdmi,
			    u32 command)
{
	int i;

	hdmi_set_bits(hdmi, 0x408, 6, 0x40, 1);
	hdmi_set_bits(hdmi, 0x408, 0, 0x7, command);
	hdmi_set_bits(hdmi, 0x408, 3, 0x8, 1);
	for (i = 0; i < 10; i++) {
		usleep_range(16, 32);
		if (readl(hdmi->core + 0x408) & BIT(19))
			break;
	}
	hdmi_set_bits(hdmi, 0x408, 3, 0x8, 0);
	hdmi_set_bits(hdmi, 0x408, 6, 0x40, 0);
	if (i == 10) {
		dev_err(disp->dev, "HDMI: no ACK to PLL command %u\n", command);
		return -ETIMEDOUT;
	}
	return 0;
}

/* The lane-disable bits of each lane; 0x2420 holds the mask of unused lanes. */
static void hdmi_set_lanes(struct t8152_hdmi *hdmi, u32 lanes)
{
	static const u32 offsets[] = { 0x64, 0x24, 0x24, 0x48, 0x48, 0x5c, 0x5c, 0x48 };
	static const u32 shifts[] = { 4, 4, 1, 0, 1, 2, 3, 4 };
	static const u32 masks[] = { 0x10, 0x10, 0x2, 0x1, 0x2, 0x4, 0x8, 0x10 };
	u32 lane, i;

	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 0);
	for (lane = 0; lane < HDMI_LANES; lane++) {
		u32 base = HDMI_LANE0 + lane * HDMI_LANE_STRIDE;

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			hdmi_set_bits(hdmi, base + offsets[i], shifts[i],
				      masks[i], lane >= lanes);
	}
	hdmi_set_bits(hdmi, 0x2420, 0, 0xf, 0xf & ~((1 << lanes) - 1));
	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 1);
}

static int hdmi_activate(struct t8152_display *disp)
{
	return 0;
}

static int hdmi_deactivate(struct t8152_display *disp)
{
	dev_info(disp->dev, "HDMI: keeping the link up on deactivate\n");
	return 0;
}

/*
 * A new link rate: lanes off, PLL down, the PLL frequency, half link rate,
 * PLL up again.
 */
static int hdmi_set_link_rate(struct t8152_display *disp, u32 rate)
{
	struct t8152_hdmi *hdmi = disp->phy_priv;
	const u32 *w = NULL;
	bool half;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hdmi_pll_configs); i++)
		if (hdmi_pll_configs[i].rate == rate)
			w = hdmi_pll_configs[i].w;
	if (!w)
		return -EINVAL;

	dev_info(disp->dev, "HDMI: link rate %#x\n", rate);
	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 0);
	ret = hdmi_pll_command(disp, hdmi, 0);
	if (ret)
		return ret;

	/* The PLL frequency, downspread off */
	hdmi_set_bits(hdmi, 0x6c0, 0, 0x7ff, w[1]);
	hdmi_set_bits(hdmi, 0x6c0, 11, 0x800, w[6]);
	hdmi_set_bits(hdmi, 0x6c0, 13, 0x2000, w[10]);
	hdmi_set_bits(hdmi, 0x6c0, 18, 0x3c0000, w[21]);
	hdmi_set_bits(hdmi, 0x6c4, 14, 0xfffc000, w[2]);
	hdmi_set_bits(hdmi, 0x6c4, 0, 0x3fff,
		      (!(w[7] & 1) && w[2] < 2) ? 0 : w[3]);
	hdmi_set_bits(hdmi, 0x6c8, 14, 0xc000, w[0]);
	hdmi_set_bits(hdmi, 0x6c8, 8, 0x100, 0);
	hdmi_set_bits(hdmi, 0x6c8, 0, 0xff, w[8]);
	hdmi_set_bits(hdmi, 0x6c8, 9, 0x3e00, w[5]);
	hdmi_set_bits(hdmi, 0x6c8, 16, 0x10000, w[17]);
	hdmi_set_bits(hdmi, 0x6c8, 26, 0xfc000000, w[9]);
	hdmi_set_bits(hdmi, 0x6cc, 0, 0x7f, w[11]);
	hdmi_set_bits(hdmi, 0x6cc, 9, 0x600, w[13]);
	hdmi_set_bits(hdmi, 0x6cc, 11, 0x7800, w[14]);
	hdmi_set_bits(hdmi, 0x6cc, 15, 0x38000, w[15]);
	hdmi_set_bits(hdmi, 0x6cc, 20, 0x100000, w[16]);
	hdmi_set_bits(hdmi, 0x6cc, 21, 0x200000, w[18]);
	hdmi_set_bits(hdmi, 0x6cc, 22, 0xc00000, w[19]);
	hdmi_set_bits(hdmi, 0x6cc, 24, 0x3000000, w[4]);
	hdmi_set_bits(hdmi, 0x6cc, 26, 0xc000000, w[20]);

	/* Latch the link rate dependent shadow registers (APB 0xa0). */
	hdmi_apb_update(hdmi, 0xa0, BIT(3), BIT(3));
	hdmi_apb_update(hdmi, 0xa0, BIT(2), BIT(2));
	hdmi_apb_update(hdmi, 0xa0, BIT(18), BIT(18));
	hdmi_apb_update(hdmi, 0xa0, BIT(18), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(19), BIT(19));
	hdmi_apb_update(hdmi, 0xa0, BIT(19), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(2), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(3), 0);

	/* No UHBR rates; half link rate from HBR up. */
	half = rate >= LINK_RATE_HBR;
	hdmi_set_bits(hdmi, 0x2400, 2, 0x4, 0);
	hdmi_set_bits(hdmi, 0x2400, 3, 0x8, !half);
	hdmi_set_bits(hdmi, 0x0000, 17, 0x20000, !half);

	ret = hdmi_pll_command(disp, hdmi, 7);
	if (ret)
		return ret;
	if (!(readl(hdmi->core + 0x438) & BIT(30))) {
		dev_err(disp->dev, "HDMI: PLL failed to lock at rate %#x\n", rate);
		return -EIO;
	}
	return 0;
}

static int hdmi_set_lane_count(struct t8152_display *disp, u32 lanes)
{
	/* The PHY takes no zero lane count. */
	if (lanes)
		hdmi_set_lanes(disp->phy_priv, lanes);
	return 0;
}

static int hdmi_set_drive(struct t8152_display *disp,
			  const struct t8152_drive_settings *settings, u32 count)
{
	return 0;
}

static int hdmi_set_route(struct t8152_display *disp, bool enable)
{
	return 0;
}

/*
 * The sink's EDID through the AP AUX controller aux1: the PHY takes its
 * source from aux1 for the read (its AUX reaches the PS190, which reads the
 * monitor's DDC), then from the DCP again. The display is off meanwhile.
 */
static int hdmi_read_edid(struct t8152_display *disp, u8 *buf, size_t size)
{
	struct t8152_hdmi *hdmi = disp->phy_priv;
	u32 src;
	int ret;

	if (!hdmi->aux.regs)
		return -ENODEV;
	ret = t8152_aux_init(disp, &hdmi->aux);
	if (ret)
		return ret;
	src = readl(hdmi->apb + HDMI_SOURCE_SELECT);
	writel((src & ~0xf) | HDMI_SOURCE_AUX1, hdmi->apb + HDMI_SOURCE_SELECT);
	ret = t8152_aux_read_edid(disp, &hdmi->aux, buf, size);
	writel(src, hdmi->apb + HDMI_SOURCE_SELECT);
	return ret;
}

static const struct t8152_phy_ops hdmi_ops = {
	.activate = hdmi_activate,
	.deactivate = hdmi_deactivate,
	.set_link_rate = hdmi_set_link_rate,
	.set_lane_count = hdmi_set_lane_count,
	.set_drive = hdmi_set_drive,
	.set_route = hdmi_set_route,
	.read_edid = hdmi_read_edid,
	.max_link_rate = LINK_RATE_HBR3,
};

/*
 * The PS190's power through the SMC: the {key, value} writes of
 * apple,pcon-power. The PS190 drives the HDMI HPD line only while it is
 * powered, and the AP AUX controller that reads the USB-C monitors' EDID
 * gets no reply from any USB-C port while it is off. Nothing here waits
 * for it to come up: a DCP takes the port only once it sees HPD
 * (t8152_hdmi_port_poll()), and an EDID read retries until the AUX
 * replies (t8152_aux_read_edid()). On a port taken over, these writes find
 * the PS190 powered since probe.
 */
static int hdmi_pcon_power(struct t8152_display *disp, struct t8152_hdmi *hdmi)
{
	struct platform_device *smc_pdev = of_find_device_by_node(hdmi->smc_np);
	struct apple_smc *smc = smc_pdev ? platform_get_drvdata(smc_pdev) : NULL;
	int ret = 0;

	if (!smc) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	for (int i = 0; i + 3 <= hdmi->pcon_len; i += 3) {
		u64 value = (u64)be32_to_cpu(hdmi->pcon[i + 1]) << 32 |
			    be32_to_cpu(hdmi->pcon[i + 2]);

		ret = apple_smc_write(smc, be32_to_cpu(hdmi->pcon[i]), &value,
				      sizeof(value));
		if (ret) {
			dev_err(disp->dev, "HDMI: PS190 power failed: %d\n", ret);
			goto out;
		}
	}
out:
	if (smc_pdev)
		put_device(&smc_pdev->dev);
	return ret;
}

/* The PHY's PMGR and PSD power, then the PS190's. */
static int hdmi_power_up(struct t8152_display *disp, struct t8152_hdmi *hdmi)
{
	int ret;
	u32 val;

	if ((readl(hdmi->ps) & HDMI_PS_ACTUAL) != HDMI_PS_ACTUAL) {
		val = readl(hdmi->ps) & ~(HDMI_PS_TARGET | HDMI_PS_FLAGS);
		writel(val | HDMI_PS_TARGET, hdmi->ps);
		ret = readl_poll_timeout(hdmi->ps, val,
					 (val & HDMI_PS_ACTUAL) == HDMI_PS_ACTUAL,
					 10, HDMI_POWER_TIMEOUT_US);
		if (ret) {
			dev_err(disp->dev, "HDMI: PHY power state stays %#x\n", val);
			return ret;
		}
	}
	if (readl(hdmi->psd + HDMI_PSD_STATE) & 7) {
		writel(0, hdmi->psd + HDMI_PSD_TARGET);
		ret = readl_poll_timeout(hdmi->psd + HDMI_PSD_STATE, val, !(val & 7),
					 10, HDMI_POWER_TIMEOUT_US);
		if (ret) {
			dev_err(disp->dev, "HDMI: PHY PSD state stays %#x\n", val);
			return ret;
		}
	}
	return hdmi_pcon_power(disp, hdmi);
}

/* The impedance calibration: wait for 0x1c04 bit 0, take 0x1c24 bits 12:6. */
static void hdmi_impedance_calibration(struct t8152_display *disp,
				       struct t8152_hdmi *hdmi)
{
	u32 val;

	if (readl_poll_timeout(hdmi->core + 0x1c04, val, val & 1, 10, 1000000))
		dev_warn(disp->dev, "HDMI: impedance calibration timed out\n");
	hdmi_set_bits(hdmi, 0x1c00, 4, 0x3f0, 0x3f);
	val = readl(hdmi->core + 0x1c24);
	hdmi_set_bits(hdmi, 0x1c18, 15, 0x3f8000, (val >> 6) & 0x7f);
	hdmi_set_bits(hdmi, 0x1c18, 14, 0x4000, 1);
}

/*
 * The cold start, as the bootloader does it: power, source select, PLL, AUX
 * and PHY power, the tunables, the PLL and PHY activation, four lanes and
 * the impedance calibration. Once up, the link is kept on deactivate, as
 * for the bootloader's HDMI.
 */
static int hdmi_port_activate(struct t8152_display *disp)
{
	struct t8152_hdmi *hdmi = disp->phy_priv;
	int ret;

	if (hdmi->active)
		return 0;
	ret = hdmi_power_up(disp, hdmi);
	if (ret)
		return ret;
	/* The source select: only the low nibble */
	hdmi_apb_update(hdmi, HDMI_SOURCE_SELECT, 0xf, disp->dp_source);
	/* PLL, AUX and PHY power enables */
	hdmi_apb_update(hdmi, 0x48, 0x010, 0x010);
	usleep_range(1000, 2000);
	hdmi_apb_update(hdmi, 0x48, 0x020, 0x020);
	hdmi_apb_update(hdmi, 0x48, 0x040, 0);
	hdmi_apb_update(hdmi, 0x48, 0x100, 0x100);
	usleep_range(1000, 2000);
	hdmi_apb_update(hdmi, 0x48, 0x200, 0x200);
	hdmi_apb_update(hdmi, 0x48, 0x400, 0);
	hdmi_apb_update(hdmi, 0x48, 0x001, 0x001);
	usleep_range(1000, 2000);
	hdmi_apb_update(hdmi, 0x48, 0x002, 0x002);
	hdmi_apb_update(hdmi, 0x48, 0x004, 0);

	for (int i = 0; i + 3 <= hdmi->tunable_len; i += 3) {
		void __iomem *reg = hdmi->core + be32_to_cpu(hdmi->tunable[i]);

		writel((readl(reg) & ~be32_to_cpu(hdmi->tunable[i + 1])) |
		       be32_to_cpu(hdmi->tunable[i + 2]), reg);
	}

	hdmi_set_bits(hdmi, 0x08d0, 4, 0x10, 1);	/* single-ended PLL clock */
	hdmi_set_bits(hdmi, 0x0000, 1, 0x02, 1);	/* PLL on */
	hdmi_set_bits(hdmi, 0x1400, 0, 0x01, 0);	/* AUX common control */
	hdmi_set_bits(hdmi, 0x1c00, 4, 0x3f0, 0x3c);	/* PHY on */
	hdmi_set_bits(hdmi, 0x0000, 0, 0x01, 1);
	usleep_range(1000, 2000);
	hdmi_set_lanes(hdmi, HDMI_LANES);
	hdmi_impedance_calibration(disp, hdmi);
	hdmi->active = true;
	dev_info(disp->dev, "HDMI: PHY up from cold, source %u\n", disp->dp_source);
	return 0;
}

static const struct t8152_phy_ops hdmi_port_ops = {
	.activate = hdmi_port_activate,
	.deactivate = hdmi_deactivate,
	.set_link_rate = hdmi_set_link_rate,
	.set_lane_count = hdmi_set_lane_count,
	.set_drive = hdmi_set_drive,
	.set_route = hdmi_set_route,
	.read_edid = hdmi_read_edid,
	.max_link_rate = LINK_RATE_HBR3,
};

/* Registers shared with the other DCPs that may take the HDMI port. */
static void __iomem *hdmi_ioremap(struct t8152_display *disp,
				  struct device_node *np, const char *name)
{
	int index = of_property_match_string(np, "reg-names", name);
	struct resource res;

	if (index < 0 || of_address_to_resource(np, index, &res))
		return NULL;
	return devm_ioremap(disp->dev, res.start, resource_size(&res));
}

static void hdmi_put_smc(void *data)
{
	of_node_put(data);
}

static void hdmi_port_put(void *data)
{
	guard(mutex)(&hdmi_port_lock);
	if (!--hdmi_port_users) {
		gpiod_put(hdmi_port_hpd);
		hdmi_port_hpd = NULL;
	}
}

static struct gpio_desc *hdmi_port_get(struct t8152_display *disp,
				       struct device_node *np)
{
	struct gpio_desc *hpd;
	int ret;

	scoped_guard(mutex, &hdmi_port_lock) {
		if (!hdmi_port_users) {
			hpd = fwnode_gpiod_get_index(of_fwnode_handle(np), "hpd", 0,
						     GPIOD_ASIS, "hdmi-hpd");
			if (IS_ERR(hpd))
				return hpd;
			hdmi_port_hpd = hpd;
		}
		hdmi_port_users++;
		hpd = hdmi_port_hpd;
	}
	ret = devm_add_action_or_reset(disp->dev, hdmi_port_put, NULL);
	return ret ? ERR_PTR(ret) : hpd;
}

/*
 * The HDMI port of the shared ports, for every DCP: the PHY's source select
 * takes each one's crossbar endpoint. Without the cold start description in
 * np, HDMI stays the bootloader's.
 */
int t8152_hdmi_port_init(struct t8152_display *disp, struct device_node *np)
{
	struct t8152_hdmi *hdmi;
	int ret;

	if (!of_property_present(np, "apple,pcon-power"))
		return 0;
	hdmi = devm_kzalloc(disp->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;
	hdmi->core = hdmi_ioremap(disp, np, "phy-core");
	hdmi->apb = hdmi_ioremap(disp, np, "phy-apb");
	hdmi->ps = hdmi_ioremap(disp, np, "phy-ps");
	hdmi->psd = hdmi_ioremap(disp, np, "phy-psd");
	hdmi->aux.regs = hdmi_ioremap(disp, np, "aux");
	hdmi->aux.ps = hdmi_ioremap(disp, np, "aux-ps");
	/* AUX output select 0x10: the lpdptx PHY */
	hdmi->aux.sel = 0x10;
	if (!hdmi->core || !hdmi->apb || !hdmi->ps || !hdmi->psd)
		return -ENOMEM;
	if (!hdmi->aux.ps)
		hdmi->aux.regs = NULL;
	hdmi->pcon = of_get_property(np, "apple,pcon-power", &hdmi->pcon_len);
	hdmi->pcon_len /= sizeof(__be32);
	hdmi->tunable = of_get_property(np, "apple,tunable-phy",
					&hdmi->tunable_len);
	hdmi->tunable_len /= sizeof(__be32);
	hdmi->smc_np = of_parse_phandle(np, "apple,smc", 0);
	if (!hdmi->smc_np)
		return -EINVAL;
	ret = devm_add_action_or_reset(disp->dev, hdmi_put_smc, hdmi->smc_np);
	if (ret)
		return ret;
	/* Without the bootloader's HDMI nothing else powers the PS190. */
	ret = hdmi_pcon_power(disp, hdmi);
	if (ret)
		return ret;
	/* Every DCP that may take the port watches the same line. */
	hdmi->hpd = hdmi_port_get(disp, np);
	if (IS_ERR(hdmi->hpd))
		return PTR_ERR(hdmi->hpd);
	disp->hdmi_port = hdmi;
	dev_info(disp->dev, "HDMI: port shared, taken without a USB-C display\n");
	return 0;
}

/*
 * Take the HDMI port when a sink is plugged in and this DCP drives no USB-C
 * display; give it back once the sink is gone and the plug path has
 * released the link. Called from the connector's detect with the display
 * lock held.
 */
void t8152_hdmi_port_poll(struct t8152_display *disp)
{
	struct t8152_hdmi *hdmi = disp->hdmi_port;
	int hpd;

	if (!hdmi)
		return;
	hpd = gpiod_get_value_cansleep(hdmi->hpd);
	if (hpd < 0)
		return;
	guard(mutex)(&hdmi_port_lock);
	if (disp->on_hdmi) {
		if (hpd || disp->hpd)
			return;
		disp->phy = disp->usbc_phy;
		disp->phy_priv = disp->usbc_phy_priv;
		disp->dcp_output = disp->usbc_output;
		disp->hpd_gpio = NULL;
		disp->on_hdmi = false;
		hdmi_port_owner = NULL;
		dev_info(disp->dev, "HDMI: port given back\n");
		return;
	}
	if (!hpd || disp->hpd || hdmi_port_owner || !t8152_atc_release(disp))
		return;
	hdmi_port_owner = disp->dev;
	disp->usbc_phy = disp->phy;
	disp->usbc_phy_priv = disp->phy_priv;
	disp->usbc_output = disp->dcp_output;
	disp->phy = &hdmi_port_ops;
	disp->phy_priv = hdmi;
	/* display crossbar DFP endpoint lpdptx */
	disp->dcp_output = 4;
	disp->hpd_gpio = hdmi->hpd;
	disp->on_hdmi = true;
	dev_info(disp->dev, "HDMI: sink plugged, taking the HDMI port\n");
}

/* The DCP node that describes the HDMI PHY (DCPEXT0's). */
struct device_node *t8152_hdmi_node(void)
{
	struct device_node *np;

	for_each_compatible_node(np, NULL, "apple,t8152-dcp-iboot")
		if (of_property_read_bool(np, "apple,hdmi"))
			return np;
	return NULL;
}

static void __iomem *hdmi_map(struct device *dev, struct device_node *np,
			      const char *name)
{
	int index = of_property_match_string(np, "reg-names", name);

	if (index < 0)
		return IOMEM_ERR_PTR(index);
	return devm_of_iomap(dev, np, index, NULL);
}

static bool hdmi_power_active(struct device_node *np, const char *name)
{
	int index = of_property_match_string(np, "reg-names", name);
	void __iomem *regs;
	u32 state;

	if (index < 0)
		return false;
	regs = of_iomap(np, index);
	if (!regs)
		return false;
	state = readl(regs);
	iounmap(regs);
	/* Both the requested and actual PMGR states must be active. */
	return (state & GENMASK(7, 0)) == GENMASK(7, 0);
}

/*
 * The DCP the bootloader wired to the PHY (its source select: 0 for the main
 * DCP on the HDMI-only route, 1 for DCPEXT0 next to a USB-C display), or
 * -ENODEV if it left the PHY off or the PLL unlocked.
 */
int t8152_hdmi_source(struct device_node *np)
{
	int core = of_property_match_string(np, "reg-names", "phy-core");
	int apb = of_property_match_string(np, "reg-names", "phy-apb");
	void __iomem *core_regs, *apb_regs;
	int ret = -ENODEV;

	/* The PHY aperture aborts while gated, including on a USB-C-only boot. */
	if (core < 0 || apb < 0 || !hdmi_power_active(np, "phy-ps"))
		return -ENODEV;
	core_regs = of_iomap(np, core);
	apb_regs = of_iomap(np, apb);
	if (core_regs && apb_regs && readl(core_regs + 0x438) & BIT(30))
		ret = readl(apb_regs + HDMI_SOURCE_SELECT) & 0xf;
	if (apb_regs)
		iounmap(apb_regs);
	if (core_regs)
		iounmap(core_regs);
	if (ret == 1 && !hdmi_power_active(np, "coproc-ps"))
		return -ENODEV;
	if (ret != 0 && ret != 1)
		return -ENODEV;
	return ret;
}

/* The PHY, its AUX controller and its HPD line come from np. */
int t8152_hdmi_init(struct t8152_display *disp, struct device_node *np)
{
	struct t8152_hdmi *hdmi;

	hdmi = devm_kzalloc(disp->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;
	hdmi->core = hdmi_map(disp->dev, np, "phy-core");
	if (IS_ERR(hdmi->core))
		return PTR_ERR(hdmi->core);
	hdmi->apb = hdmi_map(disp->dev, np, "phy-apb");
	if (IS_ERR(hdmi->apb))
		return PTR_ERR(hdmi->apb);

	/* AUX output select 0x10: the lpdptx PHY */
	t8152_aux_probe(disp, &hdmi->aux, 0x10, np);

	dev_info(disp->dev, "HDMI: inherited source select %u, PLL %slocked\n",
		 readl(hdmi->apb + HDMI_SOURCE_SELECT) & 0xf,
		 readl(hdmi->core + 0x438) & BIT(30) ? "" : "not ");

	disp->phy = &hdmi_ops;
	disp->phy_priv = hdmi;
	/* display crossbar DFP endpoint lpdptx */
	disp->dcp_output = 4;
	return 0;
}
