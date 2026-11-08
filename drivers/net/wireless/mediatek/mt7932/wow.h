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
#define MT7932_EVENT_CMD_RESULT		0x01
#define MT7932_EVENT_WOW_REASON		0x62
#define MT7932_WOW_PARAM_LEN		228
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
 * The WoWLAN SET is acknowledged by sequence; the event that answers it is
 * not documented. A generic command result naming the SET carries its
 * status, and a nonzero status is a rejection.
 */
static inline int mt7932_wow_ack(const u8 *reply, size_t length)
{
	if (length < 32)
		return -EPROTO;
	if (reply[28] == MT7932_EVENT_CMD_RESULT && length >= MT7932_EVENT_PAYLOAD + 2 &&
	    reply[MT7932_EVENT_PAYLOAD] == MT7932_CMD_SET_WOWLAN &&
	    reply[MT7932_EVENT_PAYLOAD + 1])
		return -EREMOTEIO;
	return 0;
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
