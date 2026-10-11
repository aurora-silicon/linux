// SPDX-License-Identifier: GPL-2.0-only
/* Apple DART IOMMU backend using Aura's native SPTM call transport.
 * Linux owns stream topology, translation-table setup, DMA apertures and
 * transactional mapping bookkeeping. Aura forwards native selectors 1:1.
 */

#include <linux/apple-dart-apif.h>
#include <linux/soc/aurora/apif-mmio.h>
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#include "dma-iommu.h"

/* Internal operations used by the IOMMU bookkeeping; not the APIF wire ABI. */
#define APIF_DART_INIT 1 /* ()                       INIT + POWERUP */
#define APIF_DART_ATTACH 2 /* (sid, dva, level) */
#define APIF_DART_MAP 3 /* (sid, dva, ipa, size, prot) */
#define APIF_DART_UNMAP 4 /* (sid, dva, size) */
#define APIF_DART_ENABLE 5 /* (sid) */
#define APIF_DART_DISABLE 6 /* (sid) */

#define APIF_DART_RETAINED_INFO 9
#define APIF_DART_RETAINED_RUN 10

struct apif_op {
	u32 op;
	u32 flags;
	u64 arg[6];
	u64 ret;
};

struct apif_batch {
	u32 count;
	u32 flags;
	s32 status;
	u32 done;
	struct apif_op op[];
};

#define DART_APIF_PAGE_SHIFT 14
#define DART_APIF_PAGE_SIZE (1UL << DART_APIF_PAGE_SHIFT) /* 16 KiB */
/* apif's DART MAP forwards one contiguous run within a single 32 MiB leaf-table
 * span, so every MAP op must stay inside one aligned block.
 */
#define DART_APIF_BLOCK_SHIFT 25
#define DART_APIF_BLOCK_SIZE (1UL << DART_APIF_BLOCK_SHIFT) /* 32 MiB */

#define DART_APIF_MAX_STREAMS 256
#define DART_APIF_APERTURE_END (SZ_4G - 1)
#define DART_APIF_NATIVE_APERTURE_END GENMASK_ULL(41, 0)

/* Preserve batched per-block operations and the existing rollback accounting. */
#define APIF_BATCH_MAX_OPS \
	((PAGE_SIZE - sizeof(struct apif_batch)) / sizeof(struct apif_op))

static const struct iommu_ops apple_dart_apif_ops;

struct apif_retained_run {
	u64 dva, ipa, size;
};
struct apif_retained_sid {
	unsigned int root_level, count;
	struct apif_retained_run *runs;
};

struct apple_dart_apif {
	struct device *dev;
	struct iommu_device iommu;
	struct aurora_apif *apif;
	struct aurora_apif_op *native_ops;
	struct aurora_apif_boot_iommu *boot_info;
	u64 *page_lists[APIF_BATCH_MAX_OPS];
	u64 page_list_pa[APIF_BATCH_MAX_OPS];
	u32 dart_id;
	u32 num_streams;
	u64 iova_force; /* Native address force mask, hidden from DMA API/device addresses. */
	u64 dma_min, dma_max;
	bool reserve_last_page;
	bool inited;
	bool ownership_uncertain; /* A table operation needs a fresh boot after lost completion. */
	struct apif_retained_sid *retained[DART_APIF_MAX_STREAMS];
	struct mutex init_lock; /* serialises one-time init */
	/* Protected by apple_dart_apif_groups_lock; group-owned lifetime. */
	struct iommu_group *sid2group[DART_APIF_MAX_STREAMS];

	struct apif_batch *batch; /* internal mapping-operation buffer */
	spinlock_t batch_lock; /* protects operation buffers and stream updates */
};

/* Match the native DART driver's capacity for shared display DMA domains. */
#define APIF_MAX_DARTS 8

struct apif_stream_map {
	struct apple_dart_apif *dart;
	DECLARE_BITMAP(sids, DART_APIF_MAX_STREAMS);
};

struct apple_dart_apif_master {
	unsigned int num_darts;
	struct apif_stream_map streams[APIF_MAX_DARTS];
};

struct apple_dart_apif_domain {
	struct iommu_domain domain;
	unsigned int num_darts;
	struct apif_stream_map streams[APIF_MAX_DARTS];
	struct mutex lock; /* serialises domain attachments */
	/* Mapping calls may run concurrently on distinct ranges. An attachment
	 * must replay a stable mapping set before publishing its new streams.
	 */
	rwlock_t streams_lock;
	/* Key: dart-id:8, sid:8, block:40. SPTM tables cannot be attached twice. */
	struct xarray l2_attached;
	bool has_retained;
	/* One canonical mapping set and IOVA allocator for every DART. */
	struct xarray mappings;
};

static struct apif_stream_map *
apif_find_stream(struct apple_dart_apif_domain *dom,
		 struct apple_dart_apif *dart)
{
	if (dom)
		for (unsigned int i = 0; i < dom->num_darts; i++)
			if (dom->streams[i].dart == dart)
				return &dom->streams[i];
	return NULL;
}

/* Domain topology is fixed before the first attachment. Later devices can
 * add SIDs on these DARTs, but cannot change an active allocator's aperture.
 */
static int apif_domain_configure(struct apple_dart_apif_domain *dom,
				 const struct apple_dart_apif_master *master)
{
	u64 start = 0, end = DART_APIF_NATIVE_APERTURE_END;

	if (!master || !master->num_darts)
		return -ENODEV;
	if (dom->num_darts) {
		for (unsigned int i = 0; i < master->num_darts; i++)
			if (!apif_find_stream(dom, master->streams[i].dart))
				return -EINVAL;
		return 0;
	}
	for (unsigned int i = 0; i < master->num_darts; i++) {
		start = max(start, master->streams[i].dart->dma_min);
		end = min(end, master->streams[i].dart->dma_max);
	}
	if (start > end)
		return -EINVAL;
	for (unsigned int i = 0; i < master->num_darts; i++)
		dom->streams[i].dart = master->streams[i].dart;
	dom->domain.geometry.aperture_start = start;
	dom->domain.geometry.aperture_end = end;
	dom->num_darts = master->num_darts;
	return 0;
}

static struct apple_dart_apif_domain *to_apif_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_apif_domain, domain);
}

/* The native DMA address space strips the force bits before returning a device DMA address
 * and restores it for DART operations. Translate only the bounded transport
 * batch; all live-map bookkeeping and the DMA aperture remain device-visible.
 * The force bits are disjoint from the validated low-4-GiB aperture.
 */
static void apif_force_iovas(struct apple_dart_apif *dart, bool restore)
{
	if (!dart->iova_force)
		return;
	for (u32 i = 0; i < dart->batch->count; i++) {
		struct apif_op *op = &dart->batch->op[i];

		switch (op->op) {
		case APIF_DART_ATTACH:
		case APIF_DART_MAP:
		case APIF_DART_UNMAP:
			if (restore)
				op->arg[1] &= ~dart->iova_force;
			else
				op->arg[1] |= dart->iova_force;
			break;
		}
	}
}

static void apif_put_transport(void *data)
{
	aurora_apif_put(data);
}

static void apif_free_page_lists(void *data)
{
	struct apple_dart_apif *dart = data;

	for (unsigned int i = 0; i < APIF_BATCH_MAX_OPS; i++)
		kfree(dart->page_lists[i]);
}

static int apif_one(struct apple_dart_apif *dart, struct aurora_apif_op *op)
{
	return aurora_apif_submit(dart->apif, op, 1, NULL);
}

