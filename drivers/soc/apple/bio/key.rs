// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Key-operation state, result delivery and expiry for the biometric session.

use super::*;
use crate::keys::{KeyRequest, Output};

const KEY_PLAINTEXT_LIFETIME_NS: u64 = 5 * 1_000_000_000;

const INFO_BIOMETRIC_KEYS: u32 = 1 << 1;
const INFO_BIOMETRIC_DECRYPT: u32 = 1 << 2;

pub(super) fn capabilities(supported: bool) -> u32 {
    if supported { INFO_BIOMETRIC_KEYS | INFO_BIOMETRIC_DECRYPT } else { 0 }
}

pub(super) struct Operation {
    request: Option<KeyRequest>,
    deadline_ns: u64,
    terminal: Option<Result<Output>>,
    decrypt: bool,
    guidance: Guidance,
}

impl Operation {
    pub(super) fn is_terminal(&self) -> bool {
        self.terminal.is_some()
    }

    pub(super) fn can_cancel(&self) -> bool {
        self.terminal.is_none()
            || matches!(&self.terminal, Some(Ok(crate::keys::Output::Plaintext(_))))
    }

    pub(super) fn cancel(&mut self) {
        self.terminal = Some(Err(ECANCELED));
    }

    pub(super) fn capture_guide(&mut self, guide: Guidance) -> bool {
        if self.terminal.is_none() && self.guidance != guide {
            self.guidance = guide;
            true
        } else {
            false
        }
    }
}

pub(crate) fn worker_done(session: &mut Session) {
    session.worker_active = false;
}

pub(crate) fn start_key(session: &mut Session, request: crate::keys::KeyRequest) -> Result<Handled> {
    if !session.available() {
        return Err(EBUSY);
    }
    session.op = Op::Key(Operation {
        deadline_ns: request.deadline_ns,
        decrypt: matches!(&request.input, crate::keys::Input::Decrypt(_)),
        request: Some(request),
        terminal: None,
        guidance: Guidance::Place,
    });
    session.token = None;
    session.worker_active = true;
    session.unseen = false;
    Ok(Handled { start_verify: true, ..ok()? })
}

pub(crate) fn is_key_operation(session: &Session) -> bool {
    matches!(session.op, Op::Key(_))
}

pub(crate) fn take_key_request(session: &mut Session) -> Option<crate::keys::KeyRequest> {
    let Op::Key(signing) = &mut session.op else { return None; };
    if signing.terminal.is_some() || !session.open { return None; }
    signing.request.take()
}

pub(crate) fn key_is_live(session: &Session) -> bool {
    let Op::Key(signing) = &session.op else { return false; };
    session.open && signing.terminal.is_none() && crate::shim::monotonic_ns() < signing.deadline_ns
}

pub(crate) fn finish_key(session: &mut Session, result: Result<crate::keys::Output>) -> bool {
    let Op::Key(signing) = &mut session.op else { return false; };
    if !session.open || signing.terminal.is_some() { return false; }
    let now = crate::shim::monotonic_ns();
    if matches!(&result, Ok(crate::keys::Output::Plaintext(_))) {
        signing.deadline_ns = signing.deadline_ns.min(now.saturating_add(KEY_PLAINTEXT_LIFETIME_NS));
    }
    signing.terminal = Some(if now >= signing.deadline_ns {
        Err(ETIMEDOUT)
    } else {
        result
    });
    session.unseen = true;
    true
}

/// Return the remaining delivery window, wiping expired plaintext.
/// The capture worker retains ownership until delivery, cancellation or expiry.
pub(crate) fn key_plaintext_wait_ns(session: &mut Session, now: u64) -> Option<u64> {
    let Op::Key(operation) = &mut session.op else { return None; };
    if !matches!(&operation.terminal, Some(Ok(crate::keys::Output::Plaintext(_)))) {
        return None;
    }
    if now >= operation.deadline_ns {
        operation.terminal = Some(Err(ETIMEDOUT));
        session.unseen = true;
        return None;
    }
    Some(operation.deadline_ns - now)
}

pub(crate) fn poll_sign(session: &mut Session, user: UserPtr) -> Result<Handled> {
    let Op::Key(signing) = &session.op else { return Err(ENOENT); };
    if signing.decrypt { return Err(EBUSY); }
    let mut out = crate::keys::SignPoll {
        state: State::Pending as u32,
        status: 0,
        guidance: signing.guidance as u32,
        sig_len: 0,
        signature: [0; ATTEST_SIG_MAX],
    };
    match &signing.terminal {
        Some(Ok(crate::keys::Output::Signature(signature))) => {
            out.state = State::Done as u32;
            out.sig_len = signature.len;
            out.signature = signature.bytes;
        }
        Some(Err(error)) => {
            out.state = State::Failed as u32;
            out.status = error.to_errno();
        }
        Some(Ok(_)) => return Err(EIO),
        None => {},
    }
    UserSlice::new(user, core::mem::size_of_val(&out)).writer().write(&out)?;
    session.unseen = false;
    ok()
}

