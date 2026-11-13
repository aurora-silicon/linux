// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The keyboard side of the enclave's trusted-accessory client (see
 * stac_client.rs): registers with hid-apple-ta as its one client and reads
 * what identifies a Magic Keyboard with Touch ID.
 *
 * On attach it only reads: three feature reports (protocol tag, identity,
 * connection nonce). Their bodies go to the Rust side and are never logged.
 *
 * Pairing writes to the keyboard and runs only when root asks for it through
 * the write-only ta_pair parameter (off unless written). The Rust side then
 * drives the exchange through sep_stac_ta_command() and sep_stac_ta_read(),
 * under the client lock, so the keyboard cannot go away in the middle. Input
 * reports arriving while no reply is awaited are counted and dropped.
 *
 * Copyright 2026 Ryan Murray
 */

#include <linux/atomic.h>
#include <linux/hid.h>
#include <linux/hid-apple-ta.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "shim.h"

/* Feature reports of the Touch ID collection, each answered with its ID first. */
#define SEP_TA_REPORT_TAG	0x4b	/* protocol tag and version */
#define SEP_TA_REPORT_UID	0x41	/* the accessory's identity */
#define SEP_TA_REPORT_NONCE	0x4c	/* the connection nonce */

#define SEP_TA_TAG		"TACP"
#define SEP_TA_TAG_LEN		8	/* the tag and a version word */
#define SEP_TA_VERSION		1
#define SEP_TA_UID_LEN		16
#define SEP_TA_NONCE_LEN	16

#if IS_REACHABLE(CONFIG_HID_APPLE_TA)

static struct {
	struct mutex lock;	/* ta, against the identify work */
	struct apple_ta_device *ta;
	void *ctx;
	sep_stac_ta_event_fn attached;
	sep_stac_ta_event_fn detached;
	sep_stac_ta_identified_fn identified;
	sep_stac_ta_failed_fn failed;
	sep_stac_ta_pair_fn pair;
	struct work_struct identify;
	atomic_t dropped;
	bool registered;
} sep_ta = {
	.lock = __MUTEX_INITIALIZER(sep_ta.lock),
};

/* The one reply awaited, if any. */
static struct {
	spinlock_t lock;	/* all of it, against the report callback */
	u8 want;		/* the reply's report ID; 0 while none is awaited */
	u8 *buf;
	size_t cap;
	ssize_t len;		/* bytes taken with the ID, or -EMSGSIZE */
	struct completion done;
} sep_ta_rx = {
	.lock = __SPIN_LOCK_UNLOCKED(sep_ta_rx.lock),
};

static int sep_ta_read(struct apple_ta_device *ta, u8 id, u8 *out, size_t len)
{
	u8 buf[1 + SEP_TA_UID_LEN];
	int ret;

	if (len + 1 > sizeof(buf))
		return -EINVAL;
	buf[0] = id;
	ret = apple_ta_get(ta, buf, len + 1);
	if (ret >= 0 && (ret != (int)(len + 1) || buf[0] != id))
		ret = -EPROTO;
	if (ret >= 0) {
		memcpy(out, buf + 1, len);
		ret = 0;
	}
	memzero_explicit(buf, sizeof(buf));
	return ret;
}

static void sep_ta_identify_work(struct work_struct *work)
{
	u8 tag[SEP_TA_TAG_LEN], uid[SEP_TA_UID_LEN], nonce[SEP_TA_NONCE_LEN];
	struct apple_ta_device *ta;
	int ret;

	mutex_lock(&sep_ta.lock);
	ta = sep_ta.ta;
	if (!ta)
		goto out;

	ret = sep_ta_read(ta, SEP_TA_REPORT_TAG, tag, sizeof(tag));
	if (!ret && (memcmp(tag, SEP_TA_TAG, 4) ||
		     get_unaligned_le32(tag + 4) != SEP_TA_VERSION))
		ret = -EPROTONOSUPPORT;
	if (!ret)
		ret = sep_ta_read(ta, SEP_TA_REPORT_UID, uid, sizeof(uid));
	if (!ret)
		ret = sep_ta_read(ta, SEP_TA_REPORT_NONCE, nonce, sizeof(nonce));

	/* Under the lock: a detach waits until the enclave has the answer. */
	if (ret)
		sep_ta.failed(sep_ta.ctx, ret);
	else
		sep_ta.identified(sep_ta.ctx, uid, nonce);
	memzero_explicit(uid, sizeof(uid));
	memzero_explicit(nonce, sizeof(nonce));
out:
	mutex_unlock(&sep_ta.lock);
}

