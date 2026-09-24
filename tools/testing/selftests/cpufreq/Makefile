# SPDX-License-Identifier: GPL-2.0
all:

TEST_PROGS := main.sh
TEST_FILES := cpu.sh cpufreq.sh governor.sh module.sh special-tests.sh
EXTRA_CLEAN := cpufreq_selftest.dmesg_cpufreq.txt cpufreq_selftest.dmesg_full.txt cpufreq_selftest.txt

include ../lib.mk

clean:

# Source-backed host fixtures; these do not run the hardware cpufreq suite.
.PHONY: apple-host-tests
apple-host-tests:
	python3 $(CURDIR)/apple-soc-command-test.py
	python3 $(CURDIR)/apple-pmp-sample-test.py
	python3 $(CURDIR)/apple-pmp-passive-attach-test.py
	python3 $(CURDIR)/apple-pmp-passive-test.py
	python3 $(CURDIR)/apple-pmp-cooling-tiers-test.py
