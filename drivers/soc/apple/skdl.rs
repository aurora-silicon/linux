// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's kernel deny-list endpoint (`skdl`).
//!
//! The service behind it is CoreKDL: the enclave holds a signed list of
//! kernel extensions that must not be loaded while a protected service runs,
//! and macOS hands it a signed receipt of the extensions it has loaded. The
//! public `corekdld` manual names the protected services: Apple Pay and
//! FairPlay. At boot macOS negotiates the service's protocol version and
//! submits that receipt over the generic-transfer framing shared with `sbio`
//! and `stac` (see `transfer.rs`); later the CoreKDL daemon reads the
//! installed list's version and asks whether a protected service is allowed.
//!
//! Linux has neither a receipt the enclave can verify nor an Apple Pay or
//! FairPlay consumer, so the driver checks that the enclave offers the
//! endpoint with the layout it knows, reports it in `diag/skdl`, and sends
//! nothing. Leaving it attach-only causes no enclave error, timeout or
//! suspend/wake failure. It does not negotiate a version just to see an
//! answer: nothing shows that leaves no state behind, and no read is
//! meaningful without an authentic receipt. A message on the endpoint is
//! counted as unhandled.
//!
//! The boot framing observed on our own J473 macOS 27.2 captures and the
//! fixed-size offender list check are typed below, failing closed, for any
//! future consumer with a genuine receipt path; no unknown answer may ever
//! read as "allowed":
//!
//! ```text
//! TX 0x0000_0000_0001_fc17   endpoint 0x17, first packet, op 0x01 (set version), 8-byte u64 body
//! RX 0x0000_0000_0001_fc17   echoed word, empty status-0 packet
//! TX 0x0000_0000_0006_fc17   endpoint 0x17, first packet, op 0x06 (submit receipt),
//!                            u64 len + signed receipt bytes
//! RX 0x0000_0000_0006_fc17   echoed word, empty status-0 packet
//! ```

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_SKDL: u8 = 0x17;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const SKDL_NAME: u32 = u32::from_be_bytes(*b"skdl");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word on every SEP we have booted (M1 Pro, M2, M3, M4, M4 Pro, A18 Pro):
/// one page each way.
pub(crate) const SKDL_LAYOUT: u32 = 0x0101_0101;

/// Each out-of-line buffer: one 16 KiB enclave page, as the layout says.
#[allow(dead_code)]
pub(crate) const OOL_SIZE_SKDL: usize = 0x4000;

/// Boot operations observed over generic transfer on our J473 macOS 27.2
/// captures: protocol version negotiation and kernel-extension receipt
/// submission. Neither is sent by Linux.
#[allow(dead_code)]
pub(crate) const OP_SET_VERSION: u16 = 0x01;
#[allow(dead_code)]
pub(crate) const OP_SUBMIT_RECEIPT: u16 = 0x06;

#[allow(dead_code)]
pub(crate) const VERSION_BODY_LEN: usize = 8;
#[allow(dead_code)]
pub(crate) const VERSION_REQUEST_LEN: usize = transfer::HEADER_LEN + VERSION_BODY_LEN;
static_assert!(VERSION_REQUEST_LEN <= OOL_SIZE_SKDL);

/// One offender record: code directory hash (20 bytes), team identifier
/// (10), bundle identifier hash (32) and bundle version (8).
pub(crate) const OFFENDER_RECORD_LEN: usize = 20 + 10 + 32 + 8;

/// The offender list's prefix: the record count and the record bytes, two
/// little-endian 64-bit words.
pub(crate) const OFFENDER_PREFIX_LEN: usize = 16;

#[allow(dead_code)]
pub(crate) const fn encode_skdl(opcode: u16, marker: u8, seq: u16) -> Message {
    Message {
        msg0: (EP_SKDL as u64)
            | ((marker as u64) << MSG_TAG_SHIFT)
            | ((opcode as u64) << MSG_TYPE_SHIFT)
            | ((seq as u64) << 48),
        msg1: 0,
    }
}

// The two boot request words recorded on J473 under macOS 27.2.
static_assert!(
    encode_skdl(OP_SET_VERSION, transfer::MARKER_FIRST, 0).msg0 == 0x0000_0000_0001_fc17
);
static_assert!(
    encode_skdl(OP_SUBMIT_RECEIPT, transfer::MARKER_FIRST, 0).msg0 == 0x0000_0000_0006_fc17
);

/// Builds the out-of-line packet for protocol-version negotiation.
#[allow(dead_code)]
pub(crate) fn set_version_request(version: u64) -> [u8; VERSION_REQUEST_LEN] {
    let mut out = [0u8; VERSION_REQUEST_LEN];
    let packet = transfer::Packet {
        version: transfer::VERSION,
        total: VERSION_BODY_LEN as u32,
        offset: 0,
        flags: 0,
        err: 0,
        opcode: u32::from(OP_SET_VERSION),
        chunk: VERSION_BODY_LEN as u32,
    };
    let _ = packet.encode(&mut out);
    out[transfer::HEADER_LEN..].copy_from_slice(&version.to_le_bytes());
    out
}

