// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod agx_power_recovery;
mod alloc;
mod apple_gpu_topology;
mod buffer;
mod channel;
#[cfg(CONFIG_DEV_COREDUMP)]
mod crashdump;
mod debug;
mod driver;
mod drm_gpu;
mod event;
mod file;
mod float;
mod fw;
mod m3_resources;
mod m3_firmware;
mod m3_device;
mod m3_rtkit;
mod m3_init_storage;
mod m3_compute;
mod m3_render;
mod m3_queue_layout;
mod m3_sync_layout;
mod m3_state_layout;
mod m3_scene_layout;
mod m3_pool_layout;
mod m3_pool;
mod m3_parameter_layout;
mod m3_compute_layout;
mod m3_compute_sequence;
mod m3_render_sequence;
mod m3_tiler_command;
mod m3_fragment_command;
mod m3_pass_layout;
mod m3_shared_layout;
mod m3_pass;
mod m3_memory;
mod m3_compute_storage;
mod m3_submit;
mod m3_config;
mod m3_board;
mod m3_init_layout;
mod m3_runtime;
mod m3_coverage;
mod m3_drm;
mod m3_params;
mod m3_adt_config;
mod m3_thermal;
mod m3_thermal_policy;
mod m3_client;
mod g15_boot;
mod g15_initdata;
mod g15_probe;
mod g15_selftest;
mod g16_firmware;
#[cfg(CONFIG_DEV_COREDUMP)]
mod g16_fault;
mod g16_device;
mod agx_host_progress;
mod agx_memory_stats;
mod g16_rtkit;
mod g16_runtime;
mod g16_drm;
mod g16_submit;
mod g16_initdata;
mod g16_memory;
mod g16_config;
mod g16_power;
mod g16_queue;
mod g16_compute;
mod g16_attachments;
mod g16_dispatch;
mod g16_job;
mod g16_render;
mod g16_render_command;
mod g16_render_state;
mod g16_render_job;
mod g16_resources;
mod g17_adt_j700;
mod g17_asc_bringup;
mod g17_boot;
mod g17_completion;
mod g17_compute;
mod g17_drm;
mod g17_firmware;
mod g17_initdata;
mod g17_job;
mod g17_status;
mod g17_queue_limits;
mod g17_lifecycle;
mod g17_live_boot;
mod g17_manager;
mod g17_power_preflight;
mod g17_render;
mod g17_resources;
mod g17_rtkit;
mod g17_submission;
mod g17_trace_capture;
mod g17_uapi;
mod gem;
mod gpu;
mod hw;
mod identity;
mod initdata;
mod mem;
mod microseq;
mod mmu;
mod object;
mod pgtable;
mod pgtable_memory;
mod handoff_lock;
mod queue;
mod regs;
mod slotalloc;
mod uat;
mod util;
#[cfg(CONFIG_DRM_ASAHI_MAPLE_TREE)]
mod vm;
mod workqueue;

