/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_REGULATORY_H
#define MT7932_REGULATORY_H

/* Private, versioned packaging of original-derived country policy. This is
 * not firmware code and does not replace cfg80211's regulatory database.
 */
#include <crypto/sha2.h>
#include <net/cfg80211.h>

#include "channels.h"
#include "six.h"

#define MT7932_POLICY_TABLES 13
#define MT7932_POLICY_MODES 3
#define MT7932_POLICY_6G_PAGES 32

#define MT7932_6G_RECORDS 110

struct mt7932_6g_table {
	const u8 *records, *bitmap;
	u64 channels;
};

struct mt7932_policy {
	const u8 *mode[MT7932_POLICY_MODES];
	const u8 *table[MT7932_POLICY_TABLES];
	u16 length[MT7932_POLICY_TABLES];
	u8 modes, six_pages;
	struct mt7932_6g_table six_class[3];
	u64 six_channels, legacy_channels;
	const u8 *six_page[MT7932_POLICY_6G_PAGES];
	u16 six_length[MT7932_POLICY_6G_PAGES];
	u8 six_cid[MT7932_POLICY_6G_PAGES];
	const u8 *assoc_power[2];
};

struct mt7932_reg_snapshot {
	/* Requested cfg80211 country; domain carries the selected firmware
	 * profile. Stock unknown-country startup uses XZ while Linux stays 00.
	 */
	u8 alpha2[2];
	u8 domain[12 + 8 * MT7932_CHANNELS];
	u16 length;
	/* Keep numeric limits in equality checks even though CID0f contains only
	 * flags. An unsupported reduction must not be mistaken for no change.
	 */
	s32 power[MT7932_CHANNELS];
	s8 psd[MT7932_CHANNELS];
	int error;
};

/* J7RP v2 appends a country-qualified VLP section to the unchanged v1
 * tables. Data is derived externally; no captured transactions or blobs are
 * embedded in the driver. See mt7932-neo.rst for the packaging contract.
 */
static inline int mt7932_policy_6g_parse(struct mt7932_policy *out,
					 const u8 *data, size_t size, const u8 alpha2[2])
{
	size_t at = 24;
	unsigned int i;

	if (size < at || memcmp(data, "6GRP", 4) || data[4] != 1 || data[5] != 14 ||
	    !data[6] || data[6] > MT7932_POLICY_6G_PAGES || data[7] ||
	    memcmp(data + 16, alpha2, 2) || data[18] != 1 || memchr_inv(data + 19, 0, 5))
		return -EINVAL;
	out->six_channels = get_unaligned_le64(data + 8);
	out->legacy_channels = out->six_channels;
	if (!out->six_channels || (out->six_channels & ~GENMASK_ULL(58, 0)))
		return -EINVAL;
	out->six_pages = data[6];
	for (i = 0; i < out->six_pages; i++) {
		const u8 *page;
		unsigned int length, j;
		u8 cid;

		if (size - at < 4)
			return -EINVAL;
		cid = data[at];
		length = get_unaligned_le16(data + at + 2);
		if (data[at + 1] || length < 44 || length > 1020 || length > size - at - 4)
			return -EINVAL;
		page = data + at + 4;
		if (page[5] != 3 || !page[4] || page[4] > 8 || page[6] > 1 || page[7])
			return -EINVAL;
		if (cid == 0x5d) {
			if (get_unaligned_le16(page + 2) != length || page[0] != 3 ||
			    page[1] != 0x2a || memcmp(page + 8, alpha2, 2) ||
			    page[10] || page[11] || length != 44U + page[4] * 122U)
				return -EINVAL;
			for (j = 0; j < 2; j++) {
				if (page[44] != (j ? 0x0f : 0x02))
					continue;
				if (out->assoc_power[j] || page[4] != 8)
					return -EINVAL;
				out->assoc_power[j] = page;
			}
		} else if (cid == 0x95) {
			/* This legacy command has no embedded length or alpha2.
			 * Its containing country package provides that identity.
			 */
			if (get_unaligned_le32(page) || memchr_inv(page + 8, 0, 36) ||
			    length != 44U + page[4] * 33U)
				return -EINVAL;
			for (j = 0; j < page[4]; j++)
				if (memchr_inv(page + 45 + j * 33, 0x7f, 32))
					return -EINVAL;
		} else {
			return -EINVAL;
		}
		out->six_page[i] = page;
		out->six_length[i] = length;
		out->six_cid[i] = cid;
		at += 4 + length;
	}
	return at == size && out->assoc_power[0] && out->assoc_power[1] ? 0 : -EINVAL;
}

