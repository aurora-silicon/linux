// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Authorization contexts on `/dev/sep-key` (`SEP_KEY_CTX_*` in
//! `include/uapi/linux/apple-sep.h`, a design draft).

use super::*;
use crate::key_context::Context;

#[derive(Clone, Copy)]
#[repr(C)]
pub(super) struct Buffer {
    pub(super) len: u32,
    pub(super) reserved: u32,
    pub(super) ptr: u64,
}
/// `flags` NEW_BAG: the context authorizes keys under `uid`'s bag (root or
/// that user). `lifetime_s`: how long one match authorizes, from the match;
/// 0 until revoked or closed.
#[repr(C)]
struct NewContext {
    flags: u32,
    fd: i32,
    uid: u32,
    lifetime_s: u32,
}
const NEW_BAG: u32 = 1;
/// ECIES to a P-256 key: the uncompressed ephemeral point, then the 16-byte
/// tag; the IV is derived, so this is all that is not ciphertext.
const SEALED_OVERHEAD: usize = crate::refkey_seal::POINT_LEN + 16;
const UNCOMPRESSED_POINT: u8 = 0x04;
const METHOD_TOUCH: u32 = 1;
const OUTCOME_OK: u32 = 0;
const OUTCOME_NOT_MATCHED: u32 = 1;
const OUTCOME_FAILED: u32 = 5;
const LIFETIME_MAX_S: u32 = 12 * 3600;
#[repr(C)]
struct Authenticate {
    id: u64,
    timeout_ms: u32,
    method: u32,
    outcome: u32,
    reserved: u32,
}
#[repr(C)]
struct Query {
    phase: u32,
    cleanup: u32,
    reason: i32,
    activity_flags: u32,
}
#[repr(C)]
struct Revoke {
    timeout_ms: u32,
    reserved: u32,
}
#[repr(C)]
struct ContextSign {
    id: u64,
    timeout_ms: u32,
    context_fd: i32,
    blob_len: u32,
    reserved: u32,
    blob_ptr: u64,
    digest: [u8; 32],
    sig_len: u32,
    reserved2: u32,
    signature: [u8; 72],
}
#[repr(C)]
struct ContextDecrypt {
    id: u64,
    timeout_ms: u32,
    context_fd: i32,
    blob: Buffer,
    sealed: Buffer,
    plain: Buffer,
}

static_assert!(core::mem::size_of::<NewContext>() == 16);
static_assert!(core::mem::size_of::<Authenticate>() == 24);
static_assert!(core::mem::size_of::<Query>() == 16);
static_assert!(core::mem::size_of::<Revoke>() == 8);
static_assert!(core::mem::size_of::<ContextSign>() == 144);
static_assert!(core::mem::size_of::<ContextDecrypt>() == 64);
// SAFETY: fixed-width repr(C) layouts, no padding, all bit patterns valid.
unsafe impl kernel::transmute::FromBytes for NewContext {}
unsafe impl kernel::transmute::AsBytes for NewContext {}
unsafe impl kernel::transmute::FromBytes for Authenticate {}
unsafe impl kernel::transmute::AsBytes for Authenticate {}
unsafe impl kernel::transmute::AsBytes for Query {}
unsafe impl kernel::transmute::FromBytes for Revoke {}
unsafe impl kernel::transmute::FromBytes for ContextSign {}
unsafe impl kernel::transmute::AsBytes for ContextSign {}
unsafe impl kernel::transmute::FromBytes for ContextDecrypt {}
unsafe impl kernel::transmute::AsBytes for ContextDecrypt {}

const NEW: u32 = kernel::ioctl::_IOWR::<NewContext>(crate::key::MAGIC, 0x40);
const AUTH: u32 = kernel::ioctl::_IOWR::<Authenticate>(crate::key::MAGIC, 0x41);
const QUERY: u32 = kernel::ioctl::_IOR::<Query>(crate::key::MAGIC, 0x42);
const REVOKE: u32 = kernel::ioctl::_IOW::<Revoke>(crate::key::MAGIC, 0x43);
const SIGN: u32 = kernel::ioctl::_IOWR::<ContextSign>(crate::key::MAGIC, 0x44);
const DECRYPT: u32 = kernel::ioctl::_IOWR::<ContextDecrypt>(crate::key::MAGIC, 0x45);

