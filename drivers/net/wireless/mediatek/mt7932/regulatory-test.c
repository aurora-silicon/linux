// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the regulatory snapshot logic and the power package
 * parser and channel filter. The packages are local files, so
 * the parser treats them as untrusted.
 */
#include <kunit/test.h>
#include <linux/prandom.h>

#include <linux/unaligned.h>

#include "regulatory.h"

/* A world-domain snapshot: channels 1, 6, 36 and 149, the 5 GHz ones NO_IR. */
static void reg_build(struct mt7932_reg_snapshot *reg)
{
	static const u16 channels[] = {1, 6, 36, 149};
	unsigned int i;

	memset(reg, 0, sizeof(*reg));
	memcpy(reg->alpha2, "00", 2);
	memcpy(reg->domain, "XZ", 2);
	reg->domain[4] = 1;
	reg->domain[5] = 2;
	reg->domain[8] = 2;
	reg->domain[9] = 2;
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		put_unaligned_le32(channels[i] > 14 ? IEEE80211_CHAN_NO_IR : 0,
				   reg->domain + 16 + i * 8);
		reg->power[i] = 20;
	}
	reg->length = 12 + ARRAY_SIZE(channels) * 8;
}

static void reg_set_flags(struct mt7932_reg_snapshot *reg, unsigned int entry, u32 flags)
{
	put_unaligned_le32(flags, reg->domain + 16 + entry * 8);
}

/* A beacon hint lifting NO_IR only relaxes; nothing else does. */
static void mt7932_reg_relaxation_test(struct kunit *test)
{
	struct mt7932_reg_snapshot *prev, *next;

	prev = kunit_kzalloc(test, sizeof(*prev), GFP_KERNEL);
	next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, prev);
	KUNIT_ASSERT_NOT_NULL(test, next);
	reg_build(prev);

	reg_build(next);
	reg_set_flags(next, 2, 0);
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, next));
	/* An identical snapshot is not treated as a change by the caller,
	 * but it is trivially within the applied domain.
	 */
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, prev));

	/* NO_IR added: a restriction. */
	reg_build(next);
	reg_set_flags(next, 0, IEEE80211_CHAN_NO_IR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* Radar or a bandwidth limit appears, or another flag goes away. */
	reg_build(next);
	reg_set_flags(next, 2, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_set_flags(prev, 3, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40);
	reg_build(next);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(prev);
	/* A new country, power limit, channel or channel order. */
	reg_build(next);
	memcpy(next->alpha2, "AU", 2);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	next->power[1] = 19;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(40, next->domain + 12 + 3 * 8);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(44, next->domain + 12 + 4 * 8);
	next->domain[9] = 3;
	next->length += 8;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* A failed snapshot never relaxes, and a corrupt length is refused. */
	reg_build(next);
	next->error = -EOPNOTSUPP;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	prev->length = sizeof(prev->domain) + 8;
	next->length = prev->length;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
}

#define PKG_MODE_SIZE 328
#define PKG_ENTRY_SIZE 122

/* Channels of the synthetic 2.4 and 5 GHz tables 1..9, in table order. */
static const u8 pkg_channels[] = {
	1, 2, 3, 4, 5, 6, 7, 8,
	9, 10, 11, 12, 13, 14,
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128,
	132, 136, 140, 144, 149, 153, 157, 161,
	38, 46, 54, 62, 102, 110, 118, 126,
	134, 142, 151, 159, 42, 58, 106, 122,
	138, 155, 50, 114, 130, 146, 163, 167,
	165, 169, 171, 173, 175,
};

static unsigned int pkg_count(unsigned int table, bool world)
{
	if (table == 2)
		return world ? 6 : 5;
	return table == 9 ? 5 : 8;
}

static size_t pkg_size(bool world)
{
	static const u16 others[] = {532, 144, 58, 265};
	size_t size = 16 + (world ? 2 : 3) * PKG_MODE_SIZE;
	unsigned int i;

	for (i = 1; i <= 9; i++)
		size += 44 + pkg_count(i, world) * PKG_ENTRY_SIZE;
	for (i = 0; i < ARRAY_SIZE(others); i++)
		size += others[i];
	return size;
}

/* Build a structurally valid package. Channel 14 is only in the world one. */
static u8 *pkg_build(struct kunit *test, const char *alpha2, size_t *size)
{
	static const u16 lengths[] = {532, 144, 58, 265};
	static const u8 types[] = {5, 1, 4, 2};
	bool world = !memcmp(alpha2, "XZ", 2);
	unsigned int i, j, modes = world ? 2 : 3, channel = 0;
	u8 *data, *p;

	*size = pkg_size(world);
	data = kunit_kzalloc(test, *size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "J7RP", 4);
	put_unaligned_le16(1, data + 4);
	memcpy(data + 6, alpha2, 2);
	data[8] = modes;
	data[9] = MT7932_POLICY_TABLES;
	put_unaligned_le32(*size, data + 12);
	p = data + 16;
	for (i = 0; i < modes; i++, p += PKG_MODE_SIZE)
		put_unaligned_le32(17 + i, p + 4);
	for (i = 0; i < MT7932_POLICY_TABLES; i++) {
		if (i >= 1 && i <= 9) {
			unsigned int count = pkg_count(i, world);

			p[0] = 3;
			p[1] = 0x0a;
			put_unaligned_le16(44 + count * PKG_ENTRY_SIZE, p + 2);
			p[4] = count;
			p[5] = i <= 2 ? 1 : 2;
			p[6] = i == 2 || i == 9;
			memcpy(p + 8, alpha2, 2);
			for (j = 0; j < count; j++) {
				u8 *entry = p + 44 + j * PKG_ENTRY_SIZE;

				/* A country package has no channel 14 entry. */
				if (!world && pkg_channels[channel] == 14)
					channel++;
				entry[0] = pkg_channels[channel++];
				memset(entry + 1, 30, PKG_ENTRY_SIZE - 1);
			}
			p += 44 + count * PKG_ENTRY_SIZE;
		} else {
			unsigned int k = i ? i - 9 : 0;

			p[1] = 2;
			put_unaligned_le16(lengths[k], p + 2);
			p[7] = types[k];
			if (i == 10)
				memcpy(p + 8, alpha2, 2);
			p += lengths[k];
		}
	}
	KUNIT_ASSERT_PTR_EQ(test, p, data + *size);
	return data;
}

/* The entry of @channel in a parsed package, or NULL. */
static u8 *pkg_entry(const struct mt7932_policy *policy, unsigned int channel)
{
	unsigned int i, j;

	for (i = 1; i <= 9; i++)
		for (j = 0; j < policy->table[i][4]; j++)
			if (policy->table[i][44 + j * PKG_ENTRY_SIZE] == channel)
				return (u8 *)policy->table[i] + 44 + j * PKG_ENTRY_SIZE;
	return NULL;
}

static void mt7932_policy_valid_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "AU", &size);
	KUNIT_EXPECT_EQ(test, size, 10447);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	KUNIT_EXPECT_EQ(test, policy.modes, 3);
	KUNIT_EXPECT_PTR_EQ(test, policy.table[0], data + 16 + 3 * PKG_MODE_SIZE);
	KUNIT_EXPECT_EQ(test, policy.length[12], 265);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 36));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_2GHZ, 14));

	data = pkg_build(test, "XZ", &size);
	KUNIT_EXPECT_EQ(test, size, 10241);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), 0);
	KUNIT_EXPECT_EQ(test, policy.modes, 2);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_2GHZ, 14));
}

