/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Capture RTKit syslog slots before acknowledgement allows firmware reuse.
 * Format/offsets: Asahi Linux Contributors, drivers/soc/apple/rtkit.c,
 * apple_rtkit_syslog_rx_init/log at kernel fba91e5d9577.
 * This helper only reads a caller-qualified host buffer. No address mapping,
 * firmware commands or string interpretation. Output records are private:
 * little-endian u64 mailbox message, u32 slot size, then the complete slot.
 */
#ifndef DCPEXT_SYSLOG_H
#define DCPEXT_SYSLOG_H
struct dcpext_syslog {
	u32 last_index, message_size, used;
	bool initialized;
	u8 capture[1048576];
};

static int dcpext_syslog_observe(struct dcpext_syslog *s, u64 message,
				 const u8 *buffer, u32 size)
{
	u32 type = (message >> 52) & 0xff;
	u32 index = message & 0xff, stride, offset;
	if (type == 8) {
		if (s->initialized)
			return -EPROTO;
		s->last_index = index; /* Native/Linux admission uses inclusive max. */
		s->message_size = (message >> 24) & 0xff;
		if (!s->message_size)
			return -EINVAL;
		s->initialized = true;
		return 0;
	}
	if (type != 5)
		return 0;
	if (!s->initialized || !buffer || index > s->last_index)
		return -EINVAL;
	stride = 0x20 + s->message_size;
	offset = index * stride;
	if (offset > size || stride > size - offset)
		return -ERANGE;
	if (s->used > sizeof(s->capture) ||
	    stride + 12 > sizeof(s->capture) - s->used)
		return -ENOSPC;
	put_unaligned_le64(message, s->capture + s->used);
	put_unaligned_le32(stride, s->capture + s->used + 8);
	memcpy(s->capture + s->used + 12, buffer + offset, stride);
	s->used += stride + 12;
	return 0;
}
#endif
