// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

/*
 * HDCP 2.2 on the display processors' external outputs.
 *
 * DCP opens an authentication session after the link comes up and calls the
 * AP with the session's events; the AP answers each at once and runs the
 * transmitter (dcp-hdcp-tx.c) on its own ordered queue, with the SEP doing
 * the cryptography (include/linux/apple-sep-hdcp.h) and the sink's DP device
 * service moving the receiver's messages. The connector's Content
 * Protection property follows the policy in dcp-hdcp.h. No key material is
 * seen here: only the protocol fields the HDCP 2.2 messages carry in the
 * clear.
 *
 * Only a display processor whose node has "apple,hdcp-channels" takes
 * part; elsewhere the property is not attached and nothing changes. While
 * userspace does not ask for protection, DCP's sessions are declined.
 */

#include <linux/apple-sep-hdcp.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/display/drm_hdcp_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_modeset_lock.h>

#include "afk.h"
#include "connector.h"
#include "dcp.h"
#include "dcp-hdcp.h"
#include "dcp-hdcp-drv.h"
#include "dcp-hdcp-tx.h"
#include "dcp-hdcp-wire.h"
#include "dcp-internal.h"


/*
 * How long a register access may take: DCP applies its own AUX timeout
 * (DCP_HDCP_REG_TIMEOUT_MS, carried in the request); the extra 500 ms covers
 * the mailbox round trip, so a slow transaction ends with DCP's answer, not
 * ours. Not a spec value.
 */
#define DCP_HDCP_CALL_MS	(DCP_HDCP_REG_TIMEOUT_MS + 500)
/* The receiver's status on an encrypted link. */
#define DCP_HDCP_IFACES		4
#define DCP_HDCP_QUEUE		16

struct dcp_hdcp_event {
	u8 op;
	struct apple_epic_service *service;
};

struct dcp_hdcp {
	struct apple_dcp *dcp;
	u32 channel;

	/* The policy, the link generation and the session's identity. */
	struct mutex lock;
	struct dcp_hdcp_link link;
	u32 gen;
	u32 session_gen;

	/* Service pointers and session events, also touched from teardown. */
	spinlock_t svc_lock;
	struct apple_epic_service *relay;
	struct apple_epic_service *iface[DCP_HDCP_IFACES];
	struct apple_epic_service *session;
	/* Sessions DCP announced for HDCP 1.x: declined, this is HDCP 2.2 only. */
	struct apple_epic_service *hdcp1[DCP_HDCP_IFACES];
	unsigned long ev_bits;
	/* The device service was asked for a session (query) and not yet told it ended. */
	bool dev_begun;
	bool closed;
	bool cancel;
	struct dcp_hdcp_event queue[DCP_HDCP_QUEUE];
	unsigned int q_head, q_tail;
	wait_queue_head_t ev_wq;

	struct apple_dcp_afkep *dpdev_ep;
	/* The HDCP interface endpoint, when this file started it. */
	struct apple_dcp_afkep *if_ep;

	struct workqueue_struct *wq;
	struct work_struct event_work;
	struct work_struct teardown_work;
	struct work_struct prop_work;
	struct work_struct relay_work;
	struct work_struct caps_work;
	/* Accepted session opens; a teardown older than the latest is stale. */
	unsigned int open_seq;
	unsigned int td_seq;

	struct dcp_hdcp_tx tx;
};

static struct dcp_hdcp *dcp_hdcp_of(struct apple_epic_service *service)
{
	return service && service->ep ? READ_ONCE(service->ep->dcp->hdcp) : NULL;
}

/* Pins the service in @slot, NULL if there is none or it is gone. */
static struct apple_epic_service *dcp_hdcp_pin(struct dcp_hdcp *h,
					       struct apple_epic_service **slot)
{
	struct apple_epic_service *service;
	unsigned long flags;

	spin_lock_irqsave(&h->svc_lock, flags);
	service = afk_service_get(*slot);
	spin_unlock_irqrestore(&h->svc_lock, flags);
	return service;
}

/* The DP device service: register access. */

static int dcp_hdcp_reg_read(void *ctx, u32 addr, void *buf, size_t len)
{
	struct dcp_hdcp *h = ctx;
	struct apple_epic_service *service = dcp_hdcp_pin(h, &h->relay);
	size_t received = 0;
	void *body;
	int n, ret;

	if (!service)
		return -ENODEV;
	body = kzalloc(DCP_HDCP_REG_ARGS_LEN + len, GFP_KERNEL);
	if (!body) {
		afk_service_put(service);
		return -ENOMEM;
	}
	n = dcp_hdcp_reg_read_encode(body, DCP_HDCP_REG_ARGS_LEN + len, addr,
				     len, DCP_HDCP_REG_TIMEOUT_MS);
	ret = n < 0 ? n :
	      afk_service_call_timeout_len(service, DCP_HDCP_REG_GROUP,
					   DCP_HDCP_REG_READ, body, n, 0, body, n,
					   0, DCP_HDCP_CALL_MS, &received);
	if (!ret)
		ret = dcp_hdcp_reg_read_check(body, received, addr, len);
	if (!ret)
		memcpy(buf, body + DCP_HDCP_REG_ARGS_LEN, len);
	kfree_sensitive(body);
	afk_service_put(service);
	return ret;
}