/* A package serves only the country it names, in the header and table 10. */
static void mt7932_policy_country_mismatch_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "AU", &size);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
	/* A country package renamed to the world one is not the world one. */
	memcpy(data + 6, "XZ", 2);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
}

/* Every prefix of a valid package, with a consistent header, is rejected. */
static void mt7932_policy_truncation_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size, cut;
	u8 *data, *copy;

	data = pkg_build(test, "PL", &size);
	for (cut = 0; cut < size; cut++) {
		/* An exact-size copy lets KASAN catch any read past the end. */
		copy = kmemdup(data, cut, GFP_KERNEL);
		KUNIT_ASSERT_TRUE(test, copy || !cut);
		if (cut >= 16)
			put_unaligned_le32(cut, copy + 12);
		KUNIT_EXPECT_EQ_MSG(test, mt7932_policy_parse(&policy, copy, cut, "PL"),
				    -EINVAL, "length %zu", cut);
		kfree(copy);
	}
}

static void mt7932_policy_header_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *longer;

	data = pkg_build(test, "PL", &size);
	/* Trailing bytes after the last table. */
	longer = kunit_kzalloc(test, size + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, longer);
	memcpy(longer, data, size);
	put_unaligned_le32(size + 4, longer + 12);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, longer, size + 4, "PL"), -EINVAL);

	data[4] = 2;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[4] = 1;
	data[8] = 4;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[8] = 3;
	data[9] = 12;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[9] = MT7932_POLICY_TABLES;
	put_unaligned_le32(size - 1, data + 12);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(size, data + 12);
	/* Mode command lengths are bounded by the 320-byte body. */
	put_unaligned_le32(321, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(0, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(17, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
}

/* Table lengths and entry counts must agree with the table type. */
static void mt7932_policy_table_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *table;

	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	table = (u8 *)policy.table[3];
	/* An entry count that would run past the table. */
	table[4] = 9;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	table[4] = 8;
	/* A table length that claims the rest of the file. */
	put_unaligned_le16(0xffff, table + 2);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le16(44 + 8 * PKG_ENTRY_SIZE, table + 2);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	table = (u8 *)policy.table[10];
	table[7] = 2;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
}

/* The world package's 2.4 GHz tables must list channels 1..14 in order. */
static void mt7932_policy_world_order_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "XZ", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), 0);
	pkg_entry(&policy, 9)[0] = 10;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
}

static void mt7932_policy_permits_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *entry;

	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	entry = pkg_entry(&policy, 144);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	memset(entry + 1, MT7932_POLICY_NOT_PERMITTED, PKG_ENTRY_SIZE - 1);
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 144));
	/* One rate with a limit is enough to permit the channel. */
	entry[PKG_ENTRY_SIZE - 1] = 8;
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 144));
	/* A channel the package does not list is not permitted. */
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 177));
}

/* A table's band key must agree even when the numeric channel matches. */
static void mt7932_policy_band_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "AU", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_2GHZ, 5));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 5));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_6GHZ, 5));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_2GHZ, 149));
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_5GHZ, 149));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_6GHZ, 149));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NUM_NL80211_BANDS, 5));
}

static void mt7932_policy_filter_test(struct kunit *test)
{
	static const u16 channels[] = {1, 6, 13, 36, 144, 149, 165};
	struct mt7932_reg_snapshot *reg;
	struct mt7932_policy policy;
	unsigned int i;
	size_t size;
	u8 *data;

	reg = kunit_kzalloc(test, sizeof(*reg), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	memset(pkg_entry(&policy, 144) + 1, MT7932_POLICY_NOT_PERMITTED,
	       PKG_ENTRY_SIZE - 1);
	memset(pkg_entry(&policy, 6) + 1, MT7932_POLICY_NOT_PERMITTED,
	       PKG_ENTRY_SIZE - 1);
	memcpy(reg->domain, "PL", 2);
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		put_unaligned_le32(i, reg->domain + 16 + i * 8);
	}
	reg->domain[8] = 3;
	reg->domain[9] = 4;
	reg->length = 12 + ARRAY_SIZE(channels) * 8;

	mt7932_policy_filter(reg, &policy);
	KUNIT_EXPECT_EQ(test, reg->length, 12 + 5 * 8);
	KUNIT_EXPECT_EQ(test, reg->domain[8], 2);
	KUNIT_EXPECT_EQ(test, reg->domain[9], 3);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(reg->domain + 12 + 8), 13);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(reg->domain + 12 + 3 * 8), 149);
	/* Each entry keeps its own channel flags. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reg->domain + 16 + 3 * 8), 5);
	/* The vacated tail is cleared. */
	KUNIT_EXPECT_TRUE(test, !memchr_inv(reg->domain + 12 + 5 * 8, 0, 2 * 8));
}

