// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


#[cfg(test)]
use super::{
    decode_g17_firmware_variant, g17_nested_firmware_identity, FirmwareRole, G17FirmwareVariant,
};
#[cfg(not(test))]
use crate::g17_boot::{
    decode_g17_firmware_variant, g17_nested_firmware_identity, FirmwareRole, G17FirmwareVariant,
};

#[cfg(not(test))]
use core::slice;
#[cfg(not(test))]
use kernel::{
    c_str,
    io::{
        mem::{Mem, MemFlag},
        resource::Resource,
    },
    of,
    prelude::*,
};

const MAX_CONTAINER_SIZE: usize = 16 * 1024 * 1024;
const FTAB_HEADER_OFFSET: usize = 0x20;
const FTAB_TABLE_OFFSET: usize = 0x30;
const FTAB_ENTRY_SIZE: usize = 0x10;
const FTAB_ENTRY_COUNT: usize = 2;
const FTAB_TABLE_END: usize = 0x50;

const MACHO_HEADER_SIZE: usize = 0x20;
const MACHO_COMMAND_BYTES: usize = 0xb80;
const MACHO_COMMAND_END: usize = MACHO_HEADER_SIZE + MACHO_COMMAND_BYTES;

const MH_MAGIC_64: u32 = 0xfeed_facf;
const CPU_TYPE_ARM64: u32 = 0x0100_000c;
const CPU_SUBTYPE_ARM64E_LIB64: u32 = 0x8000_0002;
const MH_PRELOAD: u32 = 5;
const G17_MACHO_COMMAND_COUNT: u32 = 8;
const G17_MACHO_FLAGS: u32 = 0x0020_0001;

const LC_SEGMENT_64: u32 = 0x19;
const LC_SYMTAB: u32 = 0x2;
const LC_DYSYMTAB: u32 = 0xb;
const LC_UUID: u32 = 0x1b;
const LC_UNIXTHREAD: u32 = 0x5;

const G17_COMMANDS: [(u32, u32); 8] = [
    (LC_SEGMENT_64, 0x368),
    (LC_SEGMENT_64, 0x598),
    (LC_SEGMENT_64, 0x98),
    (LC_SEGMENT_64, 0x48),
    (LC_SYMTAB, 0x18),
    (LC_DYSYMTAB, 0x50),
    (LC_UUID, 0x18),
    (LC_UNIXTHREAD, 0x120),
];

const ARM_THREAD_STATE64: u32 = 6;
const ARM_THREAD_STATE64_COUNT: u32 = 68;
const G17_ENTRY_PC: u64 = 0xffff_fc00_0000_0000;

const SHA256_INITIAL: [u32; 8] = [
    0x6a09_e667,
    0xbb67_ae85,
    0x3c6e_f372,
    0xa54f_f53a,
    0x510e_527f,
    0x9b05_688c,
    0x1f83_d9ab,
    0x5be0_cd19,
];

const SHA256_K: [u32; 64] = [
    0x428a_2f98,
    0x7137_4491,
    0xb5c0_fbcf,
    0xe9b5_dba5,
    0x3956_c25b,
    0x59f1_11f1,
    0x923f_82a4,
    0xab1c_5ed5,
    0xd807_aa98,
    0x1283_5b01,
    0x2431_85be,
    0x550c_7dc3,
    0x72be_5d74,
    0x80de_b1fe,
    0x9bdc_06a7,
    0xc19b_f174,
    0xe49b_69c1,
    0xefbe_4786,
    0x0fc1_9dc6,
    0x240c_a1cc,
    0x2de9_2c6f,
    0x4a74_84aa,
    0x5cb0_a9dc,
    0x76f9_88da,
    0x983e_5152,
    0xa831_c66d,
    0xb003_27c8,
    0xbf59_7fc7,
    0xc6e0_0bf3,
    0xd5a7_9147,
    0x06ca_6351,
    0x1429_2967,
    0x27b7_0a85,
    0x2e1b_2138,
    0x4d2c_6dfc,
    0x5338_0d13,
    0x650a_7354,
    0x766a_0abb,
    0x81c2_c92e,
    0x9272_2c85,
    0xa2bf_e8a1,
    0xa81a_664b,
    0xc24b_8b70,
    0xc76c_51a3,
    0xd192_e819,
    0xd699_0624,
    0xf40e_3585,
    0x106a_a070,
    0x19a4_c116,
    0x1e37_6c08,
    0x2748_774c,
    0x34b0_bcb5,
    0x391c_0cb3,
    0x4ed8_aa4a,
    0x5b9c_ca4f,
    0x682e_6ff3,
    0x748f_82ee,
    0x78a5_636f,
    0x84c8_7814,
    0x8cc7_0208,
    0x90be_fffa,
    0xa450_6ceb,
    0xbef9_a3f7,
    0xc671_78f2,
];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17FirmwareError {
    UnknownVariant,
    ContainerTooLarge,
    ContainerTruncated,
    InvalidFtabHeader,
    InvalidFtabTable,
    InvalidFtabEntry,
    InvalidMachoHeader,
    InvalidMachoCommands,
    InvalidMachoThread,
    NestedSizeMismatch,
    NestedUuidMismatch,
    NestedHashMismatch,
    GfxInvalid,
    Gfx1Invalid,
}

