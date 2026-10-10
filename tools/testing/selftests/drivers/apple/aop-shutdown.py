#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile/run the production RTKit release policy with faulting/drop peers."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]

def main():
    compiler = os.environ.get("RUSTC", "rustc")
    if shutil.which(compiler) is None:
        print("SKIP: rustc is required")
        return 4
    helper = ROOT / "drivers/soc/apple/aop_shutdown.rs"
    with tempfile.TemporaryDirectory(prefix="apple-aop-shutdown-") as directory:
        directory = Path(directory)
        source = directory / "fixtures.rs"
        binary = directory / "fixtures"
        source.write_text("#[path = " + json.dumps(str(helper), ensure_ascii=False)
                          + "]\nmod shutdown;\n")
        subprocess.run([compiler, "--edition=2021", "--test", str(source),
                        "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode

if __name__ == "__main__":
    raise SystemExit(main())
