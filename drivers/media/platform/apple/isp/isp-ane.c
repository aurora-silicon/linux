// SPDX-License-Identifier: GPL-2.0-only
/*
 * ML video noise reduction (MLVNR) on the platform neural engine.
 *
 * The ISP runs its learned noise reduction on the ANE. The ANE driver loads
 * the network (ispane.afpp) into the ANE; the two firmwares exchange work
 * over a pair of rings this driver allocates and maps into both IOMMUs, and
 * the ISP firmware learns about them with CISP_CMD_IPC_ENDPOINT_SET2. Then,
 * per stream, H16 enables algorithm 0x143d and H17 sets the auto mode macOS
 * uses (CISP_CMD_APPLE_CH_MLVNR_MODE_CONFIG_SET).
 *
 * Enabling MLVNR without a live endpoint stalls the stream, so it is only
 * enabled once every step here has succeeded. It is opt-in (mlvnr=1), the
 * ANE functions are taken with symbol_get() so this driver loads without
 * the ANE driver, and without the network, the ANE or its driver the camera
 * works as before.
 *
 * Based on Ryan Murray's H16 draft (camera/isp-ane-mlvnr).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/soc/apple/ane-isp.h>

#include "isp-ane.h"
#include "isp-cmd.h"
#include "isp-drv.h"
#include "isp-iommu.h"

#define ISP_ANE_RING_LEN 0x4000
#define ISP_ALG_RAW_MLVNR 0x143d

/* The auto mode macOS 27.2 sets on the J815, as the ISP firmware logs it. */
#define ISP_MLVNR_CTRL_AUTO 2
#define ISP_MLVNR_GAIN_ENTER 0x0e00
#define ISP_MLVNR_GAIN_EXIT 0x0b00
#define ISP_MLVNR_FPS 30
#define ISP_MLVNR_RAMP 30

static bool mlvnr;
module_param(mlvnr, bool, 0644);
MODULE_PARM_DESC(mlvnr, "ML noise reduction on the neural engine (needs the ANE driver and apple/ispane-<board>.afpp; read at each firmware start)");

/* One ISP per SoC: the ANE driver's functions, while this driver holds them. */
static struct {
	typeof(&ane_isp_link_get) get;
	typeof(&ane_isp_link_put) put;
	typeof(&ane_isp_endpoint_enable) enable;
	typeof(&ane_isp_endpoint_disable) disable;
} isp_ane;

static void isp_ane_syms_put(void)
{
	if (isp_ane.get)
		symbol_put(ane_isp_link_get);
	if (isp_ane.put)
		symbol_put(ane_isp_link_put);
	if (isp_ane.enable)
		symbol_put(ane_isp_endpoint_enable);
	if (isp_ane.disable)
		symbol_put(ane_isp_endpoint_disable);
	memset(&isp_ane, 0, sizeof(isp_ane));
}

static bool isp_ane_syms_get(void)
{
	if (isp_ane.get)
		return true;
	isp_ane.get = symbol_get(ane_isp_link_get);
	isp_ane.put = symbol_get(ane_isp_link_put);
	isp_ane.enable = symbol_get(ane_isp_endpoint_enable);
	isp_ane.disable = symbol_get(ane_isp_endpoint_disable);
	if (isp_ane.get && isp_ane.put && isp_ane.enable && isp_ane.disable)
		return true;
	isp_ane_syms_put();
	return false;
}

/* The network is per board: apple,j815 -> apple/ispane-j815.afpp. */
int apple_isp_ane_init(struct apple_isp *isp)
{
	const char *board, *model;

	if (!of_property_read_string_index(of_root, "compatible", 0, &model)) {
		board = strchr(model, ',');
		snprintf(isp->ane_afpp, sizeof(isp->ane_afpp), "apple/ispane-%s.afpp",
			 board ? board + 1 : model);
	}
	return 0;
}

void apple_isp_ane_exit(struct apple_isp *isp)
{
	apple_isp_ane_stop(isp);
	if (isp->ane)
		isp_ane.put(isp->ane);
	isp->ane = NULL;
	isp_ane_syms_put();
}

/*
 * The link, at the first firmware start with mlvnr=1 rather than at probe:
 * the ANE comes up on its own schedule and the camera must not wait for it.
 */
