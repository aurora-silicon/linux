// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8152 SGX register window and PSD power-state reads used before firmware start.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
use kernel::{
    c_str,
    device::Core,
    io::{mem::DevresIoMem, Io, IoBase},
    platform,
    prelude::*,
    sync::aref::ARef,
};
use crate::g18::{hw, identity};

const ID_VERSION: usize = 0xd04000;

const ID_UNK08: usize = 0xd04008;

const ID_COUNTS_1: usize = 0xd04010;

const ID_COUNTS_2: usize = 0xd04014;

const ID_UNK18: usize = 0xd04018;

const ID_CLUSTERS: usize = 0xd0401c;

// The "G14X" register family is shared by G14X (t602x) and every AGX3 part
// (G15/G16/G17). The ID block at 0xd04000 is unchanged, and the per-cluster
// core-enable masks are three u32 words at 0xe01500 (stride 4): AGXG17X
// `readChipInfo` builds the offset (`mov w23,#0x1500; movk w23,#0xe0,lsl#16`
// @ 0xfffffe0008b4b220) and reads [base], [base+4], [base+8]; the identical
// pattern appears in all eight AGX3 kexts of the pinned 26A5378n corpus
// (e.g. AGXG15G @ 0xfffffe0008af22f0).
const CORE_MASKS_G14X: usize = 0xe01500;

/// Device resources for this GPU instance.
pub(crate) struct Resources {
    dev: ARef<platform::Device>,
    sgx: DevresIoMem,
    // The G17P SKSM FIFO lock and deferred SKSM writes are not carried: on
    // T8152 the firmware owns KSMFE queue setup (hwdata+0x2724 = 0) and the
    // host performs no SKSM MMIO.
}

impl Resources {

    /// Map the required resources given our platform device.
    pub(crate) fn new(pdev: &platform::Device<Core<'_>>) -> Result<Resources> {
        // The pinned host maps accelerator device-memory index 0 and stores
        // it at accelerator+0x348 before every SKSM scratch access. That range
        // is the J700 GPU block exposed to Linux by the named `sgx` resource
        // (the second platform `reg` tuple, after `asc`).
        let sgx_req = pdev.io_request_by_name(c_str!("sgx")).ok_or(EINVAL)?;
        let sgx_iomem = sgx_req.iomap()?.into_devres()?;

        Ok(Resources {
            // SAFETY: This device does DMA via the UAT IOMMU.
            dev: pdev.into(),
            sgx: sgx_iomem,
        })
    }

    /// Match the measured M5 native AP setup before starting firmware CPUs.
    ///
    /// The broad 25G76 AP trace writes 0x30001 at physical 0x580e90000;
    /// Linux's prior stopped probe reads 0x30000. This diagnostic accepts
    /// only those two observed states and changes only bit zero. The bit's
    /// precise hardware role remains unqualified; do not generalize it.
    pub(crate) fn m5_match_pmgr_boot_control(&self) -> Result {
        const OFFSET: usize = 0xe90000;
        let sgx = self.sgx.try_access().ok_or(ENODEV)?;
        let res = (&*sgx).as_view().relaxed();
        let before = res.try_read32(OFFSET)?;
        if before != 0x30000 && before != 0x30001 {
            dev_err!(self.dev.as_ref(), "M5_INIT unexpected PMGR control {:#x}\n", before);
            return Err(EIO);
        }
        res.try_write32(before | 1, OFFSET)?;
        let after = res.try_read32(OFFSET)?;
        dev_info!(self.dev.as_ref(), "M5_INIT native PMGR control: {:#x} -> {:#x}\n",
            before, after);
        if after != 0x30001 {
            return Err(EIO);
        }
        Ok(())
    }

