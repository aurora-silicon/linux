// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


#[cfg(not(test))]
use crate::{g15_initdata, g17_rtkit};
#[cfg(test)]
#[path = "g15_initdata.rs"]
mod g15_initdata;
#[cfg(test)]
#[path = "g17_rtkit.rs"]
mod g17_rtkit;

/// Design generation that uses this single-role contract.
const G15_GENERATION: u32 = 15;

pub(crate) mod missing {
    /// The G15 `GFX` RTKit firmware image and its nested identity.
    pub(crate) const FIRMWARE_IMAGE: u16 = 1 << 0;
    /// Core-mask / MMU-fault register bank (shared G14X family).
    pub(crate) const TOPOLOGY_REGISTERS: u16 = 1 << 1;
    /// UAT/Handoff firmware-shared state (dual-TTBR, 42-bit).
    pub(crate) const UAT_HANDOFF: u16 = 1 << 2;
    /// Bootable init-data object graph.
    pub(crate) const INITDATA_GRAPH: u16 = 1 << 3;
    /// Executable RTKit transport (HELLO/EPMAP + interface version).
    pub(crate) const RTKIT_TRANSPORT: u16 = 1 << 4;
    /// EP0 RTKit interface version for this firmware/OS build.
    pub(crate) const INTERFACE_VERSION: u16 = 1 << 5;
    /// Classic register-list submission transport.
    pub(crate) const SUBMISSION_TRANSPORT: u16 = 1 << 6;
}

const REQUIRED_PRE_HANDOFF_EVIDENCE: u16 = missing::FIRMWARE_IMAGE
    | missing::TOPOLOGY_REGISTERS
    | missing::UAT_HANDOFF
    | missing::INITDATA_GRAPH
    | missing::RTKIT_TRANSPORT
    | missing::INTERFACE_VERSION
    | missing::SUBMISSION_TRANSPORT;

/// Exact fail-closed result returned to the probe boundary.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct MissingEvidence(u16);

impl MissingEvidence {
    pub(crate) const fn bits(self) -> u16 {
        self.0
    }

    pub(crate) const fn contains(self, evidence: u16) -> bool {
        self.0 & evidence != 0
    }
}

/// The shared G14X topology/fault register bank is wired (`regs.rs` decodes
/// hardware families 0x7/0x8/0xa/0xb, i.e. G15's family byte `0x7` included).
pub(crate) const G15_TOPOLOGY_REGISTERS_IMPLEMENTED: bool = true;

/// Whether the G15 firmware's advertised RTKit interface version has been
/// confirmed to fall inside the apple-rtkit core's supported window. Unlike
/// G17 (firmware on disk, version 0xc verified), the G15 `GFX` image is absent
/// from the corpus, so this cannot be grounded offline and stays false.
pub(crate) const G15_FIRMWARE_INTERFACE_VERSION_CONFIRMED: bool = false;

const fn implemented_pre_handoff_evidence() -> u16 {
    let mut implemented = 0;

    if G15_TOPOLOGY_REGISTERS_IMPLEMENTED {
        implemented |= missing::TOPOLOGY_REGISTERS;
    }
    // These flip only when the underlying module becomes boot-complete, not by
    // a disconnected probe flag.
    if g15_initdata::GROUNDED_LAYOUT_AVAILABLE && g15_initdata::BOOTABLE_LAYOUT_AVAILABLE {
        implemented |= missing::INITDATA_GRAPH;
    }
    // The shared GPU wire codec is executable (byte-identical to G17/AGX2), but
    // G15's transport is only ready once its firmware's advertised RTKit
    // interface version is confirmed inside the apple-rtkit core window. The
    // G15 `GFX` image is not on disk, so that stays unconfirmed and this bit
    // fails closed independently of the shared codec constant.
    if g17_rtkit::GROUNDED_CODEC_AVAILABLE
        && g17_rtkit::EXECUTABLE_TRANSPORT_AVAILABLE
        && G15_FIRMWARE_INTERFACE_VERSION_CONFIRMED
    {
        implemented |= missing::RTKIT_TRANSPORT;
    }

    implemented
}

pub(crate) const fn pre_handoff_gate(gpu_generation: u32) -> Result<(), MissingEvidence> {
    if gpu_generation != G15_GENERATION {
        return Ok(());
    }

    let missing_bits = REQUIRED_PRE_HANDOFF_EVIDENCE & !implemented_pre_handoff_evidence();
    if missing_bits == 0 {
        Ok(())
    } else {
        Err(MissingEvidence(missing_bits))
    }
}

// ---------------------------------------------------------------------------
// Grounded host-observed boot facts (grade A; addresses in the pinned KC).
// ---------------------------------------------------------------------------

