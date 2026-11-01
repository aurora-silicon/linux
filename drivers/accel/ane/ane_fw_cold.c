// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Shared firmware backend. H17 is the first measured cold power provider. */
#include <crypto/sha2.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kexec.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/suspend.h>

#include "ane_fw_cold.h"
#include "ane_h16_accel.h"

#define ANE_FW_POOL_DVA 0x10000384000ULL
#define ANE_FW_IPC_DVA 0x100003d0000ULL
#define ANE_FW_HEAP_DVA 0x100003e4000ULL
#define ANE_FW_ALLOC_DVA 0x10001300000ULL
#define ANE_FW_TIMEOUT_NS (3ULL * NSEC_PER_SEC)

enum ane_fw_stage {
	ANE_STAGE_UNVERIFIED,
	ANE_STAGE_PROBED,
	ANE_STAGE_MAPPED,
	ANE_STAGE_BOOTED,
	ANE_STAGE_STARTUP,
	ANE_STAGE_PROGRAM,
	ANE_STAGE_QUARANTINED,
};

struct ane_fw_mapping {
	struct list_head list;
	struct ane_fw_surface surface;
	dma_addr_t physical;
};

struct ane_fw_backend {
	struct platform_device *pdev;
	const struct ane_fw_platform *soc;
	void __iomem *engine, *pmgr, *gate, *dart[ANE_FW_DARTS];
	/* Mapped register windows, for the MMIO trace hook. */
	struct {
		void __iomem *base;
		phys_addr_t pa;
		resource_size_t size;
	} window[3 + ANE_FW_DARTS];
	struct device *power, *memory;
	struct iommu_domain *domain;
	struct ane_fw_transport transport;
	/* Programs and their ABI 3 node, after the program stage. */
	struct ane_h16_session session;
	struct ane_fw_surface pool, ipc, heap;
	struct list_head mappings;
	/* Serialize stage changes, command publication and retained lifetimes. */
	struct mutex lock;
	struct notifier_block pm_notifier;
	struct ane_fw_cold_state cold;
	u64 ranges[ANE_FW_DAPF_RANGES * 2];
	u64 next_dva;
	enum ane_fw_stage stage;
	bool retained;
	/* ANE_SYS and the child power states are on: the engine is readable. */
	bool powered;
};

/* One ANE per SoC. The observer exists before platform driver registration:
 * the platform bus would otherwise resume genpd and attach DMA before probe.
 */
static struct ane_fw_backend *ane_backend;

/*
 * Every register access of this backend goes through these helpers, so a
 * debug build can log the physical address of each access before it is
 * made. The hook is empty here.
 */
static inline void ane_fw_trace(struct ane_fw_backend *b, char op, const void __iomem *a,
				u64 value)
{
}

static u32 ane_rd32(struct ane_fw_backend *b, const void __iomem *a)
{
	ane_fw_trace(b, 'r', a, 0);
	return readl(a);
}

static u64 ane_rd64(struct ane_fw_backend *b, const void __iomem *a)
{
	ane_fw_trace(b, 'R', a, 0);
	return readq(a);
}

static void ane_wr32(struct ane_fw_backend *b, u32 value, void __iomem *a)
{
	ane_fw_trace(b, 'w', a, value);
	writel(value, a);
}

static void ane_wr64(struct ane_fw_backend *b, u64 value, void __iomem *a)
{
	ane_fw_trace(b, 'W', a, value);
	writeq(value, a);
}

static int ane_poll32(struct ane_fw_backend *b, void __iomem *a, u32 mask, u32 want,
		      unsigned long sleep_us, u64 timeout_us)
{
	u32 value;

	ane_fw_trace(b, 'p', a, want);
	return readl_poll_timeout(a, value, (value & mask) == want, sleep_us, timeout_us);
}
static struct kexec_blocker ane_fw_kexec = {
	.users = ATOMIC_INIT(0),
	.reason = "ANE firmware retains private DMA mappings until hardware reset",
};

static int ane_fw_quarantine(struct ane_fw_backend *b, int error)
{
	unsigned int i;

	b->stage = ANE_STAGE_QUARANTINED;
	dev_err(&b->pdev->dev, "ANE quarantined (%d); mappings and power retained, reboot required\n",
		error);
	/* The engine and DARTs are only readable once powered. */
	if (!b->powered)
		return error;
	for (i = 0; i < 8; i++)
		dev_info(&b->pdev->dev, "scratch%u=%#x\n", i,
			 ane_rd32(b, b->engine + b->soc->scratch + i * 4));
	for (i = 0; i < ANE_FW_DARTS; i++)
		dev_info(&b->pdev->dev, "DART%u error=%#x address=%#x:%#x\n", i,
			 ane_rd32(b, b->dart[i] + 0x100), ane_rd32(b, b->dart[i] + 0x174),
			 ane_rd32(b, b->dart[i] + 0x170));
	return error;
}

static bool ane_fw_raw_boot(void)
{
	struct device_node *chosen __free(device_node) = of_find_node_by_path("/chosen");
	const char *evidence;

	return chosen && !of_property_read_string(chosen, "apple,ane-cold-boot", &evidence) &&
	       !strcmp(evidence, "raw-no-xnu-ane-untouched-v1");
}

/*
 * Step 1: PMGR words only. These are the only reads made with ANE_SYS in an
 * unknown power state; the engine block and its DARTs sit behind ANE_SYS and
 * an access to them while it is off stalls the bus.
 */
