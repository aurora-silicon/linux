/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_DART_H__
#define __LINUX_SOC_APPLE_DART_H__

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/types.h>

struct device;

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
#endif

#endif