static int dcp_hdcp_reg_write(void *ctx, u32 addr, const void *buf, size_t len)
{
	struct dcp_hdcp *h = ctx;
	struct apple_epic_service *service = dcp_hdcp_pin(h, &h->relay);
	size_t received = 0;
	void *body;
	int n, ret;

	if (!service)
		return -ENODEV;
	body = kzalloc(DCP_HDCP_REG_ARGS_LEN + len, GFP_KERNEL);
	if (!body) {
		afk_service_put(service);
		return -ENOMEM;
	}
	n = dcp_hdcp_reg_write_encode(body, DCP_HDCP_REG_ARGS_LEN + len, addr,
				      buf, len);
	ret = n < 0 ? n :
	      afk_service_call_timeout_len(service, DCP_HDCP_REG_GROUP,
					   DCP_HDCP_REG_WRITE, body, n, 0, body, n,
					   0, DCP_HDCP_CALL_MS, &received);
	if (!ret)
		ret = dcp_hdcp_reg_write_check(body, received, addr, len);
	kfree_sensitive(body);
	afk_service_put(service);
	return ret;
}

/* A call with a zero body whose answer is not needed. */
static int dcp_hdcp_plain_call(struct apple_epic_service *service, u16 group,
			       u32 command, size_t len, u32 value)
{
	struct dcp_hdcp_word *w;
	void *body = kzalloc(len, GFP_KERNEL);
	int ret;

	if (!body)
		return -ENOMEM;
	if (value && len >= sizeof(*w)) {
		w = body;
		w->value = cpu_to_le32(value);
	}
	ret = afk_service_call_timeout(service, group, command, body, len, 0,
				       body, len, 0, DCP_HDCP_CALL_MS);
	kfree(body);
	return ret;
}

/* The session service: events in, progress reports out. */

static void dcp_hdcp_report(void *ctx, enum dcp_hdcp_progress_code code,
			    bool active, bool repeater, bool encrypted)
{
	struct dcp_hdcp *h = ctx;
	struct apple_epic_service *service = dcp_hdcp_pin(h, &h->session);
	struct {
		struct epic_service_call call;
		struct dcp_hdcp_report report;
	} __packed msg = {};
	int ret;

	if (!service)
		return;
	msg.call.group = cpu_to_le16(DCP_HDCP_REPORT_GROUP);
	msg.call.command = cpu_to_le32(DCP_HDCP_REPORT_PROGRESS);
	msg.call.data_len = cpu_to_le32(sizeof(msg.report));
	msg.call.magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);
	dcp_hdcp_report_encode(&msg.report, code, active, repeater, encrypted);
	ret = afk_send_epic(service->ep, service->channel, 0, EPIC_TYPE_NOTIFY,
			    EPIC_CAT_REPORT, EPIC_SUBTYPE_STD_SERVICE, &msg,
			    sizeof(msg));
	if (ret)
		dev_dbg(h->dcp->dev, "HDCP: progress %#x not reported: %d\n",
			code, ret);
	afk_service_put(service);
}

static int dcp_hdcp_wait(void *ctx, enum dcp_hdcp_auth_op op, unsigned int ms)
{
	struct dcp_hdcp *h = ctx;
	unsigned long flags;
	bool got;

	wait_event_timeout(h->ev_wq,
			   test_bit(op, &h->ev_bits) || READ_ONCE(h->closed) ||
			   READ_ONCE(h->cancel), msecs_to_jiffies(ms));
	spin_lock_irqsave(&h->svc_lock, flags);
	if (h->closed || h->cancel) {
		spin_unlock_irqrestore(&h->svc_lock, flags);
		return -ECANCELED;
	}
	got = __test_and_clear_bit(op, &h->ev_bits);
	spin_unlock_irqrestore(&h->svc_lock, flags);
	return got ? 0 : -ETIMEDOUT;
}

static int dcp_hdcp_revoked(void *ctx, u8 *ids, unsigned int count)
{
	struct dcp_hdcp *h = ctx;
	struct apple_connector *connector = READ_ONCE(h->dcp->connector);

	if (!connector)
		return -ENODEV;
	return drm_hdcp_check_ksvs_revoked(connector->base.dev, ids, count);
}

static void dcp_hdcp_sleep(void *ctx, unsigned int ms)
{
	msleep(ms);
}

/* The SEP service. */

static int sep_open(void *ctx, u32 ch, u8 *rtx)
{
	return apple_sep_hdcp_open(ch, rtx);
}

static int sep_ake_cert(void *ctx, u32 ch, const u8 *cert, const u8 *rrx,
			bool *stored, u8 *ekh_km_m, u8 *ekpub_km)
{
	struct apple_sep_hdcp_stored_km km;
	int ret;

	static_assert(sizeof(km) == HDCP_2_2_E_KH_KM_M_LEN);
	ret = apple_sep_hdcp_ake_cert(ch, cert, rrx, stored, &km, ekpub_km);
	if (!ret && *stored)
		memcpy(ekh_km_m, &km, sizeof(km));
	memzero_explicit(&km, sizeof(km));
	return ret;
}

