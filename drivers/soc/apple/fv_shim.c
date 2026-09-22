// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/apple-sep-fv.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/string.h>

#include "shim.h"

static DECLARE_RWSEM(apple_sep_fv_registration_lock);
static DEFINE_MUTEX(apple_sep_fv_call_lock);
static struct sep_fv_ops apple_sep_fv_ops;
static void *apple_sep_fv_context;

int sep_fv_register_v2(void *context, const struct sep_fv_ops *ops)
{
	int ret = 0;

	if (!context || !ops || !ops->unwrap_media_key ||
	    !ops->unwrap_volume_key || !ops->load_class_keys ||
	    !ops->unload_class_keys || !ops->unwrap_file_key ||
	    !ops->new_file_key)
		return -EINVAL;

	down_write(&apple_sep_fv_registration_lock);
	if (apple_sep_fv_context)
		ret = -EBUSY;
	else {
		apple_sep_fv_ops = *ops;
		apple_sep_fv_context = context;
	}
	up_write(&apple_sep_fv_registration_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sep_fv_register_v2);

void sep_fv_unregister_v2(void *context)
{
	down_write(&apple_sep_fv_registration_lock);
	if (apple_sep_fv_context == context) {
		apple_sep_fv_context = NULL;
		memzero_explicit(&apple_sep_fv_ops, sizeof(apple_sep_fv_ops));
	}
	up_write(&apple_sep_fv_registration_lock);
}
EXPORT_SYMBOL_GPL(sep_fv_unregister_v2);

static int apple_sep_fv_call(int (*call)(const struct sep_fv_ops *, void *),
			     void *argument)
{
	int ret;

	down_read(&apple_sep_fv_registration_lock);
	if (!apple_sep_fv_context) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&apple_sep_fv_call_lock);
	ret = call(&apple_sep_fv_ops, argument);
	mutex_unlock(&apple_sep_fv_call_lock);
out:
	up_read(&apple_sep_fv_registration_lock);
	return ret;
}

struct media_key_call {
	const u8 *wrapped;
	size_t wrapped_len;
	u32 protection_class;
	struct apple_sep_fv_key *key;
};

static int call_unwrap_media_key(const struct sep_fv_ops *ops, void *argument)
{
	struct media_key_call *call = argument;

	return ops->unwrap_media_key(apple_sep_fv_context, call->wrapped,
				     call->wrapped_len, call->protection_class,
				     call->key);
}

int apple_sep_fv_unwrap_media_key(const u8 *wrapped, size_t wrapped_len,
				  u32 protection_class,
				  struct apple_sep_fv_key *key)
{
	struct media_key_call call = {
		.wrapped = wrapped,
		.wrapped_len = wrapped_len,
		.protection_class = protection_class,
		.key = key,
	};
	int ret;

	if (!wrapped || !wrapped_len || !key)
		return -EINVAL;
	memzero_explicit(key, sizeof(*key));
	ret = apple_sep_fv_call(call_unwrap_media_key, &call);
	if (ret)
		memzero_explicit(key, sizeof(*key));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_fv_unwrap_media_key);

struct volume_key_call {
	const u8 *volume_uuid;
	const u8 *secret;
	size_t secret_len;
	const u8 *unlock_record;
	size_t unlock_record_len;
	const u8 *volume_key;
	size_t volume_key_len;
	struct apple_sep_fv_key *key;
};

static int call_unwrap_volume_key(const struct sep_fv_ops *ops, void *argument)
{
	struct volume_key_call *call = argument;

	return ops->unwrap_volume_key(apple_sep_fv_context, call->secret,
			call->secret_len, call->unlock_record,
			call->unlock_record_len, call->volume_key,
			call->volume_key_len, call->key);
}

int apple_sep_fv_unwrap_volume_key(const u8 *secret, size_t secret_len,
				   const u8 *unlock_record,
				   size_t unlock_record_len,
				   const u8 *volume_key,
				   size_t volume_key_len,
				   struct apple_sep_fv_key *key)
{
	struct volume_key_call call = {
		.secret = secret,
		.secret_len = secret_len,
		.unlock_record = unlock_record,
		.unlock_record_len = unlock_record_len,
		.volume_key = volume_key,
		.volume_key_len = volume_key_len,
		.key = key,
	};
	int ret;

	if ((!secret && secret_len) || (secret && !secret_len) ||
	    (!unlock_record && unlock_record_len) ||
	    (unlock_record && !unlock_record_len) ||
	    !volume_key || !volume_key_len || !key)
		return -EINVAL;
	memzero_explicit(key, sizeof(*key));
	ret = apple_sep_fv_call(call_unwrap_volume_key, &call);
	if (ret)
		memzero_explicit(key, sizeof(*key));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_fv_unwrap_volume_key);

static int call_load_class_keys(const struct sep_fv_ops *ops, void *argument)
{
	struct volume_key_call *call = argument;

	return ops->load_class_keys(apple_sep_fv_context, call->volume_uuid,
			call->secret,
			call->secret_len, call->unlock_record,
			call->unlock_record_len, call->volume_key,
			call->volume_key_len);
}

