/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define BIT(n) (1U << (n))
#define lockdep_assert_held(x) ((void)0)
#define list_for_each_entry(r, head, link) \
	for ((r) = *(head); (r); (r) = (r)->next)
/* CORE_HEADER */
struct mux_chip;
struct mux_control {
	struct mux_chip *chip;
};
/* Up to the DP-alt output and four DP IN outputs (the T8142 layout). */
struct mux_chip {
	struct mux_control mux[1 + DCP_FABRIC_DPIN_MAX];
	unsigned int controllers;
};
struct apple_connector {
	u32 candidate_crtcs;
};
struct apple_crtc {
	struct {
		unsigned int index;
	} base;
};
struct apple_dcp {
	struct dcp_fabric_pipeline fabric;
	struct apple_crtc *crtc;
	bool tb_retiring, external, clamshell, clamshell_free;
	void *fixed_phy, *active_typec_route;
};
struct apple_dcp_typec_route {
	struct apple_dcp_typec_route *next;
	struct apple_dcp *dcp;
	struct dcp_fabric_route core;
	bool tunnel;
	unsigned int tunnel_dpin, mux_index;
	struct mux_control *xbar, *dpin[DCP_FABRIC_DPIN_MAX];
};
struct apple_dcp_typec_port {
	struct dcp_fabric_port core;
	struct dcp_fabric_plan plan;
	struct apple_dcp_typec_route *routes, *owner, *secondary_owner,
		*preferred_route;
	struct apple_connector *connector, *secondary_connector;
	bool dp_wanted, dp_hpd, typec_yielded;
};
static bool t6030, t8132_ext;
static bool of_machine_is_compatible(const char *compatible)
{
	assert(!strcmp(compatible, "apple,t6030"));
	return t6030;
}
/* Production: of_device_is_compatible(dcp->dev->of_node, "apple,t8132-dcpext"). */
static bool dcp_is_t8132_ext(const struct apple_dcp *dcp)
{
	return t8132_ext;
}
#define drm_crtc_index(crtc) ((crtc)->index)
static bool dcp_typec_route_busy(struct apple_dcp_typec_route *r)
{
	return false;
}
static bool dcp_typec_tunnel_held(struct apple_dcp *d)
{
	return false;
}
static bool dcp_typec_safe_held(struct apple_dcp *d)
{
	return false;
}
static enum dcp_fabric_presence_state dcp_hdmi_presence(struct apple_dcp *d)
{
	return DCP_FABRIC_ABSENT;
}
static bool dcpext_scanout_terminal(struct apple_dcp *d)
{
	return false;
}
static bool dcp_tb_services_ready(struct apple_dcp *d)
{
	return true;
}
/* PRODUCTION_FUNCTIONS */
int main(void)
{
	struct mux_chip chip = { .controllers = 3 };
	struct apple_crtc crtc = { .base.index = 1 };
	struct apple_dcp dcp = { .crtc = &crtc };
	struct apple_dcp_typec_route source0 = { .dcp = &dcp,
						 .xbar = &chip.mux[0] };
	struct apple_dcp_typec_route source2 = {
		.dcp = &dcp,
		.xbar = &chip.mux[0],
		.mux_index = 2,
	};
	struct apple_dcp_typec_port port = { .routes = &source0 };
	source0.next = &source2;
	for (unsigned int i = 0; i < 1 + DCP_FABRIC_DPIN_MAX; i++)
		chip.mux[i].chip = &chip;
	for (unsigned int soc = 0; soc < 2; soc++) {
		t6030 = soc;
		for (unsigned int named = 0; named < 2; named++) {
			source0.dpin[0] = source2.dpin[0] =
				named ? &chip.mux[1] : NULL;
			source0.dpin[1] = source2.dpin[1] =
				named ? &chip.mux[2] : NULL;
			dcp_fabric_snapshot_port(&port, true);
			assert(port.core.routes == &source0.core);
			assert(source0.core.next == &source2.core);
			assert(!source2.core.next);
			assert(source0.core.tunnel_clock_blocked == 0);
			assert(source2.core.tunnel_clock_blocked == 0);
			assert(dcp_typec_tunnel_ctl(&source0, 0) ==
			       &chip.mux[1]);
			assert(dcp_typec_tunnel_ctl(&source0, 1) ==
			       &chip.mux[2]);
			assert(dcp_typec_tunnel_ctl(&source2, 0) ==
			       &chip.mux[1]);
			assert(dcp_typec_tunnel_ctl(&source2, 1) ==
			       &chip.mux[2]);
			/* A three-output crossbar has no DP IN 2/3 output. */
			assert(!dcp_typec_tunnel_ctl(&source0, 2));
			assert(!dcp_typec_tunnel_ctl(&source0, 3));
			assert(!dcp_typec_tunnel_ctl(&source0, DCP_FABRIC_DPIN_MAX));
			/* Late migration checks the real route, even with a stale snapshot. */
			source2.core.tunnel_clock_blocked = 0;
			assert(dcp_typec_tunnel_ctl(&source2, 0) ==
			       &chip.mux[1]);
		}
	}
	/* T6030 has no display-engine clock for a tunnel on source 1. */
	struct apple_dcp_typec_route source1 = {
		.dcp = &dcp,
		.xbar = &chip.mux[0],
		.mux_index = 1,
	};
	for (unsigned int soc = 0; soc < 2; soc++) {
		t6030 = soc;
		for (unsigned int dpin = 0; dpin < 2; dpin++)
			assert(dcp_typec_tunnel_ctl(&source1, dpin) ==
			       (soc ? NULL : &chip.mux[1 + dpin]));
	}
	/*
	 * A T8132 dcpext is a tunnel clock source as crossbar source 2 or 4,
	 * on either DP IN, never as 0, 1 or 3, whatever the machine.
	 */
	t8132_ext = true;
	for (unsigned int soc = 0; soc < 2; soc++) {
		t6030 = soc;
		for (unsigned int source = 0; source < 5; source++) {
			struct apple_dcp_typec_route r = {
				.dcp = &dcp,
				.xbar = &chip.mux[0],
				.mux_index = source,
			};
			struct apple_dcp_typec_port p = { .routes = &r };
			bool ok = source == 2 || source == 4;

			dcp_fabric_snapshot_port(&p, true);
			assert(p.core.routes == &r.core);
			assert(r.core.tunnel_clock_blocked == (ok ? 0 : 3));
			for (unsigned int dpin = 0; dpin < 2; dpin++)
				assert(dcp_typec_tunnel_ctl(&r, dpin) ==
				       (ok ? &chip.mux[1 + dpin] : NULL));
			assert(!dcp_typec_tunnel_ctl(&r, 2) && !dcp_typec_tunnel_ctl(&r, 3));
		}
	}
	t8132_ext = false;
	/*
	 * T6050: a T8142-layout crossbar has an output per DP IN, named or by
	 * the legacy order; a four-output one has no DP IN 3.
	 */
	t6030 = false;
	for (unsigned int outputs = 4; outputs <= 1 + DCP_FABRIC_DPIN_MAX; outputs++) {
		struct mux_chip xbar = { .controllers = outputs };
		struct apple_dcp_typec_route r = { .dcp = &dcp, .xbar = &xbar.mux[0] };

		for (unsigned int i = 0; i < 1 + DCP_FABRIC_DPIN_MAX; i++)
			xbar.mux[i].chip = &xbar;
		for (unsigned int named = 0; named < 2; named++) {
			for (unsigned int dpin = 0; dpin < DCP_FABRIC_DPIN_MAX; dpin++)
				r.dpin[dpin] = named && dpin + 1 < outputs ?
					&xbar.mux[1 + dpin] : NULL;
			for (unsigned int dpin = 0; dpin < DCP_FABRIC_DPIN_MAX; dpin++)
				assert(dcp_typec_tunnel_ctl(&r, dpin) ==
				       (dpin + 1 < outputs ? &xbar.mux[1 + dpin] : NULL));
			assert(!dcp_typec_tunnel_ctl(&r, DCP_FABRIC_DPIN_MAX));
		}
	}
	/* A display that yielded both pipes to an HDMI display wants none. */
	for (unsigned int yielded = 0; yielded < 2; yielded++) {
		port.dp_wanted = true;
		port.typec_yielded = yielded;
		dcp_fabric_snapshot_port(&port, true);
		assert(port.core.wanted == !yielded);
	}
	puts("PASS: DCP snapshot, named/legacy controls, invalid DP IN, stale snapshot, T8132 dcpext sources, T6050 four DP INs, yielded port");
	return 0;
}
