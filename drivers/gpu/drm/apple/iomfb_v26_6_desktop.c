// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

/*
 * Desktop 26.6 firmware on T6041 (M4 Pro/Max) through the classic IOMFB
 * template (DCP_FIRMWARE_V_26_6_DESKTOP).  This firmware continues the 14.7
 * IOMFB, with its own method numbers, record layouts and power sequences.
 * Not to be confused with iomfb_v26_6.c, the J613 DCPLink session, nor with
 * the H17-generation 26.x firmware of iomfb_h17p.c.
 */

/* Include all per-version headers before defining DCP_FW ourself. */
#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "iomfb_v14_7_tpl.h"
#include "iomfb_v26_6_desktop.h"
#include "iomfb_h17p.h"
#include "version_utils.h"

static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),
	IOMFB_METHOD("A029", dcpep_setup_video_limits),
	IOMFB_METHOD("A131", iomfbep_a131_pmu_service_matched),
	IOMFB_METHOD("A132", iomfbep_a132_backlight_service_matched),
	IOMFB_METHOD("A385", dcpep_set_create_dfb),
	IOMFB_METHOD("A386", iomfbep_a358_vi_set_temperature_hint),
	IOMFB_METHOD("A401", dcpep_start_signal),
	IOMFB_METHOD("A406", dcpep_swap_start),
	IOMFB_METHOD("A407", dcpep_swap_submit),
	IOMFB_METHOD("A411", dcpep_set_display_device),
	IOMFB_METHOD("A412", dcpep_is_main_display),
	IOMFB_METHOD("A413", dcpep_set_digital_out_mode),
	/* prepare/finish around the display power changes */
	IOMFB_METHOD("A414", dcpep_pipe_cfg_414),
	IOMFB_METHOD("A423", iomfbep_set_matrix),
	IOMFB_METHOD("A427", iomfbep_get_color_remap_mode),
	IOMFB_METHOD("A442", dcpep_set_parameter_dcp),
	IOMFB_METHOD("A446", dcpep_create_default_fb),
	IOMFB_METHOD("A450", dcpep_enable_disable_video_power_savings),
	IOMFB_METHOD("A457", dcpep_first_client_open),
	IOMFB_METHOD("A458", iomfbep_last_client_close),
	IOMFB_METHOD("A465", dcpep_set_display_refresh_properties),
	IOMFB_METHOD("A468", dcpep_flush_supports_power),
	IOMFB_METHOD("A469", iomfbep_abort_swaps_dcp),
	IOMFB_METHOD("A473", dcpep_set_power_state),
	IOMFB_METHOD("A474", dcpep_pipe_query_474),
	/* Unnamed; sent after A473(0), before the RTKit sleep. */
	IOMFB_METHOD("A500", dcpep_unnamed_500),
	/* A364 get_mode_info and A420/A421 gamma: see iomfb_template.c */
};

#define DCP_FW v26_6_desktop
#define DCP_FW_VER DCP_FW_VERSION_DESKTOP(26, 6, 0)

#include "iomfb_template.c"

/*
 * D114 periodically publishes dictionary-valued statistics: the analytics
 * event, answered by dcp_dict_passthrough_reply().
 */
static bool trampoline_passthrough_dict_v26_6(struct apple_dcp *dcp, int tag,
					      void *out, void *in)
{
	const struct dcp_dict_passthrough_req *req = in;
	struct dcp_dict_passthrough_resp *resp = out;

	trace_iomfb_callback(dcp, tag, "dcpep_cb_passthrough_dict");
	dcp_trace_panel_property(dcp, tag, (const char *)req->key,
				 strnlen((const char *)req->key, sizeof(req->key)),
				 req->value_null ? NULL : req->value, sizeof(req->value));

	if (!req->value_null)
		dcp_panel_edid_receive(dcp, req->value, sizeof(req->value));
	dcp_dict_passthrough_reply(req, resp);

	return true;
}

