/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple DCP display driver on the iBoot interface (T8152).
 *
 * The T8152 DCP firmware (AppleDCP-1317) offers a small "disp0-service", the
 * interface iBoot and aura use, next to the full IOMobileFramebuffer protocol.
 * This driver uses only that service, the DPTX port service the firmware
 * calls back into for the PHY and the DPAV controller services it expects to
 * be opened, all over AFKEPV2. It is a port of aura's src/afk.c,
 * src/dcp_iboot.c, src/dcp/dptx_port_ep.c and src/dcp/atc_phy.c.
 */
#ifndef __APPLE_DCPIB_H__
#define __APPLE_DCPIB_H__

#include <drm/drm_rect.h>
#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

struct dcpib;
struct dcpib_ep;
struct dcpib_service;
struct spmi_device;
struct drm_edid;

#define DCPIB_EP_IBOOT		0x23
#define DCPIB_EP_DPAV		0x24
#define DCPIB_EP_DPTX		0x2a
#define DCPIB_FIRST_EP		0x20
#define DCPIB_NR_EPS		0x10

#define DCPIB_MAX_SERVICES	8

/* IOReturn values the firmware understands. */
#define KIO_RETURN_SUCCESS	0
#define KIO_RETURN_ERROR	0xe00002bcU
#define KIO_RETURN_BAD_ARG	0xe00002c2U
#define KIO_RETURN_UNSUPPORTED	0xe00002c7U
#define KIO_RETURN_NOT_READY	0xe00002d8U

/* DPCD link rates in units of 0.27 Gbps */
#define DCPIB_LINK_RATE_RBR	0x06
#define DCPIB_LINK_RATE_HBR	0x0a
#define DCPIB_LINK_RATE_HBR2	0x14
#define DCPIB_LINK_RATE_HBR3	0x1e

struct dcpib_dma {
	void *cpu;
	dma_addr_t iova;
	size_t size;
};

struct dcpib_service_ops {
	/* Firmware-initiated standard service call; returns an IOReturn. */
	u32 (*call)(struct dcpib_service *service, u16 group, u32 command,
		    const void *data, size_t data_size, void *reply,
		    size_t reply_size);
};

struct dcpib_service {
	struct dcpib_ep *ep;
	const struct dcpib_service_ops *ops;
	char name[32];
	u16 interface;
	unsigned int unit;
	void *cookie;
};

struct dcpib_ring {
	bool ready;
	u32 *rptr;
	u32 *wptr;
	void *buf;
	u32 bufsz;
	u32 block;
};

struct dcpib_ep {
	struct dcpib *dcp;
	u8 ep;
	const struct dcpib_service_ops *ops;

	struct dcpib_dma ring;
	u16 tag;
	struct dcpib_ring tx, rx;
	struct dcpib_dma txbuf, rxbuf;

	struct completion started;
	struct completion stopped;
	struct work_struct rx_work;

	/* Serialises TX ring writes and the sequence number. */
	struct mutex tx_lock;
	u8 seq;

	struct dcpib_service services[DCPIB_MAX_SERVICES];
	unsigned int nr_services;
	unsigned int expected;
	struct completion services_done;

	/* One outstanding AP command at a time. */
	struct mutex cmd_lock;
	bool cmd_waiting;
	u16 cmd_interface;
	u8 cmd_type;
	u8 cmd_id;
	u8 next_cmd_id;
	void *cmd_out;
	size_t cmd_out_len;
	int cmd_ret;
	struct completion cmd_done;
};

struct dcpib_timing_mode {
	__le32 valid;
	__le32 width;
	__le32 height;
	__le32 fps;		/* 16.16 */
	u8 pad[8];
} __packed;

struct dcpib_color_mode {
	__le32 valid;
	__le32 colorimetry;
	__le32 eotf;
	__le32 encoding;
	__le32 bpp;
	u8 pad[4];
} __packed;

struct dcpib_drive_settings {
	__le32 format;
	__le32 voltage;
	__le32 pre_emphasis;
} __packed;

