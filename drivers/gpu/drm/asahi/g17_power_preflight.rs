// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G17 cold-boot and power preflight.
//!
//! This module joins three independently grounded contracts without issuing
//! MMIO, mailbox traffic, DMA, or firmware writes:
//!
//! - the decoded hardware identity and HAL/submission axes,
//! - the target-specific G17P/G17G/G17S dual-role host boot layout, and
//! - the bounded GFX1 SLEEP/NAP/0x21 transition grammar.
//!
//! A successful static preflight identifies the model which a future live
//! implementation must use. It is not device admission. Even after the pure
//! cold-boot observer reaches its final state, [`G17PowerPreflight::execution_gate`]
//! remains closed until every named live ownership/causality gap is resolved.

#![cfg_attr(not(test), allow(dead_code))]

#[cfg(not(test))]
use crate::{agx_power_recovery, g17_asc_bringup, g17_boot, identity};
#[cfg(test)]
#[path = "agx_power_recovery.rs"]
mod agx_power_recovery;
#[cfg(test)]
#[path = "g17_asc_bringup.rs"]
mod g17_asc_bringup;
#[cfg(test)]
#[path = "g17_boot.rs"]
mod g17_boot;
#[cfg(test)]
#[path = "identity.rs"]
mod identity;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PowerTarget {
    /// A18 Pro, HAL200, dual firmware roles, classic + SKSM linked.
    G17P,
    /// M5, HAL300, dual firmware roles, SKSM-only host target.
    G17G,
    G17S,
}

pub(crate) const G17_POWER_TARGETS: [G17PowerTarget; 3] = [
    G17PowerTarget::G17P,
    G17PowerTarget::G17G,
    G17PowerTarget::G17S,
];

impl G17PowerTarget {
    const fn boot_target(self) -> g17_boot::G17HostBootEvidenceTarget {
        match self {
            Self::G17P => g17_boot::G17HostBootEvidenceTarget::G17P,
            Self::G17G => g17_boot::G17HostBootEvidenceTarget::G17G,
            Self::G17S => g17_boot::G17HostBootEvidenceTarget::G17S,
        }
    }

    const fn gfx1_target(self) -> agx_power_recovery::G17Gfx1PowerEvidenceTarget {
        match self {
            Self::G17P => agx_power_recovery::G17Gfx1PowerEvidenceTarget::G17P,
            Self::G17G => agx_power_recovery::G17Gfx1PowerEvidenceTarget::G17G,
            Self::G17S => agx_power_recovery::G17Gfx1PowerEvidenceTarget::G17S,
        }
    }

    const fn pm_target(self) -> agx_power_recovery::G17PmConfigEvidenceTarget {
        match self {
            Self::G17P => agx_power_recovery::G17PmConfigEvidenceTarget::G17P,
            Self::G17G => agx_power_recovery::G17PmConfigEvidenceTarget::G17G,
            Self::G17S => agx_power_recovery::G17PmConfigEvidenceTarget::G17S,
        }
    }

    const fn asc_target(self) -> g17_asc_bringup::G17AscEvidenceTarget {
        match self {
            Self::G17P => g17_asc_bringup::G17AscEvidenceTarget::G17P,
            Self::G17G => g17_asc_bringup::G17AscEvidenceTarget::G17G,
            Self::G17S => g17_asc_bringup::G17AscEvidenceTarget::G17S,
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PowerPreflightError {
    UnsupportedGeneration,
    UnsupportedVariant,
    UnexpectedUscGeneration,
    UnexpectedHalGeneration,
    UnexpectedFirmwareRoleTopology,
    UnexpectedSubmissionTransport,
    UnexpectedUatWidth,
    MissingT8140Topology,
    UnexpectedT8140Topology,
    BootSequence(g17_boot::BootError),
    ColdBootIncomplete,
    UnprovedPowerTransition(agx_power_recovery::PowerTransitionError),
}

/// A18 Pro performance-state handling selected only after exact D93AP
/// topology admission.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8140PerformanceStateStrategy {
    /// D93AP has `perf-state-count = 0`, sixteen zero records, and no
    /// `perf-states-sram`; do not run the legacy OPP synthesis path.
    NoLegacyOppSynthesis,
}

/// A18 Pro ASC launch handling selected only after exact D93AP admission.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8140AscLaunchStrategy {
    /// Start the independent GFX ASC, then GFX1 ASC, require both starts, and
    /// only then enter the combined-started wait.
    DualAscGfxThenGfx1ThenCombinedWait,
}

/// Operational strategy carried by an admitted A18 Pro preflight.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PPlatformStrategy {
    pub(crate) performance_states: T8140PerformanceStateStrategy,
    pub(crate) asc_launch: T8140AscLaunchStrategy,
    pub(crate) asc_providers: [agx_power_recovery::T8140G17PAscProvider; 2],
}

