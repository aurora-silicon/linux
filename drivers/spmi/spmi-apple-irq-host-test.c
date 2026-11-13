// SPDX-License-Identifier: GPL-2.0
/*
 * Host check for the Apple SPMI interrupt register layout (not built by kbuild):
 *   cc -Wall -Werror -o /tmp/spmi-irq-test drivers/spmi/spmi-apple-irq-host-test.c && /tmp/spmi-irq-test
 *
 * The expected values are the layouts measured on hardware or traced from macOS:
 * generation 1 (T8103/T6000): mask words from 0x20, ack/pending words from 0x60;
 * generation 4 (T8142/T6050): ack/pending words from 0x600, source n in bit n % 32
 * of word n / 32, the controller's fault source 263 acked at 0x620 bit 7.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t u32;
#define BIT(n) (1U << (n))

#include "spmi-apple-irq.h"

int main(void)
{
	/* generation 1 */
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN1_IRQ_MASK, 3) == 0x20);
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN1_IRQ_ACK, 3) == 0x60);
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN1_IRQ_ACK, 40) == 0x64);
	assert(apple_spmi_irq_bit(40) == BIT(8));

	/* generation 4: the PMU alarm source seen on K114c, the hpm2 wake bit macOS acks, the fault source */
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN4_IRQ_ACK, 2) == 0x600);
	assert(apple_spmi_irq_bit(2) == BIT(2));
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN4_IRQ_ACK, 19) == 0x600);
	assert(apple_spmi_irq_bit(19) == BIT(19));
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN4_IRQ_ACK, APPLE_SPMI_GEN4_FAULT_IRQ) == 0x620);
	assert(apple_spmi_irq_bit(APPLE_SPMI_GEN4_FAULT_IRQ) == BIT(7));
	/* no hardware mask bank for gen 4 */
	assert(APPLE_SPMI_GEN4_IRQ_MASK == 0);
	/* the last peripheral source is in word 7 */
	assert(apple_spmi_irq_reg(APPLE_SPMI_GEN4_IRQ_ACK, 255) == 0x61c);

	puts("spmi-apple-irq host check: ok");
	return 0;
}