static void mt7932_policy_6g_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t base_size, size;
	u8 *base, *data, *ext;
	unsigned int i;

	base = pkg_build(test, "AU", &base_size);
	size = base_size + 24 + 2 * 1024;
	data = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, base, base_size);
	put_unaligned_le16(2, data + 4);
	put_unaligned_le32(size, data + 12);
	ext = data + base_size;
	memcpy(ext, "6GRP", 4);
	ext[4] = 1;
	ext[5] = 14;
	ext[6] = 2;
	put_unaligned_le64(BIT_ULL(1) | BIT_ULL(9), ext + 8);
	memcpy(ext + 16, "AU", 2);
	ext[18] = 1;
	for (i = 0; i < 2; i++) {
		u8 *record = ext + 24 + i * 1024, *page = record + 4;

		record[0] = 0x5d;
		put_unaligned_le16(1020, record + 2);
		page[0] = 3;
		page[1] = 0x2a;
		put_unaligned_le16(1020, page + 2);
		page[4] = 8;
		page[5] = 3;
		memcpy(page + 8, "AU", 2);
		page[44] = i ? 0x0f : 0x02;
	}
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	KUNIT_EXPECT_EQ(test, policy.six_pages, 2);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_6GHZ, 5));
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, NL80211_BAND_6GHZ, 37));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, NL80211_BAND_6GHZ, 1));
	KUNIT_EXPECT_TRUE(test, policy.assoc_power[0]);
	KUNIT_EXPECT_TRUE(test, policy.assoc_power[1]);
	/* Bound every extension record before dereferencing it. */
	for (i = base_size; i < size; i++) {
		put_unaligned_le32(i, data + 12);
		KUNIT_EXPECT_LT(test, mt7932_policy_parse(&policy, data, i, "AU"), 0);
	}
	put_unaligned_le32(size, data + 12);
	ext[16] = 'U';
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	ext[16] = 'A';
	ext[24 + 1024 + 4 + 44] = 2;
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	ext[24 + 1024 + 4 + 44] = 0x0f;
	ext[24 + 4 + 1] = 0x0a;
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	ext[24 + 4 + 1] = 0x2a;
	put_unaligned_le64(BIT_ULL(59), ext + 8);
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
}

/* Random corruption must never be accepted in a way that reads out of bounds. */
static void mt7932_policy_corruption_test(struct kunit *test)
{
	struct rnd_state rnd;
	struct mt7932_policy policy;
	unsigned int round, flips, channel, band;
	size_t size;
	u8 *data, *copy;

	prandom_seed_state(&rnd, 7932);
	data = pkg_build(test, "AU", &size);
	copy = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	for (round = 0; round < 2000; round++) {
		memcpy(copy, data, size);
		for (flips = 1 + prandom_u32_state(&rnd) % 4; flips; flips--)
			copy[prandom_u32_state(&rnd) % size] ^= 1 << (prandom_u32_state(&rnd) % 8);
		if (mt7932_policy_parse(&policy, copy, size, "AU"))
			continue;
		for (band = 0; band < NUM_NL80211_BANDS; band++)
			for (channel = 0; channel < 256; channel++)
				mt7932_policy_permits(&policy, band, channel);
	}
}

static void mt7932_6g_power_limit_test(struct kunit *test)
{
	struct mt7932_6g_limits limits = {.eirp = 24, .psd = -10};
	u8 page[44 + 122] = {};
	unsigned int i;

	page[4] = 1;
	page[44] = 5;
	memset(page + 45, 28, 121);
	page[45] = 0xc4;
	page[46] = 0x7f;
	mt7932_6g_limit_page(page, &limits);
	KUNIT_EXPECT_EQ(test, page[44], 5);
	KUNIT_EXPECT_EQ(test, page[45], 0xc4);
	KUNIT_EXPECT_EQ(test, page[46], 0x7f);
	for (i = 47; i < sizeof(page); i++)
		KUNIT_EXPECT_LE(test, (s8)page[i], 24);
	/* First RU26 cell: -5 dBm/MHz plus at most 3 dB occupied-band gain. */
	KUNIT_EXPECT_EQ(test, (s8)page[45 + 1 + 3 + 6 * 9], -4);
	limits.psd = -256;
	mt7932_6g_limit_page(page, &limits);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(page + 47, 0xc4, sizeof(page) - 47));
}

/* A J7RP v2 package: @alpha2's v1 tables plus VLP pages for @primaries. */
static u8 *pkg_build_v2(struct kunit *test, const char *alpha2, u64 primaries, size_t *size)
{
	size_t base_size;
	u8 *base, *data, *ext;
	unsigned int i;

	base = pkg_build(test, alpha2, &base_size);
	*size = base_size + 24 + 2 * 1024;
	data = kunit_kzalloc(test, *size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, base, base_size);
	put_unaligned_le16(2, data + 4);
	put_unaligned_le32(*size, data + 12);
	ext = data + base_size;
	memcpy(ext, "6GRP", 4);
	ext[4] = 1;
	ext[5] = 14;
	ext[6] = 2;
	put_unaligned_le64(primaries, ext + 8);
	memcpy(ext + 16, alpha2, 2);
	ext[18] = 1;
	for (i = 0; i < 2; i++) {
		u8 *record = ext + 24 + i * 1024, *page = record + 4;

		record[0] = 0x5d;
		put_unaligned_le16(1020, record + 2);
		page[0] = 3;
		page[1] = 0x2a;
		put_unaligned_le16(1020, page + 2);
		page[4] = 8;
		page[5] = 3;
		memcpy(page + 8, alpha2, 2);
		page[44] = i ? 0x0f : 0x02;
	}
	return data;
}

