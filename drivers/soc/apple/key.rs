// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray
//! `/dev/sep-key`: P-256 keys created inside the SEP.
//!
//! Each key is an enclave ref-key bound to this SEP and to the Linux identity
//! key bag. Its private half never leaves the enclave; userspace holds only a
//! key handle, the wrapped ref-key record behind a small versioned header.
//! A handle works on the Mac that created it and nowhere else. Signing takes a
//! 32-byte digest and returns a DER `ECDSA-Sig-Value`.
//!
//! A key made with `CREATE_BIOMETRY_CURRENT_SET` carries an access-control
//! list in its record, the one macOS gives a Secure Enclave key made with
//! `.biometryCurrentSet`: signing (and key agreement) needs a credential
//! context in which the enclave itself saw a match of one of the fingers
//! enrolled now. The kernel opens that context inside the `SIGN` call, runs a
//! Touch ID match in it, signs, and deletes it before the call returns. No
//! handle to it ever reaches userspace, and every signature needs its own
//! touch. The enclave refuses the signature for any other context; the
//! header's flag only tells the kernel to ask for the touch.
//!
//! `CREATE_BIOMETRY_ANY` is macOS's `.biometryAny`: the same list without
//! the enrolled-set hash, so any finger enrolled in this catacomb matches,
//! including fingers enrolled after the key was made.
//!
//! Interface version 3 adds three calls. `CANCEL` ends a touch-gated call
//! that is waiting for a finger (it returns `ECANCELED`). `SEAL` encrypts up
//! to 64 bytes to a biometric key's public point (ECIES, done here: it needs
//! no private key and no touch). `UNSEAL` has the enclave do the key exchange
//! (`ock`) of such a blob's ephemeral point with the key, under the same
//! fresh, touch-satisfied credential context as a `SIGN`, and finishes the
//! AES-GCM decrypt in the kernel, as macOS does for Secure Enclave keys: the
//! key's access-control list constrains the key exchange as it does the
//! signature, so a sealed secret is released only after a match the enclave
//! saw. Userspace uses it for secrets that must not exist outside the
//! enclave without the user (FIDO hmac-secret).
//!
//! macOS's `.userPresence` (Touch ID or the password) and `.devicePasscode`
//! need a passcode credential the enclave checks against the user's own
//! secret. On Linux the only passcode the enclave knows today is the identity
//! key bag's secret, which root holds, so those policies would be
//! root-satisfiable. Their `CREATE` bits are reserved and refused with
//! `EOPNOTSUPP` until a per-user, password-keyed bag exists; they are never
//! emulated in software.

use crate::der::RefKeyValue as RV;
use crate::SepData;
use kernel::ioctl::{_IO, _IOR, _IOWR};
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;
use kernel::uaccess::{UserPtr, UserSlice};

pub(crate) const DEVICE_NAME: &CStr = c"sep-key";
/// Root only by default; distributions grant a group with a udev rule.
pub(crate) const DEVICE_MODE: u16 = 0o600;

pub(crate) const IFACE_VERSION: u32 = 3;

/// `INFO.features` (version 3): `CANCEL` is available.
pub(crate) const FEATURE_CANCEL: u32 = 1;
/// `INFO.features`: `SEAL` and `UNSEAL` are available.
pub(crate) const FEATURE_SEAL: u32 = 2;
const FEATURES: u32 = FEATURE_CANCEL | FEATURE_SEAL;

/// The largest secret `SEAL` takes and `UNSEAL` returns.
pub(crate) const SECRET_MAX: usize = 64;
/// The largest sealed blob: ephemeral point, ciphertext and tag fit easily.
pub(crate) const SEALED_MAX: usize = 256;

pub(crate) const HANDLE_MAX: usize = 2048;
pub(crate) const PUB_LEN: usize = 65;
pub(crate) const DIGEST_LEN: usize = 32;
pub(crate) const SIG_MAX: usize = 72;

/// `CREATE` flag: signing needs a Touch ID match of a currently enrolled
/// finger, enforced by the enclave.
pub(crate) const CREATE_BIOMETRY_CURRENT_SET: u32 = 1;
/// `CREATE` flag: signing needs a Touch ID match of any finger enrolled in
/// this catacomb, now or later, enforced by the enclave.
pub(crate) const CREATE_BIOMETRY_ANY: u32 = 2;
/// Reserved `CREATE` flags for macOS's `.userPresence` and `.devicePasscode`.
/// Refused with `EOPNOTSUPP` until the enclave can check the user's own
/// password (see the module comment).
pub(crate) const CREATE_USER_PRESENCE: u32 = 4;
pub(crate) const CREATE_DEVICE_PASSCODE: u32 = 8;
const CREATE_FLAGS: u32 = CREATE_BIOMETRY_CURRENT_SET | CREATE_BIOMETRY_ANY;
const CREATE_RESERVED: u32 = CREATE_USER_PRESENCE | CREATE_DEVICE_PASSCODE;

/// The access policy a `CREATE` request asks for: at most one policy bit.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Policy {
    None,
    BiometryCurrentSet,
    BiometryAny,
}

pub(crate) fn create_policy(flags: u32) -> Result<Policy> {
    if flags & !(CREATE_FLAGS | CREATE_RESERVED) != 0 || flags.count_ones() > 1 {
        return Err(EINVAL);
    }
    match flags {
        0 => Ok(Policy::None),
        CREATE_BIOMETRY_CURRENT_SET => Ok(Policy::BiometryCurrentSet),
        CREATE_BIOMETRY_ANY => Ok(Policy::BiometryAny),
        _ => Err(EOPNOTSUPP),
    }
}

/// A touch-gated `SIGN` asks for a finger again after a capture that saw
/// none until this long has passed; one capture itself waits up to a minute.
const TOUCH_WAIT_MS: u64 = 60_000;

const MAGIC: u32 = 0xB1;

/// Handle header: `"SEPK"`, version, flags, reserved u16, blob length (LE u32).
const HANDLE_MAGIC: &[u8; 4] = b"SEPK";
const HANDLE_VERSION: u8 = 1;
/// Handle flag: the record carries a biometric access-control list.
pub(crate) const HANDLE_BIOMETRY: u8 = 0x01;
/// Handle flag, with `HANDLE_BIOMETRY` only: the list names no enrolled-set
/// hash (biometry-any).
pub(crate) const HANDLE_BIOMETRY_ANY: u8 = 0x02;
pub(crate) const HANDLE_HEADER: usize = 12;
/// The largest enclave record a handle can carry.
pub(crate) const BLOB_MAX: usize = HANDLE_MAX - HANDLE_HEADER;

#[repr(C)]
pub(crate) struct Info {
    pub(crate) version: u32,
    /// `flags` bits `CREATE` accepts.
    pub(crate) flags_supported: u32,
    pub(crate) handle_max: u32,
    /// `FEATURE_*` (version 3; reserved and zero before).
    pub(crate) features: u32,
}

#[repr(C)]
pub(crate) struct Create {
    pub(crate) flags: u32,
    pub(crate) handle_len: u32,
    pub(crate) public_key: [u8; PUB_LEN],
    pub(crate) reserved: [u8; 3],
    pub(crate) handle: [u8; HANDLE_MAX],
}

#[repr(C)]
pub(crate) struct Sign {
    pub(crate) flags: u32,
    pub(crate) handle_len: u32,
    pub(crate) digest: [u8; DIGEST_LEN],
    pub(crate) sig_len: u32,
    pub(crate) signature: [u8; SIG_MAX],
    pub(crate) handle: [u8; HANDLE_MAX],
}

#[repr(C)]
pub(crate) struct Public {
    pub(crate) flags: u32,
    pub(crate) handle_len: u32,
    pub(crate) public_key: [u8; PUB_LEN],
    pub(crate) reserved: [u8; 3],
    pub(crate) handle: [u8; HANDLE_MAX],
}

/// `SEAL` (secret in, sealed out) and `UNSEAL` (sealed in, secret out).
#[repr(C)]
pub(crate) struct Seal {
    /// In: 0.
    pub(crate) flags: u32,
    pub(crate) handle_len: u32,
    pub(crate) secret_len: u32,
    pub(crate) sealed_len: u32,
    pub(crate) secret: [u8; SECRET_MAX],
    pub(crate) sealed: [u8; SEALED_MAX],
    pub(crate) handle: [u8; HANDLE_MAX],
}

/// Test only (module parameter `key_selftest`, root): sign with a
/// biometric key under a deliberately unsuitable context.
#[repr(C)]
pub(crate) struct Selftest {
    pub(crate) mode: u32,
    pub(crate) handle_len: u32,
    pub(crate) digest: [u8; DIGEST_LEN],
    /// 0 when the enclave signed, else the negative errno.
    pub(crate) result: i32,
    /// The enclave's status for that signature.
    pub(crate) enclave_status: i32,
    /// `SELFTEST_TOUCH_THEN_DELETED`: the second signature.
    pub(crate) result2: i32,
    pub(crate) enclave_status2: i32,
    pub(crate) handle: [u8; HANDLE_MAX],
}

