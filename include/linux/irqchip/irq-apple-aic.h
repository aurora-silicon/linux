/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_IRQCHIP_IRQ_APPLE_AIC_H
#define _LINUX_IRQCHIP_IRQ_APPLE_AIC_H

#include <linux/types.h>

#ifdef CONFIG_APPLE_AIC
void aic_sleep_mask_all(void);
void aic_sleep_restore(void);
u32 aic_sleep_ack(void);
#else
static inline void aic_sleep_mask_all(void) { }
static inline void aic_sleep_restore(void) { }
static inline u32 aic_sleep_ack(void) { return 0; }
#endif

#endif
