// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::{m3_compute_layout as command, m3_init_layout::{Error, Region}, m3_queue_layout::FirmwareVa};
pub(crate) const START: usize = 0;
pub(crate) const TIMESTAMP_START: usize = 0x1bc;
pub(crate) const WAIT: usize = 0x1f8;
pub(crate) const TIMESTAMP_END: usize = 0x1fc;
pub(crate) const FINALIZE: usize = 0x238;
pub(crate) const END: usize = 0x2b4;
pub(crate) const LENGTH: usize = 0x2b8;
pub(crate) const STORAGE: usize = 0x1000;
pub(crate) const GENERATION: usize = START + 0x34;
pub(crate) const ATTACHMENTS: usize = START + 0x54;
pub(crate) const HAS_ATTACHMENTS: usize = FINALIZE + 0x64;
pub(crate) const STAMP_VALUE: usize = FINALIZE + 0x30;
pub(crate) const USER_TIMESTAMP_POINTER: usize = 0x24;
const COMPUTE_INFO2: usize = 0x76c;
const TAIL_POOL: usize = command::TAIL + 0x2e;
const TAIL_STORE_REQUEST: usize = command::TAIL + 0x50;
const TAIL_STORE_COMPLETE: usize = command::TAIL + 0x58;
const TAIL_FLAG2: usize = command::TAIL + 0x70;
const STATS_OFFSET: usize = 0x3740; // retained HardwareData.compute statistics
fn word(out: &mut [u8], off: usize, value: u32) { out[off..off+4].copy_from_slice(&value.to_le_bytes()); }
fn pointer(out: &mut [u8], off: usize, value: u64) { out[off..off+8].copy_from_slice(&value.to_le_bytes()); }
pub(crate) struct Sequence {
    pub(crate) command: Region,
    pub(crate) stats: Region,
    pub(crate) notifier: Region,
    pub(crate) queue: FirmwareVa,
    pub(crate) context: u32,
    pub(crate) generation: u32,
    pub(crate) pool: FirmwareVa,
    /// Five distinct firmware-owned scratch buffers. They are not low client GPU pointers.
    pub(crate) scratch: [FirmwareVa; 5],
    pub(crate) fw_stamp: FirmwareVa,
    pub(crate) stamp_value: u32,
}
impl Sequence {
    /// All references and output extents are checked before output mutation.
    /// Fill the entire owned 4-KiB sequence slot, including unused zero tail.
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        if out.len() < STORAGE { return Err(Error::Size); }
        if self.context >= 64 { return Err(Error::Address); }
        let registers = self.command.at(command::REGISTERS, command::REGISTER_COUNT * command::REGISTER_STRIDE)?;
        let compute_info = self.command.at(COMPUTE_INFO2, 0x60)?;
        let command_time = self.command.at(command::TAIL, 8)?;
        let timestamps = self.command.at(command::TAIL + 8, 16)?;
        let end_timestamp = self.command.at(command::TAIL + 16, 8)?;
        let pool_field = self.command.at(TAIL_POOL, 8)?;
        let store_request = self.command.at(TAIL_STORE_REQUEST, 8)?;
        let store_complete = self.command.at(TAIL_STORE_COMPLETE, 8)?;
        let flag = self.command.at(TAIL_FLAG2, 4)?;
        let notifier_slots = self.notifier.at(0xa8, 8)?;
        let stats = self.stats.at(STATS_OFFSET, 8)?;
        let bytes = &mut out[..STORAGE];bytes.fill(0);
        word(bytes, START, 0x0b);
        for (offset, value) in [(0x14,registers),(0x1c,stats),(0x24,self.queue.get()),
            (0x44,compute_info),(0x15c,self.pool.get()),(0x174,self.scratch[0].get()),
            (0x184,self.scratch[1].get()),(0x18c,self.scratch[2].get()),
            (0x194,self.scratch[3].get()),(0x19c,self.scratch[4].get()),
            (0x1a4,flag),(0x1b4,notifier_slots)] { pointer(bytes, START+offset, value); }
        word(bytes, START+0x2c, self.context);
        word(bytes, START+0x30, 1); // retained unk_28
        word(bytes, GENERATION, self.generation);
        word(bytes, START+0x50, 0xc10000); // UUID
        for (start, target, selected) in [(TIMESTAMP_START,0x80000003,timestamps),(TIMESTAMP_END,3,end_timestamp)] {
            word(bytes, start, target);
            for (offset, value) in [(4,command_time),(0xc,timestamps),(0x14,selected),
                (0x1c,self.queue.get()),(0x2c,store_complete)] { pointer(bytes,start+offset,value); }
            word(bytes,start+0x34,0xc10000);
        }
        word(bytes, WAIT, 1); // WaitForIdle2; G15 opcode
        word(bytes, FINALIZE, 0x0c);
        for (offset, value) in [(4,stats),(0xc,self.queue.get()),(0x18,compute_info),
            (0x28,self.fw_stamp.get()),(0x58,pool_field),(0x65,flag),(0x6d,store_request)] {
            pointer(bytes,FINALIZE+offset,value);
        }
        word(bytes,FINALIZE+0x14,self.context);
        word(bytes,FINALIZE+0x24,0xc10000);
        word(bytes,STAMP_VALUE,self.stamp_value);
        word(bytes,FINALIZE+0x60,(START as i32-FINALIZE as i32) as u32);
        word(bytes,END,0x40000002); // End with qualified completion flag
        Ok(())
    }
}
