// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* External native DRM device. PRIME/shmem sources are copied into retained
 * hardware scanout by the route owner. Firmware never accesses GEM storage;
 * importing a GPU buffer therefore does not require contiguous physical RAM.
 * Native completion handling follows the internal M3 KMS implementation. */
#include <linux/module.h>
#include <linux/iosys-map.h>
#include <linux/dma-fence.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <linux/scatterlist.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include "m3_dcpext_kms.h"

struct m3_dcpext_kms {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct m3_dcpext_connector connector;
	const struct m3_dcpext_kms_ops *ops;
	void *cookie;
	struct mutex *route_lock;
	/* Outlives the hardware owner while userspace retains DRM references. */
	struct mutex owner_lock;
	bool detached;
	bool failed;
	u64 completed;
};

static struct m3_dcpext_kms *to_kms(struct drm_device *drm)
{
	return container_of(drm, struct m3_dcpext_kms, drm);
}

static int plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *c;
	struct drm_gem_object *obj;
	u64 extent;
	int ret;

	if (!p->crtc)
		return 0;
	c = drm_atomic_get_crtc_state(state, p->crtc);
	if (IS_ERR(c))
		return PTR_ERR(c);
	ret = drm_atomic_helper_check_plane_state(p, c, DRM_PLANE_NO_SCALING,
						DRM_PLANE_NO_SCALING, false, true);
	if (ret || !p->visible)
		return ret;
	/* Native v1 supports one full-screen linear RGB plane; no cropping. */
	if (p->src_x || p->src_y || p->crtc_x || p->crtc_y ||
	    p->src_w != (u32)c->mode.hdisplay << 16 ||
	    p->src_h != (u32)c->mode.vdisplay << 16 ||
	    p->crtc_w != c->mode.hdisplay || p->crtc_h != c->mode.vdisplay ||
	    p->fb->width != c->mode.hdisplay || p->fb->height != c->mode.vdisplay ||
	    p->fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    p->fb->pitches[0] < p->fb->width * 4)
		return -EINVAL;
	obj = drm_gem_fb_get_obj(p->fb, 0);
	extent = (u64)p->fb->pitches[0] * (p->fb->height - 1) +
		 (u64)p->fb->width * 4 + p->fb->offsets[0];
	return obj && extent <= obj->size ? 0 : -EINVAL;
}

static void plane_update(struct drm_plane *plane, struct drm_atomic_state *state) {}
static const struct drm_plane_helper_funcs plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = plane_check,
	.atomic_update = plane_update,
	.atomic_disable = plane_update,
};
static const struct drm_plane_funcs plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int present(struct m3_dcpext_kms *kms, struct drm_atomic_state *state,
		   bool enable)
{
	struct drm_crtc_state *c = drm_atomic_get_new_crtc_state(state, &kms->crtc);
	struct drm_connector_state *connector;
	struct drm_framebuffer *fb = NULL;
	struct sg_table *sgt = NULL;
	bool copied = false;
	struct m3_dcpext_native_mode mode;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES], data[DRM_FORMAT_MAX_PLANES];
	u64 generation = 0;
	int ret;

	if (enable) {
		struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, &kms->plane);
		if (!p)
			return -EINVAL;
		fb = p->fb;
	}
	if (fb) {
		struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
		struct scatterlist *sg;
		unsigned int i;
		u64 bytes = (u64)fb->pitches[0] * fb->height;
		bool direct = !(fb->offsets[0] & (PAGE_SIZE - 1)) &&
			bytes <= (u64)M3_DCPEXT_MAX_PITCH * M3_DCPEXT_MAX_HEIGHT &&
			fb->offsets[0] <= obj->size && bytes <= obj->size - fb->offsets[0];
		if (direct) {
			sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(obj));
			if (IS_ERR(sgt)) return PTR_ERR(sgt);
			for_each_sgtable_sg(sgt, sg, i)
				if ((sg_phys(sg) & (PAGE_SIZE - 1)) ||
				    sg_phys(sg) >= BIT_ULL(42) ||
				    sg->length > BIT_ULL(42) - sg_phys(sg)) direct = false;
			if (!direct) sgt = NULL;
		}
		if (sgt) {
			/* The shmem/PRIME helper mapped this attachment BIDIRECTIONAL. */
			dma_sync_sgtable_for_device(drm_dev_dma_dev(fb->dev), sgt, DMA_BIDIRECTIONAL);
			goto mapped;
		}
		ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
		if (ret)
			return ret;
		ret = drm_gem_fb_vmap(fb, map, data);
		if (ret)
			goto end_access;
		copied = true;
	}
