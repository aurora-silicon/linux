// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! T8140/G17P manager construction boundary.
//!
//! This path needs only target identity, init-data geometry, and the retained
//! dual-role resource owner. It does not depend on the AGX2 `HwConfig` and it
//! performs no publication during construction. Explicit queue methods own the
//! resource-0 SKSM configuration and kick-store sequence.

#[cfg(not(test))]
use kernel::prelude::*;
#[cfg(not(test))]
use kernel::sync::Arc;
#[cfg(not(test))]
use kernel::time::{delay::fsleep, Delta};
#[cfg(all(not(test), CONFIG_DEV_COREDUMP))]
use crate::g17_trace_capture::{self as trace, TraceArchive};
#[cfg(all(not(test), CONFIG_DEV_COREDUMP))]
use kernel::time::{ClockSource, Monotonic};

#[cfg(not(test))]
use core::sync::atomic::{fence, AtomicU32, Ordering};

#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_IDLE: u32 = 0;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_B2: u32 = 1;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_TAG15: u32 = 2;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_QID: u32 = 3;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_SKSM_ENTRY: u32 = 4;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_TAG16: u32 = 5;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_ADD_KICKS: u32 = 6;
#[cfg(not(test))]
pub(crate) const G17P_SUBMIT_PHASE_CL2: u32 = 7;

#[cfg(not(test))]
use crate::{
    driver::AsahiDevice, g17_adt_j700, g17_completion, g17_compute, g17_initdata, g17_lifecycle,
    g17_render, g17_resources, g17_submission, g17_uapi, hw::agx3, identity, mmu,
    module_parameters, pgtable, regs,
};
#[cfg(test)]
use super as g17_submission;
#[cfg(test)]
#[path = "g17_initdata.rs"]
mod g17_initdata;
#[cfg(test)]
#[path = "identity.rs"]
mod identity;

use g17_initdata::InstanceRole;
use identity::{FirmwareRoleTopology, GpuGen, GpuHalGeneration, GpuVariant, SubmissionTransport};

static G17P_STATE_DUMPED: core::sync::atomic::AtomicBool =
    core::sync::atomic::AtomicBool::new(false);

/// One-shot guard for the hardware-data dump: it must fire on the first
/// render checkpoint whatever the label, since gating it on a specific label
/// simply never matched.
static G17P_HW_DATA_DUMPED: core::sync::atomic::AtomicBool =
    core::sync::atomic::AtomicBool::new(false);

const G17P_BOOTSTRAP_UMA_SEQUENCE: u64 =
    g17_submission::g17p_render_flist_generation(0) as u64;
const G17P_BOOTSTRAP_UMA_HARDWARE_BUFFER_ID: u32 =
    g17_initdata::PARTIAL_OPENING_FREELIST_HARDWARE_BUFFER_ID;
const G17P_BOOTSTRAP_UMA_STAMP: i32 = 1;
// B1's type-13 completion echoes the submitted USC FList end address and the
// record's fixed +0x2c count. Keep the retained owner matched to those wire
// values rather than the adjacent operand-buffer cookie used by the older
// opening graph.
const G17P_BOOTSTRAP_UMA_COOKIE: u64 = g17_submission::G17P_USC_FREELIST_LOW_END;
const G17P_BOOTSTRAP_UMA_COUNT: u32 =
    g17_submission::G17P_USC_FREELIST_GROW_DESCRIPTOR_BYTES;
