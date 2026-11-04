/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_FRAME_H
#define APPLE_DCP_M4_FRAME_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "dcp-m4-profile.h"

/* Qualified firmware-26.6.2 swap and physical panel dimensions. */
#define DCP_M4_SWAP_SIZE		(dcp_m4_pf->swap_size)
#define DCP_M4_SURFACE_SIZE	0x22cU
#define DCP_M4_PANEL_WIDTH	(dcp_m4_pf->panel_width)
#define DCP_M4_PANEL_HEIGHT	(dcp_m4_pf->panel_height)

/* Used only after matching firmware-advertised preferred physical timing. */
enum dcp_m4_frame_format {
	DCP_M4_XRGB8888,
	DCP_M4_ARGB8888,
};

struct dcp_m4_frame {
	u64 dva;
	u64 backing_size;
	u32 width;
	u32 height;
	u32 stride;
	enum dcp_m4_frame_format format;
};

static inline int dcp_m4_frame_check(const struct dcp_m4_frame *frame)
{
	u64 size = (u64)frame->stride * frame->height;

	if (frame->width != DCP_M4_PANEL_WIDTH || frame->height != DCP_M4_PANEL_HEIGHT ||
	    frame->stride < DCP_M4_PANEL_WIDTH * 4 || frame->stride % 64 ||
	    !size || size > 0xffffffffULL || frame->backing_size < size ||
	    !frame->dva || frame->dva % 0x4000 || frame->dva >= (1ULL << 42) ||
	    size > (1ULL << 42) - frame->dva ||
	    (frame->format != DCP_M4_XRGB8888 && frame->format != DCP_M4_ARGB8888))
		return -EINVAL;
	return 0;
}

/* Display-consumer DMA aperture: the 64-GiB DART range at its 1-TiB alias. */
#define DCP_M4_SCANOUT_BASE	(1ULL << 40)
#define DCP_M4_SCANOUT_SIZE	(1ULL << 36)

/* Accumulate one mapped DMA segment; only one exactly contiguous extent qualifies. */
static inline int dcp_m4_extent_add(u64 *start, u64 *length, u64 addr, u64 size)
{
	if (!size)
		return -EINVAL;
	if (!*length) {
		if (size > U64_MAX - addr)
			return -ERANGE;
		*start = addr;
		*length = size;
		return 0;
	}
	/* Invariant: start + length never wraps. */
	if (addr != *start + *length || size > U64_MAX - addr)
		return -ERANGE;
	*length += size;
	return 0;
}

/* A client mapping must cover the needed bytes inside the SID0 aperture. */
static inline int dcp_m4_direct_extent(u64 dva, u64 mapped, u64 needed)
{
	if (!dva || dva % 0x4000 || !needed || mapped < needed ||
	    dva < DCP_M4_SCANOUT_BASE || dva - DCP_M4_SCANOUT_BASE >= DCP_M4_SCANOUT_SIZE ||
	    mapped > DCP_M4_SCANOUT_SIZE - (dva - DCP_M4_SCANOUT_BASE))
		return -ERANGE;
	return 0;
}

/* Describe a qualified client framebuffer at its SID0 DVA; no CPU access. */
static inline int dcp_m4_direct_frame(struct dcp_m4_frame *frame, u64 dva, u64 mapped,
				      u32 width, u32 height, u32 pitch, bool alpha)
{
	*frame = (struct dcp_m4_frame) {
		.dva = dva, .backing_size = mapped, .width = width, .height = height,
		.stride = pitch, .format = alpha ? DCP_M4_ARGB8888 : DCP_M4_XRGB8888,
	};
	if (dcp_m4_direct_extent(dva, mapped, (u64)pitch * height))
		return -ERANGE;
	return dcp_m4_frame_check(frame);
}

