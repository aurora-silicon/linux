// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! G15S / RTKit 2419 initialization storage layout.
//! Config owns every DMA object and MMIO mapping until firmware stops. Fixed
//! VAs, packed offsets and permissions follow the qualified setup. Owners marked
//! Hardware, Globals or Power receive the images m3_adt_config generates from the
//! device tree; every other owner starts zeroed.
use crate::m3_init_layout::{Error,Region};
pub(crate) const COUNT:usize=49;
pub(crate) const ROOT:usize=36;
pub(crate) const REGION_A:usize=37;
pub(crate) const RUNTIME_POINTERS:usize=38;
pub(crate) const HARDWARE_DATA:usize=39;
pub(crate) const UNKNOWN_PAIR:usize=40;
pub(crate) const UNKNOWN_SMALL:usize=41;
pub(crate) const UNKNOWN_C0:usize=42;
pub(crate) const UNKNOWN_C1:usize=43;
pub(crate) const UNKNOWN_C3:usize=44;
pub(crate) const GLOBALS:usize=45;
pub(crate) const GLOBALS_POWER:usize=46;
pub(crate) const CONTROL_REGION:usize=47;
pub(crate) const RUNTIME_FLAGS:usize=48;
pub(crate) const DEVICE_CONTROL:usize=12;
pub(crate) const EVENT:usize=13;
pub(crate) const FIRMWARE_LOG:usize=14;
pub(crate) const TRACE:usize=15;
pub(crate) const STATS:usize=16;
pub(crate) const FW_CONTROL_STATE:usize=34;
pub(crate) const FW_CONTROL_RING:usize=35;
/// Initial contents of an owner: zeroed, or one of the three images generated
/// from the device tree (m3_adt_config). Driver-owned references are filled
/// before firmware boot.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) enum Initial {Zero,Hardware,Globals,Power}
#[derive(Clone,Copy,Debug)]
pub(crate) struct Allocation {
    pub(crate) address:u64,pub(crate) size:usize,
    /// Additional GPU access to the firmware-root mapping; no client GPU alias.
    pub(crate) gpu_shared:bool,pub(crate) initial:Initial,
}
pub(crate) fn allocation(index:usize)->Result<Allocation,Error> {
    use Initial::*;
    let (address,size,gpu_shared,initial)=match index {
        // Twelve host submission channels: TA/fragment/compute at four priorities.
        0..=23=>if index%2==0 {
            (0xfffffc2040003fd0+(index/2) as u64*0x44000,48,false,Zero)
        }else{(0xfffffc2000002800+(index/2) as u64*0x44000,6144,false,Zero)},
        24=>(0xfffffc2040333fd0,48,false,Zero), // Device control state/ring.
        25=>(0xfffffc2000330800,14336,false,Zero),
        26=>(0xfffffc2040377fd0,48,false,Zero), // Events.
        27=>(0xfffffc20403b8800,14336,false,Zero),
        28=>(0xfffffc20403ffee0,288,false,Zero), // Six firmware-log channels.
        29=>(0xfffffc2040443000,331776,false,Zero),
        30=>(0xfffffc20404d7fd0,48,false,Zero), // Trace.
        31=>(0xfffffc2040519000,28672,false,Zero),
        32=>(0xfffffc2040563fd0,48,false,Zero), // Statistics.
        33=>(0xfffffc20405a4000,16384,false,Zero),
        FW_CONTROL_STATE=>(0xfffffc20405ebfd0,48,false,Zero),
        FW_CONTROL_RING=>(0xfffffc2000376c00,5120,false,Zero),
        ROOT=>(0xfffffc204062ff40,192,false,Zero),
        REGION_A=>(0xfffffc2040670000,16384,false,Zero),
        RUNTIME_POINTERS=>(0xfffffc20406b7b4d,1203,false,Zero),
        HARDWARE_DATA=>(0xfffffc20406fb5fc,35332,false,Hardware),
        UNKNOWN_PAIR=>(0xfffffc2040747f00,256,false,Zero),
        UNKNOWN_SMALL=>(0xfffffc204078bff0,16,false,Zero),
        UNKNOWN_C0=>(0xfffffc2070003000,4096,true,Zero),
        UNKNOWN_C1=>(0xfffffc2070008000,16384,true,Zero),
        UNKNOWN_C3=>(0xfffffc2070010000,16384,true,Zero),
        GLOBALS=>(0xfffffc20407cc204,15868,false,Globals),
        GLOBALS_POWER=>(0xfffffc204081370c,2292,false,Power),
        CONTROL_REGION=>(0xfffffc20003bbc30,50128,false,Zero),
        RUNTIME_FLAGS=>(0xfffffc2040857fc0,64,false,Zero),
        _=>return Err(Error::Bounds),
    };
    Ok(Allocation{address,size,gpu_shared,initial})
}
/// Hardware B IOMapping entries are packed 32-byte records. Physical address,
/// size, range size and flags come from the generated HwData image; Config
/// supplies the owned firmware virtual address at +8. Slots are HwDataB
/// IO-mapping table slots. offset preserves a subpage PA.
#[derive(Clone,Copy,Debug)]
pub(crate) struct IoMap {
    pub(crate) slot:usize,pub(crate) physical:u64,pub(crate) size:usize,
    pub(crate) address:u64,pub(crate) offset:usize,
}
impl IoMap {
    pub(crate) fn pointer_field(self)->Result<usize,Error> {
        if self.slot>=31 {return Err(Error::Bounds);}
        Ok(0x640+self.slot*32+8)
    }
    pub(crate) fn pointer(self,owned:Region)->Result<u64,Error> {owned.at(self.offset,1)}
}
pub(crate) const IOMAPS:[IoMap;15]=[
    IoMap { slot:0, physical:0x290d00000, size:0x144000, address:0xfffffc2068000000, offset:0x0 },
    IoMap { slot:1, physical:0x20e100000, size:0x4000, address:0xfffffc2068148000, offset:0x1000 },
    IoMap { slot:2, physical:0x351014000, size:0x4000, address:0xfffffc2068150000, offset:0x0 },
    IoMap { slot:3, physical:0x290000000, size:0x20000, address:0xfffffc2068158000, offset:0x0 },
    IoMap { slot:7, physical:0x3502bc000, size:0x4000, address:0xfffffc206817c000, offset:0x0 },
    IoMap { slot:9, physical:0x290e08000, size:0x8000, address:0xfffffc2068184000, offset:0x0 },
    IoMap { slot:11, physical:0x220000000, size:0x12c000, address:0xfffffc2068190000, offset:0x0 },
    IoMap { slot:12, physical:0x35104c000, size:0x4000, address:0xfffffc20682c0000, offset:0x0 },
    IoMap { slot:18, physical:0x3503d0000, size:0x4000, address:0xfffffc20682c8000, offset:0x0 },
    IoMap { slot:19, physical:0x3503c0000, size:0x4000, address:0xfffffc20682d0000, offset:0x0 },
    IoMap { slot:20, physical:0x3503d8000, size:0x4000, address:0xfffffc20682d8000, offset:0x0 },
    IoMap { slot:23, physical:0x293000000, size:0x400000, address:0xfffffc20682e0000, offset:0x0 },
    IoMap { slot:25, physical:0x30945c000, size:0x4000, address:0xfffffc20686e4000, offset:0x0 },
    IoMap { slot:26, physical:0x350280000, size:0x8000, address:0xfffffc20686ec000, offset:0x0 },
    IoMap { slot:29, physical:0x290e5c000, size:0x4000, address:0xfffffc20686f8000, offset:0x0 },
];
// HWDataB.gpu_region_base refers to the physical GPU reserved region (TTBs),
// not an MMIO firmware VA. Resources validates its ownership and alignment.
pub(crate) const GPU_REGION_PHYSICAL:usize=0xb44;
/// Unknown C1's qualified parameter-buffer descriptor. The first word is
/// retained verbatim; the second word supplies the 22-bit page count. This is
/// the previous render_pb initialization with its page-count patch combined.
pub(crate) fn parameter_buffer(pages:u32)->Result<[u8;16],Error> {
    if pages>0x3fffff {return Err(Error::Bounds);}
    let mut bytes=[0;16];bytes[..4].copy_from_slice(&0x07400000u32.to_le_bytes());
    bytes[4..8].copy_from_slice(&pages.to_le_bytes());Ok(bytes)
}
