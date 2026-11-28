// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! This Mac's own Touch ID factory records, read from its iBoot System
//! Container when no packaged firmware file provides them.
//!
//! The container's Hardware volume keeps them in its factory-data cache:
//! the calibration (`FSCl-<sensor>` or `FSC2-<sensor>`), on Macs whose
//! enclave takes one the sensor's signed root key (`MSRk-<sensor>`), the
//! sealing manifest (`seal-*`) and the trust bundle (`trustobject-current`
//! and `trustobject-<sha256>`). The enclave loads a record only as
//!
//! `comb { fdrd: IMG4 { IM4P, [0] seal }, secb: trust bundle }`
//!
//! An FSCl record is stored in that layout and is used as it is. A
//! standalone FSC2 or MSRk image is rebuilt in it from its own IM4P, the
//! seal and the trust bundle. Nothing is synthesised: every byte is this
//! Mac's, and the enclave verifies the signatures. The checks here are of
//! structure and provenance, and anything they do not admit is refused.
//!
//! The container is this boot's only when its preboot volume holds the
//! directory of the volume group that `/chosen/apfs-preboot-uuid` names, so
//! another Mac's container, on an external disk, is refused.
//!
//! The module uses `core` and [`apfs_read`] alone, so the host tests build
//! it unchanged.

use crate::apfs_read::{Container, Entry, Media, Read, Refusal, Volume, DT_DIR, DT_REG, ROOT_DIR};

const ROLE_PREBOOT: u16 = 0x10;
const ROLE_HARDWARE: u16 = 5 << 6;
const FACTORY_DIR: [&[u8]; 5] = [
    b"FactoryData",
    b"System",
    b"Library",
    b"Caches",
    b"com.apple.factorydata",
];

/// Larger than any factory record (the largest measured, on a J313, is
/// 75,889 bytes).
const RECORD_MAX: usize = 1 << 20;
const SUFFIX_MAX: usize = 64;
const BUNDLES_MAX: usize = 8;
const CHILDREN_MAX: usize = 64;

const CALIBRATION_KINDS: [&[u8]; 2] = [b"FSCl", b"FSC2"];
const STANDALONE_CALIBRATION: [&[u8]; 1] = [b"FSC2"];
const ROOT_KEY: [&[u8]; 1] = [b"MSRk"];
const TRUST_CURRENT: &[u8] = b"trustobject-current";
const TRUST_PREFIX: &[u8] = b"trustobject-";
const SEAL_PREFIX: &[u8] = b"seal-";

const IA5: u8 = 0x16;
const OCTET: u8 = 0x04;
const SEQ: u8 = 0x30;
const CTX0: u8 = 0xa0;

/// The records the enclave loads, each in the layout it loads them.
pub(crate) struct Records<B> {
    /// The calibration.
    pub(crate) calibration: Read<B>,
    /// Whether the calibration was rebuilt from a standalone image.
    pub(crate) resealed: bool,
    /// The signed root key, when it was asked for.
    pub(crate) root_key: Option<Read<B>>,
}

/// Reads this Mac's records from the container on `media`. `boot_group` is
/// `/chosen/apfs-preboot-uuid`; `root_key` asks for the signed root key as
/// well; `sha256` names the trust bundle.
pub(crate) fn read<M: Media>(
    media: &M,
    boot_group: &[u8; 16],
    root_key: bool,
    sha256: &dyn Fn(&[u8]) -> Read<[u8; 32]>,
) -> Read<Records<M::Buf>> {
    let container = Container::open(media)?;
    let preboot = container.volume(ROLE_PREBOOT)?;
    let group = group_name(boot_group);
    let own = match container.lookup(&preboot, ROOT_DIR, &|name| {
        name.eq_ignore_ascii_case(&group)
    }) {
        Err(Refusal::Absent) => return Err(Refusal::Foreign),
        found => found?,
    };
    if own.kind != DT_DIR {
        return Err(Refusal::Foreign);
    }

    let hardware = container.volume(ROLE_HARDWARE)?;
    let mut dir = ROOT_DIR;
    for part in FACTORY_DIR {
        let entry = container.lookup(&hardware, dir, &|name| name == part)?;
        if entry.kind != DT_DIR {
            return Err(Refusal::Malformed);
        }
        dir = entry.id;
    }
    let mut listing = Listing::new();
    container.list(&hardware, dir, &mut |name, entry| listing.add(name, entry))?;
    if listing.roots > 1 {
        return Err(Refusal::Ambiguous);
    }

    let files = Files {
        media,
        container: &container,
        volume: &hardware,
        sha256,
    };
    let mut sealing: Option<Read<Sealing<M::Buf>>> = None;
    let (calibration, resealed) = match files.calibration(&listing, &mut sealing) {
        Ok((bytes, resealed)) => (Ok(bytes), resealed),
        Err(refusal) => (Err(refusal), false),
    };
    let root_key = root_key.then(|| files.root_key(&listing, &mut sealing));
    Ok(Records {
        calibration,
        resealed,
        root_key,
    })
}

