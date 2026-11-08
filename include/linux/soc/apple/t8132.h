/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_T8132_H
#define _LINUX_SOC_APPLE_T8132_H

#include <linux/of.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * The M4 MacBook Airs, J713 (13-inch) and J715 (15-inch): one board design
 * and boot loader handoff. Their ADTs differ in the panel, the speakers, the
 * radio's firmware island and per-machine trims, all of which their device
 * trees describe.
 */
static inline bool apple_t8132_air(void)
{
	return of_machine_is_compatible("apple,j713") ||
	       of_machine_is_compatible("apple,j715");
}

/*
 * The other M4 Macs: the MacBook Pro 14-inch (J604), the Mac mini (J773g)
 * and the iMacs (J623, J624). The J604 and J773g ADTs are read; the iMacs
 * have none yet.
 */
static inline bool apple_t8132_pro_mini_imac(void)
{
	return of_machine_is_compatible("apple,j604") ||
	       of_machine_is_compatible("apple,j623") ||
	       of_machine_is_compatible("apple,j624") ||
	       of_machine_is_compatible("apple,j773g");
}

/*
 * The OS firmware the M4 PMP, PMGR ownership, display and GPU paths are
 * qualified on, as the boot loader reports it: macOS 26.6.2 (iBoot
 * mBoot-18000.161.10). These paths depend on the firmware, not the board.
 */
static inline bool apple_t8132_qualified_firmware(void)
{
	const char *version;

	return of_machine_is_compatible("apple,t8132") &&
	       !of_property_read_string(of_chosen, "asahi,os-fw-version", &version) &&
	       !strcmp(version, "26.6.2");
}

/*
 * The firmware-dependent M4 paths: every M4 Mac on the qualified firmware,
 * and the Airs on any firmware, as before.
 */
static inline bool apple_t8132_firmware_path(void)
{
	return apple_t8132_air() || apple_t8132_qualified_firmware();
}

/*
 * The radio port the Airs have: reg-on on the SMC's gP13 with the same
 * template, and the radio's two ordinary streams managed on port 0 of the
 * built-in PCIe controller. The J604 and J773g ADTs match; the iMacs are
 * assumed to.
 */
static inline bool apple_t8132_managed_radio(void)
{
	return apple_t8132_air() || apple_t8132_pro_mini_imac();
}

/*
 * The Airs' USB-C host sequence: a DWC3 enters host mode before core init
 * and gets the CIO setup before xHCI starts. Every M4 Mac has these ATCs.
 */
static inline bool apple_t8132_usb_host_first(void)
{
	return apple_t8132_air() || apple_t8132_pro_mini_imac();
}

/*
 * The Airs' MTP keyboard and trackpad, which iBoot leaves running: the
 * J604 has the same.
 */
static inline bool apple_t8132_live_mtp(void)
{
	return apple_t8132_air() || of_machine_is_compatible("apple,j604");
}

#endif
