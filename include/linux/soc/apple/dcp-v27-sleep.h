/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCP_V27_SLEEP_H
#define _APPLE_DCP_V27_SLEEP_H
#include <linux/types.h>
#include <linux/kconfig.h>
#include <linux/errno.h>
struct device;
struct module;
struct apple_rtkit;
struct apple_rtkit_v27_scope;
struct apple_dart_v27_lease;

/* arm/disarm run in the SAME suspend task, outside device PM callbacks.
 * check/stop run saved-context, all other AP CPUs parked and IRQs disabled.
 * No callback after a successful stop may return to precommit cancellation.
 */
struct apple_sleep_v27_ops {
	int (*arm)(void *data);
	int (*check)(void *data);
	int (*stop)(void *data);
	int (*disarm)(void *data, bool stopped);
};
#if IS_ENABLED(CONFIG_APPLE_SOC_SLEEP)
bool apple_sleep_v27_late(void);
int apple_sleep_v27_register(struct device *, const struct apple_sleep_v27_ops *,
	void *data, struct module *owner, bool retain_frontend);
void apple_sleep_v27_unregister(struct device *);
void apple_sleep_v27_prepared(struct device *, bool);
#else
static inline bool apple_sleep_v27_late(void) { return false; }
static inline int apple_sleep_v27_register(struct device *dev,
	const struct apple_sleep_v27_ops *ops, void *data, struct module *owner,
	bool retain_frontend) { return -EOPNOTSUPP; }
static inline void apple_sleep_v27_unregister(struct device *dev) { }
static inline void apple_sleep_v27_prepared(struct device *dev, bool prepared) { }
#endif

int apple_rtkit_v27_arm(struct apple_rtkit *, struct apple_rtkit_v27_scope **);
bool apple_rtkit_v27_live(struct apple_rtkit *);
int apple_rtkit_v27_check(struct apple_rtkit_v27_scope *);
int apple_rtkit_v27_idle_atomic(struct apple_rtkit_v27_scope *);
int apple_rtkit_v27_disarm(struct apple_rtkit_v27_scope *, bool stopped);

int apple_dart_v27_claim(struct device *provider, struct device *firmware,
	struct device *scanout, bool display, bool retain,
	struct apple_dart_v27_lease **lease);
int apple_dart_v27_check(struct apple_dart_v27_lease *);
/* physical initiators stopped by the caller; never disables an unknown SID. */
int apple_dart_v27_stop_atomic(struct apple_dart_v27_lease *);
int apple_dart_v27_finish(struct apple_dart_v27_lease *, bool stopped);
#endif
