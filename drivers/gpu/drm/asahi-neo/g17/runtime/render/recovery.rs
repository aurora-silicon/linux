// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Recovery targets for retained render graphs while firmware is halted.

use crate::g17::runtime::Registry;
use kernel::prelude::*;

impl Registry {
    pub(in crate::g17::runtime) fn render_recovery_diagnostics(
        &self,
        qid: u8,
    ) -> Option<([u8; 2], Option<u64>, u32, u8)> {
        let pair = self.render.entries.iter().flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .find(|pair| pair.qids().contains(&qid))?;
        Some((pair.qids(), pair.owner(), pair.context().id(), pair.context().generation()))
    }

    pub(in crate::g17::runtime) fn render_pass_started(&self, qid: u8, stamp: u64) -> Option<bool> {
        self.render.entries.iter().flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .find(|pair| pair.qids().contains(&qid))?
            .pass_started(stamp)
    }

    pub(in crate::g17::runtime) fn publish_render_recovery_targets(&self) -> Option<Error> {
        let mut failure = None;
        for pair in self.render.entries.iter().flatten()
            .filter_map(|entry| entry.pair.as_ref())
        {
            if let Err(error) = pair.publish_recovery_targets() {
                failure = failure.or(Some(error));
            }
        }
        failure
    }
}
