// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod alloc;
mod binding;
mod buffer;
mod channel;
mod cleanup;
mod debug;
mod driver;
mod event;
mod file;
mod float;
mod fw;
mod fwid;
#[cfg(CONFIG_DRM_ASAHI_G15)]
mod g15;
mod g17;
mod gem;

#[cfg(CONFIG_DRM_ASAHI_G16)]
mod g16;
#[cfg(CONFIG_DRM_ASAHI_G18)]
mod g18;
#[cfg(CONFIG_DRM_ASAHI_M5)]
mod m5;
mod gpu;
#[cfg(CONFIG_DRM_ASAHI_M3_T8122)]
mod j413;

mod hw;
mod initdata;
mod m3;
mod legacy_rx;
mod mem;
mod memacct;
mod microseq;
mod mmu;
mod object;
mod pgtable;
mod queue;
mod queue_share;
mod regs;
mod retain;
mod slotalloc;
mod stats;
mod stats_busy;
mod stats_raw;
mod sysfs_exports;
mod timestamp;
mod usage;
mod util;
#[cfg(CONFIG_DRM_ASAHI_MAPLE_TREE)]
mod vm;
mod workqueue;

use kernel::prelude::*;

#[cfg(CONFIG_DRM_ASAHI_G15)]
type G15Registration =
    kernel::driver::Registration<kernel::platform::Adapter<g15::Driver<'static>>>;

#[cfg(CONFIG_DRM_ASAHI_G16)]
type G16Registration = kernel::driver::Registration<kernel::platform::Adapter<g16::Driver>>;
#[cfg(not(CONFIG_DRM_ASAHI_G16))]
type G16Registration = ();

fn register_g16(module: &'static kernel::ThisModule) -> Result<Option<Pin<KBox<G16Registration>>>> {
    #[cfg(CONFIG_DRM_ASAHI_G16)]
    {
        Ok(Some(KBox::pin_init(
            kernel::driver::Registration::new(g16::DRIVER_NAME, module),
            GFP_KERNEL,
        )?))
    }
    #[cfg(not(CONFIG_DRM_ASAHI_G16))]
    {
        let _ = module;
        Ok(None)
    }
}

#[pin_data]
struct DriverModule {
    #[cfg(CONFIG_DRM_ASAHI_G15)]
    #[pin]
    _g15: G15Registration,
    // Field drop order unregisters/joins driver producers before queue teardown.
    _g16: Option<Pin<KBox<G16Registration>>>,
    #[cfg(CONFIG_DRM_ASAHI_G18)]
    #[pin]
    _g18: kernel::driver::Registration<kernel::platform::Adapter<g18::probe::Driver>>,
    #[pin]
    _driver: kernel::driver::Registration<kernel::platform::Adapter<driver::AsahiPlatformDriver>>,
    _legacy_rx: legacy_rx::QueueOwner,
    _cleanup: cleanup::QueueOwner,
    #[cfg(CONFIG_DRM_ASAHI_G15)]
    _g15_ready: (),
}

impl kernel::InPlaceModule for DriverModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        // SAFETY: Module init runs once, before either adapter is registered below.
        unsafe { binding::init() };
        // SAFETY: Module init runs once, before either adapter is registered below.
        unsafe { queue_share::init() };
        // SAFETY: Module init runs once, before either adapter is registered below.
        unsafe { g17::exec_share::init() };
        try_pin_init!(Self {
            // Initialize the queue first, before registration can invoke probe.
            _cleanup: cleanup::QueueOwner::new()?,
            _legacy_rx: legacy_rx::QueueOwner::new()?,
            #[cfg(CONFIG_DRM_ASAHI_G15)]
            _g15 <- kernel::driver::Registration::new(c"aurora-g15", module),
            _driver <- kernel::driver::Registration::new(
                <Self as kernel::ModuleMetadata>::NAME,
                module,
            ),
            _g16: register_g16(module)?,
            #[cfg(CONFIG_DRM_ASAHI_G18)]
            _g18 <- kernel::driver::Registration::new(g18::probe::DRIVER_NAME, module),
            // All fallible fields are initialized. This final unit initializer cannot fail.
            #[cfg(CONFIG_DRM_ASAHI_G15)]
            _g15_ready: g15::activate(),
        })
    }
}