    /// Read and decode the T8152 (G18G) GPU identity.
    ///
    /// Requires the [`T8152SgxPowered`] proof: the SGX PMGR2 PSD service was
    /// read and reported active immediately before, so these
    /// reads cannot hit a gated block. Only the registers 26A428
    /// `AGX·PI_400·G·A0·Accelerator::readChipInfo` reads are touched: the ID
    /// block at 0xd04000..0xd0401c and the single core-enable word at
    /// 0xe01500. No register is written.
    ///
    /// The raw words are always logged. The decode never rejects: an unknown
    /// or different family/variant byte is reported and the part is
    /// identified by its chip id and DT compatible (`soc`), and implausible
    /// counts fall back to the native registry topology (2 x 6 cores).
    pub(crate) fn get_gpu_id_t8152(
        &self,
        powered: &T8152SgxPowered,
        soc: &'static hw::agx3::SocConfig,
    ) -> Result<hw::GpuIdConfig> {
        let registry = hw::agx3::T8152_REGISTRY;
        let expected =
            identity::decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8)
                .ok_or(EINVAL)?;
        if soc.chip_id != 0x8152 || expected.gpu_gen != hw::GpuGen::G18 {
            return Err(EINVAL);
        }

        let (id_version, id_unk08, id_counts_1, id_counts_2, id_unk18, id_clusters, core_mask) = {
            let sgx = self.sgx.try_access().ok_or(ENODEV)?;
            // Ordered (non-relaxed) reads, after the ordered PSD read.
            (
                sgx.try_read32(ID_VERSION)?,
                sgx.try_read32(ID_UNK08)?,
                sgx.try_read32(ID_COUNTS_1)?,
                sgx.try_read32(ID_COUNTS_2)?,
                sgx.try_read32(ID_UNK18)?,
                sgx.try_read32(ID_CLUSTERS)?,
                sgx.try_read32(CORE_MASKS_G14X)?,
            )
        };

        dev_info!(
            self.dev.as_ref(),
            "T8152: PSD sgx state={:#x} ack={:#x}; raw GPU ID {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}; core mask {:#x}\n",
            powered.sample().state,
            powered.sample().ack,
            id_version,
            id_unk08,
            id_counts_1,
            id_counts_2,
            id_unk18,
            id_clusters,
            core_mask
        );

        let family = ((id_version >> 24) & 0xff) as u8;
        let variant = ((id_version >> 16) & 0xff) as u8;
        let reg_dies = (id_counts_1 >> 16) & 0xf;
        match identity::decode_gpu_identity(family, variant, reg_dies as u8) {
            Some(decoded) if decoded == expected => dev_info!(
                self.dev.as_ref(),
                "T8152: ID family {:#x} variant {:#x} decodes to {:?}{:?} as expected\n",
                family,
                variant,
                decoded.gpu_gen,
                decoded.gpu_variant
            ),
            Some(decoded) => dev_warn!(
                self.dev.as_ref(),
                "T8152: ID family {:#x} variant {:#x} decodes to {:?}{:?}; using the chip-id/compatible identity {:?}{:?}\n",
                family,
                variant,
                decoded.gpu_gen,
                decoded.gpu_variant,
                expected.gpu_gen,
                expected.gpu_variant
            ),
            None => dev_warn!(
                self.dev.as_ref(),
                "T8152: unknown ID family {:#x} variant {:#x} dies {}; using the chip-id/compatible identity {:?}{:?}\n",
                family,
                variant,
                reg_dies,
                expected.gpu_gen,
                expected.gpu_variant
            ),
        }

        // Same count layout as the PI_400 decode (@ 0xfffffe0008c3d53c..0x554):
        // dies [19:16], clusters per die [15:8], cores per cluster [7:0].
        let reg_cores = id_counts_1 & 0xff;
        let reg_clusters = ((id_counts_1 >> 8) & 0xff) * reg_dies;
        let registry_cores = registry.num_cores / registry.num_mgpus;
        let (num_dies, num_clusters, num_cores) = if (1..=32).contains(&reg_cores)
            && reg_clusters != 0
            && reg_cores * reg_clusters <= 32
        {
            (reg_dies, reg_clusters, reg_cores)
        } else {
            dev_warn!(
                self.dev.as_ref(),
                "T8152: implausible ID counts ({} dies, {} clusters, {} cores); using registry topology {} x {}\n",
                reg_dies,
                reg_clusters,
                reg_cores,
                registry.num_mgpus,
                registry_cores
            );
            (soc.num_dies, registry.num_mgpus, registry_cores)
        };