static int apif_prepare_table(struct apple_dart_apif *dart, u64 sid, u64 dva,
			      u64 level)
{
	u64 base = level ? round_down(dva, 1ULL << (level == 1 ? 36 : 25)) : 0;
	u64 owner = 0x4441525400000000ULL | ((u64)dart->dart_id << 16) |
		    (sid << 8) | level;
	struct aurora_apif_op op = {
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_BOOT_TABLE),
		.arg = { AURORA_APIF_BOOT_DART, dart->dart_id, sid, dva,
			 level },
	};
	u64 pa;
	int ret;

	if (sid >= dart->num_streams || level > 2)
		return -EINVAL;
	if (level == 2 && dart->retained[sid] &&
	    !dart->retained[sid]->root_level) {
		ret = apif_prepare_table(dart, sid, dva, 1);
		if (ret)
			return ret;
	}
	ret = apif_one(dart, &op);
	if (ret || op.ret)
		return ret;
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_FIND_FRAME),
		.arg = { owner, base },
	};
	ret = apif_one(dart, &op);
	if (ret || op.ret)
		return ret;
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_ALLOC_FRAME),
	};
	ret = apif_one(dart, &op);
	if (ret)
		return ret;
	pa = op.ret;
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(0, 1), /* RETYPE */
		.arg = { pa, 11, 24,
			 (((level << 16) | (sid << 8) | dart->dart_id) << 32) |
				 5 },
	};
	ret = apif_one(dart, &op);
	if (ret) {
		dart->ownership_uncertain = true;
		return ret; /* Retain the frame until a fresh boot. */
	}
	if (!op.ret) {
		op = (struct aurora_apif_op){
			.selector = AURORA_APIF_SELECTOR(
				AURORA_APIF_SERVICE, AURORA_APIF_FREE_FRAME),
			.arg = { pa },
		};
		apif_one(dart, &op);
		return -EIO;
	}
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(3, 0), /* DART MAP_TABLE */
		.arg = { dart->dart_id, sid, dva, level, pa },
	};
	ret = apif_one(dart, &op);
	if (ret) {
		dart->ownership_uncertain = true;
		return ret;
	}
	/* Publish the opaque ownership key only after the table is attached. */
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_TAG_FRAME),
		.arg = { pa, owner, base },
	};
	ret = apif_one(dart, &op);
	if (ret)
		dart->ownership_uncertain = true;
	return ret;
}

static int apif_boot_query(struct apple_dart_apif *dart,
			   struct apif_op *request)
{
	struct aurora_apif_boot_iommu *info = dart->boot_info;
	struct aurora_apif_op op = {
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_BOOT_IOMMU),
		.arg = { AURORA_APIF_BOOT_DART, dart->dart_id, request->arg[0],
			 request->op == APIF_DART_RETAINED_INFO ?
				 U64_MAX :
				 request->arg[1],
			 virt_to_phys(info), sizeof(*info) },
	};
	int ret = apif_one(dart, &op);

	if (ret)
		return ret;
	if (request->op == APIF_DART_RETAINED_INFO) {
		request->arg[1] = info->level;
		request->arg[2] = info->count;
		request->arg[3] = info->base;
		request->arg[4] = info->size;
		request->arg[5] = 1;
	} else {
		request->arg[2] = info->base;
		request->arg[3] = info->ipa;
		request->arg[4] = info->size;
		request->arg[5] = info->attrs;
	}
	return 0;
}

/* Build native calls in Linux, including address lists and permission flags.
 * Address translations form a separate batch, so failure cannot send a MAP
 * with an untranslated pointer. Mapping batches keep the original capacity
 * and prefix-completion accounting used by rollback. Caller holds batch_lock.
 */
static int apif_ring(struct apple_dart_apif *dart)
{
	struct aurora_apif_op *native = dart->native_ops;
	u32 count = dart->batch->count, n = 0, translated = 0, done = 0;
	u32 ends[APIF_BATCH_MAX_OPS];
	int ret = 0;

	dart->batch->done = 0;
	if (dart->ownership_uncertain) {
		dart->batch->status = -EIO;
		return -EIO;
	}
	if (count > APIF_BATCH_MAX_OPS)
		return -EINVAL;
	apif_force_iovas(dart, false);
	for (u32 i = 0; i < count; i++) {
		struct apif_op *op = &dart->batch->op[i];

		if (op->flags) {
			ret = -EINVAL;
			goto out;
		}
		if (op->op == APIF_DART_MAP) {
			if (op->arg[4] &
			    ~(IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |
			      IOMMU_NOEXEC | IOMMU_MMIO)) {
				ret = -EINVAL;
				goto out;
			}
			if (!dart->page_lists[i])
				dart->page_lists[i] =
					kmalloc(SZ_16K, GFP_ATOMIC);
			if (!dart->page_lists[i]) {
				ret = -ENOMEM;
				goto out;
			}
			native[translated++] = (struct aurora_apif_op){
				.selector = AURORA_APIF_SELECTOR(
					AURORA_APIF_SERVICE,
					AURORA_APIF_TRANSLATE),
				.arg = { op->arg[2], op->arg[3] },
			};
			native[translated++] = (struct aurora_apif_op){
				.selector = AURORA_APIF_SELECTOR(
					AURORA_APIF_SERVICE,
					AURORA_APIF_TRANSLATE),
				.arg = { virt_to_phys(dart->page_lists[i]),
					 SZ_16K },
			};
		}
	}
	if (translated) {
		ret = aurora_apif_submit(dart->apif, native, translated, NULL);
		if (ret)
			goto out;
		translated = 0;
		for (u32 i = 0; i < count; i++)
			if (dart->batch->op[i].op == APIF_DART_MAP) {
				dart->batch->op[i].arg[5] =
					native[translated++].ret;
				dart->page_list_pa[i] =
					native[translated++].ret;
			}
	}
	for (u32 i = 0; i < count; i++) {
		struct apif_op *op = &dart->batch->op[i];
		u64 *a = op->arg;
		struct aurora_apif_op call = {};

		call.arg[0] = dart->dart_id;
		call.arg[1] = a[0];
		op->ret = 0;
		switch (op->op) {
		case APIF_DART_INIT: {
			struct aurora_apif_op query = {
				.selector = AURORA_APIF_SELECTOR(
					AURORA_APIF_SERVICE,
					AURORA_APIF_BOOT_TABLE),
				.arg = { AURORA_APIF_BOOT_DART, dart->dart_id,
					 0, 0, U64_MAX },
			};

			ret = apif_one(dart, &query);
			if (ret)
				goto out;
			if (!query.ret) {
				call.selector =
					AURORA_APIF_SELECTOR(3, 6); /* INIT */
				native[n++] = call;
				call.selector = AURORA_APIF_SELECTOR(
					3, 5); /* POWERUP */
				native[n++] = call;
			}
			break;
		}
		case APIF_DART_ATTACH:
			ret = apif_prepare_table(dart, a[0], a[1], a[2]);
			if (ret)
				goto out;
			break;
		case APIF_DART_RETAINED_INFO:
		case APIF_DART_RETAINED_RUN:
			ret = apif_boot_query(dart, op);
			if (ret)
				goto out;
			break;
		case APIF_DART_MAP: {
			u64 pa = a[5], size = a[3];

			if (!size || size > DART_APIF_BLOCK_SIZE ||
			    ((a[1] | pa | size) & (DART_APIF_PAGE_SIZE - 1)) ||
			    (a[1] >> 25) != ((a[1] + size - 1) >> 25)) {
				ret = -EINVAL;
				goto out;
			}
			for (u64 p = 0; p < size / DART_APIF_PAGE_SIZE; p++)
				dart->page_lists[i][p] =
					pa + p * DART_APIF_PAGE_SIZE;
			call.selector = AURORA_APIF_SELECTOR(3, 2); /* MAP */
			call.arg[2] = a[1];
			call.arg[3] = dart->page_list_pa[i];
			call.arg[4] = size;
			call.arg[5] = (~a[4]) & 7;
			native[n++] = call;
			break;
		}
		case APIF_DART_UNMAP:
			call.selector = AURORA_APIF_SELECTOR(3, 3);
			call.arg[2] = a[1];
			call.arg[3] = a[2];
			call.arg[4] = 2;
			native[n++] = call;
			break;
		case APIF_DART_ENABLE:
		case APIF_DART_DISABLE:
			call.selector = AURORA_APIF_SELECTOR(
				3, op->op == APIF_DART_ENABLE ? 8 : 7);
			native[n++] = call;
			break;
		case 7: /* POWERUP */
		case 8: /* POWERDOWN */
			for (u32 sid = 0; sid < dart->num_streams; sid++)
				if (dart->retained[sid]) {
					if (op->op == 8) {
						ret = -EBUSY;
						goto out;
					}
					goto prepared;
				}
			call.selector =
				AURORA_APIF_SELECTOR(3, op->op == 7 ? 5 : 4);
			native[n++] = call;
			break;
		default:
			ret = -EINVAL;
			goto out;
		}
prepared:
		ends[i] = n;
	}
	ret = aurora_apif_submit(dart->apif, native, n, &done);
	for (u32 i = 0; i < count && ends[i] <= done; i++)
		dart->batch->done = i + 1;
out:
	apif_force_iovas(dart, true);
	dart->batch->status = ret;
	return ret;
}