/// No credential context at all.
pub(crate) const SELFTEST_NO_CONTEXT: u32 = 1;
/// A fresh context in which nothing has matched.
pub(crate) const SELFTEST_UNSATISFIED: u32 = 2;
/// A context holding only the key bag's secret (Linux's "passcode").
pub(crate) const SELFTEST_PASSCODE: u32 = 3;
/// A fresh context, deleted before the signature.
pub(crate) const SELFTEST_DELETED: u32 = 4;
/// One touch: sign in the matched context, delete it, sign again with it
/// (needs a finger).
pub(crate) const SELFTEST_TOUCH_THEN_DELETED: u32 = 5;
/// Research probe, no finger, ignores the handle: make a throwaway key with
/// macOS's `.userPresence` list (`cpo` "DeviceOwnerAuthentication") and sign
/// with a context holding only the key bag's secret (`result`) and with an
/// unsatisfied context (`result2`). Shows whether the enclave enforces that
/// policy and whether a root-held secret satisfies it; `CREATE` refuses the
/// policy either way.
pub(crate) const SELFTEST_PRESENCE_PROBE: u32 = 6;
/// Research probe, no finger: the `.userPresence` throwaway key (identity
/// bag) signed with a context whose passcode credential came from another
/// user's bag. In: `enclave_status` = that user's uid, the handle buffer =
/// that user's bag password (`handle_len` bytes). Out: `result2` = the
/// verify-secret status, `result`/`enclave_status` = the signature. Shows
/// whether a credential from one bag satisfies a key under another.
pub(crate) const SELFTEST_PRESENCE_OTHER_BAG: u32 = 7;
/// Research probe, no finger: a throwaway `.userPresence` key made under
/// the (unlocked) bag of the user in `enclave_status` (outer handle -uid).
/// `result`: signed with a context holding a passcode credential from that
/// same bag (the password in the handle buffer). `result2`: signed with a
/// fresh, unsatisfied context. Statuses in `enclave_status`/`2`.
pub(crate) const SELFTEST_USERBAG_KEY_SAME_BAG: u32 = 8;
/// As 8, but `result` signs with the Linux identity bag's secret as the
/// credential (what root holds) and `result2` with the handle buffer's
/// password used as given (pass a wrong one).
pub(crate) const SELFTEST_USERBAG_KEY_ROOT: u32 = 9;
/// Research probe, no finger (T2: a locked bag refuses without its
/// password): a throwaway `.userPresence` key under the unlocked bag of the
/// uid in `enclave_status`, password in the handle buffer. The kernel signs
/// (r0) with a context holding that bag's password credential, locks the bag
/// for good (screen lock, grace expired now), then signs with the same, now
/// stale, context (r1), with no credential (r2) and with a fresh password
/// credential (r3), unlocks the bag with the password and signs with a fresh
/// credential again (r4). The digest buffer returns, as LE i32: r0..r4 (0 =
/// signed, else the enclave status), the lock phase before the lock, after
/// it and after the unlock.
pub(crate) const SELFTEST_USERBAG_LOCKED: u32 = 10;
/// Research: create a throwaway `.userPresence` key under the unlocked bag
/// of the uid in `enclave_status` and return its record in the handle
/// buffer (`handle_len`), so a later boot can use it after the bag is
/// imported again.
pub(crate) const SELFTEST_USERBAG_KEY_MAKE: u32 = 11;
/// Research: sign the record in the handle buffer under the bag of the uid
/// in `enclave_status`, with a password credential from that bag; the
/// password is the NUL-padded `digest` (at most 32 bytes). `result` = the
/// signature, `result2` = the verify-secret status.
pub(crate) const SELFTEST_USERBAG_KEY_USE: u32 = 12;
/// One touch: sign in the matched context, then sign again in the same,
/// still open context (`result2`). Measures whether one match lets the
/// enclave do more than one operation (needs a finger).
pub(crate) const SELFTEST_TOUCH_TWO_SIGNATURES: u32 = 20;
/// One touch: sign, then unseal a secret sealed to the key in the same
/// context (`result2` is 0 when the secret came back unchanged; needs a finger).
pub(crate) const SELFTEST_TOUCH_SIGN_THEN_UNSEAL: u32 = 21;
/// No finger: unseal a secret sealed to the key with no context (`result`)
/// and with an unsatisfied one (`result2`). Both must be refused.
pub(crate) const SELFTEST_UNSEAL_UNSATISFIED: u32 = 22;
/// Research probe, no finger, ignores the handle: what the enclave does
/// for a decrypt (`oecd`) and a key exchange (`ock`) on keys with and without
/// a list. Five throwaway keys (no list; the list with `oecd` free; with no
/// decrypt entry; with `ock` free; macOS's plain list, `ock`/`odel`/`osgn`/
/// `oa`/`okd` free), each tried with `UNSEAL_CONTEXT_PROBE_ROWS`. Bit
/// `8 * key + row` of `result` (keys 0-3) or bit `row` of `result2` (key 4)
/// is set when that row worked (a key exchange only when it returned the
/// expected shared secret, an unseal only when the secret came back). The
/// kernel log has every enclave status.
pub(crate) const SELFTEST_UNSEAL_CONTEXT_PROBE: u32 = 24;

/// Rows of `SELFTEST_UNSEAL_CONTEXT_PROBE`: (name, operation, context).
/// Context 0 none, 1 a fresh unsatisfied one, 2 the key bag's secret.
/// Operation `unseal` is SEP_KEY_UNSEAL's own path.
pub(crate) const UNSEAL_CONTEXT_PROBE_ROWS: [(&str, &[u8], u8); 8] = [
    ("sign, no context", b"osgn".as_slice(), 0),
    ("sign, unsatisfied context", b"osgn", 1),
    ("decrypt, no context", b"oecd", 0),
    ("decrypt, unsatisfied context", b"oecd", 1),
    ("key exchange, no context", b"ock", 0),
    ("key exchange, unsatisfied context", b"ock", 1),
    ("key exchange, key-bag-secret context", b"ock", 2),
    ("unseal, no context", b"unseal", 0),
];

static_assert!(core::mem::size_of::<Info>() == 16);
static_assert!(core::mem::size_of::<Create>() == 2124);
static_assert!(core::mem::size_of::<Sign>() == 2164);
static_assert!(core::mem::size_of::<Public>() == 2124);
static_assert!(core::mem::size_of::<Selftest>() == 2104);
static_assert!(core::mem::size_of::<Seal>() == 2384);

