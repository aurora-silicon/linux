// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "pmp-v2-protocol.h"
#include "pmp-v2-resident.h"
#include "pmp-v2-profile.h"

struct pmp_profile_fixture {
	u8 *text;
	bool fail;
};

static int pmp_profile_read(void *cookie, u64 address, void *out, size_t size)
{
	struct pmp_profile_fixture *fixture = cookie;

	if (fixture->fail)
		return -EIO;
	if (address < 0x300500000ULL || size > 0x3c000 ||
	    address - 0x300500000ULL > 0x3c000 - size)
		return -ERANGE;
	memcpy(out, fixture->text + address - 0x300500000ULL, size);
	return 0;
}

static void pmp_test_profile_identity(struct kunit *test)
{
	struct pmp_v2_segment segment = {
		.virtual = 0x1000000, .physical = 0x300500000ULL,
		.size = 0x3c000, .flags = 3,
	};
	struct pmp_profile_fixture fixture = {};
	struct pmp_v2_profile synthetic[2] = { { .build = "synthetic" }, { .build = "other" } };
	const struct pmp_v2_profile *profile = ERR_PTR(-EINVAL);
	u8 expected[SHA256_DIGEST_SIZE], observed[SHA256_DIGEST_SIZE];
	unsigned int i;

	fixture.text = kunit_kzalloc(test, segment.size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture.text);
	put_unaligned_le32(0x64697575, fixture.text + 0x204);
	put_unaligned_le32(5, fixture.text + 0x208);
	memcpy(fixture.text + 0x214, pmp_v2_25f84_uuid, 16);
	sha256(fixture.text, segment.size, expected);
	KUNIT_ASSERT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), 0);
	KUNIT_EXPECT_MEMEQ(test, expected, observed, sizeof(expected));
	/* Synthetic bytes must never accidentally admit a real profile. */
	KUNIT_EXPECT_PTR_EQ(test, profile, NULL);
	for (i = 0; i < ARRAY_SIZE(pmp_v2_profiles); i++)
		KUNIT_EXPECT_NE(test, memcmp(observed, pmp_v2_profiles[i].digest,
					     sizeof(observed)), 0);
	/* Every admitted build has a distinct UUID and digest. */
	for (i = 1; i < ARRAY_SIZE(pmp_v2_profiles); i++) {
		KUNIT_EXPECT_NE(test, memcmp(pmp_v2_profiles[0].uuid, pmp_v2_profiles[i].uuid, 16), 0);
		KUNIT_EXPECT_NE(test, memcmp(pmp_v2_profiles[0].digest, pmp_v2_profiles[i].digest,
					     SHA256_DIGEST_SIZE), 0);
	}
	/* The matcher requires both the UUID and the full digest. */
	memcpy(synthetic[1].uuid, pmp_v2_25f84_uuid, 16);
	memcpy(synthetic[1].digest, expected, sizeof(expected));
	KUNIT_EXPECT_PTR_EQ(test, pmp_v2_profile_match(synthetic, 2, pmp_v2_25f84_uuid, expected),
			    &synthetic[1]);
	KUNIT_EXPECT_PTR_EQ(test, pmp_v2_profile_match(synthetic, 1, pmp_v2_25f84_uuid, expected),
			    NULL);
	synthetic[1].digest[0] ^= 1;
	KUNIT_EXPECT_PTR_EQ(test, pmp_v2_profile_match(synthetic, 2, pmp_v2_25f84_uuid, expected),
			    NULL);
	synthetic[1].digest[0] ^= 1;
	synthetic[1].uuid[15] ^= 1;
	KUNIT_EXPECT_PTR_EQ(test, pmp_v2_profile_match(synthetic, 2, pmp_v2_25f84_uuid, expected),
			    NULL);
	/* An unknown UUID still yields the observed digest but no profile. */
	fixture.text[0x214] ^= 1;
	profile = ERR_PTR(-EINVAL);
	KUNIT_ASSERT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), 0);
	KUNIT_EXPECT_PTR_EQ(test, profile, NULL);
	KUNIT_EXPECT_NE(test, memcmp(observed, expected, sizeof(observed)), 0);
	fixture.text[0x214] ^= 1;
	fixture.text[0x300] ^= 1;
	KUNIT_ASSERT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, NULL), 0);
	KUNIT_EXPECT_NE(test, memcmp(observed, expected, sizeof(observed)), 0);
	fixture.text[0x300] ^= 1;
	/* A malformed info block is never an admitted build. */
	put_unaligned_le32(4, fixture.text + 0x208);
	KUNIT_EXPECT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), -ENODEV);
	put_unaligned_le32(5, fixture.text + 0x208);
	fixture.fail = true;
	KUNIT_EXPECT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), -EIO);
	fixture.fail = false;
	segment.size--;
	KUNIT_EXPECT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), -ENODEV);
	segment.size++;
	segment.virtual++;
	KUNIT_EXPECT_EQ(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), -ENODEV);
	segment.virtual--;
	segment.physical++;
	KUNIT_EXPECT_LT(test, pmp_v2_profile_digest(&segment, 1, pmp_profile_read,
		&fixture, observed, &profile), 0);
}

