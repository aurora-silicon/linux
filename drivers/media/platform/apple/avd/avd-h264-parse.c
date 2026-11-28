// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone H.264 Annex-B NALU scanner and ITU-T H.264 7.3.3 slice header
 * parser for V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED on Apple AVD.
 */

#include <kunit/visibility.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "avd-h264-parse.h"

#define AVD_H264_NAL_SLICE		1
#define AVD_H264_NAL_DPA		2
#define AVD_H264_NAL_DPB		3
#define AVD_H264_NAL_DPC		4
#define AVD_H264_NAL_IDR_SLICE		5
#define AVD_H264_NAL_SEI		6
#define AVD_H264_NAL_FILLER		12
#define AVD_H264_NAL_PREFIX		14
#define AVD_H264_NAL_EXT_SLICE		20

#define AVD_H264_MAX_MMCO_OPS		64

/*
 * Scan an Annex-B byte stream for the next VCL slice NALU (types 1 and 5).
 * Non-VCL NALUs (SEI, SPS, PPS, AUD, EOS, filler; types 6..12) are skipped.
 * Forbidden-zero-bit violations, data partitioning (2..4), SVC/MVC (14, 20),
 * and other unsupported NAL types return -EINVAL.
 *
 * Returns 1 when a slice NALU is written to @nalu, 0 at end of buffer, or
 * -EINVAL on malformed Annex-B syntax or unsupported NAL unit types.
 */
int avd_h264_next_nalu(const u8 *buf, u32 len, u32 *cursor,
		       struct avd_h264_nalu *nalu)
{
	u32 pos;

	if (!cursor || !nalu)
		return -EINVAL;
	if (!buf)
		return len ? -EINVAL : 0;

	pos = *cursor;
	while (pos < len) {
		u32 zeros = 0;
		u32 nal_start, end, i;
		u8 hdr, nal_type;

		while (pos < len && buf[pos] == 0x00) {
			zeros++;
			pos++;
		}
		if (pos == len) {
			*cursor = len;
			return 0;
		}
		if (zeros < 2 || buf[pos] != 0x01)
			return -EINVAL;

		nal_start = pos + 1;
		if (nal_start >= len)
			return -EINVAL;

		/* Find the next 0x00 0x00 0x01 start code prefix. */
		end = len;
		i = nal_start;
		while (len >= 3 && i <= len - 3) {
			if (buf[i + 2] > 0x01) {
				i += 3;
				continue;
			}
			if (buf[i + 2] == 0x01 && buf[i] == 0x00 &&
			    buf[i + 1] == 0x00) {
				end = i;
				break;
			}
			i++;
		}

		*cursor = end;
		pos = end;

		/* Strip trailing zero bytes (4-byte start code prefix / padding). */
		while (end > nal_start && buf[end - 1] == 0x00)
			end--;
		if (end == nal_start)
			return -EINVAL;

		hdr = buf[nal_start];
		if (hdr & 0x80)
			return -EINVAL;

		nal_type = hdr & 0x1f;
		switch (nal_type) {
		case AVD_H264_NAL_SLICE:
		case AVD_H264_NAL_IDR_SLICE:
			nalu->off = nal_start;
			nalu->len = end - nal_start;
			nalu->nal_ref_idc = (hdr >> 5) & 0x03;
			nalu->nal_unit_type = nal_type;
			return 1;
		case AVD_H264_NAL_SEI ... AVD_H264_NAL_FILLER:
			continue;
		case AVD_H264_NAL_DPA ... AVD_H264_NAL_DPC:
		case AVD_H264_NAL_PREFIX:
		case AVD_H264_NAL_EXT_SLICE:
		default:
			return -EINVAL;
		}
	}

	*cursor = len;
	return 0;
}

bool avd_h264_mode_supported(s32 decode_mode, s32 start_code)
{
	switch (decode_mode) {
	case V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED:
		return start_code == V4L2_STATELESS_H264_START_CODE_NONE;
	case V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED:
		return start_code == V4L2_STATELESS_H264_START_CODE_ANNEX_B ||
		       start_code == V4L2_STATELESS_H264_START_CODE_NONE;
	default:
		return false;
	}
}

