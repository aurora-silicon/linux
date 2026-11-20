/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_T8132_PMP_H
#define _LINUX_SOC_APPLE_T8132_PMP_H

/*
 * The M4 PMP path's firmware gate, for the PMP modules only (apple-pmp,
 * apple-pmp-thermal). Upstream (be28e323) adds these to t8132.h and switches
 * pmp-display.h to them; both of those headers are part of the built-in Rust
 * bindings, so on a running m4drv8 kernel the modules carry the gate here and
 * leave those headers, and every built-in user of them, unchanged.
 */

#include <linux/of.h>
#include <linux/string.h>
#include <linux/soc/apple/t8132.h>

/*
 * The OS firmware whose PMP image the M4 PMP path runs: 26.6.2 (25G83), and
 * 27.2 (26B123), whose PMP image 27.2.1 (26B131) carries unchanged. The 27.2
 * image adds one boot argument (BWWD), left at the firmware's default, and
 * moves its data segment and heap; the PMP driver finds both from the image.
 */
static inline bool apple_t8132_pmp_firmware(void)
{
	const char *version;

	if (!of_machine_is_compatible("apple,t8132") ||
	    of_property_read_string(of_chosen, "asahi,os-fw-version", &version))
		return false;
	return !strcmp(version, "26.6.2") || !strcmp(version, "27.2") ||
	       !strcmp(version, "27.2.1");
}

/*
 * The M4 PMP session with its telemetry, thermal policy and power-vote
 * bridge: every M4 Mac on a PMP firmware above, and the Airs on any.
 */
static inline bool apple_t8132_pmp_path(void)
{
	return apple_t8132_air() || apple_t8132_pmp_firmware();
}

/* An M4 board on the PMP path (pmp-display.h's two board checks, gated as above). */
static inline bool apple_t8132_pmp_board(void)
{
	return apple_t8132_pmp_path() && of_machine_is_compatible("apple,t8132");
}

/*
 * The 27.2 (26B123) PMP image, also in 27.2.1, starts on its own once its CPU
 * runs: it acknowledges IOP power ON about 0.13 ms later, without a host
 * request. Asking it again (an RTKit wake) times out, so the host only waits
 * for it to report in, as the G16 27.2 start does. 26.6.2 keeps the wake.
 */
static inline bool apple_t8132_pmp_self_starts(void)
{
	const char *version;

	if (!of_machine_is_compatible("apple,t8132") ||
	    of_property_read_string(of_chosen, "asahi,os-fw-version", &version))
		return false;
	return !strcmp(version, "27.2") || !strcmp(version, "27.2.1");
}

#endif
