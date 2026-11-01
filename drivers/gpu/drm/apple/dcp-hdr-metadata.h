/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * HDR10 static metadata for the DCPAV service of desktop 26.x DCPs.
 *
 * The display's Dynamic Range and Mastering InfoFrame is link state the
 * firmware builds itself: the host sends DCPAV selector 0x15
 * (setHDRStaticMetadata) to the dcpav-service-epic service, an AFK standard
 * service command carrying a serialized two-key dictionary,
 * { "Type": "CEA-861:Type1", "Data": <24 bytes> }.  The 24 bytes are the
 * CTA-861.3 Type 1 body in the order of struct hdr_metadata_infoframe from
 * display_primaries to max_fall, little-endian.  The EOTF is not in it: the
 * firmware takes it from the colour mode the link runs.
 *
 * Ending HDR on a live link, the host first clears that InfoFrame with
 * selector 0x14 (stopInfoFrame), then sets the SDR colour mode; it never
 * sends an empty metadata block.  The stop message has a fixed 0x80 bytes:
 * the usual header, a 32-byte InfoFrame at 0x40 whose type byte selects
 * the slot to clear (0x87, Dynamic Range and Mastering), the link source
 * at 0x60 (0) and the DCP status at 0x70.
 */
#ifndef __APPLE_DCP_HDR_METADATA_H__
#define __APPLE_DCP_HDR_METADATA_H__

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <drm/drm_mode.h>

#define DCP_HDR_SELECTOR_STOP_INFOFRAME	0x14
#define DCP_HDR_SELECTOR_SET_STATIC	0x15
#define DCP_HDR_IPC_MAGIC		0x69706378	/* "xcpi" */
#define DCP_HDR_DATA_SIZE		24
#define DCP_HDR_TYPE			"CEA-861:Type1"

/*
 * The dictionary in the compact object format (header 0xd3, then 32-bit
 * tags: bit 31 marks the last object of a collection, bits 24-30 the type,
 * the low 24 bits the size; data is padded to 32 bits).  The keys are
 * symbols (type 8) whose size counts the terminating NUL, the type a string
 * (type 9) without one, the data a blob (type 10).
 */
#define DCP_HDR_OBJ_DICT		0x81000002	/* last, dictionary, 2 pairs */
#define DCP_HDR_OBJ_KEY			0x08000005	/* symbol, "Type\0" / "Data\0" */
#define DCP_HDR_OBJ_TYPE		(0x09000000 | (sizeof(DCP_HDR_TYPE) - 1))
#define DCP_HDR_OBJ_DATA		(0x8a000000 | DCP_HDR_DATA_SIZE)

#define DCP_HDR_OFF_TYPE_KEY		8
#define DCP_HDR_OFF_TYPE_VALUE		20
#define DCP_HDR_OFF_DATA_KEY		40
#define DCP_HDR_OFF_DATA_VALUE		52
#define DCP_HDR_OFF_DATA		(DCP_HDR_OFF_DATA_VALUE + 4)
#define DCP_HDR_OBJECT_SIZE		(DCP_HDR_OFF_DATA + DCP_HDR_DATA_SIZE)

/* The DCPAV IPC message: a 0x60-byte header, then the serialized object. */
#define DCP_HDR_MSG_HEADER		0x60
#define DCP_HDR_MSG_STATUS		0x40
#define DCP_HDR_MSG_SIZE		(DCP_HDR_MSG_HEADER + DCP_HDR_OBJECT_SIZE)

#define DCP_HDR_STOP_SIZE		0x80
#define DCP_HDR_STOP_INFOFRAME		0x40
#define DCP_HDR_STOP_STATUS		0x70
#define DCP_HDR_INFOFRAME_DRM		0x87

/* The 24-byte Type 1 body of @hdr; -EINVAL unless it is a Type 1 block. */
static inline int dcp_hdr_data(u8 data[DCP_HDR_DATA_SIZE],
			       const struct hdr_output_metadata *hdr)
{
	const struct hdr_metadata_infoframe *t1 = &hdr->hdmi_metadata_type1;
	unsigned int i;

	if (hdr->metadata_type || t1->metadata_type)
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		put_unaligned_le16(t1->display_primaries[i].x, data + i * 4);
		put_unaligned_le16(t1->display_primaries[i].y, data + i * 4 + 2);
	}
	put_unaligned_le16(t1->white_point.x, data + 12);
	put_unaligned_le16(t1->white_point.y, data + 14);
	put_unaligned_le16(t1->max_display_mastering_luminance, data + 16);
	put_unaligned_le16(t1->min_display_mastering_luminance, data + 18);
	put_unaligned_le16(t1->max_cll, data + 20);
	put_unaligned_le16(t1->max_fall, data + 22);
	return 0;
}