static void ane_fw_snapshot_pmgr(struct ane_fw_backend *b)
{
	struct ane_fw_cold_state *s = &b->cold;

	s->sys = ane_rd32(b, b->pmgr + 0x290);
	s->cpu = ane_rd32(b, b->pmgr + 0xc000);
	s->subblock = ane_rd32(b, b->pmgr + 0xc008);
	dev_info(&b->pdev->dev, "U2 ANE_SYS=%#x CPU_PS=%#x subblock=%#x\n",
		 s->sys, s->cpu, s->subblock);
}

/* Step 2: PWGATE, with ANE_SYS on and before the child power step. */
static void ane_fw_snapshot_gate(struct ane_fw_backend *b)
{
	struct ane_fw_cold_state *s = &b->cold;

	s->gate_request = ane_rd32(b, b->gate + 0xc);
	s->gate_ack = ane_rd32(b, b->gate + 0x30);
	dev_info(&b->pdev->dev, "U2 PWGATE=%#x/%#x\n", s->gate_request, s->gate_ack);
}

/* Step 3: the engine and its DARTs, after the child power step. */
static void ane_fw_snapshot_engine(struct ane_fw_backend *b)
{
	struct ane_fw_cold_state *s = &b->cold;
	struct device *dev = &b->pdev->dev;
	unsigned int i, j;

	s->ce_state = ane_rd64(b, b->engine + 0x18a4010);
	s->ce_error = ane_rd64(b, b->engine + 0x18a4018);
	s->rvbar = ane_rd64(b, b->engine + 0x1050000);
	s->control = ane_rd32(b, b->engine + b->soc->wrapper + 0x44);
	s->status = ane_rd32(b, b->engine + b->soc->wrapper + 0x48);
	s->pending = ane_rd32(b, b->engine + b->soc->irq_status);
	for (i = 0; i < 8; i++)
		s->scratch[i] = ane_rd32(b, b->engine + b->soc->scratch + i * 4);
	dev_info(dev, "U2 control=%#x status=%#x RVBAR=%#llx CE=%#llx/%#llx pending=%#x\n",
		 s->control, s->status, s->rvbar, s->ce_state, s->ce_error, s->pending);
	for (i = 0; i < ANE_FW_DARTS; i++) {
		s->dart[i].version = ane_rd32(b, b->dart[i] + 8);
		s->dart[i].protect = ane_rd32(b, b->dart[i] + 0x200);
		s->dart[i].unprotect = ane_rd32(b, b->dart[i] + 0x204);
		s->dart[i].lock = ane_rd32(b, b->dart[i] + 0x208);
		s->dart[i].error = ane_rd32(b, b->dart[i] + 0x100);
		for (j = 0; j < 16; j++)
			s->dart[i].enabled[j] = ane_rd32(b, b->dart[i] + 0xc00 + 4 * j);
		dev_info(dev, "U2 DART%u version=%#x protect=%#x unprotect=%#x lock=%#x error=%#x\n",
			 i, s->dart[i].version, s->dart[i].protect,
			 s->dart[i].unprotect, s->dart[i].lock, s->dart[i].error);
		for (j = 0; j < 16; j++)
			if (s->dart[i].enabled[j])
				dev_info(dev, "U2 DART%u stream enable word %u=%#x\n", i, j,
					 s->dart[i].enabled[j]);
	}
	for (i = 0; i < ANE_FW_DAPF_RANGES; i++) {
		void __iomem *r = b->engine + 0x1810000 + i * 0x40;

		dev_info(dev, "U2 DAPF%u flags=%#x start=%#llx end=%#llx enable=%#x\n",
			 i, ane_rd32(b, r), ane_rd64(b, r + 8), ane_rd64(b, r + 0x10),
			 ane_rd32(b, r + 0x20));
	}
}

static int ane_fw_pm_event(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct ane_fw_backend *b = container_of(nb, struct ane_fw_backend, pm_notifier);

	if ((event == PM_SUSPEND_PREPARE || event == PM_HIBERNATION_PREPARE ||
	     event == PM_RESTORE_PREPARE) && READ_ONCE(b->retained))
		return NOTIFY_BAD;
	return NOTIFY_DONE;
}

static int ane_fw_retain(struct ane_fw_backend *b)
{
	int ret;

	/*
	 * The kexec blocker takes built-in users only. A module, like the H16
	 * backend, pins itself and refuses system sleep, but cannot veto kexec.
	 */
	if (IS_BUILTIN(CONFIG_DRM_ACCEL_ANE)) {
		ret = kexec_block(&ane_fw_kexec);
		if (ret)
			return ret;
	}
	b->pm_notifier.notifier_call = ane_fw_pm_event;
	ret = register_pm_notifier(&b->pm_notifier);
	if (ret) {
		if (IS_BUILTIN(CONFIG_DRM_ACCEL_ANE))
			kexec_unblock(&ane_fw_kexec);
		return ret;
	}
	__module_get(THIS_MODULE);
	b->retained = true;
	return 0;
}

