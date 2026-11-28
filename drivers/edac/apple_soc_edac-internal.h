/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SOC_EDAC_INTERNAL_H
#define _APPLE_SOC_EDAC_INTERNAL_H

#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/workqueue.h>

#define APPLE_SOC_EDAC_NAME	"apple_soc_edac"

/*
 * The sources' status registers have no established acknowledge contract,
 * so a line is masked when it fires and unmasked again after a backoff.
 * A line still asserted after APPLE_SOC_EDAC_MAX_RETRIES backoffs stays
 * masked; its count keeps the record.
 */
#define APPLE_SOC_EDAC_BACKOFF_MS	1000
#define APPLE_SOC_EDAC_BACKOFF_MAX_MS	60000
#define APPLE_SOC_EDAC_MAX_RETRIES	8

#define T6041_NI_REGS		25
#define T6041_NI_STATUS		0x416c

struct apple_soc_edac_range {
	u8 first;
	u8 count;
	const char *name;
};

/*
 * A fabric NI whose error status sits in its aperture: a block of status words,
 * one per error type, whose set bits name the port. They're acknowledged by
 * writing the set bits back.
 */
struct apple_soc_edac_ni {
	u8 index;	/* interrupt index */
	u8 reg;		/* reg index of the NI's aperture */
	u16 status;	/* offset of the first status word */
	u32 words;	/* bit n: a status word at status + 4 * n */
};

struct apple_soc_edac_hw {
	const struct apple_soc_edac_range *ranges;
	size_t nranges;
	const struct apple_soc_edac_ni *ni;
	size_t nni;
	unsigned int num_irqs;
	unsigned int num_status_regs;
	bool event_log;
};

struct apple_soc_edac;

struct apple_soc_edac_irq {
	struct apple_soc_edac *edac;
	const char *source;
	unsigned int index;
	int irq;
	const struct apple_soc_edac_ni *ni;
	void __iomem *ni_base;
	unsigned int retries;
	unsigned int backoff_ms;
	unsigned long armed_jiffies;
	struct delayed_work rearm;
};

struct apple_soc_edac {
	struct device *dev;
	struct edac_device_ctl_info *dci;
	const struct apple_soc_edac_hw *hw;
	struct apple_soc_edac_irq *irqs;
	void __iomem **status_regs;
};

#if IS_ENABLED(CONFIG_EDAC_APPLE_SOC_KUNIT_TEST)
const char *apple_soc_edac_source(const struct apple_soc_edac_hw *hw,
				 unsigned int index);
u32 apple_soc_edac_read(void __iomem *reg);
void apple_soc_edac_write(void __iomem *reg, u32 val);
bool apple_soc_edac_t6041_status(struct apple_soc_edac_irq *irq_data,
				bool *error, bool *cleared);
bool apple_soc_edac_ni_status(struct apple_soc_edac_irq *irq_data,
			     bool *cleared);
void apple_soc_edac_rearm(struct work_struct *work);
irqreturn_t apple_soc_edac_irq_thread(int irq, void *data);
extern const struct of_device_id apple_soc_edac_of_match[];
#endif

#endif