/// `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`, the volume group's directory.
fn group_name(uuid: &[u8; 16]) -> [u8; 36] {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut out = [0u8; 36];
    let mut at = 0;
    for (i, byte) in uuid.iter().enumerate() {
        if matches!(i, 4 | 6 | 8 | 10) {
            out[at] = b'-';
            at += 1;
        }
        out[at] = HEX[usize::from(byte >> 4)];
        out[at + 1] = HEX[usize::from(byte & 0xf)];
        at += 2;
    }
    out
}

/// A record named `<kind>-<sensor>`.
#[derive(Clone, Copy)]
struct Named {
    id: u64,
    suffix: [u8; SUFFIX_MAX],
    len: usize,
}

impl Named {
    fn new(id: u64, suffix: &[u8]) -> Read<Named> {
        if suffix.is_empty() || suffix.len() > SUFFIX_MAX {
            return Err(Refusal::Malformed);
        }
        let mut out = Named {
            id,
            suffix: [0; SUFFIX_MAX],
            len: suffix.len(),
        };
        out.suffix[..suffix.len()].copy_from_slice(suffix);
        Ok(out)
    }

    fn sensor(&self) -> &[u8] {
        &self.suffix[..self.len]
    }
}

/// The factory-data directory's entries that matter here, counted.
struct Listing {
    calibrations: usize,
    calibration: Option<Named>,
    roots: usize,
    root: Option<Named>,
    seals: usize,
    seal: Option<u64>,
    current: Option<u64>,
    bundles: usize,
    bundle: [(u64, [u8; 32]); BUNDLES_MAX],
}

impl Listing {
    fn new() -> Listing {
        Listing {
            calibrations: 0,
            calibration: None,
            roots: 0,
            root: None,
            seals: 0,
            seal: None,
            current: None,
            bundles: 0,
            bundle: [(0, [0; 32]); BUNDLES_MAX],
        }
    }

    fn add(&mut self, name: &[u8], entry: Entry) -> Read<()> {
        let record =
            |kinds: &[&[u8]]| name.len() > 5 && name[4] == b'-' && kinds.contains(&&name[..4]);
        let ours = record(&CALIBRATION_KINDS)
            || record(&ROOT_KEY)
            || name.starts_with(SEAL_PREFIX)
            || name.starts_with(TRUST_PREFIX);
        if !ours {
            return Ok(());
        }
        if entry.kind != DT_REG {
            return Err(Refusal::Malformed);
        }
        if record(&CALIBRATION_KINDS) {
            self.calibrations += 1;
            self.calibration = Some(Named::new(entry.id, &name[5..])?);
        } else if record(&ROOT_KEY) {
            self.roots += 1;
            self.root = Some(Named::new(entry.id, &name[5..])?);
        } else if name.starts_with(SEAL_PREFIX) {
            self.seals += 1;
            self.seal = Some(entry.id);
        } else if name == TRUST_CURRENT {
            self.current = Some(entry.id);
        } else {
            let digest = hex32(&name[TRUST_PREFIX.len()..]).ok_or(Refusal::Malformed)?;
            if self.bundles == BUNDLES_MAX {
                return Err(Refusal::Ambiguous);
            }
            self.bundle[self.bundles] = (entry.id, digest);
            self.bundles += 1;
        }
        Ok(())
    }
}

fn hex32(text: &[u8]) -> Option<[u8; 32]> {
    if text.len() != 64 {
        return None;
    }
    let nibble = |c: u8| (c as char).to_digit(16).map(|v| v as u8);
    let mut out = [0u8; 32];
    for (i, pair) in text.chunks_exact(2).enumerate() {
        out[i] = nibble(pair[0])? << 4 | nibble(pair[1])?;
    }
    Some(out)
}

