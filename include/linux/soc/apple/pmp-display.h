/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_DISPLAY_H
#define _LINUX_SOC_APPLE_PMP_DISPLAY_H

#include <linux/of.h>
#include <linux/soc/apple/t8132.h>
#include <linux/types.h>

/* The retained M4 bridge and raw thermal records: the M4 firmware path. */
static inline bool apple_pmp_j713_board(void)
{
	return apple_t8132_firmware_path() && of_machine_is_compatible("apple,t8132");
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

/* J713 votes share the existing PMP owner's untagged request stream.
 * Prepare is probe-only; supplier links persist until reboot, and so do
 * accepted votes, except that the DCP releases its DISP vote while it has
 * powered the display domains down (put) and takes it again (get). Before
 * that it asks, changing nothing, whether the vote can be released now and
 * taken again later (check_release).
 */
int apple_pmp_j713_display_prepare(struct device *consumer);
int apple_pmp_j713_display_get(struct device *consumer);
int apple_pmp_j713_display_put(struct device *consumer);
int apple_pmp_j713_display_check(struct device *consumer);
int apple_pmp_j713_display_check_release(struct device *consumer);
int apple_pmp_j713_gpu_prepare(struct device *consumer);
int apple_pmp_j713_gpu_get(struct device *consumer);
int apple_pmp_j713_gpu_check(struct device *consumer);

#endif
