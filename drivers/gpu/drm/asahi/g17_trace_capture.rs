// SPDX-License-Identifier: GPL-2.0-only OR MIT


pub(crate) const MAGIC: [u8; 8] = *b"G17TRC01";
pub(crate) const VERSION: u16 = 1;
pub(crate) const HEADER_SIZE: usize = 64;
pub(crate) const RECORD_SIZE: usize = 96;
pub(crate) const DEFAULT_CAPACITY: usize = 64 * 1024 * 1024;
pub(crate) const UNKNOWN_CONTEXT: u32 = u32::MAX;
pub(crate) const UNKNOWN_QID: u32 = u32::MAX;
pub(crate) const SHARED_ROLE: u8 = u8::MAX;
pub(crate) const ROOT_UNKNOWN: u8 = u8::MAX;
pub(crate) const ROOT_CONTEXT_LOW: u8 = 0;
pub(crate) const ROOT_CONTEXT_HIGH: u8 = 1;
pub(crate) const ROOT_GLOBAL: u8 = 2;
pub(crate) const FLAG_OVERFLOW: u64 = 1;
pub(crate) const FLAG_SEALED: u64 = 2;

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Phase {
    BootConstructed = 1,
    BeforeDoorbell = 2,
    AfterReply = 3,
    AfterPairRetirement = 4,
    RecoveryBeforeAck = 5,
    PreparedBeforeFList = 6,
}

