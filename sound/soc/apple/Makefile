# SPDX-License-Identifier: GPL-2.0-only

snd-soc-aop-y		:= aop_audio.o
obj-$(CONFIG_SND_SOC_APPLE_AOP_AUDIO)	+= snd-soc-aop.o

snd-soc-apple-mca-y	:= mca.o

obj-$(CONFIG_SND_SOC_APPLE_MCA)	+= snd-soc-apple-mca.o

snd-soc-macaudio-objs	:= macaudio.o
ifeq ($(CONFIG_SND_SOC_APPLE_MACAUDIO)$(CONFIG_KUNIT)$(CONFIG_OF_OVERLAY),yyy)
snd-soc-macaudio-objs	+= macaudio-kunit.dtbo.o
endif

obj-$(CONFIG_SND_SOC_APPLE_MACAUDIO)	+= snd-soc-macaudio.o
