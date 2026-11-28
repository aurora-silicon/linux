// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The services that get a child device, and the record of those that have one.
//!
//! The firmware announces each service it offers by name, with the channel
//! the service answers on. A service this driver knows gets one platform
//! device, named for it and described by the AOP's device-tree child of that
//! name. The record allows one device per known name and per service, so its
//! size is bounded by the table below, whatever the firmware announces.

use kernel::{prelude::*, soc::apple::aop::EPICService};

/// The announced names of the services that get a device, and the device's
/// name. Each device name appears once.
const DEVICES: [(&[u8], &CStr); 7] = [
    (b"aop-audio", c"audio"),
    (b"las", c"las"),
    (b"als", c"als"),
    (b"accel", c"accel"),
    (b"gyro", c"gyro"),
    (b"devmotion6", c"devmotion6"),
    (b"als-temp", c"als-temp"),
];

/// The services that have a device, registered or queued for registration,
/// each with its entry in [`DEVICES`].
pub(super) struct Claims(KVec<(EPICService, usize)>);

impl Claims {
    pub(super) fn new() -> Result<Self> {
        Ok(Self(KVec::with_capacity(DEVICES.len(), GFP_KERNEL)?))
    }

    /// Claims the device for a service announced as `name`. Returns the name
    /// of the device to register, or `None` when there is nothing to do: the
    /// service has no device, or it was announced before as it is now. A
    /// service whose name already has a device on another channel, or whose
    /// channel already has one under another name, gets none (`EEXIST`).
    pub(super) fn claim(
        &mut self,
        service: EPICService,
        name: &[u8],
    ) -> Result<Option<&'static CStr>> {
        let Some(index) = DEVICES.iter().position(|&(known, _)| known == name) else {
            return Ok(None);
        };
        for &(claimed, claimed_index) in self.0.iter() {
            match (claimed == service, claimed_index == index) {
                (true, true) => return Ok(None),
                (false, false) => {}
                _ => return Err(EEXIST),
            }
        }
        // Each entry of the table is claimed at most once, so this stays
        // within the capacity reserved for the whole table.
        self.0.push_within_capacity((service, index)).map_err(|_| ENOSPC)?;
        Ok(Some(DEVICES[index].1))
    }

    /// Gives up the claim of a service whose device could not be registered,
    /// so that the next announcement of the service tries again.
    pub(super) fn release(&mut self, service: EPICService) {
        self.0.retain(|&mut (claimed, _)| claimed != service);
    }
}

#[kunit_tests(apple_aop_services)]
mod tests {
    use super::*;

    fn svc(endpoint: u8, channel: u32) -> EPICService {
        EPICService { channel, endpoint }
    }

    #[test]
    fn known_services_get_their_device() -> Result {
        let mut claims = Claims::new()?;
        assert_eq!(claims.claim(svc(0x21, 1), b"aop-audio")?, Some(c"audio"));
        assert_eq!(claims.claim(svc(0x23, 1), b"als")?, Some(c"als"));
        assert_eq!(claims.claim(svc(0x23, 2), b"als-temp")?, Some(c"als-temp"));
        Ok(())
    }

    #[test]
    fn unknown_services_get_nothing() -> Result {
        let mut claims = Claims::new()?;
        assert_eq!(claims.claim(svc(0x20, 1), b"misc")?, None);
        assert_eq!(claims.claim(svc(0x20, 2), b"")?, None);
        assert_eq!(claims.claim(svc(0x20, 3), b"als\0")?, None);
        // The unknown announcements took no claim on the channel.
        assert_eq!(claims.claim(svc(0x20, 1), b"als")?, Some(c"als"));
        Ok(())
    }

    #[test]
    fn an_unchanged_announcement_adds_no_device() -> Result {
        let mut claims = Claims::new()?;
        assert_eq!(claims.claim(svc(0x23, 1), b"als")?, Some(c"als"));
        for _ in 0..100 {
            assert_eq!(claims.claim(svc(0x23, 1), b"als")?, None);
        }
        Ok(())
    }

    #[test]
    fn a_name_or_channel_with_a_device_is_refused() -> Result {
        let mut claims = Claims::new()?;
        assert_eq!(claims.claim(svc(0x23, 1), b"als")?, Some(c"als"));
        // The same name on another channel or endpoint.
        assert_eq!(claims.claim(svc(0x23, 2), b"als"), Err(EEXIST));
        assert_eq!(claims.claim(svc(0x24, 1), b"als"), Err(EEXIST));
        // The same channel under another name.
        assert_eq!(claims.claim(svc(0x23, 1), b"las"), Err(EEXIST));
        // Other services are unaffected.
        assert_eq!(claims.claim(svc(0x23, 2), b"las")?, Some(c"las"));
        Ok(())
    }

    #[test]
    fn devices_are_bounded_by_the_table() -> Result {
        let mut claims = Claims::new()?;
        let mut devices = 0;
        for channel in 0..64 {
            for &(name, _) in DEVICES.iter() {
                if let Ok(Some(_)) = claims.claim(svc(0x20 + (channel % 8) as u8, channel), name) {
                    devices += 1;
                }
            }
        }
        assert_eq!(devices, DEVICES.len());
        assert_eq!(claims.0.len(), DEVICES.len());
        Ok(())
    }

    #[test]
    fn a_released_service_can_be_claimed_again() -> Result {
        let mut claims = Claims::new()?;
        assert_eq!(claims.claim(svc(0x21, 1), b"aop-audio")?, Some(c"audio"));
        assert_eq!(claims.claim(svc(0x23, 1), b"als")?, Some(c"als"));
        claims.release(svc(0x21, 1));
        assert_eq!(claims.claim(svc(0x21, 1), b"aop-audio")?, Some(c"audio"));
        assert_eq!(claims.claim(svc(0x23, 1), b"als")?, None);
        Ok(())
    }
}
