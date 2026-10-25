#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the G17 build selector with a modelled image-identity result.

The shared fwid KUnit tests check the actual SHA-256 normalization. This host
check ensures OS metadata cannot admit a different image or veto a known one.
"""

from pathlib import Path
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "drivers/gpu/drm/asahi"


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HEADER = r'''
#![allow(dead_code)]
type Result<T = ()> = std::result::Result<T, i32>;
const ENODEV: i32 = -19;
macro_rules! dev_info { ($($t:tt)*) => { () }; }
macro_rules! dev_err { ($($t:tt)*) => { () }; }
struct AsahiDrmDevice {
    version: Option<[u32; 3]>,
    image: u32,
    checked: std::cell::RefCell<Vec<u32>>,
}
mod fw {
    pub struct Variant(pub u32);
    pub mod variant {
        use super::Variant;
        pub struct Build {
            pub os_version: [u32; 3],
            pub name: &'static str,
            pub image: u32,
            pub variant: &'static Variant,
        }
        pub enum Firmware { Fixed(&'static Variant), Identified(&'static [Build]) }
    }
}
struct Gpu;
impl Gpu {
    fn os_firmware_version(dev: &AsahiDrmDevice) -> Option<[u32; 3]> { dev.version }
    fn identify_image(dev: &AsahiDrmDevice, build: &'static fw::variant::Build) -> Result {
        dev.checked.borrow_mut().push(build.image);
        if dev.image == build.image { Ok(()) } else { Err(ENODEV) }
    }
'''

TESTS = r'''
}
static FIRST: fw::Variant = fw::Variant(1);
static SECOND: fw::Variant = fw::Variant(2);
static BUILDS: [fw::variant::Build; 2] = [
    fw::variant::Build { os_version: [26,6,2], name: "first", image: 1, variant: &FIRST },
    fw::variant::Build { os_version: [27,0,0], name: "second", image: 2, variant: &SECOND },
];
fn device(version: Option<[u32; 3]>, image: u32) -> AsahiDrmDevice {
    AsahiDrmDevice { version, image, checked: std::cell::RefCell::new(Vec::new()) }
}
fn main() {
    use fw::variant::Firmware;
    for version in [None, Some([26,6,2]), Some([26,7,0]), Some([99,0,0])] {
        let dev = device(version, 1);
        assert_eq!(Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS)).unwrap().0, 1);
    }
    let dev = device(Some([27,0,0]), 1);
    assert_eq!(Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS)).unwrap().0, 1);
    assert_eq!(*dev.checked.borrow(), [2,1], "the OS version only orders the candidates");
    let dev = device(Some([26,6,2]), 9);
    assert!(Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS)).is_err());
    assert_eq!(*dev.checked.borrow(), [1,2]);
    let dev = device(None, 9);
    assert_eq!(Gpu::firmware_variant(&dev, &Firmware::Fixed(&SECOND)).unwrap().0, 2);
    assert!(dev.checked.borrow().is_empty(), "fixed Neo admission is unchanged");
    println!("PASS G17 selection: exact image required, OS hint optional, fixed Neo unchanged");
}
'''


def main():
    source = (DRIVER / "g17/mod.rs").read_text()
    selector = method(source, "    fn firmware_variant(")
    with tempfile.TemporaryDirectory(prefix="asahi-firmware-selection-") as tmp:
        root = Path(tmp)
        (root / "main.rs").write_text(HEADER + selector + TESTS)
        binary = root / "test"
        subprocess.run(["rustc", "--edition=2021", "-Dwarnings", str(root / "main.rs"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
