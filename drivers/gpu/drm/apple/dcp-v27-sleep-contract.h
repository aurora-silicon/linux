/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _DCP_V27_SLEEP_CONTRACT_H
#define _DCP_V27_SLEEP_CONTRACT_H
/*
 * From the SoC sleep driver, for the firmware 27 display sleep: a display coprocessor reports
 * whether it is quiesced for the sleep, and asks whether its power domains are up so that its
 * registers and its DARTs can be read (0) or not.
 */
void apple_sleep_display_quiesced(struct device *dev, bool quiesced);
int apple_sleep_display_v27_power_ready(struct device *dev);
#endif
