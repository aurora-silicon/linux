// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Runtime switch for M5 execution-context sharing (exec_share.rs). It is read
 * when a logical queue is created, so changing it affects only queues created
 * afterwards. Rust module parameters cannot be written at run time.
 */
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

static bool m5_shared_context;
module_param(m5_shared_context, bool, 0644);
MODULE_PARM_DESC(m5_shared_context,
		 "M5: logical queues of one GPU VM created while set share one execution context (default off)");

bool asahi_g17_m5_shared_context(void);
bool asahi_g17_m5_shared_context(void)
{
	return READ_ONCE(m5_shared_context);
}
