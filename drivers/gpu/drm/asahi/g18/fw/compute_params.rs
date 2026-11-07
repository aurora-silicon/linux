// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! T8140/G17P compute descriptor construction.
//!
//! The descriptor is the one-page selector-3 object executed by the CL path.
//! The layout combines the m1n1 builder with inspected native producers. The
//! caller supplies queue-owned object addresses and command-owned UAPI values;
//! this module only validates and encodes them.

pub(crate) const COMPUTE_DESCRIPTOR_SIZE: usize = 0x4000;
/// Registers a kick with no SCS scratch request emits.
pub(crate) const COMPUTE_REGISTER_COUNT: usize = 40;
/// One more slot for the SCS enable register `generateCLRegisterListG16Common`
/// PREPENDS when the descriptor carries a non-zero scratch request
/// (mac17p `0xfffffe0008a5fa90`..`0xfffffe0008a5fac8`).
pub(crate) const COMPUTE_REGISTER_MAX: usize = COMPUTE_REGISTER_COUNT + 1;
pub(crate) const COMPUTE_REGISTER_CAPACITY: usize = 128;
pub(crate) const COMPUTE_REGISTER_SIZE: usize = 0x0c;
pub(crate) const COMPUTE_QUEUE_GRAPH_SIZE: usize = 0x8000;
pub(crate) const COMPUTE_QUEUE_CONTEXT_SIZE: usize = 0x4000;
pub(crate) const COMPUTE_QUEUE_CONTEXT_EXTENT: usize = 8 * COMPUTE_QUEUE_CONTEXT_SIZE;
pub(crate) const COMPUTE_OPTIONAL_SIZE: usize = 0xc0;
pub(crate) const COMPUTE_EVENT_SIZE: usize = 0x400;
pub(crate) const COMPUTE_CHANNEL_WRITE_INDEX: u32 = 3;
pub(crate) const COMPUTE_QUEUE_POINTERS: usize = 0x100;
const COMPUTE_QUEUE_CONTEXT_ITEM: usize = 0x200;
/// Same offset, exported so a repeat can refresh the item the first bind wrote.
pub(crate) const COMPUTE_QUEUE_CONTEXT_ITEM_OFFSET: usize = COMPUTE_QUEUE_CONTEXT_ITEM;
pub(crate) const COMPUTE_ITEM_RING: usize = 0x4000;
pub(crate) const COMPUTE_ITEM_RING_SIZE: usize = 0x500 * 8;
pub(crate) const COMPUTE_OPTIONAL: usize = 0x6800;
pub(crate) const COMPUTE_EVENT: usize = 0x6c00;
/// Storage for the tag-14 `KsmKickQueueAddKicks` channel command.
///
/// Native allocates this record from a firmware-owned pool
/// (`AGXFirmware::requestChannelCommandKSMKickQueueAddKicks`,
/// mac17p `0xfffffe0008a2d664`). Linux keeps one retained slot per queue graph,
/// placed after the tag-16 window exactly as tags 15 and 16 are placed after the
/// command-pointer ring. A000 `0xfffffc0000011710` cache-invalidates `0x18`
/// bytes of the record before reading it, so the reserved window is zeroed as a
/// whole even though the host writes only `0x15` bytes.
/// Records one steady-state submission publishes into the item ring:
/// the tag-3 command pointer, tag 15, tag 14, tag 16.
pub(crate) const COMPUTE_REPEAT_RING_RECORD_COUNT: usize = 4;
pub(crate) const COMPUTE_ADD_KICKS: usize = 0x7000;
pub(crate) const COMPUTE_ADD_KICKS_SIZE: usize = 0x40;
const _: () = assert!(COMPUTE_ITEM_RING + COMPUTE_ITEM_RING_SIZE <= COMPUTE_OPTIONAL);
const _: () = assert!(COMPUTE_OPTIONAL + COMPUTE_OPTIONAL_SIZE <= COMPUTE_EVENT);
const _: () = assert!(COMPUTE_EVENT + COMPUTE_EVENT_SIZE <= COMPUTE_ADD_KICKS);
const _: () = assert!(COMPUTE_ADD_KICKS + COMPUTE_ADD_KICKS_SIZE <= COMPUTE_QUEUE_GRAPH_SIZE);
const _: () = assert!(COMPUTE_REGISTER_MAX <= COMPUTE_REGISTER_CAPACITY);

const COMPUTE_SELECTOR: u32 = 3;
const COMPUTE_REGISTER_START: usize = 0x40;
const COMPUTE_SECONDARY_REGISTER_START: usize = 0x760;
const COMPUTE_PRIMARY_COUNT: usize = 0x748;
const COMPUTE_ENCODER_PARAMS: usize = 0xf14;
const COMPUTE_SAMPLER_ARRAY: usize = COMPUTE_ENCODER_PARAMS + 0x18;
const COMPUTE_SAMPLER_COUNT: usize = COMPUTE_ENCODER_PARAMS + 0x20;
const COMPUTE_SAMPLER_MAX: usize = COMPUTE_ENCODER_PARAMS + 0x24;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ComputeBuildError {
    BufferTooSmall,
    AddressOverflow,
    CdmTerminatorBeforeStream,
    SubmissionIndexZero,
    QueueSubmissionZero,
    InvalidSksmEntryAliases,
    SamplerCountOutOfRange,
    SamplerPair,
    SamplerAlignment,
    ScsRequestOutOfRange,
    RegisterProgramShape,
    HardwareTimestampOutOfRange,
}

/// Low and high GPU aliases of one retained SKSM entry backing.
///
/// Native tag-15 ConfigUpdate reads these from the same KSM queue object at
/// `+0x80/+0x88`. Keeping them typed prevents the classic CL2 queue-context
/// allocation from being substituted for either address.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeSksmEntryAliases {
    low: u64,
    high: u64,
    extent: usize,
}

impl ComputeSksmEntryAliases {
    pub(crate) const fn new(
        low: u64,
        high: u64,
        extent: usize,
        alignment: u64,
    ) -> Result<Self, ComputeBuildError> {
        if low == 0
            || high == 0
            || low == high
            || extent == 0
            || alignment == 0
            || alignment & (alignment - 1) != 0
            || low & (alignment - 1) != 0
            || high & (alignment - 1) != 0
            || low.checked_add(extent as u64).is_none()
            || high.checked_add(extent as u64).is_none()
        {
            return Err(ComputeBuildError::InvalidSksmEntryAliases);
        }
        Ok(Self { low, high, extent })
    }

    pub(crate) const fn low(self) -> u64 {
        self.low
    }

    pub(crate) const fn high(self) -> u64 {
        self.high
    }

    pub(crate) const fn extent(self) -> usize {
        self.extent
    }

