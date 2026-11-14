// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * H16/H17 facts from the 2026-10-31 H16/H17 specification, sections 3-7,
 * 12 and 15, and the H16 cold-boot facts, round 5. Metadata preflight
 * performs no MMIO, DMA mapping or power access.
 */
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <crypto/sha2.h>

#include "ane_fw_cold.h"
#include "ane_fw_platform.h"
#include "ane_h16_boot_data.h"

static const char * const ane_h16_reg_names[] = {
	"engine", "pmgr", "aux-0", "aux-1", "aux-2", "aux-3", "aux-4",
};

static const u32 ane_h16_reg_sizes[] = {
	0x2000000, 0x18000, 0x4000, 0x30000, 0xff4000, 0x2000000, 0x4000,
};

/* Offsets in reg[1]; genpd, never the ANE driver, owns transitions. */
static const u32 ane_h16_power_words[] = {
	0x3c0, 0xc008, 0xc000, 0xc010, 0xc018,
};

static const char * const ane_h17_reg_names[] = { "engine", "pmgr", "pwgate" };
static const u32 ane_h17_reg_sizes[] = { 0x2000000, 0x18000, 0x34 };

/*
 * The measured TEXT of each staged firmware. A different build is refused
 * before anything is powered or written.
 */
static const u8 ane_h17_text_pin_26_6_2[] = {
	0xcd, 0xa0, 0x9e, 0xc1, 0xae, 0xe6, 0x8b, 0x88,
	0xb4, 0x7e, 0xdc, 0x0e, 0xeb, 0xc8, 0x10, 0x45,
	0x25, 0x32, 0xa8, 0x55, 0x56, 0x3f, 0xbd, 0x65,
	0x0b, 0x95, 0xe2, 0x84, 0x6c, 0x34, 0x53, 0x10,
};

static const u8 ane_t6050_text_pin_27_0[] = {
	0x93, 0xae, 0x3a, 0x37, 0x1c, 0xaa, 0x93, 0xf2,
	0xa9, 0x26, 0x8c, 0x5b, 0xfd, 0x94, 0xba, 0xa9,
	0x9d, 0x8d, 0xd5, 0x9c, 0xbe, 0xc9, 0x99, 0xe2,
	0xd1, 0xa2, 0x20, 0xd0, 0xee, 0x43, 0x8b, 0x8f,
};

const struct ane_fw_platform ane_h16_platform = {
	.h16_profiles = ane_h16_profiles,
	.h16_profile_count = ARRAY_SIZE(ane_h16_profiles),
	.h16_mmio = ane_h16_mmio_t6041,
	.h16_mmio_count = ARRAY_SIZE(ane_h16_mmio_t6041),
	.dart_params = 0x8,
	.dapf_dart = 0,
	.dapf_offset = 0x10000,
	.dapf_size = SZ_16K,
	/* The DT compatible is t6041 on the M4 Max and the M4 Pro (T6040) alike. */
	.soc = 0x6040,
	.chip_family = 16,
	.ane_type = 0x110,
	.hwx_subtype = 7,
	.task_generation = 17,
	.interrupt_count = 1,
	/* J616s IRQ 1054 drains replies and events from all F2H channels. */
	.irq_role = ANE_FW_IRQ_CHANNELS,
	.owner = ANE_FW_OWNER_AP,
	.reg_names = ane_h16_reg_names,
	.reg_sizes = ane_h16_reg_sizes,
	.reg_count = ARRAY_SIZE(ane_h16_reg_names),
	.wrapper = 0x1600000,
	.mailbox = 0x1608000,
	.scratch = 0x1880020,
	.doorbell = 0x1884000,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.power_words = ane_h16_power_words,
	.power_count = ARRAY_SIZE(ane_h16_power_words),
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0) | BIT(11),
	/*
	 * System sleep cycles the engine cold and replays every program.
	 * The warm path stays off: on 25G83 the J616s firmware does not take
	 * SUSPEND (0x24), bare or after SET_SNE_PMU_BASE2 and
	 * EXCLAVE_MODE_STOP.
	 */
	.sleep_cold = true,
	.firmware_preloaded = true,
	.safe_reset = false,
	.pmp_report = "apple,t6041-pmp-v2-report",
};

