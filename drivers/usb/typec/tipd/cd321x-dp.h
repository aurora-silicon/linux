/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CD321X_DP_H__
#define __CD321X_DP_H__

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/usb/typec_dp.h>

#include "tps6598x.h"

/* DATA_STATUS bits whose change rebuilds the DisplayPort lanes. */
#define CD321X_DP_MODE_BITS \
	(TPS_DATA_STATUS_DP_CONNECTION | TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK)

/* Why a cable update resets the Type-C session: cd321x_dp_update.link_reset */
#define CD321X_DP_RESET_UNPLUG	BIT(0)	/* the partner is gone */
#define CD321X_DP_RESET_PLUG	BIT(1)	/* the plug was removed or inserted */
#define CD321X_DP_RESET_FLIP	BIT(2)	/* the plug orientation changed */
#define CD321X_DP_RESET_TUNNEL	BIT(3)	/* Thunderbolt or USB4 came or went */
#define CD321X_DP_RESET_PARTNER	BIT(4)	/* a different partner is attached */
#define CD321X_DP_RESET_FORCED	BIT(5)	/* recovery after a failed update */

/*
 * What the display connector is told once a cable update has been applied:
 * nothing, that the sink is present, or that the sink is present and asked
 * for its link to be checked.
 */
enum cd321x_dp_event {
	CD321X_DP_EVENT_NONE,
	CD321X_DP_EVENT_CONNECTED,
	CD321X_DP_EVENT_RETRAIN,
};

/**
 * struct cd321x_dp_update - DisplayPort view of one cable update
 * @data_status: DATA_STATUS of this update
 * @changed: DATA_STATUS bits that changed since the previous update
 * @link_reset: why the Type-C session is torn down (CD321X_DP_RESET_*),
 *	or 0 if it is kept
 * @irq_hpd: the sink signalled IRQ_HPD and that has not been delivered
 * @route_active: the previous update left a display route up
 * @sink_reported: the connector was last told that a sink is present
 */
struct cd321x_dp_update {
	u32 data_status;
	u32 changed;
	u32 link_reset;
	bool irq_hpd;
	bool route_active;
	bool sink_reported;
};

/*
 * USB3_CONNECTION is part of the pin assignment while DisplayPort is up: C
 * and D, and E and F, differ only in it, and moving between them changes
 * the DisplayPort lane count.  Without DisplayPort it is a plain USB change.
 */
static inline bool cd321x_dp_mode_changed(u32 data_status, u32 changed)
{
	u32 bits = CD321X_DP_MODE_BITS;

	if (data_status & TPS_DATA_STATUS_DP_CONNECTION)
		bits |= TPS_DATA_STATUS_USB3_CONNECTION;

	return changed & bits;
}

/**
 * cd321x_dp_irq_hpd() - Track the sink's IRQ_HPD across interrupts
 * @pending: an IRQ_HPD is waiting to be delivered
 * @old: DATA_STATUS read at the previous interrupt
 * @new: DATA_STATUS read at this interrupt
 * @old_rx: DisplayPort status last received from the partner, as of @old
 * @new_rx: the same, as of @new
 *
 * TI documents the HPD IRQ bit of DATA_STATUS as sticky, and the partner's
 * DisplayPort status keeps IRQ_HPD until its next status message, so either
 * bit becoming set is a new attention.  It is looked at on every interrupt,
 * not across the debounced update, so an attention that comes and goes
 * within the debounce is not lost.  It stays pending until it has been
 * delivered, including through updates that have to be retried.  HPD going
 * low drops it: the sink is then reported gone and connected again anyway.
 *
 * Return: whether an IRQ_HPD is pending after this interrupt.
 */
static inline bool cd321x_dp_irq_hpd(bool pending, u32 old, u32 new,
				     u32 old_rx, u32 new_rx)
{
	if (!(new & CD321X_DATA_STATUS_HPD_LEVEL))
		return false;

	return pending || (~old & new & CD321X_DATA_STATUS_HPD_IRQ) ||
	       (~old_rx & new_rx & DP_STATUS_IRQ_HPD);
}

/*
 * Does the display connector lose its sink before this update is applied?
 *
 * Any HPD level change counts, however short.  The partner reports the
 * sink's short IRQ_HPD pulse as an attention with the HPD level held high.
 * A low HPD level is how DisplayPort signals an unplug, and the sink seen
 * again within the debounce may be a different one behind the same dock.
 */
static inline bool cd321x_dp_teardown(const struct cd321x_dp_update *u)
{
	if (!u->route_active)
		return false;

	return u->link_reset ||
	       !(u->data_status & TPS_DATA_STATUS_DP_CONNECTION) ||
	       !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL) ||
	       (u->changed & CD321X_DATA_STATUS_HPD_LEVEL) ||
	       cd321x_dp_mode_changed(u->data_status, u->changed);
}

/* @route_up: the update left this port's display route up. */
static inline enum cd321x_dp_event
cd321x_dp_event(const struct cd321x_dp_update *u, bool route_up)
{
	if (!route_up || !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL))
		return CD321X_DP_EVENT_NONE;

	/* IRQ_HPD on a sink that stays connected: have its link checked. */
	if (u->irq_hpd && u->route_active && u->sink_reported &&
	    !cd321x_dp_teardown(u))
		return CD321X_DP_EVENT_RETRAIN;

	return CD321X_DP_EVENT_CONNECTED;
}

#endif /* __CD321X_DP_H__ */
