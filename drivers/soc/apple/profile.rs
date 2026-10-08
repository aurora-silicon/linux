// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
// Copyright 2026 Ryan Murray

//! Typed platform profiles for the SEP driver.
//!
//! Every SoC-specific fact the driver needs — the shared-memory capacity, the
//! boot handshake, the OS-identity source and the sensor transport — is named
//! here once. The driver reads addresses from the device tree, never scans for
//! them, and never patches properties at runtime.
//!
//! Five bring-up targets are modelled:
//!
//! * `T8103` / J313 (MacBook Air, M1): the host boots the SEP with the boot
//!   endpoint handshake over a 0x30000 shared-memory window.
//! * `T6000` / J316s (MacBook Pro 16", M1 Pro): the T8103 boot handshake and
//!   key store, with the sensor on the T6020 SPI2 address.
//! * `T6020` / J414s (MacBook Pro 14", M2 Pro): the driver does the warm
//!   single-message registration over a 0x40000 window.
//! * `T8140` / J700 (MacBook Neo, A18 Pro): iBoot boots sepOS before the AP
//!   OS, so the driver uses the warm registration path.
//! * `T8112` / J413, J415 (MacBook Air, M2): iBoot hands over a running SEP and
//!   a powered sensor, so the driver uses the warm registration path.

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

/// Identity keybag `CREATE_KEYBAG` field encoding. The first word is the
/// codec's struct version: the 13.5 enclave (T8103) takes versions 0-2 only
/// and macOS creates an identity with version 2, while the T6020 enclave takes
/// variant 5. Kept per-SoC so the T8103 path is correct without disturbing the
/// proven T6020 encoding (hardware-verified for enrol, match, and reboot).
pub(crate) struct KeybagCreate {
    /// First word: struct version (request variant), echoed back in the reply.
    pub(crate) variant: u32,
    /// Third word: bag type. Identity is `0x400000` on 13.5; `0` on T6020,
    /// which distinguishes the bag by the variant word instead.
    pub(crate) bag_type: u32,
    /// Fourth word: create argument / parent handle.
    pub(crate) arg: i32,
}

/// Which key-store protocol the enclave speaks. It follows the sepOS the
/// firmware hands the SEP, not the SoC alone: the T8103 stub boots macOS 13.5
/// (22G74)'s sepOS, whose request shapes are those of the 13.5
/// `AppleSEPKeyStore`; the T6020 sepOS is newer.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum KeyStore {
    /// macOS 13.5's key store. Before any other request the endpoint is
    /// initialised as `AppleKeyStore::init_sep_endpoint` does it: `0x4d`, then
    /// `set_env` carrying the ADT's `/defaults` `cpx-encryption-mode`.
    Sepos13 { cpx_encryption_mode: u32 },
    /// The hardware-verified T6020 encoding (variant-5 create).
    Variant5,
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
    /// Whether the SEP DART IOVA window needs a DMA mask wider than 32 bits.
    pub(crate) wide_dma_mask: bool,
    /// Reserved-memory region holding the SEP firmware image (cold-boot path).
    pub(crate) firmware_region: &'static CStr,
    /// Identity keybag CREATE_KEYBAG field encoding (per-SoC; see [`KeybagCreate`]).
    pub(crate) keybag_create: KeybagCreate,
    /// Require nonce authentication and signed sensor material before capture.
    pub(crate) nonce_session: bool,
    /// Key-store protocol generation (see [`KeyStore`]).
    pub(crate) key_store: KeyStore,
    /// Platforms with reboot-tested owner export and user-before-master saves.
    pub(crate) persistent_enrol: bool,
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
    wide_dma_mask: false,
    firmware_region: c"sepfw",
    // macOS 13.5's AppleKeyStore::identity_create (0xfffffe000994b2d8):
    // version 2, type 0x400000, parent -1 (it accepts only -1 or <= -10).
    keybag_create: KeybagCreate {
        variant: 2,
        bag_type: 0x40_0000,
        arg: -1,
    },
    // The j293 and j313 ADTs both carry `/defaults` `cpx-encryption-mode = 2`.
    key_store: KeyStore::Sepos13 {
        cpx_encryption_mode: 2,
    },
    persistent_enrol: true,
    nonce_session: false,
};

/// M1 Pro (J316s). The M1 family cold-boots its SEP like T8103, so the boot
/// path, shared-memory geometry and key store follow T8103. The sensor sits on
/// SPI2 at the T6020 address, in the mode T6020 verified: the J313, J316s and
/// J414s platform device trees describe the same sensor (id 0x3352)
/// identically. Enrolment persistence is reboot-tested on J314s.
const T6000: PlatformProfile = PlatformProfile {
    name: "T6000/J316s",
    shmem_capacity: 0x3_0000,
    shmem_first_item: b"CNIP",
    bootstrap: Bootstrap::Boot,
    identity: IdentitySource::Chosen,
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
    dart_range_required: false,
    wide_dma_mask: false,
    firmware_region: c"sepfw",
    keybag_create: KeybagCreate {
        variant: T8103.keybag_create.variant,
        bag_type: T8103.keybag_create.bag_type,
        arg: T8103.keybag_create.arg,
    },
    // The J316s ADT carries `/defaults` `cpx-encryption-mode = 2`.
    key_store: KeyStore::Sepos13 {
        cpx_encryption_mode: 2,
    },
    persistent_enrol: true,
    nonce_session: false,
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
    wide_dma_mask: false,
    firmware_region: c"sepfw",
    // Proven encoding: the lenient enclave takes the variant in the first word.
    keybag_create: KeybagCreate {
        variant: 5,
        bag_type: 0,
        arg: -1,
    },
    key_store: KeyStore::Variant5,
    persistent_enrol: true,
    nonce_session: false,
};