pub(super) fn handles(cmd: u32) -> bool {
    matches!(cmd, NEW | SIGN | DECRYPT)
}

extern "C" {
    fn sep_context_file_new(
        context: *mut c_void,
        release: unsafe extern "C" fn(*mut c_void),
        ioctl: unsafe extern "C" fn(*mut c_void, c_uint, c_ulong) -> c_long,
        file: *mut *mut c_void,
        fd: *mut c_int,
    ) -> c_int;
    fn sep_context_file_finish(file: *mut c_void, fd: c_int, install: bool);
    fn sep_context_file_get(fd: c_int, context: *mut *mut c_void) -> *mut c_void;
    fn sep_context_file_put(file: *mut c_void);
}

pub(super) fn deadline(timeout_ms: u32) -> Result<u64> {
    if !(1..=120_000).contains(&timeout_ms) {
        return Err(EINVAL);
    }
    Ok(crate::shim::monotonic_ns().saturating_add(u64::from(timeout_ms) * 1_000_000))
}

fn read(buffer: &Buffer, min: usize) -> Result<KVec<u8>> {
    if buffer.reserved != 0 || !(min..=keys::HANDLE_MAX).contains(&(buffer.len as usize)) {
        return Err(EINVAL);
    }
    let mut data = KVec::new();
    data.resize(
        buffer.len as usize,
        0,
        kernel::alloc::flags::GFP_KERNEL_ACCOUNT,
    )?;
    UserSlice::new(UserPtr::from_addr(buffer.ptr as usize), data.len())
        .reader()
        .read_slice(&mut data)?;
    Ok(data)
}

/// The record behind a handle the context may authorize: an identity-bag
/// biometric key, or a user-presence key under the context's own bag.
fn decode_record(handle: &[u8], bag: Option<u32>) -> Result<KVec<u8>> {
    let h = keys::decode_any_handle_bytes(handle)?;
    let allowed = match bag {
        None => h.uid.is_none() && h.flags & keys::HANDLE_BIOMETRY != 0,
        Some(uid) => h.uid == Some(uid) && h.flags == keys::HANDLE_USER_BAG,
    };
    if !allowed {
        return Err(EOPNOTSUPP);
    }
    let mut record = KVec::new();
    record.extend_from_slice(h.blob, kernel::alloc::flags::GFP_KERNEL_ACCOUNT)?;
    Ok(record)
}

fn get_context(fd: i32) -> Result<Arc<Context>> {
    let mut pointer = core::ptr::null_mut();
    // SAFETY: the shim validates the file operations and holds an fget reference.
    let file = unsafe { sep_context_file_get(fd, &mut pointer) };
    if file.is_null() {
        return Err(EBADF);
    }
    // SAFETY: the file owns this foreign Arc until the matching put below.
    let context: Arc<Context> = unsafe { <Arc<Context> as ForeignOwnable>::borrow(pointer) }.into();
    // SAFETY: release exactly the reference returned by file_get.
    unsafe { sep_context_file_put(file) };
    Ok(context)
}

impl File {
    fn context(&self, fd: i32) -> Result<Arc<Context>> {
        let context = get_context(fd)?;
        if !Arc::ptr_eq(&context.sep, &self.sep) {
            return Err(EXDEV);
        }
        context.check_domain()?;
        Ok(context)
    }

    fn context_execute(
        &self,
        context: &Arc<Context>,
        request: Arc<Request>,
    ) -> Result<crate::key_request::Output> {
        context.register(&request.completion)?;
        let result = (|| {
            SepData::queue_context_command(self.sep.clone(), request.clone())?;
            request.completion.wait_ready()?;
            context.claim(&request.completion)
        })();
        context.forget(&request.completion);
        result
    }

