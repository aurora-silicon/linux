// SPDX-License-Identifier: (GPL-2.0-only OR MIT) AND GPL-2.0-only
/*
 * Apple SART device driver
 * Copyright (C) The Asahi Linux Contributors
 *
 * Apple SART is a simple address filter for some DMA transactions.
 * Regions of physical memory must be added to the SART's allow
 * list before any DMA can target these. Unlike a proper
 * IOMMU no remapping can be done and special support in the
 * consumer driver is required since not all DMA transactions of
 * a single device are subject to SART filtering.
 */

#include <linux/soc/apple/sart.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/types.h>

#define APPLE_SART_MAX_ENTRIES 16

/* SARTv0 registers */
#define APPLE_SART0_CONFIG(idx)       (0x00 + 4 * (idx))
#define APPLE_SART0_CONFIG_FLAGS      GENMASK(28, 24)
#define APPLE_SART0_CONFIG_SIZE       GENMASK(18, 0)
#define APPLE_SART0_CONFIG_SIZE_SHIFT 12
#define APPLE_SART0_CONFIG_SIZE_MAX   GENMASK(18, 0)

#define APPLE_SART0_PADDR(idx)  (0x40 + 4 * (idx))
#define APPLE_SART0_PADDR_SHIFT 12

#define APPLE_SART0_FLAGS_ALLOW 0xf

/* SARTv2 registers */
#define APPLE_SART2_CONFIG(idx)	      (0x00 + 4 * (idx))
#define APPLE_SART2_CONFIG_FLAGS      GENMASK(31, 24)
#define APPLE_SART2_CONFIG_SIZE	      GENMASK(23, 0)
#define APPLE_SART2_CONFIG_SIZE_SHIFT 12
#define APPLE_SART2_CONFIG_SIZE_MAX   GENMASK(23, 0)

#define APPLE_SART2_PADDR(idx)	(0x40 + 4 * (idx))
#define APPLE_SART2_PADDR_SHIFT 12

#define APPLE_SART2_FLAGS_ALLOW 0xff

/* SARTv3 registers */
#define APPLE_SART3_CONFIG(idx) (0x00 + 4 * (idx))

#define APPLE_SART3_PADDR	0x40
#define APPLE_SART3_PADDR_SHIFT 12

#define APPLE_SART3_SIZE       0x80
#define APPLE_SART3_SIZE_SHIFT 12
#define APPLE_SART3_SIZE_MAX   GENMASK(29, 0)

#define APPLE_SART3_FLAGS_ALLOW 0xff

/* SARTv4 (M5): SARTv3's entries, with the address and size arrays moved up */
#define APPLE_SART4_PADDR 0x60
#define APPLE_SART4_SIZE  0xc0

struct apple_sart_ops {
	void (*get_entry)(struct apple_sart *sart, int index, u8 *flags,
			  phys_addr_t *paddr, size_t *size);
	void (*set_entry)(struct apple_sart *sart, int index, u8 flags,
			  phys_addr_t paddr_shifted, size_t size_shifted);
	/* This is probably a bitfield but the exact meaning of each bit is unknown. */
	unsigned int flags_allow;
	/* SARTv3/v4: where the address and size arrays start */
	unsigned int paddr_reg;
	unsigned int size_reg;
	unsigned int size_shift;
	unsigned int paddr_shift;
	size_t size_max;
};

struct apple_sart {
	struct device *dev;
	void __iomem *regs;

	const struct apple_sart_ops *ops;

	unsigned long protected_entries;
	unsigned long used_entries;
	/* The registers as they stood before a firmware-driven suspend */
	struct {
		u8 flags;
		phys_addr_t paddr;
		size_t size;
	} saved[APPLE_SART_MAX_ENTRIES];
	bool saved_valid;
	/* saved[] still holds the last sleep's entries, for apple_sart_replay() */
	bool replay_valid;
};

