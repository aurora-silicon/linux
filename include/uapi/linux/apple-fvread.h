/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_APPLE_FVREAD_H
#define _UAPI_LINUX_APPLE_FVREAD_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define APPLE_FVREAD_IOC_MAGIC		0xf7
#define APPLE_FVREAD_UNLOCK_FV_OFF	(1U << 0)

struct apple_fvread_unlock {
	/* An O_RDONLY fd for the whole APFS container block device. */
	__s32 device_fd;
	__u32 flags;
	__u8 volume_uuid[16];
};

struct apple_fvread_info {
	__u64 container_bytes;
	__u32 block_size;
	__u32 flags;
	__u8 volume_uuid[16];
};

/*
 * A read for a key-aware APFS consumer.  Both offsets are in 4096-byte data
 * units.  The caller, which parsed the APFS extent, supplies the DUN; FVREAD
 * never guesses a file crypto identifier.  At most 16 blocks may be read.
 */
struct apple_fvread_read {
	__u64 physical_block;
	__u64 dun;
	__u32 block_count;
	__u32 reserved;
	__u64 buffer;
};

#define APPLE_FVREAD_MAX_FILE_KEY_RECORD	168

/*
 * Read file data with the APFS crypto-state records selected by the reader.
 * The SEP unwraps the PFK and the opaque key goes directly to Apple NVMe.
 */
struct apple_fvread_read_file {
	struct apple_fvread_read io;
	__u32 protection_class;
	__u32 wrapped_ekwk_len;
	__u32 wrapped_ek_len;
	__u32 reserved;
	__u8 wrapped_ekwk[APPLE_FVREAD_MAX_FILE_KEY_RECORD];
	__u8 wrapped_ek[APPLE_FVREAD_MAX_FILE_KEY_RECORD];
};

#define APPLE_FVREAD_IOC_UNLOCK \
	_IOW(APPLE_FVREAD_IOC_MAGIC, 0x01, struct apple_fvread_unlock)
#define APPLE_FVREAD_IOC_READ \
	_IOW(APPLE_FVREAD_IOC_MAGIC, 0x02, struct apple_fvread_read)
#define APPLE_FVREAD_IOC_READ_FILE \
	_IOW(APPLE_FVREAD_IOC_MAGIC, 0x03, struct apple_fvread_read_file)
#define APPLE_FVREAD_IOC_GET_INFO \
	_IOR(APPLE_FVREAD_IOC_MAGIC, 0x04, struct apple_fvread_info)

#endif /* _UAPI_LINUX_APPLE_FVREAD_H */
