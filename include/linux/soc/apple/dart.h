/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_DART_H__
#define __LINUX_SOC_APPLE_DART_H__

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/types.h>

struct device;
struct device_node;

/* Retained internal-display ownership is qualified only with a built-in
 * DART provider. Other consumers, including modular DART, keep the APIs below.
 */
struct device_node;

#if IS_ENABLED(CONFIG_APPLE_DART_J713_DCP)
/* J713 internal DCP only: requires validated, borrowed SID23 tables. */
int apple_dart_dcp_check(struct device *dev);
/* A borrowed RTKit payload must be fully writable, not inherited firmware TEXT. */
int apple_dart_dcp_check_buffer(struct device *dev, dma_addr_t iova, size_t size);
/* Retained CPU/DVA allocation: even failed publication pins pages until reboot. */
void *apple_dart_dcp_alloc(struct device *dev, size_t size, dma_addr_t *dva);

/* Qualified noncoherent J713 SID23/SID16 pair; no CPU access after publication. */
int apple_dart_dcp_shared_alloc(struct device *dev, struct device *piodma,
				size_t size, u64 *phys, u64 *dva);
int apple_dart_dcp_shared_map(struct device *dev, struct device *piodma,
			      u64 phys, u64 dva, size_t size);
/* SID23 only, never mapped in SID16 afterwards (J713 external DCP). */
int apple_dart_dcp_shared_alloc_link(struct device *dev, size_t size, u64 *phys, u64 *dva);
/* An existing backing in SID0/SID16 of an external display DART, at its own DVA. */
int apple_dart_dcp_dispext_map(struct device *piodma, unsigned int sid, u64 phys, size_t size,
			       u64 *dva);
/* Whether an external display DART stream is adopted and not retired. */
int apple_dart_dcp_dispext_stream_ok(struct device *piodma, unsigned int sid);
/* A host-written surface in SID0 of an external display DART (Normal-NC CPU view). */
void *apple_dart_dcp_dispext_scanout_alloc(struct device *piodma, size_t size, u64 *dva);
/* An adopted external display DART's state after its power-up; reads only. */
int apple_dart_dcp_dispext_state(struct device_node *consumer);
/*
 * Internal display DART of a DCP consumer (the DCP or its PIODMA), around a
 * power-down of the display domains by the DCP: streams off, flushed and
 * clock protection cleared before; state checked and streams back after.
 * The check says, changing nothing, whether the power-off can run now.
 */
int apple_dart_dcp_power_check(struct device *dev);
int apple_dart_dcp_power_off(struct device *dev);
int apple_dart_dcp_power_on(struct device *dev);

/*
 * Remappable host windows in SID0 of an external display DART: one
 * whole arena slice each, claimed until reboot, mapped and unmapped by the
 * host only (every change flushed and read back). Pages are the caller's.
 */
int apple_dart_dcp_dispext_window_claim(struct device *piodma, u64 *dva);
int apple_dart_dcp_dispext_window_map(struct device *piodma, u64 dva, const phys_addr_t *phys,
				      unsigned int count);
int apple_dart_dcp_dispext_window_unmap(struct device *piodma, u64 dva);
/* Power confirmed on by the caller: both frontends as adopted, no fault. */
int apple_dart_dcp_dispext_qualify(struct device *piodma);

/* Private retained SID0 scanout, not the firmware's capped D451 allocation class. */
void *apple_dart_dcp_scanout_alloc(struct device *dev, size_t size, dma_addr_t *dva);

