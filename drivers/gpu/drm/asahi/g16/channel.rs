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
use super::fw::channels::{DevCtl, SubmitRecord, RING_SLOTS, STATE_READ, STATE_WRITE};
use super::fw::initdata::TxLayout;
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
    completion: usize,
    header: usize,
}

impl Ring {
    pub(crate) fn new(
        state: Arc<FwObject>,
        ring: Arc<FwObject>,
        tx: TxLayout,
        control: bool,
    ) -> Self {
        Self {
            state,
            ring,
            completion: tx.completion(),
            header: tx.ring_header(control),
        }
    }

    /// The consumer's read index (state +0x00).
    pub(crate) fn read(&self) -> Result<u32> {
        self.state.r32(STATE_READ)
    }

    /// The completion index in the channel's state block.
    pub(crate) fn completion(&self) -> Result<u32> {
        // Submit rings with a producer header retire at the consumer's read cursor.
        if self.header != 0 {
            self.read()
        } else {
            self.state.r32(self.completion)
        }
    }

    /// The producer's write index, either in state or at the start of the ring.
    pub(crate) fn write(&self) -> Result<u32> {
        if self.header == 0 {
            self.state.r32(STATE_WRITE)
        } else {
            self.ring.r32(0)
        }
    }

    fn set_write(&self, value: u32) -> Result {
        if self.header == 0 {
            self.state.w32(STATE_WRITE, value)
        } else {
            self.ring.w32(0, value)
        }
    }
}

/// Maximum firmware log entries printed per recovery before suppressing excess entries.
pub(crate) const RECOVERY_FWLOG_CAP: usize = 32;

