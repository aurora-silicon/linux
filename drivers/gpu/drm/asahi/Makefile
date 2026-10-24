# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_DRM_ASAHI) += asahi.o
asahi-y := asahi_core.o sysfs.o g17/sync_input.o g17/sync_output.o g17/sync_scope.o g17/trace.o
CFLAGS_g17/trace.o := -I$(src)/g17
asahi-$(CONFIG_DRM_ASAHI_G16) += g16/glue.o
