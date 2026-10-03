# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_APPLE_DOCKCHANNEL) += apple-dockchannel.o
apple-dockchannel-y = dockchannel.o

obj-$(CONFIG_APPLE_MAILBOX) += apple-mailbox.o
apple-mailbox-y = mailbox.o

obj-$(CONFIG_APPLE_PMGR_MISC)	+= apple-pmgr-misc.o

obj-$(CONFIG_APPLE_RTKIT) += apple-rtkit.o
apple-rtkit-y = rtkit.o rtkit-crashlog.o

obj-$(CONFIG_APPLE_RTKIT_HELPER) += apple-rtkit-helper.o
apple-rtkit-helper-y = rtkit-helper.o

obj-$(CONFIG_APPLE_SART) += apple-sart.o
apple-sart-y = sart.o

obj-$(CONFIG_APPLE_TUNABLE) += apple-tunable.o
apple-tunable-y = tunable.o

obj-$(CONFIG_APPLE_AOP) += aop.o

obj-$(CONFIG_APPLE_SEP_FV_INTERFACE) += fv_shim.o

obj-$(CONFIG_APPLE_SEP) += apple-sep.o
apple-sep-y := sep.o hwrng_shim.o store_shim.o bio_shim.o sha_shim.o \
		crypto_shim.o sensor_shim.o p256_shim.o trusted_shim.o work_shim.o

obj-$(CONFIG_APPLE_PMP) += pmp.o
