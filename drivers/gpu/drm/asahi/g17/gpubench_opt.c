// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Measurement only: runtime switches for submission-path changes, so that
 * every variant can be compared on one boot. Each bit enables one change;
 * zero leaves the driver's behaviour unchanged. Bit 0 is a control that no
 * change reads.
 */

#include <linux/compiler.h>
#include <linux/moduleparam.h>

static unsigned int gpubench_opt;
module_param(gpubench_opt, uint, 0644);
MODULE_PARM_DESC(gpubench_opt, "Measurement: enabled submission-path changes (bit mask)");

unsigned int asahi_g17_gpubench_opt(void);
unsigned int asahi_g17_gpubench_opt(void)
{
	return READ_ONCE(gpubench_opt);
}
