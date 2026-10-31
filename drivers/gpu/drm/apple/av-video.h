/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_AV_VIDEO_H_
#define _APPLE_AV_VIDEO_H_

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <drm/drm_mode.h>

/* One fixed dictionary: Type string and the 24-byte CTA Type 1 body. */
#define DCP_HDR_DATA_SIZE 24
#define DCP_HDR_OBJECT_SIZE 68
#define DCP_HDR_MESSAGE_SIZE (0x60 + DCP_HDR_OBJECT_SIZE)

static inline int dcp_hdr_encode(u8 object[DCP_HDR_OBJECT_SIZE],
				 const struct hdr_output_metadata *metadata)
{
	const struct hdr_metadata_infoframe *hdr = &metadata->hdmi_metadata_type1;
	u8 *data = object + 44;
	unsigned int i;

	if (metadata->metadata_type || hdr->metadata_type)
		return -EINVAL;

	memset(object, 0, DCP_HDR_OBJECT_SIZE);
	put_unaligned_le32(0xd3, object);
	/* A two-pair dictionary, last in the root collection. */
	put_unaligned_le32(0x81000002, object + 4);
	put_unaligned_le32(0x09000004, object + 8);
	memcpy(object + 12, "Type", 4);
	put_unaligned_le32(0x0900000c, object + 16);
	memcpy(object + 20, "CEA-861:Type1", 12);
	put_unaligned_le32(0x09000004, object + 32);
	memcpy(object + 36, "Data", 4);
	/* The blob is the dictionary's last value. */
	put_unaligned_le32(0x8a000018, object + 40);
	for (i = 0; i < 3; i++) {
		put_unaligned_le16(hdr->display_primaries[i].x, data + i * 4);
		put_unaligned_le16(hdr->display_primaries[i].y, data + i * 4 + 2);
	}
	put_unaligned_le16(hdr->white_point.x, data + 12);
	put_unaligned_le16(hdr->white_point.y, data + 14);
	put_unaligned_le16(hdr->max_display_mastering_luminance, data + 16);
	put_unaligned_le16(hdr->min_display_mastering_luminance, data + 18);
	put_unaligned_le16(hdr->max_cll, data + 20);
	put_unaligned_le16(hdr->max_fall, data + 22);
	return 0;
}

static inline int dcp_hdr_message(u8 message[DCP_HDR_MESSAGE_SIZE],
				  const struct hdr_output_metadata *metadata)
{
	memset(message, 0, DCP_HDR_MESSAGE_SIZE);
	put_unaligned_le16(1, message + 2);
	put_unaligned_le32(0x15, message + 4);
	put_unaligned_le32(DCP_HDR_OBJECT_SIZE + 0x20, message + 8);
	put_unaligned_le32(0x69706378, message + 12);
	put_unaligned_le32(DCP_HDR_OBJECT_SIZE, message + 0x50);
	return dcp_hdr_encode(message + 0x60, metadata);
}

static inline int dcp_hdr_reply(int transport, u32 retcode,
				const u8 *reply, size_t length)
{
	if (transport)
		return transport;
	if (retcode)
		return -EIO;
	if (length < 0x44)
		return -EPROTO;
	return get_unaligned_le32(reply + 0x40) ? -EIO : 0;
}

struct apple_dcp;
struct apple_connector;
struct apple_epic_service;
struct apple_epic_service_ops;
struct apple_dcp_afkep;
const struct apple_epic_service_ops *av_video_service_ops(void);

struct drm_atomic_commit;
struct drm_crtc;
int av_video_register_cleanup(struct apple_dcp_afkep *ep);
void av_video_init(struct apple_epic_service *service, const char *name,
		   const char *class, s64 unit);
void av_video_teardown(struct apple_epic_service *service);
void av_video_atomic_modeset(struct apple_dcp *dcp, struct apple_connector *connector,
			     u64 link_generation, struct drm_crtc *crtc,
			     struct drm_atomic_commit *commit);
#endif
