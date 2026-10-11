// SPDX-License-Identifier: GPL-2.0-only
/* Copyright The Asahi Linux Contributors */
/* Copyright 2026 Aurora Silicon */

#include <linux/unaligned.h>
#include <linux/spinlock.h>

#include "dcp-link.h"
#include "iomfb_v27.h"
#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "version_utils.h"
#include "dcp-internal.h"

void iomfb_serialize_present_v27(struct dcp_swap_submit_req_v27 *wire,
				const struct dcp_swap_submit_req_h17p *request)
{
	memset(wire, 0, sizeof(*wire));
	memcpy(wire->swap.timestamp, request->swap.timestamp,
	       sizeof(wire->swap.timestamp));
	wire->swap.unk_38 = request->swap.unk_38;
	/* The common geometry runs from swap_id through bg_color. */
	memcpy(&wire->swap.swap_id, &request->swap.swap_id,
	       offsetof(struct dcp_swap_v27, reserved_158) -
	       offsetof(struct dcp_swap_v27, swap_id));
	memcpy(wire->surf, request->surf, sizeof(wire->surf));
	memcpy(wire->surf_iova, request->surf_iova,
	       sizeof(*request) -
	       offsetof(struct dcp_swap_submit_req_h17p, surf_iova));
}

struct dcp_h17p_hotplug_request {
	__le64 connected;
	u8 tiled_display[0x4c];
	u8 tiled_display_null;
	u8 padding[3];
} __packed;

struct dcp_h17p_swap_info_request {
	u8 swap_info[0xe0];
	u8 swap_info_null;
	u8 padding[3];
} __packed;

static_assert(sizeof(struct dcp_h17p_hotplug_request) == 0x58);
static_assert(sizeof(struct dcp_h17p_swap_info_request) == 0xe4);

static_assert(sizeof(struct dc_swap_complete_resp_h17p) == 0x730);
static_assert(offsetof(struct dc_swap_complete_resp_h17p, swap_id) == 0);
static_assert(offsetof(struct dcp_swap_v27, brightness_update) == 0x354);
static_assert(offsetof(struct dcp_swap_v27, brightness_nits) == 0x35e);
static_assert(sizeof(struct dcp_swap_v27) == 0x6e0);

static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),
	IOMFB_METHOD("A027", dcpep_setup_video_limits),
	IOMFB_METHOD("A136", iomfbep_a131_pmu_service_matched),
	IOMFB_METHOD("A137", iomfbep_a132_backlight_service_matched),
	IOMFB_METHOD("A389", dcpep_set_create_dfb),
	IOMFB_METHOD("A390", iomfbep_a358_vi_set_temperature_hint),
	IOMFB_METHOD("A401", dcpep_start_signal),
	IOMFB_METHOD("A406", dcpep_swap_start),
	IOMFB_METHOD("A408", dcpep_swap_submit),
	IOMFB_METHOD("A411", dcpep_set_display_device),
	IOMFB_METHOD("A412", dcpep_is_main_display),
	IOMFB_METHOD("A413", dcpep_set_digital_out_mode),
	IOMFB_METHOD("A423", iomfbep_set_matrix),
	IOMFB_METHOD("A427", iomfbep_get_color_remap_mode),
	IOMFB_METHOD("A450", dcpep_set_parameter_dcp),
	IOMFB_METHOD("A454", dcpep_create_default_fb),
	IOMFB_METHOD("A037", dcpep_get_dfb_compression_info),
	IOMFB_METHOD("A036", dcpep_get_dfb_info),
	IOMFB_METHOD("A103", dcpep_get_dfb_layout),
	IOMFB_METHOD("A453", dcpep_get_dfb_state),
	IOMFB_METHOD("A104", dcpep_set_dfb_dimensions),
	IOMFB_METHOD("A105", dcpep_commit_dfb_info),
	IOMFB_METHOD("A035", dcpep_dfb_query),
	IOMFB_METHOD("A384", dcpep_dfb_ready_query),
	IOMFB_METHOD("A353", dcpep_pipe_query_353),
	IOMFB_METHOD("A352", dcpep_pipe_set_352),
	IOMFB_METHOD("A415", dcpep_pipe_cfg_415),
	IOMFB_METHOD("A033", dcpep_pipe_cfg_031),
	IOMFB_METHOD("A414", dcpep_pipe_cfg_414),
	IOMFB_METHOD("A489", dcpep_pipe_query_478),
	IOMFB_METHOD("A485", dcpep_pipe_query_474),
	IOMFB_METHOD("A487", dcpep_pipe_set_476),
	IOMFB_METHOD("A428", dcpep_pipe_set_428),
	IOMFB_METHOD("A458", dcpep_enable_disable_video_power_savings),
	IOMFB_METHOD("A466", dcpep_first_client_open),
	IOMFB_METHOD("A467", iomfbep_last_client_close),
	IOMFB_METHOD("A475", dcpep_set_display_refresh_properties),
	IOMFB_METHOD("A478", dcpep_flush_supports_power),
	IOMFB_METHOD("A479", iomfbep_abort_swaps_dcp),
	IOMFB_METHOD("A484", dcpep_set_power_state),
	IOMFB_METHOD("A483", dcpep_register_dfb_surface),
};

