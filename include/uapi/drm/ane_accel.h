/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __DRM_UAPI_ANE_ACCEL_H__
#define __DRM_UAPI_ANE_ACCEL_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: ABI versions
 *
 * Two drivers implement this interface on an accel node, both with the DRM
 * driver name "ane":
 *
 * - ane (H13 family: T8103, T6000, T6001, T6002) implements ABI version
 *   %DRM_ANE_ABI_V1: GET_CAPS, BO_INIT, BO_FREE and SUBMIT.
 * - ane_t6021 (H14 family: T6020, T6021, T6022, T8112) implements ABI
 *   version %DRM_ANE_ABI_V2: GET_CAPS, BO_INIT, BO_FREE, PROG_LOAD,
 *   PROC_CREATE and EXEC.
 *
 * The DRM_IOCTL_VERSION major and &drm_ane_get_caps.abi_version both give
 * the ABI version. The two versions share the ioctl numbers and the
 * structures, and a number is never given to a second ioctl. A driver
 * rejects the ioctls of the other version. A structure grows only at its
 * end; &drm_ane_get_caps.size tells which fields the driver filled in.
 *
 * Every pad and reserved field, and every flags field with no flags
 * defined, must be zero. The drivers reject a nonzero value with -EINVAL.
 */
#define DRM_ANE_ABI_V1		1
#define DRM_ANE_ABI_V2		2

#define DRM_ANE_CHIP_H13	13
#define DRM_ANE_CHIP_H14	14

/* ABI version 1: entries of &drm_ane_submit.handles */
#define DRM_ANE_TILE_COUNT	0x20
/* ABI version 1: network id of the queue that SUBMIT uses */
#define DRM_ANE_FIFO_NID	0x40
/* ABI version 1: the weights start at round_up(tsk_size, DRM_ANE_CMD_GRAN) */
#define DRM_ANE_CMD_GRAN	0x10

/* ABI version 2: highest section id, and most sections per PROG_LOAD */
#define DRM_ANE_MAX_SECTIONS	9
/* ABI version 2: most generic binds per PROG_LOAD and buffers per EXEC */
#define DRM_ANE_MAX_BINDS	64

#define DRM_ANE_GET_CAPS	0x00
#define DRM_ANE_BO_INIT		0x01
#define DRM_ANE_BO_FREE		0x02
#define DRM_ANE_SUBMIT		0x03
#define DRM_ANE_PROG_LOAD	0x04
#define DRM_ANE_PROC_CREATE	0x05
#define DRM_ANE_EXEC		0x06

/**
 * struct drm_ane_get_caps - DRM_IOCTL_ANE_GET_CAPS argument
 * @flags: In. No flags are defined: must be zero.
 * @size: Out. Bytes of this structure that the driver filled in.
 * @abi_version: Out. %DRM_ANE_ABI_V1 or %DRM_ANE_ABI_V2.
 * @chip_family: Out. %DRM_ANE_CHIP_H13 or %DRM_ANE_CHIP_H14.
 * @section_size: Out. Size of the &drm_ane_section entries that PROG_LOAD
 *	reads, or 0 if the driver has no PROG_LOAD.
 * @bind_size: Out. Size of the &drm_ane_generic_bind entries that PROG_LOAD
 *	reads, or 0 if the driver has no PROG_LOAD.
 * @exec_io_size: Out. Size of the &drm_ane_exec_io entries that EXEC reads,
 *	or 0 if the driver has no EXEC.
 * @pad: Must be zero.
 */
struct drm_ane_get_caps {
	__u32 flags;
	__u32 size;
	__u32 abi_version;
	__u32 chip_family;
	__u32 section_size;
	__u32 bind_size;
	__u32 exec_io_size;
	__u32 pad;
};

/**
 * struct drm_ane_bo_init - DRM_IOCTL_ANE_BO_INIT argument
 * @handle: Out. Handle of the new buffer.
 * @pad: Must be zero.
 * @size: In. Size of the buffer in bytes, not zero.
 * @offset: Out. Offset to give to mmap() on the accel node.
 */
struct drm_ane_bo_init {
	__u32 handle;
	__u32 pad;
	__u64 size;
	__u64 offset;
};

/**
 * struct drm_ane_bo_free - DRM_IOCTL_ANE_BO_FREE argument
 * @handle: In. Handle from BO_INIT.
 * @pad: Must be zero.
 */
struct drm_ane_bo_free {
	__u32 handle;
	__u32 pad;
};

/**
 * struct drm_ane_submit - DRM_IOCTL_ANE_SUBMIT argument (ABI version 1)
 * @tsk_size: In. Size of the task microcode at the start of handles[0], not
 *	zero.
 * @td_count: In. Number of task descriptors, 1 to 0xffff.
 * @td_size: In. Size of a task descriptor: a multiple of 4, 4 to 0x40000.
 * @handles: In. Buffer per tile: handles[0] holds the microcode and the
 *	weights, handles[1] must be 0 (the driver sets it from handles[0]),
 *	and 0 marks an unused tile.
 * @btsp_handle: In. Buffer with the bootstrap task descriptor.
 * @pad: Must be zero.
 *
 * A successful SUBMIT returns after the engine completed the work, with the
 * output buffers visible to the CPU.
 */
