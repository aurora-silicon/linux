// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J613 25G83 configuration objects and firmware I/O mappings.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
#[repr(usize)]
pub(crate) enum Object {
    Root, Brn, Runtime, Globals, Control, FirmwareData, Power, Dynamic, Main,
    PowerPerformance, Timer, Activity254, Activity25c, Activity264, Activity26c,
    LogState, LogEntries, LogData, Fwctl, DevctrlState, DevctrlRing,
    KtraceState, KtraceData, StatisticsState, StatisticsData, Temperature,
    EventState, EventData, Retention, UmaTable, ParameterTable,
}
pub(crate) const OBJECTS:[Object;31]=[
    Object::Root,Object::Brn,Object::Runtime,Object::Globals,Object::Control,Object::FirmwareData,Object::Power,Object::Dynamic,Object::Main,Object::PowerPerformance,Object::Timer,Object::Activity254,Object::Activity25c,Object::Activity264,Object::Activity26c,Object::LogState,Object::LogEntries,Object::LogData,Object::Fwctl,Object::DevctrlState,Object::DevctrlRing,Object::KtraceState,Object::KtraceData,Object::StatisticsState,Object::StatisticsData,Object::Temperature,Object::EventState,Object::EventData,Object::Retention,Object::UmaTable,Object::ParameterTable
];
impl Object {
    pub(crate) const fn size(self)->usize {match self {
        Self::Root=>0xc8,
        Self::Brn=>0x4000,
        Self::Runtime=>0x4b8,
        Self::Globals=>0xe48,
        Self::Control=>0x20,
        Self::FirmwareData=>0xeaf0,
        Self::Power=>0x670,
        Self::Dynamic=>0x4000,
        Self::Main=>0x2658,
        Self::PowerPerformance=>0x6578,
        Self::Timer=>0x88,
        Self::Activity254=>0xc18,
        Self::Activity25c=>0x1248,
        Self::Activity264=>0xe10,
        Self::Activity26c=>0x60,
        Self::LogState=>0x1b0,
        Self::LogEntries=>0x28800,
        Self::LogData=>0x79800,
        Self::Fwctl=>0x4000,
        Self::DevctrlState=>0x30,
        Self::DevctrlRing=>0x4000,
        Self::KtraceState=>0x30,
        Self::KtraceData=>0x9000,
        Self::StatisticsState=>0x30,
        Self::StatisticsData=>0x4800,
        Self::Temperature=>0x4,
        Self::EventState=>0x30,
        Self::EventData=>0x4800,
        Self::Retention=>0x4000,
        Self::UmaTable=>0x4000,
        Self::ParameterTable=>0x4000,
    }}
    pub(crate) const fn needs_client_mapping(self)->bool {
        matches!(self,Self::UmaTable|Self::ParameterTable)
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) enum Error {Address,Bounds}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) enum Protection {ReadOnly,ReadWrite,Protected}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) struct PhysicalAddress(pub(crate) u64);
impl PhysicalAddress {
    pub(crate) const fn page_offset(self)->usize {(self.0&0x3fff) as usize}
    pub(crate) const fn page_base(self)->u64 {self.0&!0x3fff}
}
#[derive(Clone,Copy,Debug)]
pub(crate) struct FirmwareVa(u64);
impl FirmwareVa {
    pub(crate) fn new(value:u64)->Result<Self,Error> {
        if value<0xffff_fc00_0000_0000 {return Err(Error::Address);}
        Ok(Self(value))
    }
}
pub(crate) const IO_SLOTS:usize=crate::g16_board::MAX_IO_SLOTS;
pub(crate) const IO_ENTRY_SIZE:usize=0x28;
pub(crate) const IO_TABLE_SIZE:usize=IO_SLOTS*IO_ENTRY_SIZE;
#[derive(Clone,Copy,Debug)]
pub(crate) struct IoMap {
    pub(crate) slot:usize,pub(crate) physical:PhysicalAddress,
    pub(crate) mapped:usize,pub(crate) requested:usize,
    pub(crate) tag:u64,pub(crate) mode:u64,
}
impl IoMap {
    pub(crate) fn protection(self)->Protection {
        if crate::g16_board::get().ok().and_then(|b|b.protected_slot)==Some(self.slot) {Protection::Protected}
        else if self.mode==0 {Protection::ReadOnly}else{Protection::ReadWrite}
    }
    pub(crate) fn mapping_size(self)->Result<usize,Error> {
        if self.slot==3 {return Ok(0x18000);}
        self.mapped.checked_add(self.physical.page_offset()).and_then(|n|n.checked_add(0x3fff))
            .map(|n|n&!0x3fff).ok_or(Error::Bounds)
    }
    pub(crate) fn second_bank(self)->Result<Option<(u64,PhysicalAddress,usize)>,Error> {
        if self.slot!=3 {return Ok(None);}
        Ok(Some((0x18000,PhysicalAddress(self.physical.0.checked_add(0x2000000).ok_or(Error::Address)?),0x18000)))
    }
    pub(crate) fn encode(self,va:FirmwareVa)->Result<[u8;IO_ENTRY_SIZE],Error> {
        if self.slot>=IO_SLOTS || self.mapped==0 || self.requested==0
            || self.mapped>u32::MAX as usize || self.requested>u32::MAX as usize {
            return Err(Error::Bounds);
        }
        if va.0&0x3fff!=self.physical.0&0x3fff || va.0.checked_add(self.mapped as u64).is_none() {
            return Err(Error::Address);
        }
        let mut out=[0;IO_ENTRY_SIZE];
        out[0..8].copy_from_slice(&self.physical.0.to_le_bytes());
        out[8..16].copy_from_slice(&va.0.to_le_bytes());
        out[16..20].copy_from_slice(&(self.mapped as u32).to_le_bytes());
        out[20..24].copy_from_slice(&(self.requested as u32).to_le_bytes());
        out[24..32].copy_from_slice(&self.tag.to_le_bytes());
        out[32..40].copy_from_slice(&self.mode.to_le_bytes());Ok(out)
    }
}