/* Exact single-layer fixture, not an ABI for tiled or compressed surfaces. */
static inline void dcp_m4_surface_encode(u8 *surface, const struct dcp_m4_frame *frame)
{
	u32 size = frame->stride * frame->height;

	memset(surface, 0, DCP_M4_SURFACE_SIZE);
	surface[2] = frame->format == DCP_M4_XRGB8888;
	put_unaligned_le32(1, surface + 3);
	put_unaligned_le32(1, surface + 7);
	put_unaligned_le32(0x42475241, surface + 0x0b);
	surface[0x13] = 13;
	surface[0x14] = 12;
	put_unaligned_le32(frame->stride, surface + 0x15);
	put_unaligned_le16(1, surface + 0x19);
	surface[0x1b] = 1;
	surface[0x1c] = 1;
	put_unaligned_le32(frame->width, surface + 0x21);
	put_unaligned_le32(frame->height, surface + 0x25);
	put_unaligned_le32(size, surface + 0x29);
	put_unaligned_le32(1, surface + 0x35);
	put_unaligned_le64(1, surface + 0x51);
	put_unaligned_le32(frame->width, surface + 0x59);
	put_unaligned_le32(frame->height, surface + 0x5d);
	put_unaligned_le32(frame->stride, surface + 0x69);
	put_unaligned_le32(size, surface + 0x6d);
	put_unaligned_le16(4, surface + 0x71);
	surface[0x73] = 1;
	surface[0x74] = 1;
	put_unaligned_le64(1, surface + 0x149);
}

/* Encode an integer development brightness without floating-point operations. */
static inline u64 dcp_m4_nits_binary64(u32 nits)
{
	unsigned int bit = 0;

	if (!nits)
		return 0;
	while ((nits >> bit) > 1)
		bit++;
	return ((u64)(1023 + bit) << 52) | ((u64)(nits - (1U << bit)) << (52 - bit));
}

/*
 * External outputs: any qualified size up to 3840x2160, same surface layout.
 * The frame must already be described at its wire DVA in an owned mapping.
 */
static inline int dcp_m4_frame_check_ext(const struct dcp_m4_frame *frame)
{
	u64 size = (u64)frame->stride * frame->height;

	if (!frame->width || !frame->height || frame->width > 3840 || frame->height > 2160 ||
	    frame->stride < frame->width * 4 || frame->stride % 64 ||
	    !size || size > 0xffffffffULL || frame->backing_size < size ||
	    !frame->dva || frame->dva % 0x4000 || frame->dva >= (1ULL << 42) ||
	    size > (1ULL << 42) - frame->dva ||
	    (frame->format != DCP_M4_XRGB8888 && frame->format != DCP_M4_ARGB8888))
		return -EINVAL;
	return 0;
}

/*
 * One upper surface of an indexed swap (cursor/overlay contract): firmware
 * surface-array index 1..3, a fully on-panel destination (the caller clips),
 * no scaling. A NULL frame removes the surface.
 */
struct dcp_m4_upper {
	const struct dcp_m4_frame *frame;
	u32 index, x, y;
	u32 id, flags;
};

static inline int dcp_m4_upper_check(const struct dcp_m4_upper *u)
{
	const struct dcp_m4_frame *f = u->frame;
	u64 size;

	if (u->index < 1 || u->index > 3)
		return -EINVAL;
	if (!f)
		return 0;
	size = (u64)f->stride * f->height;
	/* Firmware rejects (or crashes on) visible surfaces below 32x32. */
	if (f->width < 32 || f->height < 32 || f->width > 256 || f->height > 256 ||
	    f->stride < f->width * 4 || f->stride % 64 || f->backing_size < size ||
	    !f->dva || f->dva % 0x4000 || f->dva >= (1ULL << 42) ||
	    f->format != DCP_M4_ARGB8888 ||
	    u->x > DCP_M4_PANEL_WIDTH - f->width || u->y > DCP_M4_PANEL_HEIGHT - f->height)
		return -EINVAL;
	return 0;
}

/*
 * Swap request. With primary, surface 0 is presented (frame) or everything is
 * blanked (NULL frame, background colour): mask 0x80000007, less the upper
 * indices in keep while presenting (so a primary flip never removes a live
 * cursor), plus them when blanking. upper adds its own update bit: a swap
 * without primary leaves surface 0 untouched (bit 0 clear, null byte 1).
 */
