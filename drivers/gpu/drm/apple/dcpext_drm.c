// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Fixed iBoot mode, CPU shadow updates, optionally synchronized buffer swaps. */
#include <linux/dma-mapping.h>
#include <linux/bitmap.h>
#include <linux/dma-fence.h>
#include <linux/iosys-map.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include "dcp.h"
#include "dcpext_mode.h"
#include "dcpext_drm.h"
#include "dcpext_scanout.h"

#define EXT_WIDTH DCPEXT_WIDTH
#define EXT_HEIGHT DCPEXT_HEIGHT
#define EXT_STRIDE DCPEXT_STRIDE
#define EXT_TILE_SIZE 32
#define EXT_TILE_COLS DIV_ROUND_UP(EXT_WIDTH, EXT_TILE_SIZE)
#define EXT_TILE_ROWS DIV_ROUND_UP(EXT_HEIGHT, EXT_TILE_SIZE)
#define EXT_TILE_COUNT (EXT_TILE_COLS * EXT_TILE_ROWS)

struct dcpext_drm {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	void *pixels;
	size_t size;
	u32 stride;
	struct apple_dcp *dcp;
	bool pageflips;
	u8 *row;
	/* Damage from the last completed frame; the idle buffer is one older. */
	DECLARE_BITMAP(previous_damage, EXT_TILE_COUNT);
	DECLARE_BITMAP(current_damage, EXT_TILE_COUNT);
	DECLARE_BITMAP(copy_damage, EXT_TILE_COUNT);
	bool damage_valid;
	/* Owned by the retained scanout, never by the detachable parent device. */
	atomic_t *terminal_error;
};

/* The firmware selected this geometry/rate; timing changes are not supported. */
static const struct drm_display_mode ext_mode = {
	DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		 594000, 3840, 4016, 4104, 4400, 0,
		 2160, 2168, 2178, 2250, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)
};

static bool ext_rect_valid(int x1, int y1, int x2, int y2, u32 pitch, size_t size)
{
	u64 end;

	if (x1 < 0 || y1 < 0 || x2 <= x1 || y2 <= y1 ||
	    x2 > EXT_WIDTH || y2 > EXT_HEIGHT || pitch < EXT_STRIDE)
		return false;
	end = (u64)(y2 - 1) * pitch + (u64)x2 * 4;
	return end <= size;
}

static int ext_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;
	struct drm_gem_object *obj;
	int ret;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						 DRM_PLANE_NO_SCALING, false, false);
	if (ret || !ps->visible)
		return ret;
	if (atomic_read(container_of(plane->dev, struct dcpext_drm, drm)->terminal_error))
		return -ENOLINK;
	/* Keep source/destination coordinates identical for bounded damage copies. */
	if (ps->src_x || ps->src_y || ps->src_w != EXT_WIDTH << 16 ||
	    ps->src_h != EXT_HEIGHT << 16 || ps->crtc_x || ps->crtc_y ||
	    ps->crtc_w != EXT_WIDTH || ps->crtc_h != EXT_HEIGHT ||
	    ps->fb->format->format != DRM_FORMAT_XRGB8888)
		return -EINVAL;
	obj = drm_gem_fb_get_obj(ps->fb, 0);
	if (!obj || ps->fb->offsets[0] > obj->size ||
	    !ext_rect_valid(0, 0, EXT_WIDTH, EXT_HEIGHT, ps->fb->pitches[0],
			    obj->size - ps->fb->offsets[0]))
		return -EINVAL;
	return 0;
}

/* A failed copy/firmware transaction must not become a successful fake flip. */
static void ext_cancel_flip(struct dcpext_drm *ext, struct drm_atomic_state *state, int error)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, &ext->crtc);
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	if (!cs)
		return;
	spin_lock_irqsave(&ext->drm.event_lock, flags);
	event = cs->event;
	cs->event = NULL;
	spin_unlock_irqrestore(&ext->drm.event_lock, flags);
	if (!event)
		return;
	if (event->base.fence) {
		dma_fence_set_error(event->base.fence, error);
		dma_fence_signal(event->base.fence);
	}
	if (event->base.completion) {
		complete_all(event->base.completion);
		if (event->base.completion_release)
			event->base.completion_release(event->base.completion);
		event->base.completion = NULL;
	}
	drm_event_cancel_free(&ext->drm, &event->base);
}