const T8140_G17P_PLATFORM_STRATEGY: T8140G17PPlatformStrategy = T8140G17PPlatformStrategy {
    performance_states: T8140PerformanceStateStrategy::NoLegacyOppSynthesis,
    asc_launch: T8140AscLaunchStrategy::DualAscGfxThenGfx1ThenCombinedWait,
    asc_providers: agx_power_recovery::D93AP_T8140_G17P_ASC_PROVIDERS,
};

pub(crate) mod missing_live {
    pub(crate) const FIRMWARE_LOAD_POWER_ORDER: u16 = 1 << 0;
    pub(crate) const FIRMWARE_LOAD_CACHE_MAINTENANCE: u16 = 1 << 1;
    pub(crate) const POWER_CALLBACK_ENDPOINT_CAUSALITY: u16 = 1 << 2;
    pub(crate) const INTERRUPT_ACK_ROUTE: u16 = 1 << 3;
    pub(crate) const COLD_BOOT_TO_SUBMISSION_CAUSALITY: u16 = 1 << 4;
    pub(crate) const GFX1_PIO_LINUX_OWNERSHIP: u16 = 1 << 5;
    pub(crate) const GFX1_POWER_SEQUENCE: u16 = 1 << 6;
    pub(crate) const RECOVERY_HANDSHAKE: u16 = 1 << 7;
    pub(crate) const COLD_BOOT_OBSERVATION: u16 = 1 << 8;
    pub(crate) const PMGR_ASC_GATE_ORDER: u16 = 1 << 9;
    pub(crate) const ASC_CONTROL_APERTURE_OWNERSHIP: u16 = 1 << 10;
    pub(crate) const IORVBAR_DMA_VISIBILITY: u16 = 1 << 11;
    pub(crate) const ASC_WRAPPER_MMIO_ENABLE: u16 = 1 << 12;
    pub(crate) const ASC_TEARDOWN_OWNER: u16 = 1 << 13;
}

const REQUIRED_LIVE_EVIDENCE: u16 = missing_live::FIRMWARE_LOAD_POWER_ORDER
    | missing_live::FIRMWARE_LOAD_CACHE_MAINTENANCE
    | missing_live::POWER_CALLBACK_ENDPOINT_CAUSALITY
    | missing_live::INTERRUPT_ACK_ROUTE
    | missing_live::COLD_BOOT_TO_SUBMISSION_CAUSALITY
    | missing_live::GFX1_PIO_LINUX_OWNERSHIP
    | missing_live::GFX1_POWER_SEQUENCE
    | missing_live::RECOVERY_HANDSHAKE
    | missing_live::COLD_BOOT_OBSERVATION
    | missing_live::PMGR_ASC_GATE_ORDER
    | missing_live::ASC_CONTROL_APERTURE_OWNERSHIP
    | missing_live::IORVBAR_DMA_VISIBILITY
    | missing_live::ASC_WRAPPER_MMIO_ENABLE
    | missing_live::ASC_TEARDOWN_OWNER;

/// Opaque result of the production power execution gate.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct MissingLiveEvidence(u16);

impl MissingLiveEvidence {
    pub(crate) const fn bits(self) -> u16 {
        self.0
    }