#define iomfb_modeset_h17p iomfb_modeset_v27
#define iomfb_flush_h17p iomfb_flush_v27
#define iomfb_poweron_h17p iomfb_poweron_v27
#define iomfb_poweroff_h17p iomfb_poweroff_v27
#define iomfb_sleep_h17p iomfb_sleep_v27
#define iomfb_shutdown_h17p iomfb_shutdown_v27
#define iomfb_present_backlight_h17p iomfb_present_backlight_v27
#define DCP_FW h17p
#define DCP_FW_VER DCP_FW_VERSION(27, 0, 0)

#include "iomfb_template.c"

static bool trampoline_rt_bandwidth_h17p(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	u64 scratch = dcp->disp_firmware_base[dcp->disp_bw_scratch_index] +
		      dcp->disp_bw_scratch_offset;
	u64 clock_request = 0;

	trace_iomfb_callback(dcp, tag, __func__);
	if (apple_dcp_h17p_rt_bw_encode_reply(in, out, scratch, clock_request))
		dev_warn(dcp->dev, "D003 bandwidth request is malformed\n");
	return true;
}

static bool trampoline_get_frequency_v27(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	const struct apple_dcp_h17p_clock_request *request = in;
	u32 index = le32_to_cpu(request->clock_id);
	u64 rate = 0;

	if (index < dcp->num_firmware_clocks)
		rate = clk_get_rate(dcp->firmware_clocks[index].clk);
	else
		dev_err(dcp->dev, "DCP requested undeclared provider clock index %u\n",
			index);
	*(__le64 *)out = cpu_to_le64(rate);
	return true;
}

static bool trampoline_hotplug_h17p(struct apple_dcp *dcp, int tag,
				    void *out, void *in)
{
	const struct dcp_h17p_hotplug_request *request = in;

	if (!(request->tiled_display_null & 1))
		memcpy(out, request->tiled_display,
		       sizeof(request->tiled_display));
	return trampoline_hotplug(dcp, tag, out, in);
}

static bool trampoline_swap_info_h17p(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	const struct dcp_h17p_swap_info_request *request = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (!(request->swap_info_null & 1))
		memcpy(out, request->swap_info, sizeof(request->swap_info));
	return true;
}

static bool iomfbep_cb_video_range_h17p(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	__le32 *resp = out;

	resp[0] = cpu_to_le32(0x435b0000);
	resp[1] = 0;
	return true;
}

struct dcp_h17p_get_data_prop_req {
	char obj[4];
	char key[0x40];
	__le32 length;
	u8 data_null;
	u8 length_null;
	u8 padding[2];
} __packed;
static_assert(sizeof(struct dcp_h17p_get_data_prop_req) == 0x4c);

struct dcp_h17p_get_data_prop_resp {
	u8 data[0xc00];
	__le32 length;
} __packed;
static_assert(sizeof(struct dcp_h17p_get_data_prop_resp) == 0xc04);