/// The channels and rings.
pub(crate) struct Channels {
    /// The 12 submit channels (TA, FS and compute are used).
    pub(crate) submit: KVec<Ring>,
    pub(crate) devctl: Ring,
    pub(crate) event: Ring,
    /// The nine log sub-rings share one state object (9 x 0x30) and one ring object.
    pub(crate) log: Ring,
    /// Backing payload buffer (`LOG_DATA`, 9 x 256 x 0xd8) indexed by firmware log records.
    log_data: Arc<FwObject>,
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
        tx: TxLayout,
        devctl: Ring,
        event: Ring,
        log: Ring,
        log_data: Arc<FwObject>,
        trace: Ring,
        stats: Ring,
        fwctl: Ring,
    ) -> Result<Self> {
        let mut submit = KVec::with_capacity(rings::CHANNELS as usize, GFP_KERNEL)?;
        for _ in 0..rings::CHANNELS {
            submit.push(
                Ring::new(
                    fw_object(objects, rings::STATE_SIZE, false)?,
                    fw_object(
                        objects,
                        rings::CHANNEL_RING_SIZE + tx.ring_header(false),
                        tx == TxLayout::StateBlock,
                    )?,
                    tx,
                    false,
                ),
                GFP_KERNEL,
            )?;
        }
        Ok(Self {
            submit,
            devctl,
            event,
            log,
            log_data,
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

    /// The channel's producer.
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
            ring.header + producer as usize * rings::CHANNEL_RECORD,
            &record.to_bytes(),
        )?;
        wmb();
        let producer = (producer + 1) % RING_SLOTS;
        ring.set_write(producer)?;
        wmb();
        self.producer[ch] = producer;
        Ok(producer)
    }

    /// Stage the boot NOP (the firmware ABI's consume-only `nop` opcode) before the initdata
    /// root is handed to the firmware.
    pub(crate) fn stage_boot_nop(&mut self, session: &Session, nop: u32) -> Result {
        if self.devctl_producer != 0
            || self.devctl.read()? != 0
            || self.devctl.completion()? != 0
            || self.devctl.write()? != 0
        {
            return Err(super::boot::eproto());
        }
        self.devctl.ring.write(0, &DevCtl::op(nop).to_bytes())?;
        wmb();
        session.set_generation(1)?;
        self.devctl.set_write(1)?;
        wmb();
        self.devctl_producer = 1;
        Ok(())
    }

    /// The one error line for a stop during the boot sequence: why, the device-control indices
    /// and the FirmwareData recovery words. Reads host RAM only, never a GPU register.
    pub(crate) fn log_stop(&self, session: &Session, init: &InitData, why: &str) {
        let ix = |r: Result<u32>| r.unwrap_or(u32::MAX);
        let fd = init.firmware_data();
        let word = |o: usize| init.control.as_ref().map_or(u32::MAX, |c| ix(c.r32(o)));
        dev_err!(
            session.dev,
            "G16 boot stopped: {}: devctl read {} completion {} write {} producer {}; phase {:#x} scheduler {:#x} ready {:#x}\n",
            why,
            ix(self.devctl.read()),
            ix(self.devctl.completion()),
            ix(self.devctl.write()),
            self.devctl_producer,
            word(fd.phase),
            word(fd.scheduler),
            ix(init.readiness())
        );
    }

    /// Drains one firmware log sub-ring. Outside recovery (`budget` is `None`), advances the
    /// consumer directly without touching the payload pages. During recovery (`Some(remaining)`),
    /// prints up to the last `*remaining` entries and counts any older/excess entries suppressed.
    #[inline(never)]
    fn drain_log_ring(
        &self,
        dev: &kernel::device::Device,
        thread: usize,
        budget: Option<&mut usize>,
        suppressed: &mut usize,
    ) -> Result {
        let offset = thread * 0x30;
        let producer = self.log.state.r32(offset + STATE_WRITE)?;
        let mut consumer = self.log.state.r32(offset + STATE_READ)?;
        if producer >= RING_SLOTS || consumer >= RING_SLOTS {
            return Err(super::boot::eproto());
        }
        if consumer == producer {
            return Ok(());
        }
        if let Some(remaining) = budget {
            rmb();
            let pending = producer.wrapping_sub(consumer) % RING_SLOTS;
            if (pending as usize) > *remaining {
                let skip = pending as usize - *remaining;
                *suppressed = suppressed.saturating_add(skip);
                consumer = (consumer + skip as u32) % RING_SLOTS;
            }
            while consumer != producer {
                let entry_off =
                    (thread * RING_SLOTS as usize + consumer as usize) * rings::EVENT_RECORD;
                let kind = self.log.ring.r32(entry_off)?;
                let slot = self.log.ring.r32(entry_off + 8)?;
                let payload_idx = thread * RING_SLOTS as usize + (slot & 255) as usize;
                let payload_off = payload_idx * rings::LOG_PAYLOAD_SIZE;
                let ptype = self.log_data.r32(payload_off)?;
                let seq = self.log_data.r32(payload_off + 4)?;
                let mut text = [0u8; rings::LOG_PAYLOAD_TEXT_SIZE];
                self.log_data
                    .read(payload_off + rings::LOG_PAYLOAD_TEXT_OFFSET, &mut text)?;
                let len = text.iter().position(|&b| b == 0).unwrap_or(text.len());
                let line = core::str::from_utf8(&text[..len])
                    .ok()
                    .map(|s| s.trim_end_matches(['\r', '\n']));
                match line {
                    Some(msg) if !msg.is_empty() && kind == 2 && ptype == 3 => {
                        dev_warn!(
                            dev,
                            "G16: fwlog[{}] seq={} slot={}: {}\n",
                            thread,
                            seq,
                            slot,
                            msg
                        );
                    }
                    Some(msg) if !msg.is_empty() => {
                        dev_warn!(
                            dev,
                            "G16: fwlog[{}] kind={} ptype={} seq={} slot={}: {}\n",
                            thread,
                            kind,
                            ptype,
                            seq,
                            slot,
                            msg
                        );
                    }
                    _ => {
                        dev_warn!(
                            dev,
                            "G16: fwlog[{}] kind={} ptype={} seq={} slot={} (non-text len={})\n",
                            thread,
                            kind,
                            ptype,
                            seq,
                            slot,
                            len
                        );
                    }
                }
                *remaining = remaining.saturating_sub(1);
                consumer = (consumer + 1) % RING_SLOTS;
            }
        }
        self.log.state.w32(offset + STATE_READ, producer)?;
        wmb();
        Ok(())
    }

    /// Drains the event and statistics receive rings, which carry completion notifications and
    /// firmware recovery announcements. Hot retirement passes use this to avoid polling the
    /// auxiliary log and trace rings on every flight; periodic housekeeping drains the rest.
    pub(crate) fn drain_events_and_stats(
        &self,
        dev: &kernel::device::Device,
        events: &mut Events,
    ) -> Result {
        let mut result = events.drain(dev, &self.event);
        let stats = events.drain_statistics(dev, &self.stats);
        if result.is_ok() {
            result = stats;
        }
        result
    }

    /// Drains the nine log sub-rings and the ktrace receive ring. When `fwlog_budget` is `Some`,
    /// prints drained firmware log records up to the remaining budget.
    pub(crate) fn drain_auxiliary_rings_with_log(
        &self,
        dev: &kernel::device::Device,
        mut fwlog_budget: Option<&mut usize>,
    ) -> Result {
        let mut result = Ok(());
        let mut suppressed = 0usize;
        let mut keep = |r: Result| {
            if result.is_ok() {
                result = r;
            }
        };
        for i in 0..rings::LOG_RINGS {
            keep(self.drain_log_ring(dev, i, fwlog_budget.as_deref_mut(), &mut suppressed));
        }
        if suppressed != 0 {
            dev_warn!(
                dev,
                "G16: fwlog: suppressed {} additional entries during recovery\n",
                suppressed
            );
        }
        keep(self.drain_trace(dev));
        result
    }

    /// Drains the nine log sub-rings and the ktrace receive ring silently.
    pub(crate) fn drain_auxiliary_rings(&self, dev: &kernel::device::Device) -> Result {
        self.drain_auxiliary_rings_with_log(dev, None)
    }

    /// Drains every receive ring: events (decoded into `events`), the nine log sub-rings, trace
    /// and stats, whatever another ring reported. The firmware waits while its event ring is
    /// full, so one bad record must never leave a ring undrained. The first error is returned
    /// once all are done.
    pub(crate) fn drain_rings(&self, dev: &kernel::device::Device, events: &mut Events) -> Result {
        let mut result = self.drain_events_and_stats(dev, events);
        let aux = self.drain_auxiliary_rings(dev);
        if result.is_ok() {
            result = aux;
        }
        result
    }

    /// Ktrace has twice the capacity of the other receive rings. A record of another type than
    /// the qualified one (5) is logged and consumed, then reported.
    fn drain_trace(&self, dev: &kernel::device::Device) -> Result {
        let producer = self.trace.write()?;
        let mut consumer = self.trace.read()?;
        if producer >= 512 || consumer >= 512 {
            return Err(super::boot::eproto());
        }
        rmb();
        let mut result = Ok(());
        while consumer != producer {
            let kind = self.trace.ring.r32(consumer as usize * 0x48)?;
            if kind != 5 && result.is_ok() {
                dev_err!(
                    dev,
                    "unexpected M4 firmware trace record type {} at {}\n",
                    kind,
                    consumer
                );
                result = Err(super::boot::eproto());
            }
            consumer = (consumer + 1) % 512;
            self.trace.state.w32(STATE_READ, consumer)?;
            wmb();
        }
        result
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
            channels.log_stop(session, init, "firmware halted before a control was posted");
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
    session.set_generation(next)?;
    channels.devctl.set_write(next)?;
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
            channels.log_stop(session, init, "firmware halted awaiting a control");
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

    /// The latched transport error: an RTKit crash or a refused RTKit buffer. The register
    /// and UAT checks of [`Session::check`] run once per pass, in its callers.
    pub(crate) fn transport_error(&self) -> Option<Error> {
        self.session.transport().err()
    }

    /// Drains the receive rings and decodes events. Called on full housekeeping and idle passes.
    pub(crate) fn drain_rings(&mut self) -> Result {
        let dev = self.session.dev.clone();
        let mut result = self.channels.drain_events_and_stats(&dev, &mut self.events);
        let aux = self.drain_auxiliary_rings();
        if result.is_ok() {
            result = aux;
        }
        result
    }

    /// Drains only the event and statistics rings on the hot retirement path.
    pub(crate) fn drain_events_and_stats(&mut self) -> Result {
        let dev = self.session.dev.clone();
        self.channels.drain_events_and_stats(&dev, &mut self.events)
    }

    /// Drains the auxiliary log and trace rings on periodic housekeeping passes. Prints firmware
    /// log records (bounded per recovery) whenever a firmware recovery is pending or running.
    pub(crate) fn drain_auxiliary_rings(&mut self) -> Result {
        let dev = self.session.dev.clone();
        let log_fwlog = self.recovery.receipt_fault || self.recovery_pending().unwrap_or(false);
        let budget = log_fwlog.then_some(&mut self.recovery.fwlog_remaining);
        self.channels.drain_auxiliary_rings_with_log(&dev, budget)
    }
}
