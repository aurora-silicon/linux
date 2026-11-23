/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_COLD_H__
#define __ANE_FW_COLD_H__

#include "ane_fw_platform.h"
#include "ane_fw_transport.h"

#define ANE_FW_DARTS 3

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

static inline bool ane_fw_cold_engine_valid(const struct ane_fw_cold_state *s,
					    const struct ane_fw_platform *soc)
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
		if ((s->dart[i].version & 0xffff) != soc->dart_revision ||
		    (s->dart[i].protect & BIT(0)) ||
		    (s->dart[i].lock & (BIT(0) | BIT(2))) || s->dart[i].error)
			return false;
	}
	return true;
}

/*
 * After our own power-down for system sleep the engine may have kept power
 * with its CPU stopped by its PMGR power state, so status, CE, scratch and
 * DAPF are not judged; what must hold is no DART fault and no DART lock
 * that blocks re-attaching SID0.
 */
static inline bool ane_fw_cold_engine_restartable(const struct ane_fw_cold_state *s,
						  const struct ane_fw_platform *soc)
{
	unsigned int i;

	for (i = 0; i < ANE_FW_DARTS; i++)
		if ((s->dart[i].version & 0xffff) != soc->dart_revision ||
		    (s->dart[i].protect & BIT(0)) ||
		    (s->dart[i].lock & (BIT(0) | BIT(2))) || s->dart[i].error)
			return false;
	return true;
}

/* ANE_SYS's actual power state (PMGR bits 7:4) reads off. */
static inline bool ane_fw_sys_off(u32 sys)
{
	return !(sys & 0xf0);
}

/*
 * A session quarantined by a hung request, powered off under its firmware
 * without a stop. down: our power-down of the quarantined engine began
 * (ANE_SYS was still held by us then, so no earlier OFF counts); sys_off:
 * genpd then reported ANE_SYS off with its PMGR actual state off; held: we
 * hold ANE_SYS or the child power states again.
 */
struct ane_fw_hung_proof {
	bool down, sys_off, held;
};

/*
 * The old firmware is gone, and the memory it could reach (pool, IPC, heap,
 * heap tail, DATA, programs closed while quarantined) may be reused, only
 * when ANE_SYS really went off after our power-down and the ANE CPU and
 * sub-block power states read off now (@pmgr, read from PMGR, which is
 * readable in any power state). Anything less keeps the quarantine.
 */
static inline bool ane_fw_hung_gone(const struct ane_fw_hung_proof *p,
				    const struct ane_fw_cold_state *pmgr)
{
	return p->down && p->sys_off && !p->held && ane_fw_cold_pmgr_valid(pmgr, true);
}

/*
 * How long a recovery still keeps ANE_SYS off, @min_ns from the OFF at
 * @off_at (boot time) given @now: the whole of it when the OFF time is
 * unknown or lies ahead.
 */
static inline u64 ane_fw_recover_off_wait(u64 off_at, u64 now, u64 min_ns)
{
	if (!off_at || now < off_at)
		return min_ns;
	return now - off_at >= min_ns ? 0 : min_ns - (now - off_at);
}

/*
 * After a recovery's cold boot, before any request (the replay of open
 * programs included) reaches the new firmware: ANE_SYS and the ANE CPU power
 * state still target and read on, no DART reports a fault, and the
 * transport, after serving what the firmware posted while it went quiet, has
 * not failed. Anything less fails the recovery. The sub-block power state and
 * CE are left to the firmware once it runs (it gates the engine itself), so
 * they are not judged.
 */
struct ane_fw_settled {
	u32 sys, cpu;
	u32 dart_error[ANE_FW_DARTS];
	int transport_error;
};

static inline bool ane_fw_recover_settled(const struct ane_fw_settled *s)
{
	unsigned int i;

	if ((s->sys & 0xff) != 0xff || (s->cpu & 0xff) != 0xff || s->transport_error)
		return false;
	for (i = 0; i < ANE_FW_DARTS; i++)
		if (s->dart_error[i])
			return false;
	return true;
}

/*
 * Runtime recoveries of a hung engine are limited. Each one powers the
 * engine off under its firmware and boots it again; a client that hangs it
 * again right away turns that into a storm of power cycles, and eight of them
 * within six seconds locked the whole SoC up on a J700. So:
 *
 * - the first recovery in @window_ns runs at once; each later one in the
 *   window waits @backoff_ns after the one before it, twice as long for each
 *   recovery already in the window, at most @window_ns;
 * - a recovery that would be the (@burst + 1)th within @window_ns trips the
 *   limit: no more runtime recoveries, the engine stays quarantined until a
 *   system sleep powers it off and boots it again.
 *
 * Every attempt counts, failed or not, from when it starts.
 */