        let mut core_masks = KVec::new();
        let mut remaining = core_mask as u64;
        let cluster_mask = (1u64 << num_cores) - 1;
        let mut total_active_cores = 0;
        for _ in 0..num_clusters {
            let mask = (remaining & cluster_mask) as u32;
            core_masks.push(mask, GFP_KERNEL)?;
            total_active_cores += mask.count_ones();
            remaining >>= num_cores;
        }
        if remaining != 0 {
            dev_warn!(
                self.dev.as_ref(),
                "T8152: core mask {:#x} has bits beyond {} x {} cores\n",
                core_mask,
                num_clusters,
                num_cores
            );
        }
        if total_active_cores != registry.num_cores
            || core_masks.as_slice() != registry.core_mask_list.as_slice()
        {
            dev_warn!(
                self.dev.as_ref(),
                "T8152: core masks {:#x?} ({} active) differ from the native registry {:#x?} ({})\n",
                core_masks,
                total_active_cores,
                registry.core_mask_list,
                registry.num_cores
            );
        }

        let mut core_masks_packed = KVec::new();
        core_masks_packed.push(core_mask, GFP_KERNEL)?;

        let (gpu_rev, gpu_rev_id) = match (id_version >> 8) & 0xff {
            0x00 => (hw::GpuRevision::A0, hw::GpuRevisionID::A0),
            0x01 => (hw::GpuRevision::A1, hw::GpuRevisionID::A1),
            0x10 => (hw::GpuRevision::B0, hw::GpuRevisionID::B0),
            0x11 => (hw::GpuRevision::B1, hw::GpuRevisionID::B1),
            0x20 => (hw::GpuRevision::C0, hw::GpuRevisionID::C0),
            0x21 => (hw::GpuRevision::C1, hw::GpuRevisionID::C1),
            a => {
                dev_warn!(
                    self.dev.as_ref(),
                    "T8152: unknown GPU revision byte {:#x}; recorded as an A0 placeholder\n",
                    a
                );
                (hw::GpuRevision::A0, hw::GpuRevisionID::A0)
            }
        };

        dev_info!(
            self.dev.as_ref(),
            "T8152: {:?}{:?} rev {:?}, {} dies, {} clusters x {} cores, {} active, masks {:#x?}\n",
            expected.gpu_gen,
            expected.gpu_variant,
            gpu_rev,
            num_dies,
            num_clusters,
            num_cores,
            total_active_cores,
            core_masks
        );

        Ok(hw::GpuIdConfig {
            gpu_gen: expected.gpu_gen,
            gpu_variant: expected.gpu_variant,
            usc_generation: expected.usc_generation,
            gpu_hal_generation: expected.gpu_hal_generation,
            gpu_rev,
            gpu_rev_id,
            num_dies,
            num_clusters,
            num_cores,
            num_frags: num_cores,
            num_gps: (id_counts_2 >> 16) & 0xff,
            total_active_cores,
            core_masks,
            core_masks_packed,
            gpc_perf_state_map: 0,
            gpc_perf_state_map_low: 0,
            gpc_perf_state_control: 0,
        })
    }

}

const T8152_PSD_STATE: usize = 0x80;

const T8152_PSD_ACK: usize = 0xbc;

const T8152_PSD_STATE_MASK: u32 = 0x7;

/// One of the three PSD services that power the T8152 GPU.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8152PsdService {
    /// `sgx`, PSD service 3.
    Sgx,
    /// `gfx-asc` (GFX role ASC), PSD service 81.
    GfxAsc,
    /// `gfx1-asc` (GFX1 role ASC), PSD service 82.
    Gfx1Asc,
}

impl T8152PsdService {
    /// Every GPU PSD service, in ADT order.
    pub(crate) const ALL: [Self; 3] = [Self::Sgx, Self::GfxAsc, Self::Gfx1Asc];

    /// Native 26A428 request/state, reproduced on J873 with PMP running.
    pub(crate) const fn active_state(self) -> u32 {
        match self {
            Self::Sgx => 0,
            Self::GfxAsc | Self::Gfx1Asc => 2,
        }
    }

    const fn resource_name(self) -> &'static CStr {
        match self {
            Self::Sgx => c_str!("psd-sgx"),
            Self::GfxAsc => c_str!("psd-gfx-asc"),
            Self::Gfx1Asc => c_str!("psd-gfx1-asc"),
        }
    }

    /// PMGR2 implicit service id.
    pub(crate) const fn service_id(self) -> u32 {
        match self {
            Self::Sgx => 3,
            Self::GfxAsc => 81,
            Self::Gfx1Asc => 82,
        }
    }

    /// CPU-physical base of the service block.
    pub(crate) const fn physical_base(self) -> u64 {
        0x3_0300_0000 + 0xa1_0000 + ((self.service_id() as u64) << 8)
    }
}

