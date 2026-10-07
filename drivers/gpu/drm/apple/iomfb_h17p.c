// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include <linux/bitops.h>
#include <linux/unaligned.h>

#include "dcp-link.h"
#include "iomfb_h17p.h"
#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "version_utils.h"

struct dcp_callback_size {
	u32 in_len;
	u32 out_len;
	bool valid;
};

/* R-IOMFB-startup-run21 and R-ANALYTICS Linux boundary observations. */
static const struct dcp_callback_size callback_sizes[IOMFB_MAX_CB] = {
	[0] = { 0x0, 0x4, true },
	[1] = { 0x0, 0x4, true },
	[3] = { 0x4, 0x14, true },
	[6] = { 0x54, 0x50, true },
	[100] = { 0x0, 0x0, true },
	[101] = { 0x0, 0x4, true },
	[102] = { 0x44, 0x0, true },
	[104] = { 0x44, 0x0, true },
	[108] = { 0x0, 0x4, true },
	[109] = { 0x0, 0x4, true },
	[110] = { 0x0, 0x4, true },
	[111] = { 0x0, 0x4, true },
	[112] = { 0x0, 0x4, true },
	[113] = { 0x0, 0x4, true },
	[114] = { 0x1044, 0x1004, true },
	[121] = { 0x0, 0x4, true },
	[123] = { 0x0, 0x4, true },
	[125] = { 0x64, 0x24, true },
	[127] = { 0x4, 0x4, true },
	[128] = { 0x1008, 0x4, true },
	[129] = { 0x40, 0x4, true },
	[201] = { 0xc, 0x10, true },
	[206] = { 0x0, 0x4, true },
	[207] = { 0x0, 0x4, true },
	[300] = { 0x10, 0x0, true },
	[400] = { 0x4c, 0xc04, true },
	[401] = { 0x50, 0xc, true },
	[406] = { 0x48, 0x0, true },
	[411] = { 0x10, 0x1c, true },
	[413] = { 0x1048, 0x4, true },
	[414] = { 0x50, 0x4, true },
	[415] = { 0x4c, 0x4, true },
	[451] = { 0x14, 0x1c, true },
	[552] = { 0x1044, 0x4, true },
	[561] = { 0x1044, 0x4, true },
	[563] = { 0x4c, 0x4, true },
	[565] = { 0x48, 0x4, true },
	[574] = { 0x4, 0x8, true },
	[575] = { 0x58, 0x4c, true },
	[582] = { 0x8, 0x4, true },
	[590] = { 0x730, 0x0, true },
	[599] = { 0x0, 0x0, true },
};

