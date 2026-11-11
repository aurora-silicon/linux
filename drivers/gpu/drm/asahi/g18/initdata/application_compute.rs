// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded application CDM owner, separate from the captured addition fixture.
use super::*;
use crate::{g18::uapi::command::TranslatedComputeCommand, g18::address_map as va, g18::fw::compute::*};
use core::sync::atomic::Ordering;

// Application-compute-only knobs K1-K5 (compute diagnostics) and
// g18_app_d62a (case 10). Unlike
// kernel::module! parameters (perm 0, set once at load), these are 0644 with
// the C uint ops, so /sys/module/aurora/parameters/g18_app_* may be changed
// between submissions. prepare_application_compute reads each exactly once
// per submission; nothing else reads them, so graphics and the stage8 fixture
// are unaffected. Registration otherwise follows g18_render_policy.rs.
macro_rules! app_knob {
    ($module:ident, $name:literal, $default:expr, $description:literal) => {
        mod $module {
            use kernel::{bindings, module_param::KernelParam};
            pub(super) static VALUE: core::sync::atomic::AtomicU32 =
                core::sync::atomic::AtomicU32::new($default);
            #[link_section = "__param"]
            #[used(compiler)]
            static PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
                name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
                    kernel::c_str!($name)
                } else { kernel::c_str!(concat!("aurora.", $name)) }),
                // Module lifetime and parameter serialization are managed by the
                // kernel, identically to the kernel::module! registration. A raw
                // address avoids a reference to the mutable extern static.
                #[cfg(MODULE)]
                mod_: &raw mut bindings::__this_module,
                #[cfg(not(MODULE))]
                mod_: core::ptr::null_mut(),
                // SAFETY: exported vmlinux ops; they access `arg` as an unsigned
                // int under the parameter lock, which AtomicU32 represents exactly.
                ops: unsafe { core::ptr::from_ref(&bindings::param_ops_uint) },
                perm: 0o644, level: -1, flags: 0,
                __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 {
                    arg: core::ptr::from_ref(&VALUE).cast_mut().cast(),
                },
            });
            const TYPE: &[u8] = concat!("parmtype=", $name, ":uint\0").as_bytes();
            #[link_section = ".modinfo"]
            #[used(compiler)]
            static TYPE_INFO: [u8; TYPE.len()] = super::modinfo(TYPE);
            const TEXT: &[u8] = concat!("parm=", $name, ":", $description, "\0").as_bytes();
            #[link_section = ".modinfo"]
            #[used(compiler)]
            static TEXT_INFO: [u8; TEXT.len()] = super::modinfo(TEXT);
        }
    };
}

const fn modinfo<const N: usize>(bytes: &[u8]) -> [u8; N] {
    let mut out = [0; N];
    let mut i = 0;
    while i < N { out[i] = bytes[i]; i += 1; }
    out
}

app_knob!(knob_d514, "g18_app_d514", 0xffff_ffff,
    "Application compute command +0xf28 (native desc+0x514). Default 0xffffffff = native, fixture and M5; 0 = candidate160. Read at every compute submission");
// Defaults are the values that retire on hardware (run 162d: exact integer
// compute and atomics readbacks). The native 0xa731=1 alone stalls the
// dispatch silently, with no fault or watchdog.
app_knob!(knob_a731, "g18_app_a731", 0,
    "Application compute first 0xa731: 0 (default) = fixture/M5 (emits 0), proven on hardware; 1 = native integer compute (d420=1,d418=2 emits 1), which stalls the dispatch on Linux. Other values refuse the submission");
app_knob!(knob_sma_pair, "g18_app_sma_pair", 0x6_0010,
    "Application compute 0xa5e1 SMA pair as d550|d558<<16: 0x60010 (16,6) fixture (default, proven on hardware), 0xb001e (30,11) native integer compute, 0xc001e (30,12) candidate160. Must fit the pool plan (30,12); other values refuse the submission");
app_knob!(knob_sma_units, "g18_app_sma_units", 0,
    "Application compute 0x10241[63:16] SMA result (AGXUMADataRec+0x100). Default 0 (proven on hardware); 0xae = native integer compute. Independent of the pool binding generation");