pub(crate) mod runtime {
    pub(crate) const MAIN: usize = 0x0;
    pub(crate) const TIMER: usize = 0x10;
    pub(crate) const SCRATCH: usize = 0x18;
    pub(crate) const EVENT_STATE: usize = 0x1c0;
    pub(crate) const EVENT_DATA: usize = 0x1c8;
    pub(crate) const LOG_STATE: usize = 0x1d0;
    pub(crate) const LOG_ENTRIES: usize = 0x1d8;
    pub(crate) const KTRACE_STATE: usize = 0x1e0;
    pub(crate) const KTRACE_DATA: usize = 0x1e8;
    pub(crate) const STATISTICS_STATE: usize = 0x1f0;
    pub(crate) const STATISTICS_DATA: usize = 0x1f8;
    pub(crate) const LOG_DATA: usize = 0x200;
    pub(crate) const UNKNOWN_250: usize = 0x250;
    pub(crate) const TILER_ENGINE: usize = 0x254;
    pub(crate) const FRAGMENT_ENGINE: usize = 0x25c;
    pub(crate) const COMPUTE_ENGINE: usize = 0x264;
    pub(crate) const RECOVERY_ENGINE: usize = 0x26c;
    pub(crate) const PARAMETER_GPU: usize = 0x2d0;
    pub(crate) const PARAMETER_FW: usize = 0x2d8;
    pub(crate) const UMA_GPU: usize = 0x2e0;
    pub(crate) const UMA_FW: usize = 0x2e8;
    pub(crate) const UNKNOWN_2F8: usize = 0x2f8;
    pub(crate) const POWER_PERFORMANCE: usize = 0x469;
}
pub(crate) mod globals {
    pub(crate) const UNKNOWN_78: usize = 0x78;
    pub(crate) const UNKNOWN_E28: usize = 0xe28;
    pub(crate) const PROGRESS_INTERVAL_MS: usize = 0x24;
    pub(crate) const UNKNOWN_998: usize = 0x998;
    pub(crate) const UNKNOWN_99C: usize = 0x99c;
    pub(crate) const UNKNOWN_9A0: usize = 0x9a0;
    pub(crate) const IDLE_OFF_DELAY_MS: usize = 0x9b8;
    pub(crate) const UNKNOWN_9BC: usize = 0x9bc;
    pub(crate) const UNKNOWN_9C0: usize = 0x9c0;
    pub(crate) const UNKNOWN_9C8: usize = 0x9c8;
    pub(crate) const UNKNOWN_9CC: usize = 0x9cc;
    pub(crate) const PERFORMANCE_CEILINGS: [usize; 3] = [0x88, 0x8c, 0x98];
}
pub(crate) mod main_data {
    pub(crate) const TIMESTAMP_WINDOW: usize = 0x28;
    pub(crate) const IOMAPS: usize = 0x640;
    pub(crate) const ACCOUNTING_ENABLE: usize = 0xea0;
    pub(crate) const UNKNOWN_EB8: usize = 0xeb8;
    pub(crate) const REFERENCE_CLOCK_KHZ: usize = 0xed0;
    pub(crate) const MAINTENANCE_INTERVAL: usize = 0xed4;
    pub(crate) const POWER_SAMPLE_INTERVAL: usize = 0xed8;
    pub(crate) const UNKNOWN_F24: usize = 0xf24;
    pub(crate) const CORE_POSITIONS: usize = 0xfc0;
    pub(crate) const MAX_PSTATE: usize = 0xfc4;
    pub(crate) const FREQUENCIES_MHZ: usize = 0xfc8;
    pub(crate) const VOLTAGES_MV: usize = 0x1008;
    pub(crate) const SRAM_VOLTAGES_MV: usize = 0x1408;
    pub(crate) const AUX_FREQUENCIES_MHZ: usize = 0x1808;
    pub(crate) const UNKNOWN_259C: usize = 0x259c;
    pub(crate) const RETENTION: usize = 0xe88;
    pub(crate) const ACCOUNTING_BUCKETS_ENABLE: usize = 0xee0;
    pub(crate) const UNKNOWN_2540: usize = 0x2540;
}
pub(crate) mod power_data {
    pub(crate) const TEMPERATURE_POINTER: usize = 0x3b50;
    pub(crate) const UNKNOWN_04: usize = 0x4;
    pub(crate) const UNKNOWN_08: usize = 0x8;
    pub(crate) const UNKNOWN_10: usize = 0x10;
    pub(crate) const UNKNOWN_14: usize = 0x14;
    pub(crate) const INITIAL_ACTUAL: usize = 0x2c;
    pub(crate) const INITIAL_TARGET: usize = 0x30;
    pub(crate) const HARDWARE_STATE: usize = 0x38;
    pub(crate) const PACKED_SELECTOR: usize = 0x279e;
    pub(crate) const CEILING_INPUTS: [usize; 23] = [
        0x9f0, 0x9f4, 0xa38, // power PI bounds/output
        0xaa8, 0xaac, 0xaf0, // PPM PI bounds/output
        0xb78, 0xb7c, 0xb80, 0xbc0, // performance bounds/base/output
        0x22b0, 0x22b4, 0x22f8, // temperature PI bounds/output
        0x2210, 0x2870, 0x2a3c, // additional selector inputs
        0x2b84, 0x2b88, 0x2b8c, 0x2bcc, // secondary performance
        0x2c40, 0x2c44, 0x2c88, // average power PI bounds/output
    ];
}
pub(crate) mod firmware_data {
    pub(crate) const FWCTL_STATE: usize = 0x4f88;
    pub(crate) const FWCTL_RING: usize = 0x4f90;
    pub(crate) const UNKNOWN_4FD4: usize = 0x4fd4;
    pub(crate) const CONSUMED_GENERATION: usize = 0xeae8;
    pub(crate) const RECOVERY_SUMMARY: usize = 0x4ebc;
}
pub(crate) mod channel {
    pub(crate) const COUNT: usize = 12;
    pub(crate) const STATE_SIZE: usize = 0x30;
    pub(crate) const SUBMIT_RING_SIZE: usize = 0x1800;
    pub(crate) const RUNTIME_SUBMIT: usize = 0x20;
    pub(crate) const DESCRIPTOR_SIZE: usize = 0x20;
    pub(crate) const RUNTIME_CONTROL: usize = 0x1a0;
    pub(crate) const READ: usize = 0;
    pub(crate) const CFI: usize = 0x10;
    pub(crate) const WRITE: usize = 0x20;
    pub(crate) const CONTROL_RECORD_SIZE: usize = 0x40;
    pub(crate) const CONTROL_CAPACITY: u32 = 256;
}
