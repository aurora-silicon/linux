/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_HDMI_CONTROL_H
#define M3_HDMI_CONTROL_H
bool m3_hdmi_phy_ready(void);
int m3_hdmi_phy_deactivate(void);
int m3_hdmi_phy_activate(void);
int m3_hdmi_phy_set_rate(unsigned int rate);
int m3_hdmi_phy_get_rate(void);
int m3_hdmi_phy_set_drive(const unsigned int drive[12]);
int m3_hdmi_xbar_activate(void);
int m3_hdmi_xbar_deactivate(void);
struct m3_usbc_route_ops {
 int (*prepare)(unsigned int slot);
 int (*hpd)(unsigned int slot);
 int (*target)(unsigned int slot);
 int (*activate)(unsigned int slot);
 int (*deactivate)(unsigned int slot);
 int (*xbar_up)(unsigned int slot);
 int (*xbar_down)(unsigned int slot);
 int (*set_rate)(unsigned int slot, unsigned int rate);
 int (*get_rate)(unsigned int slot);
 int (*set_drive)(unsigned int slot, const unsigned int drive[12]);
};
const struct m3_usbc_route_ops *m3_usbc_get_ops(void);
#endif