/// Stable wire IDs, not Rust enum-layout serialization.
#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind {
    InitdataRoot = 1,
    MainConfig = 2,
    SharedHwdataCluster = 3,
    PrivateState = 4,
    SecondaryStatusA = 5,
    RegionA = 6,
    RegionC = 7,
    Qos = 8,
    SksmQid = 9,
    CompletionRing = 10,
    ParameterMetrics = 11,
    PbDescriptorTable = 12,
    UmaDescriptorTable = 13,
    ControlShared = 14,
    PartialControl = 15,
    ContextPeer = 16,
    PartialPrimaryIndex = 17,
    RenderDescriptors = 32,
    RenderQueueGraph = 33,
    RenderSupport = 34,
    RenderState = 35,
    RenderTimestamps = 36,
    FragmentStatus = 37,
    ParameterManagement = 38,
    SceneScratch = 39,
    Discard = 40,
    SksmEntries = 48,
    ClSharedSupport = 49,
    ClChannelControl = 50,
    ClSupportState = 51,
    ClSchedulerState = 52,
    ClOperandTable = 53,
    ClientObjects = 0x100,
    TvbPayload = 0x101,
    OperandPayload = 0x102,
    MmioEvents = 0x103,
    MailboxEvents = 0x104,
    PageTables = 0x105,
    /// A whole observation could not be taken; never substitute empty data.
    CaptureBoundary = 0x106,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BufferOrigin {
    Unknown = 0,
    ExistingOwnedWc = 1,
    /// Host-owned published source bytes; not independent GPU readback.
    ExistingOwnedWb = 2,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Omission {
    NotImplemented = 1,
    Unavailable = 2,
    ReadError = 3,
    Budget = 4,
    InvalidRange = 5,
    LengthMismatch = 6,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Meta {
    pub(crate) kind: Kind,
    pub(crate) phase: Phase,
    pub(crate) role: u8,
    pub(crate) root: u8,
    pub(crate) context: u32,
    pub(crate) origin: BufferOrigin,
    pub(crate) instance: u32,
    pub(crate) dva: u64,
    pub(crate) expected_len: u64,
    pub(crate) job_stamp: u64,
    pub(crate) qid: u32,
    pub(crate) flags: u32,
    pub(crate) phase_sequence: u64,
}

impl Meta {
    pub(crate) const fn new(kind: Kind, phase: Phase, phase_sequence: u64) -> Self {
        Self {
            kind,
            phase,
            role: SHARED_ROLE,
            root: ROOT_UNKNOWN,
            context: UNKNOWN_CONTEXT,
            origin: BufferOrigin::Unknown,
            instance: 0,
            dva: 0,
            expected_len: 0,
            job_stamp: 0,
            qid: UNKNOWN_QID,
            flags: 0,
            phase_sequence,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Error {
    TooSmall,
    InvalidHeader,
    Sealed,
}

pub(crate) struct TraceArchive<'a> {
    arena: &'a mut [u8],
    used: usize,
    records: u64,
    attempted: u64,
    omissions: u64,
    dropped: u64,
    flags: u64,
}

fn put16(raw: &mut [u8], at: usize, value: u16) {
    raw[at..at + 2].copy_from_slice(&value.to_le_bytes());
}
fn put32(raw: &mut [u8], at: usize, value: u32) {
    raw[at..at + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(raw: &mut [u8], at: usize, value: u64) {
    raw[at..at + 8].copy_from_slice(&value.to_le_bytes());
}
fn get64(raw: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(raw[at..at + 8].try_into().unwrap())
}

/// Coredump reads use only the sealed used prefix. The live arena is never
/// made readable, and spare capacity must not leak as fabricated zero data.
pub(crate) fn read_sealed_prefix(raw: &[u8], output: &mut [u8], offset: usize) -> Result<usize, Error> {
    if raw.len() < HEADER_SIZE { return Err(Error::TooSmall); }
    if raw[..8] != MAGIC || raw[8..16] != [1, 0, 64, 0, 96, 0, 0, 0] {
        return Err(Error::InvalidHeader);
    }
    if get64(raw, 56) & FLAG_SEALED == 0 { return Err(Error::InvalidHeader); }
    let used = usize::try_from(get64(raw, 16)).map_err(|_| Error::InvalidHeader)?;
    if used < HEADER_SIZE || used > raw.len() || used % 8 != 0 {
        return Err(Error::InvalidHeader);
    }
    let count = output.len().min(used.saturating_sub(offset));
    if count != 0 { output[..count].copy_from_slice(&raw[offset..offset + count]); }
    Ok(count)
}

impl<'a> TraceArchive<'a> {
    pub(crate) fn new(arena: &'a mut [u8]) -> Result<Self, Error> {
        if arena.len() < HEADER_SIZE {
            return Err(Error::TooSmall);
        }
        let mut this = Self {
            arena,
            used: HEADER_SIZE,
            records: 0,
            attempted: 0,
            omissions: 0,
            dropped: 0,
            flags: 0,
        };
        this.header();
        Ok(this)
    }

    /// Reattach between observations without resetting retained before-bytes.
    pub(crate) fn resume(arena: &'a mut [u8]) -> Result<Self, Error> {
        if arena.len() < HEADER_SIZE {
            return Err(Error::TooSmall);
        }
        if arena[..8] != MAGIC || arena[8..16] != [1, 0, 64, 0, 96, 0, 0, 0] {
            return Err(Error::InvalidHeader);
        }
        let used = usize::try_from(get64(arena, 16)).map_err(|_| Error::InvalidHeader)?;
        let flags = get64(arena, 56);
        if used < HEADER_SIZE || used > arena.len() || used % 8 != 0 {
            return Err(Error::InvalidHeader);
        }
        if flags & FLAG_SEALED != 0 {
            return Err(Error::Sealed);
        }
        let this = Self {
            used,
            records: get64(arena, 24),
            attempted: get64(arena, 32),
            omissions: get64(arena, 40),
            dropped: get64(arena, 48),
            flags,
            arena,
        };
        if this.records > this.attempted
            || this.omissions > this.attempted
            || this.dropped > this.omissions
        {
            return Err(Error::InvalidHeader);
        }
        Ok(this)
    }

    fn header(&mut self) {
        self.arena[..HEADER_SIZE].fill(0);
        self.arena[..8].copy_from_slice(&MAGIC);
        put16(self.arena, 8, VERSION);
        put16(self.arena, 10, HEADER_SIZE as u16);
        put16(self.arena, 12, RECORD_SIZE as u16);
        put64(self.arena, 16, self.used as u64);
        put64(self.arena, 24, self.records);
        put64(self.arena, 32, self.attempted);
        put64(self.arena, 40, self.omissions);
        put64(self.arena, 48, self.dropped);
        put64(self.arena, 56, self.flags);
    }

    fn finish(
        &mut self,
        meta: Meta,
        reason: Option<Omission>,
        captured: usize,
        started: u64,
        finished: u64,
    ) -> bool {
        if reason.is_some() {
            self.omissions = self.omissions.saturating_add(1);
        }
        if self.arena.len() - self.used < RECORD_SIZE {
            self.dropped = self.dropped.saturating_add(1);
            self.flags |= FLAG_OVERFLOW;
            self.header();
            return false;
        }
        let size = RECORD_SIZE + ((captured + 7) & !7);
        let out = &mut self.arena[self.used..self.used + size];
        out[..RECORD_SIZE].fill(0);
        out[RECORD_SIZE + captured..].fill(0);
        put32(out, 0, size as u32);
        put16(out, 4, meta.kind as u16);
        put16(out, 6, u16::from(reason.is_some()));
        put16(out, 8, meta.phase as u16);
        out[10] = meta.role;
        out[11] = meta.root;
        put32(out, 12, meta.context);
        put16(out, 16, meta.origin as u16);
        put16(out, 18, reason.map_or(0, |v| v as u16));
        put32(out, 20, meta.instance);
        put64(out, 24, meta.dva);
        put64(out, 32, meta.expected_len);
        put64(out, 40, captured as u64);
        put64(out, 48, started);
        put64(out, 56, finished);
        put64(out, 64, self.attempted);
        put64(out, 72, meta.job_stamp);
        put32(out, 80, meta.qid);
        put32(out, 84, meta.flags);
        put64(out, 88, meta.phase_sequence);
        self.used += size;
        self.records = self.records.saturating_add(1);
        self.header();
        reason.is_none()
    }

    /// The copier receives exactly expected_len bytes of preallocated space.
    /// Clock samples bracket the actual copy, not pointer lookup or formatting.
    /// On error, any partially copied bytes are discarded and never exported.
    pub(crate) fn capture(
        &mut self,
        meta: Meta,
        clock: &mut impl FnMut() -> u64,
        copy: impl FnOnce(&mut [u8]) -> Result<(), Omission>,
    ) -> Result<bool, Error> {
        if self.flags & FLAG_SEALED != 0 {
            return Err(Error::Sealed);
        }
        self.attempted = self.attempted.saturating_add(1);
        let size = usize::try_from(meta.expected_len)
            .ok()
            .and_then(|n| n.checked_add(7).map(|padded| (n, padded & !7)))
            .and_then(|(n, padded)| padded.checked_add(RECORD_SIZE).map(|total| (n, total)));
        let Some((len, total)) = size else {
            return Ok(self.finish(meta, Some(Omission::InvalidRange), 0, 0, 0));
        };
        if total > u32::MAX as usize || total > self.arena.len() - self.used {
            self.flags |= FLAG_OVERFLOW;
            return Ok(self.finish(meta, Some(Omission::Budget), 0, 0, 0));
        }
        let start = clock();
        let result = copy(&mut self.arena[self.used + RECORD_SIZE..self.used + RECORD_SIZE + len]);
        let end = clock();
        match result {
            Ok(()) => Ok(self.finish(meta, None, len, start, end)),
            Err(reason) => Ok(self.finish(meta, Some(reason), 0, start, end)),
        }
    }

    pub(crate) fn append_bytes(
        &mut self,
        meta: Meta,
        bytes: &[u8],
        clock: &mut impl FnMut() -> u64,
    ) -> Result<bool, Error> {
        if meta.expected_len != bytes.len() as u64 {
            return self.omit(meta, Omission::LengthMismatch);
        }
        self.capture(meta, clock, |out| {
            out.copy_from_slice(bytes);
            Ok(())
        })
    }

    pub(crate) fn omit(&mut self, meta: Meta, reason: Omission) -> Result<bool, Error> {
        if self.flags & FLAG_SEALED != 0 {
            return Err(Error::Sealed);
        }
        self.attempted = self.attempted.saturating_add(1);
        if reason == Omission::Budget {
            self.flags |= FLAG_OVERFLOW;
        }
        Ok(self.finish(meta, Some(reason), 0, 0, 0))
    }

    pub(crate) fn seal(&mut self) {
        self.flags |= FLAG_SEALED;
        self.header();
    }
    pub(crate) fn bytes(&self) -> &[u8] {
        &self.arena[..self.used]
    }
}

