/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_PROGRAM_H__
#define __ANE_H16_PROGRAM_H__

#include <linux/wait.h>
#include <linux/spinlock.h>

#include "ane_fw_transport.h"
#include "ane_h16_load.h"
#include "ane_h16_pending.h"

/*
 * Program lifecycle for one H16 firmware session: LOAD_PROGRAM, CREATE_PROCESS,
 * the 0x002d/EXECUTE/0x002d sequence and TERMINATE/UNLOAD. Wire facts are from
 * the H16 cold-boot facts (rounds 1 and 5, program load and lifecycle) and the
 * SPEC-r4 EXECUTE layout. Nothing here maps memory or touches hardware; the
 * backend supplies private mappings through ane_h16_session_ops.
 */

#define ANE_H16_LOAD_SIZE 0x1c0
#define ANE_H16_LOAD_SLOTS 9
#define ANE_H16_LOAD_SLOT_OFFSET 0x08
#define ANE_H16_LOAD_SLOT_SIZE 0x30
#define ANE_H16_LOAD_ID_OFFSET 0x1b8
#define ANE_H16_LOAD_ID_REQUEST 0xffffffffU
#define ANE_H16_CREATE_SIZE 0x258
#define ANE_H16_TERMINATE_SIZE 0x10
#define ANE_H16_UNLOAD_SIZE 0xc
#define ANE_H16_GATE_SIZE 0xc
#define ANE_H16_REPORT_MAX 0x100
/* First value of the firmware mcache DSID pool (custom-BAR facts, 4). */
#define ANE_H16_MCACHE_DSID 36

/*
 * Host-built generic section: header, then one entry per surface in HWX
 * segment order, then one entry for the __DATA scratch when the program has
 * one.
 */
#define ANE_H16_GENERIC_MAGIC		1
#define ANE_H16_GENERIC_VERSION		0x10
#define ANE_H16_GENERIC_COUNT		0x204
#define ANE_H16_GENERIC_HEADER		0x208
#define ANE_H16_GENERIC_ENTRY		0x30
#define ANE_H16_GENERIC_ID_MAX		0x3c

/* Host-built operation section: a count, then fixed-size BAR records. */
#define ANE_H16_OPERATION_RECORD	0x40c
#define ANE_H16_OPERATION_RECORDS_MAX	64
#define ANE_H16_OPERATION_REFS_MAX	64

/* Host-built sections. Userspace packs their bytes; the kernel places them. */
enum ane_h16_aux {
	ANE_H16_AUX_GENERIC,
	ANE_H16_AUX_OPERATION,
	ANE_H16_AUX_PROCEDURE,
	ANE_H16_AUX_TEXT_PROPERTIES,
	ANE_H16_AUX_COUNT,
};

#define ANE_H16_AUX_MIN 0x28
#define ANE_H16_AUX_MAX SZ_64K

/* Source of one LOAD slot. */
enum ane_h16_slot_source {
	ANE_H16_SLOT_EMPTY,
	ANE_H16_SLOT_AUX_GENERIC,
	ANE_H16_SLOT_KERNEL,
	ANE_H16_SLOT_TEXT,
	ANE_H16_SLOT_AUX_OPERATION,
	ANE_H16_SLOT_AUX_PROCEDURE,
	ANE_H16_SLOT_AUX_TEXT_PROPERTIES,
};

struct ane_h16_slot_rule {
	enum ane_h16_slot_source source;
	u32 resource;
	u32 class;
};

/* Resource indices of the six populated slots (facts 9.2, r5 7.1). The
 * kernel slot index is also the operation tag of a kernel-table program.
 */
#define ANE_H16_RESOURCE_GENERIC 1002
#define ANE_H16_RESOURCE_OPERATION 1003
#define ANE_H16_RESOURCE_PROCEDURE 1004
#define ANE_H16_RESOURCE_TEXT_PROPERTIES 1005
#define ANE_H16_RESOURCE_TEXT 1006
#define ANE_H16_RESOURCE_KERNEL 1007

const struct ane_h16_slot_rule *ane_h16_slot_rule(unsigned int slot);

struct ane_h16_load_slot {
	u32 resource;
	u32 class;
	u64 dva;
	u64 size;
	bool present;
};

/* Firmware-visible placement of every LOAD input, all kernel-owned. */
struct ane_h16_load_layout {
	struct ane_h16_load_mapping regions[ANE_H16_REGIONS];
	u32 region_count;
	u64 aux_dva[ANE_H16_AUX_COUNT];
	u64 aux_size[ANE_H16_AUX_COUNT];
};

/* Round 5, section 7: resource usage type to EXECUTE descriptor class. */
static inline u32 ane_h16_usage_class(u32 usage)
{
	static const u8 classes[] = {
		[1] = 0, [2] = 1, [3] = 0, [4] = 1, [5] = 6, [6] = 8, [7] = 2,
		[8] = 8, [9] = 8, [10] = 4, [11] = 8, [12] = 0, [13] = 0,
	};

	if (usage < 1 || usage >= ARRAY_SIZE(classes))
		return 8;
	return classes[usage];
}

