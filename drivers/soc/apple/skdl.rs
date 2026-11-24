// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's kernel deny-list endpoint (`skdl`).
//!
//! The service behind it is CoreKDL: the enclave holds a signed list of
//! kernel extensions that must not be loaded while a protected service runs,
//! and macOS hands it a signed receipt of the extensions it has loaded. The
//! public `corekdld` manual names the protected services: Apple Pay and
//! FairPlay. At boot macOS negotiates the service's protocol version and
//! submits that receipt; later the CoreKDL daemon reads the installed list's
//! version and asks whether a protected service is allowed.
//!
//! Linux has neither a receipt the enclave can verify nor a protected
//! service, so the driver checks that the enclave offers the endpoint with
//! the layout it knows, reports it in `diag/skdl`, and sends nothing. It does
//! not negotiate a version just to see an answer: nothing shows that leaves
//! no state behind, and no read is meaningful without a receipt. A message on
//! the endpoint is counted as unhandled.
//!
//! The offender list the service returns has a fixed record layout. Its
//! length check is typed below, failing closed, for a client with a real
//! protected service; no unknown answer may ever read as "allowed".

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_SKDL: u8 = 0x17;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const SKDL_NAME: u32 = u32::from_be_bytes(*b"skdl");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word on every SEP we have booted (M1 Pro, M2, M4 Pro, A18 Pro): one page
/// each way.
pub(crate) const SKDL_LAYOUT: u32 = 0x0101_0101;

/// One offender record: code directory hash (20 bytes), team identifier
/// (10), bundle identifier hash (32) and bundle version (8).
pub(crate) const OFFENDER_RECORD_LEN: usize = 20 + 10 + 32 + 8;

/// The offender list's prefix: the record count and the record bytes, two
/// little-endian 64-bit words.
pub(crate) const OFFENDER_PREFIX_LEN: usize = 16;

/// Checks an offender list body: the prefix must say exactly as many record
/// bytes as its count needs, and the body must hold exactly those. Returns
/// the record count, or `None` for anything else, including overflow.
#[allow(dead_code)]
pub(crate) fn offender_count(body: &[u8]) -> Option<usize> {
    if body.len() < OFFENDER_PREFIX_LEN {
        return None;
    }
    let word = |at: usize| {
        let mut w = [0u8; 8];
        w.copy_from_slice(&body[at..at + 8]);
        u64::from_le_bytes(w)
    };
    let count = usize::try_from(word(0)).ok()?;
    let bytes = usize::try_from(word(8)).ok()?;
    if count.checked_mul(OFFENDER_RECORD_LEN)? != bytes
        || OFFENDER_PREFIX_LEN.checked_add(bytes)? != body.len()
    {
        return None;
    }
    Some(count)
}

impl SepData {
    /// Records the discovery messages of the endpoint.
    pub(crate) fn skdl_discovered(&self, f: &Fields) {
        if f.param != EP_SKDL {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.skdl_named.store(f.data_lo == SKDL_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.skdl_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Reports the endpoint once the bring-up has ended. Sends nothing.
    pub(crate) fn skdl_attach(&self) {
        let layout = self.skdl_layout.load(Relaxed);
        let offer = diag::Offer::of(
            self.endpoint_present(EP_SKDL),
            self.skdl_named.load(Relaxed),
            layout == SKDL_LAYOUT,
        );
        if offer == diag::Offer::Unknown {
            dev_dbg!(
                self.dev,
                "skdl: the endpoint advertises layout {:#010x}; left alone\n",
                layout
            );
        }
        diag::skdl(offer);
    }

    /// Nothing is asked of the service, so nothing it says is expected.
    pub(crate) fn on_skdl(&self, msg: Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "skdl: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_skdl)]
mod tests {
    use super::*;

    // The endpoint's discovery messages in our Linux boot logs (all four Macs).
    const DISCOVER_NAME: u64 = 0x736b_646c_1700_00fd;
    const DISCOVER_LAYOUT: u64 = 0x0101_0101_1701_00fd;

    fn list(count: u64, bytes: u64, body: usize) -> KVec<u8> {
        let mut v = KVec::new();
        v.extend_from_slice(&count.to_le_bytes(), GFP_KERNEL)
            .unwrap();
        v.extend_from_slice(&bytes.to_le_bytes(), GFP_KERNEL)
            .unwrap();
        v.resize(OFFENDER_PREFIX_LEN + body, 0, GFP_KERNEL).unwrap();
        v
    }

    #[test]
    fn the_discovery_words_name_the_endpoint_and_its_layout() {
        let name = decode(&Message {
            msg0: DISCOVER_NAME,
            msg1: 0,
        });
        assert_eq!(name.ep, EP_DISCOVER);
        assert_eq!(name.param, EP_SKDL);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, SKDL_NAME);
        let f = decode(&Message {
            msg0: DISCOVER_LAYOUT,
            msg1: 0,
        });
        assert_eq!(f.param, EP_SKDL);
        assert_eq!(f.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(
            diag::Offer::of(true, true, f.data_lo == SKDL_LAYOUT),
            diag::Offer::Idle
        );
    }

    #[test]
    fn an_offender_list_must_add_up() {
        assert_eq!(OFFENDER_RECORD_LEN, 70);
        assert_eq!(offender_count(&list(0, 0, 0)), Some(0));
        assert_eq!(offender_count(&list(2, 140, 140)), Some(2));
        // The byte count disagrees with the count, or the body with either.
        assert_eq!(offender_count(&list(2, 139, 139)), None);
        assert_eq!(offender_count(&list(2, 140, 141)), None);
        assert_eq!(offender_count(&list(1, 70, 69)), None);
        // Overflow in either sum.
        assert_eq!(offender_count(&list(u64::MAX, 70, 70)), None);
        assert_eq!(offender_count(&list(0, u64::MAX, 0)), None);
        assert_eq!(offender_count(&[0u8; 15]), None);
    }
}