static bool avd_h264_has_annexb_start_code(const u8 *buf, u32 len)
{
	u32 pos = 0;

	while (pos < len && buf[pos] == 0x00)
		pos++;

	return pos >= 2 && pos < len && buf[pos] == 0x01;
}

/*
 * Parse a single unprefixed ITU-T H.264 7.3.1 VCL slice NALU for
 * FRAME_BASED + START_CODE_NONE. No Annex-B start code prefix is present
 * and none may appear inside the EBSP payload.
 */
static int avd_h264_parse_raw_nalu(const u8 *buf, u32 len,
				   struct avd_h264_nalu *nalu)
{
	u32 end = len, i;
	u8 hdr, nal_type;

	if (!buf || !nalu || !len)
		return -EINVAL;

	while (end > 0 && buf[end - 1] == 0x00)
		end--;
	if (end < 2)
		return -EINVAL;

	/* An EBSP must not contain an unescaped 0x00 0x00 0x01 start code. */
	for (i = 0; end >= 3 && i <= end - 3; i++) {
		if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01)
			return -EINVAL;
	}

	hdr = buf[0];
	if (hdr & 0x80)
		return -EINVAL;

	nal_type = hdr & 0x1f;
	if (nal_type != AVD_H264_NAL_SLICE &&
	    nal_type != AVD_H264_NAL_IDR_SLICE)
		return -EINVAL;

	nalu->off = 0;
	nalu->len = end;
	nalu->nal_ref_idc = (hdr >> 5) & 0x03;
	nalu->nal_unit_type = nal_type;
	return 1;
}

/*
 * Iterate VCL slice NALUs in a FRAME_BASED OUTPUT buffer according to
 * V4L2_CID_STATELESS_H264_START_CODE:
 * - START_CODE_ANNEX_B requires Annex-B start codes (0x000001 / 0x00000001).
 * - START_CODE_NONE accepts a single unprefixed ITU-T H.264 7.3.1 VCL slice
 *   NALU. For compatibility with clients (such as Chromium's
 *   V4L2VideoDecoderDelegateH264) that set DECODE_MODE=FRAME_BASED without
 *   writing START_CODE, leaving it at this driver's START_CODE_NONE default
 *   while still emitting Annex-B start codes, a buffer starting with an
 *   Annex-B start code is dispatched to the Annex-B scanner. That cannot
 *   collide with valid START_CODE_NONE data because no valid 7.3.1 NALU
 *   begins with 0x00 (nal_unit_type 0 is invalid).
 */
int avd_h264_next_frame_nalu(const u8 *buf, u32 len, s32 start_code,
			     u32 *cursor, struct avd_h264_nalu *nalu)
{
	int ret;

	if (!cursor || !nalu)
		return -EINVAL;

	if (start_code == V4L2_STATELESS_H264_START_CODE_ANNEX_B)
		return avd_h264_next_nalu(buf, len, cursor, nalu);

	if (start_code != V4L2_STATELESS_H264_START_CODE_NONE)
		return -EINVAL;

	if (!buf)
		return len ? -EINVAL : 0;
	if (avd_h264_has_annexb_start_code(buf, len))
		return avd_h264_next_nalu(buf, len, cursor, nalu);
	if (*cursor >= len)
		return 0;

	ret = avd_h264_parse_raw_nalu(buf, len, nalu);
	if (ret > 0)
		*cursor = len;
	return ret;
}

VISIBLE_IF_KUNIT void avd_h264_br_init(struct avd_h264_br *br, const u8 *data,
				       u32 len, u32 pos)
{
	br->data = data;
	br->len = len;
	br->pos = pos;
	br->rbsp_bits = 0;
	br->bit = 0;
	br->zeros = 0;
	br->err = false;
}
EXPORT_SYMBOL_IF_KUNIT(avd_h264_br_init);