static void sep_ta_attach(void *ctx, struct apple_ta_device *ta)
{
	mutex_lock(&sep_ta.lock);
	sep_ta.ta = ta;
	atomic_set(&sep_ta.dropped, 0);
	mutex_unlock(&sep_ta.lock);
	sep_ta.attached(ctx);
}

static void sep_ta_detach(void *ctx, struct apple_ta_device *ta)
{
	int dropped;

	mutex_lock(&sep_ta.lock);
	if (sep_ta.ta == ta)
		sep_ta.ta = NULL;
	mutex_unlock(&sep_ta.lock);
	dropped = atomic_xchg(&sep_ta.dropped, 0);
	if (dropped)
		pr_info("apple-sep: stac: %d keyboard reports dropped while attached\n",
			dropped);
	sep_ta.detached(ctx);
}

static void sep_ta_report(void *ctx, struct apple_ta_device *ta, u8 id,
			  const u8 *data, size_t len)
{
	unsigned long flags;
	bool taken = false;

	spin_lock_irqsave(&sep_ta_rx.lock, flags);
	if (sep_ta_rx.want && id == sep_ta_rx.want) {
		if (len + 1 > sep_ta_rx.cap) {
			sep_ta_rx.len = -EMSGSIZE;
		} else {
			sep_ta_rx.buf[0] = id;
			memcpy(sep_ta_rx.buf + 1, data, len);
			sep_ta_rx.len = len + 1;
		}
		sep_ta_rx.want = 0;
		complete(&sep_ta_rx.done);
		taken = true;
	}
	spin_unlock_irqrestore(&sep_ta_rx.lock, flags);
	if (!taken)
		atomic_inc(&sep_ta.dropped);
}

/*
 * Sends one feature report (@cmd[0] is its ID) and, unless @reply_id is 0,
 * waits up to @timeout_ms for the input report with that ID. Returns the
 * reply's length with its ID first, 0 when none was awaited, or an error.
 * Only from the pair callback, which holds sep_ta.lock.
 */
ssize_t sep_stac_ta_command(const u8 *cmd, size_t len, u8 reply_id, u8 *reply,
			    size_t cap, unsigned int timeout_ms)
{
	unsigned long flags;
	ssize_t ret;

	lockdep_assert_held(&sep_ta.lock);
	if (!sep_ta.ta)
		return -ENODEV;
	if (reply_id) {
		if (cap < 1)
			return -EINVAL;
		spin_lock_irqsave(&sep_ta_rx.lock, flags);
		reinit_completion(&sep_ta_rx.done);
		sep_ta_rx.buf = reply;
		sep_ta_rx.cap = cap;
		sep_ta_rx.len = 0;
		sep_ta_rx.want = reply_id;
		spin_unlock_irqrestore(&sep_ta_rx.lock, flags);
	}
	ret = apple_ta_send(sep_ta.ta, HID_FEATURE_REPORT, cmd, len);
	if (ret >= 0 && ret != len)
		ret = -EIO;
	if (ret >= 0 && reply_id &&
	    !wait_for_completion_timeout(&sep_ta_rx.done,
					 msecs_to_jiffies(timeout_ms)))
		ret = -ETIMEDOUT;

	if (reply_id) {
		/* No report may land in @reply after this. */
		spin_lock_irqsave(&sep_ta_rx.lock, flags);
		sep_ta_rx.want = 0;
		sep_ta_rx.buf = NULL;
		if (ret >= 0)
			ret = sep_ta_rx.len;
		spin_unlock_irqrestore(&sep_ta_rx.lock, flags);
	} else if (ret > 0) {
		ret = 0;
	}
	return ret;
}

