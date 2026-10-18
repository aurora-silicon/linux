/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_DISPLAY_H
#define _LINUX_SOC_APPLE_PMP_DISPLAY_H

#include <linux/of.h>
#include <linux/types.h>

/*
 * The T6030 (M3 Pro) MacBook Pros, 14-inch J514S and 16-inch J516S: one SoC,
 * one PMP and GPU path. Board-specific data (PMP tunables, GPU performance
 * states) comes from the running machine's ADT, not from this match.
 */
static inline bool apple_pmp_t6030_board(void)
{
	return of_machine_is_compatible("apple,t6030") &&
	       (of_machine_is_compatible("apple,j514s") ||
		of_machine_is_compatible("apple,j516s"));
}

/*
 * The T6031 (M3 Max) MacBook Pros, 14-inch J514C and 16-inch J516C. Their
 * PMP values come from the boot loader (/chosen/asahi,t6031-pmp); the GPU
 * needs no PMP vote on this SoC.
 */
static inline bool apple_pmp_t6031_board(void)
{
	return of_machine_is_compatible("apple,t6031") &&
	       (of_machine_is_compatible("apple,j514c") ||
		of_machine_is_compatible("apple,j516c"));
}

struct device;
struct apple_pmp_display;

/* Boards whose PMP session carries the power-vote bridge below. */
bool apple_pmp_display_retained_board(void);

/*
 * Private bridge for the Rust PMP owner (pmp-core.rs); there is no second
 * mailbox owner. The bridge sends untagged power requests through the
 * owner's RTKit session and consumes their replies.
 */
struct apple_pmp_display *apple_pmp_display_register(struct device *dev,
		const void *cookie, int (*send)(const void *cookie, u64 word));
bool apple_pmp_display_receive(struct apple_pmp_display *display, u8 endpoint, u64 word);
void apple_pmp_display_fail(struct apple_pmp_display *display, int error);
/* Internal: caller holds the bound supplier device lock. */
struct apple_pmp_display *apple_pmp_display_bound_owner(struct device *dev);

/*
 * T6030 and T6031 display power. pre_run holds the inherited display and
 * seeds the PTD DISP|ANS request before the PMP CPU runs (called by
 * pmp-core.rs; a no-op on other boards). display_prepare links the DCP to
 * the bound PMP; get/check require the PMP to have acknowledged the DISP
 * request. The J514S GPU variants hold the AGX vote, retained until reboot;
 * prepare is called during probe.
 */
int apple_pmp_display_pre_run(struct device *dev);
/* True once the board's PMP node exists (added by the boot-time handoff). */
bool apple_pmp_display_present(void);
int apple_pmp_display_prepare(struct device *consumer);
int apple_pmp_display_get(struct device *consumer);
int apple_pmp_display_check(struct device *consumer);
int apple_pmp_j514s_gpu_prepare(struct device *consumer);
int apple_pmp_j514s_gpu_get(struct device *consumer);
int apple_pmp_j514s_gpu_check(struct device *consumer);

#endif
