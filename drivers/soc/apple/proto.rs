// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_CONTROL: u8 = 0x00;
pub(crate) const EP_DISCOVER: u8 = 0xFD;
pub(crate) const EP_SHMEM: u8 = 0xFE;
pub(crate) const EP_BOOT: u8 = 0xFF;
pub(crate) const EP_XARM: u8 = 0x13;
pub(crate) const EP_SBIO: u8 = 0x08;
pub(crate) const EP_SCRD: u8 = 0x0a;
pub(crate) const EP_SKS: u8 = 0x12;

pub(crate) const DISCOVER_TYPE_DESCRIPTOR: u8 = 0x00;
pub(crate) const DISCOVER_TYPE_CONFIG: u8 = 0x01;

pub(crate) const MSG_TAG_SHIFT: u32 = 8;
pub(crate) const MSG_TYPE_SHIFT: u32 = 16;
pub(crate) const MSG_PARAM_SHIFT: u32 = 24;
pub(crate) const MSG_DATA_SHIFT: u32 = 32;

// 4 KiB units even though CPU pages are 16 KiB
pub(crate) const IOVA_SHIFT: u32 = 12;

#[derive(Clone, Copy)]
pub(crate) struct Fields {
    pub(crate) ep: u8,
    pub(crate) tag: u8,
    pub(crate) ty: u8,
    pub(crate) param: u8,
    pub(crate) data_lo: u32,
}

pub(crate) fn decode(msg: &Message) -> Fields {
    Fields {
        ep: msg.msg0 as u8,
        tag: (msg.msg0 >> MSG_TAG_SHIFT) as u8,
        ty: (msg.msg0 >> MSG_TYPE_SHIFT) as u8,
        param: (msg.msg0 >> MSG_PARAM_SHIFT) as u8,
        data_lo: (msg.msg0 >> MSG_DATA_SHIFT) as u32,
    }
}

pub(crate) const fn encode_registration_msg0(iova: u64, size: usize) -> u64 {
    (EP_SHMEM as u64)
        | (((size as u64) >> IOVA_SHIFT) << MSG_TYPE_SHIFT)
        | ((iova >> IOVA_SHIFT) << MSG_DATA_SHIFT)
}

static_assert!(encode_registration_msg0(0xBEE0_0000, 0x4_0000) == 0x000b_ee00_0040_00fe);

pub(crate) fn shmem_registration(iova: u64, size: usize) -> Result<Message> {
    let unit = 1u64 << IOVA_SHIFT;

    if size == 0 || (size as u64) & (unit - 1) != 0 {
        return Err(EINVAL);
    }
    if iova & (unit - 1) != 0 {
        return Err(EINVAL);
    }

    let size_field = (size as u64) >> IOVA_SHIFT;
    if size_field > 0xFF {
        return Err(EINVAL);
    }

    let iova_field = iova >> IOVA_SHIFT;
    if iova_field > u64::from(u32::MAX) {
        return Err(EINVAL);
    }

    Ok(Message {
        msg0: encode_registration_msg0(iova, size),
        msg1: 0,
    })
}

pub(crate) const CONTROL_REPLY_TYPE: u8 = 0x01;

pub(crate) const TAG_ENTROPY: u8 = 0xE7;

pub(crate) const CONTROL_TIMEOUT_MS: u32 = 2000;

pub(crate) const TAG_POOL_FIRST: u8 = 0x01;
pub(crate) const TAG_POOL_LAST: u8 = 0x7e;

static_assert!(TAG_POOL_FIRST > 0);
static_assert!(TAG_POOL_LAST < TAG_ENTROPY);

pub(crate) struct ControlOp {
    ty: u8,
    param: u8,
    data: u32,
    expect_reply: bool,
    timeout_ms: u32,
    reserved_tag: bool,
    name: &'static CStr,
}

impl ControlOp {
    pub(crate) fn expects_reply(&self) -> bool {
        self.expect_reply
    }
    pub(crate) fn timeout_ms(&self) -> u32 {
        self.timeout_ms
    }
    pub(crate) fn uses_reserved_tag(&self) -> bool {
        self.reserved_tag
    }
    pub(crate) fn name(&self) -> &'static CStr {
        self.name
    }
}

pub(crate) fn op_nop(param: u8) -> ControlOp {
    ControlOp {
        ty: 0x00,
        param,
        data: 0,
        expect_reply: true,
        timeout_ms: CONTROL_TIMEOUT_MS,
        reserved_tag: false,
        name: c"NOP",
    }
}