/* Version 4 binds the unchanged v3 class data to the SPEC's CID5d ABI.
 * Version 3, binding revision zero, remains an unbound archive and is refused.
 */
static inline int mt7932_policy_classes_parse(struct mt7932_policy *out,
					      const u8 *data, size_t size,
					      const u8 alpha2[2])
{
	size_t at = 16;
	unsigned int i, previous = 0;

	if (size < at || memcmp(data, "6GCL", 4) || data[4] != 1 || !data[5] ||
	    data[5] > 2 || data[6] != 1 || data[7] ||
	    get_unaligned_le32(data + 8) != size ||
	    get_unaligned_le32(data + 12))
		return -EINVAL;
	for (i = 0; i < data[5]; i++) {
		const u8 *section = data + at, *record;
		struct mt7932_6g_table *table;
		unsigned int id, mode, j, channel = 1;
		u64 mask;

		if (size - at < 56)
			return -EINVAL;
		id = section[0];
		if ((id != 1 && id != 2) || id <= previous ||
		    section[1] != id - 1 || section[2] != 1 ||
		    section[3] != MT7932_6G_RECORDS ||
		    get_unaligned_le32(section + 4) !=
			    56 + MT7932_6G_RECORDS * 122 ||
		    size - at < 56 + MT7932_6G_RECORDS * 122 ||
		    memcmp(section + 16, alpha2, 2) ||
		    memchr_inv(section + 18, 0, 6))
			return -EINVAL;
		mask = get_unaligned_le64(section + 8);
		if (!mask || (mask & ~GENMASK_ULL(58, 0)))
			return -EINVAL;
		record = section + 56;
		for (j = 0; j < MT7932_6G_RECORDS; j++, record += 122) {
			unsigned int k;
			bool power = false;

			/* Authenticated matrix order: channel 2, then primary and
			 * bonded centers up to 160 MHz (excluding channel 231).
			 */
			if (record[0] != (j ? channel : 2))
				return -EINVAL;
			if (j) {
				channel += 2;
				while (channel == 31 || channel == 63 ||
				       channel == 95 || channel == 127 ||
				       channel == 159 || channel == 191 ||
				       channel == 223 || channel == 231)
					channel += 2;
			}
			if (record[0] % 4 != 1 ||
			    !(mask & BIT_ULL((record[0] - 1) / 4)))
				continue;
			for (k = 1; k < 122; k++)
				power |= record[k] != 0xc4 && record[k] != 0x7f;
			if (!power)
				return -EINVAL;
		}
		mode = id == 1 ? MT7932_6G_VLP : MT7932_6G_LPI;
		table = &out->six_class[mode];
		table->channels = mask;
		table->bitmap = section + 24;
		table->records = section + 56;
		out->six_channels |= mask;
		previous = id;
		at += 56 + MT7932_6G_RECORDS * 122;
	}
	return at == size && out->six_class[MT7932_6G_LPI].records ? 0 :
								     -EINVAL;
}

/* Normal infrastructure publication: submode zero, no invented class bits.
 * Class IDs and data must agree; callers cannot relabel VLP data as LPI.
 */
static inline int mt7932_6g_class_page(u8 *body, size_t capacity,
				       const struct mt7932_policy *policy,
				       unsigned int mode, const u8 alpha2[2],
				       unsigned int first)
{
	const struct mt7932_6g_table *table;
	unsigned int count, length;

	if (mode != MT7932_6G_LPI && mode != MT7932_6G_VLP)
		return -EOPNOTSUPP;
	table = &policy->six_class[mode];
	if (!table->records || first >= MT7932_6G_RECORDS || first % 8)
		return -EINVAL;
	count = min(8U, MT7932_6G_RECORDS - first);
	length = 44 + count * 122;
	if (capacity < length)
		return -ENOSPC;
	memset(body, 0, 44);
	body[1] = 0x0a | (mode << 4);
	put_unaligned_le16(length, body + 2);
	body[4] = count;
	body[5] = 3;
	body[6] = first + count == MT7932_6G_RECORDS;
	memcpy(body + 8, alpha2, 2);
	memcpy(body + 12, table->bitmap, 32);
	memcpy(body + 44, table->records + first * 122, count * 122);
	return length;
}

