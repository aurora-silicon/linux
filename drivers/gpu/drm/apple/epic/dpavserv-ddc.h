/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * I2C transfers to an external display's DDC bus through the DCPAV service.
 *
 * The dcpav-service-epic service carries the sink's DDC: one standard
 * service command per I2C transfer, the same "xcpi" message the HDR10
 * metadata uses (dcp-hdr-metadata.h), selector 0x0a for a write and 0x09
 * for a read.  Every u32 argument sits in its own 16-byte slot after the
 * 0x40-byte header, then the data:
 *
 *   write: 0x40 status, 0x50 chip, 0x60 offset, 0x70 length, 0x80 data
 *   read:  0x40 chip, 0x50 offset, 0x60 length, 0x70 status, 0x80 data
 *
 * The offset is the first byte the host sends after the chip address: the
 * sub-address 0x51 of a DDC/CI message, or the EDID offset at 0x50.  The
 * reply echoes the command; a read's bytes come back at 0x80, and a
 * non-zero status is the firmware's IOReturn (0xe0114000 for a sink that
 * does not answer).  The firmware passes DDC/CI framing through unchanged.
 */
#ifndef __APPLE_DPAVSERV_DDC_H__
#define __APPLE_DPAVSERV_DDC_H__

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DCPAV_I2C_SELECTOR_READ		0x09
#define DCPAV_I2C_SELECTOR_WRITE	0x0a
#define DCPAV_I2C_IPC_MAGIC		0x69706378	/* "xcpi" */
#define DCPAV_I2C_HEADER		0x80
/* DDC/CI messages are at most 39 bytes; an EDID block is 128. */
#define DCPAV_I2C_MAX_DATA		128
#define DCPAV_I2C_MSG_MAX		(DCPAV_I2C_HEADER + DCPAV_I2C_MAX_DATA)

#define DCPAV_I2C_W_STATUS		0x40
#define DCPAV_I2C_W_CHIP		0x50
#define DCPAV_I2C_W_OFFSET		0x60
#define DCPAV_I2C_W_LENGTH		0x70
#define DCPAV_I2C_R_CHIP		0x40
#define DCPAV_I2C_R_OFFSET		0x50
#define DCPAV_I2C_R_LENGTH		0x60
#define DCPAV_I2C_R_STATUS		0x70
#define DCPAV_I2C_DATA			0x80

/*
 * Build the command for one transfer into @msg (DCPAV_I2C_MSG_MAX bytes);
 * returns its size or -EINVAL.  A write sends @len bytes of @data.
 */
static inline int dcpav_i2c_message(u8 *msg, bool read, u8 chip, u8 offset,
				    const u8 *data, size_t len)
{
	size_t size = DCPAV_I2C_HEADER + len;

	if (chip > 0x7f || len > DCPAV_I2C_MAX_DATA || (read && !len) ||
	    (!read && len && !data))
		return -EINVAL;
	memset(msg, 0, size);
	put_unaligned_le16(1, msg + 0x02);
	put_unaligned_le32(read ? DCPAV_I2C_SELECTOR_READ : DCPAV_I2C_SELECTOR_WRITE,
			   msg + 0x04);
	put_unaligned_le32(size - 0x40, msg + 0x08);
	put_unaligned_le32(DCPAV_I2C_IPC_MAGIC, msg + 0x0c);
	put_unaligned_le32(chip, msg + (read ? DCPAV_I2C_R_CHIP : DCPAV_I2C_W_CHIP));
	put_unaligned_le32(offset, msg + (read ? DCPAV_I2C_R_OFFSET : DCPAV_I2C_W_OFFSET));
	put_unaligned_le32(len, msg + (read ? DCPAV_I2C_R_LENGTH : DCPAV_I2C_W_LENGTH));
	if (!read && len)
		memcpy(msg + DCPAV_I2C_DATA, data, len);
	return size;
}

/*
 * The result of a transfer: the transport and AFK errors first, then the
 * echoed selector and the firmware status.  A read copies its @len bytes.
 */
static inline int dcpav_i2c_result(int transport, u32 retcode, const u8 *reply,
				   size_t received, bool read, u8 *data, size_t len)
{
	size_t need = DCPAV_I2C_HEADER + (read ? len : 0);
	u32 selector = read ? DCPAV_I2C_SELECTOR_READ : DCPAV_I2C_SELECTOR_WRITE;

	if (transport)
		return transport;
	if (received < need || get_unaligned_le32(reply + 0x04) != selector ||
	    get_unaligned_le32(reply + 0x0c) != DCPAV_I2C_IPC_MAGIC)
		return -EPROTO;
	/* The sink did not answer (the firmware's status), or the call failed. */
	if (get_unaligned_le32(reply + (read ? DCPAV_I2C_R_STATUS : DCPAV_I2C_W_STATUS)))
		return -EREMOTEIO;
	if (retcode)
		return -EIO;
	if (read)
		memcpy(data, reply + DCPAV_I2C_DATA, len);
	return 0;
}

#endif
