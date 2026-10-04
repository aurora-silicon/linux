// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU initialization / global structures

use super::channels;
use super::types::*;
use crate::{
    default_zeroed,
    gem,
    mmu,
    no_debug,
    trivial_gpustruct, //
};

pub(crate) mod raw {
    use super::*;

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct ChannelRing<T: GpuStruct + Debug + Default, U: Copy> {
        pub(crate) state: Option<GpuWeakPointer<T>>,
        pub(crate) ring: Option<GpuWeakPointer<[U]>>,
    }

    /// Host->FW ring descriptor of the 14.x firmware ABI.
    ///
    /// Instead of one `ChannelState` pointer, each host->FW ring (pipes, DeviceControl) is
    /// described by the FW VAs of its three index words and of the ring itself. On G15 the
    /// indices (and the pipe rings) live in Fender scratch SRAM.
    #[derive(Debug, Default, Copy, Clone)]
    #[repr(C)]
    pub(crate) struct ScratchRingDesc {
        /// FW VA of the u32 read index (FW writes; scratch part B). +0x00
        pub(crate) read_index: U64,
        /// FW VA of the u32 "CFI" index (FW writes; scratch part B). +0x08
        pub(crate) cfi_index: U64,
        /// FW VA of the u32 write index (host writes; scratch part A). +0x10
        pub(crate) write_index: U64,
        /// FW VA of the ring entries. +0x18
        pub(crate) ring: U64,
    }

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct PipeChannels {
        #[ver(V < V14_8_3)]
        pub(crate) vtx: ChannelRing<channels::ChannelState, channels::PipeMsg::ver>,
        #[ver(V < V14_8_3)]
        pub(crate) frag: ChannelRing<channels::ChannelState, channels::PipeMsg::ver>,
        #[ver(V < V14_8_3)]
        pub(crate) comp: ChannelRing<channels::ChannelState, channels::PipeMsg::ver>,

        // 14.x ABI: {TA, 3D, CL} descriptors of 0x20 bytes, 0x60 per priority
        #[ver(V >= V14_8_3)]
        pub(crate) vtx: ScratchRingDesc,
        #[ver(V >= V14_8_3)]
        pub(crate) frag: ScratchRingDesc,
        #[ver(V >= V14_8_3)]
        pub(crate) comp: ScratchRingDesc,
    }
    #[versions(AGX)]
    default_zeroed!(PipeChannels::ver);

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct FwStatusFlags {
        pub(crate) halt_count: AtomicU64,
        __pad0: Pad<0x8>,
        pub(crate) halted: AtomicU32,
        __pad1: Pad<0xc>,
        pub(crate) resume: AtomicU32,
        __pad2: Pad<0xc>,
        pub(crate) unk_40: u32,
        __pad3: Pad<0xc>,
        pub(crate) unk_ctr: u32,
        /// P0+0x45c4. Non-zero makes the firmware install its work-scheduler callbacks
        /// Left 0, the first compute kick branches to address 0 (observed on J516S).
        pub(crate) sched_callbacks: u32,
        __pad4: Pad<0x8>,
        pub(crate) unk_60: u32,
        __pad5: Pad<0xc>,
        pub(crate) unk_70: u32,
        __pad6: Pad<0xc>,
    }

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct FwStatus {
        pub(crate) fwctl_channel: ChannelRing<channels::FwCtlChannelState, channels::FwCtlMsg>,
        pub(crate) flags: FwStatusFlags,
    }

    /// G15 "P0" globals-2/status block, 0xc3d0 bytes, pointed to by InitData+0xb0. It embeds the
    /// G13 `FwStatus` at +0x4568 (FWCtl ring pointers, 8 bytes
    /// of padding, then the halt/resume flags at +0x4580), which is why the
    /// FWCtl ring and flags keep their `FwStatus` field names here.
    #[repr(C)]
    pub(crate) struct StatusBlock {
        /// 0 at init; also written by the firmware register dump. +0x0
        pub(crate) unk_0: u32,
        /// Noise-suppression idle power-off state. +0x4
        pub(crate) noise_suppression_idle_pwroff: u32,
        /// 0 at init. +0x8
        pub(crate) unk_8: u32,
        /// Not touched by the host (0x0008..0x4017).
        /// TODO: firmware-private, or tables (pending stamps?) the host must fill?
        pub(crate) unk_c: Array<0x400c, u8>,
        /// Perf-counter sampler control, 0x4018..0x40a0. Unused by drm/asahi.
        pub(crate) perfctr: Array<0x88, u8>,
        /// FW-initiated recovery info (FW-written). +0x40a0/+0x40a4
        pub(crate) fw_recovery_info_a: u32,
        pub(crate) fw_recovery_info_b: u32,
        pub(crate) unk_40a8: Array<0x3f8, u8>,
        /// Recovery-packet details read on event type 4. +0x44a0
        pub(crate) recovery_packet_info: Array<0x14, u8>,
        pub(crate) unk_44b4: Array<0xc, u8>,
        /// Read while draining events. +0x44c0
        pub(crate) unk_44c0: U64,
        pub(crate) unk_44c8: Array<0x90, u8>,
        /// Read while draining events. +0x4558/+0x4560
        pub(crate) unk_4558: U64,
        pub(crate) unk_4560: U64,
        /// FWCtl `{state, ring}` FW VAs. +0x4568/+0x4570
        pub(crate) fwctl_channel: ChannelRing<channels::FwCtlChannelState, channels::FwCtlMsg>,
        /// 8 bytes of padding put the flags on a 16-byte boundary.
        pub(crate) __pad_4578: Pad<0x8>,
        /// halt_count/halted/resume/... at +0x4580/+0x4590/+0x45a0.
        /// +0x45c4 (`flags.sched_callbacks`) must be non-zero or the work scheduler
        /// is left without its callbacks.
        pub(crate) flags: FwStatusFlags,
        /// Not touched by the host (0x45c8..0xc3c7).
        pub(crate) unk_45f0: Array<0x7dd8, u8>,
        /// QoS mode, 0 at init. +0xc3c8
        pub(crate) qos_mode: u32,
        /// TODO: read-modify-written at shared-data init; left 0. +0xc3cc
        pub(crate) unk_c3cc: u32,
    }
    default_zeroed!(StatusBlock);
    no_debug!(StatusBlock);

    /// G15 "P1" power-controller shared block, 0x238 bytes, InitData+0xb8. Its 0x0c..0xd4 part
    /// keeps the order of the G13 13.5 `Globals` power
    /// fields (power zones, fast-die and PPM controllers), so the same field names are used.
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct PowerCtlBlock {
        /// FW-published power values. +0x0/+0x4
        pub(crate) fw_power_a: u32,
        pub(crate) fw_power_b: u32,
        /// Power-zone reset request. +0x8
        pub(crate) power_zone_reset: u32,
        /// +0xc..+0x5f: G13 Globals `power_zone_count`..`power_zones`, same order.
        pub(crate) power_zone_count: u32,
        pub(crate) avg_power_filter_tc_periods: u32,
        pub(crate) avg_power_ki_dt: F32,
        pub(crate) avg_power_kp: F32,
        pub(crate) avg_power_min_duty_cycle: u32,
        pub(crate) avg_power_target_filter_tc: u32,
        pub(crate) power_zones: Array<5, PowerZoneGlobal>,
        /// CLVR loop override / control effort. +0x60..+0x68
        pub(crate) clvr_loop_override_a: u32,
        pub(crate) clvr_ctrl_effort: u32,
        pub(crate) clvr_loop_override_b: u32,
        pub(crate) unk_6c: Array<0x38, u8>,
        /// Lifetime-servo temperature target. +0xa4
        pub(crate) lifetime_servo_temp_a: u32,
        /// +0xa8..+0xc8: G13 Globals `unk_89bc`..`unk_89e0`, same order.
        pub(crate) fast_die_temp_target: u32,
        pub(crate) fast_die0_release_temp: u32,
        pub(crate) unk_b0: i32,
        pub(crate) fast_die0_prop_tgt_delta: u32,
        pub(crate) fast_die0_kp: F32,
        pub(crate) fast_die0_ki_dt: F32,
        pub(crate) unk_c0: u32,
        pub(crate) unk_c4: u32,
        /// +0xc8..+0xdb: the fast-die/PPM enable flags, the PPM target (maximum power) and the
        /// PPM controller gains, in G13 Globals order (`unk_89e0`, `max_power_2`, `ppm_kp`,
        /// `ppm_ki_dt`). The two T6030 backends place them differently: the manager starts
        /// with the enable flag at +0xc8 (the layout it was validated with), the runtime backend
        /// with 0 at +0xc8 and the flag at +0xcc, as its InitData has them. See the builder.
        pub(crate) ppm_words: Array<5, u32>,
        pub(crate) unk_dc: Array<0xc, u8>,
        /// "SE" controller override / engagement enable. +0xe8/+0xec
        pub(crate) se_controller_override: u32,
        pub(crate) se_engagement: u32,
        pub(crate) unk_f0: Array<0x68, u8>,
        /// DPE leakage update config. +0x158..+0x1ab
        pub(crate) dpe_leak_cfg_a: u32,
        pub(crate) unk_15c: u32,
        pub(crate) unk_160: u32,
        pub(crate) unk_164: u32,
        pub(crate) dpe_leak_coefs: Array<0x40, u8>,
        pub(crate) dpe_leak_cfg_b: u32,
        pub(crate) unk_1ac: Array<0x8, u8>,
        /// CLPC shared data (FW-written). +0x1b4..+0x1cc
        pub(crate) clpc_shared_a: U64,
        pub(crate) clpc_shared_b: U64,
        pub(crate) clpc_split_ratio_a: u32,
        pub(crate) clpc_split_ratio_b: u32,
        pub(crate) clpc_shared_c: u32,
        /// +0x1d0 (u8) / +0x1d1 (unaligned u32)
        pub(crate) perf_ctrl_override_active: u8,
        pub(crate) consistent_perf_state: U32,
        pub(crate) unk_1d5: Array<0x3, u8>,
        /// FW-written energy / filtered power. +0x1d8/+0x1e0/+0x1e4
        pub(crate) accumulated_energy: U64,
        pub(crate) filtered_power: u32,
        pub(crate) unk_1e4: u32,
        /// SLC DSID configuration. +0x1e8..+0x22b
        pub(crate) dsid_cfg_a: U64,
        pub(crate) unk_1f0: Array<0x28, u8>,
        pub(crate) dsid_cfg_b: U64,
        pub(crate) dsid_cfg_c: U64,
        pub(crate) dsid_cfg_d: u32,
        pub(crate) unk_22c: u32,
        /// +0x230
        pub(crate) submit_in_progress_gate: u32,
        pub(crate) unk_234: u32,
    }
    default_zeroed!(PowerCtlBlock);

    /// G15 "P2" debug/trace block, 0x20 bytes, InitData+0xa8.
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct DebugBlock {
        /// Moved here from G13 `Globals.ktrace_enable`. +0x0
        pub(crate) ktrace_enable: u32,
        /// TODO: written at init with an unknown value; left 0. +0x4
        pub(crate) unk_4: u32,
        /// System-sleep notification in progress. +0x8
        pub(crate) system_sleep_in_progress: u32,
        pub(crate) unk_c: u32,
        /// Firmware-written. Read-only for the host.
        pub(crate) fw_word_10: AtomicU32,
        /// Firmware init progress: the firmware writes 0 once it consumes the INIT handshake and 1
        /// once its post-INIT initialisation is done, each followed by a barrier. The G15 driver
        /// writes a sentinel here before MSG_INIT so the three states (not consumed / stuck / done)
        /// are distinguishable.
        pub(crate) init_state: AtomicU32,
        pub(crate) unk_18: u32,
        pub(crate) unk_1c: u32,
    }
    default_zeroed!(DebugBlock);

    /// One Globals register-override record, 0x18 bytes.
    #[derive(Debug, Default, Copy, Clone)]
    #[repr(C)]
    pub(crate) struct RegisterOverride {
        pub(crate) addr: U64,
        pub(crate) value: U64,
        pub(crate) id: u32,
        pub(crate) __pad: u32,
    }

