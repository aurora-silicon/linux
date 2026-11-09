// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! SEP credential endpoint (SCRD, EP 0x0a) wire protocol.

use crate::proto::*;
use crate::sks::SKS_AUTH_TOKEN_LEN;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const SCRD_ACM_HANDLE_LEN: usize = SKS_AUTH_TOKEN_LEN;

pub(crate) const SCRD_REQUEST_COMMAND: u8 = 1;

const SCRD_MAGIC: [u8; 4] = *b"DRCS";

const SCRD_CMD_INITIALIZE: u8 = 0x0a;
const SCRD_CMD_CONTEXT_DELETE: u8 = 0x02;
const SCRD_CMD_CONTEXT_CREATE_TRACKED: u8 = 0x24;
const SCRD_CMD_CONTEXT_EXTERNALIZE: u8 = 0x13;
const SCRD_CMD_VERIFY_POLICY: u8 = 0x03;

const SCRD_INIT_LOG_LEVEL: u8 = 0x28;

const SCRD_POLICY_TOUCHID_ENROLLMENT: &[u8] = b"TouchIdEnrollment";

/// The policy an unlock evaluation asks for. Its context waits for a
/// biometric match instead of being satisfied in advance.
const SCRD_POLICY_DEVICE_OWNER_BIOMETRY: &[u8] = b"DeviceOwnerAuthenticationWithBiometrics";

/// One ACM parameter element, packed: u32 type, u32 value length, value.
const SCRD_PARAM_ELEMENT_LEN: usize = 12;
/// The parameter type of a positive user ID (a u32 value).
const SCRD_PARAM_USER_ID: u32 = 4;
const SCRD_UNLOCK_POLICY_LEN: usize =
    8 + SCRD_ACM_HANDLE_LEN + SCRD_POLICY_DEVICE_OWNER_BIOMETRY.len() + 1 + 1 + 4 + 4 + SCRD_PARAM_ELEMENT_LEN;
static_assert!(SCRD_UNLOCK_POLICY_LEN == 0x55);

const SCRD_MAX_LOGICAL: usize = 96;
static_assert!(SCRD_UNLOCK_POLICY_LEN <= SCRD_MAX_LOGICAL);

/// A tracked context-create reply: the reference, a u32 tracking payload
/// and a Boolean response byte.
pub(crate) const SCRD_TRACKED_CREATE_REPLY_LEN: usize = SCRD_ACM_HANDLE_LEN + 4 + 1;

/// The requirement an unlock preflight must leave open: a biometric match.
pub(crate) const SCRD_REQUIREMENT_BIOMETRY_MATCHED: u32 = 3;
static_assert!(
    8 + SCRD_ACM_HANDLE_LEN + SCRD_POLICY_TOUCHID_ENROLLMENT.len() + 1 + 1 + 8 <= SCRD_MAX_LOGICAL
);

pub(crate) struct ScrdCommand {
    request: u8,
    payload: [u8; SCRD_MAX_LOGICAL],
    payload_len: usize,
    name: &'static CStr,
}

impl ScrdCommand {
    pub(crate) fn request(&self) -> u8 {
        self.request
    }
    pub(crate) fn payload(&self) -> &[u8] {
        &self.payload[..self.payload_len]
    }
    pub(crate) fn name(&self) -> &'static CStr {
        self.name
    }
}

fn scrd_header(out: &mut [u8; SCRD_MAX_LOGICAL], command: u8, byte5: u8, version: u8) {
    out[0..4].copy_from_slice(&SCRD_MAGIC);
    out[4] = command;
    out[5] = byte5;
    out[6] = 0;
    out[7] = version;
}

pub(crate) fn scrd_initialize() -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_INITIALIZE, SCRD_INIT_LOG_LEVEL, 0);
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: 8,
        name: c"SCRD_INITIALIZE",
    }
}

pub(crate) fn scrd_context_create_tracked(session_uid: i32) -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_CONTEXT_CREATE_TRACKED, 0, 1);
    payload[8..12].copy_from_slice(&session_uid.to_le_bytes());
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: 12,
        name: c"SCRD_CONTEXT_CREATE",
    }
}

