/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8140_BOOTSTRAP_H
#define PCIE_APPLE_T8140_BOOTSTRAP_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

/*
 * Common PIODMA request-engine contract.
 *
 * The T8140 bootstrap and the dmaengine provider use the same request engine.
 * Keep the words below expressed as fields: this is the reviewed public
 * register contract, not a copy of an opaque firmware programming sequence.
 */
#define APPLE_PIODMA_REG_CONTROL			0x000
#define APPLE_PIODMA_CONTROL_ENABLE		BIT(0)
/*
 * Reads back set out of reset. Both control bits are cleared before the
 * routes are programmed, and the register must then read back zero; the
 * T8140 bootstrap applies the same sequence.
 */
#define APPLE_PIODMA_CONTROL_HOLD		BIT(1)

#define APPLE_PIODMA_REG_IRQ_STATUS		0x004
#define APPLE_PIODMA_REG_IRQ_ENABLE		0x008
#define APPLE_PIODMA_IRQ_COMPLETION		(BIT(0) | BIT(6))
#define APPLE_PIODMA_IRQ_ALL			GENMASK(9, 0)

#define APPLE_PIODMA_REG_FIFO_STATUS		0x00c
#define APPLE_PIODMA_FIFO_AVAILABLE		GENMASK(5, 0)
#define APPLE_PIODMA_FIFO_DEPTH			BIT(4)

#define APPLE_PIODMA_REG_TUNE0			0x010
#define APPLE_PIODMA_REG_TUNE1			0x014
#define APPLE_PIODMA_REG_TUNE2			0x018
#define APPLE_PIODMA_REG_TUNE3			0x01c
#define APPLE_PIODMA_REG_TUNE4			0x020
#define APPLE_PIODMA_TUNE_WIDE_MASK		GENMASK(28, 0)
#define APPLE_PIODMA_TUNE_NARROW_MASK		GENMASK(18, 0)
#define APPLE_PIODMA_TUNE_WINDOW_LOW		GENMASK(9, 4)
#define APPLE_PIODMA_TUNE_WINDOW_HIGH		GENMASK(16, 11)
#define APPLE_PIODMA_TUNE_CLASS			GENMASK(1, 0)
#define APPLE_PIODMA_TUNE_CLASS_NONE		0
#define APPLE_PIODMA_TUNE_CLASS_REQUEST		2
#define APPLE_PIODMA_TUNE_CLASS_COMPLETION	3
#define APPLE_PIODMA_TUNE_WINDOWS				\
	(FIELD_PREP(APPLE_PIODMA_TUNE_WINDOW_LOW, GENMASK(5, 0)) | \
	 FIELD_PREP(APPLE_PIODMA_TUNE_WINDOW_HIGH, GENMASK(5, 0)))

#define APPLE_PIODMA_REG_ROUTE_BASE		0x024
#define APPLE_PIODMA_REG_ROUTE_STRIDE		4
#define APPLE_PIODMA_ROUTE_COUNT		8

#define APPLE_PIODMA_REG_COMPLETION_LO		0x044
#define APPLE_PIODMA_REG_COMPLETION_HI		0x048
#define APPLE_PIODMA_REG_DESCRIPTOR_LO		0x04c
#define APPLE_PIODMA_REG_DESCRIPTOR_HI		0x050
#define APPLE_PIODMA_POINTER_HI			GENMASK(9, 0)

#define APPLE_PIODMA_REG_COMMAND			0x054
#define APPLE_PIODMA_COMMAND_RUN			BIT(0)
#define APPLE_PIODMA_COMMAND_IRQ			BIT(4)
#define APPLE_PIODMA_COMMAND_DESCRIPTOR		BIT(10)

#define APPLE_PIODMA_REG_TAG			0x0d8
#define APPLE_PIODMA_REG_COMPLETED_TAG		0x0dc
#define APPLE_PIODMA_TAG			GENMASK(7, 0)

#define APPLE_PIODMA_REG_MAP_BASE		0x0f0
#define APPLE_PIODMA_REG_MAP_STRIDE		4
#define APPLE_PIODMA_MAP_DESCRIPTOR_INDEX	1
#define APPLE_PIODMA_MAP_DESCRIPTOR		1

#define APPLE_PIODMA_FABRIC_REG_IRQ_STATUS	0x000
#define APPLE_PIODMA_FABRIC_REG_IRQ_ENABLE	0x004

/* Descriptor and completion pointers select the translated 1 TiB aperture. */
#define APPLE_PIODMA_PACKET_MASK			GENMASK_ULL(35, 0)
#define APPLE_PIODMA_POINTER_MASK		GENMASK_ULL(41, 0)
#define APPLE_PIODMA_POINTER_PREFIX		BIT_ULL(40)

/*
 * Four-word request packet. The framing fields are the independently reviewed
 * request-valid contract; address and length fields are documented separately
 * so callers cannot smuggle a pre-composed hardware word into the driver.
 */
#define APPLE_PIODMA_PACKET0_FRAME_LO		GENMASK(2, 0)
#define APPLE_PIODMA_PACKET0_FRAME_LO_VALUE	GENMASK(2, 0)
#define APPLE_PIODMA_PACKET0_FRAME_HI		GENMASK(15, 14)
#define APPLE_PIODMA_PACKET0_FRAME_HI_VALUE	GENMASK(1, 0)
#define APPLE_PIODMA_PACKET0_SOURCE_APERTURE1	BIT(18)
#define APPLE_PIODMA_PACKET0_SOURCE_LO		GENMASK(31, 24)
#define APPLE_PIODMA_PACKET1_SOURCE_HI		GENMASK(27, 0)
#define APPLE_PIODMA_PACKET1_DEST_LO		GENMASK(31, 28)
#define APPLE_PIODMA_PACKET2_DEST_HI		GENMASK(31, 0)
#define APPLE_PIODMA_PACKET3_LENGTH		GENMASK(31, 0)
/* Address bits carried by the low fields; the high fields hold the rest. */
#define APPLE_PIODMA_PACKET_SOURCE_LO_BITS	8
#define APPLE_PIODMA_PACKET_DEST_LO_BITS	4

/* One FIFO entry carries one 64-bit data beat; packet addresses are 32-bit aligned. */
#define APPLE_PIODMA_COPY_ALIGN			sizeof(u32)
#define APPLE_PIODMA_MAX_TRANSFER		(APPLE_PIODMA_FIFO_DEPTH * sizeof(u64))

struct device;
struct pci_dev;

#if IS_ENABLED(CONFIG_PCIE_APPLE_T8140_BOOTSTRAP)
int apple_piodma_bootstrap_get(struct device *host, struct device **supplier);
int apple_piodma_bootstrap_guard_begin(struct device *supplier,
				       struct device *host);
void apple_piodma_bootstrap_guard_end(struct device *supplier);
int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root);
void apple_piodma_bootstrap_admitted(struct device *supplier);
bool apple_piodma_bootstrap_retained(struct device *supplier);
#else
static inline int apple_piodma_bootstrap_get(struct device *host, struct device **supplier)
{
	return -ENODEV;
}

static inline int apple_piodma_bootstrap_guard_begin(struct device *supplier,
						     struct device *host)
{
	return -ENODEV;
}

static inline void apple_piodma_bootstrap_guard_end(struct device *supplier)
{
}

static inline int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root)
{
	return -ENODEV;
}

static inline void apple_piodma_bootstrap_admitted(struct device *supplier)
{
}

static inline bool apple_piodma_bootstrap_retained(struct device *supplier)
{
	return false;
}
#endif
#endif