kernel::module_platform_driver! {
    type: driver::AsahiDriver,
    name: "asahi",
    description: "AGX GPU driver for Apple silicon SoCs",
    license: "Dual MIT/GPL",
    params: {
        debug_flags: u64 {
            default: 0,
            // permissions: 0o644,
            description: "Debug flags",
        },
        m3_render_batch_size: u32 {
            default: 1,
            description: "M3 bounded ordered render batch size (1..16)",
        },
        m3_compute_batch_size: u32 {
            default: 1,
            description: "M3 bounded ordered compute batch size (1..16)",
        },
        m3_early_tiling: u32 {
            default: 0,
            description: "M3 opt-in fragment-only stage dependencies",
        },
        m3_expose: i32 {
            default: -1,
            description: "M3 render node registration: -1 = auto (only on apple,j514s, where the M3 runtime is validated), 0 = never, 1 = always",
        },
        g16_pstate: u32 {
            default: 9,
            description: "J713 fixed firmware performance state (1..9, 338..1470 MHz); bounded monitored development use",
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
        g17p_simplefb_iova: u64 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_reg_dump: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_scan_gate: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_test_stage: u32 {
            default: 2,
            // permissions: 0,
            description: "G17P (M5) bring-up control; default 2",
        },
        g17p_max_recoveries: u32 {

            default: 0,

            description: "G17P (M5) bring-up control; default 0",

        },
        g17p_native_order: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_submit_polls: u32 {
            default: 500,
            description: "G17P (M5) bring-up control; default 500",
        },
        g17p_probe_low_range: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_late_sksm: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_fw_log: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_fw_trace: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_fw_ktrace_verbose: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_fw_ktrace_budget: u32 {
            default: 32,
            description: "G17P (M5) bring-up control; default 32",
        },
        g17p_flushid_fix: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_skip: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_defer_add_kicks: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_clear_fault: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_ctx2_root: u32 {
            default: 3,
            description: "G17P (M5) bring-up control; default 3",
        },
        // NOTE: every `permissions:` line in this block is commented out, so
        // the macro gives each parameter permissions 0. That means NONE of
        // these appear under /sys/module/asahi/parameters -- they can only be
        // set on the insmod command line. Looking for one in sysfs and not
        // finding it does not mean the build lacks it; check
        // `strings asahi.ko | grep parmtype=` instead. This cost a hardware
        // cycle once already.
        g17p_vm_probe_va: u64 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_vdm_dump: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_fault_report: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_progress_watchdog: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_power_hold: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_power_wire: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_idle_selector: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_absolute_pointers: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_status_flag: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_gate_probe: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_ksm_probe: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_header_flags: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_generation_bias: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_bg_prefix: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_fragment_lifecycle: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_eot_bind_zero: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_dump_descriptor: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_mcache_hwsid: u32 {
            default: 24,
            description: "G17P (M5) bring-up control; default 24",
        },
        g17p_render_mcache_mode: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_mcache_selector: u32 {
            default: 64,
            description: "G17P (M5) bring-up control; default 64",
        },
        g17p_render_ksm_mcache: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_no_barrier: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_split_launch: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_native_doorbells: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_native_pool_a_first_record: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_native_fragment_hwpb: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_3d_rce: u32 {
            default: 0xff,
            description: "G17P (M5) bring-up control; default 0xff",
        },
        g17p_seed_work_channel: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_3d_strobe: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_irq_burst: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_slot_burst: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_dump_state: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_dump_hwdata: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_pipe_sweep: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_ta_dm: u32 {
            default: 0xff,
            description: "G17P (M5) bring-up control; default 0xff",
        },
        g17p_render_3d_dm: u32 {
            default: 0xff,
            description: "G17P (M5) bring-up control; default 0xff",
        },
        g17p_fw_trace_classes: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_ksm_cold_arm: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_admit_arm: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_no_install: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_frag_oracle: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_ta_oracle: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_usc_freelist: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_recovery_allow_active_scheduler: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_service_events: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_compute_sksm_entry: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_report_drain: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_report_drain_budget: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        // 2026-09-01 follow-up: descriptor +0x789=0x10 was also hardware-null
        // (same fault-free TA timeout and recovery as the 0x08 baseline).
        g17p_render_native_regs: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_native_bytes: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_tvb: u32 {
            default: 2,
            description: "G17P (M5) bring-up control; default 2",
        },
        g17p_dep_records: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_no_implicit_dep: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_first_submit: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_ta_qid_field: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_render_slot_template: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_seed_ts: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_kick_qos: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_tag16: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_populated_high_root: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_render_sksm: u32 {
            default: 3,
            description: "G17P (M5) bring-up control; default 3",
        },
        g17p_irq_completion: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_ring_normalize: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_repeat_settle_us: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_ring_limit: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_ring_wrap: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_trace_depth: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_completion_stamp: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_completion_stamp_us: u32 {
            default: 500,
            description: "G17P (M5) bring-up control; default 500",
        },
        g17p_completion_trace: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_repeat_context: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_repeat_ring: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_repeat_activation: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_queue_recycle: u32 {
            default: 2,
            description: "G17P (M5) bring-up control; default 2",
        },
        g17p_zero_duration: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_gate_quiesce_ms: u32 {
            default: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
        g17p_recovery_resync: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_abandon_recovery: u32 {
            default: 1,
            description: "G17P (M5) bring-up control; default 1",
        },
        g17p_sksm_mmio: u32 {
            default: 0,
            // permissions: 0,
            description: "G17P (M5) bring-up control; default 0",
        },
    },
}
