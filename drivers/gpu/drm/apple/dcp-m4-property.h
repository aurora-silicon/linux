/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_PROPERTY_H
#define APPLE_DCP_M4_PROPERTY_H

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DCP_M4_SCALARS 256
#define DCP_M4_RAW_PROPERTIES 32
#define DCP_M4_RAW_LIMIT (1U << 20)
#define DCP_M4_RAW_TOTAL (8U << 20)

struct dcp_m4_scalar_property {
	char name[64];
	u32 service;
	u64 value;
	bool used;
};

struct dcp_m4_raw_property {
	char name[64];
	u32 service;
	u32 size;
	void *data;
	bool used;
};

/* Zero-initialize in heap storage; one serialized callback owner only. */
struct dcp_m4_properties {
	struct dcp_m4_scalar_property scalar[DCP_M4_SCALARS];
	struct dcp_m4_raw_property raw[DCP_M4_RAW_PROPERTIES];
	void *chunk;
	u32 raw_bytes;
	u32 chunk_size;
	u32 chunk_filled;
	bool chunk_active;
};

static inline int dcp_m4_property_name(const void *name)
{
	return memchr(name, 0, 64) ? 0 : -EINVAL;
}

static inline int dcp_m4_scalar_slot(struct dcp_m4_properties *props, u32 service,
				   const char *name, bool create)
{
	int free_slot = -ENOSPC;

	for (unsigned int i = 0; i < DCP_M4_SCALARS; i++) {
		if (props->scalar[i].used) {
			if (props->scalar[i].service == service && !strcmp(props->scalar[i].name, name))
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return create ? free_slot : -ENOENT;
}

static inline int dcp_m4_raw_slot(struct dcp_m4_properties *props, u32 service,
				const char *name, bool create)
{
	int free_slot = -ENOSPC;

	for (unsigned int i = 0; i < DCP_M4_RAW_PROPERTIES; i++) {
		if (props->raw[i].used) {
			if (props->raw[i].service == service && !strcmp(props->raw[i].name, name))
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return create ? free_slot : -ENOENT;
}

static inline int dcp_m4_scalar_set(struct dcp_m4_properties *props, u32 service,
				  const char *name, u64 value)
{
	int slot = dcp_m4_scalar_slot(props, service, name, true);
	struct dcp_m4_scalar_property *property;

	if (slot < 0)
		return slot;
	property = &props->scalar[slot];
	strscpy(property->name, name, sizeof(property->name));
	property->service = service;
	property->value = value;
	property->used = true;
	return 0;
}

static inline int dcp_m4_raw_set(struct dcp_m4_properties *props, u32 service,
			       const char *name, const void *data, u32 size, bool take_chunk)
{
	int slot = dcp_m4_raw_slot(props, service, name, true);
	struct dcp_m4_raw_property *property;
	void *copy = NULL;

	if (slot < 0)
		return slot;
	if (size > DCP_M4_RAW_LIMIT || props->raw_bytes > DCP_M4_RAW_TOTAL - props->chunk_size)
		return -E2BIG;
	/* Include old values and the in-progress chunk in the allocation budget. */
	if (!take_chunk && size > DCP_M4_RAW_TOTAL - props->raw_bytes - props->chunk_size)
		return -ENOSPC;
	if (take_chunk) {
		if (!props->chunk_active || props->chunk_filled != props->chunk_size ||
		    size != props->chunk_size || data != props->chunk)
			return -EINVAL;
		copy = props->chunk;
	} else if (size) {
		copy = kvmalloc(size, GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, data, size);
	}
	property = &props->raw[slot];
	if (property->used) {
		props->raw_bytes -= property->size;
		kvfree(property->data);
	}
	strscpy(property->name, name, sizeof(property->name));
	property->service = service;
	property->size = size;
	property->data = copy;
	property->used = true;
	props->raw_bytes += size;
	if (take_chunk) {
		props->chunk = NULL;
		props->chunk_active = false;
		props->chunk_size = props->chunk_filled = 0;
	}
	return 0;
}

/* Only the exact-shape-validated callback dispatcher may call this hook. */
static inline int dcp_m4_property_callback(void *cookie, unsigned int method,
					 const void *input, void *output)
{
	struct dcp_m4_properties *props = cookie;
	const u8 *in = input;
	u8 *out = output;
	const char *name;
	u32 service = 0, offset = 0, size;
	int slot, ret;

	if (method == 127) {
		size = get_unaligned_le32(in);
		if (!size || size > DCP_M4_RAW_LIMIT + 1 || props->chunk_active)
			return -EINVAL;
		size--;
		if (size > DCP_M4_RAW_TOTAL - props->raw_bytes)
			return -ENOSPC;
		props->chunk = size ? kvmalloc(size, GFP_KERNEL) : NULL;
		if (size && !props->chunk)
			return -ENOMEM;
		props->chunk_size = size;
		props->chunk_filled = 0;
		props->chunk_active = true;
		put_unaligned_le32(1, out);
		return 0;
	}
	if (method == 128) {
		offset = get_unaligned_le32(in + 4096);
		size = get_unaligned_le32(in + 4100);
		if (!props->chunk_active || size > 4096 || offset != props->chunk_filled ||
		    size > props->chunk_size - props->chunk_filled)
			return -EINVAL;
		if (size)
			memcpy((u8 *)props->chunk + offset, in, size);
		props->chunk_filled += size;
		put_unaligned_le32(1, out);
		return 0;
	}
	if (method == 400 || method == 401 || method == 413 || method == 414 || method == 415) {
		service = get_unaligned_le32(in);
		offset = 4;
	}
	name = (const char *)in + offset;
	if (dcp_m4_property_name(name))
		return -EINVAL;
	switch (method) {
	case 107:
		slot = dcp_m4_scalar_slot(props, 0, name, false);
		if (slot >= 0)
			props->scalar[slot].used = false;
		return 0;
	case 401:
		slot = dcp_m4_scalar_slot(props, service, name, false);
		if (slot >= 0) {
			put_unaligned_le64(props->scalar[slot].value, out);
			out[8] = 1;
		}
		return 0;
	case 400:
		if (in[72] & 1)
			return -EINVAL;
		slot = dcp_m4_raw_slot(props, service, name, false);
		if (slot >= 0) {
			size = props->raw[slot].size;
			if (size > 0xc00 || size > get_unaligned_le32(in + 68))
				return -EMSGSIZE;
			if (size)
				memcpy(out, props->raw[slot].data, size);
			put_unaligned_le32(size, out + 0xc00);
		}
		return 0;
	case 563: case 414:
		ret = (in[offset + 72] & 1) ? 0 :
			dcp_m4_scalar_set(props, service, name, get_unaligned_le64(in + offset + 64));
		break;
	case 565: case 415:
		ret = (in[offset + 68] & 1) ? 0 :
			dcp_m4_scalar_set(props, service, name, get_unaligned_le32(in + offset + 64));
		break;
	case 102: case 104:
		return dcp_m4_scalar_set(props, 0, name,
			method == 104 ? !!in[64] : get_unaligned_le32(in + 64));
	case 413: case 552: case 561:
		ret = (in[offset + 4160] & 1) ? 0 :
			dcp_m4_raw_set(props, service, name, in + offset + 64, 4096, false);
		break;
	case 567:
		size = strnlen((const char *)in + 64, 64);
		if (size == 64)
			return -EINVAL;
		ret = dcp_m4_raw_set(props, 0, name, in + 64, size, false);
		break;
	case 129:
		ret = dcp_m4_raw_set(props, 0, name, props->chunk, props->chunk_size, true);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (ret)
		return ret;
	put_unaligned_le32(1, out);
	return 0;
}

/* Host-only property storage: never DMA buffers or live DCP session teardown. */
static inline void dcp_m4_properties_destroy(struct dcp_m4_properties *props)
{
	for (unsigned int i = 0; i < DCP_M4_RAW_PROPERTIES; i++)
		kvfree(props->raw[i].data);
	kvfree(props->chunk);
	memset(props, 0, sizeof(*props));
}

#endif
