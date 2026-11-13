/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _DCP_V27_SLEEP_CONTRACT_H
#define _DCP_V27_SLEEP_CONTRACT_H
/* Implemented by the separate parent-api patch on entry-v2. cc662 by itself
 * does not supply this M5 manager. Reconcile into the actual merged parent.
 */
void apple_sleep_display_quiesced(struct device *dev, bool quiesced);
int apple_sleep_display_v27_power_ready(struct device *dev);
#endif
