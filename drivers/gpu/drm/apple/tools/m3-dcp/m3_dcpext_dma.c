// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S external DART additive mapping diagnostic. No firmware messages.
 * Adapted from the M4 m4_dcpext_dma_probe mapper; no M4 PHY/session code.
 * T6030 SID5 TCR=0xc01 and roots qualified in the guarded 2026-09-21 audit.
 * DVA 0x40000000 belongs to M3 firmware TEXT: use empty slot 0x20000000.
 * PTE encoding and ordering: Asahi io-pgtable-dart.c, fba91e5d9577.
 * T8110 SID flush: Asahi apple-dart.c, same revision.
 * Reservations: M3 m1n1 9b9fa13 handoff and same-boot J514S table audit.
 * This is not a DMA execution test: the private DVA is never sent to DCP.
 */
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#ifndef DCPEXT_INSTANCE
#define DCPEXT_INSTANCE 0
#endif

#define LEASE_PAGE 0x4000
#ifdef DCPEXT_WITH_SESSION
#define LEASE_SIZE 0x400000
#else
#define LEASE_SIZE 0x10000
#endif
#define LEASE_DVA  0x20000000ULL
#define LEASE_SLOT (LEASE_DVA >> 25)
#define PTE_ADDRESS GENMASK_ULL(39, 10)
/* T6030 DCP is not dma-coherent. Match dma-iommu + dart_prot_to_pte:
 * shared host buffers need DART2 NO_CACHE, unlike firmware-owned TEXT/DATA.
 */
#define PTE_RW (GENMASK_ULL(51, 40) | BIT_ULL(1) | BIT_ULL(0))

static unsigned int port = DCPEXT_INSTANCE;
module_param(port, uint, 0400);
static int result = -EINPROGRESS;
module_param(result, int, 0400);
MODULE_PARM_DESC(result, "0 only after private mapping removal and inherited root comparison");
static struct device *dma_dev;
static void *pool, *leaf;
static dma_addr_t pool_dma;
static u64 *root, *saved;
static void __iomem *regs;
static phys_addr_t dart_base;
static bool region_owned, pinned;
#ifdef DCPEXT_WITH_SESSION
static bool native_desktop;
module_param(native_desktop,bool,0400);
static bool native_kms_trial;
module_param(native_kms_trial,bool,0400);
static bool native_frame;
module_param(native_frame,bool,0400);
static bool native_startup;
module_param(native_startup,bool,0400);
#include "m3_dcpext_frame.h"
#include "m3_dcpext_session.h"
#endif

static u64 encode_address(phys_addr_t address)
{
	return (address >> 4) & PTE_ADDRESS;
}

static int fabric_powered(void);
static int remove_private_mapping(void);
static void cleanup(void);


static int flush_sid(void)
{
	u32 status;

	/* DART table walks are coherent, as in apple_dart_finalize_domain. */
	/* Completion polling orders reuse; a diagnostic sleep here would
	 * impose a 20 ms floor on every runtime scanout remapping. */
	dma_wmb();
	writel(0x100 | 5, regs + 0x80);
	return readl_poll_timeout(regs + 0x80, status, !(status & BIT(31)), 1, 10000);
}

static int fabric_powered(void)
{
	struct device_node *node;
	struct regmap *map;
	u32 state, i, offset = port ? 0x3c0 : 0x3b8;
	int ret;

	node = of_find_node_by_path("/soc/power-management@350700000");
	if (!node || !of_device_is_compatible(node, "apple,t6030-pmgr")) {
		of_node_put(node);
		return -ENODEV;
	}
	map = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(map))
		return PTR_ERR(map);
	for (i = 0; i < 2; i++) {
		ret = regmap_read(map, offset + (i ? (port ? 0x30 : 0x20) : 0), &state);
		if (ret)
			return ret;
		if ((state & 0xff) != 0xff ||
		    state & (BIT(31) | BIT(28) | BIT(12) | BIT(11) | BIT(10)))
			return -EHOSTDOWN;
	}
	return 0;
}

static void cleanup(void)
{
#ifdef DCPEXT_WITH_SESSION
	session_cleanup();
#endif
	if (root)
		memunmap(root);
	if (leaf)
		free_page((unsigned long)leaf);
	if (pool)
		dma_free_coherent(dma_dev, LEASE_SIZE, pool, pool_dma);
	if (dma_dev)
		root_device_unregister(dma_dev);
	if (regs)
		iounmap(regs);
	if (region_owned)
		release_mem_region(dart_base, 0x4000);
	kfree(saved);
}