/* The selector 0x15 message carrying @data. */
static inline void dcp_hdr_message(u8 msg[DCP_HDR_MSG_SIZE],
				   const u8 data[DCP_HDR_DATA_SIZE])
{
	u8 *obj = msg + DCP_HDR_MSG_HEADER;

	memset(msg, 0, DCP_HDR_MSG_SIZE);
	put_unaligned_le16(1, msg + 0x02);
	put_unaligned_le32(DCP_HDR_SELECTOR_SET_STATIC, msg + 0x04);
	put_unaligned_le32(DCP_HDR_OBJECT_SIZE + 0x20, msg + 0x08);
	put_unaligned_le32(DCP_HDR_IPC_MAGIC, msg + 0x0c);
	put_unaligned_le32(DCP_HDR_OBJECT_SIZE, msg + 0x50);

	put_unaligned_le32(0xd3, obj);
	put_unaligned_le32(DCP_HDR_OBJ_DICT, obj + 4);
	put_unaligned_le32(DCP_HDR_OBJ_KEY, obj + DCP_HDR_OFF_TYPE_KEY);
	memcpy(obj + DCP_HDR_OFF_TYPE_KEY + 4, "Type", 5);
	put_unaligned_le32(DCP_HDR_OBJ_TYPE, obj + DCP_HDR_OFF_TYPE_VALUE);
	memcpy(obj + DCP_HDR_OFF_TYPE_VALUE + 4, DCP_HDR_TYPE,
	       sizeof(DCP_HDR_TYPE) - 1);
	put_unaligned_le32(DCP_HDR_OBJ_KEY, obj + DCP_HDR_OFF_DATA_KEY);
	memcpy(obj + DCP_HDR_OFF_DATA_KEY + 4, "Data", 5);
	put_unaligned_le32(DCP_HDR_OBJ_DATA, obj + DCP_HDR_OFF_DATA_VALUE);
	memcpy(obj + DCP_HDR_OFF_DATA, data, DCP_HDR_DATA_SIZE);
}

/* The selector 0x14 message clearing the Dynamic Range and Mastering InfoFrame. */
static inline void dcp_hdr_stop_message(u8 msg[DCP_HDR_STOP_SIZE])
{
	memset(msg, 0, DCP_HDR_STOP_SIZE);
	put_unaligned_le32(DCP_HDR_SELECTOR_STOP_INFOFRAME, msg + 0x04);
	put_unaligned_le32(DCP_HDR_STOP_SIZE - 0x40, msg + 0x08);
	put_unaligned_le32(DCP_HDR_IPC_MAGIC, msg + 0x0c);
	msg[DCP_HDR_STOP_INFOFRAME] = DCP_HDR_INFOFRAME_DRM;
}

/* The result of a DCPAV command: transport, AFK, then DCP status at @status. */
static inline int dcp_hdr_result(int transport, u32 retcode, const u8 *reply,
				 size_t length, size_t status)
{
	if (transport)
		return transport;
	if (retcode)
		return -EIO;
	if (length < status + 4)
		return -EPROTO;
	return get_unaligned_le32(reply + status) ? -EIO : 0;
}

/* The result of a selector 0x15 command. */
static inline int dcp_hdr_reply(int transport, u32 retcode, const u8 *reply,
				size_t length)
{
	return dcp_hdr_result(transport, retcode, reply, length,
			      DCP_HDR_MSG_STATUS);
}

/* The result of a selector 0x14 command. */
static inline int dcp_hdr_stop_reply(int transport, u32 retcode,
				     const u8 *reply, size_t length)
{
	return dcp_hdr_result(transport, retcode, reply, length,
			      DCP_HDR_STOP_STATUS);
}

#endif /* __APPLE_DCP_HDR_METADATA_H__ */