app_knob!(knob_prepare_test, "g18_app_prepare_test", 0,
    "Unpublished preparation rollback E2E: 0 disabled, 1 inject four allocation failures before the next compute command is encoded, 2 do this for the next two commands to cover reuse after retirement. Verify rollback then prepare each original command. Other values refuse before preparation");
app_knob!(knob_pool_test, "g18_app_pool_test", 0,
    "Pool replacement E2E: arm 1 before the first compute. Starts with the qualified 8-MiB pool, then runs original non-spill commands with 30,40,30-MiB shared pools; internal phases 2-4 replace only after verified retirement. Checks staged-owner rollback and reused addresses. Does not admit scratch shaders");
app_knob!(knob_preempt_mode, "g18_app_preempt_mode", 1,
    "Application compute 0x1a510 object: 1 (default) = fresh zeroed 0xc000 bytes mapped only in the job VM at 0x1100000000 (driver arena); 0 = candidate160 16 KiB page at 0x7050000000. Other values refuse the submission");
app_knob!(knob_d62a, "g18_app_d62a", 1,
    "Application compute desc+0x62a (0x1a458 0x14c08a60 texture mode instead of 0x10c08860): 1 (default) = from the command's DRM_ASAHI_COMPUTE_USES_TEXTURE_STATE flag, as native; 0 = never; 2 = always. Other values refuse the submission");

/// Every SMA pair witnessed in a completed native or fixture compute command.
const SMA_PAIRS: [[u64; 2]; 3] = [[30, 11], [30, 12], [16, 6]];

/// One submission's knob values, read once and logged with its kick.
#[derive(Clone, Copy)]
pub(crate) struct ApplicationComputeKnobs {
    d514: u32,
    a731: u32,
    sma_pair: u32,
    sma_units: u32,
    metadata_plan: bool,
    preempt_mode: u32,
    d62a_mode: u32,
    preempt: u64,
    preempt_bytes: u64,
    pool_slot: u32,
    /// The command's USES_TEXTURE_STATE flag and the d62a it produced.
    texture_state: bool,
    d62a: u8,
}

impl ApplicationComputeKnobs {
    fn read() -> Self {
        Self {
            d514: knob_d514::VALUE.load(Ordering::Relaxed),
            a731: knob_a731::VALUE.load(Ordering::Relaxed),
            sma_pair: knob_sma_pair::VALUE.load(Ordering::Relaxed),
            sma_units: knob_sma_units::VALUE.load(Ordering::Relaxed),
            metadata_plan: false,
            preempt_mode: knob_preempt_mode::VALUE.load(Ordering::Relaxed),
            d62a_mode: knob_d62a::VALUE.load(Ordering::Relaxed),
            preempt: 0, preempt_bytes: 0, pool_slot: 0,
            texture_state: false, d62a: 0,
        }
    }

    /// Native run112 sets desc+0x62a (0x1a458 = 0x14c08a60) on every compute
    /// that uses texture or image instructions and on none of the
    /// buffer-only ones; Mesa reports that per command.
    fn resolve_d62a(&mut self, texture_state: bool) {
        self.texture_state = texture_state;
        self.d62a = match self.d62a_mode {
            0 => 0,
            2 => 1,
            _ => u8::from(texture_state),
        };
    }

    fn pair(&self) -> [u64; 2] {
        [u64::from(self.sma_pair & 0xffff), u64::from(self.sma_pair >> 16)]
    }

    fn apply_plan(&mut self, plan: crate::g18::uapi::compute_plan::G18ComputePlan,
        dev: &AsahiDrmDevice) -> Result {
        // Fixed knobs remain available for legacy diagnostics. Never combine
        // them with shader-derived ownership or silently ignore an override.
        if self.sma_pair != 0x6_0010 || self.sma_units != 0 {
            dev_err!(dev.as_ref(), "G18_APP_COMPUTE_REFUSED metadata with SMA overrides\n");
            return Err(EINVAL);
        }
        self.sma_pair = plan.ideal.register_pair[0] | plan.ideal.register_pair[1] << 16;
        self.sma_units = plan.sma_units;
        self.metadata_plan = true;
        Ok(())
    }

