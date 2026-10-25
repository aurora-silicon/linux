// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod alloc;
mod buffer;
mod channel;
mod cleanup;
mod debug;
mod driver;
mod event;
mod file;
mod float;
mod fwid;
mod fw;
mod gem;
mod g17;
#[cfg(CONFIG_DRM_ASAHI_G16)]
mod g16;
mod gpu;
mod hw;
mod initdata;
mod mem;
mod memacct;
mod m3;
mod microseq;
mod mmu;
mod object;
mod pgtable;
mod queue;
mod regs;
mod slotalloc;
mod stats;
mod sysfs_exports;
mod timestamp;
mod util;
#[cfg(CONFIG_DRM_ASAHI_MAPLE_TREE)]
mod vm;
mod workqueue;

use kernel::prelude::*;

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
    // Field drop order unregisters/joins driver producers before queue teardown.
    _g16: Option<Pin<KBox<G16Registration>>>,
    #[pin]
    _driver: kernel::driver::Registration<kernel::platform::Adapter<driver::AsahiPlatformDriver>>,
    _cleanup: cleanup::QueueOwner,
}

impl kernel::InPlaceModule for DriverModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            // Initialize the queue first, before registration can invoke probe.
            _cleanup: cleanup::QueueOwner::new()?,
            _driver <- kernel::driver::Registration::new(
                <Self as kernel::ModuleMetadata>::NAME,
                module,
            ),
            _g16: register_g16(module)?,
        })
    }
}

module! {
    type: DriverModule,
    name: "asahi",
    description: "AGX GPU driver for Apple silicon SoCs",
    license: "Dual MIT/GPL",
    params: {
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
