// SPDX-License-Identifier: GPL-2.0-only
/*
 * GL9755 board power sequencing core. No hardware adapter is enabled until an
 * always-on insertion wake source and config-access exclusion are available.
 *
 * Card detection must never use the unidentified I2C4 client at address 0x20
 * (the board "sd-card" node): no probe, scan, quick command or read of it.
 * Insertion comes from the SMC helper interrupt or the GL9755 itself.
 */
#include <linux/errno.h>
#include <linux/lockdep.h>

#include "sdhci-pci-gli-pwrseq.h"

void gli_pwrseq_init(struct gli_pwrseq *seq,
		     const struct gli_pwrseq_ops *ops, void *ctx)
{
	*seq = (struct gli_pwrseq) {
		.ops = ops,
		.ctx = ctx,
		.state = GLI_OFF_NO_MEDIA,
	};
	mutex_init(&seq->lock);
}

static int gli_pwrseq_off(struct gli_pwrseq *seq)
{
	const struct gli_pwrseq_ops *ops = seq->ops;
	int ret;

	lockdep_assert_held(&seq->lock);
	seq->state = GLI_QUIESCING;
	if (seq->endpoint) {
		ret = ops->remove(seq->ctx);
		if (ret) {
			/* A new card or outstanding I/O vetoes power gating. */
			seq->state = GLI_ON_MEDIA;
			return ret;
		}
		seq->endpoint = false;
	}

	ops->config(seq->ctx, false);
	if (seq->port_on) {
		ret = ops->port(seq->ctx, false);
		if (ret)
			return ret;
		seq->port_on = false;
	}
	if (seq->rail_on) {
		ret = ops->rail(seq->ctx, false);
		if (ret)
			return ret;
		seq->rail_on = false;
	}
	ops->delay(seq->ctx, GLI_PWRSEQ_OFF_MS);
	seq->state = GLI_OFF_NO_MEDIA;
	return 0;
}

static int gli_pwrseq_insert(struct gli_pwrseq *seq)
{
	const struct gli_pwrseq_ops *ops = seq->ops;
	int attempt, ret, cleanup;

	lockdep_assert_held(&seq->lock);
	for (attempt = 0; attempt < GLI_PWRSEQ_ATTEMPTS; attempt++) {
		seq->state = GLI_POWERING;
		/* Track possibly partial writes, including failed enables. */
		seq->rail_on = true;
		ret = ops->rail(seq->ctx, true);
		if (ret)
			goto failed;
		ops->delay(seq->ctx, GLI_PWRSEQ_ON_MS);
		seq->port_on = true;
		ret = ops->port(seq->ctx, true);
		if (ret)
			goto failed;
		seq->state = GLI_ENUMERATING;
		ops->config(seq->ctx, true);
		seq->endpoint = true;
		ret = ops->rescan(seq->ctx);
		if (ret)
			goto failed;
		ret = ops->media(seq->ctx);
		if (ret < 0) {
			/* Unknown presence cannot authorize dropping a card's power. */
			seq->state = GLI_ON_MEDIA;
			return ret;
		}
		if (!ret)
			return gli_pwrseq_off(seq);
		seq->state = GLI_ON_MEDIA;
		/* Speed is optional; never power-cycle media to obtain Gen2. */
		ops->gen2(seq->ctx);
		return 0;
failed:
		cleanup = gli_pwrseq_off(seq);
		if (cleanup)
			return cleanup;
	}
	return ret;
}

int gli_pwrseq_event(struct gli_pwrseq *seq, enum gli_pwrseq_event event)
{
	int ret = 0;

	/* This entry point is for threaded IRQ/workqueue and PM callbacks. */
	seq->ops->bus_lock(seq->ctx);
	mutex_lock(&seq->lock);

	switch (event) {
	case GLI_SUSPEND:
		if (seq->state == GLI_QUIESCING) {
			ret = -EBUSY;
			break;
		}
		seq->suspended = true;
		break;
	case GLI_RESUME:
		seq->suspended = false;
		if (!seq->pending_insert)
			break;
		/* Service only a helper event deferred across suspend. */
		seq->pending_insert = false;
		fallthrough;
	case GLI_HELPER_INSERT:
		if (seq->suspended) {
			seq->pending_insert = true;
			break;
		}
		if (seq->state == GLI_QUIESCING) {
			seq->pending_insert = true;
			break;
		}
		if (seq->state == GLI_OFF_NO_MEDIA)
			ret = gli_pwrseq_insert(seq);
		break;
	case GLI_MEDIA_CHANGE:
		if (seq->suspended) {
			ret = -EBUSY;
			break;
		}
		if (seq->state == GLI_QUIESCING) {
			/* Retry failed shutdown without touching endpoint config. */
			ret = gli_pwrseq_off(seq);
			if (!ret && seq->pending_insert) {
				seq->pending_insert = false;
				ret = gli_pwrseq_insert(seq);
			}
			break;
		}
		if (seq->state != GLI_ON_MEDIA)
			break;
		ret = seq->ops->media(seq->ctx);
		if (!ret)
			ret = gli_pwrseq_off(seq);
		else if (ret > 0)
			ret = 0;
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&seq->lock);
	seq->ops->bus_unlock(seq->ctx);
	return ret;
}
