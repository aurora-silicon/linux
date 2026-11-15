// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/device.h>
#include <linux/string.h>
#include <drm/drm_edid.h>
#include "connector.h"
#include "afk.h"
#include "dcp.h"
#include "parser.h"
#include "dcp-180hz.h"
#include "dcp-180hz-policy.h"

static bool dcp_180hz_direct_port(struct apple_epic_service *service)
{
	struct apple_dcp *dcp;
	struct dptx_port *port;

	if (!service || !service->ep || !service->cookie)
		return false;
	dcp = service->ep->dcp;
	port = service->cookie;
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL &&
		service->ep->endpoint == DPTX_ENDPOINT_V2 && service->ep->wire_v2 &&
		port == &dcp->dptxport[0] && READ_ONCE(port->enabled) &&
		!READ_ONCE(port->closed) && READ_ONCE(port->service) == service &&
		READ_ONCE(service->enabled) && port->atcphy && port->atcphy == dcp->phy &&
		dcp->active_typec_route && !dcp->dptx_tunnel && !dcp_is_usb4_output(dcp);
}

void dcp_180hz_invalidate(struct apple_dcp *dcp)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->rate_proof.sequence++;
	dcp->rate_proof.rate.valid = false;
	dcp->rate_proof.lanes.valid = false;
	dcp->rate_proof.retries = 0;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

struct dcp_180hz_token dcp_180hz_config_begin(struct apple_epic_service *service, bool lanes)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_180hz_token token = {};
	unsigned long flags;

	/* RX callbacks never acquire lifecycle/HPD/AV mutexes. */
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	token.generation = READ_ONCE(dcp->typec_generation);
	token.sequence = dcp->rate_proof.sequence;
	token.connection_sequence = dcp->rate_proof.connection_sequence;
	token.phy = dcp->phy;
	token.route = dcp->active_typec_route;
	token.service = service;
	token.valid = !READ_ONCE(dcp->quiescing) && dcp_180hz_direct_port(service) &&
		READ_ONCE(dcp->typec_cable_connected) &&
		token.generation == dcp->metadata.epoch.generation;
	if (lanes)
		dcp->rate_proof.lanes.valid = false;
	else
		dcp->rate_proof.rate.valid = false;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	return token;
}

/* Preserve the receipt-start registry ticket even when it awaited OPEN. */
static bool dcp_180hz_timing_epoch(const struct dcp_display_mode *mode, u64 epoch)
{
	if (mode->automatic_service_pending)
		return mode->unsafe_rgb8_service_epoch != U64_MAX &&
			epoch == mode->unsafe_rgb8_service_epoch + 1;
	return epoch && mode->unsafe_rgb8_service_epoch == epoch;
}

void dcp_180hz_config_complete(struct apple_epic_service *service, bool lanes,
			      struct dcp_180hz_token token, u32 value, bool success)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_180hz_fact *fact;
	bool notify = false;
	unsigned long flags;

	if (!success) {
		dcp_180hz_invalidate(dcp);
		return;
	}
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (token.valid && token.service == service &&
	    token.sequence == dcp->rate_proof.sequence &&
	    token.generation == READ_ONCE(dcp->typec_generation) &&
	    token.connection_sequence == dcp->rate_proof.connection_sequence &&
	    READ_ONCE(dcp->typec_cable_connected) &&
	    token.phy == dcp->phy && token.route == dcp->active_typec_route &&
	    dcp_180hz_direct_port(service) && !READ_ONCE(dcp->quiescing)) {
		fact = lanes ? &dcp->rate_proof.lanes : &dcp->rate_proof.rate;
		*fact = (struct dcp_180hz_fact) { .token = token, .value = value, .valid = true };
		notify = dcp->rate_proof.rate.valid && dcp->rate_proof.lanes.valid;
		dcp->rate_proof.retries = 0;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (notify)
		dcp_180hz_notify(dcp);
}

static bool dcp_180hz_fact_current(struct apple_dcp *dcp, const struct dcp_180hz_fact *fact)
{
	const struct dcp_180hz_token *t = &fact->token;

	return fact->valid && t->generation == dcp->typec_generation &&
		t->sequence == dcp->rate_proof.sequence &&
		t->connection_sequence == dcp->rate_proof.connection_sequence && t->phy == dcp->phy &&
		t->route == dcp->active_typec_route && t->service == dcp->dptxport[0].service;
}

/* Catalog withdrawal needs a DRM refresh even when no exception is ready. */
void dcp_180hz_catalog_changed(struct apple_dcp *dcp)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);

	if (dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17P_EXTERNAL ||
	    !con || READ_ONCE(dcp->quiescing) || !READ_ONCE(con->connected) ||
	    READ_ONCE(con->dcp) != to_platform_device(dcp->dev) ||
	    READ_ONCE(con->base.registration_state) != DRM_CONNECTOR_REGISTERED)
		return;
	schedule_work(&con->hotplug_wq);
}