static void sart0_get_entry(struct apple_sart *sart, int index, u8 *flags,
	phys_addr_t *paddr, size_t *size)
{
	u32 cfg = readl(sart->regs + APPLE_SART0_CONFIG(index));
	phys_addr_t paddr_ = readl(sart->regs + APPLE_SART0_PADDR(index));
	size_t size_ = FIELD_GET(APPLE_SART0_CONFIG_SIZE, cfg);

	*flags = FIELD_GET(APPLE_SART0_CONFIG_FLAGS, cfg);
	*size = size_ << APPLE_SART0_CONFIG_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART0_PADDR_SHIFT;
}

static void sart0_set_entry(struct apple_sart *sart, int index, u8 flags,
	phys_addr_t paddr_shifted, size_t size_shifted)
{
	u32 cfg;

	cfg = FIELD_PREP(APPLE_SART0_CONFIG_FLAGS, flags);
	cfg |= FIELD_PREP(APPLE_SART0_CONFIG_SIZE, size_shifted);

	writel(paddr_shifted, sart->regs + APPLE_SART0_PADDR(index));
	writel(cfg, sart->regs + APPLE_SART0_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v0 = {
	.get_entry = sart0_get_entry,
	.set_entry = sart0_set_entry,
	.flags_allow = APPLE_SART0_FLAGS_ALLOW,
	.size_shift = APPLE_SART0_CONFIG_SIZE_SHIFT,
	.paddr_shift = APPLE_SART0_PADDR_SHIFT,
	.size_max = APPLE_SART0_CONFIG_SIZE_MAX,
};

static void sart2_get_entry(struct apple_sart *sart, int index, u8 *flags,
			    phys_addr_t *paddr, size_t *size)
{
	u32 cfg = readl(sart->regs + APPLE_SART2_CONFIG(index));
	phys_addr_t paddr_ = readl(sart->regs + APPLE_SART2_PADDR(index));
	size_t size_ = FIELD_GET(APPLE_SART2_CONFIG_SIZE, cfg);

	*flags = FIELD_GET(APPLE_SART2_CONFIG_FLAGS, cfg);
	*size = size_ << APPLE_SART2_CONFIG_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART2_PADDR_SHIFT;
}

static void sart2_set_entry(struct apple_sart *sart, int index, u8 flags,
			    phys_addr_t paddr_shifted, size_t size_shifted)
{
	u32 cfg;

	cfg = FIELD_PREP(APPLE_SART2_CONFIG_FLAGS, flags);
	cfg |= FIELD_PREP(APPLE_SART2_CONFIG_SIZE, size_shifted);

	writel(paddr_shifted, sart->regs + APPLE_SART2_PADDR(index));
	writel(cfg, sart->regs + APPLE_SART2_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v2 = {
	.get_entry = sart2_get_entry,
	.set_entry = sart2_set_entry,
	.flags_allow = APPLE_SART2_FLAGS_ALLOW,
	.size_shift = APPLE_SART2_CONFIG_SIZE_SHIFT,
	.paddr_shift = APPLE_SART2_PADDR_SHIFT,
	.size_max = APPLE_SART2_CONFIG_SIZE_MAX,
};

static void sart3_get_entry(struct apple_sart *sart, int index, u8 *flags,
			    phys_addr_t *paddr, size_t *size)
{
	phys_addr_t paddr_ = readl(sart->regs + sart->ops->paddr_reg + 4 * index);
	size_t size_ = readl(sart->regs + sart->ops->size_reg + 4 * index);

	*flags = readl(sart->regs + APPLE_SART3_CONFIG(index));
	*size = size_ << APPLE_SART3_SIZE_SHIFT;
	*paddr = paddr_ << APPLE_SART3_PADDR_SHIFT;
}

static void sart3_set_entry(struct apple_sart *sart, int index, u8 flags,
			    phys_addr_t paddr_shifted, size_t size_shifted)
{
	writel(paddr_shifted, sart->regs + sart->ops->paddr_reg + 4 * index);
	writel(size_shifted, sart->regs + sart->ops->size_reg + 4 * index);
	writel(flags, sart->regs + APPLE_SART3_CONFIG(index));
}

static struct apple_sart_ops sart_ops_v3 = {
	.get_entry = sart3_get_entry,
	.set_entry = sart3_set_entry,
	.flags_allow = APPLE_SART3_FLAGS_ALLOW,
	.paddr_reg = APPLE_SART3_PADDR,
	.size_reg = APPLE_SART3_SIZE,
	.size_shift = APPLE_SART3_SIZE_SHIFT,
	.paddr_shift = APPLE_SART3_PADDR_SHIFT,
	.size_max = APPLE_SART3_SIZE_MAX,
};

static struct apple_sart_ops sart_ops_v4 = {
	.get_entry = sart3_get_entry,
	.set_entry = sart3_set_entry,
	.flags_allow = APPLE_SART3_FLAGS_ALLOW,
	.paddr_reg = APPLE_SART4_PADDR,
	.size_reg = APPLE_SART4_SIZE,
	.size_shift = APPLE_SART3_SIZE_SHIFT,
	.paddr_shift = APPLE_SART3_PADDR_SHIFT,
	.size_max = APPLE_SART3_SIZE_MAX,
};

static int apple_sart_probe(struct platform_device *pdev)
{
	int i;
	struct apple_sart *sart;
	struct device *dev = &pdev->dev;

	sart = devm_kzalloc(dev, sizeof(*sart), GFP_KERNEL);
	if (!sart)
		return -ENOMEM;

	sart->dev = dev;
	sart->ops = of_device_get_match_data(dev);

	sart->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sart->regs))
		return PTR_ERR(sart->regs);

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		u8 flags;
		size_t size;
		phys_addr_t paddr;

		sart->ops->get_entry(sart, i, &flags, &paddr, &size);

		if (!flags)
			continue;

		dev_dbg(sart->dev,
			"SART bootloader entry: index %02d; flags: 0x%02x; paddr: %pa; size: 0x%zx\n",
			i, flags, &paddr, size);
		set_bit(i, &sart->protected_entries);
	}
	dev_dbg(dev, "%lu bootloader entries protected\n", hweight_long(sart->protected_entries));

	platform_set_drvdata(pdev, sart);
	return 0;
}

struct apple_sart *devm_apple_sart_get(struct device *dev)
{
	struct device_node *sart_node;
	struct platform_device *sart_pdev;
	struct device_link *link;
	struct apple_sart *sart;

	sart_node = of_parse_phandle(dev->of_node, "apple,sart", 0);
	if (!sart_node)
		return ERR_PTR(-ENODEV);

	sart_pdev = of_find_device_by_node(sart_node);
	of_node_put(sart_node);

	if (!sart_pdev)
		return ERR_PTR(-ENODEV);

	sart = dev_get_drvdata(&sart_pdev->dev);
	if (!sart) {
		put_device(&sart_pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	link = device_link_add(dev, &sart_pdev->dev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);
	if (!link) {
		put_device(&sart_pdev->dev);
		return ERR_PTR(-ENODEV);
	}

	put_device(&sart_pdev->dev);

	return sart;
}
EXPORT_SYMBOL_GPL(devm_apple_sart_get);

bool apple_sart_is_inherited_region(struct apple_sart *sart, phys_addr_t paddr,
				    size_t size)
{
	int i;

	if (!size || paddr > PHYS_ADDR_MAX - (size - 1))
		return false;
	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++) {
		phys_addr_t base;
		size_t length;
		u8 flags;

		/* Linux never writes protected entries, including during shutdown. */
		if (!test_bit(i, &sart->protected_entries))
			continue;
		sart->ops->get_entry(sart, i, &flags, &base, &length);
		if (flags == sart->ops->flags_allow && paddr >= base &&
		    size <= length && paddr - base <= length - size)
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(apple_sart_is_inherited_region);

static int sart_set_entry(struct apple_sart *sart, int index, u8 flags,
			  phys_addr_t paddr, size_t size)
{
	if (size & ((1 << sart->ops->size_shift) - 1))
		return -EINVAL;
	if (paddr & ((1 << sart->ops->paddr_shift) - 1))
		return -EINVAL;

	paddr >>= sart->ops->size_shift;
	size >>= sart->ops->paddr_shift;

	if (size > sart->ops->size_max)
		return -EINVAL;

	sart->ops->set_entry(sart, index, flags, paddr, size);
	return 0;
}

int apple_sart_add_allowed_region(struct apple_sart *sart, phys_addr_t paddr,
				  size_t size)
{
	int i, ret;

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		if (test_bit(i, &sart->protected_entries))
			continue;
		if (test_and_set_bit(i, &sart->used_entries))
			continue;

		ret = sart_set_entry(sart, i, sart->ops->flags_allow, paddr,
				     size);
		if (ret) {
			dev_dbg(sart->dev,
				"unable to set entry %d to [%pa, 0x%zx]\n",
				i, &paddr, size);
			clear_bit(i, &sart->used_entries);
			return ret;
		}

		dev_dbg(sart->dev, "wrote [%pa, 0x%zx] to %d\n", &paddr, size,
			i);
		return 0;
	}

	dev_warn(sart->dev,
		 "no free entries left to add [paddr: 0x%pa, size: 0x%zx]\n",
		 &paddr, size);

	return -EBUSY;
}
EXPORT_SYMBOL_GPL(apple_sart_add_allowed_region);

int apple_sart_remove_allowed_region(struct apple_sart *sart, phys_addr_t paddr,
				     size_t size)
{
	int i;

	dev_dbg(sart->dev,
		"will remove [paddr: %pa, size: 0x%zx] from allowed regions\n",
		&paddr, size);

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		u8 eflags;
		size_t esize;
		phys_addr_t epaddr;

		if (test_bit(i, &sart->protected_entries))
			continue;

		sart->ops->get_entry(sart, i, &eflags, &epaddr, &esize);

		if (epaddr != paddr || esize != size)
			continue;

		sart->ops->set_entry(sart, i, 0, 0, 0);

		clear_bit(i, &sart->used_entries);
		dev_dbg(sart->dev, "cleared entry %d\n", i);
		return 0;
	}

	dev_warn(sart->dev, "entry [paddr: 0x%pa, size: 0x%zx] not found\n",
		 &paddr, size);

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(apple_sart_remove_allowed_region);

static void apple_sart_shutdown(struct platform_device *pdev)
{
	struct apple_sart *sart = dev_get_drvdata(&pdev->dev);
	int i;

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
		if (test_bit(i, &sart->protected_entries))
			continue;

		sart->ops->set_entry(sart, i, 0, 0, 0);
	}
}

/*
 * A SART in a domain that a firmware-driven suspend powers off comes back empty,
 * and its coprocessor can't reach its firmware or buffers until the entries are
 * back. This includes the bootloader's, which Linux otherwise never writes.
 */
static int apple_sart_suspend_late(struct device *dev)
{
	struct apple_sart *sart = dev_get_drvdata(dev);
	int i;

	sart->saved_valid = false;
	sart->replay_valid = false;
	if (pm_suspend_target_state != PM_SUSPEND_MEM || !pm_suspend_via_firmware())
		return 0;

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++)
		sart->ops->get_entry(sart, i, &sart->saved[i].flags,
				     &sart->saved[i].paddr, &sart->saved[i].size);
	sart->saved_valid = true;

	return 0;
}

static int apple_sart_resume_noirq(struct device *dev)
{
	struct apple_sart *sart = dev_get_drvdata(dev);
	unsigned int restored = 0;
	int i;

	if (!sart->saved_valid || !pm_resume_via_firmware())
		return 0;
	sart->saved_valid = false;

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++) {
		/* An unused entry stays as the power left it, whatever that reads. */
		if (!sart->saved[i].size && !sart->saved[i].flags)
			continue;
		/* Every one in use, the same or not: macOS rewrites 0 to 7 at every wake. */
		sart->ops->set_entry(sart, i, sart->saved[i].flags,
				     sart->saved[i].paddr >> sart->ops->paddr_shift,
				     sart->saved[i].size >> sart->ops->size_shift);
		restored++;
	}
	dev_dbg(dev, "%u entries restored after sleep\n", restored);
	sart->replay_valid = true;

	return 0;
}

