#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production T6050 ISP host DART runtime restore and managed links.

Verifies two properties against the actual functions in drivers/iommu/apple-dart.c:
1. Runtime power-off restore (apple_dart_isp_host_reset / _restore / _retained)
   restores host SIDs (0, 1, 6, 7, 10, 11, 12), their TCR/TTBRs, and error masks
   while preserving non-host / secure core SIDs (4, 5, 8, 9) in ENABLE_STREAMS,
   ERR_SID_ENABLE, TCR, and TTBR registers.
2. apple_dart_probe_device() creates persistent managed DL_FLAG_PM_RUNTIME links
   (no DL_FLAG_STATELESS, DL_FLAG_RPM_ACTIVE, or DL_FLAG_AUTOREMOVE_*) across
   all ISP DARTs, returns -ENODEV and frees cfg on link failure without calling
   device_link_del()/device_link_remove() on managed links of a registered
   consumer, and apple_dart_release_device() cleans up cfg without calling
   device_link_remove().

Includes negative controls for both properties.
"""

from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile


def extract_define(source, name):
    m = re.search(r"^#define\s+" + re.escape(name) + r"(?=\s|\()", source, re.M)
    assert m, f"define not found: {name}"
    start = m.start()
    end = start
    while True:
        nl = source.find("\n", end)
        if nl == -1:
            end = len(source)
            break
        end = nl + 1
        if not source[:nl].endswith("\\"):
            break
    return source[start:end].rstrip("\n")


def extract_function(source, name):
    m = re.search(
        r"^static[^;{}()]*?\b" + re.escape(name) + r"\([^;{}]*\)\s*\{",
        source,
        re.M,
    )
    assert m, f"function not found: {name}"
    start = source.index("{", m.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[m.start():end]


CONSTANTS = (
    "DART_MAX_STREAMS",
    "DART_MAX_TTBR",
    "MAX_DARTS_PER_DEVICE",
    "DART_T8110_ERROR_MASK",
    "DART_T8110_ENABLE_STREAMS",
    "DART_T8110_DISABLE_STREAMS",
    "DART_T8110_V2_ERROR_MASK",
    "DART_T8110_ERR_IRQ_STATUS",
    "DART_T8110_ERR_IRQ_ENABLE",
    "DART_T8110_ERR_SID_ENABLE",
    "DART_T8110_V24_ERR_IRQ_STATUS",
    "DART_T8110_V24_ERR_IRQ_ENABLE",
    "DART_T8110_V24_ERR_SID_ENABLE",
    "DART_T8110_TCR_REMAP",
    "DART_T8110_TCR_REMAP_ENABLE",
    "DART_TCR",
    "DART_TTBR",
    "DART_T6050_ISP_HOST_SIDS",
)

FUNCTIONS = (
    "apple_dart_hw_disable_dma",
    "apple_dart_hw_clear_ttbr",
    "apple_dart_hw_clear_all_ttbrs",
    "apple_dart_hw_apply_remaps",
    "apple_dart_isp_host_enable_errors",
    "apple_dart_isp_host_reset",
    "apple_dart_save_state",
    "apple_dart_isp_host_retained",
    "apple_dart_isp_host_restore",
    "apple_dart_probe_device",
    "apple_dart_release_device",
)

PRELUDE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u64 dma_addr_t;

#define BIT(n) (1UL << (n))
#define BITS_PER_LONG 64
#define BITS_TO_LONGS(n) (((n) + 63) / 64)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctzll(m)) & (m))
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define kfree(p) free(p)
#define APPLE_DART_WRITER_CLOSED 2

#define bitmap_zero(p, n) memset((p), 0, BITS_TO_LONGS(n) * sizeof(unsigned long))
#define __set_bit(n, p) ((p)[(n) / 64] |= BIT((n) % 64))
#define for_each_set_bit(b, map, n) \
	for ((b) = 0; (b) < (int)(n); (b)++) \
		if ((map)[(b) / 64] & BIT((b) % 64))

#define dev_err(dev, fmt, ...) ((void)(dev), errors_logged++)
#define dev_dbg(dev, fmt, ...) ((void)(dev))
#define dev_err_ratelimited(dev, fmt, ...) ((void)(dev))

#define DART_T8110 2
#define DL_FLAG_STATELESS BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER BIT(1)
#define DL_FLAG_PM_RUNTIME BIT(2)
#define DL_FLAG_RPM_ACTIVE BIT(3)
#define DL_FLAG_AUTOREMOVE_SUPPLIER BIT(4)

struct device;
struct device_link {
	struct device *consumer;
	struct device *supplier;
	u32 flags;
	bool active;
};
struct iommu_device { int unused; };
struct device {
	void *iommu_priv;
	bool has_fwspec;
	bool registered;
};

struct apple_dart;
struct apple_dart_stream_map {
	struct apple_dart *dart;
	DECLARE_BITMAP(sidmap, 256);
};
struct apple_dart_master_cfg {
	struct apple_dart_stream_map stream_maps[8];
};
#define for_each_stream_map(i, base, m) \
	for ((i) = 0, (m) = &(base)->stream_maps[0]; \
	     (i) < MAX_DARTS_PER_DEVICE && (m)->dart; \
	     (m) = &(base)->stream_maps[++(i)])

struct apple_dart_hw {
	int type;
	int ttbr_count;
	u32 tcr;
	u32 ttbr;
	u32 tcr_disabled;
	u32 ttbr_valid;
	int (*invalidate_tlb)(struct apple_dart_stream_map *stream_map);
};

struct apple_dart {
	const struct apple_dart_hw *hw;
	struct device *dev;
	struct iommu_device iommu;
	u8 regs[0x9000];
	unsigned int version;
	int num_streams;
	bool locked;
	bool isp_host;
	bool isp_sleep_lost;
	bool full_errors;
	u32 save_err_irq_enable;
	u32 save_tcr[256];
	u32 save_ttbr[256][4];
	DECLARE_BITMAP(remapped, 256);
	u8 remap_target[256];
	void *sleep_record;
};

static int errors_logged;
static bool sim_locked;
static bool sim_fault_pending;
static int sim_tlb_err;
static int tlb_invalidations;
static unsigned long tlb_sid_mask;

static u32 readl(const volatile void *addr)
{
	u32 v;
	memcpy(&v, (const void *)addr, sizeof(v));
	return v;
}

static void writel(u32 val, volatile void *addr)
{
	memcpy((void *)addr, &val, sizeof(val));
}

static u32 apple_dart_readl(struct apple_dart *dart, u32 offset)
{
	assert(offset + sizeof(u32) <= sizeof(dart->regs));
	return readl(dart->regs + offset);
}

static void apple_dart_writel(struct apple_dart *dart, u32 val, u32 offset)
{
	assert(offset + sizeof(u32) <= sizeof(dart->regs));
	writel(val, dart->regs + offset);
}

static bool apple_dart_is_locked(struct apple_dart *dart)
{
	(void)dart;
	return sim_locked;
}

static bool apple_dart_fault_pending(struct apple_dart *dart)
{
	(void)dart;
	return sim_fault_pending;
}

static int test_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	tlb_invalidations++;
	tlb_sid_mask |= stream_map->sidmap[0];
	return sim_tlb_err;
}

/* Probe / release device-link mocks */
struct apple_dart_dcp { struct apple_dart *dart; };
static void *dev_iommu_priv_get(struct device *dev) { return dev->iommu_priv; }
static void dev_iommu_priv_set(struct device *dev, void *p) { dev->iommu_priv = p; }
static bool dev_iommu_fwspec_get(struct device *dev) { return dev->has_fwspec; }
static bool apple_dart_has_borrowed(struct apple_dart *d) { (void)d; return false; }
static struct apple_dart_dcp *apple_dart_borrowed_cfg(struct apple_dart_master_cfg *c) { (void)c; return NULL; }
static bool apple_dart_pm_get(struct apple_dart *d) { (void)d; return false; }
static void apple_dart_pm_put(struct apple_dart *d) { (void)d; }
static int apple_dart_dcp_validate(struct apple_dart_dcp *b, struct device *d) { (void)b; (void)d; return 0; }
static void apple_dart_sleep_release_failed(void) { abort(); }
static int apple_dart_sleep_writer_get(struct apple_dart *d) { (void)d; return 0; }
static void apple_dart_sleep_writer_put(struct apple_dart *d, int g) { (void)d; (void)g; }
static int pm_runtime_resume_and_get(struct device *d) { (void)d; return 0; }
static void pm_runtime_put(struct device *d) { (void)d; }
static int apple_dart_hw_unmap_locked_ttbr(struct apple_dart_stream_map *m, u8 idx) { (void)m; (void)idx; return 0; }

static int v27_removed;
static void apple_dart_v27_client_remove(struct apple_dart_stream_map *m)
{
	(void)m;
	v27_removed++;
}

static struct device_link sim_links[16];
static int sim_link_count;
static int sim_fail_link_call;
static int sim_link_add_calls;
static int sim_link_del_calls;
static int sim_link_remove_calls;

static int active_link_count(void)
{
	int active = 0;
	for (int i = 0; i < sim_link_count; i++)
		if (sim_links[i].active)
			active++;
	return active;
}

static struct device_link *device_link_add(struct device *consumer, struct device *supplier, u32 flags)
{
	sim_link_add_calls++;
	if (sim_fail_link_call && sim_link_add_calls == sim_fail_link_call)
		return NULL;
	assert(sim_link_count < (int)ARRAY_SIZE(sim_links));
	struct device_link *l = &sim_links[sim_link_count++];
	l->consumer = consumer;
	l->supplier = supplier;
	l->flags = flags;
	l->active = true;
	return l;
}

static void device_link_del(struct device_link *link)
{
	sim_link_del_calls++;
	assert(link && link->active);
	/* drivers/base/core.c device_link_put_kref() WARNs on registered managed consumers. */
	assert((link->flags & DL_FLAG_STATELESS) || !link->consumer->registered);
	link->active = false;
}

static void device_link_remove(void *consumer, struct device *supplier)
{
	sim_link_remove_calls++;
	for (int i = 0; i < sim_link_count; i++) {
		if (sim_links[i].active && sim_links[i].consumer == consumer &&
		    sim_links[i].supplier == supplier) {
			assert((sim_links[i].flags & DL_FLAG_STATELESS) ||
			       !sim_links[i].consumer->registered);
			sim_links[i].active = false;
			return;
		}
	}
	assert(!"device_link_remove called for missing link");
}
"""