    pub(crate) const fn contains(self, evidence: u16) -> bool {
        self.0 & evidence != 0
    }
}

fn target_for_identity(
    identity: &identity::GpuIdentity,
) -> Result<G17PowerTarget, G17PowerPreflightError> {
    use identity::{GpuGen, GpuVariant};

    if identity.gpu_gen != GpuGen::G17 {
        return Err(G17PowerPreflightError::UnsupportedGeneration);
    }

    match identity.gpu_variant {
        GpuVariant::P => Ok(G17PowerTarget::G17P),
        GpuVariant::G => Ok(G17PowerTarget::G17G),
        GpuVariant::S => Ok(G17PowerTarget::G17S),
        GpuVariant::C | GpuVariant::D | GpuVariant::A => {
            Err(G17PowerPreflightError::UnsupportedVariant)
        }
    }
}

fn validate_identity(
    identity: &identity::GpuIdentity,
) -> Result<G17PowerTarget, G17PowerPreflightError> {
    use identity::{FirmwareRoleTopology, GpuHalGeneration, SubmissionTransport};

    let target = target_for_identity(identity)?;
    if identity.usc_generation != 3 {
        return Err(G17PowerPreflightError::UnexpectedUscGeneration);
    }

    let (hal, submission) = match target {
        G17PowerTarget::G17P => (
            GpuHalGeneration::Hal200,
            SubmissionTransport::ClassicAndSksm,
        ),
        G17PowerTarget::G17G | G17PowerTarget::G17S => {
            (GpuHalGeneration::Hal300, SubmissionTransport::Sksm)
        }
    };
    if identity.gpu_hal_generation != hal {
        return Err(G17PowerPreflightError::UnexpectedHalGeneration);
    }
    if identity.firmware_roles != FirmwareRoleTopology::Dual {
        return Err(G17PowerPreflightError::UnexpectedFirmwareRoleTopology);
    }
    if identity.submission_transport != submission {
        return Err(G17PowerPreflightError::UnexpectedSubmissionTransport);
    }
    if identity.uat_input_address_bits != 42 {
        return Err(G17PowerPreflightError::UnexpectedUatWidth);
    }

    Ok(target)
}

fn missing_static_live_evidence() -> u16 {
    let mut missing = 0;
    let gaps = g17_boot::G17X_FIRMWARE_START_GAPS;

    if !gaps.load_firmware_mmio_and_power_order_proven {
        missing |= missing_live::FIRMWARE_LOAD_POWER_ORDER;
    }
    if !gaps.load_firmware_cache_maintenance_proven {
        missing |= missing_live::FIRMWARE_LOAD_CACHE_MAINTENANCE;
    }
    if !gaps.power_callback_to_endpoint_causality_proven {
        missing |= missing_live::POWER_CALLBACK_ENDPOINT_CAUSALITY;
    }
    if !gaps.interrupt_and_ack_route_proven {
        missing |= missing_live::INTERRUPT_ACK_ROUTE;
    }
    if !gaps.both_cold_boot_flags_to_submission_causality_proven {
        missing |= missing_live::COLD_BOOT_TO_SUBMISSION_CAUSALITY;
    }
    if !agx_power_recovery::G17_HOST_PIO_LINUX_MMIO_OWNERSHIP_PROVEN {
        missing |= missing_live::GFX1_PIO_LINUX_OWNERSHIP;
    }
    if !agx_power_recovery::EXECUTABLE_GFX1_POWER_SEQUENCE_AVAILABLE {
        missing |= missing_live::GFX1_POWER_SEQUENCE;
    }
    if !agx_power_recovery::EXECUTABLE_G17_RECOVERY_HANDSHAKE_AVAILABLE {
        missing |= missing_live::RECOVERY_HANDSHAKE;
    }
    let asc = g17_asc_bringup::G17_ASC_LIVE_GAPS;
    if !asc.pmgr_set_power_state_gate_order_owned {
        missing |= missing_live::PMGR_ASC_GATE_ORDER;
    }
    if !asc.asc_control_aperture_owned {
        missing |= missing_live::ASC_CONTROL_APERTURE_OWNERSHIP;
    }
    if !asc.iorvbar_aperture_and_dma_visibility_owned {
        missing |= missing_live::IORVBAR_DMA_VISIBILITY;
    }
    if !asc.wrapper_mmio_enable_source_proven {
        missing |= missing_live::ASC_WRAPPER_MMIO_ENABLE;
    }
    if !asc.stop_cpu_teardown_owner_wired {
        missing |= missing_live::ASC_TEARDOWN_OWNER;
    }

    (REQUIRED_LIVE_EVIDENCE & !missing_live::COLD_BOOT_OBSERVATION) & missing
}

