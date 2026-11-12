/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple silicon scaler/CSC (MSR) register map, global block.
 *
 * Offsets are bytes from the start of the MSR aperture (reg index 0).
 * The global control registers are common to every direct-MMIO MSR
 * generation (hardware-version 10 through 22).
 */
#ifndef APPLE_SCALER_REGS_H
#define APPLE_SCALER_REGS_H

#include <linux/bits.h>

#define ASC_HW_VERSION			0x0000
#define ASC_RESET			0x0004
#define   ASC_RESET_ENGINE		BIT(0)
#define ASC_RESET_APIODMA		0x0008
#define ASC_IS_RUNNING			0x000c
#define ASC_REG_FIFO_AVAIL		0x0014
#define ASC_IRQ_ENABLE			0x0018
#define ASC_IRQ_STATUS			0x001c
#define   ASC_IRQ_DONE			BIT(0)
#define   ASC_IRQ_DEBUG_ERROR		BIT(1)
#define   ASC_IRQ_READ_ERROR		BIT(3)
#define   ASC_IRQ_WRITE_ERROR		BIT(4)
#define   ASC_IRQ_DECOMPRESS_ERROR	BIT(9)
#define   ASC_IRQ_CONTEXT_SWITCH	BIT(10)
#define   ASC_IRQ_AXI_ERROR		BIT(12)
#define   ASC_IRQ_ERRORS		(ASC_IRQ_DEBUG_ERROR | ASC_IRQ_READ_ERROR | \
					 ASC_IRQ_WRITE_ERROR | ASC_IRQ_DECOMPRESS_ERROR | \
					 ASC_IRQ_AXI_ERROR)
#define ASC_FRAME_COUNT			0x0020
#define ASC_START			0x0080
#define ASC_PRE_START			0x0098
#define ASC_DEBUG_STATUS		0x009c
#define ASC_TRANSFORM_ID		0x0110

#endif /* APPLE_SCALER_REGS_H */
