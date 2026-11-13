/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple SPMI controller interrupt register layout, shared with the host check
 * (spmi-apple-irq-host-test.c).
 *
 * Peripheral sources 0..255 come first, then the RX FIFO notification (256) and
 * the controller's own fault sources. Source n sits in bit n % 32 of word n / 32
 * of both the mask bank and the ack/pending bank.
 */
#ifndef _SPMI_APPLE_IRQ_H
#define _SPMI_APPLE_IRQ_H

/* Generation 1 (T8103, T6000) */
#define APPLE_SPMI_GEN1_IRQ_MASK 0x20
#define APPLE_SPMI_GEN1_IRQ_ACK 0x60

/*
 * Generation 4 (T8142, T6050): ack and pending words from 0x600, the fault source
 * 263 acked at 0x620 bit 7 (traced from macOS under the hypervisor). The mask bank
 * is at 0x400 (see APPLE_SPMI_GEN4_IRQ_MASK).
 */
#define APPLE_SPMI_GEN4_IRQ_ACK 0x600
#define APPLE_SPMI_GEN4_FAULT_IRQ (256 + 7)
/* Mask bank mirrors the ack bank 0x200 below; +0x400..0x41c toggle on K114c, +0x420 does not. */
#define APPLE_SPMI_GEN4_IRQ_MASK 0x400

static inline u32 apple_spmi_irq_reg(u32 base, u32 irq)
{
	return base + (irq / 32) * 4;
}

static inline u32 apple_spmi_irq_bit(u32 irq)
{
	return BIT(irq % 32);
}

#endif /* _SPMI_APPLE_IRQ_H */
