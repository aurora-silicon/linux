// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* The DCP "disp0-service" iBoot interface (port of aura src/dcp_iboot.c). */

#include <linux/slab.h>
#include <linux/string.h>

#include "dcpib.h"

#define IBOOT_SET_SURFACE	1
#define IBOOT_SET_POWER		2
#define IBOOT_GET_HPD		3
#define IBOOT_GET_TIMING_MODES	4
#define IBOOT_GET_COLOR_MODES	5
#define IBOOT_SET_MODE		6

#define IBOOT_BUF_SIZE		0x4000

#define FMT_BGRA		1
#define ADDR_PLANAR		1
#define EOTF_GAMMA_SDR		1

struct iboot_txcmd {
	__le32 op;
	__le32 len;
	__le32 unk1;
	__le32 unk2;
} __packed;

struct iboot_rxcmd {
	__le32 op;
	__le32 len;
} __packed;

struct iboot_hpd {
	u8 hpd;
	u8 pad[3];
	__le32 timing_cnt;
	__le32 color_cnt;
} __packed;

struct iboot_plane {
	__le32 unk1;
	__le64 addr;
	__le32 tile_size;
	__le32 stride;
	__le32 unk2[4];
	__le32 addr_format;
	__le32 unk3;
} __packed;

struct iboot_layer {
	struct iboot_plane planes[3];
	__le32 unk;
	__le32 plane_cnt;
	__le32 width;
	__le32 height;
	__le32 surface_fmt;
	__le32 colorspace;
	__le32 eotf;
	u8 transform;
	u8 padding[3];
} __packed;

static int dcpib_ib_disp0_init(struct dcpib *dcp)
{
	struct dcpib_ep *ep = dcp->iboot_ep;

	for (unsigned int i = 0; i < ep->nr_services; i++) {
		if (!strcmp(ep->services[i].name, "disp0-service") ||
		    !strcmp(ep->services[i].name, "dispext-service")) {
			dcp->iboot = &ep->services[i];
			return 0;
		}
	}
	return -ENODEV;
}

int dcpib_iboot_init(struct dcpib *dcp)
{
	struct dcpib_ep *ep = dcpib_ep_start(dcp, DCPIB_EP_IBOOT, NULL, 1);

	if (IS_ERR(ep))
		return PTR_ERR(ep);
	dcp->iboot_ep = ep;
	return dcpib_ib_disp0_init(dcp);
}

/*
 * The AFKEPV2 setters answer with transport status alone: supplying a reply
 * buffer makes the firmware try to copy an empty message and assert.
 */