const struct ane_fw_platform ane_h17_platform = {
	.soc = 0x8140,
	.chip_family = 17,
	.ane_type = 0x200,
	.hwx_subtype = 9,
	.task_generation = 19,
	.interrupt_count = 2,
	.owner = ANE_FW_OWNER_EXCLAVE,
	.reg_names = ane_h17_reg_names,
	.reg_sizes = ane_h17_reg_sizes,
	.reg_count = ARRAY_SIZE(ane_h17_reg_names),
	.wrapper = 0x1600000,
	.mailbox = 0x1608000,
	.scratch = 0x1880020,
	.doorbell = 0x1884000,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0),
	.firmware_preloaded = true,
	.safe_reset = false,
	/* 25G83 boots this firmware in Exclave mode (SPEC-ANE-SLEEP-H17.md). */
	.exclave_mode_stop = true,
	.macos_sleep = true,
	/*
	 * Off: on 25G83 SUSPEND is still not taken after 0x31 (C7), so sleep
	 * cycles the engine cold and replays every program behind its handle.
	 */
	.sleep_warm = false,
	/* Cold AP-direct acquisition is staged before any power or DMA write.
	 * The observer uses the measured H17 26.6.2 register layout.
	 */
	.pmgr_sys = 0x290,
	.pmgr_cpu = 0xc000,
	.pmgr_subblock = 0xc008,
	.dart_compatible = "apple,t8140-ane-dart",
	.dart_revision = 0x203,
	.dapf_count = 4,
	.text_pin = ane_h17_text_pin_26_6_2,
};

/*
 * The M5 Pro (J714s) ANE: the H17 engine and firmware protocol (ADT
 * "ane,t8132exclave", ane-type 0x220), exclave-assigned and preloaded by
 * iBoot. From the ADT and macOS's own bring-up on this machine: ANE_SYS at
 * PMGR +0x3a8, then ANE_CPU (+0x3c8) and ANE_TDBASE (+0x3d0) behind it; the
 * scratch cells start at +0x1880028; the DARTs are revision 0x204; five DAPF
 * records. System sleep cycles the engine cold, as on T8140.
 */
const struct ane_fw_platform ane_t6050_platform = {
	.soc = 0x6050,
	.chip_family = 17,
	.ane_type = 0x220,
	.hwx_subtype = 9,
	.task_generation = 19,
	.interrupt_count = 2,
	.owner = ANE_FW_OWNER_EXCLAVE,
	.reg_names = ane_h17_reg_names,
	.reg_sizes = ane_h17_reg_sizes,
	.reg_count = ARRAY_SIZE(ane_h17_reg_names),
	.wrapper = 0x1600000,
	.mailbox = 0x1608000,
	.scratch = 0x1880028,
	.doorbell = 0x1884000,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0),
	.firmware_preloaded = true,
	.safe_reset = false,
	.sleep_warm = false,
	.pmgr_sys = 0x3a8,
	.pmgr_cpu = 0x3c8,
	.pmgr_subblock = 0x3d0,
	/* Normal T6050 DARTs, as on T6041; SID0 is the host's. */
	.dart_compatible = "apple,t6050-dart",
	.dart_revision = 0x204,
	.dapf_count = 5,
	.text_pin = ane_t6050_text_pin_27_0,
};

/*
 * The M5 (T8142, J813/J815) ANE: "ane,t8132exclave", ane-type 0x210,
 * exclave-assigned and preloaded by iBoot. From the T8142 ADT: ANE_SYS at
 * PMGR +0x2e8 (pmgr device 73, parent AFISOCNI3), ANE_CPU +0xc000 and
 * ANE_TDBASE +0xc008 as on T8140; four DAPF records. The macOS 27.0 (26A428)
 * stub's iBoot preloads TEXT 0xc0000 and DATA 0x2ac000 at T8140's IOVAs,
 * measured on a J815 under aura with TEXT sha256 1212c260...b6d4e3 (the 27.2
 * host's preload is 0xbc000/0x2b0000 at 0x100_000b_c000). Not measured yet:
 * the PWGATE window, the scratch cells and the DART revision (T8140's values
 * below). Until they are, the zero pin never matches: the hand-off is checked
 * and logged and the engine is left alone. ane_t8142_text_26a428 is the pin
 * to use then.
 */
