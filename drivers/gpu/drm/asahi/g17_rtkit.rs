// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


/// Receive-side validation and the proved field encoders are available.
pub(crate) const GROUNDED_CODEC_AVAILABLE: bool = true;
pub(crate) const EXECUTABLE_TRANSPORT_AVAILABLE: bool = APPLE_RTKIT_CORE_ACCEPTS_G17;

/// Generic `apple-rtkit` core supported interface-version window, mirrored
/// from `drivers/soc/apple/rtkit.c` (`APPLE_RTKIT_MIN/MAX_SUPPORTED_VERSION`).
pub(crate) const APPLE_RTKIT_CORE_MIN_VERSION: u16 = 11;
pub(crate) const APPLE_RTKIT_CORE_MAX_VERSION: u16 = 12;

/// Whether the generic core can negotiate the G17 firmware's advertised RTKit
/// interface version. This is the only management-layer fact the GPU codec
/// depends on; the rest of EP0 (HELLO/EPMAP framing) is the core's ABI.
pub(crate) const APPLE_RTKIT_CORE_ACCEPTS_G17: bool = INTERFACE_VERSION
    >= APPLE_RTKIT_CORE_MIN_VERSION
    && INTERFACE_VERSION <= APPLE_RTKIT_CORE_MAX_VERSION;

/// The host-served endpoint set in both recovered G17 firmware roles.
pub(crate) const EXPECTED_ENDPOINTS: [u8; 4] = [0x00, 0x01, 0x20, 0x21];

pub(crate) const ENDPOINT_MANAGEMENT: u8 = 0x00;
pub(crate) const ENDPOINT_CRASHLOG: u8 = 0x01;
pub(crate) const ENDPOINT_GFX_MESSAGES: u8 = 0x20;
pub(crate) const ENDPOINT_GFX_INTERRUPTS: u8 = 0x21;
/// Firmware-to-firmware control endpoint observed on T8140 (both instances).
///
/// It may legitimately appear in an instance's endpoint map, but it carries
/// peer traffic between the two firmware instances: the host must **never**
/// start it, ack it, or serve messages on it. It is permitted-but-unserved in
/// [`validate_endpoint_set`].
pub(crate) const ENDPOINT_FIRMWARE_PEER: u8 = 0x23;

/// Exact T8140/G17P discovery map for each independent firmware role.
pub(crate) const EXPECTED_G17P_DISCOVERY_ENDPOINTS: [u8; 5] = [
    ENDPOINT_MANAGEMENT,
    ENDPOINT_CRASHLOG,
    ENDPOINT_GFX_MESSAGES,
    ENDPOINT_GFX_INTERRUPTS,
    ENDPOINT_FIRMWARE_PEER,
];

/// Exact raw 256-bit EPMAP returned by both T8140/G17P firmware roles.
pub(crate) const EXPECTED_G17P_DISCOVERY_EPMAP: [u64; 4] = [
    (1u64 << ENDPOINT_MANAGEMENT)
        | (1u64 << ENDPOINT_CRASHLOG)
        | (1u64 << ENDPOINT_GFX_MESSAGES)
        | (1u64 << ENDPOINT_GFX_INTERRUPTS)
        | (1u64 << ENDPOINT_FIRMWARE_PEER),
    0,
    0,
    0,
];

const MANAGEMENT_TYPE_SHIFT: u32 = 52;
const MANAGEMENT_TYPE_MASK: u8 = 0x0f;

/// HELLO is generic management type one.
pub(crate) const MANAGEMENT_TYPE_HELLO: u8 = 0x01;
const GENERIC_MANAGEMENT_TYPE_MASK: u64 = 0xff << MANAGEMENT_TYPE_SHIFT;
const HELLO_MIN_VERSION_MASK: u64 = 0xffff;
const HELLO_MAX_VERSION_MASK: u64 = 0xffff << 16;
const HELLO_ALLOWED_MASK: u64 =
    GENERIC_MANAGEMENT_TYPE_MASK | HELLO_MIN_VERSION_MASK | HELLO_MAX_VERSION_MASK;

/// Byte-confirmed management type used by the endpoint-map path.
pub(crate) const MANAGEMENT_TYPE_EPMAP: u8 = 0x08;
pub(crate) const MANAGEMENT_TYPE_START_ENDPOINTS_PATH: u8 = 0x0b;
/// Literal observed in the same path. Its semantic state name is unknown.
pub(crate) const OBSERVED_START_ENDPOINTS_PAYLOAD: u16 = 0x8000;

/// Both 16-bit interface values are compared against this exact value.
///
/// Their positions inside HELLO are not exposed because those positions are
/// not byte-confirmed for G17.
pub(crate) const INTERFACE_VERSION: u16 = 0x000c;

const INITDATA_TAG: u64 = 0x0081_0000_0000_0000;
const INITDATA_TAG_CHECK_MASK: u64 = 0x00bf_0000_0000_0000;
const INITDATA_ADDRESS_MASK: u64 = 0x0000_0fff_ffff_ffff;
const INITDATA_ADDRESS_PREFIX: u64 = 0xffff_f000_0000_0000;
const RECOVERY_INSPECTION_TAG: u64 = 0x0086_0000_0000_0000;
const RECOVERY_INSPECTION_GENERATION_MASK: u64 = (1u64 << 44) - 1;
const DEVICE_CONTROL_TAG: u64 = 0x0084_0000_0000_0000;
const POWER_TRANSITION_END_TAG: u64 = 0x0089_0000_0000_0000;

