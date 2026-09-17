// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Typed platform profiles for the SEP driver.
//!
//! Every SoC-specific fact the driver needs — the shared-memory capacity, the
//! boot handshake, the OS-identity source and the sensor transport — is named
//! here once. The driver reads addresses from the device tree, never scans for
//! them, and never patches properties at runtime.
//!
//! Two bring-up targets are modelled:
//!
//! * `T8103` / J313 (MacBook Air, M1): the host boots the SEP with the boot
//!   endpoint handshake over a 0x30000 shared-memory window.
//! * `T6020` / J414s (MacBook Pro 14", M2 Pro): the driver does the warm
//!   single-message registration over a 0x40000 window.

// The boot handshake, identity source, sensor and DART fields are consumed by
// the boot-endpoint, identity and sensor-transport paths.
#![allow(dead_code)]

use kernel::of;
use kernel::prelude::*;

/// How the driver brings the shared-memory table to the SEP.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Bootstrap {
    /// Cold boot: send TZ0, map the firmware reserved region and answer the
    /// second boot acknowledgement with the firmware address and `SET_SHMEM`.
    Boot,
    /// Warm attach: send the single shared-memory registration message with the
    /// table address and size.
    WarmRegister,
}

/// Where the 16-byte OS identity comes from.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum IdentitySource {
    /// `/chosen/apfs-preboot-uuid`, the value the firmware handed to this boot
    /// and the boot chain forwards. A malformed or absent property is refused;
    /// the identity is never invented.
    Chosen,
    /// A UUID provisioned in the Linux host-state store on first bring-up.
    HostPersisted,
}

/// SPI mode as the device tree spells it. Mode 1 is CPOL=0/CPHA=1 (`spi-cpha`);
/// mode 2 is CPOL=1/CPHA=0 (`spi-cpol`).
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SpiMode {
    Mode1,
    Mode2,
}

impl SpiMode {
    /// The value the SPI shim programs into `spi_setup()`, using the kernel's
    /// `SPI_CPHA`/`SPI_CPOL` bits.
    pub(crate) const fn wire(self) -> u32 {
        match self {
            SpiMode::Mode1 => 1, // SPI_CPHA
            SpiMode::Mode2 => 2, // SPI_CPOL
        }
    }

    pub(crate) const fn name(self) -> &'static CStr {
        match self {
            SpiMode::Mode1 => c"mode 1 (CPOL=0 CPHA=1)",
            SpiMode::Mode2 => c"mode 2 (CPOL=1 CPHA=0)",
        }
    }
}

pub(crate) struct SensorProfile {
    /// SPI controller register base. Diagnostics only: the device tree is
    /// authoritative and the driver never enables or creates the bus.
    pub(crate) controller_base: u64,
    pub(crate) chip_select: u32,
    pub(crate) max_hz: u32,
    /// Chip-select setup and hold in nanoseconds.
    pub(crate) cs_setup_ns: u32,
    pub(crate) cs_hold_ns: u32,
    pub(crate) mode: SpiMode,
    pub(crate) expected_id: u16,
    /// Capture traffic stays disabled until a qualified DMA transport exists;
    /// the PIO path is status-only.
    pub(crate) capture_qualified: bool,
}

/// Identity keybag `CREATE_KEYBAG` field encoding. The strict T8103 enclave
/// reads the request's first word as the bag type and rejects the value the
/// lenient T6020 enclave accepts there; macOS carries the type in the third
/// word instead. Kept per-SoC so the strict path is correct without disturbing
/// the proven T6020 encoding (hardware-verified for enrol, match, and reboot).
pub(crate) struct KeybagCreate {
    /// First word: request variant, echoed back in the reply.
    pub(crate) variant: u32,
    /// Third word: bag type. Identity is `0x20000` on the strict path; `0` on
    /// T6020, which distinguishes the bag by the variant word instead.
    pub(crate) bag_type: u32,
    /// Fourth word: create argument / parent handle.
    pub(crate) arg: i32,
}

