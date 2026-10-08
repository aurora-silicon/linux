// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::prelude::*;
#[path = "g16_queue_layout.rs"]
mod layout;
use layout::{context,info,ring,notifier_list,barrier,client};
use crate::g16_compute::notification_layout as notification;
use crate::{driver, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Context {
    data: Buffer,
}

impl Context {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm) -> Result<Self> {
        let mut data = Buffer::new(dev, vm, context::SIZE)?;
        data.write(context::REGISTRATION, &[255, 255])?;
        data.write(context::ENABLE, &[1])?;
        data.write(context::SENTINEL_33, &[255])?;
        data.write(context::SCHEDULING_WEIGHT, &[2])?;
        Ok(Self { data })
    }
    pub(crate) fn registered(&mut self) -> Result<bool> {
        let mut state = [0; 3];
        self.data.read(context::REGISTRATION, &mut state)?;
        Ok(state[0] != 255 && state[1] != 255 && state[2] == 4)
    }
    pub(crate) fn release_args(&mut self) -> Result<([u8; 4], u64)> {
        if !self.registered()? { return Err(EINVAL); }
        let mut registration = [0u8; 3];
        let mut weight = [0u8; 1];
        self.data.read(context::REGISTRATION, &mut registration)?;
        self.data.read(context::SCHEDULING_WEIGHT, &mut weight)?;
        Ok(([weight[0], registration[0], registration[1], registration[2]], self.data.va()))
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("queue-context", &mut self.data, context::SIZE)
    }
}

pub(crate) struct Queue {
    capacity: u32,
    notification: Buffer,
    threshold: Buffer,
    submitted: u64,
    info: Buffer,
    ring_state: Buffer,
    ring: Buffer,
    notifier: Buffer,
    command: Buffer,
    stamp: Buffer,
    completion: Buffer,
}

