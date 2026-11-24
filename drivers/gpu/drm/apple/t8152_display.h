/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The T8152 (Mac mini J873g) DisplayPort hardware behind a DCP's DPTX port
 * that no subsystem driver covers yet: the ATC PHYs' DisplayPort lanes and
 * their display crossbar route (t8152_atc.c), the HDMI port's dedicated
 * DisplayPort PHY and PS190 converter (t8152_hdmi.c), the AP's DisplayPort
 * AUX controllers that read the sink's EDID (t8152_aux.c), and the DPTX port
 * service the DCP firmware calls into for the PHY (t8152_dptx.c). The rear
 * USB-C ports' DisplayPort alt mode state comes from their Type-C port
 * driver, through the ports' shared display switches (t8152_typec.c).
 *
 * Interim: the ATC DisplayPort mode moves to drivers/phy/apple/atc.c and the
 * choice of port with DP alt mode to the Type-C route layer of dcp-fabric.c,
 * each in its own change. Until then these files program the registers
 * themselves, for the iBoot interface DCPs of iomfb_iboot.c alone.
 */
#ifndef __APPLE_T8152_DISPLAY_H__
#define __APPLE_T8152_DISPLAY_H__

#include <linux/types.h>

struct apple_dcp;
struct apple_epic_service_ops;
struct device;
struct device_node;
struct gpio_desc;
struct t8152_display;
struct t8152_hdmi;
struct t8152_typec;

/* SET_DRIVE_SETTINGS and GET_DRIVE_SETTINGS records, one per lane */
struct t8152_drive_settings {
	__le32 format;
	__le32 voltage;
	__le32 pre_emphasis;
} __packed;

/* The PHY behind the DCP's DPTX port; link rates in DPCD units. */
struct t8152_phy_ops {
	int (*activate)(struct t8152_display *disp);
	int (*deactivate)(struct t8152_display *disp);
	int (*set_link_rate)(struct t8152_display *disp, u32 rate);
	/* optional */
	int (*set_lane_count)(struct t8152_display *disp, u32 lanes);
	u32 (*max_lane_count)(struct t8152_display *disp);
	int (*set_drive)(struct t8152_display *disp,
			 const struct t8152_drive_settings *settings, u32 count);
	int (*set_route)(struct t8152_display *disp, bool enable);
	/* optional: plug orientation for the next activation */
	void (*set_orientation)(struct t8152_display *disp, bool flipped);
	/* optional: the sink's EDID, returns its length */
	int (*read_edid)(struct t8152_display *disp, u8 *buf, size_t size);
	/*
	 * optional: 1 with a sink on one of the PHY's ports, 0 without; the
	 * port it found is taken by select() in the plug path
	 */
	int (*detect)(struct t8152_display *disp, bool *flipped);
	void (*select)(struct t8152_display *disp);
	u32 max_link_rate;
};

/* The DPTX port state the firmware's calls set and read back. */
struct t8152_port {
	u32 link_rate;
	u32 lanes;
	/* the drive settings the firmware set last, for GET_DRIVE_SETTINGS */
	struct t8152_drive_settings drive[4];
	u32 drive_count;
};

/* An AP DisplayPort AUX controller, and its output select */
struct t8152_aux {
	void __iomem *regs;
	void __iomem *ps;
	u32 sel;
};

/* The DisplayPort side of one DCP */
struct t8152_display {
	struct apple_dcp *dcp;
	struct device *dev;
	/* display crossbar UFP endpoint of the DCP; DPTX port 0 */
	u32 dp_source;
	/* display crossbar DFP endpoint the DCP drives */
	u32 dcp_output;
	u32 die;

	const struct t8152_phy_ops *phy;
	void *phy_priv;
	/* optional sink HPD line (HDMI), polled; the last level seen */
	struct gpio_desc *hpd_gpio;
	int hpd;
	/* USB-C: plug and HPD come from the ports' display switches */
	bool usbc;
	struct t8152_typec *typec;
	/* HDMI PHY: DCPEXT0's, or the main DCP's on an HDMI-only boot */
	bool hdmi;
	/*
	 * The HDMI port of the shared ports when the bootloader brought HDMI
	 * up on no DCP: taken while this DCP drives no USB-C display, with
	 * the USB-C PHY kept to switch back.
	 */
	struct t8152_hdmi *hdmi_port;
	bool on_hdmi;
	const struct t8152_phy_ops *usbc_phy;
	void *usbc_phy_priv;
	u32 usbc_output;

	struct t8152_port port[2];
};

/* t8152_atc.c */
int t8152_atc_boot_link(struct device *dev);
int t8152_atc_init(struct t8152_display *disp);
bool t8152_atc_release(struct t8152_display *disp);

/* t8152_hdmi.c */
struct device_node *t8152_hdmi_node(void);
int t8152_hdmi_source(struct device_node *np);
int t8152_hdmi_init(struct t8152_display *disp, struct device_node *np);
int t8152_hdmi_port_init(struct t8152_display *disp, struct device_node *np);
void t8152_hdmi_port_poll(struct t8152_display *disp);

/* t8152_aux.c */
void t8152_aux_probe(struct t8152_display *disp, struct t8152_aux *aux,
		     u32 sel, struct device_node *np);
int t8152_aux_init(struct t8152_display *disp, struct t8152_aux *aux);
int t8152_aux_read_edid(struct t8152_display *disp, struct t8152_aux *aux,
			u8 *buf, size_t size);

/* t8152_typec.c, see also iomfb_iboot.h */
int t8152_typec_init(struct t8152_display *disp);
int t8152_typec_sink(struct t8152_display *disp, u32 index, bool *flipped,
		     u32 *lanes);

/* t8152_dptx.c */
extern const struct apple_epic_service_ops t8152_dptx_port_ops;
int t8152_dptx_connect(struct t8152_display *disp, unsigned int port);
int t8152_dptx_disconnect(struct t8152_display *disp, unsigned int port);
int t8152_dptx_set_hpd(struct t8152_display *disp, unsigned int port, bool hpd);

#endif
