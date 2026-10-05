// SPDX-License-Identifier: GPL-2.0-only
/* Pure display-handoff decisions; the caller owns serialization and effects. */
#include <linux/errno.h>

#include "apple-dpin-state.h"
#include "nhi.h"

const struct apple_dpin_policy apple_dpin_disabled;

const struct apple_dpin_policy apple_dpin_m1 = {
	.flow = APPLE_DPIN_CHANGED,
	.capacity_retry = true,
	.setup_irqs = true,
	.host_policy = TB_HOST_DP_NOTIFY,
};

const struct apple_dpin_policy apple_dpin_m2 = {
	.flow = APPLE_DPIN_PRE_POST,
	/* Capacity retry remains disabled pending M2 hardware qualification. */
	.t602x_handshake = true,
	.host_policy = TB_HOST_DP_HPD_ON_ACTIVATE |
		       TB_HOST_DP_ACTIVE_BEFORE_DPRX |
		       TB_HOST_DP_KEEP_DPRX_TIMEOUT |
		       TB_HOST_DP_ADAPTER_QUIRKS |
		       TB_HOST_DP_INITIAL_BW_GRANT,
};

const struct apple_dpin_policy apple_dpin_m3 = {
	.flow = APPLE_DPIN_CHANGED,
	.host_policy = TB_HOST_DP_NOTIFY,
};

enum apple_dpin_flow apple_dpin_hooks(const struct apple_dpin_policy *policy,
				      bool queue_present, bool display_enabled)
{
	if (!queue_present || (policy->flow == APPLE_DPIN_CHANGED && !display_enabled))
		return APPLE_DPIN_DISABLED;
	return policy->flow;
}

bool apple_dpin_readiness_retry(bool active, int result, unsigned int tries)
{
	if (!active || (result != -ENODEV && result != -EAGAIN))
		return false;
	return tries < (result == -EAGAIN ? APPLE_DP_FIRMWARE_TRIES :
					  APPLE_DP_CONNECT_TRIES);
}

unsigned int apple_dpin_step(struct apple_dpin_state *s,
			     const struct apple_dpin_policy *p,
			     enum apple_dpin_event event, bool mapped, int result)
{
	unsigned int actions = 0;
	bool wait;

	switch (event) {
	case APPLE_DPIN_REARM:
		s->rearm = true;
		fallthrough;
	case APPLE_DPIN_UP:
		s->alive = true;
		s->phase = s->handed ? APPLE_DPIN_HANDED : APPLE_DPIN_ACTIVATING;
		return APPLE_DPIN_QUEUE;
	case APPLE_DPIN_DOWN:
		s->alive = false;
		s->phase = s->handed || mapped ? APPLE_DPIN_TEARDOWN : APPLE_DPIN_IDLE;
		return APPLE_DPIN_QUEUE;
	case APPLE_DPIN_WORK:
		if (!s->alive) {
			s->rearm = false;
			s->waiting = false;
			s->phase = s->handed || mapped ? APPLE_DPIN_TEARDOWN :
						       APPLE_DPIN_IDLE;
			return APPLE_DPIN_CANCEL_RETRY |
			       (s->handed || mapped ? APPLE_DPIN_DROP : 0);
		}
		if (s->rearm && s->handed) {
			s->rearm = false;
			s->phase = APPLE_DPIN_TEARDOWN;
			return APPLE_DPIN_DROP | APPLE_DPIN_AGAIN;
		}
		s->rearm = false;
		if (s->handed) {
			s->phase = APPLE_DPIN_HANDED;
			return 0;
		}
		/* Preserve legacy event bring-up while paused; #29 is separate. */
		s->phase = APPLE_DPIN_ACTIVATING;
		return APPLE_DPIN_ATTACH;
	case APPLE_DPIN_RESULT:
		if (!result) {
			s->handed = true;
			if (s->waiting)
				actions |= APPLE_DPIN_RECOVERED;
			s->waiting = false;
			s->phase = APPLE_DPIN_HANDED;
			return actions | APPLE_DPIN_LOG_CONNECTED | APPLE_DPIN_AGAIN;
		}
		wait = (result == -EBUSY || result == -EAGAIN) && p->capacity_retry;
		if (s->alive && wait) {
			if (!s->waiting)
				actions |= APPLE_DPIN_FIRST_WAIT;
			s->waiting = true;
		} else {
			s->waiting = false;
		}
		if (!wait)
			actions |= APPLE_DPIN_CANCEL_RETRY;
		if (!s->alive) {
			s->phase = mapped ? APPLE_DPIN_TEARDOWN : APPLE_DPIN_IDLE;
			return actions | APPLE_DPIN_AGAIN;
		}
		if (wait) {
			s->phase = APPLE_DPIN_WAITING_PIPELINE;
			if (!s->paused)
				actions |= APPLE_DPIN_ARM_RETRY;
			return actions;
		}
		s->phase = APPLE_DPIN_FAILED;
		return actions | APPLE_DPIN_WARN;
	case APPLE_DPIN_RETRY:
		return s->alive && s->waiting && !s->paused ? APPLE_DPIN_QUEUE : 0;
	case APPLE_DPIN_DROPPED:
		s->handed = false;
		s->phase = s->alive ? APPLE_DPIN_ACTIVATING : APPLE_DPIN_IDLE;
		return 0;
	case APPLE_DPIN_PAUSE:
		s->paused = true;
		return 0;
	case APPLE_DPIN_RESUME:
		s->paused = false;
		return s->alive && s->waiting ? APPLE_DPIN_ARM_RETRY : 0;
	}
	return 0;
}