static inline int mt7932_policy_parse(struct mt7932_policy *out,
				     const u8 *data, size_t size,
				     const u8 alpha2[2])
{
	size_t at = 16;
	unsigned int i, n, version;

	memset(out, 0, sizeof(*out));
	version = size >= 6 ? get_unaligned_le16(data + 4) : 0;
	if (size < at ||
	    size > (version == 2 || version == 4 ? 65536 : 16384) ||
	    memcmp(data, "J7RP", 4) ||
	    (version != 1 && version != 2 && version != 4) ||
	    memcmp(data + 6, alpha2, 2) || data[8] < 2 ||
	    data[8] > MT7932_POLICY_MODES || data[9] != MT7932_POLICY_TABLES ||
	    data[10] || data[11] || get_unaligned_le32(data + 12) != size)
		return -EINVAL;
	out->modes = data[8];
	for (i = 0; i < out->modes; i++, at += 328) {
		if (size - at < 328 || get_unaligned_le32(data + at) ||
		    !get_unaligned_le32(data + at + 4) ||
		    get_unaligned_le32(data + at + 4) > 320)
			return -EINVAL;
		out->mode[i] = data + at;
	}
	for (i = 0; i < MT7932_POLICY_TABLES; i++) {
		const u8 *p = data + at;

		if (size - at < 44)
			return -EINVAL;
		n = get_unaligned_le16(p + 2);
		if (n < 44 || n > 1020 || n > size - at)
			return -EINVAL;
		if (i >= 1 && i <= 9) {
			unsigned int count = i == 2 || i == 9 ? 5 : 8;

			/* The stock fallback includes power data for channel 14.
			 * This is not channel permission: our cfg80211/CID0f lists
			 * still exclude it, and the driver cannot scan or join it.
			 */
			if (i == 2 && !memcmp(alpha2, "XZ", 2))
				count = 6;
			if (p[0] != 3 || p[1] != 0x0a || p[4] != count ||
			    p[5] != (i <= 2 ? 1 : 2) ||
			    p[6] != (i == 2 || i == 9) || p[7] ||
			    memcmp(p + 8, alpha2, 2) || p[10] || p[11] ||
			    n != 44 + count * 122)
				return -EINVAL;
			if (!memcmp(alpha2, "XZ", 2)) {
				unsigned int j;

				for (j = 0; i <= 2 && j < count; j++)
					if (p[44 + j * 122] != (i - 1) * 8 + j + 1)
						return -EINVAL;
				if (i > 1 && memcmp(p + 12, out->table[1] + 12, 32))
					return -EINVAL;
			}
		} else {
			static const u16 lengths[] = {532, 144, 58, 265};
			static const u8 types[] = {5, 1, 4, 2};
			unsigned int k = i ? i - 9 : 0;

			if (n != lengths[k] || p[0] || p[1] != 2 || p[7] != types[k])
				return -EINVAL;
			if (i == 10 && (memcmp(p + 8, alpha2, 2) || p[10] || p[11]))
				return -EINVAL;
		}
		out->table[i] = p;
		out->length[i] = n;
		at += n;
	}
	if (version == 4)
		return mt7932_policy_classes_parse(out, data + at, size - at,
						   alpha2);
	return version == 2 ? mt7932_policy_6g_parse(out, data + at, size - at, alpha2) :
		at == size ? 0 : -EINVAL;
}

/*
 * Whether @next differs from the applied @prev only by NO_IR lifted on some
 * channels, as a beacon hint does in the world domain: same country, firmware
 * domain header, channel list and limits, and every channel's flags equal to
 * the applied ones less, at most, NO_IR. Such a change only permits more; the
 * firmware may keep the stricter domain until it is idle.
 */
static inline bool mt7932_reg_only_relaxes(const struct mt7932_reg_snapshot *prev,
					   const struct mt7932_reg_snapshot *next)
{
	unsigned int i, count;

	if (prev->error || next->error || prev->length != next->length ||
	    prev->length < 12 || prev->length > sizeof(prev->domain) ||
	    memcmp(prev->alpha2, next->alpha2, 2) ||
	    memcmp(prev->domain, next->domain, 12) ||
	    memcmp(prev->power, next->power, sizeof(prev->power)) ||
	    memcmp(prev->psd, next->psd, sizeof(prev->psd)))
		return false;
	count = (prev->length - 12) / 8;
	for (i = 0; i < count; i++) {
		const u8 *a = prev->domain + 12 + i * 8, *b = next->domain + 12 + i * 8;
		u32 was = get_unaligned_le32(a + 4), now = get_unaligned_le32(b + 4);

		if (memcmp(a, b, 4) || (now & ~was) ||
		    ((was & ~now) & ~IEEE80211_CHAN_NO_IR))
			return false;
	}
	return true;
}

