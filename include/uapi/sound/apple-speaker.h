/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef __UAPI_SOUND_APPLE_SPEAKER_H
#define __UAPI_SOUND_APPLE_SPEAKER_H

#include <linux/types.h>

/* 512-byte ALSA BYTES control: "Speaker Energy Grant", little endian. */
#define MACAUDIO_ENERGY_VERSION 1
#define MACAUDIO_ENERGY_SPEAKERS 6
#define MACAUDIO_ENERGY_UNIT_RATE 48000
#define MACAUDIO_ENERGY_AVAILABLE (1U << 0)
#define MACAUDIO_ENERGY_CONFIGURED (1U << 1)
#define MACAUDIO_ENERGY_TRANSFER_VALID (1U << 2)
#define MACAUDIO_ENERGY_FAILED (1U << 3)
#define MACAUDIO_ENERGY_OWNED (1U << 4)
#define MACAUDIO_ENERGY_LIVE (1U << 5)
#define MACAUDIO_ENERGY_PLAYING (1U << 6)
#define MACAUDIO_ENERGY_REVOKE (1U << 31)

/* A transfer identifier is a versioned bound, not an amplifier model name. */
#define MACAUDIO_ENERGY_TRANSFER_NONE 0
#define MACAUDIO_ENERGY_TRANSFER_IDENTITY 1
#define MACAUDIO_ENERGY_TRANSFER_BOUNDED_CURRENT 2

struct macaudio_energy_ctl {
	__le32 version;
	__le32 flags;
	__le64 epoch;
	__le64 sequence;
	__le64 clock_ns;
	__le64 deadline_ns;
	__le32 rate;
	__le32 channels;
	__le32 speakers;
	__le32 format;
	__le32 transfer;
	__le32 amp_gain_millidbv;
	__le32 safe_reduction_millidb;
	__le32 pending;
	__le32 slot[MACAUDIO_ENERGY_SPEAKERS];
	__le64 submitted[MACAUDIO_ENERGY_SPEAKERS];
	__le64 completed[MACAUDIO_ENERGY_SPEAKERS];
	__le64 source_submitted[MACAUDIO_ENERGY_SPEAKERS];
	__le64 source_completed[MACAUDIO_ENERGY_SPEAKERS];
	__le64 refunded[MACAUDIO_ENERGY_SPEAKERS];
	__le64 unbudgeted[MACAUDIO_ENERGY_SPEAKERS];
	__le64 ceiling[MACAUDIO_ENERGY_SPEAKERS];
	__le32 gain_q31[MACAUDIO_ENERGY_SPEAKERS];
	__u8 card_id[32];
	__le32 reserved[6];
};

#endif
