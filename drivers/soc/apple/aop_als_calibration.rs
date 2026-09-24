// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Loads the machine-supplied T8140 CT817 ALS initialization payload.

use kernel::{device::Device, error::code::EINVAL, error::Result, firmware::Firmware};

const PAYLOAD_SIZE: usize = 80;
const OPERATION: u64 = 0x0746_b8d6_6515_2e31;
const BODY_LENGTH: u64 = 56;

/// Loads and structurally validates the optional ALS initialization payload.
///
/// Firmware-loader errors are propagated unchanged. A loaded payload with an
/// invalid size, operation, or declared body length returns `EINVAL`.
///
/// The returned handle owns the buffer. Keep it alive while borrowing its data,
/// and pass all 80 bytes to the transport without altering the opaque contents.
pub(crate) fn load(dev: &Device) -> Result<Firmware> {
    let firmware = Firmware::request_nowarn(c"apple/t8140-ct817-cal.bin", dev)?;
    validate(firmware.data())?;
    Ok(firmware)
}

/// Checks only the structural requirements supplied by the transport contract.
pub(crate) fn validate(data: &[u8]) -> Result<()> {
    // Check the exact outer size before indexing any bytes.
    if data.len() != PAYLOAD_SIZE {
        return Err(EINVAL);
    }

    if data[..8] != OPERATION.to_le_bytes() || data[8..16] != BODY_LENGTH.to_le_bytes() {
        return Err(EINVAL);
    }

    // Bytes 16..80 are opaque and deliberately have no content restrictions.
    Ok(())
}
