// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J613 25G83 queue and client-notification storage.
pub(crate) mod context {
    pub(crate) const SIZE:usize=0x38;
    pub(crate) const REGISTRATION:usize=0; // Two slot bytes, then observed ready state 4.
    pub(crate) const ENABLE:usize=5;
    pub(crate) const SCHEDULING_WEIGHT:usize=0x26;
    pub(crate) const SENTINEL_33:usize=0x33;
}
pub(crate) mod info {
    pub(crate) const SIZE:usize=0x24c0; // Includes owned firmware scheduler storage.
    pub(crate) const RING_STATE:usize=0;
    pub(crate) const RING:usize=8;
    pub(crate) const NOTIFIER_LIST:usize=0x10;
    pub(crate) const SCHEDULER_POINTER:usize=0x18;
    pub(crate) const SCHEDULER_STORAGE:usize=0xb0;
    pub(crate) const CONSUMERS:[usize;3]=[0x20,0x24,0x28];
    pub(crate) const EVENT_ID:usize=0x2c;
    pub(crate) const PRIORITY_0:usize=0x30;
    pub(crate) const PRIORITY_1:usize=0x34;
    pub(crate) const PRIORITY_MASK:usize=0x38;
    pub(crate) const PRIORITY_5:usize=0x48;
    pub(crate) const UNKNOWN_4C:usize=0x4c;
    pub(crate) const STATE_80:usize=0x80; // Qualified idle predicate requires zero.
    pub(crate) const CONTEXT:usize=0xa4; // Packed u64 firmware VA.
}
pub(crate) mod ring {
    pub(crate) const SIZE:usize=0x60;
    pub(crate) const GPU_READ:usize=0x30;
    pub(crate) const CPU_WRITE:usize=0x40;
    pub(crate) const CAPACITY:usize=0x50;
    pub(crate) const ENTRY_SIZE:usize=8; // Firmware command VA, not a GPU VA.
}
pub(crate) mod notifier_list {
    pub(crate) const SIZE:usize=0x18;
    pub(crate) const SELF:usize=8; // Existing empty-list self link.
}
pub(crate) mod barrier {
    pub(crate) const SIZE:usize=0x40;
    pub(crate) const OPCODE:usize=0;
    pub(crate) const STAMP_0:usize=4;
    pub(crate) const STAMP_1:usize=0xc;
    pub(crate) const CONTROL_20:usize=0x20;
    pub(crate) const QUALIFIED_OPCODE:u32=4;
    pub(crate) const QUALIFIED_CONTROL:u32=0x80;
}
pub(crate) mod client {
    pub(crate) const SIZE:usize=0x1000;
    pub(crate) const THRESHOLDS:[usize;3]=[0x600,0x608,0x610];
    pub(crate) const FW_STAMPS:[usize;3]=[0x620,0x630,0x640];
    pub(crate) const USER_STAMPS:[usize;3]=[0x628,0x638,0x648];
}