/* Setup-only host queries. Firmware-owned mappings are reserved, never replayed
 * through MAP, and never inserted in the Linux-owned mappings xarray.
 */
static int apif_load_retained(struct apple_dart_apif *dart)
{
	struct device_node *np = dart->dev->of_node;
	int count = of_property_count_u32_elems(np, "apple,apif-retained-sids");
	int ret;

	if (!of_find_property(np, "apple,apif-retained-sids", NULL))
		return 0;
	if (count < 1 || count > dart->num_streams || dart->iova_force)
		return -EINVAL;
	for (int i = 0; i < count; i++) {
		struct apif_retained_sid *r;
		u32 sid;
		u64 previous = 0;
		struct apif_op *op = &dart->batch->op[0];

		if (of_property_read_u32_index(np, "apple,apif-retained-sids",
					       i, &sid) ||
		    sid >= dart->num_streams || dart->retained[sid])
			return -EINVAL;
		dart->batch->count = 1;
		*op = (struct apif_op){ .op = APIF_DART_RETAINED_INFO,
					.arg = { sid } };
		ret = apif_ring(dart);
		if (ret)
			return ret;
		if (op->arg[1] > 1 || op->arg[2] > 131072 || op->arg[5] != 1 ||
		    op->arg[3] != dart->dma_min ||
		    op->arg[4] != dart->dma_max - dart->dma_min + 1)
			return -EINVAL;
		r = devm_kzalloc(dart->dev, sizeof(*r), GFP_KERNEL);
		if (!r)
			return -ENOMEM;
		r->root_level = op->arg[1];
		r->count = op->arg[2];
		r->runs = devm_kcalloc(dart->dev, r->count, sizeof(*r->runs),
				       GFP_KERNEL);
		if (r->count && !r->runs)
			return -ENOMEM;
		for (u64 j = 0; j < r->count; j++) {
			struct apif_retained_run *run = &r->runs[j];

			*op = (struct apif_op){ .op = APIF_DART_RETAINED_RUN,
						.arg = { sid, j } };
			ret = apif_ring(dart);
			if (ret)
				return ret;
			run->dva = op->arg[2];
			run->ipa = op->arg[3];
			run->size = op->arg[4];
			if (!run->size ||
			    (run->dva | run->size) &
				    (DART_APIF_PAGE_SIZE - 1) ||
			    run->dva < dart->dma_min ||
			    run->dva > dart->dma_max ||
			    run->size - 1 > dart->dma_max - run->dva ||
			    (j && run->dva < previous) ||
			    (run->ipa != U64_MAX &&
			     ((run->ipa & (DART_APIF_PAGE_SIZE - 1)) ||
			      run->size > U64_MAX - run->ipa)))
				return -EINVAL;
			previous = run->dva + run->size;
		}
		dart->retained[sid] = r;
	}
	return 0;
}

static const struct apif_retained_run *
apif_retained_lookup(const struct apif_retained_sid *r, u64 dva, u64 size)
{
	unsigned int low = 0, high = r->count;

	while (low < high) {
		unsigned int mid = low + (high - low) / 2;
		const struct apif_retained_run *run = &r->runs[mid];

		if (run->dva + run->size <= dva)
			low = mid + 1;
		else
			high = mid;
	}
	return low < r->count && r->runs[low].dva < dva + size ? &r->runs[low] :
								 NULL;
}

static bool apif_retained_overlap(struct apple_dart_apif_domain *dom, u64 dva,
				  u64 size)
{
	unsigned int sid;

	if (!dom->has_retained)
		return false;
	for (unsigned int i = 0; i < dom->num_darts; i++) {
		struct apif_stream_map *stream = &dom->streams[i];

		for_each_set_bit(sid, stream->sids, stream->dart->num_streams) {
			struct apif_retained_sid *r =
				stream->dart->retained[sid];

			if (r && apif_retained_lookup(r, dva, size))
				return true;
		}
	}
	return false;
}

static phys_addr_t apif_retained_translate(struct apple_dart_apif_domain *dom,
					   u64 dva)
{
	unsigned int sid;
	u64 result = 0;

	if (!dom->has_retained)
		return 0;
	/* Firmware may retain different mappings on different display engines.
	 * Only a unanimous translation is meaningful for their shared domain.
	 */
	for (unsigned int i = 0; i < dom->num_darts; i++) {
		struct apif_stream_map *stream = &dom->streams[i];

		for_each_set_bit(sid, stream->sids, stream->dart->num_streams) {
			struct apif_retained_sid *r =
				stream->dart->retained[sid];
			const struct apif_retained_run *run =
				r ? apif_retained_lookup(r, dva, 1) : NULL;
			u64 pa;

			if (!run || dva < run->dva || run->ipa == U64_MAX)
				return 0;
			pa = run->ipa + dva - run->dva;
			if (result && result != pa)
				return 0;
			result = pa;
		}
	}
	return result;
}

/* Bring the DART up in SPTM exactly once, before any stream is enabled. */
static int apple_dart_apif_ensure_init(struct apple_dart_apif *dart)
{
	int ret = 0;

	mutex_lock(&dart->init_lock);
	if (!dart->inited) {
		unsigned long flags;

		spin_lock_irqsave(&dart->batch_lock, flags);
		dart->batch->count = 1;
		dart->batch->op[0] = (struct apif_op){ .op = APIF_DART_INIT };
		ret = apif_ring(dart);
		spin_unlock_irqrestore(&dart->batch_lock, flags);
		if (ret)
			ret = -EIO;
		else
			dart->inited = true;
	}
	mutex_unlock(&dart->init_lock);
	return ret;
}

/* Live mappings are indexed by IOVA page. Normal map/unmap only touches the
 * requested range; a complete walk is needed solely when adding a new SID.
 * Zero tagged values reserve XArray storage before any hardware mapping.
 */
#define APIF_MAP_PROT_BITS 8

static unsigned long apif_mapping_value(phys_addr_t pa, int prot)
{
	return ((pa >> DART_APIF_PAGE_SHIFT) << APIF_MAP_PROT_BITS) | prot;
}

static int apif_reserve_mappings(struct apple_dart_apif_domain *dom,
				 unsigned long iova, size_t size, gfp_t gfp)
{
	unsigned long first = iova >> DART_APIF_PAGE_SHIFT;
	unsigned long pages = size >> DART_APIF_PAGE_SHIFT;
	unsigned long page;
	int ret;

	for (page = 0; page < pages; page++) {
		ret = xa_insert(&dom->mappings, first + page, xa_mk_value(0),
				gfp);
		if (ret)
			goto undo;
	}
	return 0;
undo:
	while (page)
		xa_erase(&dom->mappings, first + --page);
	return ret;
}

static void apif_release_reservations(struct apple_dart_apif_domain *dom,
				      unsigned long iova, size_t size)
{
	unsigned long first = iova >> DART_APIF_PAGE_SHIFT;
	unsigned long pages = size >> DART_APIF_PAGE_SHIFT;

	for (unsigned long page = 0; page < pages; page++)
		if (xa_load(&dom->mappings, first + page) == xa_mk_value(0))
			xa_erase(&dom->mappings, first + page);
}

static void apif_record_mapping(struct apple_dart_apif_domain *dom, u64 iova,
				u64 pa, size_t size, int prot)
{
	for (size_t off = 0; off < size; off += DART_APIF_PAGE_SIZE) {
		unsigned long index = (iova + off) >> DART_APIF_PAGE_SHIFT;
		void *entry = xa_mk_value(apif_mapping_value(pa + off, prot));

		/* Reserved entries retain their nodes, so replacing a reservation
		 * requires no allocation, even in this IRQ-disabled section.
		 */
		WARN_ON(xa_err(
			xa_store(&dom->mappings, index, entry, GFP_NOWAIT)));
	}
}

