# SPDX-License-Identifier: GPL-2.0-only OR MIT

CFLAGS_trace.o = -I$(src)

appledrm_neo-y := apple_drv.o
appledrm_neo-y += afk.o dcp.o dcp_backlight.o dptxep.o iomfb.o parser.o systemep.o
appledrm_neo-$(CONFIG_DRM_APPLE_NEO_AUDIO) += audio.o
appledrm_neo-$(CONFIG_DRM_APPLE_NEO_AUDIO) += av.o
appledrm_neo-y += connector.o
appledrm_neo-y += ibootep.o
appledrm_neo-y += iomfb_v12_3.o
appledrm_neo-y += iomfb_v13_3.o
appledrm_neo-y += iomfb_h17p.o
appledrm_neo-$(CONFIG_DRM_APPLE_NEO_KUNIT_TEST) += iomfb_h17p_test.o
appledrm_neo-y += epic/dpavservep.o
appledrm_neo-y += plane.o


appledrm_neo-$(CONFIG_TRACING) += trace.o

obj-$(CONFIG_DRM_APPLE_NEO) += appledrm_neo.o
obj-$(CONFIG_DRM_APPLE_NEO_KUNIT_TEST) += afk_test.o
obj-$(CONFIG_DRM_APPLE_NEO_KUNIT_TEST) += dcp_backlight_test.o