static int ane_fw_map(struct ane_fw_backend *b, u64 dva, phys_addr_t pa,
		      size_t size, int prot)
{
	int ret = iommu_map(b->domain, dva, pa, size, prot, GFP_KERNEL);

	if (ret)
		return ret;
	/* iommu_map includes the DART's normal range synchronization and
	 * propagates its timeout. Extra invalidates do not fix stale output
	 * BAR bindings between programs; the transport's queued first EXECUTE
	 * after a program change does.
	 */
	for (unsigned int i = 0; i < ANE_FW_DARTS; i++)
		if (ane_rd32(b, b->dart[i] + 0x100))
			return -EIO;
	return 0;
}

/*
 * Coherent (uncached) memory for firmware-owned state, or, with cached,
 * CPU-cacheable memory for program surfaces that the CPU copies to and from
 * on every request. The engine does not snoop either; cached surfaces are
 * kept coherent by ane_fw_program_sync() around each use.
 */
static int ane_fw_alloc_mem(struct ane_fw_backend *b, u64 dva, size_t size,
			    int prot, bool cached, struct ane_fw_surface *s)
{
	struct ane_fw_mapping *m;
	int ret;

	m = kzalloc_obj(*m);
	if (!m)
		return -ENOMEM;
	if (cached)
		m->surface.cpu = dma_alloc_noncoherent(b->memory, size, &m->physical,
						       DMA_BIDIRECTIONAL, GFP_KERNEL);
	else
		m->surface.cpu = dma_alloc_coherent(b->memory, size, &m->physical,
						    GFP_KERNEL);
	if (!m->surface.cpu) {
		kfree(m);
		return -ENOMEM;
	}
	if (cached) {
		/* Zero it and write the zeroes back before the engine sees it. */
		memset(m->surface.cpu, 0, size);
		dma_sync_single_for_device(b->memory, m->physical, size, DMA_BIDIRECTIONAL);
		m->surface.handle = m->physical;
	}
	m->surface.dva = dva;
	m->surface.size = size;
	/* Record before publication, including a failed/partial mapping. */
	list_add_tail(&m->list, &b->mappings);
	ret = ane_fw_map(b, dva, m->physical, size, prot);
	if (!ret)
		*s = m->surface;
	return ret;
}

static int ane_fw_alloc_at(struct ane_fw_backend *b, u64 dva, size_t size,
			   int prot, struct ane_fw_surface *s)
{
	return ane_fw_alloc_mem(b, dva, size, prot, false, s);
}

static int ane_fw_alloc(void *ctx, size_t size, u32 tag, struct ane_fw_surface *s)
{
	struct ane_fw_backend *b = ctx;
	int ret;

	if (!size || size > ANE_FW_SHARED_TOTAL ||
	    b->next_dva > ANE_FW_ALLOC_DVA + SZ_512M - size)
		return -ENOSPC;
	ret = ane_fw_alloc_at(b, b->next_dva, size, IOMMU_READ | IOMMU_WRITE, s);
	if (!ret) {
		b->next_dva += size;
		dev_info(&b->pdev->dev, "SHAREDMALLOC tag=%#x bytes=%zu\n", tag, size);
	}
	return ret;
}

static u64 ane_fw_load64(void *ctx, const void *p)
{
	return le64_to_cpu(READ_ONCE(*(__le64 *)p));
}

static void ane_fw_store64(void *ctx, void *p, u64 value)
{
	WRITE_ONCE(*(__le64 *)p, cpu_to_le64(value));
}

static void ane_fw_rmb(void *ctx)
{
	dma_rmb();
}

static void ane_fw_wmb(void *ctx)
{
	dma_wmb();
}

static u32 ane_fw_read32(void *ctx, u32 reg)
{
	struct ane_fw_backend *b = ctx;

	return ane_rd32(b, b->engine + reg);
}

static void ane_fw_write32(void *ctx, u32 reg, u32 value)
{
	struct ane_fw_backend *b = ctx;

	ane_wr32(b, value, b->engine + reg);
}

static u64 ane_fw_now(void *ctx)
{
	return ktime_get_ns();
}

/*
 * Every wait polls. Sleep in the shared short steps first (see
 * ane_fw_poll_sleep_us()) and back off for long waits such as a stalled
 * request, so a stall costs no more CPU than a 1 ms poll.
 */
static void ane_fw_relax(void *ctx, u64 waited_ns)
{
	unsigned int min_us, max_us;

	min_us = ane_fw_poll_sleep_us(waited_ns, &max_us);
	usleep_range(min_us, max_us);
}

static void ane_fw_refused(void *ctx, u64 word0, u64 word1, u64 word2, int error)
{
	struct ane_fw_backend *b = ctx;

	dev_info(&b->pdev->dev, "SHAREDMALLOC refused tuple=%#llx/%#llx/%#llx error=%d\n",
		 word0, word1, word2, error);
}

static const struct ane_fw_transport_ops ane_fw_ops = {
	.load64 = ane_fw_load64, .store64 = ane_fw_store64,
	.read_barrier = ane_fw_rmb, .write_barrier = ane_fw_wmb,
	.read32 = ane_fw_read32, .write32 = ane_fw_write32,
	.now_ns = ane_fw_now, .relax = ane_fw_relax,
	.alloc = ane_fw_alloc, .refused = ane_fw_refused,
};

/*
 * Power in the order the H17 U2 runs measured: genpd ANE_SYS on, PWGATE read,
 * CPU power state on, PWGATE request cleared, sub-block power state on. Only
 * then are the engine and DARTs read and checked for their reset state, before
 * Linux writes anything to the engine. A failed check after power-on retains
 * power and mappings until reboot, like any other quarantine.
 */