/// One read-only sample of a PSD service's state and ack words.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8152PsdSample {
    pub(crate) service: T8152PsdService,
    pub(crate) state: u32,
    pub(crate) ack: u32,
}

impl T8152PsdSample {
    /// The PSD power state field, state[2:0].
    pub(crate) const fn power_state(&self) -> u32 {
        self.state & T8152_PSD_STATE_MASK
    }

    /// Point-in-time active-state proof, not a transition-completion test.
    /// J873 hardware confirms STATE reads clear ACK. A request must poll
    /// ACK first and verify STATE once; later access gates use STATE alone.
    pub(crate) const fn powered(&self) -> bool {
        self.power_state() == self.service.active_state()
    }

    /// Short steady-state verdict for logs.
    pub(crate) const fn verdict(&self) -> &'static str {
        if self.powered() {
            "powered"
        } else {
            "not powered"
        }
    }
}

/// `-EHOSTDOWN`: the block's PSD service is not proven powered, so every
/// MMIO read of it was skipped. The mailbox provider returns the same errno.
pub(crate) fn t8152_not_powered() -> Error {
    Error::from_errno(-(kernel::bindings::EHOSTDOWN as i32))
}

/// Proof that the SGX PSD service read as powered.
///
/// Built only by [`t8152_prove_sgx_powered`] from a fresh powered sample.
/// [`Resources::get_gpu_id_t8152`] requires it, so the SGX ID block cannot
/// be read without a preceding PSD read. The proof is a point-in-time fact.
/// It does not keep the block powered, so use it right away.
pub(crate) struct T8152SgxPowered {
    sample: T8152PsdSample,
}

impl T8152SgxPowered {
    pub(crate) const fn sample(&self) -> T8152PsdSample {
        self.sample
    }
}

/// Read one GPU PSD service's state and ack words. Reads only, no writes.
///
/// The block is mapped for this call alone and never claimed, so a later
/// power owner can map the same service block.
pub(crate) fn t8152_read_psd(
    pdev: &platform::Device<Core<'_>>,
    service: T8152PsdService,
) -> Result<T8152PsdSample> {
    let resource = pdev
        .resource_by_name(service.resource_name())
        .ok_or(ENODEV)?;
    if resource.start() != service.physical_base() || resource.size() != 0x100 {
        dev_err!(
            pdev.as_ref(),
            "T8152: PSD service {} resource {:#x}+{:#x} does not match {:#x}+0x100\n",
            service.service_id(),
            resource.start(),
            resource.size(),
            service.physical_base()
        );
        return Err(EINVAL);
    }
    let request = pdev
        .io_request_by_name(service.resource_name())
        .ok_or(ENODEV)?;
    let iomem = request.iomap()?.into_devres()?;
    let io = iomem.try_access().ok_or(ENODEV)?;
    // STATE consumes the transition ACK; preserve it in the snapshot first.
    let ack = io.try_read32(T8152_PSD_ACK)?;
    let state = io.try_read32(T8152_PSD_STATE)?;
    Ok(T8152PsdSample {
        service,
        state,
        ack,
    })
}

/// Read the SGX PSD service and return a power proof only if it is active
/// (state 0). Otherwise log the sample and return [`t8152_not_powered`], so
/// the caller skips every SGX read.
pub(crate) fn t8152_prove_sgx_powered(
    pdev: &platform::Device<Core<'_>>,
) -> Result<T8152SgxPowered> {
    let sample = t8152_read_psd(pdev, T8152PsdService::Sgx)?;
    if !sample.powered() {
        dev_info!(
            pdev.as_ref(),
            "T8152: sgx PSD service 3 state={:#x} (power state {}) ack={:#x} is not active ({}); SGX reads skipped\n",
            sample.state,
            sample.power_state(),
            sample.ack,
            sample.verdict()
        );
        return Err(t8152_not_powered());
    }
    Ok(T8152SgxPowered { sample })
}
