/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_FVREAD_BLOCK_H
#define APPLE_FVREAD_BLOCK_H

#include <linux/types.h>

struct apple_sep_fv_key;

int fvread_block_open(int fd, void **handle, u64 *size);
void fvread_block_close(void *handle);
ssize_t fvread_block_read_plain(void *handle, u64 offset, void *data,
				 size_t len);
ssize_t fvread_block_read_crypto(void *handle, u64 offset, u64 dun,
				  const struct apple_sep_fv_key *fvkey,
				  void *data, size_t len);

#endif /* APPLE_FVREAD_BLOCK_H */
