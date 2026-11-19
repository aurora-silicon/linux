// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Fields copied by the native 26A428 compute-state writer (8bf16cc..8bf18a8).
//! This is a bounded state view, not a command-stream or ownership validator.
//! Host policy, context, pool, scheduler and completion fields stay with callers.

#![allow(dead_code)]

use crate::g18::fw::compute::{G18ComputeCommandInputs, G18ComputeRegisterInputs};

pub(crate) const G18_COMPUTE_STATE_BYTES: usize = 0x268;

pub(crate) struct G18ComputeState<'a>(&'a [u8; G18_COMPUTE_STATE_BYTES]);

impl<'a> G18ComputeState<'a> {
    pub(crate) fn parse(bytes: &'a [u8]) -> Option<Self> {
        Some(Self(bytes.try_into().ok()?))
    }

    fn q(&self, offset: usize) -> u64 {
        u64::from_le_bytes(self.0[offset..offset + 8].try_into().unwrap())
    }

    fn w(&self, offset: usize) -> u32 {
        u32::from_le_bytes(self.0[offset..offset + 4].try_into().unwrap())
    }

    fn flag(&self, offset: usize) -> u8 { self.0[offset] & 1 }

    /// Replace only the fields sourced from userspace compute state. This
    /// deliberately does not synthesize any native host-owned resource fields.
    pub(crate) fn apply(&self, c: &mut G18ComputeCommandInputs, r: &mut G18ComputeRegisterInputs) {
        c.d488 = self.q(0x08);
        c.d490 = self.q(0x10);
        c.d498 = self.q(0x18);
        c.d4a0 = self.q(0x20);
        c.d4a8 = self.q(0x28);
        c.d4b0 = self.w(0x30);
        c.d4b8 = self.q(0x38);
        c.d4c0 = self.w(0x40);
        c.d4d0 = self.w(0x50);
        c.d4e0 = self.q(0x60);
        c.d508 = self.flag(0xc4);
        c.d628 = self.0[0xd0];
        c.d629 = self.flag(0x249);
        c.d770 = self.flag(0x24c);
        c.d8b0 = self.flag(0x24e);
        r.program = [self.q(0x58), self.q(0), self.q(0x70), self.q(0x78), self.q(0x80), self.q(0x88)];
        r.aliases = [self.q(0x1f8), self.q(0x200), self.q(0x208), self.q(0x210), self.q(0x218), self.q(0x220)];
        r.d540 = self.flag(0x1f4);
        r.d548 = self.q(0xa8);
        r.d5d9 = self.flag(0x24f);
        r.d62a = self.flag(0x24a);
        r.d62b = self.flag(0x24b);
        r.d418 = self.w(0x260);
        r.d774 = self.w(0xa4);
        // The native 16-byte store starts at descriptor +0x5b8, not +0x5c0.
        r.d5c0 = self.w(0x230);
        r.d5c4 = self.w(0x234);
        r.d5c8 = self.w(0x238);
        r.d50c = self.w(0xc0);
        r.d510 = self.w(0xc8);
        r.d514 = self.w(0xcc);
    }
}
