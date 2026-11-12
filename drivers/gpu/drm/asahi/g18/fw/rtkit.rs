// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 RTKit endpoint numbers and the two firmware messages the live boot sends.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.

pub(crate) const ENDPOINT_GFX_MESSAGES: u8 = 0x20;

pub(crate) const ENDPOINT_GFX_INTERRUPTS: u8 = 0x21;

const INITDATA_TAG: u64 = 0x0081_0000_0000_0000;

const INITDATA_ADDRESS_MASK: u64 = 0x0000_0fff_ffff_ffff;

const INITDATA_ADDRESS_PREFIX: u64 = 0xffff_f000_0000_0000;

/// `AGXArmFirmware::powerTransitionEnd(AGFIFirmwareRole, bool)` closes the GPU
/// power-on transition by sending this note to EACH firmware role.
///
/// KEXT_PROVEN, 25G83 @0xfffffe00088936a4. Both send sites are identical:
///
///     ldr   x0, [x19, #6616]  /  [x19, #6672]   ; the two role objects
///     ldr   x8, [obj + 0x8a8]                   ; sendAsyncNoteToFirmware
///     mov   x1, #0x89000000000000               ; tag 0x89, payload 0
///     mov   w2, #0x1                            ; gated=true
///     blraa x8, x16
///
/// MEASURED on hardware (m1n1 hypervisor capture): it is the LAST
/// EP-0x21 message of bring-up, sent once per role right after the initdata
/// handshake completes and 11.4 seconds before the first work doorbell:
///
///     57.95ms  role0 -> ep=0x21 0x0089000000000000
///     59.31ms  role1 -> ep=0x21 0x0089000000000000
///     ...
///   11425.12ms role0 -> ep=0x21 0x0083000000000000   <- first doorbell
///
/// This driver has never sent it. Its EP-0x21 vocabulary was 0x81 initdata,
/// 0x83 doorbell, 0x84 device control and 0x86 recovery inspection only.
const POWER_TRANSITION_END_TAG: u64 = 0x0089_0000_0000_0000;

/// Encode the role's power-transition-end note. The payload is zero in every
/// captured instance and the kext materialises the bare tag with a single
/// `movz x1, #0x89, lsl #48`.
pub(crate) const fn encode_power_transition_end() -> u64 {
    POWER_TRANSITION_END_TAG
}

/// Refusals made by the evidence-backed codec.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum WireError {
    UnexpectedEndpointSet,
    UnsupportedInterface,
    AddressOutOfRange,
    InvalidInitdataTag,
    InvalidRuntimeType,
    QueueIdOutOfRange,
    UnmodeledBits,
}

/// Encode the G17 init-data handoff exactly as the recovered host does.
///
/// The host instruction truncates to 44 bits. Linux instead rejects an
/// address unless it is already in low-44-bit or firmware-canonical form.
pub(crate) const fn encode_initdata_doorbell(firmware_va: u64) -> Result<u64, WireError> {
    let high = firmware_va & !INITDATA_ADDRESS_MASK;
    if high != 0 && high != INITDATA_ADDRESS_PREFIX {
        return Err(WireError::AddressOutOfRange);
    }

    Ok(INITDATA_TAG | (firmware_va & INITDATA_ADDRESS_MASK))
}