static u8 *pkg_build_v4(struct kunit *test, size_t *size)
{
	size_t base_size;
	u8 *base = pkg_build(test, "AU", &base_size), *data, *directory;
	unsigned int i, j;

	*size = base_size + 16 + 2 * 13476;
	data = kunit_kzalloc(test, *size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, base, base_size);
	put_unaligned_le16(4, data + 4);
	put_unaligned_le32(*size, data + 12);
	directory = data + base_size;
	memcpy(directory, "6GCL", 4);
	directory[4] = 1;
	directory[5] = 2;
	directory[6] = 1;
	put_unaligned_le32(16 + 2 * 13476, directory + 8);
	for (i = 0; i < 2; i++) {
		u8 *section = directory + 16 + i * 13476;
		unsigned int channel = 1;

		section[0] = i + 1;
		section[1] = i;
		section[2] = 1;
		section[3] = 110;
		put_unaligned_le32(13476, section + 4);
		put_unaligned_le64(BIT_ULL(i ? 1 : 9), section + 8);
		memcpy(section + 16, "AU", 2);
		for (j = 0; j < 110; j++) {
			u8 *record = section + 56 + j * 122;

			record[0] = j ? channel : 2;
			memset(record + 1, i ? 40 : 20, 121);
			if (j) {
				channel += 2;
				while (channel == 31 || channel == 63 ||
				       channel == 95 || channel == 127 ||
				       channel == 159 || channel == 191 ||
				       channel == 223 || channel == 231)
					channel += 2;
			}
		}
	}
	return data;
}

static void mt7932_policy_v4_classes_test(struct kunit *test)
{
	struct mt7932_policy *policy =
		kunit_kzalloc(test, sizeof(*policy), GFP_KERNEL);
	size_t size, base_size;
	u8 *data = pkg_build_v4(test, &size), *body, *ext;
	unsigned int first, total = 0;
	int length;

	KUNIT_ASSERT_NOT_NULL(test, policy);
	body = kunit_kzalloc(test, 1021, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
	KUNIT_EXPECT_EQ(test, policy->six_class[MT7932_6G_LPI].channels,
			BIT_ULL(1));
	KUNIT_EXPECT_EQ(test, policy->six_class[MT7932_6G_VLP].channels,
			BIT_ULL(9));
	KUNIT_EXPECT_TRUE(test,
			  mt7932_policy_permits(policy, NL80211_BAND_6GHZ, 5));
	KUNIT_EXPECT_TRUE(test,
			  mt7932_policy_permits(policy, NL80211_BAND_6GHZ, 37));
	KUNIT_EXPECT_FALSE(test,
			   mt7932_policy_permits(policy, NL80211_BAND_6GHZ, 1));
	KUNIT_EXPECT_LT(test,
			mt7932_6g_class_page(body, 1020, policy, MT7932_6G_SP,
					     "AU", 0),
			0);
	for (first = 0; first < 110; first += 8) {
		length = mt7932_6g_class_page(body, 1020, policy, MT7932_6G_LPI,
					      "AU", first);
		KUNIT_ASSERT_GT(test, length, 0);
		KUNIT_EXPECT_EQ(test, body[0], 0);
		KUNIT_EXPECT_EQ(test, body[1], 0x1a);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 2), length);
		KUNIT_EXPECT_EQ(test, body[5], 3);
		KUNIT_EXPECT_EQ(test, body[6], first == 104);
		KUNIT_EXPECT_EQ(test, body[45], 40);
		total += body[4];
	}
	KUNIT_EXPECT_EQ(test, total, 110U);
	KUNIT_EXPECT_EQ(test, length, 776);
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_class_page(body, 1020, policy, MT7932_6G_VLP,
					     "AU", 0),
			1020);
	KUNIT_EXPECT_EQ(test, body[1], 0x2a);
	KUNIT_EXPECT_EQ(test, body[45], 20);
	KUNIT_EXPECT_LT(test,
			mt7932_6g_class_page(body, 1019, policy, MT7932_6G_LPI,
					     "AU", 0),
			0);
	KUNIT_EXPECT_LT(test,
			mt7932_6g_class_page(body, 1020, policy, MT7932_6G_LPI,
					     "AU", 1),
			0);
	base_size = size - 16 - 2 * 13476;
	ext = data + base_size;
	/* Unbound v3 cannot acquire runtime authority through a path rename. */
	put_unaligned_le16(3, data + 4);
	ext[6] = 0;
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
	put_unaligned_le16(4, data + 4);
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
	ext[6] = 1;
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size - 1, "AU"),
			0);
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size, "NZ"), 0);
	ext[16 + 13476] = 3; /* never serialize SP */
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
	ext[16 + 13476] = 2;
	ext[16 + 56] = 1; /* first record must be channel 2 */
	KUNIT_EXPECT_LT(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
}

/* The public producer's 32-byte 1SS channel pad is copied verbatim. It is
 * independent of the primary permission mask and of rate-support accounting.
 */