static void apif_forget_mapping(struct apple_dart_apif_domain *dom, u64 iova,
				size_t size)
{
	for (size_t off = 0; off < size; off += DART_APIF_PAGE_SIZE)
		xa_erase(&dom->mappings, (iova + off) >> DART_APIF_PAGE_SHIFT);
}

static int apif_prepare_leaf(struct apple_dart_apif_domain *dom,
			     struct apif_stream_map *stream, unsigned int sid,
			     u64 iova)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned long key = ((unsigned long)dart->dart_id << 48) |
			    ((unsigned long)sid << 40) |
			    (iova >> DART_APIF_BLOCK_SHIFT);
	int ret;

	if (xa_load(&dom->l2_attached, key))
		return 0;
	ret = xa_insert(&dom->l2_attached, key, xa_mk_value(0), GFP_ATOMIC);
	if (ret)
		return ret;
	dart->batch->count = 1;
	dart->batch->op[0] = (struct apif_op){
		.op = APIF_DART_ATTACH,
		.arg = { sid, iova, 2 },
	};
	ret = apif_ring(dart);
	if (ret) {
		xa_erase(&dom->l2_attached, key);
		return ret;
	}
	WARN_ON(xa_err(
		xa_store(&dom->l2_attached, key, xa_mk_value(1), GFP_NOWAIT)));
	return 0;
}

/* Return the next physically contiguous, same-protection run, stopping at a
 * leaf-table boundary. Reservations are not yet live and are skipped.
 */
static bool apif_next_mapping(struct apple_dart_apif_domain *dom,
			      unsigned long *cursor, struct apif_op *op,
			      unsigned int sid)
{
	unsigned long index = *cursor, value, first;
	void *entry;
	u64 iova, pa;
	size_t size;
	int prot;

	for (;;) {
		entry = xa_find(&dom->mappings, &index, ULONG_MAX, XA_PRESENT);
		if (!entry)
			return false;
		value = xa_to_value(entry);
		if (value)
			break;
		index++;
	}
	first = index;
	prot = value & ((1UL << APIF_MAP_PROT_BITS) - 1);
	pa = (value >> APIF_MAP_PROT_BITS) << DART_APIF_PAGE_SHIFT;
	iova = (u64)first << DART_APIF_PAGE_SHIFT;
	size = DART_APIF_PAGE_SIZE;
	while ((iova + size) & (DART_APIF_BLOCK_SIZE - 1)) {
		entry = xa_load(&dom->mappings, index + 1);
		if (entry != xa_mk_value(apif_mapping_value(pa + size, prot)))
			break;
		index++;
		size += DART_APIF_PAGE_SIZE;
	}
	*cursor = index + 1;
	*op = (struct apif_op){
		.op = APIF_DART_MAP,
		.arg = { sid, iova, pa, size, prot },
	};
	return true;
}

static int apif_undo_replay(struct apple_dart_apif_domain *dom,
			    struct apif_stream_map *stream, unsigned int sid,
			    u64 mapped_end)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned long cursor = 0;
	struct apif_op op;

	while (apif_next_mapping(dom, &cursor, &op, sid) &&
	       op.arg[1] < mapped_end) {
		dart->batch->count = 1;
		dart->batch->op[0] = (struct apif_op){
			.op = APIF_DART_UNMAP,
			.arg = { sid, op.arg[1],
				 min_t(u64, op.arg[3],
				       mapped_end - op.arg[1]) },
		};
		if (apif_ring(dart)) {
			dev_err(dart->dev, "failed to undo SID %u replay\n",
				sid);
			return -EIO;
		}
	}
	return 0;
}

/* New SID is disabled throughout replay. On failure, keep it disabled and
 * remove the successful replay prefix. Existing SIDs/maps stay untouched.
 */
static int apif_replay_sid(struct apple_dart_apif_domain *dom,
			   struct apif_stream_map *stream, unsigned int sid)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned long cursor = 0;
	struct apif_op op;
	u64 mapped_end = 0;
	u32 n = 0;
	int ret;

	while (apif_next_mapping(dom, &cursor, &op, sid)) {
		if (dart->retained[sid] &&
		    apif_retained_lookup(dart->retained[sid], op.arg[1],
					 op.arg[3]))
			return -EBUSY;
		ret = apif_prepare_leaf(dom, stream, sid, op.arg[1]);
		if (ret)
			return ret;
	}
	cursor = 0;
	for (;;) {
		bool have = apif_next_mapping(dom, &cursor, &op, sid);

		if (have)
			dart->batch->op[n++] = op;
		if (n == APIF_BATCH_MAX_OPS || (!have && n)) {
			dart->batch->count = n;
			ret = apif_ring(dart);
			for (u32 i = 0; i < min(dart->batch->done, n); i++)
				mapped_end = dart->batch->op[i].arg[1] +
					     dart->batch->op[i].arg[3];
			if (ret)
				goto undo;
			n = 0;
		}
		if (!have)
			return 0;
	}
undo:
	apif_undo_replay(dom, stream, sid, mapped_end);
	return ret;
}

static int apple_dart_apif_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct apple_dart_apif_master *master = dev_iommu_priv_get(dev);
	struct apple_dart_apif_domain *dom = to_apif_domain(domain);
	struct apple_dart_apif_domain *prior = NULL;
	unsigned long added[APIF_MAX_DARTS]
			   [BITS_TO_LONGS(DART_APIF_MAX_STREAMS)] = {};
	unsigned long moved[APIF_MAX_DARTS]
			   [BITS_TO_LONGS(DART_APIF_MAX_STREAMS)] = {};
	unsigned long flags;
	unsigned int sid;
	int ret;

	if (!master || !master->num_darts)
		return -ENODEV;
	for (unsigned int i = 0; i < master->num_darts; i++) {
		ret = apple_dart_apif_ensure_init(master->streams[i].dart);
		if (ret)
			return ret;
	}
	if (old && old != domain && old->owner == &apple_dart_apif_ops &&
	    old->ops == apple_dart_apif_ops.default_domain_ops)
		prior = to_apif_domain(old);
	if (prior && (unsigned long)prior < (unsigned long)dom) {
		mutex_lock(&prior->lock);
		mutex_lock_nested(&dom->lock, SINGLE_DEPTH_NESTING);
	} else {
		mutex_lock(&dom->lock);
		if (prior)
			mutex_lock_nested(&prior->lock, SINGLE_DEPTH_NESTING);
	}
	ret = apif_domain_configure(dom, master);
	if (ret)
		goto unlock_mutex;

	local_irq_save(flags);
	if (prior && (unsigned long)prior < (unsigned long)dom) {
		write_lock(&prior->streams_lock);
		write_lock_nested(&dom->streams_lock, SINGLE_DEPTH_NESTING);
	} else {
		write_lock(&dom->streams_lock);
		if (prior)
			write_lock_nested(&prior->streams_lock,
					  SINGLE_DEPTH_NESTING);
	}
	for (unsigned int i = 0; i < master->num_darts && !ret; i++) {
		struct apple_dart_apif *dart = master->streams[i].dart;
		struct apif_stream_map *stream = apif_find_stream(dom, dart);
		struct apif_stream_map *before = apif_find_stream(prior, dart);

		spin_lock(&dart->batch_lock);
		for_each_set_bit(sid, master->streams[i].sids,
				 dart->num_streams) {
			if (test_bit(sid, stream->sids))
				continue;
			if (dart->retained[sid] && before &&
			    test_bit(sid, before->sids)) {
				set_bit(sid, moved[i]);
				ret = apif_undo_replay(prior, before, sid,
						       U64_MAX);
				if (ret)
					break;
			}
			dart->batch->count = dart->retained[sid] ? 1 : 2;
			dart->batch->op[0] = (struct apif_op){
				.op = dart->retained[sid] ?
					      APIF_DART_RETAINED_INFO :
					      APIF_DART_DISABLE,
				.arg = { sid },
			};
			if (!dart->retained[sid])
				dart->batch->op[1] = (struct apif_op){
					.op = APIF_DART_ATTACH,
					.arg = { sid, dart->dma_min, 1 },
				};
			ret = apif_ring(dart);
			if (ret)
				break;
			ret = apif_replay_sid(dom, stream, sid);
			if (ret)
				break;
			/* Include this SID in unwind even when ENABLE's completion fails. */
			set_bit(sid, added[i]);
			dart->batch->count = 1;
			dart->batch->op[0] = (struct apif_op){
				.op = dart->retained[sid] ?
					      APIF_DART_RETAINED_INFO :
					      APIF_DART_ENABLE,
				.arg = { sid },
			};
			ret = apif_ring(dart);
			if (ret)
				break;
			dom->has_retained |= !!dart->retained[sid];
			set_bit(sid, stream->sids);
		}
		spin_unlock(&dart->batch_lock);
	}

	/* Attach is one transaction across all of the master's DARTs. */
	for (unsigned int i = 0; i < master->num_darts; i++) {
		struct apple_dart_apif *dart = master->streams[i].dart;
		struct apif_stream_map *stream = apif_find_stream(dom, dart);
		struct apif_stream_map *before = apif_find_stream(prior, dart);

		spin_lock(&dart->batch_lock);
		if (ret) {
			for_each_set_bit(sid, added[i], dart->num_streams) {
				dart->batch->count = 1;
				dart->batch->op[0] = (struct apif_op){
					.op = dart->retained[sid] ?
						      APIF_DART_RETAINED_INFO :
						      APIF_DART_DISABLE,
					.arg = { sid },
				};
				if (apif_ring(dart))
					dev_err(dart->dev,
						"failed to disable SID %u during attach unwind\n",
						sid);
				apif_undo_replay(dom, stream, sid, U64_MAX);
				clear_bit(sid, stream->sids);
			}
		}
		for_each_set_bit(sid, moved[i], dart->num_streams) {
			if (ret) {
				if (apif_replay_sid(prior, before, sid))
					dev_err(dart->dev,
						"failed to restore previous domain SID %u\n",
						sid);
			} else {
				clear_bit(sid, before->sids);
			}
		}
		spin_unlock(&dart->batch_lock);
	}
	if (prior)
		write_unlock(&prior->streams_lock);
	write_unlock(&dom->streams_lock);
	local_irq_restore(flags);