TESTS = r"""
#define SECURE_SIDS (BIT(4) | BIT(5) | BIT(8) | BIT(9))
#define NON_HOST_EXTRA (SECURE_SIDS | BIT(15))

static void init_isp_dart(struct apple_dart *dart, const struct apple_dart_hw *hw,
			  struct device *dev, unsigned int version)
{
	memset(dart, 0, sizeof(*dart));
	dart->hw = hw;
	dart->dev = dev;
	dart->version = version;
	dart->num_streams = 16;
	dart->isp_host = true;
	/* Typical T6050 ISP remap: stream 6/7/10/11 remapped, 0/1/12 direct. */
	__set_bit(6, dart->remapped);
	dart->remap_target[6] = 0;
}

static void test_runtime_restore_preserves_secure_sids(const struct apple_dart_hw *hw,
							unsigned int version, bool sleep_lost)
{
	struct device dev = { 0 };
	struct apple_dart dart;
	bool v24 = version >= 0x0204;
	u32 sid_en_reg = v24 ? DART_T8110_V24_ERR_SID_ENABLE(0) : DART_T8110_ERR_SID_ENABLE(0);
	u32 irq_en_reg = v24 ? DART_T8110_V24_ERR_IRQ_ENABLE : DART_T8110_ERR_IRQ_ENABLE;

	init_isp_dart(&dart, hw, &dev, version);
	errors_logged = 0;
	sim_locked = false;
	sim_fault_pending = false;
	sim_tlb_err = 0;
	tlb_invalidations = 0;
	tlb_sid_mask = 0;

	/* Configure active host streams and secure streams before runtime suspend. */
	apple_dart_writel(&dart, DART_T6050_ISP_HOST_SIDS | SECURE_SIDS, DART_T8110_ENABLE_STREAMS);
	apple_dart_writel(&dart, 0, DART_T8110_V2_ERROR_MASK);
	apple_dart_writel(&dart, DART_T6050_ISP_HOST_SIDS | SECURE_SIDS, sid_en_reg);
	apple_dart_writel(&dart, 0x5a5a0001u, irq_en_reg);

	apple_dart_writel(&dart, 0x1u, DART_TCR(&dart, 0));
	apple_dart_writel(&dart, 0xdead0001u, DART_TTBR(&dart, 0, 0));
	apple_dart_writel(&dart, 0xdead0002u, DART_TTBR(&dart, 0, 1));
	apple_dart_writel(&dart, 0x1u, DART_TCR(&dart, 12));
	apple_dart_writel(&dart, 0xcafe0001u, DART_TTBR(&dart, 12, 0));

	apple_dart_save_state(&dart);
	dart.save_err_irq_enable = apple_dart_readl(&dart, irq_en_reg);

	/* Simulate ps_isp_sys power loss wiping host streams, then secure core re-enables 4/5/8/9. */
	memset(dart.regs, 0, sizeof(dart.regs));
	apple_dart_writel(&dart, 0xffffffffu, DART_T8110_V2_ERROR_MASK);
	apple_dart_writel(&dart, NON_HOST_EXTRA, DART_T8110_ENABLE_STREAMS);
	apple_dart_writel(&dart, SECURE_SIDS, sid_en_reg);
	apple_dart_writel(&dart, 0x80000000u, irq_en_reg);

	/* Secure SID TCR/TTBR programmed by secure core must remain untouched too. */
	apple_dart_writel(&dart, 0x3u, DART_TCR(&dart, 4));
	apple_dart_writel(&dart, 0x44440001u, DART_TTBR(&dart, 4, 0));
	apple_dart_writel(&dart, 0x3u, DART_TCR(&dart, 5));
	apple_dart_writel(&dart, 0x55550001u, DART_TTBR(&dart, 5, 0));
	apple_dart_writel(&dart, 0x3u, DART_TCR(&dart, 8));
	apple_dart_writel(&dart, 0x88880001u, DART_TTBR(&dart, 8, 0));
	apple_dart_writel(&dart, 0x3u, DART_TCR(&dart, 9));
	apple_dart_writel(&dart, 0x99990001u, DART_TTBR(&dart, 9, 0));

	assert(!apple_dart_isp_host_retained(&dart));
	dart.isp_sleep_lost = sleep_lost;

	assert(apple_dart_isp_host_restore(&dart) == 0);
	assert(!dart.isp_sleep_lost);
	assert(!errors_logged);

	/* All host SIDs AND all non-host / secure SIDs survive in ENABLE_STREAMS. */
	assert(apple_dart_readl(&dart, DART_T8110_ENABLE_STREAMS) ==
	       (DART_T6050_ISP_HOST_SIDS | NON_HOST_EXTRA));

	/* Secure SID bits in ERR_SID_ENABLE and saved IRQ enables are preserved. */
	assert(apple_dart_readl(&dart, sid_en_reg) ==
	       (DART_T6050_ISP_HOST_SIDS | SECURE_SIDS));
	assert((apple_dart_readl(&dart, irq_en_reg) & 0xda5a0001u) == 0xda5a0001u);
	assert(apple_dart_readl(&dart, DART_T8110_V2_ERROR_MASK) == 0);

	/* Host stream translations and secure stream translations are both intact. */
	assert(apple_dart_readl(&dart, DART_TCR(&dart, 0)) == 0x1u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 0, 0)) == 0xdead0001u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 0, 1)) == 0xdead0002u);
	assert(apple_dart_readl(&dart, DART_TCR(&dart, 12)) == 0x1u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 12, 0)) == 0xcafe0001u);

	assert(apple_dart_readl(&dart, DART_TCR(&dart, 4)) == 0x3u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 4, 0)) == 0x44440001u);
	assert(apple_dart_readl(&dart, DART_TCR(&dart, 5)) == 0x3u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 5, 0)) == 0x55550001u);
	assert(apple_dart_readl(&dart, DART_TCR(&dart, 8)) == 0x3u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 8, 0)) == 0x88880001u);
	assert(apple_dart_readl(&dart, DART_TCR(&dart, 9)) == 0x3u);
	assert(apple_dart_readl(&dart, DART_TTBR(&dart, 9, 0)) == 0x99990001u);

	/* TLB invalidations covered only host SIDs, never secure SIDs. */
	assert(tlb_invalidations == 7);
	assert(tlb_sid_mask == DART_T6050_ISP_HOST_SIDS);

	/* Subsequent retained check passes even if secure core changes secure SIDs. */
	apple_dart_writel(&dart, 0x77770001u, DART_TTBR(&dart, 4, 0));
	assert(apple_dart_isp_host_retained(&dart));
}

static void test_probe_managed_links(void)
{
	struct device isp_dev = { .has_fwspec = true, .registered = true };
	struct device d0_dev = { 0 }, d1_dev = { 0 }, d2_dev = { 0 };
	struct apple_dart d0 = { .dev = &d0_dev, .isp_host = true };
	struct apple_dart d1 = { .dev = &d1_dev, .isp_host = true };
	struct apple_dart d2 = { .dev = &d2_dev, .isp_host = true };
	struct apple_dart_master_cfg *cfg;

	/* Fail on the 3rd ISP DART link: returns -ENODEV, frees cfg, never calls del/remove on managed links. */
	cfg = calloc(1, sizeof(*cfg));
	assert(cfg);
	cfg->stream_maps[0].dart = &d0;
	cfg->stream_maps[1].dart = &d1;
	cfg->stream_maps[2].dart = &d2;
	isp_dev.iommu_priv = cfg;

	sim_link_count = 0;
	sim_link_add_calls = 0;
	sim_link_del_calls = 0;
	sim_link_remove_calls = 0;
	v27_removed = 0;
	sim_fail_link_call = 3;

	struct iommu_device *res = apple_dart_probe_device(&isp_dev);
	assert(IS_ERR(res) && PTR_ERR(res) == -ENODEV);
	assert(isp_dev.iommu_priv == NULL);
	assert(sim_link_add_calls == 3);
	assert(sim_link_del_calls == 0);
	assert(sim_link_remove_calls == 0);
	assert(active_link_count() == 2);
	for (int i = 0; i < 2; i++)
		assert(sim_links[i].flags == DL_FLAG_PM_RUNTIME);
	assert(v27_removed == 3);

	/* Succeed on all 3 ISP DARTs: persistent managed DL_FLAG_PM_RUNTIME links, clean release. */
	cfg = calloc(1, sizeof(*cfg));
	assert(cfg);
	cfg->stream_maps[0].dart = &d0;
	cfg->stream_maps[1].dart = &d1;
	cfg->stream_maps[2].dart = &d2;
	isp_dev.iommu_priv = cfg;

	sim_link_count = 0;
	sim_link_add_calls = 0;
	sim_link_del_calls = 0;
	sim_link_remove_calls = 0;
	v27_removed = 0;
	sim_fail_link_call = 0;

	res = apple_dart_probe_device(&isp_dev);
	assert(res == &d0.iommu);
	assert(active_link_count() == 3);
	for (int i = 0; i < 3; i++)
		assert(sim_links[i].flags == DL_FLAG_PM_RUNTIME);

	apple_dart_release_device(&isp_dev);
	assert(sim_link_del_calls == 0);
	assert(sim_link_remove_calls == 0);
	assert(active_link_count() == 3);
	assert(v27_removed == 3);
}

int main(void)
{
	const struct apple_dart_hw hw = {
		.type = DART_T8110,
		.ttbr_count = 4,
		.tcr = 0x1000,
		.ttbr = 0x1400,
		.tcr_disabled = 0,
		.ttbr_valid = BIT(0),
		.invalidate_tlb = test_invalidate_tlb,
	};

	test_runtime_restore_preserves_secure_sids(&hw, 0x0202, false);
	test_runtime_restore_preserves_secure_sids(&hw, 0x0204, false);
	test_runtime_restore_preserves_secure_sids(&hw, 0x0204, true);
	test_probe_managed_links();

	puts("PASS T6050 ISP host DART runtime restore preserves secure SIDs and probe creates managed links");
	return 0;
}
"""


