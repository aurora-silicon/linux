// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J713: each T8132 external DCP the boot loader handed over as a DisplayPort
 * output of the DRM device: a primary plane, a CRTC, an encoder and a
 * connector, whose state and modes are the DCP's sink reports. The external
 * DCPs are adopted outside the DRM device: these objects read as
 * disconnected until a DCP has a session and a sink.
 *
 * A modeset powers the display and sets the mode. A frame is scanned out
 * directly from its framebuffer where it can be (mapped in a SID0 window of
 * the DCP's display DART), else copied into a SID0 surface; either way it is
 * swapped in, its flip completed with the swap (D590).
 */
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <drm/drm_atomic.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "iomfb_m4.h"
#include "t8132_ext_drm.h"

/* Copied frames of imported framebuffers are read through their dma-buf. */
MODULE_IMPORT_NS("DMA_BUF");

/* The firmware's IDs of the CRTC's mode, from its atomic check. */
struct t8132_ext_crtc_state {
	struct drm_crtc_state base;
	struct iomfb_m4_ext_ids ids;
};

static struct t8132_ext_crtc_state *to_t8132_ext_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct t8132_ext_crtc_state, base);
}

#define T8132_EXT_MAX 2

struct t8132_ext_out {
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct platform_device *pdev;
	struct work_struct hotplug_work;
	/* The held flip event, sent with its swap's D590 or later (drm->event_lock) */
	struct drm_pending_vblank_event *event;
	struct delayed_work flip_work;
	bool first_frame;
	/* A frame was not shown, so the next one goes whole */
	bool frame_lost;
	/* The last framebuffers copied instead of scanned out, and why */
	struct drm_framebuffer *refused_fb[8];
	const char *refused_why[8];
	unsigned int refused_next;
};

static DEFINE_SPINLOCK(t8132_ext_lock);
/* Registered outputs, for t8132_ext_drm_hotplug() (t8132_ext_lock) */
static struct t8132_ext_out *t8132_ext_live[T8132_EXT_MAX];
static const u32 t8132_ext_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u64 t8132_ext_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/* The DCP's driver data once bound; an adopted external DCP is never unbound. */
static struct apple_dcp *t8132_ext_owner(struct t8132_ext_out *out)
{
	return device_is_bound(&out->pdev->dev) ? platform_get_drvdata(out->pdev) : NULL;
}

static int t8132_ext_plane_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;

	int ret;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	/* Frames go out from the framebuffer's origin. */
	if (!ret && ps->visible && (ps->src.x1 || ps->src.y1))
		ret = -EINVAL;
	return ret;
}

/*
 * Nothing to program here: the CRTC's flush presents the frame.
 * drm_atomic_helper_commit_planes() calls this for every plane it updates
 * or disables (there is no atomic_disable).
 */
static void t8132_ext_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
}

static const struct drm_plane_helper_funcs t8132_ext_plane_helpers = {
	.atomic_check = t8132_ext_plane_check,
	.atomic_update = t8132_ext_plane_update,
};

static const struct drm_plane_funcs t8132_ext_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* Send the held flip event now, if any. */
static void t8132_ext_send_event(struct t8132_ext_out *out)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&out->crtc.dev->event_lock, flags);
	event = out->event;
	out->event = NULL;
	if (event)
		drm_crtc_send_vblank_event(&out->crtc, event);
	spin_unlock_irqrestore(&out->crtc.dev->event_lock, flags);
}

static void t8132_ext_flip_work(struct work_struct *work)
{
	t8132_ext_send_event(container_of(to_delayed_work(work), struct t8132_ext_out,
					  flip_work));
}

static void t8132_ext_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	const struct drm_display_mode *mode = &crtc->state->mode;

	struct apple_dcp *owner = t8132_ext_owner(out);
	int ret;

	out->first_frame = false;
	out->frame_lost = true;
	ret = owner ? iomfb_m4_ext_enable(owner, &to_t8132_ext_crtc_state(crtc->state)->ids,
					  mode->hdisplay, mode->vdisplay) : -ENODEV;
	if (ret)
		dev_warn(&out->pdev->dev, "%s on: %ux%u@%d Hz failed: %d\n",
			 crtc->name, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode), ret);
	else
		dev_info(&out->pdev->dev, "%s on: %ux%u@%d Hz, %d kHz\n",
			 crtc->name, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			 mode->clock);
}