/* The phy behind the DCP's DPTX port. */
struct dcpib_phy_ops {
	int (*activate)(struct dcpib *dcp);
	int (*deactivate)(struct dcpib *dcp);
	int (*set_link_rate)(struct dcpib *dcp, u32 rate);	/* DPCD units */
	int (*set_lane_count)(struct dcpib *dcp, u32 lanes);	/* optional */
	int (*set_drive)(struct dcpib *dcp,
			 const struct dcpib_drive_settings *settings, u32 count);
	int (*set_route)(struct dcpib *dcp, bool enable);
	/* optional: plug orientation for the next activation */
	void (*set_orientation)(struct dcpib *dcp, bool flipped);
	/* optional: the sink's EDID, returns its length */
	int (*read_edid)(struct dcpib *dcp, u8 *buf, size_t size);
	/*
	 * optional: 1 with a sink on one of the phy's ports, 0 without; the
	 * port it found is taken by select() in the plug path
	 */
	int (*detect)(struct dcpib *dcp, bool *flipped);
	void (*select)(struct dcpib *dcp);
	u32 max_link_rate;					/* DPCD units */
	/* active pixels per second the path scans out, 0 if not limited */
	u64 min_pixel_rate, max_pixel_rate;
};

struct dcpib_port {
	struct dcpib_service *service;
	u32 link_rate;		/* DPCD units */
	u32 lanes;
	/* lanes of the last training, kept while the link is down */
	u32 trained_lanes;
	/* last drive settings the firmware set, for GET_DRIVE_SETTINGS */
	struct dcpib_drive_settings drive[4];
	u32 drive_count;
};

struct dcpib {
	/* Stop new endpoint work before detaching a failed live session. */
	struct mutex rx_gate;
	bool stopping;
	bool ready;
	bool kms_work_ready;
	struct device *dev;
	struct drm_device drm;
	struct apple_rtkit *rtk;
	struct workqueue_struct *wq;
	u32 inherited_afk_endpoints;

	struct dcpib_ep *eps[DCPIB_NR_EPS];
	struct dcpib_ep *iboot_ep;
	struct dcpib_ep *dpav_ep;
	/* the sink's EDID, read by the phy (NULL if unavailable) */
	const struct drm_edid *edid;
	struct dcpib_ep *dptx_ep;
	struct dcpib_service *iboot;
	struct dcpib_port port[2];

	/* The scanout DART's own device and domain (see dcpib_scanout_init). */
	struct platform_device *scanout;
	struct iommu_domain *scanout_dom;

	/*
	 * Scanout buffers (dcpib_boot_surface), each for the largest mode: a
	 * frame is copied into one not on screen, which SET_SURFACE then shows.
	 * Copying into the buffer being scanned out tore every frame. Three, so
	 * the one written next left the screen a frame ago.
	 */
#define DCPIB_SCANOUT_BUFS 3
	struct dcpib_dma scan[DCPIB_SCANOUT_BUFS];
	/* their pages write-combined: the pipe does not snoop the CPU caches */
	void *scan_wc[DCPIB_SCANOUT_BUFS];
	/* their addresses in the display pipe's DART (dcpib_scan_free_dva) */
	dma_addr_t scan_dva[DCPIB_SCANOUT_BUFS];
	/* scan[front] is on screen (or set as the surface next power on) */
	unsigned int front;
	/* the part of the current picture scan[i] does not hold yet */
	struct drm_rect stale[DCPIB_SCANOUT_BUFS];
	/* when scan[i] stopped being the requested surface */
	ktime_t retired[DCPIB_SCANOUT_BUFS];
	/* the last SET_SURFACE of a new buffer */
	ktime_t last_swap;
	/*
	 * A frame written within one refresh of the last swap waits in
	 * scan[pending] (-1: none) for swap_timer, at most one swap per
	 * refresh; a newer frame overwrites it.
	 */
	int pending;
	u32 pending_w, pending_h, pending_pitch;
	struct hrtimer swap_timer;
	struct work_struct swap_work;
	u32 boot_w, boot_h, boot_stride;

	const struct dcpib_phy_ops *phy;
	void *phy_priv;
	/* display-crossbar DFP endpoint the DCP drives */
	u32 dcp_output;
	/* optional sink HPD line (J873g HDMI), polled; last level seen */
	struct gpio_desc *hpd_gpio;
	int hpd;
	/* USB-C: the PD controller of the port in use, polled the same way */
	struct spmi_device *pd;
	/* HDMI phy: DCPEXT0, or the main DCP on the bootloader's HDMI-only route */
	bool hdmi;
	u32 die;