struct pmp_test {
	struct pmp_v2_protocol protocol;
	u64 next_address;
	unsigned int releases;
	bool out_of_memory;
};

static int pmp_test_allocate(struct pmp_v2_protocol *p, struct pmp_v2_buffer *b)
{
	struct pmp_test *ctx = p->private;

	if (ctx->out_of_memory)
		return -ENOMEM;
	b->allocation_size = ALIGN(b->size, SZ_16K);
	b->data = kzalloc(b->allocation_size, GFP_KERNEL);
	if (!b->data)
		return -ENOMEM;
	b->address = ctx->next_address;
	ctx->next_address += b->allocation_size;
	return 0;
}

static void pmp_test_release(struct pmp_v2_protocol *p, struct pmp_v2_buffer *b)
{
	struct pmp_test *ctx = p->private;

	ctx->releases++;
	kfree(b->data);
}

static const void *pmp_test_property(struct pmp_v2_protocol *p, const char *name, int *size)
{
	static const u8 value[] = { 0, 0, 5, 0 };

	if (strcmp(name, "threshold"))
		return NULL;
	*size = sizeof(value);
	return value;
}

static const struct pmp_v2_protocol_ops pmp_test_ops = {
	.allocate = pmp_test_allocate,
	.release = pmp_test_release,
	.property = pmp_test_property,
};

static int pmp_test_init(struct kunit *test)
{
	struct pmp_test *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->next_address = 0x10000000000ULL;
	pmp_v2_protocol_init(&ctx->protocol, &pmp_test_ops, ctx);
	test->priv = ctx;
	return 0;
}

static void pmp_test_exit(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *b, *next;
	struct pmp_v2_entry *entry, *tmp;

	/* Synthetic storage only: there is no coprocessor or outstanding DMA. */
	list_for_each_entry_safe(b, next, &p->buffers, link)
		pmp_v2_release(p, b);
	list_for_each_entry_safe(entry, tmp, &p->entries, link) {
		list_del(&entry->link);
		kfree(entry);
	}
}

static struct pmp_v2_buffer *pmp_test_buffer(struct kunit *test, size_t size)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_buffer *b = pmp_v2_allocate(&ctx->protocol, size, true);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, b);
	return b;
}

static void pmp_test_registry_init(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_buffer *outer = pmp_test_buffer(test, 16);
	struct pmp_v2_buffer *first = pmp_test_buffer(test, 256);
	struct pmp_v2_buffer *second = pmp_test_buffer(test, 256);
	u64 reply;

	put_unaligned_le64(first->address, outer->data);
	put_unaligned_le64(second->address, outer->data + 8);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(&ctx->protocol,
		(0x30ULL << 48) | outer->address, &reply), 0);
	KUNIT_ASSERT_EQ(test, reply, 0x31ULL << 48);
}