/* Timing and configuration can arrive in either order, before AV admission. */
void dcp_180hz_notify(struct apple_dcp *dcp)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	ready = dcp->metadata.epoch.connected && READ_ONCE(dcp->typec_cable_connected) &&
		!READ_ONCE(dcp->quiescing) && READ_ONCE(dcp->nr_modes) &&
		dcp_180hz_direct_port(dcp->dptxport[0].service) &&
		dcp_180hz_fact_current(dcp, &dcp->rate_proof.rate) &&
		dcp_180hz_fact_current(dcp, &dcp->rate_proof.lanes) &&
		dcp_180hz_payload(dcp->rate_proof.rate.value, dcp->rate_proof.lanes.value, 414090);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (ready && con && READ_ONCE(con->connected) && READ_ONCE(dcp->dptxport[0].connected) &&
	    READ_ONCE(con->dcp) == to_platform_device(dcp->dev) &&
	    READ_ONCE(con->base.registration_state) == DRM_CONNECTOR_REGISTERED)
		schedule_work(&con->hotplug_wq);
}

/*
 * Everything the exception mode needs except the EDID, which only the next
 * probe reads: a catalogue received for the current cable and connection,
 * the AV service of that receipt, an announced display and the configured
 * link rate and lane count.
 */
static bool dcp_180hz_link_current(struct apple_dcp *dcp, const struct dcp_display_mode *mode)
{
	struct apple_connector *con = dcp->connector;

	lockdep_assert_held(&dcp->hpd_mutex);
	lockdep_assert_held(&dcp->dcpavserv.lock);
	lockdep_assert_held(&dcp->metadata.lock);
	return con && dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	       dcp_180hz_direct_port(dcp->dptxport[0].service) &&
	       mode->unsafe_rgb8_ticket.associated &&
	       mode->unsafe_rgb8_ticket.generation == dcp->typec_generation &&
	       mode->automatic_connection_sequence == dcp->rate_proof.connection_sequence &&
	       mode->automatic_catalog_sequence == dcp->rate_proof.catalog_sequence &&
	       dcp->metadata.epoch.connected && dcp->typec_cable_connected && !dcp->quiescing &&
	       READ_ONCE(con->dcp) == to_platform_device(dcp->dev) &&
	       dcp_180hz_timing_epoch(mode, dcp->dcpavserv.epoch) &&
	       dcp->dptxport[0].connected && dcp->dptxport[0].enabled && !dcp->dptxport[0].closed &&
	       dcp_180hz_fact_current(dcp, &dcp->rate_proof.rate) &&
	       dcp_180hz_fact_current(dcp, &dcp->rate_proof.lanes) &&
	       dcp_180hz_payload(dcp->rate_proof.rate.value, dcp->rate_proof.lanes.value,
				 mode->mode.clock) &&
	       mode->sdr_rgb.depth == 8 && mode->sdr_rgb.format == DCP_COLOR_FORMAT_RGB &&
	       mode->sdr_rgb.range == DCP_COLOR_YCBCR_RANGE_FULL &&
	       mode->sdr_rgb.eotf == DCP_EOTF_SDR_GAMMA &&
	       mode->sdr_rgb.colorimetry == DCP_COLORIMETRY_SRGB;
}

bool dcp_180hz_mode_current(struct apple_dcp *dcp, const struct dcp_display_mode *mode)
{
	struct apple_connector *con = dcp->connector;
	const struct edid *raw;

	if (!dcp_180hz_link_current(dcp, mode) ||
	    !dcpavserv_edid_service_current(dcp, con->drm_edid_service_epoch) ||
	    !con->drm_edid || con->drm_edid_firmware_derived ||
	    READ_ONCE(con->edid_invalidation_pending) ||
	    con->drm_edid_generation != dcp->typec_generation)
		return false;
	raw = drm_edid_raw(con->drm_edid);
	/* The transport validates extent/checksums; this quirk accepts only its 256-byte form. */
	return raw && raw->extensions == 1 &&
		dcp_180hz_edid((const u8 *)raw, 256, &mode->mode);
}

/* Caller holds metadata.lock. */
static bool dcp_catalog_current(struct apple_dcp *dcp)
{
	const struct dcp_metadata_ticket *ticket = &dcp->metadata.catalog_ticket;

	return ticket->associated && dcp->nr_modes &&
	       ticket->generation == dcp->typec_generation &&
	       ticket->generation == dcp->metadata.epoch.generation &&
	       dcp->metadata.catalog_connection_sequence == dcp->rate_proof.connection_sequence;
}