/*
 * At a byte boundary immediately after two consecutive raw 0x00 bytes, an
 * emulation prevention byte (0x03) belongs to the EBSP framing, not the RBSP.
 */
VISIBLE_IF_KUNIT void avd_h264_br_skip_epb(struct avd_h264_br *br)
{
	if (br->bit == 0 && br->zeros >= 2 && br->pos < br->len &&
	    br->data[br->pos] == 0x03) {
		br->pos++;
		br->zeros = 0;
	}
}
EXPORT_SYMBOL_IF_KUNIT(avd_h264_br_skip_epb);

static u32 avd_h264_br_read_bit(struct avd_h264_br *br)
{
	u32 val;
	u8 b;

	if (br->err)
		return 0;

	if (br->bit == 0) {
		avd_h264_br_skip_epb(br);
		if (br->pos >= br->len) {
			br->err = true;
			return 0;
		}
		b = br->data[br->pos];
		if (b == 0x00)
			br->zeros = min_t(u8, br->zeros + 1, 2);
		else
			br->zeros = 0;
	} else {
		b = br->data[br->pos];
	}

	val = (b >> (7 - br->bit)) & 1;
	br->rbsp_bits++;
	br->bit++;
	if (br->bit == 8) {
		br->bit = 0;
		br->pos++;
	}

	return val;
}

VISIBLE_IF_KUNIT u32 avd_h264_br_read_bits(struct avd_h264_br *br,
					   unsigned int n)
{
	u32 val = 0;

	if (n == 0)
		return 0;
	if (n > 32) {
		br->err = true;
		return 0;
	}

	while (n--)
		val = (val << 1) | avd_h264_br_read_bit(br);

	return val;
}
EXPORT_SYMBOL_IF_KUNIT(avd_h264_br_read_bits);

VISIBLE_IF_KUNIT u32 avd_h264_br_read_ue(struct avd_h264_br *br)
{
	unsigned int zeros = 0;
	u32 suffix;

	while (!br->err && !avd_h264_br_read_bit(br)) {
		zeros++;
		if (zeros >= 32) {
			br->err = true;
			return 0;
		}
	}
	if (br->err || zeros == 0)
		return 0;

	suffix = avd_h264_br_read_bits(br, zeros);
	if (br->err)
		return 0;

	if (zeros == 31) {
		if (suffix > BIT(31)) {
			br->err = true;
			return 0;
		}
		return (BIT(31) - 1) + suffix;
	}

	return ((1U << zeros) - 1) + suffix;
}
EXPORT_SYMBOL_IF_KUNIT(avd_h264_br_read_ue);

VISIBLE_IF_KUNIT s32 avd_h264_br_read_se(struct avd_h264_br *br)
{
	u32 code = avd_h264_br_read_ue(br);

	if (br->err)
		return 0;
	if (code == U32_MAX) {
		br->err = true;
		return 0;
	}
	if (code & 1)
		return (s32)((code >> 1) + 1);

	return -(s32)(code >> 1);
}
EXPORT_SYMBOL_IF_KUNIT(avd_h264_br_read_se);

static int avd_h264_find_short_term_dpb(const struct v4l2_h264_dpb_entry *dpb,
					u32 cur_frame_num, u32 max_pic_num,
					s32 target_pic_num)
{
	for (unsigned int i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		s32 dpb_pic_num;

		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
			continue;
		if (dpb[i].frame_num >= max_pic_num)
			continue;

		dpb_pic_num = dpb[i].frame_num;
		if (dpb[i].frame_num > cur_frame_num)
			dpb_pic_num -= (s32)max_pic_num;

		if (dpb_pic_num == target_pic_num)
			return i;
	}

	return -EINVAL;
}

static int avd_h264_find_long_term_dpb(const struct v4l2_h264_dpb_entry *dpb,
				       u32 lt_pic_num)
{
	for (unsigned int i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM))
			continue;
		if (dpb[i].frame_num == lt_pic_num)
			return i;
	}

	return -EINVAL;
}