pub(crate) struct PlatformProfile {
    pub(crate) name: &'static str,
    /// Capacity of the boot shared-memory window. The layout is still checked
    /// against the manifests; this only bounds the allocation.
    pub(crate) shmem_capacity: usize,
    /// Fourcc of the first shared-memory item. The two paths spell it
    /// differently, so each profile carries its own spelling.
    pub(crate) shmem_first_item: &'static [u8; 4],
    pub(crate) bootstrap: Bootstrap,
    pub(crate) identity: IdentitySource,
    pub(crate) sensor: SensorProfile,
    /// Require a static `apple,dma-range` on the SEP DART. The T6020 SEP only
    /// accepts IOVAs below 4 GiB; T8103 works with the stock DART aperture.
    pub(crate) dart_range_required: bool,
    /// Reserved-memory region holding the SEP firmware image (cold-boot path).
    pub(crate) firmware_region: &'static CStr,
    /// Identity keybag CREATE_KEYBAG field encoding (per-SoC; see [`KeybagCreate`]).
    pub(crate) keybag_create: KeybagCreate,
}

const T8103: PlatformProfile = PlatformProfile {
    name: "T8103/J313",
    shmem_capacity: 0x3_0000,
    shmem_first_item: b"CNIP",
    bootstrap: Bootstrap::Boot,
    identity: IdentitySource::Chosen,
    sensor: SensorProfile {
        controller_base: 0x2_3510_8000,
        chip_select: 0,
        max_hz: 8_000_000,
        cs_setup_ns: 20,
        cs_hold_ns: 20,
        mode: SpiMode::Mode1,
        expected_id: 0x3352,
        capture_qualified: false,
    },
    dart_range_required: false,
    firmware_region: c"sepfw",
    // Strict enclave: the type goes in the third word; the first word is 0.
    keybag_create: KeybagCreate {
        variant: 0,
        bag_type: 0x20000,
        arg: 0,
    },
};

const T6020: PlatformProfile = PlatformProfile {
    name: "T6020/J414s",
    shmem_capacity: 0x4_0000,
    shmem_first_item: b"CINP",
    bootstrap: Bootstrap::WarmRegister,
    identity: IdentitySource::HostPersisted,
    sensor: SensorProfile {
        controller_base: 0x3_9b10_8000,
        chip_select: 0,
        max_hz: 8_000_000,
        cs_setup_ns: 20,
        cs_hold_ns: 20,
        mode: SpiMode::Mode2,
        expected_id: 0x3352,
        capture_qualified: false,
    },
    dart_range_required: true,
    firmware_region: c"sepfw",
    // Proven encoding: the lenient enclave takes the variant in the first word.
    keybag_create: KeybagCreate {
        variant: 5,
        bag_type: 0,
        arg: -1,
    },
};

static_assert!(T8103.shmem_capacity == 0x30000);
static_assert!(T6020.shmem_capacity == 0x40000);
static_assert!(T8103.sensor.controller_base == 0x235108000);
static_assert!(T6020.sensor.controller_base == 0x39b108000);
static_assert!(T8103.sensor.expected_id == T6020.sensor.expected_id);

/// Whether the machine root declares `compatible`.
///
/// The root property is a NUL-separated list, so whole entries are compared
/// instead of substrings: `apple,t8103` cannot match a longer unrelated value.
fn machine_has(compatible: &[u8]) -> bool {
    let Some(root) = of::root() else {
        return false;
    };
    let Ok(list) = root.get_property::<KVec<u8>>(c"compatible") else {
        return false;
    };

    let mut at = 0usize;
    while at < list.len() {
        let end = match list[at..].iter().position(|&byte| byte == 0) {
            Some(offset) => at + offset,
            None => list.len(),
        };
        if &list[at..end] == compatible {
            return true;
        }
        at = end + 1;
    }

    false
}

/// Select the profile for the running machine. An unsupported SoC is refused
/// rather than guessed at, so the driver cannot run a handshake with the wrong
/// geometry.
pub(crate) fn detect() -> Result<&'static PlatformProfile> {
    if machine_has(b"apple,t8103") {
        return Ok(&T8103);
    }
    if machine_has(b"apple,t6020") {
        return Ok(&T6020);
    }
    Err(ENODEV)
}