struct drm_ane_submit {
	__u64 tsk_size;
	__u32 td_count;
	__u32 td_size;
	__u32 handles[DRM_ANE_TILE_COUNT];
	__u32 btsp_handle;
	__u32 pad;
};

/**
 * struct drm_ane_section - one firmware program section (ABI version 2)
 * @id: Section id, 1 to %DRM_ANE_MAX_SECTIONS, unique in one PROG_LOAD.
 * @bo_handle: Buffer that holds the section bytes.
 * @size: Size of the section in bytes.
 * @offset: Offset of the section in the buffer.
 */
struct drm_ane_section {
	__u32 id;
	__u32 bo_handle;
	__u64 size;
	__u64 offset;
};

/**
 * struct drm_ane_generic_bind - buffer of a program (ABI version 2)
 * @buffer_id: Firmware buffer id.
 * @bo_handle: Buffer handle.
 * @type: Buffer type.
 * @pad: Must be zero.
 * @size: Size of the buffer in bytes.
 *
 * The driver checks @pad and ignores the other fields: the sections that
 * userspace builds already hold this information.
 */
struct drm_ane_generic_bind {
	__u32 buffer_id;
	__u32 bo_handle;
	__u32 type;
	__u32 pad;
	__u64 size;
};

/**
 * struct drm_ane_prog_load - DRM_IOCTL_ANE_PROG_LOAD argument (ABI version 2)
 * @sections_ptr: In. Pointer to struct drm_ane_section[@section_count].
 * @generic_ptr: In. Pointer to struct drm_ane_generic_bind[@generic_count].
 * @section_count: In. 1 to %DRM_ANE_MAX_SECTIONS.
 * @generic_count: In. 0 to %DRM_ANE_MAX_BINDS.
 * @prog_id_out: Out. Firmware program id. Identical sections give the same
 *	program id.
 * @pad: Must be zero.
 */
struct drm_ane_prog_load {
	__u64 sections_ptr;
	__u64 generic_ptr;
	__u32 section_count;
	__u32 generic_count;
	__u32 prog_id_out;
	__u32 pad;
};

/**
 * struct drm_ane_proc_create - DRM_IOCTL_ANE_PROC_CREATE argument
 *	(ABI version 2)
 * @prog_id: In. Program id from PROG_LOAD.
 * @proc_id_out: Out. Firmware process id.
 */
struct drm_ane_proc_create {
	__u32 prog_id;
	__u32 proc_id_out;
};

/**
 * struct drm_ane_exec_io - input or output buffer of one EXEC
 *	(ABI version 2)
 * @buffer_id: Firmware buffer id.
 * @bo_handle: Buffer handle. The driver gives its address to the firmware.
 * @type: Buffer type.
 * @flags: No flags are defined: must be zero.
 * @reserved: Must be zero.
 * @size: Size of the buffer in bytes.
 */
struct drm_ane_exec_io {
	__u32 buffer_id;
	__u32 bo_handle;
	__u32 type;
	__u32 flags;
	__u64 reserved;
	__u64 size;
};

/**
 * struct drm_ane_exec - DRM_IOCTL_ANE_EXEC argument (ABI version 2)
 * @prog_id: In. Program id from PROG_LOAD.
 * @proc_id: In. Process id from PROC_CREATE.
 * @priority: In. 2 to 7.
 * @timeout_ms: In. Firmware completion timeout, 0 for 5000 ms.
 * @count: In. 1 to %DRM_ANE_MAX_BINDS.
 * @pad: Must be zero.
 * @io_ptr: In. Pointer to struct drm_ane_exec_io[@count].
 *
 * A successful EXEC returns after the firmware acknowledged completion and
 * the task queue is idle, with the output buffers visible to the CPU.
 */
struct drm_ane_exec {
	__u32 prog_id;
	__u32 proc_id;
	__u32 priority;
	__u32 timeout_ms;
	__u32 count;
	__u32 pad;
	__u64 io_ptr;
};

#define DRM_IOCTL_ANE_GET_CAPS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_GET_CAPS, struct drm_ane_get_caps)
#define DRM_IOCTL_ANE_BO_INIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_BO_INIT, struct drm_ane_bo_init)
#define DRM_IOCTL_ANE_BO_FREE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_BO_FREE, struct drm_ane_bo_free)
#define DRM_IOCTL_ANE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_SUBMIT, struct drm_ane_submit)
#define DRM_IOCTL_ANE_PROG_LOAD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_PROG_LOAD, struct drm_ane_prog_load)
#define DRM_IOCTL_ANE_PROC_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_PROC_CREATE, struct drm_ane_proc_create)
#define DRM_IOCTL_ANE_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_EXEC, struct drm_ane_exec)

#if defined(__cplusplus)
}
#endif

#endif /* __DRM_UAPI_ANE_ACCEL_H__ */