static int ane_fw_power(struct ane_fw_backend *b, bool raw_boot)
{
	struct device *dev = &b->pdev->dev;
	u64 ce;
	int ret;

	b->power = dev_pm_domain_attach_by_id(dev, 0);
	if (IS_ERR_OR_NULL(b->power))
		return b->power ? PTR_ERR(b->power) : -ENODEV;
	ret = pm_runtime_resume_and_get(b->power);
	if (ret < 0)
		return ret;
	if ((ane_rd32(b, b->pmgr + 0x290) & 0xff) != 0xff)
		return -EIO;
	/* The child state may not have moved since the probe stage. */
	ane_fw_snapshot_pmgr(b);
	ane_fw_snapshot_gate(b);
	if (!ane_fw_cold_pmgr_valid(&b->cold, raw_boot) || !ane_fw_cold_gate_valid(&b->cold)) {
		dev_info(dev, "H17 ANE: child power or gate not in the cold state; refusing\n");
		return -EPERM;
	}
	ane_wr32(b, ane_rd32(b, b->pmgr + 0xc000) | 0xf, b->pmgr + 0xc000);
	ret = ane_poll32(b, b->pmgr + 0xc000, 0xff, 0xff, 1, 100000);
	if (ret)
		return ret;
	ane_wr32(b, 0, b->gate + 0xc);
	ret = ane_poll32(b, b->gate + 0xc, 3, 0, 1, 100000);
	if (ret)
		return ret;
	ane_wr32(b, ane_rd32(b, b->pmgr + 0xc008) | 0xf, b->pmgr + 0xc008);
	ret = ane_poll32(b, b->pmgr + 0xc008, 0xff, 0xff, 1, 100000);
	if (ret)
		return ret;
	ret = ane_poll32(b, b->gate + 0x30, U32_MAX, 0, 1, 100000);
	if (ret)
		return ret;
	b->powered = true;
	ane_fw_snapshot_engine(b);
	if (!ane_fw_cold_engine_valid(&b->cold)) {
		dev_info(dev, "H17 ANE: engine or DART not in its reset state; refusing\n");
		return -EPERM;
	}
	ane_wr64(b, 3, b->engine + 0x18a4000);
	ane_fw_trace(b, 'P', b->engine + 0x18a4008, 3);
	ret = readq_poll_timeout(b->engine + 0x18a4008, ce, (ce & 3) == 3, 1, 100000);
	if (ret)
		return ret;
	ane_wr64(b, 3, b->engine + 0x18a4008);
	if ((ane_rd64(b, b->engine + 0x18a4010) & 3) != 3 ||
	    ane_rd64(b, b->engine + 0x18a4018))
		return -EIO;
	dev_info(dev, "ANE power: ANE_SYS on, child/PWGATE/CE ready\n");
	return 0;
}

static int ane_fw_dapf(struct ane_fw_backend *b)
{
	unsigned int i;

	for (i = 0; i < ANE_FW_DAPF_RANGES; i++) {
		void __iomem *r = b->engine + 0x1810000 + i * 0x40;
		u64 start = b->ranges[i * 2], end = b->ranges[i * 2 + 1];

		if (ane_rd32(b, b->dart[0] + 0x208) & BIT(2))
			return -EPERM;
		ane_wr32(b, 0x31, r);
		ane_wr32(b, lower_32_bits(start), r + 8);
		ane_wr32(b, upper_32_bits(start), r + 0xc);
		ane_wr32(b, lower_32_bits(end), r + 0x10);
		ane_wr32(b, upper_32_bits(end), r + 0x14);
		ane_wr32(b, 1, r + 0x20);
		if ((ane_rd32(b, r) & 0x333) != 0x31 || ane_rd64(b, r + 8) != start ||
		    ane_rd64(b, r + 0x10) != end || (ane_rd32(b, r + 0x20) & 0xffff) != 1)
			return -EIO;
	}
	return 0;
}

static int ane_fw_preload_metadata(struct ane_fw_backend *b, u64 physical[2],
				   u64 size[2], u64 dva[2], u8 data_pin[SHA256_DIGEST_SIZE])
{
	struct device_node *np = b->pdev->dev.of_node;
	unsigned int i;
	int ret;

	if (!of_property_read_bool(np, "apple,firmware-preloaded") ||
	    of_property_count_u32_elems(np, "memory-region") != 2 ||
	    of_property_read_u64_array(np, "apple,firmware-iovas", dva, 2) ||
	    of_property_read_u8_array(np, "apple,preload-data-sha256", data_pin,
				      SHA256_DIGEST_SIZE))
		return -ENODATA;
	/* The same no-map, outside-System-RAM rule as the H16 preload. */
	for (i = 0; i < 2; i++) {
		const char *name;

		if (of_property_read_string_index(np, "memory-region-names", i, &name) ||
		    strcmp(name, i ? "firmware-data" : "firmware-text"))
			return -EINVAL;
		ret = ane_fw_preload_range(np, i, &physical[i], &size[i]);
		if (ret)
			return ret;
	}
	if (!ane_fw_preload_ranges_valid(b->soc, physical, size, dva))
		return -EINVAL;
	return 0;
}

