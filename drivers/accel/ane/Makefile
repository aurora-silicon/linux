# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_DRM_ACCEL_ANE) += ane.o ane_t6021.o
obj-$(CONFIG_DRM_ACCEL_ANE_PM_KUNIT_TEST) += ane_pm_test.o
obj-$(CONFIG_DRM_ACCEL_ANE_H16_VALIDATION) += ane_h16_hwx.o ane_h16_preload.o ane_h16_load.o
obj-$(CONFIG_DRM_ACCEL_ANE_KUNIT_TEST) += ane_drv_test.o
obj-$(CONFIG_DRM_ACCEL_ANE_FW_KUNIT_TEST) += ane_fw_platform_test.o \
	ane_fw_protocol_test.o ane_h16_tasks_test.o ane_h16_execute_test.o \
	ane_fw_transport_test.o ane_h16_boot_test.o \
	ane_h16_pending_test.o ane_h16_hwx_test.o ane_h16_preload_test.o

ane-y := \
	ane_drv.o \
	ane_tm.o \
	ane_dart.o \
	ane_perf.o \
	ane_boost.o

ane_t6021-y := \
	ane_fw_platform.o \
	ane_h16_backend.o \
	ane_t6021_rtclient_main.o \
	ane_t6021_fwload.o \
	ane_t6021_boot.o