    /// One InitSeq register-initialisation record, 0x18 bytes.
    ///
    /// The InitSeq buffer (InitData+0x08) is a list of these. A zeroed buffer is a valid empty
    /// list, which is what drm/asahi passes; this type only documents the format.
    #[derive(Debug, Default, Copy, Clone)]
    #[repr(C)]
    #[allow(dead_code)]
    pub(crate) struct InitSeqRecord {
        pub(crate) value: U64,
        pub(crate) reg_id: u32,
        pub(crate) extra: u32,
        /// 1 = 32-bit, 2 = 64-bit, 3 = 64-bit physical address.
        pub(crate) kind: u32,
        pub(crate) __pad: u32,
    }

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataShared1 {
        pub(crate) table: Array<16, i32>,
        pub(crate) unk_44: Array<0x60, u8>,
        pub(crate) unk_a4: u32,
        pub(crate) unk_a8: u32,
    }
    default_zeroed!(HwDataShared1);

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct HwDataShared2Curve {
        pub(crate) unk_0: u32,
        pub(crate) unk_4: u32,
        pub(crate) t1: Array<16, u16>,
        pub(crate) t2: Array<16, i16>,
        pub(crate) t3: Array<8, Array<16, i32>>,
    }

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct HwDataShared2G14 {
        pub(crate) unk_0: Array<5, u32>,
        pub(crate) unk_14: u32,
        pub(crate) unk_18: Array<8, u32>,
        pub(crate) curve1: HwDataShared2Curve,
        pub(crate) curve2: HwDataShared2Curve,
    }

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataShared2 {
        pub(crate) table: Array<10, i32>,
        pub(crate) unk_28: Array<0x10, u8>,
        pub(crate) g14: HwDataShared2G14,
        pub(crate) unk_500: u32,
        pub(crate) unk_504: u32,
        pub(crate) unk_508: u32,
        pub(crate) unk_50c: u32,
    }
    default_zeroed!(HwDataShared2);

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataShared3 {
        pub(crate) unk_0: u32,
        pub(crate) unk_4: u32,
        pub(crate) unk_8: u32,
        pub(crate) table: Array<16, u32>,
        pub(crate) unk_4c: u32,
    }
    default_zeroed!(HwDataShared3);

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataA130Extra {
        pub(crate) unk_0: Array<0x38, u8>,
        pub(crate) unk_38: u32,
        pub(crate) unk_3c: u32,
        pub(crate) gpu_se_inactive_threshold: u32,
        pub(crate) unk_44: u32,
        pub(crate) gpu_se_engagement_criteria: i32,
        pub(crate) gpu_se_reset_criteria: u32,
        pub(crate) unk_50: u32,
        pub(crate) unk_54: u32,
        pub(crate) unk_58: u32,
        pub(crate) unk_5c: u32,
        pub(crate) gpu_se_filter_a_neg: F32,
        pub(crate) gpu_se_filter_1_a_neg: F32,
        pub(crate) gpu_se_filter_a: F32,
        pub(crate) gpu_se_filter_1_a: F32,
        pub(crate) gpu_se_ki_dt: F32,
        pub(crate) gpu_se_ki_1_dt: F32,
        pub(crate) unk_78: F32,
        pub(crate) unk_7c: F32,
        pub(crate) gpu_se_kp: F32,
        pub(crate) gpu_se_kp_1: F32,
        pub(crate) unk_88: u32,
        pub(crate) unk_8c: u32,
        pub(crate) max_pstate_scaled_1: u32,
        pub(crate) unk_94: u32,
        pub(crate) unk_98: u32,
        pub(crate) unk_9c: F32,
        pub(crate) unk_a0: u32,
        pub(crate) unk_a4: u32,
        pub(crate) gpu_se_filter_time_constant_ms: u32,
        pub(crate) gpu_se_filter_time_constant_1_ms: u32,
        pub(crate) gpu_se_filter_time_constant_clks: U64,
        pub(crate) gpu_se_filter_time_constant_1_clks: U64,
        pub(crate) unk_c0: u32,
        pub(crate) unk_c4: F32,
        pub(crate) unk_c8: Array<0x4c, u8>,
        pub(crate) unk_114: F32,
        pub(crate) unk_118: u32,
        pub(crate) unk_11c: u32,
        pub(crate) unk_120: u32,
        pub(crate) unk_124: u32,
        pub(crate) max_pstate_scaled_2: u32,
        pub(crate) unk_12c: Array<0x8c, u8>,
    }
    default_zeroed!(HwDataA130Extra);

    /// G15 (14.x) shader-engine controller block of HwDataA, at +0x10b4.
    ///
    /// It holds the fields of [`HwDataA130Extra`] in a different order: the filter, gain and
    /// time-constant words move 0x14 bytes down, and the threshold/criteria header moves behind
    /// them, to +0xd8. Offsets in the field names are G15 block offsets.
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataA140Extra {
        pub(crate) unk_0: Array<0x40, u8>,
        pub(crate) unk_40: u32,
        pub(crate) unk_44: u32,
        pub(crate) unk_48: u32,
        pub(crate) gpu_se_filter_a_neg: F32,
        pub(crate) gpu_se_filter_1_a_neg: F32,
        pub(crate) gpu_se_filter_a: F32,
        pub(crate) gpu_se_filter_1_a: F32,
        pub(crate) gpu_se_ki_dt: F32,
        pub(crate) gpu_se_ki_1_dt: F32,
        pub(crate) unk_64: F32,
        pub(crate) unk_68: F32,
        pub(crate) gpu_se_kp: F32,
        pub(crate) gpu_se_kp_1: F32,
        pub(crate) unk_74: u32,
        pub(crate) unk_78: u32,
        pub(crate) max_pstate_scaled_1: u32,
        pub(crate) min_pstate_scaled: u32,
        pub(crate) unk_84: u32,
        pub(crate) unk_88: F32,
        /// Shader-engine target (ADT gpu-se-tgt).
        pub(crate) se_target: u32,
        pub(crate) unk_90: u32,
        pub(crate) gpu_se_filter_time_constant_ms: u32,
        pub(crate) gpu_se_filter_time_constant_1_ms: u32,
        pub(crate) gpu_se_filter_time_constant_clks: U64,
        pub(crate) gpu_se_filter_time_constant_1_clks: U64,
        pub(crate) unk_ac: u32,
        pub(crate) unk_b0: F32,
        pub(crate) unk_b4: Array<0x24, u8>,
        pub(crate) unk_d8: u32,
        pub(crate) unk_dc: u32,
        pub(crate) gpu_se_inactive_threshold: u32,
        pub(crate) gpu_se_engagement_criteria: i32,
        pub(crate) unk_e8: u32,
        pub(crate) unk_ec: u32,
        pub(crate) gpu_se_reset_criteria: u32,
        pub(crate) unk_f4: Array<0x28, u8>,
        pub(crate) unk_11c: F32,
        pub(crate) unk_120: Array<0xc, u8>,
        pub(crate) unk_12c: u32,
        pub(crate) max_pstate_scaled_2: u32,
        pub(crate) unk_134: Array<0x84, u8>,
    }
    default_zeroed!(HwDataA140Extra);

    #[repr(C)]
    pub(crate) struct T81xxData {
        pub(crate) unk_d8c: u32,
        pub(crate) unk_d90: u32,
        pub(crate) unk_d94: u32,
        pub(crate) unk_d98: u32,
        pub(crate) unk_d9c: F32,
        pub(crate) unk_da0: u32,
        pub(crate) unk_da4: F32,
        pub(crate) unk_da8: u32,
        pub(crate) unk_dac: F32,
        pub(crate) unk_db0: u32,
        pub(crate) unk_db4: u32,
        pub(crate) unk_db8: F32,
        pub(crate) unk_dbc: F32,
        pub(crate) unk_dc0: u32,
        pub(crate) unk_dc4: u32,
        pub(crate) unk_dc8: u32,
        pub(crate) max_pstate_scaled: u32,
    }
    default_zeroed!(T81xxData);

    #[versions(AGX)]
    #[derive(Default, Copy, Clone)]
    #[repr(C)]
    pub(crate) struct PowerZone {
        pub(crate) val: F32,
        pub(crate) target: u32,
        pub(crate) target_off: u32,
        pub(crate) filter_tc_x4: u32,
        pub(crate) filter_tc_xperiod: u32,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_10: u32,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_14: u32,
        pub(crate) filter_a_neg: F32,
        pub(crate) filter_a: F32,
        pub(crate) pad: u32,
    }

    // G15 HwDataA uses the G13 single-die layout with 8x8 coefficient tables at 0x970-0xb6f
    //, so the G14X 16-core layout is G14X-only.
    #[versions(AGX)]
    const MAX_CORES_PER_CLUSTER: usize = {
        #[ver(G >= G14X && G < G15)]
        {
            16
        }
        #[ver(G < G14X || G >= G15)]
        {
            8
        }
    };

    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct AuxLeakCoef {
        pub(crate) afr_1: Array<2, F32>,
        pub(crate) cs_1: Array<2, F32>,
        pub(crate) afr_2: Array<2, F32>,
        pub(crate) cs_2: Array<2, F32>,
    }

    #[versions(AGX)]
    #[repr(C)]
    pub(crate) struct HwDataA {
        pub(crate) unk_0: u32,
        pub(crate) clocks_per_period: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) clocks_per_period_2: u32,

        pub(crate) unk_8: u32,
        pub(crate) pwr_status: AtomicU32,
        pub(crate) unk_10: F32,
        pub(crate) unk_14: u32,
        pub(crate) unk_18: u32,
        pub(crate) unk_1c: u32,
        pub(crate) unk_20: u32,
        pub(crate) unk_24: u32,
        pub(crate) actual_pstate: u32,
        pub(crate) tgt_pstate: u32,
        pub(crate) unk_30: u32,
        pub(crate) cur_pstate: u32,
        pub(crate) unk_38: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_3c_0: u32,

        pub(crate) base_pstate_scaled: u32,
        pub(crate) unk_40: u32,
        pub(crate) max_pstate_scaled: u32,
        pub(crate) unk_48: u32,
        pub(crate) min_pstate_scaled: u32,
        pub(crate) freq_mhz: F32,
        pub(crate) unk_54: Array<0x20, u8>,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_74_0: u32,

        pub(crate) sram_k: Array<0x10, F32>,
        pub(crate) unk_b4: Array<0x100, u8>,
        pub(crate) unk_1b4: u32,
        pub(crate) temp_c: u32,
        pub(crate) avg_power_mw: u32,
        pub(crate) update_ts: U64,
        pub(crate) unk_1c8: u32,
        pub(crate) unk_1cc: Array<0x478, u8>,
        pub(crate) pad_644: Pad<0x8>,
        pub(crate) unk_64c: u32,
        pub(crate) unk_650: u32,
        pub(crate) pad_654: u32,
        pub(crate) pwr_filter_a_neg: F32,
        pub(crate) pad_65c: u32,
        pub(crate) pwr_filter_a: F32,
        pub(crate) pad_664: u32,
        pub(crate) pwr_integral_gain: F32,
        pub(crate) pad_66c: u32,
        pub(crate) pwr_integral_min_clamp: F32,
        pub(crate) max_power_1: F32,
        pub(crate) pwr_proportional_gain: F32,
        pub(crate) pad_67c: u32,
        pub(crate) pwr_pstate_related_k: F32,
        pub(crate) pwr_pstate_max_dc_offset: i32,
        pub(crate) unk_688: u32,
        pub(crate) max_pstate_scaled_2: u32,
        pub(crate) pad_690: u32,
        pub(crate) unk_694: u32,
        pub(crate) max_power_2: u32,
        pub(crate) pad_69c: Pad<0x18>,
        pub(crate) unk_6b4: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_6b8_0: Array<0x10, u8>,