/**
 * dcp_external_modes_ready - whether a probe would list the current modes
 * @dcp: an external H17P DCP
 *
 * The modes of an external display arrive in pieces, in no fixed order: the
 * TimingElements catalogue, the AV service the EDID is read from, the
 * firmware's announcement and the link configuration.  A connection is
 * ready once the catalogue is the current cable's, its EDID service is up
 * and, if the catalogue holds an exception mode, that mode passes every
 * check of dcp_180hz_mode_current() but the EDID the probe will read.  A
 * catalogue retained from an earlier connection never counts.
 *
 * Caller holds hpd_mutex.
 */
bool dcp_external_modes_ready(struct apple_dcp *dcp)
{
	bool ready, exception = false, proven = false;
	unsigned long flags;
	unsigned int i;

	lockdep_assert_held(&dcp->hpd_mutex);
	guard(mutex)(&dcp->dcpavserv.lock);
	if (!dcpavserv_edid_current(dcp, dcp->typec_generation) ||
	    !dcpavserv_edid_service_current(dcp, dcp->dcpavserv.epoch))
		return false;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	ready = dcp_catalog_current(dcp);
	for (i = 0; ready && i < dcp->nr_modes; i++) {
		if (!dcp->modes[i].automatic_rgb8_quirk)
			continue;
		exception = true;
		proven |= dcp_180hz_link_current(dcp, &dcp->modes[i]);
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);

	return ready && (!exception || proven);
}

bool dcp_180hz_pending(struct apple_dcp *dcp, const struct dcp_display_mode *mode)
{
	struct apple_connector *con = dcp->connector;
	const struct edid *raw;

	if (!con || !dcp->active_typec_route || dcp_is_usb4_output(dcp) || dcp->dptx_tunnel ||
	    !mode->unsafe_rgb8_ticket.associated ||
	    mode->unsafe_rgb8_ticket.generation != dcp->typec_generation ||
	    mode->automatic_connection_sequence != dcp->rate_proof.connection_sequence ||
	    mode->automatic_catalog_sequence != dcp->rate_proof.catalog_sequence ||
	    (!dcp_180hz_timing_epoch(mode, dcp->dcpavserv.epoch) &&
	     !(mode->automatic_service_pending && !dcp->dcpavserv.enabled &&
	      mode->unsafe_rgb8_service_epoch == dcp->dcpavserv.epoch)))
		return false;
	if (!con->drm_edid)
		return true;
	raw = drm_edid_raw(con->drm_edid);
	return raw && raw->extensions == 1 && dcp_180hz_edid((const u8 *)raw, 256, &mode->mode);
}

static void dcp_180hz_retry_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp, rate_retry_wq);
	unsigned long flags;
	bool current_generation;

	guard(mutex)(&dcp->hpd_mutex);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	current_generation = dcp->typec_cable_connected && !dcp->quiescing &&
		dcp->rate_proof.retry_generation == dcp->typec_generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (current_generation && dcp->connector &&
	    READ_ONCE(dcp->connector->dcp) == to_platform_device(dcp->dev))
		schedule_work(&dcp->connector->hotplug_wq);
}

void dcp_180hz_retry(struct apple_dcp *dcp)
{
	unsigned long flags;
	bool retry = false;

	if (!dcp->rate_retry_initialized || READ_ONCE(dcp->quiescing))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (dcp->rate_proof.retry_generation != READ_ONCE(dcp->typec_generation)) {
		dcp->rate_proof.retry_generation = READ_ONCE(dcp->typec_generation);
		dcp->rate_proof.retries = 0;
	}
	if (READ_ONCE(dcp->typec_cable_connected) && dcp->rate_proof.retries < 8) {
		dcp->rate_proof.retries++;
		retry = true;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (retry)
		mod_delayed_work(system_freezable_wq, &dcp->rate_retry_wq, msecs_to_jiffies(300));
}

static void dcp_180hz_cancel(void *data)
{
	struct apple_dcp *dcp = data;

	WRITE_ONCE(dcp->rate_retry_initialized, false);
	cancel_delayed_work_sync(&dcp->rate_retry_wq);
}

int dcp_180hz_init(struct apple_dcp *dcp)
{
	int ret;

	if (dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17P_EXTERNAL)
		return 0;
	INIT_DELAYED_WORK(&dcp->rate_retry_wq, dcp_180hz_retry_work);
	ret = devm_add_action_or_reset(dcp->dev, dcp_180hz_cancel, dcp);
	if (!ret)
		dcp->rate_retry_initialized = true;
	return ret;
}