static int ane_fw_preload(struct ane_fw_backend *b)
{
	static const u8 text_pin[SHA256_DIGEST_SIZE] = {
		0xcd, 0xa0, 0x9e, 0xc1, 0xae, 0xe6, 0x8b, 0x88,
		0xb4, 0x7e, 0xdc, 0x0e, 0xeb, 0xc8, 0x10, 0x45,
		0x25, 0x32, 0xa8, 0x55, 0x56, 0x3f, 0xbd, 0x65,
		0x0b, 0x95, 0xe2, 0x84, 0x6c, 0x34, 0x53, 0x10,
	};
	u64 physical[2], size[2], dva[2], tail, tail_size;
	struct ane_fw_surface snapshots[2], heap_tail;
	u8 digest[SHA256_DIGEST_SIZE], data_pin[SHA256_DIGEST_SIZE];
	unsigned int i;
	int ret;

	ret = ane_fw_preload_metadata(b, physical, size, dva, data_pin);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		void *source = memremap(physical[i], size[i], MEMREMAP_WB);
		struct sha256_ctx hash;

		if (!source)
			return -ENOMEM;
		ret = ane_fw_alloc_at(b, dva[i], size[i],
				      IOMMU_READ | (i ? IOMMU_WRITE : 0), &snapshots[i]);
		if (!ret)
			memcpy(snapshots[i].cpu, source, size[i]);
		memunmap(source);
		if (ret)
			return ret;
		sha256_init(&hash);
		sha256_update(&hash, snapshots[i].cpu, size[i]);
		sha256_final(&hash, digest);
		if (memcmp(digest, i ? data_pin : text_pin, sizeof(digest)))
			return -EKEYREJECTED;
	}
	/* Preserve every personalized DATA byte, including the iBoot TLV table. */
	dma_wmb();
	ret = ane_fw_heap_tail(dva, size, ANE_FW_POOL_DVA, &tail, &tail_size);
	if (ret || !tail_size)
		return ret;
	/* Zeroed by the coherent allocation; kept until reset like the preload. */
	ret = ane_fw_alloc_at(b, tail, tail_size, IOMMU_READ | IOMMU_WRITE, &heap_tail);
	if (!ret)
		dev_info(&b->pdev->dev, "ANE map: firmware heap tail at %#llx bytes %#llx\n",
			 tail, tail_size);
	return ret;
}

