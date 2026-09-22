// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Request and response images for the key-store endpoint.

use kernel::prelude::*;

extern "C" {
    fn sep_sha256(
        a: *const c_void,
        alen: usize,
        b: *const c_void,
        blen: usize,
        out: *mut u8,
    ) -> c_int;
}

pub(crate) const HEADER_SIZE: u32 = 0x50;

pub(crate) const HEADER_WIRE: usize = 0x54;

const DIGEST_OFF: usize = 0x00;
const DIGEST_LEN: usize = 16;

const DIGEST_FROM: usize = 0x10;

pub(crate) const SHA256_LEN: usize = 32;

pub(crate) fn sha256(bytes: &[u8]) -> Result<[u8; SHA256_LEN]> {
    let mut digest = [0u8; SHA256_LEN];
    // SAFETY: `bytes` is a live slice for the duration of the call and `digest`
    // is exactly the 32 bytes the shim writes. The shim reads the second
    // segment only when its length is nonzero, and it is zero here.
    let rc = unsafe {
        sep_sha256(
            bytes.as_ptr().cast(),
            bytes.len(),
            core::ptr::null(),
            0,
            digest.as_mut_ptr(),
        )
    };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    Ok(digest)
}

const OFF_VERSION: usize = 0x10;
const OFF_TIMESTAMP: usize = 0x14;
const OFF_TRAILER: usize = 0x48;

static_assert!(OFF_VERSION == DIGEST_OFF + DIGEST_LEN);
static_assert!(OFF_TIMESTAMP == OFF_VERSION + 4);
static_assert!(OFF_TRAILER == OFF_TIMESTAMP + 8 + 4 + 8 + 8 + 4 + 20);
static_assert!(OFF_TRAILER + 8 == HEADER_SIZE as usize);
static_assert!(HEADER_WIRE == 4 + HEADER_SIZE as usize);

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Version {
    V1,
}

impl Version {
    pub(crate) const fn wire(self) -> u32 {
        match self {
            Version::V1 => 1,
        }
    }

    const fn digest_end(self) -> usize {
        match self {
            Version::V1 => OFF_TRAILER,
        }
    }
}

pub(crate) struct Body {
    bytes: KVec<u8>,
}

pub(crate) fn wipe(bytes: &mut [u8]) {
    for byte in bytes {
        // SAFETY: `byte` is uniquely borrowed and valid. A volatile write keeps
        // the compiler from eliding destruction of request secrets.
        unsafe { core::ptr::write_volatile(byte, 0) };
    }
}

impl Body {
    pub(crate) fn new() -> Body {
        Body { bytes: KVec::new() }
    }

    pub(crate) fn put_u32(&mut self, v: u32) -> Result<()> {
        self.bytes.extend_from_slice(&v.to_le_bytes(), GFP_KERNEL)?;
        Ok(())
    }

    pub(crate) fn put_i32(&mut self, v: i32) -> Result<()> {
        self.bytes.extend_from_slice(&v.to_le_bytes(), GFP_KERNEL)?;
        Ok(())
    }

    pub(crate) fn put_u64(&mut self, v: u64) -> Result<()> {
        self.bytes.extend_from_slice(&v.to_le_bytes(), GFP_KERNEL)?;
        Ok(())
    }

    pub(crate) fn put_blob(&mut self, bytes: &[u8]) -> Result<()> {
        let len = u32::try_from(bytes.len()).map_err(|_| EINVAL)?;
        self.put_u32(len)?;
        self.bytes.extend_from_slice(bytes, GFP_KERNEL)?;
        let pad = bytes.len().wrapping_neg() % 4;
        for _ in 0..pad {
            self.bytes.push(0, GFP_KERNEL)?;
        }
        Ok(())
    }

    fn as_slice(&self) -> &[u8] {
        &self.bytes
    }
}

impl Drop for Body {
    fn drop(&mut self) {
        wipe(&mut self.bytes);
    }
}

