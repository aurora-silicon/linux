/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_MEMORY_H
#define APPLE_DCP_M4_MEMORY_H

#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "dcp-m4-profile.h"

#define DCP_M4_MEMORY_OBJECTS 64
#define DCP_M4_MEMORY_LIMIT (64U << 20)
#define DCP_M4_MEMORY_MAX (16U << 20)
#define DCP_M4_MEMORY_PAGE 0x4000U

struct dcp_m4_memory_object {
	u64 phys, dva, size;
	bool mapped, retired;
	/* External DCP: mapped by D201 flag 0 (SID0) / flag 1 (SID16), at dva0/dva16 */
	bool mapped0, mapped16;
	u64 dva0, dva16;
};

/* Heap-backed, single serialized callback owner. Retired IDs are never reused. */
struct dcp_m4_memory {
	struct dcp_m4_memory_object object[DCP_M4_MEMORY_OBJECTS];
	u32 count, bytes;
	bool failed;
	/*
	 * External DCP: any D451 options, an alignment up to a page,
	 * up to 16 MiB, and the NULL output flags honoured; the
	 * internal firmware's requests are only qualified as they are.
	 */
	bool relaxed;
	/* Internal DCP: also an alignment up to a page (the profile's alloc_page_align). */
	bool page_align;
};

struct dcp_m4_memory_ops {
	/* Must reserve both SID23/SID16 slots, pin backing, then publish only SID23. */
	int (*alloc)(void *cookie, u32 size, u64 *phys, u64 *dva);
	/* Must map the same backing at the same DVA in SID16, flush and verify. */
	int (*map)(void *cookie, u64 phys, u64 dva, u32 size);
	/* External DCP: map the backing in SID0 at a DVA of its own. */
	int (*map_sid0)(void *cookie, u64 phys, u32 size, u64 *dva);
	/* Map it in SID16: at @dva if paired at D451, else at one of its own. */
	int (*map_sid16)(void *cookie, u64 phys, u64 dva, u32 size, u64 *out);
	/* Whether the stream of a D201 flag may be used now (0) or not. */
	int (*usable)(void *cookie, unsigned int flag);
};

/*
 * An external firmware's D201 map_buf: +0 the D451 ID, +8 the flag (0:
 * SID0 scanout, 1: SID16 PIODMA), +9/+0xa/+0xb the buffer, map ID and DVA
 * NULL flags; out +0 a map ID of its own (ID << 1 | flag), +4 the DVA, +0xc
 * the status. A buffer already mapped on that stream answers that mapping
 * again (mappings are retained, D202 releases none). A refusal fails this
 * callback, not every later one.
 */
/* The D201 statuses of a refusal: not found, and any other error. */
#define DCP_M4_D201_NOT_FOUND	14
#define DCP_M4_D201_ERROR	1

static inline int dcp_m4_memory_map_ext(struct dcp_m4_memory *memory,
					const struct dcp_m4_memory_ops *ops, void *cookie,
					const u8 *in, u8 *out)
{
	struct dcp_m4_memory_object *object;
	u32 id = get_unaligned_le32(in);
	u32 status = DCP_M4_D201_ERROR;
	u8 flag = in[8];
	u64 dva;
	int ret;

	/*
	 * A refusal is the firmware's to handle: answered with a nonzero
	 * status (map ID and DVA left zero) and acknowledged, so that the
	 * session goes on.
	 */
	if (memory->failed || in[9] || in[10] > 1 || in[11] > 1 || flag > 1 ||
	    !ops->usable || !ops->map_sid0 || !ops->map_sid16)
		goto refuse;
	if (!id || id > memory->count || memory->object[id - 1].retired) {
		status = DCP_M4_D201_NOT_FOUND;
		goto refuse;
	}
	object = &memory->object[id - 1];
	/* A mapping made earlier only on a stream still usable now. */
	ret = ops->usable(cookie, flag);
	if (ret)
		goto refuse;
	if (flag && !object->mapped16) {
		ret = ops->map_sid16(cookie, object->phys, object->dva, object->size, &dva);
		if (ret)
			goto refuse;
		object->mapped16 = true;
		object->dva16 = dva;
	} else if (!flag && !object->mapped0) {
		ret = ops->map_sid0(cookie, object->phys, object->size, &dva);
		if (ret)
			goto refuse;
		object->mapped0 = true;
		object->dva0 = dva;
	}
	if (!in[10])
		put_unaligned_le32(id << 1 | flag, out);
	if (!in[11])
		put_unaligned_le64(flag ? object->dva16 : object->dva0, out + 4);
	put_unaligned_le32(0, out + 12);
	return 0;
refuse:
	put_unaligned_le32(status, out + 12);
	return 0;
}

