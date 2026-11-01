# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_DRM_ASAHI) += aurora.o
aurora-y := asahi_core.o fwid.o sysfs.o cooling.o g17/sync_input.o g17/sync_output.o g17/sync_scope.o g17/trace.o g17/clpc.o
CFLAGS_g17/trace.o := -I$(src)/g17

aurora-$(CONFIG_DRM_ASAHI_G15) += g15/glue.o

aurora-$(CONFIG_DRM_ASAHI_G16) += g16/glue.o