#else
static inline int apple_dart_dcp_check(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_dcp_power_check(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_dcp_power_off(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_dcp_power_on(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_dcp_check_buffer(struct device *dev, dma_addr_t iova, size_t size)
{
	return -EOPNOTSUPP;
}
static inline void *apple_dart_dcp_alloc(struct device *dev, size_t size, dma_addr_t *dva)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline int apple_dart_dcp_shared_alloc(struct device *dev, struct device *piodma,
					    size_t size, u64 *phys, u64 *dva)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_dcp_shared_map(struct device *dev, struct device *piodma,
					  u64 phys, u64 dva, size_t size)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_shared_alloc_link(struct device *dev, size_t size, u64 *phys,
						   u64 *dva)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_map(struct device *piodma, unsigned int sid, u64 phys,
					     size_t size, u64 *dva)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_state(struct device_node *consumer)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_window_claim(struct device *piodma, u64 *dva)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_window_map(struct device *piodma, u64 dva,
						    const phys_addr_t *phys, unsigned int count)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_window_unmap(struct device *piodma, u64 dva)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_qualify(struct device *piodma)
{
	return -EOPNOTSUPP;
}

static inline int apple_dart_dcp_dispext_stream_ok(struct device *piodma, unsigned int sid)
{
	return -EOPNOTSUPP;
}

static inline void *apple_dart_dcp_dispext_scanout_alloc(struct device *piodma, size_t size,
							 u64 *dva)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void *apple_dart_dcp_scanout_alloc(struct device *dev, size_t size, dma_addr_t *dva)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif

/*
 * The tunneled PCIe IOMMU shares the port clock. Stop issuing commands
 * before that clock is gated, and allow them again once it is back.
 */
#if IS_ENABLED(CONFIG_APPLE_DART)
int apple_dart_quiesce_commands(struct device *dev);
int apple_dart_resume_commands(struct device *dev);
int apple_dart_save_tunnel_state(struct device *dev);
int apple_dart_restore_tunnel_state(struct device *dev);
/* Registered J713 managed radio provider, with no owned valid root or fault. */
int apple_dart_radio_check_idle(struct device *dev);
/* A firmware-owned buffer's address for a host copy; Linux's own mappings refused. */
int apple_dart_fw_remote(struct device *dev, dma_addr_t iova, bool write, phys_addr_t *phys);
/*
 * A DART clocked by its host's port: after a real sleep it stays gated until the host has
 * set the port up and calls apple_dart_host_resume().
 */
int apple_dart_set_host_resumes(struct device *dev);
int apple_dart_host_resume(struct device *dev);
#else
static inline int apple_dart_quiesce_commands(struct device *dev) { return 0; }
static inline int apple_dart_resume_commands(struct device *dev) { return 0; }
static inline int apple_dart_save_tunnel_state(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_restore_tunnel_state(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_radio_check_idle(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_fw_remote(struct device *dev, dma_addr_t iova, bool write,
				       phys_addr_t *phys)
{
	return -EOPNOTSUPP;
}
static inline int apple_dart_set_host_resumes(struct device *dev) { return -EOPNOTSUPP; }
static inline int apple_dart_host_resume(struct device *dev) { return -EOPNOTSUPP; }
#endif

/* For a consumer whose firmware gates itself around a firmware-driven suspend. */
#if IS_REACHABLE(CONFIG_APPLE_DART)
bool apple_dart_sleep_listed(struct device *dev);
int apple_dart_sleep_prepare(struct device *dev);
void apple_dart_sleep_restore(struct device *dev);
/* A display DART's streams around a display power cycle, see the DART driver. */
int apple_dart_display_streams(struct device *dev, u32 streams, bool on, bool flush);
/* A locked display DART's TCR and TTBR words kept across a real sleep (T6041). */
int apple_dart_display_keep(struct device *dev, u32 streams, bool save);
/* A GAPF slice released before its coprocessor's CPU domain comes up after a wake. */
int apple_dart_gapf_ready(struct device *dev);
/* A display DART whose domain goes off at runtime while others still map into it. */
void apple_dart_power_dark(struct device *dev, bool dark);
/* A DART's state kept across its consumer's own power cycle, outside a system sleep. */
int apple_dart_state_save(struct device *dev);
int apple_dart_state_restore(struct device *dev);
void apple_dart_sleep_restore_lost(struct device *dev, bool lost);
#else
static inline bool apple_dart_sleep_listed(struct device *dev)
{
	return false;
}

static inline int apple_dart_sleep_prepare(struct device *dev)
{
	return -ENODEV;
}

static inline void apple_dart_sleep_restore(struct device *dev)
{
}

static inline int apple_dart_display_streams(struct device *dev, u32 streams, bool on,
					     bool flush)
{
	return -ENODEV;
}

static inline int apple_dart_display_keep(struct device *dev, u32 streams, bool save)
{
	return -ENODEV;
}

static inline int apple_dart_gapf_ready(struct device *dev)
{
	return 0;
}

static inline void apple_dart_power_dark(struct device *dev, bool dark)
{
}

static inline int apple_dart_state_save(struct device *dev)
{
	return -ENODEV;
}

static inline int apple_dart_state_restore(struct device *dev)
{
	return -ENODEV;
}

static inline void apple_dart_sleep_restore_lost(struct device *dev, bool lost)
{
}
#endif

/*
 * A DART marked apple,manual-availability answers only once its client (a
 * PCIe root complex) is up. The client makes it available, and it probes then.
 */
#if IS_ENABLED(CONFIG_APPLE_DART)
void apple_dart_make_available(struct device_node *np);
#else
static inline void apple_dart_make_available(struct device_node *np) { }
#endif

#endif