static void mt7932_policy_6g_pad_test(struct kunit *test)
{
	static const u8 expected[44] = {
		[1] = 0x1a, [2] = 0xfc, 3, 8, 3,
		[8] = 'A', 'U',
		[12] = 0x20, [16] = 0x20, [41] = 2,
	};
	struct mt7932_policy *policy =
		kunit_kzalloc(test, sizeof(*policy), GFP_KERNEL);
	size_t size;
	u8 *data = pkg_build_v4(test, &size), *body, *pad;

	KUNIT_ASSERT_NOT_NULL(test, policy);
	body = kunit_kmalloc(test, 1020, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	/* Directory plus VLP section, then LPI header and its channel pad. */
	pad = data + size - 13476 + 24;
	pad[0] = 0x20; /* channel 5 */
	pad[4] = 0x20; /* channel 37, not rate index 37 */
	pad[29] = 2; /* channel 233 */
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(policy, data, size, "AU"), 0);
	KUNIT_EXPECT_EQ(test, policy->six_class[MT7932_6G_LPI].channels,
			BIT_ULL(1));
	KUNIT_ASSERT_EQ(test, mt7932_6g_class_page(body, 1020, policy,
						   MT7932_6G_LPI, "AU", 0), 1020);
	KUNIT_EXPECT_MEMEQ(test, body, expected, sizeof(expected));
	KUNIT_ASSERT_EQ(test, mt7932_6g_class_page(body, 1020, policy,
						   MT7932_6G_LPI, "AU", 104), 776);
	KUNIT_EXPECT_MEMEQ(test, body + 12, pad, 32);
	KUNIT_ASSERT_EQ(test, mt7932_6g_class_page(body, 1020, policy,
						   MT7932_6G_VLP, "AU", 0), 1020);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(body + 12, 0, 32), NULL);
}

#define FAKE_FILES 8
#define FAKE_LOG 16

/* An in-memory firmware directory that records every lookup. */
struct fake_fs {
	struct {
		const char *path;
		const u8 *data;
		size_t size;
	} file[FAKE_FILES];
	unsigned int files, held;
	char log[FAKE_LOG][MT7932_POLICY_PATH_MAX];
	unsigned int requests;
};

static int fake_get(void *ctx, const char *path, struct mt7932_policy_file *file)
{
	struct fake_fs *fs = ctx;
	unsigned int i;

	if (fs->requests < FAKE_LOG)
		strscpy(fs->log[fs->requests], path, MT7932_POLICY_PATH_MAX);
	fs->requests++;
	for (i = 0; i < fs->files; i++) {
		if (strcmp(fs->file[i].path, path))
			continue;
		file->handle = fs;
		file->data = fs->file[i].data;
		file->size = fs->file[i].size;
		fs->held++;
		return 0;
	}
	return -ENOENT;
}

static void fake_put(void *ctx, struct mt7932_policy_file *file)
{
	struct fake_fs *fs = ctx;

	fs->held -= file->class_handle ? 2 : 1;
	file->handle = NULL;
	file->class_handle = NULL;
}

static void fake_add(struct fake_fs *fs, const char *path, const u8 *data, size_t size)
{
	fs->file[fs->files].path = path;
	fs->file[fs->files].data = data;
	fs->file[fs->files].size = size;
	fs->files++;
}

/* A country snapshot as the notifier builds it: 1, 6, 13, 36, 149 and,
 * unless @no6g, the 6 GHz primaries 5 and 37.
 */
static void reg_build_country(struct mt7932_reg_snapshot *reg, const char *alpha2, bool no6g)
{
	static const u16 channels[] = {1, 6, 13, 36, 149, 5, 37};
	unsigned int i, n = no6g ? 5 : 7;

	memset(reg, 0, sizeof(*reg));
	memcpy(reg->alpha2, alpha2, 2);
	memcpy(reg->domain, alpha2, 2);
	if (!memcmp(alpha2, "00", 2))
		memcpy(reg->domain, "XZ", 2);
	reg->domain[4] = 1;
	reg->domain[5] = 2;
	reg->domain[6] = 2;
	reg->domain[8] = 3;
	reg->domain[9] = 2;
	reg->domain[10] = n - 5;
	for (i = 0; i < n; i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		reg->power[i] = 20;
	}
	reg->length = 12 + n * 8;
}

struct select_env {
	struct fake_fs fs;
	struct mt7932_policy policy;
	struct mt7932_policy_file file;
	struct mt7932_policy_choice choice;
	struct mt7932_reg_snapshot reg;
	struct mt7932_policy_loader loader;
};

static struct select_env *select_env(struct kunit *test)
{
	struct select_env *env = kunit_kzalloc(test, sizeof(*env), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, env);
	env->loader.get = fake_get;
	env->loader.put = fake_put;
	env->loader.ctx = &env->fs;
	return env;
}

static int select_run(struct select_env *env, const char *alpha2, bool no6g)
{
	if (env->file.handle)
		fake_put(&env->fs, &env->file);
	env->fs.requests = 0;
	reg_build_country(&env->reg, alpha2, no6g);
	return mt7932_policy_select(&env->loader, &env->reg, &env->policy, &env->file,
				    &env->choice);
}

