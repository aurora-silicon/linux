// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


use core::result::Result;

/// The grounded G15 root + runtime-pointer graph is available for offline
/// validation and encoding.
pub(crate) const GROUNDED_LAYOUT_AVAILABLE: bool = true;

/// The feature-derived hw_globals scalar map, the `root+0x30..0x97` config
/// blob semantics, and the exact struct byte-sizes are UNKNOWN offline (they
/// need the G15 firmware image's copy-counts or a live boot trace), so the
/// generated object graph is not safe to hand to real firmware yet.
pub(crate) const BOOTABLE_LAYOUT_AVAILABLE: bool = false;

pub(crate) const INITDATA_MAGIC_G15: u64 = 0x0c89_c357_8392_04b8;

/// Root fields end at the role pointer (`+0xa8`, u64). The true allocation is
/// page-rounded to an UNKNOWN size (`0x8954a44`-`a6c`); this is the minimum
/// field extent an encoder must be able to write.
pub(crate) const ROOT_FIELD_EXTENT: usize = 0xb0;
/// runtime_pointers fields end at the unaligned pointer `+0x469` (u64). G17's
/// equivalent object is `0x4c0`; the exact G15 size is UNKNOWN.
pub(crate) const RT_FIELD_EXTENT: usize = 0x471;
/// hw_globals field extent is only bounded (`<= ~0x2600`, G17 `0x2828`); the
/// exact size is UNKNOWN. Used as the minimum a constant-subset encoder needs.
pub(crate) const HW_GLOBALS_MIN_EXTENT: usize = 0x2600;

// 44-bit firmware address form, identical to G17 (the init-data doorbell tag
// truncates to 44 bits; see `g15_boot::encode_initdata_doorbell`).
const INITDATA_ADDR_MASK: u64 = 0x0000_0fff_ffff_ffff;
const INITDATA_ADDR_PREFIX: u64 = 0xffff_f000_0000_0000;

const ROOT_MAGIC: usize = 0x00; // str x8,[x0]           @0x8953f20
const ROOT_FW_SHARED_ALLOC: usize = 0x08; // str x0,[x20,#0x8]     @0x8954068
const ROOT_RUNTIME_POINTERS: usize = 0x18; // str x0,[x20,#0x18]    @0x8953f64
const ROOT_SHARED: usize = 0x20; // str x0,[x20,#0x20]    @0x8953fa8
const ROOT_ROLE: usize = 0x28; // u32, part of the 8-byte const @0x89540ac
const ROOT_HOST_MAPPED_ALLOCS: usize = 0x2c; // u32, same const
const ROOT_GPU_CONFIG: core::ops::Range<usize> = 0x30..0x98; // 0x68B blob @0x895406c-a8
const ROOT_ROLE_POINTER: usize = 0xa8; // str x0,[x20,#0xa8]    @0x8953fe8

// runtime_pointers offsets (grade A).
const RT_HW_GLOBALS: usize = 0x000; // hw_globals GPU VA
const RT_ROLE_POINTER: usize = 0x200; // role GPU VA
const RT_FW_BUFFERS: [usize; 5] = [0x254, 0x25c, 0x264, 0x26c, 0x274];
/// Unaligned runtime-pointer, `add x8,x21,#0x469; str x0,[x8]` @`0x8945504`.
/// G17 places the equivalent role-1 pointer at `+0x481`.
pub(crate) const RT_UNALIGNED_POINTER: usize = 0x469;

const HW_ED0_CONST: usize = 0xed0; // mov w8,#0x5dc0; str w8,[x9,#0xed0] @0x89537a0
const HW_ED0_VALUE: u32 = 0x5dc0;
const HW_EE4_CONST: usize = 0xee4; // mov x10,#0x100000001; str x10,[x9+0xee4] @0x89537b0
const HW_EE4_VALUE: u64 = 0x0000_0001_0000_0001;
const HW_F04_CONST: usize = 0xf04; // mov w8,#0x1f; str w8,[x9,#0xf04] @0x89537b8
const HW_F04_VALUE: u32 = 0x1f;