def build_and_run(tmp, name, code):
    src = Path(tmp) / f"{name}.c"
    exe = Path(tmp) / name
    src.write_text(code)
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-sign-compare",
            "-Wno-unused-parameter",
            "-Wno-unused-function",
            "-g",
            "-fsanitize=address,undefined",
            str(src),
            "-o",
            str(exe),
        ],
        check=True,
    )
    return subprocess.run([str(exe)], capture_output=True, text=True)


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    root = Path(__file__).resolve().parents[2]
    dart_src = (root / "drivers/iommu/apple-dart.c").read_text()

    defines = "\n".join(extract_define(dart_src, name) for name in CONSTANTS)
    bodies = "\n".join(extract_function(dart_src, name) for name in FUNCTIONS)
    prod = defines + "\n" + PRELUDE + "\n" + bodies + "\n" + TESTS

    # Negative control 1 (S1): overwriting ENABLE_STREAMS with host clears secure SIDs.
    rmw_old = (
        "apple_dart_writel(dart,\n"
        "\t\t\t  apple_dart_readl(dart, DART_T8110_ENABLE_STREAMS) | host,\n"
        "\t\t\t  DART_T8110_ENABLE_STREAMS);"
    )
    assert prod.count(rmw_old) == 1, "S1 read-modify-write target not found"
    mut_s1 = prod.replace(
        rmw_old, "apple_dart_writel(dart, host, DART_T8110_ENABLE_STREAMS);"
    )

    # Negative control 2 (S2): adding DL_FLAG_STATELESS drops managed unbind ordering.
    flags_old = (
        "\t\tif (!stream_map->dart->isp_host)\n"
        "\t\t\tflags |= DL_FLAG_AUTOREMOVE_SUPPLIER | DL_FLAG_RPM_ACTIVE;"
    )
    assert prod.count(flags_old) == 1, "S2 managed flags target not found"
    mut_s2_stateless = prod.replace(
        flags_old,
        "\t\tif (stream_map->dart->isp_host)\n"
        "\t\t\tflags |= DL_FLAG_STATELESS;\n"
        "\t\telse\n"
        "\t\t\tflags |= DL_FLAG_AUTOREMOVE_SUPPLIER | DL_FLAG_RPM_ACTIVE;",
    )

    # Negative control 3 (S2 link failure): ignoring failed ISP supplier link leaves unbound DARTs.
    fail_old = (
        "\t\tif (!device_link_add(dev, stream_map->dart->dev, flags) &&\n"
        "\t\t    stream_map->dart->isp_host) {\n"
        "\t\t\tret = -ENODEV;\n"
        "\t\t\tgoto err_free_cfg;\n"
        "\t\t}"
    )
    assert prod.count(fail_old) == 1, "S2 link failure check not found"
    mut_s2_fail = prod.replace(
        fail_old,
        "\t\tdevice_link_add(dev, stream_map->dart->dev, flags);",
    )

    with tempfile.TemporaryDirectory(prefix="apple-dart-isp-host-") as tmp:
        res = build_and_run(tmp, "production", prod)
        if res.returncode != 0:
            sys.stderr.write(res.stdout + res.stderr)
            raise SystemExit(1)
        sys.stdout.write(res.stdout)

        expected_failures = (
            (
                "overwrite-enable-streams",
                mut_s1,
                "test_runtime_restore_preserves_secure_sids: "
                "Assertion `apple_dart_readl(&dart, DART_T8110_ENABLE_STREAMS) == "
                "(DART_T6050_ISP_HOST_SIDS | NON_HOST_EXTRA)' failed.",
            ),
            (
                "stateless-isp-link",
                mut_s2_stateless,
                "test_probe_managed_links: "
                "Assertion `sim_links[i].flags == DL_FLAG_PM_RUNTIME' failed.",
            ),
            (
                "ignore-isp-link-failure",
                mut_s2_fail,
                "test_probe_managed_links: "
                "Assertion `IS_ERR(res) && PTR_ERR(res) == -ENODEV' failed.",
            ),
        )
        for label, code, expected in expected_failures:
            mres = build_and_run(tmp, label, code)
            output = mres.stdout + mres.stderr
            if mres.returncode == 0:
                raise SystemExit(f"negative control {label} unexpectedly passed")
            if expected not in output:
                sys.stderr.write(output)
                raise SystemExit(
                    f"negative control {label} failed unexpectedly "
                    f"(exit {mres.returncode}); expected {expected!r}"
                )
            print(f"PASS negative control {label}: caught (exit {mres.returncode})")


if __name__ == "__main__":
    main()