/* Bound history storage without merging distant clips into one large box.
 * A tile conservatively includes unchanged edge pixels, never omits damage.
 * Scratch maps are protected by the retained scanout's presentation lock.
 */
static void ext_damage_tiles(struct dcpext_drm *ext, struct drm_plane_state *old,
			     struct drm_plane_state *ps)
{
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect bounds = DRM_RECT_INIT(0, 0, EXT_WIDTH, EXT_HEIGHT);
	struct drm_rect damage;
	unsigned int x1, x2, y, y2;

	if (!ext->damage_valid) {
		bitmap_fill(ext->current_damage, EXT_TILE_COUNT);
		return;
	}
	bitmap_zero(ext->current_damage, EXT_TILE_COUNT);
	drm_atomic_helper_damage_iter_init(&iter, old, ps);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		if (!drm_rect_intersect(&damage, &bounds))
			continue;
		x1 = damage.x1 / EXT_TILE_SIZE;
		x2 = DIV_ROUND_UP(damage.x2, EXT_TILE_SIZE);
		y2 = DIV_ROUND_UP(damage.y2, EXT_TILE_SIZE);
		for (y = damage.y1 / EXT_TILE_SIZE; y < y2; y++)
			bitmap_set(ext->current_damage, y * EXT_TILE_COLS + x1, x2 - x1);
	}
}

static void ext_copy_rect(struct dcpext_drm *ext, struct drm_plane_state *ps,
			  const struct dcpext_frame *frame, const struct drm_rect *copy)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(ps);
	struct drm_framebuffer *fb = ps->fb;
	int x, y;

	for (y = copy->y1; y < copy->y2; y++) {
		size_t offset = (size_t)y * fb->pitches[0] + copy->x1 * 4;
		size_t bytes = drm_rect_width(copy) * 4;
		u8 *dst = frame->pixels + (size_t)y * ext->stride + copy->x1 * 4;

		iosys_map_memcpy_from(ext->row, &shadow->data[0], offset, bytes);
		for (x = 0; x < drm_rect_width(copy); x++)
			ext->row[x * 4 + 3] = 0xff;
		/* Convert in cached memory, then stream to coherent scanout memory.
		 * Byte stores directly to that mapping make full 4K copies costly.
		 */
		memcpy(dst, ext->row, bytes);
	}
}

static int ext_copy_frame(struct dcpext_drm *ext, struct drm_plane_state *old,
			  struct drm_plane_state *ps)
{
	struct dcpext_frame frame;
	unsigned int row, first, end, bit, next;
	int ret;

	ret = dcpext_scanout_begin_frame(ext->dcp, &frame);
	if (ret)
		return ret;
	if (!ext_rect_valid(0, 0, EXT_WIDTH, EXT_HEIGHT, ext->stride, frame.size)) {
		dcpext_scanout_end_frame(ext->dcp, false);
		return -EINVAL;
	}
	/* The idle buffer contains frame N-2. Repair both N-1's damage and the
	 * current damage from the current framebuffer, without touching scanout.
	 * Invalid history forces two complete copies, initializing both buffers.
	 */
	ext_damage_tiles(ext, old, ps);
	if (ext->damage_valid)
		bitmap_or(ext->copy_damage, ext->current_damage,
			  ext->previous_damage, EXT_TILE_COUNT);
	else
		bitmap_copy(ext->copy_damage, ext->current_damage, EXT_TILE_COUNT);
	for (row = 0; row < EXT_TILE_ROWS; row++) {
		first = row * EXT_TILE_COLS;
		end = first + EXT_TILE_COLS;
		bit = find_next_bit(ext->copy_damage, end, first);
		while (bit < end) {
			struct drm_rect copy;

			next = find_next_zero_bit(ext->copy_damage, end, bit);
			copy.x1 = (bit - first) * EXT_TILE_SIZE;
			copy.x2 = min((next - first) * EXT_TILE_SIZE, EXT_WIDTH);
			copy.y1 = row * EXT_TILE_SIZE;
			copy.y2 = min((row + 1) * EXT_TILE_SIZE, EXT_HEIGHT);
			ext_copy_rect(ext, ps, &frame, &copy);
			bit = find_next_bit(ext->copy_damage, end, next);
		}
	}
	ret = dcpext_scanout_end_frame(ext->dcp, true);
	/* Ordered atomic commits advance history only after a completed swap. */
	if (!ret) {
		bitmap_copy(ext->previous_damage, ext->current_damage, EXT_TILE_COUNT);
		ext->damage_valid = true;
	}
	return ret;
}