static int sep_h_prime(void *ctx, u32 ch, const u8 *rxcaps, const u8 *txcaps,
		       const u8 *h_prime)
{
	return apple_sep_hdcp_ake_h_prime(ch, rxcaps, txcaps, h_prime);
}

static int sep_pairing(void *ctx, u32 ch, const u8 *ekh_km)
{
	return apple_sep_hdcp_ake_pairing(ch, ekh_km);
}

static int sep_lc_init(void *ctx, u32 ch, u8 *rn)
{
	return apple_sep_hdcp_lc_init(ch, rn);
}

static int sep_l_prime(void *ctx, u32 ch, const u8 *l_prime)
{
	return apple_sep_hdcp_lc_l_prime(ch, l_prime);
}

static int sep_ske(void *ctx, u32 ch, u8 *edkey_ks, u8 *riv)
{
	return apple_sep_hdcp_ske(ch, edkey_ks, riv);
}

static int sep_repeater_list(void *ctx, u32 ch, const u8 *ids,
			     unsigned int count, const u8 *rxinfo,
			     const u8 *seq_num_v, const u8 *v_prime, u8 *v)
{
	return apple_sep_hdcp_repeater_list(ch, ids, count, rxinfo, seq_num_v,
					    v_prime, v);
}

static int sep_repeater_stream(void *ctx, u32 ch, u8 stream_id, u8 type,
			       const u8 *seq_num_m, const u8 *m_prime)
{
	return apple_sep_hdcp_repeater_stream(ch, stream_id, type, seq_num_m,
					      m_prime);
}

static int sep_finish(void *ctx, u32 ch)
{
	return apple_sep_hdcp_finish(ch);
}

static int sep_set_encryption(void *ctx, u32 ch, bool on)
{
	return apple_sep_hdcp_set_encryption(ch, on);
}

static void sep_close(void *ctx, u32 ch)
{
	apple_sep_hdcp_close(ch);
}

static const struct dcp_hdcp_tx_ops dcp_hdcp_tx_ops = {
	.read = dcp_hdcp_reg_read,
	.write = dcp_hdcp_reg_write,
	.wait = dcp_hdcp_wait,
	.report = dcp_hdcp_report,
	.revoked = dcp_hdcp_revoked,
	.sleep = dcp_hdcp_sleep,
	.sep_open = sep_open,
	.sep_ake_cert = sep_ake_cert,
	.sep_h_prime = sep_h_prime,
	.sep_pairing = sep_pairing,
	.sep_lc_init = sep_lc_init,
	.sep_l_prime = sep_l_prime,
	.sep_ske = sep_ske,
	.sep_repeater_list = sep_repeater_list,
	.sep_repeater_stream = sep_repeater_stream,
	.sep_finish = sep_finish,
	.sep_set_encryption = sep_set_encryption,
	.sep_close = sep_close,
};

/* Policy glue. */

/* What the connector this processor drives asks for. */
static void dcp_hdcp_want(struct dcp_hdcp *h, bool *want, u8 *type)
{
	struct apple_connector *connector = READ_ONCE(h->dcp->connector);

	*want = connector && READ_ONCE(connector->hdcp_attached) &&
		READ_ONCE(connector->hdcp_want);
	*type = connector ? READ_ONCE(connector->hdcp_type) : 0;
}

static void dcp_hdcp_apply(struct dcp_hdcp *h, enum dcp_hdcp_action action)
{
	unsigned long flags;

	if (action == DCP_HDCP_TEARDOWN) {
		spin_lock_irqsave(&h->svc_lock, flags);
		h->cancel = true;
		h->td_seq = h->open_seq;
		spin_unlock_irqrestore(&h->svc_lock, flags);
		wake_up_all(&h->ev_wq);
		queue_work(h->wq, &h->teardown_work);
	}
	queue_work(h->wq, &h->prop_work);
}

/* The bound state of both sides, then what the connector asks for. */
static void dcp_hdcp_sync_locked(struct dcp_hdcp *h)
{
	unsigned long now = jiffies;
	enum dcp_hdcp_action a, b;
	bool want, relay;
	u8 type;

	relay = READ_ONCE(h->relay) != NULL;
	a = dcp_hdcp_bind(&h->link, relay, apple_sep_hdcp_available(), now);
	dcp_hdcp_want(h, &want, &type);
	b = dcp_hdcp_request(&h->link, want, type, now);
	dcp_hdcp_apply(h, a == DCP_HDCP_TEARDOWN ? a : b);
}

static void dcp_hdcp_sync(struct dcp_hdcp *h)
{
	mutex_lock(&h->lock);
	dcp_hdcp_sync_locked(h);
	mutex_unlock(&h->lock);
}