/* Lookup order: policy-v2/CC, policy/CC, world; 00 only the world package. */
static void mt7932_policy_order_test(struct kunit *test)
{
	enum mt7932_policy_source sources[MT7932_POLICY_CANDIDATES];
	char path[MT7932_POLICY_PATH_MAX];
	struct select_env *env = select_env(test);

	KUNIT_ASSERT_EQ(test, mt7932_policy_candidates("AU", sources), 4);
	KUNIT_EXPECT_EQ(test, sources[0], MT7932_POLICY_V4);
	KUNIT_EXPECT_EQ(test, sources[1], MT7932_POLICY_V2);
	KUNIT_EXPECT_EQ(test, sources[2], MT7932_POLICY_V1);
	KUNIT_EXPECT_EQ(test, sources[3], MT7932_POLICY_WORLD);
	KUNIT_ASSERT_EQ(test, mt7932_policy_candidates("00", sources), 1);
	KUNIT_EXPECT_EQ(test, sources[0], MT7932_POLICY_WORLD);
	mt7932_policy_path(path, sizeof(path), MT7932_POLICY_V2, "AU");
	KUNIT_EXPECT_STREQ(test, path, "mediatek/mt7932/policy-v2/AU.bin");
	mt7932_policy_path(path, sizeof(path), MT7932_POLICY_V1, "AU");
	KUNIT_EXPECT_STREQ(test, path, "mediatek/mt7932/policy/AU.bin");
	mt7932_policy_path(path, sizeof(path), MT7932_POLICY_WORLD, "AU");
	KUNIT_EXPECT_STREQ(test, path, "mediatek/mt7932/policy/world-XZ.bin");

	/* Nothing installed: every candidate is asked for, in order. */
	KUNIT_EXPECT_EQ(test, select_run(env, "NZ", false), -ENOENT);
	KUNIT_ASSERT_EQ(test, env->fs.requests, 4);
	KUNIT_EXPECT_STREQ(test, env->fs.log[0],
			   "mediatek/mt7932/policy-v4/NZ.bin");
	KUNIT_EXPECT_STREQ(test, env->fs.log[1],
			   "mediatek/mt7932/policy-v2/NZ.bin");
	KUNIT_EXPECT_STREQ(test, env->fs.log[2],
			   "mediatek/mt7932/policy/NZ.bin");
	KUNIT_EXPECT_STREQ(test, env->fs.log[3],
			   "mediatek/mt7932/policy/world-XZ.bin");
	KUNIT_EXPECT_PTR_EQ(test, env->file.handle, NULL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_NONE);
	KUNIT_EXPECT_EQ(test, env->choice.n_tried, 4);

	/* cfg80211's numeric codes have no package of their own. */
	KUNIT_EXPECT_EQ(test, mt7932_policy_candidates("98", sources), 1);
	KUNIT_EXPECT_EQ(test, mt7932_policy_candidates("99", sources), 1);
	KUNIT_EXPECT_FALSE(test, mt7932_policy_country("00"));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_country("aU"));
	KUNIT_EXPECT_TRUE(test, mt7932_policy_country("AU"));

	/* 00 never opens a country file. */
	KUNIT_EXPECT_EQ(test, select_run(env, "00", true), -ENOENT);
	KUNIT_ASSERT_EQ(test, env->fs.requests, 1);
	KUNIT_EXPECT_STREQ(test, env->fs.log[0], "mediatek/mt7932/policy/world-XZ.bin");
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

/* v2 wins over v1 and grants its 6 GHz primaries; a bad v2 gives way to v1. */
static void mt7932_policy_v2_preferred_test(struct kunit *test)
{
	struct select_env *env = select_env(test);
	size_t v1_size, v2_size, xz_size;
	u8 *v1, *v2, *xz;

	v1 = pkg_build(test, "AU", &v1_size);
	v2 = pkg_build_v2(test, "AU", BIT_ULL(1) | BIT_ULL(9), &v2_size);
	xz = pkg_build(test, "XZ", &xz_size);
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", v2, v2_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/AU.bin", v1, v1_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);

	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->fs.requests, 2);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V2);
	KUNIT_EXPECT_EQ(test, env->choice.version, 2);
	KUNIT_EXPECT_EQ(test, env->choice.size, v2_size);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, 0);
	KUNIT_EXPECT_MEMEQ(test, env->choice.domain, "AU", 2);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 2);
	KUNIT_EXPECT_EQ(test, env->reg.domain[10], 2);
	KUNIT_EXPECT_EQ(test, env->reg.length, 12 + 7 * 8);
	KUNIT_EXPECT_PTR_EQ(test, env->file.data, (const u8 *)v2);
	KUNIT_EXPECT_EQ(test, env->fs.held, 1);

	/* A v2 package that grants only channel 5 keeps 37 out. */
	put_unaligned_le64(BIT_ULL(1), v2 + v1_size + 8);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(env->reg.domain + 12 + 5 * 8), 5);

	/* Malformed v2: v1 is used and 6 GHz leaves the firmware domain. */
	put_unaligned_le32(v2_size - 1, v2 + 12);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->fs.requests, 3);
	KUNIT_EXPECT_EQ(test, env->choice.tried[1].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V1);
	KUNIT_EXPECT_EQ(test, env->choice.version, 1);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, 0);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 0);
	KUNIT_EXPECT_EQ(test, env->reg.domain[10], 0);
	KUNIT_EXPECT_EQ(test, env->reg.length, 12 + 5 * 8);
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&env->policy, NL80211_BAND_6GHZ, 5));
	KUNIT_EXPECT_EQ(test, env->fs.held, 1);
	fake_put(&env->fs, &env->file);
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

static void mt7932_policy_v4_preferred_test(struct kunit *test)
{
	struct select_env *env = select_env(test);
	struct mt7932_policy *baseline =
		kunit_kzalloc(test, sizeof(*baseline), GFP_KERNEL);
	size_t size, v2_size;
	u8 *data = pkg_build_v4(test, &size);
	u8 *v2 = pkg_build_v2(test, "AU", BIT_ULL(9), &v2_size);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, baseline);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(baseline, v2, v2_size, "AU"),
			0);
	fake_add(&env->fs, "mediatek/mt7932/policy-v4/AU.bin", data, size);
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", v2, v2_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V2);
	KUNIT_EXPECT_EQ(test, env->choice.version, 2);
	KUNIT_EXPECT_TRUE(test, env->choice.class_valid);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 2);
	KUNIT_EXPECT_EQ(test, env->policy.legacy_channels, BIT_ULL(9));
	KUNIT_EXPECT_EQ(test, env->fs.requests, 2);
	KUNIT_EXPECT_EQ(test, env->fs.held, 2);
	/* Every original startup CID/body, and both post-queue association
	 * pages, are retained byte for byte even with the LPI sidecar.
	 */
	KUNIT_ASSERT_EQ(test, env->policy.six_pages, baseline->six_pages);
	for (i = 0; i < baseline->six_pages; i++) {
		KUNIT_EXPECT_EQ(test, env->policy.six_cid[i],
				baseline->six_cid[i]);
		KUNIT_EXPECT_EQ(test, env->policy.six_length[i],
				baseline->six_length[i]);
		KUNIT_EXPECT_MEMEQ(test, env->policy.six_page[i],
				   baseline->six_page[i],
				   baseline->six_length[i]);
	}
	for (i = 0; i < 2; i++)
		KUNIT_EXPECT_MEMEQ(test, env->policy.assoc_power[i],
				   baseline->assoc_power[i], 1020);
	/* Extra LP channels must not change the legacy VLP numeric clamp. */
	env->reg.power[mt7932_6g_reg_index(5)] = -1;
	env->reg.power[mt7932_6g_reg_index(37)] = 14;
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_limits_mask(&env->reg,
					      env->policy.legacy_channels)
				.eirp,
			28);
	KUNIT_EXPECT_EQ(test, mt7932_6g_limits(&env->reg).eirp, -2);
	data[4] = 3;
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V2);
	KUNIT_EXPECT_FALSE(test, env->choice.class_valid);
	KUNIT_EXPECT_EQ(test, env->choice.tried[0].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 1);
	fake_put(&env->fs, &env->file);
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

