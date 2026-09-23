/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * DisplayPort over Thunderbolt on Apple silicon: the pieces that bring up a
 * DP tunnel live in the Thunderbolt glue, appledrm, the ATC PHY and the
 * display crossbar. They find each other with symbol_get(), so none of these
 * modules has to be built or loaded for the others to work.
 */
#ifndef _LINUX_SOC_APPLE_DP_TUNNEL_H_
#define _LINUX_SOC_APPLE_DP_TUNNEL_H_

#include <linux/types.h>

struct device_node;
struct mux_control;
struct phy;

/*
 * appledrm: route a display pipeline to (active) or away from (!active) the
 * crossbar output of DP IN adapter @dpin (0/1) of the router wired to
 * @connector_np. @set_active(@ctx, active) runs the DP IN adapter's
 * DPTX_INACTIVE handshake and is called back from DCP's link activation.
 */
int apple_dcp_tb_dp_tunnel(struct device_node *connector_np, unsigned int dpin,
			   bool active, int (*set_active)(void *ctx, bool active),
			   void *ctx);

/*
 * ATC PHY: start the pixel clock for a DP tunnel at DP link rate code @rate,
 * or stop it (@rate == 0). The PHY must be in Thunderbolt/USB4 mode.
 */
int apple_atc_dp_tunnel_rate(struct phy *phy, u8 rate);

/*
 * Display crossbar: take the connection of an already selected output down or
 * bring it back up (clock gates and enables) around a link reconfiguration.
 * The mux selection is left alone; link_down() also leaves the ATC output
 * enable set and link_up() re-asserts it. The caller keeps the output
 * selected (holds the mux) around both. t8103-style crossbars only.
 */
int apple_dpxbar_link_down(struct mux_control *mux);
int apple_dpxbar_link_up(struct mux_control *mux);

#endif
