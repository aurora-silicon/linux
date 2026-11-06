// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Scheduler and timestamp-queue state shared with firmware. Only a freshly
//! leased node may be initialized. After publication, the host writes its
//! separate submitted counter and reads the node completion word individually.

use kernel::prelude::*;

#[repr(C)]
struct McacheDsidState {
    /// MCache clients registered by the host, one bit per client.
    clients: [u32; 2],
    /// DSID/UAT context selected by each MCache client.
    dsid: [u8; 0x28],
    /// Enabled MCache clients, one bit per client.
    enabled: [u32; 2],
    /// MCache client mode, one bit per client.
    mode: [u32; 2],
    /// Non-zero after the host changes the MCache client configuration.
    dirty: u32,
}
impl McacheDsidState {
    fn set(&mut self, client: usize, dsid: u8) {
        let word = client / u32::BITS as usize;
        let bit = 1u32 << (client % u32::BITS as usize);
        self.clients[word] |= bit;
        self.dsid[client] = dsid;
        self.enabled[word] |= bit;
        self.mode[word] |= bit;
        self.dirty = 1;
    }
}

#[repr(C)]
pub(crate) struct Scheduler {
    unk0: u64,
    unk8: [u64; 3],
    unk20: u64,
    unk28: u64,
    unk30: u64,
    unk38: [u8; 0x5ec],
    mcache: McacheDsidState,
    unk668: [u8; 0x3998],
}
impl Scheduler {
    /// Called on zeroed storage before its first publication.
    pub(crate) fn init(&mut self, mcache_dsid: Option<(usize, u8)>) {
        self.unk0 = 0x0000_0100_0000_ffff;
        self.unk20 = 0x0002_0000_0000_0000;
        self.unk30 = 0x0000_0000_ff00_0000;
        if let Some((client, dsid)) = mcache_dsid {
            self.mcache.set(client, dsid);
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct WorkNode {
    pub(crate) submitted: u64,
    pub(crate) word: u32,
    pub(crate) completed: u32,
    pub(crate) threshold: u32,
    unk14: [u32; 45],
}
impl WorkNode {
    pub(crate) fn new(submitted: u64, word: u32) -> Option<Self> {
        if submitted == 0 || submitted & 3 != 0 || word & 63 == 63 {
            return None;
        }
        Some(Self {
            submitted,
            word,
            completed: 0,
            threshold: 0x50,
            unk14: [0; 45],
        })
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct WorkHead {
    pub(crate) next: u64,
    pub(crate) previous: u64,
    pub(crate) unk10: u32,
    pub(crate) unk14: u32,
}

const _: () = {
    assert!(core::mem::size_of::<Scheduler>() == 0x4000);
    assert!(core::mem::offset_of!(Scheduler, mcache) == 0x624);
    assert!(core::mem::offset_of!(McacheDsidState, dsid) == 0x08);
    assert!(core::mem::offset_of!(McacheDsidState, enabled) == 0x30);
    assert!(core::mem::offset_of!(McacheDsidState, mode) == 0x38);
    assert!(core::mem::offset_of!(McacheDsidState, dirty) == 0x40);
    assert!(core::mem::size_of::<WorkNode>() == 0xc8);
    assert!(core::mem::offset_of!(WorkNode, completed) == 0xc);
    assert!(core::mem::size_of::<WorkHead>() == 0x18);
};

// SAFETY: These shared records contain only integer fields and arrays.
unsafe impl Zeroable for Scheduler {}
// SAFETY: WorkNode contains only integers; zero is valid for every field.
unsafe impl Zeroable for WorkNode {}
// SAFETY: WorkHead contains only integer addresses and words.
unsafe impl Zeroable for WorkHead {}

#[kunit_tests(asahi_g17_mcache_dsid)]
mod tests {
    use super::*;

    #[test]
    fn client_state_registers_the_selected_dsid() {
        let mut state = McacheDsidState {
            clients: [0; 2],
            dsid: [0; 0x28],
            enabled: [0; 2],
            mode: [0; 2],
            dirty: 0,
        };
        state.set(0x22, 0x17);
        assert_eq!(state.clients, [0, 1 << 2]);
        assert_eq!(state.dsid[0x22], 0x17);
        assert_eq!(state.enabled, [0, 1 << 2]);
        assert_eq!(state.mode, [0, 1 << 2]);
        assert_eq!(state.dirty, 1);
    }
}
