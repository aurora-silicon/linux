/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_IOMFB_M4_H__
#define __APPLE_IOMFB_M4_H__

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kconfig.h>

struct apple_dcp;
struct device;
struct dentry;
struct drm_display_mode;
struct drm_framebuffer;
struct iomfb_m4;

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)

/* Probe is inert; start adopts firmware after the common component is pinned. */
int iomfb_m4_init(struct apple_dcp *dcp);
int iomfb_m4_start(struct apple_dcp *dcp, struct device *scanout);
int iomfb_m4_activate(struct apple_dcp *dcp);
int iomfb_m4_get_mode(struct apple_dcp *dcp, struct drm_display_mode *mode);
int iomfb_m4_health(struct apple_dcp *dcp);
int iomfb_m4_error(struct apple_dcp *dcp);

/* The caller has waited for the producer fence before publishing the DMA GEM. */
int iomfb_m4_present(struct apple_dcp *dcp, struct drm_framebuffer *fb,
		     dma_addr_t dva, size_t mapped);
int iomfb_m4_blank(struct apple_dcp *dcp);
int iomfb_m4_suspend(struct apple_dcp *dcp);
int iomfb_m4_resume(struct apple_dcp *dcp);
void iomfb_m4_configure_backlight(struct apple_dcp *dcp);
void iomfb_m4_quiesce(struct apple_dcp *dcp);
void iomfb_m4_debugfs_init(struct apple_dcp *dcp, struct dentry *root);

#else
static inline int iomfb_m4_init(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_start(struct apple_dcp *dcp, struct device *scanout)
{
	return -ENODEV;
}
static inline int iomfb_m4_activate(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_get_mode(struct apple_dcp *dcp, struct drm_display_mode *mode)
{
	return -ENODEV;
}
static inline int iomfb_m4_health(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_error(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_present(struct apple_dcp *dcp, struct drm_framebuffer *fb,
				  dma_addr_t dva, size_t mapped)
{
	return -ENODEV;
}
static inline int iomfb_m4_blank(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_suspend(struct apple_dcp *dcp) { return -ENODEV; }
static inline int iomfb_m4_resume(struct apple_dcp *dcp) { return -ENODEV; }
static inline void iomfb_m4_configure_backlight(struct apple_dcp *dcp) { }
static inline void iomfb_m4_quiesce(struct apple_dcp *dcp) { }
static inline void iomfb_m4_debugfs_init(struct apple_dcp *dcp, struct dentry *root) { }
#endif

#endif