pub(crate) fn scrd_context_externalize(handle: &[u8; SCRD_ACM_HANDLE_LEN]) -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_CONTEXT_EXTERNALIZE, 0, 1);
    payload[8..8 + SCRD_ACM_HANDLE_LEN].copy_from_slice(handle);
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: 8 + SCRD_ACM_HANDLE_LEN,
        name: c"SCRD_EXTERNALIZE",
    }
}

pub(crate) fn scrd_verify_touchid_enrollment(handle: &[u8; SCRD_ACM_HANDLE_LEN]) -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_VERIFY_POLICY, 0, 1);
    let mut n = 8;
    payload[n..n + SCRD_ACM_HANDLE_LEN].copy_from_slice(handle);
    n += SCRD_ACM_HANDLE_LEN;
    payload[n..n + SCRD_POLICY_TOUCHID_ENROLLMENT.len()]
        .copy_from_slice(SCRD_POLICY_TOUCHID_ENROLLMENT);
    n += SCRD_POLICY_TOUCHID_ENROLLMENT.len();
    // then NUL, preflight(1), u32 flags, u32 param count -- all zero
    n += 1 + 1 + 4 + 4;
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: n,
        name: c"SCRD_VERIFY_POLICY",
    }
}

const SCRD_CMD_VERIFY_ACL_CONSTRAINT: u8 = 0x20;
const SCRD_ACL_VERSION: u8 = 2;

/// Command 0x20: evaluate one operation's access-control constraint (a DER
/// set, as in a key's ACL) on the context, as LocalAuthentication does before
/// and after a Touch ID match for a key operation. Laid out as macOS sends it:
/// header, context, u32 constraint length, constraint, u32 operation length,
/// operation, a 1 byte, two zero u32 words.
pub(crate) fn scrd_verify_acl_constraint(
    handle: &[u8; SCRD_ACM_HANDLE_LEN],
    constraint: &[u8],
    operation: &[u8],
) -> Result<KVec<u8>> {
    let mut out = KVec::new();
    let mut header = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut header, SCRD_CMD_VERIFY_ACL_CONSTRAINT, 0, SCRD_ACL_VERSION);
    out.extend_from_slice(&header[..8], GFP_KERNEL)?;
    out.extend_from_slice(handle, GFP_KERNEL)?;
    let clen = u32::try_from(constraint.len()).map_err(|_| EINVAL)?;
    out.extend_from_slice(&clen.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(constraint, GFP_KERNEL)?;
    let olen = u32::try_from(operation.len()).map_err(|_| EINVAL)?;
    out.extend_from_slice(&olen.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(operation, GFP_KERNEL)?;
    out.push(1, GFP_KERNEL)?;
    out.extend_from_slice(&[0u8; 8], GFP_KERNEL)?;
    Ok(out)
}

/// The reference of a tracked context-create reply, or `None` for any
/// other shape.
pub(crate) fn tracked_create_reference(reply: &[u8]) -> Option<[u8; SCRD_ACM_HANDLE_LEN]> {
    if reply.len() != SCRD_TRACKED_CREATE_REPLY_LEN || reply[SCRD_TRACKED_CREATE_REPLY_LEN - 1] > 1 {
        return None;
    }
    let mut handle = [0u8; SCRD_ACM_HANDLE_LEN];
    handle.copy_from_slice(&reply[..SCRD_ACM_HANDLE_LEN]);
    Some(handle)
}

pub(crate) fn scrd_context_delete(handle: &[u8; SCRD_ACM_HANDLE_LEN]) -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_CONTEXT_DELETE, 0, 1);
    payload[8..8 + SCRD_ACM_HANDLE_LEN].copy_from_slice(handle);
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: 8 + SCRD_ACM_HANDLE_LEN,
        name: c"SCRD_CONTEXT_DELETE",
    }
}

