/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_CALIBRATION_H
#define MT7932_CALIBRATION_H

#include "protocol.h"

/* Protected by the RX/procedure lock. One active logical procedure only;
 * ordinary D6 nulls carry no source-defined transaction/sequence identifier.
 */
struct mt7932_cal_completion {
	unsigned int expected, received;
	bool active, done;
	int error;
};

static inline int mt7932_cal_begin(struct mt7932_cal_completion *state, unsigned int expected)
{
	if (state->error)
		return state->error;
	if (state->active || !expected || expected > 4)
		return -EINVAL;
	state->expected = expected;
	state->received = 0;
	state->done = false;
	state->active = true;
	return 0;
}

/* Action1 SET only. Caller validates outer/event bounds and dispatches EIDd6
 * independently of pending-query sequence matching. Latch errors until reboot.
 */
static inline int mt7932_cal_null(struct mt7932_cal_completion *state,
				  const u8 *payload, size_t length, u8 version)
{
	u32 status;
	int ret;

	if (state->error)
		return state->error;
	if (length < 20) {
		ret = -EMSGSIZE;
		goto fail;
	}
	if (payload[2] || payload[3] != 1) {
		ret = -EPROTO;
		goto fail;
	}
	if (!state->active || !state->expected) {
		ret = -EPROTO;
		goto fail;
	}
	status = get_unaligned_le32(payload + 8);
	if ((version && status) || (!version && !status)) {
		ret = -EREMOTEIO;
		goto fail;
	}
	if (++state->received == state->expected) {
		state->active = false;
		state->done = true;
	}
	return state->done;
fail:
	state->error = ret;
	state->active = false;
	return ret;
}

struct mt7932_cal_segment {
	const u8 *data;
	size_t length;
};

/* Validate the entire original container before selecting any data to send.
 * NEO_MT7932_STOCK_ARTIFACTS_AND_CAL_CONVERSION: raw BLOB, not a transcript.
 */
static inline int mt7932_cal_validate(const u8 *data, size_t size)
{
	u32 end, count, i, j;

	if (size < 16 || size > 16 * 1024 * 1024 || memcmp(data, "BLOB", 4) || get_unaligned_be16(data + 8) != 12)
		return -EINVAL;
	end = get_unaligned_be32(data + 4);
	count = get_unaligned_be16(data + 10);
	if (!count || end != 16 + 20 * count || end > size)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const u8 *entry = data + 16 + 20 * i;
		u32 offset = get_unaligned_be32(entry + 4);
		u32 length = get_unaligned_be32(entry + 8), sum = 0;

		if (get_unaligned_be16(entry + 2) != 12 || offset < end ||
		    offset > size || length < 12 || length > size - offset)
			return -EINVAL;
		if (memcmp(data + offset, entry, 4) ||
		    get_unaligned_be32(data + offset + 4) != length)
			return -EINVAL;
		for (j = 0; j < length; j++)
			sum += data[offset + j];
		if (sum != get_unaligned_be32(entry + 12))
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const u8 *prior = data + 16 + 20 * j;
			u32 start = get_unaligned_be32(prior + 4);
			u32 bytes = get_unaligned_be32(prior + 8);

			if (!memcmp(entry, prior, 2) || (offset < start + bytes && start < offset + length))
				return -EINVAL;
		}
	}
	return 0;
}

/* Only call after full validation; borrowed storage remains owned by caller. */
static inline int mt7932_cal_segment(const u8 *data, unsigned int tag,
				     struct mt7932_cal_segment *segment)
{
	unsigned int i, count = get_unaligned_be16(data + 10);

	for (i = 0; i < count; i++) {
		const u8 *entry = data + 16 + 20 * i;

		if (get_unaligned_be16(entry) == tag) {
			segment->data = data + get_unaligned_be32(entry + 4) + 8;
			segment->length = get_unaligned_be32(entry + 8) - 8;
			return 0;
		}
	}
	return -ENOENT;
}

struct mt7932_cal_piece {
	const u8 *data;
	u32 length, parameter;
	u8 type, fragment;
	u8 metadata[8];
};

/* Type1 group0/1, taking the validated operating center, not always primary.
 */
static inline int mt7932_cal_association(const u8 *data, size_t size, u8 channel,
					struct mt7932_cal_piece pieces[2])
{
	struct mt7932_cal_segment segment;
	unsigned int group, part;
	int ret;

	if (channel >= 1 && channel <= 13)
		group = 0;
	else if (channel >= 36 && channel <= 48 && !(channel % 2))
		group = 1;
	else
		return -EOPNOTSUPP;
	ret = mt7932_cal_validate(data, size);
	if (ret)
		return ret;
	ret = mt7932_cal_segment(data, 0x2000 | group, &segment);
	if (ret)
		return ret;
	if (!segment.length || segment.length % 2 || segment.length / 2 > 1080)
		return -EINVAL;
	for (part = 0; part < 2; part++)
		pieces[part] = (struct mt7932_cal_piece) {
			.data = segment.data + part * (segment.length / 2),
			.length = segment.length / 2,
			.parameter = 0x04000000 | (channel << 17) | group,
			.type = 1, .fragment = 0x20 | part,
		};
	return 2;
}