module! {
    type: DriverModule,
    name: "aurora",
    description: "AGX GPU driver for Apple silicon SoCs",
    license: "Dual MIT/GPL",
    alias: ["asahi"],
    params: {
        m5_mesh_storage_chunks: u32 {
            default: 0x200,
            description: "M5 mesh rendering buffer bound to API task (object-stage) render kicks only, in 64 KiB chunks; 0 disables the binding and the task feature",
        },
        m5_thermal_governor: u32 {
            default: 0,
            description: "M5 thermal flags: bit 0 enables the host GPU power budget controller, bit 2 lets the controller lower the CPU frequency ceiling while the GPU is budget-bound, bit 3 runs the fans at their maximum while it governs; default 0 preserves the existing policy",
        },
        m5_thermal_target_mc: u32 {
            default: 90000,
            description: "M5 GPU power budget governor target for the SoC hotspot temperature computed by the SMC, in milli-degrees C (70000..104000)",
        },
        m5_depth_msaa_compression: u32 {
            default: 1,
            description: "Enable single-layer D32 compressed 2x/4x ZLS admission",
        },
        debug_flags: u64 {
            default: 0,
            // permissions: 0o644,
            description: "Debug flags",
        },
        compute_preempt: i32 {
            default: -1,
            description: "Compute preemption (-1: M2 Max or Debug0; 0: off; nonzero: on)",
        },
        fault_control: u32 {
            default: 0xb,
            // permissions: 0,
            description: "Fault control (0x0: hard faults, 0xb: macOS default)",
        },
        initial_tvb_size: usize {
            default: 0x8,
            // permissions: 0o644,
            description: "Initial TVB size in blocks",
        },
        robust_isolation: u32 {
            default: 0,
            // permissions: 0o644,
            description: "Fully isolate GPU contexts (limits performance)",
        },
        legacy_inline_submit: u32 {
            default: 1,
            description: "G13/G14: publish ready in-order jobs from SUBMIT (0: off)",
        },
        t8112_telemetry_ro: bool {
            default: false,
            description: "J413 startup trial: expose the shared telemetry window read-only",
        },
        t8112_dashboard_ram: bool {
            default: false,
            description: "J413 idle-policy trial: back the dashboard update slot with isolated RAM",
        },
        t8112_rgx_ram_pages: u32 {
            default: 255,
            description: "J413 RGX scratch trial: one bit per 16 KiB RAM page",
        },
        t8112_ram_slots: u64 {
            default: 0,
            description: "J413 idle trial: isolated RAM substitutes, bit per descriptor (0, 1, 3, 8, 10, 12, 17, 33, 40, 44)",
        },
        t8112_cre: bool {
            default: false,
            description: "J413 power trial: enable CRE during firmware initialization",
        },
        t8112_power_filter: bool {
            default: false,
            description: "J413 26B123: seed the calibrated cold power controllers",
        },
        t8112_power_curve: bool {
            default: false,
            description: "J413 indexed wake: populate the calibrated operating-point power curve",
        },
        t8112_alarm_fields: u32 {
            default: 0,
            description: "J413 indexed wake: number of additional stock alarm fields to publish",
        },
        t8112_calibrated_sensors: u64 {
            default: 0,
            description: "J413 trial: sensor inventory from this unit's PMGR calibration records",
        },
        t8112_fast_die_fields: u32 {
            default: 0,
            description: "J413 metrology trial: cumulative fast-die release temperature, mask, integral and proportional gains (0-4)",
        },
        t8112_firmware_log: bool {
            default: false,
            description: "J413 diagnostic: open the firmware log publication gate in the directory",
        },
        t8112_service_stage: bool {
            default: true,
            description: "J413 26B123: also require powered stage 2 before the first engine entry",
        },
        t8112_command_notifier: u32 {
            default: 0,
            description: "J413 first command: command offset of the per-command notifier pointer (0 none; firmware reads +0xc)",
        },
        t8112_drain_statistics: bool {
            default: false,
            description: "J413 26B123: consume the firmware statistics ring while polling",
        },
        t8112_client_counter: bool {
            default: false,
            description: "J413 first command: place the signal counter at a context-0 client VA",
        },
        t8112_priority_row: u32 {
            default: 255,
            description: "J413 first command: legacy priority row and matching submission group (0-3), 255 for the stock priority fields in group 0",
        },
        t8112_stock_header: bool {
            default: false,
            description: "J413 first command: type-9 prefix (data master at +0x18) and a tracking slot with its retirement cursor",
        },
        t8112_completion_record: bool {
            default: false,
            description: "J413 first command: completion destination at command +0x30",
        },
        t8112_job_context: u32 {
            default: 0,
            description: "J413 26B123: client context of the kernel-owned job (0 kernel context, 1-63 a clone of it)",
        },
        t8112_provider_object: u32 {
            default: 0,
            description: "J413 26B123 compute job: provider-wide object named by start/finish (0 directory +0x264, 1 directory +0x26c)",
        },
        t8112_tail_probe: u32 {
            default: 0,
            description: "J413 26B123 compute job diagnostic: poison or fill command tail words",
        },
        t8112_start_probe: u32 {
            default: 0,
            description: "J413 26B123 compute job diagnostic: poison start-record operands (1 provider, 2 channel, 3 command-relative)",
        },
        t8112_job_wait_ms: u32 {
            default: 3000,
            description: "J413 26B123: how long to wait for the held command to retire",
        },
        t8112_power_zones: u32 {
            default: 1,
            description: "J413 26B123: average-power zone count seeded in the controller (stock J413 has 0)",
        },
        t8112_rekick_ms: u32 {
            default: 0,
            description: "J413 26B123: repeat the engine kick this long after submission (0 never)",
        },
        t8112_dashboard_real: bool {
            default: false,
            description: "J413 26B123: map the real push-telemetry dashboard page writable for the firmware instead of private RAM",
        },
        t8112_state_cap: u32 {
            default: 0,
            description: "J413 26B123: upper performance-state cap in hundredths at shared configuration +0x98 (0 = ceiling)",
        },
        t8112_firmware_trace: bool {
            default: false,
            description: "J413 26B123 diagnostic: set the firmware debug bit that enables its trace ring",
        },
        t8112_trace_bits: u32 {
            default: 2,
            description: "J413 26B123 diagnostic: firmware debug category bits used with t8112_firmware_trace",
        },
        t8112_trace_show: u32 {
            default: 160,
            description: "J413 26B123 diagnostic: number of trace records to print",
        },
        t8112_hw_legacy: u64 {
            default: 0,
            description: "J413 26B123: carry legacy HwDataB configuration words to the inline hardware block (1 num_frags, 2 unk_504, 4 unk_4c0, 8 unk_534, 0x10 unk_524, 0x20 flag/clock group)",
        },
        t8112_forced_stop: u32 {
            default: 0,
            description: "J413 trial: RTKit acknowledged power-off before teardown (1 if still running, 2 always)",
        },
        t8112_queue_lane: u32 {
            default: 0,
            description: "J413 trial queue +0x48: 0 as the priority row writes it, 1 all-ones sentinel, n+2 lane n",
        },
        t8112_task_configuration: u32 {
            default: 0,
            description: "J413 trial scheduler context +0x26: 0 untouched, n+1 task-configuration byte n",
        },
        t8112_context_id_field: bool {
            default: false,
            description: "J413 trial client context: identifier in bits 63:48 of both table entries",
        },
        t8112_private_scan: u32 {
            default: 0,
            description: "J413 trial, read-only firmware data log: 1 words around pointers to the job's host objects while parked, 2 non-zero rows of the queue-record areas, 4 both after the wait ends",
        },
        t8112_drm: bool {
            default: false,
            description: "J413 27.2: keep the firmware running after the boot compute job and register a compute-only DRM device",
        },
        t8112_drm_ring_skew: bool {
            default: false,
            description: "J413 27.2 diagnostic: publish one empty submission entry at take-over",
        },
        t8112_queue_capacity: u32 {
            default: 0x500,
            description: "J413 27.2: queue capacity at position block +0x50 (16 x channel count, at most 0x800)",
        },
        t8112_job_order: u32 {
            default: 0,
            description: "J413 26B123 compute job: 0 alone as the first command, 1 as the second command after the retiring signal, 2 first with the signal published behind it",
        },
        t8112_compute_job: bool {
            default: false,
            description: "J413 26B123: submit a kernel-owned type-3 compute job (terminate-only CDM stream) in place of the signal",
        },
        t8112_command_service: bool {
            default: false,
            description: "J413 26B123: request command-handler installation in the firmware data and wait for it",
        },
        t8112_ring_start: u32 {
            default: 0,
            description: "J413 diagnostic: index of the trial ring's first entry (the ring starts empty there; 255 makes the first client command wrap)",
        },
        t8112_first_publication: bool {
            default: true,
            description: "J413 first command: first-publication byte of the submission entry",
        },
        t8112_queue_owner: u32 {
            default: 0xffffffff,
            description: "J413 first command: owning process word of the work queue",
        },
        t8112_class_opcode: u32 {
            default: 0x1b,
            description: "J413 first command: device-control opcode of the engine-class registrations",
        },
        t8112_opening_opcode: u32 {
            default: 0x15,
            description: "J413 first command: device-control opcode of the opening record (0x15 on the 27.x line, 0x16 on J613 26.6.2)",
        },
        t8112_command_engine: u32 {
            default: 2,
            description: "J413 first command: data master of the signal (0 tiler, 1 fragment, 2 compute)",
        },
        t8112_command_type: u32 {
            default: 9,
            description: "J413 first command: channel-command type word of the kernel-owned signal",
        },
        t8112_engine_classes: u32 {
            default: 0,
            description: "J413 first command: 0 none, 1 stage the extended opening control record, 2 also register two engine classes per data master",
        },
        t8112_active_followup: u32 {
            default: 0,
            description: "J413 acknowledged wake: 0 observe, 1 release, 2 complete a held fence then release",
        },
        t8112_post_power_job: bool {
            default: false,
            description: "J413 power trial: publish one completion after both real-core idle commands retire",
        },
        t8112_mtr_sensors: bool {
            default: false,
            description: "J413 stock power trial: use the G14G board sensor mask in the secure metrology controller",
        },
        t8112_sgx_open: bool {
            default: false,
            description: "J413 power trial: open the SGX synchronization window with inherited platform selection",
        },
        t8112_gpc_irq_bit: u32 {
            default: 0,
            description: "J413 stock power trial: enable one GPC interrupt bit (1 or 2)",
        },
        t8112_event_completion: bool {
            default: false,
            description: "J413 power trial: select event-assisted operating-point completion",
        },
        t8112_base_request: bool {
            default: false,
            description: "J413 power trial: initialize the wake operand from the board base operating point",
        },
        t8112_performance_control: bool {
            default: false,
            description: "J413 power trial: request firmware-managed operating points",
        },
        t8112_pmp_vote: bool {
            default: false,
            description: "J413 power trial: acquire the linked PMP AGX device before ASC start",
        },
        t8112_fender_gating: u32 {
            default: 0,
            description: "J413 idle trial: Fender gating at power-on (0 inherited, 1 hold, 2 automatic)",
        },
        t8112_idle_policy: bool {
            default: false,
            description: "J413 idle trial: request and release GPU power through the idle policy",
        },
        t8112_handshake: u32 {
            default: 0,
            description: "J413 GPU trial: 1 UAT, 2 legacy InitData, 3-6 neighbouring ABI controls, 7 G14G root, 9/10 extended channel construction, 11 J413 stopped construction, 12 J413 startup with writable MMIO limited to SGX, 13 one kernel-owned compute fence, 14 matching idle control",
        },
        legacy_rx_fastpath: u32 {
            default: 1,
            description: "G13/G14: drain GPU events on a dedicated receive worker (0: off)",
        },
        legacy_rx_util_min: u32 {
            default: 512,
            description: "G13/G14: GPU event-drain utilization minimum (0..1024)",
        },
        compute_timeout_max_ms: u32 {
            default: 30000,
            description: "G17: longest compute watchdog allowance a queue may set, ms (floor 2000)",
        },
        g15_deadline_ms: u32 {
            default: 5000,
            description: "G15: host deadline of a published prefix without progress, ms (min 100)",
        },
        g15_compute_deadline_ms: u32 {
            default: 1000,
            description: "G15: no-progress budget of a started compute before the class-2 kill, ms",
        },
        g15_repoll_us: u32 {
            default: 15,
            description: "G15: repoll interval while a ready prefix waits for record consumption, us (5..100)",
        },
        g15_resnap_us: u32 {
            default: 100,
            description: "G15: completion repoll interval, us (25..1000)",
        },
        g15_idle_standby_us: u32 {
            default: 0,
            description: "G15: firmware idle standby time override, us (Globals +0x7a8, 100..100000; 0: the device tree's, else the tuned value)",
        },
        g15_idle_min_conf: u32 {
            default: 500,
            description: "G15: firmware idle minimum confidence, permille (Globals +0x7c4)",
        },
        g15_recovery: u32 {
            default: 1,
            description: "G15: answer firmware halts with the halt/resume handshake (0: fail-stop)",
        },
        g15_stream_gate: u32 {
            default: 1,
            description: "G15: admit only known CDM/VDM stream records (0: bring-up only)",
        },
        g15_pb_max_blocks: u32 {
            default: 3072,
            description: "G15: PB growth maximum given to the firmware, 128 KiB blocks (64..4095)",
        },
        g15_early_tiling: u32 {
            default: 0,
            description: "G15: advertise FRAGMENT_BARRIERS, mapped to early tiling (slower on 2419)",
        },
        g15_accept_text_prefix: u64 {
            default: 0,
            description: "G15: admit an unpinned GPU firmware build whose TEXT SHA-256 starts with these 8 bytes (bring-up only)",
        },
        stats_export: u8 {
            default: 1,
            // permissions: 0,
            description: "Export AGX firmware stats to sysfs (1 = on, 0 = off)",
        },
        mem_limit_pct: u32 {
            default: 67,
            description: "Device-wide GPU memory limit in percent of RAM (0 or >= 100: none)",
        },
        client_mem_limit_pct: u32 {
            default: 50,
            description: "Per-client GPU memory limit in percent of RAM (0: the device limit)",
        },
        oom_attribution: u8 {
            default: 1,
            description: "Count held GPU memory in its creator's RSS for the OOM killer (1 = on)",
        },
        m5_fw_trace: u32 {
            default: 0,
            description: "M5: firmware trace classes (power bit 0, submission bit 1, recovery classifier bit 9)",
        },
        g16_deadline_ms: u32 {
            default: 5000,
            description: "G16: administrative completion-stamp deadline, ms (0 disables; long running work can exceed it)",
        },
        g16_compute_deadline_ms: u32 {
            default: 5000,
            description: "G16: compute completion-stamp deadline, ms (0 permits unbounded compute; firmware recovery stays active)",
        },
        g16_repoll_us: u32 {
            default: 15,
            description: "G16: repoll interval while publication waits for record consumption, us",
        },
        g16_resnap_us: u32 {
            default: 100,
            description: "G16: completion repoll interval, us",
        },
        g16_recovery: u32 {
            default: 1,
            description: "G16: answer qualified firmware recovery handshakes",
        },
        g16_stream_gate: u32 {
            default: 1,
            description: "G16: validate CDM/VDM stream records",
        },
    },
}
