// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The M5 render-pair qualification (linux-aurora t6050-gpu's g17/m5_render_test.rs, its
//! installation mode): one render pair ([`super::queue::m5_render::Pair`]) is built on an
//! ordinary client VM, with its parameter buffer, target, tiling state and private pool, and
//! installed: both queues must stand idle past their configuration. Draws come from clients
//! through DRM render submission, on the same pair type. The firmware session retains the pair
//! once its installation started.

use super::{
    buffer,
    context::Context,
    fw::{
        self,
        queue::*,
        render::{RenderPass, UscProgram, ZlsSurface},
    },
    m5_vm,
    object::Allocator,
    queue::{
        m5::Host,
        m5_render::{BindingArgs, Pair, PairArgs},
    },
    Firmware, Shared,
};
use crate::{driver::AsahiDrmDevice, mmu};
use kernel::{prelude::*, sync::Arc};

/// Label of the diagnostic VM; VM IDs only name VMs in diagnostics.
const VM_LABEL: u64 = 2;

/// The fixed geometry the pair's target is first sized for (S `qualification_pass`); program
/// addresses come with each draw.
fn qualification_pass() -> RenderPass {
    let surface = ZlsSurface {
        va: 0,
        compression_va: 0,
        stride: 0,
        compression_stride: 0,
    };
    let program = UscProgram {
        offset: 0,
        rsrc_spec: 0,
    };
    RenderPass {
        width: 256,
        height: 256,
        layers: 1,
        utile_width: 32,
        utile_height: 32,
        samples_log2: 0,
        sample_size: 16,
        process_empty_tiles: true,
        msaa_control: 0x88,
        ppp_control: 0x202,
        vdm_va: 0,
        scissor_va: 0,
        depth_bias_va: 0,
        depth_bias_is_int: false,
        few_primitives: true,
        fragment_barriers: false,
        occlusion_query_va: 0,
        sampler_heap_va: 0,
        sampler_count: 0,
        zls_control: 0,
        depth_dimensions: 0,
        depth: surface,
        stencil: surface,
        depth_clear: 0x3f80_0000,
        stencil_clear: 0,
        merge_upper_x: 0x3bdd_b3d9,
        merge_upper_y: 0x3bdd_b3d9,
        usc_base: 0,
        bg: program,
        partial_bg: program,
        eot_offset: 0,
        eot_rsrc_spec: 0,
        partial_eot_offset: 0,
        partial_eot_rsrc_spec: 0,
        rsrc_spec_hi: true,
    }
}

/// The retained pair of the qualification.
pub(super) struct Render {
    pair: KBox<Pair>,
}

impl Render {
    /// Builds and installs the pair. The result is retained whatever installation returns.
    pub(super) fn run(
        dev: &AsahiDrmDevice,
        firmware: &mut Firmware,
        shared: &Arc<Shared>,
    ) -> (Option<KBox<Self>>, Result) {
        let mut built = match Self::new(dev, firmware, shared) {
            Ok(built) => built,
            Err(error) => return (None, Err(error)),
        };
        let result = built.pair.install(&mut Host {
            init: &firmware.init,
            port: &firmware.port,
            qids: &mut firmware.queues.qids,
            crashed: &firmware.primary.state.shared.crashed,
        });
        let pair = &built.pair;
        match &result {
            Ok(()) => dev_info!(
                dev.as_ref(),
                "M5 render pair installed: context {}, queues {:?}, pool {} blocks of {} MiB, parameter buffer {} blocks\n",
                pair.context().id(),
                pair.qids(),
                pair.pool_blocks(),
                shared.m5.map_or(0, |gpu| gpu.block_bytes() >> 20),
                pair.parameter_blocks()
            ),
            Err(error) => dev_err!(
                dev.as_ref(),
                "M5 render pair installation failed: {:?}, cursors {:?}\n",
                error,
                pair.cursors()
            ),
        }
        if result.is_ok() {
            dev_info!(dev.as_ref(), "M5 render qualification PASS: pair installed, both queues idle past their configuration\n");
        } else {
            firmware.init.dump_m5_failure(dev);
        }
        (Some(built), result)
    }

    fn new(
        dev: &AsahiDrmDevice,
        firmware: &mut Firmware,
        shared: &Arc<Shared>,
    ) -> Result<KBox<Self>> {
        let gpu = shared.m5.ok_or(EINVAL)?;
        let uat = firmware.uat.clone();
        let alloc = Allocator {
            dev,
            uat: &uat,
            layout: firmware.variant.vm_layout,
        };
        let vm = m5_vm::client_vm(&uat, VM_LABEL, m5_vm::DRIVER_RESERVATION)?;
        let execution = uat.new_execution_context(&vm)?;
        let context = Arc::new(
            Context::new(
                &alloc,
                &firmware.pool,
                &firmware.queues.qos_ids,
                execution,
                0,
                fw::m5::queue::MEDIUM,
                shared.renders.clone(),
                shared.progress.clone(),
            )?,
            GFP_KERNEL,
        )?;
        let metrics = firmware.m5_metrics.clone().ok_or(EINVAL)?;
        let client = metrics.alias_in(
            context.vm(),
            alloc.domain(super::vm_layout::Domain::Compact),
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_SHARED_RW,
        )?;
        let lease = buffer::MetricsLease::new(
            &firmware.queues.metrics,
            &metrics,
            fw::buffer::MetricsAddresses {
                firmware: metrics.gpu_va(),
                client: client.iova(),
            },
        )?;
        let owner = firmware.queues.reserve_identity()?;
        let fragment_owner = firmware.queues.reserve_identity()?;
        let qids = &mut firmware.queues.qids;
        let tiling = qids.reserve(owner, DataMaster::Tiling, None)?;
        let fragment = match qids.reserve(fragment_owner, DataMaster::Fragment, None) {
            Ok(id) => id,
            Err(error) => {
                qids.cancel_unpublished(tiling)?;
                return Err(error);
            }
        };
        let pb = firmware.queues.buffers.allocate_render()?;
        let pm_generation = firmware.queues.render.reserve_pm_generation()?;
        let pass = qualification_pass();
        let built = Pair::new(
            &alloc,
            context,
            PairArgs {
                ids: [tiling, fragment],
                gpu: &gpu,
                pointers: &firmware.variant.pointers,
                binding: BindingArgs {
                    owner,
                    metrics: lease,
                    metrics_view: client,
                    pb,
                    pm_generation,
                    pass: &pass,
                    clusters: shared.clusters,
                    programs: firmware.variant.programs,
                },
            },
        );
        let pair = match built {
            Ok(pair) => pair,
            Err(error) => {
                let qids = &mut firmware.queues.qids;
                qids.cancel_unpublished(fragment)?;
                qids.cancel_unpublished(tiling)?;
                dev_err!(
                    dev.as_ref(),
                    "M5 render pair construction failed: {:?}\n",
                    error
                );
                return Err(error);
            }
        };
        dev_info!(
            dev.as_ref(),
            "M5 render target, parameter buffer and pool backing ready\n"
        );
        Ok(KBox::new(Self { pair }, GFP_KERNEL)?)
    }
}