const fn pad_of(len: usize) -> usize {
    len.wrapping_neg() % 4
}
static_assert!(pad_of(0) == 0);
static_assert!(pad_of(1) == 3);
static_assert!(pad_of(2) == 2);
static_assert!(pad_of(3) == 1);
static_assert!(pad_of(4) == 0);

pub(crate) struct RequestImage {
    bytes: KVec<u8>,
}

impl RequestImage {
    pub(crate) fn as_slice(&self) -> &[u8] {
        &self.bytes
    }

    pub(crate) fn len(&self) -> usize {
        self.bytes.len()
    }
}

impl Drop for RequestImage {
    fn drop(&mut self) {
        wipe(&mut self.bytes);
    }
}

pub(crate) fn build_request(
    version: Version,
    timestamp_us: u64,
    body: &Body,
) -> Result<RequestImage> {
    let mut image = RequestImage { bytes: KVec::new() };
    let bytes = &mut image.bytes;
    bytes.extend_from_slice(&HEADER_SIZE.to_le_bytes(), GFP_KERNEL)?;

    let header_at = bytes.len();
    for _ in 0..HEADER_SIZE {
        bytes.push(0, GFP_KERNEL)?;
    }

    let put = |bytes: &mut KVec<u8>, off: usize, src: &[u8]| {
        bytes[header_at + off..header_at + off + src.len()].copy_from_slice(src);
    };
    put(bytes, OFF_VERSION, &version.wire().to_le_bytes());
    put(bytes, OFF_TIMESTAMP, &timestamp_us.to_le_bytes());
    // Flags, reserved, proc_id, pid, cdhash and trailer stay zero; the enclave accepts that.

    bytes.extend_from_slice(body.as_slice(), GFP_KERNEL)?;

    static_assert!(OFF_VERSION == DIGEST_FROM);
    let end = version.digest_end();
    let head = &bytes[header_at + DIGEST_FROM..header_at + end];
    let tail = &bytes[header_at + HEADER_SIZE as usize..];

    let mut digest = [0u8; SHA256_LEN];
    // SAFETY: `head` and `tail` are live slices of `bytes` for the duration of
    // the call, and `digest` is exactly the 32 bytes the shim writes. The shim
    // reads only the two segments it is given and writes only the output.
    let rc = unsafe {
        sep_sha256(
            head.as_ptr().cast(),
            head.len(),
            tail.as_ptr().cast(),
            tail.len(),
            digest.as_mut_ptr(),
        )
    };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }

    bytes[header_at + DIGEST_OFF..header_at + DIGEST_OFF + DIGEST_LEN]
        .copy_from_slice(&digest[..DIGEST_LEN]);

    Ok(image)
}

pub(crate) struct ResponseImage<'a> {
    pub(crate) body: &'a [u8],
}

pub(crate) fn parse_response(bytes: &[u8]) -> Result<ResponseImage<'_>> {
    if bytes.len() < 4 {
        return Err(EINVAL);
    }
    let header_size = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);

    let header = header_size as usize;
    if header < OFF_VERSION + 4 {
        return Err(EINVAL);
    }
    let body_at = 4usize.checked_add(header).ok_or(EINVAL)?;
    if bytes.len() < body_at {
        return Err(EINVAL);
    }

    Ok(ResponseImage {
        body: &bytes[body_at..],
    })
}

pub(crate) fn read_blob(body: &[u8], off: usize) -> Option<(&[u8], usize)> {
    let len_end = off.checked_add(4)?;
    if body.len() < len_end {
        return None;
    }
    let len = u32::from_le_bytes([body[off], body[off + 1], body[off + 2], body[off + 3]]) as usize;
    let end = len_end.checked_add(len)?;
    if body.len() < end {
        return None;
    }
    let next = end.checked_add(pad_of(len))?;
    if body.get(end..next)?.iter().any(|byte| *byte != 0) {
        return None;
    }
    Some((&body[len_end..end], next))
}

pub(crate) fn operation_status(body: &[u8]) -> Option<i32> {
    if body.len() < 4 {
        return None;
    }
    Some(i32::from_le_bytes([body[0], body[1], body[2], body[3]]))
}