unlock_mutex:
	if (prior)
		mutex_unlock(&prior->lock);
	mutex_unlock(&dom->lock);
	return ret;
}

struct apif_map_progress {
	size_t mapped;
	unsigned int streams;
	struct apif_op partial;
};

/* Track only fully installed stream cohorts. The batch may split one cohort
 * without reducing batching; keep its progress across doorbells.
 */
static int apif_flush_maps(struct apple_dart_apif_domain *dom,
			   struct apif_stream_map *stream,
			   struct apif_map_progress *progress)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned int total_streams =
		bitmap_weight(stream->sids, dart->num_streams);
	int ret = apif_ring(dart);

	for (u32 i = 0; i < min(dart->batch->done, dart->batch->count); i++) {
		struct apif_op *op = &dart->batch->op[i];

		progress->partial = *op;
		if (++progress->streams == total_streams) {
			progress->mapped += op->arg[3];
			progress->streams = 0;
		}
	}
	if (ret && progress->streams) {
		unsigned int sid;
		u32 n = 0;

		for_each_set_bit(sid, stream->sids, dart->num_streams) {
			if (n == progress->streams)
				break;
			dart->batch->op[n++] = (struct apif_op){
				.op = APIF_DART_UNMAP,
				.arg = { sid, progress->partial.arg[1],
					 progress->partial.arg[3] },
			};
		}
		dart->batch->count = n;
		if (apif_ring(dart))
			dev_err(dart->dev, "failed to undo partial DART map\n");
	}
	return ret;
}

static int apif_map_streams(struct apple_dart_apif_domain *dom,
			    struct apif_stream_map *stream, unsigned long iova,
			    phys_addr_t paddr, size_t pgsize, size_t pgcount,
			    int prot, gfp_t gfp, size_t *mapped)
{
	struct apple_dart_apif *dart = stream->dart;
	struct apif_map_progress progress = {};
	size_t total;
	unsigned long flags;
	u32 n = 0;
	int ret;

	*mapped = 0;
	if (!dart || bitmap_empty(stream->sids, dart->num_streams))
		return -ENODEV;
	if (pgsize != DART_APIF_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova | paddr) & (DART_APIF_PAGE_SIZE - 1) ||
	    prot & ~(IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE | IOMMU_NOEXEC |
		     IOMMU_MMIO) ||
	    !paddr || iova < dart->dma_min || iova > dart->dma_max ||
	    total - 1 > dart->dma_max - iova ||
	    iova + total > DART_APIF_NATIVE_APERTURE_END)
		return -EINVAL;
	spin_lock_irqsave(&dart->batch_lock, flags);
	/* Install all leaf tables before accumulating the mapping batch. */
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk =
			min_t(size_t, total - pos,
			      DART_APIF_BLOCK_SIZE -
				      (cur & (DART_APIF_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, stream->sids, dart->num_streams) {
			ret = apif_prepare_leaf(dom, stream, sid, cur);
			if (ret)
				goto out;
		}
		pos += chunk;
	}
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk =
			min_t(size_t, total - pos,
			      DART_APIF_BLOCK_SIZE -
				      (cur & (DART_APIF_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, stream->sids, dart->num_streams) {
			dart->batch->op[n++] = (struct apif_op){
				.op = APIF_DART_MAP,
				.arg = { sid, cur, paddr + pos, chunk, prot },
			};
			if (n == APIF_BATCH_MAX_OPS) {
				dart->batch->count = n;
				ret = apif_flush_maps(dom, stream, &progress);
				if (ret)
					goto out;
				n = 0;
			}
		}
		pos += chunk;
	}
	if (n) {
		dart->batch->count = n;
		ret = apif_flush_maps(dom, stream, &progress);
	}
out:
	spin_unlock_irqrestore(&dart->batch_lock, flags);
	*mapped = progress.mapped;
	return ret ? -EIO : 0;
}

static int apif_restore_partial_unmap(struct apple_dart_apif_domain *dom,
				      struct apif_stream_map *stream,
				      const struct apif_op *partial,
				      unsigned int streams)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned long cursor = partial->arg[1] >> DART_APIF_PAGE_SHIFT;
	u64 end = partial->arg[1] + partial->arg[2];
	struct apif_op op;
	u32 n = 0;

	while (apif_next_mapping(dom, &cursor, &op, 0) && op.arg[1] < end) {
		unsigned int sid, count = 0;

		op.arg[3] = min_t(u64, op.arg[3], end - op.arg[1]);
		for_each_set_bit(sid, stream->sids, dart->num_streams) {
			if (count++ == streams)
				break;
			op.arg[0] = sid;
			dart->batch->op[n++] = op;
			if (n == APIF_BATCH_MAX_OPS) {
				dart->batch->count = n;
				if (apif_ring(dart))
					return -EIO;
				n = 0;
			}
		}
	}
	if (!n)
		return 0;
	dart->batch->count = n;
	return apif_ring(dart);
}

static int apif_flush_unmaps(struct apple_dart_apif_domain *dom,
			     struct apif_stream_map *stream,
			     struct apif_map_progress *progress)
{
	struct apple_dart_apif *dart = stream->dart;
	unsigned int total_streams =
		bitmap_weight(stream->sids, dart->num_streams);
	int ret = apif_ring(dart);

	for (u32 i = 0; i < min(dart->batch->done, dart->batch->count); i++) {
		struct apif_op *op = &dart->batch->op[i];

		progress->partial = *op;
		if (++progress->streams == total_streams) {
			progress->mapped += op->arg[2];
			progress->streams = 0;
		}
	}
	if (ret && progress->streams &&
	    apif_restore_partial_unmap(dom, stream, &progress->partial,
				       progress->streams))
		dev_err(dart->dev, "failed to restore partial DART unmap\n");
	return ret;
}

static size_t apif_unmap_streams(struct apple_dart_apif_domain *dom,
				 struct apif_stream_map *stream,
				 unsigned long iova, size_t pgsize,
				 size_t pgcount,
				 struct iommu_iotlb_gather *gather)
{
	struct apple_dart_apif *dart = stream->dart;
	struct apif_map_progress progress = {};
	size_t total;
	unsigned long flags;
	u32 n = 0;

