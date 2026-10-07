// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod alloc;
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
mod m3_soc;
mod m3_init_layout;
mod m3_runtime;
mod m3_completion;
mod m3_timeline;
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
mod t8122_admission;
mod t8122_knobs;
mod t8122_start;
mod g15_selftest;
#[cfg(CONFIG_DEV_COREDUMP)]
mod agx_fault;
mod agx_host_progress;
mod agx_timing_stats;
mod agx_memory_stats;
mod agx_memory;
mod agx_compute;
mod agx_attachments;
mod agx_render;
mod agx_render_state;
mod agx_resources;
mod agx_status;
mod agx_uapi;
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
            description: "M3 render node registration: -1 = auto (on apple,j514s and apple,j516s, where the M3 runtime is validated), 0 = never, 1 = always",
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

        // NOTE: every `permissions:` line in this block is commented out, so
        // the macro gives each parameter permissions 0. That means NONE of
        // these appear under /sys/module/asahi/parameters -- they can only be
        // set on the insmod command line. Looking for one in sysfs and not
        // finding it does not mean the build lacks it; check
        // `strings asahi.ko | grep parmtype=` instead.

    },
}