static void t8132_ext_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);

	struct apple_dcp *owner = t8132_ext_owner(out);
	int ret = 0;

	cancel_delayed_work_sync(&out->flip_work);
	t8132_ext_send_event(out);
	if (owner)
		ret = iomfb_m4_ext_blank(owner);
	dev_info(&out->pdev->dev, "%s off: %d\n", crtc->name, ret);
}

/*
 * Whether @sgt is exactly @size bytes of whole 16 KiB pages on the
 * CPU side: every entry with a page behind it (an imported, DMA-only table
 * may have none), offset and length in whole pages, the entries together
 * the object's pages, no more and no less. Before any CPU-side use of it.
 */
static bool t8132_ext_sgt_pages_ok(struct sg_table *sgt, size_t size)
{
	unsigned int want = size / PAGE_SIZE, n = 0, i;
	struct scatterlist *sg;

	if (!sgt || size % PAGE_SIZE || PAGE_SIZE != SZ_16K)
		return false;
	for_each_sgtable_sg(sgt, sg, i) {
		if (!sg_page(sg) || sg->offset % PAGE_SIZE || sg->length % PAGE_SIZE ||
		    sg->length / PAGE_SIZE > want - n)
			return false;
		n += sg->length / PAGE_SIZE;
	}
	return n == want;
}

/*
 * The 16 KiB pages behind a GEM DMA framebuffer, in order, for its SID0
 * window: an own buffer's from its sg table; an imported dma-buf's from the
 * sg table of its attachment to this device (CPU side, which the import
 * keeps mapped, and so its pages pinned, as long as the object lives).
 * Every DART page must be one 16 KiB page, 16 KiB aligned: any other
 * layout, or an entry without a page, is copied instead.
 */
static int t8132_ext_fb_pages(void *key, phys_addr_t **phys, unsigned int *count)
{
	struct drm_framebuffer *fb = key;
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_dma_object *dma;
	struct scatterlist *sg;
	struct sg_table *sgt;
	unsigned int n = 0, i, want;
	bool own;
	int ret = 0;

	if (!obj || obj->size % PAGE_SIZE || PAGE_SIZE != SZ_16K)
		return -EOPNOTSUPP;
	dma = to_drm_gem_dma_obj(obj);
	own = !drm_gem_is_imported(obj);
	sgt = own ? drm_gem_dma_get_sg_table(dma) : dma->sgt;
	if (IS_ERR_OR_NULL(sgt))
		return sgt ? PTR_ERR(sgt) : -EOPNOTSUPP;
	want = obj->size / PAGE_SIZE;
	if (!t8132_ext_sgt_pages_ok(sgt, obj->size)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	*phys = kcalloc(want, sizeof(**phys), GFP_KERNEL);
	if (!*phys) {
		ret = -ENOMEM;
		goto out;
	}
	/* Exactly the object's pages: no entry may go past them, none may be missing. */
	for_each_sgtable_sg(sgt, sg, i) {
		if (!sg_page(sg) || sg->offset % PAGE_SIZE || sg->length % PAGE_SIZE ||
		    sg->length / PAGE_SIZE > want - n) {
			ret = -EOPNOTSUPP;
			break;
		}
		for (unsigned int off = 0; off < sg->length; off += PAGE_SIZE)
			(*phys)[n++] = page_to_phys(sg_page(sg)) + sg->offset + off;
	}
	if (!ret && n != want)
		ret = -EOPNOTSUPP;
	if (ret) {
		kfree(*phys);
		*phys = NULL;
	} else {
		*count = n;
	}
out:
	if (own) {
		sg_free_table(sgt);
		kfree(sgt);
	}
	return ret;
}

static void t8132_ext_fb_get(void *key)
{
	drm_framebuffer_get(key);
}

static void t8132_ext_fb_put(void *key)
{
	drm_framebuffer_put(key);
}

static const char t8132_ext_pages_refused[] =
	"its pages are not 16 KiB pages each (or not all there)";

/* Say once per framebuffer (of the last few) why it is copied. */
static void t8132_ext_direct_refused(struct t8132_ext_out *out, struct drm_framebuffer *fb,
				     const char *why)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(out->refused_fb); i++)
		if (out->refused_fb[i] == fb && out->refused_why[i] == why)
			return;
	out->refused_fb[out->refused_next] = fb;
	out->refused_why[out->refused_next] = why;
	out->refused_next = (out->refused_next + 1) % ARRAY_SIZE(out->refused_fb);
	dev_dbg(&out->pdev->dev, "%s: framebuffer %u (%ux%u %p4cc, pitch %u, modifier %#llx, %s) copied, not scanned out: %s\n",
		out->crtc.name, fb->base.id, fb->width, fb->height, &fb->format->format,
		fb->pitches[0], fb->modifier,
		drm_gem_is_imported(drm_gem_fb_get_obj(fb, 0)) ? "imported" : "own", why);
}