pub(crate) fn op_security_mode() -> ControlOp {
    ControlOp {
        ty: 0x14,
        param: 0x00,
        data: 0,
        expect_reply: true,
        timeout_ms: CONTROL_TIMEOUT_MS,
        reserved_tag: false,
        name: c"SECMODE",
    }
}

// no op for control 0x18: it wedges the control endpoint

pub(crate) fn op_get_entropy() -> ControlOp {
    ControlOp {
        ty: 0x36,
        param: 0x00,
        data: 0,
        expect_reply: true,
        timeout_ms: CONTROL_TIMEOUT_MS,
        reserved_tag: true,
        name: c"GET_ENTROPY",
    }
}

pub(crate) fn encode_control(op: &ControlOp, tag: u8) -> Message {
    Message {
        msg0: u64::from(EP_CONTROL)
            | (u64::from(tag) << MSG_TAG_SHIFT)
            | (u64::from(op.ty) << MSG_TYPE_SHIFT)
            | (u64::from(op.param) << MSG_PARAM_SHIFT)
            | (u64::from(op.data) << MSG_DATA_SHIFT),
        msg1: 0,
    }
}

const OP_OOL_INBOUND_SIZE: u8 = 0x04;
const OP_OOL_INBOUND_ADDR: u8 = 0x02;
const OP_OOL_OUTBOUND_SIZE: u8 = 0x05;
const OP_OOL_OUTBOUND_ADDR: u8 = 0x03;

fn op_ool(ty: u8, endpoint: u8, data: u32, name: &'static CStr) -> ControlOp {
    ControlOp {
        ty,
        param: endpoint,
        data,
        expect_reply: true,
        timeout_ms: CONTROL_TIMEOUT_MS,
        reserved_tag: false,
        name,
    }
}

pub(crate) fn op_ool_inbound_size(endpoint: u8, len: u32) -> ControlOp {
    op_ool(OP_OOL_INBOUND_SIZE, endpoint, len, c"OOL_IN_SIZE")
}

pub(crate) fn op_ool_inbound_addr(endpoint: u8, iova: u64) -> ControlOp {
    op_ool(
        OP_OOL_INBOUND_ADDR,
        endpoint,
        (iova >> IOVA_SHIFT) as u32,
        c"OOL_IN_ADDR",
    )
}

pub(crate) fn op_ool_outbound_size(endpoint: u8, len: u32) -> ControlOp {
    op_ool(OP_OOL_OUTBOUND_SIZE, endpoint, len, c"OOL_OUT_SIZE")
}

pub(crate) fn op_ool_outbound_addr(endpoint: u8, iova: u64) -> ControlOp {
    op_ool(
        OP_OOL_OUTBOUND_ADDR,
        endpoint,
        (iova >> IOVA_SHIFT) as u32,
        c"OOL_OUT_ADDR",
    )
}

pub(crate) struct FieldCursor<'a> {
    body: &'a [u8],
    at: usize,
}

impl<'a> FieldCursor<'a> {
    pub(crate) fn new(body: &'a [u8]) -> FieldCursor<'a> {
        FieldCursor { body, at: 0 }
    }

    pub(crate) fn i32(&mut self) -> Option<i32> {
        let end = self.at.checked_add(4)?;
        let v = i32::from_le_bytes(self.body.get(self.at..end)?.try_into().ok()?);
        self.at = end;
        Some(v)
    }

    // blob = u32 len + bytes + zero pad to 4-byte boundary; 0x45 reply has two in a row
    pub(crate) fn blob(&mut self) -> Option<&'a [u8]> {
        let len_end = self.at.checked_add(4)?;
        let len = u32::from_le_bytes(self.body.get(self.at..len_end)?.try_into().ok()?) as usize;
        let end = len_end.checked_add(len)?;
        let bytes = self.body.get(len_end..end)?;
        let pad = len.wrapping_neg() % 4;
        let next = end.checked_add(pad)?;
        if self.body.get(end..next)?.iter().any(|byte| *byte != 0) {
            return None;
        }
        self.at = next;
        Some(bytes)
    }
}

#[derive(Clone, Copy)]
pub(crate) struct ControlReply {
    pub(crate) tag: u8,
    pub(crate) data_lo: u32,
}

impl ControlReply {
    pub(crate) fn from_message(msg: &Message) -> ControlReply {
        let f = decode(msg);
        ControlReply {
            tag: f.tag,
            data_lo: f.data_lo,
        }
    }
}