static void dcp_hdcp_prop_work(struct work_struct *work)
{
	struct dcp_hdcp *h = container_of(work, struct dcp_hdcp, prop_work);
	struct apple_connector *connector = READ_ONCE(h->dcp->connector);
	struct drm_device *drm;
	u64 value;

	if (!connector || !READ_ONCE(connector->hdcp_attached))
		return;
	mutex_lock(&h->lock);
	value = dcp_hdcp_property(&h->link);
	mutex_unlock(&h->lock);
	/* UNDESIRED is userspace's to set, never the driver's. */
	if (value == DRM_MODE_CONTENT_PROTECTION_UNDESIRED ||
	    !READ_ONCE(connector->hdcp_want))
		return;
	drm = connector->base.dev;
	drm_modeset_lock(&drm->mode_config.connection_mutex, NULL);
	if (READ_ONCE(connector->hdcp_want))
		drm_hdcp_update_content_protection(&connector->base, value);
	drm_modeset_unlock(&drm->mode_config.connection_mutex);
}

/*
 * The transmitter's capability, once per HDCP interface after it opens (as
 * macOS does at boot): DCP then offers sessions at each link-up.
 */
static void dcp_hdcp_caps_work(struct work_struct *work)
{
	struct dcp_hdcp *h = container_of(work, struct dcp_hdcp, caps_work);
	struct apple_epic_service *service;
	int i;

	for (i = 0; i < DCP_HDCP_IFACES; i++) {
		service = dcp_hdcp_pin(h, &h->iface[i]);
		if (!service)
			continue;
		dcp_hdcp_plain_call(service, DCP_HDCP_IF_GROUP, DCP_HDCP_IF_CAPS,
				    DCP_HDCP_IF_CAPS_LEN, DCP_HDCP_IF_CAPS_VALUE);
		afk_service_put(service);
	}
}

/* Tells the DP device service the session it was asked for has ended. */
static void dcp_hdcp_dev_end(struct dcp_hdcp *h)
{
	struct apple_epic_service *relay;

	if (!xchg(&h->dev_begun, false))
		return;
	relay = dcp_hdcp_pin(h, &h->relay);
	if (!relay)
		return;
	dcp_hdcp_plain_call(relay, DCP_HDCP_DEV_GROUP, DCP_HDCP_DEV_END,
			    DCP_HDCP_DEV_END_LEN, 0);
	afk_service_put(relay);
}

/* Encryption off, the SEP session closed, DCP told; the picture stays. */
static void dcp_hdcp_teardown_work(struct work_struct *work)
{
	struct dcp_hdcp *h = container_of(work, struct dcp_hdcp, teardown_work);
	unsigned long flags;
	bool was_open, stale;

	/*
	 * A session DCP opened after this teardown was asked for has already
	 * replaced what it would end (dcp_hdcp_run() aborts a leftover first).
	 */
	spin_lock_irqsave(&h->svc_lock, flags);
	stale = h->open_seq != h->td_seq;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	if (stale)
		return;
	was_open = h->tx.sep_open;
	dcp_hdcp_abort(&h->tx);
	if (was_open)
		dcp_hdcp_report(h, DCP_HDCP_PROGRESS_IDLE, false, false, false);
	dcp_hdcp_dev_end(h);
}

/* One authentication, from DCP's start of the session to its result. */
static void dcp_hdcp_run(struct dcp_hdcp *h)
{
	struct apple_epic_service *relay = dcp_hdcp_pin(h, &h->relay);
	enum dcp_hdcp_action action;
	u32 gen;
	int ret;

	mutex_lock(&h->lock);
	gen = h->session_gen;
	h->tx.type = h->link.type;
	/* Only a session the policy took, on its generation. */
	if (h->link.state != DCP_HDCP_AUTH || h->link.generation != gen) {
		mutex_unlock(&h->lock);
		afk_service_put(relay);
		return;
	}
	mutex_unlock(&h->lock);
	/* What an earlier session left open (its teardown has not run yet). */
	if (h->tx.sep_open)
		dcp_hdcp_abort(&h->tx);
	h->tx.ops = &dcp_hdcp_tx_ops;
	h->tx.ctx = h;
	h->tx.channel = h->channel;

	if (relay) {
		/* The device service's query opens each session; dcp_hdcp_dev_end() closes it. */
		dcp_hdcp_dev_end(h);
		if (!dcp_hdcp_plain_call(relay, DCP_HDCP_DEV_GROUP, DCP_HDCP_DEV_QUERY,
					 DCP_HDCP_DEV_QUERY_LEN, 0))
			WRITE_ONCE(h->dev_begun, true);
		afk_service_put(relay);
		ret = dcp_hdcp_authenticate(&h->tx);
	} else {
		ret = -ENODEV;
	}
	if (ret) {
		dev_info(h->dcp->dev, "HDCP: authentication failed at %s: %d\n",
			 h->tx.step ?: "start", ret);
		dcp_hdcp_abort(&h->tx);
	} else {
		dev_info(h->dcp->dev, "HDCP: encrypted, type %u%s\n", h->tx.type,
			 h->tx.repeater ? " (repeater)" : "");
	}
	mutex_lock(&h->lock);
	action = dcp_hdcp_auth_done(&h->link, gen, ret, jiffies);
	mutex_unlock(&h->lock);
	/* The SEP session is already closed after a failure. */
	if (action != DCP_HDCP_TEARDOWN)
		dcp_hdcp_apply(h, action);
	else
		queue_work(h->wq, &h->prop_work);
}

