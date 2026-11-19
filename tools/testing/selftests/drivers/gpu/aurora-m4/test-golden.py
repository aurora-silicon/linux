#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Entry point for immutable firmware encoder differential fixtures."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name('run-golden-tests.py')), run_name='__main__')