bool iomfb_validate_callback_h17p(int tag, u32 in_len, u32 out_len)
{
	const struct dcp_callback_size *size;

	if (tag < 0 || tag >= ARRAY_SIZE(callback_sizes))
		return false;
	size = &callback_sizes[tag];
	return size->valid && size->in_len == in_len && size->out_len == out_len;
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
/* D590 swap_complete_ap_gated is 0x730 bytes on the wire. */
static_assert(sizeof(struct dc_swap_complete_resp_h17p) == 0x730);
static_assert(offsetof(struct dc_swap_complete_resp_h17p, swap_id) == 0);

void iomfb_serialize_present_h17p(struct dcp_present_h17p *wire,
				  const struct dcp_swap_submit_req_h17p *request)
{
	size_t tail_offset = offsetof(struct dcp_swap_submit_req_h17p, surf_iova);
	size_t tail_size = sizeof(*request) - tail_offset;

	static_assert(sizeof(request->swap) == sizeof(wire->swap) + 2);
	static_assert(sizeof(request->surf) == sizeof(wire->surf));
	static_assert(sizeof(*request) -
		      offsetof(struct dcp_swap_submit_req_h17p, surf_iova) ==
		      sizeof(wire->tail) - 2);

	memcpy(wire->swap, &request->swap, sizeof(wire->swap));
	memcpy(wire->surf, request->surf, sizeof(wire->surf));
	memcpy(wire->tail, (const u8 *)request + tail_offset, tail_size);
	memset(wire->tail + tail_size, 0, sizeof(wire->tail) - tail_size);
}

/* Integer commanded nits are exact binary64 values; no kernel FP is needed. */
void iomfb_encode_backlight_h17p(struct dcp_present_h17p *wire, u32 nits)
{
	u64 value = 0;
	unsigned int exponent;

	if (nits) {
		exponent = fls(nits) - 1;
		value = (u64)(exponent + 1023) << 52;
		value |= ((u64)nits << (52 - exponent)) & GENMASK_ULL(51, 0);
	}
	/* Measured from Linux presents during ordinary sysfs brightness changes. */
	put_unaligned_le64(value, wire->swap + 0x35e);
}

static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),
	IOMFB_METHOD_H17("A025", "A029", dcpep_setup_video_limits), /* (0,0) */
	IOMFB_METHOD("A131", iomfbep_a131_pmu_service_matched), /* nested in D206 */
	IOMFB_METHOD("A132", iomfbep_a132_backlight_service_matched), /* nested in D207 */
	IOMFB_METHOD("A385", dcpep_set_create_dfb), /* first call inside D121; (0,0) */
	IOMFB_METHOD("A386", iomfbep_a358_vi_set_temperature_hint), /* nested in D100 */
	IOMFB_METHOD("A401", dcpep_start_signal),
	IOMFB_METHOD("A406", dcpep_swap_start),
	/*
	 * swap_start moved A407 -> A406 on H17P, but swap_submit did not move
	 * with it: every swap is A406 followed by A408, A407 is unused, and the
	 * A408 response is 0x0c bytes (dcp_swap_submit_resp_h17p).
	 */
	IOMFB_METHOD("A408", dcpep_swap_submit),
	IOMFB_METHOD("A411", dcpep_set_display_device),
	IOMFB_METHOD("A412", dcpep_is_main_display),
	IOMFB_METHOD("A413", dcpep_set_digital_out_mode),
	IOMFB_METHOD("A423", iomfbep_set_matrix),
	IOMFB_METHOD("A427", iomfbep_get_color_remap_mode),
	IOMFB_METHOD("A442", dcpep_set_parameter_dcp), /* (0x28, 0x4) */
	IOMFB_METHOD("A446", dcpep_create_default_fb), /* follows A385 in D121 */
	/*
	 * The default-framebuffer block the firmware expects inside D121,
	 * straight after A446 and before A025.  Sizes are (in, out).
	 */
	IOMFB_METHOD_H17("A035", "A039", dcpep_get_dfb_compression_info), /* (0x8, 0x8) */
	IOMFB_METHOD_H17("A034", "A038", dcpep_get_dfb_info), /* (0x2c, 0x2c) */
	IOMFB_METHOD("A103", dcpep_get_dfb_layout),           /* (0xc, 0x8) */
	IOMFB_METHOD("A445", dcpep_get_dfb_state),            /* (0x4, 0x8) */
	IOMFB_METHOD("A104", dcpep_set_dfb_dimensions),       /* (0x8, 0) w,h */
	IOMFB_METHOD("A105", dcpep_commit_dfb_info),          /* (0, 0) */
	IOMFB_METHOD_H17("A033", "A037", dcpep_dfb_query),    /* (0, 0x4) */
	IOMFB_METHOD("A380", dcpep_dfb_ready_query),          /* (0, 0x4) */
	IOMFB_METHOD("A415", dcpep_pipe_cfg_415),             /* (0xc, 0xc) zeros */
	/* update_notify_clients_dcp */
	IOMFB_METHOD_H17("A031", "A035", dcpep_pipe_cfg_031), /* (0x6c, 0) */
	IOMFB_METHOD("A414", dcpep_pipe_cfg_414),             /* (0x8, 0x8) zeros */
	IOMFB_METHOD("A478", dcpep_pipe_query_478),           /* (0, 0x4) */
	IOMFB_METHOD("A474", dcpep_pipe_query_474),           /* (0, 0x4) */
	IOMFB_METHOD("A476", dcpep_pipe_set_476),             /* (0x4, 0) in 1 */
	IOMFB_METHOD("A428", dcpep_pipe_set_428),             /* (0x4, 0x4) 0x10000 */
	IOMFB_METHOD("A450", dcpep_enable_disable_video_power_savings), /* in = u32 0 */
	IOMFB_METHOD("A457", dcpep_first_client_open), /* (0, 0) */
	/* D561 (displayMinRefreshInterval) arrives while A465 is in flight */
	IOMFB_METHOD("A465", dcpep_set_display_refresh_properties),
	IOMFB_METHOD("A468", dcpep_flush_supports_power), /* follows A025 in D121, in = 1 */
	/*
	 * iomfbep_last_client_close and iomfbep_abort_swaps_dcp are deliberately
	 * absent: their H17P numbers are unverified, so the teardown paths skip
	 * them (DCP_HAS_CLIENT_TEARDOWN in iomfb_template.c).
	 */
	IOMFB_METHOD("A473", dcpep_set_power_state), /* out-of-band; out 0x8 */
	IOMFB_METHOD("A472", dcpep_register_dfb_surface), /* default FB surface, nested in D582 */
};

