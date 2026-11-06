/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NVME_APPLE_PANIC_H
#define _NVME_APPLE_PANIC_H

#include <linux/types.h>

#define APPLE_PANIC_FUNK_MAGIC		0x46554e4b
#define APPLE_PANIC_FUNK_VERSION	6
#define APPLE_PANIC_FUNK_V6_HDR_SIZE	177

struct apple_panic_segment {
	const u8 *data;
	size_t size;
};

struct apple_panic_record {
	struct apple_panic_segment panic_text;
	struct apple_panic_segment stackshot;
	struct apple_panic_segment other_log;
	struct apple_panic_segment extensible_log;
	struct apple_panic_segment panic_initiator;
};

int apple_panic_parse_funk_v6(const void *data, size_t size,
			      struct apple_panic_record *record);

#endif /* _NVME_APPLE_PANIC_H */