static void ext_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct dcpext_drm *ext = container_of(plane->dev, struct dcpext_drm, drm);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(ps);
	struct drm_framebuffer *fb = ps->fb;
	struct drm_gem_object *obj;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage, bounds = DRM_RECT_INIT(0, 0, EXT_WIDTH, EXT_HEIGHT);
	int idx, x, y, ret;

	if (!ps->visible || !fb)
		return;
	ret = atomic_read(ext->terminal_error);
	if (ret)
		goto cancel;
	obj = drm_gem_fb_get_obj(fb, 0);
	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		goto cancel;
	if (!drm_dev_enter(plane->dev, &idx)) {
		ret = -ENODEV;
		goto end_access;
	}
	if (ext->pageflips) {
		ret = ext_copy_frame(ext, old, ps);
		goto exit_device;
	}
	drm_atomic_helper_damage_iter_init(&iter, old, ps);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		if (!drm_rect_intersect(&damage, &bounds))
			continue;
		if (WARN_ON_ONCE(!obj || fb->offsets[0] > obj->size ||
		    !ext_rect_valid(damage.x1, damage.y1, damage.x2, damage.y2,
				    fb->pitches[0], obj->size - fb->offsets[0]) ||
		    !ext_rect_valid(damage.x1, damage.y1, damage.x2, damage.y2,
				    ext->stride, ext->size)))
			continue;
		for (y = damage.y1; y < damage.y2; ++y) {
			u8 *dst = ext->pixels + (size_t)y * ext->stride + damage.x1 * 4;

			iosys_map_memcpy_from(dst, &shadow->data[0],
				(size_t)y * fb->pitches[0] + damage.x1 * 4,
				(damage.x2 - damage.x1) * 4);
			/* Firmware BGRA requires opaque alpha; XRGB's X is undefined. */
			for (x = 0; x < damage.x2 - damage.x1; ++x)
				dst[x * 4 + 3] = 0xff;
		}
	}
	dma_wmb();
exit_device:
	drm_dev_exit(idx);
end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
cancel:
	if (ext->pageflips && ret)
		ext_cancel_flip(ext, state, ret);
}

static void ext_plane_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct dcpext_drm *ext = container_of(plane->dev, struct dcpext_drm, drm);
	u32 *dst = ext->pixels;
	struct dcpext_frame frame;
	int idx, i, ret;

	ret = atomic_read(ext->terminal_error);
	if (ret || !drm_dev_enter(plane->dev, &idx)) {
		if (ext->pageflips)
			ext_cancel_flip(ext, state, ret ? ret : -ENODEV);
		return;
	}
	if (ext->pageflips) {
		ext->damage_valid = false;
		ret = dcpext_scanout_begin_frame(ext->dcp, &frame);
		if (ret) {
			ext_cancel_flip(ext, state, ret);
			goto exit_device;
		}
		dst = frame.pixels;
	}
	for (i = 0; i < EXT_WIDTH * EXT_HEIGHT; ++i)
		dst[i] = cpu_to_le32(0xff000000);
	dma_wmb();
	if (ext->pageflips) {
		ret = dcpext_scanout_end_frame(ext->dcp, true);
		if (ret)
			ext_cancel_flip(ext, state, ret);
	}
exit_device:
	drm_dev_exit(idx);
}

static const struct drm_plane_funcs ext_plane_funcs = {
	DRM_GEM_SHADOW_PLANE_FUNCS,
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
};
static const struct drm_plane_helper_funcs ext_plane_helpers = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ext_plane_check,
	.atomic_update = ext_plane_update,
	.atomic_disable = ext_plane_disable,
};
static enum drm_mode_status ext_mode_valid(struct drm_crtc *crtc,
					 const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &ext_mode) ? MODE_OK : MODE_BAD;
}
static int ext_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

	return cs->enable ? drm_atomic_helper_check_crtc_primary_plane(cs) : 0;
}
static const struct drm_crtc_funcs ext_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.destroy = drm_crtc_cleanup,
};
static const struct drm_crtc_helper_funcs ext_crtc_helpers = {
	.mode_valid = ext_mode_valid,
	.atomic_check = ext_crtc_check,
};
static int ext_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ext_mode);
}
static enum drm_connector_status ext_detect(struct drm_connector *connector, bool force)
{
	struct dcpext_drm *ext = container_of(connector->dev, struct dcpext_drm, drm);