static inline int dcp_m4_swap_encode_full(void *output, size_t length, u32 swap_id,
					  bool primary, const struct dcp_m4_frame *frame,
					  const struct dcp_m4_upper *upper, u32 keep,
					  bool brightness_change, u32 nits,
					  int (*check)(const struct dcp_m4_frame *frame))
{
	u8 *data = output;
	u32 mask = 0;

	if (length != DCP_M4_SWAP_SIZE || !swap_id || (brightness_change && nits > 500) ||
	    (!primary && !upper) || (keep & ~0xeU))
		return -EINVAL;
	if (primary && frame && check(frame))
		return -EINVAL;
	if (upper && dcp_m4_upper_check(upper))
		return -EINVAL;
	if (primary)
		mask = frame ? 0x80000007 & ~keep : 0x80000007 | keep;
	if (upper)
		mask |= BIT(upper->index);
	memset(data, 0, length);
	put_unaligned_le32(swap_id, data + 0x98);
	put_unaligned_le32(mask, data + 0x14c);
	put_unaligned_le32(mask, data + 0x150);
	put_unaligned_le32(primary && !frame ? dcp_m4_pf->disabled_color : 0xff000000,
			   data + 0x154);
	memset(data + dcp_m4_pf->swap_nullable, 1, 10);
	data[dcp_m4_pf->swap_tail] = 1;
	data[dcp_m4_pf->swap_tail + 1] = 1;
	if (brightness_change) {
		data[0x354] = 1;
		put_unaligned_le64(dcp_m4_nits_binary64(nits), data + dcp_m4_pf->swap_nits);
	}
	if (primary && frame) {
		put_unaligned_le32(1, data + 0x9c);
		put_unaligned_le32(frame->width, data + 0xb4);
		put_unaligned_le32(frame->height, data + 0xb8);
		put_unaligned_le32(frame->width, data + 0x114);
		put_unaligned_le32(frame->height, data + 0x118);
		dcp_m4_surface_encode(data + dcp_m4_pf->swap_surface, frame);
		put_unaligned_le64(frame->dva, data + dcp_m4_pf->swap_dva);
		data[dcp_m4_pf->swap_nullable] = 0;
	}
	if (upper && upper->frame) {
		const struct dcp_m4_frame *f = upper->frame;
		u32 i = upper->index;

		put_unaligned_le32(upper->id, data + 0x9c + 4 * i);
		/* Source x,y,w,h then destination x,y,w,h. */
		put_unaligned_le32(f->width, data + 0xac + 16 * i + 8);
		put_unaligned_le32(f->height, data + 0xac + 16 * i + 12);
		put_unaligned_le32(upper->flags, data + 0xec + 4 * i);
		put_unaligned_le32(upper->x, data + 0x10c + 16 * i);
		put_unaligned_le32(upper->y, data + 0x10c + 16 * i + 4);
		put_unaligned_le32(f->width, data + 0x10c + 16 * i + 8);
		put_unaligned_le32(f->height, data + 0x10c + 16 * i + 12);
		dcp_m4_surface_encode(data + dcp_m4_pf->swap_surface + DCP_M4_SURFACE_SIZE * i, f);
		put_unaligned_le64(f->dva, data + dcp_m4_pf->swap_dva + 8 * i);
		data[dcp_m4_pf->swap_nullable + i] = 0;
	}
	return 0;
}

/* Swap intent that registers surface 0 as the idle-cache surface. */
#define DCP_M4_SWAP_INTENT_IDLE_CACHE	8

/*
 * Retarget an encoded present: the DCP registers rather than presents its
 * sole surface, which must be a distinct object (surface_id other than
 * the primary's 1) from every surface on screen.
 */
