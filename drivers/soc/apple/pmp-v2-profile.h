/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_PMP_V2_PROFILE_H
#define APPLE_PMP_V2_PROFILE_H

#include <crypto/sha2.h>

/*
 * Resident firmware identities that share the private DATA profile used by
 * pmp-v2-data.h and pmp-v2-sample.h.
 *
 * Each entry is the RTKit build UUID stored in the firmware info block at
 * TEXT+0x204 ("uuid", version 5, UUID at +0x10) and the SHA-256 of the whole
 * resident __TEXT segment (vmaddr 0x1000000, 0x3c000 bytes). DATA is mutable
 * and is checked independently by the live reader; this is not a whole-image
 * hash.
 *
 * 25F84 (macOS 26.5.2, RTKit-3255.120.11.release) is the build the profile was
 * originally qualified on. 25G83 (macOS 26.6.2, RTKit-3255.160.4.release,
 * Firmware/pmp/t8140pmp.im4p of UniversalMac_26.6.2_25G83_Restore.ipsw) was
 * admitted after comparing the two Mach-O payloads: every load command,
 * section address and section size is identical except that the RTKit
 * version string in __TEXT.__const is one byte shorter, which shifts
 * __cstring by one byte. All 408 differing __text words are 404 ADD-immediate
 * string references adjusted by exactly one and the four words of the build
 * UUID; the 273 differing __DATA bytes are all string pointers adjusted by
 * one. __text, _rtk_mtab, __cstring, every _rtk_* DATA section and the
 * __zerofill layout are byte-identical, so the fixed DATA addresses of the
 * private sensor ABI are unchanged. The 25F84 digest computed from its IPSW
 * payload reproduces the originally qualified constant exactly, which is the
 * evidence that iBoot loads __TEXT verbatim.
 */
struct pmp_v2_profile {
	const char *build;
	u8 uuid[16];
	u8 digest[SHA256_DIGEST_SIZE];
};

static const struct pmp_v2_profile pmp_v2_profiles[] = {
	{
		.build = "25F84",
		.uuid = {
			0xf3, 0xb1, 0xbc, 0xa0, 0x7a, 0x39, 0x31, 0x3e,
			0x9f, 0x0b, 0xad, 0xcb, 0xf1, 0xcf, 0xe3, 0x66,
		},
		.digest = {
			0x83, 0xc6, 0x62, 0xeb, 0x09, 0x4a, 0xa1, 0x0a,
			0x42, 0x60, 0x6e, 0xc7, 0xba, 0x7e, 0x16, 0x03,
			0x59, 0xe7, 0x08, 0x26, 0x70, 0x0e, 0xbd, 0x88,
			0xf8, 0x5b, 0xac, 0x77, 0x4d, 0xd4, 0x87, 0x99,
		},
	},
	{
		.build = "25G83",
		.uuid = {
			0x84, 0xa0, 0x26, 0xde, 0x88, 0x96, 0x3c, 0xf4,
			0xa5, 0xb5, 0x36, 0x78, 0x79, 0xc8, 0xfb, 0xee,
		},
		.digest = {
			0x8f, 0x94, 0x4b, 0x29, 0xb8, 0xd6, 0xf6, 0xce,
			0xbe, 0x64, 0x5f, 0x9d, 0xc7, 0x51, 0x08, 0x4b,
			0x36, 0x14, 0x96, 0x3f, 0xf0, 0x11, 0xc8, 0x87,
			0xd4, 0x70, 0x31, 0x87, 0x3d, 0x7c, 0xd8, 0x4a,
		},
	},
};

/* Kept for the profile KUnit case; the first entry is the original 25F84. */
#define pmp_v2_25f84_uuid (pmp_v2_profiles[0].uuid)
#define pmp_v2_25f84_digest (pmp_v2_profiles[0].digest)

static const struct pmp_v2_profile *pmp_v2_profile_match(const struct pmp_v2_profile *table,
		unsigned int count, const u8 uuid[16], const u8 digest[SHA256_DIGEST_SIZE])
{
	unsigned int i;

	/* Both the build UUID and the full TEXT digest must match one entry. */
	for (i = 0; i < count; i++)
		if (!memcmp(uuid, table[i].uuid, 16) &&
		    !memcmp(digest, table[i].digest, SHA256_DIGEST_SIZE))
			return &table[i];
	return NULL;
}

/* Caller has admitted non-overlapping, overflow-checked resident mappings.
 * Read must return an error unless every requested byte was obtained.
 * Hash only while stopped, before publishing a new owned epoch.
 * On success *profile is the admitted identity or NULL for an unknown build
 * whose info block still parses; digest is always the observed TEXT hash.
 */
static int pmp_v2_profile_digest(const struct pmp_v2_segment *segments,
		unsigned int count, int (*read)(void *, u64, void *, size_t),
		void *cookie, u8 digest[SHA256_DIGEST_SIZE],
		const struct pmp_v2_profile **profile)
{
	struct sha256_ctx hash;
	u8 chunk[256], uuid[16];
	u64 physical;
	unsigned int i, offset;
	int ret;

	if (profile)
		*profile = NULL;
	for (i = 0; i < count; i++)
		if (segments[i].virtual == 0x1000000 &&
		    segments[i].size == 0x3c000 && (segments[i].flags & 1))
			break;
	if (i == count)
		return -ENODEV;
	ret = pmp_v2_translate(segments, count, 0x1000204, 64, false, &physical);
	if (ret)
		return ret;
	ret = read(cookie, physical, chunk, 64);
	if (ret)
		return ret;
	if (get_unaligned_le32(chunk) != 0x64697575 ||
	    get_unaligned_le32(chunk + 4) != 5)
		return -ENODEV;
	memcpy(uuid, chunk + 0x10, sizeof(uuid));
	sha256_init(&hash);
	for (offset = 0; offset < 0x3c000; offset += sizeof(chunk)) {
		ret = pmp_v2_translate(segments, count, 0x1000000 + offset,
				       sizeof(chunk), false, &physical);
		if (ret)
			return ret;
		ret = read(cookie, physical, chunk, sizeof(chunk));
		if (ret)
			return ret;
		sha256_update(&hash, chunk, sizeof(chunk));
	}
	sha256_final(&hash, digest);
	if (profile)
		*profile = pmp_v2_profile_match(pmp_v2_profiles, ARRAY_SIZE(pmp_v2_profiles),
						uuid, digest);
	return 0;
}

#endif
