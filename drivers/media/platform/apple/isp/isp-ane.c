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
/*
 * How the ISP firmware addresses memory it shares with another firmware:
 * the DART IOVA with bit 40 set. macOS 27.2 sends the rings so
 * (IPC_ENDPOINT_SET2 0x1000bd00000 / 0x1000bd08000 for IOVAs 0xbd00000 /
 * 0xbd08000), and the firmware's own SHAREDMALLOC list uses the same form
 * (0x1000ac80000). Without it (the first J815 run, 2026-11-21) the ANE never
 * saw the ISP's first command (GetProgramDescription) and the ISP firmware
 * asserted after 200 ms.
 */
#define ISP_ANE_SHARED_TAG BIT_ULL(40)
#define ISP_ALG_RAW_MLVNR 0x143d

/* macOS 27.2's auto mode on the J815 (firmware log "MLVNR MacOS Auto Mode"). */
#define ISP_MLVNR_CTRL_AUTO 2
#define ISP_MLVNR_GAIN_ENTER 0x0e00
#define ISP_MLVNR_GAIN_EXIT 0x0b00
#define ISP_MLVNR_FPS 30
#define ISP_MLVNR_RAMP 30

static bool mlvnr;
module_param(mlvnr, bool, 0644);
MODULE_PARM_DESC(mlvnr, "ML noise reduction on the neural engine (needs the ANE driver and apple/ispane-<board>.afpp; read at each firmware start)");

/*
 * TEST: the auto mode's gain thresholds (1/256 steps; enter >= exit, the
 * firmware asserts otherwise). macOS: 14x / 11x, so MLVNR runs only in dim
 * light. 256:256 makes it run in any light, to see it work. Read per stream.
 */
static unsigned int mlvnr_gain_enter = ISP_MLVNR_GAIN_ENTER;
module_param(mlvnr_gain_enter, uint, 0644);
MODULE_PARM_DESC(mlvnr_gain_enter, "TEST: MLVNR auto mode: switch on at this sensor gain, 1/256 steps (default 3584 = 14x, macOS)");
static unsigned int mlvnr_gain_exit = ISP_MLVNR_GAIN_EXIT;
module_param(mlvnr_gain_exit, uint, 0644);
MODULE_PARM_DESC(mlvnr_gain_exit, "TEST: MLVNR auto mode: switch off below this sensor gain, 1/256 steps, <= enter (default 2816 = 11x, macOS)");

/*
 * The frames the ISP firmware's MLVNR network takes (CMLVNRProcImp::
 * NetInputCheck, ISP firmware 26B131 adc-rheia): the input buffer's row
 * stride in bytes (16-bit samples, rows 64-byte aligned) must be one of
 * these with at least that many rows; anything else asserts the firmware
 * ("wrong input buffer size", CMLVNRProcImp.cpp:1046) and the stream stops.
 * The binned IMX958 mode (config 8, 1520 wide = 3072 bytes) is not among
 * them: MLVNR needs the full-resolution mode macOS streams (config 6,
 * 3040x2880 = 0x17c0 x 0xb40), sensor_config=6.
 */
static const struct {
	u16 stride, rows;
} isp_mlvnr_inputs[] = {
	{ 0x880, 0x780 }, { 0x900, 0x7e0 }, { 0xa80, 0x6e0 }, { 0xc40, 0x610 },
	{ 0xdc0, 0x530 }, { 0xf00, 0x438 }, { 0xfc0, 0x470 }, { 0x1280, 0x930 },
	{ 0x1780, 0xb30 }, { 0x17c0, 0xb38 }, { 0x1800, 0xbd0 }, { 0x20c0, 0xbc0 },
	{ 0x2100, 0xa50 },
};

bool isp_mlvnr_input_ok(u32 width, u32 height)
{
	u32 stride = ALIGN(width * 2, 64);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(isp_mlvnr_inputs); i++)
		if (isp_mlvnr_inputs[i].stride == stride)
			return height >= isp_mlvnr_inputs[i].rows;
	return false;
}