/* Each directory accepts one version, and the size limits hold. */
static void mt7932_policy_version_test(struct kunit *test)
{
	struct select_env *env = select_env(test);
	size_t v1_size, v2_size, xz_size, big_size = 16385, huge_size = 65537;
	u8 *v1, *v2, *xz, *big, *huge;

	v1 = pkg_build(test, "AU", &v1_size);
	v2 = pkg_build_v2(test, "AU", BIT_ULL(1), &v2_size);
	xz = pkg_build(test, "XZ", &xz_size);
	big = kunit_kzalloc(test, big_size, GFP_KERNEL);
	huge = kunit_kzalloc(test, huge_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, big);
	KUNIT_ASSERT_NOT_NULL(test, huge);

	/* A v2 package in policy/, where kernels without v2 look. */
	fake_add(&env->fs, "mediatek/mt7932/policy/AU.bin", v2, v2_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.tried[1].error, -ENOENT);
	KUNIT_EXPECT_EQ(test, env->choice.tried[2].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 0);

	/* A v1 package in policy-v2/ is not used either; policy/ still is. */
	env->fs.files = 0;
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", v1, v1_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/AU.bin", v1, v1_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.tried[1].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V1);

	/* Over 16 KiB as v1, over 64 KiB as v2. */
	memcpy(big, v1, v1_size);
	put_unaligned_le32(big_size, big + 12);
	memcpy(huge, v2, v2_size);
	put_unaligned_le32(huge_size, huge + 12);
	env->fs.files = 0;
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", huge, huge_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/AU.bin", big, big_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.tried[1].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.tried[2].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);

	/* Another country's package under this country's name. */
	env->fs.files = 0;
	fake_add(&env->fs, "mediatek/mt7932/policy/NZ.bin", v1, v1_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "NZ", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, -EINVAL);

	/* A truncated world package leaves nothing to fall back to. */
	env->fs.files = 0;
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size - 1);
	KUNIT_EXPECT_EQ(test, select_run(env, "NZ", false), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, env->file.handle, NULL);
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

/* The world fallback keeps the country, intersects channels, and has no 6 GHz. */
static void mt7932_policy_fallback_test(struct kunit *test)
{
	struct select_env *env = select_env(test);
	struct mt7932_policy world;
	size_t xz_size;
	unsigned int i;
	u8 *xz;

	xz = pkg_build(test, "XZ", &xz_size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&world, xz, xz_size, "XZ"), 0);
	/* The world profile forbids 149; the country lists it. */
	memset(pkg_entry(&world, 149) + 1, MT7932_POLICY_NOT_PERMITTED, PKG_ENTRY_SIZE - 1);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);

	KUNIT_ASSERT_EQ(test, select_run(env, "CA", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, -ENOENT);
	KUNIT_EXPECT_EQ(test, env->choice.version, 1);
	KUNIT_EXPECT_MEMEQ(test, env->choice.alpha2, "CA", 2);
	KUNIT_EXPECT_MEMEQ(test, env->reg.alpha2, "CA", 2);
	KUNIT_EXPECT_MEMEQ(test, env->reg.domain, "XZ", 2);
	KUNIT_EXPECT_MEMEQ(test, env->choice.domain, "XZ", 2);
	/* 1, 6, 13 and 36 remain; 149 and both 6 GHz primaries are gone. */
	KUNIT_EXPECT_EQ(test, env->reg.domain[8], 3);
	KUNIT_EXPECT_EQ(test, env->reg.domain[9], 1);
	KUNIT_EXPECT_EQ(test, env->reg.domain[10], 0);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 0);
	KUNIT_ASSERT_EQ(test, env->reg.length, 12 + 4 * 8);
	/* The world profile permits 14 and 165, which the country does not
	 * list: the fallback never adds a channel.
	 */
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&env->policy, NL80211_BAND_2GHZ, 14));
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&env->policy, NL80211_BAND_5GHZ, 165));
	for (i = 0; i < 4; i++) {
		u16 ch = get_unaligned_le16(env->reg.domain + 12 + i * 8);

		KUNIT_EXPECT_TRUE(test, ch != 14 && ch != 149 && ch != 165);
	}
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&env->policy, NL80211_BAND_6GHZ, 5));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&env->policy, NL80211_BAND_6GHZ, 37));

	/* An intersected domain falls back with the reason recorded. */
	KUNIT_ASSERT_EQ(test, select_run(env, "98", false), 0);
	KUNIT_EXPECT_EQ(test, env->fs.requests, 1);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, -EOPNOTSUPP);
	KUNIT_EXPECT_MEMEQ(test, env->reg.domain, "XZ", 2);
	KUNIT_EXPECT_EQ(test, env->reg.domain[10], 0);

	/* 00 uses the same package as its own: no fallback is reported. */
	KUNIT_ASSERT_EQ(test, select_run(env, "00", true), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, 0);
	KUNIT_EXPECT_MEMEQ(test, env->reg.domain, "XZ", 2);
	fake_put(&env->fs, &env->file);
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

