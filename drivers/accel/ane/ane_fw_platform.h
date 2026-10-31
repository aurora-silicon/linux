/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_PLATFORM_H__
#define __ANE_FW_PLATFORM_H__

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/types.h>

struct platform_device;
struct ane_h16_build;

enum ane_fw_owner {
	ANE_FW_OWNER_AP,
	ANE_FW_OWNER_EXCLAVE,
};

enum ane_fw_irq_role {
	ANE_FW_IRQ_UNKNOWN,
	ANE_FW_IRQ_CHANNELS,
};

/* Later firmware generations must never enter the H14 staging path. */
struct ane_fw_platform {
	const struct ane_h16_build *h16_boot;
	u32 soc;
	u16 chip_family;
	u16 ane_type;
	u8 hwx_subtype;
	u8 task_generation;
	u8 interrupt_count;
	enum ane_fw_irq_role irq_role;
	enum ane_fw_owner owner;
	const char * const *reg_names;
	const u32 *reg_sizes;
	u32 reg_count;
	u32 wrapper;
	u32 mailbox;
	u32 scratch;
	u32 doorbell;
	u32 irq_status;
	u32 irq_ack;
	const u32 *power_words;
	u32 power_count;
	u64 preload_dva[2];
	u64 preload_size[2];
	u16 stream_mask;
	bool firmware_preloaded;
	bool safe_reset;
};

/* Pure range check: the boot loader supplies the physical addresses. */
static inline bool
ane_fw_preload_ranges_valid(const struct ane_fw_platform *soc, const u64 phys[2],
			    const u64 size[2], const u64 dva[2])
{
	u64 end[2];
	unsigned int i;

	for (i = 0; i < 2; i++) {
		if (!phys[i] || !IS_ALIGNED(phys[i], SZ_16K) ||
		    size[i] != soc->preload_size[i] ||
		    dva[i] != soc->preload_dva[i] ||
		    check_add_overflow(phys[i], size[i], &end[i]))
			return false;
	}
	return end[0] <= phys[1] || end[1] <= phys[0];
}

extern const struct ane_fw_platform ane_h16_platform, ane_h17_platform;
int ane_fw_platform_preflight(struct platform_device *pdev,
			      const struct ane_fw_platform *soc);

int ane_fw_platform_probe(struct platform_device *pdev,
			  const struct ane_fw_platform *soc);
void ane_fw_platform_remove(struct platform_device *pdev);
int ane_h16_backend_probe(struct platform_device *pdev, const struct ane_fw_platform *soc,
			  unsigned int stage);
void ane_h16_backend_remove(struct platform_device *pdev);

#endif /* __ANE_FW_PLATFORM_H__ */
