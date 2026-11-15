// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module parameters of the G18 backend.
//!
//! They are registered exactly as `kernel::module!` registers the driver's
//! own parameters (a `__param` entry with `perm` 0 and the `parmtype`/`parm`
//! modinfo strings, prefixed by `aurora.` when built in), but only when the
//! backend is built, so other configurations keep their parameter set. The
//! names, types, defaults and descriptions are those of the J873 kernel.

#![allow(non_upper_case_globals)]

use kernel::module_param::{ModuleParamAccess, PARAM_OPS_U32, PARAM_OPS_U64};

/// Copy `bytes` into an array of its exact length, for a `.modinfo` string.
const fn modinfo<const N: usize>(bytes: &[u8]) -> [u8; N] {
    let mut out = [0; N];
    let mut i = 0;
    while i < N {
        out[i] = bytes[i];
        i += 1;
    }
    out
}

macro_rules! g18_param {
    ($name:ident: $ty:ident = $default:expr, $ops:ident, $description:literal) => {
        pub(crate) static $name: ModuleParamAccess<$ty> = ModuleParamAccess::new($default);

        const _: () = {
            #[link_section = "__param"]
            #[used(compiler)]
            static PARAM: kernel::module_param::KernelParam =
                kernel::module_param::KernelParam::new(kernel::bindings::kernel_param {
                    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
                        kernel::c_str!(stringify!($name))
                    } else {
                        kernel::c_str!(concat!("aurora.", stringify!($name)))
                    }),
                    // `__this_module` is constructed by the kernel at load time and is not
                    // freed until the module is unloaded. A raw address avoids a reference to
                    // the mutable extern static.
                    #[cfg(MODULE)]
                    mod_: &raw mut kernel::bindings::__this_module,
                    #[cfg(not(MODULE))]
                    mod_: core::ptr::null_mut(),
                    ops: core::ptr::from_ref(&$ops),
                    perm: 0, // Will not appear in sysfs
                    level: -1,
                    flags: 0,
                    __bindgen_anon_1: kernel::bindings::kernel_param__bindgen_ty_1 {
                        arg: $name.as_void_ptr(),
                    },
                });

            #[cfg(MODULE)]
            const TYPE: &[u8] =
                concat!("parmtype=", stringify!($name), ":", stringify!($ty), "\0").as_bytes();
            #[cfg(not(MODULE))]
            const TYPE: &[u8] =
                concat!("aurora.parmtype=", stringify!($name), ":", stringify!($ty), "\0")
                    .as_bytes();
            #[cfg_attr(not(target_os = "macos"), link_section = ".modinfo")]
            #[used(compiler)]
            static TYPE_INFO: [u8; TYPE.len()] = modinfo(TYPE);

            #[cfg(MODULE)]
            const TEXT: &[u8] =
                concat!("parm=", stringify!($name), ":", $description, "\0").as_bytes();
            #[cfg(not(MODULE))]
            const TEXT: &[u8] =
                concat!("aurora.parm=", stringify!($name), ":", $description, "\0").as_bytes();
            #[cfg_attr(not(target_os = "macos"), link_section = ".modinfo")]
            #[used(compiler)]
            static TEXT_INFO: [u8; TEXT.len()] = modinfo(TEXT);
        };
    };
}

g18_param!(g18_initdata_test: u32 = 10, PARAM_OPS_U32,
    "T8152/J873 G18G bring-up stage. 0 refuses before MMIO/DMA. 1=read-only powered-resource probe. 2=RTKit. 3=initdata ACKs. 4=openings, idle policy, 60 s observation. 5=empty queue. 6=queue plus scheduler binding. 7=owned empty UMA pool binding. 8=compute diagnostic. 9=H1 render replay. 10=firmware plus application DRM render node. Stages 5/6/7 then observe without GPU jobs. Firmware stages require prior power, loader receipt and apple_mailbox.ascwrap_v8_cpu_control=1; all owners remain retained until reboot. 90=SGX power-on; 92=GFX1 power-on; 91=GFX power-on; 93=idle requests. Power stages are never implied. Mutating stages require user-authorized bring-up. Other values refused");
g18_param!(g18_compute_variant: u32 = 0, PARAM_OPS_U32,
    "Stage 8 compute diagnostic. 0=captured compute-add. 1=empty stream: captured barrier and terminate replace the launch; all output must remain poisoned. 2=64x64 compute triangle using a separately verified private shader patch; no TA/3D or Mesa. Variants 1 and 2 require g18_initdata_test=8. Other values are refused");
g18_param!(g18_compute_owner_test: u32 = 0, PARAM_OPS_U32,
    "J873 allocation-only E2E before firmware publication: 1 verifies shared-compute backing, guarded directory growth, allocator rollback and process aliases. Never submits GPU work. Other nonzero values refused");
