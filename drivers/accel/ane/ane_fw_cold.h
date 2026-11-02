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
	unsigned int i;

	if ((s->ce_state & 3) || s->ce_error || s->control || s->status != 0x6a ||
	    s->rvbar != 0x10000000001ULL || s->pending)
		return false;
	for (i = 0; i < ARRAY_SIZE(s->scratch); i++)
		if (s->scratch[i])
			return false;
	/*
	 * Stream enables are not judged: the Linux DART driver enables every
	 * stream globally when it binds and isolates them through the per-stream
	 * TCRs, so they read set on every boot once the DARTs are bound.
	 */
	for (i = 0; i < ANE_FW_DARTS; i++) {
		if ((s->dart[i].version & 0xffff) != 0x203 ||
		    (s->dart[i].protect & BIT(0)) ||
		    (s->dart[i].lock & (BIT(0) | BIT(2))) || s->dart[i].error)
			return false;
	}
	return true;
}

static inline bool ane_fw_cold_valid(const struct ane_fw_cold_state *s, bool raw_boot)
{
	return ane_fw_cold_pmgr_valid(s, raw_boot) && ane_fw_cold_gate_valid(s) &&
	       ane_fw_cold_engine_valid(s);
}

/* Bytes of firmware memory from the TEXT DVA (boot argument +0x10). */
#define ANE_FW_SPAN 0x380000ULL

/*
 * The firmware owns ANE_FW_SPAN bytes from its TEXT DVA, but the bootloader
 * preload covers only TEXT and DATA. The firmware keeps runtime heap state in
 * the rest of the span (0x14000 bytes past DATA on the 26.6.2 build), which
 * the host supplies zeroed and device-writable before boot. Returns the tail
 * DVA and size (0 when DATA ends the span), or -ERANGE when the layout would
 * overlap DATA, is not in whole 16 KiB pages, or reaches the command pool.
 */
static inline int ane_fw_heap_tail(const u64 dva[2], const u64 size[2], u64 limit,
				   u64 *tail, u64 *tail_size)
{
	u64 start = dva[1] + size[1], end = dva[0] + ANE_FW_SPAN;

	if (dva[1] < dva[0] + size[0] || start < dva[1] || start > end ||
	    !IS_ALIGNED(start, SZ_16K) || !IS_ALIGNED(end, SZ_16K) || end > limit)
		return -ERANGE;
	*tail = start;
	*tail_size = end - start;
	return 0;
}

/* The exact /chosen/apple,ane-cold-boot value a raw loader boot publishes. */
#define ANE_FW_COLD_BOOT_EVIDENCE "raw-no-xnu-ane-untouched-v1"

/*
 * What the loader handed off, as read from the DT and the TEXT segment, and
 * the live state of the engine. marker is /chosen/apple,ane-cold-boot or
 * NULL; regions_ok says both preload reservations are enabled, no-map, not
 * reusable and outside System RAM; text_ok that TEXT matches the driver's
 * firmware pin; pmgr the PMGR words (the only engine-side state readable
 * whatever the power state); crash_kernel that this is a kdump kernel.
 */
struct ane_fw_handoff {
	const char *marker;
	bool preloaded;
	bool regions_ok;
	u64 phys[2], size[2], dva[2];
	bool text_ok;
	const struct ane_fw_cold_state *pmgr;
	bool crash_kernel;
};

/* The loader's part: raw-boot evidence, the filled reservations, the TEXT. */
static inline bool ane_fw_cold_handoff_loader_ok(const struct ane_fw_handoff *h,
						 const struct ane_fw_platform *soc)
{
	return h->marker && !strcmp(h->marker, ANE_FW_COLD_BOOT_EVIDENCE) &&
	       h->preloaded && h->regions_ok &&
	       ane_fw_preload_ranges_valid(soc, h->phys, h->size, h->dva) && h->text_ok;
}

/*
 * Start at boot only with the whole hand-off and a cold engine. kexec copies
 * /chosen, marker included, from the running DT, so the marker alone does
 * not prove this boot's state: the ANE CPU and sub-block must still read off
 * in PMGR (a kernel that ran the ANE keeps them on), and a kdump kernel is
 * always warm. Anything less leaves the engine alone.
 */
static inline bool ane_fw_cold_handoff_valid(const struct ane_fw_handoff *h,
					     const struct ane_fw_platform *soc)
{
	return ane_fw_cold_handoff_loader_ok(h, soc) && !h->crash_kernel && h->pmgr &&
	       ane_fw_cold_pmgr_valid(h->pmgr, true);
}

int ane_fw_staged_init(struct platform_device *pdev, const struct ane_fw_platform *soc);
int ane_fw_staged_bind(struct platform_device *pdev);
bool ane_fw_staged_bound(struct device *dev);
void ane_fw_staged_unbind(struct platform_device *pdev);
int ane_fw_staged_suspend(struct device *dev);
void ane_fw_staged_start(void);
bool ane_fw_staged_exit(void);

#endif /* __ANE_FW_COLD_H__ */