static bool trampoline_get_data_prop_v27(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	const struct dcp_h17p_get_data_prop_req *req = in;
	struct dcp_h17p_get_data_prop_resp *resp = out;
	const struct property *prop;
	u32 cap = min_t(u32, le32_to_cpu(req->length), sizeof(resp->data));

	if (memcmp(req->obj, "VORP", sizeof(req->obj)) ||
	    strnlen(req->key, sizeof(req->key)) == sizeof(req->key))
		return true;

	prop = of_find_property(dcp->dev->of_node, req->key, NULL);
	if (!prop || prop->length < 0 || prop->length > cap)
		return true;
	memcpy(resp->data, prop->value, prop->length);
	resp->length = cpu_to_le32(prop->length);
	return true;
}

struct dcp_allocate_buffer_req_v27 {
	__le32 options;
	__le64 size;
	__le32 alignment;
	u8 dva_null;
	u8 capacity_null;
	u8 padding[2];
} __packed;

struct dcp_allocate_buffer_resp_v27 {
	__le64 dva;
	__le64 capacity;
	__le32 mem_desc_id;
} __packed;

struct dcp_buffer_lifecycle_req_v27 {
	__le32 mem_desc_id;
	__le32 options;
} __packed;

static_assert(sizeof(struct dcp_allocate_buffer_req_v27) == 0x14);
static_assert(sizeof(struct dcp_allocate_buffer_resp_v27) == 0x14);
static_assert(offsetof(struct dcp_allocate_buffer_resp_v27, mem_desc_id) == 0x10);
static_assert(sizeof(struct dcp_buffer_lifecycle_req_v27) == 8);

static bool trampoline_allocate_buffer_v27(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	const struct dcp_allocate_buffer_req_v27 *req = in;
	struct dcp_allocate_buffer_resp_v27 *resp = out;
	struct dcp_allocate_buffer_req allocation = {
		.unk0 = le32_to_cpu(req->options),
		.size = le64_to_cpu(req->size),
		.unk2 = le32_to_cpu(req->alignment),
	};
	struct dcp_allocate_buffer_resp buffer;

	if (!allocation.size || allocation.size > SIZE_MAX - SZ_16K ||
	    allocation.unk2 > PAGE_SIZE ||
	    (allocation.unk2 && !is_power_of_2(allocation.unk2)))
		return true;
	buffer = dcpep_cb_allocate_buffer(dcp, &allocation);
	if (!req->dva_null)
		resp->dva = cpu_to_le64(buffer.dva);
	if (!req->capacity_null)
		resp->capacity = cpu_to_le64(ALIGN(buffer.dva_size, SZ_16K));
	resp->mem_desc_id = cpu_to_le32(buffer.mem_desc_id);
	return true;
}

static bool trampoline_buffer_lifecycle_v27(struct apple_dcp *dcp, int tag,
					  void *out, void *in)
{
	const struct dcp_buffer_lifecycle_req_v27 *req = in;
	__le32 *status = out;
	u32 id = le32_to_cpu(req->mem_desc_id);

	if (!id || id >= DCP_MAX_MAPPINGS ||
	    !test_bit(id, dcp->memdesc_map) || !dcp->memdesc[id].buf) {
		*status = cpu_to_le32(14);
		return true;
	}
	if (tag == 452)
		dma_wmb();
	else
		dma_rmb();
	*status = 0;
	return true;
}

static bool trampoline_ideal_screen_space_v27(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	const char *gamut;
	u32 space = 1;

	if (!of_property_read_string(dcp->dev->of_node,
				     "apple,artwork-display-gamut", &gamut)) {
		if (!strcmp(gamut, "P3"))
			space = 2;
		else if (!strcmp(gamut, "AdobeRGB"))
			space = 3;
	}
	*(__le32 *)out = cpu_to_le32(space);
	return true;
}