/* Every sensor preset this camera can stream must be one MLVNR takes. */
static bool isp_mlvnr_presets_ok(struct apple_isp *isp)
{
	int i;

	for (i = 0; i < isp->num_presets; i++) {
		const struct isp_preset *p = &isp->presets[i];

		if (!isp_mlvnr_input_ok(p->input_dim.x, p->input_dim.y)) {
			dev_info(isp->dev, "MLVNR: off: sensor mode %u (%ux%u, rows of %u bytes) is not a frame the network takes; it needs the full-resolution mode macOS uses (apple_isp.sensor_config=6)\n",
				 p->index, p->input_dim.x, p->input_dim.y,
				 ALIGN(p->input_dim.x * 2, 64));
			return false;
		}
	}
	return true;
}

static bool mlvnr_ring_tag = true;
module_param(mlvnr_ring_tag, bool, 0644);
MODULE_PARM_DESC(mlvnr_ring_tag, "Hand the MLVNR rings to the ISP firmware as shared memory (IOVA | bit 40), as macOS does (default on; read at each firmware start)");

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

/*
 * What the two firmwares left in the rings, for a stall: the ring head
 * (macOS: 0x3f40/0x7 at +0, read cursor at +0x40, write cursor at +0x80)
 * and the first record, 0x40-byte lines marked f0 f0 5a 5a.
 */
static void isp_ane_ring_dump(struct apple_isp *isp, struct isp_surf *rings)
{
	static const char *const names[] = { "read", "write" };
	unsigned int i, used;

	if (!rings->virt)
		return;
	for (i = 0; i < 2; i++) {
		const u8 *r = (const u8 *)rings->virt + i * ISP_ANE_RING_LEN;
		const u32 *w = (const u32 *)r;

		for (used = ISP_ANE_RING_LEN; used && !r[used - 1]; used--)
			;
		dev_info(isp->dev, "MLVNR: %s ring at stop: %u bytes used; head %08x %08x, +0x40 %08x %08x, +0x80 %08x %08x\n",
			 names[i], used, w[0], w[1], w[0x10], w[0x11], w[0x20], w[0x21]);
		if (used > 0xc0)
			print_hex_dump(KERN_INFO, "MLVNR ring: ", DUMP_PREFIX_OFFSET, 16, 4,
				       r + 0xc0, min(used - 0xc0, 0x400U), false);
	}
}

/*
 * The ANE firmware asks for the ISP firmware's shared surfaces by the ID this
 * driver gave in the SHAREDMALLOC reply (isp-ipc.c), and the ANE driver maps
 * them into the ANE DART: a reference on the surface until it is unmapped.
 * Only surfaces the firmware still holds; leaf calls (gc_lock only).
 */
static int isp_ane_surface_get(void *ctx, u32 id, struct ane_isp_surface *s)
{
	struct apple_isp *isp = ctx;
	struct isp_surf *surf;
	int err = -ENOENT;

	mutex_lock(&isp->gc_lock);
	list_for_each_entry(surf, &isp->gc, head) {
		if (surf->id != id)
			continue;
		if (refcount_inc_not_zero(&surf->refcount)) {
			s->sgt = &surf->sgt;
			s->size = surf->size;
			s->iova = surf->iova;
			s->tag = surf->type;
			s->cookie = surf;
			err = 0;
		}
		break;
	}
	mutex_unlock(&isp->gc_lock);
	return err;
}

static void isp_ane_surface_put(void *ctx, void *cookie)
{
	isp_put_surface(ctx, cookie);
}