static struct pmp_v2_buffer *pmp_test_descriptor(struct kunit *test, u16 id,
					       const char *name, u64 length)
{
	struct pmp_v2_buffer *b = pmp_test_buffer(test, 0x51);

	strscpy(b->data, name, 48);
	strscpy(b->data + 0x30, "16p16", 8);
	put_unaligned_le64(length, b->data + 0x40);
	put_unaligned_le64(id, b->data + 0x48);
	return b;
}

static void pmp_test_high_address(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	u64 reply;

	KUNIT_ASSERT_EQ(test, pmp_v2_handle(&ctx->protocol, 0x10ULL << 48, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, 0x11ULL << 48);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(&ctx->protocol, (0x12ULL << 48) | 33, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x13ULL << 48) | 0x10000000000ULL);
	KUNIT_EXPECT_EQ(test, ctx->protocol.allocated, (size_t)SZ_16K);
}

static void pmp_test_id_zero_snapshot(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *d;
	struct pmp_v2_entry *entry;
	u64 reply;

	pmp_test_registry_init(test);
	d = pmp_test_descriptor(test, 0, "firmware-limit", 4);
	put_unaligned_le32(0x1234, p->first->data);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x33ULL << 48) | 4);
	entry = pmp_v2_entry_find(p, 0);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	KUNIT_EXPECT_FALSE(test, entry->default_from_adt);
	KUNIT_EXPECT_EQ(test, entry->updates, 0ULL);
	KUNIT_EXPECT_EQ(test, entry->last_update_ns, 0ULL);
	put_unaligned_le32(0x5678, p->first->data);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(entry->value), 0x1234U);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, 0x36ULL << 48, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x37ULL << 48) | 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(entry->value), 0x5678U);
	KUNIT_EXPECT_EQ(test, entry->updates, 1ULL);
	KUNIT_EXPECT_GE(test, entry->last_update_ns, entry->registered_ns);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, 0x34ULL << 48, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x35ULL << 48) | 4);
	KUNIT_EXPECT_EQ(test, p->entry_count, 0U);
	KUNIT_EXPECT_EQ(test, p->buffer_count, 4U);
}

static void pmp_test_optional_default(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *d;
	u64 reply;

	pmp_test_registry_init(test);
	d = pmp_test_descriptor(test, 7, "missing", 0);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, 0x33ULL << 48);
	KUNIT_EXPECT_EQ(test, p->entry_count, 0U);
	strscpy(d->data, "threshold", 48);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x33ULL << 48) | 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(p->first->data), 0x50000U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(p->second->data), 0U);
	KUNIT_ASSERT_NOT_NULL(test, pmp_v2_entry_find(p, 7));
	KUNIT_EXPECT_TRUE(test, pmp_v2_entry_find(p, 7)->default_from_adt);
	KUNIT_EXPECT_EQ(test, pmp_v2_entry_find(p, 7)->updates, 0ULL);
}

static void pmp_test_active_free_pins(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	u64 reply;

	pmp_test_registry_init(test);
	KUNIT_EXPECT_LT(test, pmp_v2_handle(p, (0x14ULL << 48) | p->first->address, &reply), 0);
	KUNIT_EXPECT_TRUE(test, p->failed);
	KUNIT_EXPECT_EQ(test, p->buffer_count, 3U);
	KUNIT_EXPECT_EQ(test, ctx->releases, 0U);
}

static void pmp_test_unknown_free(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *system = pmp_v2_allocate(p, 64, false);
	u64 reply;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, system);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x14ULL << 48) | system->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, 0x15ULL << 48);
	KUNIT_EXPECT_EQ(test, p->buffer_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->releases, 0U);
}

