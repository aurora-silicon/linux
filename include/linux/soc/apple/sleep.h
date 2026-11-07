/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * What drivers tell the code that takes an Apple SoC into a firmware-driven
 * suspend.
 */
#ifndef _LINUX_SOC_APPLE_SLEEP_H_
#define _LINUX_SOC_APPLE_SLEEP_H_

#include <linux/compiler_types.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#if IS_ENABLED(CONFIG_APPLE_T8140_SLEEP)
/*
 * A coprocessor that can't be stopped until a sleep is certain, and can't be
 * restarted without one. While one is registered and not ready, a suspend is
 * refused. Once ready, the last step before the commit runs @last_stop, with
 * interrupts off and every refusal behind it, then clears its run bit. With
 * @pcie_ans, that also lowers the PCIe and ANS domains, which nothing else may do
 * under it. @last_stop can't refuse: a nonzero return means it failed, and the sleep
 * goes on.
 */
int apple_sleep_coproc_register(struct device *dev, void __iomem *asc, bool pcie_ans,
				int (*last_stop)(void *data), void *data);

/*
 * The same with no ASC to clear: @last_stop clears its own run bits and lowers its own power
 * domain, and runs before every other coprocessor's. The refusal checks leave that domain on.
 */
int apple_sleep_coproc_register_first(struct device *dev, int (*last_stop)(void *data),
				      void *data);
void apple_sleep_coproc_unregister(struct device *dev);

/*
 * For a registered coprocessor: the last of its stop that may still refuse the sleep, run
 * with interrupts off before any coprocessor's @last_stop. A negative return refuses, and the
 * coprocessor must still be able to carry on.
 */
int apple_sleep_coproc_set_last_check(struct device *dev, int (*last_check)(void *data));

/*
 * The enclave's sleep request, which it comes back from only through a real sleep: @tell
 * runs with interrupts off as the very last refusal, after every other one and before any
 * coprocessor stops. Nonzero refuses the sleep.
 */
int apple_sleep_enclave_register(struct device *dev, int (*tell)(void *data), void *data);
void apple_sleep_enclave_unregister(struct device *dev);

/* Its driver has quiesced it for that last step, or has taken it back. */
void apple_sleep_coproc_set_ready(struct device *dev, bool ready);

/* Whether the last step stopped it: after a refused sleep, false means it still runs. */
bool apple_sleep_coproc_stopped(struct device *dev);

/*
 * The USB host sleeps in place for this sleep, as macOS's does: its always-on domain stays
 * up through the commit. Its PHY sets it once the controller is in its hibernation state,
 * and clears it at the wake.
 */
void apple_sleep_usb_set_asleep(bool asleep);

/*
 * The fixed hub's ports with a device at or below them that can wake the system, bit n for
 * port n. The host that sleeps in place sets them before the freeze and clears them after.
 */
void apple_sleep_usb_set_wake_ports(unsigned long ports);

/* For the port controller wired to the fixed hub's @port, as it arms USB wake. */
bool apple_sleep_usb_port_can_wake(unsigned int port);

/* The last sleep ended the debug link on @dev, whose driver may rearm it. */
bool apple_sleep_debug_link_was_cut(struct device *dev);

/* The same, without taking it: the port controller's own setup after its cold reset. */
bool apple_sleep_debug_link_cut_peek(struct device *dev);

/*
 * End the debug link now, if this sleep hasn't: true if this call did. For the SMC's driver,
 * ahead of its AP idle notice. It takes over a second, and doesn't sleep.
 */
bool apple_sleep_debug_link_cut(void);

/* After a deep sleep, put back the pins it reset. ANS's driver calls it before the run bit. */
void apple_sleep_restore_pins(void);

/*
 * After a refused sleep, for a firmware that idled on a domain that can't gate alone: the
 * sleep engine's PMGR reset of its own domains, its DARTs kept. The caller starts it after.
 */
int apple_sleep_engine_reset(struct device *dev);
#else
static inline int apple_sleep_coproc_register(struct device *dev, void __iomem *asc,
					      bool pcie_ans, int (*last_stop)(void *data),
					      void *data)
{
	return -EOPNOTSUPP;
}

static inline int apple_sleep_coproc_register_first(struct device *dev,
						    int (*last_stop)(void *data), void *data)
{
	return -EOPNOTSUPP;
}

static inline void apple_sleep_coproc_unregister(struct device *dev)
{
}

static inline int apple_sleep_coproc_set_last_check(struct device *dev,
						    int (*last_check)(void *data))
{
	return -EOPNOTSUPP;
}

static inline int apple_sleep_enclave_register(struct device *dev, int (*tell)(void *data),
					       void *data)
{
	return -EOPNOTSUPP;
}

static inline void apple_sleep_enclave_unregister(struct device *dev)
{
}

static inline void apple_sleep_coproc_set_ready(struct device *dev, bool ready)
{
}

static inline bool apple_sleep_coproc_stopped(struct device *dev)
{
	return false;
}

static inline void apple_sleep_usb_set_asleep(bool asleep)
{
}

static inline void apple_sleep_usb_set_wake_ports(unsigned long ports)
{
}

static inline bool apple_sleep_usb_port_can_wake(unsigned int port)
{
	return false;
}

static inline bool apple_sleep_debug_link_was_cut(struct device *dev)
{
	return false;
}

static inline bool apple_sleep_debug_link_cut_peek(struct device *dev)
{
	return false;
}

static inline bool apple_sleep_debug_link_cut(void)
{
	return false;
}

static inline void apple_sleep_restore_pins(void)
{
}

static inline int apple_sleep_engine_reset(struct device *dev)
{
	return -EOPNOTSUPP;
}
#endif

#endif
