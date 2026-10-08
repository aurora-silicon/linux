/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_REGULATORY_H
#define MT7932_REGULATORY_H

/* Private, versioned packaging of original-derived country policy. This is
 * not firmware code and does not replace cfg80211's regulatory database.
 */
#include "channels.h"

#define MT7932_POLICY_TABLES 13
#define MT7932_POLICY_MODES 3

struct mt7932_policy {
	const u8 *mode[MT7932_POLICY_MODES];
	const u8 *table[MT7932_POLICY_TABLES];
	u16 length[MT7932_POLICY_TABLES];
	u8 modes;
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
	int error;
};

static inline int mt7932_policy_parse(struct mt7932_policy *out,
				     const u8 *data, size_t size,
				     const u8 alpha2[2])
{
	size_t at = 16;
	unsigned int i, n;

	memset(out, 0, sizeof(*out));
	if (size < at || size > 16384 || memcmp(data, "J7RP", 4) ||
	    get_unaligned_le16(data + 4) != 1 || memcmp(data + 6, alpha2, 2) ||
	    data[8] < 2 || data[8] > MT7932_POLICY_MODES ||
	    data[9] != MT7932_POLICY_TABLES || data[10] || data[11] ||
	    get_unaligned_le32(data + 12) != size)
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
	return at == size ? 0 : -EINVAL;
}

#endif