// SAFETY: `repr(C)` with no padding (the asserts above), so each value is a
// faithful byte image and every bit pattern is a valid value.
unsafe impl kernel::transmute::AsBytes for Info {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for Create {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for Create {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for Sign {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for Sign {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for Public {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for Public {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for Seal {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for Seal {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for Selftest {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for Selftest {}

pub(crate) const IOC_INFO: u32 = _IOR::<Info>(MAGIC, 0x20);
pub(crate) const IOC_CREATE: u32 = _IOWR::<Create>(MAGIC, 0x21);
pub(crate) const IOC_SIGN: u32 = _IOWR::<Sign>(MAGIC, 0x22);
pub(crate) const IOC_PUBLIC: u32 = _IOWR::<Public>(MAGIC, 0x23);
pub(crate) const IOC_SELFTEST: u32 = _IOWR::<Selftest>(MAGIC, 0x24);
pub(crate) const IOC_CANCEL: u32 = _IO(MAGIC, 0x25);
pub(crate) const IOC_SEAL: u32 = _IOWR::<Seal>(MAGIC, 0x26);
pub(crate) const IOC_UNSEAL: u32 = _IOWR::<Seal>(MAGIC, 0x27);

pub(crate) fn info() -> Info {
    Info {
        version: IFACE_VERSION,
        flags_supported: CREATE_FLAGS,
        handle_max: HANDLE_MAX as u32,
        features: FEATURES,
    }
}

/// No flags, biometry (current set), or biometry plus biometry-any.
fn handle_flags_valid(flags: u8) -> bool {
    matches!(flags, 0 | HANDLE_BIOMETRY | 0x03)
}

/// Writes the handle for `blob` with `flags` into `out`; returns its length.
pub(crate) fn encode_handle(blob: &[u8], flags: u8, out: &mut [u8; HANDLE_MAX]) -> Result<usize> {
    if blob.is_empty() || blob.len() > BLOB_MAX || !handle_flags_valid(flags) {
        return Err(EINVAL);
    }
    let len = u32::try_from(blob.len()).map_err(|_| EINVAL)?;
    out.fill(0);
    out[..4].copy_from_slice(HANDLE_MAGIC);
    out[4] = HANDLE_VERSION;
    out[5] = flags;
    out[8..12].copy_from_slice(&len.to_le_bytes());
    out[HANDLE_HEADER..HANDLE_HEADER + blob.len()].copy_from_slice(blob);
    Ok(HANDLE_HEADER + blob.len())
}

/// The enclave record inside the first `len` bytes of `handle`, and the
/// handle's flags. Refuses an unknown version or flag, a non-zero reserved
/// field or a length that does not match exactly. The flags are not
/// authenticated; the enclave enforces the record's own list either way.
pub(crate) fn decode_handle(handle: &[u8; HANDLE_MAX], len: u32) -> Result<(&[u8], u8)> {
    let len = len as usize;
    if !(HANDLE_HEADER + 1..=HANDLE_MAX).contains(&len) {
        return Err(EINVAL);
    }
    if &handle[..4] != HANDLE_MAGIC || handle[4] != HANDLE_VERSION || !handle_flags_valid(handle[5]) {
        return Err(EINVAL);
    }
    if handle[6] != 0 || handle[7] != 0 {
        return Err(EINVAL);
    }
    let blob_len = u32::from_le_bytes([handle[8], handle[9], handle[10], handle[11]]) as usize;
    if blob_len != len - HANDLE_HEADER {
        return Err(EINVAL);
    }
    Ok((&handle[HANDLE_HEADER..len], handle[5]))
}

/// Copies a DER signature into the reply; refuses one that does not fit.
pub(crate) fn put_signature(sig: &[u8], out: &mut Sign) -> Result<()> {
    if sig.is_empty() || sig.len() > SIG_MAX {
        return Err(EIO);
    }
    out.signature.fill(0);
    out.signature[..sig.len()].copy_from_slice(sig);
    out.sig_len = sig.len() as u32;
    Ok(())
}

/// Copies an unsealed secret into the reply; refuses one that does not fit.
pub(crate) fn put_secret(secret: &[u8], out: &mut Seal) -> Result<()> {
    if secret.is_empty() || secret.len() > SECRET_MAX {
        return Err(EIO);
    }
    out.secret.fill(0);
    out.secret[..secret.len()].copy_from_slice(secret);
    out.secret_len = secret.len() as u32;
    Ok(())
}

/// The ECIES blob for `secret` to the public point of the record `blob`.
pub(crate) fn seal_to(blob: &[u8], secret: &[u8]) -> Result<KVec<u8>> {
    if secret.is_empty() || secret.len() > SECRET_MAX {
        return Err(EINVAL);
    }
    let point = SepData::user_key_public(blob)?;
    let sealed = crate::refkey_seal::ecies_seal(&point, secret)?;
    if sealed.len() > SEALED_MAX {
        return Err(EIO);
    }
    Ok(sealed)
}

/// macOS's `.userPresence` list: every constrained operation names the
/// `DeviceOwnerAuthentication` policy (Touch ID or the password). Used only
/// by `SELFTEST_PRESENCE_PROBE`.
pub(crate) fn presence_acl() -> Result<KVec<u8>> {
    let constraint =
        crate::der::encode_refkey_set(&[(b"cpo".as_slice(), RV::Utf8(b"DeviceOwnerAuthentication"))])?;
    let acl = crate::der::encode_refkey_set(&[
        (b"oa".as_slice(), RV::Bool(true)),
        (b"ock".as_slice(), RV::Der(&constraint)),
        (b"odel".as_slice(), RV::Bool(true)),
        (b"okd".as_slice(), RV::Der(&constraint)),
        (b"osgn".as_slice(), RV::Der(&constraint)),
    ])?;
    crate::der::encode_refkey_set(&[(b"acl".as_slice(), RV::Der(&acl))])
}

/// The access-control list of a biometry-current-set key, in the key
/// store's DER, and the constraint its signing operation carries.
///
/// As macOS writes it for `.privateKeyUsage | .biometryCurrentSet`: sign,
/// key agreement (`ock`) and derivation (`okd`) need a biometric credential
/// (`cbio`) for this catacomb (`pbioc`) and enrolled set (`pbioh`); delete
/// (`odel`) and attest (`oa`) are free. SEP_KEY_UNSEAL uses the key
/// exchange (`UNSEAL_ACL_OP`), so a secret sealed to the key comes back only
/// after a match too. With no `enrolled_hash` it is the
/// `.biometryAny` list: `cbio` names the catacomb only.
pub(crate) struct BioAcl {
    pub(crate) ed: KVec<u8>,
    pub(crate) constraint: KVec<u8>,
}

/// The list operation SEP_KEY_UNSEAL needs: the enclave's key exchange
/// (`ock`). The enclave refuses its own decrypt (`oecd`) for every key with
/// a list, even one that leaves it free (self-test 24, 11-11), so the
/// shared secret comes from `ock` and the AES-GCM decrypt runs in the kernel,
/// as macOS does it for Secure Enclave keys.
pub(crate) const UNSEAL_ACL_OP: &[u8] = b"ock";

pub(crate) fn bio_acl(catacomb: &[u8; 16], enrolled_hash: Option<&[u8; 32]>) -> Result<BioAcl> {
    let cbio = match enrolled_hash {
        Some(hash) => crate::der::encode_refkey_set(&[
            (b"pbioc".as_slice(), RV::Octets(catacomb)),
            (b"pbioh".as_slice(), RV::Octets(hash)),
        ])?,
        None => crate::der::encode_refkey_set(&[(b"pbioc".as_slice(), RV::Octets(catacomb))])?,
    };
    let constraint = crate::der::encode_refkey_set(&[(b"cbio".as_slice(), RV::Der(&cbio))])?;
    let acl = crate::der::encode_refkey_set(&[
        (b"oa".as_slice(), RV::Bool(true)),
        (b"ock".as_slice(), RV::Der(&constraint)),
        (b"odel".as_slice(), RV::Bool(true)),
        (b"okd".as_slice(), RV::Der(&constraint)),
        (b"osgn".as_slice(), RV::Der(&constraint)),
    ])?;
    let ed = crate::der::encode_refkey_set(&[(b"acl".as_slice(), RV::Der(&acl))])?;
    Ok(BioAcl { ed, constraint })
}

/// What one verify run of a touch-gated signature ended with.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum TouchOutcome {
    Matched,
    NoMatch,
    Failed(u32),
}

/// What a touch-gated call has the enclave do in the matched context.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum TouchOp {
    /// `osgn` over the job's digest.
    Sign,
    /// The key exchange (`ock`) for the job's sealed blob, then the decrypt.
    Unseal,
}

/// The touch-gated operation a `SIGN` or `UNSEAL` call is waiting for. Lives
/// only for that call; the verify it runs fills it in.
pub(crate) struct KeyTouch {
    blob: KVec<u8>,
    op: TouchOp,
    digest: [u8; DIGEST_LEN],
    sealed: KVec<u8>,
    constraint: KVec<u8>,
    sign_again_after_delete: bool,
    /// Self-test only: a second operation in the same matched context.
    then: Option<TouchOp>,
    outcome: Option<TouchOutcome>,
    sig: Option<Result<KVec<u8>>>,
    status: i32,
    again: Option<Result<KVec<u8>>>,
    again_status: i32,
}

impl KeyTouch {
    pub(crate) fn set_outcome(&mut self, outcome: &crate::bio::VerifyOutcome) {
        self.outcome = Some(match outcome {
            crate::bio::VerifyOutcome::Matched(_) => TouchOutcome::Matched,
            crate::bio::VerifyOutcome::NoMatch => TouchOutcome::NoMatch,
            crate::bio::VerifyOutcome::Failed(s) => TouchOutcome::Failed(*s),
        });
    }
}

fn errno_of<T>(r: &Option<Result<T>>) -> i32 {
    match r {
        Some(Ok(_)) => 0,
        Some(Err(e)) => e.to_errno(),
        None => EIO.to_errno(),
    }
}

#[::kernel::macros::kunit_tests(apple_sep_key_handle)]
mod tests {
    use super::*;

    fn blob() -> [u8; 100] {
        let mut b = [0u8; 100];
        for (i, v) in b.iter_mut().enumerate() {
            *v = i as u8 ^ 0x5a;
        }
        b
    }

    #[test]
    fn round_trip() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), 0, &mut h).unwrap();
        assert_eq!(n, HANDLE_HEADER + 100);
        assert_eq!(&h[..4], b"SEPK");
        assert_eq!(decode_handle(&h, n as u32).unwrap(), (&blob()[..], 0));
    }

    #[test]
    fn length_must_match_exactly() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), 0, &mut h).unwrap() as u32;
        assert!(decode_handle(&h, n - 1).is_err());
        assert!(decode_handle(&h, n + 1).is_err());
        assert!(decode_handle(&h, 0).is_err());
        assert!(decode_handle(&h, HANDLE_MAX as u32 + 1).is_err());
    }