/* The firmware can publish panel identity through dictionary setters too. */
static bool trampoline_panel_dictionary_v26_6(struct apple_dcp *dcp, int tag,
					      void *out, void *in)
{
	/* Service dictionary setters prefix the same record with a handle. */
	const struct dcp_dict_passthrough_req *req = tag == 413 ? in + 4 : in;
	const char *key = (const char *)req->key;
	struct dcp_display_metadata metadata;
	struct dcp_parse_ctx ctx;
	int ret;

	trace_iomfb_callback(dcp, tag, __func__);
	dcp_trace_panel_property(dcp, tag, key, strnlen(key, sizeof(req->key)),
				 req->value_null ? NULL : req->value, sizeof(req->value));
	if (!dcp_has_panel(dcp) || req->value_null ||
	    !memchr(key, 0, sizeof(req->key)))
		goto done;
	/* A real EDID may be a blob or nested in a property dictionary. */
	dcp_panel_edid_receive(dcp, req->value, sizeof(req->value));
	if (strcmp(key, "DisplayAttributes") && strcmp(key, "ProductAttributes"))
		goto done;
	ret = parse(req->value, sizeof(req->value), &ctx);
	if (ret)
		goto done;
	if (!strcmp(key, "ProductAttributes"))
		ret = parse_panel_product_metadata(&ctx, &metadata);
	else
		ret = parse_panel_metadata(&ctx, &metadata);
	if (!ret)
		dcp_panel_metadata_replace(dcp, &metadata);
done:
	/* Preserve the existing setter acknowledgment for unrelated properties. */
	*(u8 *)out = true;
	return true;
}

static bool iomfb_check_callback_exact_v26_6_desktop(int tag, u32 in_len, u32 out_len)
{
	switch (tag) {
	case 201:
		return in_len == 12 && out_len == 16;
	case 202:
		return in_len == 24 && !out_len;
	case 451:
		return in_len == 20 && out_len == 28;
	case 452:
		return in_len == 24 && out_len == 20;
	case 454:
		return in_len == 4 && out_len == 4;
	case 413:
		return in_len == 4168 && (out_len == 1 || out_len == 4);
	case 552:
	case 561:
		return in_len == 4164 && (out_len == 1 || out_len == 4);
	default:
		return true;
	}
}

/*
 * unmap_piodma (D202): the buffer id, the id again, the DVA (unaligned) and a
 * trailing word. The older firmware sends the DVA after a virtual address.
 */
struct dcp_unmap_buf_req_v26_6 {
	__le64 buffer;
	__le32 id;
	__le64 dva;
	__le32 unk;
} __packed;
static_assert(sizeof(struct dcp_unmap_buf_req_v26_6) == 24);

static bool trampoline_unmap_piodma_v26_6(struct apple_dcp *dcp, int tag,
					  void *out, void *in)
{
	const struct dcp_unmap_buf_req_v26_6 *wire = in;
	struct dcp_unmap_buf_resp request = {
		.buffer = get_unaligned_le64(&wire->buffer),
		.dva = get_unaligned_le64(&wire->dva),
	};

	trace_iomfb_callback(dcp, tag, "dcpep_cb_unmap_piodma");
	dcpep_unmap_piodma(dcp, &request);

	return true;
}

/*
 * release_mem_desc (D454): the firmware sends the 32-bit descriptor id and
 * wants a 32-bit result, where the older firmware took a byte.
 */
static bool trampoline_release_mem_desc_v26_6(struct apple_dcp *dcp, int tag,
					      void *out, void *in)
{
	u32 id = get_unaligned_le32(in);

	trace_iomfb_callback(dcp, tag, "dcpep_cb_release_mem_desc");
	put_unaligned_le32(dcpep_cb_release_mem_desc(dcp, &id), out);

	return true;
}

