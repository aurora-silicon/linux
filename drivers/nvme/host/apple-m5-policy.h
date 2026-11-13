/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _APPLE_NVME_M5_POLICY_H_
#define _APPLE_NVME_M5_POLICY_H_
#include <linux/atomic.h>
#include <linux/completion.h>

struct apple_nvme_m5_policy {
	u32 flush_nsid;
	u32 shn;
	unsigned int drain_ms, flush_ms, phy_to_run_ms, rebuild_ms;
};

/* T6050's ANS2 in a system sleep: the namespace flushed, the controller's shutdown, the waits. */
static const struct apple_nvme_m5_policy apple_nvme_t6050_sleep_policy = {
	.flush_nsid = 1,
	.shn = NVME_CC_SHN_MASK,
	.drain_ms = 1000,
	.flush_ms = 10000,
	.phy_to_run_ms = 70,
	.rebuild_ms = 10000,
};

enum apple_nvme_m5_phase {
	APPLE_M5_ANS_LIVE,
	APPLE_M5_ANS_PREPARING,
	APPLE_M5_ANS_PREPARED,
	APPLE_M5_ANS_FINAL,
	APPLE_M5_ANS_STARTING,
	APPLE_M5_ANS_REBUILDING,
	APPLE_M5_ANS_FAILED,
};

struct request_queue;

struct apple_nvme_m5_sleep {
	enum apple_nvme_m5_phase phase;
	atomic64_t submissions;
	bool registered, frozen, admin_frozen, held;
	int stop_error, flush_result;
	struct completion flush_done, rebuild_done;
	int rebuild_result;
	bool rebuild_active, rebuild_after_wake, rebuild_aborted, rebuild_pinned;
	struct request_queue *flushed_queue;
};
#endif
