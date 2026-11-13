/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Included by rtkit.c. Existing common RTKit/mailbox function bodies unchanged. */
#include <linux/module.h>
struct apple_rtkit_v27_scope {
	struct apple_rtkit *rtk;
	struct module *provider_module;
	DECLARE_KFIFO(deferred, struct apple_mbox_msg, 64);
	bool held, stopped, failed;
	int error;
};

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
		apple_rtkit_rx_dispatch(rtk, msg.msg1, msg.msg0, false);
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

int apple_rtkit_v27_check(struct apple_rtkit_v27_scope *s)
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
	ret = apple_mbox_recv_nowait(s->rtk->mbox, &msg);
	if (ret > 0) {
		kfifo_put(&s->deferred, msg);
		return -EAGAIN;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_v27_check);

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
				} else if (msg.msg1 == APPLE_RTKIT_EP_MGMT && iop_sent && !iop_ack &&
					   type == APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK &&
					   arg == APPLE_RTKIT_PWR_STATE_IDLE) {
					iop_ack = true;
				} else if (msg.msg1 == APPLE_RTKIT_EP_MGMT && iop_sent && !done &&
					   type == APPLE_RTKIT_MGMT_IDLE_NOTIFY && !arg) {
					done = true;
				} else if (apple_rtkit_oslog_owed_msg(rtk, msg.msg1, msg.msg0)) {
					echo = msg;
					echo_pending = true;
				} else {
					kfifo_put(&s->deferred, msg);
					ret = -EPROTO;
					goto poison;
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