/// GPU virtual addresses consumed by the grounded G15 root.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub(crate) struct RootAddresses {
    /// runtime_pointers GPU VA (root+0x18). Required.
    pub(crate) runtime_pointers: u64,
    /// role-independent shared GPU VA (root+0x20). Required.
    pub(crate) shared: u64,
    /// role GPU VA (root+0xa8). Required.
    pub(crate) role: u64,
    /// firmware shared allocation GPU VA (root+0x08). Optional (may be zero).
    pub(crate) firmware_shared_allocation: u64,
}

/// GPU virtual addresses consumed by the grounded G15 runtime_pointers table.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub(crate) struct RuntimePointerAddresses {
    /// hw_globals GPU VA (runtime_pointers+0x000). Required.
    pub(crate) hw_globals: u64,
    /// role GPU VA (runtime_pointers+0x200). Required.
    pub(crate) role: u64,
    /// Five firmware sub-allocation GPU VAs (+0x254/25c/264/26c/274). Each is
    /// optional because the matching host stores are conditional on the
    /// firmware sub-allocation existing.
    pub(crate) firmware_buffers: [u64; 5],
}

/// Fail-closed validation errors for the grounded G15 layout subset.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum InitdataError {
    BufferTooSmall,
    AddressOutOfRange,
    NullRequiredAddress,
    InterfaceMismatch,
    HostMappedAllocationsDisabled,
}

fn address_representable(address: u64) -> bool {
    let high = address & !INITDATA_ADDR_MASK;
    high == 0 || high == INITDATA_ADDR_PREFIX
}

fn required_address(address: u64) -> Result<(), InitdataError> {
    if address == 0 {
        Err(InitdataError::NullRequiredAddress)
    } else if !address_representable(address) {
        Err(InitdataError::AddressOutOfRange)
    } else {
        Ok(())
    }
}

