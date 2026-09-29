// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S native SID0 scanout. Explicit-load, fixed inherited panel mode only. */
#include <linux/dma-mapping.h>
#include <linux/backlight.h>
#include <linux/aperture.h>
#include <linux/of_address.h>
#include <linux/dma-buf.h>
#include <linux/debugfs.h>
#include <linux/iosys-map.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include "m3_dcp_native.h"
#include "m3_dcp_kms.h"
#include "m3_dcp_brightness.h"

/* Diagnostic history capacity, unrelated to submission or buffering limits. */
#define M3_PRESENT_HISTORY 256
struct m3_present_record {
	u64 sequence, submitted_ns, completed_ns;
	u32 framebuffer;
	s32 status;
};

struct m3_kms {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct m3_dcp_native *native;
	u32 brightness_nits;
	/* Keep scanout pinned independently of atomic state until real completion. */
	struct drm_framebuffer *active_fb, *failed_fb;
	struct mutex present_lock;
	bool failed;
	u64 completed;
	u64 swap_total_ns, swap_max_ns;
	spinlock_t history_lock;
	u64 history_count;
	struct m3_present_record history[M3_PRESENT_HISTORY];
};

static struct m3_kms *to_m3(struct drm_device *drm)
{
	return container_of(drm, struct m3_kms, drm);
}

static enum drm_mode_status m3_mode_valid(struct drm_crtc *crtc,
					const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &to_m3(crtc->dev)->mode) ? MODE_OK : MODE_BAD;
}

static int m3_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *c;
	struct drm_gem_dma_object *obj;
	u64 bytes;
	int ret;

	if (READ_ONCE(to_m3(plane->dev)->failed))
		return -EIO;
	if (!p->crtc)
		return 0;
	c = drm_atomic_get_crtc_state(state, p->crtc);
	if (IS_ERR(c))
		return PTR_ERR(c);
	ret = drm_atomic_helper_check_plane_state(p, c, DRM_PLANE_NO_SCALING,
						DRM_PLANE_NO_SCALING, false, true);
	if (ret || !p->visible)
		return ret;
	/* Only the qualified full-panel, uncompressed, single-plane layout. */
	if (p->src_x || p->src_y || p->crtc_x || p->crtc_y ||
	    p->src_w != 3024U << 16 || p->src_h != 1964U << 16 ||
	    p->crtc_w != 3024 || p->crtc_h != 1964 ||
	    p->fb->width != 3024 || p->fb->height != 1964 ||
	    p->fb->modifier != DRM_FORMAT_MOD_LINEAR || p->fb->offsets[0] ||
	    p->fb->pitches[0] < 12096 || (p->fb->pitches[0] & 63))
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(p->fb, 0);
	bytes = (u64)p->fb->pitches[0] * p->fb->height;
	if (!obj || bytes > obj->base.size || bytes > U32_MAX || !obj->dma_addr)
		return -EINVAL;
	return 0;
}

static void m3_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	/* One RPC contains the complete atomic state, submitted by CRTC flush. */
}

static const struct drm_plane_helper_funcs m3_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = m3_plane_check,
	.atomic_update = m3_plane_update,
	.atomic_disable = m3_plane_update,
};

static const struct drm_plane_funcs m3_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static void m3_surface(u8 *s, struct drm_framebuffer *fb)
{
	u32 stride = fb->pitches[0], bytes = stride * fb->height;

	memset(s, 0, 0x22c);
	/* DCP has no XRGB8. Premultiplied RGB over black preserves XRGB pixels. */
	s[2] = fb->format->format == DRM_FORMAT_XRGB8888;
	put_unaligned_le32(1, s + 3);
	put_unaligned_le32(1, s + 7);
	put_unaligned_le32(0x42475241, s + 0xb);
	s[0x13] = 13;
	s[0x14] = 12;
	put_unaligned_le32(stride, s + 0x15);
	put_unaligned_le16(1, s + 0x19);
	s[0x1b] = s[0x1c] = 1;
	put_unaligned_le32(fb->width, s + 0x21);
	put_unaligned_le32(fb->height, s + 0x25);
	put_unaligned_le32(bytes, s + 0x29);
	put_unaligned_le32(1, s + 0x35);
	put_unaligned_le64(1, s + 0x51);
	put_unaligned_le32(fb->width, s + 0x59);
	put_unaligned_le32(fb->height, s + 0x5d);
	put_unaligned_le32(stride, s + 0x69);
	put_unaligned_le32(bytes, s + 0x6d);
	put_unaligned_le16(4, s + 0x71);
	s[0x73] = s[0x74] = 1;
	put_unaligned_le64(1, s + 0x149);
}