int apple_sep_fv_load_class_keys_v2(const u8 volume_uuid[16],
				    const u8 *secret, size_t secret_len,
				    const u8 *unlock_record,
				    size_t unlock_record_len,
				    const u8 *volume_key,
				    size_t volume_key_len)
{
	struct volume_key_call call = {
		.volume_uuid = volume_uuid,
		.secret = secret,
		.secret_len = secret_len,
		.unlock_record = unlock_record,
		.unlock_record_len = unlock_record_len,
		.volume_key = volume_key,
		.volume_key_len = volume_key_len,
	};

	if (!volume_uuid || (!secret && secret_len) || (secret && !secret_len) ||
	    (!unlock_record && unlock_record_len) ||
	    (unlock_record && !unlock_record_len) ||
	    !volume_key || !volume_key_len)
		return -EINVAL;
	return apple_sep_fv_call(call_load_class_keys, &call);
}
EXPORT_SYMBOL_GPL(apple_sep_fv_load_class_keys_v2);

struct unload_class_keys_call {
	const u8 *volume_uuid;
	const u8 *volume_key;
	size_t volume_key_len;
};

static int call_unload_class_keys(const struct sep_fv_ops *ops,
				  void *argument)
{
	struct unload_class_keys_call *call = argument;

	return ops->unload_class_keys(apple_sep_fv_context, call->volume_uuid,
				      call->volume_key,
				      call->volume_key_len);
}

int apple_sep_fv_unload_class_keys_v2(const u8 volume_uuid[16],
				      const u8 *volume_key,
				      size_t volume_key_len)
{
	struct unload_class_keys_call call = {
		.volume_uuid = volume_uuid,
		.volume_key = volume_key,
		.volume_key_len = volume_key_len,
	};

	if (!volume_uuid || !volume_key || !volume_key_len)
		return -EINVAL;
	return apple_sep_fv_call(call_unload_class_keys, &call);
}
EXPORT_SYMBOL_GPL(apple_sep_fv_unload_class_keys_v2);

struct file_key_call {
	const u8 *volume_uuid;
	u32 protection_class;
	const u8 *wrapped_ekwk;
	size_t wrapped_ekwk_len;
	const u8 *wrapped_ek;
	size_t wrapped_ek_len;
	struct apple_sep_fv_key *key;
};

static int call_unwrap_file_key(const struct sep_fv_ops *ops, void *argument)
{
	struct file_key_call *call = argument;

	return ops->unwrap_file_key(apple_sep_fv_context, call->volume_uuid,
			call->protection_class, call->wrapped_ekwk,
			call->wrapped_ekwk_len, call->wrapped_ek,
			call->wrapped_ek_len, call->key);
}

int apple_sep_fv_unwrap_file_key(const u8 volume_uuid[16],
				 u32 protection_class,
				 const u8 *wrapped_ekwk,
				 size_t wrapped_ekwk_len,
				 const u8 *wrapped_ek,
				 size_t wrapped_ek_len,
				 struct apple_sep_fv_key *key)
{
	struct file_key_call call = {
		.volume_uuid = volume_uuid,
		.protection_class = protection_class,
		.wrapped_ekwk = wrapped_ekwk,
		.wrapped_ekwk_len = wrapped_ekwk_len,
		.wrapped_ek = wrapped_ek,
		.wrapped_ek_len = wrapped_ek_len,
		.key = key,
	};
	int ret;

	if (!volume_uuid || !wrapped_ekwk || !wrapped_ekwk_len || !wrapped_ek ||
	    !wrapped_ek_len || !key)
		return -EINVAL;
	memzero_explicit(key, sizeof(*key));
	ret = apple_sep_fv_call(call_unwrap_file_key, &call);
	if (ret)
		memzero_explicit(key, sizeof(*key));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_fv_unwrap_file_key);

struct new_file_key_call {
	const u8 *volume_uuid;
	u32 protection_class;
	u64 crypto_id;
	u16 key_revision;
	struct apple_sep_fv_new_file_key *key;
};

static int call_new_file_key(const struct sep_fv_ops *ops, void *argument)
{
	struct new_file_key_call *call = argument;

	return ops->new_file_key(apple_sep_fv_context, call->volume_uuid,
				 call->protection_class, call->crypto_id,
				 call->key_revision, call->key);
}

int apple_sep_fv_new_file_key_v2(const u8 volume_uuid[16],
				 u32 protection_class,
				 u64 crypto_id, u16 key_revision,
				 struct apple_sep_fv_new_file_key *key)
{
	struct new_file_key_call call = {
		.volume_uuid = volume_uuid,
		.protection_class = protection_class,
		.crypto_id = crypto_id,
		.key_revision = key_revision,
		.key = key,
	};
	int ret;

	if (!volume_uuid || !crypto_id || !key_revision || !key)
		return -EINVAL;
	memzero_explicit(key, sizeof(*key));
	ret = apple_sep_fv_call(call_new_file_key, &call);
	if (ret)
		memzero_explicit(key, sizeof(*key));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_fv_new_file_key_v2);
