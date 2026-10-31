/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_HOST_TYPES_H_
#define AVD_HOST_TYPES_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
/* Host transport test runs on little-endian build hosts. */
typedef uint32_t __le32;
#endif