static void m3_finish_event(struct drm_crtc *crtc, struct drm_atomic_state *state, bool success)
{
	struct drm_crtc_state *c = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event = c->event;
	unsigned long flags;

	c->event = NULL;
	if (!event)
		return;
	if (!success) {
		/* Release helper waiters without reporting a successful page flip. */
		if (event->base.fence) {
			dma_fence_set_error(event->base.fence, -EIO);
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

static bool m3_present_locked(struct m3_kms *m3, struct drm_framebuffer *fb)
{
	u8 surface[0x22c];
	dma_addr_t dva = 0;
	u64 start, elapsed;
	int ret;

	lockdep_assert_held(&m3->present_lock);
	if (m3->failed)
		return false;
	if (fb) {
		drm_framebuffer_get(fb);
		m3_surface(surface, fb);
		dva = drm_fb_dma_get_gem_obj(fb, 0)->dma_addr;
	}
	start = ktime_get_ns();
	ret = m3_dcp_native_swap(m3->native, fb ? surface : NULL, dva,
				fb ? fb->width : 0, fb ? fb->height : 0,
				fb ? m3->brightness_nits : 0);
	elapsed = ktime_get_ns() - start;
	/* Never make a timing reader wait on present_lock or copy scanout pixels. */
	spin_lock(&m3->history_lock);
	m3->history[m3->history_count % M3_PRESENT_HISTORY] =
		(struct m3_present_record) {
			.sequence = m3->history_count + 1,
			.submitted_ns = start,
			.completed_ns = start + elapsed,
			.framebuffer = fb ? fb->base.id : 0,
			.status = ret,
		};
	m3->history_count++;
	spin_unlock(&m3->history_lock);
	if (ret) {
		/* Either buffer may still be scanned out. Never unmap them on error. */
		m3->failed_fb = fb;
		WRITE_ONCE(m3->failed, true);
		drm_err(&m3->drm, "native DCP flip failed %d; buffers pinned until reboot\n", ret);
		return false;
	}
	if (m3->active_fb)
		drm_framebuffer_put(m3->active_fb);
	m3->active_fb = fb;
	m3->completed++;
	m3->swap_total_ns += elapsed;
	m3->swap_max_ns = max(m3->swap_max_ns, elapsed);
	if (m3->completed <= 3 || !(m3->completed % 600))
		drm_info(&m3->drm, "native KMS completed=%llu surface=%d imported=%d\n",
			 m3->completed, !!fb, fb && !!fb->obj[0]->import_attach);
	return true;
}

static bool m3_present(struct m3_kms *m3, struct drm_framebuffer *fb)
{
	bool success;

	mutex_lock(&m3->present_lock);
	success = m3_present_locked(m3, fb);
	mutex_unlock(&m3->present_lock);
	return success;
}

static int m3_backlight_update(struct backlight_device *backlight)
{
	struct m3_kms *m3 = bl_get_data(backlight);
	int ret = 0;

	/* Serialize with KMS and retain the active framebuffer through D589.
	 * Brightness must also work when an idle compositor submits no frames.
	 * An inactive CRTC retains the request until its next enabling swap.
	 */
	mutex_lock(&m3->present_lock);
	if (m3->failed) {
		ret = -EIO;
		goto out;
	}
	m3->brightness_nits = backlight_get_brightness(backlight);
	/* A repeated userspace request must repair firmware state as well. */
	m3_dcp_native_brightness_invalidate(m3->native);
	if (m3->active_fb && !m3_present_locked(m3, m3->active_fb))
		ret = -EIO;
out:
	mutex_unlock(&m3->present_lock);
	return ret;
}

static const struct backlight_ops m3_backlight_ops = {
	.update_status = m3_backlight_update,
};

static void m3_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct m3_kms *m3 = to_m3(crtc->dev);
	bool success;

	if (!drm_atomic_get_new_crtc_state(state, crtc)->active)
		return;
	success = m3_present(m3, drm_atomic_get_new_plane_state(state, &m3->plane)->fb);
	if (success)
		drm_crtc_handle_vblank(crtc);
	m3_finish_event(crtc, state, success);
}

static void m3_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	const struct drm_vblank_crtc_config config = { .offdelay_ms = 0 };

	/* Firmware may reset brightness while the panel is powered down. */
	m3_dcp_native_brightness_invalidate(to_m3(crtc->dev)->native);
	drm_crtc_vblank_on_config(crtc, &config);
}

static void m3_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	bool success = m3_present(to_m3(crtc->dev), NULL);

	if (success)
		drm_crtc_handle_vblank(crtc);
	m3_finish_event(crtc, state, success);
	drm_crtc_vblank_off(crtc);
}