static int dcpib_ib_cmd(struct dcpib *dcp, u32 op, const void *in,
			size_t in_size, void *out, size_t out_cap,
			size_t min_size)
{
	struct iboot_txcmd *tx;
	struct iboot_rxcmd *rx = NULL;
	bool setter = op == IBOOT_SET_POWER || op == IBOOT_SET_MODE ||
		      op == IBOOT_SET_SURFACE;
	size_t rxlen = IBOOT_BUF_SIZE;
	int ret;

	if (!dcp->iboot)
		return -ENODEV;
	tx = kzalloc(sizeof(*tx) + in_size, GFP_KERNEL);
	if (!tx)
		return -ENOMEM;
	tx->op = cpu_to_le32(op);
	tx->len = cpu_to_le32(sizeof(*tx) + in_size);
	if (in_size)
		memcpy(tx + 1, in, in_size);

	if (!setter) {
		rx = kzalloc(rxlen, GFP_KERNEL);
		if (!rx) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = dcpib_command(dcp->iboot, 0xc0, tx, sizeof(*tx) + in_size, rx,
			    setter ? NULL : &rxlen);
	if (ret || setter)
		goto out;

	if (rxlen < sizeof(*rx) || le32_to_cpu(rx->len) < sizeof(*rx) ||
	    le32_to_cpu(rx->len) > rxlen) {
		dev_err(dcp->dev, "iboot: bad response length for op %u\n", op);
		ret = -EPROTO;
		goto out;
	}
	rxlen = le32_to_cpu(rx->len) - sizeof(*rx);
	if (rxlen < min_size) {
		ret = -EPROTO;
		goto out;
	}
	memcpy(out, rx + 1, min(rxlen, out_cap));
out:
	kfree(rx);
	kfree(tx);
	return ret;
}

int dcpib_ib_set_power(struct dcpib *dcp, bool on)
{
	__le32 power = cpu_to_le32(on);

	/* The command carries one byte of payload. */
	return dcpib_ib_cmd(dcp, IBOOT_SET_POWER, &power, 1, NULL, 0, 0);
}

int dcpib_ib_get_hpd(struct dcpib *dcp, u32 *timings, u32 *colors)
{
	struct iboot_hpd hpd;
	int ret = dcpib_ib_cmd(dcp, IBOOT_GET_HPD, NULL, 0, &hpd, sizeof(hpd),
			       sizeof(hpd));

	if (ret)
		return ret;
	if (timings)
		*timings = le32_to_cpu(hpd.timing_cnt);
	if (colors)
		*colors = le32_to_cpu(hpd.color_cnt);
	return !!hpd.hpd;
}

static int dcpib_ib_get_list(struct dcpib *dcp, u32 op, size_t entry,
			     void **list, unsigned int *count)
{
	u8 *buf;
	u32 n;
	int ret;

	buf = kzalloc(IBOOT_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = dcpib_ib_cmd(dcp, op, NULL, 0, buf, IBOOT_BUF_SIZE, sizeof(__le32));
	if (ret)
		goto out;
	/* The response holds a count followed by the entries. */
	n = le32_to_cpu(*(__le32 *)buf);
	if (n > (IBOOT_BUF_SIZE - sizeof(__le32)) / entry) {
		ret = -EPROTO;
		goto out;
	}
	kfree(*list);
	*list = kmemdup(buf + sizeof(__le32), n * entry, GFP_KERNEL);
	*count = *list ? n : 0;
	ret = *list ? 0 : -ENOMEM;
out:
	kfree(buf);
	return ret;
}

int dcpib_ib_get_modes(struct dcpib *dcp)
{
	int ret;

	ret = dcpib_ib_get_list(dcp, IBOOT_GET_TIMING_MODES,
				sizeof(struct dcpib_timing_mode),
				(void **)&dcp->tmodes, &dcp->nr_tmodes);
	if (ret)
		return ret;
	return dcpib_ib_get_list(dcp, IBOOT_GET_COLOR_MODES,
				 sizeof(struct dcpib_color_mode),
				 (void **)&dcp->cmodes, &dcp->nr_cmodes);
}

int dcpib_ib_set_mode(struct dcpib *dcp, const struct dcpib_timing_mode *t,
		      const struct dcpib_color_mode *c)
{
	dev_dbg(dcp->dev, "SET_MODE %ux%u fps %#x, bpp %u\n",
		le32_to_cpu(t->width), le32_to_cpu(t->height),
		le32_to_cpu(t->fps), le32_to_cpu(c->bpp));
	struct {
		struct dcpib_timing_mode t;
		struct dcpib_color_mode c;
	} __packed cmd = { *t, *c };

	return dcpib_ib_cmd(dcp, IBOOT_SET_MODE, &cmd, sizeof(cmd), NULL, 0, 0);
}

int dcpib_ib_set_surface(struct dcpib *dcp, u64 iova, u32 width, u32 height,
			 u32 stride)
{
	dev_dbg(dcp->dev, "SET_SURFACE %#llx %ux%u stride %u\n", iova, width,
		height, stride);
	struct iboot_layer layer = {
		.planes = { {
			.addr = cpu_to_le64(iova),
			.stride = cpu_to_le32(stride),
			.addr_format = cpu_to_le32(ADDR_PLANAR),
		} },
		.plane_cnt = cpu_to_le32(1),
		.width = cpu_to_le32(width),
		.height = cpu_to_le32(height),
		.surface_fmt = cpu_to_le32(FMT_BGRA),
		.colorspace = cpu_to_le32(2),
		.eotf = cpu_to_le32(EOTF_GAMMA_SDR),
	};

	return dcpib_ib_cmd(dcp, IBOOT_SET_SURFACE, &layer, sizeof(layer),
			    NULL, 0, 0);
}
