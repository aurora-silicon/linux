// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ane_h16_preload.h"

/* Ordered aether patchbay framing. Offsets are relative to DATA, whose
 * original VM base is 0xc0000. Only explicitly mutable value bytes normalize.
 */
static const struct {
	char tag[4];
	u32 value;
	u8 width;
	bool mutable;
} ane_h16_patchbay[] = {
	{ "BECA", 0x5a20, 8, true },
	{ "SECA", 0x5a30, 8, false },
	{ "GKTS", 0x5a40, 8, true },
	{ "otcd", 0x5a50, 8, false },
	{ "oscd", 0x5a60, 8, false },
	{ "oecd", 0x5a70, 8, false },
	{ "DILS", 0x5a80, 8, true },
	{ "SRSA", 0x5a90, 8, false },
	{ "GFCM", 0x5aa0, 8, false },
	{ "SSSC", 0x5ab0, 8, true },
	{ "qF8v", 0x5ac0, 4, true },
	{ "LRSD", 0x5acc, 1, false },
	{ "SVSD", 0x5ad5, 8, false },
	{ "LCSD", 0x5ae5, 8, false },
	{ "ZSTR", 0x5af5, 4, false },
	{ "BPTP", 0x5b01, 8, false },
	{ "TNGI", 0x5b11, 1, false },
	{ "oeNS", 0x5b1a, 1, false },
	{ "SEBC", 0x5b23, 8, false },
	{ "ECAP", 0x5b33, 8, true },
	{ "BVTP", 0x5b43, 8, true },
	{ "sP1T", 0x5b53, 8, true },
	{ "BtpG", 0x5b63, 8, true },
	{ "StpG", 0x5b73, 8, false },
	{ "BlpP", 0x5b83, 8, true },
	{ "SPTP", 0x5b93, 8, false },
	{ "SZSD", 0x5ba3, 8, false },
	{ "LZSD", 0x5bb3, 8, false },
	{ "S2xG", 0x5bc3, 8, true },
	{ "SNUT", 0x5bd3, 8, true },
	{ "ZNUT", 0x5be3, 4, false },
	{ "OTTR", 0x5bef, 8, true },
	{ "LLTR", 0x5bff, 1, false },
	{ "_COS", 0x5c08, 4, true },
	{ "RCOS", 0x5c14, 4, true },
	{ "dApC", 0x5c20, 8, true },
	{ "dArW", 0x5c30, 8, true },
	{ "fVED", 0x5c40, 1, true },
	{ "GLFp", 0x5c49, 8, true },
	{ "ABOI", 0x5c59, 8, false },
	{ "ZSOI", 0x5c69, 4, false },
	{ "ARcM", 0x5c75, 4, true },
};

int ane_h16_preload_digest(const void *text, size_t text_size,
			   const void *data, size_t data_size,
			   u8 digest[SHA256_DIGEST_SIZE])
{
	static const u8 zero[64];
	const u8 *bytes = data;
	struct sha256_ctx ctx;
	size_t cursor = 0, remaining;
	unsigned int i;

	if (!text || !data || !digest || text_size != ANE_H16_PRELOAD_TEXT_SIZE ||
	    data_size != ANE_H16_PRELOAD_DATA_SIZE)
		return -EINVAL;
	/* Validate all framing before producing a digest. Odd offsets are valid. */
	for (i = 0; i < ARRAY_SIZE(ane_h16_patchbay); i++) {
		u32 value = ane_h16_patchbay[i].value;

		if (memcmp(bytes + value - 8, ane_h16_patchbay[i].tag, 4) ||
		    get_unaligned_le32(bytes + value - 4) != ane_h16_patchbay[i].width)
			return -ENOEXEC;
	}
	sha256_init(&ctx);
	sha256_update(&ctx, text, text_size);
	for (i = 0; i < ARRAY_SIZE(ane_h16_patchbay); i++) {
		u32 value = ane_h16_patchbay[i].value;

		if (!ane_h16_patchbay[i].mutable)
			continue;
		sha256_update(&ctx, bytes + cursor, value - cursor);
		sha256_update(&ctx, zero, ane_h16_patchbay[i].width);
		cursor = value + ane_h16_patchbay[i].width;
	}
	sha256_update(&ctx, bytes + cursor, 0x17c80 - cursor);
	for (remaining = 0x6a0; remaining; ) {
		size_t length = min(remaining, sizeof(zero));

		sha256_update(&ctx, zero, length);
		remaining -= length;
	}
	cursor = 0x18320;
	sha256_update(&ctx, bytes + cursor, data_size - cursor);
	sha256_final(&ctx, digest);
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_preload_digest);

int ane_h16_preload_validate(const void *text, size_t text_size,
			     const void *data, size_t data_size)
{
	static const u8 expected[SHA256_DIGEST_SIZE] = {
		0x1f, 0x86, 0x5e, 0xd0, 0x4c, 0x4c, 0x38, 0xf6,
		0xfd, 0xcc, 0x45, 0x43, 0xa8, 0x81, 0x4f, 0x81,
		0x8d, 0x3c, 0xea, 0x58, 0x3d, 0xe6, 0x4d, 0x22,
		0xff, 0xe9, 0x6e, 0x3a, 0x93, 0xca, 0x46, 0x97,
	};
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	ret = ane_h16_preload_digest(text, text_size, data, data_size, digest);
	if (ret)
		return ret;
	return memcmp(digest, expected, sizeof(digest)) ? -ENOEXEC : 0;
}
EXPORT_SYMBOL_GPL(ane_h16_preload_validate);

MODULE_DESCRIPTION("H16 resident firmware identity validation");
MODULE_LICENSE("Dual MIT/GPL");
