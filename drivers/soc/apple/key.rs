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
//! There is no access control in this version: anyone who can open the node
//! and holds a handle can sign with it. The `flags` words are reserved for an
//! enclave-enforced policy and must be zero.

use crate::SepData;
use kernel::ioctl::{_IOR, _IOWR};
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;
use kernel::uaccess::{UserPtr, UserSlice};

pub(crate) const DEVICE_NAME: &CStr = c"sep-key";
/// Root only by default; distributions grant a group with a udev rule.
pub(crate) const DEVICE_MODE: u16 = 0o600;

pub(crate) const IFACE_VERSION: u32 = 1;

pub(crate) const HANDLE_MAX: usize = 2048;
pub(crate) const PUB_LEN: usize = 65;
pub(crate) const DIGEST_LEN: usize = 32;
pub(crate) const SIG_MAX: usize = 72;

const MAGIC: u32 = 0xB1;

/// Handle header: `"SEPK"`, version, flags, reserved u16, blob length (LE u32).
const HANDLE_MAGIC: &[u8; 4] = b"SEPK";
const HANDLE_VERSION: u8 = 1;
pub(crate) const HANDLE_HEADER: usize = 12;
/// The largest enclave record a handle can carry.
pub(crate) const BLOB_MAX: usize = HANDLE_MAX - HANDLE_HEADER;

#[repr(C)]
pub(crate) struct Info {
    pub(crate) version: u32,
    /// `flags` bits `CREATE` and `SIGN` accept; zero in this version.
    pub(crate) flags_supported: u32,
    pub(crate) handle_max: u32,
    pub(crate) reserved: u32,
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

static_assert!(core::mem::size_of::<Info>() == 16);
static_assert!(core::mem::size_of::<Create>() == 2124);
static_assert!(core::mem::size_of::<Sign>() == 2164);
static_assert!(core::mem::size_of::<Public>() == 2124);

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

pub(crate) const IOC_INFO: u32 = _IOR::<Info>(MAGIC, 0x20);
pub(crate) const IOC_CREATE: u32 = _IOWR::<Create>(MAGIC, 0x21);
pub(crate) const IOC_SIGN: u32 = _IOWR::<Sign>(MAGIC, 0x22);
pub(crate) const IOC_PUBLIC: u32 = _IOWR::<Public>(MAGIC, 0x23);

pub(crate) fn info() -> Info {
    Info {
        version: IFACE_VERSION,
        flags_supported: 0,
        handle_max: HANDLE_MAX as u32,
        reserved: 0,
    }
}

/// Writes the handle for `blob` into `out`; returns its length.
pub(crate) fn encode_handle(blob: &[u8], out: &mut [u8; HANDLE_MAX]) -> Result<usize> {
    if blob.is_empty() || blob.len() > BLOB_MAX {
        return Err(EINVAL);
    }
    let len = u32::try_from(blob.len()).map_err(|_| EINVAL)?;
    out.fill(0);
    out[..4].copy_from_slice(HANDLE_MAGIC);
    out[4] = HANDLE_VERSION;
    out[5] = 0;
    out[8..12].copy_from_slice(&len.to_le_bytes());
    out[HANDLE_HEADER..HANDLE_HEADER + blob.len()].copy_from_slice(blob);
    Ok(HANDLE_HEADER + blob.len())
}

/// The enclave record inside the first `len` bytes of `handle`. Refuses an
/// unknown version, any flag, a non-zero reserved field or a length that does
/// not match exactly.
pub(crate) fn decode_handle(handle: &[u8; HANDLE_MAX], len: u32) -> Result<&[u8]> {
    let len = len as usize;
    if !(HANDLE_HEADER + 1..=HANDLE_MAX).contains(&len) {
        return Err(EINVAL);
    }
    if &handle[..4] != HANDLE_MAGIC || handle[4] != HANDLE_VERSION || handle[5] != 0 {
        return Err(EINVAL);
    }
    if handle[6] != 0 || handle[7] != 0 {
        return Err(EINVAL);
    }
    let blob_len = u32::from_le_bytes([handle[8], handle[9], handle[10], handle[11]]) as usize;
    if blob_len != len - HANDLE_HEADER {
        return Err(EINVAL);
    }
    Ok(&handle[HANDLE_HEADER..len])
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
        let n = encode_handle(&blob(), &mut h).unwrap();
        assert_eq!(n, HANDLE_HEADER + 100);
        assert_eq!(&h[..4], b"SEPK");
        assert_eq!(decode_handle(&h, n as u32).unwrap(), &blob()[..]);
    }

    #[test]
    fn length_must_match_exactly() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), &mut h).unwrap() as u32;
        assert!(decode_handle(&h, n - 1).is_err());
        assert!(decode_handle(&h, n + 1).is_err());
        assert!(decode_handle(&h, 0).is_err());
        assert!(decode_handle(&h, HANDLE_MAX as u32 + 1).is_err());
    }

    #[test]
    fn header_fields_are_checked() {
        let mut h = [0u8; HANDLE_MAX];
        let n = encode_handle(&blob(), &mut h).unwrap() as u32;
        for at in [0usize, 4, 5, 6, 7] {
            let mut bad = h;
            bad[at] ^= 0x01;
            assert!(decode_handle(&bad, n).is_err());
        }
    }

    #[test]
    fn empty_and_oversize_blobs_are_refused() {
        let mut h = [0u8; HANDLE_MAX];
        assert!(encode_handle(&[], &mut h).is_err());
        let big = [1u8; BLOB_MAX + 1];
        assert!(encode_handle(&big, &mut h).is_err());
        let max = [1u8; BLOB_MAX];
        assert_eq!(encode_handle(&max, &mut h).unwrap(), HANDLE_MAX);
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
}

impl SepData {
    pub(crate) fn register_key(&self) -> Result<()> {
        let mut guard = self.key_dev.lock();
        if guard.is_some() {
            return Ok(());
        }
        let ctx = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        // SAFETY: as in `register_bio`: `remove()` drops the registration
        // before the `Arc` that keeps `ctx` alive, and the shim's unregister
        // clears the stored context under the lock every callback takes.
        let dev = unsafe {
            crate::shim::BioChardev::register(
                DEVICE_NAME,
                DEVICE_MODE,
                ctx,
                key_open_trampoline,
                key_release_trampoline,
                key_ioctl_trampoline,
                key_ready_trampoline,
            )
        }?;
        *guard = Some(dev);
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
                if req.flags != 0 || req.reserved != [0; 3] {
                    return Err(EINVAL);
                }
                let (blob, point) = self.user_key_create()?;
                req.handle_len = encode_handle(&blob, &mut req.handle)? as u32;
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
                    let blob = decode_handle(&req.handle, req.handle_len)?;
                    self.user_key_sign(blob, &req.digest)?
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
                    SepData::user_key_public(decode_handle(&req.handle, req.handle_len)?)?;
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
