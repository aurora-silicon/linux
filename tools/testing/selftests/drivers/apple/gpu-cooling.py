#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production absolute-power cap suite on the host, without a GPU."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = root / "drivers/gpu/drm/asahi/cooling_test.c"
with tempfile.TemporaryDirectory(prefix="asahi-cooling-") as work:
    binary = str(Path(work) / "cap-tests")
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-DASAHI_HOST_TEST", str(source), "-o", binary], check=True)
    subprocess.run([binary], check=True)

    # Compile the exact typed firmware members independently of the kernel.
    power = root / "drivers/gpu/drm/asahi/g17/fw/power.rs"
    rust = Path(work) / "power.rs"
    rust.write_text(f'''#![allow(dead_code)]
#[path = "{power}"] mod power;
fn main() {{
    assert_eq!(core::mem::offset_of!(power::PowerState, ppm_target_mw), 0x12c);
    assert_eq!(core::mem::offset_of!(power::RegionCPowerG16S26A428, max_power_mw), 0x88);
    let mut p = power::PowerState {{ prefix: [0xa5; 0x128], ppm_enabled: 1,
        ppm_target_mw: 9750, suffix: [0x5a; 0x590] }};
    p.ppm_target_mw = 2387;
    let enabled = p.ppm_enabled;
    assert_eq!(enabled, 1);
    assert!(p.prefix.iter().all(|v| *v == 0xa5));
    assert!(p.suffix.iter().all(|v| *v == 0x5a));
    println!("PASS: separate typed PPM fields, canaries, unadmitted G16S 26A428 +0x88");
}}
''')
    subprocess.run([os.environ.get("RUSTC", "rustc"), "--edition=2021", str(rust),
                    "-o", binary], check=True)
    subprocess.run([binary], check=True)
