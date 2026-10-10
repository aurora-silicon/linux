#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Production J616s getter/identity/profile gates, with read-only IPC fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def main():
    compiler = os.environ.get("RUSTC", "rustc")
    if not shutil.which(compiler):
        print("SKIP: rustc required")
        return 4
    source = (ROOT / "sound/soc/apple/j616s_aop_jack.rs").read_text()
    first = source.index("fn get_property(")
    last = source.index("\n}\n", first) + 2
    getter = source[first:last]
    helper = ROOT / "sound/soc/apple/j616s_jack_profile.rs"
    rust = r'''
use std::cell::Cell;
#[path = HELPER]
mod profile;
use profile::Route;
type KVec<T> = Vec<T>;
type Result<T> = std::result::Result<T, i32>;
const EIO: i32 = -5;
struct EPICService;
trait AOP {
    fn epic_call_ret(&self, service: &EPICService, subtype: u16,
        request: &[u8], capacity: usize) -> Result<(u32, KVec<u8>)>;
}
GETTER
struct ReadOnly { route: Route, retcode: u32, reply: Vec<u8>, calls: Cell<u32> }
impl AOP for ReadOnly {
    fn epic_call_ret(&self, _: &EPICService, subtype: u16,
        request: &[u8], capacity: usize) -> Result<(u32, Vec<u8>)> {
        self.calls.set(self.calls.get() + 1);
        assert_eq!(subtype, 0x20);
        assert_eq!(request.len(), 52);
        assert_eq!(&request[8..12], &0xc3000004u32.to_le_bytes());
        assert_eq!(request, profile::get_request(self.route, 701));
        assert_eq!(capacity, 114); // One extra byte exposes a too-long reply.
        Ok((self.retcode, self.reply.clone()))
    }
}
fn framed(bytes: &[u8]) -> Vec<u8> {
    let mut reply = (bytes.len() as u32).to_le_bytes().to_vec();
    reply.extend_from_slice(bytes); reply
}
#[test]
fn getter_uses_exact_native_record_and_refuses_bad_replies_without_retry() {
    for route in [Route::Cout, Route::Cin] {
        let good = framed(route.profile());
        let mut wrong_length = good.clone(); wrong_length[0] = 108;
        let mut extra = good.clone(); extra.push(0);
        for (code, reply, accepted) in [
            (0, good.clone(), true), (1, good.clone(), false),
            (0, good[..112].to_vec(), false), (0, wrong_length, false),
            (0, extra, false), (0, vec![0; 3], false),
        ] {
            let ipc = ReadOnly { route, retcode: code, reply, calls: Cell::new(0) };
            assert_eq!(get_property(&ipc, &EPICService, route, 701, 109).is_ok(), accepted);
            assert_eq!(ipc.calls.get(), 1);
        }
    }
}
#[test]
fn both_profiles_are_exact_including_unknown_fields() {
    for route in [Route::Cout, Route::Cin] {
        let good = framed(route.profile());
        assert!(profile::profile_reply(route, &good));
        assert!(!profile::profile_reply(
            if route == Route::Cout { Route::Cin } else { Route::Cout }, &good));
        for byte in 4..good.len() {
            let mut changed = good.clone(); changed[byte] ^= 1;
            assert!(!profile::profile_reply(route, &changed));
        }
    }
}
#[test]
fn idle_is_required_and_state_payload_lengths_are_strict() {
    assert!(profile::idle_reply(&framed(b"eldi")));
    for bytes in [b"drwp", b" 1wp", b"nnur", b"0000"] {
        assert!(!profile::idle_reply(&framed(bytes)));
    }
    assert!(profile::property_data(&[0; 3], 0).is_none());
    assert!(profile::property_data(&[0; 4], usize::MAX).is_none());
}
#[test]
fn firmware_protocol_and_endpoint_are_independently_pinned() {
    let uuid = profile::FIRMWARE_UUID; let name = profile::FIRMWARE_NAME;
    assert!(profile::identity_matches(uuid, name, 12, 0x22));
    for version in [0, 11, 13] { assert!(!profile::identity_matches(uuid, name, version, 0x22)); }
    for endpoint in [0x20, 0x21, 0x23] { assert!(!profile::identity_matches(uuid, name, 12, endpoint)); }
    assert!(!profile::identity_matches(b"unknown", name, 12, 0x22));
    assert!(!profile::identity_matches(uuid, b"j700", 12, 0x22));
}
#[test]
fn getter_encoding_retains_native_fourcc_and_32_bit_slot_route_metadata() {
    for (route, device, channels, channel) in [
        (Route::Cout, b"tuoc", 2, 4), (Route::Cin, b" nic", 1, 5),
    ] {
        let request = profile::get_request(route, 200);
        assert_eq!(&request[..4], &[0; 4]);
        assert_eq!(&request[4..8], &[255; 4]);
        assert_eq!(&request[12..32], &[0; 20]);
        assert_eq!(&request[32..40], &0x30u64.to_le_bytes());
        assert_eq!(&request[40..44], device);
        assert_eq!(&request[44..48], &200u32.to_le_bytes());
        assert_eq!(&request[48..52], &1u32.to_le_bytes());
        assert_eq!(route.channels(), channels); assert_eq!(route.dma_channel(), channel);
    }
}
'''.replace("HELPER", json.dumps(str(helper))).replace("GETTER", getter)
    with tempfile.TemporaryDirectory(prefix="j616s-jack-profile-") as directory:
        directory = Path(directory)
        rs, binary = directory / "fixtures.rs", directory / "fixtures"
        rs.write_text(rust)
        subprocess.run([compiler, "--edition=2021", "--test", str(rs), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