static void dcp_hdcp_close(struct dcp_hdcp *h, struct apple_epic_service *service)
{
	enum dcp_hdcp_action action;
	unsigned long flags;
	bool ours;

	/* A close of an earlier session, after DCP opened a newer one: nothing to end. */
	spin_lock_irqsave(&h->svc_lock, flags);
	ours = !h->session || h->session == service;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	if (!ours)
		return;
	dcp_hdcp_abort(&h->tx);
	mutex_lock(&h->lock);
	action = dcp_hdcp_closed(&h->link, h->session_gen, jiffies);
	mutex_unlock(&h->lock);
	if (action != DCP_HDCP_TEARDOWN)
		dcp_hdcp_apply(h, action);
	/* The session goes back to idle on DCP's side. */
	if (service)
		dcp_hdcp_report(h, DCP_HDCP_PROGRESS_IDLE, false, false, false);
	dcp_hdcp_dev_end(h);
	spin_lock_irqsave(&h->svc_lock, flags);
	if (h->session == service)
		h->session = NULL;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	queue_work(h->wq, &h->prop_work);
}

static void dcp_hdcp_event_work(struct work_struct *work)
{
	struct dcp_hdcp *h = container_of(work, struct dcp_hdcp, event_work);
	struct dcp_hdcp_event ev;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&h->svc_lock, flags);
		if (h->q_head == h->q_tail) {
			spin_unlock_irqrestore(&h->svc_lock, flags);
			return;
		}
		ev = h->queue[h->q_tail++ % DCP_HDCP_QUEUE];
		spin_unlock_irqrestore(&h->svc_lock, flags);

		switch (ev.op) {
		case DCP_HDCP_AUTH_OPEN:
			dcp_hdcp_report(h, DCP_HDCP_PROGRESS_IDLE, false, false,
					false);
			break;
		case DCP_HDCP_AUTH_START:
			dcp_hdcp_report(h, DCP_HDCP_PROGRESS_STARTED, false,
					false, false);
			break;
		case DCP_HDCP_AUTH_CONFIG:
			dcp_hdcp_run(h);
			break;
		case DCP_HDCP_AUTH_CLOSE:
			dcp_hdcp_close(h, ev.service);
			break;
		}
		afk_service_put(ev.service);
	}
}

static void dcp_hdcp_queue(struct dcp_hdcp *h, struct apple_epic_service *service,
			   u8 op)
{
	struct apple_epic_service *pinned = afk_service_get(service);
	unsigned long flags;
	bool full;

	if (!pinned)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	full = h->q_head - h->q_tail >= DCP_HDCP_QUEUE;
	if (!full)
		h->queue[h->q_head++ % DCP_HDCP_QUEUE] =
			(struct dcp_hdcp_event){ op, pinned };
	spin_unlock_irqrestore(&h->svc_lock, flags);
	if (full) {
		dev_warn_ratelimited(h->dcp->dev, "HDCP: session event %#x dropped\n", op);
		afk_service_put(pinned);
		return;
	}
	queue_work(h->wq, &h->event_work);
}

/* Whether DCP announced @service as an HDCP 1.x session. */
static bool dcp_hdcp_is_hdcp1(struct dcp_hdcp *h, struct apple_epic_service *service)
{
	unsigned long flags;
	bool ret = false;
	int i;

	spin_lock_irqsave(&h->svc_lock, flags);
	for (i = 0; i < DCP_HDCP_IFACES; i++)
		ret |= h->hdcp1[i] == service;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	return ret;
}

/*
 * DCP announces one session per protocol version; the provider class says
 * which, where the announcement carries one. A session without a class is
 * taken for HDCP 2.2: the receiver registers it reads decide.
 */
static void dcp_hdcp_session_init(struct apple_epic_service *service,
				  const char *name, const char *class, s64 unit)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	int i;

	if (!h || !class || !strstr(class, "HDCP1"))
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	for (i = 0; i < DCP_HDCP_IFACES; i++) {
		if (h->hdcp1[i] == service)
			break;
		if (!h->hdcp1[i]) {
			h->hdcp1[i] = service;
			break;
		}
	}
	spin_unlock_irqrestore(&h->svc_lock, flags);
	dev_dbg(h->dcp->dev, "HDCP: HDCP 1.x session announced, declined\n");
}