	if (!dart || bitmap_empty(stream->sids, dart->num_streams) ||
	    pgsize != DART_APIF_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova & (DART_APIF_PAGE_SIZE - 1)) || iova < dart->dma_min ||
	    iova > dart->dma_max || total - 1 > dart->dma_max - iova)
		return 0;
	spin_lock_irqsave(&dart->batch_lock, flags);
	for (size_t pos = 0; pos < total;) {
		u64 cur = iova + pos;
		size_t chunk =
			min_t(size_t, total - pos,
			      DART_APIF_BLOCK_SIZE -
				      (cur & (DART_APIF_BLOCK_SIZE - 1)));
		unsigned int sid;

		for_each_set_bit(sid, stream->sids, dart->num_streams) {
			dart->batch->op[n++] = (struct apif_op){
				.op = APIF_DART_UNMAP,
				.arg = { sid, cur, chunk },
			};
			if (n == APIF_BATCH_MAX_OPS) {
				dart->batch->count = n;
				if (apif_flush_unmaps(dom, stream, &progress))
					goto out;
				n = 0;
			}
		}
		pos += chunk;
	}
	if (n) {
		dart->batch->count = n;
		apif_flush_unmaps(dom, stream, &progress);
	}
out:
	spin_unlock_irqrestore(&dart->batch_lock, flags);
	return progress.mapped;
}

/* A buffer has one IOVA and one software mapping, regardless of how many
 * display engines can scan it out. Forward whole batches to each DART; do
 * not split them into per-page or per-stream transactions.
 */
static int apple_dart_apif_map_pages(struct iommu_domain *domain,
				     unsigned long iova, phys_addr_t paddr,
				     size_t pgsize, size_t pgcount, int prot,
				     gfp_t gfp, size_t *mapped)
{
	struct apple_dart_apif_domain *dom = to_apif_domain(domain);
	size_t done[APIF_MAX_DARTS] = {}, total;
	unsigned long flags;
	unsigned int active = 0;
	int ret;

	*mapped = 0;
	if (!dom->num_darts)
		return -ENODEV;
	if (pgsize != DART_APIF_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova | paddr) & (DART_APIF_PAGE_SIZE - 1) || !paddr ||
	    total - 1 > U64_MAX - paddr ||
	    prot & ~(IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE | IOMMU_NOEXEC |
		     IOMMU_MMIO) ||
	    iova < domain->geometry.aperture_start ||
	    iova > domain->geometry.aperture_end ||
	    total - 1 > domain->geometry.aperture_end - iova ||
	    iova + total > DART_APIF_NATIVE_APERTURE_END)
		return -EINVAL;
	ret = apif_reserve_mappings(dom, iova, total, gfp);
	if (ret)
		return ret;
	read_lock_irqsave(&dom->streams_lock, flags);
	if (apif_retained_overlap(dom, iova, total)) {
		ret = -EBUSY;
		goto out;
	}
	for (unsigned int i = 0; i < dom->num_darts; i++) {
		struct apif_stream_map *stream = &dom->streams[i];

		if (bitmap_empty(stream->sids, stream->dart->num_streams))
			continue;
		active++;
		ret = apif_map_streams(dom, stream, iova, paddr, pgsize,
				       pgcount, prot, gfp, &done[i]);
		if (ret)
			break;
	}
	if (!active)
		ret = -ENODEV;
	if (!ret) {
		apif_record_mapping(dom, iova, paddr, total, prot);
		*mapped = total;
	} else {
		/* A later DART failed. Undo the successful prefixes on every
		 * engine before letting the DMA layer reuse this IOVA.
		 */
		for (unsigned int i = 0; i < dom->num_darts; i++)
			if (done[i] &&
			    apif_unmap_streams(dom, &dom->streams[i], iova,
					       pgsize, done[i] / pgsize,
					       NULL) != done[i])
				dev_err(dom->streams[i].dart->dev,
					"failed to unwind multi-DART mapping\n");
	}
out:
	read_unlock_irqrestore(&dom->streams_lock, flags);
	if (ret)
		apif_release_reservations(dom, iova, total);
	return ret;
}

static size_t apple_dart_apif_unmap_pages(struct iommu_domain *domain,
					  unsigned long iova, size_t pgsize,
					  size_t pgcount,
					  struct iommu_iotlb_gather *gather)
{
	struct apple_dart_apif_domain *dom = to_apif_domain(domain);
	size_t done[APIF_MAX_DARTS] = {}, total, common;
	unsigned long flags;
	unsigned int active = 0;

	if (!dom->num_darts || pgsize != DART_APIF_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgsize, pgcount, &total) ||
	    (iova & (DART_APIF_PAGE_SIZE - 1)) ||
	    iova < domain->geometry.aperture_start ||
	    iova > domain->geometry.aperture_end ||
	    total - 1 > domain->geometry.aperture_end - iova)
		return 0;
	read_lock_irqsave(&dom->streams_lock, flags);
	if (apif_retained_overlap(dom, iova, total)) {
		common = 0;
		goto out;
	}
	for (unsigned int i = 0; i < dom->num_darts; i++) {
		struct apif_stream_map *stream = &dom->streams[i];

		if (bitmap_empty(stream->sids, stream->dart->num_streams))
			continue;
		active++;
		done[i] = apif_unmap_streams(dom, stream, iova, pgsize, pgcount,
					     gather);
		if (done[i] != total)
			break;
	}
	common = active ? total : 0;
	for (unsigned int i = 0; i < dom->num_darts; i++)
		if (!bitmap_empty(dom->streams[i].sids,
				  dom->streams[i].dart->num_streams))
			common = min(common, done[i]);
	/* Preserve the canonical mapping until all engines have agreed. On
	 * failure restore the suffix removed from earlier engines, retaining
	 * their existing leaf tables and full-sized transport batches.
	 */
	for (unsigned int i = 0; i < dom->num_darts; i++) {
		struct apif_stream_map *stream = &dom->streams[i];
		struct apple_dart_apif *dart = stream->dart;
		struct apif_op suffix = {
			.arg = { 0, iova + common, done[i] - common },
		};

		if (done[i] <= common)
			continue;
		spin_lock(&dart->batch_lock);
		if (apif_restore_partial_unmap(
			    dom, stream, &suffix,
			    bitmap_weight(stream->sids, dart->num_streams)))
			dev_err(dart->dev,
				"failed to restore multi-DART unmap suffix\n");
		spin_unlock(&dart->batch_lock);
	}
	apif_forget_mapping(dom, iova, common);
out:
	read_unlock_irqrestore(&dom->streams_lock, flags);
	return common;
}

static void apple_dart_apif_iotlb_sync(struct iommu_domain *domain,
				       struct iommu_iotlb_gather *gather)
{
	/* apif invalidates inside every MAP/UNMAP it forwards to SPTM, so a
	 * separate flush is a no-op.
	 */
}

static phys_addr_t apple_dart_apif_iova_to_phys(struct iommu_domain *domain,
						dma_addr_t iova)
{
	struct apple_dart_apif_domain *dom = to_apif_domain(domain);
	void *entry = xa_load(&dom->mappings, iova >> DART_APIF_PAGE_SHIFT);
	unsigned long value = entry ? xa_to_value(entry) : 0;

	if (!value)
		return apif_retained_translate(dom, iova);
	return ((value >> APIF_MAP_PROT_BITS) << DART_APIF_PAGE_SHIFT) |
	       (iova & (DART_APIF_PAGE_SIZE - 1));
}

static void apple_dart_apif_domain_free(struct iommu_domain *domain)
{
	struct apple_dart_apif_domain *dom = to_apif_domain(domain);

	if (dom->has_retained) {
		for (unsigned int i = 0; i < dom->num_darts; i++) {
			struct apif_stream_map *stream = &dom->streams[i];
			unsigned int sid;
			unsigned long flags;

			spin_lock_irqsave(&stream->dart->batch_lock, flags);
			for_each_set_bit(sid, stream->sids,
					 stream->dart->num_streams)
				apif_undo_replay(dom, stream, sid, U64_MAX);
			spin_unlock_irqrestore(&stream->dart->batch_lock,
					       flags);
		}
	}
	xa_destroy(&dom->l2_attached);
	xa_destroy(&dom->mappings);
	kfree(dom);
}