/// Command 3 for the unlock policy, with the positive user as its one
/// parameter. `preflight` asks what is still required without consuming
/// anything; the final call after a match completes the evaluation.
pub(crate) fn scrd_verify_unlock_policy(
    handle: &[u8; SCRD_ACM_HANDLE_LEN],
    user: i32,
    preflight: bool,
) -> ScrdCommand {
    let mut payload = [0u8; SCRD_MAX_LOGICAL];
    scrd_header(&mut payload, SCRD_CMD_VERIFY_POLICY, 0, 1);
    let mut n = 8;
    payload[n..n + SCRD_ACM_HANDLE_LEN].copy_from_slice(handle);
    n += SCRD_ACM_HANDLE_LEN;
    payload[n..n + SCRD_POLICY_DEVICE_OWNER_BIOMETRY.len()]
        .copy_from_slice(SCRD_POLICY_DEVICE_OWNER_BIOMETRY);
    n += SCRD_POLICY_DEVICE_OWNER_BIOMETRY.len() + 1;
    payload[n] = u8::from(preflight);
    n += 1;
    // Maximum global credential age: 0, the backend default.
    n += 4;
    payload[n..n + 4].copy_from_slice(&1u32.to_le_bytes());
    n += 4;
    payload[n..n + 4].copy_from_slice(&SCRD_PARAM_USER_ID.to_le_bytes());
    payload[n + 4..n + 8].copy_from_slice(&4u32.to_le_bytes());
    payload[n + 8..n + 12].copy_from_slice(&(user as u32).to_le_bytes());
    n += SCRD_PARAM_ELEMENT_LEN;
    ScrdCommand {
        request: SCRD_REQUEST_COMMAND,
        payload,
        payload_len: n,
        name: if preflight { c"SCRD_UNLOCK_PREFLIGHT" } else { c"SCRD_UNLOCK_POLICY" },
    }
}

/// A command-3 answer: the policy result and, when present, the first
/// requirement's type, state and flags.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct PolicyAnswer {
    pub(crate) satisfied: bool,
    pub(crate) requirement: Option<(u32, u32, u32)>,
}

pub(crate) fn parse_policy_answer(reply: &[u8]) -> Option<PolicyAnswer> {
    if reply.len() < 4 {
        return None;
    }
    let word = |at: usize| u32::from_le_bytes([reply[at], reply[at + 1], reply[at + 2], reply[at + 3]]);
    let satisfied = match word(0) {
        0 => false,
        1 => true,
        _ => return None,
    };
    let rest = reply.len() - 4;
    if rest == 0 {
        return Some(PolicyAnswer { satisfied, requirement: None });
    }
    if rest < 16 || word(16) as usize != rest - 16 {
        return None;
    }
    Some(PolicyAnswer {
        satisfied,
        requirement: Some((word(4), word(8), word(12))),
    })
}

pub(crate) struct ScrdReply {
    pub(crate) request: u8,
    pub(crate) response_size: u16,
    pub(crate) status: i32,
}

pub(crate) fn decode_scrd_reply(msg: &Message) -> ScrdReply {
    let b = msg.msg0.to_le_bytes();
    ScrdReply {
        request: b[1],
        response_size: u16::from_le_bytes([b[2], b[3]]),
        status: i32::from_le_bytes([b[4], b[5], b[6], b[7]]),
    }
}

pub(crate) fn encode_scrd(request: u8, len: usize) -> Message {
    let len = (len & 0xffff) as u16;
    let len = len.to_le_bytes();
    Message {
        msg0: u64::from_le_bytes([EP_SCRD, request, len[0], len[1], 0, 0, 0, 0]),
        msg1: 0,
    }
}

static_assert!(EP_SCRD == 0x0a);
static_assert!(SCRD_CMD_INITIALIZE == 0x0a);
static_assert!(SCRD_CMD_CONTEXT_CREATE_TRACKED == 0x24);
static_assert!(SCRD_CMD_CONTEXT_EXTERNALIZE == 0x13);
static_assert!(SCRD_CMD_VERIFY_POLICY == 0x03);