    /// Return corresponding addresses within both aliases of the one backing.
    pub(crate) const fn range_at(
        self,
        offset: usize,
        size: usize,
    ) -> Result<[u64; 2], ComputeBuildError> {
        let end = match offset.checked_add(size) {
            Some(end) => end,
            None => return Err(ComputeBuildError::InvalidSksmEntryAliases),
        };
        if end > self.extent {
            return Err(ComputeBuildError::InvalidSksmEntryAliases);
        }
        Ok([self.low + offset as u64, self.high + offset as u64])
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeRegister {
    pub(crate) number: u32,
    pub(crate) value: u64,
}

impl ComputeRegister {
    const fn new(number: u32, value: u64) -> Self {
        Self { number, value }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeRegisterParameters {
    pub(crate) preempt_base: u64,
    pub(crate) cdm_base: u64,
    pub(crate) usc_exec_base: u64,
    pub(crate) helper_binary: u64,
    pub(crate) helper_data: u64,
    pub(crate) helper_config: u64,
    pub(crate) dispatch_identity: u64,
    pub(crate) context_id: u32,
    pub(crate) work_ordinal: u32,
    pub(crate) robustness: u64,
    pub(crate) operand_state_base: u64,
    /// Native CL prepare stores the allocated FList ID at descriptor+0x798;
    /// generateRegisterList emits it at 0x14028. This is not an enable gate.
    pub(crate) usc_flist_id: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorObjects {
    pub(crate) scheduler_record: u64,
    pub(crate) descriptor_low_alias: u64,
    pub(crate) dispatch_a: u64,
    pub(crate) dispatch_b: u64,
    pub(crate) status_a: u64,
    pub(crate) status_b: u64,
    pub(crate) shared_control: u64,
    pub(crate) usage_record: u64,
}

/// Descriptor offset of the completion-stamp value firmware copies to the
/// support-page stamp word (`*(descriptor + 0x0F40)`) after the kick's
/// completion cache maintenance (b1 `AGFAcceleratorProcessTimeStampQueue`,
/// see `G17P_COMPUTE_STAMP_SUPPORT_OFFSET`).
pub(crate) const COMPUTE_COMPLETION_STAMP_VALUE: usize = 0x0f50;
/// Initial stamp for bootstrap records. User publication overwrites this with
/// the physical queue's monotonic kick value, independently of TSQ slot index.
pub(crate) const COMPUTE_COMPLETION_STAMP_CONSTANT: u32 = 0x100;

/// Native EventMachine increments a stable event index's value by 0x100.
/// B1 coalesces timestamp records at that index before cache maintenance:
/// the newest store must cover every earlier completed kick in that prefix.
pub(crate) const fn completion_stamp_value(kick_timestamp: u64) -> u32 {
    (kick_timestamp as u32).wrapping_shl(8)
}

/// Modular prefix comparison, valid while outstanding kicks span less than
/// half the 24-bit stamp sequence (the queue's admission bound is much lower).
/// Zero is a valid wrapped store. Counter/ring identity must also pass.
pub(crate) const fn completion_stamp_covers(stamp_word: u32, kick_timestamp: u64) -> bool {
    let delta = stamp_word.wrapping_sub(completion_stamp_value(kick_timestamp));
    delta & 0xff == 0 && delta < 0x8000_0000
}

/// Bind the native allocation epoch after building any user compute record.
/// Zero is a valid wrapped epoch; never replace it with a default value.
pub(crate) fn set_context_generation(raw: &mut [u8], generation: u8) -> Result<(), ComputeBuildError> {
    if raw.len() <= 0x0fd3 {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    put_u8(raw, 0x0fd3, generation);
    Ok(())
}

/// Bind the physical queue's exact publication prefix before exposing a record.
pub(crate) fn set_completion_stamp_value(raw: &mut [u8], kick_timestamp: u64) -> Result<(), ComputeBuildError> {
    if raw.len() < COMPUTE_COMPLETION_STAMP_VALUE + 4 {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    put_u32(raw, COMPUTE_COMPLETION_STAMP_VALUE, completion_stamp_value(kick_timestamp));
    Ok(())
}

/// CL's native completion scratch payload is zero; its low forty bits are
/// the same converted hardware timestamp used by the KSM entry. This field
/// is a u64 at +0xf60, not a u32 ordinal or a QID-packed dependency token.
pub(crate) fn set_hardware_timestamp(raw: &mut [u8], timestamp: u64) -> Result<(), ComputeBuildError> {
    if raw.len() < 0x0f68 { return Err(ComputeBuildError::BufferTooSmall); }
    if timestamp >> 40 != 0 { return Err(ComputeBuildError::HardwareTimestampOutOfRange); }
    put_u64(raw, 0x0f60, timestamp);
    Ok(())
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorMetadata {
    pub(crate) submit_sequence: u64,
    pub(crate) context_id: u32,
    /// Event stamp index (+0xf54), distinct from the TSQ pointer slot.
    pub(crate) grid_index: u32,
    pub(crate) work_ordinal: u32,
    pub(crate) queue_submission: u32,
    /// Historical name: +0xf58 selects a pointer slot within the TSQ node.
    /// It is NOT an unbounded submission ordinal. User queues use slot zero.
    pub(crate) queue_ordinal: u32,
    /// Bootstrap seed for the low word at +0xf60; user publication binds the
    /// complete forty-bit hardware timestamp with set_hardware_timestamp.
    pub(crate) submission_index: u32,
    pub(crate) support_control: u32,
    pub(crate) support_flags: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorCommand {
    pub(crate) cdm_terminator: u64,
    pub(crate) sampler_array: u64,
    pub(crate) sampler_count: u32,
    pub(crate) user_timestamp_start: u64,
    pub(crate) user_timestamp_end: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeQueueGraphAddresses {
    pub(crate) queue_record: u64,
    pub(crate) pointers: u64,
    pub(crate) job_list: u64,
    pub(crate) item_ring: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeChannelGroupAddresses {
    pub(crate) queue: u64,
    pub(crate) optional: u64,
    pub(crate) event: u64,
    pub(crate) write_index: u32,
}

fn add(address: u64, offset: u64) -> Result<u64, ComputeBuildError> {
    address
        .checked_add(offset)
        .ok_or(ComputeBuildError::AddressOverflow)
}

/// The three register values that carry a compute kick's ray-traversal (SCS)
/// scratch request.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ScsRequestRegisters {
    /// 0x101d1: pages in the low half, MiB in the high half.
    pub(crate) size_word: u64,
    /// 0xd479: the request's "extras" count.
    pub(crate) extras: u64,
    /// 0x107a1: window and enable, or the disabled constant.
    pub(crate) window_word: u64,
    /// `AGXCLCommandDescriptor+0x5c8`, which `processComputeSetup` sets to
    /// `pages != 0` (mac17p `0xfffffe0008a07ab8`..`0xfffffe0008a07ac0`).
    /// `generateCLRegisterListG16Common` branches on it twice: once to prepend
    /// register `0x17d9 = 1` (`0xfffffe0008a5fa90`) and once to choose the
    /// 0x107a0 value (`0xfffffe0008a60280`).  The three request registers alone
    /// do NOT tell the two arms apart, so carry the flag rather than inferring
    /// it from a value.
    pub(crate) enabled: bool,
}

/// The MiB half of the SCS request, derived from the page count exactly as
/// AGXMetalG17P derives it in `ComputeContext::endComputePass`
/// (0x216ac1f5c..0x216ac1f6c: `pages << 2`, `+ 0x3ff`, `>> 10`).
fn scs_request_mib(pages: u32) -> u32 {
    ((pages as u64 + 0xff) >> 8) as u32
}

/// Derive the per-core SCS request from Apple's packed ScsLayoutGen1 word.
///
/// AGXMetalG17P's `getSectionEnd(ScsSection::End)` has four fixed sections
/// totaling 0x58000 bytes, followed by per-VRID ray-stack, driver-payload and
/// user-payload storage. The virtual-ray count is the packed clique byte times
/// 32. `endComputePass` rounds that total to 4 KiB and adds one guard page.
///
/// A layout with neither user nor driver payload is the native "no traversal
/// scratch" gate even if other fields are populated. Arithmetic is checked
/// here rather than reproducing Apple's 32-bit wrapping: a wrapped request
/// would under-allocate kernel-owned backing.
pub(crate) fn scs_pages_for_layout(layout: u64) -> Result<u32, ComputeBuildError> {
    let depth = layout & 0xffff;
    let cliques = (layout >> 24) & 0xff;
    let user = (layout >> 32) & 0xffff;
    let driver = (layout >> 48) & 0xffff;

    if user == 0 && driver == 0 {
        return Ok(0);
    }

    let vrids = cliques
        .checked_mul(32)
        .ok_or(ComputeBuildError::ScsRequestOutOfRange)?;
    let per_vrid = depth
        .checked_mul(8)
        .and_then(|value| value.checked_add(driver))
        .and_then(|value| value.checked_add(user))
        .ok_or(ComputeBuildError::ScsRequestOutOfRange)?;
    let bytes = vrids
        .checked_mul(per_vrid)
        .and_then(|value| value.checked_add(0x58000))
        .and_then(|value| value.checked_add(0x1fff))
        .ok_or(ComputeBuildError::ScsRequestOutOfRange)?
        & !0xfff;
    let pages = bytes >> 12;

    if pages > 0xffff {
        return Err(ComputeBuildError::ScsRequestOutOfRange);
    }

    Ok(pages as u32)
}

/// Encode an SCS scratch request the way the kext's register builder does
/// (generateCLRegisterListG16Common): the page and MiB counts share 0x101d1,
/// the extras count is its own register, and 0x107a1 either carries the window
/// with the enable bit or reads back the disabled constant 0x00ff_0000.
///
/// `mib` of zero means "derive it the way Metal does", which is what a faithful
/// request wants; pass a non-zero value only to sweep the two halves of 0x101d1
/// independently.  A request of zero pages reproduces the register program this
/// driver has always emitted, so the default path is unchanged.
pub(crate) fn scs_request_registers(
    pages: u32,
    mib: u32,
    extras: u32,
    window: u32,
) -> Result<ScsRequestRegisters, ComputeBuildError> {
    if pages == 0 {
        return Ok(ScsRequestRegisters {
            size_word: 0,
            extras: 0,
            window_word: 0xff_0000,
            enabled: false,
        });
    }
    // Keep the page and MiB halves distinct, and the window below bit 24's
    // enable flag. Reject unrepresentable diagnostic inputs before publishing
    // any register program instead of overlapping fields or truncating them.
    if pages > 0xffff || mib > 0xffff || window > 0xff {
        return Err(ComputeBuildError::ScsRequestOutOfRange);
    }
    let mib = if mib == 0 { scs_request_mib(pages) } else { mib };
    Ok(ScsRequestRegisters {
        size_word: (pages as u64) | ((mib as u64) << 16),
        extras: extras as u64,
        window_word: ((window as u64) << 16) | 0x0100_0000,
        enabled: true,
    })
}

/// One compute kick's primary register program.
///
/// The length is not fixed: `generateCLRegisterListG16Common` emits one extra
/// leading register when the descriptor carries an SCS scratch request, so the
/// descriptor's own count word at `COMPUTE_PRIMARY_COUNT` has to follow.
#[derive(Debug, Copy, Clone)]
pub(crate) struct ComputeRegisterProgram {
    registers: [ComputeRegister; COMPUTE_REGISTER_MAX],
    len: usize,
}

impl ComputeRegisterProgram {
    pub(crate) fn as_slice(&self) -> &[ComputeRegister] {
        &self.registers[..self.len]
    }

    pub(crate) fn len(&self) -> usize {
        self.len
    }
}

/// Build the compute register program, taking the SCS request from the module
/// parameters.
pub(crate) fn build_compute_registers(
    p: ComputeRegisterParameters,
) -> Result<ComputeRegisterProgram, ComputeBuildError> {
    let scs = scs_request_registers(
        crate::g18::fw::config::COMPUTE_SCS_PAGES,
        crate::g18::fw::config::COMPUTE_SCS_MIB,
        crate::g18::fw::config::COMPUTE_SCS_EXTRAS,
        crate::g18::fw::config::COMPUTE_SCS_WINDOW,
    )?;
    build_compute_registers_with_scs(p, scs)
}

/// Build a production compute register program from the per-command SCS ABI.
/// A zero/non-RT layout is byte-identical to the historical register program.
pub(crate) fn build_compute_registers_for_layout(
    p: ComputeRegisterParameters,
    layout: u64,
) -> Result<ComputeRegisterProgram, ComputeBuildError> {
    let pages = scs_pages_for_layout(layout)?;
    let scs = scs_request_registers(pages, 0, 0, 0)?;
    build_compute_registers_with_scs(p, scs)
}

/// The register program for an explicit SCS request.
///
/// Split out of `build_compute_registers` so the enabled arm is reachable from
/// a host test without a module parameter.
pub(crate) fn build_compute_registers_with_scs(
    p: ComputeRegisterParameters,
    scs: ScsRequestRegisters,
) -> Result<ComputeRegisterProgram, ComputeBuildError> {
    // Native reads CommandBufferState+8 and masks its low six bits here.
    // The logical compute ordinal at descriptor+0xf70 is a separate identity.
    let context_word = ((p.context_id as u64) << 8) | u64::from(p.work_ordinal & 63);

    let body = [
        ComputeRegister::new(0x1a510, p.preempt_base),
        ComputeRegister::new(0x1a420, p.cdm_base),
        ComputeRegister::new(0x1a4d0, add(p.preempt_base, 0x1480)?),
        ComputeRegister::new(0x1a4d8, add(p.preempt_base, 0x1488)?),
        ComputeRegister::new(0x1a4e0, add(p.preempt_base, 0x1490)?),
        ComputeRegister::new(0x1a4e8, add(p.preempt_base, 0x1498)?),
        ComputeRegister::new(0x10071, p.usc_exec_base),
        ComputeRegister::new(0x11841, p.helper_binary),
        ComputeRegister::new(0x11849, p.helper_data),
        ComputeRegister::new(0x11f81, p.helper_config),
        ComputeRegister::new(0x1a440, 0x0000_0001_5402_4201),
        ComputeRegister::new(0x1a458, 0x0000_0000_10c0_8860),
        ComputeRegister::new(0x101d9, 0x1c),
        ComputeRegister::new(0x1a089, 0),
        ComputeRegister::new(0x1a091, 0),
        ComputeRegister::new(0x1a059, 0),
        ComputeRegister::new(0x1a061, 0),
        ComputeRegister::new(0x1a0b9, 0),
        ComputeRegister::new(0x1a0c1, 0),
        ComputeRegister::new(0x101d1, scs.size_word),
        ComputeRegister::new(0x0d479, scs.extras),
        ComputeRegister::new(0x1a0e9, 8),
        ComputeRegister::new(0x107a1, scs.window_word),
        // `A[47:32]` is the per-thread private-context ("PTC"/DUPM) maximum.
        // `generateCLRegisterListG16Common` builds this value twice and both
        // times from the same accelerator constant, so the two emissions are
        // always equal; this driver's 0x132 was not:
        //
        //   first emission  `0xfffffe0008a6030c`: reads
        //     `AGXCLCommandDescriptor+0x548` and emits
        //     `CL[0x548] ? 0x0040_0020 | CL[0x548] << 32
        //                : 0x0000_0060_0040_0020` (`0xfffffe0008a6032c`);
        //   second emission `0xfffffe0008a60b4c`: reads the accelerator fields
        //     directly -- `accel[0x10790] ? (u32)accel[0x10790]
        //                                 : accel[0x10778]` -- and applies the
        //     identical expression (`0xfffffe0008a60b7c`).
        //
        // `CL+0x548` is seeded from `AGXUMADataRec+0x88`
        // (`AGXCLCommandDescriptor::prepare`, `0xfffffe00089fb85c`), which
        // `AGXUSCPrivMemPoolUMA::allocatePagesEnd` fills with that same
        // `accel[0x10790] ?: accel[0x10778]` (`0xfffffe0008aa7688`..
        // `0xfffffe0008aa76a0`).
        //
        // For G17P that constant is 0x60: `AGXAcceleratorG16::configureDevice`
        // stores `accel[0x10778] = 0x60` unconditionally
        // (`0xfffffe0008a55688`: `mov w8, #0x60`; `0xfffffe0008a5568c`:
        // `str w8, [x21, #0x1b8]`, x21 = accel+0x105c0), and
        // `AGXAcceleratorG17P::configureDevice` calls it as its first
        // instruction (`0xfffffe0008a64df8`).  Nothing in com.apple.AGXG17P
        // writes `accel[0x10790]`, so the `?:` selects `accel[0x10778]`, and
        // Apple's own zero-source fallback constant in the same expression is
        // literally `0x60` -- the two agree.  The render lists already emit
        // `0x0000_0060_0040_0020` for their analogues 0x0a5a1 and 0x0a5a9.
        ComputeRegister::new(0x0a599, 0x0000_0060_0040_0020),
        ComputeRegister::new(0x0d411, 0x0000_0002_0000_0001),
        ComputeRegister::new(0x1a540, p.dispatch_identity),
        ComputeRegister::new(0x014a9, p.dispatch_identity),
        ComputeRegister::new(0x0a351, p.dispatch_identity),
        ComputeRegister::new(0x10201, context_word),
        ComputeRegister::new(0x10428, context_word),
        ComputeRegister::new(0x14028, u64::from(p.usc_flist_id)),
        ComputeRegister::new(0x14070, p.robustness | 1),
        ComputeRegister::new(0x10229, add(p.operand_state_base, 0x12800)?),
        ComputeRegister::new(0x140a8, add(p.operand_state_base, 0x13000)?),
        ComputeRegister::new(0x10099, add(p.operand_state_base, 0x9405)?),
        ComputeRegister::new(0x10091, add(p.operand_state_base, 0x12400)?),
        ComputeRegister::new(0x0a5c1, add(p.operand_state_base, 0x0005)?),
        ComputeRegister::new(0x0a5c9, add(p.operand_state_base, 0x9000)?),
        ComputeRegister::new(0x1a440, 0x0000_0001_5402_4209),
        ComputeRegister::new(0x0a599, 0x0000_0060_0040_0020),
    ];

    // `generateCLRegisterListG16Common` writes its first register list entry
    // at buffer offset 0 only when `AGXCLCommandDescriptor+0x5c8` is clear.
    // When it is set the first entry is `0x17d9 = 1` and everything else moves
    // up by one 12-byte entry (mac17p `0xfffffe0008a5fa90` takes the branch,
    // `0xfffffe0008a5faa0` ORs the selector `0x17d9` into the entry word,
    // `0xfffffe0008a5fab0` stores the value 1, `0xfffffe0008a5fac4` sets the
    // cursor to 12 so `0x1a510` lands at entry 1; the clear arm at
    // `0xfffffe0008a5facc` sets the cursor to 0 instead).  The TA and 3D list
    // builders do the same thing with the same selector and value, each gated
    // on its own descriptor flag (`0xfffffe0008a585b8` on TA `+0x598`,
    // `0xfffffe0008a5a84c` on 3D `+0xb70`).
    //
    // This driver had never emitted `0x17d9`, which is the only difference
    // left between its `g17p_compute_scs_pages=1` program and the kext's: that
    // run already sent the byte-exact 0x101d1 / 0xd479 / 0x107a1 triple and its
    // shader still never launched.  That `0x17d9` is what the launch was
    // waiting on is an INFERENCE, not a measured result.
    //
    // What `0x17d9` means to the hardware is not established either.  None of
    // the selectors this program emits appears as an immediate anywhere in the
    // B100 firmware image (checked 0x17d8/0x17d9, 0x101d0, 0xd478, 0x107a0,
    // 0xa598, 0x1a510, 0x14070 and 0x1748), so the firmware replays the list
    // rather than interpreting it and the meaning is the GPU's, not the
    // firmware's.
    let mut registers = [ComputeRegister::new(0, 0); COMPUTE_REGISTER_MAX];
    let mut len = 0;
    if scs.enabled {
        registers[len] = ComputeRegister::new(0x17d9, 1);
        len += 1;
    }
    for register in body {
        registers[len] = register;
        len += 1;
    }
    Ok(ComputeRegisterProgram { registers, len })
}

fn put_u8(raw: &mut [u8], offset: usize, value: u8) {
    raw[offset] = value;
}

fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    raw[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

/// Build the retained queue record named by every compute CL entry.
pub(crate) fn build_compute_queue_graph(
    base: u64,
    channel_control: u64,
    raw: &mut [u8],
) -> Result<ComputeQueueGraphAddresses, ComputeBuildError> {
    if raw.len() < COMPUTE_QUEUE_GRAPH_SIZE {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    let addresses = ComputeQueueGraphAddresses {
        queue_record: base,
        pointers: add(base, 0x100)?,
        job_list: add(base, 0x200)?,
        item_ring: add(base, 0x4000)?,
    };
    let raw = &mut raw[..COMPUTE_QUEUE_GRAPH_SIZE];
    raw.fill(0);
    put_u64(raw, 0x00, addresses.pointers);
    put_u64(raw, 0x08, addresses.item_ring);
    put_u64(raw, 0x10, addresses.job_list);
    put_u32(raw, 0x24, u32::MAX);
    put_u32(raw, 0x28, 2);
    put_u32(raw, 0x2c, 2);
    raw[0x36..0x38].fill(0xff);
    put_u32(raw, 0x40, 2);
    put_u32(raw, 0x44, u32::MAX);
    // Bootstrap/unowned graph. A logical bind supplies the creator's TGID.
    put_u32(raw, 0x48, 0);
    put_u64(raw, 0x9c, channel_control);
    put_u32(raw, 0x100 + 0x50, u32::MAX);
    put_u32(raw, 0x100 + 0x60, compute_item_ring_entries());
    put_u64(raw, 0x200 + 0x08, addresses.job_list);
    Ok(addresses)
}

/// Usable entries in the queue's item ring, as programmed into the graph's
/// pointer block at `+0x160` and read back by both the host publisher and the
/// firmware.
///
/// Normally the full `0x500`. `g17p_ring_limit` shrinks it so the ring WRAP can
/// be reached in a handful of submissions instead of 320: at four records per
/// submission, a 32-entry ring wraps on the eighth. Wrap behaviour is identical
/// at any size, so this turns a 400-iteration run -- which also overflows the
/// kernel log and destroys its own evidence -- into a 20-iteration one.
pub(crate) fn compute_item_ring_entries() -> u32 {
    const FULL: u32 = 0x500;
    let requested = crate::g18::fw::config::RING_LIMIT;
    if requested == 0 {
        return FULL;
    }
    // Must stay a multiple of the per-submission record count so a batch never
    // straddles the wrap differently than it would at full size, and must leave
    // room for at least two submissions plus the always-empty slot.
    let clamped = requested.clamp(
        2 * COMPUTE_REPEAT_RING_RECORD_COUNT as u32 + 4,
        FULL,
    );
    clamped - (clamped % COMPUTE_REPEAT_RING_RECORD_COUNT as u32)
}

/// Identity of the configuration last published on one physical CL QID.
///
/// Logical scheduler pages are unique and retained until processor stop.
/// Their address therefore also distinguishes two root leases that reuse the
/// same numeric context ID, including queues sharing one VM. The root lease
/// is immutable while current; submit validates that before using this key.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeConfigIdentity {
    pub(crate) channel_control: u64,
    pub(crate) qos_id: u8,
    pub(crate) context_id: Option<u16>,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeConfigUpdates {
    pub(crate) qos: bool,
    pub(crate) context: bool,
}

impl ComputeConfigIdentity {
    pub(crate) fn updates_from(self, published: Option<Self>) -> ComputeConfigUpdates {
        match published {
            None => ComputeConfigUpdates { qos: true, context: true },
            Some(previous) => ComputeConfigUpdates {
                // QID membership is scoped to its logical scheduler page.
                // Policy is currently immutable for this logical lease.
                qos: self.channel_control != previous.channel_control
                    || self.qos_id != previous.qos_id,
                context: self.channel_control != previous.channel_control
                    || self.context_id != previous.context_id,
            },
        }
    }
}

/// Encode the tag-15 `KSMKickQueueConfigUpdate` record inside a retained
/// queue graph.
///
/// A retained submission may carry this record without reinstalling the QID
/// or changing its QoS/context. Native computes +0x52 from QoS ID, QID
/// membership and policy changes, and +0x62 from the channel's cached GART
/// allocation generation. See audit_g17_native_scheduler_owner.py.
pub(crate) fn encode_compute_config_update(
    graph: &mut [u8],
    sksm_entries: ComputeSksmEntryAliases,
    shared_control: u64,
    channel_control: u64,
    queue_id: u8,
    hardware_buffer_id: u8,
    install_queue: bool,
    updates: ComputeConfigUpdates,
    context_id: Option<u16>,
) -> Result<(), ComputeBuildError> {
    // AGXChannel::getPid supplies a full u32 at tag15 +0x5a. The queue's
    // logical-owner bind populates the same PID at record +0x48; reading that
    // retained identity avoids capturing a workqueue PID during submission.
    let owner_pid = u32::from_le_bytes(
        graph.get(0x48..0x4c).ok_or(ComputeBuildError::BufferTooSmall)?
            .try_into().map_err(|_| ComputeBuildError::BufferTooSmall)?,
    );
    let optional_raw = graph
        .get_mut(COMPUTE_OPTIONAL..COMPUTE_OPTIONAL + COMPUTE_OPTIONAL_SIZE)
        .ok_or(ComputeBuildError::BufferTooSmall)?;
    encode_compute_config_update_record(
        optional_raw,
        owner_pid,
        sksm_entries,
        shared_control,
        channel_control,
        queue_id,
        hardware_buffer_id,
        install_queue,
        updates,
        context_id,
    )
}

/// Encode one tag-15 `KSMKickQueueConfigUpdate` record into `optional_raw`
/// (`COMPUTE_OPTIONAL_SIZE` bytes), wherever that record lives.
///
/// A queue's FIRST bind encodes it in the queue graph's optional window; every
/// later publication encodes it in its own per-publication record slot. The
/// firmware reads a record through the item-ring pointer that names it, which
/// can be long after the host has published further kicks, so a record must
/// never be rewritten while a ring entry still names it. Measured on Neo
/// 2026-09-26 19:41: a repeat encoded into the graph window while the first
/// bind's ring entry still pointed there rewrote +0x1a (queue-install) to 0;
/// the firmware then consumed the first bind with the install branch skipped,
/// left the per-queue slot unpopulated, and its tag-16 handler faulted on the
/// NULL slot (pc=0x1190c far=0x140 esr=0x96000004), the same fault shape the
/// +0x1a note below records for sending 0 directly.
pub(crate) fn encode_compute_config_update_record(
    optional_raw: &mut [u8],
    owner_pid: u32,
    sksm_entries: ComputeSksmEntryAliases,
    shared_control: u64,
    channel_control: u64,
    queue_id: u8,
    hardware_buffer_id: u8,
    install_queue: bool,
    updates: ComputeConfigUpdates,
    context_id: Option<u16>,
) -> Result<(), ComputeBuildError> {
    let optional_raw = optional_raw
        .get_mut(..COMPUTE_OPTIONAL_SIZE)
        .ok_or(ComputeBuildError::BufferTooSmall)?;
    // Record +0x32 is the hardware context this queue's work is declared to run
    // in. It has always been the literal 2, from when every compute client ran
    // in the fixed pair `T8140_COMPUTE_CTXS = [2, 3]`.
    //
    // Those two contexts are a SINGLE GLOBAL alias pair:
    // `install_t8140_compute_context_alias` publishes the current client's VM
    // root into both and records one `t8140_alias_owner`. With one compute
    // client that is correct. With a second, the alias is re-pointed and the
    // loser's kicks -- still declaring 2 -- no longer name their own root,
    // while their real root lives in the app context the execution context
    // allocated (5..62). The kick already declares that real context at
    // `sgx+0x10428`; only this record still says 2.
    //
    // Probe-gated and default-off: 0 sends the historical constant, byte
    // identical to the previous driver; 1 sends the client's real context when
    // one is known. See `g17p_compute_config_context`.
    let declared_context = match context_id {
        Some(context) if crate::g18::fw::config::COMPUTE_CONFIG_CONTEXT != 0 => {
            context
        }
        _ => 2u16,
    };
    let flushid_fix = crate::g18::fw::config::FLUSHID_FIX != 0;
    let (flushid_object, flushid_slot, flushid_index) = if flushid_fix {
        (0u64, 0xffffu16, 0xffffu16)
    } else {
        (shared_control, 0x3fu16, crate::g18::fw::config::compute_flist_id(queue_id))
    };
    optional_raw.fill(0);
    put_u32(optional_raw, 0x00, 0x0f);
    put_u64(optional_raw, 0x08, sksm_entries.low());
    put_u64(optional_raw, 0x10, sksm_entries.high());
    for (offset, value) in [
        (0x18, u16::from(queue_id)),
        // +0x1a is the QUEUE-INSTALL flag, and it must be non-zero on a first
        // bind. Firmware RE (2026-08-31): the only writer of the per-queue slot
        // `0x127628 + qid*0x28` is at 0x119b8, and that store -- together with
        // slot[+0x00]=1, the per-data-master active bitmap, the sgx+0x21008
        // queue-base write and the 0x40 KTrace receipt -- is reachable only when
        // this field is non-zero. Sending 0 left slot[+0x08] NULL, and the tag-16
        // handler then faulted dereferencing it (firmware exception pc=0x1190c
        // far=0x340 esr=0x96000004, a level-0 translation fault), which is why
        // pause bit 7 taken at 0x118c0 was never cleared at 0x11948.
        //
        // The two captured macOS records show 0 here because both were taken on
        // already-installed queues, not on a first bind. Absence of the 0x40
        // KTrace code is the receipt that the install branch was not taken.
        // Queue-install flag: 1 only on a queue's FIRST bind. Native clears it
        // afterwards -- leaving it set forces a disable/reprogram/enable cycle
        // on the KSMFE queue for every submission, which is why a second submit
        // published fine and then never completed.
        (0x1a, if install_queue { 1 } else { 0 }),
        (0x1e, 2),
        (0x22, 2),
        (0x32, declared_context),
        (0x3e, flushid_slot),
        (0x46, flushid_index),
        (0x52, u16::from(updates.qos)),
        // AGXCLChannelSKSM::submitBuffer copies AGXQOSQueue +0x10 here.
        // Native retained CL queues receive distinct HardwareBufferIDs in
        // allocation order (1, 2, 3 in the captured QID 4/5/7 sequence).
        // This field is therefore queue identity, not the literal 1 from the
        // first output-positive queue.
        (0x56, u16::from(hardware_buffer_id)),
        (0x5e, 2),
        (0x62, u16::from(updates.context)),
        (0x66, 1),
    ] {
        put_u16(optional_raw, offset, value);
    }
    put_u64(optional_raw, 0x36, flushid_object);
    put_u64(optional_raw, 0x4a, channel_control);
    put_u32(optional_raw, 0x5a, owner_pid);
    optional_raw[0x76..0x86].fill(0xff);
    Ok(())
}

/// Encode one queue-context item.
///
/// The array is a sibling of the SKSM entry array, not a single record:
/// `COMPUTE_QUEUE_CONTEXT_EXTENT` is `8 * 0x4000 = 0x20000`, the item stride is
/// `0x200`, so it holds `0x100` items -- exactly the queue geometry's
/// `stamp_count`, and exactly the size and stride of the entry backing
/// (`0x100 * 0x200 = 0x20000`). The first bind writes item 1 and consumes stamp
/// index 1, which is consistent with the item being indexed by stamp.
///
/// That reading is INFERRED from the geometry, not proven, which is why the
/// repeat path selects between "rewrite item 1" and "write item[stamp]" by
/// parameter instead of assuming.
pub(crate) fn encode_compute_queue_context_item(
    queue_context: &mut [u8],
    item_index: usize,
    descriptor: u64,
    queue_base: u64,
    queue_id: u8,
) -> Result<(), ComputeBuildError> {
    let offset = item_index
        .checked_mul(COMPUTE_QUEUE_CONTEXT_ITEM)
        .ok_or(ComputeBuildError::AddressOverflow)?;
    let end = offset
        .checked_add(COMPUTE_QUEUE_CONTEXT_ITEM)
        .ok_or(ComputeBuildError::AddressOverflow)?;
    if end > queue_context.len() {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    let context = &mut queue_context[offset..end];
    context.fill(0);
    put_u64(context, 0x00, 0x1000_1000_0000_0004);
    put_u64(context, 0x10, descriptor);
    put_u64(context, 0x18, queue_base);
    put_u64(context, 0x20, 0xffff_0801_0000_0001);
    put_u64(context, 0x28, u64::from(queue_id) << 40);
    put_u64(context, 0x130, 0);
    put_u64(context, 0x138, 4);
    put_u64(context, 0x150, 0x0001_1003_8001_a002);
    put_u64(context, 0x158, 0x0000_2003_8001_a03b);
    put_u64(context, 0x178, 0x003f_ffff_ffff_ffff);
    Ok(())
}

/// Stage the first hardware-proven direct-compute group in a retained queue.
///
/// This is the CL_2 channel graph used by the output-positive m1n1 add3 test:
/// selector-3 descriptor, selector-0x0f optional record, selector-0x0e event,
/// followed by queue write index 3. The caller publishes the outer channel
/// slot and its producer only after this function returns.
pub(crate) fn build_initial_compute_channel_group(
    base: u64,
    descriptor: u64,
    sksm_entries: ComputeSksmEntryAliases,
    shared_control: u64,
    channel_control: u64,
    queue_id: u8,
    hardware_buffer_id: u8,
    graph: &mut [u8],
    queue_context: &mut [u8],
    install_queue: bool,
    context_id: Option<u16>,
) -> Result<ComputeChannelGroupAddresses, ComputeBuildError> {
    if graph.len() < COMPUTE_QUEUE_GRAPH_SIZE
        || queue_context.len() < COMPUTE_QUEUE_CONTEXT_SIZE
    {
        return Err(ComputeBuildError::BufferTooSmall);
    }

    let optional = add(base, COMPUTE_OPTIONAL as u64)?;
    let event = add(base, COMPUTE_EVENT as u64)?;

    // Record +0x48 is the process PID, not a UUID. Preserve the logical
    // owner's value installed when this physical graph was bound.
    for (index, address) in [descriptor, optional, event].into_iter().enumerate() {
        put_u64(graph, COMPUTE_ITEM_RING + index * 8, address);
    }
    put_u32(
        graph,
        COMPUTE_QUEUE_POINTERS + 0x40,
        COMPUTE_CHANNEL_WRITE_INDEX,
    );

    // B1 registers an exact pointer/cookie at table[record+0x46]; later
    // descriptor processing selects that same table with FList+0x08. Both
    // IDs must identify our canonical compute pool, not render's ID zero.
    // The cookie is stable for this retained session incarnation. All queues
    // register the same pointer/cookie; their scheduler identity is separate.
    encode_compute_config_update(
        graph,
        sksm_entries,
        shared_control,
        channel_control,
        queue_id,
        hardware_buffer_id,
        install_queue,
        ComputeConfigUpdates { qos: true, context: true },
        context_id,
    )?;

    let event_raw = &mut graph[COMPUTE_EVENT..COMPUTE_EVENT + COMPUTE_EVENT_SIZE];
    event_raw.fill(0);
    put_u32(event_raw, 0x00, 0x0e);
    put_u32(event_raw, 0x04, (1u32 << 16) | u32::from(queue_id));
    put_u32(event_raw, 0x08, 0x0102);
    put_u32(event_raw, 0x10, 0x0200);

    queue_context[..COMPUTE_QUEUE_CONTEXT_SIZE].fill(0);
    // Item index 1, which is also the stamp index the first bind consumes
    // (the geometry seeds `current_timestamp` at 1). See
    // `encode_compute_queue_context_item` for why that coincidence matters.
    encode_compute_queue_context_item(queue_context, 1, descriptor, base, queue_id)?;

    Ok(ComputeChannelGroupAddresses {
        queue: base,
        optional,
        event,
        write_index: COMPUTE_CHANNEL_WRITE_INDEX,
    })
}

fn encode_registers(raw: &mut [u8], offset: usize, registers: &[ComputeRegister]) {
    for (index, register) in registers.iter().enumerate() {
        let entry = offset + index * COMPUTE_REGISTER_SIZE;
        put_u32(raw, entry, register.number);
        put_u64(raw, entry + 4, register.value);
    }
}

fn register_value(registers: &[ComputeRegister], number: u32, occurrence: usize) -> u64 {
    registers
        .iter()
        .filter(|register| register.number == number)
        .nth(occurrence)
        .expect("fixed G17P register program lost a required register")
        .value
}

fn register_count_through(
    registers: &[ComputeRegister],
    number: u32,
    occurrence: usize,
) -> Result<u16, ComputeBuildError> {
    let index = registers
        .iter()
        .enumerate()
        .filter(|(_, register)| register.number == number)
        .nth(occurrence)
        .map(|(index, _)| index)
        .ok_or(ComputeBuildError::RegisterProgramShape)?;
    u16::try_from(index + 1).map_err(|_| ComputeBuildError::RegisterProgramShape)
}

/// Resume checkpoints consumed by B1 after a nonterminal CL completion.
///
/// The no-extra G16 common producer records the current register count after
/// the first 0x0a5c9, the final 0x1a440 (twice), and the final 0x0a599.  B1
/// reads descriptor +0x1a/+0x1c on status 2 and copies the selected low seven
/// bits into the selected SKSM kick-ring entry at +0x150.  Derive the counts from the
/// actual program so an SCS 0x17d9 prefix, or a later list edit, cannot leave
/// stale resume indices in the descriptor.
pub(crate) fn compute_resume_checkpoints(
    registers: &[ComputeRegister],
) -> Result<[u16; 4], ComputeBuildError> {
    let first_tail = register_count_through(registers, 0x0a5c9, 0)?;
    let final_control = register_count_through(registers, 0x1a440, 1)?;
    let final_tail = register_count_through(registers, 0x0a599, 1)?;
    Ok([first_tail, final_control, final_control, final_tail])
}

/// Encode a selector-3 descriptor without dropping sampler, timestamp, or CDM
/// command state.
pub(crate) fn build_compute_descriptor(
    program: &ComputeRegisterProgram,
    objects: ComputeDescriptorObjects,
    metadata: ComputeDescriptorMetadata,
    command: ComputeDescriptorCommand,
    raw: &mut [u8],
) -> Result<[u16; 4], ComputeBuildError> {
    let registers = program.as_slice();
    if raw.len() < COMPUTE_DESCRIPTOR_SIZE {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    if command.cdm_terminator < register_value(registers, 0x1a420, 0) {
        return Err(ComputeBuildError::CdmTerminatorBeforeStream);
    }
    if metadata.submission_index == 0 {
        return Err(ComputeBuildError::SubmissionIndexZero);
    }
    if metadata.queue_submission == 0 {
        return Err(ComputeBuildError::QueueSubmissionZero);
    }
    if command.sampler_count == u32::MAX {
        return Err(ComputeBuildError::SamplerCountOutOfRange);
    }
    if (command.sampler_array == 0) != (command.sampler_count == 0) {
        return Err(ComputeBuildError::SamplerPair);
    }
    if command.sampler_array & 7 != 0 {
        return Err(ComputeBuildError::SamplerAlignment);
    }

    let raw = &mut raw[..COMPUTE_DESCRIPTOR_SIZE];
    raw.fill(0);
    put_u32(raw, 0x00, COMPUTE_SELECTOR);
    put_u64(raw, 0x04, metadata.submit_sequence);
    put_u32(raw, 0x0c, metadata.context_id);
    put_u64(raw, 0x10, objects.scheduler_record);
    let resume_checkpoints = compute_resume_checkpoints(registers)?;
    for (index, value) in resume_checkpoints.into_iter().enumerate() {
        put_u16(raw, 0x18 + index * 2, value);
    }

    encode_registers(raw, COMPUTE_REGISTER_START, registers);
    put_u64(
        raw,
        0x740,
        add(objects.descriptor_low_alias, COMPUTE_REGISTER_START as u64)?,
    );
    // The count word follows the program, which grows by one entry when the
    // kick carries an SCS request -- exactly as the kext's own
    // `[sp,#0x58]`/`[sp,#0x5a]` byte-size and entry-count pair does
    // (`0xfffffe0008a5fab4` seeds them with 12 and 1 on the request arm).
    put_u32(
        raw,
        COMPUTE_PRIMARY_COUNT,
        ((program.len() * COMPUTE_REGISTER_SIZE) as u32) << 16 | program.len() as u32,
    );

    let secondary = [
        ComputeRegister::new(0x10099, register_value(registers, 0x0a5c1, 0)),
        ComputeRegister::new(0x10091, register_value(registers, 0x0a5c9, 0)),
        ComputeRegister::new(0x0a5c1, register_value(registers, 0x10099, 0)),
        ComputeRegister::new(0x0a5c9, register_value(registers, 0x10091, 0)),
    ];
    encode_registers(raw, COMPUTE_SECONDARY_REGISTER_START, &secondary);
    put_u64(
        raw,
        0x0e60,
        add(
            objects.descriptor_low_alias,
            COMPUTE_SECONDARY_REGISTER_START as u64,
        )?,
    );
    put_u32(raw, 0x0e68, 0x0030_0004);
    put_u32(raw, 0x0f28, u32::MAX);

    put_u64(raw, 0x0ed8, register_value(registers, 0x1a510, 0));
    put_u64(raw, 0x0ee0, command.cdm_terminator);
    put_u64(raw, 0x0f08, register_value(registers, 0x1a440, 0));
    put_u32(
        raw,
        0x0f20,
        (register_value(registers, 0x1a540, 0) >> 32) as u32,
    );
    put_u64(raw, COMPUTE_SAMPLER_ARRAY, command.sampler_array);
    put_u32(raw, COMPUTE_SAMPLER_COUNT, command.sampler_count);
    put_u32(
        raw,
        COMPUTE_SAMPLER_MAX,
        if command.sampler_count == 0 {
            0
        } else {
            command.sampler_count + 1
        },
    );
    put_u64(raw, 0x0f40, objects.dispatch_a);
    put_u64(raw, 0x0f48, objects.dispatch_b);
    // Bootstrap seed only. Every user publication route binds the actual kick
    // timestamp separately; queue_ordinal is a TSQ slot, not a stamp sequence.
    put_u32(raw, 0x0f50, COMPUTE_COMPLETION_STAMP_CONSTANT);
    put_u32(raw, 0x0f54, metadata.grid_index);
    put_u32(raw, 0x0f58, metadata.queue_ordinal);
    put_u32(raw, 0x0f60, metadata.submission_index);
    put_u64(
        raw,
        0x0f68,
        register_value(registers, 0x1a540, 0) & 0xffff_ffff,
    );
    put_u32(raw, 0x0f70, metadata.work_ordinal);
    put_u64(raw, 0x0f7c, objects.status_a);
    put_u64(raw, 0x0f84, objects.status_b);
    put_u64(raw, 0x0f8c, command.user_timestamp_start);
    put_u64(raw, 0x0f94, command.user_timestamp_end);

    put_u16(raw, 0x0fb0, 0x001a);
    put_u64(raw, 0x0fb2, objects.shared_control);
    put_u32(raw, 0x0fba, metadata.support_control);
    put_u32(raw, 0x0fbe, metadata.support_flags);
    put_u8(raw, 0x0fc5, 0x9f);
    put_u32(raw, 0x0fc8, (metadata.work_ordinal & 3) << 30);
    put_u64(raw, 0x0fcb, objects.usage_record);
    put_u8(raw, 0x0fd3, 1);
    Ok(resume_checkpoints)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn register_work_state_does_not_overwrite_the_context_id() {
        for word in [0, 62, 64, 0x1234_5678, u32::MAX - 1] {
            let mut p = register_parameters();
            p.context_id = 9;
            p.work_ordinal = word;
            let program = build_compute_registers(p).unwrap();
            let expected = (9 << 8) | u64::from(word & 63);
            assert_eq!(register_value(program.as_slice(), 0x10201, 0), expected);
            assert_eq!(register_value(program.as_slice(), 0x10428, 0), expected);
        }
    }

    #[test]
    fn hardware_timestamp_keeps_all_forty_bits_and_zero_payload() {
        for timestamp in [0, 1, 0xffff_ffff, 0x1_0000_0000, 0xff_ffff_ffff] {
            let mut raw = [0xa5u8; 0x1000];
            set_hardware_timestamp(&mut raw, timestamp).unwrap();
            assert_eq!(u64::from_le_bytes(raw[0xf60..0xf68].try_into().unwrap()), timestamp);
            assert!(raw[..0xf60].iter().all(|&v| v == 0xa5));
            assert!(raw[0xf68..].iter().all(|&v| v == 0xa5));
        }
        let mut raw = [0xa5u8; 0x1000];
        assert_eq!(set_hardware_timestamp(&mut raw, 1 << 40), Err(ComputeBuildError::HardwareTimestampOutOfRange));
        assert!(raw.iter().all(|&v| v == 0xa5));
        assert_eq!(set_hardware_timestamp(&mut [0; 4], 1), Err(ComputeBuildError::BufferTooSmall));
    }

    #[test]
    fn allocation_epoch_changes_only_its_byte_and_accepts_wrap() {
        for epoch in [0, 1, 2, 3, 127, 255] {
            let mut raw = [0xa5u8; 0x1000];
            set_context_generation(&mut raw, epoch).unwrap();
            assert_eq!(raw[0xfd3], epoch);
            assert!(raw[..0xfd3].iter().all(|&b| b == 0xa5));
            assert!(raw[0xfd4..].iter().all(|&b| b == 0xa5));
        }
        assert_eq!(set_context_generation(&mut [0; 0xfd3], 1), Err(ComputeBuildError::BufferTooSmall));
    }

    #[test]
    fn completion_stamp_covers_only_the_flushed_prefix_including_wrap() {
        for kick in [1u64, 0x100, 0x3628, 0x3629, 0xff_ffff, 0x100_0000, 0x123_4567_89ab] {
            let stamp = completion_stamp_value(kick);
            assert!(completion_stamp_covers(stamp, kick));
            assert!(!completion_stamp_covers(stamp, kick + 1));
            assert!(completion_stamp_covers(completion_stamp_value(kick + 16), kick));
            assert!(!completion_stamp_covers(stamp | 1, kick));
        }
        assert!(!completion_stamp_covers(0, 1));
        assert!(completion_stamp_covers(0, 0x100_0000));
        assert!(completion_stamp_covers(0, 0xff_ffff));
        assert!(!completion_stamp_covers(0xffff_ff00, 0x100_0000));
        assert!(!completion_stamp_covers(0x8000_0100, 1));
        let mut raw = [0u8; COMPUTE_COMPLETION_STAMP_VALUE + 4];
        set_completion_stamp_value(&mut raw, 0x3629).unwrap();
        assert_eq!(
            u32::from_le_bytes(raw[0x0f50..0x0f54].try_into().unwrap()),
            0x362900,
        );
        let mut raw2 = [0u8; COMPUTE_COMPLETION_STAMP_VALUE + 4];
        set_completion_stamp_value(&mut raw2, 1).unwrap();
        assert_ne!(raw2, raw);
        assert_eq!(set_completion_stamp_value(&mut [0; 4], 1), Err(ComputeBuildError::BufferTooSmall));
    }

    fn fnv1a64(raw: &[u8]) -> u64 {
        let mut value = 0xcbf2_9ce4_8422_2325u64;
        for byte in raw {
            value ^= *byte as u64;
            value = value.wrapping_mul(0x0000_0100_0000_01b3);
        }
        value
    }

    fn register_parameters() -> ComputeRegisterParameters {
        ComputeRegisterParameters {
            preempt_base: 0x100_0404_0000,
            cdm_base: 0x100_0410_0000,
            usc_exec_base: 0x1_0000_0000,
            helper_binary: 0,
            helper_data: 0,
            helper_config: 0,
            dispatch_identity: 0x1234_5678_0000_0001,
            context_id: 3,
            work_ordinal: 7,
            robustness: 0x100_0430_0000,
            operand_state_base: 0x70_0022_8000,
            usc_flist_id: 2,
        }
    }

    #[test]
    fn register_program_matches_the_hardware_tested_m1n1_builder() {
        let program = build_compute_registers(register_parameters()).unwrap();
        let registers = program.as_slice();
        assert_eq!(program.len(), COMPUTE_REGISTER_COUNT);
        let mut raw = [0u8; COMPUTE_REGISTER_COUNT * COMPUTE_REGISTER_SIZE];
        encode_registers(&mut raw, 0, registers);
        // Re-pinned when 0x0a599's first emission was corrected from 0x132
        // to the 0x60 both of the kext's emissions carry.
        assert_eq!(fnv1a64(&raw), 0x2d33_2bc4_ecd2_8bdd);
        assert_eq!(registers[0], ComputeRegister::new(0x1a510, 0x100_0404_0000));
        assert_eq!(registers[39], ComputeRegister::new(0x0a599, 0x6000_4000_20));
    }

    #[test]
    fn both_0xa599_emissions_carry_the_same_accelerator_constant() {
        // `generateCLRegisterListG16Common` reads one accelerator quantity for
        // both 0xa598 emissions (`0xfffffe0008a6030c` through
        // `AGXCLCommandDescriptor+0x548`, and `0xfffffe0008a60b4c` straight
        // from `accel[0x10790] ?: accel[0x10778]`), so they cannot disagree.
        // This driver's two writes did; the first carried 0x132.
        let program = build_compute_registers(register_parameters()).unwrap();
        let first = register_value(program.as_slice(), 0x0a599, 0);
        let second = register_value(program.as_slice(), 0x0a599, 1);
        assert_eq!(first, second);
        // 0x60 is `AGXAcceleratorG16::configureDevice`'s
        // `accel[0x10778] = 0x60` (`0xfffffe0008a5568c`), and also the kext's
        // own zero-source fallback constant in the same expression.
        assert_eq!(first, 0x0000_0060_0040_0020);
    }

    #[test]
    fn an_scs_request_prepends_the_0x17d9_enable_register() {
        let scs = scs_request_registers(89, 1, 0, 0).unwrap();
        assert!(scs.enabled);
        let program =
            build_compute_registers_with_scs(register_parameters(), scs).unwrap();
        assert_eq!(program.len(), COMPUTE_REGISTER_MAX);
        // Entry 0 is the enable register, and the historical program follows
        // it unchanged -- the kext moves its cursor to 12 before writing
        // 0x1a510 (`0xfffffe0008a5fac4`).
        assert_eq!(program.as_slice()[0], ComputeRegister::new(0x17d9, 1));
        assert_eq!(
            program.as_slice()[1],
            ComputeRegister::new(0x1a510, 0x100_0404_0000)
        );
        // Nothing else moves: the same registers in the same order, and only
        // the three request registers change value.
        let disabled = build_compute_registers(register_parameters()).unwrap();
        assert_eq!(program.as_slice().len(), disabled.as_slice().len() + 1);
        for (enabled, disabled) in program.as_slice()[1..].iter().zip(disabled.as_slice()) {
            assert_eq!(enabled.number, disabled.number);
            if !matches!(enabled.number, 0x101d1 | 0x0d479 | 0x107a1) {
                assert_eq!(enabled.value, disabled.value);
            }
        }
        // The three request registers still carry the request itself.
        assert_eq!(register_value(program.as_slice(), 0x101d1, 0), 0x0001_0059);
        assert_eq!(register_value(program.as_slice(), 0x107a1, 0), 0x0100_0000);
        assert_eq!(
            compute_resume_checkpoints(program.as_slice()).unwrap(),
            [39, 40, 40, 41]
        );
    }

    #[test]
    fn no_scs_request_emits_no_enable_register() {
        let program = build_compute_registers(register_parameters()).unwrap();
        assert!(program
            .as_slice()
            .iter()
            .all(|register| register.number != 0x17d9));
        assert_eq!(register_value(program.as_slice(), 0x107a1, 0), 0xff_0000);
    }

    #[test]
    fn an_empty_scs_request_reproduces_the_historical_registers() {
        // Zero pages is the kext's "this pass asks for no ray-traversal
        // scratch" branch, and is what this driver emitted before the request
        // became settable.  register_program_matches_the_hardware_tested_m1n1_builder
        // above pins the same thing through the whole program's hash.
        let scs = scs_request_registers(0, 0, 0, 0).unwrap();
        assert_eq!(scs.size_word, 0);
        assert_eq!(scs.extras, 0);
        assert_eq!(scs.window_word, 0xff_0000);
        // The window is ignored while no pages are requested.
        assert_eq!(scs_request_registers(0, 7, 3, 9), scs_request_registers(0, 7, 3, 0));
    }

    #[test]
    fn a_nonempty_scs_request_packs_pages_mib_extras_and_window() {
        // 89 pages is the four-section floor (0x58000 bytes) plus the guard
        // page AGXMetalG17P adds; one MiB covers it ((89 * 4 + 0x3ff) >> 10).
        let scs = scs_request_registers(89, 1, 0, 0).unwrap();
        assert_eq!(scs.size_word, 0x0001_0059);
        assert_eq!(scs.extras, 0);
        assert_eq!(scs.window_word, 0x0100_0000);

        let wide = scs_request_registers(815, 4, 2, 0x20).unwrap();
        assert_eq!(wide.size_word, 0x0004_032f);
        assert_eq!(wide.extras, 2);
        assert_eq!(wide.window_word, 0x0120_0000);
    }

    #[test]
    fn an_unset_mib_is_derived_from_the_page_count_like_metal_does() {
        // AGXMetalG17P computes MiB as (pages * 4 + 0x3ff) >> 10 and never
        // leaves it zero next to a non-zero page count, so the driver must not
        // either: an operator who sets only the page count still gets the
        // register value a real ray-tracing kick carries.
        // 0x59 pages = the four fixed sections plus the guard page.
        assert_eq!(scs_request_registers(0x59, 0, 0, 0).unwrap().size_word, 0x0001_0059);
        // 0x32f pages = the intersector<triangle_data> layout
        // (traversal depth 0x70, driver payload 0x48, 96 cliques).
        assert_eq!(scs_request_registers(0x32f, 0, 0, 0).unwrap().size_word, 0x0004_032f);
        // 0x3c8 pages = the intersection_query<> layout (driver payload 0x114).
        assert_eq!(scs_request_registers(0x3c8, 0, 0, 0).unwrap().size_word, 0x0004_03c8);
        // 0x6a7 pages = intersection_query<instancing,triangle_data> on a
        // top-level structure, the Vulkan rayQueryEXT shape
        // (traversal depth 0xe8, driver payload 0x128).
        assert_eq!(scs_request_registers(0x6a7, 0, 0, 0).unwrap().size_word, 0x0007_06a7);
        // An explicit value still wins, so the halves stay separable.
        assert_eq!(scs_request_registers(0x59, 9, 0, 0).unwrap().size_word, 0x0009_0059);
        // Zero pages keeps the historical register exactly.
        assert_eq!(scs_request_registers(0, 0, 0, 0).unwrap().size_word, 0);
    }

    #[test]
    fn scs_layout_sizes_match_the_measured_metal_shapes() {
        let layout = |depth: u64, driver: u64| depth | (96 << 24) | (driver << 48);

        assert_eq!(scs_pages_for_layout(layout(0x70, 0x48)).unwrap(), 0x32f);
        assert_eq!(scs_pages_for_layout(layout(0x70, 0x114)).unwrap(), 0x3c8);
        assert_eq!(scs_pages_for_layout(layout(0xe8, 0x128)).unwrap(), 0x6a7);

        // The native endComputePass gate suppresses a request when both
        // payload sizes are zero, even if depth/clique fields are populated.
        assert_eq!(scs_pages_for_layout(0x70 | (96 << 24)).unwrap(), 0);
        assert_eq!(scs_pages_for_layout(0).unwrap(), 0);
    }

    #[test]
    fn scs_layout_rejects_an_unrepresentable_request() {
        assert_eq!(
            scs_pages_for_layout(u64::MAX),
            Err(ComputeBuildError::ScsRequestOutOfRange)
        );
    }

    #[test]
    fn descriptor_preserves_cdm_sampler_timestamp_and_queue_state() {
        let registers = build_compute_registers(register_parameters()).unwrap();
        let mut raw = [0u8; COMPUTE_DESCRIPTOR_SIZE];
        build_compute_descriptor(
            &registers,
            ComputeDescriptorObjects {
                scheduler_record: 0xffff_fc20_c099_8100,
                descriptor_low_alias: 0x70_0034_0000,
                dispatch_a: 0xffff_fc20_001c_8028,
                dispatch_b: 0xffff_fc20_c07c_0028,
                status_a: 0xffff_fc20_0002_4c68,
                status_b: 0xffff_fc20_0002_4c70,
                shared_control: 0xffff_fc20_c095_0000,
                usage_record: 0xffff_fc20_0171_0000,
            },
            ComputeDescriptorMetadata {
                submit_sequence: 5,
                context_id: 3,
                grid_index: 2,
                work_ordinal: 7,
                queue_submission: 8,
                queue_ordinal: 7,
                submission_index: 9,
                support_control: 0xe0a0_0001,
                support_flags: 0,
            },
            ComputeDescriptorCommand {
                cdm_terminator: 0x100_0410_002c,
                sampler_array: 0x100_0800_0000,
                sampler_count: 4,
                user_timestamp_start: 0x200_0000_4000,
                user_timestamp_end: 0x200_0000_4008,
            },
            &mut raw,
        )
        .unwrap();

        // Re-pinned with the register-derived B1 resume checkpoints, and again
        // with the constant completion stamp at +0x0F50 (COMPUTE_COMPLETION_STAMP_CONSTANT).
        assert_eq!(fnv1a64(&raw), 0x8a56_8b1c_2a27_0524);
        assert_eq!(u64::from_le_bytes(raw[0x0ee0..0x0ee8].try_into().unwrap()), 0x100_0410_002c);
        assert_eq!(u64::from_le_bytes(raw[COMPUTE_SAMPLER_ARRAY..COMPUTE_SAMPLER_ARRAY + 8].try_into().unwrap()), 0x100_0800_0000);
        assert_eq!(u32::from_le_bytes(raw[COMPUTE_SAMPLER_COUNT..COMPUTE_SAMPLER_COUNT + 4].try_into().unwrap()), 4);
        assert_eq!(u64::from_le_bytes(raw[0x0f8c..0x0f94].try_into().unwrap()), 0x200_0000_4000);
        // The raw builder retains its bootstrap seed. User publication binds
        // the physical queue's actual completion prefix after this step.
        assert_eq!(u32::from_le_bytes(raw[0x0f50..0x0f54].try_into().unwrap()), COMPUTE_COMPLETION_STAMP_CONSTANT);
        assert_eq!(
            [
                u16::from_le_bytes(raw[0x18..0x1a].try_into().unwrap()),
                u16::from_le_bytes(raw[0x1a..0x1c].try_into().unwrap()),
                u16::from_le_bytes(raw[0x1c..0x1e].try_into().unwrap()),
                u16::from_le_bytes(raw[0x1e..0x20].try_into().unwrap()),
            ],
            [38, 39, 39, 40]
        );
    }

    #[test]
    fn invalid_command_relations_are_caught_before_encoding() {
        let registers = build_compute_registers(register_parameters()).unwrap();
        let objects = ComputeDescriptorObjects {
            scheduler_record: 1,
            descriptor_low_alias: 0x70_0000_0000,
            dispatch_a: 2,
            dispatch_b: 3,
            status_a: 4,
            status_b: 5,
            shared_control: 6,
            usage_record: 7,
        };
        let metadata = ComputeDescriptorMetadata {
            submit_sequence: 1,
            context_id: 3,
            grid_index: 2,
            work_ordinal: 0,
            queue_submission: 1,
            queue_ordinal: 0,
            submission_index: 1,
            support_control: 0xe0a0_0001,
            support_flags: 0,
        };
        let mut raw = [0u8; COMPUTE_DESCRIPTOR_SIZE];
        assert_eq!(
            build_compute_descriptor(
                &registers,
                objects,
                metadata,
                ComputeDescriptorCommand {
                    cdm_terminator: register_parameters().cdm_base - 4,
                    sampler_array: 0,
                    sampler_count: 0,
                    user_timestamp_start: 0,
                    user_timestamp_end: 0,
                },
                &mut raw,
            ),
            Err(ComputeBuildError::CdmTerminatorBeforeStream)
        );
        assert_eq!(
            build_compute_descriptor(
                &registers,
                objects,
                metadata,
                ComputeDescriptorCommand {
                    cdm_terminator: register_parameters().cdm_base,
                    sampler_array: 0,
                    sampler_count: 1,
                    user_timestamp_start: 0,
                    user_timestamp_end: 0,
                },
                &mut raw,
            ),
            Err(ComputeBuildError::SamplerPair)
        );
    }

    #[test]
    fn queue_graph_matches_the_hardware_tested_m1n1_builder() {
        let mut raw = [0u8; COMPUTE_QUEUE_GRAPH_SIZE];
        let addresses = build_compute_queue_graph(
            0xffff_fc20_c100_0000,
            0xffff_fc20_c200_0000,
            &mut raw,
        )
        .unwrap();
        assert_eq!(
            addresses,
            ComputeQueueGraphAddresses {
                queue_record: 0xffff_fc20_c100_0000,
                pointers: 0xffff_fc20_c100_0100,
                job_list: 0xffff_fc20_c100_0200,
                item_ring: 0xffff_fc20_c100_4000,
            }
        );
        assert_eq!(u32::from_le_bytes(raw[0x48..0x4c].try_into().unwrap()), 0);
        // Normalize only the captured process identity for the historical hash.
        put_u32(&mut raw, 0x48, 0x1aa);
        assert_eq!(fnv1a64(&raw), 0x3490_ef31_add0_ac88);
    }

    #[test]
    fn initial_channel_group_matches_the_output_positive_add3_builder() {
        let base = 0xffff_fc20_c100_0000;
        let descriptor = 0xffff_fc20_c200_0000;
        let mut graph = [0u8; COMPUTE_QUEUE_GRAPH_SIZE];
        build_compute_queue_graph(base, 0xffff_fc20_c500_0000, &mut graph).unwrap();
        put_u32(&mut graph, 0x48, 0x1234_5678);
        let mut context = vec![0u8; COMPUTE_QUEUE_CONTEXT_EXTENT];
        let addresses = build_initial_compute_channel_group(
            base,
            descriptor,
            ComputeSksmEntryAliases::new(
                0x70_0040_0000,
                0xffff_fc20_c300_0000,
                0x8000,
                0x100,
            )
            .unwrap(),
            0xffff_fc20_c400_0000,
            0xffff_fc20_c500_0000,
            4,
            1,
            &mut graph,
            &mut context,
            true,
            None,
        )
        .unwrap();
        assert_eq!(
            addresses,
            ComputeChannelGroupAddresses {
                queue: base,
                optional: base + 0x6800,
                event: base + 0x6c00,
                write_index: 3,
            }
        );
        // The compute pool uses ID 1 consistently in registration and FList.
        // Normalize only the captured PID for the original whole-graph golden.
        assert_eq!(&graph[COMPUTE_OPTIONAL + 0x46..COMPUTE_OPTIONAL + 0x48], &[5, 0]);
        assert_eq!(&graph[0x48..0x4c], &0x1234_5678u32.to_le_bytes());
        assert_eq!(&graph[COMPUTE_OPTIONAL + 0x5a..COMPUTE_OPTIONAL + 0x5e], &0x1234_5678u32.to_le_bytes());
        put_u32(&mut graph, 0x48, 0x170);
        put_u32(&mut graph, COMPUTE_OPTIONAL + 0x5a, 0x170);
        assert_eq!(fnv1a64(&graph), 0x66c2_81b4_ef4e_f1db);
        assert_eq!(fnv1a64(&context), 0x5ba5_b355_8c90_0783);
    }

    #[test]
    fn config_update_uses_physical_graph_flist_index_not_qos() {
        let mut graph = [0u8; COMPUTE_QUEUE_GRAPH_SIZE];
        let aliases = ComputeSksmEntryAliases::new(
            0x70_0040_0000,
            0xffff_fc20_c300_0000,
            0x8000,
            0x100,
        )
        .unwrap();
        encode_compute_config_update(
            &mut graph,
            aliases,
            0xffff_fc20_c400_0000,
            0xffff_fc20_c500_0000,
            5,
            2,
            true,
            ComputeConfigUpdates { qos: true, context: true },
            None,
        )
        .unwrap();
        assert_eq!(&graph[COMPUTE_OPTIONAL + 0x46..COMPUTE_OPTIONAL + 0x48], &[6, 0]);
        assert_eq!(
            &graph[COMPUTE_OPTIONAL + 0x56..COMPUTE_OPTIONAL + 0x58],
            &[2, 0]
        );
    }

    #[test]
    fn sksm_entry_aliases_preserve_one_backing_offset() {
        const EXTENT: usize = 0x8000;
        let aliases = ComputeSksmEntryAliases::new(
            0x70_004d_8000,
            0xffff_fc20_0027_8000,
            EXTENT,
            0x100,
        )
        .unwrap();
        assert_eq!(
            aliases.range_at(0x200, 0x200),
            Ok([0x70_004d_8200, 0xffff_fc20_0027_8200])
        );
        assert_eq!(
            aliases.range_at(EXTENT - 0x200, 0x200),
            Ok([0x70_004d_fe00, 0xffff_fc20_0027_fe00])
        );
        assert_eq!(
            aliases.range_at(EXTENT, 1),
            Err(ComputeBuildError::InvalidSksmEntryAliases)
        );
        assert_eq!(
            ComputeSksmEntryAliases::new(0x70_004d_8000, 0x70_004d_8000, EXTENT, 0x100),
            Err(ComputeBuildError::InvalidSksmEntryAliases)
        );
        assert_eq!(
            ComputeSksmEntryAliases::new(
                0x70_004d_8001,
                0xffff_fc20_0027_8000,
                EXTENT,
                0x100,
            ),
            Err(ComputeBuildError::InvalidSksmEntryAliases)
        );
    }
}