static bool trampoline_tiling_state_v27(struct apple_dcp *dcp, int tag,
				       void *out, void *in)
{
	const struct dcpep_get_tiling_state_req *req = in;
	struct dcpep_get_tiling_state_resp *resp = out;

	resp->value = req->value;
	resp->ret = 0;
	if (!req->value_null && (req->param == 1 || req->param == 11) &&
	    dcp->connector && !dcp->connector->base.has_tile) {
		resp->value = 0;
		resp->ret = 1;
	}
	return true;
}

#include "iomfb-display-identity-v27.inc"
#include "iomfb-external-counts-v27.inc"
#include "iomfb-boolean-properties-v27.inc"

static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = dcpep_cb_d000_h17p,
	[1] = trampoline_true,
	[2] = trampoline_nop,
	[3] = trampoline_rt_bandwidth_h17p,
	[6] = trampoline_set_frame_sync_props_h17p,
	[100] = iomfbep_cb_match_pmu_service,
	[101] = trampoline_zero,
	[102] = trampoline_nop,
	[104] = trampoline_nop,
	[106] = trampoline_nop,
	[107] = trampoline_true,
	[108] = trampoline_true,
	[109] = trampoline_true,
	[110] = trampoline_true,
	[111] = trampoline_create_backlight_service,
	[112] = trampoline_true,
	[113] = trampoline_zero,
	[116] = trampoline_nop,
	[117] = trampoline_zero,
	[118] = trampoline_zero,
	[119] = trampoline_zero,
	[114] = trampoline_tiling_state_v27,
	[120] = dcpep_cb_boot_1,
	[121] = trampoline_false,
	[122] = trampoline_false,
	[123] = trampoline_zero,
	[124] = trampoline_read_edt_data,
	[126] = trampoline_prop_start,
	[127] = trampoline_prop_chunk,
	[128] = trampoline_prop_end,
	[129] = trampoline_allocate_bandwidth,
	[130] = trampoline_update_external_counts_v27,
	[131] = trampoline_get_external_counts_v27,
	[132] = trampoline_apply_suppression_v27,
	[133] = trampoline_display_identity_v27,
	[201] = trampoline_map_piodma,
	[202] = trampoline_unmap_piodma,
	[206] = iomfbep_cb_match_pmu_service_2,
	[207] = iomfbep_cb_match_backlight_service,
	[204] = trampoline_ideal_screen_space_v27,
	[208] = trampoline_nop,
	[209] = trampoline_get_time,
	[300] = trampoline_pr_publish,
	[400] = trampoline_get_data_prop_v27,
	[401] = trampoline_get_uint_prop,
	[404] = trampoline_nop,
	[406] = trampoline_set_fx_prop,
	[408] = trampoline_get_frequency_v27,
	[411] = trampoline_map_reg,
	[413] = trampoline_true,
	[414] = trampoline_sr_set_property_int,
	[415] = trampoline_true,
	[451] = trampoline_allocate_buffer_v27,
	[452] = trampoline_buffer_lifecycle_v27,
	[453] = trampoline_buffer_lifecycle_v27,
	[454] = trampoline_release_mem_desc,
	[552] = trampoline_true,
	[561] = trampoline_true,
	[563] = trampoline_true,
	[565] = trampoline_true,
	[567] = trampoline_true,
	[569] = trampoline_set_boolean_property_v27,
	[572] = trampoline_zero,
	[574] = iomfbep_cb_video_range_h17p,
	[575] = trampoline_hotplug_h17p,
	[576] = trampoline_nop,
	[586] = iomfbep_cb_create_dfb_surface,
	[587] = trampoline_nop,
	[588] = trampoline_nop,
	[589] = trampoline_swap_info_h17p,

	[592] = trampoline_nop,
	[594] = trampoline_swap_complete,
	[596] = trampoline_swap_complete_intent_gated,
	[597] = trampoline_abort_swap_ap_gated,
	[598] = trampoline_enable_backlight_message_ap_gated,
	[599] = trampoline_nop,
	[601] = trampoline_false,
	[602] = trampoline_false,
	[603] = trampoline_nop,
};