pub(crate) const fn encode_power_transition_end() -> u64 {
    POWER_TRANSITION_END_TAG
}
const DEVICE_CONTROL_PRIMARY_MODE: u64 = 0x11;

const RUNTIME_TYPE_SHIFT: u32 = 44;
const RUNTIME_TYPE_MASK: u64 = 0xffff;
const RUNTIME_TYPE_VALID_MASK: u32 = 0x13ff_ffbe;

const KICK_STAMP_INDEX_MASK: u64 = 0xff;
const KICK_STAMP_SHIFT: u32 = 8;
const KICK_TIMESTAMP_MASK: u64 = 0x0000_00ff_ffff_ffff;
const KICK_QUEUE_ID_SHIFT: u32 = 40;
const KICK_QUEUE_ID_MASK: u8 = 0x7f;
const KICK_RESERVED_MASK: u64 = 0xffff_8000_0000_0000;
const KICK_QUEUE_RECORD_BASE: u32 = 0x10;
const KICK_QUEUE_RECORD_STRIDE: u32 = 0x20;

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

/// Raw facts exported by the generic discovery-only RTKit client.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct DiscoveryObservation<'a> {
    pub(crate) raw_hello: u64,
    pub(crate) hello_min_version: u16,
    pub(crate) hello_max_version: u16,
    pub(crate) endpoints: &'a [u64; 4],
}

/// Return whether an endpoint is instantiated by the recovered G17 roles.
pub(crate) const fn endpoint_is_present(endpoint: u8) -> bool {
    endpoint == ENDPOINT_MANAGEMENT
        || endpoint == ENDPOINT_CRASHLOG
        || endpoint == ENDPOINT_GFX_MESSAGES
        || endpoint == ENDPOINT_GFX_INTERRUPTS
        || endpoint == ENDPOINT_FIRMWARE_PEER
}

/// Return whether the host may start/serve an endpoint. EP 0x23 exists but is
/// firmware-to-firmware only.
pub(crate) const fn endpoint_is_host_served(endpoint: u8) -> bool {
    endpoint == ENDPOINT_MANAGEMENT
        || endpoint == ENDPOINT_CRASHLOG
        || endpoint == ENDPOINT_GFX_MESSAGES
        || endpoint == ENDPOINT_GFX_INTERRUPTS
}

/// Require every host-served endpoint `{0x00, 0x01, 0x20, 0x21}` to be
/// present exactly once.
///
/// Ordering is irrelevant; duplicates and unknown endpoints (e.g. 0x22) are
/// rejected. The firmware-to-firmware endpoint 0x23 is *permitted* in the map
/// (T8140 advertises it) but confers nothing: the host must never start or
/// serve it ([`ENDPOINT_FIRMWARE_PEER`]).
pub(crate) fn validate_endpoint_set(endpoints: &[u8]) -> Result<(), WireError> {
    let mut seen = 0u8;
    let mut peer_seen = false;
    for endpoint in endpoints {
        if *endpoint == ENDPOINT_FIRMWARE_PEER {
            if peer_seen {
                return Err(WireError::UnexpectedEndpointSet);
            }
            peer_seen = true;
            continue;
        }
        let Some(index) = EXPECTED_ENDPOINTS
            .iter()
            .position(|expected| expected == endpoint)
        else {
            return Err(WireError::UnexpectedEndpointSet);
        };
        let bit = 1u8 << index;
        if seen & bit != 0 {
            return Err(WireError::UnexpectedEndpointSet);
        }
        seen |= bit;
    }

    if seen == (1 << EXPECTED_ENDPOINTS.len()) - 1 {
        Ok(())
    } else {
        Err(WireError::UnexpectedEndpointSet)
    }
}

/// Admit one raw T8140/G17P role discovery.
///
/// The raw HELLO must contain only management type one and the two generic
/// version fields, both exactly `0x0c`.  The endpoint map is exact, including
/// firmware-to-firmware EP 0x23; unlike [`validate_endpoint_set`], this live
/// admission does not permit the peer endpoint to be absent.
pub(crate) fn validate_g17p_discovery(
    observation: &DiscoveryObservation<'_>,
) -> Result<(), WireError> {
    if observation.raw_hello & !HELLO_ALLOWED_MASK != 0
        || ((observation.raw_hello & GENERIC_MANAGEMENT_TYPE_MASK) >> MANAGEMENT_TYPE_SHIFT)
            != MANAGEMENT_TYPE_HELLO as u64
    {
        return Err(WireError::UnsupportedInterface);
    }

    let raw_min = (observation.raw_hello & HELLO_MIN_VERSION_MASK) as u16;
    let raw_max = ((observation.raw_hello & HELLO_MAX_VERSION_MASK) >> 16) as u16;
    if raw_min != observation.hello_min_version || raw_max != observation.hello_max_version {
        return Err(WireError::UnsupportedInterface);
    }
    validate_interface_versions(raw_min, raw_max)?;

    if observation.endpoints != &EXPECTED_G17P_DISCOVERY_EPMAP {
        return Err(WireError::UnexpectedEndpointSet);
    }

    Ok(())
}

