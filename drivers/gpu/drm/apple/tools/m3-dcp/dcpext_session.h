/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Transport-independent RTKit system-session diagnostic. No MMIO or DMA API.
 * Protocol reference: Asahi Linux Contributors, drivers/soc/apple/rtkit.c
 * in linux/linux-displayport at base fba91e5d9577 (see provenance manifest).
 * This component starts no application endpoints and cannot read EDID itself.
 */
#ifndef DCPEXT_SESSION_H
#define DCPEXT_SESSION_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

enum dcpext_phase {
	DCPEXT_NEW, DCPEXT_HELLO, DCPEXT_EPMAP, DCPEXT_IOP_ON,
	DCPEXT_AP_ON, DCPEXT_RUNNING, DCPEXT_AP_QUIESCE,
	DCPEXT_IOP_QUIESCE, DCPEXT_QUIESCED, DCPEXT_FAILED,
};

struct dcpext_buffer {
	u64 dva;
	u32 size;
	bool ready, inherited;
};

struct dcpext_session_ops {
	/* Single serialized caller. All callbacks must be bounded, non-reentrant.
	 * send must supply the DMA write barrier before mailbox publication.
	 * recv's caller supplies the DMA read barrier after mailbox receipt.
	 */
	u64 (*now_ms)(void *cookie);
	int (*send)(void *cookie, u8 ep, u64 msg);
	/* Nonzero requested address: validate the ENTIRE inherited mapping
	 * against live reserved RAM; do not clear/rewrite firmware-owned memory.
	 * M4 DART wire addresses retain their 1-TiB bias. EP8 may instead name
	 * the physical OS-log carveout: accept only an exact live reservation,
	 * never a generic physical-address fallback or an invented DART map.
	 * Zero DVA: obtain a zeroed, mapped host buffer of at least size bytes.
	 * Retain ownership even on errors: the engine never frees anything.
	 * This callback neither sends messages nor starts firmware execution.
	 */
	int (*buffer)(void *cookie, u8 ep, u64 requested, u32 size, u64 *dva);
};

struct dcpext_session {
	const struct dcpext_session_ops *ops;
	void *cookie;
	enum dcpext_phase phase;
	int error;
	u64 deadline, last_time;
	u32 duration_ms; /* 0=8s; explicit bounded image observation, at most60s. */
	u32 message_limit; /* 0=256; native+AV 512; bounded 16s KMS 8192. */
	u32 endpoints[8];
	u32 started;
	u8 bases;
	unsigned int received;
	bool runtime; /* Explicit post-startup desktop state; RPCs keep their deadlines. */
	u64 runtime_window;
	u32 runtime_messages;
	bool hello, epmap_done, iop_on, touched;
	bool sleep_iop; /* Linux shutdown: AP quiesced, then restartable IOP sleep. */
	bool defer_iop_quiesce, ap_quiesced;
	unsigned int oslog_notices;
	struct dcpext_buffer buffers[9];
};

/* Zero-initialize a new instance before begin; never reinitialize a live one.
 * begin admits only the system endpoint profile seen on J514S 14.6 DCPEXT.
 * The adapter must separately qualify the firmware UUID and DART handoff.
 */
int dcpext_session_begin(struct dcpext_session *s,
			 const struct dcpext_session_ops *ops, void *cookie);
int dcpext_session_poll(struct dcpext_session *s);
int dcpext_session_enter_runtime(struct dcpext_session *s);
int dcpext_session_leave_runtime(struct dcpext_session *s);
/* Adapter accounts every mailbox endpoint, including application traffic. */
int dcpext_session_runtime_message(struct dcpext_session *s);
int dcpext_session_receive(struct dcpext_session *s, unsigned int ep, u64 msg);
int dcpext_session_quiesce(struct dcpext_session *s);
/* Optional ordering experiment: only after AP ACK, let the owner stop its
 * application channels while IOP remains awake, then request IOP quiescence.
 */
int dcpext_session_finish_quiesce(struct dcpext_session *s);

/* QUIESCED means both protocol ACKs arrived. It is NOT a cleanup function or
 * proof of real hardware quiescence. Before freeing, the hardware adapter must
 * also stop/drain its transport and complete DART removal/invalidation.
 * FAILED is sticky; late ACKs cannot permit freeing possibly reachable RAM.
 */
#endif
