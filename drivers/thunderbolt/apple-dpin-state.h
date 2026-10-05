/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DPIN_STATE_H
#define _APPLE_DPIN_STATE_H

#include <linux/bits.h>
#include <linux/types.h>

#define APPLE_DPIN_RETRY_MS	2000
#define APPLE_DP_CONNECT_TRIES	60
#define APPLE_DP_CONNECT_WAIT_MS	500
#define APPLE_DP_FIRMWARE_TRIES	240

enum apple_dpin_flow {
	APPLE_DPIN_DISABLED,
	APPLE_DPIN_CHANGED,
	APPLE_DPIN_PRE_POST,
};

struct apple_dpin_policy {
	enum apple_dpin_flow flow;
	bool capacity_retry;
	bool setup_irqs;
	bool t602x_handshake;
	unsigned long host_policy;
};

extern const struct apple_dpin_policy apple_dpin_disabled;
extern const struct apple_dpin_policy apple_dpin_m1;
extern const struct apple_dpin_policy apple_dpin_m2;
extern const struct apple_dpin_policy apple_dpin_m3;

enum apple_dpin_phase {
	APPLE_DPIN_IDLE,
	APPLE_DPIN_ACTIVATING,
	APPLE_DPIN_WAITING_PIPELINE,
	APPLE_DPIN_HANDED,
	APPLE_DPIN_TEARDOWN,
	APPLE_DPIN_FAILED,
};

/* Desired tunnel state and worker-owned handoff state are separate facts. */
struct apple_dpin_state {
	enum apple_dpin_phase phase;
	bool alive;
	bool handed;
	bool rearm;
	bool waiting;
	bool paused;
};

enum apple_dpin_event {
	APPLE_DPIN_UP,
	APPLE_DPIN_REARM,
	APPLE_DPIN_DOWN,
	APPLE_DPIN_WORK,
	APPLE_DPIN_RESULT,
	APPLE_DPIN_DROPPED,
	APPLE_DPIN_RETRY,
	APPLE_DPIN_PAUSE,
	APPLE_DPIN_RESUME,
};

#define APPLE_DPIN_QUEUE		BIT(0)
#define APPLE_DPIN_CANCEL_RETRY	BIT(1)
#define APPLE_DPIN_DROP		BIT(2)
#define APPLE_DPIN_ATTACH		BIT(3)
#define APPLE_DPIN_AGAIN		BIT(4)
#define APPLE_DPIN_FIRST_WAIT	BIT(5)
#define APPLE_DPIN_ARM_RETRY	BIT(6)
#define APPLE_DPIN_WARN		BIT(7)
#define APPLE_DPIN_RECOVERED	BIT(8)
#define APPLE_DPIN_LOG_CONNECTED	BIT(9)

unsigned int apple_dpin_step(struct apple_dpin_state *state,
			     const struct apple_dpin_policy *policy,
			     enum apple_dpin_event event, bool mapped, int result);
bool apple_dpin_readiness_retry(bool active, int result, unsigned int tries);
enum apple_dpin_flow apple_dpin_hooks(const struct apple_dpin_policy *policy,
				      bool queue_present, bool display_enabled);

#endif