    /// Refuse, before any owner changes, values that would make the command
    /// inconsistent. Nothing is published for a refused submission.
    fn validate(&self, dev: &AsahiDrmDevice) -> Result {
        let refusal = if self.a731 > 1 {
            Some(("g18_app_a731", self.a731, "admitted 0 (fixture) or 1 (native)"))
        } else if !SMA_PAIRS.contains(&self.pair()) {
            Some(("g18_app_sma_pair", self.sma_pair,
                "admitted 0xb001e (30,11), 0xc001e (30,12) or 0x60010 (16,6)"))
        } else if self.preempt_mode > 1 {
            Some(("g18_app_preempt_mode", self.preempt_mode,
                "admitted 0 (0x7050000000 page) or 1 (driver arena)"))
        } else if self.d62a_mode > 2 {
            Some(("g18_app_d62a", self.d62a_mode,
                "admitted 0 (never), 1 (from the command flag) or 2 (always)"))
        } else { None };
        if let Some((knob, value, reason)) = refusal {
            dev_err!(dev.as_ref(),
                "G18_APP_COMPUTE_REFUSED knob={} value={:#x}: {}; command not sent\n",
                knob, value, reason);
            return Err(EINVAL);
        }
        Ok(())
    }
}

impl kernel::fmt::Display for ApplicationComputeKnobs {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let [d550, d558] = self.pair();
        write!(f, "d514={:#x} a731={} sma_pair=({},{}) reg_a5e1={:#x} sma_units={:#x} reg_10241={:#x} preempt_mode={} preempt={:#x}+{:#x} texture_state={} d62a_mode={} d62a={} metadata_plan={}",
            self.d514, self.a731, d550, d558, d550 | d558 << 16, self.sma_units,
            (self.pool_slot as i32 as i64 as u64) | u64::from(self.sma_units) << 16,
            self.preempt_mode, self.preempt, self.preempt_bytes,
            u8::from(self.texture_state), self.d62a_mode, self.d62a, u8::from(self.metadata_plan))
    }
}

pub(super) struct ApplicationCompute {
    guard: Option<mmu::T8140VmJobGuard>,
    aliases: KVec<mmu::KernelMapping>,
    // g18_app_preempt_mode=1 object, mapped only in the submitting job VM.
    // Replaced only after the previous generation retired.
    driver_preempt: Option<MappedObject>,
    pool: G18UmaPoolMemory,
    binding: G18UmaPoolBinding,
    previous_pool: Option<G18UmaPoolMemory>,
    previous_binding: Option<G18UmaPoolBinding>,
    bound_context: u32,
    successful_retirement: bool,
    /// Distinguish production shader plans from the legacy replacement probe.
    metadata_pool: bool,
    preempt: MappedObject,
    generation: u32,
    retired: bool,
}

