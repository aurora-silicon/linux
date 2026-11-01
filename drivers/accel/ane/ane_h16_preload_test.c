// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "ane_h16_preload.h"

/* Synthetic framing only, with no firmware bytes. Walk records independently
 * from their widths rather than sharing the production offset manifest.
 */
static const struct {
	char tag[4];
	u8 width;
	bool mutable;
} records[] = {
	{ "BECA", 8, true }, { "SECA", 8, false }, { "GKTS", 8, true },
	{ "otcd", 8, false }, { "oscd", 8, false }, { "oecd", 8, false },
	{ "DILS", 8, true }, { "SRSA", 8, false }, { "GFCM", 8, false },
	{ "SSSC", 8, true }, { "qF8v", 4, true }, { "LRSD", 1, false },
	{ "SVSD", 8, false }, { "LCSD", 8, false }, { "ZSTR", 4, false },
	{ "BPTP", 8, false }, { "TNGI", 1, false }, { "oeNS", 1, false },
	{ "SEBC", 8, false }, { "ECAP", 8, true }, { "BVTP", 8, true },
	{ "sP1T", 8, true }, { "BtpG", 8, true }, { "StpG", 8, false },
	{ "BlpP", 8, true }, { "SPTP", 8, false }, { "SZSD", 8, false },
	{ "LZSD", 8, false }, { "S2xG", 8, true }, { "SNUT", 8, true },
	{ "ZNUT", 4, false }, { "OTTR", 8, true }, { "LLTR", 1, false },
	{ "_COS", 4, true }, { "RCOS", 4, true }, { "dApC", 8, true },
	{ "dArW", 8, true }, { "fVED", 1, true }, { "GLFp", 8, true },
	{ "ABOI", 8, false }, { "ZSOI", 4, false }, { "ARcM", 4, true },
};

struct preload_fixture {
	u8 *text;
	u8 *data;
};

static void preload_free(void *allocation)
{
	kvfree(allocation);
}

static int preload_fixture_init(struct kunit *test)
{
	struct preload_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	size_t pos = 0x5a18;
	unsigned int i;

	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->text = kvmalloc(ANE_H16_PRELOAD_TEXT_SIZE, GFP_KERNEL);
	if (!f->text)
		return -ENOMEM;
	if (kunit_add_action_or_reset(test, preload_free, f->text))
		return -ENOMEM;
	f->data = kvmalloc(ANE_H16_PRELOAD_DATA_SIZE, GFP_KERNEL);
	if (!f->data)
		return -ENOMEM;
	if (kunit_add_action_or_reset(test, preload_free, f->data))
		return -ENOMEM;
	memset(f->text, 0x5a, ANE_H16_PRELOAD_TEXT_SIZE);
	memset(f->data, 0xa5, ANE_H16_PRELOAD_DATA_SIZE);
	for (i = 0; i < ARRAY_SIZE(records); i++) {
		memcpy(f->data + pos, records[i].tag, 4);
		put_unaligned_le32(records[i].width, f->data + pos + 4);
		pos += 8 + records[i].width;
	}
	return pos == 0x5c79 ? 0 : -EINVAL;
}

static int preload_digest(struct preload_fixture *f, u8 *digest)
{
	return ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
				      f->data, ANE_H16_PRELOAD_DATA_SIZE, digest);
}

static void preload_identity_test(struct kunit *test)
{
	/* Independently normalized synthetic bytes, SHA-256 from Python hashlib. */
	static const u8 expected[SHA256_DIGEST_SIZE] = {
		0x67, 0xb5, 0x33, 0xc6, 0x0b, 0x11, 0x03, 0x2b,
		0x3e, 0x75, 0x27, 0x7b, 0x2d, 0xe1, 0x82, 0xc6,
		0xca, 0x53, 0xa7, 0x18, 0x1f, 0xfc, 0xcc, 0xfe,
		0xd3, 0xb5, 0xe8, 0x22, 0xaa, 0x12, 0xd0, 0xbb,
	};
	struct preload_fixture *f = test->priv;
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	KUNIT_EXPECT_EQ(test, preload_digest(f, digest), 0);
	KUNIT_EXPECT_MEMEQ(test, digest, expected, sizeof(digest));
	/* Correct framing alone cannot authenticate a synthetic image. */
	ret = ane_h16_preload_validate(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
				       f->data, ANE_H16_PRELOAD_DATA_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
}

static void preload_mutable_test(struct kunit *test)
{
	struct preload_fixture *f = test->priv;
	u8 baseline[SHA256_DIGEST_SIZE], digest[SHA256_DIGEST_SIZE];
	size_t pos = 0x5a18;
	unsigned int i, mutable = 0;

	KUNIT_ASSERT_EQ(test, preload_digest(f, baseline), 0);
	for (i = 0; i < ARRAY_SIZE(records); i++) {
		if (records[i].mutable) {
			memset(f->data + pos + 8, 0x3c, records[i].width);
			KUNIT_ASSERT_EQ(test, preload_digest(f, digest), 0);
			KUNIT_EXPECT_MEMEQ(test, digest, baseline, sizeof(digest));
			mutable++;
		}
		pos += 8 + records[i].width;
	}
	KUNIT_EXPECT_EQ(test, mutable, 20);
	memset(f->data + 0x17c80, 0x7e, 0x6a0);
	KUNIT_ASSERT_EQ(test, preload_digest(f, digest), 0);
	KUNIT_EXPECT_MEMEQ(test, digest, baseline, sizeof(digest));
	/* Validation and normalization never erase the accepted boot data. */
	KUNIT_EXPECT_EQ(test, f->data[0x5a20], 0x3c);
	KUNIT_EXPECT_EQ(test, f->data[0x17c80], 0x7e);
	KUNIT_EXPECT_EQ(test, f->data[0x1831f], 0x7e);
}

static void preload_framing_test(struct kunit *test)
{
	struct preload_fixture *f = test->priv;
	u8 digest[SHA256_DIGEST_SIZE];
	size_t pos = 0x5a18;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(records); i++) {
		f->data[pos] ^= 1;
		KUNIT_EXPECT_EQ(test, preload_digest(f, digest), -ENOEXEC);
		f->data[pos] ^= 1;
		put_unaligned_le32(U32_MAX, f->data + pos + 4);
		KUNIT_EXPECT_EQ(test, preload_digest(f, digest), -ENOEXEC);
		put_unaligned_le32(records[i].width, f->data + pos + 4);
		pos += 8 + records[i].width;
	}
	KUNIT_EXPECT_EQ(test, preload_digest(f, digest), 0);
}

