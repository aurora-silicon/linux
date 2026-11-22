#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build and run the PCIe-C MSI slice host test (apple_pciec_msi_slice())."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'msi-slice'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-o', str(exe),
                    str(HERE / 'pcie-apple-msi-slice-tests.c')], check=True)
    subprocess.run([str(exe)], check=True)