/// The seal and the trust bundle a rebuilt record carries.
struct Sealing<B> {
    seal: B,
    trust: B,
}

struct Files<'a, 'm, M: Media> {
    media: &'m M,
    container: &'a Container<'m, M>,
    volume: &'a Volume<M::Buf>,
    sha256: &'a dyn Fn(&[u8]) -> Read<[u8; 32]>,
}

impl<M: Media> Files<'_, '_, M> {
    fn read(&self, id: u64) -> Read<M::Buf> {
        self.container.read_file(self.volume, id, RECORD_MAX)
    }

    /// The calibration, and whether it had to be rebuilt.
    fn calibration(
        &self,
        listing: &Listing,
        sealing: &mut Option<Read<Sealing<M::Buf>>>,
    ) -> Read<(M::Buf, bool)> {
        let found = match listing.calibrations {
            0 => return Err(Refusal::Absent),
            1 => listing.calibration.ok_or(Refusal::Absent)?,
            _ => return Err(Refusal::Ambiguous),
        };
        if listing
            .root
            .is_some_and(|root| root.sensor() != found.sensor())
        {
            return Err(Refusal::Mismatched);
        }
        let bytes = self.read(found.id)?;
        let outer = whole(&bytes)?;
        let (first, count) = children::<1>(&bytes, &outer)?;
        if count == 0 {
            return Err(Refusal::Malformed);
        }
        let first = text(&bytes, &first[0]);
        if first == Some(&b"comb"[..]) {
            combined(&bytes, &CALIBRATION_KINDS)?;
            return Ok((bytes, false));
        }
        if first != Some(&b"IMG4"[..]) {
            return Err(Refusal::Malformed);
        }
        let payload = image(&bytes, &outer, &STANDALONE_CALIBRATION)?;
        let records = self.sealing(listing, sealing)?;
        let rebuilt = reseal(
            self.media,
            &bytes[payload.start..payload.end],
            records,
            &STANDALONE_CALIBRATION,
        )?;
        Ok((rebuilt, true))
    }

    /// The signed root key, rebuilt from its standalone image.
    fn root_key(
        &self,
        listing: &Listing,
        sealing: &mut Option<Read<Sealing<M::Buf>>>,
    ) -> Read<M::Buf> {
        let found = listing.root.ok_or(Refusal::Absent)?;
        let bytes = self.read(found.id)?;
        let outer = whole(&bytes)?;
        let payload = image(&bytes, &outer, &ROOT_KEY)?;
        let records = self.sealing(listing, sealing)?;
        reseal(
            self.media,
            &bytes[payload.start..payload.end],
            records,
            &ROOT_KEY,
        )
    }

    /// Reads the sealing records at most once.
    fn sealing<'s>(
        &self,
        listing: &Listing,
        cache: &'s mut Option<Read<Sealing<M::Buf>>>,
    ) -> Read<&'s Sealing<M::Buf>> {
        if cache.is_none() {
            *cache = Some(self.read_sealing(listing));
        }
        match cache {
            Some(Ok(records)) => Ok(records),
            Some(Err(refusal)) => Err(*refusal),
            None => Err(Refusal::Absent),
        }
    }

    /// The one seal, and the trust bundle in use: the named bundle whose
    /// digest and bytes match `trustobject-current`, or without that, the
    /// only named bundle, which must match its name.
    fn read_sealing(&self, listing: &Listing) -> Read<Sealing<M::Buf>> {
        let seal = match listing.seals {
            0 => return Err(Refusal::Absent),
            1 => self.read(listing.seal.ok_or(Refusal::Absent)?)?,
            _ => return Err(Refusal::Ambiguous),
        };
        let outer = whole(&seal)?;
        manifest(&seal, &outer)?;
        let bundles = &listing.bundle[..listing.bundles];
        let trust = if let Some(current) = listing.current {
            let trust = self.read(current)?;
            let digest = (self.sha256)(&trust)?;
            let mut named = bundles.iter().filter(|bundle| bundle.1 == digest);
            let (id, _) = named.next().ok_or(Refusal::Malformed)?;
            if named.next().is_some() {
                return Err(Refusal::Ambiguous);
            }
            if *self.read(*id)? != *trust {
                return Err(Refusal::Malformed);
            }
            trust
        } else {
            let (id, digest) = match bundles {
                [] => return Err(Refusal::Absent),
                [only] => *only,
                _ => return Err(Refusal::Ambiguous),
            };
            let trust = self.read(id)?;
            if (self.sha256)(&trust)? != digest {
                return Err(Refusal::Malformed);
            }
            trust
        };
        whole(&trust)?;
        Ok(Sealing { seal, trust })
    }
}