/* H.264 7.3.3.1 & 8.2.4.3.1 for frame pictures (frame_mbs_only_flag == 1). */
static int
avd_h264_parse_ref_list_mod(struct avd_h264_br *br,
			    const struct v4l2_ctrl_h264_sps *sps,
			    const struct v4l2_ctrl_h264_decode_params *decode,
			    u32 frame_num, struct v4l2_h264_reference *list,
			    u8 num_active_minus1)
{
	u32 max_pic_num = 1U << (sps->log2_max_frame_num_minus4 + 4);
	u32 num_active = (u32)num_active_minus1 + 1;
	s32 pic_num_pred = frame_num;
	u32 ref_idx = 0;

	if (!avd_h264_br_read_bit(br))
		return br->err ? -EINVAL : 0;

	for (unsigned int ops = 0; ops < V4L2_H264_REF_LIST_LEN + 1; ops++) {
		u32 idc = avd_h264_br_read_ue(br);
		int target;
		u32 c, n;

		if (br->err)
			return -EINVAL;
		if (idc == 3)
			return 0;
		if (idc > 3 || ref_idx >= num_active)
			return -EINVAL;

		if (idc == 0 || idc == 1) {
			u32 diff_minus1 = avd_h264_br_read_ue(br);
			s32 diff, pic_num_no_wrap, pic_num;

			if (br->err || diff_minus1 >= max_pic_num)
				return -EINVAL;

			diff = (s32)diff_minus1 + 1;
			if (idc == 0) {
				pic_num_no_wrap = pic_num_pred - diff;
				if (pic_num_no_wrap < 0)
					pic_num_no_wrap += (s32)max_pic_num;
			} else {
				pic_num_no_wrap = pic_num_pred + diff;
				if (pic_num_no_wrap >= (s32)max_pic_num)
					pic_num_no_wrap -= (s32)max_pic_num;
			}

			pic_num_pred = pic_num_no_wrap;
			if (pic_num_no_wrap > (s32)frame_num)
				pic_num = pic_num_no_wrap - (s32)max_pic_num;
			else
				pic_num = pic_num_no_wrap;

			target = avd_h264_find_short_term_dpb(decode->dpb,
							      frame_num,
							      max_pic_num,
							      pic_num);
		} else {
			u32 lt_pic_num = avd_h264_br_read_ue(br);

			if (br->err)
				return -EINVAL;

			target = avd_h264_find_long_term_dpb(decode->dpb,
							     lt_pic_num);
		}

		if (target < 0)
			return -EINVAL;

		for (c = num_active; c > ref_idx; c--)
			list[c] = list[c - 1];

		list[ref_idx].fields = V4L2_H264_FRAME_REF;
		list[ref_idx].index = target;
		ref_idx++;

		n = ref_idx;
		for (c = ref_idx; c <= num_active; c++) {
			if (list[c].index != target)
				list[n++] = list[c];
		}
		for (c = n; c <= num_active; c++) {
			list[c].fields = 0;
			list[c].index = 0xff;
		}
	}

	return -EINVAL;
}

static int
avd_h264_parse_weight_entry(struct avd_h264_br *br,
			    struct v4l2_h264_weight_factors *wf, u32 i,
			    u32 luma_denom, u32 chroma_denom, bool chroma)
{
	if (avd_h264_br_read_bit(br)) {
		s32 w = avd_h264_br_read_se(br);
		s32 o = avd_h264_br_read_se(br);

		if (br->err || w < -128 || w > 127 || o < -128 || o > 127)
			return -EINVAL;
		wf->luma_weight[i] = w;
		wf->luma_offset[i] = o;
	} else {
		wf->luma_weight[i] = 1 << luma_denom;
		wf->luma_offset[i] = 0;
	}

	if (!chroma) {
		/*
		 * Keep monochrome chroma weights at the implicit identity
		 * (1 << 0) so stream_weights() does not emit chroma commands.
		 */
		wf->chroma_weight[i][0] = 1;
		wf->chroma_weight[i][1] = 1;
		wf->chroma_offset[i][0] = 0;
		wf->chroma_offset[i][1] = 0;
		return br->err ? -EINVAL : 0;
	}

	if (!avd_h264_br_read_bit(br)) {
		wf->chroma_weight[i][0] = 1 << chroma_denom;
		wf->chroma_weight[i][1] = 1 << chroma_denom;
		wf->chroma_offset[i][0] = 0;
		wf->chroma_offset[i][1] = 0;
		return br->err ? -EINVAL : 0;
	}

	for (int j = 0; j < 2; j++) {
		s32 w = avd_h264_br_read_se(br);
		s32 o = avd_h264_br_read_se(br);

		if (br->err || w < -128 || w > 127 || o < -128 || o > 127)
			return -EINVAL;
		wf->chroma_weight[i][j] = w;
		wf->chroma_offset[i][j] = o;
	}

	return br->err ? -EINVAL : 0;
}