static const u8 ane_t8142_text_pin_unmeasured[SHA256_DIGEST_SIZE];
static const u8 ane_t8142_text_26a428[] __maybe_unused = {
	0x12, 0x12, 0xc2, 0x60, 0x6e, 0xf8, 0xca, 0xce,
	0x51, 0xfd, 0x95, 0x1e, 0x60, 0xbc, 0xce, 0x75,
	0xbe, 0x8e, 0x65, 0xd5, 0xf0, 0xfe, 0x19, 0x37,
	0xac, 0x3b, 0x58, 0x07, 0x49, 0xd6, 0xb4, 0xe3,
};

const struct ane_fw_platform ane_t8142_platform = {
	.soc = 0x8142,
	.chip_family = 17,
	.ane_type = 0x210,
	.hwx_subtype = 9,
	.task_generation = 19,
	.interrupt_count = 2,
	.owner = ANE_FW_OWNER_EXCLAVE,
	.reg_names = ane_h17_reg_names,
	.reg_sizes = ane_h17_reg_sizes,
	.reg_count = ARRAY_SIZE(ane_h17_reg_names),
	.wrapper = 0x1600000,
	.mailbox = 0x1608000,
	.scratch = 0x1880020,
	.doorbell = 0x1884000,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.preload_dva = { 0x10000000000, 0x100000c0000 },
	.preload_size = { 0xc0000, 0x2ac000 },
	.stream_mask = BIT(0),
	.firmware_preloaded = true,
	.safe_reset = false,
	.sleep_warm = false,
	.pmgr_sys = 0x2e8,
	.pmgr_cpu = 0xc000,
	.pmgr_subblock = 0xc008,
	.dart_compatible = "apple,t8142-dart",
	.dart_revision = 0x203,
	.dapf_count = 4,
	.text_pin = ane_t8142_text_pin_unmeasured,
};

/*
 * The iBoot preload is firmware memory. Accept it only as a no-map child of
 * /reserved-memory that Linux does not manage as System RAM, whether the
 * loader reserved it at boot or left it outside the memory map. The physical
 * range comes from the node itself, so the check does not depend on when the
 * node was added.
 */
