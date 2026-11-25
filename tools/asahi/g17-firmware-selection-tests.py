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
// The device argument is dropped; the message and its arguments are still built.
macro_rules! dev_info { ($dev:expr, $($t:tt)*) => { { let _ = format!($($t)*); } }; }
macro_rules! dev_err { ($dev:expr, $($t:tt)*) => { { let _ = format!($($t)*); } }; }
struct AsahiDrmDevice {
    version: Option<[u32; 3]>,
    image: u32,
    checked: std::rc::Rc<std::cell::RefCell<Vec<u32>>>,
}
struct CoreLayout {
    cores: Option<(u32, u32)>,
}
struct Config {
    id: u32,
    core_layout: CoreLayout,
}
mod fwid {
    pub struct FirmwareBuild {
        pub name: &'static str,
        pub image: u32,
        pub text_sha256: [u8; 32],
    }
    // The loaded TEXT, modelled by its image number; records every image it is tried against.
    pub struct Measured {
        pub image: u32,
        pub checked: std::rc::Rc<std::cell::RefCell<Vec<u32>>>,
    }
    impl Measured {
        pub fn find(&mut self, builds: &'static [FirmwareBuild]) -> Option<&'static FirmwareBuild> {
            builds.iter().find(|build| {
                self.checked.borrow_mut().push(build.image);
                build.image == self.image
            })
        }
        pub fn identity(&mut self, _: impl IntoIterator<Item = &'static FirmwareBuild>) -> u32 {
            self.image
        }
    }
}
mod fw {
    pub struct Variant(pub u32);
    pub mod variant {
        use super::Variant;
        pub struct Build {
            pub os_version: [u32; 3],
            pub name: &'static str,
            pub images: &'static [crate::fwid::FirmwareBuild],
            pub variant: &'static Variant,
            pub config: Option<&'static crate::Config>,
        }
        pub enum Firmware { Fixed(&'static Variant), Identified(&'static [Build]) }
    }
}
struct Gpu;
impl Gpu {
    fn os_firmware_version(dev: &AsahiDrmDevice) -> Option<[u32; 3]> { dev.version }
    fn measure_text(dev: &AsahiDrmDevice) -> Result<fwid::Measured> {
        Ok(fwid::Measured { image: dev.image, checked: dev.checked.clone() })
    }
'''

TESTS = r'''
}
static IMAGE_1: [fwid::FirmwareBuild; 1] = [fwid::FirmwareBuild { name: "image 1", image: 1, text_sha256: [1; 32] }];
static IMAGE_2: [fwid::FirmwareBuild; 1] = [fwid::FirmwareBuild { name: "image 2", image: 2, text_sha256: [2; 32] }];
static FIRST: fw::Variant = fw::Variant(1);
static SECOND: fw::Variant = fw::Variant(2);
static OWN: Config = Config { id: 7, core_layout: CoreLayout { cores: None } };
static BUILDS: [fw::variant::Build; 2] = [
    fw::variant::Build { os_version: [26,6,2], name: "first", images: &IMAGE_1, variant: &FIRST, config: None },
    fw::variant::Build { os_version: [27,0,0], name: "second", images: &IMAGE_2, variant: &SECOND,
                         config: Some(&OWN) },
];
// One image admitted with the configurations of two core layouts, as T6041 25G83 is.
static FORTY: Config = Config { id: 40, core_layout: CoreLayout { cores: Some((4, 40)) } };
static THIRTY_TWO: Config = Config { id: 32, core_layout: CoreLayout { cores: Some((4, 32)) } };
static SHARED: [fw::variant::Build; 3] = [
    fw::variant::Build { os_version: [26,5,2], name: "other", images: &IMAGE_1, variant: &FIRST, config: None },
    fw::variant::Build { os_version: [26,6,2], name: "40", images: &IMAGE_2, variant: &SECOND,
                         config: Some(&FORTY) },
    fw::variant::Build { os_version: [26,6,2], name: "32", images: &IMAGE_2, variant: &SECOND,
                         config: Some(&THIRTY_TWO) },
];
const CORES: (u32, u32) = (4, 40);
fn device(version: Option<[u32; 3]>, image: u32) -> AsahiDrmDevice {
    AsahiDrmDevice { version, image, checked: Default::default() }
}
fn main() {
    use fw::variant::Firmware;
    for version in [None, Some([26,6,2]), Some([26,7,0]), Some([99,0,0])] {
        let dev = device(version, 1);
        let (variant, config) = Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS), CORES).unwrap();
        assert_eq!(variant.0, 1);
        assert!(config.is_none(), "a build without its own configuration keeps the probed one");
    }
    let dev = device(Some([27,0,0]), 1);
    assert_eq!(Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS), CORES).unwrap().0.0, 1);
    assert_eq!(*dev.checked.borrow(), [2,1], "the OS version only orders the candidates");
    let dev = device(None, 2);
    let (variant, config) = Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS), CORES).unwrap();
    assert_eq!((variant.0, config.map(|c| c.id)), (2, Some(7)), "the identified build's configuration");
    let dev = device(Some([26,6,2]), 9);
    assert!(Gpu::firmware_variant(&dev, &Firmware::Identified(&BUILDS), CORES).is_err());
    assert_eq!(*dev.checked.borrow(), [1,2]);
    let dev = device(None, 9);
    let (variant, config) = Gpu::firmware_variant(&dev, &Firmware::Fixed(&SECOND), CORES).unwrap();
    assert_eq!(variant.0, 2);
    assert!(config.is_none());
    assert!(dev.checked.borrow().is_empty(), "fixed Neo admission is unchanged");
    for (cores, expected) in [((4, 40), 40), ((4, 32), 32), ((4, 36), 40)] {
        for version in [None, Some([26,6,2])] {
            let dev = device(version, 2);
            let (variant, config) =
                Gpu::firmware_variant(&dev, &Firmware::Identified(&SHARED), cores).unwrap();
            assert_eq!((variant.0, config.map(|c| c.id)), (2, Some(expected)),
                       "the configuration of the probed core layout, else the first one");
        }
    }
    let dev = device(None, 1);
    let (variant, config) =
        Gpu::firmware_variant(&dev, &Firmware::Identified(&SHARED), (4, 32)).unwrap();
    assert_eq!(variant.0, 1);
    assert!(config.is_none(), "the core layout does not change which image is admitted");
    println!("PASS G17 selection: exact image required, OS hint optional, build configuration \
              follows the image and the probed core layout, fixed Neo unchanged");
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