pub(crate) const G15_ROLE_RECORD_BASE: u32 = 0x14f0;
/// Per-role record stride (single role, so only role 0 is used).
pub(crate) const G15_ROLE_RECORD_STRIDE: u32 = 0x38;
/// `started` byte, relative to the role record (`record+0x28`).
pub(crate) const G15_ROLE_STARTED_OFFSET: u32 = 0x28;
pub(crate) const G15_BOOT_POLL_FLAG_OFFSET: u32 = 0x1581;
pub(crate) const G15_INITDATA_DOORBELL_ENDPOINT: u8 = 0x20;

/// Compose the G15 init-data doorbell exactly as G17 does (the wire form is
/// byte-identical: `(0x81 << 48) | (fw_va & 0xFFFFFFFFFFF)`). Delegates to the
/// shared codec so the two never drift.
pub(crate) const fn encode_initdata_doorbell(firmware_va: u64) -> Result<u64, g17_rtkit::WireError> {
    g17_rtkit::encode_initdata_doorbell(firmware_va)
}


pub(crate) const G15_UAT_TTBR_COUNT: u32 = 2;
pub(crate) const G15_UAT_TTBR_SELECT_BIT: u32 = 42;
pub(crate) const G15_PTE_BASE: u32 = 0xc02;
pub(crate) const G15_PTE_TTBR_SHIFT: u32 = 11;
pub(crate) const G15_PTE_VALID_BITS: u64 = (1 << 0) | (1 << 55);
pub(crate) const G15_UAT_VALID_OPTIONS_MASK: u32 = 0x30f;

/// Fail-closed errors in the host-observed single-role boot sequence.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum BootError {
    /// The init-data root must be built and validated before the kick.
    InitdataRootIncomplete,
    /// The role start call failed; the boot attempt is permanently failed.
    KickFailed,
    /// The root was changed after the kick.
    RootMutationAfterKick,
    /// An earlier sequencing error already failed this boot attempt.
    AlreadyFailed,
}

/// Pure state model for one G15 single-role cold boot.
///
/// There is exactly one `GFX` role; the combined/started notifications only
/// record flags because they may race with the host's start call.
#[cfg_attr(not(test), allow(dead_code))]
#[derive(Debug, Default)]
pub(crate) struct SingleRoleBoot {
    initdata_root_ready: bool,
    kicked: bool,
    kick_succeeded: bool,
    started_notified: bool,
    cold_boot_complete: bool,
    failed: bool,
}

#[cfg_attr(not(test), allow(dead_code))]
impl SingleRoleBoot {
    pub(crate) const fn new() -> Self {
        Self {
            initdata_root_ready: false,
            kicked: false,
            kick_succeeded: false,
            started_notified: false,
            cold_boot_complete: false,
            failed: false,
        }
    }

    /// Mark the single init-data root ready for the shared UAT.
    pub(crate) fn mark_initdata_root_ready(&mut self) -> Result<(), BootError> {
        if self.failed {
            return Err(BootError::AlreadyFailed);
        }
        if self.kicked {
            self.failed = true;
            return Err(BootError::RootMutationAfterKick);
        }
        self.initdata_root_ready = true;
        Ok(())
    }

    /// Record the return value of the single role start call.
    pub(crate) fn record_kick(&mut self, succeeded: bool) -> Result<(), BootError> {
        if self.failed {
            return Err(BootError::AlreadyFailed);
        }
        if !self.initdata_root_ready {
            self.failed = true;
            return Err(BootError::InitdataRootIncomplete);
        }
        self.kicked = true;
        if !succeeded {
            self.failed = true;
            return Err(BootError::KickFailed);
        }
        self.kick_succeeded = true;
        Ok(())
    }

    /// Record the role's started callback.
    pub(crate) fn notify_started(&mut self) {
        self.started_notified = true;
    }

    /// Record the role's cold-boot-complete callback.
    pub(crate) fn notify_cold_boot_complete(&mut self) {
        self.cold_boot_complete = true;
    }

    /// The single ordered role start call returned success.
    pub(crate) fn kick_gate_open(&self) -> bool {
        !self.failed && self.kick_succeeded
    }

    /// Full readiness requires the kick to succeed and both callbacks.
    pub(crate) fn handoff_ready(&self) -> bool {
        self.kick_gate_open() && self.started_notified && self.cold_boot_complete
    }
}