/// Rejection reasons for the authoritative loaded-segment handoff.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17LoadedFirmwareError {
    UnsupportedVariant,
    /// The variant is known (G17P) but the corpus pins no loaded-segment
    /// identity for it: T8140 role images are iBoot-loaded carveouts and the
    /// firmware's own init-data version-word gate is the grounded identity
    /// check there, not an m1n1 loaded-pair handoff.
    NoPinnedLoadedIdentity,
    MissingGfx,
    MissingGfx1,
    InvalidGfx,
    InvalidGfx1,
    OverlappingPhysicalSegments,
}

#[derive(Debug, Copy, Clone)]
pub(crate) struct G17LoadedRoleObservation<'a> {
    pub(crate) role: FirmwareRole,
    pub(crate) uuid: &'a [u8],
    pub(crate) segment_names: &'a [u8],
    pub(crate) iovas: &'a [u64],
    pub(crate) remaps: &'a [u64],
    pub(crate) physical: &'a [u64],
    pub(crate) physical_sizes: &'a [u32],
    pub(crate) sizes: &'a [u32],
    pub(crate) flags: &'a [u32],
    pub(crate) text: &'a [u8],
}

#[derive(Debug, Copy, Clone)]
struct G17LoadedRoleIdentity {
    role: FirmwareRole,
    uuid: &'static [u8],
    iovas: [u64; 3],
    sizes: [u32; 3],
    flags: [u32; 3],
    text_sha256: [u8; 32],
}

const G17_LOADED_SEGMENT_NAMES: &[u8] = b"__TEXT\0__DATA\0__DATA_SHARED_RO\0";

const G17S_LOADED_GFX: G17LoadedRoleIdentity = G17LoadedRoleIdentity {
    role: FirmwareRole::Gfx,
    uuid: b"05236520-33F1-3D41-83CB-169FAAADBCA7\0",
    iovas: [
        0xffff_fc00_0000_0000,
        0xffff_fc00_0005_0000,
        0xffff_fc00_0010_c000,
    ],
    sizes: [0x5_0000, 0xb_c000, 0x8000],
    flags: [0x1, 0x10, 0x0],
    text_sha256: [
        0x55, 0x14, 0xe1, 0x38, 0x5e, 0x88, 0x2c, 0x5a, 0xcb, 0x37, 0x3d, 0xef, 0x5f, 0xbe, 0x87,
        0xb5, 0xd2, 0x18, 0x2d, 0xd1, 0x73, 0xee, 0xaf, 0x59, 0x3b, 0xb9, 0x1e, 0x35, 0x54, 0xd2,
        0x76, 0x28,
    ],
};