static int m3_enable_vblank(struct drm_crtc *crtc) { return 0; }
static void m3_disable_vblank(struct drm_crtc *crtc) { }

static const struct drm_crtc_helper_funcs m3_crtc_helpers = {
	.mode_valid = m3_mode_valid,
	.atomic_enable = m3_crtc_enable,
	.atomic_disable = m3_crtc_disable,
	.atomic_flush = m3_crtc_flush,
};

static const struct drm_crtc_funcs m3_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = m3_enable_vblank,
	.disable_vblank = m3_disable_vblank,
};

static int m3_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev,
							 &to_m3(connector->dev)->mode);
	if (!mode)
		return 0;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_connector_status m3_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs m3_connector_helpers = {
	.get_modes = m3_get_modes,
};

static const struct drm_connector_funcs m3_connector_funcs = {
	.detect = m3_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs m3_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int m3_atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct m3_kms *m3 = to_m3(drm);
	struct drm_crtc_state *c;
	int ret;

	if (READ_ONCE(m3->failed))
		return -EIO;
	c = drm_atomic_get_crtc_state(state, &m3->crtc);
	if (IS_ERR(c))
		return PTR_ERR(c);
	ret = drm_atomic_add_affected_planes(state, &m3->crtc);
	return ret ?: drm_atomic_helper_check(drm, state);
}