/// Require independent, exact discoveries from both T8140 firmware roles.
pub(crate) fn validate_g17p_dual_discovery(
    gfx: &DiscoveryObservation<'_>,
    gfx1: &DiscoveryObservation<'_>,
) -> Result<(), WireError> {
    validate_g17p_discovery(gfx)?;
    validate_g17p_discovery(gfx1)
}

/// Extract the byte-confirmed EP0 management type from bits 55:52.
pub(crate) const fn management_type(message: u64) -> u8 {
    ((message >> MANAGEMENT_TYPE_SHIFT) as u8) & MANAGEMENT_TYPE_MASK
}

/// Accept only the interface pair checked by the recovered firmware.
pub(crate) const fn validate_interface_versions(first: u16, second: u16) -> Result<(), WireError> {
    if first == INTERFACE_VERSION && second == INTERFACE_VERSION {
        Ok(())
    } else {
        Err(WireError::UnsupportedInterface)
    }
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

pub(crate) const fn encode_recovery_inspection(generation: u64) -> u64 {
    RECOVERY_INSPECTION_TAG | (generation & RECOVERY_INSPECTION_GENERATION_MASK)
}

pub(crate) const fn encode_primary_device_control() -> u64 {
    DEVICE_CONTROL_TAG | DEVICE_CONTROL_PRIMARY_MODE
}

/// Decode an init-data handoff using the exact firmware tag check.
///
/// Bit 54 is intentionally ignored by the firmware's `0x00bf...` mask. The
/// returned address is canonicalized exactly as the firmware does.
pub(crate) const fn decode_initdata_doorbell(message: u64) -> Result<u64, WireError> {
    if message & INITDATA_TAG_CHECK_MASK != INITDATA_TAG {
        return Err(WireError::InvalidInitdataTag);
    }

    Ok(INITDATA_ADDRESS_PREFIX | (message & INITDATA_ADDRESS_MASK))
}

/// Extract the G17 EP 0x21 dispatch field from bits 59:44.
pub(crate) const fn runtime_message_type(message: u64) -> u16 {
    ((message >> RUNTIME_TYPE_SHIFT) & RUNTIME_TYPE_MASK) as u16
}

pub(crate) const fn validate_runtime_message_type(message_type: u16) -> Result<(), WireError> {
    if message_type < 32 && (RUNTIME_TYPE_VALID_MASK & (1u32 << message_type)) != 0 {
        Ok(())
    } else {
        Err(WireError::InvalidRuntimeType)
    }
}

/// Proven fields in the host-produced SKSM kick word.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct KickWord {
    pub(crate) stamp_index: u8,
    pub(crate) stamp: u32,
    pub(crate) queue_id: u8,
    /// Bits 39:0, matching the firmware's printed timestamp value.
    pub(crate) timestamp: u64,
    /// QID bit 6, used by firmware to select a valid-queue mask word.
    pub(crate) mask_selector: u8,
}

impl KickWord {
    /// Encode only the exact host-produced fields; bits 63:47 remain zero.
    pub(crate) const fn encode(
        queue_id: u8,
        stamp: u32,
        stamp_index: u8,
    ) -> Result<u64, WireError> {
        if queue_id > KICK_QUEUE_ID_MASK {
            return Err(WireError::QueueIdOutOfRange);
        }

        Ok((stamp_index as u64 & KICK_STAMP_INDEX_MASK)
            | ((stamp as u64) << KICK_STAMP_SHIFT)
            | ((queue_id as u64) << KICK_QUEUE_ID_SHIFT))
    }

    /// Decode a host-produced kick, refusing the unmodeled upper 17 bits.
    pub(crate) const fn decode_strict(word: u64) -> Result<Self, WireError> {
        if word & KICK_RESERVED_MASK != 0 {
            return Err(WireError::UnmodeledBits);
        }

        Ok(Self {
            stamp_index: (word & KICK_STAMP_INDEX_MASK) as u8,
            stamp: ((word >> KICK_STAMP_SHIFT) & u32::MAX as u64) as u32,
            queue_id: ((word >> KICK_QUEUE_ID_SHIFT) as u8) & KICK_QUEUE_ID_MASK,
            timestamp: word & KICK_TIMESTAMP_MASK,
            mask_selector: ((word >> 46) & 1) as u8,
        })
    }
}

/// Byte offset of one firmware queue record: `0x10 + QID * 0x20`.
pub(crate) const fn kick_queue_record_offset(queue_id: u8) -> Result<u32, WireError> {
    if queue_id > KICK_QUEUE_ID_MASK {
        Err(WireError::QueueIdOutOfRange)
    } else {
        Ok(KICK_QUEUE_RECORD_BASE + queue_id as u32 * KICK_QUEUE_RECORD_STRIDE)
    }
}