/*
 * A coprocessor's driver calls this right before its run bit. macOS writes the entries back
 * there, config first, then address, then size, so this does the same on SARTv3. Unused
 * entries stay as they read.
 */
int apple_sart_replay(struct apple_sart *sart)
{
	int i, bad = 0;

	if ((sart->ops != &sart_ops_v3 && sart->ops != &sart_ops_v4) ||
	    !sart->replay_valid)
		return -EOPNOTSUPP;

	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++) {
		phys_addr_t paddr;
		size_t size;
		u8 flags;

		if (!sart->saved[i].size && !sart->saved[i].flags)
			continue;
		writel(sart->saved[i].flags, sart->regs + APPLE_SART3_CONFIG(i));
		writel(sart->saved[i].paddr >> APPLE_SART3_PADDR_SHIFT,
		       sart->regs + sart->ops->paddr_reg + 4 * i);
		writel(sart->saved[i].size >> APPLE_SART3_SIZE_SHIFT,
		       sart->regs + sart->ops->size_reg + 4 * i);
		sart->ops->get_entry(sart, i, &flags, &paddr, &size);
		if (flags == sart->saved[i].flags && paddr == sart->saved[i].paddr &&
		    size == sart->saved[i].size)
			continue;
		bad++;
		dev_warn(sart->dev, "entry %d reads %#x %pa+%#zx, wrote %#x %pa+%#zx\n",
			 i, flags, &paddr, size, sart->saved[i].flags, &sart->saved[i].paddr,
			 sart->saved[i].size);
	}

	return bad;
}
EXPORT_SYMBOL_GPL(apple_sart_replay);