    pub(super) fn context_ioctl(&self, cmd: u32, user: UserPtr) -> Result {
        self.check_api()?;
        if self.context_generation != self.sep.key_context_generation.load(Relaxed) {
            return Err(ENODEV);
        }
        if cmd == NEW {
            let mut out: NewContext = UserSlice::new(user, 16).reader().read()?;
            if out.flags & !NEW_BAG != 0 || out.fd != -1 || out.lifetime_s > LIFETIME_MAX_S {
                return Err(EINVAL);
            }
            let bag = if out.flags & NEW_BAG != 0 {
                keys::bag_user(out.uid)?;
                keys::require_admin_or(out.uid)?;
                Some(out.uid)
            } else {
                if out.uid != 0 {
                    return Err(EINVAL);
                }
                None
            };
            let context = Context::new(
                self.sep.clone(),
                self.context_generation,
                bag,
                out.lifetime_s,
            )?;
            let pointer = context.clone().into_foreign();
            let mut file = core::ptr::null_mut();
            let mut fd = -1;
            // SAFETY: ownership transfers to C only on success; callbacks match.
            let status = unsafe {
                sep_context_file_new(pointer, release_context, context_ioctl, &mut file, &mut fd)
            };
            if status != 0 {
                // SAFETY: failed creation did not consume this foreign Arc.
                drop(unsafe { <Arc<Context> as ForeignOwnable>::from_foreign(pointer) });
                context.close();
                return Err(Error::from_errno(status));
            }
            out.fd = fd;
            let result = UserSlice::new(user, 16).writer().write(&out);
            // SAFETY: reserved fd and file belong to this task. Install only
            // after copyout; failure releases both and asynchronously closes.
            unsafe { sep_context_file_finish(file, fd, result.is_ok()) };
            return result;
        }
        if cmd == SIGN {
            let mut out: ContextSign = UserSlice::new(user, 144).reader().read()?;
            if out.reserved != 0
                || out.reserved2 != 0
                || out.sig_len != 0
                || out.signature != [0; 72]
            {
                return Err(EINVAL);
            }
            let deadline_ns = deadline(out.timeout_ms)?;
            let blob = read(
                &Buffer {
                    len: out.blob_len,
                    reserved: 0,
                    ptr: out.blob_ptr,
                },
                1,
            )?;
            let context = self.context(out.context_fd)?;
            let blob = decode_record(&blob, context.bag)?;
            let request = Request::new(
                out.id,
                crate::key_request::KeyRequest {
                    blob,
                    input: crate::key_request::Input::ContextSign(context.clone(), out.digest),
                    deadline_ns,
                },
            )?;
            let _active = self.begin(request.clone())?;
            let crate::key_request::Output::Signature(signature) =
                self.context_execute(&context, request)?
            else {
                return Err(EIO);
            };
            if signature.is_empty() || signature.len() > keys::SIG_MAX {
                return Err(EIO);
            }
            out.sig_len = signature.len() as u32;
            out.signature[..signature.len()].copy_from_slice(&signature);
            return UserSlice::new(user, 144).writer().write(&out);
        }
        let mut out: ContextDecrypt = UserSlice::new(user, 64).reader().read()?;
        let deadline_ns = deadline(out.timeout_ms)?;
        let blob = read(&out.blob, 1)?;
        let sealed = read(&out.sealed, SEALED_OVERHEAD)?;
        if sealed.len() > keys::SEALED_MAX
            || sealed[0] != UNCOMPRESSED_POINT
            || out.plain.reserved != 0
            || out.plain.len as usize > keys::HANDLE_MAX
        {
            return Err(EINVAL);
        }
        if (out.plain.len as usize) < sealed.len() - SEALED_OVERHEAD {
            return Err(ENOSPC);
        }
        let context = self.context(out.context_fd)?;
        let blob = decode_record(&blob, context.bag)?;
        let request = Request::new(
            out.id,
            crate::key_request::KeyRequest {
                blob,
                input: crate::key_request::Input::ContextDecrypt(context.clone(), sealed),
                deadline_ns,
            },
        )?;
        let _active = self.begin(request.clone())?;
        let crate::key_request::Output::Plaintext(plain) =
            self.context_execute(&context, request)?
        else {
            return Err(EIO);
        };
        if plain.len() > out.plain.len as usize || plain.len() > keys::SECRET_MAX {
            return Err(EIO);
        }
        UserSlice::new(
            UserPtr::from_addr(out.plain.ptr as usize),
            out.plain.len as usize,
        )
        .writer()
        .write_slice(&plain)?;
        out.plain.len = plain.len() as u32;
        UserSlice::new(user, 64).writer().write(&out)
    }
}

