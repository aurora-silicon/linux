#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production PCIE fault harness."""
from apif_mmio import run_tests, pcie_test

if __name__ == "__main__":
    run_tests([("pcie", pcie_test)])