/// One DER element: its tag's first byte and its byte range.
#[derive(Clone, Copy, Default)]
struct Tlv {
    tag: u8,
    start: usize,
    content: usize,
    end: usize,
}

/// The DER element at `at`, which must end by `limit`. Lengths are definite
/// and minimal; high tag numbers are read so that they can be skipped.
fn element(data: &[u8], at: usize, limit: usize) -> Read<Tlv> {
    let byte = |i: usize| {
        if i < limit {
            data.get(i).copied().ok_or(Refusal::Malformed)
        } else {
            Err(Refusal::Malformed)
        }
    };
    let tag = byte(at)?;
    let mut i = at + 1;
    if tag & 0x1f == 0x1f {
        let mut number = 0u32;
        for n in 0.. {
            let b = byte(i)?;
            i += 1;
            if n == 4 || (n == 0 && b == 0x80) {
                return Err(Refusal::Malformed);
            }
            number = number << 7 | u32::from(b & 0x7f);
            if b & 0x80 == 0 {
                break;
            }
        }
        if number < 0x1f {
            return Err(Refusal::Malformed);
        }
    }
    let first = byte(i)?;
    i += 1;
    let len = if first < 0x80 {
        usize::from(first)
    } else {
        let count = usize::from(first & 0x7f);
        if count == 0 || count > 4 {
            return Err(Refusal::Malformed);
        }
        let mut len = 0usize;
        for k in 0..count {
            let b = byte(i)?;
            i += 1;
            if k == 0 && b == 0 {
                return Err(Refusal::Malformed);
            }
            len = len << 8 | usize::from(b);
        }
        if len < 0x80 {
            return Err(Refusal::Malformed);
        }
        len
    };
    let end = i.checked_add(len).ok_or(Refusal::Malformed)?;
    if end > limit {
        return Err(Refusal::Malformed);
    }
    Ok(Tlv {
        tag,
        start: at,
        content: i,
        end,
    })
}

/// The whole of `data` as one SEQUENCE.
fn whole(data: &[u8]) -> Read<Tlv> {
    let outer = element(data, 0, data.len())?;
    if outer.tag != SEQ || outer.end != data.len() {
        return Err(Refusal::Malformed);
    }
    Ok(outer)
}

/// The first `N` children of `parent` and how many it has; all of them must
/// be complete elements that tile its content.
fn children<const N: usize>(data: &[u8], parent: &Tlv) -> Read<([Tlv; N], usize)> {
    let mut out = [Tlv::default(); N];
    let mut count = 0;
    let mut at = parent.content;
    while at < parent.end {
        if count == CHILDREN_MAX {
            return Err(Refusal::Malformed);
        }
        let child = element(data, at, parent.end)?;
        if count < N {
            out[count] = child;
        }
        count += 1;
        at = child.end;
    }
    Ok((out, count))
}

fn text<'d>(data: &'d [u8], t: &Tlv) -> Option<&'d [u8]> {
    (t.tag == IA5).then(|| &data[t.content..t.end])
}

/// A manifest: one SEQUENCE that starts with `IM4M`.
fn manifest(data: &[u8], t: &Tlv) -> Read<()> {
    let (first, count) = children::<1>(data, t)?;
    if t.tag != SEQ || count == 0 || text(data, &first[0]) != Some(&b"IM4M"[..]) {
        return Err(Refusal::Malformed);
    }
    Ok(())
}

/// The IM4P element of the image `img`: `IMG4 { IM4P { type, version,
/// payload, ... }, [0] IM4M }` with its type one of `kinds`.
fn image(data: &[u8], img: &Tlv, kinds: &[&[u8]]) -> Read<Tlv> {
    let (parts, count) = children::<3>(data, img)?;
    if img.tag != SEQ
        || count != 3
        || text(data, &parts[0]) != Some(&b"IMG4"[..])
        || parts[1].tag != SEQ
        || parts[2].tag != CTX0
    {
        return Err(Refusal::Malformed);
    }
    let (fields, count) = children::<4>(data, &parts[1])?;
    if count < 4
        || text(data, &fields[0]) != Some(&b"IM4P"[..])
        || !text(data, &fields[1]).is_some_and(|kind| kinds.contains(&kind))
        || fields[2].tag != IA5
        || fields[3].tag != OCTET
        || fields[3].content == fields[3].end
    {
        return Err(Refusal::Malformed);
    }
    let (wrapper, count) = children::<1>(data, &parts[2])?;
    if count != 1 {
        return Err(Refusal::Malformed);
    }
    manifest(data, &wrapper[0])?;
    Ok(parts[1])
}

