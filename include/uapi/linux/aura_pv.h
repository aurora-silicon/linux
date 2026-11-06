/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_AURA_PV_H
#define _UAPI_LINUX_AURA_PV_H

#include <linux/bits.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define AURA_PV_ABI_MAJOR 0
#define AURA_PV_ABI_MINOR 1
#define AURA_PV_FUNC_UID 0xa000
#define AURA_PV_FUNC_VERSION 0xa001
#define AURA_PV_FUNC_GET_CAPS 0xa002
#define AURA_PV_FUNC_RUN 0xa010

#define AURA_PV_STATUS_OK 0
#define AURA_PV_FEATURE_BOUNDED_INT BIT_ULL(0)
#define AURA_PV_FEATURE_HOST_STATE BIT_ULL(1)
#define AURA_PV_GUEST_MAGIC 0x4155524150475630ULL
#define AURA_PV_LAYOUT_CAPS 0x43504130U
#define AURA_PV_LAYOUT_RUN 0x52554e30U
#define AURA_PV_OUTPUT_RESULT BIT_ULL(0)
#define AURA_PV_OUTPUT_HOST BIT_ULL(1)
#define AURA_PV_OUTPUT_ERROR BIT_ULL(2)
#define AURA_PV_MAX_PROGRAM 8
#define AURA_PV_PAGE_BYTES 4096

struct aura_pv_caps {
	__u32 major;
	__u32 minor;
	__u32 bytes;
	__u32 layout_id;
	__u64 features;
	__u64 max_control_bytes;
	__u64 shared_alignment;
	__u64 vmid_bits;
	__u64 ipa_bits;
	__u64 s2_granule;
	__u64 host_el;
	__u64 reserved_base;
	__u64 reserved_end;
};

struct aura_pv_run {
	__u64 request_seq;
	__u64 initial;
	__u64 program[AURA_PV_MAX_PROGRAM];
	__u64 program_words;
	__u64 result;
	__u64 response_seq;
	__u64 host_sentinel_status;
	__s64 status;
};

#define AURA_PV_IOC_MAGIC 'P'
#define AURA_PV_IOC_DISCOVER _IOR(AURA_PV_IOC_MAGIC, 0, struct aura_pv_caps)
#define AURA_PV_IOC_RUN _IOWR(AURA_PV_IOC_MAGIC, 1, struct aura_pv_run)

#endif