static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = trampoline_true, /* did_boot_signal */
	[1] = trampoline_true, /* did_power_on_signal */
	[2] = trampoline_nop, /* will_power_off_signal */
	[3] = trampoline_rt_bandwidth,
	[6] = trampoline_set_frame_sync_props,
	[100] = iomfbep_cb_match_pmu_service,
	[101] = trampoline_zero, /* get_display_default_stride */
	[102] = trampoline_nop, /* set_number_property */
	[104] = trampoline_nop, /* set_boolean_property */
	[107] = trampoline_nop, /* remove_property */
	[108] = trampoline_true, /* create_provider_service */
	[109] = trampoline_true, /* create_product_service */
	[110] = trampoline_true, /* create_pmu_service */
	[111] = trampoline_true, /* create_iomfb_service */
	[112] = trampoline_create_backlight_service,
	[113] = trampoline_true, /* create_nvram_service? */
	[114] = trampoline_passthrough_dict_v26_6,
	/* set_tiling_state: the value followed by success */
	[115] = trampoline_get_tiling_state,
	[116] = trampoline_set_tiling_state_v26_6,
	[121] = dcpep_cb_boot_1, /* start_hardware_boot */
	[122] = trampoline_false, /* is_dark_boot / is_waking_from_hibernate */
	[123] = trampoline_false,
	[125] = trampoline_read_edt_data,
	[127] = trampoline_prop_start,
	[128] = trampoline_prop_chunk,
	[129] = trampoline_prop_end,
	[134] = trampoline_zero,
	[201] = trampoline_map_piodma,
	/* a mode change retires its piodma buffers, then their descriptors */
	[202] = trampoline_unmap_piodma_v26_6,
	[206] = iomfbep_cb_match_pmu_service_2,
	[207] = iomfbep_cb_match_backlight_service,
	[208] = trampoline_nop, /* update_backlight_factor_prop */
	[209] = trampoline_get_time,
	[300] = trampoline_pr_publish,
	[400] = trampoline_nop,
	[401] = trampoline_get_uint_prop,
	[406] = trampoline_nop, /* set_fx_prop: a property notification */
	[408] = trampoline_get_frequency,
	[411] = trampoline_map_reg,
	[413] = trampoline_panel_dictionary_v26_6, /* sr_set_property_dict */
	[414] = trampoline_sr_set_property_int,
	[415] = trampoline_true, /* sr_set_property_bool */
	[451] = trampoline_allocate_buffer,
	[452] = trampoline_map_physical,
	[454] = trampoline_release_mem_desc_v26_6,
	[552] = trampoline_panel_dictionary_v26_6,
	[561] = trampoline_panel_dictionary_v26_6,
	[563] = trampoline_true, /* set_property_int */
	[565] = trampoline_true, /* set_property_bool */
	[567] = trampoline_true, /* set_property_str */
	/* a zero status for both its start and stop events */
	[572] = trampoline_zero,
	[575] = trampoline_hotplug,
	[576] = trampoline_nop,
	[581] = trampoline_nop,
	[582] = trampoline_true, /* create_default_fb_surface */
	[588] = trampoline_nop, /* resize_default_fb_surface_gated */
	[590] = trampoline_swap_complete,
	[592] = trampoline_swap_complete_intent_gated,
	[593] = trampoline_abort_swap_ap_gated,
	/* a four-byte padded notification; only its first byte is read */
	[594] = trampoline_enable_backlight_message_ap_gated,
	[595] = trampoline_nop,
	[598] = trampoline_nop,
	[599] = trampoline_nop,
};

/*
 * What each handler above reads from the request and writes to the reply.
 * The firmware may send longer records; a shorter one is refused.
 */
struct dcp_cb_min_v26_6_desktop {
	u32 in_len;
	u32 out_len;
	bool valid;
};

#define CB_MIN(in, out) { .in_len = (in), .out_len = (out), .valid = true }