impl G18FirmwareMemory {
    pub(crate) fn prepare_application_compute(&mut self, dev: &AsahiDrmDevice,
        vm: &mmu::Vm, command: &TranslatedComputeCommand)
        -> Result<(G18PreparedComputeCommand, G18UmaPoolBinding, u32, ApplicationComputeKnobs)> {
        if command.sampler_heap != 0 || command.sampler_count != 0
            || command.g17p_add3_buffers.is_some()
            || command.control_stream_end.checked_sub(command.control_stream_base)
                .map_or(true, |n| n > crate::g18::drm::G18_COMPUTE_STREAM_BYTES) {
            pr_err!("G18 compute prepare rejected: sampler_heap={:#x} samplers={} add3={} stream {:#x}..{:#x} (admits at most {} bytes)\n",
                command.sampler_heap, command.sampler_count, command.g17p_add3_buffers.is_some(),
                command.control_stream_base, command.control_stream_end,
                crate::g18::drm::G18_COMPUTE_STREAM_BYTES);
            return Err(ENOTSUPP);
        }
        let mut knobs = ApplicationComputeKnobs::read();
        knobs.validate(dev)?;
        knobs.resolve_d62a(command.g18_texture_state);
        let diagnostic = knob_prepare_test::VALUE.swap(0, Ordering::Relaxed);
        if diagnostic > 2 { return Err(EINVAL); }
        let pool_test = knob_pool_test::VALUE.swap(0, Ordering::Relaxed);
        if pool_test > 4 || (pool_test != 0 && diagnostic != 0) { return Err(EINVAL); }
        let metadata_plan = match command.g18_resource_descriptor.as_ref() {
            Some(words) => Some(crate::g18::uapi::compute_plan::G18ComputePlan::from_descriptor(words)
                .ok_or(EINVAL)?),
            None => None,
        };
        if let Some(plan) = metadata_plan {
            if diagnostic != 0 || pool_test != 0 { return Err(EINVAL); }
            knobs.apply_plan(plan, dev)?;
        }
        if self.application_compute.as_ref().is_some_and(|job| job.metadata_pool)
            && (diagnostic != 0 || pool_test != 0) { return Err(EINVAL); }
        let pool_plan = if pool_test != 0 {
            let mut words = [48,12,0,0,2304,24,0,0,2,1,0,0,0,0,0,0,1,0,0,128,4,0,0];
            if pool_test == 3 { words[4] = 3072; }
            Some(crate::g18::uapi::compute_plan::G18ComputePlan::from_descriptor(&words).ok_or(EINVAL)?)
        } else { None };
        if pool_test == 1 && self.application_compute.is_some()
            || pool_test > 1 && self.application_compute.is_none() { return Err(EINVAL); }
        if self.application_compute.is_none() {
            let high = va::DYNAMIC_HIGH_START..va::DYNAMIC_HIGH_END;
            let mut pool = if let Some(plan) = metadata_plan {
                G18UmaPoolMemory::new_shared_compute(dev, &self.uat, high, plan)?
            } else {
                G18UmaPoolMemory::new_application_compute(dev, &self.uat, high)?
            };
            let binding = pool.prepare_binding()?;
            let preempt = MappedObject::new_wc_in_range(dev, self.uat.kernel_lower_vm(),
                0x70_5000_0000..0x70_5100_0000, 0x4000, 0x4000,
                mmu::PROT_GPU_FW_SHARED_RW)?;
            let mut owner = KBox::new(ApplicationCompute {
                guard: None, aliases: KVec::new(), driver_preempt: None, pool, binding,
                previous_pool: None, previous_binding: None, bound_context: 0,
                successful_retirement: true,
                metadata_pool: metadata_plan.is_some(),
                preempt, generation: 0, retired: true,
            }, GFP_KERNEL)?;
            self.scheduler.prepare(0, 1)?;
            owner.pool.load_hardware_descriptor(dev, &mut self.uma, binding,
                j873_sma_device_words(2, 6)?)?;
            self.application_compute = Some(owner);
        }
        if diagnostic != 0 {
            let before_generation = self.application_compute_generation();
            let before = self.scheduler.snapshot(0)?;
            for checkpoint in 1..=4 {
                let result = self.prepare_application_compute_attempt(dev, vm, command, knobs, checkpoint, None);
                if !matches!(result, Err(error) if error == ENOMEM) { return Err(EIO); }
                let owner = self.application_compute.as_ref().ok_or(EIO)?;
                let after = self.scheduler.snapshot(0)?;
                if !owner.retired || owner.guard.is_some() || !owner.aliases.is_empty()
                    || owner.driver_preempt.is_some() || owner.generation != before_generation
                    || !self.compute.unpublished_idle()
                    || (before.submitted, before.completed, before.linked)
                        != (after.submitted, after.completed, after.linked) { return Err(EIO); }
                dev_info!(dev.as_ref(),
                    "G18_COMPUTE_PREPARE_E2E checkpoint={} rollback=pass generation={} scheduler={}/{}/{} published=0\n",
                    checkpoint, before_generation, after.submitted, after.completed, after.linked);
            }
        }
        if pool_test > 1 {
            let job = &mut **self.application_compute.as_mut().ok_or(EIO)?;
            let scheduler = self.scheduler.snapshot(0)?;
            if !job.retired || !job.successful_retirement || job.previous_pool.is_some()
                || job.generation == 0 || scheduler.submitted != job.generation
                || scheduler.completed != job.generation || scheduler.linked != 0 {
                return Err(EBUSY);
            }
            job.pool.verify_replacement_rollback(dev, &self.uat, pool_plan.ok_or(EINVAL)?)?;
        }
        let replacement = if let Some(plan) = metadata_plan {
            let job = self.application_compute.as_ref().ok_or(EIO)?;
            (job.pool.shared_compute_plan() != Some(plan)).then_some(plan)
        } else if pool_test > 1 { pool_plan } else { None };
        let prepared = self.prepare_application_compute_attempt(dev, vm, command, knobs, 0, replacement)?;
        if pool_test != 0 && pool_test < 4 {
            let _ = knob_pool_test::VALUE.compare_exchange(0, pool_test + 1,
                Ordering::Relaxed, Ordering::Relaxed);
        }
        if diagnostic == 2 {
            // The next submission still requires this command to retire.
            // Preserve a concurrent explicit parameter change, and never add
            // a fallible operation after the prepared owner was committed.
            let _ = knob_prepare_test::VALUE.compare_exchange(0, 1,
                Ordering::Relaxed, Ordering::Relaxed);
        }
        Ok(prepared)
    }