        pub(crate) max_pstate_scaled_3: u32,
        pub(crate) unk_6bc: u32,
        pub(crate) pad_6c0: Pad<0x14>,
        pub(crate) ppm_filter_tc_periods_x4: u32,
        pub(crate) unk_6d8: u32,
        pub(crate) pad_6dc: u32,
        pub(crate) ppm_filter_a_neg: F32,
        pub(crate) pad_6e4: u32,
        pub(crate) ppm_filter_a: F32,
        pub(crate) pad_6ec: u32,
        pub(crate) ppm_ki_dt: F32,
        pub(crate) pad_6f4: u32,
        pub(crate) pwr_integral_min_clamp_2: u32,
        pub(crate) unk_6fc: F32,
        pub(crate) ppm_kp: F32,
        pub(crate) pad_704: u32,
        pub(crate) unk_708: u32,
        pub(crate) pwr_min_duty_cycle: u32,
        pub(crate) max_pstate_scaled_4: u32,
        pub(crate) unk_714: u32,
        pub(crate) pad_718: u32,
        pub(crate) unk_71c: F32,
        pub(crate) max_power_3: u32,
        pub(crate) cur_power_mw_2: u32,
        pub(crate) ppm_filter_tc_ms: u32,
        pub(crate) unk_72c: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) ppm_filter_tc_clks: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_730_4: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_730_8: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_730_c: u32,

        pub(crate) unk_730: F32,
        pub(crate) unk_734: u32,
        pub(crate) unk_738: u32,
        pub(crate) unk_73c: u32,
        pub(crate) unk_740: u32,
        pub(crate) unk_744: u32,
        pub(crate) unk_748: Array<0x4, F32>,
        pub(crate) unk_758: u32,
        pub(crate) perf_tgt_utilization: u32,
        pub(crate) pad_760: u32,
        pub(crate) perf_boost_min_util: u32,
        pub(crate) perf_boost_ce_step: u32,
        pub(crate) perf_reset_iters: u32,
        pub(crate) pad_770: u32,
        pub(crate) unk_774: u32,
        pub(crate) unk_778: u32,
        pub(crate) perf_filter_drop_threshold: u32,
        pub(crate) perf_filter_a_neg: F32,
        pub(crate) perf_filter_a2_neg: F32,
        pub(crate) perf_filter_a: F32,
        pub(crate) perf_filter_a2: F32,
        pub(crate) perf_ki: F32,
        pub(crate) perf_ki2: F32,
        pub(crate) perf_integral_min_clamp: F32,
        pub(crate) unk_79c: F32,
        pub(crate) perf_kp: F32,
        pub(crate) perf_kp2: F32,
        pub(crate) boost_state_unk_k: F32,
        pub(crate) base_pstate_scaled_2: u32,
        pub(crate) max_pstate_scaled_5: u32,
        pub(crate) base_pstate_scaled_3: u32,
        pub(crate) pad_7b8: u32,
        pub(crate) perf_cur_utilization: F32,
        pub(crate) perf_tgt_utilization_2: u32,
        pub(crate) pad_7c4: Pad<0x18>,
        pub(crate) unk_7dc: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_7e0_0: Array<0x10, u8>,

        pub(crate) base_pstate_scaled_4: u32,
        pub(crate) pad_7e4: u32,
        pub(crate) unk_7e8: Array<0x14, u8>,
        pub(crate) unk_7fc: F32,
        pub(crate) pwr_min_duty_cycle_2: F32,
        pub(crate) max_pstate_scaled_6: F32,
        pub(crate) max_freq_mhz: u32,
        pub(crate) pad_80c: u32,
        pub(crate) unk_810: u32,
        pub(crate) pad_814: u32,
        pub(crate) pwr_min_duty_cycle_3: u32,
        pub(crate) unk_81c: u32,
        pub(crate) pad_820: u32,
        pub(crate) min_pstate_scaled_4: F32,
        pub(crate) max_pstate_scaled_7: u32,
        pub(crate) unk_82c: u32,
        pub(crate) unk_alpha_neg: F32,
        pub(crate) unk_alpha: F32,
        pub(crate) unk_838: u32,
        pub(crate) unk_83c: u32,
        pub(crate) pad_840: Pad<0x2c>,
        pub(crate) unk_86c: u32,
        pub(crate) fast_die0_sensor_mask: U64,
        #[ver(G >= G14X && G < G15)]
        pub(crate) fast_die1_sensor_mask: U64,
        pub(crate) fast_die0_release_temp_cc: u32,
        pub(crate) unk_87c: i32,
        pub(crate) unk_880: u32,
        pub(crate) unk_884: u32,
        pub(crate) pad_888: u32,
        pub(crate) unk_88c: u32,
        pub(crate) pad_890: u32,
        pub(crate) unk_894: F32,
        pub(crate) pad_898: u32,
        pub(crate) fast_die0_ki_dt: F32,
        pub(crate) pad_8a0: u32,
        pub(crate) unk_8a4: u32,
        pub(crate) unk_8a8: F32,
        pub(crate) fast_die0_kp: F32,
        pub(crate) pad_8b0: u32,
        pub(crate) unk_8b4: u32,
        pub(crate) pwr_min_duty_cycle_4: u32,
        pub(crate) max_pstate_scaled_8: u32,
        pub(crate) max_pstate_scaled_9: u32,
        pub(crate) fast_die0_prop_tgt_delta: u32,
        pub(crate) unk_8c8: u32,
        pub(crate) unk_8cc: u32,
        pub(crate) pad_8d0: Pad<0x14>,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_8e4_0: Array<0x10, u8>,

        pub(crate) unk_8e4: u32,
        pub(crate) unk_8e8: u32,
        pub(crate) max_pstate_scaled_10: u32,
        pub(crate) unk_8f0: u32,
        pub(crate) unk_8f4: u32,
        pub(crate) pad_8f8: u32,
        pub(crate) pad_8fc: u32,
        pub(crate) unk_900: Array<0x24, u8>,

        pub(crate) unk_coef_a1: Array<8, Array<MAX_CORES_PER_CLUSTER::ver, F32>>,
        pub(crate) unk_coef_a2: Array<8, Array<MAX_CORES_PER_CLUSTER::ver, F32>>,

        pub(crate) pad_b24: Pad<0x70>,
        pub(crate) max_pstate_scaled_11: u32,
        pub(crate) freq_with_off: u32,
        pub(crate) unk_b9c: u32,
        pub(crate) unk_ba0: U64,
        pub(crate) unk_ba8: U64,
        pub(crate) unk_bb0: u32,
        pub(crate) unk_bb4: u32,

        #[ver(V >= V13_3)]
        pub(crate) pad_bb8_0: Pad<0x200>,
        #[ver(V >= V13_5)]
        pub(crate) pad_bb8_200: Pad<0x8>,

        pub(crate) pad_bb8: Pad<0x74>,
        pub(crate) unk_c2c: u32,
        pub(crate) power_zone_count: u32,
        pub(crate) max_power_4: u32,
        pub(crate) max_power_5: u32,
        pub(crate) max_power_6: u32,
        pub(crate) unk_c40: u32,
        pub(crate) unk_c44: F32,
        pub(crate) avg_power_target_filter_a_neg: F32,
        pub(crate) avg_power_target_filter_a: F32,
        pub(crate) avg_power_target_filter_tc_x4: u32,
        pub(crate) avg_power_target_filter_tc_xperiod: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) avg_power_target_filter_tc_clks: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_c58_4: u32,

        pub(crate) power_zones: Array<5, PowerZone::ver>,
        pub(crate) avg_power_filter_tc_periods_x4: u32,
        pub(crate) unk_cfc: u32,
        pub(crate) unk_d00: u32,
        pub(crate) avg_power_filter_a_neg: F32,
        pub(crate) unk_d08: u32,
        pub(crate) avg_power_filter_a: F32,
        pub(crate) unk_d10: u32,
        pub(crate) avg_power_ki_dt: F32,
        pub(crate) unk_d18: u32,
        pub(crate) unk_d1c: u32,
        pub(crate) unk_d20: F32,
        pub(crate) avg_power_kp: F32,
        pub(crate) unk_d28: u32,
        pub(crate) unk_d2c: u32,
        pub(crate) avg_power_min_duty_cycle: u32,
        pub(crate) max_pstate_scaled_12: u32,
        pub(crate) max_pstate_scaled_13: u32,
        pub(crate) unk_d3c: u32,
        pub(crate) max_power_7: F32,
        pub(crate) max_power_8: u32,
        pub(crate) unk_d48: u32,
        pub(crate) avg_power_filter_tc_ms: u32,
        pub(crate) unk_d50: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) avg_power_filter_tc_clks: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_d54_4: Array<0xc, u8>,

        pub(crate) unk_d54: Array<0x10, u8>,
        pub(crate) max_pstate_scaled_14: u32,
        pub(crate) unk_d68: Array<0x24, u8>,

        pub(crate) t81xx_data: T81xxData,

        pub(crate) unk_dd0: Array<0x40, u8>,

        #[ver(V >= V13_2)]
        pub(crate) unk_e10_pad: Array<0x10, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_e10_0: HwDataA130Extra,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_e10_0: HwDataA140Extra,

        pub(crate) unk_e10: Array<0xc, u8>,

        // G15 offsets match G13 13.5 up to ~0x1224 and are +0x10 from 0x1288
        // (`fast_die0_sensor_mask_2`, G13 0x1278). TODO: exact insertion point between
        // 0x1224 and 0x1288 is not known; placed as late as possible.
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1278_g15: Array<0x10, u8>,

        pub(crate) fast_die0_sensor_mask_2: U64,
        #[ver(G >= G14X && G < G15)]
        pub(crate) fast_die1_sensor_mask_2: U64,

        pub(crate) unk_e24: u32,
        pub(crate) unk_e28: u32,
        pub(crate) unk_e2c: Pad<0x1c>,
        pub(crate) unk_coef_b1: Array<8, Array<MAX_CORES_PER_CLUSTER::ver, F32>>,
        pub(crate) unk_coef_b2: Array<8, Array<MAX_CORES_PER_CLUSTER::ver, F32>>,

        #[ver(G >= G14X && G < G15)]
        pub(crate) pad_1048_0: Pad<0x600>,

        pub(crate) pad_1048: Pad<0x5e4>,

        pub(crate) fast_die0_sensor_mask_alt: U64,
        #[ver(G >= G14X && G < G15)]
        pub(crate) fast_die1_sensor_mask_alt: U64,
        #[ver(V < V13_0B4)]
        pub(crate) fast_die0_sensor_present: U64,

        pub(crate) unk_163c: u32,

        pub(crate) unk_1640: Array<0x2000, u8>,

        #[ver(G >= G14X && G < G15)]
        pub(crate) unk_3640_0: Array<0x2000, u8>,

        pub(crate) unk_3640: u32,
        pub(crate) unk_3644: u32,
        pub(crate) hws1: HwDataShared1,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_hws2: Array<16, u16>,

        pub(crate) hws2: HwDataShared2,
        pub(crate) unk_3c00: u32,
        pub(crate) unk_3c04: u32,
        pub(crate) hws3: HwDataShared3,
        pub(crate) unk_3c58: Array<0x3c, u8>,
        pub(crate) unk_3c94: u32,
        pub(crate) unk_3c98: U64,
        pub(crate) unk_3ca0: U64,
        pub(crate) unk_3ca8: U64,
        pub(crate) unk_3cb0: U64,
        pub(crate) ts_last_idle: U64,
        pub(crate) ts_last_poweron: U64,
        pub(crate) ts_last_poweroff: U64,
        pub(crate) unk_3cd0: U64,
        pub(crate) unk_3cd8: U64,

        #[ver(V >= V13_0B4)]
        pub(crate) unk_3ce0_0: u32,

        pub(crate) unk_3ce0: u32,
        pub(crate) unk_3ce4: u32,
        pub(crate) unk_3ce8: u32,
        pub(crate) unk_3cec: u32,
        pub(crate) unk_3cf0: u32,
        pub(crate) core_leak_coef: Array<8, F32>,
        pub(crate) sram_leak_coef: Array<8, F32>,

        #[ver(V >= V13_0B4)]
        pub(crate) aux_leak_coef: AuxLeakCoef,
        #[ver(V >= V13_0B4)]
        pub(crate) unk_3d34_0: Array<0x18, u8>,

        pub(crate) unk_3d34: Array<0x38, u8>,

        // G15 tail, 0x422c..0x4360.
        #[ver(V >= V14_8_3)]
        pub(crate) unk_422c: Array<0x20, u8>,
        #[ver(V >= V14_8_3)]
        /// System counter value at init. +0x424c
        /// The runtime backend fills it; the manager leaves it 0, as validated.
        pub(crate) init_timestamp: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_4254: Array<0x40, u8>,
        #[ver(V >= V14_8_3)]
        /// +0x4294/+0x4298
        pub(crate) unk_4294: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_4298: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_429c: Array<0x8, u8>,
        #[ver(V >= V14_8_3)]
        /// Three 8-entry per-cluster tables. +0x42a4
        /// TODO: source values unknown; left 0.
        pub(crate) cluster_tables: Array<3, Array<8, u32>>,
        #[ver(V >= V14_8_3)]
        /// Eight words, source unknown. +0x4304..+0x4320
        pub(crate) unk_4304: Array<8, u32>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_4324: Array<0x1c, u8>,
        #[ver(V >= V14_8_3)]
        /// Read-modify-written at init. +0x4340
        pub(crate) unk_4340: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_4348: Array<0x10, u8>,
        #[ver(V >= V14_8_3)]
        /// GPU keep-alive mode (read by the host). +0x4358
        pub(crate) gpu_keepalive_mode: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_435c: u32,
    }
    #[versions(AGX)]
    default_zeroed!(HwDataA::ver);
    #[versions(AGX)]
    no_debug!(HwDataA::ver);

    #[derive(Debug, Default, Clone, Copy)]
    #[repr(C)]
    pub(crate) struct IOMapping {
        pub(crate) phys_addr: U64,
        pub(crate) virt_addr: U64,
        pub(crate) total_size: u32,
        pub(crate) element_size: u32,
        pub(crate) readwrite: U64,
    }

    #[versions(AGX)]
    const IO_MAPPING_COUNT: usize = {
        #[ver(V < V13_0B4)]
        {
            0x14
        }
        #[ver(V >= V13_0B4 && V < V13_3)]
        {
            0x17
        }
        #[ver(V >= V13_3 && V < V13_5)]
        {
            0x18
        }
        #[ver(V >= V13_5 && V < V14_8_3)]
        {
            0x19
        }
        // 31 IOMapping slots (0x3e0 / 0x20)
        #[ver(V >= V14_8_3)]
        {
            0x1f
        }
    };

    /// A G15 performance-state table: the highest entry, then per entry the frequency (MHz)
    /// and the core and SRAM voltages (mV), eight voltage columns each.
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataBPStateTable {
        pub(crate) max_pstate: u32,
        pub(crate) frequencies: Array<0x10, u32>,
        pub(crate) voltages: Array<0x10, [u32; 0x8]>,
        pub(crate) voltages_sram: Array<0x10, [u32; 0x8]>,
    }

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataBAuxPStates {
        pub(crate) cs_max_pstate: u32,
        pub(crate) cs_frequencies: Array<0x10, u32>,
        pub(crate) cs_voltages: Array<0x10, Array<0x2, u32>>,
        pub(crate) cs_voltages_sram: Array<0x10, Array<0x2, u32>>,
        pub(crate) cs_unkpad: u32,
        pub(crate) afr_max_pstate: u32,
        pub(crate) afr_frequencies: Array<0x8, u32>,
        pub(crate) afr_voltages: Array<0x8, Array<0x2, u32>>,
        pub(crate) afr_voltages_sram: Array<0x8, Array<0x2, u32>>,
        pub(crate) afr_unkpad: u32,
    }

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct HwDataB {
        #[ver(V < V13_0B4)]
        pub(crate) unk_0: U64,

        pub(crate) unk_8: U64,

        #[ver(V < V13_0B4)]
        pub(crate) unk_10: U64,

        pub(crate) unk_18: U64,
        pub(crate) unk_20: U64,
        pub(crate) unk_28: U64,
        pub(crate) unk_30: U64,
        pub(crate) timestamp_area_base: U64,
        #[ver(V < V14_8_3)]
        pub(crate) pad_40: Pad<0x20>,
        #[ver(V >= V14_8_3)]
        /// G15: a pointer. +0x30
        /// TODO: what it points to; left 0.
        pub(crate) unk_30_ptr: U64,

        #[ver(V < V13_0B4)]
        pub(crate) yuv_matrices: Array<0xf, Array<3, Array<4, i16>>>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) yuv_matrices: Array<0x3f, Array<3, Array<4, i16>>>,

        // G15: two tables of 32 CSC matrices (3x4 i16) at 0x38..0x637
        #[ver(V >= V14_8_3)]
        pub(crate) yuv_matrices: Array<0x40, Array<3, Array<4, i16>>>,

        pub(crate) pad_1c8: Pad<0x8>,
        pub(crate) io_mappings: Array<IO_MAPPING_COUNT::ver, IOMapping>,

        #[ver(V >= V13_0B4)]
        pub(crate) sgx_sram_ptr: U64,

        pub(crate) chip_id: u32,
        pub(crate) unk_454: u32,
        pub(crate) unk_458: u32,
        pub(crate) unk_45c: u32,
        pub(crate) unk_460: u32,
        pub(crate) unk_464: u32,
        pub(crate) unk_468: u32,
        pub(crate) unk_46c: u32,
        pub(crate) unk_470: u32,
        pub(crate) unk_474: u32,
        pub(crate) unk_478: u32,
        pub(crate) unk_47c: u32,
        pub(crate) unk_480: u32,
        pub(crate) unk_484: u32,
        pub(crate) unk_488: u32,
        pub(crate) unk_48c: u32,
        pub(crate) base_clock_khz: u32,
        pub(crate) power_sample_period: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pad_498: Pad<0x4>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_49c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4a0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4a4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pad_4a8: Pad<0x4>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4ac: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pad_4b0: Pad<0x8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4b8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4bc: Array<0x4, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4c0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4c4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4c8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4cc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4d0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4d4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4d8: Array<0x4, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4dc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4e0: U64,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4e8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4ec: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4f0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4f4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4f8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4fc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_500: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_504_0: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_504: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_508: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_50c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_510: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_514: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_518: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_51c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_520: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_524: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_528: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_52c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_530: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_534_0: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_534: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_538: u32,

        #[ver(V < V14_8_3)]
        pub(crate) num_frags: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_540: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_544: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_548: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_54c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_550: u32,

        // G15 0xa70..0xb3f. The G13 13.5 0x9b0..0xa73 group,
        // with 12 bytes inserted at an unknown position; the words are named by their G15 offset.
        // TODO: map these to the G13 names (`num_frags`, `unk_524` secure cache flush, ...)
        // once their values are known.
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a70: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a74: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a78: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a7c: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a84: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a88: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a8c: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a90: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a94: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a98: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_a9c: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_aa0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_aa4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_aa8: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_aac: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ab0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ab4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ab8: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_abc: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ac4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ac8: Array<0x4, u32>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ad8: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_adc: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ae0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ae4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_ae8: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_af0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_af4: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_afc: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b00: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b04: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b08: Array<0x4, u32>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b18: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b1c: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b20: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b24: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_b2c: Array<0x14, u8>,

        pub(crate) unk_554: u32,
        pub(crate) uat_ttb_base: U64,
        pub(crate) gpu_core_id: u32,
        pub(crate) gpu_rev_id: u32,
        pub(crate) num_cores: u32,
        pub(crate) max_pstate: u32,

        #[ver(V < V13_0B4)]
        pub(crate) num_pstates: u32,

        pub(crate) frequencies: Array<0x10, u32>,
        pub(crate) voltages: Array<0x10, [u32; 0x8]>,
        pub(crate) voltages_sram: Array<0x10, [u32; 0x8]>,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_9f4_0: Pad<64>,

        #[ver(V >= V14_8_3)]
        /// G15: second 16-entry frequency table in MHz (meaning inferred). +0xf9c
        pub(crate) frequencies_2: Array<0x10, u32>,

        pub(crate) sram_k: Array<0x10, F32>,
        pub(crate) unk_9f4: Array<0x10, u32>,
        pub(crate) rel_max_powers: Array<0x10, u32>,
        pub(crate) rel_boost_freqs: Array<0x10, u32>,

        #[ver(V >= V13_3)]
        pub(crate) unk_arr_0: Array<32, u32>,

        #[ver(V < V13_0B4)]
        pub(crate) min_sram_volt: u32,

        #[ver(V < V13_0B4)]
        pub(crate) unk_ab8: u32,

        #[ver(V < V13_0B4)]
        pub(crate) unk_abc: u32,

        #[ver(V < V13_0B4)]
        pub(crate) unk_ac0: u32,

        #[ver(V >= V13_0B4)]
        pub(crate) aux_ps: HwDataBAuxPStates,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) pad_ac4_0: Array<0x44c, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) pad_ac4: Pad<0x8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_acc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_ad0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pad_ad4: Pad<0x10>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_ae4: Array<0x4, u32>,
        #[ver(V < V14_8_3)]
        pub(crate) pad_af4: Pad<0x4>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_af8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pad_afc: Pad<0x8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b04: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b08: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b0c: u32,

        #[ver(G >= G14X && V < V14_8_3)]
        pub(crate) pad_b10_0: Array<0x8, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_b10: u32,

        // G15 0x134c..0x17eb: a third p-state table at 0x134c, then the
        // words before `timer_offset`. G15 is +4 relative to G13 here (+0xd0 at `timer_offset`).
        #[ver(V >= V14_8_3)]
        /// Third p-state table: the secondary (lowest frequency per voltage) table, with one
        /// voltage column. +0x134c
        pub(crate) aux_ps_3: HwDataBPStateTable,
        #[ver(V >= V14_8_3)]
        pub(crate) aux_ps_3_pad: Array<0xc, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_179c: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17a4: Array<0x10, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17b4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17b8: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17bc: u32,
        #[ver(V >= V14_8_3)]
        /// Unit enable bit masks. +0x17c0/+0x17c8
        pub(crate) unit_mask_a: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unit_mask_b: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17cc: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17d0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17d4: Array<0x10, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17e4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17e8: u32,

        pub(crate) timer_offset: U64,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b1c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b20: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b24: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b28: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b2c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b30: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b34: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_b38_0: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_b38_4: u32,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_b38_8: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_b38: Array<0xc, u32>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b68: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_b6c: Array<0xd0, u8>,

        #[ver(G >= G14X && V < V14_8_3)]
        pub(crate) unk_c3c_0: Array<0x8, u8>,

        #[ver(G < G14X && V >= V13_5 && V < V14_8_3)]
        pub(crate) unk_c3c_8: Array<0x10, u8>,

        #[ver(V >= V13_5 && V < V14_8_3)]
        pub(crate) unk_c3c_18: Array<0x20, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_c3c: u32,

        // G15 0x17f4..0x1867.
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17f4: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_17fc: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1800: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1804: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1808: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_180c: u32,
        #[ver(V >= V14_8_3)]
        /// +0x1810 and +0x1814 are both 1 (written as one 64-bit store); the firmware copies them
        /// to separate globals.
        pub(crate) unk_1810: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1814: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1818: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_181c: u32,
        #[ver(V >= V14_8_3)]
        /// 48 bytes of 0xff ("all units enabled" masks?). +0x1820
        pub(crate) all_ones_masks: Array<0x30, u8>,
        #[ver(V >= V14_8_3)]
        /// FSTP override enable. +0x1850
        pub(crate) fstp_override: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1854: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_185c: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1860: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1864: u32,
    }
    #[versions(AGX)]
    default_zeroed!(HwDataB::ver);

    #[derive(Debug)]
    #[repr(C, packed)]
    pub(crate) struct GpuStatsVtx {
        // This changes all the time and we don't use it, let's just make it a big buffer
        pub(crate) opaque: Array<0x3000, u8>,
    }
    default_zeroed!(GpuStatsVtx);

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct GpuStatsFrag {
        // This changes all the time and we don't use it, let's just make it a big buffer
        // except for these two fields which may need init.
        #[ver(G >= G14X && V < V14_8_3)]
        pub(crate) unk1_0: Array<0x910, u8>,
        // G15: the two -1 fields are at E2+0xc18/+0xc30
        #[ver(V >= V14_8_3)]
        pub(crate) unk1_0: Array<0xb10, u8>,
        pub(crate) unk1: Array<0x100, u8>,
        pub(crate) cur_stamp_id: i32,
        pub(crate) unk2: Array<0x14, u8>,
        pub(crate) unk_id: i32,
        #[ver(V < V14_8_3)]
        pub(crate) unk3: Array<0x1000, u8>,
        // G15 E2 is 0x1248 bytes in total
        #[ver(V >= V14_8_3)]
        pub(crate) unk3: Array<0x614, u8>,
    }

    #[versions(AGX)]
    impl Default for GpuStatsFrag::ver {
        fn default() -> Self {
            Self {
                #[ver(G >= G14X)]
                unk1_0: Default::default(),
                unk1: Default::default(),
                cur_stamp_id: -1,
                unk2: Default::default(),
                unk_id: -1,
                unk3: Default::default(),
            }
        }
    }

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct GpuGlobalStatsVtx {
        pub(crate) total_cmds: u32,
        pub(crate) stats: GpuStatsVtx,
    }
    default_zeroed!(GpuGlobalStatsVtx);

    #[versions(AGX)]
    #[derive(Debug, Default)]
    #[repr(C)]
    pub(crate) struct GpuGlobalStatsFrag {
        pub(crate) total_cmds: u32,
        pub(crate) unk_4: u32,
        pub(crate) stats: GpuStatsFrag::ver,
    }

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct GpuStatsComp {
        // This changes all the time and we don't use it, let's just make it a big buffer
        pub(crate) opaque: Array<0x3000, u8>,
    }
    default_zeroed!(GpuStatsComp);

    // Not part of the 0x490-byte 14.x RuntimePointers, so the G15V14_8_3
    // instantiation is unused.
    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    #[allow(dead_code)]
    pub(crate) struct RuntimeScratch {
        pub(crate) unk_280: Array<0x6800, u8>,
        pub(crate) unk_6a80: u32,
        pub(crate) gpu_idle: u32,
        pub(crate) unkpad_6a88: Pad<0x14>,
        pub(crate) unk_6a9c: u32,
        pub(crate) unk_ctr0: u32,
        pub(crate) unk_ctr1: u32,
        pub(crate) unk_6aa8: u32,
        pub(crate) unk_6aac: u32,
        pub(crate) unk_ctr2: u32,
        pub(crate) unk_6ab4: u32,
        pub(crate) unk_6ab8: u32,
        pub(crate) unk_6abc: u32,
        pub(crate) unk_6ac0: u32,
        pub(crate) unk_6ac4: u32,
        pub(crate) unk_ctr3: u32,
        pub(crate) unk_6acc: u32,
        pub(crate) unk_6ad0: u32,
        pub(crate) unk_6ad4: u32,
        pub(crate) unk_6ad8: u32,
        pub(crate) unk_6adc: u32,
        pub(crate) unk_6ae0: u32,
        pub(crate) unk_6ae4: u32,
        pub(crate) unk_6ae8: u32,
        pub(crate) unk_6aec: u32,
        pub(crate) unk_6af0: u32,
        pub(crate) unk_ctr4: u32,
        pub(crate) unk_ctr5: u32,
        pub(crate) unk_6afc: u32,
        pub(crate) pad_6b00: Pad<0x38>,

        #[ver(G >= G14X)]
        pub(crate) pad_6b00_extra: Array<0x4800, u8>,

        pub(crate) unk_6b38: u32,
        pub(crate) pad_6b3c: Pad<0x84>,
    }
    #[versions(AGX)]
    default_zeroed!(RuntimeScratch::ver);

    #[versions(AGX)]
    #[repr(C)]
    pub(crate) struct RuntimePointers<'a> {
        // G15 / 14.8.3: 0x490 bytes, packed (HwDataA pointer at +0x441). GpuPointer/U64 are
        // packed(1), so repr(C) gives the firmware offsets.
        #[ver(V >= V14_8_3)]
        /// +0x000
        pub(crate) hwdata_b: GpuPointer<'a, super::HwDataB::ver>,
        #[ver(V >= V14_8_3)]
        /// Hardware-erratum (BRN) table. +0x008
        pub(crate) brn_table: GpuPointer<'a, &'a [u8]>,
        #[ver(V >= V14_8_3)]
        /// E7 (0x88 bytes, zeroed). +0x010
        pub(crate) unkptr_e7: GpuPointer<'a, &'a [u8]>,

        pub(crate) pipes: Array<4, PipeChannels::ver>,

        #[ver(V < V14_8_3)]
        pub(crate) device_control:
            ChannelRing<channels::ChannelState, channels::DeviceControlMsg::ver>,
        #[ver(V >= V14_8_3)]
        /// +0x198
        pub(crate) device_control: ScratchRingDesc,
        pub(crate) event: ChannelRing<channels::ChannelState, channels::RawEventMsg>,
        pub(crate) fw_log: ChannelRing<channels::FwLogChannelState, channels::RawFwLogMsg>,
        pub(crate) ktrace: ChannelRing<channels::ChannelState, channels::RawKTraceMsg>,
        pub(crate) stats: ChannelRing<channels::ChannelState, channels::RawStatsMsg::ver>,

        #[ver(V >= V14_8_3)]
        /// FWLog payload buffer (0x600 x 0xd8). +0x1f8
        pub(crate) fwlog_buf: Option<GpuWeakPointer<[channels::RawFwLogPayloadMsg]>>,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_200: Pad<0x30>,
        #[ver(V >= V14_8_3)]
        /// 0 at init. +0x230
        pub(crate) unk_230: u32,

        #[ver(V < V14_8_3)]
        pub(crate) __pad0: Pad<0x50>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_160: U64,
        #[ver(V < V14_8_3)]
        pub(crate) unk_168: U64,
        pub(crate) stats_vtx: GpuPointer<'a, super::GpuGlobalStatsVtx>,
        pub(crate) stats_frag: GpuPointer<'a, super::GpuGlobalStatsFrag::ver>,
        pub(crate) stats_comp: GpuPointer<'a, super::GpuStatsComp>,

        #[ver(V >= V14_8_3)]
        /// E5 (0x60 bytes, zeroed; FW status word at +0). +0x24c
        pub(crate) unkptr_e5: GpuPointer<'a, &'a [u8]>,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_254: Pad<0x54>,
        #[ver(V >= V14_8_3)]
        /// Optional pointer, left 0. +0x2a8
        pub(crate) unkptr_2a8: U64,
        #[ver(V >= V14_8_3)]
        /// PB descriptor table GPU/FW VA. +0x2b0/+0x2b8
        pub(crate) buffer_mgr_ctl_gpu_addr: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) buffer_mgr_ctl_fw_addr: U64,
        #[ver(V >= V14_8_3)]
        /// UMA page-pool descriptor table GPU/FW VA. +0x2c0/+0x2c8
        pub(crate) uma_pool_desc_gpu_addr: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) uma_pool_desc_fw_addr: U64,
        #[ver(V >= V14_8_3)]
        /// Configuration words, left 0. +0x2d0/+0x2d4
        pub(crate) unk_2d0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_2d4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_2d8: Pad<0xd8>,
        #[ver(V >= V14_8_3)]
        /// 0xff at init (context-switch histogram control). +0x3b0
        pub(crate) cswitch_hist_ctl: u8,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_3b1: Array<0x90, u8>,

        // G15: +0x441 (unaligned)
        pub(crate) hwdata_a: GpuPointer<'a, super::HwDataA::ver>,

        #[ver(V >= V14_8_3)]
        /// 16 configuration bytes, left 0. +0x449
        pub(crate) unk_449: Array<0x10, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_459: Pad<0x14>,
        #[ver(V >= V14_8_3)]
        /// Zero at init. +0x46d
        pub(crate) unk_46d: Array<0x20, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_48d: Pad<0x3>,
        #[ver(V < V14_8_3)]
        pub(crate) unkptr_190: GpuPointer<'a, &'a [u8]>,
        #[ver(V < V14_8_3)]
        pub(crate) unkptr_198: GpuPointer<'a, &'a [u8]>,
        #[ver(V < V14_8_3)]
        pub(crate) hwdata_b: GpuPointer<'a, super::HwDataB::ver>,
        #[ver(V < V14_8_3)]
        pub(crate) hwdata_b_2: GpuPointer<'a, super::HwDataB::ver>,
        #[ver(V < V14_8_3)]
        pub(crate) fwlog_buf: Option<GpuWeakPointer<[channels::RawFwLogPayloadMsg]>>,
        #[ver(V < V14_8_3)]
        pub(crate) unkptr_1b8: GpuPointer<'a, &'a [u8]>,

        #[ver(G < G14X && V < V14_8_3)]
        pub(crate) unkptr_1c0: GpuPointer<'a, &'a [u8]>,
        #[ver(G < G14X && V < V14_8_3)]
        pub(crate) unkptr_1c8: GpuPointer<'a, &'a [u8]>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_1d0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_1d4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_1d8: Array<0x3c, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) buffer_mgr_ctl_gpu_addr: U64,
        #[ver(V < V14_8_3)]
        pub(crate) buffer_mgr_ctl_fw_addr: U64,
        #[ver(V < V14_8_3)]
        pub(crate) __pad1: Pad<0x5c>,
        #[ver(V < V14_8_3)]
        pub(crate) gpu_scratch: RuntimeScratch::ver,
    }
    #[versions(AGX)]
    no_debug!(RuntimePointers::ver<'_>);

    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct PendingStamp {
        pub(crate) info: AtomicU32,
        pub(crate) wait_value: AtomicU32,
    }
    default_zeroed!(PendingStamp);

    #[derive(Debug, Clone, Copy)]
    #[repr(C, packed)]
    pub(crate) struct FaultInfo {
        pub(crate) unk_0: u32,
        pub(crate) unk_4: u32,
        pub(crate) queue_uuid: u32,
        pub(crate) unk_c: u32,
        pub(crate) unk_10: u32,
        pub(crate) unk_14: u32,
    }
    default_zeroed!(FaultInfo);

    #[derive(Debug, Clone, Copy)]
    #[repr(C)]
    pub(crate) struct PowerZoneGlobal {
        pub(crate) target: u32,
        pub(crate) target_off: u32,
        pub(crate) filter_tc: u32,
    }
    default_zeroed!(PowerZoneGlobal);

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct Globals {
        // G15 / 14.8.3 layout: 0xe00 bytes, cached, packed.
        // It re-packs the G13 field groups; the large G13 arrays are gone.
        #[ver(V >= V14_8_3)]
        /// Timer context-switch mode, 0 or 7. +0x0
        pub(crate) timer_cswitch: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_4: u32,
        #[ver(V >= V14_8_3)]
        /// +0x8
        pub(crate) dm_pause_mode: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_c: u32,
        #[ver(V >= V14_8_3)]
        /// +0x10
        pub(crate) dm_pause_timer: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_14: u32,
        #[ver(V >= V14_8_3)]
        /// +0x18
        pub(crate) disable_cswitch: u32,
        #[ver(V >= V14_8_3)]
        /// Configuration word. +0x1c
        pub(crate) unk_1c: u32,
        #[ver(V >= V14_8_3)]
        /// +0x20/+0x24
        pub(crate) relaxed_cl_kill_timeout: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) frg_task_timeout: u32,
        #[ver(V >= V14_8_3)]
        /// G13 `unk_24`. +0x28
        pub(crate) unk_28: u32,
        #[ver(V >= V14_8_3)]
        /// Debug flags bit 0 (G13 `debug`). +0x2c
        pub(crate) debug: u32,
        #[ver(V >= V14_8_3)]
        /// +0x30
        pub(crate) smart_idle_off_enable: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_34: u32,
        #[ver(V >= V14_8_3)]
        /// Constant 0x78. +0x38
        pub(crate) unk_38: u32,
        #[ver(V >= V14_8_3)]
        /// +0x3c/+0x40
        pub(crate) cpms_window_size: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) cpms_tfca_size: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_44: u32,
        #[ver(V >= V14_8_3)]
        /// Kick channel QoS pair. +0x48/+0x4c
        pub(crate) kick_channel_qos_b: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) kick_channel_qos_a: u32,
        #[ver(V >= V14_8_3)]
        /// G13 `unk_54`/`unk_56`/`unk_58` moved -0x14. +0x50..+0x54 (inferred)
        pub(crate) unk_50: u16,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_52: u16,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_54: u16,
        #[ver(V >= V14_8_3)]
        /// G13 `unk_5a`/`unk_5e`/`unk_62` moved -0x14 (unaligned). +0x56..+0x5e
        pub(crate) unk_56: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_5a: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) cswitch_timer_multiplier: U32,
        #[ver(V >= V14_8_3)]
        /// +0x62 (G13 `unk_66`, tentative)
        pub(crate) cdm_cs_mode_change: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_66: Array<0xa, u8>,
        #[ver(V >= V14_8_3)]
        /// Command submission enable; must be non-zero to submit work. +0x70
        pub(crate) command_submission_enabled: u32,
        #[ver(V >= V14_8_3)]
        /// Pending-submission counter (G13 `pending_submissions`). +0x74
        pub(crate) pending_submissions: AtomicU32,
        #[ver(V >= V14_8_3)]
        /// +0x78..+0x80
        pub(crate) power_if_target: Array<3, u32>,
        #[ver(V >= V14_8_3)]
        /// +0x84..+0x8c
        pub(crate) controller_override_0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) controller_override_1: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) perf_state_cap: u32,
        #[ver(V >= V14_8_3)]
        /// Perf-controller block. +0x90..+0xc7
        pub(crate) perf_controller: Array<0x38, u8>,
        #[ver(V >= V14_8_3)]
        /// +0xc8/+0xcc
        pub(crate) deadline_control_effort: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) deadline_controller_override: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_d0: Array<0xa0, u8>,
        #[ver(V >= V14_8_3)]
        /// DPE (dynamic power estimation) block. +0x170..+0x69f
        pub(crate) dpe: Array<0x530, u8>,
        #[ver(V >= V14_8_3)]
        /// +0x6a0
        pub(crate) dpe_sl_config: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_6a8: Array<0x100, u8>,
        #[ver(V >= V14_8_3)]
        /// Smart idle-off config. +0x7a8
        pub(crate) smart_idle_off_cfg: Array<10, u32>,
        #[ver(V >= V14_8_3)]
        /// +0x7d0..+0x7dc
        pub(crate) ut_engagement: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) clvr_engagement: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) keepalive_perf_threshold_rd: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) keepalive_off_threshold_rd: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_7e0: u32,
        #[ver(V >= V14_8_3)]
        /// Register overrides. +0x7e4
        pub(crate) reg_overrides: Array<16, RegisterOverride>,
        #[ver(V >= V14_8_3)]
        /// +0x964
        pub(crate) reg_override_count: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_968: u32,
        #[ver(V >= V14_8_3)]
        /// Display power-management params. +0x96c/+0x974
        pub(crate) dpm_param_a: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) dpm_param_b: U64,
        #[ver(V >= V14_8_3)]
        /// +0x97c..+0x984
        pub(crate) progress_check_interval_3d: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) progress_check_interval_ta: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) progress_check_interval_cl: u32,
        #[ver(V >= V14_8_3)]
        /// +0x988..+0x99c: the G13 `unk_1102c_0`..`unk_1102c` group (inferred)
        pub(crate) unk_1102c_0: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1102c_4: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1102c_8: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1102c_c: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1102c_10: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_1102c: u32,
        #[ver(V >= V14_8_3)]
        /// +0x9a0..+0x9a8
        pub(crate) idle_off_delay_ms: AtomicU32,
        #[ver(V >= V14_8_3)]
        pub(crate) fender_idle_off_delay_ms: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) fw_early_wake_timeout_ms: u32,
        #[ver(V >= V14_8_3)]
        /// +0x9ac
        pub(crate) gvdm_timer_interval: u32,
        #[ver(V >= V14_8_3)]
        /// +0x9b0/+0x9b4
        pub(crate) cl_context_switch_timeout_ms: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) cl_kill_timeout_ms: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_9b8: u32,
        #[ver(V >= V14_8_3)]
        /// +0x9bc
        pub(crate) cdm_backoff_timeout: u8,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_9bd: Array<0x4, u8>,
        #[ver(V >= V14_8_3)]
        /// +0x9c1/+0x9c3
        pub(crate) fwutil_default_fab_pstate: Array<0x2, u8>,
        #[ver(V >= V14_8_3)]
        pub(crate) fwutil_timer_period: u8,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_9c4: Array<0x19, u8>,
        #[ver(V >= V14_8_3)]
        /// Unaligned. +0x9dd
        pub(crate) mtr_sensor_ptd_override_mask: U64,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_9e5: Array<0x400, u8>,
        #[ver(V >= V14_8_3)]
        /// Read by the host when it creates command queues. +0xde5
        pub(crate) unk_de5: U32,
        #[ver(V >= V14_8_3)]
        /// Keep-alive overrides/thresholds. +0xde9..+0xdf5
        pub(crate) gpu_keepalive_override: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) gfxc_keepalive_override: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) keepalive_perf_threshold: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) keepalive_off_threshold: U32,
        #[ver(V >= V14_8_3)]
        /// Soft-fault settings (possibly G13 `fault_control`). +0xdf9
        pub(crate) soft_fault_settings: U32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_dfd: Array<0x3, u8>,
        #[ver(V >= V14_8_3)]
        /// The G13 pending-stamp table is not in the 0xe00-byte G15 Globals. This
        /// zero-length field keeps `GpuManager::mark_pending_events()` compiling as a no-op.
        /// TODO: find the G15 pending-stamp table (possibly in P0 0x0008..0x4017).
        pub(crate) pending_stamps: Array<0, PendingStamp>,

        #[ver(V < V14_8_3)]
        pub(crate) ktrace_enable: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_4: Array<0x20, u8>,

        #[ver(V >= V13_2 && V < V14_8_3)]
        pub(crate) unk_24_0: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_24: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) debug: u32,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_28_4: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_28: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_2c_0: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_2c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_30: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_34: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_38: Array<0x1c, u8>,

        // pub(crate) sub: GlobalsSub::ver,
        #[ver(V < V14_8_3)]
        pub(crate) unk_54: u16,
        #[ver(V < V14_8_3)]
        pub(crate) unk_56: u16,
        #[ver(V < V14_8_3)]
        pub(crate) unk_58: u16,
        #[ver(V < V14_8_3)]
        pub(crate) unk_5a: U32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_5e: U32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_62: U32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_66_0: Array<0xc, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_66: U32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_6a: Array<0x16, u8>,
        // end GlobalsSub::ver
        #[ver(V < V14_8_3)]
        pub(crate) unk_80: Array<0xf80, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_1000: Array<0x7000, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_8000: Array<0x900, u8>,

        #[ver(G >= G14X && V < V14_8_3)]
        pub(crate) unk_8900_pad: Array<0x484c, u8>,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_8900_pad2: Array<0x54, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_8900: u32,
        #[ver(V < V14_8_3)]
        pub(crate) pending_submissions: AtomicU32,
        #[ver(V < V14_8_3)]
        pub(crate) max_power: u32,
        #[ver(V < V14_8_3)]
        pub(crate) max_pstate_scaled: u32,
        #[ver(V < V14_8_3)]
        pub(crate) max_pstate_scaled_2: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_8914: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_8918: u32,
        #[ver(V < V14_8_3)]
        pub(crate) max_pstate_scaled_3: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_8920: u32,
        #[ver(V < V14_8_3)]
        pub(crate) power_zone_count: u32,
        #[ver(V < V14_8_3)]
        pub(crate) avg_power_filter_tc_periods: u32,
        #[ver(V < V14_8_3)]
        pub(crate) avg_power_ki_dt: F32,
        #[ver(V < V14_8_3)]
        pub(crate) avg_power_kp: F32,
        #[ver(V < V14_8_3)]
        pub(crate) avg_power_min_duty_cycle: u32,
        #[ver(V < V14_8_3)]
        pub(crate) avg_power_target_filter_tc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) power_zones: Array<5, PowerZoneGlobal>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_8978: Array<0x44, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_89bc_0: Array<0x3c, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_89bc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) fast_die0_release_temp: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_89c4: i32,
        #[ver(V < V14_8_3)]
        pub(crate) fast_die0_prop_tgt_delta: u32,
        #[ver(V < V14_8_3)]
        pub(crate) fast_die0_kp: F32,
        #[ver(V < V14_8_3)]
        pub(crate) fast_die0_ki_dt: F32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_89d4: Array<0xc, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_89e0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) max_power_2: u32,
        #[ver(V < V14_8_3)]
        pub(crate) ppm_kp: F32,
        #[ver(V < V14_8_3)]
        pub(crate) ppm_ki_dt: F32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_89f0: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_89f4_0: Array<0x8, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_89f4_8: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_89f4_c: Array<0x50, u8>,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_89f4_5c: Array<0xc, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_89f4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) hws1: HwDataShared1,
        #[ver(V < V14_8_3)]
        pub(crate) hws2: HwDataShared2,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) idle_off_standby_timer: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_hws2_4: Array<0x8, F32>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_hws2_24: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_hws2_28: u32,

        #[ver(V < V14_8_3)]
        pub(crate) hws3: HwDataShared3,
        #[ver(V < V14_8_3)]
        pub(crate) unk_9004: Array<8, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_900c: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_9010_0: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_9010_4: Array<0x14, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) unk_9010: Array<0x2c, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_903c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_9040: Array<0xc0, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_9100: Array<0x6f00, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_10000: Array<0xe50, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_10e50: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_10e54: Array<0x2c, u8>,

        #[ver(((G >= G14X && V < V13_3) || (G <= G14 && V >= V13_3)) && V < V14_8_3)]
        pub(crate) unk_x_pad: Array<0x4, u8>,

        // bit 0: sets sgx_reg 0x17620
        // bit 1: sets sgx_reg 0x17630
        #[ver(V < V14_8_3)]
        pub(crate) fault_control: u32,
        #[ver(V < V14_8_3)]
        pub(crate) do_init: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_10e88: Array<0x188, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) idle_ts: U64,
        #[ver(V < V14_8_3)]
        pub(crate) idle_unk: U64,
        #[ver(V < V14_8_3)]
        pub(crate) progress_check_interval_3d: u32,
        #[ver(V < V14_8_3)]
        pub(crate) progress_check_interval_ta: u32,
        #[ver(V < V14_8_3)]
        pub(crate) progress_check_interval_cl: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_1102c_0: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_1102c_4: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_1102c_8: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_1102c_c: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_1102c_10: u32,

        #[ver(V < V14_8_3)]
        pub(crate) unk_1102c: u32,
        #[ver(V < V14_8_3)]
        pub(crate) idle_off_delay_ms: AtomicU32,
        #[ver(V < V14_8_3)]
        pub(crate) fender_idle_off_delay_ms: u32,
        #[ver(V < V14_8_3)]
        pub(crate) fw_early_wake_timeout_ms: u32,
        #[ver(V == V13_3 && V < V14_8_3)]
        pub(crate) ps_pad_0: Pad<0x8>,
        #[ver(V < V14_8_3)]
        pub(crate) pending_stamps: Array<0x100, PendingStamp>,
        #[ver(V != V13_3 && V < V14_8_3)]
        pub(crate) ps_pad_0: Pad<0x8>,
        #[ver(V < V14_8_3)]
        pub(crate) unkpad_ps: Pad<0x78>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_117bc: u32,
        #[ver(V < V14_8_3)]
        pub(crate) fault_info: FaultInfo,
        #[ver(V < V14_8_3)]
        pub(crate) counter: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_118dc: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_118e0_0: Array<0x9c, u8>,

        #[ver(G >= G14X && V < V14_8_3)]
        pub(crate) unk_118e0_9c: Array<0x580, u8>,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_118e0_9c_x: Array<0x8, u8>,

        #[ver(V < V14_8_3)]
        pub(crate) cl_context_switch_timeout_ms: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) cl_kill_timeout_ms: u32,

        #[ver(V < V14_8_3)]
        pub(crate) cdm_context_store_latency_threshold: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_118e8: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_118ec: Array<0x400, u8>,
        #[ver(V < V14_8_3)]
        pub(crate) unk_11cec: Array<0x54, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_11d40: Array<0x19c, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_11edc: u32,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_11ee0: Array<0x1c, u8>,

        #[ver(V >= V13_0B4 && V < V14_8_3)]
        pub(crate) unk_11efc: u32,

        #[ver(V >= V13_3 && V < V14_8_3)]
        pub(crate) unk_11f00: Array<0x280, u8>,
    }
    #[versions(AGX)]
    default_zeroed!(Globals::ver);

    #[derive(Debug, Default, Clone, Copy)]
    #[repr(C, packed)]
    pub(crate) struct UatLevelInfo {
        pub(crate) unk_3: u8,
        pub(crate) unk_1: u8,
        pub(crate) unk_2: u8,
        pub(crate) index_shift: u8,
        pub(crate) num_entries: u16,
        pub(crate) unk_4: u16,
        pub(crate) unk_8: U64,
        pub(crate) unk_10: U64,
        pub(crate) index_mask: U64,
    }

    #[versions(AGX)]
    #[derive(Debug)]
    #[repr(C)]
    pub(crate) struct InitData<'a> {
        #[ver(V >= V13_0B4)]
        pub(crate) ver_info: Array<0x4, u16>,

        pub(crate) unk_buf: GpuPointer<'a, &'a [u8]>,
        pub(crate) unk_8: u32,
        pub(crate) unk_c: u32,
        pub(crate) runtime_pointers: GpuPointer<'a, super::RuntimePointers::ver>,
        pub(crate) globals: GpuPointer<'a, super::Globals::ver>,
        #[ver(V < V14_8_3)]
        pub(crate) fw_status: GpuPointer<'a, super::FwStatus>,
        #[ver(V >= V14_8_3)]
        /// G15: no FwStatus pointer; u32 0 then u32 1 (possibly a host-mapped FW allocations
        /// flag). +0x28/+0x2c
        pub(crate) unk_28: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) unk_2c: u32,
        // 0x30..0x97 on G15 is a 0x68-byte copy of the UAT pmap descriptor (+0x18..+0x7f). Its
        // first 0x64 bytes have the G13 layout (3 levels, 0x20-byte records).
        pub(crate) uat_page_size: u16,
        pub(crate) uat_page_bits: u8,
        pub(crate) uat_num_levels: u8,
        pub(crate) uat_level_info: Array<0x3, UatLevelInfo>,
        #[ver(V < V14_8_3)]
        pub(crate) __pad0: Pad<0x14>,
        #[ver(V < V14_8_3)]
        pub(crate) host_mapped_fw_allocations: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_ac: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b0: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b4: u32,
        #[ver(V < V14_8_3)]
        pub(crate) unk_b8: u32,

        #[ver(V >= V14_8_3)]
        /// G15: last 4 bytes of the pmap descriptor copy (descriptor +0x7c).
        /// TODO: meaning unknown; left 0.
        pub(crate) unk_94: u32,
        #[ver(V >= V14_8_3)]
        pub(crate) __pad_98: Pad<0x10>,
        #[ver(V >= V14_8_3)]
        /// P2 debug block (0x20). +0xa8
        pub(crate) debug_block: GpuPointer<'a, super::DebugBlock>,
        #[ver(V >= V14_8_3)]
        /// P0 status block (0xc3d0). +0xb0
        pub(crate) status_block: GpuPointer<'a, super::StatusBlock>,
        #[ver(V >= V14_8_3)]
        /// P1 power-controller block (0x238). +0xb8
        pub(crate) power_ctl_block: GpuPointer<'a, super::PowerCtlBlock>,
    }
}

