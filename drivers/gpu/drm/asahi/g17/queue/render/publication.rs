// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Preflighted item-ring publications. Each lane retains an independent cursor
//! and omits its configuration record only when its durable identity matches.

use super::{memory, retirement::Window};
use crate::g17::fw::{
    kick::KickTimestamp,
    queue::{DataMaster, KickAnnounce, KickPredecessor, QueueConfig, ITEM_RING_ENTRIES},
};
use core::sync::atomic::{fence, AtomicU32, AtomicU64, Ordering};
use kernel::prelude::*;

fn identity(mut config: QueueConfig) -> QueueConfig {
    config.install = 0;
    config.context_update = 0;
    config.completion_seed_valid = 0;
    config.completion_seed = 0;
    config.qos_update = 0;
    config
}
pub(in crate::g17::queue) fn required(previous: Option<QueueConfig>, config: QueueConfig) -> bool {
    config.install != 0
        || config.context_update != 0
        || config.completion_seed_valid != 0
        || config.qos_update != 0
        || previous.map(identity) != Some(identity(config))
}

const fn item_layout(present: bool, lead: bool, predecessor: bool) -> (u32, usize) {
    let count = 2 + present as u32 + lead as u32 + predecessor as u32;
    let prefix = count as usize - 1 - predecessor as usize;
    (count, prefix)
}