    fn prepare_application_compute_attempt(&mut self, dev: &AsahiDrmDevice,
        vm: &mmu::Vm, command: &TranslatedComputeCommand,
        mut knobs: ApplicationComputeKnobs, fail_checkpoint: u32,
        replacement: Option<crate::g18::uapi::compute_plan::G18ComputePlan>)
        -> Result<(G18PreparedComputeCommand, G18UmaPoolBinding, u32, ApplicationComputeKnobs)> {
        let job = &mut **self.application_compute.as_mut().ok_or(EIO)?;
        if !job.retired || job.guard.is_some() || !self.compute.unpublished_idle()
            || job.previous_pool.is_some() || job.previous_binding.is_some() { return Err(EBUSY); }
        let generation = job.generation.checked_add(1).ok_or(EOVERFLOW)?;
        if generation > 0x00ff_ffff { return Err(EOVERFLOW); }
        let previous = self.scheduler.snapshot(0)?;
        if previous.submitted != job.generation || previous.completed != job.generation
            || previous.linked != 0 { return Err(EBUSY); }
        // The pool is sized once for its plan pair; a larger pair would name
        // backing that does not exist. The slot must not reach the SMA half.
        let mut candidate = if let Some(plan) = replacement {
            if !job.successful_retirement || job.generation == 0 { return Err(EBUSY); }
            let mut pool = job.pool.replacement_shared_compute(dev, &self.uat,
                va::DYNAMIC_HIGH_START..va::DYNAMIC_HIGH_END, plan)?;
            let mut binding = pool.prepare_binding()?;
            binding.generation = job.binding.generation.checked_add(1).ok_or(EOVERFLOW)?;
            if binding.generation == u64::MAX { return Err(EOVERFLOW); }
            Some((pool, binding))
        } else { None };
        let (pool, binding) = match candidate.as_mut() {
            Some((pool, binding)) => (pool, *binding),
            None => (&mut job.pool, job.binding),
        };
        let plan = pool.scratch_plan().ok_or(EINVAL)?;
        let pair = knobs.pair();
        if let Some(expected) = command.g18_resource_descriptor.as_ref() {
            let expected = crate::g18::uapi::compute_plan::G18ComputePlan::from_descriptor(expected)
                .ok_or(EINVAL)?;
            if pool.shared_compute_plan() != Some(expected)
                || pair != expected.ideal.register_pair.map(u64::from)
                || knobs.sma_units != expected.sma_units || !knobs.metadata_plan {
                return Err(EIO);
            }
            crate::g18_kick_info!(dev.as_ref(),
                "G18_COMPUTE_RESOURCE_PLAN bytes={} blocks={} pair={:?} counters={:?} sma_units={} replacement={}\n",
                expected.ideal.bytes, expected.ideal.block_count, pair,
                expected.ideal.chunk_counters, expected.sma_units, u8::from(replacement.is_some()));
        } else if job.metadata_pool
            && (plan.block_bytes != 0x20_0000 || plan.block_count < 4
                || plan.register_pair[0] < 30 || plan.register_pair[1] < 12
                || plan.chunk_counters[1] < 260)
        {
            dev_err!(dev.as_ref(), "G18_APP_COMPUTE_REFUSED legacy command exceeds retained native pool\n");
            return Err(ENOTSUPP);
        }
        if pair[0] > plan.register_pair[0] || pair[1] > plan.register_pair[1] {
            dev_err!(dev.as_ref(),
                "G18_APP_COMPUTE_REFUSED knob=g18_app_sma_pair value={:#x}: exceeds pool plan {:?}; command not sent\n",
                knobs.sma_pair, plan.register_pair);
            return Err(EINVAL);
        }
        if binding.slot >= 0x1_0000 {
            dev_err!(dev.as_ref(),
                "G18_APP_COMPUTE_REFUSED knob=g18_app_sma_units value={:#x}: pool slot {} overlaps 0x10241[63:16]; command not sent\n",
                knobs.sma_units, binding.slot);
            return Err(EINVAL);
        }
        knobs.pool_slot = binding.slot;
        let driver = va::APPLICATION_COMPUTE_PREEMPT_DRIVER;
        let driver_bytes = va::APPLICATION_COMPUTE_PREEMPT_DRIVER_BYTES;
        let driver_range = driver..driver + driver_bytes + mmu::UAT_PGSZ as u64;
        let reserved = vm.kernel_range();
        if knobs.preempt_mode == 1
            && (driver_range.start < reserved.start || driver_range.end > reserved.end) {
            dev_err!(dev.as_ref(),
                "G18_APP_COMPUTE_REFUSED knob=g18_app_preempt_mode value=1: VM_CREATE driver range {:#x}..{:#x} does not contain {:#x}..{:#x}; command not sent\n",
                reserved.start, reserved.end, driver_range.start, driver_range.end);
            return Err(EINVAL);
        }
        let guard = vm.retain_t8140_job()?;
        job.aliases.clear();
        job.driver_preempt = None;
        let attempt = (|| {
            if fail_checkpoint == 1 { return Err(ENOMEM); }
            let context = self.compute.bind_process(dev, &self.uat, vm)?;
            if fail_checkpoint == 2 { return Err(ENOMEM); }
            let mut aliases = pool.compute_application_aliases(dev, &self.uat, vm)?;
            if fail_checkpoint == 3 { return Err(ENOMEM); }
            let mut driver_preempt = None;
            let preempt = if knobs.preempt_mode == 0 {
                let preempt = job.preempt.iova();
                job.preempt.with_bytes_mut(|bytes| { bytes.fill(0); Ok(()) })?;
                let alias = job.preempt.map_alias_at(vm, preempt, mmu::PROT_GPU_SHARED_RW)?;
                aliases.push(alias, GFP_KERNEL)?;
                if vm.translate_iova(preempt)? != self.uat.kernel_lower_vm().translate_iova(preempt)? {
                    return Err(EFAULT);
                }
                knobs.preempt_bytes = 0x4000;
                preempt
            } else {
                // Native keeps this object in the process VM only (0x100_003f_0000,
                // 48 KiB). The driver arena is refused to userspace binds, and the
                // range admits exactly one placement: the object plus one guard.
                let object = MappedObject::new_wc_in_range_with_guards(dev, vm, driver_range,
                    driver_bytes as usize, mmu::UAT_PGSZ as u64, mmu::PROT_GPU_SHARED_RW, 1)?;
                let preempt = object.iova();
                driver_preempt = Some(object);
                if preempt != driver { return Err(EFAULT); }
                for offset in (0..driver_bytes).step_by(mmu::UAT_PGSZ) {
                    vm.translate_iova(preempt + offset)?;
                }
                if vm.translate_iova(preempt + driver_bytes).is_ok() { return Err(EFAULT); }
                knobs.preempt_bytes = driver_bytes;
                preempt
            };
            knobs.preempt = preempt;
            if fail_checkpoint == 4 { return Err(ENOMEM); }
            let inputs = G18ComputeCommandInputs {
                sequence: u64::from(generation), d100: 1,
                completion: G18ComputeCompletionState {
                    stamp: generation << 8, index: 11, entries: 256, generation: 0, base: 1,
                },
                d4e0: command.control_stream_terminator, d7b8: binding.state,
                ..Default::default()
            };
            // G18 preemption pointers are +0x1b80..+0x1b98 (native gallery),
            // not the M5 +0x1480 layout. These are fresh driver-owned zero pages.
            let registers = G18ComputeRegisterInputs {
                program: [preempt, command.control_stream_base, preempt + 0x1b80,
                    preempt + 0x1b88, preempt + 0x1b90, preempt + 0x1b98],
                d550: pair[0], d558: pair[1], d774: 0x1c, d5c4: 8,
                d420: knobs.a731 as u8, d418: 2, d50c: 1, d514: knobs.d514,
                d62a: knobs.d62a,
                context_id: context, pool_slot: binding.slot,
                sma_units: u64::from(knobs.sma_units),
                a10aac: 1, a6d0: 1, clusters: 2, cluster_width: 6,
                ..Default::default()
            };
            let prepared = self.compute.prepare(self.scheduler.binding(0)?, &inputs, &registers)?;
            Ok((prepared, aliases, driver_preempt, knobs))
        })();
        let (prepared, aliases, driver_preempt, knobs) = match attempt {
            Ok(value) => value,
            Err(error) => {
                // The local VM guard outlives process/alias cleanup. A cleanup
                // failure remains fatal; it is never called a clean rollback.
                self.compute.abort_unpublished_process()?;
                return Err(error);
            }
        };
        job.guard = Some(guard);
        job.aliases = aliases;
        job.driver_preempt = driver_preempt;
        job.retired = false;
        job.generation = generation;
        if command.g18_resource_descriptor.is_some() { job.metadata_pool = true; }
        if let Some((pool, binding)) = candidate {
            job.previous_pool = Some(core::mem::replace(&mut job.pool, pool));
            job.previous_binding = Some(job.binding);
            job.binding = binding;
        }
        self.completion_masks = [0; 2];
        Ok((prepared, job.binding, generation, knobs))
    }

