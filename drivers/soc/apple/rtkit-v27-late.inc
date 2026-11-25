/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Included by rtkit.c. Existing common RTKit/mailbox function bodies unchanged. */
#include <linux/module.h>
struct apple_rtkit_v27_scope {
	struct apple_rtkit *rtk;
	struct module *provider_module;
	DECLARE_KFIFO(deferred, struct apple_mbox_msg, 64);
	bool held, stopped, failed;
	int error;
	/*
	 * Run once between the AP state's acknowledgment and the IOP request, interrupts off.
	 * An error fails the idle.
	 */
	int (*after_ap)(void *data);
	void *after_ap_data;
};

void apple_rtkit_v27_set_after_ap(struct apple_rtkit_v27_scope *s, int (*fn)(void *), void *data)
{
	if (s) {
		s->after_ap = fn;
		s->after_ap_data = data;
	}
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_set_after_ap);

bool apple_rtkit_v27_live(struct apple_rtkit *rtk)
{
	return !rtk->crashed && !rtk->shutting_down &&
		rtk->ap_power_state == APPLE_RTKIT_PWR_STATE_ON &&
		rtk->iop_power_state == APPLE_RTKIT_PWR_STATE_ON;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_live);

int apple_rtkit_v27_disarm(struct apple_rtkit_v27_scope *s, bool stopped)
{
	struct apple_mbox_msg msg;
	struct apple_rtkit *rtk;

	if (!s || !s->held)
		return -EINVAL;
	rtk = s->rtk;
	if (s->failed || (stopped && !s->stopped))
		return -EUCLEAN; /* retain reservation/storage/RX on uncertain stop */
	/* Caller has verified restored provider power before stopped=true. */
	if (stopped)
		apple_mbox_set_recv_enabled(rtk->mbox, true);
	s->held = false;
	mutex_unlock(&rtk->mbox->lifecycle_lock);
	/* Process context, outside provider lifecycle lock. Do not discard an
	 * accepted packet just because precommit admission had to refuse.
	 */
	while (kfifo_get(&s->deferred, &msg))
		apple_rtkit_rx_dispatch(rtk, msg.msg1, msg.msg0, false, 0);
	apple_mbox_release_rx(rtk->mbox);
	module_put(s->provider_module);
	put_device(rtk->mbox->dev);
	kfree(s);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_disarm);

int apple_rtkit_v27_arm(struct apple_rtkit *rtk, struct apple_rtkit_v27_scope **result)
{
	struct apple_rtkit_v27_scope *s;
	struct apple_mbox *b = rtk->mbox;
	struct apple_mbox_msg msg;
	int ret;

	if (!IS_ENABLED(CONFIG_APPLE_SOC_SLEEP) ||
	    !apple_rtkit_v27_rx_drained(rtk) || rtk->strict_idle_poisoned || rtk->crashed ||
	    rtk->shutting_down || rtk->ap_power_state != APPLE_RTKIT_PWR_STATE_ON ||
	    rtk->iop_power_state != APPLE_RTKIT_PWR_STATE_ON || b->irq_recv_not_empty < 0 ||
	    !apple_mbox_poll_one_supported(b))
		return -EBUSY;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	if (!mutex_trylock(&b->lifecycle_lock)) {
		kfree(s);
		return -EBUSY;
	}
	if (b->removing || !b->active || b->cookie != rtk || b->rx != apple_rtkit_rx ||
	    !b->dev->driver || !try_module_get(b->dev->driver->owner)) {
		mutex_unlock(&b->lifecycle_lock);
		kfree(s);
		return -ENODEV;
	}
	s->provider_module = b->dev->driver->owner;
	get_device(b->dev);
	s->rtk = rtk;
	s->held = true;
	INIT_KFIFO(s->deferred);
	apple_mbox_hold_rx(b); /* process-context IRQ synchronization, never final commit */
	if (!apple_rtkit_v27_rx_drained(rtk)) {
		ret = -EBUSY;
		goto undo;
	}
	ret = apple_mbox_recv_nowait(b, &msg);
	if (ret > 0) {
		kfifo_put(&s->deferred, msg);
		ret = -EAGAIN;
	}
	if (ret)
		goto undo;
	*result = s;
	return 0;
undo:
	apple_rtkit_v27_disarm(s, false);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_arm);

/*
 * Before the commit, a pending message refuses the sleep (it is kept for the disarm). After it,
 * the stop that follows reads the mailbox itself: a message there is left to it.
 */