    #[test]
    fn header_fields_are_checked() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), 0, &mut h).unwrap() as u32;
        for at in [0usize, 4, 6, 7] {
            let mut bad = h;
            bad[at] ^= 0x01;
            assert!(decode_handle(&bad, n).is_err());
        }
    }

    #[test]
    fn empty_and_oversize_blobs_are_refused() {
        let mut h = [0u8; HANDLE_MAX];
        assert!(encode_handle(&[], 0, &mut h).is_err());
        let big = [1u8; BLOB_MAX + 1];
        assert!(encode_handle(&big, 0, &mut h).is_err());
        let max = [1u8; BLOB_MAX];
        assert_eq!(encode_handle(&max, 0, &mut h).unwrap(), HANDLE_MAX);
    }

    #[test]
    fn signature_bounds() {
        let mut s = Sign {
            flags: 0,
            handle_len: 0,
            digest: [0; DIGEST_LEN],
            sig_len: 0,
            signature: [0; SIG_MAX],
            handle: [0; HANDLE_MAX],
        };
        assert!(put_signature(&[], &mut s).is_err());
        assert!(put_signature(&[0x30; SIG_MAX + 1], &mut s).is_err());
        put_signature(&[0x30, 0x02, 0x01, 0x01], &mut s).unwrap();
        assert_eq!(s.sig_len, 4);
    }

    #[test]
    fn biometry_flag_round_trips_and_unknown_flags_are_refused() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), HANDLE_BIOMETRY, &mut h).unwrap() as u32;
        assert_eq!(decode_handle(&h, n).unwrap().1, HANDLE_BIOMETRY);
        assert!(encode_handle(&blob(), 0x02, &mut h).is_err());
        assert!(encode_handle(&blob(), 0x04, &mut h).is_err());
        let mut bad = h;
        bad[5] = 0x02;
        assert!(decode_handle(&bad, n).is_err());
        let n = encode_handle(&blob(), HANDLE_BIOMETRY | HANDLE_BIOMETRY_ANY, &mut h).unwrap() as u32;
        assert_eq!(decode_handle(&h, n).unwrap().1, HANDLE_BIOMETRY | HANDLE_BIOMETRY_ANY);
    }

    #[test]
    fn version_three_lists_cancel_and_seal() {
        let i = info();
        assert_eq!(i.version, 3);
        assert_eq!(i.features, FEATURE_CANCEL | FEATURE_SEAL);
        assert_eq!(IOC_CANCEL, 0xb125);
        assert_eq!(IOC_SEAL >> 16 & 0x3fff, 2384);
        assert_eq!(IOC_UNSEAL & 0xff, 0x27);
    }

    #[test]
    fn secret_bounds() {
        let mut s = Seal {
            flags: 0,
            handle_len: 0,
            secret_len: 0,
            sealed_len: 0,
            secret: [0xaa; SECRET_MAX],
            sealed: [0; SEALED_MAX],
            handle: [0; HANDLE_MAX],
        };
        assert!(put_secret(&[], &mut s).is_err());
        assert!(put_secret(&[1; SECRET_MAX + 1], &mut s).is_err());
        put_secret(&[7; 5], &mut s).unwrap();
        assert_eq!(s.secret_len, 5);
        assert_eq!(&s.secret[..6], &[7, 7, 7, 7, 7, 0]);
        assert!(seal_to(&[0u8; 10], &[]).is_err());
        assert!(seal_to(&[0u8; 10], &[1; SECRET_MAX + 1]).is_err());
    }

    #[test]
    fn create_takes_one_policy_and_refuses_the_password_ones() {
        assert_eq!(create_policy(0).unwrap(), Policy::None);
        assert_eq!(create_policy(CREATE_BIOMETRY_CURRENT_SET).unwrap(), Policy::BiometryCurrentSet);
        assert_eq!(create_policy(CREATE_BIOMETRY_ANY).unwrap(), Policy::BiometryAny);
        assert_eq!(create_policy(CREATE_USER_PRESENCE).unwrap_err(), EOPNOTSUPP);
        assert_eq!(create_policy(CREATE_DEVICE_PASSCODE).unwrap_err(), EOPNOTSUPP);
        assert_eq!(create_policy(CREATE_BIOMETRY_CURRENT_SET | CREATE_BIOMETRY_ANY).unwrap_err(), EINVAL);
        assert_eq!(create_policy(0x10).unwrap_err(), EINVAL);
        assert_eq!(info().flags_supported, CREATE_BIOMETRY_CURRENT_SET | CREATE_BIOMETRY_ANY);
    }

    #[test]
    fn biometry_any_list_names_the_catacomb_only() {
        let acl = bio_acl(&[0x11; 16], None).unwrap();
        // SET { SEQ { "cbio", SET { SEQ { "pbioc", OCTETS[16] } } } }
        assert_eq!(acl.constraint.len(), 2 + 2 + 6 + 2 + (2 + 7 + 18));
        assert!(!acl.ed.windows(5).any(|w| w == b"pbioh"));
        assert!(acl.ed.windows(5).any(|w| w == b"pbioc"));
    }

    #[test]
    fn presence_list_has_the_captured_shape() {
        let ed = presence_acl().unwrap();
        // macOS: each constrained op is SET(34) { SEQ { "cpo", "DeviceOwnerAuthentication" } }.
        assert_eq!(ed.windows(2).filter(|w| *w == [0x31u8, 34].as_slice()).count(), 3);
        assert_eq!(ed.windows(25).filter(|w| *w == b"DeviceOwnerAuthentication").count(), 3);
    }

    #[test]
    fn biometric_acl_has_the_key_store_shape() {
        let acl = bio_acl(&[0x11; 16], Some(&[0x22; 32])).unwrap();
        // SET { SEQ { "cbio", SET { SEQ { "pbioc", OCTETS[16] }, SEQ { "pbioh", OCTETS[32] } } } }
        assert_eq!(acl.constraint.len(), 2 + 2 + 6 + 2 + (2 + 7 + 18) + (2 + 7 + 34));
        assert_eq!(&acl.constraint[..2], &[0x31, 0x50]);
        assert_eq!(&acl.constraint[4..10], b"\x0c\x04cbio");
        let ed = &acl.ed;
        assert_eq!(ed[0], 0x31);
        // The list names all six operations; delete and attest are free.
        for op in [b"oa".as_slice(), b"ock", b"odel", b"okd", b"osgn"] {
            assert!(ed.windows(op.len() + 2).any(|w| w[0] == 0x0c && w[1] as usize == op.len() && &w[2..] == op));
        }
        assert!(ed.windows(3).any(|w| w == [0x01u8, 0x01, 0x01].as_slice()));
    }

    #[test]
    fn acl_constraint_command_has_the_captured_layout() {
        let acl = bio_acl(&[0x11; 16], Some(&[0x22; 32])).unwrap();
        let cmd = crate::scrd::scrd_verify_acl_constraint(&[0x33; 16], &acl.constraint, b"ock").unwrap();
        // macOS: 138 bytes for a 94-byte constraint and "ock"; this one is 80 bytes.
        assert_eq!(cmd.len(), 8 + 16 + 4 + acl.constraint.len() + 4 + 3 + 1 + 8);
        assert_eq!(&cmd[..8], b"DRCS\x20\x00\x00\x02");
        assert_eq!(&cmd[24..28], &(acl.constraint.len() as u32).to_le_bytes());
    }
}

impl SepData {
    pub(crate) fn register_key(&self) -> Result<()> {
        let ctx = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        self.publish_chardev(&self.key_dev, || {
            // SAFETY: as in `register_bio`: `remove()` drops the registration
            // before the `Arc` that keeps `ctx` alive, and the shim's unregister
            // waits for running ioctls and clears the stored context before
            // any later callback. Ioctls run concurrently: a touch-gated SIGN or
            // UNSEAL sleeps up to a minute, and CANCEL must reach it meanwhile.
            // They serialise on `catacomb_writer`; CANCEL only sets a flag.
            unsafe {
                crate::shim::BioChardev::register_concurrent(
                    DEVICE_NAME,
                    DEVICE_MODE,
                    ctx,
                    key_open_trampoline,
                    key_release_trampoline,
                    key_ioctl_trampoline,
                    key_ready_trampoline,
                )
            }
        })?;
        Ok(())
    }

    /// Whether the capture a verify is waiting on is still wanted: the
    /// touch-gated signature that owns it, or /dev/sep-bio's live session.
    pub(crate) fn capture_live(&self) -> bool {
        (self.key_touch_active.load(Relaxed)
            && !self.key_touch_cancel.load(Relaxed)
            && !self.shutting_down.load(Relaxed))
            || crate::bio::capture_is_live(&self.bio_session.lock(), self.capture_epoch.load(Relaxed))
    }

    fn key_bio_acl(&self, any: bool) -> Result<BioAcl> {
        if !self.touchid_started.load(Relaxed) {
            return Err(ENODEV);
        }
        let (catacomb, enrolled) = self.catacomb_binding()?;
        bio_acl(&catacomb, (!any).then_some(&enrolled))
    }

    /// Evaluates the signing constraint on the fresh context before the
    /// match, as LocalAuthentication does. Best effort: logged only.
    pub(crate) fn key_touch_preflight(&self, evaluation: &[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]) {
        let (constraint, op) = {
            let guard = self.key_touch.lock();
            let Some(job) = guard.as_ref() else {
                return;
            };
            let mut c = KVec::new();
            if c.extend_from_slice(&job.constraint, GFP_KERNEL).is_err() {
                return;
            }
            let op: &[u8] = match job.op {
                TouchOp::Sign => b"osgn",
                TouchOp::Unseal => UNSEAL_ACL_OP,
            };
            (c, op)
        };
        let Ok(payload) = crate::scrd::scrd_verify_acl_constraint(evaluation, &constraint, op) else {
            return;
        };
        match self.scrd_transact(
            c"SCRD_VERIFY_ACL_CONSTRAINT",
            crate::scrd::SCRD_REQUEST_COMMAND,
            &payload,
            crate::SCRD_TIMEOUT_MS,
        ) {
            Some((reply, out)) => {
                let word = |at: usize| {
                    out.get(at..at + 4)
                        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                };
                dev_info!(
                    self.dev,
                    "sep-key: signing constraint on the new context: status {}, {} reply bytes, satisfied {:?}, first requirement {:?}\n",
                    reply.status,
                    out.len(),
                    word(0),
                    word(4)
                );
            }
            None => dev_info!(self.dev, "sep-key: signing constraint check got no answer\n"),
        }
    }