    /// The binding the firmware must still own before the next tag15.
    pub(crate) fn application_compute_previous_binding(&self) -> Result<(G18UmaPoolBinding, u32)> {
        let job = self.application_compute.as_ref().ok_or(EIO)?;
        Ok((job.previous_binding.unwrap_or(job.binding), job.bound_context))
    }

    /// Called only after the session checked the old firmware binding and the
    /// retired scheduler. Both views remain owned if any later step fails.
    pub(crate) fn install_application_compute_pool(&mut self, dev: &AsahiDrmDevice) -> Result {
        let job = &mut **self.application_compute.as_mut().ok_or(EIO)?;
        if let Some(previous) = job.previous_pool.as_mut() {
            let old = job.previous_binding.ok_or(EIO)?;
            let scheduler = self.scheduler.snapshot(0)?;
            if !job.successful_retirement || job.generation < 2
                || scheduler.submitted != job.generation - 1
                || scheduler.completed != job.generation - 1 || scheduler.linked != 0 {
                return Err(EBUSY);
            }
            job.pool.replace_hardware_descriptor(dev, &mut self.uma, job.binding,
                previous, old, j873_sma_device_words(2, 6)?)?;
            crate::g18_kick_info!(dev.as_ref(),
                "G18_COMPUTE_POOL_SWITCH compute={} old_state={:#x} new_state={:#x} old_generation={} new_generation={} slot={} old_retained=1\n",
                job.generation, old.state, job.binding.state, old.generation,
                job.binding.generation, job.binding.slot);
        }
        Ok(())
    }