static struct iommu_domain *
apple_dart_apif_domain_alloc_paging(struct device *dev)
{
	struct apple_dart_apif_domain *dom;
	int ret;

	dom = kzalloc_obj(*dom);
	if (!dom)
		return NULL;
	mutex_init(&dom->lock);
	rwlock_init(&dom->streams_lock);
	xa_init(&dom->l2_attached);
	xa_init(&dom->mappings);
	dom->domain.pgsize_bitmap = DART_APIF_PAGE_SIZE;
	dom->domain.geometry.aperture_end = DART_APIF_APERTURE_END;
	dom->domain.geometry.force_aperture = true;
	if (dev && dev_iommu_priv_get(dev)) {
		ret = apif_domain_configure(dom, dev_iommu_priv_get(dev));
		if (ret) {
			kfree(dom);
			return ERR_PTR(ret);
		}
	}
	return &dom->domain;
}

static struct iommu_device *apple_dart_apif_probe_device(struct device *dev)
{
	struct apple_dart_apif_master *master = dev_iommu_priv_get(dev);

	if (!dev_iommu_fwspec_get(dev) || !master || !master->num_darts)
		return ERR_PTR(-ENODEV);
	for (unsigned int i = 0; i < master->num_darts; i++)
		device_link_add(dev, master->streams[i].dart->dev,
				DL_FLAG_PM_RUNTIME |
					DL_FLAG_AUTOREMOVE_SUPPLIER);
	return &master->streams[0].dart->iommu;
}

static void apple_dart_apif_release_device(struct device *dev)
{
	kfree(dev_iommu_priv_get(dev));
}

static int apple_dart_apif_of_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct apple_dart_apif_master *master = dev_iommu_priv_get(dev);
	struct platform_device *iommu_pdev;
	struct apple_dart_apif *dart;
	u32 sid;

	if (args->args_count != 1)
		return -EINVAL;
	sid = args->args[0];
	if (sid >= DART_APIF_MAX_STREAMS)
		return -EINVAL;

	iommu_pdev = of_find_device_by_node(args->np);
	if (!iommu_pdev)
		return -ENODEV;
	dart = platform_get_drvdata(iommu_pdev);
	put_device(&iommu_pdev->dev);
	if (!dart)
		return -ENODEV;

	if (sid >= dart->num_streams)
		return -EINVAL;
	if (!master) {
		master = kzalloc_obj(*master);
		if (!master)
			return -ENOMEM;
		dev_iommu_priv_set(dev, master);
	}
	for (unsigned int i = 0; i < master->num_darts; i++) {
		if (master->streams[i].dart == dart) {
			set_bit(sid, master->streams[i].sids);
			return 0;
		}
	}
	if (master->num_darts == APIF_MAX_DARTS)
		return -EINVAL;
	master->streams[master->num_darts].dart = dart;
	set_bit(sid, master->streams[master->num_darts].sids);
	master->num_darts++;
	return 0;
}

/* Devices sharing a stream cannot have independent IOVA allocators. PCI
 * multifunction/topology grouping is also required even with distinct SIDs.
 * Mirrors the native Apple DART lifetime model: the group owns its SID union,
 * and its release callback clears the reverse lookup before freeing it.
 */
static DEFINE_MUTEX(apple_dart_apif_groups_lock);

static void apple_dart_apif_release_group(void *data)
{
	struct apple_dart_apif_master *group_master = data;
	unsigned int sid;

	mutex_lock(&apple_dart_apif_groups_lock);
	for (unsigned int i = 0; i < group_master->num_darts; i++) {
		struct apif_stream_map *stream = &group_master->streams[i];

		for_each_set_bit(sid, stream->sids, stream->dart->num_streams)
			stream->dart->sid2group[sid] = NULL;
	}
	mutex_unlock(&apple_dart_apif_groups_lock);
	kfree(group_master);
}

static int apif_merge_master(struct apple_dart_apif_master *dst,
			     const struct apple_dart_apif_master *src)
{
	struct apple_dart_apif_master merged = *dst;

	for (unsigned int i = 0; i < src->num_darts; i++) {
		const struct apif_stream_map *from = &src->streams[i];
		unsigned int j;

		for (j = 0; j < merged.num_darts; j++)
			if (merged.streams[j].dart == from->dart)
				break;
		if (j == merged.num_darts) {
			if (j == APIF_MAX_DARTS)
				return -EINVAL;
			merged.streams[j] = *from;
			merged.num_darts++;
		} else {
			bitmap_or(merged.streams[j].sids,
				  merged.streams[j].sids, from->sids,
				  from->dart->num_streams);
		}
	}
	*dst = merged;
	return 0;
}

static struct iommu_group *apple_dart_apif_device_group(struct device *dev)
{
	struct apple_dart_apif_master *master = dev_iommu_priv_get(dev);
	struct apple_dart_apif_master *group_master;
	struct iommu_group *group = NULL, *put_group = NULL;
	struct iommu_group *result = ERR_PTR(-EINVAL);
	unsigned int sid;

	if (!master || !master->num_darts)
		return result;
	mutex_lock(&apple_dart_apif_groups_lock);
	for (unsigned int i = 0; i < master->num_darts; i++) {
		struct apif_stream_map *stream = &master->streams[i];

		for_each_set_bit(sid, stream->sids, stream->dart->num_streams) {
			struct iommu_group *existing =
				stream->dart->sid2group[sid];

			if (!existing)
				continue;
			if (group && existing != group)
				goto out;
			group = existing;
		}
	}
	if (group) {
		group = iommu_group_ref_get(group);
	} else {
#ifdef CONFIG_PCI
		if (dev_is_pci(dev))
			group = pci_device_group(dev);
		else
#endif
			group = generic_device_group(dev);
	}
	if (IS_ERR(group)) {
		result = group;
		goto out;
	}
	if (!group) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}
	group_master = iommu_group_get_iommudata(group);
	if (!group_master) {
		group_master = kmemdup(master, sizeof(*master), GFP_KERNEL);
		if (!group_master) {
			result = ERR_PTR(-ENOMEM);
			put_group = group;
			goto out;
		}
		iommu_group_set_iommudata(group, group_master,
					  apple_dart_apif_release_group);
	} else if (apif_merge_master(group_master, master)) {
		put_group = group;
		goto out;
	}
	for (unsigned int i = 0; i < master->num_darts; i++) {
		struct apif_stream_map *stream = &master->streams[i];

		for_each_set_bit(sid, stream->sids, stream->dart->num_streams)
			stream->dart->sid2group[sid] = group;
	}
	result = group;
out:
	mutex_unlock(&apple_dart_apif_groups_lock);
	/* Dropping the last reference can invoke release_group: do it unlocked. */
	if (put_group)
		iommu_group_put(put_group);
	return result;
}