static int dcp_hdcp_session_call(struct apple_epic_service *service, u16 group,
				 u32 command, const void *data, size_t size,
				 void *reply, size_t room)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	bool accept = false, ours;
	int ret;

	if (h && !group && command == DCP_HDCP_AUTH_OPEN) {
		mutex_lock(&h->lock);
		dcp_hdcp_sync_locked(h);
		accept = !dcp_hdcp_is_hdcp1(h, service) &&
			 dcp_hdcp_open(&h->link, h->gen, jiffies);
		if (accept)
			h->session_gen = h->gen;
		mutex_unlock(&h->lock);
		spin_lock_irqsave(&h->svc_lock, flags);
		if (accept) {
			h->session = service;
			h->ev_bits = 0;
			h->closed = false;
			h->cancel = false;
			h->open_seq++;
		} else if (h->session == service) {
			/* The slot of an earlier session, declined this time. */
			h->session = NULL;
		}
		spin_unlock_irqrestore(&h->svc_lock, flags);
	}
	ret = dcp_hdcp_auth_reply(group, command, data, size, reply, room,
				  accept);
	if (ret == -EOPNOTSUPP && h && !group) {
		/* Not a session step: answered and logged. */
		memset(reply, 0, room);
		memcpy(reply, data, min(size, room));
		dev_dbg(h->dcp->dev, "HDCP: session call %#x\n", command);
		return 0;
	}
	if (ret || !h)
		return ret;

	spin_lock_irqsave(&h->svc_lock, flags);
	ours = h->session == service;
	switch (command) {
	case DCP_HDCP_AUTH_H_PRIME:
	case DCP_HDCP_AUTH_PAIRING:
	case DCP_HDCP_AUTH_LIST:
		if (ours)
			__set_bit(command, &h->ev_bits);
		break;
	case DCP_HDCP_AUTH_CLOSE:
		if (ours)
			h->closed = true;
		break;
	}
	spin_unlock_irqrestore(&h->svc_lock, flags);
	wake_up_all(&h->ev_wq);

	if (ours && (command == DCP_HDCP_AUTH_OPEN || command == DCP_HDCP_AUTH_START ||
		     command == DCP_HDCP_AUTH_CONFIG || command == DCP_HDCP_AUTH_CLOSE))
		dcp_hdcp_queue(h, service, command);
	return 0;
}

static void dcp_hdcp_session_teardown(struct apple_epic_service *service)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	bool ours;
	int i;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	for (i = 0; i < DCP_HDCP_IFACES; i++)
		if (h->hdcp1[i] == service)
			h->hdcp1[i] = NULL;
	ours = h->session == service;
	if (ours) {
		h->closed = true;
		h->session = NULL;
	}
	spin_unlock_irqrestore(&h->svc_lock, flags);
	if (ours) {
		wake_up_all(&h->ev_wq);
		dcp_hdcp_apply(h, DCP_HDCP_TEARDOWN);
	}
}

static void dcp_hdcp_relay_work(struct work_struct *work)
{
	struct dcp_hdcp *h = container_of(work, struct dcp_hdcp, relay_work);
	struct apple_epic_service *service = dcp_hdcp_pin(h, &h->relay);

	if (!service)
		return;
	/* The device service's information call, once after it is opened. */
	dcp_hdcp_plain_call(service, DCP_HDCP_DEV_GROUP, DCP_HDCP_DEV_INFO,
			    DCP_HDCP_DEV_INFO_LEN, 0);
	afk_service_put(service);
	dcp_hdcp_sync(h);
}

static void dcp_hdcp_relay_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	h->relay = service;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	dev_dbg(h->dcp->dev, "HDCP: DP device service on interface %u\n",
		service->channel);
	queue_work(h->wq, &h->relay_work);
}

static void dcp_hdcp_relay_teardown(struct apple_epic_service *service)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	bool ours;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	ours = h->relay == service;
	if (ours)
		h->relay = NULL;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	if (ours)
		dcp_hdcp_apply(h, DCP_HDCP_TEARDOWN);
}

static void dcp_hdcp_iface_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	int i;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	for (i = 0; i < DCP_HDCP_IFACES; i++) {
		if (h->iface[i] == service)
			break;
		if (!h->iface[i]) {
			h->iface[i] = service;
			break;
		}
	}
	spin_unlock_irqrestore(&h->svc_lock, flags);
	queue_work(h->wq, &h->caps_work);
}

static void dcp_hdcp_iface_teardown(struct apple_epic_service *service)
{
	struct dcp_hdcp *h = dcp_hdcp_of(service);
	unsigned long flags;
	int i;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	for (i = 0; i < DCP_HDCP_IFACES; i++)
		if (h->iface[i] == service)
			h->iface[i] = NULL;
	spin_unlock_irqrestore(&h->svc_lock, flags);
}

static void dcp_hdcp_other_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
}

#define DCP_HDCP_RELAY_OPS {						\
	.name = "dcpdp-device-epic",					\
	.compact_full = "dcpdp-device-epic:0",				\
	.singleton = true,						\
	.init = dcp_hdcp_relay_init,					\
	.teardown = dcp_hdcp_relay_teardown,				\
}

static const struct apple_epic_service_ops dcp_hdcp_service_ops[] = {
	DCP_HDCP_RELAY_OPS,
	{
		.name = "dcpdptx-hdcp-interface",
		.init = dcp_hdcp_iface_init,
		.teardown = dcp_hdcp_iface_teardown,
	},
	{
		.name = "dcpdptx-hdcp-auth-session",
		.compact_full = "dcpdptx-hdcp-auth-session",
		/*
		 * Not a singleton: DCP announces one session per protocol
		 * version and closes each before it powers down, so every
		 * one needs its handler to answer.
		 */
		.init = dcp_hdcp_session_init,
		.call_v2 = dcp_hdcp_session_call,
		.teardown = dcp_hdcp_session_teardown,
	},
	{},
};

/* The DP device endpoint when this file starts it: the sink's services. */
static const struct apple_epic_service_ops dcp_hdcp_dpdev_ops[] = {
	DCP_HDCP_RELAY_OPS,
	{ .name = "*", .init = dcp_hdcp_other_init },
	{},
};