static int ane_fw_prepare(struct ane_fw_backend *b)
{
	struct device *dev = &b->pdev->dev;
	unsigned int i;
	int ret;

	ret = ane_fw_power(b, ane_fw_raw_boot());
	if (ret)
		return ret;
	ret = of_dma_configure(dev, dev->of_node, true);
	if (ret)
		return ret;
	ret = iommu_device_claim_dma_owner(dev, b);
	if (ret)
		return ret;
	b->domain = iommu_paging_domain_alloc(dev);
	if (IS_ERR(b->domain))
		return PTR_ERR(b->domain);
	ret = iommu_attach_device(b->domain, dev);
	if (ret)
		return ret;
	if (!(b->domain->pgsize_bitmap & SZ_16K) || !b->domain->ops->iotlb_sync_map ||
	    b->domain->geometry.aperture_start ||
	    b->domain->geometry.aperture_end < ANE_FW_ALLOC_DVA + SZ_512M)
		return -ERANGE;
	b->memory = root_device_register("ane-fw-memory");
	if (IS_ERR(b->memory))
		return PTR_ERR(b->memory);
	ret = dma_coerce_mask_and_coherent(b->memory, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = ane_fw_preload(b);
	if (ret)
		return ret;
	for (i = 0; i < ANE_FW_DAPF_RANGES; i++) {
		u64 page = round_down(b->ranges[i * 2], SZ_16K);

		ret = ane_fw_map(b, page, page, SZ_16K, IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO);
		if (ret)
			return ret;
	}
	ret = ane_fw_dapf(b);
	if (ret)
		return ret;
	ret = ane_fw_alloc_at(b, ANE_FW_POOL_DVA, SZ_256K,
			      IOMMU_READ | IOMMU_WRITE, &b->pool);
	if (ret)
		return ret;
	ret = ane_fw_alloc_at(b, ANE_FW_IPC_DVA, SZ_64K,
			      IOMMU_READ | IOMMU_WRITE, &b->ipc);
	if (ret)
		return ret;
	ret = ane_fw_alloc_at(b, ANE_FW_HEAP_DVA, 15 * SZ_1M,
			      IOMMU_READ | IOMMU_WRITE, &b->heap);
	if (!ret)
		dev_info(dev, "ANE map: reserved preload, SID0 on three DARTs, DAPF verified\n");
	return ret;
}

static int ane_fw_scratch_wait(struct ane_fw_backend *b, unsigned int cell, u32 wanted)
{
	return ane_poll32(b, b->engine + b->soc->scratch + cell * 4, U32_MAX, wanted,
			  100, 3000000);
}

static int ane_fw_boot(struct ane_fw_backend *b)
{
	void __iomem *scratch = b->engine + b->soc->scratch;
	u8 *init = b->pool.cpu + 0x180;
	u64 table;
	unsigned int i;
	int ret;

	for (i = 0; i < 8; i++)
		ane_wr32(b, 0, scratch + i * 4);
	ane_wr32(b, 1, scratch + 6 * 4);
	ane_wr32(b, 0, b->engine + b->soc->wrapper + 0x44);
	dma_wmb();
	ane_wr32(b, 0x10, b->engine + b->soc->wrapper + 0x44);
	ret = ane_fw_scratch_wait(b, 7, 0x08042006);
	if (ret)
		return ret;
	dev_info(&b->pdev->dev, "ANE boot: READY\n");
	if (ane_rd32(b, scratch) != 8 || ane_rd32(b, scratch + 4) != 0xc440 ||
	    ane_rd32(b, scratch + 12) != 0xf00000 || ane_rd32(b, scratch + 16))
		return -EPROTO;
	put_unaligned_le64(b->ipc.dva, init + 8);
	put_unaligned_le64(ANE_FW_SPAN, init + 0x10);
	put_unaligned_le64(0xfc80000, init + 0x18);
	put_unaligned_le64(b->heap.dva, init + 0x20);
	put_unaligned_le64(b->heap.size, init + 0x28);
	put_unaligned_le64(0xc441, init + 0x50);
	put_unaligned_le64(b->pool.dva, init + 0x58);
	put_unaligned_le64(b->pool.size, init + 0x60);
	put_unaligned_le32(64, init + 0x68);
	put_unaligned_le32(4, init + 0x12c);
	dma_wmb();
	ane_wr32(b, lower_32_bits(b->pool.dva + 0x180), scratch);
	ane_wr32(b, upper_32_bits(b->pool.dva + 0x180), scratch + 4);
	ane_wr32(b, 0xf7fbdff9, scratch + 7 * 4);
	ret = ane_fw_scratch_wait(b, 7, 0x08042006);
	if (ret)
		return ret;
	table = (u64)ane_rd32(b, scratch + 4) << 32 | ane_rd32(b, scratch);
	if (table != b->ipc.dva)
		return -EPROTO;
	ret = ane_fw_transport_init(&b->transport, b->soc, &ane_fw_ops, b,
				    b->ipc, b->pool, 0x4000, table, ANE_FW_TABLE_SIZE);
	if (ret)
		return ret;
	ret = ane_fw_transport_arm(&b->transport);
	if (ret)
		return ret;
	ane_wr32(b, 0x08042006, scratch + 3 * 4);
	ret = ane_fw_scratch_wait(b, 3, 0);
	if (!ret)
		dev_info(&b->pdev->dev, "ANE boot: eight channels armed, scratch3 cleared (polled)\n");
	return ret;
}

static int ane_fw_command(struct ane_fw_backend *b, unsigned int channel,
			  void *command, size_t size, u64 deadline)
{
	struct ane_fw_transaction *tx = &b->transport.pending[channel];
	int ret;

	ret = ane_fw_transport_publish(&b->transport, channel, command, size, deadline);
	if (ret)
		return ret;
	ret = ane_fw_transport_wait(&b->transport, channel);
	if (!ret)
		memcpy(command, b->pool.cpu + tx->dva - b->pool.dva, size);
	return ret;
}

static int ane_fw_startup(struct ane_fw_backend *b)
{
	static const struct { u16 id, size; } commands[] = {
		{ 4, 12 }, { 0, 12 }, { 3, 16 }, { 0x22, 100 },
	};
	u8 command[100];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		memset(command, 0, sizeof(command));
		put_unaligned_le16(commands[i].id, command + 4);
		ret = ane_fw_command(b, ANE_FW_IO, command, commands[i].size,
				     ktime_get_ns() + ANE_FW_TIMEOUT_NS);
		if (ret)
			return ret;
		if (commands[i].id == 4 && get_unaligned_le32(command + 8))
			return -EPROTO;
		if (commands[i].id == 3 && ane_fw_config_status(command, 16))
			return -EPROTO;
		dev_info(&b->pdev->dev, "ANE startup: command %#x ack=0\n", commands[i].id);
		if (commands[i].id == 3)
			dev_info(&b->pdev->dev, "ANE CONFIG_GET tick=%u\n",
				 get_unaligned_le32(command + 8));
		if (commands[i].id == 0x22)
			dev_info(&b->pdev->dev, "ANE RESOURCE_INFO: %*ph\n", 100, command);
	}
	return 0;
}

/* Private program mappings come from the same window as SHAREDMALLOC. */
static int ane_fw_program_map(void *ctx, size_t size, bool write, struct ane_fw_surface *out)
{
	struct ane_fw_backend *b = ctx;
	int ret;

	if (!size || !IS_ALIGNED(size, SZ_16K) ||
	    b->next_dva > ANE_FW_ALLOC_DVA + SZ_512M - size)
		return -ENOSPC;
	ret = ane_fw_alloc_mem(b, b->next_dva, size,
			       IOMMU_READ | (write ? IOMMU_WRITE : 0), true, out);
	if (ret)
		return ret;
	b->next_dva += size;
	return 0;
}

static void ane_fw_program_log(void *ctx, const char *item, u64 a, u64 c)
{
	struct ane_fw_backend *b = ctx;

	dev_info(&b->pdev->dev, "H17 %s %llx %llx\n", item, a, c);
}

/* Program surfaces are cacheable: clean before the engine, invalidate after. */
static void ane_fw_program_sync(void *ctx, const struct ane_fw_surface *s, bool for_device)
{
	struct ane_fw_backend *b = ctx;

	if (for_device)
		dma_sync_single_for_device(b->memory, s->handle, s->size, DMA_BIDIRECTIONAL);
	else
		dma_sync_single_for_cpu(b->memory, s->handle, s->size, DMA_BIDIRECTIONAL);
}