int apple_sart_sleep_validate(struct apple_sart *sart)
{
	int i;

	if (sart->ops != &sart_ops_v4 || !sart->saved_valid)
		return -EOPNOTSUPP;
	for (i = 0; i < APPLE_SART_MAX_ENTRIES; i++) {
		phys_addr_t paddr;
		size_t size;
		u8 flags;

		sart->ops->get_entry(sart, i, &flags, &paddr, &size);
		if (flags != sart->saved[i].flags || paddr != sart->saved[i].paddr ||
		    size != sart->saved[i].size || (!!flags != !!size))
			return -ESTALE;
		if ((test_bit(i, &sart->protected_entries) ||
		     test_bit(i, &sart->used_entries)) && (!flags || !size))
			return -ESTALE;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sart_sleep_validate);

static const struct dev_pm_ops apple_sart_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(apple_sart_suspend_late, NULL)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, apple_sart_resume_noirq)
};

static const struct of_device_id apple_sart_of_match[] = {
	{
		.compatible = "apple,t6050-sart",
		.data = &sart_ops_v4,
	},
	{
		.compatible = "apple,t6000-sart",
		.data = &sart_ops_v3,
	},
	{
		.compatible = "apple,t8103-sart",
		.data = &sart_ops_v2,
	},
	{
		.compatible = "apple,t8015-sart",
		.data = &sart_ops_v0,
	},
	{}
};
MODULE_DEVICE_TABLE(of, apple_sart_of_match);

static struct platform_driver apple_sart_driver = {
	.driver = {
		.name = "apple-sart",
		.of_match_table = apple_sart_of_match,
		.pm = pm_sleep_ptr(&apple_sart_pm_ops),
	},
	.probe = apple_sart_probe,
	.shutdown = apple_sart_shutdown,
};
module_platform_driver(apple_sart_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple SART driver");
