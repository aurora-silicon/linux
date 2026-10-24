# SPDX-License-Identifier: GPL-2.0-only OR MIT

CFLAGS_trace.o = -I$(src)

appledrm-y := apple_drv.o apple_resume.o
appledrm-y += afk.o dcp.o dcp_backlight.o dptxep.o iomfb.o parser.o systemep.o
appledrm-$(CONFIG_DRM_APPLE_AUDIO) += audio.o
appledrm-$(CONFIG_DRM_APPLE_AUDIO) += av.o
appledrm-y += connector.o dcp-display-metadata.o dcp-180hz.o
appledrm-y += ibootep.o
appledrm-y += iomfb_v12_3.o
appledrm-y += iomfb_v13_3.o
appledrm-y += iomfb_v14_7.o dcp-property-store.o
appledrm-$(CONFIG_DRM_APPLE_DCP_T6030_EXT) += dcpext-t6030.o
ifdef CONFIG_DRM_APPLE_DCP_T6030_EXT
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcpext-t6030-test.o
endif
appledrm-y += iomfb_h17p.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += iomfb_h17p_test.o parser_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += iomfb_v14_7_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += apple_resume_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_external_resume_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_external_ready_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_external_announce_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_external_relink_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_dropped_swap_test.o
appledrm-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_panel_hold_test.o
appledrm-y += epic/dpavservep.o
appledrm-y += plane.o


appledrm-$(CONFIG_TRACING) += trace.o

obj-$(CONFIG_DRM_APPLE) += appledrm.o
obj-$(CONFIG_DRM_APPLE_KUNIT_TEST) += afk_test.o
obj-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp_backlight_test.o
obj-$(CONFIG_DRM_APPLE_KUNIT_TEST) += dcp-announce-test.o

obj-$(CONFIG_DRM_APPLE_KUNIT_TEST) += afk-service-test.o

obj-$(CONFIG_DRM_APPLE_PARSER_KUNIT_TEST) += afkv2-test.o