/*
 * The frame scanned out directly from @fb: 2 once its swap is
 * submitted (the flip comes with its D590), -ENOSPC or -EOPNOTSUPP to copy
 * it instead, else the error (the frame lost). A linear XRGB8888 or
 * ARGB8888 GEM DMA buffer, own or imported, from its first byte, at the
 * mode's size, the pitch a multiple of 64 bytes, at most 32 MiB, and every
 * 16 KiB of it one 16 KiB page (t8132_ext_fb_pages()).
 *
 * An imported buffer is cleaned out of the CPU caches for the display every
 * frame, its fences already waited for: its exporter may map it cached
 * (aurora-g16 WRITEBACK objects), no importer can tell, PRIME maps it with
 * DMA_ATTR_SKIP_CPU_SYNC and the exporter has no end_cpu_access, while the
 * display reads memory uncached. A sync for the attached device (this one,
 * dma-noncoherent) on the import's own sg table does it, whatever window it is
 * in. The GPU's rendering fence is waited for before the commit gets here (the
 * plane's default GEM prepare_fb, then drm_atomic_helper_wait_for_fences() in
 * the commit tail, blocking or not).
 */
static int t8132_ext_present_direct(struct t8132_ext_out *out, struct apple_dcp *owner,
				    struct drm_framebuffer *fb, struct drm_gem_object *obj)
{
	const struct drm_display_mode *mode = &out->crtc.state->mode;
	struct iomfb_m4_ext_fb d = {
		.key = fb, .width = fb->width, .height = fb->height, .pitch = fb->pitches[0],
		.pages = t8132_ext_fb_pages, .get = t8132_ext_fb_get, .put = t8132_ext_fb_put,
	};
	const char *why = NULL;
	int ret;

	if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
		why = "not linear";
	else if (fb->format->format != DRM_FORMAT_XRGB8888 &&
		 fb->format->format != DRM_FORMAT_ARGB8888)
		why = "not XRGB8888 or ARGB8888";
	else if (fb->offsets[0])
		why = "not from its first byte";
	else if (fb->width != mode->hdisplay || fb->height != mode->vdisplay)
		why = "not the mode's size";
	else if (fb->pitches[0] % 64 || fb->pitches[0] < fb->width * 4)
		why = "the pitch is not a multiple of 64 bytes";
	else if ((u64)fb->pitches[0] * fb->height > SZ_32M || obj->size > SZ_32M)
		why = "over 32 MiB";
	if (why) {
		t8132_ext_direct_refused(out, fb, why);
		return -EOPNOTSUPP;
	}
	if (drm_gem_is_imported(obj)) {
		struct drm_gem_dma_object *dma = to_drm_gem_dma_obj(obj);

		if (!dma->sgt || !obj->import_attach) {
			t8132_ext_direct_refused(out, fb, "no import mapping to clean");
			return -EOPNOTSUPP;
		}
		/*
		 * Its pages first, every frame (a walk of its entries, a few
		 * microseconds; not cached per framebuffer, whose address may be
		 * a freed one's): the sync goes through each entry's page.
		 */
		if (!t8132_ext_sgt_pages_ok(dma->sgt, obj->size)) {
			t8132_ext_direct_refused(out, fb, t8132_ext_pages_refused);
			return -EOPNOTSUPP;
		}
		dma_sync_sgtable_for_device(obj->import_attach->dev, dma->sgt, DMA_TO_DEVICE);
	}
	ret = iomfb_m4_ext_present_direct(owner, &d);
	if (ret == -EOPNOTSUPP)
		t8132_ext_direct_refused(out, fb, t8132_ext_pages_refused);
	return ret ?: 2;
}