/* Called only after protocol completion, or by the no-firmware mapper. On
 * any mismatch all allocations and the module reference remain owned. */
static int remove_private_mapping(void)
{
	u64 entry = encode_address(virt_to_phys(leaf)) | 1;
	unsigned int i;
	int ret;

	/* Verify only our private entries; inherited mappings must be unchanged. */
	for (i = 0; i < LEASE_SIZE / LEASE_PAGE; i++)
		if ((((u64 *)leaf)[i] & PTE_ADDRESS) << 4 != pool_dma + i * LEASE_PAGE) {
			ret = -EIO;
			return ret;
		}
	ret = -EAGAIN;
	if (cmpxchg64_relaxed(&root[LEASE_SLOT], entry, 0) != entry)
		return ret;
	ret = flush_sid();
	if (ret)
		return ret;
	if (memcmp(root, saved, LEASE_PAGE) || (readl(regs + 0x100) & BIT(31))) {
		ret = -EIO;
		return ret;
	}
	return 0;
}

static int __init probe_init(void)
{
	struct device_node *dcp = NULL, *dart = NULL, *ram = NULL;
	struct resource resource, reservation;
	phys_addr_t root_pa, leaf_pa;
	const char *uuid;
	char alias[16];
	u32 marker, ttbr, tcr, protect, params[2];
	u64 entry;
	int count, segment_bytes, i, ret = -ENODEV;

	if (port > 1 || PAGE_SIZE != LEASE_PAGE ||
	    !of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	snprintf(alias, sizeof(alias), "dcpext%u", port);
	dcp = of_find_node_by_path(alias);
	if (!dcp || of_device_is_available(dcp) ||
	    !of_device_is_compatible(dcp, "apple,t6030-dcpext") ||
	    of_property_read_u32(dcp, "apple,j514s-dcpext-reservations", &marker) || marker != 1 ||
	    of_property_read_string(dcp, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "DDF38191-93B3-324A-BC8F-643006F5AC82"))
		goto done;
	if (!of_get_property(dcp, "apple,j514s-dcpext-segments", &segment_bytes) ||
	    segment_bytes <= 0 || segment_bytes % 32)
		goto done;
	count = of_property_count_u32_elems(dcp, "memory-region");
	if (count != segment_bytes / 32 + 3 ||
	    of_property_read_u32_array(dcp, "iommus", params, 2) || params[1] != 5)
		goto done;
	dart = of_find_node_by_phandle(params[0]);
	ram = of_parse_phandle(dcp, "memory-region", count - 3);
	dart_base = port ? 0x2d530c000ULL : 0x2d130c000ULL;
	if (!dart || of_device_is_available(dart) ||
	    !of_device_is_compatible(dart, "apple,t8110-dart") ||
	    of_address_to_resource(dart, 0, &resource) || resource.start != dart_base ||
	    resource_size(&resource) != 0x4000 || !ram ||
	    !of_device_is_compatible(ram, "apple,dart-mem") ||
	    !of_property_read_bool(ram, "no-map") ||
	    of_address_to_resource(ram, 0, &reservation))
		goto done;
	ret = fabric_powered();
	if (ret)
		goto done;
	ret = -EBUSY;
	if (!request_mem_region(dart_base, 0x4000, "m3-dcpext-dma-probe"))
		goto done;
	region_owned = true;
	/* Match of_iomap/devm_ioremap_resource: T6030 soc is nonposted-mmio. */
	if (!(resource.flags & IORESOURCE_MEM_NONPOSTED)) {
		ret = -EINVAL;
		goto done;
	}
	regs = ioremap_np(dart_base, 0x4000);
	ret = -ENOMEM;
	if (!regs)
		goto done;
	protect = readl(regs + 0x200);
	tcr = readl(regs + 0x1000 + 5 * 4);
	ttbr = readl(regs + 0x1400 + 5 * 4);
	root_pa = (u64)((ttbr & GENMASK(29, 2)) >> 2) << 14;
	pr_info("m3_dcpext_dma: protect=%#x tcr=%#x ttbr=%#x root=%pap reservation=%pr error=%#x\n",
		protect, tcr, ttbr, &root_pa, &reservation, readl(regs + 0x100));
	ret = -EINVAL;
	/* T8110 bits 11:8 are ignored while REMAP_EN (bit 7) is clear.
	 * Firmware also leaves 0x801 here; require all active controls exactly
	 * as before and recheck the untouched full TCR before publication. */
	if (!(protect & 1) || (tcr & ~GENMASK(11, 8)) != 1 || !(ttbr & 1) ||
	    root_pa < reservation.start || root_pa > reservation.end ||
	    reservation.end - root_pa + 1 < LEASE_PAGE || (readl(regs + 0x100) & BIT(31)))
		goto done;
	root = memremap(root_pa, LEASE_PAGE, MEMREMAP_WB);
	ret = -ENOMEM;
	if (!root)
		goto done;
	saved = kmemdup(root, LEASE_PAGE, GFP_KERNEL);
	leaf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!saved || !leaf)
		goto done;
	ret = -EBUSY;
	if (READ_ONCE(root[LEASE_SLOT]))
		goto done;
#ifdef DCPEXT_WITH_SESSION
 ret = session_prepare(dcp);
 if (!ret) ret = session_tables(&reservation);
 if (!ret) ret = session_audit();
 if (ret) goto done;
#endif
	dma_dev = root_device_register(port ? "m3-dcpext1" : "m3-dcpext-dma-probe");
	if (IS_ERR(dma_dev)) {
		ret = PTR_ERR(dma_dev);
		dma_dev = NULL;
		goto done;
	}
	/* No OF node / IOMMU attachment: obtain a host-owned direct allocation.
	 * Require identity DMA-to-physical translation for the DART leaf.
	 */
	ret = dma_coerce_mask_and_coherent(dma_dev, DMA_BIT_MASK(42));
	if (ret)
		goto done;
	pool = dma_alloc_coherent(dma_dev, LEASE_SIZE, &pool_dma, GFP_KERNEL);
	ret = -ENOMEM;
	if (!pool)
		goto done;
	leaf_pa = virt_to_phys(leaf);
	pr_info("m3_dcpext_dma: pool=%pad physical=%#llx leaf=%pap\n",
		&pool_dma, (u64)dma_to_phys(dma_dev, pool_dma), &leaf_pa);
	ret = -EINVAL;
	if ((pool_dma | leaf_pa) & (LEASE_PAGE - 1) ||
	    dma_to_phys(dma_dev, pool_dma) != pool_dma ||
	    pool_dma + LEASE_SIZE > BIT_ULL(42) || leaf_pa >= BIT_ULL(42))
		goto done;
	for (i = 0; i < LEASE_SIZE / LEASE_PAGE; i++)
		((u64 *)leaf)[i] = encode_address(pool_dma + i * LEASE_PAGE) | PTE_RW;
	memset(pool, 0xa5, LEASE_SIZE);
	dma_wmb();
	entry = encode_address(leaf_pa) | 1;
	ret = -EAGAIN;
	if (readl(regs + 0x1400 + 5 * 4) != ttbr ||
	    readl(regs + 0x1000 + 5 * 4) != tcr ||
	    readl(regs + 0x200) != protect || memcmp(root, saved, LEASE_PAGE))
		goto done;
	/* A published table must survive failed invalidation/teardown. */
	__module_get(THIS_MODULE);
	pinned = true;
	if (cmpxchg64_relaxed(&root[LEASE_SLOT], 0, entry)) {
		module_put(THIS_MODULE);
		pinned = false;
		goto done;
	}
	ret = flush_sid();
	if (ret)
		goto done;
	pr_info("m3_dcpext_dma_probe: port=%u SID5 private DVA=%#llx bytes=%#x mapped; no firmware notification\n",
		port, LEASE_DVA, LEASE_SIZE);
#ifdef DCPEXT_WITH_SESSION
 ret = frame_prepare_publish(dcp,count);
 if(!ret)ret = pio_prepare_publish(dcp,count);
 if(!ret)ret = session_start();
 goto done; /* Keep every DMA allocation and module reference until reboot. */
#endif
	ret = remove_private_mapping();
	if (ret)
		goto done;
	module_put(THIS_MODULE);
	pinned = false;
	pr_info("m3_dcpext_dma_probe: private mapping removed; inherited root unchanged\n");
done:
	pr_info("m3_dcpext_dma: result=%d published=%u\n", ret, pinned);
	of_node_put(ram);
	of_node_put(dart);
	of_node_put(dcp);
	result = ret;
	if (pinned) {
		pr_info("m3_dcpext_dma_probe: result=%d; retaining all allocations/resources until reboot\n", ret);
		return 0;
	}
	cleanup();
	return ret;
}

static void __exit probe_exit(void)
{
}
module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S external DART additive mapping/remove diagnostic, no firmware execution");