/// Pure, target-bound observer for a future G17 dual-role cold boot.
///
/// Methods record already-observed host facts. They do not cause a role kick,
/// send a message, write a register, or admit submission.
#[derive(Debug)]
pub(crate) struct G17PowerPreflight {
    target: G17PowerTarget,
    t8140_strategy: Option<T8140G17PPlatformStrategy>,
    boot: g17_boot::DualRoleBoot,
}

impl G17PowerPreflight {
    /// Bind a preflight to one exact decoded target and all independent axes.
    ///
    /// G17P additionally requires the topology observed by the platform
    /// decoder. Supplying the expected constant without observing the device
    /// is not admission; production callers must pass their decoded value.
    pub(crate) fn new(
        identity: &identity::GpuIdentity,
        observed_t8140_topology: Option<&agx_power_recovery::T8140G17PAdtTopology>,
    ) -> Result<Self, G17PowerPreflightError> {
        let target = validate_identity(identity)?;
        let decoded = observed_t8140_topology
            .map(agx_power_recovery::decode_t8140_g17p_asc_providers);
        let t8140_strategy = Self::admit_t8140_strategy(target, decoded)?;

        Ok(Self {
            target,
            t8140_strategy,
            boot: g17_boot::DualRoleBoot::new(),
        })
    }

    /// Bind a preflight from what a *Linux* platform decoder can observe.
    ///
    /// This is the production T8140 entry point: the Linux device tree does
    /// not carry the full ADT node, but its GPU node's two `mboxes` expose
    /// exactly the dual ASCWrap v6 provider topology. The observation is
    /// bus-translated and then compared against the pinned D93AP providers;
    /// only an exact match admits the A18 Pro platform strategy. Supplying
    /// the expected constant without observing the device remains impossible
    /// from this path — the caller passes raw `reg`/interrupt values it read
    /// from the live tree.
    pub(crate) fn new_with_linux_dt(
        identity: &identity::GpuIdentity,
        observed: Option<&agx_power_recovery::T8140G17PLinuxDtTopology>,
    ) -> Result<Self, G17PowerPreflightError> {
        let target = validate_identity(identity)?;
        let decoded =
            observed.map(agx_power_recovery::decode_t8140_g17p_asc_providers_from_linux_dt);
        let t8140_strategy = Self::admit_t8140_strategy(target, decoded)?;

        Ok(Self {
            target,
            t8140_strategy,
            boot: g17_boot::DualRoleBoot::new(),
        })
    }

    /// Admit the T8140/G17P platform strategy from decoded providers.
    ///
    /// `decoded` is `None` when no topology observation was supplied at all,
    /// `Some(None)` when an observation was supplied but did not decode, and
    /// `Some(Some(providers))` when it decoded. Only providers exactly equal
    /// to the pinned D93AP expectation admit the strategy; every other
    /// combination fails closed.
    fn admit_t8140_strategy(
        target: G17PowerTarget,
        decoded: Option<Option<[agx_power_recovery::T8140G17PAscProvider; 2]>>,
    ) -> Result<Option<T8140G17PPlatformStrategy>, G17PowerPreflightError> {
        match (target, decoded) {
            (G17PowerTarget::G17P, Some(Some(providers)))
                if providers == T8140_G17P_PLATFORM_STRATEGY.asc_providers =>
            {
                Ok(Some(T8140_G17P_PLATFORM_STRATEGY))
            }
            (G17PowerTarget::G17P, Some(_)) => {
                Err(G17PowerPreflightError::UnexpectedT8140Topology)
            }
            (G17PowerTarget::G17P, None) => Err(G17PowerPreflightError::MissingT8140Topology),
            (_, Some(_)) => Err(G17PowerPreflightError::UnexpectedT8140Topology),
            (_, None) => Ok(None),
        }
    }

