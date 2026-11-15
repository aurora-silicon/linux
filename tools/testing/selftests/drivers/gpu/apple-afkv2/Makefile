# SPDX-License-Identifier: GPL-2.0-only
HOSTCC ?= cc
export HOSTCC
export AFKV2_SOURCE_ROOT := $(abspath ../../../../../..)

TEST_PROGS := run-afkv2-tests.sh test-admission.py
TEST_FILES := codec.c owned-prefix.c owned-suffix.c extract-owned.py inline-edid.c extract-service.py extract-hpd.py extract-send.py extract-edid.py extract-ring.py extract-notify.py extract-compact.py compact.c host-include

include ../../../lib.mk
