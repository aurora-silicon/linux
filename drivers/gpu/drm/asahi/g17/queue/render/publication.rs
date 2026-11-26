// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Preflighted item-ring publications. Each lane retains an independent cursor
//! and omits its configuration record only when its durable identity matches.
//!
//! One decision per publication ([`Firmware`]) says whether the firmware has work to do before
//! the render's kicks run: a configuration or lead item, a first record, a class, context or
//! identity change, a new recovery generation, a replay, or a queue with no render in flight
//! (whose tiling kick takes the entry signal). The decision is made once, before the kicks are
//! encoded, and everything that involves the firmware follows from it: the tiling kick's entry
//! gate, each lane's signal item (and so its cursor advance), the outer records and the channel
//! notification. A render that needs none of it adds no firmware items: its two host-port kicks
//! are its whole announcement.

use super::{memory, retirement::Window};
use crate::g17::fw::{
    m5,
    kick::KickTimestamp,
    queue::{DataMaster, KickAnnounce, QueueConfig, ITEM_RING_ENTRIES},
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

/// Why a publication needs firmware work before its kicks: its first failed warm condition.
#[cfg_attr(not(CONFIG_KUNIT = "y"), allow(dead_code))]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Reason {
    /// The firmware interface has no host-port kick ring (G17P), uses record-announced
    /// forms (J813), or has not qualified warm host-port-only announcements.
    Records,
    /// The ring carries no first record of the pair yet.
    First,
    /// The pair's registration is of another class, or is installed by this publication.
    Class,
    /// A queue configuration or lead item is in this publication.
    Config,
    /// A context or identity update rides on this publication.
    Update,
    /// No outer publication of this pair in the current recovery generation.
    Epoch,
    /// A republished (replayed) group.
    Replay,
    /// No earlier render on this pair is still live in hardware (its fragment stamp or pass
    /// end timestamp is already written, or none is active): the tiling kick takes the entry
    /// signal, which the firmware gives once it has processed the signal item.
    Unretained,
}

/// The entry gate of an M5 tiling kick: event 254, which the firmware clears in the kick once
/// it has processed the render's signal item (and the configuration items before it).
pub(crate) const ENTRY_GATE: [u64; 4] = [0, 0, 0, 1u64 << (m5::compute::ENTRY_GATE_EVENT - 192)];

/// What a publication's warm conditions see.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Warmth {
    /// This priority class's ring already carries the pair's first records.
    pub(crate) ring_started: bool,
    /// The pair's registration is of this class and is not installed by this publication.
    pub(crate) registered: bool,
    /// A queue configuration record or a lead item is part of this publication.
    pub(crate) config: bool,
    /// A context or identity update, or a retirement of an unpublished binding, rides on it.
    pub(crate) update: bool,
    /// The pair's last outer record was published in the current recovery generation.
    pub(crate) channel_current: bool,
    /// The command is a republished (replayed) group.
    pub(crate) replay: bool,
    /// No earlier render on this pair is still live in hardware (its fragment stamp or
    /// pass-end timestamp is already written, or none is active), so the tiling kick takes
    /// the entry signal and a doorbell.
    pub(crate) unretained: bool,
}

/// None: the publication is warm. Otherwise the first condition that needs firmware work.
pub(crate) fn outer_reason(warmth: &Warmth) -> Option<Reason> {
    if !warmth.ring_started {
        Some(Reason::First)
    } else if !warmth.registered {
        Some(Reason::Class)
    } else if warmth.config {
        Some(Reason::Config)
    } else if warmth.update {
        Some(Reason::Update)
    } else if !warmth.channel_current {
        Some(Reason::Epoch)
    } else if warmth.replay {
        Some(Reason::Replay)
    } else if warmth.unretained {
        Some(Reason::Unretained)
    } else {
        None
    }
}

/// The firmware work a render publication needs before its kicks run, decided once.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Firmware {
    /// None: the kicks alone announce the render. No entry gate, no items, no cursor
    /// advance, no outer records, no notification.
    None,
    /// Outer records and a notification announce the render without advancing the item
    /// rings (used on `host_only_render_kicks` boards that do not run warm host-port kicks,
    /// such as G17C): no entry gate, no items, outer records and a notification follow.
    RecordsOnly,
    /// The firmware has work first, for the reason given: the tiling kick waits on its entry
    /// gate, each lane carries its items and a signal item, and outer records and a
    /// notification announce them.
    Needed(Reason),
}