static const struct ane_h16_session_ops ane_fw_session_ops = {
	.map = ane_fw_program_map,
	.log = ane_fw_program_log,
	.sync = ane_fw_program_sync,
};

/*
 * After startup: the shared program session and its ABI 3 node. From here on
 * the node serializes all firmware traffic. The firmware frees each process
 * pool after UNLOAD, often only during a later program, so FREE stays
 * admitted; backing is retained until reboot.
 */
/*
 * <debugfs>/ane-h17/<dev>/execute: per-phase EXECUTE times of the shared
 * session, see ane_h16_session_stats_show(). Writing anything resets them.
 * Created at the program stage; the retained backend pins the module, so
 * the file and its session live until reboot.
 */
static int ane_fw_execute_show(struct seq_file *m, void *v)
{
	return ane_h16_session_stats_show(m, m->private);
}

static int ane_fw_execute_open(struct inode *inode, struct file *file)
{
	return single_open(file, ane_fw_execute_show, inode->i_private);
}

static ssize_t ane_fw_execute_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	ane_h16_session_stats(((struct seq_file *)file->private_data)->private, NULL, true);
	return count;
}

static const struct file_operations ane_fw_execute_fops = {
	.owner = THIS_MODULE,
	.open = ane_fw_execute_open,
	.read = seq_read,
	.write = ane_fw_execute_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void ane_fw_debugfs(struct ane_fw_backend *b)
{
	struct dentry *root = debugfs_lookup("ane-h17", NULL);
	bool found = root;

	if (!found)
		root = debugfs_create_dir("ane-h17", NULL);
	debugfs_create_file("execute", 0600,
			    debugfs_create_dir(dev_name(&b->pdev->dev), root),
			    &b->session, &ane_fw_execute_fops);
	if (found)
		dput(root);
}

static int ane_fw_program_stage(struct ane_fw_backend *b)
{
	int ret;

	b->transport.allow_free = true;
	ret = ane_h16_session_init(&b->session, &b->transport, &ane_fw_session_ops, b,
				   ANE_FW_TIMEOUT_NS);
	if (ret)
		return ret;
	ret = ane_h16_accel_register(&b->pdev->dev, &b->session);
	if (ret)
		return ret;
	ane_fw_debugfs(b);
	dev_info(&b->pdev->dev, "ANE program: session and ABI 3 node ready\n");
	return 0;
}

static ssize_t firmware_stage_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const char * const names[] = {
		"unverified", "probe", "map", "boot", "startup", "program", "quarantined",
	};
	struct ane_fw_backend *b = ane_backend;

	return sysfs_emit(buf, "%s\n", names[READ_ONCE(b->stage)]);
}

static ssize_t ane_fw_stage_action(struct device *dev, const char *buf, size_t count)
{
	struct ane_fw_backend *b = ane_backend;
	enum ane_fw_stage next;
	int ret = 0;

	guard(mutex)(&b->lock);
	if (b->stage == ANE_STAGE_QUARANTINED)
		return -EIO;
	if (sysfs_streq(buf, "probe")) {
		if (b->retained)
			return -EBUSY;
		/* PMGR only: ANE_SYS may be off, which makes the engine unreadable. */
		ane_fw_snapshot_pmgr(b);
		if (!ane_fw_cold_pmgr_valid(&b->cold, ane_fw_raw_boot())) {
			dev_info(dev, "H17 ANE: cold acquisition unproven (loader or child power state); refusing\n");
			return -EPERM;
		}
		b->stage = ANE_STAGE_PROBED;
		dev_info(dev, "ANE probe: raw-boot evidence and cold child power verified; no writes\n");
		return count;
	}
	if (sysfs_streq(buf, "map"))
		next = ANE_STAGE_MAPPED;
	else if (sysfs_streq(buf, "boot"))
		next = ANE_STAGE_BOOTED;
	else if (sysfs_streq(buf, "startup"))
		next = ANE_STAGE_STARTUP;
	else if (sysfs_streq(buf, "program"))
		next = ANE_STAGE_PROGRAM;
	else
		return -EINVAL;
	if (next != b->stage + 1)
		return -EINVAL;
	if (next == ANE_STAGE_MAPPED) {
		u64 physical[2], size[2], dva[2];
		u8 data_pin[SHA256_DIGEST_SIZE];

		ane_fw_snapshot_pmgr(b);
		if (!ane_fw_cold_pmgr_valid(&b->cold, ane_fw_raw_boot())) {
			dev_info(dev, "H17 ANE: cold acquisition unproven (loader or child power state); refusing\n");
			return -EPERM;
		}
		ret = ane_fw_preload_metadata(b, physical, size, dva, data_pin);
		if (ret) {
			dev_info(dev, "H17 ANE: loader preload reservation or digest missing; refusing\n");
			return ret;
		}
		ret = ane_fw_retain(b);
		if (ret)
			return ret;
		ret = ane_fw_prepare(b);
	} else if (next == ANE_STAGE_BOOTED) {
		ret = ane_fw_boot(b);
	} else if (next == ANE_STAGE_STARTUP) {
		ret = ane_fw_startup(b);
	} else {
		ret = ane_fw_program_stage(b);
	}
	if (ret)
		return ane_fw_quarantine(b, ret);
	b->stage = next;
	return count;
}