static bool apple_dart_apif_capable(struct device *dev, enum iommu_cap cap)
{
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

/* PCIe captures the MSI doorbell before DART translation. Advertise the
 * identity MSI page to the DMA layer so it never tries to map this non-RAM
 * address through the SPTM transport. Other devices retain normal reservations.
 * This follows apple-dart.c's hardware MSI reservation (Asahi contributors).
 */
static void apple_dart_apif_get_resv_regions(struct device *dev,
					     struct list_head *head)
{
	struct apple_dart_apif_master *master = dev_iommu_priv_get(dev);
	unsigned int sid;

	for (unsigned int i = 0; master && i < master->num_darts; i++) {
		struct apif_stream_map *stream = &master->streams[i];

		/* Reserve the exclusive-end granule when required by the aperture.
		 * Narrower native apertures declare that restriction in DT.
		 */
		if (stream->dart->dma_max == DART_APIF_NATIVE_APERTURE_END ||
		    stream->dart->reserve_last_page) {
			struct iommu_resv_region *region =
				iommu_alloc_resv_region(
					stream->dart->dma_max + 1 -
						DART_APIF_PAGE_SIZE,
					DART_APIF_PAGE_SIZE, 0,
					IOMMU_RESV_RESERVED, GFP_KERNEL);

			if (region)
				list_add_tail(&region->list, head);
		}
		for_each_set_bit(sid, stream->sids, stream->dart->num_streams) {
			struct apif_retained_sid *r =
				stream->dart->retained[sid];
			if (!r)
				continue;
			for (unsigned int i = 0; i < r->count; i++) {
				struct iommu_resv_region *region =
					iommu_alloc_resv_region(
						r->runs[i].dva, r->runs[i].size,
						0, IOMMU_RESV_RESERVED,
						GFP_KERNEL);
				if (region)
					list_add_tail(&region->list, head);
			}
		}
	}
	if (dev_is_pci(dev)) {
		struct pci_host_bridge *bridge =
			pci_find_host_bridge(to_pci_dev(dev)->bus);
		struct device_node *np = bridge->dev.parent->of_node;
		struct iommu_resv_region *region;
		u64 address;

		if (of_device_is_compatible(np, "apple,apif-pcie") &&
		    !of_property_read_u64(np, "apple,msi-address", &address)) {
			region = iommu_alloc_resv_region(
				address & PAGE_MASK, PAGE_SIZE,
				IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO,
				IOMMU_RESV_MSI, GFP_KERNEL);
			if (region)
				list_add_tail(&region->list, head);
		}
	}
	iommu_dma_get_resv_regions(dev, head);
}

static const struct iommu_ops apple_dart_apif_ops = {
	.get_resv_regions = apple_dart_apif_get_resv_regions,
	.capable = apple_dart_apif_capable,
	.domain_alloc_paging = apple_dart_apif_domain_alloc_paging,
	.probe_device = apple_dart_apif_probe_device,
	.release_device = apple_dart_apif_release_device,
	.device_group = apple_dart_apif_device_group,
	.of_xlate = apple_dart_apif_of_xlate,
	.owner = THIS_MODULE,
	.default_domain_ops =
		&(const struct iommu_domain_ops){
			.attach_dev = apple_dart_apif_attach_dev,
			.map_pages = apple_dart_apif_map_pages,
			.unmap_pages = apple_dart_apif_unmap_pages,
			.iotlb_sync = apple_dart_apif_iotlb_sync,
			.iova_to_phys = apple_dart_apif_iova_to_phys,
			.free = apple_dart_apif_domain_free,
		},
};

/* Used only after the PCI host has stopped every endpoint on this DART.
 * Native POWERDOWN/POWERUP retains the SPTM-owned tables and live mappings.
 */
int apple_dart_apif_set_power(struct device *dev, bool on)
{
	struct apple_dart_apif_master *master;
	struct apple_dart_apif *dart;
	unsigned long flags;
	int ret;

	if (!dev->iommu || !dev->iommu->iommu_dev ||
	    dev->iommu->iommu_dev->ops != &apple_dart_apif_ops)
		return -ENODEV;
	master = dev_iommu_priv_get(dev);
	if (!master || master->num_darts != 1)
		return -ENODEV;
	dart = master->streams[0].dart;
	mutex_lock(&dart->init_lock);
	if (!dart->inited) {
		ret = -EINVAL;
		goto out;
	}
	spin_lock_irqsave(&dart->batch_lock, flags);
	dart->batch->count = 1;
	dart->batch->op[0] = (struct apif_op){ .op = on ? 7 : 8 };
	ret = apif_ring(dart);
	spin_unlock_irqrestore(&dart->batch_lock, flags);
out:
	mutex_unlock(&dart->init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_apif_set_power);

static int apple_dart_apif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dart_apif *dart;
	int ret;

	dart = devm_kzalloc(dev, sizeof(*dart), GFP_KERNEL);
	if (!dart)
		return -ENOMEM;

	dart->dev = dev;
	mutex_init(&dart->init_lock);
	spin_lock_init(&dart->batch_lock);

	dart->apif = aurora_apif_get(dev);
	if (IS_ERR(dart->apif))
		return dev_err_probe(dev, PTR_ERR(dart->apif),
				     "APIF transport unavailable\n");
	ret = devm_add_action_or_reset(dev, apif_put_transport, dart->apif);
	if (ret)
		return ret;
	dart->native_ops = devm_kcalloc(dev, 2 * APIF_BATCH_MAX_OPS,
					sizeof(*dart->native_ops), GFP_KERNEL);
	if (!dart->native_ops)
		return -ENOMEM;
	dart->boot_info =
		devm_kzalloc(dev, sizeof(*dart->boot_info), GFP_KERNEL);
	if (!dart->boot_info)
		return -ENOMEM;
	if (of_property_read_u32(dev->of_node, "apple,apif-dart-id",
				 &dart->dart_id))
		return dev_err_probe(dev, -EINVAL,
				     "missing apple,apif-dart-id\n");
	if (dart->dart_id > 0xff)
		return dev_err_probe(dev, -EINVAL, "dart-id out of range\n");

	dart->dma_max = DART_APIF_APERTURE_END;
	if (of_find_property(dev->of_node, "apple,dma-range", NULL)) {
		u64 range[2], end;

		if (of_property_read_u64_array(dev->of_node, "apple,dma-range",
					       range, 2) ||
		    !range[1] ||
		    (range[0] | range[1]) & (DART_APIF_PAGE_SIZE - 1) ||
		    check_add_overflow(range[0], range[1] - 1, &end) ||
		    end > DART_APIF_NATIVE_APERTURE_END)
			return dev_err_probe(dev, -EINVAL, "bad DMA range\n");
		dart->dma_min = range[0];
		dart->dma_max = end;
	}
	if (of_find_property(dev->of_node, "apple,apif-iova-force", NULL)) {
		if (of_property_read_u64(dev->of_node, "apple,apif-iova-force",
					 &dart->iova_force) ||
		    (dart->iova_force && dart->iova_force != BIT_ULL(40)))
			return dev_err_probe(dev, -EINVAL,
					     "unsupported IOVA force mask\n");
		/* A native DVA aperture already contains its high address bits.
		 * Hidden force bits are only valid for the low-address device view.
		 */
		if (dart->iova_force && dart->dma_max >= dart->iova_force)
			return dev_err_probe(
				dev, -EINVAL,
				"IOVA force overlaps DMA aperture\n");
	}

	dart->reserve_last_page = of_property_read_bool(
		dev->of_node, "apple,apif-reserve-last-page");
	dart->num_streams = DART_APIF_MAX_STREAMS;
	of_property_read_u32(dev->of_node, "apple,apif-num-streams",
			     &dart->num_streams);
	if (!dart->num_streams || dart->num_streams > DART_APIF_MAX_STREAMS)
		return dev_err_probe(dev, -EINVAL, "bad stream count\n");

	dart->batch = devm_kzalloc(dev, PAGE_SIZE, GFP_KERNEL);
	if (!dart->batch)
		return -ENOMEM;
	ret = apif_load_retained(dart);
	if (ret)
		return dev_err_probe(dev, ret,
				     "retained DART discovery failed\n");

	ret = devm_add_action_or_reset(dev, apif_free_page_lists, dart);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, dart);

	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL,
				     "apple-dart-apif.%s", dev_name(dev));
	if (ret)
		return ret;

	ret = iommu_device_register(&dart->iommu, &apple_dart_apif_ops, dev);
	if (ret)
		goto err_sysfs;

	return 0;

err_sysfs:
	iommu_device_sysfs_remove(&dart->iommu);
	return ret;
}

static void apple_dart_apif_remove(struct platform_device *pdev)
{
	struct apple_dart_apif *dart = platform_get_drvdata(pdev);

	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);
}

static const struct of_device_id apple_dart_apif_of_match[] = {
	{ .compatible = "apple,dart-apif" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_dart_apif_of_match);

static struct platform_driver apple_dart_apif_driver = {
	.driver = {
		.name = "apple-dart-apif",
		.suppress_bind_attrs = true,
		.of_match_table = apple_dart_apif_of_match,
	},
	.probe = apple_dart_apif_probe,
	.remove = apple_dart_apif_remove,
};
module_platform_driver(apple_dart_apif_driver);

MODULE_DESCRIPTION("Apple DART IOMMU over the Aurora platform interface");
MODULE_LICENSE("GPL");