/// A complete combined record whose image's type is one of `kinds`.
fn combined(data: &[u8], kinds: &[&[u8]]) -> Read<()> {
    let outer = whole(data)?;
    let (fields, count) = children::<3>(data, &outer)?;
    if count != 3
        || text(data, &fields[0]) != Some(&b"comb"[..])
        || fields[1].tag != SEQ
        || fields[2].tag != SEQ
    {
        return Err(Refusal::Malformed);
    }
    let (record, count) = children::<2>(data, &fields[1])?;
    if count != 2 || text(data, &record[0]) != Some(&b"fdrd"[..]) || record[1].tag != OCTET {
        return Err(Refusal::Malformed);
    }
    let (security, count) = children::<2>(data, &fields[2])?;
    if count != 2
        || text(data, &security[0]) != Some(&b"secb"[..])
        || security[1].tag != OCTET
        || security[1].content == security[1].end
    {
        return Err(Refusal::Malformed);
    }
    let img = element(data, record[1].content, record[1].end)?;
    if img.end != record[1].end {
        return Err(Refusal::Malformed);
    }
    image(data, &img, kinds)?;
    Ok(())
}

/// The size of an element whose content is `len` bytes.
fn sized(len: usize) -> Read<usize> {
    let header = if len < 0x80 {
        2
    } else {
        2 + (usize::BITS - len.leading_zeros()).div_ceil(8) as usize
    };
    if len > u32::MAX as usize {
        return Err(Refusal::TooLarge);
    }
    len.checked_add(header).ok_or(Refusal::TooLarge)
}

struct Writer<'a> {
    out: &'a mut [u8],
    at: usize,
}

impl Writer<'_> {
    fn put(&mut self, bytes: &[u8]) -> Read<()> {
        let end = self.at + bytes.len();
        self.out
            .get_mut(self.at..end)
            .ok_or(Refusal::Malformed)?
            .copy_from_slice(bytes);
        self.at = end;
        Ok(())
    }

    fn header(&mut self, tag: u8, len: usize) -> Read<()> {
        if len < 0x80 {
            return self.put(&[tag, len as u8]);
        }
        let bytes = (len as u32).to_be_bytes();
        let skip = bytes.iter().take_while(|&&b| b == 0).count();
        self.put(&[tag, 0x80 | (4 - skip) as u8])?;
        self.put(&bytes[skip..])
    }

    fn text(&mut self, value: &[u8]) -> Read<()> {
        self.header(IA5, value.len())?;
        self.put(value)
    }
}

/// `comb { fdrd: IMG4 { im4p, [0] seal }, secb: trust }`, checked as a
/// combined record of type `kind` before it is returned.
fn reseal<M: Media>(
    media: &M,
    im4p: &[u8],
    records: &Sealing<M::Buf>,
    kind: &[&[u8]],
) -> Read<M::Buf> {
    let (seal, trust) = (&*records.seal, &*records.trust);
    let image_content = sized(4)? + im4p.len() + sized(seal.len())?;
    let image = sized(image_content)?;
    let fdrd = sized(4)? + sized(image)?;
    let secb = sized(4)? + sized(trust.len())?;
    let comb = sized(4)? + sized(fdrd)? + sized(secb)?;
    let total = sized(comb)?;
    let mut out = media.buffer(total)?;
    let mut w = Writer {
        out: &mut out,
        at: 0,
    };
    w.header(SEQ, comb)?;
    w.text(b"comb")?;
    w.header(SEQ, fdrd)?;
    w.text(b"fdrd")?;
    w.header(OCTET, image)?;
    w.header(SEQ, image_content)?;
    w.text(b"IMG4")?;
    w.put(im4p)?;
    w.header(CTX0, seal.len())?;
    w.put(seal)?;
    w.header(SEQ, secb)?;
    w.text(b"secb")?;
    w.header(OCTET, trust.len())?;
    w.put(trust)?;
    if w.at != total {
        return Err(Refusal::Malformed);
    }
    combined(&out, kind)?;
    Ok(out)
}