/* After the firmware has started, before any stream. */
int apple_isp_ane_start(struct apple_isp *isp)
{
	struct ane_isp_endpoint ep = {
		.ring_len = ISP_ANE_RING_LEN,
		.surface_get = isp_ane_surface_get,
		.surface_put = isp_ane_surface_put,
		.surface_ctx = isp,
	};
	struct isp_surf *rings;
	int err;

	if (!READ_ONCE(mlvnr) || isp->ane_rings || isp->fw_recovering)
		return 0;
	if (!isp->ane_afpp[0] || !isp->hw->ane_doorbell_known)
		return 0;
	if (!isp_mlvnr_presets_ok(isp))
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

	/*
	 * Surfaces the firmware asks for from here on get IDs (only if the ANE
	 * driver maps by them: an older one would refuse the request and fail).
	 * macOS: the ISP firmware's AneClient allocates its 30 shared buffers
	 * right after IPC_ENDPOINT_SET2.
	 */
	isp->ane_ids_given = 0;
	if (!isp->ane_next_id)
		isp->ane_next_id = 0x100;
	WRITE_ONCE(isp->ane_ids, ep.surface_map);

	/* The read ring is the first half, the write ring the second. */
	isp->ane_ring_fw = rings->iova | (READ_ONCE(mlvnr_ring_tag) ? ISP_ANE_SHARED_TAG : 0);
	err = isp_cmd_ipc_endpoint_set2(isp, isp->ane_ring_fw, ISP_ANE_RING_LEN,
					isp->ane_ring_fw + ISP_ANE_RING_LEN, ISP_ANE_RING_LEN,
					ep.ane_doorbell, ep.ane_doorbell_sel);
	if (err) {
		dev_err(isp->dev, "MLVNR: ISP endpoint set failed (%d): off\n", err);
		WRITE_ONCE(isp->ane_ids, false);
		isp_ane.disable(isp->ane);
		goto free;
	}
	isp->ane_rings = rings;
	isp->ane_doorbell = ep.ane_doorbell;
	isp->ane_doorbell_sel = ep.ane_doorbell_sel;
	dev_info(isp->dev, "MLVNR: endpoint up: read ring %#llx, write ring %#llx (firmware form), ANE doorbell %#llx/%u; surface IDs %s\n",
		 isp->ane_ring_fw, isp->ane_ring_fw + ISP_ANE_RING_LEN, ep.ane_doorbell,
		 ep.ane_doorbell_sel, ep.surface_map ? "on" : "off (the ANE driver does not map them)");
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
	isp_ane_ring_dump(isp, rings);
	dev_info(isp->dev, "MLVNR: %u shared surfaces given IDs\n", isp->ane_ids_given);
	isp_cmd_ipc_endpoint_unset2(isp, isp->ane_ring_fw, ISP_ANE_RING_LEN,
				    isp->ane_ring_fw + ISP_ANE_RING_LEN, ISP_ANE_RING_LEN,
				    isp->ane_doorbell, isp->ane_doorbell_sel);
	WRITE_ONCE(isp->ane_ids, false);
	isp_ane.disable(isp->ane);
	isp_free_surface(isp, rings);
	isp->ane_rings = NULL;
}

/* Per stream, after the MBNR set-up, where macOS sends it. */
int apple_isp_ane_stream_start(struct apple_isp *isp, u32 ch)
{
	u32 enter, exit;
	int err;

	if (!isp->ane_rings)
		return 0;
	if (!isp->hw->mlvnr_mode_config)
		return isp_cmd_ch_algorithm_enable(isp, ch, ISP_ALG_RAW_MLVNR, 1);
	enter = READ_ONCE(mlvnr_gain_enter);
	exit = READ_ONCE(mlvnr_gain_exit);
	if (!enter || exit > enter) {
		enter = ISP_MLVNR_GAIN_ENTER;
		exit = ISP_MLVNR_GAIN_EXIT;
	}
	err = isp_cmd_apple_ch_mlvnr_mode_config_set(isp, ch, ISP_MLVNR_CTRL_AUTO,
						     enter, exit,
						     ISP_MLVNR_FPS, ISP_MLVNR_FPS,
						     ISP_MLVNR_RAMP);
	if (!err)
		dev_info(isp->dev, "MLVNR: auto mode on channel %u (gain enter %u, exit %u /256)\n",
			 ch, enter, exit);
	return err;
}