int ane_h16_load_slots(const struct ane_h16_hwx *hwx,
		       const struct ane_h16_load_plan *plan,
		       const struct ane_h16_load_layout *layout,
		       struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS]);
int ane_h16_load_encode(void *command, size_t length,
			const struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS]);
int ane_h16_load_reply(const void *reply, size_t length, const void *request,
		       u32 *program);
int ane_h16_create_encode(void *command, size_t length, u32 program);
int ane_h16_create_reply(const void *reply, size_t length, u32 program, u32 *process);
int ane_h16_terminate_encode(void *command, size_t length, u32 program, u32 process);
int ane_h16_unload_encode(void *command, size_t length, u32 program);
int ane_h16_id_reply(const void *reply, size_t length, const void *request);
int ane_h16_gate_encode(void *command, size_t length, u32 value);

int ane_h16_process_bindings(const struct ane_h16_hwx *hwx, const void *generic,
			     size_t generic_size,
			     struct ane_h16_process_binding *bindings,
			     unsigned int *count,
			     struct ane_h16_process_binding *scratch);
int ane_h16_operation_validate(const struct ane_h16_hwx_snapshot *snapshot,
			       const struct ane_h16_hwx *hwx, const void *operation,
			       size_t operation_size,
			       const struct ane_h16_process_binding *bindings,
			       unsigned int count,
			       const struct ane_h16_process_binding *scratch);
int ane_h16_custom_bars(const struct ane_h16_hwx_snapshot *snapshot,
			const struct ane_h16_hwx *hwx, u32 dsid,
			struct ane_h16_custom_bar *bars, unsigned int *count);

/* Private, pinned mappings in the firmware domain. Never released before reset. */
struct ane_h16_session_ops {
	int (*map)(void *ctx, size_t size, bool device_write,
		   struct ane_fw_surface *surface);
	void (*log)(void *ctx, const char *item, u64 a, u64 b);
	/*
	 * Optional. For a surface with a handle: before the engine uses it
	 * (for_device) write back CPU writes; after the engine wrote it
	 * (!for_device) drop stale lines before the CPU reads. Without it,
	 * or for a surface without a handle, memory is coherent.
	 */
	void (*sync)(void *ctx, const struct ane_fw_surface *surface, bool for_device);
	/*
	 * Optional. Called with true before an EXECUTE touches the firmware
	 * and with false after it ended, successful or not, so a backend can
	 * hold a performance vote only while work is in flight.
	 */
	void (*busy)(void *ctx, bool busy);
};

struct ane_h16_program {
	struct ane_h16_hwx_snapshot snapshot;
	/* Parsed again from the authenticated bytes, never the cached info. */
	struct ane_h16_hwx hwx;
	struct ane_h16_load_plan plan;
	struct ane_fw_surface regions[ANE_H16_REGIONS];
	struct ane_fw_surface aux[ANE_H16_AUX_COUNT];
	struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS];
	struct ane_h16_resource resources[ANE_H16_SURFACES];
	struct ane_fw_surface *surfaces[ANE_H16_SURFACES];
	struct ane_h16_custom_bar bars[ANE_H16_MAX_CUSTOM_BARS];
	unsigned int bar_count;
	unsigned int resource_count;
	/* Per-surface buffer size from the generic section, sent in EXECUTE. */
	u64 declared[ANE_H16_SURFACES];
	/*
	 * The __DATA scratch: a kernel-owned EXECUTE input that carries the
	 * DRAM edges between tasks. Userspace never sees or fills it.
	 */
	struct ane_h16_execute_buffer scratch;
	bool has_scratch;
	struct ane_h16_session *session;
	/* Session-local identity; firmware program IDs are reused after UNLOAD. */
	u64 generation;
	struct ane_h16_request_key key;
	bool created;
	bool closed;
	/* Torn down cleanly: its memory went back to the session's spares. */
	bool released;
	/* Firmware boot the program was loaded in; an older one means lost. */
	u64 epoch;
};

/* The quiet time that ends serving the SHAREDMALLOC ring after UNLOAD. */
#define ANE_H16_SETTLE_QUIET_NS (10ULL * 1000 * 1000)
#define ANE_H16_SETTLE_LIMIT_NS (100ULL * 1000 * 1000)

/*
 * Where the time of one EXECUTE goes, in nanoseconds. The firmware phases
 * are measured by ane_h16_session_execute(); a reply or report is "seen"
 * when a poll finds it, so they include the poll latency. The copy and
 * ioctl phases come from the caller through ane_h16_session_account().
 */