    /// The caller must first verify the new firmware binding, including both
    /// directions of queue membership. A completion stamp alone is insufficient.
    pub(crate) fn retire_application_compute(&mut self, generation: u32, context: u32) -> Result {
        let job = &mut **self.application_compute.as_mut().ok_or(EIO)?;
        if job.retired || job.generation != generation { return Err(EINVAL); }
        self.compute.retire_application(generation << 8)?;
        job.bound_context = context;
        job.successful_retirement = true;
        job.retired = true;
        job.guard = None;
        job.previous_pool = None;
        job.previous_binding = None;
        Ok(())
    }

    /// The last compute stamp kicked to QID 11 (0 before the first dispatch).
    pub(crate) fn application_compute_generation(&self) -> u32 {
        self.application_compute.as_ref().map_or(0, |job| job.generation)
    }

    /// As [`Self::retire_application_compute`] for a dispatch a recovery ended.
    pub(crate) fn retire_recovered_application_compute(&mut self, generation: u32) -> Result {
        let job = &mut **self.application_compute.as_mut().ok_or(EIO)?;
        if job.retired || job.generation != generation { return Err(EINVAL); }
        self.compute.retire_recovered()?;
        job.successful_retirement = false;
        job.retired = true;
        job.guard = None;
        Ok(())
    }

    pub(crate) fn quarantine_application_compute(&mut self) {
        if let Some(job) = &self.application_compute {
            if let Some(guard) = &job.guard { guard.quarantine(); }
        }
    }
}