/// A context file's ioctl.
///
/// # Safety
/// `pointer` is the foreign `Context` Arc the anonymous file holds; the file
/// reference the VFS holds for this call keeps it from being released.
unsafe extern "C" fn context_ioctl(pointer: *mut c_void, cmd: c_uint, arg: c_ulong) -> c_long {
    // SAFETY: live until `release_context`, which cannot run during this call.
    let borrowed = unsafe { <Arc<Context> as ForeignOwnable>::borrow(pointer) };
    let context: Arc<Context> = borrowed.into();
    let result = (|| {
        let _call = Call::enter(&context.sep)?;
        let user = UserPtr::from_addr(arg as usize);
        if cmd == QUERY {
            let (phase, cleanup, reason) = context.query();
            return UserSlice::new(user, 16).writer().write(&Query {
                phase,
                cleanup,
                reason,
                activity_flags: context.sep.context_sensor_waiting(&context) as u32,
            });
        }
        if cmd == REVOKE {
            let request: Revoke = UserSlice::new(user, 8).reader().read()?;
            if request.reserved != 0 {
                return Err(EINVAL);
            }
            let until = deadline(request.timeout_ms)?;
            context.revoke(EKEYREVOKED);
            return context.wait_revoke(until);
        }
        if cmd == IOC_CANCEL {
            let request: Cancel = UserSlice::new(user, 8).reader().read()?;
            return context.cancel(request.request_id);
        }
        if cmd != AUTH {
            return Err(ENOTTY);
        }
        context.check_domain()?;
        if context.sep.key_context_failed.load(Relaxed) {
            return Err(EIO);
        }
        let mut out: Authenticate = UserSlice::new(user, 24).reader().read()?;
        if out.id == 0 || out.reserved != 0 || out.outcome != 0 {
            return Err(EINVAL);
        }
        if out.method != METHOD_TOUCH {
            return Err(EOPNOTSUPP);
        }
        let request = Request::new(
            out.id,
            crate::key_request::KeyRequest {
                blob: KVec::new(),
                input: crate::key_request::Input::Authenticate(context.clone()),
                deadline_ns: deadline(out.timeout_ms)?,
            },
        )?;
        SepData::queue_key(context.sep.clone(), request.clone())?;
        let result = request
            .completion
            .wait_ready()
            .and_then(|()| context.claim(&request.completion));
        context.forget(&request.completion);
        let status = match result {
            Ok(crate::key_request::Output::Authenticated) => Ok(()),
            Ok(_) => Err(EIO),
            Err(error) => Err(error),
        };
        if let Err(error) = status {
            context.revoke(error);
        }
        out.outcome = match status {
            Ok(()) => OUTCOME_OK,
            Err(error) if error == EACCES => OUTCOME_NOT_MATCHED,
            _ => OUTCOME_FAILED,
        };
        if let Err(error) = UserSlice::new(user, 24).writer().write(&out) {
            context.revoke(error);
            return Err(error);
        }
        status
    })();
    match result {
        Ok(()) => 0,
        Err(error) => error.to_errno() as c_long,
    }
}

/// A context file's release.
///
/// # Safety
/// C passes the file's foreign `Context` Arc once, after every file call has
/// returned.
unsafe extern "C" fn release_context(pointer: *mut c_void) {
    // SAFETY: the foreign Arc the file held, given back exactly once.
    let context = unsafe { <Arc<Context> as ForeignOwnable>::from_foreign(pointer) };
    context.close();
}