/// MacBook Neo. The J700 ADT records a pre-booted SEP, a spi2 Mesa sensor
/// (0x3356), and a SEP DART aperture above 4 GiB. These values describe that
/// board; device-tree nodes remain authoritative for addresses and resources.
const T8140: PlatformProfile = PlatformProfile {
    key_store: KeyStore::Variant5,
    persistent_enrol: true,
    name: "T8140/J700",
    shmem_capacity: 0x4_0000,
    shmem_first_item: b"CINP",
    bootstrap: Bootstrap::WarmRegister,
    identity: IdentitySource::Chosen,
    sensor: SensorProfile {
        controller_base: 0x3_8510_8000,
        chip_select: 0,
        max_hz: 8_000_000,
        cs_setup_ns: 20,
        cs_hold_ns: 20,
        mode: SpiMode::Mode2,
        expected_id: 0x3356,
        capture_qualified: false,
    },
    dart_range_required: true,
    wide_dma_mask: true,
    firmware_region: c"sepfw",
    // J700 hardware refused the strict encoding and accepted this T6020 form
    // (linux-aurora commit 18165cf1de, 2026-09-19).
    keybag_create: KeybagCreate {
        variant: 5,
        bag_type: 0,
        arg: -1,
    },
    nonce_session: true,
};

/// MacBook Air M2 (J413, J415). iBoot leaves the SEP running and the Mesa
/// sensor powered (J415: AP GPIO 178 is a driven-high output at handoff), so
/// this is a warm-registration platform like T6020. The J415 ADT describes the
/// sensor exactly as J413's does: spi2 at the T8103 address, power on AP GPIO
/// 178, data-ready on 177, 8 MHz, chip select 0. The SEP DART aperture is
/// 0x4000 + 0xffff0000, below 4 GiB, so no wide DMA mask. `/defaults`
/// carries `cpx-encryption-mode = 2`.
///
/// The shared-memory geometry and key-store dialect follow the other
/// warm-registration platforms (T6020, T8140).
const T8112: PlatformProfile = PlatformProfile {
    name: "T8112/J415",
    shmem_capacity: 0x4_0000,
    shmem_first_item: b"CINP",
    bootstrap: Bootstrap::WarmRegister,
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
    dart_range_required: true,
    wide_dma_mask: false,
    firmware_region: c"sepfw",
    // J415 accepted the 13.5 GET_CAPABILITIES/SET_ENV init but refused the
    // strict CREATE_KEYBAG (mailbox status -1, empty reply), as J700 did;
    // use the T6020 form like the other warm-registration platforms.
    keybag_create: KeybagCreate {
        variant: 5,
        bag_type: 0,
        arg: -1,
    },
    key_store: KeyStore::Variant5,
    persistent_enrol: true,
    nonce_session: false,
};

static_assert!(T8103.shmem_capacity == 0x30000);
static_assert!(T6020.shmem_capacity == 0x40000);
static_assert!(T8140.shmem_capacity == 0x40000);
static_assert!(T8103.sensor.controller_base == 0x235108000);
static_assert!(T6020.sensor.controller_base == 0x39b108000);
static_assert!(T8140.sensor.controller_base == 0x385108000);
static_assert!(T8103.sensor.expected_id == T6020.sensor.expected_id);
static_assert!(T6000.shmem_capacity == T8103.shmem_capacity);
static_assert!(T6000.sensor.controller_base == T6020.sensor.controller_base);
static_assert!(T6000.sensor.expected_id == T6020.sensor.expected_id);
static_assert!(T8112.shmem_capacity == T6020.shmem_capacity);
static_assert!(T8112.sensor.controller_base == T8103.sensor.controller_base);

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
    // t6020.dtsi is defined as a cut-down t6021: it includes t6021.dtsi and
    // disables the parts the smaller die lacks. Both pull in t602x-die0.dtsi,
    // so the SEP, its DART and the SPI controller sit at the same addresses
    // with the same interrupts on either part, and the T6020 constants apply
    // to the M2 Max unchanged.
    if machine_has(b"apple,t6021") {
        return Ok(&T6020);
    }
    if machine_has(b"apple,t6000") {
        return Ok(&T6000);
    }
    // t6000.dtsi is defined as a cut-down t6001: it includes t6001.dtsi and
    // deletes the parts the smaller die lacks. Both pull in t600x-die0.dtsi,
    // so the SEP, its mailbox, its DART and the SPI controller sit at the same
    // addresses with the same interrupts on either part, and the T6000
    // constants apply to the M1 Max unchanged. The Mac Studio (j375c) is also
    // a t6001 but leaves the SEP disabled, so nothing binds there; apple,t6002
    // (M1 Ultra, no built-in sensor) stays unmapped.
    if machine_has(b"apple,t6001") {
        return Ok(&T6000);
    }
    if machine_has(b"apple,t8140") {
        return Ok(&T8140);
    }
    if machine_has(b"apple,t8112") {
        return Ok(&T8112);
    }
    Err(ENODEV)
}
