#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Keep the stock transport builder identical to the reviewed integration base.

This checks the whole source (including run, adaptation and PP kicks), with only
its exported ops symbol renamed so the queued implementation can coexist.
"""
import hashlib
from pathlib import Path

source = (Path(__file__).resolve().parent.parent / 'avd-vp9-legacy.c').read_bytes()
source = source.replace(b'avd_vp9_legacy_fmt_ops =', b'avd_vp9_fmt_ops =')
# aurora-silicon/linux d826cca0dff6fffdc444e8f900821c424023c3d7, avd-vp9.c
assert hashlib.sha256(source).hexdigest() == "3123467aace91c04ac8b760007a0102c1d78b1d56b35d7881e1d2e4cd2db0677"
print("VP9 stock builder: source byte-identical to d826cca0 (export rename only) PASS")