const G17S_LOADED_GFX1: G17LoadedRoleIdentity = G17LoadedRoleIdentity {
    role: FirmwareRole::Gfx1,
    uuid: b"0E98AFB8-0680-3B15-932E-910D0A1CCD78\0",
    iovas: [
        0xffff_fc00_0000_0000,
        0xffff_fc00_0005_4000,
        0xffff_fc00_0010_8000,
    ],
    sizes: [0x5_4000, 0xb_4000, 0x8000],
    flags: [0x1, 0x10, 0x0],
    text_sha256: [
        0x66, 0xcb, 0x87, 0x04, 0x78, 0xa5, 0x6b, 0xc9, 0x4d, 0x9d, 0x1c, 0x21, 0x0b, 0xd5, 0x1a,
        0xda, 0xe0, 0x5d, 0xa0, 0xd6, 0x78, 0x59, 0x97, 0xe0, 0xb4, 0xff, 0x42, 0xee, 0x2a, 0x61,
        0x6f, 0xbe,
    ],
};

/// Proof that both role images were validated for one variant in one call.
///
/// The fields are intentionally private. Only the atomic raw-container and
/// loaded-segment pair validators can construct this value outside tests, so a
/// static expected-identity lookup cannot accidentally clear the runtime gate.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17FirmwareIdentityAdmission {
    variant: G17FirmwareVariant,
}

impl G17FirmwareIdentityAdmission {
    pub(crate) const fn variant(self) -> G17FirmwareVariant {
        self.variant
    }

    #[cfg(test)]
    pub(super) const fn test_only(variant: G17FirmwareVariant) -> Self {
        Self { variant }
    }
}

#[derive(Debug, Copy, Clone)]
struct RoleFtabLayout {
    container_size: usize,
    entry_length: usize,
}

const fn role_layout(role: FirmwareRole) -> RoleFtabLayout {
    match role {
        FirmwareRole::Gfx => RoleFtabLayout {
            container_size: 0x23_3570,
            entry_length: 0x11_9a90,
        },
        FirmwareRole::Gfx1 => RoleFtabLayout {
            container_size: 0x22_b5a0,
            entry_length: 0x11_5aa8,
        },
    }
}

fn get_u32(data: &[u8], offset: usize) -> Option<u32> {
    let bytes: [u8; 4] = data.get(offset..offset.checked_add(4)?)?.try_into().ok()?;
    Some(u32::from_le_bytes(bytes))
}

fn get_u64(data: &[u8], offset: usize) -> Option<u64> {
    let bytes: [u8; 8] = data.get(offset..offset.checked_add(8)?)?.try_into().ok()?;
    Some(u64::from_le_bytes(bytes))
}

/// The FTAB entry tag for one variant, when the corpus pins one. G17P has no
/// pinned FTAB pair, so it selects nothing.
fn expected_tag(variant: G17FirmwareVariant) -> Result<[u8; 4], G17FirmwareError> {
    g17_nested_firmware_identity(FirmwareRole::Gfx, variant)
        .map(|identity| identity.entry_tag)
        .ok_or(G17FirmwareError::UnknownVariant)
}

fn select_nested<'a>(
    role: FirmwareRole,
    variant: G17FirmwareVariant,
    container: &'a [u8],
) -> Result<&'a [u8], G17FirmwareError> {
    if container.len() > MAX_CONTAINER_SIZE {
        return Err(G17FirmwareError::ContainerTooLarge);
    }
    if container.len() < FTAB_TABLE_END {
        return Err(G17FirmwareError::ContainerTruncated);
    }

    if container.get(FTAB_HEADER_OFFSET..FTAB_HEADER_OFFSET + 4) != Some(b"rkos")
        || container.get(FTAB_HEADER_OFFSET + 4..FTAB_HEADER_OFFSET + 8) != Some(b"ftab")
    {
        return Err(G17FirmwareError::InvalidFtabHeader);
    }
    if get_u32(container, FTAB_HEADER_OFFSET + 8) != Some(FTAB_ENTRY_COUNT as u32)
        || get_u32(container, FTAB_HEADER_OFFSET + 12) != Some(0)
    {
        return Err(G17FirmwareError::InvalidFtabTable);
    }

    let layout = role_layout(role);
    if container.len() != layout.container_size {
        return Err(G17FirmwareError::InvalidFtabTable);
    }

    let tags = [*b"a010", *b"a000"];
    let mut selected = None;
    let wanted = expected_tag(variant)?;
    for (index, tag) in tags.iter().enumerate() {
        let descriptor = FTAB_TABLE_OFFSET + index * FTAB_ENTRY_SIZE;
        let offset = FTAB_TABLE_END + index * layout.entry_length;
        let end = offset
            .checked_add(layout.entry_length)
            .ok_or(G17FirmwareError::InvalidFtabEntry)?;

        if container.get(descriptor..descriptor + 4) != Some(tag)
            || get_u32(container, descriptor + 4) != Some(offset as u32)
            || get_u32(container, descriptor + 8) != Some(layout.entry_length as u32)
            || get_u32(container, descriptor + 12) != Some(0)
            || offset % 8 != 0
            || end > container.len()
        {
            return Err(G17FirmwareError::InvalidFtabEntry);
        }
        if *tag == wanted {
            selected = container.get(offset..end);
        }
    }

    if FTAB_TABLE_END + FTAB_ENTRY_COUNT * layout.entry_length != container.len() {
        return Err(G17FirmwareError::InvalidFtabTable);
    }
    selected.ok_or(G17FirmwareError::InvalidFtabEntry)
}

