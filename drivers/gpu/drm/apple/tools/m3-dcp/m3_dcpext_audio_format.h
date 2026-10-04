/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded OSSerialize format selection. Encoding: Hector Martin / Asahi;
 * format fields: Martin Povišer / Asahi parser.c. M3 14.6 cookies are 32 bytes, verified in live metadata-2.
 * Initial PCM qualification is stereo S16_LE at 48 kHz only.
 */
#ifndef M3_DCPEXT_AUDIO_FORMAT_H
#define M3_DCPEXT_AUDIO_FORMAT_H
struct audio_object {
	const u8 *raw;
	u32 size, pos, budget;
};
static int audio_object_tag(struct audio_object *o, u32 *type, u32 *count)
{
	u32 v;
	o->pos = (o->pos + 3) & ~3U;
	if (!o->budget-- || o->pos > o->size || o->size - o->pos < 4)
		return -EPROTO;
	v = get_unaligned_le32(o->raw + o->pos);
	o->pos += 4;
	if (v & 0x60000000)
		return -EPROTO;
	*type = (v >> 24) & 31;
	*count = v & 0xffffff;
	return 0;
}
static int audio_object_bytes(struct audio_object *o, u32 n, const u8 **p)
{
	if (o->pos > o->size || n > o->size - o->pos)
		return -EPROTO;
	*p = o->raw + o->pos;
	o->pos += n;
	return 0;
}
static int audio_object_skip(struct audio_object *o, unsigned int depth)
{
	u32 t, n;
	const u8 *p;
	int ret;
	if (depth > 12 || audio_object_tag(o, &t, &n))
		return -EPROTO;
	switch (t) {
	case 1:
	case 2:
		if (n > 256)
			return -E2BIG;
		for (u32 i = 0; i < n * (t == 1 ? 2 : 1); i++) {
			ret = audio_object_skip(o, depth + 1);
			if (ret)
				return ret;
		}
		return 0;
	case 4: return n == 64 ? audio_object_bytes(o, 8, &p) : -EPROTO;
	case 9:
	case 10: return audio_object_bytes(o, n, &p);
	case 11: return n <= 1 ? 0 : -EPROTO;
	default: return -EPROTO;
	}
}
static int audio_object_string(struct audio_object *o, const u8 **p, u32 *n)
{
	u32 t;
	if (audio_object_tag(o, &t, n) || t != 9)
		return -EPROTO;
	return audio_object_bytes(o, *n, p);
}
static bool audio_key(const u8 *p, u32 n, const char *key)
{
	return n == strlen(key) && !memcmp(p, key, n);
}
static int audio_object_number(struct audio_object *o, u64 *value)
{
	u32 t, n;
	const u8 *p;
	if (audio_object_tag(o, &t, &n) || t != 4 || n != 64 ||
	    audio_object_bytes(o, 8, &p))
		return -EPROTO;
	*value = get_unaligned_le64(p);
	return 0;
}
static int audio_stereo_layout(struct audio_object *o)
{
	u32 t, count;
	bool found = false;
	if (audio_object_tag(o, &t, &count) || t != 2 || count > 256)
		return -EPROTO;
	for (u32 i = 0; i < count; i++) {
		u32 pairs;
		u64 channels = 0, active = 0;
		bool stereo = false;
		if (audio_object_tag(o, &t, &pairs) || t != 1 || pairs > 64)
			return -EPROTO;
		for (u32 j = 0; j < pairs; j++) {
			const u8 *key;
			u32 len;
			int ret;
			if (audio_object_string(o, &key, &len))
				return -EPROTO;
			if (audio_key(key, len, "ChannelCount"))
				ret = audio_object_number(o, &channels);
			else if (audio_key(key, len, "ActiveChannelCount"))
				ret = audio_object_number(o, &active);
			else if (audio_key(key, len, "ChannelLayout")) {
				u32 n;
				const u8 *left, *right;
				u32 ln, rn;
				struct audio_object saved = *o;
				ret = audio_object_tag(o, &t, &n);
				if (!ret && t == 2 && n == 2 &&
				    !audio_object_string(o, &left, &ln) &&
				    !audio_object_string(o, &right, &rn))
					stereo = audio_key(left, ln, "Front Left") && audio_key(right, rn, "Front Right");
				*o = saved;
				ret = audio_object_skip(o, 0);
			} else
				ret = audio_object_skip(o, 0);
			if (ret)
				return ret;
		}
		found |= channels == 2 && active == 2 && stereo;
	}
	return found ? 1 : 0;
}
static int m3_audio_select_stereo(const u8 *raw, u32 bytes, u8 cookie[32])
{
	struct audio_object o = { .raw = raw, .size = bytes, .pos = 4, .budget = 32768 };
	u32 t, count;
	unsigned int matches = 0;
	if (bytes < 8 || bytes > 256 * 1024 || get_unaligned_le32(raw) != 0xd3 ||
	    audio_object_tag(&o, &t, &count) || t != 2 || count > 256)
		return -EPROTO;
	for (u32 i = 0; i < count; i++) {
		const u8 *blob = NULL;
		u32 pairs, blob_size = 0, seen = 0;
		u64 fields[6] = {0};
		bool is_virtual = true, stereo = false;
		static const char * const keys[] = { "ElementType", "Format", "ChannelCount", "SampleSize", "LinkSampleRate", "StreamSampleRate" };
		if (audio_object_tag(&o, &t, &pairs) || t != 1 || pairs > 64)
			return -EPROTO;
		for (u32 j = 0; j < pairs; j++) {
			const u8 *key;
			u32 len, k;
			int ret;
			if (audio_object_string(&o, &key, &len))
				return -EPROTO;
			for (k = 0; k < 6; k++)
				if (audio_key(key, len, keys[k]))
					break;
			if (k < 6) {
				if (seen & (1U << k))
					return -EPROTO;
				seen |= 1U << k;
				ret = audio_object_number(&o, &fields[k]);
			} else if (audio_key(key, len, "IsVirtual")) {
				u32 n;
				if (seen & 64)
					return -EPROTO;
				seen |= 64;
				ret = audio_object_tag(&o, &t, &n);
				if (ret || t != 11 || n > 1)
					return -EPROTO;
				is_virtual = n;
			} else if (audio_key(key, len, "ElementData")) {
				if (blob || audio_object_tag(&o, &t, &blob_size) || t != 10)
					return -EPROTO;
				ret = audio_object_bytes(&o, blob_size, &blob);
			} else if (audio_key(key, len, "AudioChannelLayoutElements")) {
				ret = audio_stereo_layout(&o);
				stereo = ret == 1;
				if (ret > 0)
					ret = 0;
			} else
				ret = audio_object_skip(&o, 0);
			if (ret)
				return ret;
		}
		if (seen != 127 || is_virtual || !stereo || fields[0] != 2 ||
		    fields[1] != 1 || fields[2] != 2 || fields[3] != 16 ||
		    fields[4] != 48000 || fields[5] != 48000)
			continue;
		if (!blob || blob_size != 32 || get_unaligned_le32(blob) != 1 ||
		    get_unaligned_le32(blob + 4) != 2 || get_unaligned_le32(blob + 8) != 16 ||
		    get_unaligned_le32(blob + 12) != 48000 ||
		    get_unaligned_le32(blob + 16) || get_unaligned_le32(blob + 20))
			return -EPROTO;
		memcpy(cookie, blob, 32);
		matches++;
	}
	return matches == 1 ? 0 : -ENODATA;
}
#endif