g18_param!(g18_render_variant: u32 = 0, PARAM_OPS_U32,
    "Stage 9 H1 diagnostic: 0=colour, 2=terminated VDM plus background clear. Other variants refused before MMIO. One asynchronous worker pass, captured/M5 H1 policy, all owners retained; see the H1 render-runtime notes");
g18_param!(g18_render_profile: u32 = 1, PARAM_OPS_U32,
    "Stage 9 G18GFX01 v2 profile: 1=colour-a, 2=colour-b, 4=fragment-witness (colour only). Gears profile3 is offline only and refused by H1. Every other stage, including the default stage10, requires 1");
g18_param!(g18_admit_compute: u32 = 1, PARAM_OPS_U32,
    "G18 DRM adapter: 1 admits single-dispatch compute commands (K1). 0 refuses them");
g18_param!(g18_admit_msaa4: u32 = 1, PARAM_OPS_U32,
    "G18 DRM adapter (g18_initdata_test=10 only): 1 (default) admits exactly the native 4x MSAA render (4 samples, 32x16 utiles, sample positions 0xeaa26e26, 8-byte samples, one layer, no depth/stencil) with its native 4x command fields; Mesa needs AGX_G18_MSAA4=1 to submit it. 0 refuses every multisampled render");
g18_param!(g18_admit_graphics_scratch: u32 = 1, PARAM_OPS_U32,
    "G18 DRM adapter: 1 (default) advertises DRM_ASAHI_FEATURE_G18_GRAPHICS_SCRATCH and admits single-sample renders carrying the G18 render resource suffix; the kernel derives both TA/3D private-memory launch pairs and selects the native ideal or minimum result that fits the owned render pool. 0 refuses the suffix");
g18_param!(g18_usc_statistics: u32 = 0, PARAM_OPS_U32,
    "Stage 9 colour fixture only: 1 enables owned TA/3D SMAPP statistics pages. Raw memory statistics, not shader invocation counts. Default 0");
g18_param!(g18_sma_minimum: u32 = 1, PARAM_OPS_U32,
    "Stage9/profile4 or stage10/profile1, variant0: 1 probes fixed native minimum TA/fragment resource words with unchanged owned backing. Default 1");
g18_param!(g18_graphics_stall_trace: u32 = 0, PARAM_OPS_U32,
    "Stage9/profile4 or stage10/profile1, variant0: 1 enables firmware category9 diagnostics and the native TA mode2 progress sampler. No timeout changes. Default 0");
g18_param!(g18_kick_log: u32 = 0, PARAM_OPS_U32,
    "Stage10 application DRM log volume: 0 (default) omits the per-kick, frame-pass and per-job resource lines (G18_APP_KICK, G18_APP_FRAME_PASS, G18_APP_COMPUTE_KNOBS, G18_COMPUTE_*, G18_SHARED_COMPUTE_*, G18 UMA/compute-context lines, G18_POOL_DESCRIPTOR_CURSOR, role-0 type-1 G18_EVENT) once G18_DRM_READY is logged; 1 prints them as before. Startup, fault, recovery, refusal, wait, stall, admission and sampled summary lines are always printed");
g18_param!(g18_vdm_high_alias: u32 = 0, PARAM_OPS_U32,
    "Stage9/profile4/variant0 with minimum SMA and stall tracing: 1 fetches unchanged native VDM bytes through the Gallium-address alias. Diagnostic only. Default 0");
g18_param!(g18_ppp_high_alias: u32 = 0, PARAM_OPS_U32,
    "With the stage9 high-VDM control: 1 aliases unchanged native PPP pages into the Gallium range and relocates only their VDM pointers. Diagnostic only. Default 0");
g18_param!(g18_sma_ideal: u32 = 0, PARAM_OPS_U32,
    "Stage9 variant0 profile4 only: 1 probes native ideal graphics resource policy with 40MiB backing and precommitted directory growth. Default 0");
g18_param!(g18_native_barrier: u32 = 0, PARAM_OPS_U32,
    "Stage9 variant0 profile4 with ideal SMA only: 1 probes the native same-group TA-to-3D barrier kind3. Default 0");
g18_param!(g17p_simplefb_iova: u64 = 0, PARAM_OPS_U64,
    "Diagnostic simplefb GPU address override; 0 disables it");
g18_param!(g17p_fw_log: u32 = 0, PARAM_OPS_U32,
    "Enable the GPU firmware log ring");
g18_param!(g17p_frg_hwdsid_clear: u32 = 0, PARAM_OPS_U32,
    "Diagnostic: 1 writes FRG descriptor +0x222d as 0 (native light captures); 0 preserves 1");
g18_param!(g17p_trace_depth: u32 = 0, PARAM_OPS_U32,
    "Trace the first N compute submissions per queue; 0 disables success-path tracing");
g18_param!(g17p_completion_trace: u32 = 0, PARAM_OPS_U32,
    "Completion logging level: 0 off, 1 summary, 2 detailed");