static void m3_commit_tail(struct drm_atomic_state *state)
{
	/* Each flush waits for its matching D589 before releasing the previous FB.
	 * Waiting for an additional vblank here would stall an idle display.
	 */
	drm_atomic_helper_commit_modeset_disables(state->dev, state);
	drm_atomic_helper_commit_modeset_enables(state->dev, state);
	drm_atomic_helper_commit_planes(state->dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(state->dev, state);
}

static const struct drm_mode_config_funcs m3_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = m3_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static const struct drm_mode_config_helper_funcs m3_mode_helpers = {
	.atomic_commit_tail = m3_commit_tail,
};

static int m3_dumb_create(struct drm_file *file, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	/* llvmpipe pads its storage to 64-pixel tiles. The scanout framebuffer
	 * and plane still have to match the exact native panel extent. */
	if (args->bpp != 32 || !args->width || args->width > ALIGN(3024, 64) ||
	    !args->height || args->height > ALIGN(1964, 64))
		return -EINVAL;
	args->pitch = ALIGN(args->width * 4, 64);
	args->size = PAGE_ALIGN((u64)args->pitch * args->height);
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

DEFINE_DRM_GEM_DMA_FOPS(m3_fops);
static const struct drm_driver m3_driver = {
	/* DRM core owns syncobj timelines, including fences imported from the
	 * separate render device. Compositors query this display node too. */
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
			   DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(m3_dumb_create),
	.fops = &m3_fops,
	.name = "m3-dcp",
	.desc = "J514S native DCP scanout",
	.major = 0,
	.minor = 1,
};


static int m3_status(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m3_kms *m3 = to_m3(entry->dev);
	struct drm_framebuffer *fb;

	mutex_lock(&m3->present_lock);
	fb = m3->active_fb;
	seq_printf(seq, "completed %llu\nfailed %u\nactive_fb %u\nimported %u\n",
		   m3->completed, m3->failed, fb ? fb->base.id : 0,
		   fb && !!fb->obj[0]->import_attach);
	seq_printf(seq, "swap_total_us %llu\nswap_max_us %llu\n",
		   m3->swap_total_ns / 1000, m3->swap_max_ns / 1000);
	mutex_unlock(&m3->present_lock);
	return 0;
}

static int m3_analytics(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m3_kms *m3 = to_m3(entry->dev);

	m3_dcp_native_analytics_show(m3->native, seq);
	return 0;
}

static int m3_completion(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m3_kms *m3 = to_m3(entry->dev);

	m3_dcp_native_completion_show(m3->native, seq);
	return 0;
}

static int m3_present_history(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m3_kms *m3 = to_m3(entry->dev);
	struct m3_present_record *records;
	u64 count, first, i;

	records = kmalloc(sizeof(m3->history), GFP_KERNEL);
	if (!records)
		return -ENOMEM;
	spin_lock(&m3->history_lock);
	count = m3->history_count;
	memcpy(records, m3->history, sizeof(m3->history));
	spin_unlock(&m3->history_lock);
	seq_puts(seq, "sequence submitted_ns completed_ns framebuffer status\n");
	first = count > M3_PRESENT_HISTORY ? count - M3_PRESENT_HISTORY : 0;
	for (i = first; i < count; i++) {
		struct m3_present_record *r = &records[i % M3_PRESENT_HISTORY];

		seq_printf(seq, "%llu %llu %llu %u %d\n", r->sequence,
			   r->submitted_ns, r->completed_ns, r->framebuffer, r->status);
	}
	kfree(records);
	return 0;
}

struct m3_snapshot {
	size_t size;
	u8 bytes[];
};

static int m3_snapshot_open(struct inode *inode, struct file *file)
{
	struct m3_kms *m3 = inode->i_private;
	struct m3_snapshot *snapshot;
	struct drm_framebuffer *fb;
	struct dma_buf *dmabuf = NULL;
	struct iosys_map map = {};
	u32 bytes;
	int ret;

	ret = mutex_lock_interruptible(&m3->present_lock);
	if (ret)
		return ret;
	fb = m3->active_fb;
	if (m3->failed || !fb) {
		ret = -ENODATA;
		goto unlock;
	}
	bytes = fb->pitches[0] * fb->height;
	if (bytes > SZ_32M) {
		ret = -E2BIG;
		goto unlock;
	}
	snapshot = kvmalloc(sizeof(*snapshot) + 32 + bytes, GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto unlock;
	}
	/* Only this root-only diagnostic maps or reads imported GPU pixels. */
	if (fb->obj[0]->import_attach) {
		dmabuf = fb->obj[0]->import_attach->dmabuf;
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret)
			goto free;
		ret = dma_buf_vmap_unlocked(dmabuf, &map);
		if (ret) {
			dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
			goto free;
		}
	} else {
		ret = drm_gem_dma_vmap(drm_fb_dma_get_gem_obj(fb, 0), &map);
		if (ret)
			goto free;
	}
	snapshot->size = 32 + bytes;
	put_unaligned_le32(0x4246334d, snapshot->bytes); /* M3FB */
	put_unaligned_le32(fb->width, snapshot->bytes + 4);
	put_unaligned_le32(fb->height, snapshot->bytes + 8);
	put_unaligned_le32(fb->pitches[0], snapshot->bytes + 12);
	put_unaligned_le32(fb->format->format, snapshot->bytes + 16);
	put_unaligned_le32(fb->base.id, snapshot->bytes + 20);
	put_unaligned_le32(!!dmabuf, snapshot->bytes + 24);
	put_unaligned_le32(bytes, snapshot->bytes + 28);
	iosys_map_memcpy_from(snapshot->bytes + 32, &map, 0, bytes);
	if (dmabuf) {
		dma_buf_vunmap_unlocked(dmabuf, &map);
		ret = dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret)
			goto free;
	}
	file->private_data = snapshot;
	mutex_unlock(&m3->present_lock);
	return 0;
free:
	kvfree(snapshot);
unlock:
	mutex_unlock(&m3->present_lock);
	return ret;
}

static ssize_t m3_snapshot_read(struct file *file, char __user *data, size_t size, loff_t *pos)
{
	struct m3_snapshot *snapshot = file->private_data;

	return simple_read_from_buffer(data, size, pos, snapshot->bytes, snapshot->size);
}

static int m3_snapshot_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct file_operations m3_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = m3_snapshot_open,
	.read = m3_snapshot_read,
	.release = m3_snapshot_release,
	.llseek = default_llseek,
};

static void m3_panel_size(struct m3_kms *m3)
{
	struct device_node *dcp, *panel;
	u32 width = 0, height = 0;

	/* This firmware's DisplayAttributes omits image dimensions. As with the
	 * M1/M2 internal panels, use board data rather than inventing an EDID or
	 * encoding a desktop scale. Native scanout includes the notch region.
	 */
	dcp = of_find_node_by_path("dcp");
	panel = of_get_compatible_child(dcp, "apple,panel");
	of_node_put(dcp);
	if (panel) {
		of_property_read_u32(panel, "width-mm", &width);
		of_property_read_u32(panel, "height-mm", &height);
		of_node_put(panel);
	}
	if (!width || !height || width > 2000 || height > 2000) {
		drm_warn(&m3->drm, "panel physical size missing or invalid; update J514S device tree\n");
		return;
	}
	m3->mode.width_mm = width;
	m3->mode.height_mm = height;
	drm_info(&m3->drm, "panel physical size %ux%u mm from device tree\n", width, height);
}