fn validate_macho_structure(image: &[u8]) -> Result<[u8; 16], G17FirmwareError> {
    if image.len() < MACHO_COMMAND_END {
        return Err(G17FirmwareError::InvalidMachoHeader);
    }

    let expected_header = [
        MH_MAGIC_64,
        CPU_TYPE_ARM64,
        CPU_SUBTYPE_ARM64E_LIB64,
        MH_PRELOAD,
        G17_MACHO_COMMAND_COUNT,
        MACHO_COMMAND_BYTES as u32,
        G17_MACHO_FLAGS,
        0,
    ];
    for (index, expected) in expected_header.iter().enumerate() {
        if get_u32(image, index * 4) != Some(*expected) {
            return Err(G17FirmwareError::InvalidMachoHeader);
        }
    }

    let mut cursor = MACHO_HEADER_SIZE;
    let mut uuid = None;
    for (expected_cmd, expected_size) in G17_COMMANDS {
        let size = expected_size as usize;
        let end = cursor
            .checked_add(size)
            .ok_or(G17FirmwareError::InvalidMachoCommands)?;
        if size < 8
            || size % 8 != 0
            || end > MACHO_COMMAND_END
            || get_u32(image, cursor) != Some(expected_cmd)
            || get_u32(image, cursor + 4) != Some(expected_size)
        {
            return Err(G17FirmwareError::InvalidMachoCommands);
        }
        if expected_cmd == LC_UUID {
            let value: [u8; 16] = image
                .get(cursor + 8..cursor + 24)
                .ok_or(G17FirmwareError::InvalidMachoCommands)?
                .try_into()
                .map_err(|_| G17FirmwareError::InvalidMachoCommands)?;
            if uuid.replace(value).is_some() {
                return Err(G17FirmwareError::InvalidMachoCommands);
            }
        } else if expected_cmd == LC_UNIXTHREAD {
            if get_u32(image, cursor + 8) != Some(ARM_THREAD_STATE64)
                || get_u32(image, cursor + 12) != Some(ARM_THREAD_STATE64_COUNT)
                || get_u64(image, cursor + 0x108) != Some(0)
                || get_u64(image, cursor + 0x110) != Some(G17_ENTRY_PC)
                || get_u32(image, cursor + 0x118) != Some(0)
                || get_u32(image, cursor + 0x11c) != Some(0)
            {
                return Err(G17FirmwareError::InvalidMachoThread);
            }
        }
        cursor = end;
    }

    if cursor != MACHO_COMMAND_END {
        return Err(G17FirmwareError::InvalidMachoCommands);
    }
    uuid.ok_or(G17FirmwareError::InvalidMachoCommands)
}