impl Queue {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm, context: &Context,
        capacity: u32) -> Result<Self> {
        if !capacity.is_power_of_two() || !(2..=65536).contains(&capacity) { return Err(EINVAL); }
        let mut queue = Self {
            capacity,
            notification: Buffer::new(dev, vm, notification::SIZE)?,
            threshold: Buffer::new(dev, vm, 8)?,
            submitted: 0,
            info: Buffer::new(dev, vm, info::SIZE)?,
            ring_state: Buffer::new(dev, vm, ring::SIZE)?,
            ring: Buffer::new(dev, vm, capacity as usize * ring::ENTRY_SIZE)?,
            notifier: Buffer::new(dev, vm, notifier_list::SIZE)?,
            command: Buffer::new(dev, vm, barrier::SIZE)?,
            stamp: Buffer::new(dev, vm, 8)?,
            completion: Buffer::new(dev, vm, 8)?,
        };
        queue.notification.u64(notification::THRESHOLD, queue.threshold.va())?;
        queue.notification.u32(notification::GENERATION, crate::g16_compute::EVENT_GENERATION)?;
        queue.notification.u32(notification::UNKNOWN_10, notification::QUALIFIED_UNKNOWN_10)?;
        queue.notification.write(notification::SLOT_SENTINELS, &[255; notification::SLOT_SENTINELS_SIZE])?;
        queue.notification.u32(notification::UNKNOWN_114, notification::QUALIFIED_UNKNOWN_114)?;
        queue.info.u64(info::RING_STATE, queue.ring_state.va())?;
        queue.info.u64(info::RING, queue.ring.va())?;
        queue.info.u64(info::NOTIFIER_LIST, queue.notifier.va())?;
        queue.info.u64(info::SCHEDULER_POINTER, queue.info.va() + info::SCHEDULER_STORAGE as u64)?;
        queue.info.u32(info::EVENT_ID, u32::MAX)?;
        queue.info.u32(info::PRIORITY_0, 2)?;
        queue.info.u32(info::PRIORITY_1, 2)?;
        queue.info.u64(info::PRIORITY_MASK, 0xffff000000000000)?;
        queue.info.u32(info::PRIORITY_5, 2)?;
        queue.info.u32(info::UNKNOWN_4C, u32::MAX)?;
        queue.info.u64(info::CONTEXT, context.data.va())?;
        queue.notifier.u64(notifier_list::SELF, queue.notifier.va())?;
        queue.ring_state.u32(ring::CAPACITY, capacity)?;
        queue.command.u32(barrier::OPCODE, barrier::QUALIFIED_OPCODE)?;
        queue.command.u64(barrier::STAMP_0, queue.stamp.va())?;
        queue.command.u64(barrier::STAMP_1, queue.stamp.va())?;
        queue.command.u32(barrier::CONTROL_20, barrier::QUALIFIED_CONTROL)?;
        Ok(queue)
    }

    pub(crate) fn notification(&self) -> crate::g16_compute::Notification {
        crate::g16_compute::Notification { address: self.notification.va(), threshold: self.threshold.va(),
            stamp: self.completion.va(), fw_stamp: self.stamp.va(), event_seq: self.submitted }
    }
    pub(crate) fn completion_stamp(&mut self) -> Result<u32> {
        self.completion.read_u32(0)
    }
    pub(crate) fn firmware_stamp(&mut self) -> Result<u32> {
        self.stamp.read_u32(0)
    }
    pub(crate) fn set_render_context(&mut self, context: u32) -> Result {
        if context >= 64 { return Err(EINVAL); }
        self.notification.u32(notification::CONTEXT, context)
    }
    pub(crate) fn advance_threshold(&mut self) -> Result {
        self.submitted = self.submitted.checked_add(1).ok_or(EOVERFLOW)?;
        self.threshold.u64(0, self.submitted)?;
        Ok(())
    }

    pub(crate) fn prepare_barrier(&mut self) -> Result<u64> {
        if self.ring_state.read_u32(ring::CPU_WRITE)? != 0 { return Err(EBUSY); }
        self.ring.u64(0, self.command.va())?;
        g16_memory::publish();
        self.ring_state.u32(ring::CPU_WRITE, 1)?;
        g16_memory::publish();
        Ok(self.info.va())
    }

    pub(crate) fn prepare_command(&mut self, command: u64) -> Result<u64> {
        if command & 7 != 0 || self.ring_state.read_u32(ring::CPU_WRITE)? != 0 { return Err(EINVAL); }
        self.ring.u64(0, command)?;
        g16_memory::publish();
        self.ring_state.u32(ring::CPU_WRITE, 1)?;
        g16_memory::publish();
        Ok(self.info.va())
    }

    pub(crate) fn append_idle(&mut self, command: u64) -> Result<(u64, u32)> {
        self.append_batch_idle(&[command])
    }
    pub(crate) fn address(&self) -> u64 { self.info.va() }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump, names: [&str; 2]) -> Result {
        dump.buffer(names[0], &mut self.info, info::SIZE)?;
        dump.buffer(names[1], &mut self.ring_state, ring::SIZE)?;
        if names[0] == "queue-compute" {
            dump.buffer("compute-notification", &mut self.notification, notification::SIZE)?;
            dump.buffer("compute-threshold", &mut self.threshold, 8)?;
            dump.buffer("compute-user-stamp", &mut self.completion, 8)?;
            dump.buffer("compute-firmware-stamp", &mut self.stamp, 8)?;
        }
        Ok(())
    }
    pub(crate) fn append_batch_idle(&mut self, commands: &[u64]) -> Result<(u64, u32)> {
        if commands.is_empty() || commands.len() >= self.capacity as usize
            || commands.iter().any(|p| *p & 7 != 0) { return Err(EINVAL); }
        let status = self.status()?;
        let write = status[4];
        if status != [write, write, write, write, write, 0] { return Err(EBUSY); }
        let mut next = write;
        for command in commands {
            self.ring.u64(next as usize * ring::ENTRY_SIZE, *command)?;
            next = (next + 1) & (self.capacity - 1);
        }
        g16_memory::publish();
        self.ring_state.u32(ring::CPU_WRITE, next)?;
        g16_memory::publish();
        Ok((self.info.va(), next))
    }

    pub(crate) fn append_batch(&mut self, commands: &[u64]) -> Result<(u64, u32)> {
        if commands.is_empty() || commands.len() >= self.capacity as usize
            || commands.iter().any(|p| *p & 7 != 0) { return Err(EINVAL); }
        let status = self.status()?;
        let write = status[4];
        let mask = self.capacity - 1;
        for index in &status[..4] {
            if *index >= self.capacity { return Err(EIO); }
            let free = index.wrapping_sub(write).wrapping_sub(1) & mask;
            if (free as usize) < commands.len() { return Err(EBUSY); }
        }
        let mut next = write;
        for command in commands {
            self.ring.u64(next as usize * ring::ENTRY_SIZE, *command)?;
            next = (next + 1) & mask;
        }
        g16_memory::publish();
        self.ring_state.u32(ring::CPU_WRITE, next)?;
        g16_memory::publish();
        Ok((self.info.va(), next))
    }
    pub(crate) fn consumed_through(&self, status: &[u32; 6], next: u32) -> bool {
        let mask = self.capacity - 1;
        status[..4].iter().all(|index| (index.wrapping_sub(next) & mask) < self.capacity / 2)
    }
    pub(crate) fn info_snapshot(&mut self, out: &mut [u32]) -> Result {
        for (i, word) in out.iter_mut().enumerate() { *word = self.info.read_u32(i * 4)?; }
        Ok(())
    }
    pub(crate) fn status(&mut self) -> Result<[u32; 6]> {
        Ok([self.info.read_u32(info::CONSUMERS[0])?, self.info.read_u32(info::CONSUMERS[1])?, self.info.read_u32(info::CONSUMERS[2])?,
            self.ring_state.read_u32(ring::GPU_READ)?, self.ring_state.read_u32(ring::CPU_WRITE)?,
            self.info.read_u32(info::STATE_80)?])
    }
}