    pub(crate) const fn target(&self) -> G17PowerTarget {
        self.target
    }

    /// Present only when exact D93AP topology admission selected the A18 Pro
    /// no-legacy-OPP and dual-ASC launch paths.
    pub(crate) const fn t8140_strategy(&self) -> Option<T8140G17PPlatformStrategy> {
        self.t8140_strategy
    }

    pub(crate) const fn boot_layout(&self) -> g17_boot::G17HostBootLayout {
        g17_boot::g17_host_boot_layout(self.target.boot_target())
    }

    pub(crate) const fn boot_text_evidence(&self) -> g17_boot::G17HostBootTextEvidence {
        g17_boot::g17_host_boot_text_evidence(self.target.boot_target())
    }

    pub(crate) const fn asc_text_evidence(&self) -> g17_asc_bringup::G17AscTextEvidence {
        g17_asc_bringup::g17_asc_text_evidence(self.target.asc_target())
    }

    pub(crate) const fn opaque_pm_config(&self) -> &'static [u64; 6] {
        agx_power_recovery::g17_pm_config_for_evidence_target(self.target.pm_target())
    }

    pub(crate) fn mark_role_root_ready(
        &mut self,
        role: g17_boot::FirmwareRole,
    ) -> Result<(), G17PowerPreflightError> {
        self.boot
            .mark_initdata_root_ready(role)
            .map_err(G17PowerPreflightError::BootSequence)
    }

    pub(crate) fn record_role_kick(
        &mut self,
        role: g17_boot::FirmwareRole,
        succeeded: bool,
    ) -> Result<(), G17PowerPreflightError> {
        self.boot
            .record_kick(role, succeeded)
            .map_err(G17PowerPreflightError::BootSequence)
    }

    pub(crate) fn notify_role_started(&mut self, role: g17_boot::FirmwareRole) {
        self.boot.notify_started(role);
    }

    pub(crate) fn notify_role_cold_boot_complete(&mut self, role: g17_boot::FirmwareRole) {
        self.boot.notify_cold_boot_complete(role);
    }

    /// Whether all three independent host-observed cold-boot gates closed.
    pub(crate) fn cold_boot_observed(&self) -> bool {
        self.boot.handoff_ready()
    }

    /// Validate a requested edge against the pinned GFX1 transition grammar.
    ///
    /// This becomes available only after the complete pure cold-boot
    /// observation. Success proves only the edge shape. Callers must still
    /// pass [`Self::execution_gate`] before any eventual hardware action.
    pub(crate) fn validate_power_transition_shape(
        &self,
        old: u8,
        new: u8,
    ) -> Result<(), G17PowerPreflightError> {
        if !self.boot.handoff_ready() {
            return Err(G17PowerPreflightError::ColdBootIncomplete);
        }
        match agx_power_recovery::validate_g17_gfx1_power_transition(
            self.target.gfx1_target(),
            old,
            new,
        ) {
            Ok(()) => Ok(()),
            Err(e) => Err(G17PowerPreflightError::UnprovedPowerTransition(e)),
        }
    }

    /// Final production power gate. It remains separate from cold-boot state.
    pub(crate) fn execution_gate(&self) -> Result<(), MissingLiveEvidence> {
        let mut missing = missing_static_live_evidence();
        if !self.boot.handoff_ready() {
            missing |= missing_live::COLD_BOOT_OBSERVATION;
        }
        if missing == 0 {
            Ok(())
        } else {
            Err(MissingLiveEvidence(missing))
        }
    }
}

