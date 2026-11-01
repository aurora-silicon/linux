// SPDX-License-Identifier: GPL-2.0-only
/*
 * ML video noise reduction on the platform neural engine.
 *
 * On H16-family ISPs (J616s, J700) the ISP runs its low-light ML noise
 * reduction (RawMLVNR, algorithm 0x143d) on the ANE. The ANE driver loads
 * the network into the ANE; the two firmwares exchange work over a pair of
 * rings this driver allocates and maps into both IOMMUs, and the ISP
 * firmware learns about them with CISP_CMD_IPC_ENDPOINT_SET2.
 *
 * Enabling 0x143d without a live endpoint stalls the stream, so the
 * algorithm is only enabled once every step here has succeeded. Without
 * the network or an ANE, the camera works as before.
 */
#include <linux/of.h>
#include <linux/string.h>
#include <linux/soc/apple/ane-isp.h>

#include "isp-ane.h"
#include "isp-cmd.h"
#include "isp-drv.h"
#include "isp-iommu.h"

#define ISP_ANE_RING_LEN 0x4000
#define ISP_ALG_RAW_MLVNR 0x143d

int apple_isp_ane_init(struct apple_isp *isp)
{
	const char *board, *model;
	struct device_node *np;

	np = of_parse_phandle(isp->dev->of_node, "apple,ane", 0);
	if (!np)
		return 0;
	/* the network is per board: apple,j700 -> apple/ispane-j700.afpp */
	if (!of_property_read_string_index(of_root, "compatible", 0, &model)) {
		board = strchr(model, ',');
		snprintf(isp->ane_afpp, sizeof(isp->ane_afpp), "apple/ispane-%s.afpp",
			 board ? board + 1 : model);
	}
	isp->ane = ane_isp_link_get(isp->dev, np);
	of_node_put(np);
	if (IS_ERR(isp->ane)) {
		int err = PTR_ERR(isp->ane);

		isp->ane = NULL;
		if (err == -EPROBE_DEFER)
			return err;
		dev_info(isp->dev, "no neural engine link (%d): ML noise reduction off\n", err);
	}
	return 0;
}

void apple_isp_ane_exit(struct apple_isp *isp)
{
	apple_isp_ane_stop(isp);
	if (isp->ane)
		ane_isp_link_put(isp->ane);
	isp->ane = NULL;
}

/* After the firmware has started, before any stream. */
int apple_isp_ane_start(struct apple_isp *isp)
{
	struct ane_isp_endpoint ep = { .ring_len = ISP_ANE_RING_LEN };
	struct isp_surf *rings;
	int err;

	if (!isp->ane || isp->ane_rings)
		return 0;
	if (!isp->ane_afpp[0] || !isp->hw->ane_doorbell_known)
		return 0;

	rings = __isp_alloc_surface(isp, 2 * ISP_ANE_RING_LEN, false);
	if (!rings)
		return -ENOMEM;
	err = isp_surf_vmap(isp, rings);
	if (err)
		goto free;
	memset(rings->virt, 0, 2 * ISP_ANE_RING_LEN);

	ep.rings = &rings->sgt;
	ep.isp_doorbell = isp->hw->ane_isp_doorbell;
	ep.isp_doorbell_sel = isp->hw->ane_isp_doorbell_sel;
	err = ane_isp_endpoint_enable(isp->ane, isp->ane_afpp, &ep);
	if (err) {
		dev_info(isp->dev, "neural engine endpoint not enabled (%d): ML noise reduction off\n",
			 err);
		goto free;
	}

	err = isp_cmd_ipc_endpoint_set2(isp, rings->iova, ISP_ANE_RING_LEN,
					rings->iova + ISP_ANE_RING_LEN, ISP_ANE_RING_LEN,
					ep.ane_doorbell, ep.ane_doorbell_sel);
	if (err) {
		dev_err(isp->dev, "ISP endpoint set failed: %d\n", err);
		ane_isp_endpoint_disable(isp->ane);
		goto free;
	}
	isp->ane_rings = rings;
	dev_info(isp->dev, "ML noise reduction available\n");
	return 0;

free:
	isp_free_surface(isp, rings);
	return 0; /* the camera works without it */
}

/* Before the firmware stops: endpoints down before the rings go away. */
void apple_isp_ane_stop(struct apple_isp *isp)
{
	if (!isp->ane_rings)
		return;
	isp_cmd_ipc_endpoint_unset2(isp);
	ane_isp_endpoint_disable(isp->ane);
	isp_free_surface(isp, isp->ane_rings);
	isp->ane_rings = NULL;
}

/* Per stream, before the AE start configuration. */
int apple_isp_ane_stream_start(struct apple_isp *isp, u32 ch)
{
	if (!isp->ane_rings)
		return 0;
	return isp_cmd_ch_algorithm_enable(isp, ch, ISP_ALG_RAW_MLVNR, 1);
}
