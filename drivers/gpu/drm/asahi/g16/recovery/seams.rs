// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Failure propagation through the shared Asahi VM registry.

use kernel::prelude::*;

use crate::g16::Firmware;

impl Firmware {
    /// A fail-stop invalidates every live VM, including those without current work.
    pub(crate) fn fail_all_vms(&mut self, error: Error) {
        self.vms.fail_all(error)
    }
}
