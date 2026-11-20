// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bring-up only: print the legacy 13.5 command layouts the 27.2 encoders are derived from.

use crate::fw;
use core::mem::{offset_of, size_of};
use kernel::prelude::*;

macro_rules! dump {
    ($dev:expr, $ty:ty, [$($f:ident),* $(,)?]) => {{
        dev_info!($dev, "LAYOUT {} size={:#x}\n", stringify!($ty), size_of::<$ty>());
        $( dev_info!($dev, "LAYOUT {}.{} = {:#x}\n", stringify!($ty), stringify!($f), offset_of!($ty, $f)); )*
    }};
}

pub(crate) fn dump_layouts(dev: &kernel::device::Device) {
    use fw::vertex::raw as v;
    use fw::fragment::raw as f;
    use fw::compute::raw as c;
    use fw::microseq as m;
    dump!(dev, v::RunVertexG13V13_5<'static>, [tag, counter, vm_slot, unk_8, notifier, buffer_slot,
        unk_1c, buffer, scene, unk_buffer_buf, unk_34, job_params1, tiling_params, tpc, tpc_size,
        microsequence, microsequence_size, fragment_stamp_slot, fragment_stamp_value, unk_pointee,
        unk_pad, job_params2, encoder_params, unk_55c, unk_560, sync_grow, unk_568, uses_scratch,
        meta, unk_after_meta, unk_buf_0, unk_buf_8, unk_buf_10, command_time, timestamp_pointers,
        user_timestamp_pointers, client_sequence, pad_5d5, unk_5d8, unk_5dc, unk_ts, unk_5dd_8]);
    dump!(dev, f::RunFragmentG13V13_5<'static>, [tag, counter, vm_slot, unk_8, microsequence,
        microsequence_size, notifier, buffer, scene, unk_buffer_buf, tvb_tilemap, ppp_multisamplectl,
        samples, tiles_per_mtile_y, tiles_per_mtile_x, unk_50, unk_58, isp_merge_upper_x,
        isp_merge_upper_y, unk_68, tile_count, job_params1, job_params2, job_params3, unk_758_flag,
        unk_75c_flag, unk_buf, busy_flag, tvb_overflow_count, unk_878, encoder_params,
        process_empty_tiles, no_clear_pipeline_textures, msaa_zs, unk_pointee, unk_v13_3, meta,
        unk_after_meta, unk_buf_0, unk_buf_8, unk_buf_10, command_time, timestamp_pointers,
        user_timestamp_pointers, client_sequence, pad_925, unk_928, unk_92c, unk_ts, unk_92d_8]);
    dump!(dev, c::RunComputeG13V13_5<'static>, [tag, counter, unk_4, vm_slot, notifier, unk_pointee,
        __pad0, job_params1, __pad1, microsequence, microsequence_size, job_params2, encoder_params,
        meta, command_time, timestamp_pointers, user_timestamp_pointers, client_sequence, pad_2d1,
        unk_2d4, unk_2d8, context_store_req, context_store_compl, unk_2e9, unk_flag, unk_pad]);
    dump!(dev, c::RunComputeG14V13_5<'static>, [tag, counter, unk_4, vm_slot, notifier, unk_pointee,
        __pad0, job_params1, __pad1, microsequence, microsequence_size, job_params2, encoder_params,
        meta, command_time, timestamp_pointers, user_timestamp_pointers, client_sequence, pad_2d1,
        unk_2d4, unk_2d8, context_store_req, context_store_compl, unk_2e9, unk_flag, unk_pad]);
    dump!(dev, m::StartVertexG13V13_5<'static>, [header, tiling_params, job_params1, buffer, scene,
        stats, work_queue, vm_slot, unk_38, event_generation, buffer_slot, unk_44, event_seq, unk_50,
        unk_pointer, unk_job_buf, unk_64, unk_68, uuid, attachments, padding, counter, notifier_buf,
        unk_178]);
    dump!(dev, m::FinalizeVertexG13V13_5, [header, scene, buffer, stats, work_queue, vm_slot, unk_28,
        unk_pointer, unk_34, uuid, fw_stamp, stamp_value, unk_48, unk_50, unk_54, unk_58, unk_60,
        unk_64, unk_68, restart_branch_offset, has_attachments, unk_74]);
    dump!(dev, m::StartFragmentG13V13_5<'static>, [header, job_params2, job_params1, scene, stats,
        busy_flag, tvb_overflow_count, unk_pointer, work_queue, work_item, vm_slot, unk_50,
        event_generation, buffer_slot, sync_grow, event_seq, unk_68, unk_758_flag, unk_job_buf,
        unk_7c_0, unk_7c, unk_80, unk_84, uuid, attachments, padding, counter, notifier_buf]);
    dump!(dev, m::FinalizeFragmentG13V13_5, [header, uuid, unk_8, fw_stamp, stamp_value, unk_18, scene,
        buffer, unk_2c, stats, unk_pointer, busy_flag, work_queue, work_item, vm_slot, unk_60,
        unk_758_flag, unk_6c_0, unk_6c, unk_74, unk_7c, unk_84, unk_8c, restart_branch_offset,
        has_attachments, unk_9c]);
    dump!(dev, m::StartComputeG14V13_5<'static>, [header, unk_pointer, job_params1, stats, work_queue,
        vm_slot, unk_28, event_generation, event_seq, unk_38, job_params2, unk_44, uuid, attachments,
        padding, unk_flag, counter, notifier_buf]);
    dump!(dev, m::FinalizeComputeG14V13_5<'static>, [header, stats, work_queue, vm_slot, job_params2,
        unk_24, uuid, fw_stamp, stamp_value, unk_38, unk_58, restart_branch_offset, has_attachments,
        unk_64, unk_flag, unk_79]);
    dev_info!(dev, "LAYOUT sizes vtx.jp1={:#x} vtx.tiling={:#x} vtx.jp2={:#x} frag.jp1={:#x} frag.jp2={:#x} frag.jp3={:#x} comp.jp1={:#x} comp.jp2={:#x}\n",
        size_of::<v::JobParameters1G13V13_5<'static>>(), size_of::<v::TilingParameters>(),
        size_of::<v::JobParameters2<'static>>(), size_of::<f::JobParameters1G13V13_5<'static>>(),
        size_of::<f::JobParameters2>(), size_of::<f::JobParameters3G13V13_5>(),
        size_of::<c::JobParameters1<'static>>(), size_of::<c::JobParameters2G13V13_5<'static>>());
}
