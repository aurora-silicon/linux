/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_WOW_H
#define MT7932_WOW_H

#include <linux/bits.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

/*
 * Wake-on-WLAN commands, in the order the stock driver sends them for an
 * associated sleep: the magic packet entry on the WoW configuration command,
 * the power-save profile, suspend mode, then WoW start. Waking reverses it.
 */
#define MT7932_CID_WOW_CTRL		0x4a
#define MT7932_CID_WOW_CONFIG		0x4f
#define MT7932_CID_SUSPEND_MODE		0x58
#define MT7932_EID_WOW_REASON		0x62

#define MT7932_WOW_CTRL_LENGTH		228
#define MT7932_SUSPEND_MODE_LENGTH	68
/* The configuration command is a zeroed 32-byte header and one TLV. */
#define MT7932_WOW_CONFIG_HEADER	32
#define MT7932_WOW_MAGIC_LENGTH		(MT7932_WOW_CONFIG_HEADER + 20)
#define MT7932_WOW_CLEAR_LENGTH		(MT7932_WOW_CONFIG_HEADER + 12)
#define MT7932_WOW_TLV_MAGIC		0x06
#define MT7932_WOW_TLV_CLEAR		0x18

#define MT7932_WOW_START		1
#define MT7932_WOW_STOP			2

#define MT7932_WOW_DETECT_MAGIC		BIT(0)
#define MT7932_WOW_DETECT_DISCONNECT	BIT(2)
#define MT7932_WOW_DETECT_BEACON_LOST	BIT(4)

/*
 * Drop everything but magic packets, EAPOL and ARP requests to this station,
 * as the stock driver filters for a sleep.
 */
#define MT7932_WOW_FILTER		0x63

/* The stock configuration wakes the host on the radio's GPIO 13, level 5. */
#define MT7932_WOW_HIF_GPIO		3
#define MT7932_WOW_GPIO_PIN		13
#define MT7932_WOW_GPIO_LEVEL		5
#define MT7932_WOW_PIN_COUNT		1

/* Power-save profile for the sleep, and the suspend mode's DTIM multiple. */
#define MT7932_WOW_POWER_MODE		2
#define MT7932_WOW_MDTIM		1

#define MT7932_WOW_REASON_MAGIC		0
#define MT7932_WOW_REASON_DISCONNECT	8
#define MT7932_WOW_REASON_NONE		0xff
/* The stock driver ignores a shorter reason event. */
#define MT7932_WOW_REASON_LENGTH	132

/* The firmware detectors for the triggers cfg80211 asked for; 0 if none applies. */
static inline u8 mt7932_wow_detect(const struct cfg80211_wowlan *wow)
{
	u8 detect = 0;

	if (!wow)
		return 0;
	if (wow->magic_pkt)
		detect |= MT7932_WOW_DETECT_MAGIC;
	if (wow->disconnect)
		detect |= MT7932_WOW_DETECT_DISCONNECT | MT7932_WOW_DETECT_BEACON_LOST;
	return detect;
}

/* Start, with these detectors, or stop. */
static inline void mt7932_wow_ctrl(u8 out[MT7932_WOW_CTRL_LENGTH], bool start, u8 detect)
{
	memset(out, 0, MT7932_WOW_CTRL_LENGTH);
	out[0] = start ? MT7932_WOW_START : MT7932_WOW_STOP;
	if (start) {
		out[1] = detect;
		put_unaligned_le16(MT7932_WOW_FILTER, out + 2);
	}
	/* Scenario 0, then the wake signal: one pin, GPIO, which pin, level. */
	out[5] = MT7932_WOW_PIN_COUNT;
	out[8] = MT7932_WOW_HIF_GPIO;
	out[9] = MT7932_WOW_GPIO_PIN;
	out[10] = MT7932_WOW_GPIO_LEVEL;
	/* No UDP or TCP wake ports. */
}

/* BSS 0 into or out of suspend mode; the DTIM multiple and WoW flag stay set. */
static inline void mt7932_suspend_mode(u8 out[MT7932_SUSPEND_MODE_LENGTH], bool enable)
{
	memset(out, 0, MT7932_SUSPEND_MODE_LENGTH);
	out[1] = enable;
	out[2] = MT7932_WOW_MDTIM;
	out[3] = 1;
}

/* Magic packet entry 0 for this address, matched from the start of the frame. */
static inline void mt7932_wow_magic(u8 out[MT7932_WOW_MAGIC_LENGTH], const u8 *addr)
{
	u8 *tlv = out + MT7932_WOW_CONFIG_HEADER;

	memset(out, 0, MT7932_WOW_MAGIC_LENGTH);
	put_unaligned_le16(MT7932_WOW_TLV_MAGIC, tlv);
	put_unaligned_le16(MT7932_WOW_MAGIC_LENGTH - MT7932_WOW_CONFIG_HEADER, tlv + 2);
	tlv[4] = 0;	/* entry */
	tlv[5] = 1;	/* enabled */
	tlv[6] = 1;	/* offset counts forward */
	put_unaligned_le16(0, tlv + 8);
	ether_addr_copy(tlv + 10, addr);
}

/* Remove every WoW configuration entry. */
static inline void mt7932_wow_clear(u8 out[MT7932_WOW_CLEAR_LENGTH])
{
	u8 *tlv = out + MT7932_WOW_CONFIG_HEADER;

	memset(out, 0, MT7932_WOW_CLEAR_LENGTH);
	put_unaligned_le16(MT7932_WOW_TLV_CLEAR, tlv);
	put_unaligned_le16(MT7932_WOW_CLEAR_LENGTH - MT7932_WOW_CONFIG_HEADER, tlv + 2);
}

/*
 * The reason from a wake reason event body, or -EINVAL if it is short.
 * MT7932_WOW_REASON_NONE means the radio did not wake the host.
 */
static inline int mt7932_wow_reason(const u8 *body, size_t length)
{
	if (length < MT7932_WOW_REASON_LENGTH)
		return -EINVAL;
	return body[0];
}

/*
 * What to tell cfg80211 for a reason. False if the radio did not wake the
 * host. A reason without a cfg80211 trigger reports a wake with none set.
 */
static inline bool mt7932_wow_wakeup(u8 reason, struct cfg80211_wowlan_wakeup *wakeup)
{
	memset(wakeup, 0, sizeof(*wakeup));
	wakeup->pattern_idx = -1;
	switch (reason) {
	case MT7932_WOW_REASON_NONE:
		return false;
	case MT7932_WOW_REASON_MAGIC:
		wakeup->magic_pkt = true;
		break;
	case MT7932_WOW_REASON_DISCONNECT:
		wakeup->disconnect = true;
		break;
	}
	return true;
}

#endif
