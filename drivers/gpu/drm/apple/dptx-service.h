/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DPTX_SERVICE_H
#define APPLE_DPTX_SERVICE_H

#include "afk.h"
#include "dptxep.h"

/*
 * T8140 external firmware names its one port by the DCP index; the others,
 * firmware 27 among them, by port unit 0 or 1.
 */
static inline int dptx_service_port(const char *name, const char *class,
				    s64 unit, bool unit_is_index, u32 index,
				    u32 num_ports)
{
	if (!name || !class || strcmp(name, "dcpdptx-port-epic") ||
	    strcmp(class, "AppleDCPDPTXRemotePort"))
		return -EINVAL;
	if (unit_is_index)
		return num_ports == 1 && unit == index ? 0 : -EINVAL;
	return unit >= 0 && unit < 2 ? unit : -EINVAL;
}

static inline bool dptx_service_available(struct dptx_port *port)
{
	/* Pairs with publication before readers use the service and its cookie. */
	return smp_load_acquire(&port->enabled);
}

static inline int dptx_service_bind(struct apple_epic_service *service,
				    struct dptx_port *port, s64 unit)
{
	if (port->service)
		return -EBUSY;
	port->unit = unit;
	/* No owner is published until OPEN and AFK publication succeed. */
	service->cookie = port;
	return 0;
}

static inline void dptx_service_publish(struct apple_epic_service *service)
{
	struct dptx_port *port = service->cookie;

	port->service = service;
	port->connected = false;
	/* Pair with dcp.c readers before they dereference the service cookie. */
	smp_store_release(&port->enabled, true);
}

static inline void dptx_service_unbind(struct apple_epic_service *service)
{
	struct dptx_port *port = service->cookie;

	if (port && port->service == service) {
		WRITE_ONCE(port->closed, true);
		/* Make terminal state visible to readers of availability. */
		smp_store_release(&port->enabled, false);
		WRITE_ONCE(port->connected, false);
		reinit_completion(&port->enable_completion);
	}
	/*
	 * Keep the published owner and cookie valid for retained callers. Once
	 * this session closes, bind fails closed until reboot:
	 * an in-flight connect must never switch to a new firmware session.
	 */
}

#endif
