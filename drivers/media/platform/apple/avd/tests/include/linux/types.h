/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_HOST_TYPES_H_
#define AVD_HOST_TYPES_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Host-only UAPI types use the C library instead of kernel headers. */
typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;
typedef __u8 u8;
typedef __s8 s8;
typedef __u16 u16;
typedef __s16 s16;
typedef __u32 u32;
typedef __s32 s32;
typedef __u64 u64;
typedef __s64 s64;
#endif
