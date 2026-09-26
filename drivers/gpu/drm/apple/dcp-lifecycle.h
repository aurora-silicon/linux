/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_DCP_LIFECYCLE_H__
#define __APPLE_DCP_LIFECYCLE_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>

#define APPLE_DCP_COPROC_CPU_CONTROL_RUN	BIT(4)
#define APPLE_DCP_COPROC_CPU_CONTROL_STOP_SECOND_CLEAR	BIT(5)
#define APPLE_DCP_COPROC_CPU_STATUS_RUNNING	BIT(0)
#define APPLE_DCP_COPROC_CPU_STATUS_STOPPED	BIT(1)

enum apple_dcp_coprocessor_start {
	APPLE_DCP_COPROCESSOR_COLD_START,
	APPLE_DCP_COPROCESSOR_ADOPT_AND_RESTART,
	APPLE_DCP_COPROCESSOR_QUIESCED_RESTART,
	APPLE_DCP_COPROCESSOR_IDLE_RESTART,
	APPLE_DCP_COPROCESSOR_ASC_STOPPED_RESTART,
};

enum apple_dcp_loader_handoff {
	APPLE_DCP_LOADER_HANDOFF_NONE,
	APPLE_DCP_LOADER_HANDOFF_RTKIT_QUIESCED,
	APPLE_DCP_LOADER_HANDOFF_RTKIT_IDLE,
	APPLE_DCP_LOADER_HANDOFF_ASC_STOPPED,
};

static inline bool apple_dcp_coprocessor_is_active(u32 cpu_control, u32 cpu_status)
{
	if (cpu_control & APPLE_DCP_COPROC_CPU_CONTROL_RUN ||
	    cpu_status & APPLE_DCP_COPROC_CPU_STATUS_RUNNING)
		return true;

	return !(cpu_status & APPLE_DCP_COPROC_CPU_STATUS_STOPPED);
}

static inline enum apple_dcp_coprocessor_start
apple_dcp_coprocessor_start_mode(enum apple_dcp_loader_handoff handoff,
				 bool can_adopt,
				 u32 cpu_control, u32 cpu_status)
{
	if (handoff == APPLE_DCP_LOADER_HANDOFF_ASC_STOPPED)
		return APPLE_DCP_COPROCESSOR_ASC_STOPPED_RESTART;
	if (handoff == APPLE_DCP_LOADER_HANDOFF_RTKIT_IDLE)
		return APPLE_DCP_COPROCESSOR_IDLE_RESTART;
	if (handoff == APPLE_DCP_LOADER_HANDOFF_RTKIT_QUIESCED)
		return APPLE_DCP_COPROCESSOR_QUIESCED_RESTART;

	if (can_adopt &&
	    apple_dcp_coprocessor_is_active(cpu_control, cpu_status))
		return APPLE_DCP_COPROCESSOR_ADOPT_AND_RESTART;

	return APPLE_DCP_COPROCESSOR_COLD_START;
}

static inline bool
apple_dcp_needs_adoption(enum apple_dcp_coprocessor_start start_mode)
{
	return start_mode == APPLE_DCP_COPROCESSOR_ADOPT_AND_RESTART;
}

static inline bool
apple_dcp_needs_fresh_boot(enum apple_dcp_coprocessor_start start_mode)
{
	return start_mode == APPLE_DCP_COPROCESSOR_ASC_STOPPED_RESTART;
}

static inline u32 apple_dcp_coprocessor_stop_first(u32 control)
{
	return control & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN;
}

static inline u32 apple_dcp_coprocessor_stop_second(u32 control)
{
	return control & ~APPLE_DCP_COPROC_CPU_CONTROL_STOP_SECOND_CLEAR;
}

#endif /* __APPLE_DCP_LIFECYCLE_H__ */