static const struct {
	u16 input, output;
} callback_sizes[IOMFB_MAX_CB] = {
	[0] = { 0x0, 0x4 },
	[1] = { 0x0, 0x4 },
	[2] = { 0x0, 0x0 },
	[3] = { 0x4, 0x14 },
	[6] = { 0x54, 0x50 },
	[300] = { 0x10, 0x0 },
	[100] = { 0x0, 0x0 },
	[101] = { 0x0, 0x4 },
	[102] = { 0x44, 0x0 },
	[104] = { 0x44, 0x0 },
	[106] = { 0x40, 0x0 },
	[107] = { 0x0, 0x4 },
	[108] = { 0x0, 0x4 },
	[109] = { 0x0, 0x4 },
	[110] = { 0x0, 0x4 },
	[111] = { 0x0, 0x4 },
	[112] = { 0x0, 0x4 },
	[113] = { 0x1044, 0x1004 },
	[116] = { 0xc, 0x0 },
	[117] = { 0x14, 0x4 },
	[118] = { 0x3e88, 0x4 },
	[119] = { 0x4, 0x4 },
	[114] = { 0x10, 0x8 },
	[120] = { 0x0, 0x4 },
	[121] = { 0x0, 0x4 },
	[122] = { 0x0, 0x4 },
	[123] = { 0x0, 0x4 },
	[124] = { 0x64, 0x24 },
	[126] = { 0x4, 0x4 },
	[127] = { 0x1008, 0x4 },
	[128] = { 0x40, 0x4 },
	[129] = { 0x1c, 0x14 },
	[130] = { 0x8, 0x0 },
	[131] = { 0x10, 0xc },
	[132] = { 0x4, 0x0 },
	[133] = { 0x0, 0x4 },
	[201] = { 0xc, 0x10 },
	[202] = { 0x18, 0x0 },
	[206] = { 0x0, 0x4 },
	[207] = { 0x0, 0x4 },
	[204] = { 0x0, 0x4 },
	[208] = { 0x4, 0x0 },
	[209] = { 0x0, 0x8 },
	[400] = { 0x4c, 0xc04 },
	[401] = { 0x50, 0xc },
	[404] = { 0x48, 0x0 },
	[406] = { 0x48, 0x0 },
	[408] = { 0x8, 0x8 },
	[411] = { 0x10, 0x1c },
	[413] = { 0x1048, 0x4 },
	[414] = { 0x50, 0x4 },
	[415] = { 0x4c, 0x4 },
	[451] = { 0x14, 0x14 },
	[452] = { 0x8, 0x4 },
	[453] = { 0x8, 0x4 },
	[454] = { 0x4, 0x4 },
	[552] = { 0x1044, 0x4 },
	[561] = { 0x1044, 0x4 },
	[563] = { 0x4c, 0x4 },
	[565] = { 0x48, 0x4 },
	[567] = { 0x80, 0x4 },
	[569] = { 0x44, 0x4 },
	[572] = { 0x4, 0x4 },
	[574] = { 0x4, 0x8 },
	[575] = { 0x58, 0x4c },
	[576] = { 0x4, 0x0 },
	[586] = { 0x8, 0x4 },
	[587] = { 0x0, 0x0 },
	[588] = { 0x18, 0x0 },
	[589] = { 0xe4, 0xe0 },
	[592] = { 0x0, 0x0 },
	[594] = { 0x730, 0x0 },
	[596] = { 0x14, 0x0 },
	[597] = { 0x4, 0x0 },
	[598] = { 0x4, 0x0 },
	[599] = { 0x4, 0x0 },
	[601] = { 0x0, 0x4 },
	[602] = { 0x0, 0x4 },
	[603] = { 0x0, 0x0 },
};

bool iomfb_v27_callback_size_valid(unsigned int tag, u32 input, u32 output)
{
	if (tag >= ARRAY_SIZE(callback_sizes) || !cb_handlers[tag])
		return false;
	return input >= callback_sizes[tag].input &&
	       output >= callback_sizes[tag].output;
}

void iomfb_start_v27(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;
	dcp_start_signal(dcp, false, dcp_started, NULL);
}

#undef DCP_FW_VER
#undef DCP_FW
