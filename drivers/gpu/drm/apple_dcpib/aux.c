// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The AP's DisplayPort AUX controllers aux0/aux1 (ADT dptx0-aux, dptx1-aux;
 * macOS AppleAUXDPTX): Analogix-derived DPTX cores with Apple's request
 * framing (AppleDPTXController::setupAuxTransaction, readDPCDGated). They
 * attach to the display crossbar as UFP endpoints 5 and 6; the phy code
 * routes one to its port for the time of a read. Used for the sink's EDID,
 * which the DCP firmware does not hand out on its iBoot interface.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include <drm/drm_edid.h>

#include "dcpib.h"

#define AUX_TIMEOUT_US	10000
#define AUX_SEL		0x000	/* output: 9 crossbar UFP aux0, 0x10 lpdptx phy */
#define AUX_FUNC_EN_2	0x01c
#define AUX_FUNC_EN_N	BIT(2)
#define AUX_HW_RETRY	0x390
#define AUX_INT_STA	0x3dc
#define AUX_REPLY	BIT(1)
#define AUX_ERROR	BIT(0)
#define AUX_CH_STA	0x780
#define AUX_DEFER_CTL	0x788
#define AUX_RX_COMM	0x78c
#define AUX_BUF_CTL	0x790
#define AUX_BUF_CLR	BIT(9)
#define AUX_BUF_COUNT	GENMASK(8, 0)
#define AUX_LEN		0x794
#define AUX_ADDR_7_0	0x798
#define AUX_ADDR_15_8	0x79c
#define AUX_CMD_ADDR	0x7a0	/* request in [7:4], address [19:16] */
#define AUX_CTL		0x7a4
#define AUX_GO		BIT(0)
#define AUX_ADDR_ONLY	BIT(1)
#define AUX_DATA	0xc00	/* one byte per word */
#define AUX_MAX		16

#define AUX_NATIVE_WRITE	0x80
#define AUX_I2C_WRITE	0x00
#define AUX_I2C_READ	0x10
#define AUX_I2C_MOT	0x40
#define DDC_ADDR	0x50
#define DPCD_SET_POWER	0x600
#define DP_SET_POWER_D0	1

static void aux_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | value, reg);
}

int dcpib_aux_init(struct dcpib *dcp, struct dcpib_aux *aux)
{
	u32 reg;

	/* PMGR GLOBAL_AUX_CTRLn */
	aux_update(aux->ps, 0xf, 0xf);
	if (readl_poll_timeout(aux->ps, reg, ((reg >> 4) & 0xf) == 0xf, 1,
			       AUX_TIMEOUT_US)) {
		dev_err(dcp->dev, "AUX: controller does not power up\n");
		return -ETIMEDOUT;
	}
	writel(AUX_REPLY | AUX_ERROR, aux->regs + AUX_INT_STA);
	aux_update(aux->regs + AUX_FUNC_EN_2, AUX_FUNC_EN_N, AUX_FUNC_EN_N);
	udelay(10);
	aux_update(aux->regs + AUX_FUNC_EN_2, AUX_FUNC_EN_N, 0);
	writel(3 << 8, aux->regs + AUX_HW_RETRY);
	writel(BIT(7) | 1, aux->regs + AUX_DEFER_CTL);
	aux_update(aux->regs + AUX_SEL, 0x3f, aux->sel);
	return 0;
}

