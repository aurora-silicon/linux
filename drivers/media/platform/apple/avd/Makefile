# SPDX-License-Identifier: GPL-2.0-only

apple-avd-y := avd-drv.o avd-v4l2.o avd-hw.o avd-pm.o avd-h264.o \
	avd-h264-parse.o avd-vp9.o \
	avd-hevc.o avd-av1.o avd-av1-entropymode.o avd-queue.o avd-backend-open.o \
	avd-backend-apple.o avd-backend-queue.o avd-backend-kopsia.o
apple-avd-$(CONFIG_VIDEO_APPLE_AVD_KUNIT_TEST) += avd-h264-test.o \
	avd-queue-test.o avd-pm-test.o avd-hevc-test.o avd-job-test.o \
	avd-fmt-test.o avd-av1-test.o avd-v4l2-test.o avd-vp9-test.o
obj-$(CONFIG_VIDEO_APPLE_AVD) += apple-avd.o
