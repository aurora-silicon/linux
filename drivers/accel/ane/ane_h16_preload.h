/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_PRELOAD_H__
#define __ANE_H16_PRELOAD_H__

#include <crypto/sha2.h>

#define ANE_H16_PRELOAD_TEXT_SIZE 0xc0000
#define ANE_H16_PRELOAD_DATA_SIZE 0x2ac000
/* 0: H16 27.0 26A428, 1: H16 26.5.2 25F84 */
#define ANE_H16_PRELOAD_BUILDS 2

/* A build's DATA framing offset (relative to 26A428) and its identity digest. */
struct ane_h16_preload_layout {
	u32 shift;
	bool unknown;
	u8 digest[SHA256_DIGEST_SIZE];
};

extern const struct ane_h16_preload_layout ane_h16_preload_layouts[ANE_H16_PRELOAD_BUILDS];

/* Inputs are stable, host-owned snapshots of the reserved live pages.
 * Digest computation is not validation against the pinned firmware identity.
 * Neither function changes the snapshots or the live bootloader values.
 */
int ane_h16_preload_digest(const void *text, size_t text_size,
			   const void *data, size_t data_size,
			   u8 digest[SHA256_DIGEST_SIZE]);
int ane_h16_preload_validate(const void *text, size_t text_size,
			     const void *data, size_t data_size);
int ane_h16_preload_digest_layout(const struct ane_h16_preload_layout *layout,
				  const void *text, size_t text_size,
				  const void *data, size_t data_size,
				  u8 digest[SHA256_DIGEST_SIZE]);
int ane_h16_preload_identify(const void *text, size_t text_size,
			     const void *data, size_t data_size);

#endif /* __ANE_H16_PRELOAD_H__ */