#[derive(Debug)]
pub(crate) struct ChannelRing<T: GpuStruct + Debug + Default, U: Copy>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug,
{
    pub(crate) state: GpuObject<T>,
    pub(crate) ring: GpuArray<U>,
}

impl<T: GpuStruct + Debug + Default, U: Copy> ChannelRing<T, U>
where
    for<'a> <T as GpuStruct>::Raw<'a>: Debug,
{
    pub(crate) fn to_raw(&self) -> raw::ChannelRing<T, U> {
        raw::ChannelRing {
            state: Some(self.state.weak_pointer()),
            ring: Some(self.ring.weak_pointer()),
        }
    }
}

trivial_gpustruct!(FwStatus);
trivial_gpustruct!(StatusBlock);
trivial_gpustruct!(PowerCtlBlock);
trivial_gpustruct!(DebugBlock);
trivial_gpustruct!(GpuGlobalStatsVtx);
#[versions(AGX)]
trivial_gpustruct!(GpuGlobalStatsFrag::ver);
trivial_gpustruct!(GpuStatsComp);

#[versions(AGX)]
trivial_gpustruct!(HwDataA::ver);

#[versions(AGX)]
trivial_gpustruct!(HwDataB::ver);

#[versions(AGX)]
#[derive(Debug)]
pub(crate) struct Stats {
    pub(crate) vtx: GpuObject<GpuGlobalStatsVtx>,
    pub(crate) frag: GpuObject<GpuGlobalStatsFrag::ver>,
    pub(crate) comp: GpuObject<GpuStatsComp>,
}