static void pmp_test_invalid_descriptor(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *d;
	u64 reply;

	pmp_test_registry_init(test);
	d = pmp_test_descriptor(test, 1, "overlong", 257);
	KUNIT_EXPECT_LT(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_TRUE(test, p->failed);
	KUNIT_EXPECT_EQ(test, p->entry_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->releases, 0U);
}

static void pmp_test_duplicate_entry(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *d;
	u64 reply;

	pmp_test_registry_init(test);
	d = pmp_test_descriptor(test, 2, "present", 4);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), -EEXIST);
	KUNIT_EXPECT_EQ(test, p->entry_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->releases, 0U);
}

static void pmp_test_no_ack_loop(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	u64 reply = U64_MAX;

	pmp_test_registry_init(test);
	KUNIT_EXPECT_LT(test, pmp_v2_handle(&ctx->protocol, 0x37ULL << 48, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, U64_MAX);
	KUNIT_EXPECT_TRUE(test, ctx->protocol.failed);
}

static void pmp_test_same_name_distinct_ids(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	struct pmp_v2_protocol *p = &ctx->protocol;
	struct pmp_v2_buffer *d;
	u64 reply;

	pmp_test_registry_init(test);
	d = pmp_test_descriptor(test, 70, "temp-sensor-version", 4);
	put_unaligned_le32(1, p->first->data);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	put_unaligned_le64(73, d->data + 0x48);
	put_unaligned_le32(2, p->first->data);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x32ULL << 48) | d->address, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, (0x33ULL << 48) | 4);
	KUNIT_EXPECT_EQ(test, p->entry_count, 2U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(pmp_v2_entry_find(p, 70)->value), 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(pmp_v2_entry_find(p, 73)->value), 2U);
	put_unaligned_le32(3, p->first->data);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x36ULL << 48) | 73, &reply), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(pmp_v2_entry_find(p, 70)->value), 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(pmp_v2_entry_find(p, 73)->value), 3U);
	KUNIT_ASSERT_EQ(test, pmp_v2_handle(p, (0x34ULL << 48) | 70, &reply), 0);
	KUNIT_EXPECT_NULL(test, pmp_v2_entry_find(p, 70));
	KUNIT_EXPECT_NOT_NULL(test, pmp_v2_entry_find(p, 73));
	KUNIT_EXPECT_FALSE(test, p->failed);
}