/* One AUX request of up to 16 bytes; returns the bytes transferred. */
static int aux_xfer(struct dcpib_aux *aux, u32 cmd, u32 addr, u8 *buf,
			    u32 len, bool write)
{
	u32 sta, n;
	int i;

	for (int tries = 0; tries < 7; tries++) {
		writel(AUX_BUF_CLR, aux->regs + AUX_BUF_CTL);
		writel(len ? len - 1 : 0, aux->regs + AUX_LEN);
		writel(addr & 0xff, aux->regs + AUX_ADDR_7_0);
		writel((addr >> 8) & 0xff, aux->regs + AUX_ADDR_15_8);
		writel((cmd & 0xf0) | ((addr >> 16) & 0xf), aux->regs + AUX_CMD_ADDR);
		aux_update(aux->regs + AUX_CTL, AUX_ADDR_ONLY, len ? 0 : AUX_ADDR_ONLY);
		for (i = 0; write && i < len; i++)
			writel(buf[i], aux->regs + AUX_DATA + 4 * i);
		writel(AUX_REPLY | AUX_ERROR, aux->regs + AUX_INT_STA);
		aux_update(aux->regs + AUX_CTL, AUX_GO, AUX_GO);
		if (readl_poll_timeout(aux->regs + AUX_INT_STA, sta,
				       sta & (AUX_REPLY | AUX_ERROR), 10, 20000))
			return -ETIMEDOUT;
		writel(AUX_REPLY | AUX_ERROR, aux->regs + AUX_INT_STA);
		if ((sta & AUX_ERROR) || (readl(aux->regs + AUX_CH_STA) & 0xf))
			return -EIO;
		/* [3:2] I2C and [1:0] AUX reply: 0 ACK, 1 NACK, 2 DEFER */
		sta = readl(aux->regs + AUX_RX_COMM);
		if (sta & 0x5)
			return -ENXIO;
		if (sta & 0xa) {
			usleep_range(500, 1000);
			continue;
		}
		if (write)
			return len;
		n = min(readl(aux->regs + AUX_BUF_CTL) & AUX_BUF_COUNT, len);
		for (i = 0; i < n; i++)
			buf[i] = readl(aux->regs + AUX_DATA + 4 * i);
		return n;
	}
	return -EBUSY;
}

/* DDC over I2C-over-AUX: offset 0, then 16-byte reads up to @size. */
static int aux_ddc_read(struct dcpib_aux *aux, u8 *buf, size_t size)
{
	u8 offset = 0;
	size_t done = 0;
	int ret;

	ret = aux_xfer(aux, AUX_I2C_WRITE | AUX_I2C_MOT, DDC_ADDR,
			       &offset, 1, true);
	while (ret >= 0 && done < size) {
		ret = aux_xfer(aux, AUX_I2C_READ | AUX_I2C_MOT, DDC_ADDR,
				       buf + done, min_t(size_t, size - done, AUX_MAX),
				       false);
		if (ret > 0)
			done += ret;
		else if (!ret)
			ret = -EIO;
	}
	/* End the I2C transaction with an address-only read without MOT. */
	aux_xfer(aux, AUX_I2C_READ, DDC_ADDR, NULL, 0, false);
	return ret < 0 ? ret : done;
}

/*
 * The EDID over DDC, the base block and its extensions; the sink answers
 * AUX only some time after the path is up, and a sink just plugged in a
 * little later still.
 */
int dcpib_aux_read_edid(struct dcpib *dcp, struct dcpib_aux *aux, u8 *buf,
			size_t size)
{
	int ret = -EIO, ext;

	u8 d0 = DP_SET_POWER_D0;

	for (int i = 0; i < 30 && (ret == -EIO || ret == -ETIMEDOUT); i++) {
		msleep(i ? 100 : 20);
		/*
		 * A sink left in D3 (after an unplug) NACKs DDC: wake it
		 * first, as the DP spec asks before AUX use.
		 */
		ret = aux_xfer(aux, AUX_NATIVE_WRITE, DPCD_SET_POWER, &d0, 1, true);
		if (ret < 0)
			continue;
		usleep_range(1000, 2000);
		ret = aux_ddc_read(aux, buf, min_t(size_t, size, EDID_LENGTH));
	}
	if (ret != EDID_LENGTH)
		return ret < 0 ? ret : -EIO;
	ext = min_t(size_t, buf[0x7e], size / EDID_LENGTH - 1);
	return ext ? aux_ddc_read(aux, buf, (1 + ext) * EDID_LENGTH) : ret;
}

/* The optional "aux" and "aux-ps" (PMGR, mapped without claiming) regs. */
/* The controller named by np's "aux" and "aux-ps" regs (np need not be dcp's). */
void dcpib_aux_probe_node(struct dcpib *dcp, struct dcpib_aux *aux, u32 sel,
			  struct device_node *np)
{
	int ps = of_property_match_string(np, "reg-names", "aux-ps");
	int regs = of_property_match_string(np, "reg-names", "aux");
	struct resource res;

	aux->sel = sel;
	if (ps < 0 || regs < 0 || of_address_to_resource(np, ps, &res))
		return;
	aux->ps = devm_ioremap(dcp->dev, res.start, resource_size(&res));
	aux->regs = devm_of_iomap(dcp->dev, np, regs, NULL);
	if (IS_ERR(aux->regs) || !aux->ps)
		aux->regs = NULL;
}

void dcpib_aux_probe(struct dcpib *dcp, struct dcpib_aux *aux, u32 sel)
{
	dcpib_aux_probe_node(dcp, aux, sel, dcp->dev->of_node);
}