const struct apple_epic_service_ops *
dcp_hdcp_match_service(struct apple_dcp_afkep *ep, const char *service_name,
		       bool cut)
{
	const struct apple_epic_service_ops *ops;
	size_t len;

	if (!ep || !READ_ONCE(ep->dcp->hdcp) || !service_name)
		return NULL;
	for (ops = dcp_hdcp_service_ops; ops->name[0]; ops++) {
		len = strlen(ops->name);
		if (!strncmp(ops->name, service_name, len) &&
		    (!service_name[len] || service_name[len] == ':'))
			return ops;
		if (cut && afk_compact_cut_of(ops, service_name))
			return ops;
	}
	return NULL;
}

/*
 * The endpoints that carry the HDCP interface services, by firmware: the
 * first one advertised and not driven by anything else is started here.
 */
static const u8 dcp_hdcp_if_endpoints[] = { AV_AUDIO_ENDPOINT_DESKTOP, DPTX_HDCP_ENDPOINT };

static struct apple_dcp_afkep *dcp_hdcp_start_one(struct apple_dcp *dcp, u8 endpoint,
						  const struct apple_epic_service_ops *ops,
						  struct apple_dcp_afkep **slot, const char *what)
{
	struct apple_dcp_afkep *ep;
	int ret;

	if (!apple_rtkit_has_endpoint(dcp->rtk, endpoint) || dcp_endpoint_routed(dcp, endpoint))
		return NULL;
	ep = afk_init(dcp, endpoint, ops);
	if (IS_ERR(ep)) {
		dev_warn(dcp->dev, "HDCP: %s endpoint %#x not set up: %ld\n", what,
			 endpoint, PTR_ERR(ep));
		return NULL;
	}
	ep->debugfs_entry = dcp->ep_debugfs[endpoint - 0x20];
	/* Routed before the start: afk_start() waits for the firmware's answer. */
	WRITE_ONCE(*slot, ep);
	ret = afk_start(ep);
	if (ret)
		dev_warn(dcp->dev, "HDCP: %s endpoint %#x not started: %d\n", what,
			 endpoint, ret);
	else
		dev_info(dcp->dev, "HDCP: started the %s endpoint %#x\n", what, endpoint);
	return ep;
}

void dcp_hdcp_start_endpoints(struct apple_dcp *dcp)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);
	int i;

	if (!h || IS_ERR_OR_NULL(dcp->rtk))
		return;
	if (!h->dpdev_ep)
		dcp_hdcp_start_one(dcp, DPDEV_ENDPOINT, dcp_hdcp_dpdev_ops,
				   &h->dpdev_ep, "DP device");
	for (i = 0; i < ARRAY_SIZE(dcp_hdcp_if_endpoints) && !h->if_ep; i++)
		dcp_hdcp_start_one(dcp, dcp_hdcp_if_endpoints[i], dcp_hdcp_service_ops,
				   &h->if_ep, "HDCP interface");
}

struct apple_dcp_afkep *dcp_hdcp_route(struct apple_dcp *dcp, u32 endpoint)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);
	struct apple_dcp_afkep *ep;

	if (!h)
		return NULL;
	ep = READ_ONCE(h->dpdev_ep);
	if (ep && ep->endpoint == endpoint)
		return ep;
	ep = READ_ONCE(h->if_ep);
	return ep && ep->endpoint == endpoint ? ep : NULL;
}

void dcp_hdcp_link_changed(struct apple_dcp *dcp)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);
	enum dcp_hdcp_action action;

	if (!h)
		return;
	mutex_lock(&h->lock);
	h->gen++;
	action = dcp_hdcp_link_event(&h->link, h->gen, jiffies);
	mutex_unlock(&h->lock);
	if (action == DCP_HDCP_TEARDOWN)
		dcp_hdcp_apply(h, action);
	dcp_hdcp_sync(h);
}

static bool dcp_hdcp_channel(struct apple_dcp *dcp, u32 *channel)
{
	return !of_property_read_u32_index(dcp->dev->of_node,
					   "apple,hdcp-channels", 0, channel);
}

static void dcp_hdcp_destroy_wq(void *data)
{
	destroy_workqueue(data);
}

struct dcp_hdcp *dcp_hdcp_init(struct apple_dcp *dcp)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);
	u32 channel;

	if (h || dcp_has_panel(dcp) || !dcp_hdcp_channel(dcp, &channel))
		return h;
	h = devm_kzalloc(dcp->dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return NULL;
	h->dcp = dcp;
	h->channel = channel;
	mutex_init(&h->lock);
	spin_lock_init(&h->svc_lock);
	init_waitqueue_head(&h->ev_wq);
	INIT_WORK(&h->event_work, dcp_hdcp_event_work);
	INIT_WORK(&h->teardown_work, dcp_hdcp_teardown_work);
	INIT_WORK(&h->prop_work, dcp_hdcp_prop_work);
	INIT_WORK(&h->relay_work, dcp_hdcp_relay_work);
	INIT_WORK(&h->caps_work, dcp_hdcp_caps_work);
	/* Not freezable: a suspend's teardown must reach DCP before it sleeps. */
	h->wq = alloc_ordered_workqueue("apple-dcp-hdcp", 0);
	if (!h->wq)
		return NULL;
	if (devm_add_action_or_reset(dcp->dev, dcp_hdcp_destroy_wq, h->wq))
		return NULL;
	h->tx.ops = &dcp_hdcp_tx_ops;
	h->tx.ctx = h;
	h->tx.channel = channel;
	WRITE_ONCE(dcp->hdcp, h);
	dev_info(dcp->dev, "HDCP: transmitter on SEP channel %u\n", channel);
	return h;
}