/*
 * Present the primary plane's new frame: scanned out directly where it can
 * be, else copied into the DCP and swapped in; 2 once a direct frame's swap
 * is submitted, 1 if the commit has no new frame (no plane state), so
 * nothing was tried. A copied framebuffer is read through its kernel
 * mapping (dma-buf vmap for an imported one) inside CPU access.
 */
static int t8132_ext_present(struct t8132_ext_out *out, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, &out->plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, &out->plane);
	struct apple_dcp *owner = t8132_ext_owner(out);
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct iosys_map map, data;
	struct drm_rect damage;
	struct dma_buf *buf;
	int ret;

	if (!new || !old)
		return 1;
	fb = new->fb;
	if (!owner || !fb || !new->visible)
		return -ENOLINK;
	if (out->frame_lost)
		damage = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	else if (!drm_atomic_helper_damage_merged(old, new, &damage))
		damage = DRM_RECT_INIT(0, 0, 0, 0);
	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj)
		return -EINVAL;
	/* Scanned out directly where it can be, else copied below. */
	ret = t8132_ext_present_direct(out, owner, fb, obj);
	if (ret != -ENOSPC && ret != -EOPNOTSUPP)
		return ret;
	if (!drm_gem_is_imported(obj)) {
		struct drm_gem_dma_object *dma = to_drm_gem_dma_obj(obj);

		if (!dma->vaddr)
			return -EINVAL;
		iosys_map_set_vaddr(&data, dma->vaddr + fb->offsets[0]);
		return iomfb_m4_ext_present(owner, &data, fb->pitches[0], &damage);
	}
	buf = obj->import_attach->dmabuf;
	ret = dma_buf_vmap_unlocked(buf, &map);
	if (ret)
		return ret;
	ret = dma_buf_begin_cpu_access(buf, DMA_FROM_DEVICE);
	if (!ret) {
		data = map;
		iosys_map_incr(&data, fb->offsets[0]);
		ret = iomfb_m4_ext_present(owner, &data, fb->pitches[0], &damage);
		dma_buf_end_cpu_access(buf, DMA_FROM_DEVICE);
	}
	dma_buf_vunmap_unlocked(buf, &map);
	return ret;
}

/* The commit's flip event as the one held (an older one is sent first). */
static struct drm_pending_vblank_event *t8132_ext_hold_event(struct t8132_ext_out *out,
							     struct drm_crtc_state *cs)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&out->crtc.dev->event_lock, flags);
	event = cs->event;
	cs->event = NULL;
	if (event) {
		if (out->event)
			drm_crtc_send_vblank_event(&out->crtc, out->event);
		out->event = event;
	}
	spin_unlock_irqrestore(&out->crtc.dev->event_lock, flags);
	return event;
}