pub(crate) fn refuse_reasons() -> MissingEvidence {
    match pre_handoff_gate(G15_GENERATION) {
        Ok(()) => MissingEvidence(0),
        Err(missing) => missing,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn topology_registers_are_the_only_implemented_pre_handoff_axis() {
        assert!(pre_handoff_gate(13).is_ok());
        assert!(pre_handoff_gate(14).is_ok());
        assert!(pre_handoff_gate(16).is_ok());
        assert!(pre_handoff_gate(17).is_ok());

        let missing = pre_handoff_gate(G15_GENERATION).unwrap_err();
        assert!(G15_TOPOLOGY_REGISTERS_IMPLEMENTED);
        assert!(!missing.contains(missing::TOPOLOGY_REGISTERS));
        assert_eq!(
            missing.bits(),
            REQUIRED_PRE_HANDOFF_EVIDENCE & !missing::TOPOLOGY_REGISTERS
        );
        for evidence in [
            missing::FIRMWARE_IMAGE,
            missing::UAT_HANDOFF,
            missing::INITDATA_GRAPH,
            missing::RTKIT_TRANSPORT,
            missing::INTERFACE_VERSION,
            missing::SUBMISSION_TRANSPORT,
        ] {
            assert!(missing.contains(evidence));
        }
        // The underlying modules are grounded but not bootable for G15. The
        // shared GPU wire codec is now executable (grounded on the on-disk M5
        // firmware), but G15's transport still fails closed because the G15
        // firmware's RTKit interface version cannot be confirmed offline.
        assert!(g15_initdata::GROUNDED_LAYOUT_AVAILABLE);
        assert!(!g15_initdata::BOOTABLE_LAYOUT_AVAILABLE);
        assert!(g17_rtkit::GROUNDED_CODEC_AVAILABLE);
        assert!(g17_rtkit::EXECUTABLE_TRANSPORT_AVAILABLE);
        assert!(!G15_FIRMWARE_INTERFACE_VERSION_CONFIRMED);
        assert!(missing.contains(missing::RTKIT_TRANSPORT));
    }

    #[test]
    fn refuse_reasons_matches_the_gate() {
        assert_eq!(refuse_reasons(), pre_handoff_gate(15).unwrap_err());
    }

    #[test]
    fn host_boot_facts_are_the_grounded_g15_values() {
        assert_eq!(G15_ROLE_RECORD_BASE, 0x14f0);
        assert_eq!(G15_ROLE_RECORD_STRIDE, 0x38);
        assert_eq!(G15_ROLE_STARTED_OFFSET, 0x28);
        assert_eq!(G15_BOOT_POLL_FLAG_OFFSET, 0x1581);
        assert_eq!(G15_INITDATA_DOORBELL_ENDPOINT, 0x20);
    }

    #[test]
    fn initdata_doorbell_matches_the_shared_codec() {
        let va = 0x0123_4567_89ab;
        assert_eq!(
            encode_initdata_doorbell(va),
            g17_rtkit::encode_initdata_doorbell(va)
        );
        assert_eq!(
            encode_initdata_doorbell(va).unwrap(),
            0x0081_0000_0000_0000u64 | va
        );
    }

    #[test]
    fn uat_pte_encoder_shape_is_grounded_but_dprr_is_unknown() {
        assert_eq!(G15_UAT_TTBR_COUNT, 2);
        assert_eq!(G15_UAT_TTBR_SELECT_BIT, 42);
        assert_eq!(G15_PTE_BASE, 0xc02);
        assert_eq!(G15_PTE_TTBR_SHIFT, 11);
        assert_eq!(G15_PTE_VALID_BITS, (1 << 0) | (1 << 55));
        assert_eq!(G15_UAT_VALID_OPTIONS_MASK, 0x30f);
        // G15's base differs from G17's single-stage encoder (0x403/0xc03).
        assert_ne!(G15_PTE_BASE, 0x403);
    }

    #[test]
    fn single_role_boot_requires_root_then_successful_kick_then_callbacks() {
        let mut boot = SingleRoleBoot::new();
        assert_eq!(boot.record_kick(true), Err(BootError::InitdataRootIncomplete));

        let mut boot = SingleRoleBoot::new();
        boot.mark_initdata_root_ready().unwrap();
        boot.record_kick(true).unwrap();
        assert!(boot.kick_gate_open());
        assert!(!boot.handoff_ready());
        boot.notify_started();
        assert!(!boot.handoff_ready());
        boot.notify_cold_boot_complete();
        assert!(boot.handoff_ready());
    }

    #[test]
    fn boot_fails_closed_on_kick_failure_and_late_mutation() {
        let mut boot = SingleRoleBoot::new();
        boot.mark_initdata_root_ready().unwrap();
        boot.record_kick(false).unwrap_err();
        boot.notify_started();
        boot.notify_cold_boot_complete();
        assert!(!boot.kick_gate_open());
        assert!(!boot.handoff_ready());

        let mut boot = SingleRoleBoot::new();
        boot.mark_initdata_root_ready().unwrap();
        boot.record_kick(true).unwrap();
        assert_eq!(
            boot.mark_initdata_root_ready(),
            Err(BootError::RootMutationAfterKick)
        );
    }
}