static void pmp_test_allocation_failure(struct kunit *test)
{
	struct pmp_test *ctx = test->priv;
	u64 reply;

	ctx->out_of_memory = true;
	KUNIT_EXPECT_EQ(test, pmp_v2_handle(&ctx->protocol, (0x12ULL << 48) | 64, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply, 0x13ULL << 48);
	KUNIT_EXPECT_FALSE(test, ctx->protocol.failed);
	KUNIT_EXPECT_EQ(test, ctx->protocol.buffer_count, 0U);
}

static void pmp_test_segment_bounds(struct kunit *test)
{
	u8 raw[64] = {};
	struct pmp_v2_segment segments[2];
	u64 physical;

	put_unaligned_le64(0x300500000ULL, raw);
	put_unaligned_le64(0x1000000, raw + 8);
	put_unaligned_le64(0x300500000ULL, raw + 16);
	put_unaligned_le32(0x1000, raw + 24);
	put_unaligned_le32(3, raw + 28);
	put_unaligned_le64(0x300501000ULL, raw + 32);
	put_unaligned_le64(0x1001000, raw + 40);
	put_unaligned_le64(0x300501000ULL, raw + 48);
	put_unaligned_le32(0x1000, raw + 56);
	put_unaligned_le32(6, raw + 60);
	KUNIT_ASSERT_EQ(test, pmp_v2_segments(raw, sizeof(raw), segments, 2), 2);
	KUNIT_ASSERT_EQ(test, pmp_v2_translate(segments, 2, 0x1001023, 20, true, &physical), 0);
	KUNIT_EXPECT_EQ(test, physical, 0x300501023ULL);
	KUNIT_EXPECT_EQ(test, pmp_v2_translate(segments, 2, 0x1000000, 4, true, &physical), -EPERM);
	KUNIT_EXPECT_EQ(test, pmp_v2_translate(segments, 2, U64_MAX, 4, false, &physical), -EINVAL);
	/* A second virtual mapping must not make TEXT writable through an alias. */
	put_unaligned_le64(0x300500000ULL, raw + 32);
	KUNIT_EXPECT_EQ(test, pmp_v2_segments(raw, sizeof(raw), segments, 2), -EINVAL);
}

static int pmp_test_resident_read(void *cookie, u64 address, void *data, size_t size)
{
	if (address < 0x300500000ULL || address + size > 0x300502000ULL)
		return -ERANGE;
	memcpy(data, cookie + address - 0x300500000ULL, size);
	return 0;
}

static void pmp_test_header_versions(struct kunit *test)
{
	struct pmp_v2_segment segments[] = {
		{ .physical = 0x300500000ULL, .virtual = 0x1000000, .size = 0x1000, .flags = 3 },
		{ .physical = 0x300501000ULL, .virtual = 0x1001000, .size = 0x1000, .flags = 6 },
	};
	u8 *data = kunit_kzalloc(test, 0x2000, GFP_KERNEL);
	u64 physical;
	size_t size;

	KUNIT_ASSERT_NOT_NULL(test, data);
	put_unaligned_le32(0x64697575, data + 0x204);
	put_unaligned_le32(5, data + 0x208);
	put_unaligned_le32(0x1013, data + 0x22c);
	put_unaligned_le32(31, data + 0x230);
	KUNIT_ASSERT_EQ(test, pmp_v2_locate(segments, 2, pmp_test_resident_read, data,
					 &physical, &size), 0);
	KUNIT_EXPECT_EQ(test, physical, 0x300501013ULL);
	KUNIT_EXPECT_EQ(test, size, (size_t)31);
	/* An earlier recognized v4 header wins, with the different field layout. */
	put_unaligned_le32(0x64697575, data + 0x20);
	put_unaligned_le32(4, data + 0x24);
	put_unaligned_le32(0x1040, data + 0x40);
	put_unaligned_le32(16, data + 0x44);
	KUNIT_ASSERT_EQ(test, pmp_v2_locate(segments, 2, pmp_test_resident_read, data,
					 &physical, &size), 0);
	KUNIT_EXPECT_EQ(test, physical, 0x300501040ULL);
	put_unaligned_le32(0xffe, data + 0x40);
	KUNIT_EXPECT_EQ(test, pmp_v2_locate(segments, 2, pmp_test_resident_read, data,
					 &physical, &size), -ERANGE);
}

static void pmp_test_packed_records(struct kunit *test)
{
	u8 data[23] = {};
	u8 before[23];
	struct pmp_v2_patch patches[] = {
		{ .key = 0x42444944, .value = 100 },
		{ .key = 0x46535444, .value = 1 },
	};

	put_unaligned_le32(0x1234, data);
	put_unaligned_le32(3, data + 4);
	memcpy(data + 8, "abc", 3);
	put_unaligned_le32(0x42444944, data + 11);
	put_unaligned_le32(4, data + 15);
	memcpy(before, data, sizeof(data));
	KUNIT_ASSERT_EQ(test, pmp_v2_patch_plan(data, sizeof(data), patches, 2), 0);
	KUNIT_EXPECT_TRUE(test, patches[0].present);
	KUNIT_EXPECT_EQ(test, patches[0].offset, (size_t)19);
	KUNIT_EXPECT_FALSE(test, patches[1].present);
	KUNIT_EXPECT_MEMEQ(test, data, before, sizeof(data));
	KUNIT_EXPECT_EQ(test, pmp_v2_patch_plan(data, sizeof(data) - 1, patches, 2), -EINVAL);
}

static void pmp_test_duplicate_patch(struct kunit *test)
{
	u8 data[24] = {};
	struct pmp_v2_patch patch = { .key = 0x42444944, .value = 100 };

	put_unaligned_le32(patch.key, data);
	put_unaligned_le32(4, data + 4);
	put_unaligned_le32(patch.key, data + 12);
	put_unaligned_le32(4, data + 16);
	KUNIT_EXPECT_EQ(test, pmp_v2_patch_plan(data, sizeof(data), &patch, 1), -EINVAL);
}

#include "pmp-v2-data.h"
#include "pmp-v2-sample-age.h"

static void pmp_test_sample_age(struct kunit *test)
{
	u64 age;

	KUNIT_EXPECT_EQ(test, pmp_sample_age(1000000000, 24000000, &age), 0);
	KUNIT_EXPECT_EQ(test, age, 0ULL);
	KUNIT_EXPECT_EQ(test, pmp_sample_age(1000400000, 24000000, &age), 0);
	KUNIT_EXPECT_EQ(test, age, 400000ULL);
	KUNIT_EXPECT_EQ(test, pmp_sample_age(1005000000, 24000000, &age), -ETIME);
	KUNIT_EXPECT_EQ(test, pmp_sample_age(999999999, 24000000, &age), -ETIME);
	KUNIT_EXPECT_EQ(test, pmp_sample_age(U64_MAX, U64_MAX, &age), -ERANGE);
	/* Advancing but delayed stream must fail, regardless of receipt cadence. */
	KUNIT_EXPECT_EQ(test, pmp_sample_age(1010000000, 24024000, &age), -ETIME);
}

static void pmp_test_debug_data_bounds(struct kunit *test)
{
	u64 physical = 0;

	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x105a2c0, 0x298, &physical), 0);
	KUNIT_EXPECT_EQ(test, physical, 0x30055a2c0ULL);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x103c000, 0x5c000, &physical), 0);
	KUNIT_EXPECT_EQ(test, physical, 0x30053c000ULL);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x1097ff8, 8, &physical), 0);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x1097ff8, 9, &physical), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x103bfff, 1, &physical), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0x103c000, 0, &physical), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(U64_MAX - 3, 8, &physical), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_data_range(0xffff04000000ULL, 16, &physical), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x105e3c0, 0x38000), 0);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x10963b8, 8), 0);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x10963b8, 9), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x105e3bc, 4), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x105e3c1, 4), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(0x103c000, 4), -ERANGE);
	KUNIT_EXPECT_EQ(test, pmp_v2_heap_range(U64_MAX - 3, 8), -ERANGE);
}