mapped:
	mutex_lock(&kms->owner_lock);
	if (kms->detached) {
		ret = -ENODEV;
		goto unlock_owner;
	}
	mutex_lock(kms->route_lock);
	ret = kms->failed ? -EIO : 0;
	if (!ret && enable) {
		connector = drm_atomic_get_new_connector_state(state, &kms->connector.base);
		ret = connector ? m3_dcpext_connector_commit_native(&kms->connector,
				connector, &c->mode, &mode, &generation) : -EINVAL;
	}
	if (!ret)
		ret = kms->ops->present(kms->cookie, enable ? &mode : NULL, generation,
				copied ? &data[0] : NULL, fb ? fb->pitches[0] : 0,
				fb && fb->format->format == DRM_FORMAT_XRGB8888, fb, sgt);
	if (!ret && enable)
		kms->completed++;
	if (ret && ret != -ESTALE && ret != -ENODEV) {
		kms->failed = true;
		drm_err(&kms->drm, "external commit failed %d; scanout retained by owner\n", ret);
	}
	mutex_unlock(kms->route_lock);
unlock_owner:
	mutex_unlock(&kms->owner_lock);
	if (copied)
		drm_gem_fb_vunmap(fb, map);
end_access:
	if (fb && !sgt)
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	return ret;
}

static void finish_event(struct drm_crtc *crtc, struct drm_atomic_state *state, int ret)
{
	struct drm_crtc_state *c = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event = c->event;
	unsigned long flags;

	if (!ret && c->active)
		drm_crtc_handle_vblank(crtc);
	if (!event)
		return;
	c->event = NULL;
	if (ret) {
		/* Cancel the userspace event without claiming a presentation, but
		 * release the helper dependency and signal an error on its fence.
		 * Otherwise a rejected commit blocks every later modeset/unplug. */
		if (event->base.fence) {
			dma_fence_set_error(event->base.fence, ret);
			dma_fence_signal(event->base.fence);
		}
		if (event->base.completion) {
			complete_all(event->base.completion);
			event->base.completion_release(event->base.completion);
		}
		drm_event_cancel_free(crtc->dev, &event->base);
		return;
	}
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	if (drm_atomic_get_new_crtc_state(state, crtc)->active)
		finish_event(crtc, state, present(to_kms(crtc->dev), state, true));
}
static void crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	const struct drm_vblank_crtc_config config = { .offdelay_ms = 0 };
	drm_crtc_vblank_on_config(crtc, &config);
}
static void crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	int ret = present(to_kms(crtc->dev), state, false);
	/* An active-to-active modeset completes after its new surface, not
	 * after temporarily powering off the old timing. */
	if (!drm_atomic_get_new_crtc_state(state, crtc)->active)
		finish_event(crtc, state, ret);
	drm_crtc_vblank_off(crtc);
}
static int enable_vblank(struct drm_crtc *crtc) { return 0; }
static void disable_vblank(struct drm_crtc *crtc) {}
static const struct drm_crtc_helper_funcs crtc_helpers = {
	.atomic_enable = crtc_enable,
	.atomic_disable = crtc_disable,
	.atomic_flush = crtc_flush,
};
static const struct drm_crtc_funcs crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = enable_vblank,
	.disable_vblank = disable_vblank,
};
static const struct drm_encoder_funcs encoder_funcs = { .destroy = drm_encoder_cleanup };

