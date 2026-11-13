// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The keyboard side of the enclave's trusted-accessory client (see
 * stac_client.rs): registers with hid-apple-ta as its one client and reads
 * what identifies a Magic Keyboard with Touch ID.
 *
 * Read-only: the only requests sent to the keyboard are three feature-report
 * reads (protocol tag, identity, connection nonce). Their bodies go to the
 * Rust side and are never logged. Input reports are not expected while
 * nothing is asked of the keyboard; any that arrive are counted and dropped.
 *
 * Copyright 2026 Ryan Murray
 */

#include <linux/atomic.h>
#include <linux/hid.h>
#include <linux/hid-apple-ta.h>
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
	struct work_struct identify;
	atomic_t dropped;
	bool registered;
} sep_ta = {
	.lock = __MUTEX_INITIALIZER(sep_ta.lock),
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
	atomic_inc(&sep_ta.dropped);
}

static const struct apple_ta_client_ops sep_ta_ops = {
	.attach = sep_ta_attach,
	.detach = sep_ta_detach,
	.report = sep_ta_report,
};

int sep_stac_ta_register(void *ctx, sep_stac_ta_event_fn attached,
			 sep_stac_ta_event_fn detached,
			 sep_stac_ta_identified_fn identified,
			 sep_stac_ta_failed_fn failed)
{
	int ret;

	if (sep_ta.registered)
		return -EBUSY;
	INIT_WORK(&sep_ta.identify, sep_ta_identify_work);
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
			 sep_stac_ta_failed_fn failed)
{
	return -EOPNOTSUPP;
}

void sep_stac_ta_unregister(void)
{
}

void sep_stac_ta_identify(void)
{
}

#endif