const G17P_NATIVE_RENDER_KICK_AUXILIARY: u64 = 0x003f_ffff_ffff_ffff;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PNativeRenderPublishPhase {
    FragmentVisible,
    TilingPrefix,
    TilingSksm,
    TilingKick,
    TilingOuter,
    PoolTransition,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PNativeRenderPublishOrder {
    phase: G17PNativeRenderPublishPhase,
}

impl G17PNativeRenderPublishOrder {
    const fn after_fragment_doorbell() -> Self {
        Self {
            phase: G17PNativeRenderPublishPhase::FragmentVisible,
        }
    }

    fn advance(
        &mut self,
        next: G17PNativeRenderPublishPhase,
    ) -> core::result::Result<(), ()> {
        let expected = match self.phase {
            G17PNativeRenderPublishPhase::FragmentVisible => {
                G17PNativeRenderPublishPhase::TilingPrefix
            }
            G17PNativeRenderPublishPhase::TilingPrefix => {
                G17PNativeRenderPublishPhase::TilingSksm
            }
            G17PNativeRenderPublishPhase::TilingSksm => {
                G17PNativeRenderPublishPhase::TilingKick
            }
            G17PNativeRenderPublishPhase::TilingKick => {
                G17PNativeRenderPublishPhase::TilingOuter
            }
            G17PNativeRenderPublishPhase::TilingOuter => {
                G17PNativeRenderPublishPhase::PoolTransition
            }
            G17PNativeRenderPublishPhase::PoolTransition => return Err(()),
        };
        if next != expected {
            return Err(());
        }
        self.phase = next;
        Ok(())
    }
}

fn g17p_native_fragment_only_visible(
    previous_tiling_inner: u32,
    tiling_inner: u32,
    fragment_inner: u32,
    published_prefix: u32,
    fragment_outer: u32,
    fragment_outer_next: u32,
    tiling_outer: u32,
    tiling_outer_slot: u8,
) -> bool {
    tiling_inner == previous_tiling_inner
        && fragment_inner == published_prefix
        && fragment_outer == fragment_outer_next
        && tiling_outer == u32::from(tiling_outer_slot)
}

fn g17p_native_tiling_fully_visible(
    tiling_inner: u32,
    published_prefix: u32,
    tiling_outer: u32,
    tiling_outer_next: u32,
) -> bool {
    tiling_inner == published_prefix && tiling_outer == tiling_outer_next
}

fn g17p_native_fragment_first_mode(mode: u32) -> bool {
    mode == 2 || mode == 3
}

fn g17p_render_deferred_outer_mode(
    native_doorbell_mode: u32,
    split_launch: bool,
) -> g17_resources::G17PDeferredOuterPublication {
    if split_launch {
        // The TA group is complete, but the USC FList control notification
        // can wake it before the intended initial doorbell. Retain both here;
        // split launch publishes only TA after the FList transaction retires.
        g17_resources::G17PDeferredOuterPublication::Both
    } else if g17p_native_fragment_first_mode(native_doorbell_mode) {
        g17_resources::G17PDeferredOuterPublication::Tiling
    } else if native_doorbell_mode == 0 {
        g17_resources::G17PDeferredOuterPublication::Both
    } else {
        g17_resources::G17PDeferredOuterPublication::None
    }
}

fn g17p_native_fragment_preack_mode(mode: u32) -> bool {
    mode == 3
}

fn g17p_native_fragment_outer_consumed(counters: [u32; 3], expected: u32) -> bool {
    counters[0] == expected && counters[2] == expected
}

const G17P_TA_HARDWARE_BUFFER_ID_COUNT: usize = 0x7f;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PHardwareBufferIdError {
    Exhausted,
    InvalidId,
    DoubleFree,
}

#[derive(Debug, Clone)]
struct G17PHardwareBufferIdAllocator {
    free: [u64; 2],
    stack: [u8; G17P_TA_HARDWARE_BUFFER_ID_COUNT],
    stack_len: usize,
    next_parameter_buffer_token: u64,
}

impl G17PHardwareBufferIdAllocator {
    fn new() -> Self {
        let mut stack = [0u8; G17P_TA_HARDWARE_BUFFER_ID_COUNT];
        for (index, slot) in stack.iter_mut().enumerate() {
            *slot = (G17P_TA_HARDWARE_BUFFER_ID_COUNT - 1 - index) as u8;
        }
        Self {
            // 127 valid bits: IDs 0..126.
            free: [u64::MAX, u64::MAX >> 1],
            stack,
            stack_len: G17P_TA_HARDWARE_BUFFER_ID_COUNT,
            next_parameter_buffer_token: 0,
        }
    }

    fn is_free(&self, id: u32) -> bool {
        let index = id as usize;
        index < G17P_TA_HARDWARE_BUFFER_ID_COUNT
            && self.free[index / 64] & (1u64 << (index % 64)) != 0
    }

    fn allocate(&mut self) -> core::result::Result<u32, G17PHardwareBufferIdError> {
        let id = if self.stack_len != 0 {
            self.stack_len -= 1;
            self.stack[self.stack_len] as u32
        } else if self.free[0] != 0 {
            self.free[0].trailing_zeros()
        } else if self.free[1] != 0 {
            64 + self.free[1].trailing_zeros()
        } else {
            return Err(G17PHardwareBufferIdError::Exhausted);
        };
        if !self.is_free(id) {
            return Err(G17PHardwareBufferIdError::DoubleFree);
        }
        let index = id as usize;
        self.free[index / 64] &= !(1u64 << (index % 64));
        Ok(id)
    }

    fn allocate_with_token(
        &mut self,
    ) -> core::result::Result<(u32, u64), G17PHardwareBufferIdError> {
        let id = self.allocate()?;
        let token = if self.next_parameter_buffer_token == u64::MAX {
            0
        } else {
            self.next_parameter_buffer_token
        };
        self.next_parameter_buffer_token = token.wrapping_add(1);
        Ok((id, token))
    }

    fn release(&mut self, id: u32) -> core::result::Result<(), G17PHardwareBufferIdError> {
        let index = id as usize;
        if index >= G17P_TA_HARDWARE_BUFFER_ID_COUNT {
            return Err(G17PHardwareBufferIdError::InvalidId);
        }
        if self.is_free(id) {
            return Err(G17PHardwareBufferIdError::DoubleFree);
        }
        self.free[index / 64] |= 1u64 << (index % 64);
        if self.stack_len < self.stack.len() {
            self.stack[self.stack_len] = id as u8;
            self.stack_len += 1;
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PBootstrapUmaCompletion {
    pub(crate) stamp: i32,
    pub(crate) hardware_buffer_id: u32,
    pub(crate) shared_control: u64,
    pub(crate) operand_table: u64,
    pub(crate) cookie: u64,
    pub(crate) count: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PBootstrapUmaRelease {
    pub(crate) sequence: u64,
    pub(crate) hardware_buffer_id: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PBootstrapUmaRequestState {
    Submitted,
    Ready,
    Published,
    Sent,
    Completed,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PBootstrapUmaLifecycleError {
    WrongPhase,
    WrongCompletion,
    WrongOwner,
    WrongReferenceCount,
    WrongPendingCount,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct G17PBootstrapUmaLifecycle {
    sequence: u64,
    hardware_buffer_id: u32,
    hardware_buffer_owned: bool,
    hardware_buffer_references: u32,
    pending_release_count: u32,
    release_producer_after: Option<u32>,
    request: Option<G17PBootstrapUmaCompletion>,
    state: G17PBootstrapUmaRequestState,
}

impl G17PBootstrapUmaLifecycle {
    const fn new() -> Self {
        Self {
            sequence: G17P_BOOTSTRAP_UMA_SEQUENCE,
            hardware_buffer_id: G17P_BOOTSTRAP_UMA_HARDWARE_BUFFER_ID,
            hardware_buffer_owned: true,
            hardware_buffer_references: 4,
            pending_release_count: 2,
            release_producer_after: None,
            request: Some(G17PBootstrapUmaCompletion {
                stamp: G17P_BOOTSTRAP_UMA_STAMP,
                hardware_buffer_id: G17P_BOOTSTRAP_UMA_HARDWARE_BUFFER_ID,
                shared_control: g17_initdata::CONTROL_SHARED_ADDRESS,
                operand_table: g17_initdata::CONTROL_OPERAND_TABLE_ADDRESS,
                cookie: G17P_BOOTSTRAP_UMA_COOKIE,
                count: G17P_BOOTSTRAP_UMA_COUNT,
            }),
            state: G17PBootstrapUmaRequestState::Submitted,
        }
    }

    /// The current fixed pool has one cold grow request. Reacquiring the
    /// unchanged pool creates command references, not another grow request.
    /// B1 12278..12298 calls 1b230 from warm tag-15 even with install=0;
    /// this rebinds the owner/generation invalidated by the preceding 0x2e.
    fn acquire(
        previous: Option<&Self>,
        sequence: u64,
    ) -> core::result::Result<Self, G17PBootstrapUmaLifecycleError> {
        let Some(previous) = previous else {
            return if sequence == G17P_BOOTSTRAP_UMA_SEQUENCE {
                Ok(Self::new())
            } else {
                Err(G17PBootstrapUmaLifecycleError::WrongOwner)
            };
        };
        if previous.hardware_buffer_id != G17P_BOOTSTRAP_UMA_HARDWARE_BUFFER_ID
            || previous.sequence.checked_add(1) != Some(sequence)
        {
            return Err(G17PBootstrapUmaLifecycleError::WrongOwner);
        }
        let overlapping = previous.state == G17PBootstrapUmaRequestState::Ready
            && previous.hardware_buffer_owned
            && previous.hardware_buffer_references >= 2
            && previous.pending_release_count >= 2;
        let reacquire = previous.state == G17PBootstrapUmaRequestState::Completed
            && !previous.hardware_buffer_owned
            && previous.hardware_buffer_references == 0
            && previous.pending_release_count == 0;
        if !overlapping && !reacquire {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        Ok(Self {
            sequence,
            hardware_buffer_id: previous.hardware_buffer_id,
            hardware_buffer_owned: true,
            hardware_buffer_references: previous
                .hardware_buffer_references
                .checked_add(3)
                .ok_or(G17PBootstrapUmaLifecycleError::WrongReferenceCount)?,
            pending_release_count: previous
                .pending_release_count
                .checked_add(2)
                .ok_or(G17PBootstrapUmaLifecycleError::WrongPendingCount)?,
            release_producer_after: None,
            request: None,
            state: G17PBootstrapUmaRequestState::Submitted,
        })
    }

    fn retire_prepare_reference(
        &mut self,
    ) -> core::result::Result<(), G17PBootstrapUmaLifecycleError> {
        if self.state != G17PBootstrapUmaRequestState::Submitted
            || !self.hardware_buffer_owned
            || self.hardware_buffer_references < if self.request.is_some() { 4 } else { 3 }
        {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        self.hardware_buffer_references -= 1;
        if self.request.is_none() {
            self.state = G17PBootstrapUmaRequestState::Ready;
        }
        Ok(())
    }

    fn match_completion(
        &mut self,
        completion: G17PBootstrapUmaCompletion,
    ) -> core::result::Result<(), G17PBootstrapUmaLifecycleError> {
        if self.state != G17PBootstrapUmaRequestState::Submitted {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        if Some(completion) != self.request {
            return Err(G17PBootstrapUmaLifecycleError::WrongCompletion);
        }
        if !self.hardware_buffer_owned
            || completion.hardware_buffer_id != self.hardware_buffer_id
        {
            return Err(G17PBootstrapUmaLifecycleError::WrongOwner);
        }
        if self.hardware_buffer_references != 3 {
            return Err(G17PBootstrapUmaLifecycleError::WrongReferenceCount);
        }
        if self.pending_release_count == 0 {
            return Err(G17PBootstrapUmaLifecycleError::WrongPendingCount);
        }

        self.hardware_buffer_references = 2;
        self.state = G17PBootstrapUmaRequestState::Ready;
        Ok(())
    }

    fn retire_render_references(
        &mut self,
    ) -> core::result::Result<Option<G17PBootstrapUmaRelease>, G17PBootstrapUmaLifecycleError> {
        if self.state != G17PBootstrapUmaRequestState::Ready
            || !self.hardware_buffer_owned
            || self.hardware_buffer_references < 2
        {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        if self.pending_release_count < 2 {
            return Err(G17PBootstrapUmaLifecycleError::WrongPendingCount);
        }
        self.hardware_buffer_references -= 2;
        self.pending_release_count -= 2;
        if self.hardware_buffer_references != 0 {
            return Ok(None);
        }
        self.hardware_buffer_owned = false;
        Ok(Some(G17PBootstrapUmaRelease {
            sequence: self.sequence,
            hardware_buffer_id: self.hardware_buffer_id,
        }))
    }

    fn pending_release(&self) -> Option<G17PBootstrapUmaRelease> {
        (self.state == G17PBootstrapUmaRequestState::Ready
            && self.hardware_buffer_references == 0)
        .then_some(
            G17PBootstrapUmaRelease {
                sequence: self.sequence,
                hardware_buffer_id: self.hardware_buffer_id,
            },
        )
    }

    fn mark_published(
        &mut self,
        release: G17PBootstrapUmaRelease,
    ) -> core::result::Result<(), G17PBootstrapUmaLifecycleError> {
        if self.pending_release() != Some(release) {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        self.state = G17PBootstrapUmaRequestState::Published;
        Ok(())
    }

    fn mark_sent(
        &mut self,
        producer_after: u32,
    ) -> core::result::Result<(), G17PBootstrapUmaLifecycleError> {
        if self.state != G17PBootstrapUmaRequestState::Published
            || self.release_producer_after.is_some()
        {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        self.release_producer_after = Some(producer_after);
        self.state = G17PBootstrapUmaRequestState::Sent;
        Ok(())
    }

    fn release_sent(&self) -> bool {
        self.state == G17PBootstrapUmaRequestState::Sent
            && self.release_producer_after.is_some()
    }

    fn mark_completed(
        &mut self,
        producer_after: u32,
    ) -> core::result::Result<(), G17PBootstrapUmaLifecycleError> {
        if !self.release_sent() || self.release_producer_after != Some(producer_after) {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        self.release_producer_after = None;
        self.state = G17PBootstrapUmaRequestState::Completed;
        Ok(())
    }

    fn observe_release_consumer(
        &mut self,
        consumer: u32,
    ) -> core::result::Result<bool, G17PBootstrapUmaLifecycleError> {
        let target = self
            .release_producer_after
            .ok_or(G17PBootstrapUmaLifecycleError::WrongPhase)?;
        if !self.release_sent() {
            return Err(G17PBootstrapUmaLifecycleError::WrongPhase);
        }
        if consumer != target {
            return Ok(false);
        }
        self.mark_completed(target)?;
        Ok(true)
    }
}

const G17P_GFX_LINK_DATA_BASE: u64 = 0xffff_fc00_0004_c000;
const G17P_GFX_LINK_DATA_END: u64 = 0xffff_fc00_0018_0000;
const G17P_GFX_PHYSICAL_DATA_BASE: u64 = 0x0000_0100_01d7_c000;

/// Translate a GFX firmware LINK address into the physical address the host
/// can map. Same arithmetic the render status-page probes use.
pub(crate) fn g17p_gfx_link_to_physical(link: u64) -> u64 {
    G17P_GFX_PHYSICAL_DATA_BASE + (link - G17P_GFX_LINK_DATA_BASE)
}
/// Firmware per-QID dependency records: `0x1261f0 + qid*0x18`, valid byte at
/// `+0`, completed stamp at `+8`. Firmware DATA segment, so host-readable.
const G17P_DEPENDENCY_RECORD_LINK_BASE: u64 = 0xffff_fc00_0012_61f0;
const G17P_GDIRECTOR_SHADOW_LINK_BASE: u64 = 0xffff_fc00_0012_f560;
/// Submit-time firmware DRAM the tag-15 handler maintains. These are the only
/// structures the KSM front-end plausibly scans that have never been diffed
/// between a queue that launches and one that does not.
/// Per-DM active-queue bitmap, `0x128a30 + dm*0x10` (+`0x08` is its sibling).
/// The scheduler pass ANDs against exactly these before launching.
const G17P_DM_ACTIVE_BITMAP_LINK_BASE: u64 = 0xffff_fc00_0012_8a30;
/// Per-queue state, `0x127628 + qid*0x28` (written at fw `0x119b4`).
const G17P_QUEUE_STATE_LINK_BASE: u64 = 0xffff_fc00_0012_7628;
/// KSM park record, `0x58010 + qid*0x28`: byte A at +0x418, B at +0x419,
/// C at +0x41a, deadline at +0x400. FW_PROVEN: the scheduler pass compares
/// A and B at 0x13100 and PARKS the queue when they are equal -- KTrace 0x42,
/// runnable bit cleared, then a status-4 NOP retire. A is written only by the
/// admit block at 0x12e38 (which also zeroes B and C); B only by the retire
/// path at 0xcf0c. Both start at zero, so a queue whose admit never ran is
/// parked by construction. Nothing has ever read these bytes from the host.
const G17P_KSM_PARK_RECORD_LINK_BASE: u64 = 0xffff_fc00_0005_8010;
/// Ready mask (fw `0x11b5c`).
const G17P_READY_MASK_LINK_BASE: u64 = 0xffff_fc00_0017_6ec0;
/// `0x175aa0 + qid*0x28 + 0x40` (fw `0x11b3c`).
const G17P_QUEUE_FLAG_LINK_BASE: u64 = 0xffff_fc00_0017_5aa0;
const G17P_DEPENDENCY_RECORD_STRIDE: u64 = 0x18;
const G17P_GFX1_LINK_DATA_BASE: u64 = 0xffff_fc00_0004_c000;
const G17P_GFX1_LINK_DATA_END: u64 = 0xffff_fc00_0017_8000;
const G17P_GFX1_PHYSICAL_DATA_BASE: u64 = 0x0000_0100_01eb_0000;
const G17P_SCHEDULER_ROOT_LINK_ADDRESS: u64 = 0xffff_fc00_0017_7190;
const G17P_SCHEDULER_ROOT_PHYSICAL_ADDRESS: u64 = 0x0000_0100_01ea_7190;
const G17P_SCHEDULER_ROOT_SNAPSHOT_SIZE: usize = 0x80;
const G17P_SCHEDULER_ROOT_PENDING_OFFSET: usize = 0x00;
const G17P_SCHEDULER_ROOT_DEFERRED_OFFSET: usize = 0x50;
const G17P_SCHEDULER_ROOT_STATE_OFFSET: usize = 0x68;
const G17P_RUNTIME_MAIN_GLOBAL_LINK_ADDRESS: u64 = 0xffff_fc00_0017_7030;
const G17P_RUNTIME_MAIN_GLOBAL_PHYSICAL_ADDRESS: u64 = 0x0000_0100_01ea_7030;
const G17P_TYPE4_EP_HANDLER_LINK_ADDRESS: u64 = 0xffff_fc00_0002_2e24;
const G17P_TYPE4_THUNK_LINK_ADDRESS: u64 = 0xffff_fc00_0002_635c;
const G17P_TYPE4_DRAIN_LINK_ADDRESS: u64 = 0xffff_fc00_0000_3a74;
const G17P_TYPE4_PENDING_MASK: u64 = 0x0002_0000;
const G17P_SCHEDULER_DEFERRED_REQUEUE_LINK_ADDRESS: u64 = 0xffff_fc00_0002_5ea4;
const G17P_TYPE4_MAIN_CONTROL_POINTER_OFFSETS: [usize; 3] = [0x1a0, 0x1a8, 0x1b0];
const G17P_MBI_GLOBAL_LINK_ADDRESS: u64 = 0xffff_fc00_0006_e840;
const G17P_MBI_STATIC_ARRAY_LINK_ADDRESS: u64 = 0xffff_fc00_0006_e800;
const G17P_MBI_GROUP_COUNT: u32 = 8;
const G17P_MBI_TRACE_CAPACITY: u32 = 128;
const G17P_MBI_TRACE_RECORD_SIZE: usize = 0x18;
// Primary A000 0x19184..0x191a0 initializes the adjacent endpoint handles;
// EP21 type 9 increments the lease at 0x22de4..0x22df8.
const G17P_PRIMARY_PEER_LEASE_LINK_ADDRESS: u64 = 0xffff_fc00_0017_7798;
const G17P_PRIMARY_PEER_LEASE_PHYSICAL_ADDRESS: u64 = 0x0000_0100_01ea_7798;
// Exact J700 GFX1 A000 (SHA-256 0caf8165...17fd63) initializes handles at
// 0x1bde0..0x1bdfc, increments this lease at 0x26738..0x26748, and decrements
// it only on ACTIVE->SLEEP at 0x25e70..0x25e7c. ACTIVE->NAP bypasses the write.
const G17P_GFX1_PEER_LEASE_LINK_ADDRESS: u64 = 0xffff_fc00_0016_d1c8;
const G17P_GFX1_PEER_LEASE_PHYSICAL_ADDRESS: u64 = 0x0000_0100_01fd_11c8;
const G17P_RTKIT_PRIVATE_ADDRESS_MASK: u64 = (1u64 << 42) - 1;

#[cfg(not(test))]
const _: () = {
    assert!(
        G17P_SCHEDULER_ROOT_PHYSICAL_ADDRESS
            == g17_adt_j700::J700_GFX_DATA.base
                + (G17P_SCHEDULER_ROOT_LINK_ADDRESS - G17P_GFX_LINK_DATA_BASE)
    );
    assert!(
        G17P_SCHEDULER_ROOT_PHYSICAL_ADDRESS + G17P_SCHEDULER_ROOT_SNAPSHOT_SIZE as u64
            <= g17_adt_j700::J700_GFX_DATA.base + g17_adt_j700::J700_GFX_DATA.size
    );
    assert!(G17P_SCHEDULER_ROOT_PHYSICAL_ADDRESS & 7 == 0);
    assert!(G17P_SCHEDULER_ROOT_SNAPSHOT_SIZE & 7 == 0);
    assert!(G17P_SCHEDULER_ROOT_STATE_OFFSET + 1 <= G17P_SCHEDULER_ROOT_SNAPSHOT_SIZE);
    assert!(G17P_GFX_LINK_DATA_END - G17P_GFX_LINK_DATA_BASE == 0x13_4000);
    assert!(G17P_GFX1_LINK_DATA_END - G17P_GFX1_LINK_DATA_BASE == 0x12_c000);
    assert!(G17P_GFX_PHYSICAL_DATA_BASE == g17_adt_j700::J700_GFX_DATA.base);
    assert!(G17P_GFX1_PHYSICAL_DATA_BASE == g17_adt_j700::J700_GFX1_DATA.base);
    assert!(
        G17P_RUNTIME_MAIN_GLOBAL_PHYSICAL_ADDRESS
            == g17_adt_j700::J700_GFX_DATA.base
                + (G17P_RUNTIME_MAIN_GLOBAL_LINK_ADDRESS - G17P_GFX_LINK_DATA_BASE)
    );
    assert!(
        G17P_RUNTIME_MAIN_GLOBAL_PHYSICAL_ADDRESS + core::mem::size_of::<u64>() as u64
            <= g17_adt_j700::J700_GFX_DATA.base + g17_adt_j700::J700_GFX_DATA.size
    );
    assert!(
        G17P_PRIMARY_PEER_LEASE_PHYSICAL_ADDRESS
            == G17P_GFX_PHYSICAL_DATA_BASE
                + (G17P_PRIMARY_PEER_LEASE_LINK_ADDRESS - G17P_GFX_LINK_DATA_BASE)
    );
    assert!(
        G17P_GFX1_PEER_LEASE_PHYSICAL_ADDRESS
            == G17P_GFX1_PHYSICAL_DATA_BASE
                + (G17P_GFX1_PEER_LEASE_LINK_ADDRESS - G17P_GFX1_LINK_DATA_BASE)
    );
};

/// Grounded manager inputs for T8140. Power and tuning values are excluded.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140ManagerConfig {
    pub(crate) chip_id: u32,
    pub(crate) base_clock_hz: u32,
    pub(crate) hw_family: u8,
    pub(crate) hw_variant: u8,
    pub(crate) num_dies: u32,
    pub(crate) uat_ias: u8,
    pub(crate) uat_oas: u32,
    pub(crate) uat_page_size: u16,
    pub(crate) uat_page_bits: u8,
    pub(crate) uat_levels: [(u8, u16); g17_initdata::UAT_LEVEL_COUNT],
    pub(crate) initdata_version: [u16; 4],
    pub(crate) root_sizes: [usize; 2],
    pub(crate) secondary_root_delta: u64,
    pub(crate) secondary_shared_region_delta: u64,
    pub(crate) shared_cluster_size: usize,
    pub(crate) work_channel_count: usize,
    pub(crate) submission_backend: identity::SubmissionBackendSelection,
}

/// Exact facts needed to construct, but not execute, a T8140 manager.
pub(crate) const T8140_MANAGER_CONFIG: T8140ManagerConfig = T8140ManagerConfig {
    chip_id: 0x8140,
    base_clock_hz: 24_000_000,
    hw_family: 0x0a,
    hw_variant: 0,
    num_dies: 1,
    uat_ias: 42,
    uat_oas: 42,
    uat_page_size: g17_initdata::UAT_PAGE_SIZE,
    uat_page_bits: g17_initdata::UAT_PAGE_BITS,
    uat_levels: g17_initdata::UAT_LEVELS,
    initdata_version: g17_initdata::INITDATA_VERSION,
    root_sizes: [
        g17_initdata::ROOT_SIZE_PRIMARY,
        g17_initdata::ROOT_SIZE_SECONDARY,
    ],
    secondary_root_delta: g17_initdata::SECONDARY_ROOT_DELTA,
    secondary_shared_region_delta: g17_initdata::SECONDARY_SHARED_REGION_DELTA,
    shared_cluster_size: g17_initdata::NATIVE_SHARED_CLUSTER_SIZE,
    work_channel_count: g17_initdata::WORK_CHANNEL_COUNT,
    submission_backend: identity::T8140_G17P_SUBMISSION_BACKEND,
};

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchGeometry {
    pub(crate) write0_base: u32,
    pub(crate) write1_base: u32,
    pub(crate) stride: u32,
}

/// The two offsets used by the fixed G17P scratch FIFO slot.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchOffsets {
    pub(crate) write0: u32,
    pub(crate) write1: u32,
}

impl G17SksmScratchGeometry {
    /// Apply the exact host equation for fixed FIFO slot 9.
    ///
    /// The slot is independent of the SKSM queue ID carried in each write.
    pub(crate) const fn fifo_offsets(
        self,
    ) -> core::result::Result<G17SksmScratchOffsets, ConstructionError> {
        const FIFO_SLOT: u32 = 9;

        let fifo_offset = match self.stride.checked_mul(FIFO_SLOT) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write0 = match self.write0_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write1 = match self.write1_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };

        Ok(G17SksmScratchOffsets { write0, write1 })
    }
}

/// Target configuration supplied by platform discovery.
///
/// There is no default T8140 scratch geometry. A caller must recover the
/// three values through the platform configuration path before construction.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PPlatformConfig {
    pub(crate) sksm_scratch: G17SksmScratchGeometry,
    pub(crate) sksm_queue_geometry: g17_submission::G17SksmQueueGeometry,
}

pub(crate) const T8140_G17P_PLATFORM_CONFIG: G17PPlatformConfig = G17PPlatformConfig {
    sksm_scratch: G17SksmScratchGeometry {
        write0_base: 0x4000,
        write1_base: 0x4008,
        stride: 0x10,
    },
    sksm_queue_geometry: g17_submission::T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
};

/// SoC and register identity presented to the manager constructor.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct TargetObservation {
    pub(crate) chip_id: u32,
    pub(crate) declared_gen: GpuGen,
    pub(crate) declared_variant: GpuVariant,
    pub(crate) hw_family: u8,
    pub(crate) hw_variant: u8,
    pub(crate) num_dies: u32,
    pub(crate) uat_ias: u8,
    pub(crate) uat_oas: u32,
}

/// Ownership and placement facts copied from the retained resource owner.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ResourceObservation {
    pub(crate) uat_ttb_base: u64,
    pub(crate) primary_uat_owner: u64,
    pub(crate) secondary_uat_owner: u64,
    pub(crate) primary_role: InstanceRole,
    pub(crate) secondary_role: InstanceRole,
    pub(crate) primary_root: u64,
    pub(crate) secondary_root: u64,
    pub(crate) shared_cluster_size: usize,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ConstructionError {
    WrongChip,
    WrongDeclaredIdentity,
    WrongHardwareIdentity,
    WrongUatGeometry,
    UndecodableIdentity,
    WrongDecodedIdentity,
    WrongSubmissionBackend,
    NullUatOwner,
    SplitUatOwner,
    WrongRoleTopology,
    WrongRootPlacement,
    WrongSharedClusterSize,
    WrongSksmScratchGeometry,
    WrongSksmQueueGeometry,
    SksmScratchOffsetOverflow,
}

/// Validate the exact T8140 identity without consulting an AGX2 `HwConfig`.
pub(crate) fn validate_target(
    target: TargetObservation,
) -> core::result::Result<(), ConstructionError> {
    let config = &T8140_MANAGER_CONFIG;

    if target.chip_id != config.chip_id {
        return Err(ConstructionError::WrongChip);
    }
    if target.declared_gen != GpuGen::G17 || target.declared_variant != GpuVariant::P {
        return Err(ConstructionError::WrongDeclaredIdentity);
    }
    if target.hw_family != config.hw_family
        || target.hw_variant != config.hw_variant
        || target.num_dies != config.num_dies
    {
        return Err(ConstructionError::WrongHardwareIdentity);
    }
    if target.uat_ias != config.uat_ias || target.uat_oas != config.uat_oas {
        return Err(ConstructionError::WrongUatGeometry);
    }

    let num_dies =
        u8::try_from(target.num_dies).map_err(|_| ConstructionError::UndecodableIdentity)?;
    let decoded = identity::decode_gpu_identity(target.hw_family, target.hw_variant, num_dies)
        .ok_or(ConstructionError::UndecodableIdentity)?;
    if decoded.gpu_gen != GpuGen::G17
        || decoded.gpu_variant != GpuVariant::P
        || decoded.usc_generation != 3
        || decoded.gpu_hal_generation != GpuHalGeneration::Hal200
        || decoded.firmware_roles != FirmwareRoleTopology::Dual
        || decoded.submission_transport != SubmissionTransport::ClassicAndSksm
        || decoded.uat_input_address_bits != config.uat_ias
    {
        return Err(ConstructionError::WrongDecodedIdentity);
    }
    let submission_backend = identity::require_executable_submission_backend(decoded)
        .map_err(|_| ConstructionError::WrongSubmissionBackend)?;
    if submission_backend != config.submission_backend {
        return Err(ConstructionError::WrongSubmissionBackend);
    }

    Ok(())
}

pub(crate) fn validate_resources(
    resources: ResourceObservation,
) -> core::result::Result<(), ConstructionError> {
    if resources.uat_ttb_base == 0
        || resources.primary_uat_owner == 0
        || resources.secondary_uat_owner == 0
    {
        return Err(ConstructionError::NullUatOwner);
    }
    if resources.uat_ttb_base != resources.primary_uat_owner
        || resources.uat_ttb_base != resources.secondary_uat_owner
    {
        return Err(ConstructionError::SplitUatOwner);
    }
    if resources.primary_role != InstanceRole::Primary
        || resources.secondary_role != InstanceRole::Secondary
    {
        return Err(ConstructionError::WrongRoleTopology);
    }
    let secondary_root = resources
        .primary_root
        .checked_add(T8140_MANAGER_CONFIG.secondary_root_delta)
        .ok_or(ConstructionError::WrongRootPlacement)?;
    if resources.secondary_root != secondary_root {
        return Err(ConstructionError::WrongRootPlacement);
    }
    if resources.shared_cluster_size != T8140_MANAGER_CONFIG.shared_cluster_size {
        return Err(ConstructionError::WrongSharedClusterSize);
    }

    Ok(())
}

/// Admit only the exact geometry produced by the pinned G17P host.
pub(crate) fn validate_platform(
    platform: G17PPlatformConfig,
) -> core::result::Result<(), ConstructionError> {
    if platform.sksm_scratch != T8140_G17P_PLATFORM_CONFIG.sksm_scratch {
        return Err(ConstructionError::WrongSksmScratchGeometry);
    }
    if platform.sksm_queue_geometry
        != T8140_G17P_PLATFORM_CONFIG.sksm_queue_geometry
    {
        return Err(ConstructionError::WrongSksmQueueGeometry);
    }
    platform.sksm_scratch.fifo_offsets()?;
    platform
        .sksm_queue_geometry
        .validate()
        .map_err(|_| ConstructionError::WrongSksmQueueGeometry)?;
    Ok(())
}

#[cfg(not(test))]
fn target_observation(soc: &'static agx3::SocConfig) -> TargetObservation {
    TargetObservation {
        chip_id: soc.chip_id,
        declared_gen: soc.gpu_gen,
        declared_variant: soc.gpu_variant,
        hw_family: soc.hw_family,
        hw_variant: soc.hw_variant,
        num_dies: soc.num_dies,
        uat_ias: soc.uat_ias,
        uat_oas: soc.uat_oas,
    }
}

#[cfg(not(test))]
fn resource_observation(
    handoff: &g17_resources::G17ResourceHandoff,
    uat: &mmu::Uat,
) -> ResourceObservation {
    ResourceObservation {
        uat_ttb_base: uat.ttb_base(),
        primary_uat_owner: handoff.primary.uat_owner,
        secondary_uat_owner: handoff.secondary.uat_owner,
        primary_role: handoff.primary.role,
        secondary_role: handoff.secondary.role,
        primary_root: handoff.primary.instance.root,
        secondary_root: handoff.secondary.instance.root,
        shared_cluster_size: handoff.shared.hw_data_bundle_alloc,
    }
}

/// Retained construction state for both T8140 firmware roles.
///
/// Creating this object allocates and validates resources. It performs no
/// firmware publication, ASC start, MMIO write, or submission.
#[cfg(not(test))]
pub(crate) struct G17PManagerConstruction {
    resources: g17_resources::G17ResourceOwner,
    /// One bounded first-render archive. Allocated before initdata publication;
    /// exported by ownership transfer only after a terminal observation.
    #[cfg(CONFIG_DEV_COREDUMP)]
    trace_capture: Option<KBox<G17PTraceDump>>,
    platform: G17PPlatformConfig,
    num_clusters: u32,
    /// Cached SGX 0xe01480 bits 19:16 from the pre-ASC GPU-ID probe.
    gpc_perf_state_map: u32,
    /// Cached SGX 0xe01480 bits 3:0 from the same probe.
    gpc_perf_state_map_low: u32,
    /// Cached SGX 0xe0141c bit 0 from the same probe.
    gpc_perf_state_control: u32,
    render_storage: Option<g17_resources::G17PUserRenderStorage>,
    render_tracker: Option<G17PPartialOpeningRenderTracker>,
    render_bind: Option<G17PRenderRootOwner>,
    render_context_aliases: Option<mmu::T8140NativeContextAliases>,
    render_submission_ordinal: u32,
    render_auxiliary_timestamp: u64,
    render_hardware_buffer_id: Option<u32>,
    /// Set immediately after the outer command pair becomes firmware-visible.
    /// A pre-publication construction failure can return its ID immediately;
    /// a post-publication failure must retain it until the CPUs stop.
    render_hardware_buffer_published: bool,
    render_hardware_buffer_ids: G17PHardwareBufferIdAllocator,
    /// The SKSM half of the render path: one KSM kick queue per render data
    /// master. Present only while `g17p_render_sksm` is armed.
    render_sksm: Option<G17PRenderSksmQueues>,
    bootstrap_uma: Option<G17PBootstrapUmaLifecycle>,
    prepared_render_flist: Option<G17PBootstrapUmaLifecycle>,
    /// Device-global FList generation. Queue-pair ordinals are per slot, but
    /// the shared USC pool accepts one monotonic owner sequence.
    next_render_flist_sequence: u64,
    /// The manager's render methods operate on one selected state to keep the
    /// byte-critical publication code single-copy.  The other bounded slot is
    /// parked here while its independent queue pair executes.
    parked_render_slots: [Option<G17PRenderSlotState>; 2],
    active_render_slot: u8,
    accepted_firmware_recovery_generation: u64,
    pending_primary_recovery: Option<(u64, u32)>,
    /// Tag-14 AddKicks held back until the firmware recovery/power-up closes.
    pending_add_kicks: Option<[u8; g17_submission::G17P_KSM_ADD_KICKS_COMMAND_SIZE]>,
    /// Descriptor address published by the most recent compute submission.
    /// The KSM completion record echoes it at +0x30, so it identifies which
    /// submission a populated record belongs to.
    last_published_descriptor: u64,
    next_compute_queue_id: Option<u8>,
    /// End timestamp of the last completion already handed to a waiter. The
    /// KSM record is a single overwritten slot, so without this the record
    /// left behind by submission N is indistinguishable from submission N+1's
    /// and every later wait would be satisfied instantly by stale data.
    last_completion_end: u64,
    /// Set once a submission has zeroed the queue's completion stamp, so a
    /// retire knows the word is meaningful. Cleared only by rebuilding the
    /// manager.
    compute_stamp_armed: bool,
    /// How many retires have actually had to wait for the completion stamp.
    /// Counted rather than logged per submission, because a per-submission
    /// console write costs about a millisecond on a dockchannel console and
    /// would pad the very timing this gate exists to measure.
    compute_stamp_waits: AtomicU32,
    /// How many retires the completion stamp never arrived for. Hardware says
    /// this is a real minority case (19 of 300), not a modelling error, so the
    /// warning is rate-limited rather than removed: a per-submission console
    /// write costs about a millisecond and would itself distort the timing.
    compute_stamp_misses: AtomicU32,
}

#[cfg(not(test))]
#[derive(Default)]
struct G17PRenderSlotState {
    #[cfg(CONFIG_DEV_COREDUMP)]
    trace_capture: Option<KBox<G17PTraceDump>>,
    storage: Option<g17_resources::G17PUserRenderStorage>,
    tracker: Option<G17PPartialOpeningRenderTracker>,
    bind: Option<G17PRenderRootOwner>,
    context_aliases: Option<mmu::T8140NativeContextAliases>,
    submission_ordinal: u32,
    auxiliary_timestamp: u64,
    hardware_buffer_id: Option<u32>,
    hardware_buffer_published: bool,
    sksm: Option<G17PRenderSksmQueues>,
}

#[cfg(not(test))]
enum G17PRenderRootOwner {
    Bootstrap(mmu::VmBind),
    Job {
        context: Arc<mmu::T8140ComputeExecutionContext>,
        flist_bind: mmu::VmBind,
    },
}

#[cfg(not(test))]
impl G17PRenderRootOwner {
    fn matches(&self, vm: &mmu::Vm) -> bool {
        match self {
            Self::Bootstrap(bind) => bind.matches(vm),
            Self::Job { context, flist_bind } => {
                context.matches(vm) && flist_bind.matches(vm)
            }
        }
    }

    fn root(&self) -> u64 {
        match self {
            Self::Bootstrap(bind) => bind.root(),
            Self::Job { context, flist_bind } => {
                debug_assert_eq!(context.root(), flist_bind.root());
                flist_bind.root()
            }
        }
    }

    fn vm_id(&self) -> u64 {
        match self {
            Self::Bootstrap(bind) => bind.vm_id(),
            Self::Job { context, flist_bind } => {
                debug_assert_eq!(context.vm_id(), flist_bind.vm_id());
                flist_bind.vm_id()
            }
        }
    }

    fn vm(&self) -> &mmu::Vm {
        match self {
            Self::Bootstrap(bind) => bind.vm(),
            Self::Job { context, flist_bind } => {
                debug_assert!(context.matches(flist_bind.vm()));
                flist_bind.vm()
            }
        }
    }

    fn bootstrap(&self) -> Option<&mmu::VmBind> {
        match self { Self::Bootstrap(bind) => Some(bind), Self::Job { .. } => None }
    }
}

#[cfg(not(test))]
impl G17PRenderSlotState {
    fn scheduler_active(&self) -> bool {
        self.storage.is_some()
            || self.tracker.is_some()
            || self.bind.is_some()
            || self.submission_ordinal != 0
    }
}

#[cfg(all(not(test), CONFIG_DEV_COREDUMP))]
pub(crate) struct G17PTraceDump {
    arena: KVVec<u8>,
    used: usize,
    phase_sequence: u64,
    prepared: bool,
    job_stamp: u64,
}

#[cfg(all(not(test), CONFIG_DEV_COREDUMP))]
impl G17PTraceDump {
    fn new(resources: &mut g17_resources::G17ResourceOwner) -> Result<KBox<Self>> {
        let mut arena = KVVec::from_elem(0, trace::DEFAULT_CAPACITY, GFP_KERNEL)?;
        let mut archive = TraceArchive::new(&mut arena).map_err(|_| EINVAL)?;
        if resources.capture_owned_trace(
            &mut archive, trace::Phase::BootConstructed, 1,
            &mut || Monotonic::ktime_get() as u64,
        ).is_err() {
            archive.omit(
                trace::Meta::new(trace::Kind::CaptureBoundary, trace::Phase::BootConstructed, 1),
                trace::Omission::ReadError,
            ).map_err(|_| EINVAL)?;
        }
        Ok(KBox::new(Self {
            arena, used: 0, phase_sequence: 1, prepared: false, job_stamp: 0,
        }, GFP_KERNEL)?)
    }

    pub(crate) fn len(&self) -> usize { self.used }
}

#[cfg(all(not(test), CONFIG_DEV_COREDUMP))]
impl kernel::devcoredump::DevCoreDump for G17PTraceDump {
    fn read(&self, output: &mut [u8], offset: usize) -> Result<usize> {
        // No caller is allowed to expose an in-progress archive.
        if self.used == 0 { return Err(EINVAL); }
        trace::read_sealed_prefix(&self.arena[..self.used], output, offset).map_err(|_| EINVAL)
    }
}

/// One record a steady-state submission publishes into the item ring.
enum G17PRepeatRecord<'a> {
    Pointer(u64),
    AddKicks(&'a [u8; g17_submission::G17P_KSM_ADD_KICKS_COMMAND_SIZE]),
    EntrySignal(&'a [u8; g17_submission::G17P_COMPUTE_OPTIONAL_EVENT_SIZE]),
}

/// Records one steady-state submission appends to the queue's item ring:
/// the tag-3 command pointer, tag 15, tag 14, tag 16.
const G17P_COMPUTE_REPEAT_RING_RECORDS: u32 = 4;
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum G17PSksmQueueHardwareState {
    PreparedMemory,
    HardwareRegistered,
}

#[cfg(not(test))]
struct G17PRenderSksmQueue {
    queue_id: u8,
    /// True for the TA/tiling queue, false for the 3D/fragment queue. Selects
    /// which bootstrap context peer backs the entry ring.
    tiling: bool,
    configure_pair: g17_submission::G17SksmOrderedWritePair,
    enable_pair: g17_submission::G17SksmOrderedWritePair,
    producer: g17_submission::G17PClKickProducerState,
    registered: bool,
}

/// The render pair, TA first. Held next to `render_storage` and released with
/// it, since the entry rings live in resources the storage object aliases.
#[cfg(not(test))]
struct G17PRenderSksmQueues {
    scratch: G17SksmScratchGeometry,
    uat_owner: u64,
    tiling: G17PRenderSksmQueue,
    fragment: G17PRenderSksmQueue,
}

/// Session-wide counters for the four decisive state-machine events.
///
/// Printed on EVERY `G17PMARK` line. The kernel log ring wraps easily on a
/// verbose boot -- especially with `g17p_fw_trace=1` -- and the two rarest
/// markers (`tag15-install`, fired once at the very first submit of a boot)
/// are exactly the ones that get evicted first. Carrying the counts on every
/// marker line means any single surviving line reconstructs the whole state
/// machine: how many QIDs were installed, how many zero-duration retires were
/// caught, how many sessions were rebuilt, how many handoffs retained.
pub(crate) static G17P_MARK_TAG15_INSTALLS: AtomicU32 = AtomicU32::new(0);
pub(crate) static G17P_MARK_ZERO_DURATION: AtomicU32 = AtomicU32::new(0);
pub(crate) static G17P_MARK_GATE_REBUILDS: AtomicU32 = AtomicU32::new(0);
pub(crate) static G17P_MARK_RETAINED_HANDOFFS: AtomicU32 = AtomicU32::new(0);

/// Compact `i/z/r/h` census appended to every marker line.
pub(crate) fn g17p_mark_counts() -> (u32, u32, u32, u32) {
    (
        G17P_MARK_TAG15_INSTALLS.load(Ordering::Relaxed),
        G17P_MARK_ZERO_DURATION.load(Ordering::Relaxed),
        G17P_MARK_GATE_REBUILDS.load(Ordering::Relaxed),
        G17P_MARK_RETAINED_HANDOFFS.load(Ordering::Relaxed),
    )
}

pub(crate) const G17P_COMPUTE_QUEUE_ID_BASE: u8 = 4;
/// Last QID the allocator may hand out. `KICK_QID_MASK` is 127 and the
/// firmware's KSM restore set has 128 slots. IDs 0..3 are reserved, 4 is the
/// first compute queue, 5/6 are the retained render pair, and rebuilt compute
/// graphs consume 7..127 exactly once. Exhaustion returns ENOSPC rather than
/// reprogramming a queue still installed in the firmware.
pub(crate) const G17P_COMPUTE_QUEUE_ID_LAST: u8 = 127;
const _: () = assert!(G17P_COMPUTE_QUEUE_ID_BASE > 1);
const _: () = assert!(G17P_COMPUTE_QUEUE_ID_LAST <= 127);

#[cfg(not(test))]
pub(crate) struct G17PSksmQueue {
    queue_id: u8,
    scratch: G17SksmScratchGeometry,
    uat_owner: u64,
    configure_pair: g17_submission::G17SksmOrderedWritePair,
    enable_pair: g17_submission::G17SksmOrderedWritePair,
    disable_pair: g17_submission::G17SksmOrderedWritePair,
    hardware_state: G17PSksmQueueHardwareState,
    storage: g17_resources::G17PClRuntimeStorage,
    compute: g17_resources::G17PComputeDescriptorStorage,
    producer: g17_submission::G17PClKickProducerState,
    direct_lifecycle: g17_submission::G17PDirectClLifecycleState,
    configure_guard_accelerator_stamp: u32,
    configure_guard_event_stamp: u32,
    submission_ordinal: u32,
    b2_activation: g17_submission::G17PClB2ActivationState,
    independent_context: bool,
    completion_descriptor: u64,
    completion_last_end: u64,
    completion_stamp_armed: bool,
}

impl G17PSksmQueue {
    /// The hardware compute QID this graph is published as.
    ///
    /// Needed outside this module so a client handoff can name the QID whose
    /// firmware-side progress record it has to restore.
    pub(crate) fn queue_id(&self) -> u8 {
        self.queue_id
    }
    pub(crate) fn requires_first_install(&self) -> bool {
        self.hardware_state == G17PSksmQueueHardwareState::PreparedMemory
            && self.submission_ordinal == 0
    }

    /// `(submission ordinal, next kick stamp, item-ring producer)`.
    ///
    /// Sampled when the graph is handed from one DRM client to the next, so a
    /// log line proves the retained queue really did carry its stamp sequence
    /// and ring cursor across the handoff instead of restarting them. Every
    /// field is a plain read of an object the driver allocated -- no sgx MMIO,
    /// so it is safe with the GPU cores gated.
    pub(crate) fn handoff_snapshot(&mut self) -> (u32, u64, u32) {
        let ring = self.compute.compute_ring_producer().unwrap_or(u32::MAX);
        (self.submission_ordinal, self.producer.current_timestamp, ring)
    }
}

/// Command-owned fields used to build one compute descriptor and CL entry.
/// The queue supplies descriptor storage, sequencing, preemption storage, and
/// the retained support graph.
#[cfg(not(test))]
pub(crate) struct G17PComputeSubmissionContext<'a> {
    pub(crate) context_id: u32,
    pub(crate) dispatch_identity: u64,
    pub(crate) execution_gate: u64,
    pub(crate) timestamps: g17_uapi::ComputeTimestampAddresses,
    pub(crate) descriptor_flag_4c: bool,
    pub(crate) descriptor_flag_5c8: bool,
    pub(crate) converted_command_timestamp: u64,
    pub(crate) barriers: &'a [g17_submission::G17PClBarrierDependency],
    pub(crate) mcache: Option<g17_submission::G17PClMcacheAperture>,
    pub(crate) rce_kind: u8,
    pub(crate) auxiliary: u64,
}

/// User-VM aliases and its active TTBAT binding. The live runtime retains this
/// object until the entry completes or both processors stop during recovery.
#[cfg(not(test))]
pub(crate) struct G17PComputeUserBinding {
    mappings: g17_resources::G17PComputeUserMappings,
    _sksm_entries: mmu::KernelMapping,
    _vm_bind: Option<mmu::VmBind>,
    _execution_context: Option<Arc<mmu::T8140ComputeExecutionContext>>,
}

#[cfg(not(test))]
impl G17PComputeUserBinding {
    /// Whether this retained binding already covers `vm`.
    ///
    /// A repeat submission reuses the binding rather than mapping the same
    /// fixed user VAs a second time, which would fail; a different VM has to
    /// go through a fresh bind.
    pub(crate) fn matches(&self, vm: &mmu::Vm) -> bool {
        self._vm_bind.as_ref().is_some_and(|bind| bind.matches(vm))
            || self._execution_context.as_ref().is_some_and(|context| context.matches(vm))
    }

    /// The UAT binding this client's submissions run against.
    ///
    /// Needed so every kick -- not just the one that created this binding --
    /// can re-prove that hardware contexts 2/3 still name this VM's root.
    pub(crate) fn vm_bind(&self) -> &mmu::VmBind {
        self._vm_bind.as_ref().expect("legacy binding required")
    }
}

/// An outgoing client's compute binding, kept alive after its submission was
/// abandoned, with only the UAT slot given back.
///
/// A submission that was published and never seen to retire may still be held
/// by the firmware, and that kick names this client's page tables and the GEM
/// objects mapped into them. Dropping the binding outright would unmap and then
/// free exactly that memory. Every `mmu::KernelMapping` here owns an
/// `Arc<VmInner>`, so retaining them retains the page tables, the gpuvm and its
/// GEM references -- nothing the firmware could still dereference is released.
///
/// The `mmu::VmBind` is deliberately NOT retained. Slot release is refcounted
/// on the `VmBind` alone (`active_users` reaching zero clears `binding.binding`),
/// independently of the `VmInner` lifetime, so dropping it hands UAT slot 1
/// straight back to the next client while the memory stays parked.
///
/// Released only once both firmware processors have stopped, in
/// `release_runtime_objects` -- the same discipline `retired_queues` uses.
#[cfg(not(test))]
pub(crate) struct G17PQuarantinedComputeBinding {
    _mappings: g17_resources::G17PComputeUserMappings,
    _sksm_entries: mmu::KernelMapping,
    _execution_context: Option<Arc<mmu::T8140ComputeExecutionContext>>,
}

#[cfg(not(test))]
impl G17PComputeUserBinding {
    /// Give the UAT slot back and park everything else.
    pub(crate) fn quarantine(self) -> G17PQuarantinedComputeBinding {
        let Self {
            mappings,
            _sksm_entries,
            _vm_bind,
            _execution_context,
        } = self;
        // Explicit and load-bearing: this is what frees UAT slot 1.
        drop(_vm_bind);
        G17PQuarantinedComputeBinding {
            _mappings: mappings,
            _sksm_entries,
            _execution_context,
        }
    }
}

#[cfg(not(test))]
pub(crate) struct G17PComputeTimeoutSnapshot {
    pub(crate) graph: g17_resources::G17PComputeGraphSnapshot,
    pub(crate) channel: g17_resources::G17PComputeChannelSnapshot,
    pub(crate) channel_scan:
        [g17_resources::G17PChannelScanSnapshot; g17_resources::G17P_DATA_MASTER_CHANNEL_COUNT],
    pub(crate) channel_control_prefix: [u8; 0x38],
    pub(crate) control: g17_resources::G17PControlCounters,
}

/// The two complete queue groups exposed by the exact first graphics publish.
#[cfg(not(test))]
pub(crate) struct G17PPartialOpeningRenderSubmission {
    pub(crate) fragment_queue_gpu_va: u64,
    pub(crate) fragment_queue_write_index: u32,
    pub(crate) tiling_queue_gpu_va: u64,
    pub(crate) tiling_queue_write_index: u32,
}

/// Completion cursors retained across observations of one graphics pair.
#[cfg(not(test))]
pub(crate) struct G17PPartialOpeningRenderTracker {
    tiling_previous: g17_completion::QueueIndices,
    fragment_previous: g17_completion::QueueIndices,
    tiling_prefix: u32,
    fragment_prefix: u32,
}

#[cfg(not(test))]

/// DIAGNOSTIC CONTROL, compute path only. When 0, the compute submit publishes
/// everything it normally does -- tag 3/15/14/16, the classic item-ring CL_2
/// command, the doorbell, the producer advance -- but never writes the SKSM
/// kick entry body. It answers one question about the CONTROL, not the failing
/// path: is compute actually scheduled through the SKSM entry ring, or through
/// the classic item ring with its SKSM entry vestigial?
///
/// This matters because "compute completes with `g17p_sksm_mmio=0`" was used to
/// dismiss the suppressed SKSM queue-enable as a render blocker. That argument
/// only carries if compute's completion depends on SKSM scheduling. If compute
/// still passes 64/64 with this at 0, it does not, and the enable becomes a live
/// candidate again -- render has no classic path to fall back on.
pub(crate) fn g17p_compute_sksm_entry_enabled() -> bool {
    *crate::module_parameters::g17p_compute_sksm_entry.value() != 0
}


/// Result of one render completion poll.
#[derive(Debug)]
pub(crate) enum G17PRenderPollOutcome {
    /// Both halves retired; the render is done.
    Complete(g17_completion::PairedRenderCompletion),
    /// A completed job list was quiesced; re-poll immediately, do not sleep.
    Retry,
    /// Nothing yet; sleep before polling again.
    Pending,
}

impl G17PManagerConstruction {
    fn take_active_render_slot_state(&mut self) -> G17PRenderSlotState {
        G17PRenderSlotState {
            #[cfg(CONFIG_DEV_COREDUMP)]
            trace_capture: self.trace_capture.take(),
            storage: self.render_storage.take(),
            tracker: self.render_tracker.take(),
            bind: self.render_bind.take(),
            context_aliases: self.render_context_aliases.take(),
            submission_ordinal: core::mem::take(&mut self.render_submission_ordinal),
            auxiliary_timestamp: core::mem::take(&mut self.render_auxiliary_timestamp),
            hardware_buffer_id: self.render_hardware_buffer_id.take(),
            hardware_buffer_published: core::mem::take(&mut self.render_hardware_buffer_published),
            sksm: self.render_sksm.take(),
        }
    }

    fn install_active_render_slot_state(&mut self, state: G17PRenderSlotState) {
        #[cfg(CONFIG_DEV_COREDUMP)]
        {
            self.trace_capture = state.trace_capture;
        }
        self.render_storage = state.storage;
        self.render_tracker = state.tracker;
        self.render_bind = state.bind;
        self.render_context_aliases = state.context_aliases;
        self.render_submission_ordinal = state.submission_ordinal;
        self.render_auxiliary_timestamp = state.auxiliary_timestamp;
        self.render_hardware_buffer_id = state.hardware_buffer_id;
        self.render_hardware_buffer_published = state.hardware_buffer_published;
        self.render_sksm = state.sksm;
    }

    /// Select one of the two bounded render owners without altering any
    /// firmware-visible bytes.  All state reachable by a submitted TA/3D pair
    /// moves together; the global outer rings and HardwareBufferID allocator
    /// remain shared and are still published under the runtime mutex.
    pub(crate) fn select_render_slot(&mut self, slot: u8) -> Result {
        if slot >= g17_resources::G17PRenderQueuePair::SLOT_COUNT {
            return Err(EINVAL);
        }
        if self.active_render_slot == slot {
            return Ok(());
        }
        let current_slot = self.active_render_slot as usize;
        if self.parked_render_slots[current_slot].is_some() {
            return Err(EIO);
        }
        let current = self.take_active_render_slot_state();
        self.parked_render_slots[current_slot] = Some(current);
        let incoming = self.parked_render_slots[slot as usize]
            .take()
            .unwrap_or_default();
        self.install_active_render_slot_state(incoming);
        self.active_render_slot = slot;
        Ok(())
    }

    fn parked_render_scheduler_active(&self) -> bool {
        self.parked_render_slots
            .iter()
            .flatten()
            .any(G17PRenderSlotState::scheduler_active)
    }

    pub(crate) fn new(
        dev: &AsahiDevice,
        soc: &'static agx3::SocConfig,
        platform: G17PPlatformConfig,
        num_clusters: u32,
        gpc_perf_state_map: u32,
        gpc_perf_state_map_low: u32,
        gpc_perf_state_control: u32,
    ) -> Result<KBox<Self>> {
        let uat = KBox::new(mmu::Uat::new_t8140(dev, true)?, GFP_KERNEL)?;
        Self::new_with_uat(
            dev,
            soc,
            platform,
            num_clusters,
            gpc_perf_state_map,
            gpc_perf_state_map_low,
            gpc_perf_state_control,
            uat,
        )
    }

    /// Kept out of line and heap-returned: the construction locals plus the
    /// assembled manager otherwise land several times in the probe-path stack
    /// frames, which overflowed the 32 KiB task stack on hardware.
    #[inline(never)]
    pub(crate) fn new_with_uat(
        dev: &AsahiDevice,
        soc: &'static agx3::SocConfig,
        platform: G17PPlatformConfig,
        num_clusters: u32,
        gpc_perf_state_map: u32,
        gpc_perf_state_map_low: u32,
        gpc_perf_state_control: u32,
        uat: KBox<mmu::Uat>,
    ) -> Result<KBox<Self>> {
        if num_clusters == 0
            || gpc_perf_state_map > 0xf
            || gpc_perf_state_map_low > 0xf
            || gpc_perf_state_control > 1
        {
            return Err(EINVAL);
        }
        validate_target(target_observation(soc)).map_err(|_| EINVAL)?;
        validate_platform(platform).map_err(|_| EINVAL)?;

        let mut resources = g17_resources::G17ResourceOwner::new_with_uat(dev, uat)?;
        g17_resources::validate_handoff(resources.handoff()).map_err(|_| EINVAL)?;
        validate_resources(resource_observation(resources.handoff(), resources.uat()))
            .map_err(|_| EINVAL)?;

        #[cfg(CONFIG_DEV_COREDUMP)]
        let trace_capture = match G17PTraceDump::new(&mut resources) {
            Ok(capture) => Some(capture),
            Err(error) => {
                dev_warn!(dev.as_ref(), "G17P binary capture unavailable: {:?}\n", error);
                None
            }
        };

        // Keep the complete manager value off the probe task's stack.  The
        // two-slot owner contains three render-state images (active plus two
        // parked slots); constructing `Self` by value made rustc reserve
        // 0x3a40 bytes in this function, on top of boot_initial_session's
        // 0x890-byte caller frame.  That exceeds arm64's 16 KiB kernel stack
        // before RTKit/initdata can return from module probe.  An in-place
        // initializer writes every field directly into the final allocation.
        KBox::init(
            try_init!(Self {
                resources,
                #[cfg(CONFIG_DEV_COREDUMP)]
                trace_capture,
                platform,
                num_clusters,
                gpc_perf_state_map,
                gpc_perf_state_map_low,
                gpc_perf_state_control,
                render_storage: None,
                render_tracker: None,
                render_bind: None,
                render_context_aliases: None,
                render_submission_ordinal: 0,
                render_auxiliary_timestamp: 0,
                render_hardware_buffer_id: None,
                render_hardware_buffer_published: false,
                render_hardware_buffer_ids: G17PHardwareBufferIdAllocator::new(),
                render_sksm: None,
                bootstrap_uma: None,
                prepared_render_flist: None,
                next_render_flist_sequence: G17P_BOOTSTRAP_UMA_SEQUENCE,
                parked_render_slots: [None, None],
                active_render_slot: 0,
                accepted_firmware_recovery_generation: 0,
                pending_primary_recovery: None,
                pending_add_kicks: None,
                last_published_descriptor: 0,
                next_compute_queue_id: Some(G17P_COMPUTE_QUEUE_ID_BASE),
                last_completion_end: 0,
                compute_stamp_armed: false,
                compute_stamp_waits: AtomicU32::new(0),
                compute_stamp_misses: AtomicU32::new(0),
            }),
            GFP_KERNEL,
        )
    }

    /// Large prepared-source observation before FList and outer publication.
    /// Diagnostic modes which expose an outer early receive an explicit
    /// omission instead of a misleading or timing-invasive "before" snapshot.
    pub(crate) fn capture_prepared_render_trace(&mut self, outers_deferred: bool) -> Result {
        #[cfg(CONFIG_DEV_COREDUMP)]
        {
            let Some(capture) = self.trace_capture.as_mut() else { return Ok(()); };
            if capture.prepared { return Ok(()); }
            capture.prepared = true;
            capture.job_stamp = self.render_sksm.as_ref()
                .map_or(0, |queues| queues.fragment.producer.last_add_kicks_timestamp);
            self.capture_render_trace_observation(trace::Phase::PreparedBeforeFList, outers_deferred)?;
        }
        Ok(())
    }

    #[cfg(CONFIG_DEV_COREDUMP)]
    fn capture_render_trace_observation(&mut self, phase: trace::Phase, safe: bool) -> Result {
        let Some(capture) = self.trace_capture.as_mut() else { return Ok(()); };
        capture.phase_sequence += 1;
        let sequence = capture.phase_sequence;
        let stamp = capture.job_stamp;
        let mut archive = TraceArchive::resume(&mut capture.arena).map_err(|_| EINVAL)?;
        let mut boundary = trace::Meta::new(trace::Kind::CaptureBoundary, phase, sequence);
        boundary.job_stamp = stamp;
        if !safe {
            archive.omit(boundary, trace::Omission::NotImplemented).map_err(|_| EINVAL)?;
            return Ok(());
        }
        let mut clock = || Monotonic::ktime_get() as u64;
        if self.resources.capture_owned_trace(&mut archive, phase, sequence, &mut clock).is_err() {
            archive.omit(boundary, trace::Omission::ReadError).map_err(|_| EINVAL)?;
        }
        match self.render_storage.as_mut() {
            Some(storage) => {
                if storage.capture_owned_trace(&mut archive, phase, sequence, stamp, &mut clock).is_err() {
                    boundary.instance = 1;
                    archive.omit(boundary, trace::Omission::ReadError).map_err(|_| EINVAL)?;
                }
            }
            None => {
                boundary.instance = 1;
                archive.omit(boundary, trace::Omission::Unavailable).map_err(|_| EINVAL)?;
            }
        }
        Ok(())
    }

    /// Only call after paired completion (before FList release), or after the
    /// state-1 recovery handshake has been validated (before ACK). A timeout
    /// alone is not permission to label a running GPU's memory "halted".
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn finish_render_trace(
        &mut self, phase: trace::Phase,
    ) -> Result<Option<KBox<G17PTraceDump>>> {
        if !matches!(phase, trace::Phase::AfterPairRetirement | trace::Phase::RecoveryBeforeAck) {
            return Err(EINVAL);
        }
        if !self.trace_capture.as_ref().is_some_and(|capture| capture.prepared) {
            return Ok(None);
        }
        self.capture_render_trace_observation(phase, true)?;
        let mut capture = self.trace_capture.take().ok_or(EINVAL)?;
        let mut archive = TraceArchive::resume(&mut capture.arena).map_err(|_| EINVAL)?;
        archive.seal();
        let used = archive.bytes().len();
        drop(archive);
        capture.used = used;
        Ok(Some(capture))
    }

    pub(crate) fn config(&self) -> &'static T8140ManagerConfig {
        &T8140_MANAGER_CONFIG
    }

    pub(crate) fn handoff(&self) -> &g17_resources::G17ResourceHandoff {
        self.resources.handoff()
    }

    pub(crate) fn control_counters(&mut self) -> Result<g17_resources::G17PControlCounters> {
        self.resources.control_counters()
    }

    pub(crate) fn control_pointer_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PControlPointerSnapshot> {
        self.resources.control_pointer_snapshot()
    }

    pub(crate) fn status_a_snapshot(&mut self) -> Result<g17_resources::G17PStatusASnapshot> {
        self.resources.status_a_snapshot()
    }

    pub(crate) fn firmware_recovery_handshake_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PFirmwareRecoveryHandshakeSnapshot> {
        self.resources.firmware_recovery_handshake_snapshot()
    }

    pub(crate) fn firmware_fault_report_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PFirmwareFaultReportSnapshot> {
        self.resources.firmware_fault_report_snapshot()
    }

    pub(crate) fn firmware_dm1_slot0_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PFirmwareDm1Slot0Snapshot> {
        self.resources.firmware_dm1_slot0_snapshot()
    }

    pub(crate) fn drain_firmware_event(
        &mut self, role: g17_initdata::InstanceRole,
    ) -> Result<Option<g17_resources::G17PPrimaryFirmwareEventRecord>> {
        self.resources.drain_firmware_event(role)
    }

    pub(crate) fn drain_primary_firmware_event(
        &mut self,
    ) -> Result<Option<g17_resources::G17PPrimaryFirmwareEventRecord>> {
        self.resources.drain_primary_firmware_event()
    }

    pub(crate) fn queue_primary_recovery(
        &mut self,
        generation: u64,
        records: u32,
    ) -> Result {
        if records == 0 || self.pending_primary_recovery.is_some() {
            return Err(EBUSY);
        }
        self.pending_primary_recovery = Some((generation, records));
        Ok(())
    }

    pub(crate) fn pending_primary_recovery(&self) -> Option<(u64, u32)> {
        self.pending_primary_recovery
    }

    pub(crate) fn complete_pending_primary_recovery(
        &mut self,
        generation: u64,
        records: u32,
    ) -> Result {
        if self.pending_primary_recovery != Some((generation, records)) {
            return Err(EINVAL);
        }
        self.pending_primary_recovery = None;
        Ok(())
    }

    pub(crate) fn primary_firmware_recovery_cause_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PFirmwareRecoveryCauseSnapshot> {
        self.resources.primary_firmware_recovery_cause_snapshot()
    }

    pub(crate) fn arm_firmware_trace_classes(&mut self, mask: u32) -> Result {
        self.resources.arm_firmware_trace_classes(mask)
    }

    pub(crate) fn check_report_ring_pointers(
        &mut self,
        role: InstanceRole,
    ) -> Result<(u64, u64)> {
        self.resources.check_report_ring_pointers(role)
    }

    pub(crate) fn drain_report_ring(
        &mut self,
        role: InstanceRole,
        advance: bool,
        print_budget: u32,
    ) -> Result<g17_resources::G17PReportDrainStats> {
        self.resources.drain_report_ring(role, advance, print_budget)
    }

    pub(crate) fn check_ktrace_ring_pointers(
        &mut self,
        role: InstanceRole,
    ) -> Result<(u64, u64)> {
        self.resources.check_ktrace_ring_pointers(role)
    }

    pub(crate) fn drain_ktrace_ring(
        &mut self,
        role: InstanceRole,
        verbose: bool,
        print_budget: u32,
    ) -> Result<g17_resources::G17PKtraceDrainStats> {
        self.resources
            .drain_ktrace_ring(role, verbose, print_budget)
    }

    /// Publish the held-back tag-14 AddKicks. Call only once the firmware
    /// recovery has closed, otherwise the unhalt's 128-queue restore erases it.
    pub(crate) fn publish_pending_add_kicks(
        &mut self,
        queue: &mut G17PSksmQueue,
        expected_producer: u32,
    ) -> Result<bool> {
        let Some(bytes) = self.pending_add_kicks.take() else {
            return Ok(false);
        };
        let after = queue
            .compute
            .publish_add_kicks_command(expected_producer, &bytes)?;
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: deferred tag-14 AddKicks published at producer {} -> {}\n",
            expected_producer,
            after
        );
        Ok(true)
    }

    pub(crate) fn has_pending_add_kicks(&self) -> bool {
        self.pending_add_kicks.is_some()
    }

    pub(crate) fn dump_firmware_log(&mut self, max_records: usize) -> Result<usize> {
        self.resources.dump_firmware_log(max_records)
    }

    pub(crate) fn dump_completion_stamps(&mut self, label: &'static str) -> Result {
        self.resources.dump_completion_stamps(label)
    }

    pub(crate) fn dump_completion_records(&mut self, label: &'static str) -> Result {
        self.resources.dump_completion_records(label)
    }

    /// Publish one item-ring record, reporting the ring state if it is refused.
    ///
    /// The four publishes used to `?` straight out, so an exhausted or
    /// unreclaimed ring surfaced as a bare `EBUSY` from four frames down with
    /// nothing to say which record failed or what the cursors were. At the ring
    /// wrap that is precisely the information needed.
    fn publish_repeat_record(
        &mut self,
        queue: &mut G17PSksmQueue,
        which: &'static str,
        producer: u32,
        record: G17PRepeatRecord<'_>,
    ) -> Result<u32> {
        let result = match record {
            G17PRepeatRecord::Pointer(gpu_va) => {
                queue.compute.publish_command_pointer_at(producer, gpu_va)
            }
            G17PRepeatRecord::AddKicks(bytes) => {
                queue.compute.publish_add_kicks_command(producer, bytes)
            }
            G17PRepeatRecord::EntrySignal(bytes) => {
                queue.compute.publish_initial_entry_signal_at(producer, bytes)
            }
        };
        match result {
            Ok(next) => Ok(next),
            Err(error) => {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P compute: publishing {} at ring producer {} failed ({:?}); the target slot was not free\n",
                    which,
                    producer,
                    error
                );
                self.log_compute_ring_state(queue, "publish-failed");
                Err(error)
            }
        }
    }

    pub(crate) fn log_compute_retire(
        &mut self,
        queue: &mut G17PSksmQueue,
        completion: [u64; 2],
        ordinal: u32,
    ) {
        let support = queue.compute.read_compute_support_words();
        let duration = completion[1].wrapping_sub(completion[0]);
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P retire[{}]: descriptor {:#x} start {:#x} end {:#x} duration {:#x} support(dispatch-a,dispatch-b,status-a,status-b)={:#x?}\n",
            ordinal,
            self.last_published_descriptor,
            completion[0],
            completion[1],
            duration,
            support
        );
        if *module_parameters::g17p_completion_trace.value() > 1 {
            let _ = self.dump_compute_completion_records("retire");
        }
    }

    /// Decode the completion ring against the descriptor the in-flight
    /// submission is waiting for.
    pub(crate) fn dump_compute_completion_records(&mut self, label: &'static str) -> Result {
        let expected = self.last_published_descriptor;
        self.resources
            .dump_compute_completion_records(label, expected)
    }

    pub(crate) fn read_compute_completion(&mut self) -> Result<Option<[u64; 2]>> {
        let expected = self.last_published_descriptor;
        let last_end = self.last_completion_end;
        self.resources.read_compute_completion(expected, last_end)
    }

    /// Mark a completion as consumed so the next wait cannot be satisfied by
    /// the record it left behind.
    pub(crate) fn commit_compute_completion(&mut self, completion: [u64; 2]) {
        self.last_completion_end = completion[1];
    }

    pub(crate) fn dump_primary_recovery_cause_block(&mut self) -> Result {
        self.resources.dump_primary_recovery_cause_block()
    }

    pub(crate) fn primary_firmware_recovery_info_snapshot(
        &mut self,
    ) -> Result<g17_resources::G17PFirmwareRecoveryInfoSnapshot> {
        self.resources.primary_firmware_recovery_info_snapshot()
    }

    pub(crate) fn validate_empty_primary_firmware_recovery(
        &self,
        packet_generation: u64,
    ) -> Result {
        if packet_generation != self.accepted_firmware_recovery_generation {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P recovery: packet generation {} does not match host accepted generation {}\n",
                packet_generation,
                self.accepted_firmware_recovery_generation
            );
            return Err(EIO);
        }
        Ok(())
    }

    /// Is there live render scheduler state?
    ///
    /// This is the exact predicate
    /// `validate_empty_primary_firmware_recovery_scheduler` refuses on. It is
    /// exposed separately because that refusal is unsatisfiable for a
    /// render-blamed recovery: the firmware raises the restart request DURING
    /// the render wait, so `render_storage` and `render_tracker` are
    /// necessarily populated at exactly the moment we need to answer.
    pub(crate) fn render_scheduler_active(&self) -> bool {
        self.render_storage.is_some()
            || self.render_tracker.is_some()
            || self.render_bind.is_some()
            || self.render_submission_ordinal != 0
            || self.parked_render_scheduler_active()
    }

    pub(crate) fn validate_empty_primary_firmware_recovery_scheduler(&self) -> Result {
        if self.render_storage.is_some()
            || self.render_tracker.is_some()
            || self.render_bind.is_some()
            || self.render_submission_ordinal != 0
            || self.parked_render_scheduler_active()
        {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P recovery: empty-scheduler validation failed storage={} tracker={} bind={} ordinal={}\n",
                self.render_storage.is_some(),
                self.render_tracker.is_some(),
                self.render_bind.is_some(),
                self.render_submission_ordinal
            );
            return Err(EBUSY);
        }
        Ok(())
    }

    pub(crate) fn accept_empty_primary_firmware_recovery(
        &mut self,
        packet_generation: u64,
    ) -> Result<u64> {
        self.validate_empty_primary_firmware_recovery(packet_generation)?;
        self.accepted_firmware_recovery_generation =
            self.accepted_firmware_recovery_generation.wrapping_add(1);
        Ok(self.accepted_firmware_recovery_generation)
    }

    pub(crate) fn acknowledge_primary_firmware_recovery(
        &mut self,
        expected: u32,
        next: u32,
    ) -> Result {
        self.resources
            .acknowledge_primary_firmware_recovery(expected, next)
    }

    pub(crate) fn reset_empty_sksm_last_submitted_hw_timestamps(&mut self) -> Result {
        self.resources
            .reset_empty_sksm_last_submitted_hw_timestamps()
    }

    /// Hexdump one kernel-built record, for the add3-vs-Honeykrisp diff.
    ///
    /// Everything audited so far is an object the GPU READS. These are the
    /// records the DRIVER WRITES from the UAPI submit arguments, and add3 and
    /// Honeykrisp reach them through different callers with different
    /// arguments -- the last surface where the two paths can differ.
    fn log_record(&self, label: &str, bytes: &[u8]) {
        if *module_parameters::g17p_fault_report.value() == 0 {
            return;
        }
        for (index, chunk) in bytes.chunks(16).enumerate() {
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P rec {} +{:#05x} {:02x?}\n",
                label,
                index * 16,
                chunk,
            );
        }
    }

    pub(crate) fn g17p_last_submitted_hw_timestamp(&mut self, qid: usize) -> Result<(u32, u64)> {
        self.resources.g17p_last_submitted_hw_timestamp(qid)
    }

    pub(crate) fn validate_native_pre_user_roots(&self) -> Result {
        if !self
            .resources
            .uat()
            .t8140_partial_opening_kernel_contexts_ready()
        {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P roots: native pre-user context 0/1 table is invalid\n"
            );
            return Err(EIO);
        }
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P roots: native pre-user context 0/1 table retained; user bind owns first split\n"
        );
        Ok(())
    }

    pub(crate) fn control_opening_effect(
        &mut self,
    ) -> Result<g17_resources::G17PControlOpeningEffect> {
        self.resources.control_opening_effect()
    }

    /// Stage the same queue-pair-zero TA/3D bootstrap used by the
    /// output-positive source path. The VDM stream is a driver-owned terminate
    /// command, so this establishes scheduler lifecycle without user BOs.
    pub(crate) fn stage_compute_bootstrap_render(
        &mut self,
        dev: &AsahiDevice,
    ) -> Result<mmu::Vm> {
        let kernel_range = self.resources.uat().kernel_va_range().inspect_err(|error| {
            dev_err!(
                dev.as_ref(),
                "G17P bootstrap: kernel VA range failed ({:?})\n",
                error
            );
        })?;
        let vm = self
            .new_user_vm_inner(0x8140_b007, kernel_range, true)
            .inspect_err(|error| {
                dev_err!(
                    dev.as_ref(),
                    "G17P bootstrap: render VM setup failed ({:?})\n",
                    error
                );
            })?;
        let mut parameters = g17_render::G17pRenderParameters::source_rsrc8();
        parameters.gpc_perf_state_map = self.gpc_perf_state_map;
        parameters.gpc_perf_state_map_low = self.gpc_perf_state_map_low;
        parameters.gpc_perf_state_control = self.gpc_perf_state_control;
        parameters.encoder = g17_resources::G17P_BOOTSTRAP_VDM_VA;
        parameters.process_empty_tiles = false;
        parameters.tile_config &= !0x1_0000;
        let timestamp = g17_uapi::UapiTimestamp {
            handle: 0,
            offset: 0,
        };
        let command = g17_uapi::TranslatedRenderCommand {
            flags: 0,
            vdm_base: parameters.encoder,
            parameters,
            vertex_attachments: g17_uapi::UapiAttachmentList::EMPTY,
            fragment_attachments: g17_uapi::UapiAttachmentList::EMPTY,
            vertex_timestamps: g17_uapi::UapiTimestamps {
                start: timestamp,
                end: timestamp,
            },
            fragment_timestamps: g17_uapi::UapiTimestamps {
                start: timestamp,
                end: timestamp,
            },
        };
        if self.render_tracker.is_some()
            || self.render_storage.is_some()
            || self.render_bind.is_some()
            || self.render_submission_ordinal != 0
        {
            return Err(EBUSY);
        }
        self.render_storage = Some(
            self.resources
                .new_user_render_storage(
                    dev,
                    &vm,
                    g17_resources::G17PRenderQueuePair::for_slot(0).ok_or(EINVAL)?,
                    None,
                    1,
                    &command,
                    self.num_clusters,
                    [0; 4],
                    KVec::new(),
                    0,
                )
                .inspect_err(|error| {
                    dev_err!(
                        dev.as_ref(),
                        "G17P bootstrap: render graph setup failed ({:?})\n",
                        error
                    );
                })?,
        );
        let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
        if let Some(failed) = storage.first_unreachable(self.resources.uat(), &vm) {
            storage.log_reachability(dev, self.resources.uat(), &vm);
            dev_err!(
                dev.as_ref(),
                "G17P bootstrap render: storage unreachable, first failing predicate is {}\n",
                failed
            );
            return Err(EFAULT);
        }
        let binding = self.resources.uat().bind(&vm)?;
        if binding.slot() != 1 {
            return Err(EFAULT);
        }
        self.resources
            .uat()
            .install_t8140_partial_opening_shared_render_roots(&binding)?;
        self.render_bind = Some(G17PRenderRootOwner::Bootstrap(binding));
        Ok(vm)
    }

    /// Apply the measured post-opening context split after primary
    /// control-done and before either first-work producer becomes visible.
    pub(crate) fn split_compute_bootstrap_render_contexts(&self) -> Result {
        let binding = self.render_bind.as_ref().and_then(G17PRenderRootOwner::bootstrap).ok_or(ENODEV)?;
        self.resources
            .uat()
            .split_t8140_partial_opening_render_roots(binding)?;
        if !self
            .resources
            .uat()
            .t8140_first_work_render_context_ready(binding)
        {
            return Err(EFAULT);
        }
        Ok(())
    }

    /// Publish the staged bootstrap pair after primary control-done.
    pub(crate) fn publish_staged_compute_bootstrap_render(
        &mut self,
    ) -> Result<g17_submission::PreparedG17PPartialOpeningWorkPair> {
        if self.render_tracker.is_some()
            || self.render_bind.is_none()
            || self.render_submission_ordinal != 0
        {
            return Err(EBUSY);
        }
        if !self.resources.uat().t8140_first_work_render_context_ready(
            self.render_bind.as_ref().and_then(G17PRenderRootOwner::bootstrap).ok_or(ENODEV)?,
        ) {
            return Err(EFAULT);
        }
        let (fragment_queue, tiling_queue, previous, late_tiling) = {
            let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
            // The successful first-partial path keeps both inner groups blank
            // until primary control-done. Restore 3D now; TA remains held by
            // the late publication callback below.
            storage.stage_fragment(0)?;
            let previous = storage.snapshot()?;
            let late_tiling = storage.prepare_late_tiling_publication(0)?;
            let (fragment_queue, tiling_queue) = storage.queue_gpu_vas();
            (fragment_queue, tiling_queue, previous, late_tiling)
        };
        let (prepared, render_bind) = self.resources.publish_partial_opening_pair(
            None,
            true,
            g17_resources::G17PRenderQueuePair::for_slot(0).ok_or(EINVAL)?,
            g17_resources::G17PDeferredOuterPublication::None,
            fragment_queue,
            3,
            tiling_queue,
            3,
            true,
            || {},
            move || late_tiling.publish(),
        )?;
        if render_bind.is_some() {
            return Err(EINVAL);
        }
        self.render_tracker = Some(G17PPartialOpeningRenderTracker {
            tiling_previous: previous.tiling,
            fragment_previous: previous.fragment,
            tiling_prefix: 3,
            fragment_prefix: 3,
        });
        Ok(prepared)
    }

    /// Match the source bootstrap fence: both three-item inner queues and
    /// both outer channel consumers must retire before the graph can drop.
    pub(crate) fn wait_compute_bootstrap_render_retirement(&mut self) -> Result {
        const POLLS: usize = 500;
        let mut last = None;
        for _ in 0..POLLS {
            fence(Ordering::Acquire);
            let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
            let outer = self.resources.partial_opening_outer_counters()?;
            // m1n1's fence owns the prefix once done/read cover the three
            // items and the outer consumers reach producer one. The queue may
            // already advertise a later write prefix (the positive run is
            // 3/3/6), so equality with write=3 is not an ownership rule.
            let inner_retired = state.tiling.done >= 3
                && state.tiling.read >= 3
                && state.tiling.write >= 3
                && state.fragment.done >= 3
                && state.fragment.read >= 3
                && state.fragment.write >= 3;
            let outer_retired = outer
                .into_iter()
                .all(|counters| counters[2] == 1 && counters[..2] == [counters[2]; 2]);
            if inner_retired && outer_retired {
                self.render_tracker = None;
                self.render_bind = None;
                self.render_storage = None;
                self.render_sksm = None;
                self.render_submission_ordinal = 0;
                self.resources
                    .uat()
                    .restore_t8140_kernel_context_after_bootstrap_render()?;
                return Ok(());
            }
            last = Some((state, outer));
            fsleep(Delta::from_millis(1));
        }
        if let Some((state, outer)) = last {
            pr_err!(
                "G17P bootstrap timeout: TA {:?}, 3D {:?}, outer 3D/TA {:?}\n",
                state.tiling,
                state.fragment,
                outer
            );
        }
        Err(ETIMEDOUT)
    }

    pub(crate) fn prepare_compute_runtime_class1(&mut self) -> Result {
        self.resources.prepare_compute_runtime_class1()
    }

    pub(crate) fn publish_compute_runtime_control_record(
        &mut self,
        index: usize,
    ) -> Result<u32> {
        self.resources.publish_compute_runtime_control_record(index)
    }

    /// Append one live device-control record to the primary channel-12 ring and
    /// advance its host-owned producer. The caller owns the EP-0x21 mailbox and
    /// must send `encode_primary_device_control()` afterwards.
    pub(crate) fn publish_primary_device_control_record(
        &mut self,
        opcode: u32,
        arg: u32,
    ) -> Result<g17_resources::G17PDeviceControlPublication> {
        self.resources
            .publish_primary_device_control_record(opcode, arg)
    }

    /// Activate this prepared pair's FList lease. Only the cold lease has a
    /// pending async grow to publish; a warm pair becomes Ready without 0x20.
    pub(crate) fn publish_render_usc_freelist_request(
        &mut self,
    ) -> Result<Option<g17_resources::G17PDeviceControlPublication>> {
        if self.render_storage.is_none() {
            return Err(EINVAL);
        }
        let mut lifecycle = self.prepared_render_flist.ok_or(EIO)?;
        lifecycle.retire_prepare_reference().map_err(|_| EIO)?;
        if lifecycle.request.is_none() {
            self.next_render_flist_sequence = lifecycle.sequence.checked_add(1).ok_or(EOVERFLOW)?;
            self.bootstrap_uma = Some(lifecycle);
            self.prepared_render_flist = None;
            return Ok(None);
        }
        if lifecycle.sequence != G17P_BOOTSTRAP_UMA_SEQUENCE {
            return Err(EINVAL);
        }

        let mut record = [0u8; g17_initdata::CONTROL_RECORD_SIZE];
        g17_submission::encode_g17p_usc_freelist_control(
            g17_initdata::CONTROL_SHARED_ADDRESS,
            g17_submission::G17P_USC_FREELIST_LOW_VA,
            g17_submission::G17P_USC_FREELIST_LOW_END,
            g17_submission::G17PUscFreelistSequence {
                word_04: G17P_BOOTSTRAP_UMA_STAMP as u32,
                word_0c: G17P_BOOTSTRAP_UMA_SEQUENCE as u32,
                word_30: G17P_BOOTSTRAP_UMA_HARDWARE_BUFFER_ID,
            },
            &mut record,
        )
        .map_err(|_| EINVAL)?;

        let publication = self
            .resources
            .publish_primary_device_control_raw(&record)?;
        self.bootstrap_uma = Some(lifecycle);
        self.prepared_render_flist = None;
        self.next_render_flist_sequence = G17P_BOOTSTRAP_UMA_SEQUENCE + 1;
        Ok(Some(publication))
    }

    /// Match a firmware type-13 completion to the retained FList request.
    /// This retires the async-grow reference only; TA and 3D still own it.
    pub(crate) fn match_render_usc_freelist_completion(
        &mut self,
        completion: G17PBootstrapUmaCompletion,
    ) -> Result {
        self.bootstrap_uma
            .as_mut()
            .ok_or(EIO)?
            .match_completion(completion)
            .map_err(|_| EIO)?;
        self.resources.log_render_uma_growth("type13");
        Ok(())
    }

    /// Whether the asynchronous opcode-0x20 grow has retired its own
    /// reference. TA/3D references intentionally remain live until paired
    /// render completion.
    pub(crate) fn render_usc_freelist_grow_matched(&self) -> bool {
        self.bootstrap_uma
            .as_ref()
            .is_some_and(|lifecycle| lifecycle.state == G17PBootstrapUmaRequestState::Ready)
    }

    pub(crate) fn retire_render_usc_freelist_at_completion(
        &mut self,
    ) -> Result<Option<G17PBootstrapUmaRelease>> {
        let Some(lifecycle) = self.bootstrap_uma.as_mut() else {
            return Ok(None);
        };
        let owned = lifecycle.hardware_buffer_owned;
        let references = lifecycle.hardware_buffer_references;
        let pending = lifecycle.pending_release_count;
        match lifecycle.retire_render_references() {
            Ok(release) => Ok(release),
            Err(error) => {
                // This used to collapse every lifecycle state into a bare EIO,
                // which is what the render now fails with AFTER its paired
                // completion fires -- i.e. after the GPU has done the whole
                // render and both stages have stamped real timestamps. Name
                // the phase so the failure is diagnosable from one run.
                dev_warn!(
                    self.resources.dev().as_ref(),
                    "G17P render: FList retire refused ({:?}) owned={} references={} pending={}\n",
                    error,
                    owned,
                    references,
                    pending,
                );
                Err(EIO)
            }
        }
    }

    /// Publish the release generated by this pair's final reference drop.
    /// The caller still owns the EP-0x21 announcement.
    pub(crate) fn publish_render_usc_freelist_release(
        &mut self,
        release: G17PBootstrapUmaRelease,
    ) -> Result<g17_resources::G17PDeviceControlPublication> {
        let pending = self
            .bootstrap_uma
            .as_ref()
            .and_then(G17PBootstrapUmaLifecycle::pending_release);
        if pending != Some(release) {
            dev_warn!(
                self.resources.dev().as_ref(),
                "G17P render: FList release mismatch pending={:?} wanted={:?}\n",
                pending,
                release,
            );
            return Err(EIO);
        }
        let mut record = [0u8; g17_initdata::CONTROL_RECORD_SIZE];
        g17_initdata::encode_bootstrap_uma_release_record(
            release.sequence,
            release.hardware_buffer_id,
            &mut record,
        )
        .map_err(|_| EINVAL)?;
        let publication = self
            .resources
            .publish_primary_device_control_raw(&record)?;
        self.bootstrap_uma
            .as_mut()
            .ok_or(EIO)?
            .mark_published(release)
            .map_err(|_| EIO)?;
        Ok(publication)
    }

    pub(crate) fn mark_render_usc_freelist_release_sent(
        &mut self,
        producer_after: u32,
    ) -> Result {
        self.bootstrap_uma
            .as_mut()
            .ok_or(EIO)?
            .mark_sent(producer_after)
            .map_err(|_| EIO)
    }

    pub(crate) fn pending_render_usc_freelist_release_producer(&self) -> Option<u32> {
        self.bootstrap_uma
            .as_ref()
            .filter(|lifecycle| lifecycle.release_sent())
            .and_then(|lifecycle| lifecycle.release_producer_after)
    }

    /// Complete the lifecycle only once firmware's consumer reaches the
    /// release. If observation times out, the owner remains retained in Sent
    /// state until next-admission settlement or the stopped-processor teardown
    /// boundary.
    pub(crate) fn complete_render_usc_freelist_release_if_consumed(
        &mut self,
        producer_after: u32,
    ) -> Result<bool> {
        let lifecycle = self.bootstrap_uma.as_mut().ok_or(EIO)?;
        if lifecycle.release_producer_after != Some(producer_after) {
            return Err(EINVAL);
        }
        let counters = self.resources.control_counters()?;
        lifecycle
            .observe_release_consumer(
                counters.primary[g17_initdata::CHANNEL_STATE_CONSUMER],
            )
            .map_err(|_| EIO)
    }

    pub(crate) fn prepare_compute_runtime_class2(&mut self) -> Result {
        self.resources.prepare_compute_runtime_class2()
    }

    pub(crate) fn render_flist_owned(&self) -> bool {
        self.resources.render_flist_owned()
    }

    pub(crate) fn stage_compute_flist_state(&mut self) -> Result {
        self.resources.stage_compute_flist_state()
    }

    pub(crate) fn rewrite_compute_dispatch_record(&mut self) -> Result {
        self.resources.rewrite_compute_dispatch_record()
    }

    pub(crate) fn uat(&self) -> &mmu::Uat {
        self.resources.uat()
    }

    pub(crate) fn mark_firmware_cache_flush_ready(&self) {
        self.resources.mark_firmware_cache_flush_ready();
    }

    fn validate_partial_opening_resources(&self) -> Result {
        let binding = self.resources.partial_opening_binding();
        let context = g17_submission::G17P_PARTIAL_OPENING_CONTEXT;
        let expected_fwctl = binding
            .kernel_va_base
            .checked_add(g17_submission::G17P_PARTIAL_OPENING_FWCTL_OFFSET)
            .ok_or(EINVAL)?;
        let addresses_ready = binding.scheduler_page
            == g17_submission::G17P_PARTIAL_OPENING_SCHEDULER_GPU_VA
            && binding.primary_index_page
                == g17_submission::G17P_PARTIAL_OPENING_PRIMARY_INDEX_GPU_VA
            && binding.fwctl == expected_fwctl;
        let context_ready = context.context0_root_slot == 0
            && context.context0_context_id == 0
            && context.render_root_slot == 1
            && context.render_context_id == 1
            && context.context0_empty_high_root_slot == 0
            && context.render_empty_high_root_slot == 1
            && context.transport_pair == 0
            && context.descriptor_pair == 0
            && context.shared_queue_pair_namespace == 0
            && context.optional_identity_context_id == 1
            && context.descriptor_context_id == 1
            && context.scheduler_class == 2
            && context.work_doorbell_channel == 0
            && context.context0_low_root_present_at_first_work
            && context.render_low_root_present_at_first_work
            && !context.source_firmware_high_root_table_resident;
        let roots_ready = match self.render_bind.as_ref() {
            Some(binding) => binding.bootstrap().is_some_and(|binding| self
                .resources
                .uat()
                .t8140_partial_opening_shared_render_context_ready(binding)),
            None => self
                .resources
                .uat()
                .t8140_partial_opening_kernel_contexts_ready(),
        };
        if !addresses_ready || !context_ready || !roots_ready {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P render: opening contract addresses={} context={} roots={} (scheduler {:#x}, index {:#x}, fwctl {:#x}/{:#x})\n",
                addresses_ready,
                context_ready,
                roots_ready,
                binding.scheduler_page,
                binding.primary_index_page,
                binding.fwctl,
                expected_fwctl
            );
            return Err(EINVAL);
        }
        Ok(())
    }

    /// Apply first-work status immediately before the caller sends primary
    /// control-done 0x84.
    pub(crate) fn prepare_partial_opening_control_done(&mut self) -> Result {
        self.validate_partial_opening_resources().inspect_err(|error| {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P render: opening validation failed ({:?})\n",
                error
            );
        })?;
        self.resources
            .apply_partial_opening_pre_control_status()
            .inspect_err(|error| {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P render: status-B update failed ({:?})\n",
                    error
                );
            })
    }

    /// Publish the first-partial operand table in the control-done to
    /// first-work interval.
    pub(crate) fn publish_partial_opening_operand_table(&mut self) -> Result {
        self.resources.publish_partial_opening_operand_table()
    }

    /// Publish the exact first 3D/TA outer-ring pair. The callback installs
    /// the late TA descriptor, item-ring, and queue-head state between the 3D
    /// producer and the TA outer slot. The retained live runtime sends the
    /// returned channel-0 doorbell exactly once.
    pub(crate) fn submit_partial_opening_render<F>(
        &mut self,
        submission: G17PPartialOpeningRenderSubmission,
        publish_late_tiling_state: F,
    ) -> Result<(
        G17PPartialOpeningRenderTracker,
        g17_submission::PreparedG17PPartialOpeningWorkPair,
    )>
    where
        F: FnOnce(),
    {
        self.validate_partial_opening_resources()?;
        if submission.fragment_queue_write_index != 3 || submission.tiling_queue_write_index != 3 {
            return Err(EINVAL);
        }
        let (prepared, render_bind) = self.resources.publish_partial_opening_pair(
            None,
            false,
            g17_resources::G17PRenderQueuePair::for_slot(0).ok_or(EINVAL)?,
            g17_resources::G17PDeferredOuterPublication::None,
            submission.fragment_queue_gpu_va,
            submission.fragment_queue_write_index,
            submission.tiling_queue_gpu_va,
            submission.tiling_queue_write_index,
            true,
            || {},
            publish_late_tiling_state,
        )?;
        if render_bind.is_some() {
            return Err(EINVAL);
        }
        if prepared.paired_doorbell != g17_submission::G17P_PARTIAL_OPENING_PAIRED_DOORBELL
            || prepared.paired_doorbell != 0x0083_0000_0000_0000
        {
            return Err(EINVAL);
        }
        let tracker = G17PPartialOpeningRenderTracker {
            tiling_previous: g17_completion::QueueIndices {
                done: 0,
                read: 0,
                write: submission.tiling_queue_write_index,
            },
            fragment_previous: g17_completion::QueueIndices {
                done: 0,
                read: 0,
                write: submission.fragment_queue_write_index,
            },
            tiling_prefix: submission.tiling_queue_write_index,
            fragment_prefix: submission.fragment_queue_write_index,
        };
        Ok((tracker, prepared))
    }

    /// Classify paired render progress from queue indices plus the scheduler
    /// job list and both descriptor timestamp quartets.
    pub(crate) fn observe_partial_opening_render_completion(
        &self,
        tracker: &mut G17PPartialOpeningRenderTracker,
        tiling_current: g17_completion::QueueIndices,
        fragment_current: g17_completion::QueueIndices,
        execution: g17_completion::RenderExecutionState,
    ) -> Result<g17_completion::PairedRenderCompletion> {
        let completion = g17_completion::observe_paired_render_completion(
            tracker.tiling_previous,
            tiling_current,
            tracker.tiling_prefix,
            tracker.fragment_previous,
            fragment_current,
            tracker.fragment_prefix,
            execution,
        )
        .map_err(|_| EINVAL)?;
        tracker.tiling_previous = tiling_current;
        tracker.fragment_previous = fragment_current;
        Ok(completion)
    }

    pub(crate) fn new_user_vm(
        &mut self,
        id: u64,
        kernel_range: core::ops::Range<u64>,
    ) -> Result<mmu::Vm> {
        self.new_user_vm_inner(id, kernel_range, false)
    }

    fn new_user_vm_inner(
        &mut self,
        id: u64,
        kernel_range: core::ops::Range<u64>,
        bootstrap_vdm: bool,
    ) -> Result<mmu::Vm> {
        let vm = self
            .resources
            .uat()
            .new_vm(id, kernel_range)
            .inspect_err(|error| {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P VM: context allocation failed ({:?})\n",
                    error
                );
            })?;
        self.resources
            .install_user_vm_aliases(&vm, bootstrap_vdm)
            .inspect_err(|error| {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P VM: fixed aliases failed ({:?})\n",
                    error
                );
            })?;
        Ok(vm)
    }

    pub(crate) fn map_timestamp_buffer(
        &self,
        mut bo: crate::gem::ObjectRef,
        range: core::ops::Range<usize>,
    ) -> Result<mmu::KernelMapping> {
        bo.map_range_into_range(
            self.resources.uat().kernel_vm(),
            range,
            g17_resources::g17p_dynamic_kernel_va_range(
                self.resources.uat().kernel_va_range()?).ok_or(ERANGE)?,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW,
            false,
        )
    }

    /// Release one stopped firmware session without replacing its UAT owner.
    pub(crate) fn release_runtime_objects_after_processor_stop(&mut self) {
        for parked in &mut self.parked_render_slots {
            let Some(mut state) = parked.take() else { continue };
            if let Some(id) = state.hardware_buffer_id.take() {
                if let Err(error) = self.render_hardware_buffer_ids.release(id) {
                    dev_warn!(
                        self.resources.dev().as_ref(),
                        "G17P render: failed to release parked TA HardwareBufferID {} after processor stop ({:?})\n",
                        id,
                        error,
                    );
                }
            }
            // Both firmware CPUs are stopped; dropping the complete state now
            // retracts its command root before any reachable mapping backing.
            drop(state);
        }
        if let Some(id) = self.render_hardware_buffer_id.take() {
            match self.render_hardware_buffer_ids.release(id) {
                Ok(()) => dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P render: released TA HardwareBufferID {} after processor stop\n",
                    id,
                ),
                Err(error) => dev_warn!(
                    self.resources.dev().as_ref(),
                    "G17P render: failed to release TA HardwareBufferID {} after processor stop ({:?})\n",
                    id,
                    error,
                ),
            }
        }
        self.render_hardware_buffer_published = false;
        self.render_bind = None;
        self.render_tracker = None;
        self.render_storage = None;
        // Both CPUs are stopped at this boundary. The alias owner's Drop
        // removes the context-0 leaves and invalidates ASIDs 0/1.
        self.render_context_aliases = None;
        self.render_sksm = None;
        self.bootstrap_uma = None;
        self.prepared_render_flist = None;
        self.render_submission_ordinal = 0;
        self.render_auxiliary_timestamp = 0;
        self.next_render_flist_sequence = G17P_BOOTSTRAP_UMA_SEQUENCE;
        self.active_render_slot = 0;
    }

    /// Restore cold initdata in the existing mappings after both CPUs stop.
    pub(crate) fn reset_session_after_processor_stop(&mut self) -> Result {
        if self.render_bind.is_some()
            || self.render_tracker.is_some()
            || self.render_storage.is_some()
            || self.render_context_aliases.is_some()
            || self.parked_render_scheduler_active()
        {
            return Err(EBUSY);
        }
        self.resources.reset_session_after_processor_stop()?;
        self.accepted_firmware_recovery_generation = 0;
        self.pending_primary_recovery = None;
        Ok(())
    }

    /// The retained graph and global aliases must agree on the incoming VM.
    /// A new client cannot safely reuse either owner's mappings.
    pub(crate) fn retained_render_has_other_owner(&self, vm: &mmu::Vm) -> bool {
        let graph_retained = self.render_storage.is_some();
        let aliases_retained = self.render_context_aliases.is_some();
        let retained = graph_retained || aliases_retained;
        let graph_same_vm = !graph_retained
            || self
                .render_bind
                .as_ref()
                .is_some_and(|binding| binding.matches(vm));
        let aliases_same_vm = self
            .render_context_aliases
            .as_ref()
            .map_or(true, |aliases| aliases.matches(vm));
        let same_vm = graph_same_vm && aliases_same_vm;
        retained && !same_vm
    }

    fn require_retired_render_owner(&mut self) -> Result {
        if self.render_tracker.is_some() || self.render_hardware_buffer_id.is_some()
            || self.render_hardware_buffer_published || self.prepared_render_flist.is_some()
            || self.render_submission_ordinal == 0
        {
            return Err(EBUSY);
        }
        // The USC FList is a device-global refcounted pool. Another slot may
        // legitimately retain its references after this slot's TA/3D pair is
        // complete; those references do not keep this job's graph or VM alive.
        let snapshot = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
        if !snapshot.job_list_empty || !snapshot.tiling.idle() || !snapshot.fragment.idle() {
            return Err(EBUSY);
        }
        // Do not require the device-global outer channels to be empty here.
        // This selected slot's paired completion already proved that its two
        // queue items were consumed and its job list is idle.  A later outer
        // item may legitimately belong to the other independently rooted
        // slot; using its live producer as an idle condition reintroduced the
        // full render/render lease at every cross-VM handoff.
        Ok(())
    }

    /// Retire one VM's job-private graph before another VM constructs its own
    /// graph on the already-installed physical QID pair.  Queue producer state
    /// and the monotonically increasing submission ordinal stay retained; no
    /// global context-0 mapping is repointed.
    pub(crate) fn handoff_retained_render_vm(&mut self, vm: &mmu::Vm) -> Result {
        if self.render_storage.is_none() || !self.retained_render_has_other_owner(vm) {
            return Ok(());
        }
        self.require_retired_render_owner()?;
        let old_root = self.render_bind.as_ref().map_or(0, |binding| binding.root());
        let old_vm_id = self.render_bind.as_ref().map_or(0, |binding| binding.vm_id());
        self.render_storage.as_mut().ok_or(ENODEV)?.release_client_context();
        self.render_bind = None;
        self.render_context_aliases = None;
        self.render_storage = None;
        dev_info!(self.resources.dev().as_ref(),
            "G17P render VM handoff: retired job-private graph vm={} root={:#x}; incoming VM builds a new root at ordinal {}, QID producer state retained\n",
            old_vm_id, old_root, self.render_submission_ordinal);
        Ok(())
    }

    pub(crate) fn retained_render_needs_recycle(&self, vm: &mmu::Vm) -> bool {
        g17_lifecycle::retained_render_requires_recycle(
            self.render_storage.is_some() || self.render_context_aliases.is_some(),
            !self.retained_render_has_other_owner(vm),
            self.render_submission_ordinal,
        )
    }

    fn render_stage_fail(stage: &str, error: kernel::error::Error) -> kernel::error::Error {
        pr_info!(
            "G17P render: STAGE FAIL stage={} errno={}\n",
            stage,
            error.to_errno()
        );
        error
    }

    fn new_render_sksm_queues(
        &self,
        pair: g17_resources::G17PRenderQueuePair,
        entry_low_vas: [u64; 2],
    ) -> Result<G17PRenderSksmQueues> {
        const RENDER_COMPLETION_SELECTOR: u8 =
            g17_submission::G17P_NATIVE_RENDER_DOORBELL_PRIORITY;
        let geometry = self.sksm_queue_geometry();
        let scratch = self.sksm_scratch();
        let build = |queue_id: u8, data_master: u32, tiling: bool, entry_gpu_va: u64| -> Result<G17PRenderSksmQueue> {
            let routing = g17_submission::G17PComputeQueueRouting {
                queue_id,
                completion_selector: RENDER_COMPLETION_SELECTOR as u32,
                entry_gpu_va,
                geometry,
            };
            let config = g17_submission::prepare_g17p_render_sksm_queue_config(
                routing,
                data_master,
                scratch,
            )
            .map_err(|_| Self::render_stage_fail("sksm-queue-config", EINVAL))?;
            Ok(G17PRenderSksmQueue {
                queue_id,
                tiling,
                configure_pair: config.write_pair(),
                enable_pair: config.enable_pair,
                producer: geometry
                    .initial_producer_state(
                        queue_id,
                        RENDER_COMPLETION_SELECTOR,
                        g17_submission::G17PClQosConfig {
                            queue_byte_32: match *crate::module_parameters::g17p_render_kick_qos
                                .value()
                            {
                                1 if tiling => 1,
                                2 => 1,
                                _ => pair.qos_hardware_buffer_id(),
                            },
                            class_40: match *crate::module_parameters::g17p_render_kick_qos
                                .value()
                            {
                                1 if tiling => 0x08,
                                2 => 0x08,
                                _ if tiling => 0x0c,
                                _ => 0x18,
                            },
                            word_48: 0xffff,
                        },
                    )
                    .map_err(|_| Self::render_stage_fail("sksm-producer-state", EINVAL))?,
                registered: false,
            })
        };
        Ok(G17PRenderSksmQueues {
            scratch,
            uat_owner: self.uat().ttb_base(),
            tiling: build(
                pair.tiling as u8,
                g17_submission::G17P_TA_DATA_MASTER,
                true,
                entry_low_vas[0],
            )?,
            fragment: build(
                pair.fragment as u8,
                match *crate::module_parameters::g17p_render_3d_dm.value() {
                    0xff => g17_submission::G17P_3D_DATA_MASTER,
                    other => other,
                },
                false,
                entry_low_vas[1],
            )?,
        })
    }

    /// Publish one render group's SKSM half and return the KSM producer stamp
    /// its tag-14 AddKicks must announce.
    ///
    /// Ordering mirrors `submit_translated_first_bind_channel` exactly: the
    /// caller has already published the group's command record and tag-15
    /// ConfigUpdate pointers, this configures the QID on the bridge once, then
    /// writes the kick entry body, and the caller publishes tag-14 afterwards.
    fn publish_render_sksm_group(
        &mut self,
        dev: &AsahiDevice,
        registers: &regs::Resources,
        tiling: bool,
        queue_record_gpu_va: u64,
        barriers: &[g17_submission::G17PClBarrierDependency],
    ) -> Result<u64> {
        let ordinal = self.render_submission_ordinal;
        let auxiliary_timestamp = self.render_auxiliary_timestamp;
        if auxiliary_timestamp == 0 {
            return Err(Self::render_stage_fail("sksm-auxiliary-timestamp", EINVAL));
        }
        let scratch = self.sksm_scratch();
        let uat_owner = self.uat().ttb_base();
        let sksm_mode = *crate::module_parameters::g17p_render_sksm.value();
        let minimal = sksm_mode == 2;
        let native_shaped = sksm_mode == 3 || sksm_mode == 4;
        let storage = self.render_storage.as_ref().ok_or(ENODEV)?;
        let descriptor_high = storage.descriptor_high_va(tiling)?;
        let descriptor_low = storage.descriptor_low_va(tiling)?;
        let work_items = storage.work_item_gpu_vas(tiling)?;
        let mcache = if *crate::module_parameters::g17p_render_ksm_mcache.value() == 0 {
            None
        } else if tiling {
            self.render_storage.as_ref().ok_or(ENODEV)?.tiling_mcache()
        } else {
            self.render_storage.as_ref().ok_or(ENODEV)?.fragment_mcache()
        };
        let empty_binding = g17_submission::G17PClRceBinding { address: 0, tag: 0 };
        let command_state = if minimal {
            g17_submission::G17PComputeClCommandState {
                event_mask: [0; 4],
                rce_bindings: [empty_binding; 4],
            }
        } else if native_shaped {
            g17_submission::prepare_g17p_render_cl_command_state(
                descriptor_low,
                tiling,
            )
                .map_err(|_| Self::render_stage_fail("sksm-render-command-state", EINVAL))?
        } else {
            g17_submission::prepare_g17p_compute_cl_command_state(descriptor_low, ordinal)
                .map_err(|_| Self::render_stage_fail("sksm-command-state", EINVAL))?
        };

        let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
        let queues = self.render_sksm.as_mut().ok_or(ENODEV)?;
        if queues.scratch != scratch || queues.uat_owner != uat_owner {
            let scratch_differs = queues.scratch != scratch;
            let recorded_uat = queues.uat_owner;
            pr_info!(
                "G17P render: SKSM mismatch scratch_differs={} uat {:#x} vs {:#x}\n",
                scratch_differs,
                recorded_uat,
                uat_owner
            );
            return Err(Self::render_stage_fail("sksm-scratch-uat-mismatch", EINVAL));
        }
        let queue = if tiling {
            &mut queues.tiling
        } else {
            &mut queues.fragment
        };
        if queue.tiling != tiling {
            return Err(Self::render_stage_fail("sksm-queue-role-mismatch", EINVAL));
        }
        let prepared = g17_submission::prepare_g17p_cl_kick_entry(
            queue.producer,
            g17_submission::G17PClKickEntryOperands {
                descriptor_flag_4c: !tiling
                    && (*crate::module_parameters::g17p_render_header_flags.value() & 1 != 0),
                descriptor_flag_5c8:
                    *crate::module_parameters::g17p_render_header_flags.value() & 2 != 0,
                converted_command_timestamp: queue.producer.current_timestamp,
                barriers,
                mcache,
                payload: [descriptor_high, queue_record_gpu_va],
                event_mask: command_state.event_mask,
                rce_kind: 0,
                rce_bindings: command_state.rce_bindings,
                auxiliary: auxiliary_timestamp,
            },
        )
        .map_err(|_| Self::render_stage_fail("sksm-kick-entry", EINVAL))?;
        if !queue.registered {
            registers
                .g17p_sksm_configure_queue(queue.configure_pair, queue.enable_pair)
                .map_err(|error| Self::render_stage_fail("sksm-configure-queue", error))?;
            queue.registered = true;
        }
        storage
            .write_sksm_entry(
                tiling,
                prepared.entry_offset as usize,
                prepared.zero_length as usize,
                &prepared.entry,
            )
            .map_err(|error| Self::render_stage_fail("sksm-write-entry", error))?;
        fence(Ordering::SeqCst);
        let stamp = prepared.current_timestamp;
        queue.producer = prepared.producer_after;
        queue.producer.last_add_kicks_timestamp = stamp;
        dev_info!(
            dev.as_ref(),
            "G17P render SKSM: {} qid {} entry_offset {:#x} entry_low {:#x} descriptor=[{:#x},{:#x}] items=[{:#x},{:#x},{:#x}] stamp {:#x} barriers {} mcache-ranges {} submission {}\n",
            if tiling { "TA" } else { "3D" },
            queue.queue_id,
            prepared.entry_offset,
            storage.sksm_entry_low_va(tiling),
            descriptor_high,
            descriptor_low,
            work_items[0],
            work_items[1],
            work_items[2],
            stamp,
            barriers.len(),
            mcache.map_or(0, |value| value.count),
            ordinal
        );
        Ok(stamp)
    }

    fn stage_render_groups_with_sksm(
        &mut self,
        dev: &AsahiDevice,
        registers: &regs::Resources,
    ) -> Result<(
        u64,
        u64,
        g17_resources::G17PUserRenderQueueState,
        Option<g17_resources::G17PLateTilingPublication>,
    )> {
        let ordinal = self.render_submission_ordinal;
        let channel_payload = g17_submission::g17p_serialized_render_scratch_payload(ordinal);
        let fragment_command_timestamp = self
            .render_sksm
            .as_ref()
            .ok_or(ENODEV)?
            .fragment
            .producer
            .current_timestamp;
        let tiling_command_timestamp = self.render_sksm.as_ref().ok_or(ENODEV)?
            .tiling.producer.current_timestamp;
        let (fragment_queue, tiling_queue) = {
            let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
            let scratch = storage
                .stage_fragment_completion_scratch(
                    tiling_command_timestamp, fragment_command_timestamp, channel_payload,
                )
                .inspect_err(|error| {
                    dev_err!(
                        dev.as_ref(),
                        "G17P render: fragment completion scratch stage failed ({:?})\n",
                        error
                    );
                })?;
            dev_info!(
                dev.as_ref(),
                "G17P render: 3D completion scratch={:#018x} timestamp={:#x} payload={}\n",
                scratch,
                fragment_command_timestamp,
                channel_payload,
            );
            storage
                .dump_submit_ready_fragment_descriptor(dev, ordinal)
                .inspect_err(|error| {
                    dev_err!(
                        dev.as_ref(),
                        "G17P render: submit-ready fragment descriptor dump failed ({:?})\n",
                        error
                    );
                })?;
            storage.stage_fragment_prefix(ordinal).inspect_err(|error| {
                dev_err!(
                    dev.as_ref(),
                    "G17P render: fragment prefix stage failed ({:?})\n",
                    error
                );
            })?;
            storage.queue_gpu_vas()
        };
        let (ta_stamp, ta_queue_id) = {
            let queues = self.render_sksm.as_ref().ok_or(ENODEV)?;
            (
                queues.tiling.producer.current_timestamp,
                queues.tiling.queue_id,
            )
        };
        let split_launch =
            *crate::module_parameters::g17p_render_split_launch.value() != 0;
        let native_fragment_first = g17p_native_fragment_first_mode(
            *crate::module_parameters::g17p_render_native_doorbells.value(),
        );
        if !split_launch {
            let event_word = u32::try_from(ta_stamp)
                .ok()
                .and_then(|stamp| stamp.checked_mul(0x100))
                .ok_or_else(|| Self::render_stage_fail("sksm-event-word", EINVAL))?;
            let barrier = [g17_submission::G17PClBarrierDependency {
                event_word,
                shared_stamp: 0,
                queue_id: ta_queue_id,
            }];
            let suppress_barrier =
                *crate::module_parameters::g17p_render_no_barrier.value() != 0;
            let barrier_slice: &[g17_submission::G17PClBarrierDependency] =
                if suppress_barrier { &[] } else { &barrier };
            if suppress_barrier {
                pr_info!(
                    "G17P render: DIAGNOSTIC barrier dependency suppressed (event_word {:#x} qid {})\n",
                    event_word,
                    ta_queue_id
                );
            }
            let fragment_stamp = self
                .publish_render_sksm_group(dev, registers, false, fragment_queue, barrier_slice)
                .map_err(|error| Self::render_stage_fail("sksm-publish-fragment", error))?;
            let fragment_stamp = u32::try_from(fragment_stamp)
                .map_err(|_| Self::render_stage_fail("sksm-fragment-stamp-width", EINVAL))?;
            self.render_storage
                .as_mut()
                .ok_or(ENODEV)?
                .stage_fragment_kick(ordinal, fragment_stamp)
                .inspect_err(|error| {
                    dev_err!(
                        dev.as_ref(),
                        "G17P render: fragment AddKicks stage failed ({:?})\n",
                        error
                    );
                })?;
        } else {
            dev_info!(
                dev.as_ref(),
                "G17P render split-launch: retaining 3D SKSM entry and AddKicks until TA completion\n"
            );
        }
        let previous = self
            .render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .snapshot()
            .inspect_err(|error| {
                dev_err!(
                    dev.as_ref(),
                    "G17P render: initial snapshot failed ({:?})\n",
                    error
                );
            })?;
        if native_fragment_first {
            // Modes 2/3 return with a COMPLETE 3D group but no TA item/SKSM
            // publication. The caller retains the TA outer producer, sends
            // the 3D doorbell, and invokes `publish_native_render_tiling_group`
            // to construct TA in its exact tag15 -> SKSM -> tag14 chronology.
            let queues = self.render_sksm.as_ref().ok_or(ENODEV)?;
            if queues.tiling.producer.current_timestamp != ta_stamp {
                return Err(Self::render_stage_fail(
                    "native-ta-sksm-advanced-before-3d-doorbell",
                    EINVAL,
                ));
            }
            dev_info!(
                dev.as_ref(),
                "G17P render native chronology: complete 3D visible; retaining complete TA group until after Fragment doorbell\n"
            );
            return Ok((fragment_queue, tiling_queue, previous, None));
        }
        let tiling_stamp = self
            .publish_render_sksm_group(dev, registers, true, tiling_queue, &[])
            .map_err(|error| Self::render_stage_fail("sksm-publish-tiling", error))?;
        // The barrier the 3D kick already carries names this exact value.
        if tiling_stamp != ta_stamp {
            pr_info!(
                "G17P render: SKSM tiling stamp {} != barrier stamp {}\n",
                tiling_stamp,
                ta_stamp
            );
            return Err(Self::render_stage_fail("sksm-tiling-stamp-mismatch", EINVAL));
        }
        let tiling_stamp = u32::try_from(tiling_stamp)
            .map_err(|_| Self::render_stage_fail("sksm-tiling-stamp-width", EINVAL))?;
        let late_tiling = self
            .render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .prepare_late_tiling_publication_with_stamp(ordinal, tiling_stamp)
            .inspect_err(|error| {
                dev_err!(dev.as_ref(), "G17P render: tiling stage failed ({:?})\n", error);
            })?;
        Ok((fragment_queue, tiling_queue, previous, Some(late_tiling)))
    }

    pub(crate) fn begin_translated_render(
        &mut self,
        dev: &AsahiDevice,
        vm: &mmu::Vm,
        queue_pair: g17_resources::G17PRenderQueuePair,
        execution_context: Arc<mmu::T8140ComputeExecutionContext>,
        command: &g17_uapi::TranslatedRenderCommand,
        user_timestamps: [u64; 4],
        user_timestamp_aliases: KVec<mmu::KernelMapping>,
        registers: &regs::Resources,
    ) -> Result<g17_submission::PreparedG17PPartialOpeningWorkPair> {
        self.select_render_slot(queue_pair.slot())?;
        let descriptor_context_id = execution_context.context_id() as u16;
        if self.render_tracker.is_some() || self.render_hardware_buffer_id.is_some() {
            return Err(EBUSY);
        }
        if g17_resources::g17p_render_tvb_mode() >= 2 {
            let required=g17_resources::required_render_tvb_blocks(&command.parameters,self.num_clusters).ok_or(EINVAL)?;
            if self.render_storage.as_ref().is_some_and(|storage| storage.tvb_capacity()<required) {
                self.require_retired_render_owner()?;
                self.resources.grow_render_tvb(self.render_storage.as_mut().ok_or(EIO)?,vm,required)?;
            }
        }
        self.render_auxiliary_timestamp = G17P_NATIVE_RENDER_KICK_AUXILIARY;
        let (hardware_buffer_id, parameter_buffer_token) = self
            .render_hardware_buffer_ids
            .allocate_with_token()
            .map_err(|_| EBUSY)?;
        self.render_hardware_buffer_id = Some(hardware_buffer_id);
        self.render_hardware_buffer_published = false;

        let mut command = *command;
        command.parameters.ta_hardware_buffer_id = hardware_buffer_id;
        dev_info!(
            dev.as_ref(),
            "G17P render: allocated TA HardwareBufferID {} for ordinal {}\n",
            hardware_buffer_id,
            self.render_submission_ordinal,
        );
        let result = self.begin_translated_render_with_hardware_buffer(
            dev,
            vm,
            queue_pair,
            execution_context,
            descriptor_context_id,
            &command,
            parameter_buffer_token,
            user_timestamps,
            user_timestamp_aliases,
            registers,
        );
        if result.is_err() && !self.render_hardware_buffer_published {
            // acquire() stages a copy of the completed FList owner. Until
            // outer publication it holds no firmware reference and must be
            // cancelled with the HardwareBufferID reservation. Otherwise a
            // fallible PM allocation leaves every later VM handoff EBUSY.
            self.prepared_render_flist = None;
            self.render_auxiliary_timestamp = 0;
            let id = self.render_hardware_buffer_id.take().ok_or(EIO)?;
            self.render_hardware_buffer_ids.release(id).map_err(|_| EIO)?;
        }
        result
    }

    fn begin_translated_render_with_hardware_buffer(
        &mut self,
        dev: &AsahiDevice,
        vm: &mmu::Vm,
        queue_pair: g17_resources::G17PRenderQueuePair,
        execution_context: Arc<mmu::T8140ComputeExecutionContext>,
        descriptor_context_id: u16,
        command: &g17_uapi::TranslatedRenderCommand,
        parameter_buffer_token: u64,
        user_timestamps: [u64; 4],
        user_timestamp_aliases: KVec<mmu::KernelMapping>,
        registers: &regs::Resources,
    ) -> Result<g17_submission::PreparedG17PPartialOpeningWorkPair> {
        let render_sksm = *crate::module_parameters::g17p_render_sksm.value();
        let native_doorbell_mode =
            *crate::module_parameters::g17p_render_native_doorbells.value();
        let split_launch = *crate::module_parameters::g17p_render_split_launch.value() != 0;
        if native_doorbell_mode > 3
            || (native_doorbell_mode != 0 && split_launch)
            || (g17p_native_fragment_first_mode(native_doorbell_mode) && render_sksm == 0)
        {
            return Err(EINVAL);
        }
        // Both render paths publish tag-16 when it is armed -- the SKSM
        // staging path through `stage_fragment_kick`, and the plain path
        // through `stage_fragment`. The absolute prefix includes all earlier
        // groups in the retained ring; it is not merely this group's 3/4
        // items. The same target feeds both outer slots and the tracker.
        let published_prefix =
            g17_resources::g17p_render_published_prefix(self.render_submission_ordinal)?;
        if self.render_tracker.is_some() {
            return Err(EBUSY);
        }
        self.prepared_render_flist =
            if *crate::module_parameters::g17p_render_usc_freelist.value() != 0 {
                Some(G17PBootstrapUmaLifecycle::acquire(
                    self.bootstrap_uma.as_ref(),
                    self.next_render_flist_sequence,
                ).map_err(|_| EBUSY)?)
            } else {
                None
            };
        self.resources
            .stage_render_region_views()
            .map_err(|error| Self::render_stage_fail("stage-region-views", error))?;
        self.resources
            .stage_render_flist_state()
            .map_err(|error| Self::render_stage_fail("stage-render-flist", error))?;
        // The render storage owns the complete SecureGart low-view closure in
        // its application-GART root.  User work must never repoint the global
        // context-0 lower VM; that singleton was the fundamental cross-client
        // exclusion and made a second independently rooted render impossible.
        if self.render_context_aliases.is_some() {
            return Err(Self::render_stage_fail("legacy-context0-alias-live", EBUSY));
        }
        let retained = self.render_storage.is_some();
        // Which construction path a render takes has never been logged, and a
        // cold boot was measured taking the RESTAGE branch on what should be a
        // first render (93cc292: neither log line inside new_user_render_storage
        // appears cold, so the UMA page-pool descriptor is never staged, while
        // the tiler parameter-management graph that faults binds that pool).
        // Name the branch and its inputs so the two readings -- genuinely
        // retained, versus construction attempted and abandoned -- separate in
        // one run.
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P render storage path: retained={} ordinal={} bind={} tracker={}\n",
            retained,
            self.render_submission_ordinal,
            self.render_bind.is_some(),
            self.render_tracker.is_some(),
        );
        if retained
            && !self
                .render_bind
                .as_ref()
                .is_some_and(|binding| binding.matches(vm))
        {
            return Err(EBUSY);
        }
        if retained {
            let parameter_page_list_vas = self.resources.parameter_buffer_page_list_vas();
            self.render_storage
                .as_mut()
                .ok_or(ENODEV)?
                .restage_command(
                    dev,
                    self.resources.uat(),
                    vm,
                    descriptor_context_id,
                    parameter_page_list_vas,
                    command,
                    self.num_clusters,
                    user_timestamps,
                    Some(user_timestamp_aliases),
                    self.render_submission_ordinal,
                )
                .map_err(|error| Self::render_stage_fail("restage-command", error))?;
            // The retained PM owner validates its unchanged PBDesc identity.
            // Firmware owns the live page-list/PBDesc cursors; do not reset
            // them to cold values when preparing another Scene. The cold
            // new_user_render_storage path below still initializes them.
        } else {
            if self.render_bind.is_some() {
                return Err(EIO);
            }
            self.render_storage = Some(
                self.resources
                    .new_user_render_storage(
                        dev,
                        vm,
                        queue_pair,
                        Some(execution_context.clone()),
                        descriptor_context_id,
                        command,
                        self.num_clusters,
                        user_timestamps,
                        user_timestamp_aliases,
                        self.render_submission_ordinal,
                    )
                    .map_err(|error| Self::render_stage_fail("new-user-render-storage", error))?,
            );
            if self.render_submission_ordinal != 0 {
                let parameter_page_list_vas = self.resources.parameter_buffer_page_list_vas();
                let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
                storage
                    .rebase_empty_queue_cursors(self.render_submission_ordinal)
                    .map_err(|error| Self::render_stage_fail("rebase-new-vm-queue", error))?;
                storage.restage_command(
                        dev,
                        self.resources.uat(),
                        vm,
                        descriptor_context_id,
                        parameter_page_list_vas,
                        command,
                        self.num_clusters,
                        user_timestamps,
                        None,
                        self.render_submission_ordinal,
                    )
                    .map_err(|error| Self::render_stage_fail("restage-new-vm-command", error))?;
            }
        }
        {
            let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
            storage
                .set_parameter_buffer_lease_token(parameter_buffer_token)
                .map_err(|error| Self::render_stage_fail("set-pb-lease-token", error))?;
            if let Some(failed) = storage.first_unreachable(self.resources.uat(), vm) {
                storage.log_reachability(dev, self.resources.uat(), vm);
                dev_err!(
                    dev.as_ref(),
                    "G17P render: storage unreachable, first failing predicate is {}\n",
                    failed
                );
                return Err(EFAULT);
            }
        }
        if render_sksm != 0 && self.render_sksm.is_none() {
            let (pair, entry_low_vas) = {
                let storage = self.render_storage.as_ref().ok_or(ENODEV)?;
                (
                    storage.queue_pair(),
                    [storage.sksm_entry_low_va(true), storage.sksm_entry_low_va(false)],
                )
            };
            self.render_sksm = Some(
                self.new_render_sksm_queues(pair, entry_low_vas)
                    .map_err(|error| Self::render_stage_fail("new-render-sksm-queues", error))?,
            );
        }
        let (fragment_queue, tiling_queue, previous, late_tiling) = if render_sksm != 0 {
            self.stage_render_groups_with_sksm(dev, registers)
                .map_err(|error| Self::render_stage_fail("stage-render-groups-sksm", error))?
        } else {
            let storage = self.render_storage.as_mut().ok_or(ENODEV)?;
            storage
                .stage_fragment(self.render_submission_ordinal)
                .inspect_err(|error| {
                    dev_err!(dev.as_ref(), "G17P render: fragment stage failed ({:?})\n", error);
                })?;
            let previous = storage.snapshot().inspect_err(|error| {
                dev_err!(dev.as_ref(), "G17P render: initial snapshot failed ({:?})\n", error);
            })?;
            let late_tiling = storage
                .prepare_late_tiling_publication(self.render_submission_ordinal)
                .inspect_err(|error| {
                    dev_err!(dev.as_ref(), "G17P render: tiling stage failed ({:?})\n", error);
                })?;
            let (fragment_queue, tiling_queue) = storage.queue_gpu_vas();
            (fragment_queue, tiling_queue, previous, Some(late_tiling))
        };
        // What render's tag-15 ACTUALLY carries, both queues, every submit.
        // `+0x1a` is the queue-install flag: firmware RE says the per-queue slot
        // `0x127628 + qid*0x28` is written -- and the `0x40 tag15-programmed`
        // KTrace receipt emitted -- only when it is non-zero. Render's value has
        // been taken on trust from the spec; print it so it is a measurement.
        let fragment_header = self
            .render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .fragment_config_update_header()
            .map_err(|error| Self::render_stage_fail("decode-fragment-tag15", error))?;
        let tiling_header = late_tiling
            .as_ref()
            .and_then(|publication| publication.config_update_header());
        dev_info!(
            dev.as_ref(),
            "G17P render tag15: 3D selector {:#x} qid {} install {} dm {} gfx {} ctx {} flush-gen {:#x} flushid-index {} scheduler-state {:#x} +0x56 {} entry {:#x}/{:#x} pb={:#x}/{:#x}/id{}/bit{}\n",
            fragment_header.selector,
            fragment_header.queue_id,
            fragment_header.install,
            fragment_header.data_master,
            fragment_header.graphics,
            fragment_header.context_id,
            fragment_header.flush_generation,
            fragment_header.flushid_index,
            fragment_header.scheduler_state,
            fragment_header.field_56,
            fragment_header.entry_low,
            fragment_header.entry_high,
            fragment_header.parameter_buffer_object,
            fragment_header.parameter_buffer_token,
            fragment_header.parameter_buffer_id,
            fragment_header.parameter_buffer_bit,
        );
        if let Some(header) = tiling_header {
            dev_info!(
                dev.as_ref(),
                "G17P render tag15: TA selector {:#x} qid {} install {} dm {} gfx {} ctx {} flush-gen {:#x} flushid-index {} scheduler-state {:#x} +0x56 {} entry {:#x}/{:#x} pb={:#x}/{:#x}/id{}/bit{}\n",
                header.selector,
                header.queue_id,
                header.install,
                header.data_master,
                header.graphics,
                header.context_id,
                header.flush_generation,
                header.flushid_index,
                header.scheduler_state,
                header.field_56,
                header.entry_low,
                header.entry_high,
                header.parameter_buffer_object,
                header.parameter_buffer_token,
                header.parameter_buffer_id,
                header.parameter_buffer_bit,
            );
        }

        let fragment_qos = self
            .resources
            .publish_render_qos_queue_record(fragment_header)
            .map_err(|error| Self::render_stage_fail("publish-fragment-qos", error))?;
        dev_info!(
            dev.as_ref(),
            "G17P render QoS: 3D qid {} record {:?}\n",
            fragment_header.queue_id,
            fragment_qos,
        );
        if let Some(header) = tiling_header {
            let tiling_qos = self
                .resources
                .publish_render_qos_queue_record(header)
                .map_err(|error| Self::render_stage_fail("publish-tiling-qos", error))?;
            dev_info!(
                dev.as_ref(),
                "G17P render QoS: TA qid {} record {:?}\n",
                header.queue_id,
                tiling_qos,
            );
        }
        let pm_submission = self.render_storage.as_mut().ok_or(ENODEV)?
            .prepare_pm_submitted_operation()
            .map_err(|error| Self::render_stage_fail("prepare-pm-submitted-operation", error))?;
        let pm_submitted_count = pm_submission.as_ref().map(|operation| operation.next());
        // The TA/3D descriptors execute in their independently allocated
        // application context, but the retained USC FList service still
        // fetches its run table through the legacy application-GART slot.
        // Keep the same VM rooted there for the lifetime of this render graph.
        let (prepared, render_bind) = self
            .resources
            .publish_partial_opening_pair(
                if retained { None } else { Some(vm) },
                retained,
                queue_pair,
                g17p_render_deferred_outer_mode(native_doorbell_mode, split_launch),
                fragment_queue,
                published_prefix,
                tiling_queue,
                published_prefix,
                self.render_submission_ordinal == 0,
                move || {
                    if let Some(operation) = pm_submission {
                        operation.commit();
                    }
                },
                move || {
                    if let Some(publication) = late_tiling {
                        publication.publish();
                    }
                },
            )
            .map_err(|error| Self::render_stage_fail("publish-partial-opening-pair", error))?;
        dev_info!(dev.as_ref(), "G17P render PM: submitted-operations committed {:?}\n", pm_submitted_count);
        // The ID is now firmware/GPU-owned. Any later validation failure must
        // retain it until semantic completion or a stopped-CPU teardown.
        self.render_hardware_buffer_published = true;
        pr_info!(
            "G17P render: outer publish OK, retained={} render_bind={}\n",
            retained,
            render_bind.is_some()
        );
        if retained {
            if render_bind.is_some() {
                return Err(Self::render_stage_fail("retained-bind-unexpected", EINVAL));
            }
        } else {
            let flist_bind = render_bind
                .ok_or_else(|| Self::render_stage_fail("job-flist-bind-missing", EINVAL))?;
            self.render_bind = Some(G17PRenderRootOwner::Job {
                context: execution_context,
                flist_bind,
            });
        }
        let roots = self.resources.uat().t8140_compute_context_roots();
        dev_info!(
            dev.as_ref(),
            "G17P render context roots pre-kick: ctx0=[{:#x},{:#x}] ctx1=[{:#x},{:#x}] ctx2=[{:#x},{:#x}] ctx3=[{:#x},{:#x}]\n",
            roots[0].0,
            roots[0].1,
            roots[1].0,
            roots[1].1,
            roots[2].0,
            roots[2].1,
            roots[3].0,
            roots[3].1,
        );
        for offset in [0x1e00u64, 0x1f80u64] {
            let (canonical, client) = vm
                .t8140_client_null_view_probe(offset)
                .map_err(|error| {
                    dev_err!(
                        dev.as_ref(),
                        "G17P render client null view: +{:#x} does not translate ({:?})\n",
                        offset,
                        error,
                    );
                    Self::render_stage_fail("client-null-view-unmapped", error)
                })?;
            if canonical != client {
                dev_err!(
                    dev.as_ref(),
                    "G17P render client null view: +{:#x} client PA {:#x} != canonical PA {:#x}\n",
                    offset,
                    client,
                    canonical,
                );
                return Err(Self::render_stage_fail("client-null-view-mismatch", EFAULT));
            }
            dev_info!(
                dev.as_ref(),
                "G17P render client null view: +{:#x} -> PA {:#x}, identical to canonical {:#x}+{:#x}\n",
                offset,
                client,
                0x0010_0000_0000u64,
                offset,
            );
        }
        self.render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .audit_fragment_rce_mapping(dev, vm)
            .map_err(|error| Self::render_stage_fail("fragment-rce-vm-audit", error))?;
        if g17p_native_fragment_first_mode(native_doorbell_mode) {
            fence(Ordering::Acquire);
            let current = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
            let outer = self.resources.partial_opening_outer_counters()?;
            let fragment_sksm_visible = self
                .render_sksm
                .as_ref()
                .is_some_and(|queues| queues.fragment.producer.previous_valid);
            if !fragment_sksm_visible
                || !g17p_native_fragment_only_visible(
                    previous.tiling.write,
                    current.tiling.write,
                    current.fragment.write,
                    published_prefix,
                    outer[0][2],
                    prepared.fragment.next_producer,
                    outer[1][2],
                    prepared.tiling.slot_index,
                )
            {
                return Err(Self::render_stage_fail(
                    "native-fragment-only-visibility",
                    EINVAL,
                ));
            }
        }
        self.render_tracker = Some(G17PPartialOpeningRenderTracker {
            tiling_previous: previous.tiling,
            fragment_previous: previous.fragment,
            tiling_prefix: published_prefix,
            fragment_prefix: published_prefix,
        });
        Ok(prepared)
    }

    /// Expose the complete default-profile render pair after the USC FList
    /// update/grow has retired and before the one paired work doorbell.
    pub(crate) fn publish_default_render_pair_after_freelist(
        &mut self,
        prepared: &g17_submission::PreparedG17PPartialOpeningWorkPair,
    ) -> Result {
        if *crate::module_parameters::g17p_render_native_doorbells.value() != 0
            || *crate::module_parameters::g17p_render_split_launch.value() != 0
            || self.render_tracker.is_none()
            || self.render_storage.is_none()
            || self.render_bind.is_none()
        {
            return Err(EINVAL);
        }
        self.resources.publish_deferred_partial_opening_pair(prepared)
    }

    /// Split launch's initial phase: expose only the complete TA outer slot
    /// after the same first-submit FList transaction has retired. The fragment
    /// prefix stays hidden until TA completion publishes its SKSM/AddKicks and
    /// matching deferred outer slot as one later transaction.
    pub(crate) fn publish_split_render_tiling_after_freelist(
        &mut self,
        plan: &g17_submission::G17PPartialOpeningOuterSlotPlan,
    ) -> Result {
        if *crate::module_parameters::g17p_render_native_doorbells.value() != 0
            || *crate::module_parameters::g17p_render_split_launch.value() == 0
            || self.render_tracker.is_none()
            || self.render_storage.is_none()
            || self.render_bind.is_none()
        {
            return Err(EINVAL);
        }
        self.resources.publish_deferred_partial_opening_tiling(plan)
    }

    /// DRAM-only status-page sample; safe at any point, timeout included.
    /// Compute's status page, read exactly as render's is. Same VA.
    pub(crate) fn log_compute_status_pages(&mut self, label: &str) {
        self.resources.log_compute_status_pages(label);
    }

    pub(crate) fn log_compute_sksm_entries(
        &mut self,
        queue: &mut G17PSksmQueue,
        label: &str,
    ) {
        let dev = self.resources.dev().clone();
        let stride = queue.producer.geometry.entry_stride() as usize;
        queue.storage.log_entry(label, &dev, 0);
        queue.storage.log_entry(label, &dev, stride);
    }

    /// DIAGNOSTIC. Release the fragment's parent link (see
    /// `g17p_release_fragment_parent`).
    pub(crate) fn release_fragment_parent(&mut self, index: usize) -> Result<(u32, u32)> {
        self.resources.g17p_release_fragment_parent(index)
    }

    pub(crate) fn log_render_sksm_entries(&mut self, label: &str) {
        // DIAGNOSTIC ONLY, and it must stay that way. This runs from the
        // render submit path at "pre-doorbell" -- i.e. immediately before the
        // doorbell is rung -- and hexdumps roughly 19 KB per call once the two
        // descriptor regions grew to 0x2400. Leaving that in the default path
        // put a printk storm between building the kick and ringing it, and the
        // render came back VK_ERROR_DEVICE_LOST on BOTH the cold and the warm
        // path. Gate it exactly like log_render_status_pages just below.
        if *crate::module_parameters::g17p_dep_records.value() == 0 {
            return;
        }
        let strides = match self.render_sksm.as_ref() {
            Some(queues) => [
                queues.tiling.producer.geometry.entry_stride() as usize,
                queues.fragment.producer.geometry.entry_stride() as usize,
            ],
            None => [0usize, 0usize],
        };
        let dev = self.resources.dev().clone();
        if let Some(storage) = self.render_storage.as_mut() {
            storage.log_render_queue_record(&dev, true, label);
            storage.log_render_queue_record(&dev, false, label);
        }
        let dumpdev = self.resources.dev().clone();
        if let Some(storage) = self.render_storage.as_mut() {
            storage.dump_graph_region(&dumpdev, "opt-TA", label, 0x3000, 0xc0);
            storage.dump_graph_region(&dumpdev, "opt-3D", label, 0x30c0, 0xc0);
            storage.dump_graph_region(&dumpdev, "evt-TA", label, 0x3200, 0x40);
            storage.dump_graph_region(&dumpdev, "evt-3D", label, 0x3600, 0x40);
            storage.dump_descriptor_region(&dumpdev, "desc-TA", label, 0x0000, 0x2400);
            storage.dump_descriptor_region(&dumpdev, "desc-3D", label, 0x4000, 0x2400);
            storage.dump_descriptor_region(&dumpdev, "kind-TA", label, 0x0840, 0x40);
            storage.dump_descriptor_region(&dumpdev, "kind-3D", label, 0x4840, 0x40);
            storage.dump_descriptor_region(&dumpdev, "link-TA", label, 0x0780, 0x40);
        }
        if let Some(storage) = self.render_storage.as_mut() {
            storage.dump_sksm_entry(&dumpdev, true, label);
            storage.dump_sksm_entry(&dumpdev, false, label);
            storage.log_sksm_entry(&dumpdev, true, label, 0);
            storage.log_sksm_entry(&dumpdev, true, label, strides[0]);
            storage.log_sksm_entry(&dumpdev, true, label, strides[0] + 0x120);
            storage.log_sksm_entry(&dumpdev, false, label, 0);
            storage.log_sksm_entry(&dumpdev, false, label, strides[1]);
            storage.log_sksm_entry(&dumpdev, false, label, strides[1] + 0x120);
        }
    }

    pub(crate) fn log_render_status_pages(&mut self, label: &str) {
        if *crate::module_parameters::g17p_dep_records.value() != 0 {
            let dev = self.resources.dev().clone();
            {
                let phys = G17P_GFX_PHYSICAL_DATA_BASE
                    + (G17P_GDIRECTOR_SHADOW_LINK_BASE - G17P_GFX_LINK_DATA_BASE);
                match pgtable::LiveFirmwareU64Probe::new(phys)
                    .and_then(|p| p.observe_mask(u64::MAX, 1))
                    .map(|o| o.first)
                {
                    Ok(v) => dev_info!(
                        dev.as_ref(),
                        "G17P gdirector-shadow[{}] link={:#x} raw={:#018x} hwbuf0={{prio {:#04x},share {:#04x}}} hwbuf1={{{:#04x},{:#04x}}} hwbuf2={{{:#04x},{:#04x}}} hwbuf3={{{:#04x},{:#04x}}}\n",
                        label,
                        G17P_GDIRECTOR_SHADOW_LINK_BASE,
                        v,
                        v & 0xff, (v >> 8) & 0xff,
                        (v >> 16) & 0xff, (v >> 24) & 0xff,
                        (v >> 32) & 0xff, (v >> 40) & 0xff,
                        (v >> 48) & 0xff, (v >> 56) & 0xff,
                    ),
                    Err(e) => dev_warn!(
                        dev.as_ref(),
                        "G17P gdirector-shadow[{}] unreadable ({:?})\n",
                        label,
                        e
                    ),
                }
            }
            {
                let rd = |link: u64| -> Option<u64> {
                    let phys = G17P_GFX_PHYSICAL_DATA_BASE
                        + (link - G17P_GFX_LINK_DATA_BASE);
                    pgtable::LiveFirmwareU64Probe::new(phys)
                        .and_then(|p| p.observe_mask(u64::MAX, 1))
                        .map(|o| o.first)
                        .ok()
                };
                for dm in 0u64..3 {
                    let a = rd(G17P_DM_ACTIVE_BITMAP_LINK_BASE + dm * 0x10);
                    let b = rd(G17P_DM_ACTIVE_BITMAP_LINK_BASE + dm * 0x10 + 8);
                    dev_info!(
                        dev.as_ref(),
                        "G17P dm-active[{}] dm={} lo={:#018x} hi={:#018x}\n",
                        label,
                        dm,
                        a.unwrap_or(u64::MAX),
                        b.unwrap_or(u64::MAX),
                    );
                }
                dev_info!(
                    dev.as_ref(),
                    "G17P ready-mask[{}] = {:#018x}\n",
                    label,
                    rd(G17P_READY_MASK_LINK_BASE).unwrap_or(u64::MAX),
                );
                for qid in [
                    4u64,
                    u64::from(g17_resources::g17p_render_queue_id(true)),
                    u64::from(g17_resources::g17p_render_queue_id(false)),
                ] {
                    let park = G17P_KSM_PARK_RECORD_LINK_BASE + qid * 0x28;
                    let counters = rd(park + 0x418);
                    let deadline = rd(park + 0x400);
                    // Same record through the SECOND ASC's data window. The
                    // scheduler pass may not run on the ASC whose base the
                    // other probes here use, and a per-queue deadline that
                    // reads identically for every qid says this one does not.
                    let rd1 = |link: u64| -> Option<u64> {
                        let phys = G17P_GFX1_PHYSICAL_DATA_BASE
                            + (link - G17P_GFX1_LINK_DATA_BASE);
                        pgtable::LiveFirmwareU64Probe::new(phys)
                            .and_then(|p| p.observe_mask(u64::MAX, 1))
                            .map(|o| o.first)
                            .ok()
                    };
                    let counters1 = rd1(park + 0x418);
                    let deadline1 = rd1(park + 0x400);
                    let raw1 = counters1.unwrap_or(u64::MAX);
                    let dl1 = deadline1.unwrap_or(u64::MAX);
                    dev_info!(
                        dev.as_ref(),
                        "G17P park-gfx1[{}] qid={} raw={:#018x} deadline={:#018x}\n",
                        label,
                        qid,
                        raw1,
                        dl1,
                    );
                    let raw = counters.unwrap_or(u64::MAX);
                    let byte_a = raw & 0xff;
                    let byte_b = (raw >> 8) & 0xff;
                    let byte_c = (raw >> 16) & 0xff;
                    let parked = byte_a == byte_b;
                    let dl = deadline.unwrap_or(u64::MAX);
                    dev_info!(
                        dev.as_ref(),
                        "G17P park[{}] qid={} A={} B={} C={} A==B(park)={} raw={:#018x} deadline={:#018x}\n",
                        label,
                        qid,
                        byte_a,
                        byte_b,
                        byte_c,
                        parked,
                        raw,
                        dl,
                    );
                }
                for qid in [
                    4u64,
                    u64::from(g17_resources::g17p_render_queue_id(true)),
                    u64::from(g17_resources::g17p_render_queue_id(false)),
                ] {
                    let st = rd(G17P_QUEUE_STATE_LINK_BASE + qid * 0x28);
                    let st2 = rd(G17P_QUEUE_STATE_LINK_BASE + qid * 0x28 + 8);
                    let fl = rd(G17P_QUEUE_FLAG_LINK_BASE + qid * 0x28 + 0x40);
                    dev_info!(
                        dev.as_ref(),
                        "G17P qstate[{}] qid={} +0={:#018x} +8={:#018x} flag+0x40={:#018x}\n",
                        label,
                        qid,
                        st.unwrap_or(u64::MAX),
                        st2.unwrap_or(u64::MAX),
                        fl.unwrap_or(u64::MAX),
                    );
                }
            }
            for qid in [
                4u8,
                g17_resources::g17p_render_queue_id(true) as u8,
                g17_resources::g17p_render_queue_id(false) as u8,
            ] {
                let link = G17P_DEPENDENCY_RECORD_LINK_BASE
                    + u64::from(qid) * G17P_DEPENDENCY_RECORD_STRIDE;
                let phys = G17P_GFX_PHYSICAL_DATA_BASE
                    + (link - G17P_GFX_LINK_DATA_BASE);
                let head = pgtable::LiveFirmwareU64Probe::new(phys)
                    .and_then(|p| p.observe_mask(u64::MAX, 1))
                    .map(|o| o.first);
                let stamp = pgtable::LiveFirmwareU64Probe::new(phys + 8)
                    .and_then(|p| p.observe_mask(u64::MAX, 1))
                    .map(|o| o.first);
                match (head, stamp) {
                    (Ok(head), Ok(stamp)) => dev_info!(
                        dev.as_ref(),
                        "G17P dep-record[{}] qid={} link={:#x} phys={:#x} head={:#018x} valid={} completed={:#018x}\n",
                        label,
                        qid,
                        link,
                        phys,
                        head,
                        head & 1,
                        stamp,
                    ),
                    _ => dev_warn!(
                        dev.as_ref(),
                        "G17P dep-record[{}] qid={} unreadable at phys={:#x}\n",
                        label,
                        qid,
                        phys,
                    ),
                }
            }
        }

        {
            let dev = self.resources.dev().clone();
            {
                let mut pa = [0u8; 0x200];
                if self
                    .render_storage
                    .as_mut()
                    .map(|s| s.read_pool_a_record(&mut pa).is_ok())
                    .unwrap_or(false)
                {
                    let r0 = u32::from_le_bytes([pa[0x94], pa[0x95], pa[0x96], pa[0x97]]);
                    let r1 = u32::from_le_bytes([
                        pa[0x194], pa[0x195], pa[0x196], pa[0x197],
                    ]);
                    let v1 = if r1 != 0 {
                        "rec1 QUEUED (job linked, nothing dequeues DM0)"
                    } else {
                        "rec1 NOT queued"
                    };
                    dev_info!(
                        dev.as_ref(),
                        "G17P pool-a[{}] rec0(+0x00={:02x?} +0x10={:02x?} +0x94={:#010x}) rec1(+0x00={:02x?} +0x10={:02x?} +0x94={:#010x} link={:02x?}) {}\n",
                        label,
                        &pa[0x00..0x08],
                        &pa[0x10..0x14],
                        r0,
                        &pa[0x100..0x108],
                        &pa[0x110..0x114],
                        r1,
                        &pa[0x198..0x1a8],
                        v1,
                    );
                }
            }
            let mut cc = [0u8; 0x40];
            let ok = self
                .render_storage
                .as_mut()
                .map(|s| s.read_channel_control(&mut cc).is_ok())
                .unwrap_or(false);
            if ok {
                dev_info!(
                    dev.as_ref(),
                    "G17P render channel-control[{}]: pid={:#04x} idx={:#04x} f2={:#04x} prio={:#04x} class={:#04x} qidmask={:02x?} hwbuf={:#04x}\n",
                    label,
                    cc[0],
                    cc[1],
                    cc[2],
                    cc[3],
                    cc[4],
                    &cc[0x12..0x1a],
                    cc[0x33],
                );
            }
        }

        self.resources.dump_render_qos_regions(label);
        if *crate::module_parameters::g17p_dump_state.value() != 0
            && !G17P_STATE_DUMPED.swap(true, core::sync::atomic::Ordering::Relaxed)
        {
            let kib = *crate::module_parameters::g17p_dump_state.value() as usize;
            if kib == 1 {
                self.resources.checksum_primary_state(label);
            } else {
                self.resources.dump_primary_state(label, kib * 1024);
            }
        }
        if *crate::module_parameters::g17p_dump_hwdata.value() != 0
            && !G17P_HW_DATA_DUMPED.swap(true, core::sync::atomic::Ordering::Relaxed)
        {
            self.resources.dump_hw_data(label);
        }
        self.resources.log_render_parameter_buffer_descriptor(
            label,
            self.render_hardware_buffer_id.unwrap_or(0),
        );
        let selected_metric = self
            .render_storage
            .as_ref()
            .ok_or(ENODEV)
            .and_then(|storage| storage.selected_scene_index())
            .and_then(|scene| self.resources.selected_parameter_metric(scene));
        let dev = self.resources.dev().clone();
        match selected_metric {
            Ok(value) => dev_info!(
                dev.as_ref(),
                "G17P render PM[{}]: actual-selected-metric={:#x}\n",
                label,
                value,
            ),
            Err(error) => dev_warn!(
                dev.as_ref(),
                "G17P render PM[{}]: actual selected metric read failed ({:?})\n",
                label,
                error,
            ),
        }
        if let Some(storage) = self.render_storage.as_mut() {
            match storage.render_activation_coordinates() {
                Ok((ta_qid, ta_stamp, sku_reserved, sku_pointer, sku_header)) => dev_info!(
                    dev.as_ref(),
                    "G17P render activation[{}]: TA-link qid={} stamp={:#x} predecessor={:#x}; 3D-SKU reserved={:#x} stream={:#x} header={:#x}\n",
                    label,
                    ta_qid,
                    ta_stamp,
                    ta_stamp.wrapping_sub(1) & ((1u64 << 40) - 1),
                    sku_reserved,
                    sku_pointer,
                    sku_header,
                ),
                Err(error) => dev_warn!(
                    dev.as_ref(),
                    "G17P render activation[{}]: descriptor coordinates unreadable ({:?})\n",
                    label,
                    error,
                ),
            }
            match storage.render_activation_gate_words() {
                Ok(gate) => dev_info!(
                    dev.as_ref(),
                    "G17P render activation gate[{}]: TA(+7a4,+866,+892,+8ba,+8cb)={:x?}\n",
                    label,
                    gate,
                ),
                Err(error) => dev_warn!(
                    dev.as_ref(),
                    "G17P render activation gate[{}]: descriptor inputs unreadable ({:?})\n",
                    label,
                    error,
                ),
            }
            storage.log_status_pages(&dev, label);
            storage.log_parameter_management(&dev, label);
        }
    }

    /// Snapshot every host-DRAM render progress marker while firmware is
    /// halted at recovery state 1, before the host acknowledges the restart.
    /// The recovery path retires both queue graphs and writes a synthetic 3D
    /// completion record, so the ordinary timeout dump is too late to tell a
    /// launch failure from recovery cleanup.
    /// Locate a reported MMU fault address inside the render VM's own tables.
    ///
    /// The G14X fault-address register is documented as VA >> 6, but that
    /// reading and the raw one cannot be told apart from a fault inside the
    /// first page. Report both candidates against the client root: whether the
    /// VM maps the page, and every mapped run within +/-1 MiB of it. A
    /// candidate that abuts or falls just outside a live run is the real
    /// address; one that lands in empty space at every scale is not.
    pub(crate) fn log_fault_address_candidates(&mut self, label: &'static str, addr_word: u64) {
        let Some(bind) = self.render_bind.as_ref() else {
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P fault address[{}]: no retained render bind to walk\n",
                label,
            );
            return;
        };
        let vm = bind.vm();
        let dev = self.resources.dev().clone();
        let candidates = [
            ("raw", addr_word),
            ("shift6", (addr_word & ((1u64 << 42) - 1)) << 6),
            ("shift6-masked", (addr_word << 6) & ((1u64 << 42) - 1)),
        ];
        for (name, candidate) in candidates {
            let page = candidate & !(mmu::UAT_PGSZ as u64 - 1);
            let mapped = vm.covers_range(page, mmu::UAT_PGSZ as u64, true, false);
            let translation = vm.translate_iova(candidate);
            dev_info!(
                dev.as_ref(),
                "G17P fault address[{}]: {} candidate {:#x} page {:#x} mapped={} translate={:?}\n",
                label,
                name,
                candidate,
                page,
                mapped,
                translation,
            );
            let window_start = page.saturating_sub(0x10_0000);
            let window_end = match page.checked_add(0x10_0000) {
                Some(end) => end,
                None => continue,
            };
            let mut ranges = [pgtable::MappedRange::default(); 16];
            match vm.mapped_ranges(window_start..window_end, &mut ranges) {
                Ok((count, truncated)) => {
                    dev_info!(
                        dev.as_ref(),
                        "G17P fault address[{}]: {} neighbourhood {:#x}..{:#x} has {} run(s){}\n",
                        label,
                        name,
                        window_start,
                        window_end,
                        count,
                        if truncated { " TRUNCATED" } else { "" },
                    );
                    for run in ranges.iter().take(count) {
                        dev_info!(
                            dev.as_ref(),
                            "G17P fault address[{}]:   {:#x}..{:#x} pte={:#x}\n",
                            label,
                            run.start,
                            run.end,
                            run.pte,
                        );
                    }
                }
                Err(error) => dev_info!(
                    dev.as_ref(),
                    "G17P fault address[{}]: {} neighbourhood walk failed ({:?})\n",
                    label,
                    name,
                    error,
                ),
            }
        }

        // The whole client root, so any decode of the address register can be
        // tested against the real layout instead of one window at a time.
        let mut all = [pgtable::MappedRange::default(); 96];
        // The full 42-bit low half, from the client null view up to the
        // unknown page, so no decode of the address register is excluded by
        // the window rather than by the tables.
        let user_range = 0u64..(1u64 << 42);
        match vm.mapped_ranges(user_range.clone(), &mut all) {
            Ok((count, truncated)) => {
                dev_info!(
                    dev.as_ref(),
                    "G17P fault address[{}]: client root {:#x}..{:#x} holds {} run(s){}\n",
                    label,
                    user_range.start,
                    user_range.end,
                    count,
                    if truncated { " TRUNCATED" } else { "" },
                );
                for run in all.iter().take(count) {
                    dev_info!(
                        dev.as_ref(),
                        "G17P fault address[{}]: run {:#x}..{:#x} ({:#x}) pte={:#x}\n",
                        label,
                        run.start,
                        run.end,
                        run.end - run.start,
                        run.pte,
                    );
                }
            }
            Err(error) => dev_info!(
                dev.as_ref(),
                "G17P fault address[{}]: client root walk failed ({:?})\n",
                label,
                error,
            ),
        }
    }

    pub(crate) fn log_render_recovery_pre_ack(&mut self, label: &'static str) {
        let render_qids = self.render_sksm.as_ref().map(|queues| {
            [
                queues.tiling.queue_id as usize,
                queues.fragment.queue_id as usize,
            ]
        });
        if let Some([tiling_qid, fragment_qid]) = render_qids {
            let tiling_last = self.g17p_last_submitted_hw_timestamp(tiling_qid);
            let fragment_last = self.g17p_last_submitted_hw_timestamp(fragment_qid);
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P render recovery pre-ack[{}]: last-submitted TA(qid {})={:?} 3D(qid {})={:?}\n",
                label,
                tiling_qid,
                tiling_last,
                fragment_qid,
                fragment_last,
            );
        }
        {
            let dev = self.resources.dev().clone();
            if let Some(storage) = self.render_storage.as_mut() {
                match storage.snapshot() {
                    Ok(state) => dev_info!(
                        dev.as_ref(),
                        "G17P render recovery pre-ack[{}]: tiling done/read/write {}/{}/{} fragment {}/{}/{} job-list-empty {} tiling-ts [{:#x},{:#x}] fragment-ts [{:#x},{:#x}]\n",
                        label,
                        state.tiling.done,
                        state.tiling.read,
                        state.tiling.write,
                        state.fragment.done,
                        state.fragment.read,
                        state.fragment.write,
                        state.job_list_empty,
                        state.tiling_timestamps[0],
                        state.tiling_timestamps[1],
                        state.fragment_timestamps[0],
                        state.fragment_timestamps[1],
                    ),
                    Err(error) => dev_warn!(
                        dev.as_ref(),
                        "G17P render recovery pre-ack[{}]: queue snapshot failed ({:?})\n",
                        label,
                        error
                    ),
                }
            }
        }
        self.log_render_status_pages(label);
        self.log_render_sksm_entries(label);
        if g17p_native_fragment_first_mode(
            *crate::module_parameters::g17p_render_native_doorbells.value(),
        ) {
            self.log_render_outer_channels(label);
        }
        if let Err(error) = self.dump_completion_records(label) {
            let dev = self.resources.dev().clone();
            dev_warn!(
                dev.as_ref(),
                "G17P render recovery pre-ack[{}]: completion dump failed ({:?})\n",
                label,
                error
            );
        }
    }

    /// DRAM-only TA/3D outer-channel state. The current slot record pointer and
    /// queue-id byte prove whether firmware consumed the Fragment slot after
    /// its dedicated doorbell without touching the SGX MMIO aperture.
    pub(crate) fn log_render_outer_channels(&mut self, label: &'static str) {
        let snapshots = match self.resources.snapshot_all_primary_work_channels() {
            Ok(snapshots) => snapshots,
            Err(error) => {
                dev_warn!(
                    self.resources.dev().as_ref(),
                    "G17P render outer[{}]: snapshot failed ({:?})\n",
                    label,
                    error
                );
                return;
            }
        };
        for snapshot in snapshots.into_iter() {
            let name = if snapshot.table_index
                == g17_submission::G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX
            {
                "TA"
            } else if snapshot.table_index
                == g17_submission::G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX
            {
                "3D"
            } else if snapshot.table_index
                == g17_submission::G17P_COMPUTE_CHANNEL_TABLE_INDEX
            {
                "CL"
            } else {
                "--"
            };
            if snapshot.cursors == [0, 0, 0] && snapshot.current_slot_queue == 0 {
                continue;
            }
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P render outer[{}]: {} table={} cursors(actual,coherency,producer)={:?} current-slot={} record={:#x} kind={} queue-byte={}\n",
                label,
                name,
                snapshot.table_index,
                snapshot.cursors,
                snapshot.current_slot_index,
                snapshot.current_slot_queue,
                snapshot.current_slot_kind,
                snapshot.current_slot_queue_id,
            );
        }
    }

    pub(crate) fn wait_native_render_fragment_outer_consumed(
        &mut self,
        expected: u32,
    ) -> Result {
        const POLLS: usize = 100;
        const POLL_US: i64 = 100;

        if !g17p_native_fragment_preack_mode(
            *crate::module_parameters::g17p_render_native_doorbells.value(),
        ) {
            return Err(EINVAL);
        }

        let mut last = [0; 3];
        for poll in 0..=POLLS {
            fence(Ordering::Acquire);
            last = self.resources.partial_opening_outer_counters()?[0];
            if last[2] != expected {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P render native pre-ack: Fragment producer drifted, expected {} cursors(actual,coherency,producer)={:?}\n",
                    expected,
                    last
                );
                return Err(EINVAL);
            }
            if g17p_native_fragment_outer_consumed(last, expected) {
                dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P render native pre-ack: Fragment outer consumed after {} us, cursors(actual,coherency,producer)={:?}\n",
                    poll * POLL_US as usize,
                    last
                );
                return Ok(());
            }
            if poll != POLLS {
                fsleep(Delta::from_micros(POLL_US));
            }
        }

        dev_err!(
            self.resources.dev().as_ref(),
            "G17P render native pre-ack: Fragment outer was not consumed within {} us, expected {} cursors(actual,coherency,producer)={:?}; TA remains unpublished\n",
            POLLS * POLL_US as usize,
            expected,
            last
        );
        Err(ETIMEDOUT)
    }

    /// Observe the TA-owned execution timestamps for the opt-in split render
    /// launch. This is DRAM-only and does not advance either completion
    /// tracker: the ordinary paired wait still owns final retirement after the
    /// deferred fragment producer is published.
    pub(crate) fn split_render_tiling_completion(&mut self) -> Result<Option<[u64; 2]>> {
        if *crate::module_parameters::g17p_render_split_launch.value() == 0
            || self.render_tracker.is_none()
        {
            return Err(EINVAL);
        }
        fence(Ordering::Acquire);
        let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
        if state.tiling_timestamps.iter().all(|value| *value != 0) {
            Ok(Some(state.tiling_timestamps))
        } else {
            Ok(None)
        }
    }

    /// Read both engine timestamp pairs without imposing the split-launch
    /// policy gate. This is used only by the post-doorbell KSM diagnostic.
    pub(crate) fn render_engine_timestamps(&mut self) -> Result<[[u64; 2]; 2]> {
        if self.render_tracker.is_none() {
            return Err(EINVAL);
        }
        fence(Ordering::Acquire);
        let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
        Ok([state.tiling_timestamps, state.fragment_timestamps])
    }

    /// Publish the complete fragment group retained by a split launch after
    /// [`Self::split_render_tiling_completion`] has proved TA execution.
    /// Ordering is strict: SKSM entry, tag-14/AddKicks and inner producer, then
    /// outer slot and producer. The second doorbell is sent by the caller only
    /// after this returns.
    pub(crate) fn publish_split_render_fragment_group(
        &mut self,
        dev: &AsahiDevice,
        registers: &regs::Resources,
        plan: &g17_submission::G17PPartialOpeningOuterSlotPlan,
    ) -> Result {
        if *crate::module_parameters::g17p_render_split_launch.value() == 0
            || self.render_tracker.is_none()
            || self.render_storage.is_none()
            || self.render_bind.is_none()
        {
            return Err(EINVAL);
        }
        let (ta_stamp, ta_queue_id) = {
            let queues = self.render_sksm.as_ref().ok_or(ENODEV)?;
            if !queues.tiling.producer.previous_valid {
                return Err(EINVAL);
            }
            (
                queues.tiling.producer.previous_timestamp,
                queues.tiling.queue_id,
            )
        };
        let event_word = u32::try_from(ta_stamp)
            .ok()
            .and_then(|stamp| stamp.checked_mul(0x100))
            .ok_or(EINVAL)?;
        let barrier = [g17_submission::G17PClBarrierDependency {
            event_word,
            shared_stamp: 0,
            queue_id: ta_queue_id,
        }];
        let suppress_barrier =
            *crate::module_parameters::g17p_render_no_barrier.value() != 0;
        let barrier_slice: &[g17_submission::G17PClBarrierDependency] =
            if suppress_barrier { &[] } else { &barrier };
        if suppress_barrier {
            dev_info!(
                dev.as_ref(),
                "G17P render split-launch: DIAGNOSTIC barrier dependency suppressed (event_word {:#x} qid {})\n",
                event_word,
                ta_queue_id
            );
        }
        let fragment_queue = self
            .render_storage
            .as_ref()
            .ok_or(ENODEV)?
            .queue_gpu_vas()
            .0;
        let fragment_stamp = self
            .publish_render_sksm_group(dev, registers, false, fragment_queue, barrier_slice)
            .map_err(|error| Self::render_stage_fail("split-sksm-publish-fragment", error))?;
        let fragment_stamp = u32::try_from(fragment_stamp)
            .map_err(|_| Self::render_stage_fail("split-sksm-fragment-stamp-width", EINVAL))?;
        self.render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .stage_fragment_kick(self.render_submission_ordinal, fragment_stamp)
            .map_err(|error| Self::render_stage_fail("split-fragment-addkicks", error))?;
        fence(Ordering::SeqCst);
        let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
        if state.fragment.write
            != g17_resources::g17p_render_published_prefix(self.render_submission_ordinal)?
        {
            return Err(Self::render_stage_fail("split-fragment-inner-prefix", EINVAL));
        }
        self.resources
            .publish_deferred_partial_opening_fragment(plan)
            .map_err(|error| Self::render_stage_fail("split-fragment-outer", error))
    }

    pub(crate) fn publish_native_render_tiling_group(
        &mut self,
        dev: &AsahiDevice,
        registers: &regs::Resources,
        plan: &g17_submission::G17PPartialOpeningOuterSlotPlan,
    ) -> Result {
        if !g17p_native_fragment_first_mode(
            *crate::module_parameters::g17p_render_native_doorbells.value(),
        )
            || *crate::module_parameters::g17p_render_split_launch.value() != 0
            || self.render_tracker.is_none()
            || self.render_storage.is_none()
            || self.render_bind.is_none()
            || self.render_sksm.is_none()
        {
            return Err(EINVAL);
        }
        let ordinal = self.render_submission_ordinal;
        let (tiling_queue, expected_stamp) = {
            let storage = self.render_storage.as_ref().ok_or(ENODEV)?;
            let queues = self.render_sksm.as_ref().ok_or(ENODEV)?;
            (
                storage.queue_gpu_vas().1,
                queues.tiling.producer.current_timestamp,
            )
        };

        // Complete every fallible preparation before publishing TA prefix 2.
        let late_tiling = self
            .render_storage
            .as_mut()
            .ok_or(ENODEV)?
            .prepare_late_tiling_publication_with_stamp(
                ordinal,
                u32::try_from(expected_stamp).map_err(|_| EINVAL)?,
            )
            .map_err(|error| Self::render_stage_fail("native-prepare-tiling", error))?;
        let mut order = G17PNativeRenderPublishOrder::after_fragment_doorbell();

        let tiling_header = late_tiling
            .config_update_header()
            .ok_or_else(|| Self::render_stage_fail("native-decode-tiling-tag15", EINVAL))?;
        dev_info!(
            dev.as_ref(),
            "G17P render native TA tag15: selector={:#x} entry=[{:#x},{:#x}] qid={} install={} dm={} graphics={} context={} generation={} flushid={} scheduler={:#x} field56={} pb=[{:#x},{:#x},id={},bit={}]\n",
            tiling_header.selector,
            tiling_header.entry_low,
            tiling_header.entry_high,
            tiling_header.queue_id,
            tiling_header.install,
            tiling_header.data_master,
            tiling_header.graphics,
            tiling_header.context_id,
            tiling_header.flush_generation,
            tiling_header.flushid_index,
            tiling_header.scheduler_state,
            tiling_header.field_56,
            tiling_header.parameter_buffer_object,
            tiling_header.parameter_buffer_token,
            tiling_header.parameter_buffer_id,
            tiling_header.parameter_buffer_bit,
        );
        let tiling_qos = self
            .resources
            .publish_render_qos_queue_record(tiling_header)
            .map_err(|error| Self::render_stage_fail("native-publish-tiling-qos", error))?;
        dev_info!(
            dev.as_ref(),
            "G17P render QoS: delayed native TA qid {} record {:?}\n",
            tiling_header.queue_id,
            tiling_qos,
        );

        late_tiling.publish_native_prefix();
        order
            .advance(G17PNativeRenderPublishPhase::TilingPrefix)
            .map_err(|_| Self::render_stage_fail("native-order-prefix", EINVAL))?;
        fence(Ordering::SeqCst);

        let tiling_stamp = self
            .publish_render_sksm_group(dev, registers, true, tiling_queue, &[])
            .map_err(|error| Self::render_stage_fail("native-sksm-publish-tiling", error))?;
        if tiling_stamp != expected_stamp {
            return Err(Self::render_stage_fail("native-tiling-stamp-mismatch", EINVAL));
        }
        order
            .advance(G17PNativeRenderPublishPhase::TilingSksm)
            .map_err(|_| Self::render_stage_fail("native-order-sksm", EINVAL))?;

        late_tiling.publish_native_kick();
        order
            .advance(G17PNativeRenderPublishPhase::TilingKick)
            .map_err(|_| Self::render_stage_fail("native-order-kick", EINVAL))?;
        fence(Ordering::SeqCst);
        let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
        let published_prefix = g17_resources::g17p_render_published_prefix(ordinal)?;
        if state.tiling.write != published_prefix {
            return Err(Self::render_stage_fail("native-tiling-inner-prefix", EINVAL));
        }

        self.resources
            .publish_deferred_partial_opening_tiling(plan)
            .map_err(|error| Self::render_stage_fail("native-tiling-outer", error))?;
        order
            .advance(G17PNativeRenderPublishPhase::TilingOuter)
            .map_err(|_| Self::render_stage_fail("native-order-outer", EINVAL))?;
        fence(Ordering::Acquire);
        let outer = self.resources.partial_opening_outer_counters()?;
        if !g17p_native_tiling_fully_visible(
            state.tiling.write,
            published_prefix,
            outer[1][2],
            plan.next_producer,
        ) {
            return Err(Self::render_stage_fail("native-complete-ta-visibility", EINVAL));
        }

        late_tiling.publish_native_pool_transition();
        order
            .advance(G17PNativeRenderPublishPhase::PoolTransition)
            .map_err(|_| Self::render_stage_fail("native-order-pool", EINVAL))?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// One iteration of the render completion poll.
    ///
    /// Split out of `wait_translated_render_completion` so the runtime can
    /// interleave `service_firmware_events_inline` between polls. The compute
    /// submit does exactly that, and its poll loop carries the reason:
    /// "The submit path holds the runtime mutex for the whole wait, which is
    /// why this drains here rather than from an RTKit handler or a workqueue."
    /// Render's wait had no such interleave, so a firmware restart request
    /// raised during the wait stayed unread until the wait gave up.
    pub(crate) fn poll_translated_render_completion(
        &mut self,
    ) -> Result<G17PRenderPollOutcome> {
        {
            fence(Ordering::Acquire);
            let state = self.render_storage.as_mut().ok_or(ENODEV)?.snapshot()?;
            // B1 unlinks EventControl only after its completed count reaches
            // its own submitted-count slot (168d0, KTrace 0x10b/0x10d).
            // Queue retirement and timestamps do not authorize host writes
            // to this firmware-owned list. Keep polling until it is empty.
            let tracker = self.render_tracker.as_mut().ok_or(ENODEV)?;
            let window = g17_resources::g17p_render_completion_window(self.render_submission_ordinal)?;
            if window.target() != tracker.tiling_prefix || window.target() != tracker.fragment_prefix {
                return Err(EINVAL);
            }
            let completion = g17_completion::observe_paired_render_window(
                window,
                tracker.tiling_previous,
                state.tiling,
                tracker.fragment_previous,
                state.fragment,
                g17_completion::RenderExecutionState {
                    job_list_empty: state.job_list_empty,
                    tiling_timestamps: state.tiling_timestamps,
                    fragment_timestamps: state.fragment_timestamps,
                },
            )
            .map_err(|_| EINVAL)?;
            tracker.tiling_previous = state.tiling;
            tracker.fragment_previous = state.fragment;
            if completion.complete {
                // Publish the exact GPU-written pass envelope while the completed job's
                // VM binding, ordinal and QIDs are still owned.  This is
                // observation only: it neither gates completion nor changes
                // queue state.  Invalid retirement-only pairs are omitted.
                if let Some(interval) =
                    g17_completion::render_gpu_interval(
                        g17_completion::RenderExecutionState {
                            job_list_empty: state.job_list_empty,
                            tiling_timestamps: state.tiling_timestamps,
                            fragment_timestamps: state.fragment_timestamps,
                        },
                    )
                {
                    let (vm_id, root) = self.render_bind.as_ref()
                        .map_or((0, 0), |binding| (binding.vm_id(), binding.root()));
                    let fallback_pair = self.render_storage
                        .as_ref()
                        .map(g17_resources::G17PUserRenderStorage::queue_pair)
                        .unwrap_or(g17_resources::G17PRenderQueuePair::for_slot(
                            self.active_render_slot,
                        ).ok_or(EINVAL)?);
                    let (tiling_qid, fragment_qid) = self.render_sksm.as_ref()
                        .map_or((fallback_pair.tiling, fallback_pair.fragment), |queues| (
                            u16::from(queues.tiling.queue_id),
                            u16::from(queues.fragment.queue_id),
                        ));
                    dev_info!(
                        self.resources.dev().as_ref(),
                        "G17P render: GPU-INTERVAL ordinal={} vm={} root={:#x} ta-qid={} fragment-qid={} start={} end={}\n",
                        self.render_submission_ordinal,
                        vm_id,
                        root,
                        tiling_qid,
                        fragment_qid,
                        interval[0],
                        interval[1],
                    );
                }
                let retired_sksm = self.render_sksm.as_ref().map(|queues| {
                    let ta = g17_submission::complete_g17p_cl_kicks(queues.tiling.producer, 1)?;
                    let fragment = g17_submission::complete_g17p_cl_kicks(queues.fragment.producer, 1)?;
                    Ok::<_, g17_submission::SubmissionError>((ta, fragment))
                }).transpose().map_err(|_| EIO)?;
                let next_ordinal = self.render_submission_ordinal.checked_add(1).ok_or(EOVERFLOW)?;
                let hardware_buffer_id = self.render_hardware_buffer_id.ok_or(EIO)?;
                self.render_hardware_buffer_ids
                    .release(hardware_buffer_id)
                    .map_err(|_| EIO)?;
                if let (Some(queues), Some((ta, fragment))) = (self.render_sksm.as_mut(), retired_sksm) {
                    queues.tiling.producer = ta;
                    queues.fragment.producer = fragment;
                }
                self.render_hardware_buffer_id = None;
                self.render_tracker = None;
                self.render_hardware_buffer_published = false;
                self.render_auxiliary_timestamp = 0;
                dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P render: released TA HardwareBufferID {} at paired completion\n",
                    hardware_buffer_id,
                );
                self.render_submission_ordinal = next_ordinal;
                return Ok(G17PRenderPollOutcome::Complete(completion));
            }
        }
        Ok(G17PRenderPollOutcome::Pending)
    }

    /// Number of polls the render wait makes, at 1ms each.
    pub(crate) const RENDER_COMPLETION_POLLS: usize = 2000;

    pub(crate) fn wait_translated_render_completion(
        &mut self,
    ) -> Result<g17_completion::PairedRenderCompletion> {
        for _ in 0..Self::RENDER_COMPLETION_POLLS {
            match self.poll_translated_render_completion()? {
                G17PRenderPollOutcome::Complete(completion) => return Ok(completion),
                G17PRenderPollOutcome::Retry => continue,
                G17PRenderPollOutcome::Pending => fsleep(Delta::from_millis(1)),
            }
        }
        self.report_render_wait_timeout();
        Err(ETIMEDOUT)
    }

    /// Explain a render wait timeout. Split out with the poll body so both the
    /// plain and the event-servicing loops report identically.
    pub(crate) fn report_render_wait_timeout(&mut self) {
        let polls = Self::RENDER_COMPLETION_POLLS;
        // The graph remains pinned because firmware may still own it. Runtime
        // teardown stops both provider CPUs before manager resources drop.
        //
        // Say WHY the wait failed. Without this a render timeout is anonymous,
        // and the three stages it can stall in are indistinguishable:
        //
        //   read  < target       the firmware has not consumed this group
        //   done  < target       consumed but not retired
        //   timestamps still 0   retired while the GPU wrote nothing -- the
        //                        exact shape the compute tag-16 bug had
        //
        // Every field is a plain read of an object the driver itself allocated:
        // DRAM only, no sgx MMIO, which would take an asynchronous SError with
        // the GPU cores gated -- and by the time a submit has timed out they
        // are.
        let final_state = self
            .render_storage
            .as_mut()
            .and_then(|storage| storage.snapshot().ok());
        let target =
            g17_resources::g17p_render_published_prefix(self.render_submission_ordinal).ok();
        if let Some(state) = final_state {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P render: completion timed out after {} polls; ordinal {} target {:?} tiling done/read/write {}/{}/{} fragment {}/{}/{} job-list-empty {} tiling-ts [{:#x},{:#x}] fragment-ts [{:#x},{:#x}]\n",
                polls,
                self.render_submission_ordinal,
                target,
                state.tiling.done,
                state.tiling.read,
                state.tiling.write,
                state.fragment.done,
                state.fragment.read,
                state.fragment.write,
                state.job_list_empty,
                state.tiling_timestamps[0],
                state.tiling_timestamps[1],
                state.fragment_timestamps[0],
                state.fragment_timestamps[1],
            );
        }
        match self
            .render_storage
            .as_mut()
            .ok_or(ENODEV)
            .and_then(|storage| storage.fragment_word_20f0())
        {
            Ok(value) => dev_err!(
                self.resources.dev().as_ref(),
                "G17P render descriptor diagnostic: +0x20f0={:#010x} (not an activation witness)\n",
                value,
            ),
            Err(error) => dev_err!(
                self.resources.dev().as_ref(),
                "G17P render descriptor diagnostic: +0x20f0 read failed ({:?})\n",
                error,
            ),
        }
        let context_qos = self.render_storage.as_mut().and_then(|storage| {
            let header = storage.fragment_config_update_header().ok()?;
            let tiling_header = storage.tiling_config_update_header().ok()?;
            let prefix = storage.render_channel_control_prefix().ok()?;
            Some((header, tiling_header, prefix))
        });
        if let Some((header, tiling_header, prefix)) = context_qos {
            let slot = header.field_56 as u8;
            let fragment_qos = self
                .resources
                .qos_queue_record(header.queue_id as u8)
                .ok();
            let tiling_qos = self
                .resources
                .qos_queue_record(tiling_header.queue_id as u8)
                .ok();
            let bitmap_lo = u64::from_le_bytes([
                prefix[0x12], prefix[0x13], prefix[0x14], prefix[0x15],
                prefix[0x16], prefix[0x17], prefix[0x18], prefix[0x19],
            ]);
            let bitmap_hi = u64::from_le_bytes([
                prefix[0x1a], prefix[0x1b], prefix[0x1c], prefix[0x1d],
                prefix[0x1e], prefix[0x1f], prefix[0x20], prefix[0x21],
            ]);
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P render context/qos: requested-slot={} ctxdesc(slot={},ctx={},prio={},class={},state={},bound-slot={}) qid-bitmap={:#018x}/{:#018x} qos-records TA(qid {})={:?} 3D(qid {})={:?}\n",
                slot,
                prefix[0],
                prefix[1],
                prefix[3],
                prefix[4],
                prefix[0x26],
                prefix[0x33],
                bitmap_lo,
                bitmap_hi,
                tiling_header.queue_id,
                tiling_qos,
                header.queue_id,
                fragment_qos,
            );
        }
    }

    pub(crate) fn sksm_scratch(&self) -> G17SksmScratchGeometry {
        self.platform.sksm_scratch
    }

    pub(crate) fn sksm_queue_geometry(&self) -> g17_submission::G17SksmQueueGeometry {
        self.platform.sksm_queue_geometry
    }

    /// Return the hardware QID of a queue owned by this manager.
    pub(crate) fn compute_queue_id(&self, queue: &G17PSksmQueue) -> Result<u8> {
        if queue.scratch != self.sksm_scratch() || queue.uat_owner != self.uat().ttb_base() {
            return Err(EINVAL);
        }
        Ok(queue.queue_id)
    }

    /// Prove that a prepared compute queue is still host-only and has never
    /// been registered, published, kicked, or assigned a command timestamp.
    /// Submission ordinal of the retained queue, for trace labelling.
    pub(crate) fn compute_queue_submission_ordinal(&self, queue: &G17PSksmQueue) -> u32 {
        queue.submission_ordinal
    }

    /// Whether the retained queue is past its first bind and can take a
    /// steady-state submission.
    pub(crate) fn compute_queue_is_installed(&self, queue: &G17PSksmQueue) -> bool {
        queue.hardware_state == G17PSksmQueueHardwareState::HardwareRegistered
            && queue.submission_ordinal != 0
            && queue.scratch == self.sksm_scratch()
            && queue.uat_owner == self.uat().ttb_base()
    }

    pub(crate) fn validate_prepublication_compute_queue(
        &self,
        queue: &G17PSksmQueue,
    ) -> Result {
        if queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
            || queue.hardware_state != G17PSksmQueueHardwareState::PreparedMemory
            || queue.submission_ordinal != 0
            || queue.b2_activation != g17_submission::G17PClB2ActivationState::Pending
            || queue.producer.submitted != 0
            || queue.producer.current_timestamp != queue.producer.geometry.timestamp_seed()
            || queue.producer.last_add_kicks_timestamp
                != queue.producer.geometry.timestamp_seed()
            || queue.producer.base_timestamp != queue.producer.geometry.timestamp_seed()
            || queue.producer.previous_valid
            || queue.producer.previous_timestamp != 0
            || queue.producer.queue_id != queue.queue_id
            || !queue.direct_lifecycle.queue_config_dirty
            || queue.direct_lifecycle.cached_channel_identity != 0
            || queue.direct_lifecycle.next_correlation_token != 1
        {
            return Err(EBUSY);
        }
        Ok(())
    }

    pub(crate) fn bind_compute_user_vm(
        &mut self,
        queue: &mut G17PSksmQueue,
        vm: &mmu::Vm,
    ) -> Result<G17PComputeUserBinding> {
        if queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
        {
            return Err(EINVAL);
        }
        self.resources.cache_compute_vm_mappings(vm).inspect_err(|error| {
            pr_info!("G17P compute VM: FList aliases failed {:?}\n", error);
        })?;
        let sksm_entries = queue.storage.map_user_entry_alias(vm).inspect_err(|error| {
            pr_info!("G17P compute VM: canonical SKSM entry alias {:#x} failed {:?}\n",
                queue.storage.entry_gpu_va(), error);
        })?;
        let mappings = queue.compute.map_user_execution_state(vm).inspect_err(|error| {
            pr_info!("G17P compute VM: execution aliases failed {:?}\n", error);
        })?;
        // Bracketed to localise the ordering hang: after a failed render the
        // next compute goes silent for tens of seconds here. slotalloc's
        // get_inner is documented "Blocks if no slots are free", and T8140
        // limits the allocator to ONE user slot, so a retained binding from a
        // previous client blocks every later one.
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: acquiring UAT bind for vm\n"
        );
        let vm_bind = self.resources.uat().bind(vm)?;
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: UAT bind acquired, slot {}\n",
            vm_bind.slot()
        );
        // Slot 1 is where the VM is *bound*: the first-partial opening pins
        // the render/descriptor/optional-identity context there and the
        // root-phase machine reads `ttbs[0]`/`ttbs[1]` back verbatim. It is
        // not the context the kick names -- the compute submission declares
        // `context_id = 2`, which needs its own translation root.
        if vm_bind.slot() != 1 || !mappings.all_reachable_from(vm) {
            return Err(EFAULT);
        }
        // Publish that root before anything can be kicked. Without it GPU
        // context 2 has no lower and no upper root at all, and the first
        // firmware-half fetch the front end makes takes a Page Invalid read
        // fault with vm_slot = 2 (unit 0xd9, gUPM).
        self.resources
            .uat()
            .install_t8140_compute_context_alias(&vm_bind)?;
        if *module_parameters::g17p_ctx2_root.value() != 0
            && !self
                .resources
                .uat()
                .t8140_compute_context_alias_ready(&vm_bind)
        {
            let roots = self.resources.uat().t8140_compute_context_roots();
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P compute: context alias did not take (root={:#x}): ctx1 ttb0={:#x}, ctx2 ttb0={:#x} ttb1={:#x}, ctx3 ttb0={:#x} ttb1={:#x}\n",
                vm_bind.root(),
                roots[1].0,
                roots[2].0,
                roots[2].1,
                roots[3].0,
                roots[3].1,
            );
            return Err(EFAULT);
        }
        let (descriptor_high, descriptor_low) = queue.compute.descriptor_gpu_vas(0);
        let encoded_low = mappings.descriptor_gpu_va(0);
        if encoded_low != descriptor_low {
            return Err(EFAULT);
        }
        let high_pa = self.uat().kernel_vm().translate_iova(descriptor_high)?;
        let context0_pa = self
            .uat()
            .kernel_lower_vm()
            .translate_iova(descriptor_low)?;
        let context1_pa = vm.translate_iova(encoded_low)?;
        if high_pa != context0_pa || high_pa != context1_pa {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P descriptor aliases disagree high={:#x}->{:#x} context0-low={:#x}->{:#x} context1-low={:#x}->{:#x}\n",
                descriptor_high,
                high_pa,
                descriptor_low,
                context0_pa,
                encoded_low,
                context1_pa
            );
            return Err(EFAULT);
        }
        let entry_aliases = queue.storage.entry_aliases()?;
        if sksm_entries.iova() != entry_aliases.low()
            || sksm_entries.size() != entry_aliases.extent()
        {
            return Err(EFAULT);
        }
        let entry_high_pa = self
            .uat()
            .kernel_vm()
            .translate_iova(entry_aliases.high())?;
        let entry_context0_pa = self
            .uat()
            .kernel_lower_vm()
            .translate_iova(entry_aliases.low())?;
        let entry_context1_pa = vm.translate_iova(sksm_entries.iova())?;
        if entry_high_pa != entry_context0_pa || entry_high_pa != entry_context1_pa {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P SKSM entry aliases disagree high={:#x}->{:#x} context0-low={:#x}->{:#x} context1-low={:#x}->{:#x}\n",
                entry_aliases.high(),
                entry_high_pa,
                entry_aliases.low(),
                entry_context0_pa,
                sksm_entries.iova(),
                entry_context1_pa
            );
            return Err(EFAULT);
        }
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P SKSM entry aliases: high={:#x} context0-low={:#x} context1-low={:#x} backing-pa={:#x} size={:#x}\n",
            entry_aliases.high(),
            entry_aliases.low(),
            sksm_entries.iova(),
            entry_high_pa,
            entry_aliases.extent()
        );
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P descriptor aliases: high={:#x} context0-low={:#x} encoded/context1-low={:#x} backing-pa={:#x}\n",
            descriptor_high,
            descriptor_low,
            encoded_low,
            high_pa
        );
        Ok(G17PComputeUserBinding {
            mappings,
            _sksm_entries: sksm_entries,
            _vm_bind: Some(vm_bind),
            _execution_context: None,
        })
    }

    pub(crate) fn bind_independent_compute_user_vm(
        &mut self, queue: &mut G17PSksmQueue, vm: &mmu::Vm,
    ) -> Result<(G17PComputeUserBinding, Arc<mmu::T8140ComputeExecutionContext>)> {
        self.resources.cache_compute_vm_mappings(vm)?;
        let sksm_entries = queue.storage.map_user_entry_alias(vm)?;
        let mappings = queue.compute.map_user_execution_state(vm)?;
        let context = self.uat().allocate_t8140_compute_execution_context(vm)?;
        let (high, low) = queue.compute.descriptor_gpu_vas(0);
        if !mappings.all_reachable_from(vm) || mappings.descriptor_gpu_va(0) != low
            || vm.translate_iova(low)? != self.uat().kernel_vm().translate_iova(high)?
        { return Err(EFAULT); }
        queue.independent_context = true;
        Ok((G17PComputeUserBinding {
            mappings, _sksm_entries: sksm_entries, _vm_bind: None,
            _execution_context: Some(context.clone()),
        }, context))
    }

    /// Completion identity and visibility belong to the queue, not the last
    /// global publisher. Other QIDs may finish in either order. Never clear
    /// their shared completion ring while another submission is outstanding.
    pub(crate) fn poll_independent_compute_completion(
        &mut self, queue: &mut G17PSksmQueue,
    ) -> Result<Option<[u64; 2]>> {
        if !queue.independent_context || queue.completion_descriptor == 0 { return Ok(None); }
        let Some(timestamps) = self.resources.read_compute_completion(
            queue.completion_descriptor, queue.completion_last_end,
        )? else { return Ok(None); };
        if timestamps[0] == timestamps[1] { return Err(EIO); }
        if queue.completion_stamp_armed && queue.compute.read_compute_stamp()? == 0 {
            return Err(EAGAIN);
        }
        if let Some(pair) = self.resources.read_compute_execution_interval(
            queue.completion_descriptor, timestamps[1],
        )? {
            dev_info!(self.resources.dev().as_ref(),
                "G17P async compute: GPU-INTERVAL qid={} descriptor={:#x} start={} end={}\n",
                queue.queue_id, queue.completion_descriptor, pair[0], pair[1]);
        }
        queue.producer = g17_submission::complete_g17p_cl_kicks(queue.producer, 1)
            .map_err(|_| EINVAL)?;
        queue.completion_last_end = timestamps[1];
        queue.completion_descriptor = 0;
        queue.completion_stamp_armed = false;
        Ok(Some(timestamps))
    }

    fn allocate_compute_queue_id(&mut self) -> Result<u8> {
        let id = self.next_compute_queue_id.ok_or(ENOSPC)?;
        let mut next = id.checked_add(1);
        while next.is_some_and(|candidate| {
            candidate == g17_resources::g17p_render_queue_id(true) as u8
                || candidate == g17_resources::g17p_render_queue_id(false) as u8
        }) {
            next = next.and_then(|candidate| candidate.checked_add(1));
        }
        self.next_compute_queue_id =
            next.filter(|candidate| *candidate <= G17P_COMPUTE_QUEUE_ID_LAST);
        Ok(id)
    }

    pub(crate) fn prepare_g17p_compute_queue(
        &mut self,
        dev: &AsahiDevice,
    ) -> Result<G17PSksmQueue> {
        let queue_id = self.allocate_compute_queue_id()?;
        let result = (|| {
        const COMPLETION_SELECTOR: u8 = 2;

        let geometry = self.sksm_queue_geometry();
        let mut storage =
            g17_resources::G17PClRuntimeStorage::new(dev, self.uat(), geometry)?;
        let compute = g17_resources::G17PComputeDescriptorStorage::new(
            dev,
            self.uat(),
            storage.channel_control_gpu_va(),
        )?;
        let operand_table_gpu_va = storage.operand_table_gpu_va();
        let support_state_gpu_va = storage.support_state_gpu_va();
        let scheduler_state_gpu_va = storage.scheduler_state_gpu_va();
        storage.with_static_pages_mut(
            |shared_support, support_state, scheduler_state, channel_control| {
                g17_submission::apply_g17p_cl_retained_static_state(
                    shared_support,
                    support_state,
                    scheduler_state,
                    channel_control,
                    operand_table_gpu_va,
                    support_state_gpu_va,
                    scheduler_state_gpu_va,
                )
                .map_err(|_| EINVAL)
            },
        )?;
        let routing = g17_submission::G17PComputeQueueRouting {
            queue_id,
            completion_selector: COMPLETION_SELECTOR as u32,
            entry_gpu_va: storage.entry_gpu_va(),
            geometry,
        };
        let config =
            g17_submission::prepare_g17p_compute_sksm_queue_config(routing, self.sksm_scratch())
                .map_err(|_| EINVAL)?;

        Ok(G17PSksmQueue {
            queue_id: routing.queue_id,
            scratch: self.sksm_scratch(),
            uat_owner: self.uat().ttb_base(),
            configure_pair: config.write_pair(),
            enable_pair: config.enable_pair,
            disable_pair: config.disable_pair,
            hardware_state: G17PSksmQueueHardwareState::PreparedMemory,
            storage,
            compute,
            producer: geometry
                .initial_producer_state(
                    queue_id,
                    COMPLETION_SELECTOR,
                    g17_submission::G17PClQosConfig {
                        queue_byte_32: 1,
                        class_40: 8,
                        word_48: 0xffff,
                    },
                )
                .map_err(|_| EINVAL)?,
            direct_lifecycle: g17_submission::G17PDirectClLifecycleState {
                queue_config_dirty: true,
                cached_channel_identity: 0,
                next_correlation_token: 1,
            },
            configure_guard_accelerator_stamp: 0,
            configure_guard_event_stamp: 0,
            submission_ordinal: 0,
            b2_activation: g17_submission::G17PClB2ActivationState::Pending,
            independent_context: false,
            completion_descriptor: 0,
            completion_last_end: 0,
            completion_stamp_armed: false,
        })
        })();
        // No firmware publication occurs in this constructor. A failed host
        // allocation must not consume the finite QID namespace permanently.
        if result.is_err() { self.next_compute_queue_id = Some(queue_id); }
        result
    }

    pub(crate) fn register_prepared_compute_sksm_queue(
        &self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
    ) -> Result {
        if queue.scratch != self.sksm_scratch() || queue.uat_owner != self.uat().ttb_base() {
            return Err(EINVAL);
        }
        match queue.hardware_state {
            G17PSksmQueueHardwareState::HardwareRegistered => return Ok(()),
            G17PSksmQueueHardwareState::PreparedMemory => {}
        }
        if queue.configure_guard_accelerator_stamp != queue.configure_guard_event_stamp {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P QID {} configure guard mismatch accelerator={} event-machine={}\n",
                queue.queue_id,
                queue.configure_guard_accelerator_stamp,
                queue.configure_guard_event_stamp
            );
            return Err(EBUSY);
        }
        queue.storage.prepare_live_mappings(self.uat())?;
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P submitBuffer/prepareMappings: low/high QID4 aliases published to live UAT\n"
        );

        fence(Ordering::SeqCst);
        registers.g17p_sksm_configure_queue(queue.configure_pair, queue.enable_pair)?;
        queue.hardware_state = G17PSksmQueueHardwareState::HardwareRegistered;
        Ok(())
    }

    /// Remove a retained queue from the hardware bridge before releasing its
    /// backing storage. Repeated teardown is harmless.
    pub(crate) fn unregister_compute_sksm_queue(
        &self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
    ) -> Result {
        if queue.scratch != self.sksm_scratch() || queue.uat_owner != self.uat().ttb_base() {
            return Err(EINVAL);
        }
        if queue.hardware_state == G17PSksmQueueHardwareState::PreparedMemory {
            return Ok(());
        }

        registers.g17p_sksm_publish_write_pair(queue.disable_pair)?;
        queue.hardware_state = G17PSksmQueueHardwareState::PreparedMemory;
        Ok(())
    }

    /// Publish one complete CL entry, then perform the queue's ordered SKSM
    /// notification. The first post-registration entry publishes the retained
    /// B2 state; later entries keep those provider-owned pages unchanged.
    pub(crate) fn submit_compute_entry(
        &mut self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        operands: g17_submission::G17PClKickEntryOperands<'_>,
    ) -> Result<u64> {
        if queue.hardware_state != G17PSksmQueueHardwareState::HardwareRegistered
            || queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
        {
            return Err(EINVAL);
        }
        let prepared = g17_submission::prepare_g17p_cl_kick_entry(queue.producer, operands)
            .map_err(|_| EINVAL)?;
        self.publish_compute_entry(registers, queue, prepared)
    }

    fn publish_compute_entry(
        &mut self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        prepared: g17_submission::PreparedG17PClKickEntry,
    ) -> Result<u64> {
        if g17p_compute_sksm_entry_enabled() {
            queue.storage.write_entry(
                prepared.entry_offset as usize,
                prepared.zero_length as usize,
                &prepared.entry,
            )?;
        }

        self.publish_compute_b2_once(queue)?;

        fence(Ordering::SeqCst);
        queue.producer = prepared.producer_after;
        let timestamp = prepared.current_timestamp;
        self.add_sksm_kicks(registers, queue, 1)?;
        queue.producer.last_add_kicks_timestamp = timestamp;
        fence(Ordering::SeqCst);
        Ok(timestamp)
    }

    fn publish_compute_b2_once(&mut self, queue: &mut G17PSksmQueue) -> Result {
        self.resources.stage_compute_region_views()?;
        if queue.b2_activation == g17_submission::G17PClB2ActivationState::Pending {
            let channel_control_gpu_va = queue.storage.channel_control_gpu_va();
            queue.b2_activation =
                self.resources
                    .with_b2_pages_mut(|computed, region_1, region_2| {
                        g17_submission::apply_g17p_cl_b2_primary_state(
                            queue.b2_activation,
                            computed,
                            region_1,
                            region_2,
                            channel_control_gpu_va,
                        )
                        .map_err(|_| EINVAL)
                    })?;
        }
        Ok(())
    }

    /// The binary-verified first-bind chronology: tag 3 -> 15 -> QID
    /// configure -> SKSM entry -> 14 -> 16 -> CL_2, ending with the queue
    /// registered on the SKSM bridge. It is shape-independent -- the command
    /// only supplies the CDM range, USC window, sampler state and timestamps
    /// that `build_compute_descriptor` consumes -- so any well-formed compute
    /// command may drive it, not just the add3 probe.
    fn submit_translated_first_bind_channel(
        &mut self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        submission_phase: &AtomicU32,
        binding: &G17PComputeUserBinding,
        command: &g17_uapi::TranslatedComputeCommand,
        context: G17PComputeSubmissionContext<'_>,
        add3_buffers: Option<[u64; 3]>,
    ) -> Result<g17_submission::PreparedG17PDirectClNotifications> {
        if queue.submission_ordinal != 0 {
            return Err(EINVAL);
        }
        // This release gate is deliberately first: any future contract
        // regression must fail before descriptor, ring, SKSM, MMIO, or mailbox
        // state changes.
        g17_submission::require_g17p_first_bind_direct_hardware_release(
            g17_submission::G17P_FIRST_BIND_COMMAND_PLAN,
        )
        .map_err(|_| ENOTSUPP)?;

        let descriptor_slot = 0;
        let (descriptor_high, _) = queue.compute.descriptor_gpu_vas(descriptor_slot);
        let descriptor_low = binding.mappings.descriptor_gpu_va(descriptor_slot);
        match add3_buffers {
            Some(buffers) => queue.compute.write_g17p_add3_resource_table(buffers)?,
            None => dev_info!(
                self.resources.dev().as_ref(),
                "G17P compute: generic first-bind, no add3 operand table; 0x1a510 still names the retained preempt object\n"
            ),
        }

        let user_layout = binding.mappings.layout();
        let shared_control = queue.storage.shared_support_gpu_va();
        let objects = g17_compute::ComputeDescriptorObjects {
            scheduler_record: queue.compute.scheduler_record_gpu_va(),
            descriptor_low_alias: descriptor_low,
            dispatch_a: queue.compute.dispatch_a_gpu_va(),
            dispatch_b: queue.compute.dispatch_b_gpu_va(),
            status_a: queue.compute.status_a_gpu_va(),
            status_b: queue.compute.status_b_gpu_va(),
            shared_control,
            zero_page: queue.compute.zero_page_gpu_va(),
        };
        queue.compute.with_descriptor_mut(descriptor_slot, |raw| {
            g17_uapi::build_compute_descriptor(
                command,
                g17_uapi::ComputeInternalState {
                    preempt_base: user_layout.preempt,
                    dispatch_identity: context.dispatch_identity,
                    context_id: context.context_id,
                    work_ordinal: 0,
                    robustness: user_layout.robustness,
                    operand_state_base: user_layout.operand_state,
                    execution_gate: context.execution_gate,
                },
                objects,
                g17_compute::ComputeDescriptorMetadata {
                    submit_sequence: 0,
                    context_id: context.context_id,
                    grid_index: u32::from(queue.queue_id),
                    work_ordinal: 0,
                    queue_submission: 1,
                    queue_ordinal: 0,
                    submission_index: 1,
                    support_control: 0xe0a0_0001,
                    support_flags: 0,
                },
                context.timestamps,
                raw,
            )
            .map_err(|_| EINVAL)
        })?;
        queue
            .compute
            .with_descriptor_mut(descriptor_slot, |raw| {
                self.log_record("descriptor", &raw[..raw.len().min(320)]);
                Ok(())
            })?;

        let command_state = g17_submission::prepare_g17p_compute_cl_command_state(
            descriptor_low,
            queue.submission_ordinal,
        )
        .map_err(|_| EINVAL)?;
        let prepared_entry = g17_submission::prepare_g17p_cl_kick_entry(
            queue.producer,
            g17_submission::G17PClKickEntryOperands {
                descriptor_flag_4c: false,
                descriptor_flag_5c8: false,
                converted_command_timestamp: 1,
                barriers: &[],
                mcache: None,
                payload: [descriptor_high, queue.compute.queue_record_gpu_va()],
                event_mask: command_state.event_mask,
                rce_kind: 0,
                rce_bindings: command_state.rce_bindings,
                auxiliary: context.auxiliary,
            },
        )
        .map_err(|_| EINVAL)?;
        if prepared_entry.entry_index != 1
            || prepared_entry.entry_offset != queue.producer.geometry.entry_stride()
            || prepared_entry.current_timestamp != queue.producer.geometry.timestamp_seed()
        {
            return Err(EINVAL);
        }
        let backend = g17_submission::T8140_G17P_CL_BACKEND;
        if !g17_submission::g17p_first_bind_tag15_required(true)
            || !g17_submission::g17p_first_bind_tag16_required(backend, true)
        {
            return Err(EINVAL);
        }
        let mut entry_signal =
            [0u8; g17_submission::G17P_COMPUTE_OPTIONAL_EVENT_SIZE];
        // Tag 16 names the last retired entry. A newly installed queue has no
        // predecessor, so the protocol value is zero.
        let first_bind_old_timestamp = prepared_entry.current_timestamp.saturating_sub(1);
        g17_submission::encode_g17p_compute_optional_event(
            g17_submission::G17PComputeOptionalEvent {
                queue_id: u16::from(queue.queue_id),
                old_timestamp: first_bind_old_timestamp,
            },
            &mut entry_signal,
        )
        .map_err(|_| EINVAL)?;
        if let Ok(window) = queue.compute.g17p_preempt_argument_window() {
            // +0x1480 is where 0x1a4d0..0x1a4e8 point; +0x14a0 (this dump's
            // +0x20) is the add3 operand table. Populated for the proof path,
            // untouched for a Honeykrisp dispatch.
            self.log_record("preempt+0x1480", &window);
        }
        self.log_record("cl-kick-entry", &prepared_entry.entry);
        self.log_record("tag16-entry-signal", &entry_signal);

        let sksm_entry_aliases = queue.storage.entry_aliases()?;
        // Tag 15 installs a queue exactly once. This path is reachable only for
        // the prepared first bind; retained submissions use the repeat encoder
        // with the install flag clear.
        let install_queue = matches!(
            queue.hardware_state,
            G17PSksmQueueHardwareState::PreparedMemory
        );
        // The single most diagnostic line for a client handoff. The tag-15
        // queue-install flag must be published exactly ONCE per boot -- on the
        // first client's first bind. A second occurrence means some path has
        // handed the graph on by rebuilding it, and the firmware is being told
        // to install a QID it already has, which reprograms a live KSMFE queue
        // and leaves the submission published-but-never-completed.
        dev_info!(
            self.resources.dev().as_ref(),
            "G17PMARK tag15-install i={} z={} r={} h={} qid={} flag={} (first-bind chronology; a second install on the SAME qid in one boot is the second-client bug)\n",
            {
                let n = G17P_MARK_TAG15_INSTALLS.fetch_add(1, Ordering::Relaxed) + 1;
                n
            },
            G17P_MARK_ZERO_DURATION.load(Ordering::Relaxed),
            G17P_MARK_GATE_REBUILDS.load(Ordering::Relaxed),
            G17P_MARK_RETAINED_HANDOFFS.load(Ordering::Relaxed),
            queue.queue_id,
            install_queue as u32,
        );
        let mut group = queue
            .compute
            .stage_initial_channel_group_without_fallback_event(
                descriptor_high,
                sksm_entry_aliases,
                shared_control,
                queue.storage.channel_control_gpu_va(),
                queue.queue_id,
                install_queue,
            )?;
        let lifecycle = g17_submission::prepare_g17p_direct_cl_lifecycle(
            queue.direct_lifecycle,
            group.queue,
            true,
            backend,
        )
        .map_err(|_| EINVAL)?;
        if !lifecycle.publish_config_update
            || !lifecycle.publish_entry_signal
            || lifecycle.correlation_token == 0
        {
            return Err(EINVAL);
        }
        if *module_parameters::g17p_fault_report.value() != 0 {
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P rec tag15: install_queue={} descriptor_high={:#x} shared_control={:#x} channel_control={:#x} group(queue={:#x} optional={:#x} event={:#x} write_index={})\n",
                install_queue,
                descriptor_high,
                shared_control,
                queue.storage.channel_control_gpu_va(),
                group.queue,
                group.optional,
                group.event,
                group.write_index,
            );
        }
        self.arm_compute_completion(queue, descriptor_high)?;
        self.publish_compute_b2_once(queue)?;
        submission_phase.store(G17P_SUBMIT_PHASE_B2, Ordering::Release);
        queue
            .compute
            .publish_initial_config_update_pointer(group)?;
        submission_phase.store(G17P_SUBMIT_PHASE_TAG15, Ordering::Release);
        self.register_prepared_compute_sksm_queue(registers, queue)?;
        submission_phase.store(G17P_SUBMIT_PHASE_QID, Ordering::Release);
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P submitBuffer/configureHardware: tag15 published, then QID 4 configured and enabled; writing first SKSM entry\n"
        );
        if g17p_compute_sksm_entry_enabled() {
            queue.storage.write_entry(
                prepared_entry.entry_offset as usize,
                prepared_entry.zero_length as usize,
                &prepared_entry.entry,
            )?;
        }
        fence(Ordering::SeqCst);
        queue.producer = prepared_entry.producer_after;
        submission_phase.store(G17P_SUBMIT_PHASE_SKSM_ENTRY, Ordering::Release);
        let descriptor_generation =
            g17_submission::advance_g17p_compute_descriptor_generation(0, 0)
                .map_err(|_| EINVAL)?;
        queue.direct_lifecycle = lifecycle.next;
        let native_order = *module_parameters::g17p_native_order.value() != 0;
        let add_kicks = g17_submission::prepare_g17p_ksm_add_kicks_command(
            g17_submission::G17PKsmAddKicksCommand {
                queue_id: queue.queue_id,
                add_count: descriptor_generation,
                queue_priority: queue.producer.completion_selector,
                entry_stamp: prepared_entry.current_timestamp,
            },
        )
        .map_err(|_| EINVAL)?;
        self.log_record("tag14-add-kicks", &add_kicks.bytes);

        let skip = *module_parameters::g17p_skip.value();
        if native_order {
            let defer_kicks = *module_parameters::g17p_defer_add_kicks.value() != 0;
            let after_kicks = if skip & 1 != 0 {
                dev_info!(self.resources.dev().as_ref(), "G17P bisect: tag-14 AddKicks SKIPPED\n");
                2
            } else if defer_kicks {
                self.pending_add_kicks = Some(add_kicks.bytes);
                dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P compute: tag-14 AddKicks deferred until recovery closes\n"
                );
                2
            } else {
                queue
                    .compute
                    .publish_add_kicks_command(2, &add_kicks.bytes)?
            };
            queue.producer.last_add_kicks_timestamp = prepared_entry.current_timestamp;
            fence(Ordering::SeqCst);
            submission_phase.store(G17P_SUBMIT_PHASE_ADD_KICKS, Ordering::Release);
            group.write_index = if skip & 2 != 0 {
                dev_info!(self.resources.dev().as_ref(), "G17P bisect: tag-16 EntrySignal SKIPPED\n");
                after_kicks
            } else {
                queue
                    .compute
                    .publish_initial_entry_signal_at(after_kicks, &entry_signal)?
            };
            submission_phase.store(G17P_SUBMIT_PHASE_TAG16, Ordering::Release);
        } else {
            queue
                .compute
                .publish_initial_entry_signal(&entry_signal)?;
            submission_phase.store(G17P_SUBMIT_PHASE_TAG16, Ordering::Release);
            group.write_index = queue
                .compute
                .publish_add_kicks_command(group.write_index, &add_kicks.bytes)?;
            queue.producer.last_add_kicks_timestamp = prepared_entry.current_timestamp;
            fence(Ordering::SeqCst);
            submission_phase.store(G17P_SUBMIT_PHASE_ADD_KICKS, Ordering::Release);
        }
        let prepared = self
            .resources
            .publish_compute_channel(group.queue, group.write_index, queue.queue_id)?;
        submission_phase.store(G17P_SUBMIT_PHASE_CL2, Ordering::Release);
        if lifecycle.notifications.activation != Some(prepared.doorbell) {
            return Err(EINVAL);
        }
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: native-direct-first-v2 qid {} entry_offset {:#x} entry_low {:#x} entry_high {:#x} timestamp {:#x} descriptor_generation {} tag15_producer 2 tag16_producer 3 tag14_producer {} addkicks-via-channel-command=true correlation_token {}\n",
            queue.queue_id,
            prepared_entry.entry_offset,
            sksm_entry_aliases.low(),
            sksm_entry_aliases.high(),
            prepared_entry.current_timestamp,
            descriptor_generation,
            group.write_index,
            lifecycle.correlation_token
        );
        queue.submission_ordinal = 1;
        self.log_compute_ring_state(queue, "first-post-cl2");
        self.log_compute_outer_channel("first-post-cl2");
        Ok(lifecycle.notifications)
    }

    fn submit_translated_repeat_channel(
        &mut self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        submission_phase: &AtomicU32,
        binding: &G17PComputeUserBinding,
        command: &g17_uapi::TranslatedComputeCommand,
        context: G17PComputeSubmissionContext<'_>,
        add3_buffers: Option<[u64; 3]>,
    ) -> Result<g17_submission::PreparedG17PDirectClNotifications> {
        if queue.hardware_state != G17PSksmQueueHardwareState::HardwareRegistered
            || queue.submission_ordinal == 0
        {
            return Err(EINVAL);
        }
        let _ = registers;
        // The add3 operand table lives in the retained preempt object and is
        // unchanged by a completed submission, but the probe may have staged
        // new buffers, so honour the flag exactly as the first bind does.
        if let Some(buffers) = add3_buffers {
            queue.compute.write_g17p_add3_resource_table(buffers)?;
        }

        let ordinal = queue.submission_ordinal;
        let next_ordinal = ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        let sequence_ordinal: u32 = 0;
        let work_ordinal = 0;
        let queue_submission = sequence_ordinal.checked_add(1).ok_or(EINVAL)?;
        let stamp = g17_submission::decode_kick_timestamp(queue.producer.current_timestamp)
            .map_err(|_| EINVAL)?;
        let descriptor_slot = stamp.stamp_index;
        let (descriptor_high, _) = queue.compute.descriptor_gpu_vas(descriptor_slot);
        let descriptor_low = binding.mappings.descriptor_gpu_va(descriptor_slot);
        let user_layout = binding.mappings.layout();
        let shared_control = queue.storage.shared_support_gpu_va();
        let objects = g17_compute::ComputeDescriptorObjects {
            scheduler_record: queue.compute.scheduler_record_gpu_va(),
            descriptor_low_alias: descriptor_low,
            dispatch_a: queue.compute.dispatch_a_gpu_va(),
            dispatch_b: queue.compute.dispatch_b_gpu_va(),
            status_a: queue.compute.status_a_gpu_va(),
            status_b: queue.compute.status_b_gpu_va(),
            shared_control,
            zero_page: queue.compute.zero_page_gpu_va(),
        };
        queue.compute.with_descriptor_mut(descriptor_slot, |raw| {
            g17_uapi::build_compute_descriptor(
                command,
                g17_uapi::ComputeInternalState {
                    preempt_base: user_layout.preempt,
                    dispatch_identity: context.dispatch_identity,
                    context_id: context.context_id,
                    work_ordinal,
                    robustness: user_layout.robustness,
                    operand_state_base: user_layout.operand_state,
                    execution_gate: context.execution_gate,
                },
                objects,
                g17_compute::ComputeDescriptorMetadata {
                    submit_sequence: sequence_ordinal as u64,
                    context_id: context.context_id,
                    grid_index: u32::from(queue.queue_id),
                    work_ordinal,
                    queue_submission,
                    queue_ordinal: sequence_ordinal,
                    submission_index: queue_submission,
                    support_control: 0xe0a0_0001,
                    support_flags: 0,
                },
                context.timestamps,
                raw,
            )
            .map_err(|_| EINVAL)
        })?;

        let command_state = g17_submission::prepare_g17p_compute_cl_command_state(
            descriptor_low,
            0,
        )
        .map_err(|_| EINVAL)?;
        let prepared_entry = g17_submission::prepare_g17p_cl_kick_entry(
            queue.producer,
            g17_submission::G17PClKickEntryOperands {
                descriptor_flag_4c: context.descriptor_flag_4c,
                descriptor_flag_5c8: context.descriptor_flag_5c8,
                converted_command_timestamp: context.converted_command_timestamp,
                barriers: context.barriers,
                mcache: context.mcache,
                payload: [descriptor_high, queue.compute.queue_record_gpu_va()],
                event_mask: command_state.event_mask,
                rce_kind: context.rce_kind,
                rce_bindings: command_state.rce_bindings,
                auxiliary: context.auxiliary,
            },
        )
        .map_err(|_| EINVAL)?;
        self.log_record("repeat-cl-kick-entry", &prepared_entry.entry);
        if prepared_entry.entry_index != descriptor_slot {
            return Err(EINVAL);
        }
        let backend = g17_submission::T8140_G17P_CL_BACKEND;
        let mut entry_signal = [0u8; g17_submission::G17P_COMPUTE_OPTIONAL_EVENT_SIZE];
        // Tag 16 names the last retired entry, never the kick being published.
        let entry_old_timestamp = queue.producer.previous_timestamp;
        g17_submission::encode_g17p_compute_optional_event(
            g17_submission::G17PComputeOptionalEvent {
                queue_id: u16::from(queue.queue_id),
                old_timestamp: entry_old_timestamp,
            },
            &mut entry_signal,
        )
        .map_err(|_| EINVAL)?;
        let sksm_entry_aliases = queue.storage.entry_aliases()?;
        // Reclaim everything the firmware has finished with, so slots come back
        // round instead of the ring being single-use.
        let trace = Self::compute_trace_enabled(ordinal);
        match queue.compute.recycle_consumed_ring_slots() {
            Ok(_) if !trace => {}
            Ok(0) => {}
            Ok(reclaimed) => dev_info!(
                self.resources.dev().as_ref(),
                "G17P compute: reclaimed {} consumed item-ring slots\n",
                reclaimed
            ),
            Err(error) => dev_err!(
                self.resources.dev().as_ref(),
                "G17P compute: item-ring reclaim failed ({:?}); publishing without it\n",
                error
            ),
        }
        match *module_parameters::g17p_repeat_context.value() {
            0 => {}
            2 => queue
                .compute
                .write_repeat_queue_context_item(
                    descriptor_slot,
                    descriptor_high,
                    queue.queue_id,
                )?,
            _ => queue.compute.refresh_repeat_queue_context(descriptor_high)?,
        }
        let rewind = *module_parameters::g17p_repeat_ring.value() == 0;
        let group = queue.compute.stage_repeat_config_update(
            sksm_entry_aliases,
            shared_control,
            queue.storage.channel_control_gpu_va(),
            queue.queue_id,
            rewind,
        )?;
        // `queue_config_dirty` is already false here, so the lifecycle reports
        // no first-bind publications and -- because the channel identity is
        // unchanged -- no priority-2 activation either. The repeat notifies
        // with the direct EP 0x21 kick alone.
        let mut lifecycle = g17_submission::prepare_g17p_direct_cl_lifecycle(
            queue.direct_lifecycle,
            group.queue,
            true,
            backend,
        )
        .map_err(|_| EINVAL)?;
        if *module_parameters::g17p_repeat_activation.value() != 0
            && lifecycle.notifications.activation.is_none()
        {
            lifecycle.notifications.activation =
                Some(g17_submission::encode_g17p_cl_activation(2).map_err(|_| EINVAL)?);
        }

        let free = queue.compute.compute_ring_free_slots()?;
        if free < G17P_COMPUTE_REPEAT_RING_RECORDS {
            dev_err!(
                self.resources.dev().as_ref(),
                "G17P compute: item ring full, {} slots free but {} needed; the firmware consumer has stopped advancing\n",
                free,
                G17P_COMPUTE_REPEAT_RING_RECORDS
            );
            return Err(ENOSPC);
        }
        let producer_before = group.write_index;
        self.arm_compute_completion(queue, descriptor_high)?;
        let after_command = self.publish_repeat_record(
            queue,
            "the tag-3 command pointer",
            producer_before,
            G17PRepeatRecord::Pointer(descriptor_high),
        )?;
        submission_phase.store(G17P_SUBMIT_PHASE_B2, Ordering::Release);
        let after_config = self.publish_repeat_record(
            queue,
            "the tag-15 ConfigUpdate pointer",
            after_command,
            G17PRepeatRecord::Pointer(group.optional),
        )?;
        submission_phase.store(G17P_SUBMIT_PHASE_TAG15, Ordering::Release);

        if g17p_compute_sksm_entry_enabled() {
            queue.storage.write_entry(
                prepared_entry.entry_offset as usize,
                prepared_entry.zero_length as usize,
                &prepared_entry.entry,
            )?;
        }
        fence(Ordering::SeqCst);
        queue.producer = prepared_entry.producer_after;
        submission_phase.store(G17P_SUBMIT_PHASE_SKSM_ENTRY, Ordering::Release);

        let descriptor_generation =
            g17_submission::advance_g17p_compute_descriptor_generation(0, 0)
                .map_err(|_| EINVAL)?;
        let add_kicks = g17_submission::prepare_g17p_ksm_add_kicks_command(
            g17_submission::G17PKsmAddKicksCommand {
                queue_id: queue.queue_id,
                add_count: descriptor_generation,
                queue_priority: queue.producer.completion_selector,
                entry_stamp: prepared_entry.current_timestamp,
            },
        )
        .map_err(|_| EINVAL)?;
        let after_kicks = self.publish_repeat_record(
            queue,
            "the tag-14 AddKicks record",
            after_config,
            G17PRepeatRecord::AddKicks(&add_kicks.bytes),
        )?;
        queue.producer.last_add_kicks_timestamp = prepared_entry.current_timestamp;
        fence(Ordering::SeqCst);
        submission_phase.store(G17P_SUBMIT_PHASE_ADD_KICKS, Ordering::Release);
        let write_index = self.publish_repeat_record(
            queue,
            "the tag-16 EntrySignal record",
            after_kicks,
            G17PRepeatRecord::EntrySignal(&entry_signal),
        )?;
        submission_phase.store(G17P_SUBMIT_PHASE_TAG16, Ordering::Release);

        // Depth 320 used to fail BEFORE reaching this point: the host-side
        // publisher compared the ring transition's `(producer + 1) % count`
        // against a locally computed `producer + 1`, so the fourth record of
        // submission 320 -- the one that lands on the ring's last slot -- was
        // refused with EBUSY. That is fixed in `publish_command_pointer_at` and
        // its two siblings, so a wrapped write index now actually reaches the
        // outer channel. Whether the firmware reads a producer of 0 as
        // "wrapped" or as "empty" is a separate, still-open question, and
        // `g17p_ring_wrap=1` reports the head as `count` instead so one boot
        // decides it. With `g17p_ring_limit=32` the wrap arrives on submission
        // 8, so that boot costs 20 iterations rather than 400.
        let reported_write_index = if write_index == 0
            && *module_parameters::g17p_ring_wrap.value() != 0
        {
            let entries = queue.compute.compute_ring_entries()?;
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P compute: item-ring producer wrapped to 0; reporting write index {} to the outer channel\n",
                entries
            );
            entries
        } else {
            write_index
        };
        let prepared = self
            .resources
            .publish_compute_channel(group.queue, reported_write_index, queue.queue_id)?;
        submission_phase.store(G17P_SUBMIT_PHASE_CL2, Ordering::Release);
        let _ = prepared;
        queue.direct_lifecycle = lifecycle.next;
        queue.submission_ordinal = next_ordinal;
        // Optional settle. The 319-deep build padded every submission with
        // several console writes here; this makes that padding explicit and
        // measurable instead of an accident of log verbosity.
        let settle = *module_parameters::g17p_repeat_settle_us.value();
        if settle != 0 {
            fsleep(Delta::from_micros(settle as i64));
        }
        self.log_compute_ring_state(queue, "repeat-post-cl2");
        if !trace {
            return Ok(lifecycle.notifications);
        }
        self.log_compute_outer_channel("repeat-post-cl2");
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: repeat submission {} qid {} descriptor-slot {} descriptor {:#x} stamp {:#x} ring {} -> {} activation {:?} kick {:?}\n",
            ordinal,
            queue.queue_id,
            descriptor_slot,
            descriptor_high,
            prepared_entry.current_timestamp,
            producer_before,
            write_index,
            lifecycle.notifications.activation,
            lifecycle.notifications.direct_kick
        );
        Ok(lifecycle.notifications)
    }

    /// Build and publish one complete userspace compute command.
    ///
    /// Descriptor and CL-entry slots share the configured stamp lifetime. The
    /// descriptor is written first, then its upper address and the retained
    /// queue record are placed in the CL payload before the ordered SKSM kick.
    pub(crate) fn submit_translated_compute(
        &mut self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        submission_phase: &AtomicU32,
        binding: &G17PComputeUserBinding,
        command: &g17_uapi::TranslatedComputeCommand,
        context: G17PComputeSubmissionContext<'_>,
    ) -> Result<Option<g17_submission::PreparedG17PDirectClNotifications>> {
        if queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
        {
            return Err(EINVAL);
        }
        // Prove the kick's translation regime before anything is published.
        //
        // The kick declares hardware context 2, and every later workload
        // context 3 (`T8140_COMPUTE_CTXS`). If those entries do not name this
        // binding's VM the GPU has no root to walk at all: the work is
        // accepted and silently never runs -- no fault, no blamed queue, a
        // zero-duration retire, and a destination buffer that comes back
        // untouched. That is indistinguishable from "the shader did nothing",
        // which is exactly how it stayed hidden.
        //
        // `install_t8140_compute_context_alias` runs only on the bind that
        // creates a client's retained binding. Every repeat submission on that
        // binding, and any submission after another VM was created or
        // destroyed in between, previously reached the kick with nobody having
        // re-checked those two entries.
        if let Some(execution) = binding._execution_context.as_ref() {
            if context.context_id != execution.context_id() || !execution.is_current() {
                return Err(EFAULT);
            }
            dev_info!(self.resources.dev().as_ref(),
                "G17P independent compute root: qid={} context={} vm={} root={:#x}\n",
                queue.queue_id, execution.context_id(), execution.vm_id(), execution.root());
        } else {
        self.resources
            .uat()
            .ensure_t8140_compute_context_alias(binding.vm_bind())?;
        {
            let roots = self.resources.uat().t8140_compute_context_roots();
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P vm-audit(pre-kick): vm {} root={:#x} ctx1 ttb0={:#x} ctx2 ttb0={:#x} ttb1={:#x} ctx3 ttb0={:#x} ttb1={:#x}\n",
                binding.vm_bind().vm_id(),
                binding.vm_bind().root(),
                roots[1].0,
                roots[2].0,
                roots[2].1,
                roots[3].0,
                roots[3].1,
            );
        }
        }
        // Which chronology a command needs is a property of the *queue*, not
        // of the command's shape. A queue that has never been configured on
        // the SKSM bridge needs the first-bind sequence above; one that has
        // needs the post-registration sequence below. `g17p_add3_buffers` only
        // says whether the probe wants its private operand table written into
        // the preempt object, so it must not be what decides the route -- that
        // coupling is exactly why a Honeykrisp dispatch, which carries its
        // bindings in the command stream and therefore sets no flag and
        // declares no attachments, fell through to the ENOTSUPP gate below
        // while the add3 probe reached the firmware.
        let first_bind = queue.hardware_state == G17PSksmQueueHardwareState::PreparedMemory
            && queue.submission_ordinal == 0;
        // Which of the three chronologies this submit takes. A submit that
        // produces no record dump at all took a path with no logging, so name
        // the route before branching -- otherwise "no output" is ambiguous
        // between "different path" and "never got here".
        if *module_parameters::g17p_fault_report.value() != 0 {
            let attachments = command.attachments.as_slice();
            let route = if first_bind {
                "first-bind"
            } else if queue.hardware_state
                != G17PSksmQueueHardwareState::HardwareRegistered
            {
                "REFUSED-not-registered"
            } else if queue.submission_ordinal != 0 {
                "repeat"
            } else {
                "steady-state"
            };
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P rec route={} hardware_state={:?} submission_ordinal={} first_bind={} add3_proof={}\n",
                route,
                queue.hardware_state,
                queue.submission_ordinal,
                first_bind,
                command.g17p_add3_buffers.is_some(),
            );
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P rec uapi: usc_exec_base={:#x} cs=[{:#x},{:#x}) term={:#x} sampler={:#x}/{} attachments={}\n",
                command.usc_exec_base,
                command.control_stream_base,
                command.control_stream_end,
                command.control_stream_terminator,
                command.sampler_heap,
                command.sampler_count,
                attachments.len(),
            );
            for (index, entry) in attachments.iter().enumerate() {
                dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P rec uapi: attachment[{}] address={:#x} size={:#x}\n",
                    index,
                    entry.address,
                    entry.size,
                );
            }
        }
        // Which chronology to run is decided by the queue, never by the
        // command's flags. The add3 probe used to force the first-bind route
        // unconditionally, which meant its second submission either hit the
        // `submission_ordinal != 0` gate or re-ran the first-bind builder on a
        // live queue and reset the item-ring producer under the firmware.
        if first_bind {
            return self
                .submit_translated_first_bind_channel(
                    registers,
                    queue,
                    submission_phase,
                    binding,
                    command,
                    context,
                    command.g17p_add3_buffers,
                )
                .map(Some);
        }

        // Post-registration path. It assumes the tag/configure chronology has
        // already run, so a still-prepared queue is refused here exactly as
        // before the queue has been registered.
        if queue.hardware_state != G17PSksmQueueHardwareState::HardwareRegistered {
            return Err(ENOTSUPP);
        }

        if queue.submission_ordinal != 0 {
            return self
                .submit_translated_repeat_channel(
                    registers,
                    queue,
                    submission_phase,
                    binding,
                    command,
                    context,
                    command.g17p_add3_buffers,
                )
                .map(Some);
        }

        let timestamp = g17_submission::decode_kick_timestamp(queue.producer.current_timestamp)
            .map_err(|_| EINVAL)?;
        let (descriptor_high, _) = queue.compute.descriptor_gpu_vas(timestamp.stamp_index);
        let descriptor_low = binding
            .mappings
            .descriptor_gpu_va(timestamp.stamp_index);
        let command_state = g17_submission::prepare_g17p_compute_cl_command_state(
            descriptor_low,
            queue.submission_ordinal,
        )
        .map_err(|_| EINVAL)?;
        let payload = [descriptor_high, queue.compute.queue_record_gpu_va()];
        let operands = command.entry_operands(g17_uapi::ComputeEntryContext {
            descriptor_flag_4c: context.descriptor_flag_4c,
            descriptor_flag_5c8: context.descriptor_flag_5c8,
            converted_command_timestamp: context.converted_command_timestamp,
            barriers: context.barriers,
            mcache: context.mcache,
            payload,
            event_mask: command_state.event_mask,
            rce_kind: context.rce_kind,
            rce_bindings: command_state.rce_bindings,
            auxiliary: context.auxiliary,
        });
        let prepared = g17_submission::prepare_g17p_cl_kick_entry(queue.producer, operands)
            .map_err(|_| EINVAL)?;
        if prepared.entry_index != timestamp.stamp_index {
            return Err(EINVAL);
        }
        // The fall-through path. It publishes a kick entry WITHOUT ever having
        // run the first-bind chronology (tag 3 -> 15 -> QID configure -> SKSM
        // entry -> 14 -> 16 -> CL_2) for this binding, so name it loudly.
        self.log_record("fallthrough-cl-kick-entry", &prepared.entry);

        let ordinal = queue.submission_ordinal;
        let next_ordinal = ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        let queue_submission = ordinal.checked_add(1).ok_or(EINVAL)?;
        let user_layout = binding.mappings.layout();
        let preempt_base = user_layout.preempt;
        let robustness = user_layout.robustness;
        let operand_state_base = user_layout.operand_state;
        let shared_control = queue.storage.shared_support_gpu_va();
        let objects = g17_compute::ComputeDescriptorObjects {
            scheduler_record: queue.compute.scheduler_record_gpu_va(),
            descriptor_low_alias: descriptor_low,
            dispatch_a: queue.compute.dispatch_a_gpu_va(),
            dispatch_b: queue.compute.dispatch_b_gpu_va(),
            status_a: queue.compute.status_a_gpu_va(),
            status_b: queue.compute.status_b_gpu_va(),
            shared_control,
            zero_page: queue.compute.zero_page_gpu_va(),
        };
        queue
            .compute
            .with_descriptor_mut(prepared.entry_index, |raw| {
                g17_uapi::build_compute_descriptor(
                    command,
                    g17_uapi::ComputeInternalState {
                        preempt_base,
                        dispatch_identity: context.dispatch_identity,
                        context_id: context.context_id,
                        work_ordinal: ordinal,
                        robustness,
                        operand_state_base,
                        execution_gate: context.execution_gate,
                    },
                    objects,
                    g17_compute::ComputeDescriptorMetadata {
                        submit_sequence: ordinal as u64,
                        context_id: context.context_id,
                        grid_index: u32::from(queue.queue_id),
                        work_ordinal: ordinal,
                        queue_submission,
                        queue_ordinal: ordinal,
                        submission_index: queue_submission,
                        support_control: 0xe0a0_0001,
                        support_flags: 0,
                    },
                    context.timestamps,
                    raw,
                )
                .map_err(|_| EINVAL)
            })?;

        fence(Ordering::SeqCst);
        let timestamp = self.publish_compute_entry(registers, queue, prepared)?;
        queue.submission_ordinal = next_ordinal;
        let _ = timestamp;
        Ok(None)
    }

    /// Log the queue-graph ring cursors. DRAM only -- safe on a timeout path,
    /// unlike anything that reads the sgx register file once the cores have
    /// powered back down.
    pub(crate) fn log_compute_ring_state(
        &mut self,
        queue: &mut G17PSksmQueue,
        label: &'static str,
    ) {
        // Never gate the failure samples: they are the whole point.
        if !Self::compute_trace_enabled(queue.submission_ordinal)
            && label != "timeout"
            && label != "publish-failed"
        {
            return;
        }
        let state = match queue.compute.read_compute_ring_state() {
            Ok(state) => state,
            Err(error) => {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P ring[{}]: unreadable ({:?})\n",
                    label,
                    error
                );
                return;
            }
        };
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P ring[{}]: queue {:#x} consumer {} producer {} mirror {:#x} count {:#x} submitted {} ordinal {} reclaim-cursor {} reclaimed-total {} items {:#x?}\n",
            label,
            state.queue,
            state.consumer,
            state.producer,
            state.mirror,
            state.count,
            queue.producer.submitted,
            queue.submission_ordinal,
            state.reclaimed,
            state.reclaimed_total,
            state.items
        );
    }

    fn compute_trace_enabled(ordinal: u32) -> bool {
        let depth = *module_parameters::g17p_trace_depth.value();
        depth == 0 || ordinal <= depth
    }

    /// Log the OUTER CL_2 work-channel cursors and its most recent slot.
    ///
    /// The inner item ring and the outer channel are two different rings and
    /// either can stall independently: the item ring says whether the firmware
    /// walked the records we appended, this says whether it ever looked at the
    /// slot that points at them. Also DRAM only -- `primary_state` and
    /// `shared_cluster` are driver-allocated objects, not MMIO.
    pub(crate) fn log_compute_outer_channel(&mut self, label: &'static str) {
        let snapshot = match self.resources.snapshot_compute_channel() {
            Ok(snapshot) => snapshot,
            Err(error) => {
                dev_err!(
                    self.resources.dev().as_ref(),
                    "G17P outer[{}]: unreadable ({:?})\n",
                    label,
                    error
                );
                return;
            }
        };
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P outer[{}]: table {} ring {:#x} cursors(actual,mirror,producer)={:?} slot {} queue {:#x} kind {} flags {:#x}\n",
            label,
            snapshot.table_index,
            snapshot.ring,
            snapshot.cursors,
            snapshot.slot_index,
            snapshot.slot_queue,
            snapshot.slot_kind,
            snapshot.slot_flags
        );
    }

    /// Zero the KSM completion record and remember which descriptor the
    /// submission about to be published owns.
    ///
    /// Must run after the descriptor is encoded and before ANY firmware-
    /// visible publication, so that from the firmware's first opportunity to
    /// write the record onwards, "populated" means "written for this
    /// submission". Without it the record left by a previous submission
    /// satisfies the next wait immediately -- which is how a run whose GPU
    /// cores never powered (`gpc-state = 0x0`) still reported a woken doorbell
    /// and retired.
    fn arm_compute_completion(
        &mut self,
        queue: &mut G17PSksmQueue,
        descriptor: u64,
    ) -> Result {
        if queue.independent_context {
            let count = queue.submission_ordinal.checked_add(1).ok_or(EOVERFLOW)?;
            let [previous, completed] = queue.compute.publish_compute_submitted_count(count)?;
            if queue.submission_ordinal < 2 {
                dev_info!(self.resources.dev().as_ref(),
                    "G17P compute TSQ: qid={} target={} previous={} completed={} before-publication\n",
                    queue.queue_id, count, previous, completed);
            }
        }
        let previous = if queue.independent_context { [0, 0] }
            else { self.resources.clear_compute_completion()? };
        queue.completion_descriptor = descriptor;
        self.last_published_descriptor = descriptor;
        // Arm the completion stamp as well. The KSM completion record is
        // produced by the KSM hardware block and consumed by the firmware at
        // the TOP of the completion interrupt; the firmware's GPU
        // cache-maintenance command and the stamp store both happen after
        // that. So the record proves scheduling, and only the stamp proves
        // visibility -- see `await_compute_stamp`.
        if *module_parameters::g17p_completion_stamp.value() != 0 {
            let discarded = queue.compute.arm_compute_stamp()?;
            self.compute_stamp_armed = true;
            queue.completion_stamp_armed = true;
            if discarded != 0 && Self::compute_trace_enabled(queue.submission_ordinal) {
                dev_info!(
                    self.resources.dev().as_ref(),
                    "G17P completion stamp: discarded {:#x} before publishing {:#x}\n",
                    discarded,
                    descriptor
                );
            }
        }
        // Do NOT reset `last_completion_end` here. It is the floor that makes
        // "strictly newer than the completion already consumed" mean anything,
        // and zeroing it once per submission made that test vacuous -- which
        // is why tightening the scan from `end == last_consumed_end` to
        // `end <= last_consumed_end` measured as noise: with the floor at 0 the
        // two predicates are identical. GPU timestamps are monotonic across a
        // manager's lifetime, so the previous submission's end is a valid
        // floor for this one; the field is initialised to 0 when the manager is
        // constructed, and a recovery rebuilds the manager, so a teardown
        // resets it without any per-submission help.
        if previous[0] != 0 {
            dev_info!(
                self.resources.dev().as_ref(),
                "G17P completion: cleared stale record descriptor {:#x} end {:#x} before publishing {:#x}\n",
                previous[0],
                previous[1],
                descriptor
            );
        }
        Ok(())
    }

    /// Completion ingress for the descriptor-provided completed-kick count.
    /// Sample everything host-visible that describes whether the retained
    /// compute queue and the firmware still agree about QID 4.
    ///
    /// Taken either side of a serviced firmware recovery, so the divergence a
    /// fault introduces can be SEEN rather than inferred. A boot with no fault
    /// hands the graph to any number of sequential clients; one GMMU fault in
    /// between and every later kick is accepted, retired and never executed --
    /// so whatever moves across this boundary is the mechanism.
    ///
    /// HOST DRAM ONLY, deliberately. Every field is a read of an object this
    /// driver allocated or of the firmware handoff region. Nothing here touches
    /// the sgx register file: a recovery is serviced with the GPU cores gated,
    /// and an sgx read in that state takes an asynchronous SError and panics
    /// the machine. That is also why the firmware's KSM queue slot at
    /// `0x127628 + qid*0x28` is NOT sampled here -- it lives in the ASC's own
    /// address space, reachable only through sgx MMIO or the firmware's own
    /// mapping, so there is no safe host read of it on this path.
    pub(crate) fn log_compute_recovery_alignment(
        &mut self,
        queue: &mut G17PSksmQueue,
        label: &'static str,
    ) {
        let fw_stamp = self.g17p_last_submitted_hw_timestamp(queue.queue_id as usize);
        let ring = queue.compute.read_compute_ring_state().ok();
        let free = queue.compute.compute_ring_free_slots().ok();
        let producer = queue.producer;
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P recovery-align[{}]: qid={} fw-last-submitted={:?} host(ordinal={} stamp={:#x} prev-valid={} prev-stamp={:#x} base={:#x} submitted={} addkicks={:#x}) ring(consumer={:?} producer={:?} free={:?})\n",
            label,
            queue.queue_id,
            fw_stamp,
            queue.submission_ordinal,
            producer.current_timestamp,
            producer.previous_valid,
            producer.previous_timestamp,
            producer.base_timestamp,
            producer.submitted,
            producer.last_add_kicks_timestamp,
            ring.as_ref().map(|state| state.consumer),
            ring.as_ref().map(|state| state.producer),
            free,
        );
        // The KSM pause-reason mask, read HOST-SAFELY.
        //
        // The mask itself lives at `sgx+0x2_11c8` and its refcount at the
        // firmware's `0x128a68`; both are inside the sgx 0x0..0x2ffff window,
        // which is unreachable from the AP and takes an asynchronous SError.
        // With the cores demonstrably gated after a fault-blamed recovery that
        // hazard is the NORMAL state on this path, so neither is read here.
        // The firmware publishes the same value itself: channel-0 KTrace code
        // 0x46 carries the pause reason, and the KTrace ring is host DRAM.
        // Needs `asahi.g17p_fw_trace=1` -- with tracing off the ring is empty
        // and `consumed` reads 0, which is why that is reported too.
        let ktrace = self.resources.drain_ktrace_ring(
            InstanceRole::Primary,
            *module_parameters::g17p_fw_ktrace_verbose.value() != 0,
            *module_parameters::g17p_fw_ktrace_budget.value(),
        );
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P recovery-align[{}]: ktrace(consumed={:?} pause-mask={:?} saw-addkicks={:?} lost={:?}) -- pause-mask needs g17p_fw_trace=1\n",
            label,
            ktrace.as_ref().map(|stats| stats.consumed),
            ktrace.as_ref().map(|stats| stats.last_pause_mask),
            ktrace.as_ref().map(|stats| stats.saw_add_kicks),
            ktrace.as_ref().map(|stats| stats.lost_records),
        );
        let status_a = self.resources.status_a_snapshot();
        let channel = self.resources.snapshot_compute_channel();
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P recovery-align[{}]: statusA(scan-active={:?} active={:?} scheduler-constructed={:?}) outer(cursors={:?} slot-queue={:?})\n",
            label,
            status_a.as_ref().map(|state| state.scan_active),
            status_a.as_ref().map(|state| state.active),
            status_a.as_ref().map(|state| state.scheduler_constructed),
            channel.as_ref().map(|state| state.cursors),
            channel.as_ref().map(|state| state.slot_queue),
        );
    }

    /// Realign the retained compute queue with the firmware after a recovery.
    ///
    /// A recovery is the one event that can move the firmware's idea of this
    /// QID without the host publishing anything, so continuity is exactly the
    /// assumption that must not be made across it. See `g17p_recovery_resync`
    /// for the individual arms; returns the mask actually applied.
    pub(crate) fn resynchronise_compute_queue_after_recovery(
        &mut self,
        queue: &mut G17PSksmQueue,
    ) -> Result<u32> {
        let mode = *module_parameters::g17p_recovery_resync.value();
        if mode == 0 {
            return Ok(0);
        }
        let mut applied = 0u32;
        if mode & 1 != 0 && queue.producer.previous_valid {
            queue.producer.previous_valid = false;
            queue.producer.previous_timestamp = 0;
            applied |= 1;
        }
        if mode & 2 != 0 {
            if let Ok((valid, stamp)) =
                self.g17p_last_submitted_hw_timestamp(queue.queue_id as usize)
            {
                // Forward only. Rewinding the stamp is what makes a kick look
                // already retired, which is the failure this whole line of work
                // started from.
                if valid != 0 && stamp >= queue.producer.current_timestamp {
                    let aligned = stamp.wrapping_add(1);
                    if g17_submission::decode_kick_timestamp(aligned).is_ok() {
                        queue.producer.current_timestamp = aligned;
                        queue.producer.base_timestamp = aligned;
                        applied |= 2;
                    }
                }
            }
        }
        if mode & 4 != 0 && queue.direct_lifecycle.cached_channel_identity != 0 {
            // Only the notification predicate: `publish_config_update` keys off
            // `queue_config_dirty`, which stays false, so this cannot re-assert
            // the tag-15 queue-install flag.
            queue.direct_lifecycle.cached_channel_identity = 0;
            applied |= 4;
        }
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P recovery-resync: qid={} mode={:#x} applied={:#x} -> stamp={:#x} prev-valid={} prev-stamp={:#x} channel-identity={:#x}\n",
            queue.queue_id,
            mode,
            applied,
            queue.producer.current_timestamp,
            queue.producer.previous_valid,
            queue.producer.previous_timestamp,
            queue.direct_lifecycle.cached_channel_identity,
        );
        Ok(applied)
    }

    pub(crate) fn resynchronise_compute_queue_after_abandon(
        &mut self,
        queue: &mut G17PSksmQueue,
    ) -> Result<u32> {
        let outstanding = queue.producer.submitted;
        if outstanding != 0 {
            queue.producer =
                g17_submission::complete_g17p_cl_kicks(queue.producer, outstanding)
                    .map_err(|_| EINVAL)?;
        }
        self.compute_stamp_armed = false;
        let stale = self.resources.clear_compute_completion()?;
        self.last_published_descriptor = 0;
        let ring = queue.compute.read_compute_ring_state().ok();
        let free = queue.compute.compute_ring_free_slots().ok();
        dev_info!(
            self.resources.dev().as_ref(),
            "G17P compute: abandoned kick written off on qid {}; outstanding {} -> 0, ordinal={} stamp={:#x} ring consumer={:?} producer={:?} free={:?} stale-record desc={:#x} end={:#x}\n",
            queue.queue_id,
            outstanding,
            queue.submission_ordinal,
            queue.producer.current_timestamp,
            ring.as_ref().map(|state| state.consumer),
            ring.as_ref().map(|state| state.producer),
            free,
            stale[0],
            stale[1],
        );
        Ok(outstanding)
    }

    pub(crate) fn complete_compute_entries(
        &self,
        queue: &mut G17PSksmQueue,
        completed_count: u32,
    ) -> Result {
        if queue.hardware_state != G17PSksmQueueHardwareState::HardwareRegistered
            || queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
        {
            return Err(EINVAL);
        }
        self.await_compute_stamp(queue);
        queue.producer = g17_submission::complete_g17p_cl_kicks(queue.producer, completed_count)
            .map_err(|_| EINVAL)?;
        Ok(())
    }

    /// Hold a retire until the firmware has published this submission's
    /// completion stamp, so that "complete" implies "the dispatch's writes are
    /// visible".
    ///
    /// Firmware RE (`b1`) settles the ordering. The KSM completion record is
    /// written by the KSM hardware block, and the firmware's completion
    /// interrupt handler CONSUMES it first (`0xd550` drains the ring, `0xab64`
    /// decodes one record). Only afterwards, in
    /// `AGFAcceleratorProcessTimeStampQueue` (`0x168d0`), does it issue the GPU
    /// cache-maintenance command to `sgx+0x1_8030`, poll `sgx+0x1_8038` bit 1
    /// until it clears, scan the fault units, and finally store the stamp
    /// value from descriptor `+0x0F50` through the pointer at descriptor
    /// `+0x0F40`. That pointer is this queue's support-page dispatch-A word.
    ///
    /// So the record's arrival is upstream of the flush and says nothing about
    /// the dispatch's stores; the stamp store is downstream of it and says
    /// exactly that. Waiting for the record and not the stamp is why a
    /// submission retired cleanly and the output buffer then took a further
    /// 0-200us to show correct values -- and why adding console verbosity or a
    /// settle "fixed" depth: both simply spent that time somewhere else.
    ///
    /// This is a data gate, not a settle: when the stamp is already present --
    /// which it will be whenever the driver was not the first thing to look --
    /// nothing is waited for at all. If the stamp never arrives the retire
    /// proceeds anyway, so the worst case is exactly the previous behaviour.
    fn await_compute_stamp(&self, queue: &mut G17PSksmQueue) {
        const POLL_US: u32 = 5;
        if *module_parameters::g17p_completion_stamp.value() == 0 || !self.compute_stamp_armed {
            return;
        }
        let budget_us = *module_parameters::g17p_completion_stamp_us.value();
        let mut waited_us = 0u32;
        loop {
            match queue.compute.read_compute_stamp() {
                Ok(0) => {}
                Ok(stamp) => {
                    if waited_us != 0 {
                        // Log the first wait unconditionally -- a quiet run
                        // must still say whether this gate does any work --
                        // and later ones only while tracing is on.
                        let first = self.compute_stamp_waits.fetch_add(1, Ordering::Relaxed) == 0;
                        if first || Self::compute_trace_enabled(queue.submission_ordinal) {
                            dev_info!(
                                self.resources.dev().as_ref(),
                                "G17P completion stamp: {:#x} arrived {} us after the KSM record (waits so far {})\n",
                                stamp,
                                waited_us,
                                self.compute_stamp_waits.load(Ordering::Relaxed)
                            );
                        }
                    }
                    return;
                }
                Err(error) => {
                    dev_warn!(
                        self.resources.dev().as_ref(),
                        "G17P completion stamp unreadable ({:?}); retiring on the KSM record alone\n",
                        error
                    );
                    return;
                }
            }
            if waited_us >= budget_us {
                // Rate-limited: hardware shows a real minority of submissions
                // never publish a stamp (19 of 300), and one console write per
                // occurrence costs about a millisecond, which would distort the
                // very timing this gate measures. Report the first, then every
                // 64th, always with the running total.
                let misses = self.compute_stamp_misses.fetch_add(1, Ordering::Relaxed) + 1;
                if misses == 1 || misses % 64 == 0 {
                    dev_warn!(
                        self.resources.dev().as_ref(),
                        "G17P completion stamp never published within {} us ({} so far); retiring on the KSM record alone\n",
                        budget_us,
                        misses
                    );
                }
                return;
            }
            fsleep(Delta::from_micros(POLL_US as i64));
            waited_us = waited_us.saturating_add(POLL_US);
        }
    }

    pub(crate) fn snapshot_compute_timeout(
        &mut self,
        queue: &mut G17PSksmQueue,
    ) -> Result<G17PComputeTimeoutSnapshot> {
        Ok(G17PComputeTimeoutSnapshot {
            graph: queue.compute.snapshot_initial_channel_group()?,
            channel: self.resources.snapshot_compute_channel()?,
            channel_scan: self.resources.snapshot_all_primary_work_channels()?,
            channel_control_prefix: queue.storage.snapshot_channel_control_prefix()?,
            control: self.resources.control_counters()?,
        })
    }

    /// Publish newly appended kicks for a hardware-registered queue.
    pub(crate) fn add_sksm_kicks(
        &self,
        registers: &regs::Resources,
        queue: &mut G17PSksmQueue,
        add_count: u32,
    ) -> Result {
        if queue.hardware_state != G17PSksmQueueHardwareState::HardwareRegistered
            || queue.scratch != self.sksm_scratch()
            || queue.uat_owner != self.uat().ttb_base()
        {
            return Err(EINVAL);
        }

        let pair = g17_submission::prepare_sksm_add_kicks_mmio(
            g17_submission::SksmAddKicksTarget::G17P,
            queue.queue_id,
            add_count,
            queue.scratch,
        )
        .map_err(|_| EINVAL)?
        .write_pair();
        registers.g17p_sksm_publish_write_pair(pair)
    }
}

