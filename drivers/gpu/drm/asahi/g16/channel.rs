// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 submit pipes, the device-control ring, receive rings and doorbells.
//!
//! Twelve submit pairs use 256 x 0x18 records. State cells remain ASC-uncached while
//! submit arrays are cached. The device-control ring is 256 x 0x40; its generation is
//! published through SGX+0xd60000 before its producer and the 0x0084 policy doorbell.
//! Nine log threads and event/statistics receive records use the M4 0x48-byte ABI.

use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta, Instant, Monotonic},
};

use super::boot::Session;
use super::event::Events;
use super::fw::channels::{
    DevCtl, SubmitRecord, RING_SLOTS, STATE_COMPLETION, STATE_READ, STATE_WRITE,
};
use super::hw::{self, rings};
use super::initdata::InitData;
use super::object::{Class, FwObject, Objects, Placement, Request};
use super::regs::{rmb, wmb};
use super::Firmware;

/// A submit channel.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Channel {
    Ta = 0,
    Fs = 1,
    Compute = 2,
}

/// A ring and its state block.
pub(crate) struct Ring {
    pub(crate) state: Arc<FwObject>,
    pub(crate) ring: Arc<FwObject>,
}

impl Ring {
    /// The consumer's read index (state +0x00).
    pub(crate) fn read(&self) -> Result<u32> {
        self.state.r32(STATE_READ)
    }

    /// The completion index (state +0x10).
    pub(crate) fn completion(&self) -> Result<u32> {
        self.state.r32(STATE_COMPLETION)
    }

    /// The producer's write index (state +0x20).
    pub(crate) fn write(&self) -> Result<u32> {
        self.state.r32(STATE_WRITE)
    }
}

/// The channels and rings.
pub(crate) struct Channels {
    /// The 12 submit channels (TA, FS and compute are used).
    pub(crate) submit: KVec<Ring>,
    pub(crate) devctl: Ring,
    pub(crate) event: Ring,
    /// The nine log sub-rings share one state object (9 x 0x30) and one ring object.
    pub(crate) log: Ring,
    pub(crate) trace: Ring,
    pub(crate) stats: Ring,
    /// Allocated for the firmware; the host never uses it.
    _fwctl: Ring,
    /// Host producers of channels 0 TA, 1 FS, 2 compute.
    producer: [u32; 3],
    /// Host producer of the device-control ring.
    devctl_producer: u32,
}

fn fw_object(objects: &mut Objects, size: usize, cached: bool) -> Result<Arc<FwObject>> {
    objects.alloc(Request {
        size,
        class: if cached { Class::FwCached } else { Class::Fw },
        placement: Placement::Anywhere,
        offset: 0,
        client: None,
        lower: None,
        cpu: true,
    })
}

impl Channels {
    /// Attach the six rings from the 31-object graph and allocate the twelve submit pairs.
    /// Their state is uncached; the submit records use the qualified ASC-cached class.
    pub(crate) fn alloc_submit_and_attach(
        objects: &mut Objects,
        devctl: Ring,
        event: Ring,
        log: Ring,
        trace: Ring,
        stats: Ring,
        fwctl: Ring,
    ) -> Result<Self> {
        let mut submit = KVec::with_capacity(rings::CHANNELS as usize, GFP_KERNEL)?;
        for _ in 0..rings::CHANNELS {
            submit.push(
                Ring {
                    state: fw_object(objects, rings::STATE_SIZE, false)?,
                    ring: fw_object(objects, rings::CHANNEL_RING_SIZE, true)?,
                },
                GFP_KERNEL,
            )?;
        }
        Ok(Self {
            submit,
            devctl,
            event,
            log,
            trace,
            stats,
            _fwctl: fwctl,
            producer: [0; 3],
            devctl_producer: 0,
        })
    }

    /// A submit channel's ring.
    pub(crate) fn channel(&self, channel: Channel) -> &Ring {
        &self.submit[channel as usize]
    }

    /// The host producer of `channel`.
    pub(crate) fn producer(&self, channel: Channel) -> u32 {
        self.producer[channel as usize]
    }

    /// The firmware's read index of `channel`.
    pub(crate) fn read(&self, channel: Channel) -> Result<u32> {
        self.channel(channel).read()
    }

    /// The channel's write cell (state +0x20).
    pub(crate) fn write_cell(&self, channel: Channel) -> Result<u32> {
        self.channel(channel).write()
    }

    /// Writes one submit record at the channel's producer and advances it, only if every earlier
    /// record was consumed. Returns the new producer: the read index at which this record has
    /// been consumed.
    pub(crate) fn post(&mut self, channel: Channel, record: &SubmitRecord) -> Result<u32> {
        self.post_inner(channel, record, false)
    }

    /// Like [`Self::post`], but behind unconsumed records while fewer than
    /// [`hw::sizes::AHEAD_RECORDS`] are outstanding.
    #[allow(dead_code)]
    pub(crate) fn post_ahead(&mut self, channel: Channel, record: &SubmitRecord) -> Result<u32> {
        self.post_inner(channel, record, true)
    }

    fn post_inner(&mut self, channel: Channel, record: &SubmitRecord, ahead: bool) -> Result<u32> {
        let ch = channel as usize;
        let producer = self.producer[ch];
        let ring = &self.submit[ch];
        let consumer = ring.read()?;
        if consumer >= RING_SLOTS || producer >= RING_SLOTS {
            return Err(super::boot::eproto());
        }
        // Never over an unconsumed record: behind them only with room (ahead).
        let busy = if ahead {
            producer.wrapping_sub(consumer) % RING_SLOTS >= hw::sizes::AHEAD_RECORDS
        } else {
            consumer != producer
        };
        if busy || ring.write()? != producer {
            return Err(EBUSY);
        }
        ring.ring.write(
            producer as usize * rings::CHANNEL_RECORD,
            &record.to_bytes(),
        )?;
        wmb();
        let producer = (producer + 1) % RING_SLOTS;
        ring.state.w32(STATE_WRITE, producer)?;
        wmb();
        self.producer[ch] = producer;
        Ok(producer)
    }