static void t8132_ext_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	struct drm_framebuffer *fb = crtc->primary->state->fb;
	struct drm_pending_vblank_event *event;
	int ret;

	/*
	 * The event is held before the frame goes out: its swap's D590 may come
	 * at any time from then on (t8132_ext_drm_flip_done()).
	 */
	event = t8132_ext_hold_event(out, cs);
	ret = t8132_ext_present(out, state);
	/* Its damage is lost with it: whole frames until one is shown. */
	if (ret <= 0)
		out->frame_lost = !!ret;
	if (fb && !out->first_frame && (ret <= 0 || ret == 2)) {
		out->first_frame = true;
		dev_dbg(&out->pdev->dev, "%s: first frame %ux%u %p4cc, modifier %#llx: %d\n",
			crtc->name, fb->width, fb->height, &fb->format->format, fb->modifier, ret);
	} else if (ret < 0 && ret != -EAGAIN) {
		dev_info_ratelimited(&out->pdev->dev, "%s: frame not shown: %d\n",
				     crtc->name, ret);
	}
	/*
	 * A copied frame's swap has completed (D590): its flip now, else a
	 * frame later. A direct frame's comes with its D590 (or when its swap
	 * is given up).
	 */
	if (event && ret != 2)
		mod_delayed_work(system_wq, &out->flip_work, ret >= 0 ? 0 : msecs_to_jiffies(16));
}

/* A modeset only to a mode the sink offered, within the link. */
static int t8132_ext_crtc_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	struct apple_dcp *owner;
	int ret = drm_crtc_helper_atomic_check(crtc, state);

	if (ret || !cs->enable || !drm_atomic_crtc_needs_modeset(cs))
		return ret;
	owner = t8132_ext_owner(out);
	ret = owner ? iomfb_m4_ext_mode_ids(owner, &cs->mode,
					    &to_t8132_ext_crtc_state(cs)->ids) : -ENODEV;
	if (ret)
		drm_dbg_kms(crtc->dev, "%s: mode " DRM_MODE_FMT " refused: %d\n", crtc->name,
			    DRM_MODE_ARG(&cs->mode), ret);
	return ret ? -EINVAL : 0;
}

static const struct drm_crtc_helper_funcs t8132_ext_crtc_helpers = {
	.atomic_check = t8132_ext_crtc_check,
	.atomic_enable = t8132_ext_crtc_enable,
	.atomic_disable = t8132_ext_crtc_disable,
	.atomic_flush = t8132_ext_crtc_flush,
};

/* The last object freed: drm_mode_config_cleanup() destroys CRTCs after the rest. */
static void t8132_ext_crtc_destroy(struct drm_crtc *crtc)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);

	cancel_delayed_work_sync(&out->flip_work);
	cancel_work_sync(&out->hotplug_work);
	drm_crtc_cleanup(crtc);
	put_device(&out->pdev->dev);
	kfree(out);
}

static void t8132_ext_crtc_reset(struct drm_crtc *crtc)
{
	struct t8132_ext_crtc_state *state = kzalloc_obj(*state);

	if (crtc->state) {
		__drm_atomic_helper_crtc_destroy_state(crtc->state);
		kfree(to_t8132_ext_crtc_state(crtc->state));
		crtc->state = NULL;
	}
	if (state)
		__drm_atomic_helper_crtc_reset(crtc, &state->base);
}

static struct drm_crtc_state *t8132_ext_crtc_duplicate(struct drm_crtc *crtc)
{
	struct t8132_ext_crtc_state *state;

	if (WARN_ON(!crtc->state))
		return NULL;
	state = kzalloc_obj(*state);
	if (!state)
		return NULL;
	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);
	state->ids = to_t8132_ext_crtc_state(crtc->state)->ids;
	return &state->base;
}

static void t8132_ext_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_t8132_ext_crtc_state(state));
}

static const struct drm_crtc_funcs t8132_ext_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = t8132_ext_crtc_destroy,
	.reset = t8132_ext_crtc_reset,
	.atomic_duplicate_state = t8132_ext_crtc_duplicate,
	.atomic_destroy_state = t8132_ext_crtc_destroy_state,
};

static enum drm_connector_status t8132_ext_detect(struct drm_connector *connector,
						  bool force)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	struct apple_dcp *owner = t8132_ext_owner(out);

	return owner && iomfb_m4_ext_detect(owner) > 0 ? connector_status_connected :
							  connector_status_disconnected;
}

static int t8132_ext_get_modes(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	struct apple_dcp *owner = t8132_ext_owner(out);

	return owner ? iomfb_m4_ext_get_modes(owner, connector) : 0;
}