pub(super) struct Lane<'a> {
    items: [&'a AtomicU64; 5],
    producer: &'a AtomicU32,
    values: [u64; 5],
    config: QueueConfig,
    config_destination: *mut u64,
    config_present: bool,
    announce: KickAnnounce,
    announce_destination: *mut KickAnnounce,
    predecessor: Option<KickPredecessor>,
    predecessor_destination: *mut KickPredecessor,
    prefix: usize,
    item_count: usize,
    prefix_end: u32,
    pub(super) window: Window,
}
impl<'a> Lane<'a> {
    #[expect(
        clippy::too_many_arguments,
        reason = "The publication lane describes independent fields of the firmware wire record."
    )]
    pub(super) fn new(
        graph: &'a memory::Graph,
        stage: usize,
        ordinal: u64,
        base: u32,
        config: QueueConfig,
        present: bool,
        timestamp: KickTimestamp,
        lead: bool,
        predecessor: Option<KickPredecessor>,
    ) -> Result<Self> {
        let (count, prefix) = item_layout(present, lead, predecessor.is_some());
        let window = Window::new(base, count, ITEM_RING_ENTRIES)?;
        let offset = ordinal as usize % memory::RECORDS;
        let config_offset = memory::CONFIG[stage] + offset * memory::CONFIG_STRIDE;
        let announce_offset = memory::ANNOUNCE[stage] + offset * memory::ANNOUNCE_STRIDE;
        let config_destination = graph
            .config
            .pointer(config_offset, size_of::<QueueConfig>())?
            .cast::<u64>();
        if config_destination.align_offset(align_of::<u64>()) != 0 {
            return Err(EINVAL);
        }
        let announce_destination = graph
            .queues
            .pointer(announce_offset, size_of::<KickAnnounce>())?
            .cast::<KickAnnounce>();
        let predecessor_offset = config_offset + memory::PREDECESSOR;
        let predecessor_destination = graph
            .config
            .pointer(predecessor_offset, size_of::<KickPredecessor>())?
            .cast::<KickPredecessor>();
        let index =
            |delta: u32| memory::ITEMS[stage] + ((base + delta) % ITEM_RING_ENTRIES) as usize * 8;
        let items = [
            graph.queues.dword(index(0))?,
            graph.queues.dword(index(1))?,
            graph.queues.dword(index(2))?,
            graph.queues.dword(index(3))?,
            graph.queues.dword(index(4))?,
        ];
        let announce_va = graph.queues.gpu_va() + announce_offset as u64;
        let descriptor = graph.descriptor_va(stage, ordinal);
        // Item order: [lead command], descriptor, [configuration], announcement,
        // [predecessor]. The optional final record is published with the announcement.
        let mut values = [0; 5];
        let mut n = 0;
        let mut push = |value: u64| {
            values[n] = value;
            n += 1;
        };
        if lead {
            push(graph.config.gpu_va() + (config_offset + memory::LEAD) as u64);
        }
        push(descriptor);
        if present {
            push(graph.config.gpu_va() + config_offset as u64);
        }
        push(announce_va);
        if predecessor.is_some() {
            push(graph.config.gpu_va() + predecessor_offset as u64);
        }
        Ok(Self {
            items,
            producer: graph
                .queues
                .word(memory::POINTERS[stage] + graph.pointers.producer)?,
            values,
            config,
            config_destination,
            config_present: present,
            announce: KickAnnounce::new(
                config.qid.try_into()?,
                if stage == 0 {
                    DataMaster::Tiling
                } else {
                    DataMaster::Fragment
                },
                timestamp,
                config.priority as u8,
            )?,
            announce_destination,
            predecessor,
            predecessor_destination,
            prefix,
            item_count: count as usize,
            prefix_end: (base + count - 1 - u32::from(predecessor.is_some())) % ITEM_RING_ENTRIES,
            window,
        })
    }
    fn items(&self, range: core::ops::Range<usize>) {
        for index in range {
            self.items[index].store(self.values[index], Ordering::Relaxed);
        }
    }
    fn config(&self) {
        if !self.config_present {
            return;
        }
        let source = core::ptr::from_ref(&self.config).cast::<u64>();
        for index in 0..size_of::<QueueConfig>() / 8 {
            // SAFETY: QueueConfig is packed integer storage with no padding;
            // every source qword is read unaligned. The destination is a
            // checked, aligned record owned by this unpublished ordinal.
            unsafe {
                self.config_destination
                    .add(index)
                    .write(source.add(index).read_unaligned());
            }
        }
    }
    fn announce(&self) {
        // SAFETY: The full record is inside the retained queue graph and is
        // exclusively owned by this ordinal. Publication follows its copy.
        unsafe {
            self.announce_destination.write_unaligned(self.announce);
        }
    }
    fn predecessor(&self) {
        let Some(predecessor) = self.predecessor else { return };
        // SAFETY: The record is inside the retained per-ordinal configuration
        // slot and no firmware pointer names it before publication.
        unsafe { self.predecessor_destination.write_unaligned(predecessor); }
    }
    pub(super) fn publish_prefix(&self) {
        self.items(0..self.prefix);
        self.config();
        fence(Ordering::SeqCst);
        self.producer.store(self.prefix_end, Ordering::Relaxed);
    }
    pub(super) fn publish_kick(&self) {
        self.announce();
        self.predecessor();
        self.items(self.prefix..self.item_count);
        fence(Ordering::SeqCst);
        self.producer.store(self.window.target(), Ordering::Relaxed);
    }
    pub(super) fn publish_tiling(
        &self,
        event_slot: &AtomicU32,
        next_slot: u32,
        inner: &AtomicU32,
        next_inner: u32,
    ) {
        self.items(0..self.item_count);
        self.config();
        self.announce();
        self.predecessor();
        fence(Ordering::SeqCst);
        self.producer.store(self.window.target(), Ordering::Relaxed);
        event_slot.store(next_slot, Ordering::Relaxed);
        inner.store(next_inner, Ordering::Relaxed);
    }
}

#[kernel::macros::kunit_tests(asahi_g16_per_record_render_publication)]
mod tests {
    use super::item_layout;

    #[test]
    fn default_layout_is_unchanged() {
        for present in [false, true] {
            for lead in [false, true] {
                let old_count = 2 + present as u32 + lead as u32;
                assert_eq!(item_layout(present, lead, false),
                           (old_count, old_count as usize - 1));
            }
        }
    }

    #[test]
    fn predecessor_extends_only_the_kick_suffix() {
        for present in [false, true] {
            for lead in [false, true] {
                let plain = item_layout(present, lead, false);
                let exact = item_layout(present, lead, true);
                assert_eq!(exact.0, plain.0 + 1);
                assert_eq!(exact.1, plain.1);
            }
        }
    }
}