#[versions(AGX)]
#[derive(Debug)]
pub(crate) struct RuntimePointers {
    pub(crate) stats: Stats::ver,

    pub(crate) hwdata_a: GpuObject<HwDataA::ver>,
    #[ver(V < V14_8_3)]
    pub(crate) unkptr_190: GpuArray<u8>,
    #[ver(V < V14_8_3)]
    pub(crate) unkptr_198: GpuArray<u8>,
    pub(crate) hwdata_b: GpuObject<HwDataB::ver>,

    #[ver(V < V14_8_3)]
    pub(crate) unkptr_1b8: GpuArray<u8>,
    #[ver(V < V14_8_3)]
    pub(crate) unkptr_1c0: GpuArray<u8>,
    #[ver(V < V14_8_3)]
    pub(crate) unkptr_1c8: GpuArray<u8>,

    // G15 group-A regions referenced from RuntimePointers
    #[ver(V >= V14_8_3)]
    pub(crate) brn_table: GpuArray<u8>,
    #[ver(V >= V14_8_3)]
    pub(crate) unkptr_e7: GpuArray<u8>,
    #[ver(V >= V14_8_3)]
    pub(crate) unkptr_e5: GpuArray<u8>,
    #[ver(V >= V14_8_3)]
    pub(crate) uma_pool_desc: GpuArray<u8>,