#define DCP_FW h17p
#define DCP_FW_VER DCP_FW_VERSION(26, 6, 0)

#include "iomfb_template.c"

static bool trampoline_rt_bandwidth_h17p(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	/*
	 * The bandwidth scratch and doorbell come from the DCP's DT node; the
	 * apple,bw-doorbell binding has no offset cell.
	 */
	u64 scratch = dcp->disp_bw_scratch_res.start +
		      dcp->disp_bw_scratch_offset;
	u64 clock_request = dcp->disp_bw_doorbell_res.start;

	trace_iomfb_callback(dcp, tag, __func__);
	if (apple_dcp_h17p_rt_bw_encode_reply(in, out, scratch, clock_request))
		dev_warn(dcp->dev, "D003 bandwidth request is malformed\n");
	return true;
}

static bool trampoline_get_frequency_h17p(struct apple_dcp *dcp, int tag,
					   void *out, void *in)
{
	const struct apple_dcp_h17p_clock_request *request = in;
	u64 rate;
	int ret;

	trace_iomfb_callback(dcp, tag, __func__);
	ret = apple_dcp_h17p_clock_rate(request, clk_get_rate(dcp->clk),
					  clk_get_rate(dcp->clk_194), &rate);
	if (ret) {
		dev_warn(dcp->dev, "unknown display clock ID %#x\n",
			 le32_to_cpu(request->clock_id));
		rate = 0;
	}

	*(__le64 *)out = cpu_to_le64(rate);
	return true;
}

static bool trampoline_analytics_h17p(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G)
		return trampoline_zero(dcp, tag, out, in);

	trace_iomfb_callback(dcp, tag, __func__);
	/* The dispatcher has validated the measured D114 reply size. */
	memset(out, 0, 0x1004);
	*(u8 *)out = 'd';
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

struct dcp_h17p_provider_request {
	u8 scope[4];
	char name[64];
	__le32 capacity;
	u8 reserved[4];
} __packed;

struct dcp_h17p_provider_reply {
	u8 data[0xc00];
	__le32 length;
} __packed;

struct dcp_h17p_provider_property {
	const char *name;
	const char *dt_name;
	u32 length;
};

static_assert(sizeof(struct dcp_h17p_provider_request) == 0x4c);
static_assert(sizeof(struct dcp_h17p_provider_reply) == 0xc04);

/* Only these provider transfers have been measured against the disp0 ADT. */
static const struct dcp_h17p_provider_property provider_properties[] = {
	{ "power-lut-data-x", "apple,power-lut-data-x", 4 },
	{ "power-lut-data-y", "apple,power-lut-data-y", 4 },
	{ "power-lut-data-xindex", "apple,power-lut-data-xindex", 52 },
	{ "power-lut-data-yindex", "apple,power-lut-data-yindex", 4 },
	{ "power-lut-data-lut", "apple,power-lut-data-lut", 52 },
	{ "power-lut-vbatt-cur-nominal", "apple,power-lut-vbatt-cur-nominal", 0 },
};

