/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_HDCP_DRV_H__
#define __APPLE_DCP_HDCP_DRV_H__

#include <linux/kconfig.h>
#include <linux/types.h>

struct apple_connector;
struct apple_dcp;
struct apple_dcp_afkep;
struct apple_epic_service_ops;
struct drm_atomic_commit;
struct drm_connector;
struct platform_device;

/*
 * HDCP 2.2 on the display processors' external outputs: the DCP services
 * (dcp-hdcp-wire.h), the transmitter (dcp-hdcp-tx.h) and the Content
 * Protection policy (dcp-hdcp.h), joined to the connector properties.
 */
/* Whether something already drives @endpoint (dcp.c). */
bool dcp_endpoint_routed(struct apple_dcp *dcp, u8 endpoint);

#if IS_ENABLED(CONFIG_DRM_APPLE_HDCP)
/* A display processor's HDCP state, if it has an HDCP channel; idempotent. */
struct dcp_hdcp *dcp_hdcp_init(struct apple_dcp *dcp);
void dcp_hdcp_fini(struct apple_dcp *dcp);
/* Starts the DP device and HDCP interface endpoints when no other path did. */
void dcp_hdcp_start_endpoints(struct apple_dcp *dcp);
/* Routes the endpoints this file started. */
struct apple_dcp_afkep *dcp_hdcp_route(struct apple_dcp *dcp, u32 endpoint);
/* The HDCP services, matched by name ahead of a catch-all. */
const struct apple_epic_service_ops *
dcp_hdcp_match_service(struct apple_dcp_afkep *ep, const char *service_name,
		       bool cut);
/* Hotplug, power off or on, suspend, a new route: a new link generation. */
void dcp_hdcp_link_changed(struct apple_dcp *dcp);

int dcp_hdcp_attach_connector(struct apple_connector *connector,
			      struct platform_device **dcps, unsigned int n,
			      int connector_type);
int dcp_hdcp_atomic_check(struct drm_connector *conn,
			   struct drm_atomic_commit *state);
void dcp_hdcp_commit(struct drm_atomic_commit *state);
#else
static inline struct dcp_hdcp *dcp_hdcp_init(struct apple_dcp *dcp) { return NULL; }
static inline void dcp_hdcp_fini(struct apple_dcp *dcp) { }
static inline void dcp_hdcp_start_endpoints(struct apple_dcp *dcp) { }
static inline struct apple_dcp_afkep *dcp_hdcp_route(struct apple_dcp *dcp,
						     u32 endpoint)
{
	return NULL;
}

static inline const struct apple_epic_service_ops *
dcp_hdcp_match_service(struct apple_dcp_afkep *ep, const char *service_name,
		       bool cut)
{
	return NULL;
}

static inline void dcp_hdcp_link_changed(struct apple_dcp *dcp) { }
static inline int dcp_hdcp_attach_connector(struct apple_connector *connector,
					    struct platform_device **dcps,
					    unsigned int n, int connector_type)
{
	return 0;
}

static inline int dcp_hdcp_atomic_check(struct drm_connector *conn,
					struct drm_atomic_commit *state)
{
	return 0;
}
static inline void dcp_hdcp_commit(struct drm_atomic_commit *state) { }
#endif

#endif /* __APPLE_DCP_HDCP_DRV_H__ */