/* H.264 7.3.3.2 prediction weight table. */
static int
avd_h264_parse_pred_weight_table(struct avd_h264_br *br,
				 const struct v4l2_ctrl_h264_sps *sps,
				 const struct v4l2_ctrl_h264_slice_params *sl,
				 struct v4l2_ctrl_h264_pred_weights *weights)
{
	bool chroma = sps->chroma_format_idc != 0 &&
		      !(sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE);
	u32 luma_denom, chroma_denom = 0;
	int nlists = sl->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 : 1;

	luma_denom = avd_h264_br_read_ue(br);
	if (br->err || luma_denom > 7)
		return -EINVAL;
	weights->luma_log2_weight_denom = luma_denom;

	if (chroma) {
		chroma_denom = avd_h264_br_read_ue(br);
		if (br->err || chroma_denom > 7)
			return -EINVAL;
		weights->chroma_log2_weight_denom = chroma_denom;
	}

	for (int l = 0; l < nlists; l++) {
		struct v4l2_h264_weight_factors *wf = &weights->weight_factors[l];
		u32 count = (l == 0 ? sl->num_ref_idx_l0_active_minus1 :
				      sl->num_ref_idx_l1_active_minus1) + 1;

		for (u32 i = 0; i < count; i++) {
			int ret = avd_h264_parse_weight_entry(br, wf, i,
							      luma_denom,
							      chroma_denom,
							      chroma);
			if (ret)
				return ret;
		}
	}

	return br->err ? -EINVAL : 0;
}

/* H.264 7.3.3.3 decoded reference picture marking. */
static int avd_h264_parse_dec_ref_pic_marking(struct avd_h264_br *br,
					      u8 nal_unit_type)
{
	if (nal_unit_type == AVD_H264_NAL_IDR_SLICE) {
		avd_h264_br_read_bits(br, 2);
		return br->err ? -EINVAL : 0;
	}

	if (!avd_h264_br_read_bit(br))
		return br->err ? -EINVAL : 0;

	for (unsigned int ops = 0; ops < AVD_H264_MAX_MMCO_OPS; ops++) {
		u32 op = avd_h264_br_read_ue(br);

		if (br->err)
			return -EINVAL;
		if (op == 0)
			return 0;
		if (op > 6)
			return -EINVAL;

		if (op == 1 || op == 3)
			avd_h264_br_read_ue(br);
		if (op == 2)
			avd_h264_br_read_ue(br);
		if (op == 3 || op == 6)
			avd_h264_br_read_ue(br);
		if (op == 4)
			avd_h264_br_read_ue(br);

		if (br->err)
			return -EINVAL;
	}

	return -EINVAL;
}