static const struct dcp_h17p_provider_property *
dcp_provider_property_h17p(const struct dcp_h17p_provider_request *request)
{
	unsigned int i;

	if (memcmp(request->scope, "VORP", sizeof(request->scope)) ||
	    le32_to_cpu(request->capacity) !=
				 sizeof_field(struct dcp_h17p_provider_reply, data) ||
	    memchr_inv(request->reserved, 0, sizeof(request->reserved)) ||
	    strnlen(request->name, sizeof(request->name)) == sizeof(request->name))
		return NULL;

	/* Bytes following the first NUL were not initialized by the firmware. */
	for (i = 0; i < ARRAY_SIZE(provider_properties); i++)
		if (!strcmp(request->name, provider_properties[i].name))
			return &provider_properties[i];
	return NULL;
}

static bool trampoline_provider_property_h17p(struct apple_dcp *dcp, int tag,
					      void *out, void *in)
{
	const struct dcp_h17p_provider_property *property;
	struct dcp_h17p_provider_reply *reply = out;
	struct device_node *node = dcp->dev->of_node;
	int length, ret;

	/* The dispatcher already cleared the exact H17G output length. */
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G)
		return true;

	trace_iomfb_callback(dcp, tag, __func__);
	property = dcp_provider_property_h17p(in);
	if (!property) {
		dev_err(dcp->dev, "unqualified D400 provider property request\n");
		goto fail;
	}

	length = of_property_count_u8_elems(node, property->dt_name);
	if (!property->length) {
		/* Only this absent property has a measured empty reply. */
		if (of_property_present(node, property->dt_name))
			goto fail;
	} else if (length != (int)property->length) {
		dev_err(dcp->dev, "invalid provider property %s: %d bytes\n",
			property->dt_name, length);
		goto fail;
	}

	memset(reply, 0, sizeof(*reply));
	if (property->length) {
		ret = of_property_read_u8_array(node, property->dt_name,
						reply->data, property->length);
		if (ret)
			goto fail;
	}
	reply->length = cpu_to_le32(property->length);
	return true;

fail:
	WRITE_ONCE(dcp->crashed, true);
	return false;
}

static bool trampoline_allocate_buffer_h17p(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	const struct dcp_allocate_buffer_req *wire = in;
	struct dcp_allocate_buffer_req request = { 0 };
	struct dcp_allocate_buffer_resp reply;
	u32 options;

	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G)
		return trampoline_allocate_buffer(dcp, tag, out, in);

	static_assert(sizeof(*wire) == 0x14);
	static_assert(sizeof(reply) == 0x1c);
	static_assert(offsetof(struct dcp_allocate_buffer_req, size) == 4);
	static_assert(offsetof(struct dcp_allocate_buffer_req, unk2) == 12);
	static_assert(offsetof(struct dcp_allocate_buffer_req, paddr_null) == 16);

	trace_iomfb_callback(dcp, tag, __func__);
	options = get_unaligned_le32(&wire->unk2);
	/* Only these option combinations have observed successful replies. */
	if (get_unaligned_le32(&wire->unk0) != 0x703 ||
	    (options != 4 && options != SZ_16K) ||
	    memchr_inv(&wire->paddr_null, 0, 4))
		goto fail;

	request.size = get_unaligned_le64(&wire->size);
	reply = dcpep_cb_allocate_buffer(dcp, &request);
	if (!reply.dva_size)
		goto fail;

	put_unaligned_le64(reply.paddr, out);
	put_unaligned_le64(reply.dva, (u8 *)out + 8);
	put_unaligned_le64(reply.dva_size, (u8 *)out + 16);
	put_unaligned_le32(reply.mem_desc_id, (u8 *)out + 24);
	return true;

fail:
	/* Allocation failures have no measured H17P reply encoding. */
	dev_err(dcp->dev, "unqualified or failed D451 buffer allocation\n");
	WRITE_ONCE(dcp->crashed, true);
	return false;
}