#[kernel::macros::kunit_tests(apple_sep_sign_lifecycle)]
mod sign_tests {
    use super::*;

    fn request(deadline_ns: u64, decrypt: bool) -> crate::keys::KeyRequest {
        let input = if decrypt { crate::keys::Input::Decrypt(KVec::new()) }
            else { crate::keys::Input::Sign([0; 32]) };
        crate::keys::KeyRequest { blob: KVec::new(), input, deadline_ns }
    }

    #[test]
    fn undelivered_plaintext_expires_or_cancels() -> Result {
        for cancel in [false, true] {
            let mut session = Session::new();
            open(&mut session)?;
            start_key(&mut session, request(u64::MAX, true))?;
            let _ = take_key_request(&mut session);
            assert!(finish_key(&mut session, Ok(crate::keys::Output::Plaintext(crate::Secret(KVec::new())))));
            let Op::Key(operation) = &session.op else { return Err(EINVAL); };
            let deadline = operation.deadline_ns;
            assert_eq!(key_plaintext_wait_ns(&mut session, deadline - 1), Some(1));
            if cancel { assert!(abort_capture(&mut session)); }
            assert_eq!(key_plaintext_wait_ns(&mut session, deadline), None);
            let Op::Key(operation) = &session.op else { return Err(EINVAL); };
            let expected = if cancel { ECANCELED } else { ETIMEDOUT };
            assert!(matches!(&operation.terminal, Some(Err(error)) if *error == expected));
        }
        Ok(())
    }

    #[test]
    fn canceled_or_closed_worker_cannot_complete_a_new_request() -> Result {
        for (close, decrypt) in [(false, false), (true, false), (false, true), (true, true)] {
            let mut session = Session::new();
            open(&mut session)?;
            start_key(&mut session, request(u64::MAX, decrypt))?;
            assert!(start_key(&mut session, request(u64::MAX, decrypt)).is_err());
            assert!(take_key_request(&mut session).is_some());
            assert!(take_key_request(&mut session).is_none());
            if close { release(&mut session); } else { abort_capture(&mut session); }
            assert!(!key_is_live(&session));
            assert!(!finish_key(&mut session, Err(EIO)));
            assert!(!session.available());
            assert!(open(&mut session).is_err());
            worker_done(&mut session);
            if close { open(&mut session)?; }
            start_key(&mut session, request(0, decrypt))?;
            assert!(!key_is_live(&session));
            assert!(finish_key(&mut session, Err(EIO)));
            let Op::Key(signing) = &session.op else { return Err(EINVAL); };
            assert!(matches!(signing.terminal, Some(Err(ETIMEDOUT))));
        }
        Ok(())
    }
}

pub(crate) fn poll_decrypt(session: &mut Session, user: UserPtr) -> Result<Handled> {
    let mut out: crate::keys::DecryptPoll =
        UserSlice::new(user, core::mem::size_of::<crate::keys::DecryptPoll>()).reader().read()?;
    let capacity = out.plain_len as usize;
    if capacity > crate::keys::MAX_BLOB { return Err(EINVAL); }
    let Op::Key(operation) = &mut session.op else { return Err(ENOENT); };
    if !operation.decrypt { return Err(EBUSY); }
    if crate::shim::monotonic_ns() >= operation.deadline_ns {
        operation.terminal = Some(Err(ETIMEDOUT));
    }
    out.state = State::Pending as u32;
    out.status = 0;
    out.guidance = operation.guidance as u32;
    out.plain_len = 0;
    let mut consume = false;
    match &operation.terminal {
        Some(Ok(crate::keys::Output::Plaintext(plain))) => {
            out.state = State::Done as u32;
            out.plain_len = plain.len() as u32;
            if capacity < plain.len() {
                UserSlice::new(user, core::mem::size_of_val(&out)).writer().write(&out)?;
                return Err(ENOSPC);
            }
            UserSlice::new(UserPtr::from_addr(out.plain_ptr as usize), capacity)
                .writer().write_slice(plain)?;
            consume = true;
        }
        Some(Ok(crate::keys::Output::Consumed)) => return Err(ENODATA),
        Some(Ok(_)) => return Err(EIO),
        Some(Err(error)) => {
            out.state = State::Failed as u32;
            out.status = error.to_errno();
        }
        None => {},
    }
    UserSlice::new(user, core::mem::size_of_val(&out)).writer().write(&out)?;
    if consume { operation.terminal = Some(Ok(crate::keys::Output::Consumed)); }
    session.unseen = false;
    ok()
}