int apple_rtkit_v27_check(struct apple_rtkit_v27_scope *s, bool committed)
{
	struct apple_mbox_msg msg;
	int ret;

	if (!s || !s->held || s->failed || s->stopped ||
	    s->rtk->mbox->removing || !s->rtk->mbox->active ||
	    s->rtk->crashed || !apple_rtkit_v27_rx_drained(s->rtk) ||
	    s->rtk->ap_power_state != APPLE_RTKIT_PWR_STATE_ON ||
	    s->rtk->iop_power_state != APPLE_RTKIT_PWR_STATE_ON)
		return -EBUSY;
	if (kfifo_is_full(&s->deferred))
		return -ENOBUFS;
	if (committed)
		return 0;
	ret = apple_mbox_recv_nowait(s->rtk->mbox, &msg);
	if (ret > 0) {
		kfifo_put(&s->deferred, msg);
		return -EAGAIN;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_check);

/*
 * A second port's AP idle with interrupts off: sent, and its acknowledgment awaited by polling
 * the port's mailbox, for at most @us. The port's tracked state is not updated.
 */
int apple_rtkit_v27_port_ap_idle_atomic(struct apple_rtkit *rtk, unsigned int us)
{
	struct apple_mbox_msg msg = {
		.msg0 = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, APPLE_RTKIT_MGMT_SET_AP_PWR_STATE) |
			FIELD_PREP(APPLE_RTKIT_MGMT_PWR_STATE, APPLE_RTKIT_PWR_STATE_IDLE),
		.msg1 = APPLE_RTKIT_EP_MGMT,
	};
	unsigned int i, n = us / 10;
	int ret = -ETIMEDOUT;

	for (i = 0; i < n; i++) {
		ret = apple_mbox_send_nowait(rtk->mbox, msg);
		if (ret != -EAGAIN && ret != -EBUSY)
			break;
		udelay(10);
	}
	if (ret)
		return ret;
	for (; i < n; i++) {
		struct apple_mbox_msg in;

		ret = apple_mbox_recv_nowait(rtk->mbox, &in);
		if (ret < 0 && ret != -EBUSY)
			return ret;
		if (ret > 0 && in.msg1 == APPLE_RTKIT_EP_MGMT &&
		    FIELD_GET(APPLE_RTKIT_MGMT_TYPE, in.msg0) == APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK)
			return FIELD_GET(APPLE_RTKIT_MGMT_PWR_STATE, in.msg0) ==
			       APPLE_RTKIT_PWR_STATE_IDLE ? 0 : -EPROTO;
		udelay(10);
	}
	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_port_ap_idle_atomic);

int apple_rtkit_v27_idle_atomic(struct apple_rtkit_v27_scope *s)
{
	struct apple_rtkit *rtk;
	struct apple_mbox_msg msg, echo = {};
	bool ap_sent = false, ap_ack = false, iop_sent = false, iop_ack = false;
	bool done = false, echo_pending = false;
	unsigned int tick;
	int ret = -ETIMEDOUT;

	if (!s || !s->held || s->failed || !irqs_disabled())
		return -EINVAL;
	rtk = s->rtk;
	/* One shared 100 ms budget. No jiffies, allocation, worker, sleeping
	 * locks, normal send, or unbounded RX drain in saved context.
	 */
	for (tick = 0; tick < 10000; tick++) {
		if (rtk->crashed || rtk->mbox->removing) {
			ret = -EIO;
			goto poison;
		}
		if (echo_pending) {
			ret = apple_mbox_send_nowait(rtk->mbox, echo);
			if (!ret)
				echo_pending = false;
		} else if (!ap_sent || (ap_ack && !iop_sent)) {
			bool ap = !ap_sent;

			msg.msg1 = APPLE_RTKIT_EP_MGMT;
			msg.msg0 = FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, ap ?
				APPLE_RTKIT_MGMT_SET_AP_PWR_STATE : APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE) |
				FIELD_PREP(APPLE_RTKIT_MGMT_PWR_STATE, APPLE_RTKIT_PWR_STATE_IDLE);
			dma_wmb();
			ret = apple_mbox_send_nowait(rtk->mbox, msg);
			if (!ret) {
				if (ap)
					ap_sent = true;
				else
					iop_sent = true;
			}
		} else {
			ret = 0;
		}
		if (ret && ret != -EAGAIN && ret != -EBUSY)
			goto poison;
		/* Reserve storage before consuming any potentially foreign packet. */
		if (kfifo_is_full(&s->deferred)) {
			ret = -ENOBUFS;
			goto poison;
		}
		if (!echo_pending) {
			ret = apple_mbox_recv_nowait(rtk->mbox, &msg);
			if (ret < 0 && ret != -EBUSY)
				goto poison;
			if (ret > 0) {
				u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg.msg0);
				u64 arg = msg.msg0 & GENMASK_ULL(51, 0);

				dma_rmb();
				if (msg.msg1 == APPLE_RTKIT_EP_MGMT && ap_sent && !ap_ack &&
				    type == APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK &&
				    arg == APPLE_RTKIT_PWR_STATE_IDLE) {
					ap_ack = true;
					if (s->after_ap) {
						ret = s->after_ap(s->after_ap_data);
						if (ret)
							goto poison;
					}
				} else if (msg.msg1 == APPLE_RTKIT_EP_MGMT && iop_sent && !iop_ack &&
					   type == APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK &&
					   arg == APPLE_RTKIT_PWR_STATE_IDLE) {
					iop_ack = true;
				} else if (msg.msg1 == APPLE_RTKIT_EP_MGMT && iop_sent && !done &&
					   type == APPLE_RTKIT_MGMT_IDLE_NOTIFY && !arg) {
					done = true;
				} else if (msg.msg1 == APPLE_RTKIT_EP_MGMT ||
					   msg.msg1 == APPLE_RTKIT_EP_CRASHLOG ||
					   msg.msg1 >= APPLE_RTKIT_APP_ENDPOINT_START) {
					/* A crash, unexpected handshake, or live application RPC. */
					kfifo_put(&s->deferred, msg);
					ret = -EPROTO;
					goto poison;
				} else if (apple_rtkit_oslog_owed_msg(rtk, msg.msg1, msg.msg0) ||
					   apple_rtkit_notification_msg(msg.msg1, msg.msg0)) {
					/* Echo now. The ring entry can be reused after the echo. */
					echo = msg;
					echo_pending = true;
				} else {
					/* Other system messages are replayed on disarm. */
					kfifo_put(&s->deferred, msg);
				}
			}
		}
		if (ap_ack && iop_ack && done && !echo_pending) {
			rtk->ap_power_state = rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_IDLE;
			s->stopped = true;
			apple_mbox_set_recv_enabled(rtk->mbox, false);
			return 0;
		}
		udelay(10);
	}
	ret = -ETIMEDOUT;
poison:
	s->failed = true;
	s->error = ret;
	rtk->strict_idle_poisoned = true;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_idle_atomic);