impl Firmware {
    /// The decision for a publication whose warm conditions are `warmth`.
    pub(crate) fn decide(warmth: &Warmth) -> Self {
        match outer_reason(warmth) {
            Some(reason) => Self::Needed(reason),
            None => Self::None,
        }
    }
    /// Whether this publication is announced by host-port kicks alone (no outer records or
    /// doorbell unless the post-kick re-check falls back).
    pub(crate) fn warm(self) -> bool {
        self == Self::None
    }
    /// The M5 tiling kick's event mask.
    pub(crate) fn ta_event_mask(self) -> [u64; 4] {
        match self {
            Self::Needed(_) => ENTRY_GATE,
            Self::None | Self::RecordsOnly => [0; 4],
        }
    }
    /// How many items a lane with a configuration record (`present`) and a lead item (`lead`)
    /// carries: those, and its signal item (in the G17P form also the descriptor), or none at
    /// all. A configuration or lead item without item-ring work is a contradiction (EINVAL).
    pub(crate) fn lane_items(self, present: bool, lead: bool, m5: bool) -> Result<u32> {
        match self {
            Self::None | Self::RecordsOnly if present || lead || !m5 => Err(EINVAL),
            Self::None | Self::RecordsOnly => Ok(0),
            Self::Needed(_) => Ok(u32::from(present) + u32::from(lead) + if m5 { 1 } else { 2 }),
        }
    }
}

/// The kick announcement record: the G17P one, or the M5 notification.
enum Announcement {
    Neo(KickAnnounce),
    M5(m5::queue::Notification),
}

pub(super) struct Lane<'a> {
    items: [&'a AtomicU64; 4],
    producer: &'a AtomicU32,
    values: [u64; 4],
    config: QueueConfig,
    config_destination: *mut u64,
    config_present: bool,
    announce: Announcement,
    announce_destination: *mut u8,
    prefix: usize,
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
        firmware: Firmware,
    ) -> Result<Self> {
        // macOS 27.0 on the J813 (common 26A428 interface) publishes the Neo form: its
        // TA, 3D and compute item rings carry [configuration], descriptor, KickAnnounce (tag 0xe)
        // records, never the M5 Pro's descriptor-less tag-16 notification.
        let j813 = crate::g17::J813_FORMS.load(core::sync::atomic::Ordering::Relaxed);
        let m5 = graph.is_m5() && !j813;
        // M5 items carry no descriptor: [lead], [configuration], notification; or none.
        let count = firmware.lane_items(present, lead, m5)?;
        let window = if count == 0 {
            Window::empty(base, ITEM_RING_ENTRIES)?
        } else {
            Window::new(base, count, ITEM_RING_ENTRIES)?
        };
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
            .pointer(announce_offset, if m5 { size_of::<m5::queue::Notification>() }
                else { size_of::<KickAnnounce>() })?;
        let index =
            |delta: u32| memory::ITEMS[stage] + ((base + delta) % ITEM_RING_ENTRIES) as usize * 8;
        let items = [
            graph.queues.dword(index(0))?,
            graph.queues.dword(index(1))?,
            graph.queues.dword(index(2))?,
            graph.queues.dword(index(3))?,
        ];
        let announce_va = graph.queues.gpu_va() + announce_offset as u64;
        let descriptor = graph.descriptor_va(stage, ordinal);
        // Item order: [lead command], descriptor, [configuration], announcement.
        let mut values = [0; 4];
        let mut n = 0;
        let mut push = |value: u64| {
            values[n] = value;
            n += 1;
        };
        if lead {
            push(graph.config.gpu_va() + (config_offset + memory::LEAD) as u64);
        }
        if j813 && present {
            push(graph.config.gpu_va() + config_offset as u64);
        }
        if !m5 {
            push(descriptor);
        }
        if present && !j813 {
            push(graph.config.gpu_va() + config_offset as u64);
        }
        push(announce_va);
        Ok(Self {
            items,
            producer: graph
                .queues
                .word(memory::POINTERS[stage] + graph.pointers.producer)?,
            values,
            config,
            config_destination,
            config_present: present,
            announce: if m5 {
                Announcement::M5(m5::queue::notification(config.qid.try_into()?, timestamp)?)
            } else {
                Announcement::Neo(KickAnnounce::new(
                    config.qid.try_into()?,
                    if stage == 0 {
                        DataMaster::Tiling
                    } else {
                        DataMaster::Fragment
                    },
                    timestamp,
                    config.priority as u8,
                )?)
            },
            announce_destination,
            prefix: (count as usize).saturating_sub(1),
            prefix_end: (base + count.saturating_sub(1)) % ITEM_RING_ENTRIES,
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
            match &self.announce {
                Announcement::Neo(value) => self.announce_destination
                    .cast::<KickAnnounce>().write_unaligned(*value),
                Announcement::M5(value) => self.announce_destination
                    .cast::<m5::queue::Notification>().write_unaligned(*value),
            }
        }
    }
    /// Fill the retained records without advancing the firmware-visible cursor.
    /// M5's host-port transaction must occur between this and publish_kick.
    pub(super) fn prepare(&self) {
        if self.window.is_empty() {
            return;
        }
        self.items(0..self.prefix + 1);
        self.config();
        self.announce();
    }
    pub(super) fn publish_prefix(&self) {
        if self.window.is_empty() {
            return;
        }
        self.items(0..self.prefix);
        self.config();
        fence(Ordering::SeqCst);
        self.producer.store(self.prefix_end, Ordering::Relaxed);
    }
    pub(super) fn publish_kick(&self) {
        if self.window.is_empty() {
            return;
        }
        self.announce();
        self.items(self.prefix..self.prefix + 1);
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
        if self.window.is_empty() {
            return;
        }
        self.items(0..self.prefix + 1);
        self.config();
        self.announce();
        fence(Ordering::SeqCst);
        self.producer.store(self.window.target(), Ordering::Relaxed);
        event_slot.store(next_slot, Ordering::Relaxed);
        inner.store(next_inner, Ordering::Relaxed);
    }
}

