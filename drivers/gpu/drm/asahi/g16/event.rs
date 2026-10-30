// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 receive records. Events and statistics use complete 0x48-byte records.
//!
//! Type 1 carries 128 completion bits. The native runtime holds events 0x70
//! (TA), 0x71 (fragment) and 15 (compute). Type 4 and statistics type 0x16
//! announce the firmware's generation-based recovery; neither is a job receipt.

use kernel::{bindings, device, prelude::*};

use super::channel::Ring;
use super::fw::channels::{RING_SLOTS, STATE_READ, STATE_WRITE};
use super::hw::rings;
use super::regs::{mb, rmb, wmb};

pub(crate) const RECORD_WORDS: usize = 18;
pub(crate) const STAGE_EVENTS: [u32; 3] = [0x70, 0x71, 15];
/// Retained only for the allocator's pending-growth container. M4 does not
/// interpret a G15 type-7 record as an allocator request.
pub(crate) const GROW_QUEUE: usize = 8;

/// A complete recovery event copied before the consumer is advanced.
#[derive(Copy, Clone, Default)]
pub(crate) struct ErrorEvent {
    #[allow(dead_code)]
    pub(crate) kind: u32,
    pub(crate) raw: [u32; RECORD_WORDS],
    pub(crate) slot: i32,
}

impl ErrorEvent {
    pub(crate) fn generation(&self) -> u64 {
        u64::from(self.raw[1]) | (u64::from(self.raw[2]) << 32)
    }
}

pub(crate) struct Events {
    pub(crate) compute_events: u32,
    pub(crate) stage_events: u32,
    pub(crate) errors: u32,
    pub(crate) last_error: Option<ErrorEvent>,
    pub(crate) recovery_statistics: u32,
    pub(crate) last_statistics: Option<[u32; RECORD_WORDS]>,
    /// First notification of the pending recovery, for its absolute deadline.
    pub(crate) recovery_started_ns: Option<u64>,
    pub(crate) channel_errors: u32,
    pub(crate) event_ns: [u64; 3],
    pub(crate) grow: KVec<[u32; 14]>,
    #[allow(dead_code)]
    pub(crate) grow_lost: u32,
}

impl Events {
    pub(crate) fn new() -> Self {
        Self {
            compute_events: 0,
            stage_events: 0,
            errors: 0,
            last_error: None,
            recovery_statistics: 0,
            last_statistics: None,
            recovery_started_ns: None,
            channel_errors: 0,
            event_ns: [0; 3],
            grow: KVec::new(),
            grow_lost: 0,
        }
    }

    pub(crate) fn reserve(&mut self) -> Result {
        self.grow.reserve(GROW_QUEUE, GFP_KERNEL)?;
        Ok(())
    }

    fn words(ring: &Ring, index: u32) -> Result<[u32; RECORD_WORDS]> {
        let mut words = [0; RECORD_WORDS];
        let base = index as usize * rings::EVENT_RECORD;
        for (i, word) in words.iter_mut().enumerate() {
            *word = ring.ring.r32(base + i * 4)?;
        }
        Ok(words)
    }

    fn indices(ring: &Ring) -> Result<(u32, u32)> {
        let producer = ring.state.r32(STATE_WRITE)?;
        let consumer = ring.state.r32(STATE_READ)?;
        if producer >= RING_SLOTS || consumer >= RING_SLOTS {
            return Err(super::boot::eproto());
        }
        rmb();
        Ok((consumer, producer))
    }

    /// Decode notifications, copying every record before returning its slot.
    pub(crate) fn drain(&mut self, dev: &device::Device, ring: &Ring) -> Result {
        let (mut read, producer) = Self::indices(ring)?;
        // SAFETY: ktime_get has no preconditions.
        let now = unsafe { bindings::ktime_get() } as u64;
        let mut error = None;
        while read != producer {
            let words = Self::words(ring, read)?;
            match words[0] {
                1 => {
                    for (stage, event) in STAGE_EVENTS.iter().enumerate() {
                        let word = 1 + *event as usize / 32;
                        if words[word] & (1u32 << (*event % 32)) == 0 {
                            continue;
                        }
                        self.event_ns[stage] = now;
                        self.stage_events = self.stage_events.wrapping_add(1);
                        if stage == 2 {
                            self.compute_events = self.compute_events.wrapping_add(1);
                        }
                    }
                }
                4 => {
                    self.recovery_started_ns.get_or_insert(now);
                    self.last_error = Some(ErrorEvent {
                        kind: 4,
                        raw: words,
                        slot: words[3] as i32,
                    });
                    self.errors = self.errors.wrapping_add(1);
                    dev_warn!(dev, "M4 firmware recovery event: {:08x?}\n", words);
                }
                kind => {
                    // The qualified M4 provider accepts only stage and recovery events.
                    // Do not apply another firmware's context-kill or PB-growth protocol.
                    dev_err!(
                        dev,
                        "unexpected M4 firmware event {}: {:08x?}\n",
                        kind,
                        words
                    );
                    self.channel_errors = self.channel_errors.wrapping_add(1);
                    error = Some(super::boot::eproto());
                }
            }
            read = (read + 1) % RING_SLOTS;
            mb();
            ring.state.w32(STATE_READ, read)?;
            wmb();
        }
        error.map_or(Ok(()), Err)
    }

    /// Preserve recovery statistics instead of silently discarding their notification.
    pub(crate) fn drain_statistics(&mut self, dev: &device::Device, ring: &Ring) -> Result {
        let (mut read, producer) = Self::indices(ring)?;
        while read != producer {
            let words = Self::words(ring, read)?;
            if words[0] == 0x16 {
                // SAFETY: ktime_get has no preconditions.
                let now = unsafe { bindings::ktime_get() } as u64;
                self.recovery_started_ns.get_or_insert(now);
                self.last_statistics = Some(words);
                self.recovery_statistics = self.recovery_statistics.wrapping_add(1);
                dev_warn!(dev, "M4 firmware recovery statistics: {:08x?}\n", words);
            }
            read = (read + 1) % RING_SLOTS;
            mb();
            ring.state.w32(STATE_READ, read)?;
            wmb();
        }
        Ok(())
    }
}