enum ane_h16_exec_phase {
	ANE_H16_T_LOCK,		/* waiting for the session lock */
	ANE_H16_T_COPY_IN,	/* inputs copied in, outputs poisoned */
	ANE_H16_T_PREPARE,	/* checks, command buffer, until the opening gate */
	ANE_H16_T_GATE_OPEN,	/* 0x002d 0 published until its reply */
	ANE_H16_T_SUBMIT_ACK,	/* EXECUTE published until its channel-7 ack */
	ANE_H16_T_ACK_REPORT,	/* the ack until the first report of the UUID */
	ANE_H16_T_REPORT_DONE,	/* that report until the completion report */
	ANE_H16_T_GATE_CLOSE,	/* 0x002d 1 published until its reply */
	ANE_H16_T_FINISH,	/* read barrier, freeing the command and its arena */
	ANE_H16_T_COPY_OUT,	/* outputs copied out */
	ANE_H16_T_FIRMWARE,	/* opening gate published until closing gate reply */
	ANE_H16_T_IOCTL,	/* the whole request after copying its buffer table */
	ANE_H16_T_PHASES
};

struct ane_h16_exec_sample {
	u64 ns[ANE_H16_T_PHASES];
	/* Sleeps between polls over all four waits. */
	u64 polls;
};

struct ane_h16_exec_stats {
	u64 count;
	struct ane_h16_exec_sample last, sum, min, max;
};

extern const char *const ane_h16_exec_phase_names[ANE_H16_T_PHASES];
void ane_h16_exec_stats_add(struct ane_h16_exec_stats *st,
			    const struct ane_h16_exec_sample *x);

/*
 * Programs of one firmware session: one loaded at a time, any number per
 * boot; a switch-safe transport may hold several. See ane_h16_session_load().
 */
/*
 * Program memory given back by a program that the firmware unloaded. It
 * stays mapped until reset like every firmware-visible mapping, and serves
 * a later program's mapping of the same size and permission instead of a
 * new one, so loading the same model again and again uses no new memory.
 */
#define ANE_H16_SPARES 64

struct ane_h16_spare {
	struct ane_fw_surface surface;
	bool write;
};

struct ane_h16_session {
	struct ane_fw_transport *transport;
	const struct ane_h16_session_ops *ops;
	void *ctx;
	u64 timeout_ns;
	struct ane_h16_pending_table pending;
	/* The last loaded program. */
	struct ane_h16_program *program;
	struct ane_h16_spare spares[ANE_H16_SPARES];
	unsigned int spare_count;
	/* Quiet time that ends serving the SHAREDMALLOC ring after UNLOAD. */
	u64 settle_ns;
	/*
	 * Firmware boot count: a restart (system sleep) loses every program
	 * loaded before it. While asleep, requests wait in
	 * ane_h16_session_enter(); lock is the caller's serializing lock.
	 */
	u64 epoch;
	bool asleep;
	wait_queue_head_t wake;
	/* The accel node lock the sleep gate serializes on. */
	struct mutex *lock;
	/* Generation of the last loaded program, and of the last one executed. */
	u64 generation;
	u64 executed;
	u32 dsid;
	int error;
	/* Firmware log lines printed after the last failed EXECUTE. */
	unsigned int fwlog_lines;
	bool quarantined;
	/* Firmware phases of the last successful EXECUTE. */
	struct ane_h16_exec_sample timing;
	/* Accounted EXECUTEs; the lock lets a reader snapshot them. */
	spinlock_t stats_lock;
	struct ane_h16_exec_stats stats;
};

int ane_h16_session_init(struct ane_h16_session *s, struct ane_fw_transport *transport,
			 const struct ane_h16_session_ops *ops, void *ctx,
			 u64 timeout_ns);
int ane_h16_session_load(struct ane_h16_session *s,
			 struct ane_h16_hwx_snapshot *snapshot,
			 const void *const aux[ANE_H16_AUX_COUNT],
			 const size_t aux_size[ANE_H16_AUX_COUNT],
			 struct ane_h16_program **program);
int ane_h16_session_execute(struct ane_h16_session *s, struct ane_h16_program *p);
int ane_h16_session_close(struct ane_h16_session *s, struct ane_h16_program *p);
void ane_h16_session_put(struct ane_h16_session *s, struct ane_h16_program *p);
int ane_h16_session_enter(struct ane_h16_session *s);
void ane_h16_session_sleep(struct ane_h16_session *s, bool asleep);
void ane_h16_session_restart(struct ane_h16_session *s);
bool ane_h16_program_lost(const struct ane_h16_session *s, const struct ane_h16_program *p);
void ane_h16_session_sync(struct ane_h16_session *s, struct ane_h16_program *p,
			  bool for_device);
void ane_h16_session_account(struct ane_h16_session *s, u64 lock_ns, u64 copy_in_ns,
			     u64 copy_out_ns, u64 ioctl_ns);
void ane_h16_session_stats(struct ane_h16_session *s, struct ane_h16_exec_stats *out,
			   bool reset);

struct seq_file;
int ane_h16_session_stats_show(struct seq_file *m, struct ane_h16_session *s);

#define ANE_H16_FWLOG_RECORD 0x80
size_t ane_h16_fwlog_line(const u8 *record, char *line);

#endif /* __ANE_H16_PROGRAM_H__ */
