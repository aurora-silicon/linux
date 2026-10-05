/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DPIN_REQUEST_H
#define _APPLE_DPIN_REQUEST_H

#include "apple-dpin-state.h"

struct apple_dpin_request_ops {
	void (*inactive)(void *ctx);
	void (*mask_irqs)(void *ctx);
	void *ctx;
};

/* Caller holds the DP-IN lock across token, MMIO and lifecycle publication. */
static inline bool
apple_dpin_powered_request(struct apple_dpin_state *state, struct apple_dpin_tokens *tokens,
			   const struct apple_dpin_policy *policy, u64 generation,
			   bool active, bool mapped, const struct apple_dpin_request_ops *ops)
{
	bool replacement = active && tokens->requested != generation;
	enum apple_dpin_event event = apple_dpin_request_event(state, tokens, generation, active);

	if (!apple_dpin_token_request(tokens, generation, active))
		return false;
	/* Revoke the old callback first, then idle before any queued route drop. */
	if (state->alive && mapped && (!active || replacement)) {
		ops->inactive(ops->ctx);
		if (!active && policy->flow == APPLE_DPIN_CHANGED)
			ops->mask_irqs(ops->ctx);
	}
	if (replacement) {
		state->waiting = false;
		state->deferred_first = false;
		state->replay_queued = false;
	}
	/* A running worker must never see the new token with the old event. */
	apple_dpin_step(state, policy, event, mapped, 0);
	return true;
}

#endif
