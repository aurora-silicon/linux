#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile complete kernel/Rust UAPI definitions; optionally compare real Mesa."""
import argparse
import json
import os
from pathlib import Path
from abi import c_layout, differences, rust_layout
from common import FIXTURES, kernel_root, output_dir, verify_fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, default=os.environ.get('ASAHI_M4_MESA'),
                        help='actual Mesa checkout; no vendored header substitute')
    parser.add_argument('--kernel', type=Path, default=kernel_root())
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    verify_fixtures()
    out = output_dir()
    bindings = FIXTURES / 'public-uapi.rs'
    header = args.kernel.resolve() / 'include/uapi/drm/asahi_drm.h'
    kernel = c_layout(header, bindings, out, 'kernel-layout')
    rust = rust_layout(bindings, out)
    errors = differences(kernel, rust)
    mesa = None
    if args.mesa:
        mesa = args.mesa.resolve() / 'include/drm-uapi/asahi_drm.h'
        errors += differences(kernel, c_layout(mesa, bindings, out, 'mesa-layout'))
    report = {'passed': not errors, 'scope': 'Compiled public UAPI structures, field offsets and constants; frozen params prefix plus exact known capacity-tail assertions; no parser or feature-semantic coverage',
              'kernel': str(header), 'bindings': str(bindings), 'comparisons': len(kernel),
              'mesa': str(mesa) if mesa else None,
              'mesa_comparison': 'performed' if mesa else 'not requested; set ASAHI_M4_MESA or --mesa',
              'errors': errors}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
