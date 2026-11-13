// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's hibernation endpoint (`hibe`).
//!
//! The service behind it protects a hibernation image: before the image is
//! committed it hands the host a wrapped image key and a resume
//! authorisation, and on the cold boot that resumes the image it takes the
//! authorisation back and decides whether the resume may go on. That gives
//! the saved memory image the enclave's protection against disclosure,
//! modification and replay. Ordinary suspend never uses it: macOS sends
//! nothing on the endpoint at boot, at idle, at display sleep or across
//! system sleep (our 11-12 traces on a J616s under macOS 27 agree).
//!
//! Linux writes no hibernation image on these machines, so the driver does
//! what macOS does at boot and nothing more: it checks that the enclave
//! offers the endpoint with the layout it knows (one page each way) and
//! reports it in `diag/hibe`. No request is sent and no buffer is
//! registered. The two exchanges a hibernation writer would need are typed
//! below by their fixed layouts; they are not sent until such a writer exists
//! and their mailbox words come from our own recording of a hibernation. A
//! message on the endpoint is counted as unhandled.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_HIBE: u8 = 0x14;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const HIBE_NAME: u32 = u32::from_be_bytes(*b"hibe");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word on every SEP we have booted (M1 Pro, M2, M4 Pro, A18 Pro): one page
/// each way.
pub(crate) const HIBE_LAYOUT: u32 = 0x0101_0101;

/// Sizes of the opaque objects the service hands out and takes back. The
/// host never interprets them: neither is a plain key.
pub(crate) const WRAPPED_KEY_LEN: usize = 64;
pub(crate) const RESUME_AUTH_LEN: usize = 64;

/// The prepare reply: the wrapped image key, a boot word for the image
/// metadata, then the resume authorisation, at fixed offsets.
pub(crate) const PREPARE_REPLY_LEN: usize = WRAPPED_KEY_LEN + 4 + RESUME_AUTH_LEN;
static_assert!(PREPARE_REPLY_LEN <= 0x4000 && RESUME_AUTH_LEN <= 0x4000);

/// What the service gives a hibernation writer before the image is committed.
/// Kept by the writer until the resume, then cleared.
#[allow(dead_code)]
pub(crate) struct Prepared {
    pub(crate) wrapped_image_key: [u8; WRAPPED_KEY_LEN],
    pub(crate) boot_word: u32,
    pub(crate) resume_authorization: [u8; RESUME_AUTH_LEN],
}

/// Splits a prepare reply at its fixed boundaries. `None` unless `bytes` is
/// exactly one reply long: the layout has no length field to trust.
#[allow(dead_code)]
pub(crate) fn split_prepare_reply(bytes: &[u8]) -> Option<Prepared> {
    if bytes.len() != PREPARE_REPLY_LEN {
        return None;
    }
    let mut wrapped_image_key = [0u8; WRAPPED_KEY_LEN];
    wrapped_image_key.copy_from_slice(&bytes[..WRAPPED_KEY_LEN]);
    let at = WRAPPED_KEY_LEN;
    let boot_word = u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
    let mut resume_authorization = [0u8; RESUME_AUTH_LEN];
    resume_authorization.copy_from_slice(&bytes[at + 4..]);
    Some(Prepared {
        wrapped_image_key,
        boot_word,
        resume_authorization,
    })
}

impl SepData {
    /// Records the discovery messages of the endpoint.
    pub(crate) fn hibe_discovered(&self, f: &Fields) {
        if f.param != EP_HIBE {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.hibe_named.store(f.data_lo == HIBE_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.hibe_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Reports the endpoint once the bring-up has ended. Sends nothing.
    pub(crate) fn hibe_attach(&self) {
        let layout = self.hibe_layout.load(Relaxed);
        let offer = diag::Offer::of(
            self.endpoint_present(EP_HIBE),
            self.hibe_named.load(Relaxed),
            layout == HIBE_LAYOUT,
        );
        if offer == diag::Offer::Unknown {
            dev_dbg!(
                self.dev,
                "hibe: the endpoint advertises layout {:#010x}; left alone\n",
                layout
            );
        }
        diag::hibe(offer);
    }

    /// Nothing is asked of the service, so nothing it says is expected.
    pub(crate) fn on_hibe(&self, msg: Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "hibe: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_hibe)]
mod tests {
    use super::*;

    // The endpoint's discovery messages in our Linux boot logs (all four Macs).
    const DISCOVER_NAME: u64 = 0x6869_6265_1400_00fd;
    const DISCOVER_LAYOUT: u64 = 0x0101_0101_1401_00fd;

    #[test]
    fn the_discovery_words_name_the_endpoint_and_its_layout() {
        let name = decode(&Message {
            msg0: DISCOVER_NAME,
            msg1: 0,
        });
        assert_eq!(name.ep, EP_DISCOVER);
        assert_eq!(name.param, EP_HIBE);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, HIBE_NAME);
        let f = decode(&Message {
            msg0: DISCOVER_LAYOUT,
            msg1: 0,
        });
        assert_eq!(f.param, EP_HIBE);
        assert_eq!(f.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(
            diag::Offer::of(true, true, f.data_lo == HIBE_LAYOUT),
            diag::Offer::Idle
        );
    }

    #[test]
    fn only_a_named_endpoint_with_the_known_layout_is_offered() {
        assert_eq!(diag::Offer::of(false, false, false), diag::Offer::Absent);
        assert_eq!(diag::Offer::of(true, false, true), diag::Offer::Unknown);
        assert_eq!(diag::Offer::of(true, true, false), diag::Offer::Unknown);
    }

    #[test]
    fn a_prepare_reply_splits_at_fixed_offsets() {
        let mut r = [0u8; PREPARE_REPLY_LEN];
        r[..WRAPPED_KEY_LEN].fill(0x11);
        r[WRAPPED_KEY_LEN..WRAPPED_KEY_LEN + 4].copy_from_slice(&0x0403_0201u32.to_le_bytes());
        r[WRAPPED_KEY_LEN + 4..].fill(0x22);
        let p = split_prepare_reply(&r).unwrap();
        assert_eq!(p.wrapped_image_key, [0x11; WRAPPED_KEY_LEN]);
        assert_eq!(p.boot_word, 0x0403_0201);
        assert_eq!(p.resume_authorization, [0x22; RESUME_AUTH_LEN]);
        assert!(split_prepare_reply(&r[..PREPARE_REPLY_LEN - 1]).is_none());
        let mut long = [0u8; PREPARE_REPLY_LEN + 1];
        long[..PREPARE_REPLY_LEN].copy_from_slice(&r);
        assert!(split_prepare_reply(&long).is_none());
    }
}