/* Every entry of a validated package starts with its channel number and is
 * followed by per-rate power limits. A country that does not permit a
 * channel sets all of that channel's limits to this marker.
 */
#define MT7932_POLICY_NOT_PERMITTED 0xc4

static inline bool mt7932_policy_permits(const struct mt7932_policy *policy,
					 enum nl80211_band band, unsigned int channel)
{
	unsigned int i, j, k;
	u8 firmware_band;

	switch (band) {
	case NL80211_BAND_2GHZ:
		firmware_band = 1;
		break;
	case NL80211_BAND_5GHZ:
		firmware_band = 2;
		break;
	case NL80211_BAND_6GHZ:
		return mt7932_channel_band(band, channel) &&
			(policy->six_channels & BIT_ULL((channel - 1) / 4));
	default:
		/* J7RP v1 carries no 6 GHz per-rate power pages. */
		return false;
	}

	for (i = 1; i <= 9; i++) {
		const u8 *table = policy->table[i];

		if (table[5] != firmware_band)
			continue;
		for (j = 0; j < table[4]; j++) {
			const u8 *entry = table + 44 + j * 122;

			if (entry[0] != channel)
				continue;
			for (k = 1; k < 122; k++)
				if (entry[k] != MT7932_POLICY_NOT_PERMITTED)
					return true;
			return false;
		}
	}
	return false;
}

/* VLP per-rate cells use the native half-dBm units also used by
 * mt76_connac_get_ch_power(). Preserve unsupported/unpopulated sentinels.
 * The PSD gains use power-of-two bandwidths below each allocation's occupied
 * bandwidth, so integer rounding can only reduce power.
 */
struct mt7932_6g_limits {
	int eirp, psd;
};