static inline void dcp_m4_swap_set_intent(void *output, u32 intent, u32 surface_id)
{
	u8 *data = output;

	put_unaligned_le32(intent, data + dcp_m4_pf->swap_intent);
	put_unaligned_le32(surface_id, data + 0x9c);
}

static inline int dcp_m4_swap_encode_checked(void *output, size_t length, u32 swap_id,
					     const struct dcp_m4_frame *frame,
					     bool brightness_change, u32 nits,
					     int (*check)(const struct dcp_m4_frame *frame))
{
	return dcp_m4_swap_encode_full(output, length, swap_id, true, frame, NULL, 0,
				       brightness_change, nits, check);
}

static inline int dcp_m4_swap_encode(void *output, size_t length, u32 swap_id,
				     const struct dcp_m4_frame *frame,
				     bool brightness_change, u32 nits)
{
	return dcp_m4_swap_encode_checked(output, length, swap_id, frame, brightness_change,
					  nits, dcp_m4_frame_check);
}

static inline int dcp_m4_swap_start_reply(const void *input, size_t size, u32 *swap_id)
{
	const u8 *data = input;
	u32 id;

	if (size != 8)
		return -EMSGSIZE;
	id = get_unaligned_le32(data);
	if (!id || get_unaligned_le32(data + 4))
		return -EPROTO;
	*swap_id = id;
	return 0;
}

/* A407 reply and D590 are independent gates; callers must wait for both. */
static inline int dcp_m4_swap_submit_reply(const void *input, size_t size)
{
	const u8 *data = input;

	if (size != 12)
		return -EMSGSIZE;
	return get_unaligned_le32(data + 5) ? -EPROTO : 0;
}

static inline int dcp_m4_swap_complete(const void *input, size_t size, u32 pending_id)
{
	if (size != dcp_m4_pf->done_size)
		return -EMSGSIZE;
	return pending_id && get_unaligned_le32(input) == pending_id ? 0 : -EPROTO;
}

/* Serialized owner only. The caller retains both scanout buffers on any error. */
struct dcp_m4_swap_state {
	u32 id;
	int error;
	bool replied;
	bool completed;
	bool emitted;
};

static inline int dcp_m4_swap_fail(struct dcp_m4_swap_state *swap, int error)
{
	if (!swap->error)
		swap->error = error < 0 ? error : -EPROTO;
	return swap->error;
}

/* Begin after A406 supplies its validated ID, before publishing A407. */
static inline int dcp_m4_swap_begin(struct dcp_m4_swap_state *swap, u32 id)
{
	if (swap->error)
		return swap->error;
	if (!id)
		return dcp_m4_swap_fail(swap, -EINVAL);
	if (swap->id && !swap->emitted)
		return dcp_m4_swap_fail(swap, -EBUSY);
	*swap = (struct dcp_m4_swap_state){.id = id};
	return 0;
}

static inline int dcp_m4_swap_record_reply(struct dcp_m4_swap_state *swap,
					 const void *reply, size_t size)
{
	int ret;

	if (swap->error)
		return swap->error;
	if (!swap->id || swap->replied)
		return dcp_m4_swap_fail(swap, -EPROTO);
	ret = dcp_m4_swap_submit_reply(reply, size);
	if (ret)
		return dcp_m4_swap_fail(swap, ret);
	swap->replied = true;
	return 0;
}

/* Called by the exact-shape-validated D590 hook; callback alone is not a flip. */
static inline int dcp_m4_swap_record_done(struct dcp_m4_swap_state *swap, u32 id)
{
	if (swap->error)
		return swap->error;
	if (!swap->id || id != swap->id || swap->completed)
		return dcp_m4_swap_fail(swap, -EPROTO);
	swap->completed = true;
	return 0;
}

/* One completion event, only after both the A407 reply and matching D590. */
static inline int dcp_m4_swap_take_completion(struct dcp_m4_swap_state *swap)
{
	if (swap->error)
		return swap->error;
	if (!swap->id || !swap->replied || !swap->completed || swap->emitted)
		return 0;
	swap->emitted = true;
	return 1;
}

#endif