    pub(crate) buffer_mgr_ctl: gem::ObjectRef,
    pub(crate) buffer_mgr_ctl_low_mapping: Option<mmu::KernelMapping>,
    pub(crate) buffer_mgr_ctl_high_mapping: Option<mmu::KernelMapping>,
}

#[versions(AGX)]
impl GpuStruct for RuntimePointers::ver {
    type Raw<'a> = raw::RuntimePointers::ver<'a>;
}

#[versions(AGX)]
trivial_gpustruct!(Globals::ver);

#[versions(AGX)]
#[derive(Debug)]
pub(crate) struct InitData {
    pub(crate) unk_buf: GpuArray<u8>,
    pub(crate) runtime_pointers: GpuObject<RuntimePointers::ver>,
    pub(crate) globals: GpuObject<Globals::ver>,
    #[ver(V < V14_8_3)]
    pub(crate) fw_status: GpuObject<FwStatus>,
    #[ver(V >= V14_8_3)]
    /// G15: the P0 status block, which embeds the FwStatus FWCtl ring and halt flags. It keeps
    /// the `fw_status` name so the FwStatus users see the same fields.
    pub(crate) fw_status: GpuObject<StatusBlock>,
    #[ver(V >= V14_8_3)]
    pub(crate) power_ctl_block: GpuObject<PowerCtlBlock>,
    #[ver(V >= V14_8_3)]
    pub(crate) debug_block: GpuObject<DebugBlock>,
}

