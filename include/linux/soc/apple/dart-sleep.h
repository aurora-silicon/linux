/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_DART_SLEEP_H
#define _LINUX_SOC_APPLE_DART_SLEEP_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct task_struct;

/* Caller-owned record. Private identity is checked before every operation. */
struct apple_dart_sleep {
	void *private;
	struct device *provider, *client;
	struct task_struct *owner;
	u64 generation;
	int result;
	bool active, sealed, verified;
};

/*
 * Bind at host probe, adding a host->DART supplier link. begin/finish execute
 * in the host's synchronous system-PM task. Seal follows child noirq callbacks,
 * with bus mastering held and no pending transaction. The same task stops the
 * streams while the port clock is still on and restores them after power loss,
 * before the PCI core restores endpoint state. Abort reopens only unchanged
 * state; failed recovery keeps DMA and mapping writers closed.
 */
#if IS_ENABLED(CONFIG_APPLE_DART)
int apple_dart_sleep_bind(struct device *backend, struct device *client,
			  struct apple_dart_sleep *sleep);
int apple_dart_sleep_begin(struct apple_dart_sleep *sleep, u64 generation);
int apple_dart_sleep_seal(struct apple_dart_sleep *sleep);
int apple_dart_sleep_abort(struct apple_dart_sleep *sleep);
int apple_dart_sleep_finish(struct apple_dart_sleep *sleep);
/* Native radio: readiness is MMIO-free; off precedes port clock removal.
 * Restore is process context after parent/port clocks, with endpoint DMA held.
 */
int apple_dart_sleep_power_ready(struct apple_dart_sleep *sleep);
int apple_dart_sleep_power_off(struct apple_dart_sleep *sleep);
int apple_dart_sleep_power_restore(struct apple_dart_sleep *sleep);
#else
static inline int apple_dart_sleep_bind(struct device *backend, struct device *client,
					struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_begin(struct apple_dart_sleep *sleep, u64 generation)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_seal(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_abort(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_finish(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_power_ready(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_power_off(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_sleep_power_restore(struct apple_dart_sleep *sleep)
{
	return -EOPNOTSUPP;
}
#endif
#endif
