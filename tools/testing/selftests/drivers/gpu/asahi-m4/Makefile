# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Build/install has no compiler requirement. CPU tests run through run_tests.
# GPU execution requires the explicit `make hardware` target.
.DEFAULT_GOAL := all
PYTHON ?= python3
TEST_PROGS := run.sh
TEST_FILES := $(wildcard *.py *.rs *.c) fixtures README

.PHONY: cpu hardware
cpu:
	$(PYTHON) $(CURDIR)/run-host-tests.py $(if $(ASAHI_M4_OUTPUT),--output $(ASAHI_M4_OUTPUT))

hardware:
	$(PYTHON) $(CURDIR)/run-hardware-tests.py --mesa "$(MESA)" $(if $(RENDER_NODE),--render-node "$(RENDER_NODE)") $(if $(ROUNDS),--rounds "$(ROUNDS)") $(if $(ASAHI_M4_OUTPUT),--output "$(ASAHI_M4_OUTPUT)")

include ../../../lib.mk