    /// Stage the boot NOP before the initdata root is handed to the firmware.
    pub(crate) fn stage_boot_nop(&mut self, session: &Session) -> Result {
        if self.devctl_producer != 0
            || self.devctl.read()? != 0
            || self.devctl.completion()? != 0
            || self.devctl.write()? != 0
        {
            return Err(super::boot::eproto());
        }
        self.devctl
            .ring
            .write(0, &DevCtl::op(super::fw::channels::devctl::NOP).to_bytes())?;
        wmb();
        session.regs.set_generation(1)?;
        self.devctl.state.w32(STATE_WRITE, 1)?;
        wmb();
        self.devctl_producer = 1;
        Ok(())
    }

    /// Consumes a receive ring to its firmware producer.
    fn drain_one(ring: &Ring, offset: usize, capacity: u32) -> Result {
        let producer = ring.state.r32(offset + STATE_WRITE)?;
        let consumer = ring.state.r32(offset + STATE_READ)?;
        if producer >= capacity || consumer >= capacity {
            return Err(super::boot::eproto());
        }
        if consumer != producer {
            ring.state.w32(offset + STATE_READ, producer)?;
            wmb();
        }
        Ok(())
    }

    /// Drains every receive ring: events (decoded into `events`), the nine log sub-rings, trace
    /// and stats.
    pub(crate) fn drain_rings(&self, dev: &kernel::device::Device, events: &mut Events) -> Result {
        events.drain(dev, &self.event)?;
        for i in 0..rings::LOG_RINGS {
            Self::drain_one(&self.log, i * 0x30, 256)?;
        }
        // Ktrace has twice the capacity of the other receive rings.
        let producer = self.trace.write()?;
        let mut consumer = self.trace.read()?;
        if producer >= 512 || consumer >= 512 {
            return Err(super::boot::eproto());
        }
        rmb();
        while consumer != producer {
            if self.trace.ring.r32(consumer as usize * 0x48)? != 5 {
                return Err(super::boot::eproto());
            }
            consumer = (consumer + 1) % 512;
            self.trace.state.w32(STATE_READ, consumer)?;
            wmb();
        }
        events.drain_statistics(dev, &self.stats)
    }
}

/// Posts one 0x40-byte device-control record (EP 0x21 `0x0084000000000011`) behind the earlier
/// ones (one posted without waiting may still be pending: wait for its consumption first) and,
/// with `wait`, waits for its consumption and completion. A firmware halted for recovery consumes
/// nothing until the host's resume: EBUSY then, so the caller can answer the halt instead of
/// timing out. Callers serialize (the device lock once the backend runs).
pub(crate) fn devctl_submit(
    session: &mut Session,
    channels: &mut Channels,
    events: &mut Events,
    init: &InitData,
    record: &DevCtl,
    wait: bool,
) -> Result {
    let dev = session.dev.clone();
    let producer = channels.devctl_producer;
    let next = (producer + 1) % RING_SLOTS;
    let start = Instant::<Monotonic>::now();
    let expired = || start.elapsed() > Delta::from_millis(2000);
    let opcode = record.opcode();
    while channels.devctl.read()? != producer {
        channels.drain_rings(&dev, events)?;
        session.check()?;
        if init.halted()? {
            return Err(EBUSY);
        }
        if expired() {
            dev_err!(
                dev,
                "control {:#x}: earlier record not consumed (read={} producer={})\n",
                opcode,
                channels.devctl.read()?,
                producer
            );
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
    channels
        .devctl
        .ring
        .write(producer as usize * rings::DEVCTL_RECORD, &record.to_bytes())?;
    wmb();
    session.regs.set_generation(next)?;
    channels.devctl.state.w32(STATE_WRITE, next)?;
    wmb();
    channels.devctl_producer = next;
    session.send(hw::ep::DOORBELL, hw::engine::DEVCTL_DOORBELL)?;
    if !wait {
        return Ok(());
    }
    loop {
        channels.drain_rings(&dev, events)?;
        session.check()?;
        if channels.devctl.read()? == next && channels.devctl.completion()? == next {
            return Ok(());
        }
        if init.halted()? {
            return Err(EBUSY);
        }
        if expired() {
            dev_err!(
                dev,
                "control {:#x}: read={} completion={} producer={}\n",
                opcode,
                channels.devctl.read()?,
                channels.devctl.completion()?,
                next
            );
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
}

impl Firmware {
    /// Posts a device-control record; with `wait` waits for its consumption and completion,
    /// otherwise returns once posted. EBUSY while the firmware is halted; the transport error if
    /// the session failed.
    #[allow(dead_code)]
    pub(crate) fn devctl(&mut self, record: &DevCtl, wait: bool) -> Result {
        devctl_submit(
            &mut self.session,
            &mut self.channels,
            &mut self.events,
            &self.init,
            record,
            wait,
        )
    }

    /// The latched transport error: an RTKit crash or a refused RTKit buffer.
    pub(crate) fn transport_error(&self) -> Option<Error> {
        self.session.check().err()
    }

    /// Drains the receive rings and decodes events. Called first in every poll pass.
    pub(crate) fn drain_rings(&mut self) -> Result {
        let dev = self.session.dev.clone();
        self.channels.drain_rings(&dev, &mut self.events)
    }
}
