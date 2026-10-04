// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! CPU-owned sticky VM failure reporting. No GPU mapping or firmware access.
use core::sync::atomic::{AtomicI32, Ordering};

pub(crate) const PARAM_GROUP_VM_STATUS: u32 = 1;
pub(crate) const FEATURE_VM_STATUS: u64 = 1 << 1;
pub(crate) const VM_STATUS_SIZE: usize = 16;

/// Shared by the VM and accepted jobs; never cleared by queue destruction or
/// successful work. A newly created VM receives a new status object.
pub(crate) struct VmStatus { error: AtomicI32 }
impl VmStatus {
    pub(crate) fn new() -> Self { Self { error: AtomicI32::new(0) } }
    pub(crate) fn record(&self, error: i32) {
        if error < 0 {
            let _ = self.error.compare_exchange(0, error, Ordering::AcqRel, Ordering::Acquire);
        }
    }
    pub(crate) fn get(&self) -> i32 { self.error.load(Ordering::Acquire) }
}

/// GET_PARAMS group 1 is an in/out request: vm_id, flags=0, error(out), pad=0.
/// Decode once, before lookup, so a racing userspace edit cannot change owner.
pub(crate) fn decode_request(bytes: &[u8; VM_STATUS_SIZE]) -> Option<u32> {
    let word = |i| u32::from_ne_bytes(bytes[i..i+4].try_into().unwrap());
    if word(4) != 0 || word(12) != 0 { return None; }
    Some(word(0))
}
pub(crate) fn encode_response(vm_id: u32, error: i32) -> [u8; VM_STATUS_SIZE] {
    let mut bytes = [0; VM_STATUS_SIZE];
    bytes[0..4].copy_from_slice(&vm_id.to_ne_bytes());
    bytes[8..12].copy_from_slice(&error.to_ne_bytes());
    bytes
}
