/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_COLD_H__
#define __ANE_FW_COLD_H__

#include "ane_fw_transport.h"

#define ANE_FW_DARTS 3
#define ANE_FW_DAPF_RANGES 4

struct ane_fw_cold_state {
	u32 sys, cpu, subblock, gate_request, gate_ack;
	u64 ce_state, ce_error, rvbar;
	u32 control, status, pending, scratch[8];
	struct {
		u32 version, protect, unprotect, lock, error;
		u32 enabled[16];
	} dart[ANE_FW_DARTS];
};

/*
 * Cold acquisition is proved in three steps, each read only once its block is
 * known to be powered, in the order the H17 U2 runs measured as safe:
 *
 * 1. PMGR, readable with ANE_SYS in either state: the ANE CPU and sub-block
 *    power states are off. ANE_SYS itself may read on or off (fact 5: it can
 *    be on while genpd reports off, and generic DART runtime PM turns it off),
 *    so it is no marker.
 * 2. PWGATE, after ANE_SYS is on: the gate is requested and not acknowledged.
 * 3. The engine and its DARTs, only after the child power step: CE, CPU and
 *    wrapper reset values, and unlocked, unfaulted DARTs with no stream on.
 *    Nothing in the engine has been written by Linux at that point.
 *
 * DAPF records and invalid TTBR payloads are random at reset, not markers.
 */
static inline bool ane_fw_cold_pmgr_valid(const struct ane_fw_cold_state *s, bool raw_boot)
{
	return raw_boot && !(s->cpu & 0xff) && s->subblock == 0x300;
}

static inline bool ane_fw_cold_gate_valid(const struct ane_fw_cold_state *s)
{
	return s->gate_request == 3 && !s->gate_ack;
}

static inline bool ane_fw_cold_engine_valid(const struct ane_fw_cold_state *s)
{
	unsigned int i, j;

	if ((s->ce_state & 3) || s->ce_error || s->control || s->status != 0x6a ||
	    s->rvbar != 0x10000000001ULL || s->pending)
		return false;
	for (i = 0; i < ARRAY_SIZE(s->scratch); i++)
		if (s->scratch[i])
			return false;
	for (i = 0; i < ANE_FW_DARTS; i++) {
		if ((s->dart[i].version & 0xffff) != 0x203 ||
		    (s->dart[i].protect & BIT(0)) ||
		    (s->dart[i].lock & (BIT(0) | BIT(2))) || s->dart[i].error)
			return false;
		for (j = 0; j < ARRAY_SIZE(s->dart[i].enabled); j++)
			if (s->dart[i].enabled[j])
				return false;
	}
	return true;
}

static inline bool ane_fw_cold_valid(const struct ane_fw_cold_state *s, bool raw_boot)
{
	return ane_fw_cold_pmgr_valid(s, raw_boot) && ane_fw_cold_gate_valid(s) &&
	       ane_fw_cold_engine_valid(s);
}

int ane_fw_staged_init(struct platform_device *pdev, const struct ane_fw_platform *soc);
bool ane_fw_staged_exit(void);

#endif /* __ANE_FW_COLD_H__ */
