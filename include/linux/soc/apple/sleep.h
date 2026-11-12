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
struct device_node;

#if IS_ENABLED(CONFIG_APPLE_SOC_SLEEP)
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

/* For a SoC sleep driver of its own (T6041): the coprocessor stop, with interrupts off. */
int apple_sleep_coprocs_stop_all(void);
bool apple_sleep_coprocs_ans_stopped(void);

/*
 * The same with no ASC to clear: @last_stop clears its own run bits and lowers its own power
 * domain, and runs before every other coprocessor's. The refusal checks leave that domain on.
 */
int apple_sleep_coproc_register_first(struct device *dev, int (*last_stop)(void *data),
				      void *data);
void apple_sleep_coproc_unregister(struct device *dev);

/*
 * During a suspend: a driver left something the DRAM-kept wake would stall on, so this
 * sleep takes the deep path. False if no sleep driver can do that.
 */
bool apple_sleep_request_deep_once(void);

/*
 * For a registered coprocessor: the last of its stop that may still refuse the sleep, run
 * with interrupts off before any coprocessor's @last_stop. A negative return refuses, and the
 * coprocessor must still be able to carry on.
 */
int apple_sleep_coproc_set_last_check(struct device *dev, int (*last_check)(void *data));

/*
 * The enclave's irreversible sleep request. @stop runs with interrupts off,
 * after every refusal and before other irreversible coprocessor stops. Nonzero
 * may refuse ONLY if no request was sent. Once sent, protocol failure is fatal;
 * success must continue through physical sleep/wake, even for a newly pending wake.
 * Unsupported/no-provider paths refuse before sending, never at device suspend.
 */
int apple_sleep_enclave_register(struct device *dev, int (*stop)(void *data), void *data);
void apple_sleep_enclave_unregister(struct device *dev);

/* Its driver has quiesced it for that last step, or has taken it back. */
void apple_sleep_coproc_set_ready(struct device *dev, bool ready);

/* Whether the last step stopped it: after a refused sleep, false means it still runs. */
bool apple_sleep_coproc_stopped(struct device *dev);

/*
 * Whether this sleep's commit stops the registered coprocessors. Then one that can't be
 * registered refuses the sleep: stopped any earlier, a later refusal would strand it.
 */
bool apple_sleep_commit_stops_coprocs(void);

/*
 * The USB host sleeps in place for this sleep, as macOS's does: its always-on domain stays
 * up through the commit. Its PHY sets it once the controller is in its hibernation state,
 * and clears it at the wake.
 */
void apple_sleep_usb_set_asleep(bool asleep);

/*
 * The USB host is up, as macOS's USB_SLP_DDR vote has it: from the host's start to its stop,
 * awake and asleep. It sets bit 0 of the wake block's word 0 for every sleep in between.
 */
void apple_sleep_usb_slp_ddr(bool up);

/*
 * The fixed hub's ports with a device at or below them that can wake the system, bit n for
 * port n. The host that sleeps in place sets them before the freeze and clears them after.
 */
void apple_sleep_usb_set_wake_ports(unsigned long ports);

/*
 * For a driver that takes its own power domain down for a sleep and up again, as macOS's USB
 * host does: @pd is the domain's power controller node. Lowering waits until it reads off.
 * 0, -ETIMEDOUT, -ENODEV without the sleep driver, or -EINVAL for another node, which leave
 * the domain alone and which apple_sleep_domain_check() reports beforehand.
 */
int apple_sleep_domain_set(struct device_node *pd, bool on);
int apple_sleep_domain_check(struct device_node *pd);

/* For the PMGR's driver at the wake: a driver took @pd down and brings it up itself. */
bool apple_sleep_domain_is_down(struct device_node *pd);

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

/* This sleep will end the debug link: for the SMC's driver, which warns the SMC after it. */
bool apple_sleep_debug_link_up(void);

/*
 * From the SMC's driver, in its noirq resume: the SMC woke the AP for the lid alone, and the
 * lid reads closed. If nothing else woke the SoC, the PM core goes back to sleep once every
 * device has resumed, as macOS does from a dark wake.
 */
void apple_sleep_lid_closed_wake(void);

/*
 * From the SMC's driver, in its noirq resume: the lid as it reads now, after every
 * sleep the SMC was told of, whatever woke it.
 */
void apple_sleep_lid_at_wake(bool closed);

/*
 * For the display: 1 if the lid read closed in this wake's noirq resume, 0 if open,
 * -ENODATA if it wasn't read. macOS keeps the internal panel off with the lid closed.
 */
int apple_sleep_lid_closed_at_wake(void);

/*
 * From the SMC's driver, in its noirq resume: whether the SMC's record of the wake has the
 * radios' source.
 */
void apple_sleep_radio_at_wake(bool woke);

/*
 * For the radios: 1 if the SMC's record of this wake has their source, 0 if it was read
 * without it, -ENODATA if it wasn't read.
 */
int apple_sleep_radio_woke(void);

/*
 * From the SMC's driver, in its noirq resume: the SMC woke the AP for the port alone. If
 * this sleep cut the debug link and nothing else woke the SoC, it's a dark wake.
 */
void apple_sleep_debug_link_cut_wake(void);

/* The radio's host has its port back after a real sleep, so the next commit may stop it. */
void apple_sleep_radio_port_up(void);

/* From RTKit: this coprocessor's firmware crashed. */
void apple_sleep_coproc_crashed(struct device *dev);

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

static inline bool apple_sleep_request_deep_once(void)
{
	return false;
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

static inline bool apple_sleep_commit_stops_coprocs(void)
{
	return false;
}

static inline void apple_sleep_usb_set_asleep(bool asleep)
{
}

static inline void apple_sleep_usb_slp_ddr(bool up)
{
}

static inline void apple_sleep_usb_set_wake_ports(unsigned long ports)
{
}

static inline int apple_sleep_domain_set(struct device_node *pd, bool on)
{
	return -ENODEV;
}

static inline int apple_sleep_domain_check(struct device_node *pd)
{
	return -ENODEV;
}

static inline bool apple_sleep_domain_is_down(struct device_node *pd)
{
	return false;
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

static inline bool apple_sleep_debug_link_up(void)
{
	return false;
}

static inline void apple_sleep_lid_closed_wake(void)
{
}

static inline void apple_sleep_lid_at_wake(bool closed)
{
}

static inline int apple_sleep_lid_closed_at_wake(void)
{
	return -ENODATA;
}

static inline void apple_sleep_radio_at_wake(bool woke)
{
}

static inline int apple_sleep_radio_woke(void)
{
	return -ENODATA;
}

static inline void apple_sleep_debug_link_cut_wake(void)
{
}

static inline void apple_sleep_radio_port_up(void)
{
}

static inline void apple_sleep_coproc_crashed(struct device *dev)
{
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
