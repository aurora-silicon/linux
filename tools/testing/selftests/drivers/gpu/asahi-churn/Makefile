# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Hardware only, with the Vulkan loader and Mesa's Asahi driver: not in the
# selftests' TARGETS. See queue-churn.c.
CFLAGS += -O2 -Wall
LDLIBS += -lvulkan
TEST_GEN_PROGS := queue-churn
include ../../../lib.mk