static void t8132_ext_hotplug_work(struct work_struct *work)
{
	struct t8132_ext_out *out = container_of(work, struct t8132_ext_out, hotplug_work);

	drm_kms_helper_connector_hotplug_event(&out->connector);
}

/* Registered: hotplug reports from the DCP are passed on from now. */
static int t8132_ext_late_register(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	unsigned long flags;
	int ret = -EBUSY;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++) {
		if (!t8132_ext_live[i]) {
			t8132_ext_live[i] = out;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
	return ret;
}

static void t8132_ext_early_unregister(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	unsigned long flags;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++)
		if (t8132_ext_live[i] == out)
			t8132_ext_live[i] = NULL;
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
	cancel_work_sync(&out->hotplug_work);
}

static const struct drm_connector_funcs t8132_ext_connector_funcs = {
	.detect = t8132_ext_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.late_register = t8132_ext_late_register,
	.early_unregister = t8132_ext_early_unregister,
};

static const struct drm_connector_helper_funcs t8132_ext_connector_helpers = {
	.get_modes = t8132_ext_get_modes,
};

bool t8132_ext_crtc(const struct drm_crtc *crtc)
{
	return crtc && crtc->funcs == &t8132_ext_crtc_funcs;
}

bool t8132_ext_connector(const struct drm_connector *connector)
{
	return connector && connector->funcs == &t8132_ext_connector_funcs;
}

bool t8132_ext_plane(const struct drm_plane *plane)
{
	return plane && plane->funcs == &t8132_ext_plane_funcs;
}

bool t8132_ext_drm_in_commit(struct drm_atomic_commit *state)
{
	struct drm_crtc_state *cs;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, cs, i)
		if (t8132_ext_crtc(crtc))
			return true;
	return false;
}

/*
 * Every flip of these CRTCs in @state, before its old planes are cleaned up:
 * a frame's flip comes with its swap's D590, or when the swap is given up
 * (5 s, or at once when the link goes), so 6 s bound it. Until then the
 * old framebuffer may still be scanned out: blocking commits return, and
 * DRM lets go of it, only after.
 */
void t8132_ext_drm_wait_flips(struct drm_atomic_commit *state)
{
	/*
	 * Called after drm_atomic_helper_commit_hw_done(): the CRTC states
	 * belong to the next commit by then. Only the commit references this
	 * state holds itself are used, as drm_atomic_helper_wait_for_flip_done().
	 */
	for (int i = 0; i < state->dev->mode_config.num_crtc; i++) {
		struct drm_crtc_commit *commit = state->crtcs[i].commit;
		struct drm_crtc *crtc = state->crtcs[i].ptr;

		if (!crtc || !commit || !t8132_ext_crtc(crtc))
			continue;
		if (!wait_for_completion_timeout(&commit->flip_done, msecs_to_jiffies(6000)))
			drm_err(crtc->dev, "[CRTC:%d:%s] flip not done in 6 s\n",
				crtc->base.id, crtc->name);
	}
}

/*
 * The swap of @dcpext's last direct frame completed (D590) or was
 * given up: its held flip event now. Any context but hard IRQ.
 */
void t8132_ext_drm_flip_done(struct device *dcpext)
{
	unsigned long flags;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++)
		if (t8132_ext_live[i] && &t8132_ext_live[i]->pdev->dev == dcpext)
			t8132_ext_send_event(t8132_ext_live[i]);
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
}

void t8132_ext_drm_hotplug(struct device *dcpext)
{
	unsigned long flags;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++)
		if (t8132_ext_live[i] && &t8132_ext_live[i]->pdev->dev == dcpext)
			schedule_work(&t8132_ext_live[i]->hotplug_work);
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
}

/*
 * One output, all of it or nothing: a failure unwinds what was initialised,
 * so that the DRM device goes on without that output.
 */