/*
 * Component unbind: the state is device-managed and goes with the bind, so
 * nothing may reach it afterwards. Stop all work, end what is open, shut the
 * endpoints this file started, and unpublish it.
 */
void dcp_hdcp_fini(struct apple_dcp *dcp)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);
	struct apple_dcp_afkep *ep;
	unsigned long flags;

	if (!h)
		return;
	spin_lock_irqsave(&h->svc_lock, flags);
	h->cancel = true;
	spin_unlock_irqrestore(&h->svc_lock, flags);
	wake_up_all(&h->ev_wq);
	disable_work_sync(&h->event_work);
	disable_work_sync(&h->teardown_work);
	disable_work_sync(&h->prop_work);
	disable_work_sync(&h->relay_work);
	disable_work_sync(&h->caps_work);
	dcp_hdcp_abort(&h->tx);
	dcp_hdcp_dev_end(h);
	ep = xchg(&h->if_ep, NULL);
	if (ep)
		afk_shutdown(ep);
	ep = xchg(&h->dpdev_ep, NULL);
	if (ep)
		afk_shutdown(ep);
	WRITE_ONCE(dcp->hdcp, NULL);
}

/*
 * Before DCP sleeps or powers a pipe down: the link change, then its
 * teardown done here, so encryption is off and DCP has the device service's
 * end call before it waits for it.
 */
void dcp_hdcp_quiesce(struct apple_dcp *dcp)
{
	struct dcp_hdcp *h = READ_ONCE(dcp->hdcp);

	dcp_hdcp_link_changed(dcp);
	if (h)
		flush_work(&h->teardown_work);
}

/* DRM. */

int dcp_hdcp_attach_connector(struct apple_connector *connector,
			      struct platform_device **dcps, unsigned int n,
			      int connector_type)
{
	bool channel = false;
	unsigned int i;
	int ret;

	if (connector->hdcp_attached ||
	    connector_type == DRM_MODE_CONNECTOR_eDP)
		return 0;
	for (i = 0; i < n; i++) {
		struct apple_dcp *dcp = dcps[i] ? platform_get_drvdata(dcps[i]) : NULL;

		if (dcp && dcp_hdcp_init(dcp))
			channel = true;
	}
	if (!dcp_hdcp_attach_property(true, channel,
				      IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE)))
		return 0;
	ret = drm_connector_attach_content_protection_property(&connector->base,
							       true);
	if (ret)
		return ret;
	connector->hdcp_attached = true;
	return 0;
}

void dcp_hdcp_atomic_check(struct drm_connector *conn,
			   struct drm_atomic_commit *state)
{
	struct drm_connector_state *old = drm_atomic_get_old_connector_state(state, conn);
	struct drm_connector_state *new = drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *crtc_state;

	if (!to_apple_connector(conn)->hdcp_attached || !old || !new)
		return;
	/* A connector switched off keeps asking: DESIRED again when it is back. */
	if (!new->crtc) {
		if (old->content_protection == DRM_MODE_CONTENT_PROTECTION_ENABLED)
			new->content_protection = DRM_MODE_CONTENT_PROTECTION_DESIRED;
		return;
	}
	crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	if (old->content_protection != DRM_MODE_CONTENT_PROTECTION_ENABLED ||
	    new->content_protection == DRM_MODE_CONTENT_PROTECTION_UNDESIRED)
		return;
	/* A mode set or another type: protected again only after a new session. */
	if ((crtc_state && drm_atomic_crtc_needs_modeset(crtc_state)) ||
	    old->hdcp_content_type != new->hdcp_content_type)
		new->content_protection = DRM_MODE_CONTENT_PROTECTION_DESIRED;
}

void dcp_hdcp_commit(struct drm_atomic_commit *state)
{
	struct drm_connector_state *new;
	struct drm_connector *conn;
	int i;

	for_each_new_connector_in_state(state, conn, new, i) {
		struct apple_connector *connector = to_apple_connector(conn);
		struct platform_device *pdev = READ_ONCE(connector->dcp);
		struct apple_dcp *dcp;

		if (!connector->hdcp_attached)
			continue;
		WRITE_ONCE(connector->hdcp_type, new->hdcp_content_type);
		WRITE_ONCE(connector->hdcp_want, new->content_protection !=
				DRM_MODE_CONTENT_PROTECTION_UNDESIRED);
		dcp = pdev ? platform_get_drvdata(pdev) : NULL;
		if (dcp && READ_ONCE(dcp->hdcp))
			dcp_hdcp_sync(dcp->hdcp);
	}
}

#if IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST)
#include "dcp-hdcp-glue-test.c"
#endif