fn optional_address(address: u64) -> Result<(), InitdataError> {
    if address == 0 || address_representable(address) {
        Ok(())
    } else {
        Err(InitdataError::AddressOutOfRange)
    }
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

/// Encode the grounded G15 single-role init-data root.
///
/// The whole buffer is zeroed first, so unknown bytes (the gpu-config blob and
/// any page-rounded tail) are deterministic rather than stale host memory.
/// `root` must be at least [`ROOT_FIELD_EXTENT`]; the true allocation size is
/// page-rounded and UNKNOWN.
pub(crate) fn encode_grounded_root(
    addresses: &RootAddresses,
    root: &mut [u8],
) -> Result<(), InitdataError> {
    if root.len() < ROOT_FIELD_EXTENT {
        return Err(InitdataError::BufferTooSmall);
    }
    required_address(addresses.runtime_pointers)?;
    required_address(addresses.shared)?;
    required_address(addresses.role)?;
    optional_address(addresses.firmware_shared_allocation)?;

    root.fill(0);
    put_u64(root, ROOT_MAGIC, INITDATA_MAGIC_G15);
    put_u64(
        root,
        ROOT_FW_SHARED_ALLOC,
        addresses.firmware_shared_allocation,
    );
    put_u64(root, ROOT_RUNTIME_POINTERS, addresses.runtime_pointers);
    put_u64(root, ROOT_SHARED, addresses.shared);
    // The single 8-byte constant at 0x712dba8 = {role:u32=0, host_mapped:u32=1}.
    put_u32(root, ROOT_ROLE, 0);
    put_u32(root, ROOT_HOST_MAPPED_ALLOCS, 1);
    // root+0x30..0x97 is a 0x68-byte opaque config blob; its semantics are
    // UNKNOWN, so it stays zero.
    put_u64(root, ROOT_ROLE_POINTER, addresses.role);
    Ok(())
}

/// Encode the grounded G15 runtime_pointers pointer graph.
///
/// Only the pointer slots whose offset and source are grounded are written.
/// The unaligned pointer at `+0x469` and the conditional pointer at `+0x2c8`
/// target objects whose contents are UNKNOWN offline and stay zero.
pub(crate) fn encode_grounded_runtime_pointers(
    addresses: &RuntimePointerAddresses,
    runtime_pointers: &mut [u8],
) -> Result<(), InitdataError> {
    if runtime_pointers.len() < RT_FIELD_EXTENT {
        return Err(InitdataError::BufferTooSmall);
    }
    required_address(addresses.hw_globals)?;
    required_address(addresses.role)?;
    for buffer in addresses.firmware_buffers {
        optional_address(buffer)?;
    }

    runtime_pointers.fill(0);
    put_u64(runtime_pointers, RT_HW_GLOBALS, addresses.hw_globals);
    put_u64(runtime_pointers, RT_ROLE_POINTER, addresses.role);
    for (slot, address) in RT_FW_BUFFERS.iter().zip(addresses.firmware_buffers) {
        put_u64(runtime_pointers, *slot, address);
    }
    Ok(())
}

pub(crate) fn encode_grounded_hw_globals_constants(
    hw_globals: &mut [u8],
) -> Result<(), InitdataError> {
    if hw_globals.len() < HW_GLOBALS_MIN_EXTENT {
        return Err(InitdataError::BufferTooSmall);
    }
    hw_globals.fill(0);
    put_u32(hw_globals, HW_ED0_CONST, HW_ED0_VALUE);
    put_u64(hw_globals, HW_EE4_CONST, HW_EE4_VALUE);
    put_u32(hw_globals, HW_F04_CONST, HW_F04_VALUE);
    Ok(())
}

/// Reproduce the firmware root-acceptance gates.
///
/// Grade B: these two checks are carried over from the G17 firmware (whose
/// `0x81`-tag parse validates magic and the host-mapped-allocs flag); the G15
/// firmware image is not on disk, so this is a self-consistency model of our
/// own encoder output, not a confirmed G15 firmware read.
pub(crate) fn check_firmware_root_gates(root: &[u8]) -> Result<(), InitdataError> {
    if root.len() < ROOT_FIELD_EXTENT {
        return Err(InitdataError::BufferTooSmall);
    }
    if get_u64(root, ROOT_MAGIC) != INITDATA_MAGIC_G15 {
        return Err(InitdataError::InterfaceMismatch);
    }
    if get_u32(root, ROOT_HOST_MAPPED_ALLOCS) == 0 {
        return Err(InitdataError::HostMappedAllocationsDisabled);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn root_addresses() -> RootAddresses {
        RootAddresses {
            runtime_pointers: 0x1_0000_4000,
            shared: 0x6000_0000,
            role: 0x1_0000_c000,
            firmware_shared_allocation: 0x5000_0000,
        }
    }

    fn rt_addresses() -> RuntimePointerAddresses {
        RuntimePointerAddresses {
            hw_globals: 0x1_0000_8000,
            role: 0x1_0000_c000,
            firmware_buffers: [0x2000, 0x3000, 0x4000, 0x5000, 0x6000],
        }
    }

    #[test]
    fn root_magic_is_the_g15_value_not_g17() {
        assert_eq!(INITDATA_MAGIC_G15, 0x0c89_c357_8392_04b8);
        assert_ne!(INITDATA_MAGIC_G15, 0x0c8d_e339_0728_04c0);
    }

    #[test]
    fn root_encodes_single_role_proven_fields() {
        let mut root = vec![0xa5u8; ROOT_FIELD_EXTENT];
        encode_grounded_root(&root_addresses(), &mut root).unwrap();

        assert_eq!(get_u64(&root, 0x00), INITDATA_MAGIC_G15);
        assert_eq!(get_u64(&root, 0x08), 0x5000_0000);
        assert_eq!(get_u64(&root, 0x18), 0x1_0000_4000);
        assert_eq!(get_u64(&root, 0x20), 0x6000_0000);
        assert_eq!(get_u32(&root, 0x28), 0); // role 0 (single role)
        assert_eq!(get_u32(&root, 0x2c), 1); // host_mapped_allocs
        assert_eq!(get_u64(&root, 0xa8), 0x1_0000_c000);
        // The opaque config blob and any tail stay zero.
        assert!(root[ROOT_GPU_CONFIG].iter().all(|b| *b == 0));
        // Single role: no cross-role fields (G17 wrote root+0xb0/0xb8/0xc0).
        assert!(check_firmware_root_gates(&root).is_ok());
    }

    #[test]
    fn runtime_pointers_encode_the_grounded_graph_and_unaligned_offset() {
        assert_eq!(RT_UNALIGNED_POINTER, 0x469);
        let mut rt = vec![0xa5u8; RT_FIELD_EXTENT];
        encode_grounded_runtime_pointers(&rt_addresses(), &mut rt).unwrap();

        assert_eq!(get_u64(&rt, 0x000), 0x1_0000_8000);
        assert_eq!(get_u64(&rt, 0x200), 0x1_0000_c000);
        assert_eq!(get_u64(&rt, 0x254), 0x2000);
        assert_eq!(get_u64(&rt, 0x25c), 0x3000);
        assert_eq!(get_u64(&rt, 0x264), 0x4000);
        assert_eq!(get_u64(&rt, 0x26c), 0x5000);
        assert_eq!(get_u64(&rt, 0x274), 0x6000);
        // The unaligned +0x469 target is UNKNOWN and stays zero.
        assert_eq!(get_u64(&rt, 0x469), 0);
    }

    #[test]
    fn hw_globals_constant_subset_is_written_and_nothing_else() {
        let mut hw = vec![0xa5u8; HW_GLOBALS_MIN_EXTENT];
        encode_grounded_hw_globals_constants(&mut hw).unwrap();
        assert_eq!(get_u32(&hw, 0xed0), 0x5dc0);
        assert_eq!(get_u64(&hw, 0xee4), 0x0000_0001_0000_0001);
        assert_eq!(get_u32(&hw, 0xf04), 0x1f);
        // A representative feature-derived offset stays zero (UNKNOWN).
        assert_eq!(get_u32(&hw, 0xec4), 0);
        assert_eq!(get_u32(&hw, 0x25ec), 0);
    }

    #[test]
    fn addresses_and_sizes_fail_closed() {
        let mut root = vec![0u8; ROOT_FIELD_EXTENT];
        let mut a = root_addresses();
        a.runtime_pointers = 1 << 44;
        assert_eq!(
            encode_grounded_root(&a, &mut root),
            Err(InitdataError::AddressOutOfRange)
        );
        a = root_addresses();
        a.role = 0;
        assert_eq!(
            encode_grounded_root(&a, &mut root),
            Err(InitdataError::NullRequiredAddress)
        );
        let mut small = vec![0u8; ROOT_FIELD_EXTENT - 1];
        assert_eq!(
            encode_grounded_root(&root_addresses(), &mut small),
            Err(InitdataError::BufferTooSmall)
        );
    }

    #[test]
    fn root_gate_rejects_wrong_magic_and_disabled_gate() {
        let mut root = vec![0u8; ROOT_FIELD_EXTENT];
        encode_grounded_root(&root_addresses(), &mut root).unwrap();
        root[0] ^= 1;
        assert_eq!(
            check_firmware_root_gates(&root),
            Err(InitdataError::InterfaceMismatch)
        );
        put_u64(&mut root, 0, INITDATA_MAGIC_G15);
        put_u32(&mut root, ROOT_HOST_MAPPED_ALLOCS, 0);
        assert_eq!(
            check_firmware_root_gates(&root),
            Err(InitdataError::HostMappedAllocationsDisabled)
        );
    }

    #[test]
    fn layout_is_grounded_but_not_bootable() {
        assert!(GROUNDED_LAYOUT_AVAILABLE);
        assert!(!BOOTABLE_LAYOUT_AVAILABLE);
    }
}