    /// One operation of the waiting job in `evaluation`.
    fn key_touch_op(
        &self,
        op: TouchOp,
        blob: &[u8],
        digest: &[u8; DIGEST_LEN],
        sealed: &[u8],
        evaluation: &[u8; crate::scrd::SCRD_ACM_HANDLE_LEN],
    ) -> Result<KVec<u8>> {
        match op {
            TouchOp::Sign => self.user_key_sign(blob, digest, Some(evaluation)),
            TouchOp::Unseal => self.user_key_unseal(blob, sealed, Some(evaluation)),
        }
    }

    /// Runs the waiting job in `evaluation`, the context the match just
    /// satisfied. Called from the verify, at most once with success.
    pub(crate) fn key_touch_sign(&self, evaluation: &[u8; crate::scrd::SCRD_ACM_HANDLE_LEN], when: &CStr) {
        let (op, then, blob, digest, sealed) = {
            let guard = self.key_touch.lock();
            let Some(job) = guard.as_ref() else {
                return;
            };
            if matches!(job.sig, Some(Ok(_))) {
                return;
            }
            let mut b = KVec::new();
            let mut d = KVec::new();
            if b.extend_from_slice(&job.blob, GFP_KERNEL).is_err()
                || d.extend_from_slice(&job.sealed, GFP_KERNEL).is_err()
            {
                return;
            }
            (job.op, job.then, b, job.digest, d)
        };
        let r = self.key_touch_op(op, &blob, &digest, &sealed, evaluation);
        let status = self.last_key_status.load(Relaxed) as i32;
        dev_info!(
            self.dev,
            "sep-key: touch-gated {:?} {}: {} (enclave status {})\n",
            op,
            when,
            (if r.is_ok() { "done" } else { "refused" }),
            status
        );
        let ok = r.is_ok();
        if let Some(job) = &mut *self.key_touch.lock() {
            job.sig = Some(r);
            job.status = status;
        }
        // Self-test only: a second operation in the context that is still open.
        if let (true, Some(second)) = (ok, then) {
            let r2 = self.key_touch_op(second, &blob, &digest, &sealed, evaluation);
            let status2 = self.last_key_status.load(Relaxed) as i32;
            dev_info!(
                self.dev,
                "sep-key: self-test: second operation ({:?}) in the same matched context: {} (enclave status {})\n",
                second,
                (if r2.is_ok() { "DONE" } else { "refused" }),
                status2
            );
            if let Some(job) = &mut *self.key_touch.lock() {
                if job.again.is_none() {
                    job.again = Some(r2);
                    job.again_status = status2;
                }
            }
        }
    }

    /// Self-test only: after the matched context is deleted, sign once more
    /// with its old handle.
    pub(crate) fn key_touch_after_close(&self, evaluation: &[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]) {
        let (blob, digest) = {
            let guard = self.key_touch.lock();
            let Some(job) = guard.as_ref() else {
                return;
            };
            if !job.sign_again_after_delete || !matches!(job.sig, Some(Ok(_))) {
                return;
            }
            let mut b = KVec::new();
            if b.extend_from_slice(&job.blob, GFP_KERNEL).is_err() {
                return;
            }
            (b, job.digest)
        };
        let r = self.user_key_sign(&blob, &digest, Some(evaluation));
        let status = self.last_key_status.load(Relaxed) as i32;
        dev_info!(
            self.dev,
            "sep-key: self-test: signature with the deleted matched context: {} (enclave status {})\n",
            (if r.is_ok() { "SIGNED" } else { "refused" }),
            status
        );
        if let Some(job) = &mut *self.key_touch.lock() {
            job.again = Some(r);
            job.again_status = status;
        }
    }

    /// A touch-gated signature: one fresh credential context, one Touch ID
    /// match in it, the signature, and the context deleted, all inside this
    /// call. Waits about a minute for a finger.
    fn key_sign_with_touch(&self, blob: &[u8], flags: u8, digest: &[u8; DIGEST_LEN], again: bool) -> Result<KeyTouch> {
        self.key_touch_run(blob, flags, TouchOp::Sign, digest, &[], again, None)
    }

    /// A touch-gated operation (see `key_sign_with_touch`). `CANCEL` ends
    /// the wait with `ECANCELED`.
    #[allow(clippy::too_many_arguments)]
    fn key_touch_run(
        &self,
        blob: &[u8],
        flags: u8,
        op: TouchOp,
        digest: &[u8; DIGEST_LEN],
        sealed: &[u8],
        again: bool,
        then: Option<TouchOp>,
    ) -> Result<KeyTouch> {
        let acl = self.key_bio_acl(flags & HANDLE_BIOMETRY_ANY != 0)?;
        let mut owned = KVec::new();
        owned.extend_from_slice(blob, GFP_KERNEL)?;
        let mut owned_sealed = KVec::new();
        owned_sealed.extend_from_slice(sealed, GFP_KERNEL)?;
        // Serialises with /dev/sep-bio's enrol and verify work, as they do
        // with each other.
        let _writer = self.catacomb_writer.lock();
        *self.key_touch.lock() = Some(KeyTouch {
            blob: owned,
            op,
            digest: *digest,
            sealed: owned_sealed,
            constraint: acl.constraint,
            sign_again_after_delete: again,
            then,
            outcome: None,
            sig: None,
            status: 0,
            again: None,
            again_status: 0,
        });
        self.key_touch_cancel.store(false, Relaxed);
        self.key_touch_active.store(true, Relaxed);
        dev_info!(self.dev, "sep-key: waiting for a Touch ID match ({:?})\n", op);
        let deadline = crate::shim::boottime_ns().saturating_add(TOUCH_WAIT_MS * 1_000_000);
        let ended = loop {
            if self.shutting_down.load(Relaxed) {
                break Err(ENODEV);
            }
            if self.key_touch_cancel.load(Relaxed) {
                break Err(ECANCELED);
            }
            if !self.capture_begin() {
                break Err(EAGAIN);
            }
            self.run_verify();
            self.capture_end();
            let outcome = match &mut *self.key_touch.lock() {
                Some(job) => job.outcome.take(),
                None => None,
            };
            if self.key_touch_cancel.load(Relaxed) && outcome != Some(TouchOutcome::Matched) {
                break Err(ECANCELED);
            }
            match outcome {
                Some(TouchOutcome::Matched) => break Ok(()),
                Some(TouchOutcome::NoMatch) => break Err(EACCES),
                Some(TouchOutcome::Failed(s)) if s == crate::ENROL_STATUS_RETRY => {
                    if crate::shim::boottime_ns() >= deadline {
                        break Err(ETIMEDOUT);
                    }
                }
                Some(TouchOutcome::Failed(s)) if s == crate::ENROL_STATUS_LOCKOUT => break Err(EACCES),
                Some(TouchOutcome::Failed(_)) | None => break Err(EIO),
            }
        };
        self.key_touch_active.store(false, Relaxed);
        self.key_touch_cancel.store(false, Relaxed);
        let job = self.key_touch.lock().take().ok_or(EIO)?;
        dev_info!(
            self.dev,
            "sep-key: touch-gated {:?} ended: {:?}, result {}\n",
            op,
            ended,
            errno_of(&job.sig)
        );
        ended?;
        Ok(job)
    }