/* H17P callback numbering is not a uniform shift of the v13.5 table. */
static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = dcpep_cb_d000_h17p, /* acked after a nested A033 */
	[1] = trampoline_true,
	[2] = trampoline_nop,
	[3] = trampoline_rt_bandwidth_h17p,
	[6] = trampoline_set_frame_sync_props_h17p, /* 0x54/0x50, identity scale */
	[100] = iomfbep_cb_match_pmu_service, /* match_pmu_service */
	[101] = trampoline_zero,
	[102] = trampoline_nop, /* set_number_property */
	[104] = trampoline_nop, /* set_boolean_property */
	[107] = trampoline_nop,
	[108] = trampoline_true, /* create_provider_service */
	[109] = trampoline_true, /* create_product_service */
	[110] = trampoline_true, /* create_PMU_service */
	[111] = trampoline_true, /* create_iomfb_service */
	[112] = trampoline_create_backlight_service, /* create_backlight_service */
	[113] = trampoline_true, /* create_nvram_service */
	[114] = trampoline_analytics_h17p, /* CoreAnalyticsSendEvent */
	[117] = trampoline_nop, /* set_idle_caching_state_ap */
	[118] = trampoline_zero, /* upload_trace_start */
	[119] = trampoline_zero, /* upload_trace_chunk */
	[120] = trampoline_zero, /* upload_trace_end */
	[121] = dcpep_cb_boot_1, /* start_hardware_boot */
	[122] = trampoline_false, /* is_dark_boot */
	[123] = trampoline_false, /* is_waking_from_hibernate */
	[124] = trampoline_zero, /* detect_fastsim */
	[125] = trampoline_read_edt_data, /* read_edt_data */
	[127] = trampoline_prop_start, /* setDCPAVPropStart */
	[128] = trampoline_prop_chunk, /* setDCPAVPropChunk */
	[129] = trampoline_prop_end, /* setDCPAVPropEnd */
	[130] = trampoline_allocate_bandwidth, /* allocate_bandwidth */
	[201] = trampoline_map_piodma, /* map_buf */
	[202] = trampoline_unmap_piodma, /* unmap_buf */
	[206] = iomfbep_cb_match_pmu_service_2, /* match_pmu_service */
	[207] = iomfbep_cb_match_backlight_service, /* match_backlight_service */
	[208] = trampoline_nop, /* update_backlight_factor_prop */
	[209] = trampoline_get_time, /* get_calendar_time_ms */
	[300] = trampoline_pr_publish,
	[400] = trampoline_provider_property_h17p,
	[401] = trampoline_get_uint_prop, /* get_uint_prop */
	[404] = trampoline_nop, /* set_uint_prop */
	[406] = trampoline_set_fx_prop, /* set_fx_prop */
	[408] = trampoline_get_frequency_h17p, /* getClockFrequency */
	[411] = trampoline_map_reg, /* mapDeviceMemoryWithIndex */
	[413] = trampoline_true, /* setProperty */
	[414] = trampoline_sr_set_property_int, /* setProperty */
	[415] = trampoline_true, /* setProperty */
	[451] = trampoline_allocate_buffer_h17p, /* allocate_buffer */
	[452] = trampoline_map_physical, /* prepare */
	[454] = trampoline_release_mem_desc, /* release_descriptor */
	[552] = trampoline_true,
	[561] = trampoline_true,
	[563] = trampoline_true,
	[565] = trampoline_true,
	[567] = trampoline_true,
	[572] = trampoline_zero, /* powerUpDART */
	[575] = trampoline_hotplug_h17p,
	[576] = trampoline_nop,
	[582] = iomfbep_cb_create_dfb_surface, /* create_default_fb_surface */
	[583] = trampoline_nop, /* clear_default_surface */
	[584] = trampoline_nop, /* swap_notify_gated */
	[585] = trampoline_swap_info_h17p, /* swap_info_notify_dispatch */
	[590] = trampoline_swap_complete, /* swap_complete_ap_gated */
	[592] = trampoline_swap_complete_intent_gated, /* swap_complete_intent_gated */
	[593] = trampoline_abort_swap_ap_gated, /* abort_swap_ap_gated */
	[594] = trampoline_enable_backlight_message_ap_gated,
	[595] = trampoline_nop, /* setSystemConsoleMode */
	[597] = trampoline_false, /* isDFBAllocated */
	[598] = trampoline_false,
	[599] = trampoline_nop, /* find_swap_function_gated */
};

void iomfb_start_h17p(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;
	dcp_start_signal(dcp, false, dcp_started, NULL);
}

#undef DCP_FW_VER
#undef DCP_FW