/* Called only after the dispatcher validates exact callback input/output sizes. */
static inline int dcp_m4_memory_callback(struct dcp_m4_memory *memory,
					 const struct dcp_m4_memory_ops *ops, void *cookie,
					 unsigned int method, const void *input, void *output)
{
	struct dcp_m4_memory_object *object;
	const u8 *in = input;
	u8 *out = output;
	u64 size, phys = 0, dva = 0, id;
	u32 align;
	int ret = -EINVAL;

	if (memory->failed)
		return -EIO;
	if (method == 451) {
		size = get_unaligned_le64(in + 4);
		align = get_unaligned_le32(in + 12);
		/*
		 * The qualified internal firmware requests 4, the J604's also a
		 * power of two within a page. An external one's alignment, in
		 * bytes, must be a power of two within a page, which every buffer
		 * has.
		 */
		if (memory->relaxed && (!size || size > DCP_M4_MEMORY_MAX ||
					(align && (align > DCP_M4_MEMORY_PAGE ||
						   (align & (align - 1)))) ||
					in[16] > 1 || in[17] > 1 || in[18] > 1))
			goto failed;
		if (!memory->relaxed &&
		    (get_unaligned_le32(in) != 0x703 || !size || size > DCP_M4_MEMORY_MAX ||
		     (align != 4 && (!memory->page_align || !is_power_of_2(align) ||
				     align > DCP_M4_MEMORY_PAGE)) ||
		     in[16] || in[17] || in[18]))
			goto failed;
		/* Byte 19 is padding, observed nonzero in otherwise valid requests. */
		size = (size + DCP_M4_MEMORY_PAGE - 1) & ~(u64)(DCP_M4_MEMORY_PAGE - 1);
		if (memory->count >= DCP_M4_MEMORY_OBJECTS || memory->bytes > DCP_M4_MEMORY_LIMIT - size) {
			ret = -ENOSPC;
			goto failed;
		}
		if (!ops->alloc) {
			ret = -EOPNOTSUPP;
			goto failed;
		}
		ret = ops->alloc(cookie, size, &phys, &dva);
		if (ret)
			goto failed;
		/* Never publish malformed allocator output to the firmware. */
		if (!phys || ((phys | dva) & (DCP_M4_MEMORY_PAGE - 1)) ||
		    phys >= (1ULL << 42) || size > (1ULL << 42) - phys ||
		    dva < 0x10000000000ULL || dva >= 0x11000000000ULL ||
		    size > 0x11000000000ULL - dva) {
			ret = -ERANGE;
			goto failed;
		}
		for (unsigned int i = 0; i < memory->count; i++) {
			object = &memory->object[i];
			if ((dva < object->dva + object->size && object->dva < dva + size) ||
			    (phys < object->phys + object->size && object->phys < phys + size)) {
				ret = -EEXIST;
				goto failed;
			}
		}
		object = &memory->object[memory->count++];
		*object = (struct dcp_m4_memory_object){ .phys = phys, .dva = dva, .size = size };
		memory->bytes += size;
		/* The output was zeroed; a NULL output pointer stays unwritten. */
		if (!in[16])
			put_unaligned_le64(phys, out);
		if (!in[17])
			put_unaligned_le64(dva, out + 8);
		if (!in[18])
			put_unaligned_le64(size, out + 16);
		put_unaligned_le32(memory->count, out + 24);
		return 0;
	}
	if (method != 201 && method != 454) {
		ret = -EOPNOTSUPP;
		goto failed;
	}
	id = method == 201 ? get_unaligned_le64(in) : get_unaligned_le32(in);
	if (method == 201 && memory->relaxed)
		return dcp_m4_memory_map_ext(memory, ops, cookie, in, out);
	if (!id || id > memory->count || (method == 201 && get_unaligned_le32(in + 8) != 1))
		goto failed;
	object = &memory->object[id - 1];
	if (object->retired)
		goto failed;
	if (method == 454) {
		/* No fence exists: retain backing, both translations and the ID. */
		object->retired = true;
		put_unaligned_le32(1, out);
		return 0;
	}
	if (object->mapped)
		goto failed;
	if (!ops->map) {
		ret = -EOPNOTSUPP;
		goto failed;
	}
	ret = ops->map(cookie, object->phys, object->dva, object->size);
	if (ret)
		goto failed;
	object->mapped = true;
	put_unaligned_le32(id, out);
	put_unaligned_le64(object->dva, out + 4);
	put_unaligned_le32(0, out + 12);
	return 0;
failed:
	memory->failed = true;
	return ret < 0 ? ret : -EPROTO;
}

#endif