static ssize_t firmware_stage_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int sleep_flags = lock_system_sleep();
	ssize_t ret = ane_fw_stage_action(dev, buf, count);

	unlock_system_sleep(sleep_flags);
	return ret;
}
static DEVICE_ATTR_RW(firmware_stage);

static void ane_fw_observer_free(struct ane_fw_backend *b)
{
	unsigned int i;

	for (i = 0; i < ANE_FW_DARTS; i++)
		if (b->dart[i])
			iounmap(b->dart[i]);
	if (b->engine)
		iounmap(b->engine);
	if (b->pmgr)
		iounmap(b->pmgr);
	if (b->gate)
		iounmap(b->gate);
	put_device(&b->pdev->dev);
	kfree(b);
}

/* Record a mapped register window; the trace hook names accesses by PA. */
static int ane_fw_window(struct ane_fw_backend *b, unsigned int index, void __iomem *base,
			 struct device_node *np, unsigned int reg)
{
	struct resource res;

	if (!base)
		return -ENOMEM;
	if (of_address_to_resource(np, reg, &res))
		return -EINVAL;
	b->window[index].base = base;
	b->window[index].pa = res.start;
	b->window[index].size = resource_size(&res);
	return 0;
}

int ane_fw_staged_init(struct platform_device *pdev, const struct ane_fw_platform *soc)
{
	struct device_node *np = pdev->dev.of_node;
	struct ane_fw_backend *b;
	unsigned int i;
	u32 dart_phandles[ANE_FW_DARTS];
	int ret = -EINVAL;

	if (ane_backend || soc->soc != 0x8140)
		return -EOPNOTSUPP;
	b = kzalloc_obj(*b);
	if (!b)
		return -ENOMEM;
	b->pdev = pdev;
	get_device(&pdev->dev);
	b->soc = soc;
	b->next_dva = ANE_FW_ALLOC_DVA;
	mutex_init(&b->lock);
	INIT_LIST_HEAD(&b->mappings);
	if (of_property_count_strings(np, "reg-names") != soc->reg_count ||
	    of_irq_count(np) != soc->interrupt_count ||
	    of_count_phandle_with_args(np, "power-domains", "#power-domain-cells") != 1)
		goto fail;
	for (i = 0; i < soc->reg_count; i++) {
		struct resource res;
		const char *name;

		if (of_address_to_resource(np, i, &res) ||
		    resource_size(&res) != soc->reg_sizes[i] ||
		    of_property_read_string_index(np, "reg-names", i, &name) ||
		    strcmp(name, soc->reg_names[i]))
			goto fail;
	}
	if (of_property_read_u64_array(np, "apple,dapf-ranges", b->ranges,
				       ARRAY_SIZE(b->ranges)))
		goto fail;
	for (i = 0; i < ANE_FW_DAPF_RANGES; i++) {
		u64 start = b->ranges[i * 2], end = b->ranges[i * 2 + 1];

		if (start > end || round_down(start, SZ_16K) != round_down(end, SZ_16K))
			goto fail;
		for (unsigned int j = 0; j < i; j++)
			if (round_down(start, SZ_16K) == round_down(b->ranges[j * 2], SZ_16K))
				goto fail;
	}
	if (of_count_phandle_with_args(np, "iommus", "#iommu-cells") != ANE_FW_DARTS)
		goto fail;
	for (i = 0; i < ANE_FW_DARTS; i++) {
		struct of_phandle_args args;

		ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", i, &args);
		if (ret)
			goto fail;
		if (args.args_count != 1 || args.args[0] ||
		    !of_device_is_compatible(args.np, "apple,t8140-ane-dart")) {
			of_node_put(args.np);
			ret = -EINVAL;
			goto fail;
		}
		for (unsigned int j = 0; j < i; j++) {
			if (dart_phandles[j] == args.np->phandle) {
				of_node_put(args.np);
				ret = -EINVAL;
				goto fail;
			}
		}
		dart_phandles[i] = args.np->phandle;
		b->dart[i] = of_iomap(args.np, 0);
		ret = ane_fw_window(b, 3 + i, b->dart[i], args.np, 0);
		of_node_put(args.np);
		if (ret)
			goto fail;
	}
	b->engine = of_iomap(np, 0);
	b->pmgr = of_iomap(np, 1);
	b->gate = of_iomap(np, 2);
	ret = ane_fw_window(b, 0, b->engine, np, 0);
	if (!ret)
		ret = ane_fw_window(b, 1, b->pmgr, np, 1);
	if (!ret)
		ret = ane_fw_window(b, 2, b->gate, np, 2);
	if (ret)
		goto fail;
	ane_backend = b;
	ret = device_create_file(&pdev->dev, &dev_attr_firmware_stage);
	if (ret) {
		ane_backend = NULL;
		goto fail;
	}
	dev_info(&pdev->dev, "H17 ANE: read-only staged observer ready; IRQs 655/668 remain masked\n");
	return 0;
fail:
	ane_fw_observer_free(b);
	return ret;
}

bool ane_fw_staged_exit(void)
{
	struct ane_fw_backend *b = ane_backend;

	if (!b)
		return false;
	/* A retained backend pins the module. No unload or in-place retry. */
	if (WARN_ON(b->retained))
		return true;
	device_remove_file(&b->pdev->dev, &dev_attr_firmware_stage);
	ane_backend = NULL;
	ane_fw_observer_free(b);
	return true;
}