/* Plan all conditional power-on fragments before the first command is sent. */
static inline int mt7932_cal_power_on(const u8 *data, size_t size, bool six_ghz,
				      struct mt7932_cal_piece pieces[14])
{
	static const unsigned int tags[] = {0x1001, 0x1002, 0x1003, 0x1011, 0x1012, 0x1021, 0x1022};
	static const u8 groups[] = {0, 0, 0, 1, 1, 2, 2};
	static const u8 fragments[] = {0x30, 0x31, 0x32, 0x20, 0x21, 0x20, 0x21};
	static const u32 params[] = {0, 0x10000, 8, 0x10};
	struct mt7932_cal_segment segment;
	unsigned int i, part, count, at = 0;
	int ret = mt7932_cal_validate(data, size);

	if (ret)
		return ret;
	for (i = 0; i < (six_ghz ? 7 : 5); i++) {
		ret = mt7932_cal_segment(data, tags[i], &segment);
		if (ret)
			return ret;
		if (!segment.length || segment.length > 1080)
			return -EINVAL;
		pieces[at++] = (struct mt7932_cal_piece) {
			.data = segment.data, .length = segment.length,
			.parameter = groups[i], .type = 0, .fragment = fragments[i],
		};
	}
	for (i = 0; i < (six_ghz ? 4 : 3); i++) {
		u32 param = params[i];
		unsigned int tag = 0x2000 | (param & 0x7f) | ((param & 0x10000) ? 0x80 : 0);

		ret = mt7932_cal_segment(data, tag, &segment);
		if (ret)
			return ret;
		count = tag & 0x80 ? 1 : 2;
		if (!segment.length || segment.length % count || segment.length / count > 1080)
			return -EINVAL;
		for (part = 0; part < count; part++)
			pieces[at++] = (struct mt7932_cal_piece) {
				.data = segment.data + part * (segment.length / count),
				.length = segment.length / count, .parameter = param,
				.type = 1, .fragment = count << 4 | part,
			};
	}
	return at;
}

/* Bounded original-own-data5GHz D7 path. Plan all dependencies before send;
 * special upper0x1001 needs absent tag2081 and must not send a partial plan.
 */
static inline int mt7932_cal_requested_5g(const u8 *data, size_t size,
					 u32 upper, u32 channel, u8 version,
					 u8 module, struct mt7932_cal_piece pieces[7])
{
	struct mt7932_cal_segment segment;
	unsigned int i, part, at = 0, selected;
	const u8 types[] = {1, 3, 2};
	u32 params[3];
	unsigned int tags[3];
	int ret;

	if (upper != 0x1000 || version != 12 || module != 0x89 ||
	    channel < 36 || channel > 48 || channel % 2)
		return -EOPNOTSUPP;
	ret = mt7932_cal_validate(data, size);
	if (ret)
		return ret;
	ret = mt7932_cal_segment(data, 0x501, &segment);
	if (ret)
		return ret;
	if (segment.length != 200)
		return -EINVAL;
	/* No alternate override/disable bitmap is qualified for this own OCA2. */
	for (i = 0; i < segment.length; i++)
		if (segment.data[i])
			return -EOPNOTSUPP;
	/* Own row610f07 does not remap center selectors1/2; only primaries. */
	selected = channel % 4 ? channel : channel <= 40 ? 38 : 46;
	params[0] = 0x10000001;
	params[1] = 0x10001000 | channel;
	params[2] = 0x10001000 | (channel << 17) | selected;
	tags[0] = 0x2001;
	tags[1] = 0x4100 | channel;
	tags[2] = 0x3100 | selected;
	for (i = 0; i < 3; i++) {
		unsigned int count = i == 1 ? 1 : 2;
		unsigned int stride = i == 0 ? 1080 : i == 1 ? 20 : 612;
		unsigned int meta = i == 0 ? 0 : i == 1 ? 4 : 8;

		ret = mt7932_cal_segment(data, tags[i], &segment);
		if (ret)
			return ret;
		if (segment.length != count * stride)
			return -EINVAL;
		for (part = 0; part < count; part++) {
			const u8 *record = segment.data + part * stride;

			pieces[at] = (struct mt7932_cal_piece) {
				.data = record + meta, .length = stride - meta,
				.parameter = params[i], .type = types[i],
				.fragment = count << 4 | part,
			};
			memcpy(pieces[at++].metadata, record, meta);
		}
	}
	return at;
}

/* A genuine D7 event, not a fabricated channel-based startup trigger.
 * Initial PL profile: reject unqualified upper bits and all non-2G requests.
 */
