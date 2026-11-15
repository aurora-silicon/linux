/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * DCPLink RPC transport for the J613/25G83 DCP firmware (macOS 26.6.2 (25G83) IOMFB)
 *
 * Copyright The Asahi Linux Contributors
 * Based on the M3 Pro DCP lab client by Eryk Wieliczko.
 */

#ifndef __APPLE_IOMFB_V26_6_LINK_H__
#define __APPLE_IOMFB_V26_6_LINK_H__

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "dcp-link.h"

#define DCP_V26_LINK_MAX_EVENTS		32
#define DCP_V26_LINK_MAX_CALLBACKS	8

/* Method and callback tags: 'A' or 'D' followed by three decimal digits. */
#define DCP_V26_TAG(c, n) (((u32)(c) << 24) | ((u32)('0' + (n) / 100) << 16) | \
			   ((u32)('0' + (n) / 10 % 10) << 8) | (u32)('0' + (n) % 10))

struct apple_rtkit;
struct work_struct;

/*
 * Handles one callback from the firmware. @out is zeroed and is sent back
 * as the reply when the handler returns 0; any error stops the link.
 */
typedef int (*dcp_v26_callback_fn)(void *cookie, u32 tag, const void *in,
				   u32 in_size, void *out, u32 out_size);

struct dcp_v26_link {
	struct device *dev;
	struct apple_rtkit *rtk;
	void *rpc;
	dma_addr_t rpc_dva;

	/* Protects everything below; never held across a callback handler. */
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head events;
	unsigned int event_count;
	unsigned int callback_count;
	/* Callback handlers running on the caller's stack. */
	unsigned int callback_depth;
	bool failed;
	bool enabled;
	struct work_struct *idle_work;

	struct completion ready;
	int ready_result;

	/*
	 * Receive times (CLOCK_MONOTONIC) of the message being handled: of the
	 * callback whose handler runs, of the last reply, and of the last swap
	 * completion. Only the thread that owns the RPC stream uses them.
	 */
	ktime_t callback_time;
	ktime_t reply_time;
	ktime_t completion_time;

	/* Our calls: [0] outer, [1] made from inside a callback. */
	struct {
		bool active;
		bool remote;
		u32 offset;
		u32 size;
		struct apple_dcp_link_rpc_header header;
	} calls[2];

	/* Firmware callbacks awaiting a reply, innermost last. */
	struct {
		u64 message;
		void *output;
		u32 size;
		u32 call;
	} callbacks[DCP_V26_LINK_MAX_CALLBACKS];
};

void dcp_v26_link_init(struct dcp_v26_link *link, struct device *dev,
		       struct apple_rtkit *rtk);
int dcp_v26_link_start(struct dcp_v26_link *link);
void dcp_v26_link_receive(struct dcp_v26_link *link, u64 message);
void dcp_v26_link_fail(struct dcp_v26_link *link);
bool dcp_v26_link_failed(struct dcp_v26_link *link);
void dcp_v26_link_set_idle_work(struct dcp_v26_link *link, struct work_struct *work);
int dcp_v26_link_call(struct dcp_v26_link *link, u32 tag, const void *input,
		      u32 input_size, void *output, u32 output_size,
		      u32 completion_id, dcp_v26_callback_fn callback, void *cookie);
int dcp_v26_link_pump(struct dcp_v26_link *link, unsigned long timeout,
		      dcp_v26_callback_fn callback, void *cookie);

#endif /* __APPLE_IOMFB_V26_6_LINK_H__ */