static const struct dcp_cb_min_v26_6_desktop cb_min[IOMFB_MAX_CB] = {
	[0] = CB_MIN(0, sizeof(u8)),
	[1] = CB_MIN(0, sizeof(u8)),
	[2] = CB_MIN(0, 0),
	[3] = CB_MIN(0, sizeof(struct dcp_rt_bandwidth)),
	[6] = CB_MIN(sizeof(struct dcp_set_frame_sync_props_req_v26_6_desktop),
		     sizeof(struct dcp_set_frame_sync_props_resp_v26_6_desktop)),
	[100] = CB_MIN(0, 0),
	[101] = CB_MIN(0, sizeof(u32)),
	[102] = CB_MIN(0, 0),
	[104] = CB_MIN(0, 0),
	[107] = CB_MIN(0, 0),
	[108] = CB_MIN(0, sizeof(u8)),
	[109] = CB_MIN(0, sizeof(u8)),
	[110] = CB_MIN(0, sizeof(u8)),
	[111] = CB_MIN(0, sizeof(u8)),
	[112] = CB_MIN(0, sizeof(u8)),
	[113] = CB_MIN(0, sizeof(u8)),
	[114] = CB_MIN(sizeof(struct dcp_dict_passthrough_req),
		       sizeof(struct dcp_dict_passthrough_resp)),
	[115] = CB_MIN(sizeof(struct dcpep_get_tiling_state_req),
		       sizeof(struct dcpep_get_tiling_state_resp)),
	[116] = CB_MIN(sizeof(struct dcpep_get_tiling_state_req), sizeof(u32)),
	[121] = CB_MIN(0, 0),
	[122] = CB_MIN(0, sizeof(u8)),
	[123] = CB_MIN(0, sizeof(u8)),
	[125] = CB_MIN(sizeof(struct dcp_read_edt_data_req),
		       sizeof(dcp_read_edt_data_resp_t)),
	[127] = CB_MIN(sizeof(u32), sizeof(u8)),
	[128] = CB_MIN(sizeof(struct dcp_set_dcpav_prop_chunk_req), sizeof(u8)),
	[129] = CB_MIN(sizeof(struct dcp_set_dcpav_prop_end_req), sizeof(u8)),
	[134] = CB_MIN(0, sizeof(u32)),
	[201] = CB_MIN(sizeof(struct dcp_map_buf_req),
		       sizeof(struct DCP_FW_NAME(dcp_map_buf_resp))),
	[202] = CB_MIN(sizeof(struct dcp_unmap_buf_req_v26_6), 0),
	[206] = CB_MIN(0, sizeof(u8)),
	[207] = CB_MIN(0, sizeof(u8)),
	[208] = CB_MIN(0, 0),
	[209] = CB_MIN(0, sizeof(u64)),
	[300] = CB_MIN(sizeof(struct iomfb_property), 0),
	[400] = CB_MIN(0, 0),
	[401] = CB_MIN(sizeof(struct dcp_get_uint_prop_req),
		       sizeof(struct dcp_get_uint_prop_resp)),
	[406] = CB_MIN(0, 0),
	[408] = CB_MIN(0, sizeof(u64)),
	[411] = CB_MIN(sizeof(struct DCP_FW_NAME(dcp_map_reg_req)),
		       sizeof(struct DCP_FW_NAME(dcp_map_reg_resp))),
	[413] = CB_MIN(0, sizeof(u8)),
	[414] = CB_MIN(sizeof(struct iomfb_sr_set_property_int_req), sizeof(u8)),
	[415] = CB_MIN(0, sizeof(u8)),
	[451] = CB_MIN(sizeof(struct dcp_allocate_buffer_req),
		       sizeof(struct dcp_allocate_buffer_resp)),
	[452] = CB_MIN(sizeof(struct dcp_map_physical_req),
		       sizeof(struct dcp_map_physical_resp)),
	[454] = CB_MIN(sizeof(u32), sizeof(u32)),
	[552] = CB_MIN(0, sizeof(u8)),
	[561] = CB_MIN(0, sizeof(u8)),
	[563] = CB_MIN(0, sizeof(u8)),
	[565] = CB_MIN(0, sizeof(u8)),
	[567] = CB_MIN(0, sizeof(u8)),
	[572] = CB_MIN(0, sizeof(u32)),
	[575] = CB_MIN(sizeof(struct dcp_hotplug_req_v26_6),
		       sizeof(struct dcp_hotplug_resp_v26_6)),
	[576] = CB_MIN(0, 0),
	[581] = CB_MIN(0, 0),
	[582] = CB_MIN(0, sizeof(u8)),
	[588] = CB_MIN(0, 0),
	[590] = CB_MIN(sizeof(struct DCP_FW_NAME(dc_swap_complete_resp)), 0),
	[592] = CB_MIN(sizeof(dcp_swap_intent_t), 0),
	[593] = CB_MIN(sizeof(u32), 0),
	[594] = CB_MIN(sizeof(u8), 0),
	[595] = CB_MIN(0, 0),
	[598] = CB_MIN(0, 0),
	[599] = CB_MIN(0, 0),
};

#undef CB_MIN

bool iomfb_check_callback_v26_6_desktop(int tag, u32 in_len, u32 out_len)
{
	if (tag < 0 || tag >= IOMFB_MAX_CB || !cb_min[tag].valid)
		return false;
	return in_len >= cb_min[tag].in_len && out_len >= cb_min[tag].out_len &&
	       iomfb_check_callback_exact_v26_6_desktop(tag, in_len, out_len);
}

void DCP_FW_NAME(iomfb_start)(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;

	dcp_start_signal(dcp, false, dcp_started, NULL);
}

#undef DCP_FW_VER
#undef DCP_FW
