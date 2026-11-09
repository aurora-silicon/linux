/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_WOW_H
#define MT7932_WOW_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "protocol.h"

/*
 * Wake on WLAN uses the legacy CONNAC WoWLAN SET (CMD_ID_SET_WOWLAN in
 * MediaTek's public driver source) with its fixed 228-byte parameter block,
 * and the legacy wake-reason event that the firmware sends once the host
 * has resumed. All fields not set here stay zero: no packet filter, no
 * scenario, no port list, and only the first of the two wake-HIF records.
 */
#define MT7932_CMD_SET_WOWLAN		0x4a
#define MT7932_CMD_SUSPEND_MODE		0x58
#define MT7932_CMD_HIF_CTRL		0xf6
#define MT7932_EVENT_WOW_REASON		0x62
#define MT7932_EVENT_HIF_CTRL		0xf6
#define MT7932_WOW_PARAM_LEN		228
#define MT7932_SUSPEND_PARAM_LEN		68
#define MT7932_HIF_PARAM_LEN		36
#define MT7932_WOW_START		1
#define MT7932_WOW_STOP			2
/* Trigger bitmap of the CONNAC2 family. */
#define MT7932_WOW_MAGIC		BIT(0)
#define MT7932_WOW_DISCONNECT		BIT(2)
#define MT7932_WOW_BEACON_LOSS		BIT(4)
/*
 * Packet filter while armed: drop everything for the host except a magic
 * packet, so ordinary traffic neither reaches nor wakes it.
 */
#define MT7932_WOW_FILTER		2
#define MT7932_WOW_DROP_ALL		BIT(0)
#define MT7932_WOW_MAGIC_TO_HOST	BIT(1)
/* The first wake-HIF record: which host interface the firmware wakes. */
#define MT7932_WOW_HIF			8
#define MT7932_WOW_HIF_PCIE		2
/* Legacy event payloads follow the 36-byte event header. */
#define MT7932_EVENT_PAYLOAD		36
#define MT7932_WOW_REASON_MAGIC		0
#define MT7932_WOW_REASON_DISCONNECT	8
#define MT7932_MCU_CMD_IRQ		BIT(29)
#define MT7932_MCU_WAKE_RX_PCIE		BIT(0)
#define MT7932_HIF_TRAFFIC_IDLE		2

/*
 * A lost association can surface as an explicit disconnect or as beacon
 * loss, so the cfg80211 disconnect trigger arms both.
 */
static inline u8 mt7932_wow_triggers(bool magic, bool disconnect)
{
	return (magic ? MT7932_WOW_MAGIC : 0) |
	       (disconnect ? MT7932_WOW_DISCONNECT | MT7932_WOW_BEACON_LOSS : 0);
}

/* STOP carries no triggers. */
static inline void mt7932_wow_param(u8 *out, u8 command, u8 triggers)
{
	memset(out, 0, MT7932_WOW_PARAM_LEN);
	out[0] = command;
	out[1] = command == MT7932_WOW_START ? triggers : 0;
	if (command == MT7932_WOW_START)
		put_unaligned_le16(MT7932_WOW_DROP_ALL | MT7932_WOW_MAGIC_TO_HOST,
				   out + MT7932_WOW_FILTER);
	out[MT7932_WOW_HIF] = MT7932_WOW_HIF_PCIE;
}

/*
 * Legacy per-BSS suspend policy. The formerly reserved fourth byte is the
 * public connac wow_suspend selector: one changes the DTIM policy to WoW.
 */
static inline void mt7932_suspend_param(u8 *out, bool enable)
{
	memset(out, 0, MT7932_SUSPEND_PARAM_LEN);
	out[0] = 0;		/* BSS 0 */
	out[1] = enable;
	out[2] = 1;		/* mdtim */
	out[3] = 1;		/* wow_suspend */
}

/* Public legacy HIF control: PCIe, TX, stop/resume. */
static inline void mt7932_hif_param(u8 *out, bool stop)
{
	memset(out, 0, MT7932_HIF_PARAM_LEN);
	out[0] = MT7932_WOW_HIF_PCIE;
	out[1] = 1;		/* TX direction */
	out[2] = stop;
}

static inline int mt7932_hif_event(const struct mt7932_event *event)
{
	if (event->eid != MT7932_EVENT_HIF_CTRL)
		return -ENOMSG;
	if (event->length < MT7932_EVENT_PAYLOAD + 3)
		return -EMSGSIZE;
	if (event->packet[MT7932_EVENT_PAYLOAD] != MT7932_WOW_HIF_PCIE)
		return -EPROTO;
	return 0;
}

/* Both directions must be idle before ownership can pass to firmware. */
static inline int mt7932_hif_idle(const struct mt7932_event *event)
{
	int ret = mt7932_hif_event(event);

	if (ret)
		return ret;
	if (event->packet[MT7932_EVENT_PAYLOAD + 1] != MT7932_HIF_TRAFFIC_IDLE ||
	    event->packet[MT7932_EVENT_PAYLOAD + 2] != MT7932_HIF_TRAFFIC_IDLE)
		return -EBUSY;
	return 0;
}

static inline bool mt7932_wow_wake_signal(u32 cause, u32 software)
{
	return (cause & MT7932_MCU_CMD_IRQ) &&
	       (software & MT7932_MCU_WAKE_RX_PCIE);
}

/* The firmware's wake reason, or a negative error for any other event. */
static inline int mt7932_wow_reason(const struct mt7932_event *event)
{
	if (event->eid != MT7932_EVENT_WOW_REASON)
		return -ENOMSG;
	if (event->length < MT7932_EVENT_PAYLOAD + 1)
		return -EMSGSIZE;
	return event->packet[MT7932_EVENT_PAYLOAD];
}

#endif
