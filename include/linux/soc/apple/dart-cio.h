/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_APPLE_DART_CIO_H
#define __LINUX_SOC_APPLE_DART_CIO_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device;
struct apple_dart_cio;

/*
 * Bind once, while the host is probing. The synchronous host PM transaction
 * calls prepare before suspend callbacks, off after every requester is stopped
 * but before clocks go away, on after clocks return but before DMA, and finish
 * after recovery. on also unwinds a partial off. While the DART is closed, page
 * tables are updated in memory only. finish always ends the transaction; a DART
 * that was not restored stays closed, its registers unused, until reboot.
 * check asks, with reads only and before off, whether off could save the DART:
 * prepare makes the checks that hold with requesters running, and an owner asks
 * again with them stopped before a step it cannot take back.
 */
#if IS_REACHABLE(CONFIG_APPLE_DART)
int apple_dart_cio_bind(struct device *owner, struct device *provider,
			struct apple_dart_cio **state);
int apple_dart_cio_prepare(struct apple_dart_cio *state);
/*
 * Runtime variant of apple_dart_cio_prepare(): drops the PM_SUSPEND_MEM check
 * and leaves a successful transaction active until apple_dart_cio_finish().
 * Callers must serialize the transaction against their device's suspend and
 * removal paths and finish it on every exit.
 */
int apple_dart_cio_prepare_runtime(struct apple_dart_cio *state);
int apple_dart_cio_check(struct apple_dart_cio *state, bool quiesced);
int apple_dart_cio_off(struct apple_dart_cio *state);
int apple_dart_cio_on(struct apple_dart_cio *state);
int apple_dart_cio_finish(struct apple_dart_cio *state);
#else
static inline int apple_dart_cio_bind(struct device *owner, struct device *provider,
				      struct apple_dart_cio **state)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_prepare(struct apple_dart_cio *s)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_prepare_runtime(struct apple_dart_cio *s)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_check(struct apple_dart_cio *s, bool quiesced)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_off(struct apple_dart_cio *s)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_on(struct apple_dart_cio *s)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_cio_finish(struct apple_dart_cio *s)
{
	return -EOPNOTSUPP;
}

#endif
#endif