/*
 * Reads one feature report into @buf (@len bytes, ID first). Returns the
 * length read. Only from the pair callback.
 */
ssize_t sep_stac_ta_read(u8 id, u8 *buf, size_t len)
{
	int ret;

	lockdep_assert_held(&sep_ta.lock);
	if (!sep_ta.ta)
		return -ENODEV;
	if (len < 1)
		return -EINVAL;
	buf[0] = id;
	ret = apple_ta_get(sep_ta.ta, buf, len);
	if (ret > 0 && buf[0] != id)
		ret = -EPROTO;
	return ret;
}

/*
 * Writing 1 to /sys/module/apple_sep/parameters/ta_pair pairs the attached
 * keyboard, if the enclave reports it unpaired. Nothing pairs otherwise.
 */
static int sep_ta_pair_set(const char *val, const struct kernel_param *kp)
{
	bool go;
	int ret;

	ret = kstrtobool(val, &go);
	if (ret || !go)
		return ret;
	if (!sep_ta.registered)
		return -ENODEV;
	ret = mutex_lock_interruptible(&sep_ta.lock);
	if (ret)
		return ret;
	ret = sep_ta.ta ? sep_ta.pair(sep_ta.ctx) : -ENODEV;
	mutex_unlock(&sep_ta.lock);
	return ret;
}

static const struct kernel_param_ops sep_ta_pair_ops = {
	.set = sep_ta_pair_set,
};
module_param_cb(ta_pair, &sep_ta_pair_ops, NULL, 0200);
MODULE_PARM_DESC(ta_pair, "Write 1 to pair the attached Magic Keyboard with Touch ID");

static const struct apple_ta_client_ops sep_ta_ops = {
	.attach = sep_ta_attach,
	.detach = sep_ta_detach,
	.report = sep_ta_report,
};

int sep_stac_ta_register(void *ctx, sep_stac_ta_event_fn attached,
			 sep_stac_ta_event_fn detached,
			 sep_stac_ta_identified_fn identified,
			 sep_stac_ta_failed_fn failed,
			 sep_stac_ta_pair_fn pair)
{
	int ret;

	if (sep_ta.registered)
		return -EBUSY;
	INIT_WORK(&sep_ta.identify, sep_ta_identify_work);
	init_completion(&sep_ta_rx.done);
	sep_ta.pair = pair;
	sep_ta.ctx = ctx;
	sep_ta.attached = attached;
	sep_ta.detached = detached;
	sep_ta.identified = identified;
	sep_ta.failed = failed;
	sep_ta.registered = true;
	ret = apple_ta_register_client(&sep_ta_ops, ctx);
	if (ret)
		sep_ta.registered = false;
	return ret;
}

void sep_stac_ta_unregister(void)
{
	if (!sep_ta.registered)
		return;
	/* Detaches every keyboard; the work then finds none. */
	apple_ta_unregister_client(&sep_ta_ops);
	cancel_work_sync(&sep_ta.identify);
	sep_ta.registered = false;
}

void sep_stac_ta_identify(void)
{
	if (sep_ta.registered)
		queue_work(system_freezable_wq, &sep_ta.identify);
}

#else /* !IS_REACHABLE(CONFIG_HID_APPLE_TA) */

int sep_stac_ta_register(void *ctx, sep_stac_ta_event_fn attached,
			 sep_stac_ta_event_fn detached,
			 sep_stac_ta_identified_fn identified,
			 sep_stac_ta_failed_fn failed,
			 sep_stac_ta_pair_fn pair)
{
	return -EOPNOTSUPP;
}

ssize_t sep_stac_ta_command(const u8 *cmd, size_t len, u8 reply_id, u8 *reply,
			    size_t cap, unsigned int timeout_ms)
{
	return -ENODEV;
}

ssize_t sep_stac_ta_read(u8 id, u8 *buf, size_t len)
{
	return -ENODEV;
}

void sep_stac_ta_unregister(void)
{
}

void sep_stac_ta_identify(void)
{
}

#endif
