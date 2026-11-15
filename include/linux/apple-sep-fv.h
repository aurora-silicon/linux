/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_APPLE_SEP_FV_H
#define _LINUX_APPLE_SEP_FV_H

#include <linux/types.h>

#define APPLE_SEP_FV_OPAQUE_KEY_SIZE 64
#define APPLE_SEP_FV_IV_KEY_SIZE 16
#define APPLE_SEP_FV_MAX_WRAPPED_KEY_SIZE 168

struct apple_sep_fv_key {
	u8 opaque[APPLE_SEP_FV_OPAQUE_KEY_SIZE];
	u8 iv[APPLE_SEP_FV_IV_KEY_SIZE];
};

struct apple_sep_fv_new_file_key {
	struct apple_sep_fv_key key;
	u8 wrapped_ekwk[APPLE_SEP_FV_MAX_WRAPPED_KEY_SIZE];
	u8 wrapped_ek[APPLE_SEP_FV_MAX_WRAPPED_KEY_SIZE];
	size_t wrapped_ekwk_len;
	size_t wrapped_ek_len;
};

int apple_sep_fv_unwrap_media_key(const u8 *wrapped, size_t wrapped_len,
				  u32 protection_class,
				  struct apple_sep_fv_key *key);
int apple_sep_fv_unwrap_volume_key(const u8 *secret, size_t secret_len,
				   const u8 *unlock_record,
				   size_t unlock_record_len,
				   const u8 *volume_key,
				   size_t volume_key_len,
				   struct apple_sep_fv_key *key);
int apple_sep_fv_load_class_keys_v2(const u8 volume_uuid[16],
				    const u8 *secret, size_t secret_len,
				    const u8 *unlock_record,
				    size_t unlock_record_len,
				    const u8 *volume_key,
				    size_t volume_key_len);
int apple_sep_fv_unload_class_keys_v2(const u8 volume_uuid[16],
				      const u8 *volume_key,
				      size_t volume_key_len);
int apple_sep_fv_unwrap_file_key(const u8 volume_uuid[16],
				 u32 protection_class,
				 const u8 *wrapped_ekwk,
				 size_t wrapped_ekwk_len,
				 const u8 *wrapped_ek,
				 size_t wrapped_ek_len,
				 struct apple_sep_fv_key *key);
int apple_sep_fv_new_file_key_v2(const u8 volume_uuid[16],
				 u32 protection_class,
				 u64 crypto_id, u16 key_revision,
				 struct apple_sep_fv_new_file_key *key);

#endif