static inline void mt7932_6g_limit_page(u8 *page, const struct mt7932_6g_limits *limits)
{
	static const u8 counts[] = {1, 3, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
	static const u8 gains[] = {24, 24, 24, 30, 24, 30, 36, 42, 6, 12, 18, 24, 30, 36, 42};
	unsigned int entry, group, k;

	for (entry = 0; entry < page[4]; entry++) {
		u8 *cell = page + 45 + entry * 122;

		for (group = 0; group < ARRAY_SIZE(counts); group++) {
			int cap = min(limits->eirp, limits->psd + gains[group]);

			for (k = 0; k < counts[group]; k++, cell++)
				if (*cell != 0xc4 && *cell != 0x7f) {
					if (cap < -128)
						*cell = 0xc4;
					else if ((s8)*cell > cap)
						*cell = (u8)cap;
				}
		}
	}
}

static inline unsigned int mt7932_6g_reg_index(unsigned int channel)
{
	return MT7932_CHANNELS_2G + MT7932_CHANNELS_5G + (channel - 1) / 4;
}

static inline struct mt7932_6g_limits
mt7932_6g_limits_mask(const struct mt7932_reg_snapshot *reg, u64 channels)
{
	struct mt7932_6g_limits limits = { .eirp = 28, .psd = 256 };
	unsigned int i, begin = reg->domain[8] + reg->domain[9];

	for (i = begin; i < begin + reg->domain[10]; i++) {
		const u8 *entry = reg->domain + 12 + i * 8;
		unsigned int index = mt7932_6g_reg_index(get_unaligned_le16(entry));

		if (!(channels & BIT_ULL((get_unaligned_le16(entry) - 1) / 4)))
			continue;
		limits.eirp = min_t(int, limits.eirp, 2 * reg->power[index]);
		if (get_unaligned_le32(entry + 4) & IEEE80211_CHAN_PSD)
			limits.psd = min_t(int, limits.psd, 2 * reg->psd[index]);
	}
	return limits;
}

static inline struct mt7932_6g_limits
mt7932_6g_limits(const struct mt7932_reg_snapshot *reg)
{
	return mt7932_6g_limits_mask(reg, ~0ULL);
}

/* Remove the channels the country package forbids from a CID0f domain. */
static inline void mt7932_policy_filter(struct mt7932_reg_snapshot *reg,
					const struct mt7932_policy *policy)
{
	unsigned int i, kept = 0, count = (reg->length - 12) / 8;
	unsigned int count2 = reg->domain[8], count5 = reg->domain[9], count6 = reg->domain[10];

	reg->domain[8] = 0;
	reg->domain[9] = 0;
	reg->domain[10] = 0;
	for (i = 0; i < count; i++) {
		const u8 *entry = reg->domain + 12 + i * 8;
		unsigned int channel = get_unaligned_le16(entry);
		enum nl80211_band band = i < count2 ? NL80211_BAND_2GHZ :
			i < count2 + count5 ? NL80211_BAND_5GHZ :
			i < count2 + count5 + count6 ? NL80211_BAND_6GHZ : NUM_NL80211_BANDS;

		if (!mt7932_policy_permits(policy, band, channel))
			continue;
		memmove(reg->domain + 12 + kept * 8, entry, 8);
		reg->domain[8 + mt7932_channel_band(band, channel) - 1]++;
		kept++;
	}
	memset(reg->domain + 12 + kept * 8, 0, (count - kept) * 8);
	reg->length = 12 + kept * 8;
}

/*
 * Country package lookup. policy/ holds J7RP version 1 (2.4 and 5 GHz, at
 * most 16 KiB), which every kernel can read; policy-v2/ holds version 2
 * (version 1 plus 6 GHz VLP pages), which older kernels never open.
 */
enum mt7932_policy_source {
	MT7932_POLICY_NONE,
	MT7932_POLICY_V4, /* policy-v4/<CC>.bin */
	MT7932_POLICY_V2, /* policy-v2/<CC>.bin */
	MT7932_POLICY_V1, /* policy/<CC>.bin */
	MT7932_POLICY_WORLD, /* policy/world-XZ.bin */
};

#define MT7932_POLICY_CANDIDATES 4
#define MT7932_POLICY_PATH_MAX 48

struct mt7932_policy_file {
	const void *handle;
	const u8 *data;
	size_t size;
	/* Optional v4 LPI sidecar; the primary remains the original v2. */
	const void *class_handle;
};

/* get() returns 0 with @file filled in, or an error; put() releases it. */
struct mt7932_policy_loader {
	int (*get)(void *ctx, const char *path, struct mt7932_policy_file *file);
	void (*put)(void *ctx, struct mt7932_policy_file *file);
	void *ctx;
};

/* What was applied, for the debugfs readback. */
struct mt7932_policy_choice {
	u8 alpha2[2], domain[2];
	enum mt7932_policy_source source;
	u16 version;
	u32 size;
	u8 sha256[SHA256_DIGEST_SIZE];
	u8 class_sha256[SHA256_DIGEST_SIZE];
	bool class_valid;
	/* 6 GHz primaries left in the firmware domain. */
	u8 six_primaries;
	/* Why the country's own packages were not used: -ENOENT, -EINVAL, or
	 * -EOPNOTSUPP for a numeric cfg80211 code.
	 */
	int fallback;
	int error;
	u32 generation;
	unsigned int n_tried;
	struct {
		enum mt7932_policy_source source;
		int error;
	} tried[MT7932_POLICY_CANDIDATES];
};

/* An ISO 3166 alpha2, which may have a package of its own. */
static inline bool mt7932_policy_country(const u8 alpha2[2])
{
	return alpha2[0] >= 'A' && alpha2[0] <= 'Z' && alpha2[1] >= 'A' && alpha2[1] <= 'Z';
}

unsigned int mt7932_policy_candidates(const u8 alpha2[2],
				      enum mt7932_policy_source sources[MT7932_POLICY_CANDIDATES]);
void mt7932_policy_path(char *buf, size_t len, enum mt7932_policy_source source,
			const u8 alpha2[2]);
/*
 * Load the first usable package for @reg's country, parse it into @policy
 * and filter @reg's CID0f domain with it. On success @file holds the
 * package @policy points into; the caller releases it with loader->put().
 */
int mt7932_policy_select(const struct mt7932_policy_loader *loader,
			 struct mt7932_reg_snapshot *reg, struct mt7932_policy *policy,
			 struct mt7932_policy_file *file, struct mt7932_policy_choice *choice);

#endif