fn sha256_compress(state: &mut [u32; 8], block: &[u8]) {
    debug_assert_eq!(block.len(), 64);
    let mut words = [0u32; 64];
    for (index, word) in words.iter_mut().take(16).enumerate() {
        let offset = index * 4;
        *word = u32::from_be_bytes([
            block[offset],
            block[offset + 1],
            block[offset + 2],
            block[offset + 3],
        ]);
    }
    for index in 16..64 {
        let s0 = words[index - 15].rotate_right(7)
            ^ words[index - 15].rotate_right(18)
            ^ (words[index - 15] >> 3);
        let s1 = words[index - 2].rotate_right(17)
            ^ words[index - 2].rotate_right(19)
            ^ (words[index - 2] >> 10);
        words[index] = words[index - 16]
            .wrapping_add(s0)
            .wrapping_add(words[index - 7])
            .wrapping_add(s1);
    }

    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = *state;
    for index in 0..64 {
        let sum1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let choose = (e & f) ^ ((!e) & g);
        let temp1 = h
            .wrapping_add(sum1)
            .wrapping_add(choose)
            .wrapping_add(SHA256_K[index])
            .wrapping_add(words[index]);
        let sum0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let majority = (a & b) ^ (a & c) ^ (b & c);
        let temp2 = sum0.wrapping_add(majority);

        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(temp1);
        d = c;
        c = b;
        b = a;
        a = temp1.wrapping_add(temp2);
    }

    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

fn sha256(data: &[u8]) -> [u8; 32] {
    let mut state = SHA256_INITIAL;
    let mut chunks = data.chunks_exact(64);
    for block in &mut chunks {
        sha256_compress(&mut state, block);
    }

    let remainder = chunks.remainder();
    let mut tail = [0u8; 128];
    tail[..remainder.len()].copy_from_slice(remainder);
    tail[remainder.len()] = 0x80;
    let tail_len = if remainder.len() < 56 { 64 } else { 128 };
    tail[tail_len - 8..tail_len].copy_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for block in tail[..tail_len].chunks_exact(64) {
        sha256_compress(&mut state, block);
    }

    let mut digest = [0u8; 32];
    for (index, word) in state.iter().enumerate() {
        digest[index * 4..index * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    digest
}

fn validate_loaded_role(
    observed: &G17LoadedRoleObservation<'_>,
    expected: &G17LoadedRoleIdentity,
) -> bool {
    if observed.role != expected.role
        || observed.uuid != expected.uuid
        || observed.segment_names != G17_LOADED_SEGMENT_NAMES
        || observed.iovas != expected.iovas
        || observed.sizes != expected.sizes
        || observed.flags != expected.flags
        || observed.remaps.len() != 3
        || observed.physical.len() != 3
        || observed.physical_sizes != expected.sizes
        || observed.remaps != observed.physical
        || observed.text.len() != expected.sizes[0] as usize
        || sha256(observed.text) != expected.text_sha256
    {
        return false;
    }

    for index in 0..3 {
        let size = expected.sizes[index] as u64;
        if observed.physical[index] & 0x3fff != 0
            || size & 0x3fff != 0
            || observed.physical[index].checked_add(size).is_none()
        {
            return false;
        }
    }

    true
}

fn role_segments_overlap(role: &G17LoadedRoleObservation<'_>) -> bool {
    for left_index in 0..3 {
        let left_start = role.physical[left_index];
        let Some(left_end) = left_start.checked_add(role.sizes[left_index] as u64) else {
            return true;
        };
        for right_index in left_index + 1..3 {
            let right_start = role.physical[right_index];
            let Some(right_end) = right_start.checked_add(role.sizes[right_index] as u64) else {
                return true;
            };
            if left_start < right_end && right_start < left_end {
                return true;
            }
        }
    }
    false
}

fn role_pairs_overlap(
    left: &G17LoadedRoleObservation<'_>,
    right: &G17LoadedRoleObservation<'_>,
) -> bool {
    for left_index in 0..3 {
        let left_start = left.physical[left_index];
        let Some(left_end) = left_start.checked_add(left.sizes[left_index] as u64) else {
            return true;
        };

        for right_index in 0..3 {
            let right_start = right.physical[right_index];
            let Some(right_end) = right_start.checked_add(right.sizes[right_index] as u64) else {
                return true;
            };
            if left_start < right_end && right_start < left_end {
                return true;
            }
        }
    }

    false
}

fn admit_g17_loaded_pair_against(
    raw_variant: u8,
    gfx: Option<&G17LoadedRoleObservation<'_>>,
    gfx1: Option<&G17LoadedRoleObservation<'_>>,
    expected_gfx: &G17LoadedRoleIdentity,
    expected_gfx1: &G17LoadedRoleIdentity,
) -> Result<G17FirmwareIdentityAdmission, G17LoadedFirmwareError> {
    if raw_variant == G17FirmwareVariant::G17P as u8 {
        return Err(G17LoadedFirmwareError::NoPinnedLoadedIdentity);
    }
    if raw_variant != G17FirmwareVariant::G17S as u8 {
        return Err(G17LoadedFirmwareError::UnsupportedVariant);
    }

    let gfx = gfx.ok_or(G17LoadedFirmwareError::MissingGfx)?;
    let gfx1 = gfx1.ok_or(G17LoadedFirmwareError::MissingGfx1)?;
    if !validate_loaded_role(gfx, expected_gfx) {
        return Err(G17LoadedFirmwareError::InvalidGfx);
    }
    if !validate_loaded_role(gfx1, expected_gfx1) {
        return Err(G17LoadedFirmwareError::InvalidGfx1);
    }
    if role_segments_overlap(gfx) || role_segments_overlap(gfx1) || role_pairs_overlap(gfx, gfx1) {
        return Err(G17LoadedFirmwareError::OverlappingPhysicalSegments);
    }

    Ok(G17FirmwareIdentityAdmission {
        variant: G17FirmwareVariant::G17S,
    })
}

pub(crate) fn admit_g17_loaded_pair(
    raw_variant: u8,
    gfx: Option<&G17LoadedRoleObservation<'_>>,
    gfx1: Option<&G17LoadedRoleObservation<'_>>,
) -> Result<G17FirmwareIdentityAdmission, G17LoadedFirmwareError> {
    admit_g17_loaded_pair_against(raw_variant, gfx, gfx1, &G17S_LOADED_GFX, &G17S_LOADED_GFX1)
}

#[cfg(not(test))]
#[derive(Debug)]
pub(crate) enum G17LoadedHandoffError {
    MissingNode,
    InvalidEnvelope,
    InvalidResources,
    MappingFailed,
    Admission(G17LoadedFirmwareError),
}

#[cfg(not(test))]
struct RoleReservedResources {
    text: Resource,
    physical: [u64; 3],
    sizes: [u32; 3],
}

#[cfg(not(test))]
fn read_role_resources(
    node: &of::Node,
    names: [&CStr; 3],
) -> Result<RoleReservedResources, G17LoadedHandoffError> {
    let text = node
        .reserved_mem_region_to_resource_byname(names[0])
        .map_err(|_| G17LoadedHandoffError::InvalidResources)?;
    let data = node
        .reserved_mem_region_to_resource_byname(names[1])
        .map_err(|_| G17LoadedHandoffError::InvalidResources)?;
    let shared_ro = node
        .reserved_mem_region_to_resource_byname(names[2])
        .map_err(|_| G17LoadedHandoffError::InvalidResources)?;

    let physical = [text.start(), data.start(), shared_ro.start()];
    let sizes = [
        text.size()
            .try_into()
            .map_err(|_| G17LoadedHandoffError::InvalidResources)?,
        data.size()
            .try_into()
            .map_err(|_| G17LoadedHandoffError::InvalidResources)?,
        shared_ro
            .size()
            .try_into()
            .map_err(|_| G17LoadedHandoffError::InvalidResources)?,
    ];

    Ok(RoleReservedResources {
        text,
        physical,
        sizes,
    })
}

#[cfg(not(test))]
fn unique_nonzero_phandles(phandles: &[u32]) -> bool {
    if phandles.len() != 6 {
        return false;
    }
    for (index, phandle) in phandles.iter().enumerate() {
        if *phandle == 0 || phandles[index + 1..].contains(phandle) {
            return false;
        }
    }
    true
}

#[cfg(not(test))]
pub(crate) fn admit_g17_loaded_pair_from_handoff(
    raw_variant: u8,
) -> Result<G17FirmwareIdentityAdmission, G17LoadedHandoffError> {
    const COMPATIBLE: &[u8] = b"apple,g17-firmware-handoff-v1\0";
    const MEMORY_REGION_NAMES: &[u8] = concat!(
        "gfx-fw-text\0gfx-fw-data\0gfx-fw-shared-ro\0",
        "gfx1-fw-text\0gfx1-fw-data\0gfx1-fw-shared-ro\0"
    )
    .as_bytes();

    if raw_variant == G17FirmwareVariant::G17P as u8 {
        // Known variant, but no loaded-pair identity exists to admit: T8140
        // role images are iBoot-loaded carveouts and m1n1 publishes no
        // g17-firmware-handoff node for them. The probe path for T8140 keys
        // firmware identity off the init-data version-word gate instead.
        return Err(G17LoadedHandoffError::Admission(
            G17LoadedFirmwareError::NoPinnedLoadedIdentity,
        ));
    }
    if raw_variant != G17FirmwareVariant::G17S as u8 {
        return Err(G17LoadedHandoffError::Admission(
            G17LoadedFirmwareError::UnsupportedVariant,
        ));
    }

    let chosen = of::chosen().ok_or(G17LoadedHandoffError::MissingNode)?;
    let mut handoff = None;
    for node in chosen.children() {
        let matches = node
            .get_property::<KVec<u8>>(c_str!("compatible"))
            .map(|value| value.as_slice() == COMPATIBLE)
            .unwrap_or(false);
        if matches {
            if handoff.replace(node).is_some() {
                return Err(G17LoadedHandoffError::InvalidEnvelope);
            }
        }
    }
    let handoff = handoff.ok_or(G17LoadedHandoffError::MissingNode)?;

    let compatible: KVec<u8> = handoff
        .get_property(c_str!("compatible"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let version: u32 = handoff
        .get_property(c_str!("apple,handoff-version"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let memory_region_names: KVec<u8> = handoff
        .get_property(c_str!("memory-region-names"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let memory_regions: KVec<u32> = handoff
        .get_property(c_str!("memory-region"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    if compatible.as_slice() != COMPATIBLE
        || version != 1
        || memory_region_names.as_slice() != MEMORY_REGION_NAMES
        || !unique_nonzero_phandles(&memory_regions)
    {
        return Err(G17LoadedHandoffError::InvalidEnvelope);
    }

    let gfx_uuid: KVec<u8> = handoff
        .get_property(c_str!("apple,gfx-firmware-uuid"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx_names: KVec<u8> = handoff
        .get_property(c_str!("apple,gfx-segment-names"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx_iovas: KVec<u64> = handoff
        .get_property(c_str!("apple,gfx-segment-iovas"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx_remaps: KVec<u64> = handoff
        .get_property(c_str!("apple,gfx-segment-remaps"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx_sizes: KVec<u32> = handoff
        .get_property(c_str!("apple,gfx-segment-sizes"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx_flags: KVec<u32> = handoff
        .get_property(c_str!("apple,gfx-segment-flags"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;

    let gfx1_uuid: KVec<u8> = handoff
        .get_property(c_str!("apple,gfx1-firmware-uuid"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx1_names: KVec<u8> = handoff
        .get_property(c_str!("apple,gfx1-segment-names"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx1_iovas: KVec<u64> = handoff
        .get_property(c_str!("apple,gfx1-segment-iovas"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx1_remaps: KVec<u64> = handoff
        .get_property(c_str!("apple,gfx1-segment-remaps"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx1_sizes: KVec<u32> = handoff
        .get_property(c_str!("apple,gfx1-segment-sizes"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;
    let gfx1_flags: KVec<u32> = handoff
        .get_property(c_str!("apple,gfx1-segment-flags"))
        .map_err(|_| G17LoadedHandoffError::InvalidEnvelope)?;

    let gfx_resources = read_role_resources(
        &handoff,
        [
            c_str!("gfx-fw-text"),
            c_str!("gfx-fw-data"),
            c_str!("gfx-fw-shared-ro"),
        ],
    )?;
    let gfx1_resources = read_role_resources(
        &handoff,
        [
            c_str!("gfx1-fw-text"),
            c_str!("gfx1-fw-data"),
            c_str!("gfx1-fw-shared-ro"),
        ],
    )?;

    // SAFETY: These are bootloader-reserved, `no-map`, immutable firmware
    // text resources. This function exposes only shared byte slices for
    // hashing, performs no write and initiates no DMA, and drops both mappings
    // before returning the identity-only admission token.
    let gfx_text = unsafe { Mem::try_new(gfx_resources.text, MemFlag::WB.into()) }
        .map_err(|_| G17LoadedHandoffError::MappingFailed)?;
    // SAFETY: Same handoff and read-only hashing contract as GFX above.
    let gfx1_text = unsafe { Mem::try_new(gfx1_resources.text, MemFlag::WB.into()) }
        .map_err(|_| G17LoadedHandoffError::MappingFailed)?;
    let gfx_text_bytes = unsafe { slice::from_raw_parts(gfx_text.ptr(), gfx_text.size()) };
    let gfx1_text_bytes = unsafe { slice::from_raw_parts(gfx1_text.ptr(), gfx1_text.size()) };

    let gfx = G17LoadedRoleObservation {
        role: FirmwareRole::Gfx,
        uuid: &gfx_uuid,
        segment_names: &gfx_names,
        iovas: &gfx_iovas,
        remaps: &gfx_remaps,
        physical: &gfx_resources.physical,
        physical_sizes: &gfx_resources.sizes,
        sizes: &gfx_sizes,
        flags: &gfx_flags,
        text: gfx_text_bytes,
    };
    let gfx1 = G17LoadedRoleObservation {
        role: FirmwareRole::Gfx1,
        uuid: &gfx1_uuid,
        segment_names: &gfx1_names,
        iovas: &gfx1_iovas,
        remaps: &gfx1_remaps,
        physical: &gfx1_resources.physical,
        physical_sizes: &gfx1_resources.sizes,
        sizes: &gfx1_sizes,
        flags: &gfx1_flags,
        text: gfx1_text_bytes,
    };

    admit_g17_loaded_pair(raw_variant, Some(&gfx), Some(&gfx1))
        .map_err(G17LoadedHandoffError::Admission)
}

fn validate_selected_nested(
    role: FirmwareRole,
    variant: G17FirmwareVariant,
    image: &[u8],
) -> Result<(), G17FirmwareError> {
    let expected =
        g17_nested_firmware_identity(role, variant).ok_or(G17FirmwareError::UnknownVariant)?;
    if image.len() != expected.size as usize {
        return Err(G17FirmwareError::NestedSizeMismatch);
    }
    let uuid = validate_macho_structure(image)?;
    if uuid != expected.uuid {
        return Err(G17FirmwareError::NestedUuidMismatch);
    }
    if sha256(image) != expected.sha256 {
        return Err(G17FirmwareError::NestedHashMismatch);
    }
    Ok(())
}

fn validate_role_container(
    role: FirmwareRole,
    variant: G17FirmwareVariant,
    container: &[u8],
) -> Result<(), G17FirmwareError> {
    let selected = select_nested(role, variant, container)?;
    validate_selected_nested(role, variant, selected)
}

/// Validate an exact role pair atomically from authoritative FTAB bytes.
///
/// No admission exists if either role fails. The returned token proves only
/// byte identity; all relocation, mapping, power, recovery, and submission
/// gates remain independent.
pub(crate) fn admit_g17_firmware_pair(
    raw_variant: u8,
    gfx_container: &[u8],
    gfx1_container: &[u8],
) -> Result<G17FirmwareIdentityAdmission, G17FirmwareError> {
    let variant =
        decode_g17_firmware_variant(raw_variant).map_err(|_| G17FirmwareError::UnknownVariant)?;
    validate_role_container(FirmwareRole::Gfx, variant, gfx_container)
        .map_err(|_| G17FirmwareError::GfxInvalid)?;
    validate_role_container(FirmwareRole::Gfx1, variant, gfx1_container)
        .map_err(|_| G17FirmwareError::Gfx1Invalid)?;
    Ok(G17FirmwareIdentityAdmission { variant })
}