static int atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct m3_dcpext_kms *kms = to_kms(drm);
	struct drm_crtc_state *crtc;
	struct drm_connector_state *connector;
	int ret;

	crtc = drm_atomic_get_crtc_state(state, &kms->crtc);
	if (IS_ERR(crtc))
		return PTR_ERR(crtc);
	if (READ_ONCE(kms->failed) && crtc->active)
		return -EIO;
	connector = drm_atomic_get_connector_state(state, &kms->connector.base);
	if (IS_ERR(connector))
		return PTR_ERR(connector);
	ret = drm_atomic_add_affected_planes(state, &kms->crtc);
	return ret ?: drm_atomic_helper_check(drm, state);
}
static void commit_tail(struct drm_atomic_state *state)
{
	drm_atomic_helper_commit_modeset_disables(state->dev, state);
	drm_atomic_helper_commit_modeset_enables(state->dev, state);
	drm_atomic_helper_commit_planes(state->dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	/* present already waited for firmware completion; no timer vblank. */
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(state->dev, state);
}
static const struct drm_mode_config_funcs mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static const struct drm_mode_config_helper_funcs mode_helpers = { .atomic_commit_tail = commit_tail };
DEFINE_DRM_GEM_FOPS(kms_fops);
static const struct drm_driver kms_driver = {
	/* The common DRM syncobj implementation supports imported render fences
	 * without requiring a render engine on the display device. */
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
			   DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.fops = &kms_fops,
	.name = "m3-dcpext",
	.desc = "J514S native HDMI",
	.major = 0, .minor = 1,
};

struct m3_dcpext_kms *m3_dcpext_kms_create(struct device *dev,
		const struct m3_dcpext_kms_ops *ops, void *cookie, struct mutex *route_lock, int connector_type)
{
	static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct m3_dcpext_kms *kms;
	struct drm_device *drm;
	int ret;

	if (!ops || !ops->present || !route_lock)
		return ERR_PTR(-EINVAL);
	kms = devm_drm_dev_alloc(dev, &kms_driver, struct m3_dcpext_kms, drm);
	if (IS_ERR(kms))
		return kms;
	drm = &kms->drm;
	kms->ops = ops;
	kms->cookie = cookie;
	kms->route_lock = route_lock;
	mutex_init(&kms->owner_lock);
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ERR_PTR(ret);
	drm->mode_config.min_width = drm->mode_config.min_height = 1;
	drm->mode_config.max_width = M3_DCPEXT_MAX_WIDTH;
	drm->mode_config.max_height = M3_DCPEXT_MAX_HEIGHT;
	drm->mode_config.funcs = &mode_funcs;
	drm->mode_config.helper_private = &mode_helpers;
	ret = drm_universal_plane_init(drm, &kms->plane, 1, &plane_funcs, formats,
			ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(&kms->plane, &plane_helpers);
	ret = drm_crtc_init_with_planes(drm, &kms->crtc, &kms->plane, NULL, &crtc_funcs, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(&kms->crtc, &crtc_helpers);
	ret = drm_encoder_init(drm, &kms->encoder, &encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ERR_PTR(ret);
	kms->encoder.possible_crtcs = 1;
	ret = m3_dcpext_connector_init(drm, &kms->connector, &kms->encoder, connector_type);
	if (ret)
		return ERR_PTR(ret);
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ERR_PTR(ret);
	drm_mode_config_reset(drm);
	return kms;
}
struct m3_dcpext_connector *m3_dcpext_kms_connector(struct m3_dcpext_kms *kms)
{
	return &kms->connector;
}
int m3_dcpext_kms_register(struct m3_dcpext_kms *kms)
{
	return drm_dev_register(&kms->drm, 0);
}
void m3_dcpext_kms_unplug(struct m3_dcpext_kms *kms)
{
	drm_dev_unplug(&kms->drm);
	drm_atomic_helper_shutdown(&kms->drm);
	/* Shutdown performs the final disable while the owner is still alive.
	 * Serialize with any late commit before severing callback references.
	 * Existing DRM/GEM file references may survive physical device removal.
	 */
	mutex_lock(&kms->owner_lock);
	kms->detached = true;
	kms->ops = NULL;
	kms->cookie = NULL;
	kms->route_lock = NULL;
	mutex_unlock(&kms->owner_lock);
}
MODULE_IMPORT_NS("DMA_BUF");
