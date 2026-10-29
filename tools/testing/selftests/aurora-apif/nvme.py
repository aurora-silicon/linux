#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production NVME fault harness."""
from apif_mmio import run_tests, nvme_test

if __name__ == "__main__":
    run_tests([("nvme", nvme_test)])