static struct kunit_case pmp_test_cases[] = {
	KUNIT_CASE(pmp_test_profile_identity),
	KUNIT_CASE(pmp_test_sample_age),
	KUNIT_CASE(pmp_test_debug_data_bounds),
	KUNIT_CASE(pmp_test_high_address),
	KUNIT_CASE(pmp_test_id_zero_snapshot),
	KUNIT_CASE(pmp_test_optional_default),
	KUNIT_CASE(pmp_test_active_free_pins),
	KUNIT_CASE(pmp_test_unknown_free),
	KUNIT_CASE(pmp_test_invalid_descriptor),
	KUNIT_CASE(pmp_test_duplicate_entry),
	KUNIT_CASE(pmp_test_same_name_distinct_ids),
	KUNIT_CASE(pmp_test_no_ack_loop),
	KUNIT_CASE(pmp_test_allocation_failure),
	KUNIT_CASE(pmp_test_segment_bounds),
	KUNIT_CASE(pmp_test_header_versions),
	KUNIT_CASE(pmp_test_packed_records),
	KUNIT_CASE(pmp_test_duplicate_patch),
	{}
};

static struct kunit_suite pmp_test_suite = {
	.name = "apple-pmp-v2-protocol",
	.init = pmp_test_init,
	.exit = pmp_test_exit,
	.test_cases = pmp_test_cases,
};

kunit_test_suite(pmp_test_suite);

MODULE_LICENSE("GPL");