/* Country changes in a row, as the notifier drives them without a reload. */
static void mt7932_policy_switch_test(struct kunit *test)
{
	static const struct {
		const char *alpha2;
		enum mt7932_policy_source source;
		u8 domain[2];
		u8 six;
		int fallback;
	} steps[] = {
		{ "AU", MT7932_POLICY_V2, "AU", 2, 0 },
		{ "US", MT7932_POLICY_V1, "US", 0, 0 },
		{ "CA", MT7932_POLICY_WORLD, "XZ", 0, -ENOENT },
		{ "00", MT7932_POLICY_WORLD, "XZ", 0, 0 },
		{ "AU", MT7932_POLICY_V2, "AU", 2, 0 },
		{ "US", MT7932_POLICY_V1, "US", 0, 0 },
	};
	struct select_env *env = select_env(test);
	u8 digest[2][SHA256_DIGEST_SIZE];
	size_t au_size, us_size, ca_size, xz_size;
	u8 *au, *us, *ca, *xz;
	unsigned int i;

	au = pkg_build_v2(test, "AU", BIT_ULL(1) | BIT_ULL(9), &au_size);
	us = pkg_build(test, "US", &us_size);
	ca = pkg_build(test, "CA", &ca_size);
	xz = pkg_build(test, "XZ", &xz_size);
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", au, au_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/US.bin", us, us_size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);
	sha256(au, au_size, digest[0]);
	sha256(us, us_size, digest[1]);

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		KUNIT_ASSERT_EQ_MSG(test, select_run(env, steps[i].alpha2,
						     !memcmp(steps[i].alpha2, "00", 2)), 0,
				    "step %u", i);
		KUNIT_EXPECT_EQ_MSG(test, env->choice.source, steps[i].source, "step %u", i);
		KUNIT_EXPECT_MEMEQ_MSG(test, env->choice.domain, steps[i].domain, 2,
				       "step %u", i);
		KUNIT_EXPECT_EQ_MSG(test, env->choice.six_primaries, steps[i].six, "step %u", i);
		KUNIT_EXPECT_EQ_MSG(test, env->choice.fallback, steps[i].fallback, "step %u", i);
		KUNIT_EXPECT_EQ_MSG(test, env->fs.held, 1, "step %u", i);
		if (steps[i].source == MT7932_POLICY_V2)
			KUNIT_EXPECT_MEMEQ(test, env->choice.sha256, digest[0], SHA256_DIGEST_SIZE);
		if (steps[i].source == MT7932_POLICY_V1)
			KUNIT_EXPECT_MEMEQ(test, env->choice.sha256, digest[1], SHA256_DIGEST_SIZE);
	}
	/* A package installed later is used on the next change. */
	fake_add(&env->fs, "mediatek/mt7932/policy/CA.bin", ca, ca_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "CA", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_V1);
	KUNIT_EXPECT_EQ(test, env->choice.fallback, 0);
	KUNIT_EXPECT_MEMEQ(test, env->choice.domain, "CA", 2);
	fake_put(&env->fs, &env->file);
	KUNIT_EXPECT_EQ(test, env->fs.held, 0);
}

/* Unbound class archives must never be consumed as firmware command pages. */
static void mt7932_policy_unbound_test(struct kunit *test)
{
	struct select_env *env = select_env(test);
	struct mt7932_policy policy;
	size_t size, xz_size;
	u8 *data, *xz;

	data = pkg_build_v2(test, "AU", BIT_ULL(1), &size);
	xz = pkg_build(test, "XZ", &xz_size);
	put_unaligned_le16(3, data + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "AU"),
			-EINVAL);
	fake_add(&env->fs, "mediatek/mt7932/policy-v2/AU.bin", data, size);
	fake_add(&env->fs, "mediatek/mt7932/policy/AU.bin", data, size);
	fake_add(&env->fs, "mediatek/mt7932/policy/world-XZ.bin", xz, xz_size);
	KUNIT_ASSERT_EQ(test, select_run(env, "AU", false), 0);
	KUNIT_EXPECT_EQ(test, env->choice.tried[1].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.tried[2].error, -EINVAL);
	KUNIT_EXPECT_EQ(test, env->choice.source, MT7932_POLICY_WORLD);
	KUNIT_EXPECT_EQ(test, env->choice.six_primaries, 0);
}

static struct kunit_case mt7932_policy_test_cases[] = {
	KUNIT_CASE(mt7932_reg_relaxation_test),
	KUNIT_CASE(mt7932_policy_valid_test),
	KUNIT_CASE(mt7932_policy_country_mismatch_test),
	KUNIT_CASE(mt7932_policy_truncation_test),
	KUNIT_CASE(mt7932_policy_header_test),
	KUNIT_CASE(mt7932_policy_table_test),
	KUNIT_CASE(mt7932_policy_world_order_test),
	KUNIT_CASE(mt7932_policy_permits_test),
	KUNIT_CASE(mt7932_policy_filter_test),
	KUNIT_CASE(mt7932_policy_band_test),
	KUNIT_CASE(mt7932_policy_6g_test),
	KUNIT_CASE(mt7932_6g_power_limit_test),
	KUNIT_CASE(mt7932_policy_corruption_test),
	KUNIT_CASE(mt7932_policy_order_test),
	KUNIT_CASE(mt7932_policy_v4_classes_test),
	KUNIT_CASE(mt7932_policy_6g_pad_test),
	KUNIT_CASE(mt7932_policy_v4_preferred_test),
	KUNIT_CASE(mt7932_policy_v2_preferred_test),
	KUNIT_CASE(mt7932_policy_version_test),
	KUNIT_CASE(mt7932_policy_unbound_test),
	KUNIT_CASE(mt7932_policy_fallback_test),
	KUNIT_CASE(mt7932_policy_switch_test),
	{}
};

static struct kunit_suite mt7932_policy_test_suite = {
	.name = "mt7932-regulatory",
	.test_cases = mt7932_policy_test_cases,
};

kunit_test_suite(mt7932_policy_test_suite);