static int t8132_ext_out_init(struct drm_device *drm, struct platform_device *pdev)
{
	struct t8132_ext_out *out = kzalloc(sizeof(*out), GFP_KERNEL);
	int ret;

	if (!out)
		return -ENOMEM;
	/* Dropped with @out, by the CRTC's destroy. */
	out->pdev = to_platform_device(get_device(&pdev->dev));
	INIT_WORK(&out->hotplug_work, t8132_ext_hotplug_work);
	INIT_DELAYED_WORK(&out->flip_work, t8132_ext_flip_work);
	/* The CRTC this plane may use is the next one initialised. */
	ret = drm_universal_plane_init(drm, &out->plane, BIT(drm->mode_config.num_crtc),
				       &t8132_ext_plane_funcs, t8132_ext_formats,
				       ARRAY_SIZE(t8132_ext_formats), t8132_ext_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		goto err_free;
	drm_plane_helper_add(&out->plane, &t8132_ext_plane_helpers);
	drm_plane_enable_fb_damage_clips(&out->plane);
	/* Every plane of the device has a zpos, or none may: the panel's do. */
	ret = drm_plane_create_zpos_immutable_property(&out->plane, 0);
	/* Frames go out opaque, ARGB8888 ones too. */
	if (!ret)
		ret = drm_plane_create_blend_mode_property(&out->plane,
							   BIT(DRM_MODE_BLEND_PIXEL_NONE));
	if (ret)
		goto err_plane;
	ret = drm_crtc_init_with_planes(drm, &out->crtc, &out->plane, NULL,
					&t8132_ext_crtc_funcs, "dcpext-%s", dev_name(&pdev->dev));
	if (ret)
		goto err_plane;
	drm_crtc_helper_add(&out->crtc, &t8132_ext_crtc_helpers);
	ret = drm_simple_encoder_init(drm, &out->encoder, DRM_MODE_ENCODER_TMDS);
	if (ret)
		goto err_crtc;
	out->encoder.possible_crtcs = drm_crtc_mask(&out->crtc);
	ret = drm_connector_init(drm, &out->connector, &t8132_ext_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		goto err_encoder;
	drm_connector_helper_add(&out->connector, &t8132_ext_connector_helpers);
	out->connector.polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_attach_encoder(&out->connector, &out->encoder);
	if (ret)
		goto err_connector;
	dev_dbg(&pdev->dev, "DRM output: CRTC %u, connector %s\n",
		drm_crtc_index(&out->crtc), out->connector.name);
	return 0;

	/* Nothing of a partly initialised output is left for the DRM device. */
err_connector:
	drm_connector_cleanup(&out->connector);
err_encoder:
	drm_encoder_cleanup(&out->encoder);
err_crtc:
	drm_crtc_cleanup(&out->crtc);
err_plane:
	drm_plane_cleanup(&out->plane);
err_free:
	put_device(&pdev->dev);
	kfree(out);
	return ret;
}

/*
 * Called once per DRM device, before drm_mode_config_reset(). Not an error
 * for the DRM device if anything fails here: the panel goes on without.
 */
int t8132_ext_drm_init(struct drm_device *drm)
{
	struct device_node *np;
	unsigned int count = 0;
	int err;
	int ret = 0;

	if (!of_machine_is_compatible("apple,j713"))
		return 0;
	for_each_compatible_node(np, NULL, "apple,t8132-dcpext") {
		struct platform_device *pdev;
		u32 ready;

		/* Only a DCP the boot loader handed over can ever be adopted. */
		if (!of_device_is_available(np) || count >= T8132_EXT_MAX ||
		    of_property_read_u32(np, "apple,t8132-dcpext-firmware-ready", &ready) ||
		    ready != 1)
			continue;
		pdev = of_find_device_by_node(np);
		if (!pdev)
			continue;
		err = t8132_ext_out_init(drm, pdev);
		/* The panel and the other output go on without this one. */
		if (err)
			dev_err(&pdev->dev, "no DRM output: %d\n", err);
		else
			count++;
		put_device(&pdev->dev);
		ret = ret ?: err;
	}
	if (count || ret)
		drm_info(drm, "%u T8132 external output(s): %d\n", count, ret);
	return ret;
}