#[kernel::macros::kunit_tests(asahi_g17_render_publication)]
mod tests {
    use super::*;

    const WARM: Warmth = Warmth {
        ring_started: true,
        registered: true,
        config: false,
        update: false,
        channel_current: true,
        replay: false,
        unretained: false,
    };

    /// The decision and everything derived from it: the tiling gate, both lanes' items (so
    /// their cursor advance), and whether outer records and a notification follow.
    fn derived(warmth: Warmth, present: [bool; 2], lead: [bool; 2]) -> (Firmware, [u64; 4], [u32; 2]) {
        let firmware = Firmware::decide(&warmth);
        let items = [0, 1].map(|stage| firmware.lane_items(present[stage], lead[stage], true).unwrap());
        (firmware, firmware.ta_event_mask(), items)
    }

    #[test]
    fn a_warm_render_adds_no_firmware_work() {
        let (firmware, mask, items) = derived(WARM, [false; 2], [false; 2]);
        assert_eq!(firmware, Firmware::None);
        assert_eq!(mask, [0; 4]);
        assert_eq!(items, [0, 0]);
        assert!(firmware.warm());
    }

    #[test]
    fn records_only_omits_items_and_entry_gate_but_keeps_outer_announcement() {
        let firmware = Firmware::RecordsOnly;
        assert!(!firmware.warm());
        assert_eq!(firmware.ta_event_mask(), [0; 4]);
        assert_eq!(firmware.lane_items(false, false, true).unwrap(), 0);
        assert!(firmware.lane_items(true, false, true).is_err());
        assert!(firmware.lane_items(false, true, true).is_err());
        assert!(firmware.lane_items(false, false, false).is_err());
    }

    #[test]
    fn each_cold_condition_gates_the_kick_and_adds_one_signal_item_a_lane() {
        for (warmth, reason) in [
            (Warmth { ring_started: false, ..WARM }, Reason::First),
            (Warmth { registered: false, ..WARM }, Reason::Class),
            (Warmth { update: true, ..WARM }, Reason::Update),
            (Warmth { channel_current: false, ..WARM }, Reason::Epoch),
            (Warmth { replay: true, ..WARM }, Reason::Replay),
            (Warmth { unretained: true, ..WARM }, Reason::Unretained),
        ] {
            let (firmware, mask, items) = derived(warmth, [false; 2], [false; 2]);
            assert_eq!(firmware, Firmware::Needed(reason));
            assert_eq!(mask, ENTRY_GATE);
            assert_eq!(items, [1, 1]);
        }
    }

    #[test]
    fn configuration_and_lead_items_ride_on_a_gated_publication() {
        let config = Warmth { config: true, ..WARM };
        assert_eq!(derived(config, [true, false], [false; 2]), (Firmware::Needed(Reason::Config), ENTRY_GATE, [2, 1]));
        assert_eq!(derived(config, [false; 2], [false, true]), (Firmware::Needed(Reason::Config), ENTRY_GATE, [1, 2]));
        assert_eq!(derived(config, [true; 2], [true; 2]), (Firmware::Needed(Reason::Config), ENTRY_GATE, [3, 3]));
    }

    #[test]
    fn a_configuration_without_firmware_work_is_refused() {
        assert!(Firmware::None.lane_items(true, false, true).is_err());
        assert!(Firmware::None.lane_items(false, true, true).is_err());
        // Only an interface with host-port kicks has kick-only renders.
        assert!(Firmware::None.lane_items(false, false, false).is_err());
        assert_eq!(Firmware::Needed(Reason::Records).lane_items(false, false, false).unwrap(), 2);
    }

    #[test]
    fn the_first_cold_condition_names_the_reason() {
        let all = Warmth {
            ring_started: false,
            registered: false,
            config: true,
            update: true,
            channel_current: false,
            replay: true,
            unretained: true,
        };
        assert_eq!(Firmware::decide(&all), Firmware::Needed(Reason::First));
        assert_eq!(Firmware::decide(&Warmth { config: true, replay: true, ..WARM }), Firmware::Needed(Reason::Config));
    }
}
