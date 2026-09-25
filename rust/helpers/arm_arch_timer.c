// SPDX-License-Identifier: GPL-2.0

#include <clocksource/arm_arch_timer.h>

/*
 * Named apart from the C symbols: the arm64 headers already declare
 * arch_timer_read_counter (a function pointer) and arch_timer_get_rate
 * in the generated bindings.
 */
__rust_helper u64 rust_helper_arch_timer_counter(void)
{
	return arch_timer_read_counter();
}

__rust_helper u32 rust_helper_arch_timer_rate(void)
{
	return arch_timer_get_rate();
}