int m3_dcp_kms_register(struct m3_dcp_native *native, bool takeover)
{
	static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct backlight_properties backlight_props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = M3_PANEL_MAX_NITS,
		.brightness = M3_PANEL_DEFAULT_NITS,
	};
	struct backlight_device *backlight;
	struct device_node *node;
	struct platform_device *pdev;
	struct m3_kms *m3;
	struct drm_device *drm;
	const void *timing;
	u32 size;
	int ret;

	node = of_find_node_by_path("disp0");
	if (!node || !of_device_is_available(node) ||
	    !of_device_is_compatible(node, "apple,t6030-display-diagnostics") ||
	    !of_property_read_bool(node, "apple,j514s-inherited-mappings")) {
		of_node_put(node);
		return -ENODEV;
	}
	pdev = of_find_device_by_node(node);
	if (!pdev) {
		of_node_put(node);
		return -ENODEV;
	}
	/* Native client and its device mappings remain pinned until reboot. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(42));
	if (!ret)
		ret = of_dma_configure(&pdev->dev, node, true);
	of_node_put(node);
	if (ret)
		return ret;
	m3 = devm_drm_dev_alloc(&pdev->dev, &m3_driver, struct m3_kms, drm);
	if (IS_ERR(m3))
		return PTR_ERR(m3);
	drm = &m3->drm;
	m3->native = native;
	m3->brightness_nits = M3_PANEL_DEFAULT_NITS;
	mutex_init(&m3->present_lock);
	spin_lock_init(&m3->history_lock);
	timing = m3_dcp_native_property(native, "PreferredTimingElements", &size);
	if (!timing)
		return -ENODATA;
	ret = m3_dcp_preferred_mode(timing, size, &m3->mode);
	kfree(timing);
	if (ret)
		return ret;
	m3_panel_size(m3);
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = drm->mode_config.max_width = 3024;
	drm->mode_config.min_height = drm->mode_config.max_height = 1964;
	drm->mode_config.funcs = &m3_mode_funcs;
	drm->mode_config.helper_private = &m3_mode_helpers;
	ret = drm_universal_plane_init(drm, &m3->plane, 1, &m3_plane_funcs, formats,
				       ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&m3->plane, &m3_plane_helpers);
	ret = drm_crtc_init_with_planes(drm, &m3->crtc, &m3->plane, NULL, &m3_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&m3->crtc, &m3_crtc_helpers);
	ret = drm_encoder_init(drm, &m3->encoder, &m3_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	m3->encoder.possible_crtcs = 1;
	ret = drm_connector_init(drm, &m3->connector, &m3_connector_funcs, DRM_MODE_CONNECTOR_eDP);
	if (ret)
		return ret;
	drm_connector_helper_add(&m3->connector, &m3_connector_helpers);
	ret = drm_connector_attach_encoder(&m3->connector, &m3->encoder);
	if (ret)
		return ret;
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	if (takeover) {
		struct resource fb;

		node = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
		ret = node ? of_address_to_resource(node, 0, &fb) : -ENODEV;
		of_node_put(node);
		if (ret)
			return ret;
		ret = aperture_remove_conflicting_devices(fb.start, resource_size(&fb), m3_driver.name);
		if (ret)
			return ret;
	}
	drm_debugfs_add_file(drm, "m3_status", m3_status, NULL);
	drm_debugfs_add_file(drm, "m3_analytics", m3_analytics, NULL);
	drm_debugfs_add_file(drm, "m3_completion", m3_completion, NULL);
	drm_debugfs_add_file(drm, "m3_present_history", m3_present_history, NULL);
	backlight = devm_backlight_device_register(&pdev->dev, "apple-panel-bl",
						 &pdev->dev, m3, &m3_backlight_ops,
						 &backlight_props);
	if (IS_ERR(backlight))
		return PTR_ERR(backlight);
	ret = drm_dev_register(drm, 0);
	if (!ret)
		debugfs_create_file("m3_scanout", 0400, drm->debugfs_root, m3, &m3_snapshot_fops);
	if (!ret)
		drm_info(drm, "M3 native KMS ready: %ux%u at %d Hz, SID0 DMA scanout\n",
			 m3->mode.hdisplay, m3->mode.vdisplay, drm_mode_vrefresh(&m3->mode));
	return ret;
}

MODULE_IMPORT_NS("DMA_BUF");
