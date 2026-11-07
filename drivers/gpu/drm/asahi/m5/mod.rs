// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple M5 (T8142, G17G) GPU support for the 25G76 firmware (RTKit-3255.160.4).
//!
//! The M5 has two GPU coprocessors, GFX and GFX1, as the A18 Pro does. The boot loader (aura)
//! loads their firmware, checks both loaded TEXT images against the 25G76 build and says so
//! in `/chosen/aurora,m5-gpu-firmware-profile`. The firmware interface is HAL300 and differs
//! from the 26A428 and 25G83 builds the shared G17 code runs, so this backend builds its own
//! initialization graph ([`initdata`], [`resources`]) and talks to the firmware over its own
//! channels ([`channel`], [`sksm`], [`completion`]).
//!
//! # Plugging into the shared driver
//!
//! - `ProbeConfig::M5` (`apple,agx-t8142`) calls [`probe`], which starts both coprocessors
//!   ([`live_boot`]) and returns a [`Gpu`] implementing [`crate::gpu::Gpu`].
//! - The UAT is the shared [`crate::mmu::Uat`] with published roots, as on the A18 Pro, plus
//!   [`crate::mmu::M5ExecutionContext`] for application contexts. User VMs carry a status and
//!   a job lifetime, and the backend uses the tracked submission model of `file.rs`.
//! - The UAPI is the shared one. The backend reports `VM_STATUS` (with its mirror) and
//!   `SOFT_FAULTS`, and one hardware command per submission.
//!
//! # Submission
//!
//! One render pair (TA and fragment queues) and one compute queue serve every client: the
//! queues are installed by the first render and handed from client VM to client VM, with
//! the driver's pools aliased at the same addresses in each. A submission is published,
//! then polled to retirement before the ioctl returns, so there is one job in flight.
//! Renders and compute are admitted only within the state the firmware programs have been
//! verified with ([`render`], [`compute`]); anything else is refused with `ENOTSUPP`
//! before it reaches the firmware.
//!
//! # Failures
//!
//! There is no recovery yet. A job that fails or does not retire marks its VM, stops both
//! coprocessors and retains every firmware-visible object until reboot; the device is lost
//! for all clients. The module stays loaded for the same reason.

mod channel;
mod command;
mod completion;
mod compute;
mod drm;
mod fw_msg;
mod initdata;
mod live_boot;
mod mapped;
mod provider;
mod render;
mod render_desc;
mod render_target;
mod resources;
mod sksm;
mod submission;
mod vm_layout;

use kernel::{device::Core, of, platform, prelude::*, sync::Arc};

use crate::{driver::AsahiDrmDevice, regs};

pub(crate) use drm::M5DrmGpu as Gpu;

/// The firmware profile aura publishes in `/chosen/aurora,m5-gpu-firmware-profile` after it
/// checked both loaded TEXT images against the exact 25G76 build.
const FIRMWARE_PROFILE: &[u8] = b"M5-RTKit-3255.160.4.release\0";

/// Starts both firmware instances and returns the GPU, ready for DRM registration.
pub(crate) fn probe(
    pdev: &platform::Device<Core<'_>>,
    dev: &AsahiDrmDevice,
    res: regs::G17Resources,
) -> Result<Arc<Gpu>> {
    let chosen = of::chosen().ok_or(ENODEV)?;
    let profile: KVec<u8> = chosen.get_property(c"aurora,m5-gpu-firmware-profile")?;
    if profile.as_slice() != FIRMWARE_PROFILE {
        dev_err!(pdev.as_ref(), "M5: GPU firmware profile not published or not 25G76\n");
        return Err(ENODEV);
    }
    let id = res.get_g17_id()?;
    let cores: u32 = id.core_masks.iter().map(|mask| mask.count_ones()).sum();
    // M5 GPUs have 10 cores, or 8 with two fused off (MacBook Air).
    if id.family != 0xb || id.num_dies != 1 || !matches!(cores, 8 | 10) {
        dev_err!(pdev.as_ref(), "M5: unexpected GPU family {:#x}, {} dies, {} cores\n",
            id.family, id.num_dies, cores);
        return Err(ENODEV);
    }
    // Before the session: a CLPC that has not probed yet defers the GPU.
    let clpc = drm::M5ClpcInput::new(pdev.as_ref())?;
    let runtime = live_boot::M5Runtime::start_application(pdev, dev.into(), res)?;
    let runtime = Arc::pin_init(kernel::new_mutex!(runtime), GFP_KERNEL)?;
    Ok(Arc::new(Gpu::new(runtime, id, clpc), GFP_KERNEL)?)
}
