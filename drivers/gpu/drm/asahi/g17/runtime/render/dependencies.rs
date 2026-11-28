// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Both engines preserve the covered render prefix while physical graphs are borrowed.

use crate::g17::{dependency::Frontier, job::Packet, runtime::Registry};
use kernel::{prelude::*, sync::Arc};

impl Registry {
    pub(in crate::g17::runtime) fn render_prefix(
        &self,
        owner: u64,
        packet: &Arc<Packet>,
    ) -> Result<Frontier> {
        self.render_prefix_at(owner, packet, packet.order.wait_through[0])
    }
    pub(in crate::g17::runtime) fn render_prefix_at(
        &self,
        owner: u64,
        packet: &Arc<Packet>,
        prefix: Option<u64>,
    ) -> Result<Frontier> {
        let mut frontier = Frontier::new();
        let compute = packet.engine() == crate::g17::job::Engine::Compute;
        for entry in self.render.entries.iter().flatten() {
            if let Some(pair) = entry.pair.as_ref() {
                pair.collect_dependencies(
                    owner,
                    packet.completion.status(),
                    prefix,
                    &mut frontier,
                    compute,
                )?;
            } else if let Some(dependencies) = entry.borrowed_dependencies.as_ref() {
                dependencies.collect(
                    owner,
                    packet.completion.status(),
                    prefix,
                    &mut frontier,
                    compute,
                )?;
            }
        }
        Ok(frontier)
    }
    pub(in crate::g17::runtime) fn tiling_prefix(
        &self,
        owner: u64,
        packet: &Arc<Packet>,
    ) -> Result<Frontier> {
        let mut frontier = Frontier::new();
        for entry in self.render.entries.iter().flatten() {
            if let Some(pair) = entry.pair.as_ref() {
                pair.collect_tiling_dependencies(
                    owner,
                    packet.completion.status(),
                    packet.order.wait_through[0],
                    &mut frontier,
                )?;
            } else if let Some(dependencies) = entry.borrowed_dependencies.as_ref() {
                dependencies.collect_tiling(
                    owner,
                    packet.completion.status(),
                    packet.order.wait_through[0],
                    &mut frontier,
                )?;
            }
        }
        Ok(frontier)
    }
    /// True when any render pair other than `slot` has live prefix-covered work for
    /// `owner` (including pairs lent out for preparation or heap growth via
    /// `borrowed_dependencies`) across either `wait_through[0]` or
    /// `tiling_wait_through[0]`. Used by `lend_render` so `Pair::own_dependencies`
    /// matches `Registry::render_dependencies` for every direct render, including
    /// staged renders with `stage_prefixes`.
    pub(in crate::g17::runtime) fn has_other_pair_dependencies(
        &self,
        slot: u8,
        owner: u64,
        packet: &Arc<Packet>,
    ) -> Result<bool> {
        let prefixes = [
            packet.order.wait_through[0],
            packet.order.tiling_wait_through[0],
        ];
        if prefixes[0].is_none() && prefixes[1].is_none() {
            return Ok(false);
        }
        let mut frontier = Frontier::new();
        for (index, entry) in self.render.entries.iter().enumerate() {
            if usize::from(slot) == index {
                continue;
            }
            let Some(entry) = entry.as_ref() else {
                continue;
            };
            if entry.reservation.owner != owner {
                continue;
            }
            for prefix in prefixes.into_iter().flatten() {
                if let Some(pair) = entry.pair.as_ref() {
                    pair.collect_dependencies(
                        owner,
                        packet.completion.status(),
                        Some(prefix),
                        &mut frontier,
                        false,
                    )?;
                    if frontier.as_slice().is_empty() {
                        pair.collect_tiling_dependencies(
                            owner,
                            packet.completion.status(),
                            Some(prefix),
                            &mut frontier,
                        )?;
                    }
                } else if let Some(dependencies) = entry.borrowed_dependencies.as_ref() {
                    dependencies.collect(
                        owner,
                        packet.completion.status(),
                        Some(prefix),
                        &mut frontier,
                        false,
                    )?;
                    if frontier.as_slice().is_empty() {
                        dependencies.collect_tiling(
                            owner,
                            packet.completion.status(),
                            Some(prefix),
                            &mut frontier,
                        )?;
                    }
                }
                if !frontier.as_slice().is_empty() {
                    return Ok(true);
                }
            }
        }
        Ok(false)
    }
}