static inline int mt7932_cal_requested(const u8 *data, size_t size,
				       const u8 *event, size_t length, u8 version,
				       u8 module, struct mt7932_cal_piece pieces[7])
{
	static const u8 rules[13][3] = {
		{0,15,3}, {0,15,3}, {0x31,7,3}, {0x21,7,3},
		{0}, {0}, {0}, {0}, {0}, {0}, {0}, {0x61,15,7}, {0x61,15,7},
	};
	static const u8 types[] = {1, 3, 2, 2};
	struct mt7932_cal_segment segment;
	u32 upper, band, channel, selected, p, action, q, params[4];
	u8 rule[3], disable[2] = {};
	unsigned int width, at = 0, i, part;
	int ret;

	if (length != 16 || version > 12)
		return -EINVAL;
	upper = get_unaligned_le32(event + 4);
	band = get_unaligned_le32(event + 8);
	channel = get_unaligned_le32(event + 12);
	if (band == 1)
		return mt7932_cal_requested_5g(data, size, upper, channel, version, module, pieces);
	if (band || !channel || channel > 13 || (upper & ~0x1001U))
		return -EOPNOTSUPP;
	ret = mt7932_cal_validate(data, size);
	if (ret)
		return ret;
	ret = mt7932_cal_segment(data, 0x501, &segment);
	if (ret)
		return ret;
	if (segment.length != 200)
		return -EINVAL;
	memcpy(rule, rules[version], 3);
	/* Validated file version12: versions2..10 suppress its rule override. */
	if (segment.data[1] && (version < 2 || version >= 11)) {
		memcpy(rule, segment.data, 3);
		memcpy(disable, segment.data + 3, 2);
	}
	width = channel == 3 || channel == 8 || channel == 12;
	if (!(rule[2] & 1) || !(rule[1] & (1U << width)))
		return -EOPNOTSUPP;
	selected = channel;
	if ((rule[0] & 16) && (rule[0] & (1U << width)) &&
	    !(disable[(channel - 1) / 8] & (1U << ((channel - 1) % 8))))
		selected = channel <= 5 ? 3 : channel <= 10 ? 8 : 12;
	p = upper << 16 | channel;
	action = p & 0x0e000000;
	q = (action >> 25) == 3 ? p : p & 0xf1ffffff;
	params[0] = (q & 0x7e000000) | (p & 0x10000);
	params[1] = q & 0x7e00ffff;
	params[2] = (q & 0x7fff0fff) ^ 0x10000;
	params[3] = q | action;
	for (i = 0; i < 4; i++) {
		u32 param = params[i];
		unsigned int tag, count, stride, meta, raw_count;

		if (types[i] == 1) {
			tag = 0x2000 | ((param & 0x10000) ? 0x80 : 0);
			count = tag & 0x80 ? 1 : 2;
			meta = 0;
		} else if (types[i] == 3) {
			tag = 0x4000 | channel;
			count = 1;
			meta = 4;
		} else {
			param = ((param | (channel << 17)) & ~0xfffU) | selected;
			tag = 0x3000 | selected | ((param & 0x10000) ? 0x80 : 0);
			raw_count = module & 0x80 ? ((module & 15) == 1 ? 4 : 8) : 2;
			if (tag & 0x80)
				raw_count /= 2;
			count = module & 15 ? (tag & 0x80 ? 1 : 3) : raw_count;
			meta = 8;
		}
		ret = mt7932_cal_segment(data, tag, &segment);
		if (ret)
			return ret;
		if (types[i] == 1) {
			if (!segment.length || segment.length % count)
				return -EINVAL;
			stride = segment.length / count;
		} else if (types[i] == 3) {
			if (segment.length != 20)
				return -EINVAL;
			stride = 20;
		} else {
			if (segment.length != raw_count * 612 || count > raw_count)
				return -EINVAL;
			stride = 612;
		}
		if (stride <= meta || stride - meta > 1080 || count > 7 - at)
			return -EINVAL;
		for (part = 0; part < count; part++) {
			const u8 *record = segment.data + part * stride;

			pieces[at] = (struct mt7932_cal_piece) {
				.data = record + meta, .length = stride - meta,
				.parameter = param, .type = types[i], .fragment = count << 4 | part,
			};
			memcpy(pieces[at].metadata, record, meta);
			at++;
		}
	}
	return at;
}

static inline int mt7932_cal_body(u8 *body, size_t capacity,
				  const struct mt7932_cal_piece *piece, u8 preload_version)
{
	if (capacity < 20 || !piece->length || piece->length > 1080 || piece->length > capacity - 20)
		return -EINVAL;
	memset(body, 0, 20);
	body[0] = piece->type;
	body[1] = preload_version ? 0xff : 0;
	body[2] = piece->fragment;
	body[3] = 1;
	put_unaligned_le32(piece->length, body + 4);
	put_unaligned_le32(piece->parameter, body + 8);
	memcpy(body + 12, piece->metadata, 8);
	memcpy(body + 20, piece->data, piece->length);
	return 20 + piece->length;
}

#endif