#define ANE_FW_RECOVER_BURST_MAX 16

struct ane_fw_recover_policy {
	unsigned int burst;
	u64 window_ns, backoff_ns;
};

struct ane_fw_recover_limit {
	/* Start times of the latest attempts, a ring of count entries before next. */
	u64 at[ANE_FW_RECOVER_BURST_MAX];
	unsigned int next, count;
	bool tripped;
};

enum ane_fw_recover_verdict {
	ANE_FW_RECOVER_NOW,
	ANE_FW_RECOVER_LATER,
	ANE_FW_RECOVER_STOP,
};

/* Attempts that started less than @window_ns before @now, and the latest start. */
static inline unsigned int ane_fw_recover_recent(const struct ane_fw_recover_limit *l,
						 u64 now, u64 window_ns, u64 *last)
{
	unsigned int i, recent = 0;

	*last = 0;
	for (i = 0; i < l->count; i++) {
		u64 at = l->at[(l->next + ANE_FW_RECOVER_BURST_MAX - 1 - i) %
			       ANE_FW_RECOVER_BURST_MAX];

		if (at > now || now - at >= window_ns)
			break;
		if (!recent++)
			*last = at;
	}
	return recent;
}

/*
 * May a recovery start at @now? LATER sets *@wait_ns, after which to ask
 * again; STOP trips the limit, which stays tripped until
 * ane_fw_recover_reset().
 */
static inline enum ane_fw_recover_verdict
ane_fw_recover_admit(struct ane_fw_recover_limit *l, const struct ane_fw_recover_policy *p,
		     u64 now, u64 *wait_ns)
{
	unsigned int burst = clamp(p->burst, 1U, (unsigned int)ANE_FW_RECOVER_BURST_MAX);
	u64 last, delay = p->backoff_ns;
	unsigned int recent, i;

	*wait_ns = 0;
	if (l->tripped)
		return ANE_FW_RECOVER_STOP;
	recent = ane_fw_recover_recent(l, now, p->window_ns, &last);
	if (recent >= burst) {
		l->tripped = true;
		return ANE_FW_RECOVER_STOP;
	}
	if (!recent)
		return ANE_FW_RECOVER_NOW;
	for (i = 1; i < recent && delay < p->window_ns; i++)
		delay *= 2;
	delay = min(delay, p->window_ns);
	if (now - last >= delay)
		return ANE_FW_RECOVER_NOW;
	*wait_ns = last + delay - now;
	return ANE_FW_RECOVER_LATER;
}

/* A recovery attempt starts at @now. */
static inline void ane_fw_recover_note(struct ane_fw_recover_limit *l, u64 now)
{
	l->at[l->next] = now;
	l->next = (l->next + 1) % ANE_FW_RECOVER_BURST_MAX;
	if (l->count < ANE_FW_RECOVER_BURST_MAX)
		l->count++;
}

static inline void ane_fw_recover_reset(struct ane_fw_recover_limit *l)
{
	memset(l, 0, sizeof(*l));
}

/*
 * RVBAR (engine +0x1050000) and the TEXT entry: latched at TEXT (bit 0 set)
 * is ready; with bit 0 clear the entry may be written; latched at any other
 * entry is refused (never write RVBAR while bit 0 is set).
 */
enum ane_fw_rvbar {
	ANE_FW_RVBAR_LATCHED,
	ANE_FW_RVBAR_WRITABLE,
	ANE_FW_RVBAR_FOREIGN,
};

static inline enum ane_fw_rvbar ane_fw_rvbar_check(u64 rvbar, u64 text)
{
	if (rvbar == (text | 1))
		return ANE_FW_RVBAR_LATCHED;
	return rvbar & 1 ? ANE_FW_RVBAR_FOREIGN : ANE_FW_RVBAR_WRITABLE;
}

static inline bool ane_fw_cold_valid(const struct ane_fw_cold_state *s,
				     const struct ane_fw_platform *soc, bool raw_boot)
{
	return ane_fw_cold_pmgr_valid(s, raw_boot) && ane_fw_cold_gate_valid(s) &&
	       ane_fw_cold_engine_valid(s, soc);
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
int ane_fw_staged_runtime_suspend(struct device *dev);
int ane_fw_staged_runtime_resume(struct device *dev);
void ane_fw_staged_start(void);
bool ane_fw_staged_exit(void);

#endif /* __ANE_FW_COLD_H__ */