    /// A credential context holding only the identity key bag's secret, the
    /// Linux stand-in for a passcode (self-test).
    fn passcode_context(&self) -> Result<[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]> {
        let du = crate::sks::DesignateUser::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
        let crate::keybag::State::Present(stored) = crate::keybag::read(crate::keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        let (status, _) = self.scrd_command(&crate::scrd::scrd_initialize()).ok_or(EIO)?;
        if status != 0 {
            return Err(EIO);
        }
        let (status, out) = self
            .scrd_command(&crate::scrd::scrd_context_create_tracked(crate::SBIO_PROBE_USER_ID))
            .ok_or(EIO)?;
        let handle = crate::scrd::tracked_create_reference(&out)
            .filter(|_| status == 0)
            .ok_or(EIO)?;
        let ok = (|| -> Option<()> {
            let (status, _) = self.scrd_command(&crate::scrd::scrd_context_externalize(&handle))?;
            if status != 0 {
                return None;
            }
            let out = self.sks_send(self.sks_req_verify_secret(du.special_handle(), stored.secret(), &handle))?;
            dev_info!(self.dev, "sep-key: self-test: passcode credential added, status {}\n", out.reply.status);
            (out.reply.status == 0).then_some(())
        })();
        if ok.is_none() {
            self.close_unlock_evaluation(&handle);
            return Err(EIO);
        }
        Ok(handle)
    }

    /// A fresh tracked context for `uid`; with `secret`, a passcode
    /// credential from `bag`'s verify-secret is added. Returns the context
    /// and the verify status (0 when none was asked).
    fn key_context_for(
        &self,
        uid: i32,
        cred: Option<(crate::sks::SpecialHandle, &[u8])>,
    ) -> Result<([u8; crate::scrd::SCRD_ACM_HANDLE_LEN], i32)> {
        let (status, _) = self.scrd_command(&crate::scrd::scrd_initialize()).ok_or(EIO)?;
        if status != 0 {
            return Err(EIO);
        }
        let (status, out) = self.scrd_command(&crate::scrd::scrd_context_create_tracked(uid)).ok_or(EIO)?;
        let ctx = crate::scrd::tracked_create_reference(&out).filter(|_| status == 0).ok_or(EIO)?;
        let (status, _) = self.scrd_command(&crate::scrd::scrd_context_externalize(&ctx)).ok_or(EIO)?;
        if status != 0 {
            self.close_unlock_evaluation(&ctx);
            return Err(EIO);
        }
        let mut verify = 0;
        if let Some((bag, secret)) = cred {
            verify = match self.sks_send(self.sks_req_verify_secret(bag, secret, &ctx)) {
                Some(out) => out.reply.status as i32,
                None => -(EIO.to_errno()),
            };
        }
        Ok((ctx, verify))
    }

    fn key_userbag_record(&self, req: &mut Selftest) -> Result<()> {
        let uid = req.enclave_status;
        if uid < crate::sks::SKS_DESIGNATE_USER_MIN || uid == crate::SBIO_PROBE_USER_ID {
            return Err(EINVAL);
        }
        let user = crate::sks::DesignateUser::new(uid).ok_or(EINVAL)?;
        if req.mode == SELFTEST_USERBAG_KEY_MAKE {
            let ed = presence_acl()?;
            let blob = self.user_key_create_in_bag(user, Some(&ed))?;
            if blob.len() > HANDLE_MAX {
                return Err(E2BIG);
            }
            req.handle.fill(0);
            req.handle[..blob.len()].copy_from_slice(&blob);
            req.handle_len = blob.len() as u32;
            req.result = 0;
            dev_info!(self.dev, "sep-key: self-test 11: key under uid {}'s bag, record {} bytes\n", uid, blob.len());
            return Ok(());
        }
        let len = req.handle_len as usize;
        if len == 0 || len > HANDLE_MAX {
            return Err(EINVAL);
        }
        let mut pw = req.digest;
        req.digest.fill(0);
        let plen = pw.iter().position(|&b| b == 0).unwrap_or(pw.len());
        if plen == 0 {
            return Err(EINVAL);
        }
        let mut record = KVec::new();
        record.extend_from_slice(&req.handle[..len], GFP_KERNEL)?;
        let made = self.key_context_for(uid, Some((user.special_handle(), &pw[..plen])));
        pw.fill(0);
        let (ctx, v) = made?;
        req.result2 = v;
        self.last_key_status.store(0, Relaxed);
        let r = self.user_key_sign_in_bag(user, &record, &[0x5a; DIGEST_LEN], Some(&ctx));
        self.close_unlock_evaluation(&ctx);
        req.result = r.err().map(|e| e.to_errno()).unwrap_or(0);
        req.enclave_status = self.last_key_status.load(Relaxed) as i32;
        dev_info!(
            self.dev,
            "sep-key: self-test 12: saved key under uid {}'s bag: verify {} -> {} ({})\n",
            uid,
            v,
            (if req.result == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status
        );
        Ok(())
    }

    fn key_userbag_locked(&self, req: &mut Selftest) -> Result<()> {
        let uid = req.enclave_status;
        let len = req.handle_len as usize;
        if len == 0 || len > 256 || uid < crate::sks::SKS_DESIGNATE_USER_MIN || uid == crate::SBIO_PROBE_USER_ID {
            return Err(EINVAL);
        }
        let mut pw = [0u8; 256];
        pw[..len].copy_from_slice(&req.handle[..len]);
        req.handle.fill(0);
        let user = crate::sks::DesignateUser::new(uid).ok_or(EINVAL)?;
        let out = (|| -> Result<[i32; 8]> {
            let ed = presence_acl()?;
            let blob = self.user_key_create_in_bag(user, Some(&ed))?;
            let digest = [0x5au8; DIGEST_LEN];
            let sign = |ctx: Option<&[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]>| -> i32 {
                self.last_key_status.store(0, Relaxed);
                match self.user_key_sign_in_bag(user, &blob, &digest, ctx) {
                    Ok(_) => 0,
                    Err(_) => match self.last_key_status.load(Relaxed) as i32 {
                        0 => -(EIO.to_errno()),
                        s => s,
                    },
                }
            };
            let mut r = [0i32; 8];
            r[5] = self.userbag_lock_phase(user).map(|p| p as i32).unwrap_or(-1);
            let (stale, _) = self.key_context_for(uid, Some((user.special_handle(), &pw[..len])))?;
            r[0] = sign(Some(&stale));
            // Lock for good: the screen lock, then grace-timer fires (as the
            // Mac's kernel sends them) until the store locks the bag, for at
            // most LOCK_WAIT_S. The store sets the grace period itself.
            const LOCK_WAIT_S: u64 = 330;
            self.userbag_lock(user, true, false)?;
            let start = crate::shim::boottime_ns();
            let mut locked = false;
            while crate::shim::boottime_ns() - start < LOCK_WAIT_S * 1_000_000_000 {
                kernel::time::delay::fsleep(kernel::time::Delta::from_millis(5000));
                match self.userbag_timer_fire(user) {
                    Ok(true) => {
                        locked = true;
                        break;
                    }
                    Ok(false) => {}
                    Err(e) => {
                        dev_info!(self.dev, "sep-key: self-test 10: timer fire returned {:?}\n", e);
                        break;
                    }
                }
            }
            dev_info!(
                self.dev,
                "sep-key: self-test 10: bag locked by the store: {} after {} s\n",
                locked,
                (crate::shim::boottime_ns() - start) / 1_000_000_000
            );
            r[6] = self.userbag_lock_phase(user).map(|p| p as i32).unwrap_or(-1);
            r[1] = sign(Some(&stale));
            self.close_unlock_evaluation(&stale);
            r[2] = sign(None);
            let (fresh, v) = self.key_context_for(uid, Some((user.special_handle(), &pw[..len])))?;
            r[3] = if v == 0 { sign(Some(&fresh)) } else { v };
            self.close_unlock_evaluation(&fresh);
            self.userbag_unlock(user, &pw[..len])?;
            r[7] = self.userbag_lock_phase(user).map(|p| p as i32).unwrap_or(-1);
            let (fresh2, v2) = self.key_context_for(uid, Some((user.special_handle(), &pw[..len])))?;
            r[4] = if v2 == 0 { sign(Some(&fresh2)) } else { v2 };
            self.close_unlock_evaluation(&fresh2);
            Ok(r)
        })();
        pw.fill(0);
        let r = out?;
        for (i, v) in r.iter().enumerate() {
            req.digest[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        req.result = 0;
        dev_info!(
            self.dev,
            "sep-key: self-test 10, uid {}'s bag: phase {} -> sign {}; locked (phase {}): stale credential {}, none {}, fresh password credential {}; unlocked (phase {}): {}\n",
            uid, r[5], r[0], r[6], r[1], r[2], r[3], r[7], r[4]
        );
        Ok(())
    }

    fn key_userbag_probe(&self, req: &mut Selftest) -> Result<()> {
        let uid = req.enclave_status;
        let len = req.handle_len as usize;
        if len == 0 || len > 256 || uid < crate::sks::SKS_DESIGNATE_USER_MIN || uid == crate::SBIO_PROBE_USER_ID {
            return Err(EINVAL);
        }
        let mut pw = [0u8; 256];
        pw[..len].copy_from_slice(&req.handle[..len]);
        req.handle.fill(0);
        let user = crate::sks::DesignateUser::new(uid).ok_or(EINVAL)?;
        let ed = presence_acl()?;
        let made = self.user_key_create_in_bag(user, Some(&ed));
        let blob = match made {
            Ok(b) => b,
            Err(e) => {
                pw.fill(0);
                dev_info!(self.dev, "sep-key: self-test: key under uid {}'s bag not created ({:?})\n", uid, e);
                return Err(e);
            }
        };
        let digest = req.digest;
        let sign = |ctx: &[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]| -> (i32, i32) {
            self.last_key_status.store(0, Relaxed);
            let r = self.user_key_sign_in_bag(user, &blob, &digest, Some(ctx));
            (r.err().map(|e| e.to_errno()).unwrap_or(0), self.last_key_status.load(Relaxed) as i32)
        };
        let first = if req.mode == SELFTEST_USERBAG_KEY_SAME_BAG {
            self.key_context_for(uid, Some((user.special_handle(), &pw[..len])))
        } else {
            let du = crate::sks::DesignateUser::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
            match crate::keybag::read(crate::keybag::Slot::Identity) {
                Ok(crate::keybag::State::Present(stored)) => {
                    self.key_context_for(crate::SBIO_PROBE_USER_ID, Some((du.special_handle(), stored.secret())))
                }
                _ => Err(ENODEV),
            }
        };
        let (ctx, v1) = match first {
            Ok(c) => c,
            Err(e) => {
                pw.fill(0);
                return Err(e);
            }
        };
        (req.result, req.enclave_status) = sign(&ctx);
        self.close_unlock_evaluation(&ctx);
        let second = if req.mode == SELFTEST_USERBAG_KEY_SAME_BAG {
            self.key_context_for(uid, None)
        } else {
            self.key_context_for(uid, Some((user.special_handle(), &pw[..len])))
        };
        pw.fill(0);
        let (ctx2, v2) = second?;
        (req.result2, req.enclave_status2) = sign(&ctx2);
        self.close_unlock_evaluation(&ctx2);
        dev_info!(
            self.dev,
            "sep-key: self-test {}: key under uid {}'s bag: first ({}) verify {} -> {} ({}); second ({}) verify {} -> {} ({})\n",
            req.mode,
            uid,
            (if req.mode == SELFTEST_USERBAG_KEY_SAME_BAG { "same-bag password" } else { "identity-bag secret" }),
            v1,
            (if req.result == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status,
            (if req.mode == SELFTEST_USERBAG_KEY_SAME_BAG { "unsatisfied" } else { "given password" }),
            v2,
            (if req.result2 == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status2
        );
        Ok(())
    }

    fn key_presence_other_bag(&self, req: &mut Selftest) -> Result<()> {
        let uid = req.enclave_status;
        let len = req.handle_len as usize;
        if len == 0 || len > 256 || uid < crate::sks::SKS_DESIGNATE_USER_MIN || uid == crate::SBIO_PROBE_USER_ID {
            return Err(EINVAL);
        }
        let du = crate::sks::DesignateUser::new(uid).ok_or(EINVAL)?;
        let ed = presence_acl()?;
        let (blob, _) = self.user_key_create(Some(&ed))?;
        let digest = req.digest;
        let (status, _) = self.scrd_command(&crate::scrd::scrd_initialize()).ok_or(EIO)?;
        if status != 0 {
            return Err(EIO);
        }
        let (status, out) = self.scrd_command(&crate::scrd::scrd_context_create_tracked(uid)).ok_or(EIO)?;
        let ctx = crate::scrd::tracked_create_reference(&out).filter(|_| status == 0).ok_or(EIO)?;
        let verify = (|| -> Option<i32> {
            let (status, _) = self.scrd_command(&crate::scrd::scrd_context_externalize(&ctx))?;
            if status != 0 {
                return None;
            }
            let out = self.sks_send(self.sks_req_verify_secret(du.special_handle(), &req.handle[..len], &ctx))?;
            Some(out.reply.status as i32)
        })();
        req.handle.fill(0);
        req.result2 = verify.unwrap_or(-(EIO.to_errno()));
        self.last_key_status.store(0, Relaxed);
        let r = self.user_key_sign(&blob, &digest, Some(&ctx));
        req.result = r.err().map(|e| e.to_errno()).unwrap_or(0);
        req.enclave_status = self.last_key_status.load(Relaxed) as i32;
        self.close_unlock_evaluation(&ctx);
        dev_info!(
            self.dev,
            "sep-key: self-test presence key with a credential from uid {}'s bag: verify status {}, signature {} (enclave status {})\n",
            uid,
            req.result2,
            (if req.result == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status
        );
        Ok(())
    }

    fn key_presence_probe(&self, req: &mut Selftest) -> Result<()> {
        let ed = presence_acl()?;
        let (blob, _) = self.user_key_create(Some(&ed))?;
        let digest = req.digest;
        let user = crate::sbio::UserId::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
        let record = |r: Result<KVec<u8>>, s: &SepData| -> (i32, i32) {
            (r.err().map(|e| e.to_errno()).unwrap_or(0), s.last_key_status.load(Relaxed) as i32)
        };
        self.last_key_status.store(0, Relaxed);
        let ctx = self.passcode_context()?;
        (req.result, req.enclave_status) = record(self.user_key_sign(&blob, &digest, Some(&ctx)), self);
        self.close_unlock_evaluation(&ctx);
        self.last_key_status.store(0, Relaxed);
        let ctx = self.open_unlock_evaluation(user).ok_or(EIO)?;
        (req.result2, req.enclave_status2) = record(self.user_key_sign(&blob, &digest, Some(&ctx)), self);
        self.close_unlock_evaluation(&ctx);
        dev_info!(
            self.dev,
            "sep-key: self-test presence probe: bag-secret context {} (enclave status {}), unsatisfied context {} (enclave status {})\n",
            (if req.result == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status,
            (if req.result2 == 0 { "SIGNED" } else { "refused" }),
            req.enclave_status2
        );
        Ok(())
    }

    /// One `SELFTEST_UNSEAL_CONTEXT_PROBE` key: the bits of the rows that
    /// worked on it.
    fn key_unseal_context_probe_key(&self, name: &str, free: Option<&[&[u8]]>, secret: &[u8]) -> Result<i32> {
        use crate::der::RefKeyValue as RV;
        let ed = match free {
            None => None,
            Some(ops) => {
                let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
                for op in ops {
                    items.push((op, RV::Bool(true)), GFP_KERNEL)?;
                }
                let acl = crate::der::encode_refkey_set(&items)?;
                Some(crate::der::encode_refkey_set(&[(b"acl".as_slice(), RV::Der(&acl))])?)
            }
        };
        let (blob, point) = self.user_key_create(ed.as_deref())?;
        let sealed = seal_to(&blob, secret)?;
        let digest = [0x5au8; DIGEST_LEN];
        let user = crate::sbio::UserId::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
        let mut bits = 0i32;
        for (row, (what, op, ctx_kind)) in UNSEAL_CONTEXT_PROBE_ROWS.iter().enumerate() {
            let ctx = match ctx_kind {
                0 => None,
                1 => Some(self.open_unlock_evaluation(user).ok_or(EIO)?),
                _ => Some(self.passcode_context()?),
            };
            let (eph, z) = crate::refkey_seal::ecdh_probe_pair(&point)?;
            self.last_key_status.store(0, Relaxed);
            let r = match *op {
                b"unseal" => self.user_key_unseal(&blob, &sealed, ctx.as_ref()).map(|v| (0, v)),
                b"osgn" => self.user_key_raw_op(&blob, op, b"d", &digest, ctx.as_ref()),
                b"oecd" => self.user_key_raw_op(&blob, op, b"d", &sealed, ctx.as_ref()),
                _ => self.user_key_raw_op(&blob, op, b"pub", &eph, ctx.as_ref()),
            };
            if let Some(c) = ctx.as_ref() {
                self.close_unlock_evaluation(c);
            }
            let (status, out) = match r {
                Ok(v) => v,
                Err(_) => (self.last_key_status.load(Relaxed) as i32, KVec::new()),
            };
            let ok = status == 0
                && match *op {
                    b"oecd" | b"unseal" => out[..] == secret[..],
                    b"ock" => out.windows(32).any(|w| w == z),
                    _ => !out.is_empty(),
                };
            dev_info!(
                self.dev,
                "sep-key: self-test decrypt probe: {}: {}: {} (enclave status {}, {} reply bytes)\n",
                name,
                what,
                (if ok { "OK" } else { "refused" }),
                status,
                out.len()
            );
            if ok {
                bits |= 1 << row;
            }
        }
        Ok(bits)
    }

    fn key_unseal_context_probe(&self, req: &mut Selftest) -> Result<()> {
        const OECD_FREE: &[&[u8]] = &[b"oa", b"odel", b"osgn", b"oecd"];
        const NO_DECRYPT: &[&[u8]] = &[b"oa", b"odel", b"osgn"];
        const OCK_FREE: &[&[u8]] = &[b"oa", b"odel", b"osgn", b"ock"];
        const MACOS_PLAIN: &[&[u8]] = &[b"oa", b"ock", b"odel", b"okd", b"osgn"];
        let secret = req.digest;
        let keys: [(&str, Option<&[&[u8]]>); 5] = [
            ("no list", None),
            ("oecd free", Some(OECD_FREE)),
            ("no decrypt entry", Some(NO_DECRYPT)),
            ("ock free", Some(OCK_FREE)),
            ("macOS plain", Some(MACOS_PLAIN)),
        ];
        let (mut lo, mut hi) = (0i32, 0i32);
        for (i, (name, free)) in keys.iter().enumerate() {
            let bits = match self.key_unseal_context_probe_key(name, *free, &secret) {
                Ok(b) => b,
                Err(e) => {
                    dev_info!(self.dev, "sep-key: self-test decrypt probe: {}: key failed ({:?})\n", name, e);
                    0
                }
            };
            if i < 4 {
                lo |= bits << (8 * i);
            } else {
                hi = bits;
            }
        }
        req.result = lo;
        req.enclave_status = 0;
        req.result2 = hi;
        req.enclave_status2 = 0;
        Ok(())
    }

    fn key_selftest(&self, req: &mut Selftest) -> Result<()> {
        if crate::module_parameters::key_selftest.value() == 0 {
            return Err(EPERM);
        }
        crate::bio::require_admin()?;
        if req.mode == SELFTEST_PRESENCE_PROBE {
            return self.key_presence_probe(req);
        }
        if req.mode == SELFTEST_PRESENCE_OTHER_BAG {
            return self.key_presence_other_bag(req);
        }
        if req.mode == SELFTEST_USERBAG_KEY_MAKE || req.mode == SELFTEST_USERBAG_KEY_USE {
            return self.key_userbag_record(req);
        }
        if req.mode == SELFTEST_USERBAG_LOCKED {
            return self.key_userbag_locked(req);
        }
        if req.mode == SELFTEST_USERBAG_KEY_SAME_BAG || req.mode == SELFTEST_USERBAG_KEY_ROOT {
            return self.key_userbag_probe(req);
        }
        if req.mode == SELFTEST_UNSEAL_CONTEXT_PROBE {
            return self.key_unseal_context_probe(req);
        }
        let (blob, flags) = decode_handle(&req.handle, req.handle_len)?;
        if flags & HANDLE_BIOMETRY == 0 {
            return Err(EINVAL);
        }
        let mut owned = KVec::new();
        owned.extend_from_slice(blob, GFP_KERNEL)?;
        let digest = req.digest;
        let user = crate::sbio::UserId::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
        let record = |r: Result<KVec<u8>>, s: &SepData| -> (i32, i32) {
            (r.err().map(|e| e.to_errno()).unwrap_or(0), s.last_key_status.load(Relaxed) as i32)
        };
        self.last_key_status.store(0, Relaxed);
        let (result, status) = match req.mode {
            SELFTEST_NO_CONTEXT => record(self.user_key_sign(&owned, &digest, None), self),
            SELFTEST_UNSATISFIED => {
                let ctx = self.open_unlock_evaluation(user).ok_or(EIO)?;
                let r = record(self.user_key_sign(&owned, &digest, Some(&ctx)), self);
                self.close_unlock_evaluation(&ctx);
                r
            }
            SELFTEST_PASSCODE => {
                let ctx = self.passcode_context()?;
                let r = record(self.user_key_sign(&owned, &digest, Some(&ctx)), self);
                self.close_unlock_evaluation(&ctx);
                r
            }
            SELFTEST_DELETED => {
                let ctx = self.open_unlock_evaluation(user).ok_or(EIO)?;
                self.close_unlock_evaluation(&ctx);
                record(self.user_key_sign(&owned, &digest, Some(&ctx)), self)
            }
            SELFTEST_TOUCH_THEN_DELETED => {
                let job = self.key_sign_with_touch(&owned, flags, &digest, true)?;
                req.result2 = errno_of(&job.again);
                req.enclave_status2 = job.again_status;
                (errno_of(&job.sig), job.status)
            }
            SELFTEST_TOUCH_TWO_SIGNATURES => {
                let job = self.key_touch_run(&owned, flags, TouchOp::Sign, &digest, &[], false,
                                             Some(TouchOp::Sign))?;
                req.result2 = errno_of(&job.again);
                req.enclave_status2 = job.again_status;
                (errno_of(&job.sig), job.status)
            }
            SELFTEST_TOUCH_SIGN_THEN_UNSEAL => {
                let sealed = seal_to(&owned, &digest)?;
                let job = self.key_touch_run(&owned, flags, TouchOp::Sign, &digest, &sealed, false,
                                             Some(TouchOp::Unseal))?;
                req.result2 = match &job.again {
                    Some(Ok(secret)) if secret[..] == digest[..] => 0,
                    Some(Ok(_)) => EIO.to_errno(),
                    other => errno_of(other),
                };
                req.enclave_status2 = job.again_status;
                (errno_of(&job.sig), job.status)
            }
            SELFTEST_UNSEAL_UNSATISFIED => {
                let sealed = seal_to(&owned, &digest)?;
                let r = record(self.user_key_unseal(&owned, &sealed, None), self);
                self.last_key_status.store(0, Relaxed);
                let ctx = self.open_unlock_evaluation(user).ok_or(EIO)?;
                (req.result2, req.enclave_status2) =
                    record(self.user_key_unseal(&owned, &sealed, Some(&ctx)), self);
                self.close_unlock_evaluation(&ctx);
                r
            }
            _ => return Err(EINVAL),
        };
        req.result = result;
        req.enclave_status = status;
        dev_info!(
            self.dev,
            "sep-key: self-test mode {}: {} (enclave status {})\n",
            req.mode,
            (if result == 0 { "SIGNED" } else { "refused" }),
            status
        );
        Ok(())
    }

    fn key_ioctl(&self, cmd: u32, arg: usize) -> Result<()> {
        if self.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        let user = UserPtr::from_addr(arg);
        match cmd {
            IOC_INFO => UserSlice::new(user, core::mem::size_of::<Info>())
                .writer()
                .write(&info()),
            IOC_CREATE => {
                let slice = UserSlice::new(user, core::mem::size_of::<Create>());
                let (mut reader, mut writer) = slice.reader_writer();
                let mut req: Create = reader.read()?;
                if req.reserved != [0; 3] {
                    return Err(EINVAL);
                }
                let (acl, flags) = match create_policy(req.flags)? {
                    Policy::None => (None, 0),
                    Policy::BiometryCurrentSet => (Some(self.key_bio_acl(false)?), HANDLE_BIOMETRY),
                    Policy::BiometryAny => (Some(self.key_bio_acl(true)?), HANDLE_BIOMETRY | HANDLE_BIOMETRY_ANY),
                };
                let (blob, point) = self.user_key_create(acl.as_ref().map(|a| &a.ed[..]))?;
                req.handle_len = encode_handle(&blob, flags, &mut req.handle)? as u32;
                req.public_key = point;
                writer.write(&req)
            }
            IOC_SIGN => {
                let slice = UserSlice::new(user, core::mem::size_of::<Sign>());
                let (mut reader, mut writer) = slice.reader_writer();
                let mut req: Sign = reader.read()?;
                if req.flags != 0 {
                    return Err(EINVAL);
                }
                let sig = {
                    let (blob, flags) = decode_handle(&req.handle, req.handle_len)?;
                    if flags & HANDLE_BIOMETRY != 0 {
                        let digest = req.digest;
                        let job = self.key_sign_with_touch(blob, flags, &digest, false)?;
                        job.sig.ok_or(EIO)??
                    } else {
                        self.user_key_sign(blob, &req.digest, None)?
                    }
                };
                put_signature(&sig, &mut req)?;
                writer.write(&req)
            }
            IOC_PUBLIC => {
                let slice = UserSlice::new(user, core::mem::size_of::<Public>());
                let (mut reader, mut writer) = slice.reader_writer();
                let mut req: Public = reader.read()?;
                if req.flags != 0 || req.reserved != [0; 3] {
                    return Err(EINVAL);
                }
                req.public_key =
                    SepData::user_key_public(decode_handle(&req.handle, req.handle_len)?.0)?;
                writer.write(&req)
            }
            IOC_CANCEL => {
                // Ends the touch-gated call that is waiting, whoever made it:
                // like pulling a security key, it can only make a request fail.
                if self.key_touch_active.load(Relaxed) {
                    self.key_touch_cancel.store(true, Relaxed);
                    dev_info!(self.dev, "sep-key: waiting operation cancelled\n");
                }
                Ok(())
            }
            IOC_SEAL | IOC_UNSEAL => {
                let slice = UserSlice::new(user, core::mem::size_of::<Seal>());
                let (mut reader, mut writer) = slice.reader_writer();
                let mut req: Seal = reader.read()?;
                if req.flags != 0 {
                    return Err(EINVAL);
                }
                let (blob, flags) = decode_handle(&req.handle, req.handle_len)?;
                // Only for keys whose use the enclave gates on a touch.
                if flags & HANDLE_BIOMETRY == 0 {
                    return Err(EINVAL);
                }
                if cmd == IOC_SEAL {
                    let n = req.secret_len as usize;
                    if n == 0 || n > SECRET_MAX {
                        return Err(EINVAL);
                    }
                    let sealed = seal_to(blob, &req.secret[..n])?;
                    req.sealed.fill(0);
                    req.sealed[..sealed.len()].copy_from_slice(&sealed);
                    req.sealed_len = sealed.len() as u32;
                } else {
                    let n = req.sealed_len as usize;
                    if n == 0 || n > SEALED_MAX {
                        return Err(EINVAL);
                    }
                    let job = self.key_touch_run(blob, flags, TouchOp::Unseal, &[0; DIGEST_LEN],
                                                 &req.sealed[..n], false, None)?;
                    let secret = job.sig.ok_or(EIO)??;
                    let r = put_secret(&secret, &mut req);
                    let mut secret = secret;
                    secret.fill(0);
                    r?;
                }
                let r = writer.write(&req);
                req.secret.fill(0);
                r
            }
            IOC_SELFTEST => {
                let slice = UserSlice::new(user, core::mem::size_of::<Selftest>());
                let (mut reader, mut writer) = slice.reader_writer();
                let mut req: Selftest = reader.read()?;
                self.key_selftest(&mut req)?;
                writer.write(&req)
            }
            _ => Err(ENOTTY),
        }
    }
}

// /dev/sep-key has no session: any number of openers, no poll.

/// # Safety
/// `ctx` is the `SepData` pointer given to `register_key`, valid while the
/// registration lives (see `register_key`).
unsafe extern "C" fn key_open_trampoline(ctx: *mut c_void) -> c_int {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    if this.shutting_down.load(Relaxed) {
        return ENODEV.to_errno();
    }
    0
}

/// # Safety
/// As for `key_open_trampoline`.
unsafe extern "C" fn key_release_trampoline(_ctx: *mut c_void) {}

/// # Safety
/// As for `key_open_trampoline`.
unsafe extern "C" fn key_ioctl_trampoline(ctx: *mut c_void, cmd: c_uint, arg: c_ulong) -> c_long {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    match this.key_ioctl(cmd, arg) {
        Ok(()) => 0,
        Err(e) => e.to_errno() as c_long,
    }
}

/// # Safety
/// As for `key_open_trampoline`.
unsafe extern "C" fn key_ready_trampoline(_ctx: *mut c_void) -> c_int {
    0
}