	/* iBoot state below, under lock */
	struct mutex lock;
	bool powered;
	struct dcpib_timing_mode *tmodes;
	unsigned int nr_tmodes;
	struct dcpib_color_mode *cmodes;
	unsigned int nr_cmodes;
	struct dcpib_timing_mode cur_mode;
	bool mode_set;
	/* SET_MODE of cur_mode failed; the next plug sets it again */
	bool mode_failed;
	/* USB-C: re-enter DP alt mode when the sink stopped answering */
	struct work_struct pd_reset_work;
	unsigned long pd_reset_time;
	/*
	 * The DCPAV video interface started: SET_MODE again, or the
	 * firmware's idle power off timer takes the link down 30 s later.
	 */
	struct delayed_work av_work;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct dcpib *to_dcpib(struct drm_device *drm)
{
	return container_of(drm, struct dcpib, drm);
}

/* afk.c */
struct dcpib_ep *dcpib_ep_start(struct dcpib *dcp, u8 ep,
				const struct dcpib_service_ops *ops,
				unsigned int expected);
void dcpib_ep_stop(struct dcpib_ep *ep);
void dcpib_ep_recv(struct dcpib_ep *ep, u64 message);
int dcpib_command(struct dcpib_service *service, u8 type, const void *tx,
		  size_t txlen, void *rx, size_t *rxlen);
int dcpib_service_call(struct dcpib_service *service, u16 group, u32 command,
		       const void *data, size_t data_len, size_t data_pad,
		       void *output, size_t output_len, size_t output_pad);
int dcpib_dma_alloc(struct dcpib *dcp, struct dcpib_dma *dma, size_t size);
void dcpib_dma_free(struct dcpib *dcp, struct dcpib_dma *dma);

/* iboot.c */
int dcpib_iboot_init(struct dcpib *dcp);

void dcpib_kms_stop(struct dcpib *dcp);
int dcpib_ib_set_power(struct dcpib *dcp, bool on);
int dcpib_ib_get_hpd(struct dcpib *dcp, u32 *timings, u32 *colors);
int dcpib_ib_get_modes(struct dcpib *dcp);
int dcpib_ib_set_mode(struct dcpib *dcp, const struct dcpib_timing_mode *t,
		      const struct dcpib_color_mode *c);
int dcpib_ib_set_surface(struct dcpib *dcp, u64 iova, u32 width, u32 height,
			 u32 stride);

/* drv.c */
int dcpib_wait_hpd(struct dcpib *dcp);

/* dptx.c */
int dcpib_dptx_init(struct dcpib *dcp);
int dcpib_dpav_init(struct dcpib *dcp);
int dcpib_dptx_connect(struct dcpib *dcp, unsigned int port);
int dcpib_dptx_set_hpd(struct dcpib *dcp, unsigned int port, bool hpd);

/* aux.c */
struct dcpib_aux {
	void __iomem *regs;
	void __iomem *ps;
	u32 sel;	/* output select of the core */
};

void dcpib_aux_probe(struct dcpib *dcp, struct dcpib_aux *aux, u32 sel);
void dcpib_aux_probe_node(struct dcpib *dcp, struct dcpib_aux *aux, u32 sel,
			  struct device_node *np);
int dcpib_aux_init(struct dcpib *dcp, struct dcpib_aux *aux);
int dcpib_aux_read_edid(struct dcpib *dcp, struct dcpib_aux *aux, u8 *buf,
			size_t size);

/* atc.c, hdmi.c */
int dcpib_atc_init(struct dcpib *dcp);
struct device_node *dcpib_hdmi_node(void);
int dcpib_hdmi_source(struct device_node *np);
int dcpib_hdmi_init(struct dcpib *dcp, struct device_node *np);

/* pd.c */
struct spmi_device *dcpib_pd_get(struct dcpib *dcp, struct device_node *np);
int dcpib_pd_sink(struct spmi_device *pd, bool *flipped);
int dcpib_pd_reconnect(struct dcpib *dcp);

/* kms.c */
int dcpib_kms_init(struct dcpib *dcp);
int dcpib_set_boot_mode(struct dcpib *dcp);
void dcpib_update_edid(struct dcpib *dcp);
void dcpib_reset_mode(struct dcpib *dcp);
void dcpib_av_started(struct dcpib *dcp);

#endif
