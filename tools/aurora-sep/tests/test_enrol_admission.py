# SPDX-License-Identifier: MIT
"""Execute the kernel's enrol request and persistence admission functions."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


APPLE = Path(__file__).resolve().parents[3] / "drivers/soc/apple"


def function(file, name):
    source = (APPLE / file).read_text()
    match = re.search(r"^fn " + name + r"\(", source, re.M)
    if match is None:
        raise AssertionError(f"missing driver function {name}")
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#![allow(dead_code)]
type Error = i32;
type Result<T> = std::result::Result<T, Error>;
const EINVAL: Error = -22;
const ENXIO: Error = -6;
const EIO: Error = -5;
const ENOSPC: Error = -28;
const ENODATA: Error = -61;
const EROFS: Error = -30;
const PRIVATE_TYPE_CATACOMB_OWNER: u8 = 0xf2;
const PRIVATE_TYPE_CATACOMB_USER: u8 = 0xf3;
const PRIVATE_TYPE_CATACOMB_MASTER: u8 = 0xf4;

struct EnrolPrerequisites {
    lane: bool,
    live_count: Option<usize>,
    bag_bound: bool,
    catacomb_readable: [bool; 3],
    catacomb_writable: [bool; 3],
    keybag_writable: bool,
    host_store_capacity: bool,
}
const LABEL_LEN: usize = 128;
struct EnrolStart {
    flags: u32,
    account_uid: u32,
    finger: u32,
    reserved: u32,
    label: [u8; LABEL_LEN],
}

FUNCTIONS

fn ready() -> EnrolPrerequisites {
    EnrolPrerequisites {
        lane: true, live_count: Some(1), bag_bound: true,
        catacomb_readable: [true; 3], catacomb_writable: [true; 3],
        keybag_writable: true, host_store_capacity: true,
    }
}
fn request() -> EnrolStart {
    let mut r = EnrolStart {
        flags: 0, account_uid: 1000, finger: 7, reserved: 0,
        label: [0; LABEL_LEN],
    };
    r.label[..28].copy_from_slice(b"FP1-20261027-7-12345678-user");
    r
}

fn main() {
    assert!(admit_enrol(&ready()).is_ok());
    let mut p = ready(); p.lane = false; assert_eq!(admit_enrol(&p), Err(ENXIO));
    let mut p = ready(); p.live_count = None; assert_eq!(admit_enrol(&p), Err(EIO));
    let mut p = ready(); p.live_count = Some(3); assert_eq!(admit_enrol(&p), Err(ENOSPC));
    let mut p = ready(); p.bag_bound = false; assert_eq!(admit_enrol(&p), Err(ENODATA));
    for i in 0..3 {
        let mut p = ready(); p.catacomb_readable[i] = false;
        assert_eq!(admit_enrol(&p), Err(ENODATA));
        let mut p = ready(); p.catacomb_writable[i] = false;
        assert_eq!(admit_enrol(&p), Err(EROFS));
    }
    let mut p = ready(); p.keybag_writable = false; assert_eq!(admit_enrol(&p), Err(EROFS));
    let mut p = ready(); p.host_store_capacity = false; assert_eq!(admit_enrol(&p), Err(ENOSPC));

    assert!(catacomb_needed_for_enrol(PRIVATE_TYPE_CATACOMB_OWNER, Some(0)));
    assert!(!catacomb_needed_for_enrol(PRIVATE_TYPE_CATACOMB_USER, Some(0)));
    assert!(!catacomb_needed_for_enrol(PRIVATE_TYPE_CATACOMB_MASTER, Some(0)));
    assert!(catacomb_needed_for_enrol(PRIVATE_TYPE_CATACOMB_USER, Some(1)));

    assert!(validate_enrol_request(&request()).is_ok());
    let mut r = request(); r.account_uid = u32::MAX;
    assert_eq!(validate_enrol_request(&r), Err(EINVAL));
    let mut r = request(); r.finger = 0;
    assert_eq!(validate_enrol_request(&r), Err(EINVAL));
    let mut r = request(); r.finger = 11;
    assert_eq!(validate_enrol_request(&r), Err(EINVAL));
    let mut r = request(); r.finger = 6;
    assert_eq!(validate_enrol_request(&r), Err(EINVAL));
    let mut r = request(); r.label[24] = 0;
    assert_eq!(validate_enrol_request(&r), Err(EINVAL));
}
'''


class EnrolAdmissionTests(unittest.TestCase):
    def test_refused_cases_before_capture(self):
        methods = "\n".join([
            function("sbio.rs", "admit_enrol"),
            function("sbio.rs", "catacomb_needed_for_enrol"),
            function("bio.rs", "validate_enrol_request"),
        ])
        source = HARNESS.replace("FUNCTIONS", methods)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.rs").write_text(source)
            subprocess.run(["rustc", "--edition=2021", "test.rs", "-o", "test"],
                           cwd=path, check=True, capture_output=True, text=True)
            subprocess.run([str(path / "test")], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