int avd_h264_parse_slice_header(const u8 *nal, u32 nal_len,
				u8 nal_ref_idc, u8 nal_unit_type,
				const struct v4l2_ctrl_h264_sps *sps,
				const struct v4l2_ctrl_h264_pps *pps,
				const struct v4l2_ctrl_h264_decode_params *decode,
				u8 num_init_refs,
				const struct v4l2_h264_reference *init_p,
				const struct v4l2_h264_reference *init_b0,
				const struct v4l2_h264_reference *init_b1,
				struct v4l2_ctrl_h264_slice_params *sl,
				struct v4l2_ctrl_h264_pred_weights *weights,
				u32 *slice_data_off, u8 *slice_bit_off)
{
	struct avd_h264_br br;
	u32 total_mbs, slice_type, pps_id, frame_num;
	s32 qp_delta;
	u8 n_init;
	int ret;

	memset(sl, 0, sizeof(*sl));
	memset(weights, 0, sizeof(*weights));
	for (unsigned int i = 0; i < V4L2_H264_REF_LIST_LEN; i++) {
		sl->ref_pic_list0[i].index = 0xff;
		sl->ref_pic_list1[i].index = 0xff;
	}

	if (!nal || nal_len < 2 || !slice_data_off || !slice_bit_off)
		return -EINVAL;
	if (nal_unit_type != AVD_H264_NAL_SLICE &&
	    nal_unit_type != AVD_H264_NAL_IDR_SLICE)
		return -EINVAL;
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		return -EINVAL;
	if (sps->log2_max_frame_num_minus4 > 12 || sps->pic_order_cnt_type > 2)
		return -EINVAL;
	if (sps->pic_order_cnt_type == 0 &&
	    sps->log2_max_pic_order_cnt_lsb_minus4 > 12)
		return -EINVAL;
	if (pps->num_slice_groups_minus1 != 0)
		return -EINVAL;

	avd_h264_br_init(&br, nal, nal_len, 1);

	sl->first_mb_in_slice = avd_h264_br_read_ue(&br);
	total_mbs = (u32)(sps->pic_width_in_mbs_minus1 + 1) *
		    (u32)(sps->pic_height_in_map_units_minus1 + 1);
	if (br.err || sl->first_mb_in_slice >= total_mbs)
		return -EINVAL;

	slice_type = avd_h264_br_read_ue(&br);
	if (br.err || slice_type > 9)
		return -EINVAL;
	slice_type %= 5;
	if (slice_type != V4L2_H264_SLICE_TYPE_P &&
	    slice_type != V4L2_H264_SLICE_TYPE_B &&
	    slice_type != V4L2_H264_SLICE_TYPE_I)
		return -EINVAL;
	if (nal_unit_type == AVD_H264_NAL_IDR_SLICE &&
	    slice_type != V4L2_H264_SLICE_TYPE_I)
		return -EINVAL;
	sl->slice_type = slice_type;

	pps_id = avd_h264_br_read_ue(&br);
	if (br.err || pps_id > 255)
		return -EINVAL;

	if (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		sl->colour_plane_id = avd_h264_br_read_bits(&br, 2);

	frame_num = avd_h264_br_read_bits(&br, sps->log2_max_frame_num_minus4 + 4);

	if (nal_unit_type == AVD_H264_NAL_IDR_SLICE) {
		u32 idr_pic_id = avd_h264_br_read_ue(&br);

		if (br.err || idr_pic_id > 65535)
			return -EINVAL;
	}

	if (sps->pic_order_cnt_type == 0) {
		avd_h264_br_read_bits(&br,
				      sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->flags &
		    V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			avd_h264_br_read_se(&br);
	} else if (sps->pic_order_cnt_type == 1 &&
		   !(sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
		avd_h264_br_read_se(&br);
		if (pps->flags &
		    V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
			avd_h264_br_read_se(&br);
	}

	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT) {
		u32 redundant_pic_cnt = avd_h264_br_read_ue(&br);

		if (br.err || redundant_pic_cnt > 0)
			return -EINVAL;
		sl->redundant_pic_cnt = redundant_pic_cnt;
	}

	if (slice_type == V4L2_H264_SLICE_TYPE_B) {
		if (avd_h264_br_read_bit(&br))
			sl->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
	}

	if (slice_type == V4L2_H264_SLICE_TYPE_P ||
	    slice_type == V4L2_H264_SLICE_TYPE_B) {
		u32 num_l0 = pps->num_ref_idx_l0_default_active_minus1;
		u32 num_l1 = pps->num_ref_idx_l1_default_active_minus1;

		if (avd_h264_br_read_bit(&br)) {
			num_l0 = avd_h264_br_read_ue(&br);
			if (slice_type == V4L2_H264_SLICE_TYPE_B)
				num_l1 = avd_h264_br_read_ue(&br);
		}
		if (br.err || num_l0 > 15)
			return -EINVAL;
		sl->num_ref_idx_l0_active_minus1 = num_l0;

		if (slice_type == V4L2_H264_SLICE_TYPE_B) {
			if (num_l1 > 15)
				return -EINVAL;
			sl->num_ref_idx_l1_active_minus1 = num_l1;
		}
	}

	if (br.err)
		return -EINVAL;

	n_init = min_t(u8, num_init_refs, V4L2_H264_REF_LIST_LEN);
	if (slice_type == V4L2_H264_SLICE_TYPE_P) {
		if (init_p && n_init)
			memcpy(sl->ref_pic_list0, init_p,
			       n_init * sizeof(sl->ref_pic_list0[0]));
		ret = avd_h264_parse_ref_list_mod(&br, sps, decode, frame_num,
						  sl->ref_pic_list0,
						  sl->num_ref_idx_l0_active_minus1);
		if (ret)
			return ret;
	} else if (slice_type == V4L2_H264_SLICE_TYPE_B) {
		if (init_b0 && n_init)
			memcpy(sl->ref_pic_list0, init_b0,
			       n_init * sizeof(sl->ref_pic_list0[0]));
		if (init_b1 && n_init)
			memcpy(sl->ref_pic_list1, init_b1,
			       n_init * sizeof(sl->ref_pic_list1[0]));
		ret = avd_h264_parse_ref_list_mod(&br, sps, decode, frame_num,
						  sl->ref_pic_list0,
						  sl->num_ref_idx_l0_active_minus1);
		if (ret)
			return ret;
		ret = avd_h264_parse_ref_list_mod(&br, sps, decode, frame_num,
						  sl->ref_pic_list1,
						  sl->num_ref_idx_l1_active_minus1);
		if (ret)
			return ret;
	}

	if (V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, sl)) {
		ret = avd_h264_parse_pred_weight_table(&br, sps, sl, weights);
		if (ret)
			return ret;
	}

	if (nal_ref_idc != 0) {
		ret = avd_h264_parse_dec_ref_pic_marking(&br, nal_unit_type);
		if (ret)
			return ret;
	}

	if ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) &&
	    slice_type != V4L2_H264_SLICE_TYPE_I) {
		u32 cabac_init_idc = avd_h264_br_read_ue(&br);

		if (br.err || cabac_init_idc > 2)
			return -EINVAL;
		sl->cabac_init_idc = cabac_init_idc;
	}

	qp_delta = avd_h264_br_read_se(&br);
	if (br.err || qp_delta < S8_MIN || qp_delta > S8_MAX)
		return -EINVAL;
	sl->slice_qp_delta = qp_delta;

	if (pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		u32 idc = avd_h264_br_read_ue(&br);

		if (br.err || idc > 2)
			return -EINVAL;
		sl->disable_deblocking_filter_idc = idc;
		if (idc != 1) {
			s32 alpha = avd_h264_br_read_se(&br);
			s32 beta = avd_h264_br_read_se(&br);

			if (br.err || alpha < -6 || alpha > 6 ||
			    beta < -6 || beta > 6)
				return -EINVAL;
			sl->slice_alpha_c0_offset_div2 = alpha;
			sl->slice_beta_offset_div2 = beta;
		}
	}

	if (br.err)
		return -EINVAL;

	sl->header_bit_size = 8 + br.rbsp_bits;

	if (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) {
		if (br.bit) {
			br.pos++;
			br.bit = 0;
		}
	}

	avd_h264_br_skip_epb(&br);
	if (br.pos >= nal_len)
		return -EINVAL;

	*slice_data_off = br.pos;
	*slice_bit_off = br.bit;
	return 0;
}