/// What a `skdl` mailbox word is, before its out-of-line header is checked.
#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Arrival {
    /// A data packet or error report for `opcode`.
    Reply { opcode: u16, error: bool },
    /// Anything else: flow control, another endpoint, or an unknown tag.
    Other,
}

#[allow(dead_code)]
pub(crate) fn classify(msg: &Message) -> Arrival {
    let f = decode(msg);
    if f.ep != EP_SKDL {
        return Arrival::Other;
    }
    let opcode = u16::from(f.ty) | (u16::from(f.param) << 8);
    match f.tag {
        transfer::MARKER_FIRST => Arrival::Reply {
            opcode,
            error: false,
        },
        transfer::MARKER_ERROR => Arrival::Reply {
            opcode,
            error: true,
        },
        _ => Arrival::Other,
    }
}

/// Result of checking an empty-body `skdl` reply.
#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    Accepted,
    Refused(u32),
    Malformed,
}

/// Validates a reply that must carry no body (version negotiation or receipt
/// submission). Fails closed on any mismatch or unexpected payload.
#[allow(dead_code)]
pub(crate) fn check_empty_reply(
    expected_opcode: u16,
    error: bool,
    packet: &transfer::Packet,
) -> Outcome {
    if error {
        return Outcome::Refused(packet.err);
    }
    if packet.version != transfer::VERSION || packet.opcode != u32::from(expected_opcode) {
        return Outcome::Malformed;
    }
    if packet.err != 0 {
        return Outcome::Refused(packet.err);
    }
    if packet.total != 0 || packet.chunk != 0 || packet.offset != 0 {
        return Outcome::Malformed;
    }
    Outcome::Accepted
}

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

    #[test]
    fn boot_generic_transfer_framing_matches_observed_words() -> Result {
        let msg = encode_skdl(OP_SET_VERSION, transfer::MARKER_FIRST, 0);
        assert_eq!(msg.msg0, 0x0000_0000_0001_fc17);
        assert_eq!(msg.msg1, 0);
        assert_eq!(
            classify(&msg),
            Arrival::Reply {
                opcode: OP_SET_VERSION,
                error: false
            }
        );

        let bytes = set_version_request(1);
        let pkt = transfer::Packet::decode(&bytes)?;
        assert_eq!(pkt.version, transfer::VERSION);
        assert_eq!(pkt.opcode, u32::from(OP_SET_VERSION));
        assert_eq!(pkt.total, 8);
        assert_eq!(pkt.chunk, 8);
        assert_eq!(pkt.offset, 0);
        assert_eq!(pkt.err, 0);
        assert_eq!(&bytes[transfer::HEADER_LEN..], &1u64.to_le_bytes());

        let receipt = encode_skdl(OP_SUBMIT_RECEIPT, transfer::MARKER_FIRST, 0);
        assert_eq!(receipt.msg0, 0x0000_0000_0006_fc17);
        assert_eq!(
            classify(&receipt),
            Arrival::Reply {
                opcode: OP_SUBMIT_RECEIPT,
                error: false
            }
        );
        Ok(())
    }

    #[test]
    fn non_reply_words_and_other_endpoints_classify_as_other() {
        for tag in [transfer::MARKER_NEXT, transfer::MARKER_REQUEST, 0x01] {
            let msg = encode_skdl(OP_SET_VERSION, tag, 0);
            assert_eq!(classify(&msg), Arrival::Other);
        }
        let stac_msg = Message {
            msg0: 0x0000_0000_000f_fc18,
            msg1: 0,
        };
        assert_eq!(classify(&stac_msg), Arrival::Other);
        let err_msg = encode_skdl(OP_SET_VERSION, transfer::MARKER_ERROR, 0);
        assert_eq!(
            classify(&err_msg),
            Arrival::Reply {
                opcode: OP_SET_VERSION,
                error: true
            }
        );
    }

    #[test]
    fn empty_reply_validation_fails_closed() {
        let ok = transfer::Packet {
            version: transfer::VERSION,
            total: 0,
            offset: 0,
            flags: 0,
            err: 0,
            opcode: u32::from(OP_SET_VERSION),
            chunk: 0,
        };
        assert_eq!(check_empty_reply(OP_SET_VERSION, false, &ok), Outcome::Accepted);
        assert_eq!(
            check_empty_reply(OP_SET_VERSION, true, &ok),
            Outcome::Refused(0)
        );
        assert_eq!(
            check_empty_reply(OP_SET_VERSION, false, &transfer::Packet { err: 5, ..ok }),
            Outcome::Refused(5)
        );
        assert_eq!(
            check_empty_reply(OP_SUBMIT_RECEIPT, false, &ok),
            Outcome::Malformed
        );
        assert_eq!(
            check_empty_reply(
                OP_SET_VERSION,
                false,
                &transfer::Packet {
                    version: transfer::VERSION + 1,
                    ..ok
                }
            ),
            Outcome::Malformed
        );
        assert_eq!(
            check_empty_reply(
                OP_SET_VERSION,
                false,
                &transfer::Packet {
                    total: 8,
                    chunk: 8,
                    ..ok
                }
            ),
            Outcome::Malformed
        );
    }
}
