"""Compile the driver's actual entropy methods with deterministic RNG stubs."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "drivers/soc/apple"


def method(file, name):
    source = (DRIVER / file).read_text()
    match = re.search(r"^    (?:pub\(crate\) )?fn " + name + r"\(", source, re.M)
    if match is None:
        raise AssertionError(f"Missing driver method {name}")
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


@unittest.skipUnless(shutil.which("rustc"), "rustc required for driver method tests")
class EntropyTests(unittest.TestCase):
    def test_driver_entropy_methods(self):
        harness = r'''
use std::cell::{Cell, RefCell};
type Result<T> = std::result::Result<T, u8>;
const EIO: u8 = 5;
mod bio { pub const TOKEN_LEN: usize = 32; }
thread_local! { static MODE: Cell<u8> = const { Cell::new(0) }; }
mod shim {
    pub fn random_bytes(buf: &mut [u8]) -> super::Result<()> {
        super::MODE.with(|m| match m.get() {
            0 => { buf.fill(0x5a); Ok(()) },
            1 => { buf.fill(0); Ok(()) },
            2 => Err(4),
            _ => { buf.fill(0); *buf.last_mut().unwrap() = 1; Ok(()) },
        })
    }
}
struct SepData { words: RefCell<Vec<Result<u32>>>, registered: Cell<bool> }
impl SepData {
    fn get_entropy_word(&self) -> Result<u32> { self.words.borrow_mut().remove(0) }
    fn register_hwrng(&self) -> Result<()> { self.registered.set(true); Ok(()) }
METHODS
}
fn device(words: Vec<Result<u32>>) -> SepData {
    SepData { words: RefCell::new(words), registered: Cell::new(false) }
}
#[test] fn token_success() {
    MODE.with(|m| m.set(0));
    assert_eq!(device(vec![]).mint_token_bytes(), Some([0x5a; 32]));
}
#[test] fn token_zero_rejected() {
    MODE.with(|m| m.set(1));
    assert!(device(vec![]).mint_token_bytes().is_none());
}
#[test] fn token_rng_error_rejected() {
    MODE.with(|m| m.set(2));
    assert!(device(vec![]).mint_token_bytes().is_none());
}
#[test] fn token_last_byte_counts() {
    MODE.with(|m| m.set(3));
    assert_eq!(device(vec![]).mint_token_bytes().unwrap()[31], 1);
}
#[test] fn trusted_key_random_success() {
    MODE.with(|m| m.set(0));
    let mut b = [0; 32];
    assert_eq!(device(vec![]).sep_random(&mut b), Ok(()));
    assert_eq!(b, [0x5a; 32]);
}
#[test] fn trusted_key_zero_rejected() {
    MODE.with(|m| m.set(1));
    assert_eq!(device(vec![]).sep_random(&mut [0; 32]), Err(EIO));
}
#[test] fn trusted_key_error_propagated() {
    MODE.with(|m| m.set(2));
    assert_eq!(device(vec![]).sep_random(&mut [0; 32]), Err(4));
}
#[test] fn zero_hwrng_not_registered() {
    let d = device(vec![Ok(0); 4]);
    assert_eq!(d.survey_hwrng(), Err(EIO)); assert!(!d.registered.get());
}
#[test] fn answered_hwrng_registered() {
    let d = device(vec![Ok(0), Ok(1), Ok(0), Ok(0)]);
    assert_eq!(d.survey_hwrng(), Ok(())); assert!(d.registered.get());
}
#[test] fn failed_hwrng_not_registered() {
    let d = device(vec![Ok(1), Err(4)]);
    assert_eq!(d.survey_hwrng(), Err(4)); assert!(!d.registered.get());
}
'''
        methods = "\n".join([method("sbio.rs", "mint_token_bytes"),
                             method("sbio.rs", "sep_random"),
                             method("sep.rs", "survey_hwrng")])
        with tempfile.TemporaryDirectory(prefix="sep-entropy-test-") as directory:
            source = Path(directory) / "entropy.rs"
            binary = Path(directory) / "entropy-tests"
            source.write_text(harness.replace("METHODS", methods))
            subprocess.run(["rustc", "--edition=2021", "--test", str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
