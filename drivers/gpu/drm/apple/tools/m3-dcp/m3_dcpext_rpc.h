/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_RPC_H
#define M3_DCPEXT_RPC_H
#include <linux/types.h>
struct device;
struct m3_dcpext_rpc;
struct m3_dcpext_buffer {
	u64 size, physical, dva;
	u32 id, flags;
};
/* Owner retains RPC storage and every DMA allocation until qualified firmware
 * quiescence, including failures. poll dispatches mailbox messages back into
 * receive; it must honor the owner's absolute deadline/watchdog/thermal gates.
 * Calls/pump are serialized by the owner; receive may run on another worker.
 * send must not synchronously reenter receive (the RPC mutex is held).
 */
struct m3_dcpext_rpc_ops {
	int (*send)(void *cookie, u8 endpoint, u64 message);
	int (*poll)(void *cookie, unsigned long timeout);
	int (*alloc)(void *cookie, struct m3_dcpext_buffer *buffer);
	int (*map)(void *cookie, struct m3_dcpext_buffer *buffer);
	int (*retire)(void *cookie, u32 id);
	/* Firmware power vote; owner may retain an independent session vote. */
	int (*dart_power)(void *cookie, bool on);
	/* Optional bounded diagnostic sink: kind 1 reply, 2 callback,
	 * 3 host request, 4 callback reply. Failure stops transport. */
	int (*record)(void *cookie, u32 kind, u64 message, const void *data,
		      u32 size);
};
typedef int (*m3_dcpext_callback_fn)(struct m3_dcpext_rpc *, void *, u32,
				     const void *, u32, void *, u32);
struct m3_dcpext_rpc *m3_dcpext_rpc_create(struct device *, void *memory,
					   const struct m3_dcpext_rpc_ops *,
					   void *cookie);
void m3_dcpext_rpc_receive(struct m3_dcpext_rpc *, u64 message);
void m3_dcpext_rpc_crashed(struct m3_dcpext_rpc *);
/* Only after all callbacks/workers stop and firmware quiescence is proven. */
void m3_dcpext_rpc_destroy(struct m3_dcpext_rpc *);
int m3_dcpext_rpc_call(struct m3_dcpext_rpc *, u32 tag, const void *, u32,
		       void *, u32, u32 completion_id, m3_dcpext_callback_fn,
		       void *);
/* Check owner state after draining callbacks and before each send attempt.
 * The owner serializes state changes with calls/pump. A false check returns
 * -ESTALE without sending this request or poisoning the transport. */
int m3_dcpext_rpc_call_checked(struct m3_dcpext_rpc *, u32 tag, const void *, u32,
			     void *, u32, u32 completion_id,
			     m3_dcpext_callback_fn, void *, bool (*valid)(void *));
int m3_dcpext_rpc_pump(struct m3_dcpext_rpc *, unsigned long timeout,
		       m3_dcpext_callback_fn, void *);
int m3_dcpext_rpc_alloc(struct m3_dcpext_rpc *, struct m3_dcpext_buffer *);
int m3_dcpext_rpc_map(struct m3_dcpext_rpc *, struct m3_dcpext_buffer *);
int m3_dcpext_rpc_retire(struct m3_dcpext_rpc *, u32 id);
int m3_dcpext_rpc_dart_power(struct m3_dcpext_rpc *, bool on);
#endif