static void preload_immutable_test(struct kunit *test)
{
	static const u32 boundaries[] = {
		0, 0x5a17, 0x5c79, 0x17c7f, 0x18320, 0xb8000, 0x2abfff,
	};
	struct preload_fixture *f = test->priv;
	u8 baseline[SHA256_DIGEST_SIZE], digest[SHA256_DIGEST_SIZE];
	size_t pos = 0x5a18;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, preload_digest(f, baseline), 0);
	for (i = 0; i < ARRAY_SIZE(records); i++) {
		if (!records[i].mutable) {
			f->data[pos + 8] ^= 1;
			KUNIT_ASSERT_EQ(test, preload_digest(f, digest), 0);
			KUNIT_EXPECT_NE(test, memcmp(digest, baseline, sizeof(digest)), 0);
			f->data[pos + 8] ^= 1;
		}
		pos += 8 + records[i].width;
	}
	for (i = 0; i < ARRAY_SIZE(boundaries); i++) {
		f->data[boundaries[i]] ^= 1;
		KUNIT_ASSERT_EQ(test, preload_digest(f, digest), 0);
		KUNIT_EXPECT_NE(test, memcmp(digest, baseline, sizeof(digest)), 0);
		f->data[boundaries[i]] ^= 1;
	}
	f->text[ANE_H16_PRELOAD_TEXT_SIZE - 1] ^= 1;
	KUNIT_ASSERT_EQ(test, preload_digest(f, digest), 0);
	KUNIT_EXPECT_NE(test, memcmp(digest, baseline, sizeof(digest)), 0);
}

static void preload_bounds_test(struct kunit *test)
{
	struct preload_fixture *f = test->priv;
	u8 digest[SHA256_DIGEST_SIZE];
	int delta, ret;

	for (delta = -1; delta <= 1; delta += 2) {
		ret = ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE + delta,
					     f->data, ANE_H16_PRELOAD_DATA_SIZE, digest);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		ret = ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
					     f->data, ANE_H16_PRELOAD_DATA_SIZE + delta, digest);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}
	ret = ane_h16_preload_digest(NULL, ANE_H16_PRELOAD_TEXT_SIZE,
				     f->data, ANE_H16_PRELOAD_DATA_SIZE, digest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
				     NULL, ANE_H16_PRELOAD_DATA_SIZE, digest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
				     f->data, ANE_H16_PRELOAD_DATA_SIZE, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

/* 25F84 frames the same records 0x3f80 further into DATA. */
static void preload_shifted_test(struct kunit *test)
{
	struct preload_fixture *f = test->priv;
	u8 digest[SHA256_DIGEST_SIZE];
	size_t pos = 0x5a18;

	KUNIT_EXPECT_EQ(test, ane_h16_preload_digest_layout(&ane_h16_preload_layouts[1],
		f->text, ANE_H16_PRELOAD_TEXT_SIZE, f->data, ANE_H16_PRELOAD_DATA_SIZE,
		digest), -ENOEXEC);
	memmove(f->data + pos + 0x3f80, f->data + pos, 0x5c79 - pos);
	memset(f->data + pos, 0xa5, 0x3f80);
	KUNIT_EXPECT_EQ(test, ane_h16_preload_digest_layout(&ane_h16_preload_layouts[1],
		f->text, ANE_H16_PRELOAD_TEXT_SIZE, f->data, ANE_H16_PRELOAD_DATA_SIZE,
		digest), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_preload_digest(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
		f->data, ANE_H16_PRELOAD_DATA_SIZE, digest), -ENOEXEC);
	/* Framing alone identifies nothing. */
	KUNIT_EXPECT_EQ(test, ane_h16_preload_identify(f->text, ANE_H16_PRELOAD_TEXT_SIZE,
		f->data, ANE_H16_PRELOAD_DATA_SIZE), -ENOEXEC);
}

static struct kunit_case preload_cases[] = {
	KUNIT_CASE(preload_shifted_test),
	KUNIT_CASE(preload_identity_test),
	KUNIT_CASE(preload_mutable_test),
	KUNIT_CASE(preload_framing_test),
	KUNIT_CASE(preload_immutable_test),
	KUNIT_CASE(preload_bounds_test),
	{}
};

static struct kunit_suite preload_suite = {
	.name = "ane-h16-preload",
	.init = preload_fixture_init,
	.test_cases = preload_cases,
};

kunit_test_suite(preload_suite);
MODULE_LICENSE("Dual MIT/GPL");
