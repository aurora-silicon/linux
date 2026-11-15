#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Prove the host parking fixture rejects USB-only teardown cutting a DP link."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import phy
from common import block, run_unit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    parts = phy.parking(args.source)
    production = parts["production"]
    original = block(production, "static int atcphy_usb3_power_off(")
    guard = "if (atcphy_modes[atcphy->mode].enable_dp_aux)\n\t\treturn 0;"
    assert original.count(guard) == 1
    mutant = original.replace(guard, "/* negative control: incorrectly cut a live DP link */")
    units = {
        "production": parts["prefix"] + production + parts["suffix"],
        "drop-dp-preservation": parts["prefix"] + production.replace(original, mutant, 1) + parts["suffix"],
    }
    run_unit("production", units["production"], args.output, (("power",),))
    rejected = False
    try:
        run_unit("drop-dp-preservation", units["drop-dp-preservation"], args.output, (("power",),))
    except subprocess.CalledProcessError as error:
        directory = args.output / "drop-dp-preservation"
        compile_log = (directory / "compile.log").read_text()
        case_log = (directory / "case-0.log").read_text()
        assert "error:" not in compile_log
        assert "p.mode == APPLE_ATCPHY_MODE_USB3_DP" in case_log
        assert error.returncode != 0
        rejected = True
    assert rejected, "the control must detect a display dropped by USB host teardown"
    result = {
        "production_passed": True,
        "dp_preservation_mutation_rejected": True,
        "production_body_extracted_unchanged": True,
        "translation_unit_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in units.items()},
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