	return atomic_read(ext->terminal_error) ? connector_status_disconnected :
		connector_status_connected;
}
static const struct drm_connector_funcs ext_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = ext_detect,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.destroy = drm_connector_cleanup,
};
static const struct drm_connector_helper_funcs ext_connector_helpers = {
	.get_modes = ext_get_modes,
};
static const struct drm_encoder_funcs ext_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};
static const struct drm_mode_config_funcs ext_mode_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};
DEFINE_DRM_GEM_FOPS(ext_fops);
static const struct drm_driver ext_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ext_fops,
	.name = "apple-dcpext-shadow",
	.desc = "Apple external fixed iBoot shadow scanout",
	.major = 1,
	.minor = 0,
};

static void ext_unregister(void *data)
{
	struct drm_device *drm = data;

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

int dcpext_drm_register(struct apple_dcp *dcp, void *pixels, size_t size, u32 stride,
			atomic_t *terminal_error, struct drm_device **retained_drm)
{
	static const u32 formats[] = { DRM_FORMAT_XRGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct dcpext_drm *ext;
	struct drm_device *drm;
	int ret;

	if (!terminal_error || !retained_drm || atomic_read(terminal_error) || !pixels || !IS_ALIGNED((unsigned long)pixels, 4) || stride != EXT_STRIDE ||
	    !ext_rect_valid(0, 0, EXT_WIDTH, EXT_HEIGHT, stride, size))
		return -EINVAL;
	ext = devm_drm_dev_alloc(dcp->dev, &ext_driver, struct dcpext_drm, drm);
	if (IS_ERR(ext))
		return PTR_ERR(ext);
	drm = &ext->drm;
	ext->pixels = pixels;
	ext->size = size;
	ext->stride = stride;
	ext->terminal_error = terminal_error;
	ext->dcp = dcp;
	ext->pageflips = dcpext_scanout_pageflips(dcp);
	if (ext->pageflips) {
		ext->row = drmm_kmalloc(drm, EXT_STRIDE, GFP_KERNEL);
		if (!ext->row)
			return -ENOMEM;
	}
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = EXT_WIDTH;
	drm->mode_config.max_width = EXT_WIDTH;
	drm->mode_config.min_height = EXT_HEIGHT;
	drm->mode_config.max_height = EXT_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &ext_mode_funcs;
	ret = drm_universal_plane_init(drm, &ext->plane, 0, &ext_plane_funcs,
		formats, ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&ext->plane, &ext_plane_helpers);
	drm_plane_enable_fb_damage_clips(&ext->plane);
	ret = drm_crtc_init_with_planes(drm, &ext->crtc, &ext->plane, NULL,
					&ext_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&ext->crtc, &ext_crtc_helpers);
	ret = drm_encoder_init(drm, &ext->encoder, &ext_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	ext->encoder.possible_crtcs = drm_crtc_mask(&ext->crtc);
	ret = drm_connector_init(drm, &ext->connector, &ext_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	drm_connector_helper_add(&ext->connector, &ext_connector_helpers);
	ret = drm_connector_attach_encoder(&ext->connector, &ext->encoder);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	/* No vblank IRQ: the helper timestamps completion in software. In pageflip
	 * mode the copy/commit also waits for firmware completion before this event;
	 * the legacy single-buffer mode remains unsynchronized and may tear.
	 * Deliberately no fbdev/client setup, console takeover, or render node.
	 */
	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dcp->dev, ext_unregister, drm);
	if (ret)
		return ret;
	/* Keep notification work safe even after parent devres teardown. */
	drm_dev_get(drm);
	WRITE_ONCE(*retained_drm, drm);
	dev_info(dcp->dev, "external shadow DRM registered: fixed 3840x2160, CPU copies, %s\n",
		 ext->pageflips ? "firmware-completed double-buffer swaps" : "no hardware vblank");
	return 0;
}
