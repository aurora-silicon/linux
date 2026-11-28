#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for M3 Extended shared per-client GPU fault recovery logic."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--static", action="store_true", help="link without guest libraries")
    parser.add_argument("--mutate", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    src = repo / "drivers/gpu/drm/asahi/m3/m3_recovery.rs"
    args.out.mkdir(parents=True, exist_ok=True)
    entry = src
    if args.mutate:
        # Negative control: drop the cumulative quarantine budget limit.
        text = src.read_text()
        target = "Some(total) => total <= self.limit,"
        if target not in text:
            raise RuntimeError("mutation target missing in m3_recovery.rs")
        g16_logic = (repo / "drivers/gpu/drm/asahi/g16/recovery/logic.rs").resolve()
        mutated = text.replace(
            '#[path = "../g16/recovery/logic.rs"]',
            f'#[path = "{g16_logic}"]',
        ).replace(target, "Some(_) => true,")
        entry = args.out / "m3_recovery_mut.rs"
        entry.write_text(mutated)
    binary = args.out.resolve() / "tests"
    flags = ["-C", "target-feature=+crt-static"] if args.static else []
    subprocess.run(
        ["rustc", "--test", "--edition=2021", *flags, str(entry), "-o", str(binary)],
        check=True,
    )
    return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
