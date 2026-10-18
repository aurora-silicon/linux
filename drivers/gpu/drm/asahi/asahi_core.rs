// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod alloc;
mod buffer;
mod channel;
mod cleanup;
#[cfg(CONFIG_DEV_COREDUMP)]
mod crashdump;
mod debug;
mod driver;
mod event;
mod file;
mod float;
mod fw;
mod gem;
mod g17;
#[cfg(CONFIG_DRM_ASAHI_G15)]
mod g15;
mod gpu;
mod hw;
mod initdata;
mod mem;
mod microseq;
mod mmu;
mod object;
mod pgtable;
mod queue;
mod regs;
mod slotalloc;
mod stats;
mod sysfs_exports;
mod util;
#[cfg(CONFIG_DRM_ASAHI_MAPLE_TREE)]
mod vm;
mod workqueue;

use kernel::prelude::*;

/// The G15 platform driver, registered separately so that it has no module alias.
#[cfg(CONFIG_DRM_ASAHI_G15)]
type G15Registration = kernel::driver::Registration<kernel::platform::Adapter<g15::Driver>>;
#[cfg(not(CONFIG_DRM_ASAHI_G15))]
type G15Registration = ();

/// Registers the G15 platform driver when it is configured.
fn register_g15(module: &'static kernel::ThisModule) -> Result<Option<Pin<KBox<G15Registration>>>> {
    #[cfg(CONFIG_DRM_ASAHI_G15)]
    {
        Ok(Some(KBox::pin_init(
            kernel::driver::Registration::new(g15::DRIVER_NAME, module),
            GFP_KERNEL,
        )?))
    }
    #[cfg(not(CONFIG_DRM_ASAHI_G15))]
    {
        let _ = module;
        Ok(None)
    }
}

#[pin_data]
struct DriverModule {
    // Field drop order unregisters/joins driver producers before queue teardown.
    _g15: Option<Pin<KBox<G15Registration>>>,
    #[pin]
    _driver: kernel::driver::Registration<kernel::platform::Adapter<driver::AsahiDriver>>,
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
            _g15: register_g15(module)?,
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
            default: 2500,
            description: "G15: firmware idle standby time, us (Globals +0x7a8, 100..100000)",
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
        g15_early_tiling: u32 {
            default: 0,
            description: "G15: advertise FRAGMENT_BARRIERS, mapped to early tiling (slower on 2419)",
        },
        g15_accept_text_prefix: u64 {
            default: 0,
            description: "G15: admit an unpinned GPU firmware build whose TEXT SHA-256 starts with these 8 bytes (bring-up only)",
        },
    },
}