int ane_fw_preload_range(struct device_node *np, unsigned int index, u64 *phys, u64 *size)
{
	struct device_node *reserved __free(device_node) =
		of_find_node_by_path("/reserved-memory");
	struct device_node *mem __free(device_node) =
		of_parse_phandle(np, "memory-region", index);
	struct resource r;

	if (!reserved || !mem || mem->parent != reserved ||
	    !of_device_is_available(mem) ||
	    !of_property_read_bool(mem, "no-map") ||
	    of_property_read_bool(mem, "reusable") ||
	    of_address_to_resource(mem, 0, &r) || !resource_size(&r))
		return -EINVAL;
	if (region_intersects(r.start, resource_size(&r), IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return -EBUSY;
	*phys = r.start;
	*size = resource_size(&r);
	return 0;
}
EXPORT_SYMBOL_GPL(ane_fw_preload_range);

static int ane_fw_preload_check(struct device *dev,
				const struct ane_fw_platform *soc)
{
	struct device_node *np = dev->of_node;
	u64 phys[2], size[2], dva[2];
	unsigned int i;
	int ret;

	if (of_property_count_u32_elems(np, "memory-region") != 2 ||
	    of_property_count_strings(np, "memory-region-names") != 2 ||
	    of_property_count_u64_elems(np, "apple,firmware-iovas") != 2 ||
	    of_property_read_u64_array(np, "apple,firmware-iovas", dva, 2))
		return -ENODATA;

	for (i = 0; i < 2; i++) {
		struct device_node *mem __free(device_node) =
			of_parse_phandle(np, "memory-region", i);
		const char *name;

		/* Board DTs carry disabled placeholders the loader fills in. */
		if (mem && !of_device_is_available(mem))
			return -ENODEV;
		if (of_property_read_string_index(np, "memory-region-names", i,
						  &name) ||
		    strcmp(name, i ? "firmware-data" : "firmware-text"))
			return -EINVAL;
		ret = ane_fw_preload_range(np, i, &phys[i], &size[i]);
		if (ret)
			return ret;
	}

	return ane_fw_preload_ranges_valid(soc, phys, size, dva) ? 0 : -EINVAL;
}

int ane_fw_platform_preflight(struct platform_device *pdev,
			      const struct ane_fw_platform *soc)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	u32 ane_type;
	unsigned int i;
	int ret;

	/* No backend has acquired ownership, even if DT omits the marker.
	 * Check before resources, firmware, power, IOMMU or engine access.
	 */
	/*
	 * H17 is started (or left alone) by the staged path before the driver
	 * registers; the platform probe never acquires it. -ENODEV keeps the
	 * driver core quiet; the staged path already said why.
	 */
	if (soc->owner == ANE_FW_OWNER_EXCLAVE)
		return -ENODEV;
	if (of_property_read_bool(np, "apple,exclave-assigned")) {
		dev_info(dev, "ANE: exclave-assigned device has no Linux ownership provider\n");
		return -EOPNOTSUPP;
	}

	if (!of_property_read_bool(np, "apple,firmware-preloaded") ||
	    of_property_read_u32(np, "apple,ane-type", &ane_type) ||
	    ane_type != soc->ane_type)
		return dev_err_probe(dev, -EINVAL, "H16 firmware identity missing or invalid\n");

	if (of_property_count_strings(np, "reg-names") != soc->reg_count ||
	    platform_get_resource(pdev, IORESOURCE_MEM, soc->reg_count) ||
	    of_irq_count(np) != soc->interrupt_count)
		return dev_err_probe(dev, -EINVAL, "H16 resource count mismatch\n");

	for (i = 0; i < soc->reg_count; i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   soc->reg_names[i]);
		if (!res || resource_size(res) != soc->reg_sizes[i])
			return dev_err_probe(dev, -EINVAL, "H16 resource geometry mismatch\n");
	}

	ret = ane_fw_preload_check(dev, soc);
	if (ret == -ENODEV) {
		dev_info(dev, "H16 firmware preload not handed off by the bootloader\n");
		return ret;
	}
	if (ret)
		return dev_err_probe(dev, ret,
			"H16 requires bootloader-reserved no-map TEXT/DATA preload ranges\n");

	if (!soc->h16_profile_count || !soc->h16_mmio_count || soc->dapf_dart >= 3 ||
	    !soc->dapf_size || of_property_count_u64_elems(np, "apple,dapf-ranges") != 8)
		return -ENODATA;
	return 0;
}

int ane_fw_platform_probe(struct platform_device *pdev, const struct ane_fw_platform *soc)
{
	int ret;

	/* H17: the staged backend owns the engine; the device binds for it. */
	if (ane_fw_platform_staged(soc))
		return ane_fw_staged_bind(pdev);
	ret = ane_fw_platform_preflight(pdev, soc);

	if (ret)
		return ret;
	return ane_h16_backend_probe(pdev, soc);
}

void ane_fw_platform_remove(struct platform_device *pdev)
{
	if (ane_fw_staged_bound(&pdev->dev)) {
		ane_fw_staged_unbind(pdev);
		return;
	}
	ane_h16_backend_remove(pdev);
}

/* System sleep callbacks of the bound H16 or H17 device. */
int ane_fw_platform_suspend(struct device *dev)
{
	if (ane_fw_staged_bound(dev))
		return ane_fw_staged_suspend(dev);
	return ane_h16_backend_suspend(dev);
}

int ane_fw_platform_resume(struct device *dev)
{
	if (ane_fw_staged_bound(dev))
		return 0;
	return ane_h16_backend_resume(dev);
}