#[versions(AGX)]
impl GpuStruct for InitData::ver {
    type Raw<'a> = raw::InitData::ver<'a>;
}

// G15 / 14.8.3 firmware structure layout checks: the sizes and offsets the firmware expects.
mod g15_layout {
    use super::channels;
    use super::raw::*;
    use core::mem::{offset_of, size_of};
    use kernel::static_assert;

    // InitData, 0xc0 bytes used
    static_assert!(size_of::<InitDataG15V14_8_3<'static>>() == 0xc0);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, unk_buf) == 0x08);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, runtime_pointers) == 0x18);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, globals) == 0x20);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, unk_2c) == 0x2c);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, uat_page_size) == 0x30);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, debug_block) == 0xa8);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, status_block) == 0xb0);
    static_assert!(offset_of!(InitDataG15V14_8_3<'static>, power_ctl_block) == 0xb8);

    // host->FW ring descriptor
    static_assert!(size_of::<ScratchRingDesc>() == 0x20);
    static_assert!(size_of::<PipeChannelsG15V14_8_3>() == 0x60);

    // RuntimePointers: 0x490 bytes, packed
    static_assert!(size_of::<RuntimePointersG15V14_8_3<'static>>() == 0x490);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, hwdata_b) == 0x000);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, brn_table) == 0x008);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unkptr_e7) == 0x010);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, pipes) == 0x018);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, device_control) == 0x198);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, event) == 0x1b8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, fw_log) == 0x1c8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, ktrace) == 0x1d8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, stats) == 0x1e8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, fwlog_buf) == 0x1f8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unk_230) == 0x230);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, stats_vtx) == 0x234);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, stats_frag) == 0x23c);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, stats_comp) == 0x244);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unkptr_e5) == 0x24c);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unkptr_2a8) == 0x2a8);
    static_assert!(
        offset_of!(RuntimePointersG15V14_8_3<'static>, buffer_mgr_ctl_gpu_addr) == 0x2b0
    );
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, buffer_mgr_ctl_fw_addr) == 0x2b8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, uma_pool_desc_gpu_addr) == 0x2c0);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, uma_pool_desc_fw_addr) == 0x2c8);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unk_2d0) == 0x2d0);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, cswitch_hist_ctl) == 0x3b0);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, hwdata_a) == 0x441);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unk_449) == 0x449);
    static_assert!(offset_of!(RuntimePointersG15V14_8_3<'static>, unk_46d) == 0x46d);

    // Globals: 0xe00 bytes, packed
    static_assert!(size_of::<GlobalsG15V14_8_3>() == 0xe00);
    static_assert!(size_of::<RegisterOverride>() == 0x18);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_28) == 0x28);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_38) == 0x38);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_50) == 0x50);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_56) == 0x56);
    static_assert!(offset_of!(GlobalsG15V14_8_3, cswitch_timer_multiplier) == 0x5e);
    static_assert!(offset_of!(GlobalsG15V14_8_3, command_submission_enabled) == 0x70);
    static_assert!(offset_of!(GlobalsG15V14_8_3, pending_submissions) == 0x74);
    static_assert!(offset_of!(GlobalsG15V14_8_3, perf_controller) == 0x90);
    static_assert!(offset_of!(GlobalsG15V14_8_3, deadline_control_effort) == 0xc8);
    static_assert!(offset_of!(GlobalsG15V14_8_3, dpe) == 0x170);
    static_assert!(offset_of!(GlobalsG15V14_8_3, dpe_sl_config) == 0x6a0);
    static_assert!(offset_of!(GlobalsG15V14_8_3, smart_idle_off_cfg) == 0x7a8);
    static_assert!(offset_of!(GlobalsG15V14_8_3, ut_engagement) == 0x7d0);
    static_assert!(offset_of!(GlobalsG15V14_8_3, reg_overrides) == 0x7e4);
    static_assert!(offset_of!(GlobalsG15V14_8_3, reg_override_count) == 0x964);
    static_assert!(offset_of!(GlobalsG15V14_8_3, dpm_param_a) == 0x96c);
    static_assert!(offset_of!(GlobalsG15V14_8_3, progress_check_interval_3d) == 0x97c);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_1102c_0) == 0x988);
    static_assert!(offset_of!(GlobalsG15V14_8_3, idle_off_delay_ms) == 0x9a0);
    static_assert!(offset_of!(GlobalsG15V14_8_3, fender_idle_off_delay_ms) == 0x9a4);
    static_assert!(offset_of!(GlobalsG15V14_8_3, fw_early_wake_timeout_ms) == 0x9a8);
    static_assert!(offset_of!(GlobalsG15V14_8_3, cl_context_switch_timeout_ms) == 0x9b0);
    static_assert!(offset_of!(GlobalsG15V14_8_3, cdm_backoff_timeout) == 0x9bc);
    static_assert!(offset_of!(GlobalsG15V14_8_3, fwutil_default_fab_pstate) == 0x9c1);
    static_assert!(offset_of!(GlobalsG15V14_8_3, mtr_sensor_ptd_override_mask) == 0x9dd);
    static_assert!(offset_of!(GlobalsG15V14_8_3, unk_de5) == 0xde5);
    static_assert!(offset_of!(GlobalsG15V14_8_3, soft_fault_settings) == 0xdf9);

    // P0 status block
    static_assert!(size_of::<StatusBlock>() == 0xc3d0);
    static_assert!(offset_of!(StatusBlock, fw_recovery_info_a) == 0x40a0);
    static_assert!(offset_of!(StatusBlock, recovery_packet_info) == 0x44a0);
    static_assert!(offset_of!(StatusBlock, unk_44c0) == 0x44c0);
    static_assert!(offset_of!(StatusBlock, unk_4558) == 0x4558);
    static_assert!(offset_of!(StatusBlock, fwctl_channel) == 0x4568);
    static_assert!(offset_of!(StatusBlock, flags) == 0x4580);
    static_assert!(offset_of!(StatusBlock, flags.halted) == 0x4590);
    static_assert!(offset_of!(StatusBlock, flags.resume) == 0x45a0);
    static_assert!(offset_of!(StatusBlock, flags.unk_40) == 0x45b0);
    static_assert!(offset_of!(StatusBlock, flags.sched_callbacks) == 0x45c4);
    static_assert!(offset_of!(StatusBlock, qos_mode) == 0xc3c8);

    // P1 power-controller block
    static_assert!(size_of::<PowerCtlBlock>() == 0x238);
    static_assert!(offset_of!(PowerCtlBlock, power_zone_count) == 0x0c);
    static_assert!(offset_of!(PowerCtlBlock, power_zones) == 0x24);
    static_assert!(offset_of!(PowerCtlBlock, clvr_loop_override_a) == 0x60);
    static_assert!(offset_of!(PowerCtlBlock, lifetime_servo_temp_a) == 0xa4);
    static_assert!(offset_of!(PowerCtlBlock, fast_die0_release_temp) == 0xac);
    static_assert!(offset_of!(PowerCtlBlock, ppm_words) == 0xc8);
    static_assert!(offset_of!(PowerCtlBlock, unk_dc) == 0xdc);
    static_assert!(offset_of!(PowerCtlBlock, se_controller_override) == 0xe8);
    static_assert!(offset_of!(PowerCtlBlock, se_engagement) == 0xec);
    static_assert!(offset_of!(PowerCtlBlock, dpe_leak_cfg_a) == 0x158);
    static_assert!(offset_of!(PowerCtlBlock, dpe_leak_cfg_b) == 0x1a8);
    static_assert!(offset_of!(PowerCtlBlock, clpc_shared_a) == 0x1b4);
    static_assert!(offset_of!(PowerCtlBlock, perf_ctrl_override_active) == 0x1d0);
    static_assert!(offset_of!(PowerCtlBlock, consistent_perf_state) == 0x1d1);
    static_assert!(offset_of!(PowerCtlBlock, accumulated_energy) == 0x1d8);
    static_assert!(offset_of!(PowerCtlBlock, dsid_cfg_a) == 0x1e8);
    static_assert!(offset_of!(PowerCtlBlock, dsid_cfg_b) == 0x218);
    static_assert!(offset_of!(PowerCtlBlock, submit_in_progress_gate) == 0x230);

    // P2 debug block
    static_assert!(size_of::<DebugBlock>() == 0x20);
    static_assert!(offset_of!(DebugBlock, system_sleep_in_progress) == 0x08);
    static_assert!(offset_of!(DebugBlock, fw_word_10) == 0x10);
    static_assert!(offset_of!(DebugBlock, init_state) == 0x14);

    // InitSeq record
    static_assert!(size_of::<InitSeqRecord>() == 0x18);
    static_assert!(offset_of!(InitSeqRecord, kind) == 0x10);

    // HwDataB: 0x1868 bytes
    static_assert!(size_of::<HwDataBG15V14_8_3>() == 0x1868);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_30) == 0x20);
    static_assert!(offset_of!(HwDataBG15V14_8_3, timestamp_area_base) == 0x28);
    static_assert!(offset_of!(HwDataBG15V14_8_3, yuv_matrices) == 0x38);
    static_assert!(offset_of!(HwDataBG15V14_8_3, pad_1c8) == 0x638);
    static_assert!(offset_of!(HwDataBG15V14_8_3, io_mappings) == 0x640);
    static_assert!(offset_of!(HwDataBG15V14_8_3, sgx_sram_ptr) == 0xa20);
    static_assert!(offset_of!(HwDataBG15V14_8_3, chip_id) == 0xa28);
    static_assert!(offset_of!(HwDataBG15V14_8_3, base_clock_khz) == 0xa68);
    static_assert!(offset_of!(HwDataBG15V14_8_3, power_sample_period) == 0xa6c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_a9c) == 0xa9c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_ac8) == 0xac8);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_b24) == 0xb24);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_554) == 0xb40);
    static_assert!(offset_of!(HwDataBG15V14_8_3, uat_ttb_base) == 0xb44);
    static_assert!(offset_of!(HwDataBG15V14_8_3, gpu_core_id) == 0xb4c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, num_cores) == 0xb54);
    static_assert!(offset_of!(HwDataBG15V14_8_3, max_pstate) == 0xb58);
    static_assert!(offset_of!(HwDataBG15V14_8_3, frequencies) == 0xb5c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, voltages) == 0xb9c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, voltages_sram) == 0xd9c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, frequencies_2) == 0xf9c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, sram_k) == 0xfdc);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_9f4) == 0x101c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_arr_0) == 0x10dc);
    static_assert!(offset_of!(HwDataBG15V14_8_3, aux_ps) == 0x115c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, aux_ps_3) == 0x134c);
    static_assert!(size_of::<HwDataBPStateTable>() == 0x444);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_179c) == 0x179c);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unit_mask_a) == 0x17c0);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_17e8) == 0x17e8);
    static_assert!(offset_of!(HwDataBG15V14_8_3, timer_offset) == 0x17ec);
    static_assert!(offset_of!(HwDataBG15V14_8_3, all_ones_masks) == 0x1820);
    static_assert!(offset_of!(HwDataBG15V14_8_3, fstp_override) == 0x1850);
    static_assert!(offset_of!(HwDataBG15V14_8_3, unk_1860) == 0x1860);
    static_assert!(size_of::<IOMapping>() == 0x20);

    // HwDataA: 0x4360 bytes, G13 13.5 layout, +0x10 from 0x1288
    static_assert!(size_of::<HwDataAG15V14_8_3>() == 0x4360);
    static_assert!(offset_of!(HwDataAG15V14_8_3, clocks_per_period) == 0x4);
    static_assert!(offset_of!(HwDataAG15V14_8_3, pwr_status) == 0x10);
    static_assert!(offset_of!(HwDataAG15V14_8_3, actual_pstate) == 0x2c);
    static_assert!(offset_of!(HwDataAG15V14_8_3, min_pstate_scaled) == 0x54);
    static_assert!(offset_of!(HwDataAG15V14_8_3, sram_k) == 0x80);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_64c) == 0x658);
    static_assert!(offset_of!(HwDataAG15V14_8_3, pwr_filter_a_neg) == 0x664);
    static_assert!(offset_of!(HwDataAG15V14_8_3, max_power_2) == 0x6a4);
    static_assert!(offset_of!(HwDataAG15V14_8_3, perf_tgt_utilization) == 0x788);
    static_assert!(offset_of!(HwDataAG15V14_8_3, perf_filter_drop_threshold) == 0x7a8);
    static_assert!(offset_of!(HwDataAG15V14_8_3, perf_tgt_utilization_2) == 0x7ec);
    static_assert!(offset_of!(HwDataAG15V14_8_3, min_pstate_scaled_4) == 0x860);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_sensor_mask) == 0x8ac);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_release_temp_cc) == 0x8b4);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_ki_dt) == 0x8d8);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_kp) == 0x8e8);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_prop_tgt_delta) == 0x900);
    static_assert!(offset_of!(HwDataAG15V14_8_3, max_pstate_scaled_10) == 0x938);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_coef_a1) == 0x970);
    static_assert!(offset_of!(HwDataAG15V14_8_3, max_pstate_scaled_11) == 0xbe0);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_c2c) == 0xe80);
    static_assert!(offset_of!(HwDataAG15V14_8_3, power_zone_count) == 0xe84);
    static_assert!(offset_of!(HwDataAG15V14_8_3, avg_power_target_filter_a_neg) == 0xe9c);
    static_assert!(offset_of!(HwDataAG15V14_8_3, avg_power_filter_tc_periods_x4) == 0xf7c);
    static_assert!(offset_of!(HwDataAG15V14_8_3, avg_power_kp) == 0xfa8);
    static_assert!(offset_of!(HwDataAG15V14_8_3, avg_power_filter_tc_ms) == 0xfd0);
    static_assert!(offset_of!(HwDataAG15V14_8_3, max_pstate_scaled_14) == 0xff8);
    // The builder relies on this base for the G15 +0x10f4/+0x11d0 stores
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_e10_0) == 0x10b4);
    static_assert!(size_of::<HwDataA140Extra>() == 0x1b8);
    static_assert!(offset_of!(HwDataA140Extra, unk_40) == 0x40);
    static_assert!(offset_of!(HwDataA140Extra, gpu_se_filter_a_neg) == 0x4c);
    static_assert!(offset_of!(HwDataA140Extra, gpu_se_kp) == 0x6c);
    static_assert!(offset_of!(HwDataA140Extra, max_pstate_scaled_1) == 0x7c);
    static_assert!(offset_of!(HwDataA140Extra, se_target) == 0x8c);
    static_assert!(offset_of!(HwDataA140Extra, gpu_se_filter_time_constant_clks) == 0x9c);
    static_assert!(offset_of!(HwDataA140Extra, unk_b0) == 0xb0);
    static_assert!(offset_of!(HwDataA140Extra, unk_d8) == 0xd8);
    static_assert!(offset_of!(HwDataA140Extra, gpu_se_reset_criteria) == 0xf0);
    static_assert!(offset_of!(HwDataA140Extra, unk_11c) == 0x11c);
    static_assert!(offset_of!(HwDataA140Extra, max_pstate_scaled_2) == 0x130);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_sensor_mask_2) == 0x1288);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_e28) == 0x1294);
    static_assert!(offset_of!(HwDataAG15V14_8_3, fast_die0_sensor_mask_alt) == 0x1a98);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_3640) == 0x3aa4);
    static_assert!(offset_of!(HwDataAG15V14_8_3, init_timestamp) == 0x424c);
    static_assert!(offset_of!(HwDataAG15V14_8_3, cluster_tables) == 0x42a4);
    static_assert!(offset_of!(HwDataAG15V14_8_3, unk_4340) == 0x4340);
    static_assert!(offset_of!(HwDataAG15V14_8_3, gpu_keepalive_mode) == 0x4358);

    // E2 StatsFrag: 0x1248 bytes, -1 fields at +0xc18/+0xc30
    static_assert!(size_of::<GpuGlobalStatsFragG15V14_8_3>() == 0x1248);
    static_assert!(
        offset_of!(GpuGlobalStatsFragG15V14_8_3, stats)
            + offset_of!(GpuStatsFragG15V14_8_3, cur_stamp_id)
            == 0xc18
    );
    static_assert!(
        offset_of!(GpuGlobalStatsFragG15V14_8_3, stats)
            + offset_of!(GpuStatsFragG15V14_8_3, unk_id)
            == 0xc30
    );

    // G13/G14X 13.5 sizes must not change (regression guard for the per-field gating).
    // RuntimePointers: gpu_scratch at 0x280 (0x270 on G14X) + RuntimeScratch; note that these
    // are 0x20 more than ver_info word 0 (0x6ba0/0xb390). The others are the current G13/G14X
    // sizes.
    static_assert!(size_of::<RuntimePointersG13V13_5<'static>>() == 0x6bc0);
    static_assert!(size_of::<RuntimePointersG14XV13_5<'static>>() == 0xb3b0);
    static_assert!(size_of::<GlobalsG13V13_5>() == 0x12394);
    static_assert!(size_of::<GlobalsG14XV13_5>() == 0x1715c);
    static_assert!(size_of::<HwDataAG13V13_5>() == 0x421c);
    static_assert!(size_of::<HwDataAG14XV13_5>() == 0x6c34);
    static_assert!(size_of::<HwDataBG13V13_5>() == 0x1884);
    static_assert!(size_of::<HwDataBG14XV13_5>() == 0x1884);
    static_assert!(size_of::<InitDataG13V13_5<'static>>() == 0xbc);
    static_assert!(size_of::<InitDataG14XV13_5<'static>>() == 0xbc);
    static_assert!(size_of::<InitDataG13V12_3<'static>>() == 0xb4);
    static_assert!(size_of::<PipeChannelsG13V13_5>() == 0x30);
    static_assert!(size_of::<GpuGlobalStatsFragG13V13_5>() == 0x1124);
    static_assert!(size_of::<GpuGlobalStatsFragG14XV13_5>() == 0x1a34);
    static_assert!(size_of::<channels::raw::ChannelState<'static>>() == 0x30);
}
