# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_APPLE_CENTAURI_T6050_RECOVERY_KUNIT) += centauri_recovery.o

obj-$(CONFIG_APPLE_CENTAURI_T6050) += apple_centauri_t6050.o
apple_centauri_t6050-y := centauri_pci.o centauri_ipc.o
apple_centauri_t6050-$(CONFIG_APPLE_CENTAURI_T6050_WIFI) += centauri_wifi.o
ifneq ($(CONFIG_APPLE_CENTAURI_T6050_RECOVERY_KUNIT),y)
apple_centauri_t6050-$(CONFIG_APPLE_CENTAURI_T6050_WIFI) += centauri_recovery.o
endif