pub(crate) type Client = kernel::sync::Arc<kernel::sync::Mutex<ClientNotifications>>;

pub(crate) struct ClientNotifications {
    data: Buffer,
    _binding: mmu::VmBind,
    reserved: [u64; 3],
    submitted: [u64; 3],
    render_counter: u64,
}
#[derive(Clone, Copy)]
pub(crate) struct ReservationCheckpoint {
    reserved: [u64; 3],
    submitted: [u64; 3],
    render_counter: u64,
}
impl ClientNotifications {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        vm: &mmu::Vm) -> Result<Client> {
        let binding = uat.bind(vm)?;
        let mut data = Buffer::new(dev, uat.kernel_vm(), client::SIZE)?;
        for engine in 0..3 {
            let offset = engine * notification::SIZE;
            data.u64(offset + notification::THRESHOLD, data.va() + client::THRESHOLDS[engine] as u64)?;
            data.u32(offset + notification::GENERATION, crate::g16_compute::EVENT_GENERATION)?;
            data.u32(offset + notification::UNKNOWN_10, notification::QUALIFIED_UNKNOWN_10)?;
            data.u32(offset + notification::CONTEXT, binding.slot())?;
            data.write(offset + notification::SLOT_SENTINELS, &[255; notification::SLOT_SENTINELS_SIZE])?;
            data.u32(offset + notification::UNKNOWN_114, notification::QUALIFIED_UNKNOWN_114)?;
        }
        kernel::sync::Arc::pin_init(kernel::new_mutex!(Self {
            data, _binding: binding, reserved: [0; 3], submitted: [0; 3],
            render_counter: 1,
        }), GFP_KERNEL)
    }
    pub(crate) fn notification(&self, engine: usize) -> crate::g16_compute::Notification {
        let base = self.data.va();
        crate::g16_compute::Notification {
            address: base + engine as u64 * notification::SIZE as u64,
            threshold: base + client::THRESHOLDS[engine] as u64,
            fw_stamp: base + client::FW_STAMPS[engine] as u64,
            stamp: base + client::USER_STAMPS[engine] as u64,
            event_seq: self.reserved[engine],
        }
    }
    pub(crate) fn prime_unused_stamps(&mut self, render: bool, previous: u32) -> Result {
        for engine in if render { 0..2 } else { 2..3 } {
            if self.reserved[engine] != 0 || self.submitted[engine] != 0 { continue; }
            self.data.u32(client::FW_STAMPS[engine], previous)?;
            self.data.u32(client::USER_STAMPS[engine], previous)?;
        }
        Ok(())
    }
    pub(crate) fn checkpoint(&self) -> ReservationCheckpoint {
        ReservationCheckpoint { reserved: self.reserved, submitted: self.submitted,
            render_counter: self.render_counter }
    }
    pub(crate) fn restore(&mut self, checkpoint: ReservationCheckpoint) -> Result {
        if self.submitted != checkpoint.submitted { return Err(EBUSY); }
        self.reserved = checkpoint.reserved;
        self.render_counter = checkpoint.render_counter;
        Ok(())
    }
    pub(crate) fn reserve(&mut self, render: bool)
        -> Result<([crate::g16_compute::Notification; 3], u64)> {
        let notifications = core::array::from_fn(|i| self.notification(i));
        let counter = self.render_counter;
        if render {
            let next = self.render_counter.checked_add(2).ok_or(EOVERFLOW)?;
            let tiling = self.reserved[0].checked_add(1).ok_or(EOVERFLOW)?;
            let fragment = self.reserved[1].checked_add(1).ok_or(EOVERFLOW)?;
            self.reserved[0] = tiling;
            self.reserved[1] = fragment;
            self.render_counter = next;
        } else {
            self.reserved[2] = self.reserved[2].checked_add(1).ok_or(EOVERFLOW)?;
        }
        Ok((notifications, counter))
    }
    pub(crate) fn publish(&mut self, render: bool) -> Result {
        for engine in if render { 0..2 } else { 2..3 } {
            self.submitted[engine] = self.submitted[engine].checked_add(1).ok_or(EOVERFLOW)?;
            self.data.u64(client::THRESHOLDS[engine], self.submitted[engine])?;
        }
        Ok(())
    }
    pub(crate) fn stamps(&mut self, engine: usize) -> Result<[u32; 2]> {
        Ok([self.data.read_u32(client::FW_STAMPS[engine])?,
            self.data.read_u32(client::USER_STAMPS[engine])?])
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("client-notifications", &mut self.data, client::SIZE)
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_render_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("render-client-notifications", &mut self.data, client::SIZE)
    }
}