static int isp_ane_link(struct apple_isp *isp)
{
	struct device_node *np;
	struct ane_isp_link *link;

	if (isp->ane)
		return 0;
	if (!isp_ane_syms_get()) {
		dev_info(isp->dev, "MLVNR: the neural engine driver is not loaded\n");
		return -ENODEV;
	}
	np = of_parse_phandle(isp->dev->of_node, "apple,ane", 0);
	if (!np && isp->hw->ane_compatible)
		np = of_find_compatible_node(NULL, NULL, isp->hw->ane_compatible);
	if (!np)
		return -ENODEV;
	link = isp_ane.get(isp->dev, np);
	of_node_put(np);
	if (IS_ERR(link)) {
		dev_info(isp->dev, "MLVNR: no neural engine link (%ld)\n", PTR_ERR(link));
		return PTR_ERR(link);
	}
	isp->ane = link;
	return 0;
}

/* After the firmware has started, before any stream. */
int apple_isp_ane_start(struct apple_isp *isp)
{
	struct ane_isp_endpoint ep = { .ring_len = ISP_ANE_RING_LEN };
	struct isp_surf *rings;
	int err;

	if (!READ_ONCE(mlvnr) || isp->ane_rings)
		return 0;
	if (!isp->ane_afpp[0] || !isp->hw->ane_doorbell_known)
		return 0;
	if (isp_ane_link(isp))
		return 0;

	rings = __isp_alloc_surface(isp, 2 * ISP_ANE_RING_LEN, false);
	if (!rings)
		return 0;
	err = isp_surf_vmap(isp, rings);
	if (err)
		goto free;
	memset(rings->virt, 0, 2 * ISP_ANE_RING_LEN);

	ep.rings = &rings->sgt;
	ep.isp_doorbell = isp->hw->ane_isp_doorbell;
	ep.isp_doorbell_sel = isp->hw->ane_isp_doorbell_sel;
	err = isp_ane.enable(isp->ane, isp->ane_afpp, &ep);
	if (err) {
		dev_info(isp->dev, "MLVNR: neural engine endpoint not enabled (%d): off\n", err);
		goto free;
	}

	/* The read ring is the first half, the write ring the second. */
	err = isp_cmd_ipc_endpoint_set2(isp, rings->iova, ISP_ANE_RING_LEN,
					rings->iova + ISP_ANE_RING_LEN, ISP_ANE_RING_LEN,
					ep.ane_doorbell, ep.ane_doorbell_sel);
	if (err) {
		dev_err(isp->dev, "MLVNR: ISP endpoint set failed (%d): off\n", err);
		isp_ane.disable(isp->ane);
		goto free;
	}
	isp->ane_rings = rings;
	isp->ane_doorbell = ep.ane_doorbell;
	isp->ane_doorbell_sel = ep.ane_doorbell_sel;
	dev_info(isp->dev, "MLVNR: endpoint up: read ring %#llx, write ring %#llx, ANE doorbell %#llx/%u\n",
		 (u64)rings->iova, (u64)rings->iova + ISP_ANE_RING_LEN, ep.ane_doorbell,
		 ep.ane_doorbell_sel);
	return 0;

free:
	isp_free_surface(isp, rings);
	return 0; /* the camera works without it */
}

/* Before the firmware stops: endpoints down before the rings go away. */
void apple_isp_ane_stop(struct apple_isp *isp)
{
	struct isp_surf *rings = isp->ane_rings;

	if (!rings)
		return;
	isp_cmd_ipc_endpoint_unset2(isp, rings->iova, ISP_ANE_RING_LEN,
				    rings->iova + ISP_ANE_RING_LEN, ISP_ANE_RING_LEN,
				    isp->ane_doorbell, isp->ane_doorbell_sel);
	isp_ane.disable(isp->ane);
	isp_free_surface(isp, rings);
	isp->ane_rings = NULL;
}

/* Per stream, after the MBNR set-up, where macOS sends it. */
int apple_isp_ane_stream_start(struct apple_isp *isp, u32 ch)
{
	int err;

	if (!isp->ane_rings)
		return 0;
	if (!isp->hw->mlvnr_mode_config)
		return isp_cmd_ch_algorithm_enable(isp, ch, ISP_ALG_RAW_MLVNR, 1);
	err = isp_cmd_apple_ch_mlvnr_mode_config_set(isp, ch, ISP_MLVNR_CTRL_AUTO,
						     ISP_MLVNR_GAIN_ENTER, ISP_MLVNR_GAIN_EXIT,
						     ISP_MLVNR_FPS, ISP_MLVNR_FPS,
						     ISP_MLVNR_RAMP);
	if (!err)
		dev_info(isp->dev, "MLVNR: auto mode on channel %u\n", ch);
	return err;
}
