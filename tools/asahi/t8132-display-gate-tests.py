#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for the T8132 fixed-DP display gate (drivers/soc/apple/display-gate.c)."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract_block(source, marker):
    match = None
    for cand in re.finditer(marker, source, re.M):
        semi = source.find(';', cand.start())
        brace = source.find('{', cand.start())
        if brace >= 0 and (semi < 0 or brace < semi):
            match = cand
            break
    assert match, f'missing marker: {marker}'
    start = match.start()
    brace = source.index('{', start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    if pos < len(source) and source[pos] == ';':
        pos += 1
    return source[start:pos] + '\n'


def main():
    gate_src = (ROOT / 'drivers/soc/apple/display-gate.c').read_text()
    disp_hdr = (ROOT / 'include/linux/soc/apple/display.h').read_text()
    t8132_hdr = (ROOT / 'include/linux/soc/apple/t8132.h').read_text()
    dart_hdr = (ROOT / 'include/linux/soc/apple/dart.h').read_text()

    # Enforce at source level that T8132 fixed-DP gate matches the SoC, not a board name,
    # and carries no hard-coded board node paths.
    soc_block = extract_block(gate_src, r'^static const struct gate_soc gate_t8132\b')
    assert '.machine = "apple,t8132"' in soc_block, soc_block
    assert 'j773g' not in soc_block, soc_block
    assert 't8132_j773g_hdmi_nodes' not in gate_src

    uuid_lines = '\n'.join(
        line for line in disp_hdr.splitlines()
        if line.startswith('#define APPLE_T8132_DCP_26B123_UUID')
    )
    helpers = '\n'.join([
        uuid_lines,
        extract_block(disp_hdr, r'^static inline bool apple_dcp_image_v27_2\('),
        extract_block(t8132_hdr, r'^static inline bool apple_t8132_dcpext_v27_2\('),
        extract_block(dart_hdr, r'^static inline bool apple_dart_keeps_locked_roots\('),
        extract_block(gate_src, r'^static int gate_iommus_append\('),
        extract_block(gate_src, r'^static struct of_changeset \*__init gate_cs_alloc\('),
        extract_block(gate_src, r'^static void __init gate_cs_free\('),
        extract_block(gate_src, r'^static int __init gate_iommus_set\('),
        extract_block(gate_src, r'^enum \{\n\tT8132_HDMI_PHY,'),
        extract_block(gate_src, r'^static bool __init staged_node_firmware_memory\('),
        extract_block(gate_src, r'^static bool __init gate_t8132_dcp_v27_2\('),
        extract_block(gate_src, r'^static void __init gate_t8132_put_nodes\('),
        extract_block(gate_src, r'^static int __init gate_t8132_resolve_fixed_dp\('),
        extract_block(gate_src, r'^static bool __init gate_t8132_fixed_dp_ready\('),
        extract_block(gate_src, r'^static void __init gate_t8132_display_off\('),
        extract_block(gate_src, r'^static void __init gate_t8132_run\('),
    ])

    program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint32_t __be32;
#define __init
#define __initconst
#define GFP_KERNEL 0
#define CONFIG_VIDEO 1
#define IS_ENABLED(x) (x)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define kmalloc(n, g) malloc(n)
#define kfree(p) free(p)

static __be32 cpu_to_be32(u32 v) { return __builtin_bswap32(v); }
static u32 be32_to_cpu(__be32 v) { return __builtin_bswap32(v); }

struct gate_soc {
	const char *machine;
	const char *prefix;
	const char *name;
	const char *dcp_compat;
	const char *display_compat;
};

static const struct gate_soc test_soc = {
	.machine = "apple,t8132",
	.prefix = "apple-t8132-display: ",
	.name = "T8132",
	.dcp_compat = "apple,t8132-dcp",
	.display_compat = "apple,t8132-display-subsystem",
};
static const struct gate_soc *gate_soc = &test_soc;
static const char *staged_prefix;
static bool nomodeset;
static unsigned int err_count, info_count;

#define staged_info(...) do { (void)staged_prefix; info_count++; } while (0)
#define staged_err(...) do { (void)staged_prefix; err_count++; } while (0)

static bool video_firmware_drivers_only(void) { return nomodeset; }

struct property {
	const char *name;
	void *value;
	size_t length;
};

struct device_node {
	const char *path;
	const char *alias;
	const char *name;
	const char *compatible;
	u32 phandle;
	bool available;
	int refcount;

	/* Firmware identity */
	const char *fw_uuid;
	int fw_uuid_count;
	u32 fw_ver[3];
	int fw_ver_cells;
	u32 fw_compat[3];
	int fw_compat_cells;
	bool has_clock_ids;

	/* Phandle references */
	const char *phy_names[4];
	int phy_name_count;
	struct device_node *phys[4];

	struct device_node *iommus[4];
	u32 iommu_args[4];
	int iommu_count;
	struct property iommus_prop;
	__be32 iommus_buf[8];

	struct device_node *mboxes[2];
	int mbox_count;

	struct device_node *mem_regions[4];
	int mem_region_count;

	struct device_node *piodma_child;
};

struct of_phandle_args {
	struct device_node *np;
	int args_count;
	u32 args[4];
};

struct cs_op {
	struct device_node *np;
	bool is_status;
	const char *status;
	struct property *prop;
};

struct of_changeset {
	struct cs_op ops[16];
	unsigned int nops;
	bool applied;
};

#define MAX_NODES 32
static struct device_node *all_nodes[MAX_NODES];
static unsigned int nr_nodes;
static struct of_changeset *applied_cs[8];
static unsigned int nr_applied_cs;

static void sync_iommus_prop(struct device_node *np)
{
	int i;

	if (np->iommu_count <= 0) {
		np->iommus_prop.name = NULL;
		np->iommus_prop.value = NULL;
		np->iommus_prop.length = 0;
		return;
	}
	for (i = 0; i < np->iommu_count; i++) {
		np->iommus_buf[2 * i] = cpu_to_be32(np->iommus[i]->phandle);
		np->iommus_buf[2 * i + 1] = cpu_to_be32(np->iommu_args[i]);
	}
	np->iommus_prop.name = "iommus";
	np->iommus_prop.value = np->iommus_buf;
	np->iommus_prop.length = (size_t)np->iommu_count * 2 * sizeof(__be32);
}

static struct device_node *of_node_get(struct device_node *np)
{
	if (np)
		np->refcount++;
	return np;
}

static void of_node_put(struct device_node *np)
{
	if (np) {
		assert(np->refcount > 0);
		np->refcount--;
	}
}

static bool of_device_is_compatible(const struct device_node *np, const char *compat)
{
	return np && np->compatible && !strcmp(np->compatible, compat);
}

static bool of_device_is_available(const struct device_node *np)
{
	return np && np->available;
}

static bool of_property_present(const struct device_node *np, const char *name)
{
	if (!np)
		return false;
	if (!strcmp(name, "apple,firmware-compat"))
		return np->fw_compat_cells > 0;
	if (!strcmp(name, "apple,clock-ids"))
		return np->has_clock_ids;
	return false;
}

static int of_property_count_strings(const struct device_node *np, const char *name)
{
	assert(!strcmp(name, "apple,firmware-uuid"));
	return np ? np->fw_uuid_count : -EINVAL;
}

static int of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	assert(!strcmp(name, "apple,firmware-uuid"));
	if (!np || np->fw_uuid_count < 1 || !np->fw_uuid)
		return -EINVAL;
	*out = np->fw_uuid;
	return 0;
}

static int of_property_count_u32_elems(const struct device_node *np, const char *name)
{
	if (!np)
		return -EINVAL;
	if (!strcmp(name, "apple,firmware-version"))
		return np->fw_ver_cells;
	if (!strcmp(name, "apple,firmware-compat"))
		return np->fw_compat_cells;
	return -EINVAL;
}

static int of_property_read_u32_array(const struct device_node *np, const char *name,
				      u32 *out, size_t n)
{
	if (!np)
		return -EINVAL;
	if (!strcmp(name, "apple,firmware-version") && n == 3 && np->fw_ver_cells == 3) {
		memcpy(out, np->fw_ver, 3 * sizeof(u32));
		return 0;
	}
	if (!strcmp(name, "apple,firmware-compat") && n == 3 && np->fw_compat_cells == 3) {
		memcpy(out, np->fw_compat, 3 * sizeof(u32));
		return 0;
	}
	return -ENOENT;
}

static int of_property_match_string(const struct device_node *np, const char *prop, const char *val)
{
	int i;

	assert(!strcmp(prop, "phy-names"));
	if (!np)
		return -EINVAL;
	for (i = 0; i < np->phy_name_count; i++) {
		if (np->phy_names[i] && !strcmp(np->phy_names[i], val))
			return i;
	}
	return -ENODATA;
}

static struct property *of_find_property(struct device_node *np, const char *name, int *lenp)
{
	assert(!strcmp(name, "iommus"));
	if (!np || !np->iommus_prop.name)
		return NULL;
	if (lenp)
		*lenp = (int)np->iommus_prop.length;
	return &np->iommus_prop;
}

static int of_count_phandle_with_args(const struct device_node *np, const char *list,
				      const char *cells)
{
	(void)cells;
	if (!np)
		return -EINVAL;
	if (!strcmp(list, "memory-region"))
		return np->mem_region_count;
	if (!strcmp(list, "iommus"))
		return np->iommu_count;
	return -EINVAL;
}

static struct device_node *of_parse_phandle(const struct device_node *np, const char *name, int idx)
{
	assert(!strcmp(name, "memory-region"));
	if (!np || idx < 0 || idx >= np->mem_region_count || !np->mem_regions[idx])
		return NULL;
	return of_node_get(np->mem_regions[idx]);
}

static int of_parse_phandle_with_args(const struct device_node *np, const char *list,
				      const char *cells, int idx, struct of_phandle_args *out)
{
	(void)cells;
	if (!np || idx < 0)
		return -EINVAL;
	if (!strcmp(list, "phys")) {
		if (idx >= np->phy_name_count || !np->phys[idx])
			return -ENOENT;
		out->np = of_node_get(np->phys[idx]);
		out->args_count = 0;
		return 0;
	}
	if (!strcmp(list, "iommus")) {
		if (idx >= np->iommu_count || !np->iommus[idx])
			return -ENOENT;
		out->np = of_node_get(np->iommus[idx]);
		out->args_count = 1;
		out->args[0] = np->iommu_args[idx];
		return 0;
	}
	if (!strcmp(list, "mboxes")) {
		if (idx >= np->mbox_count || !np->mboxes[idx])
			return -ENOENT;
		out->np = of_node_get(np->mboxes[idx]);
		out->args_count = 0;
		return 0;
	}
	return -EINVAL;
}

static struct device_node *of_get_child_by_name(const struct device_node *np, const char *name)
{
	assert(!strcmp(name, "piodma"));
	return (np && np->piodma_child) ? of_node_get(np->piodma_child) : NULL;
}

static struct device_node *of_find_node_by_path(const char *path)
{
	unsigned int i;

	for (i = 0; i < nr_nodes; i++) {
		if ((all_nodes[i]->path && !strcmp(all_nodes[i]->path, path)) ||
		    (all_nodes[i]->alias && !strcmp(all_nodes[i]->alias, path)))
			return of_node_get(all_nodes[i]);
	}
	return NULL;
}

static struct device_node *of_find_compatible_node(struct device_node *from,
						   const char *type, const char *compat)
{
	unsigned int i = 0;

	(void)type;
	if (from) {
		while (i < nr_nodes && all_nodes[i] != from)
			i++;
		if (i < nr_nodes)
			i++;
		of_node_put(from);
	}
	for (; i < nr_nodes; i++) {
		if (of_device_is_compatible(all_nodes[i], compat))
			return of_node_get(all_nodes[i]);
	}
	return NULL;
}

#define for_each_compatible_node(dn, type, compatible) \
	for (dn = of_find_compatible_node(NULL, type, compatible); dn; \
	     dn = of_find_compatible_node(dn, type, compatible))

static struct device_node *find_by_phandle(u32 phandle)
{
	unsigned int i;

	for (i = 0; i < nr_nodes; i++) {
		if (all_nodes[i]->phandle == phandle)
			return all_nodes[i];
	}
	return NULL;
}

static void of_changeset_init(struct of_changeset *cs)
{
	memset(cs, 0, sizeof(*cs));
}

static void of_changeset_destroy(struct of_changeset *cs)
{
	unsigned int i;

	for (i = 0; i < cs->nops; i++) {
		if (!cs->ops[i].is_status && cs->ops[i].prop) {
			free(cs->ops[i].prop->value);
			free(cs->ops[i].prop);
		}
	}
	cs->nops = 0;
}

static int of_changeset_update_property(struct of_changeset *cs, struct device_node *np,
					struct property *prop)
{
	assert(cs->nops < ARRAY_SIZE(cs->ops));
	cs->ops[cs->nops++] = (struct cs_op){ .np = np, .is_status = false, .prop = prop };
	return 0;
}

static int of_changeset_update_prop_string(struct of_changeset *cs, struct device_node *np,
					   const char *name, const char *val)
{
	assert(!strcmp(name, "status"));
	assert(cs->nops < ARRAY_SIZE(cs->ops));
	cs->ops[cs->nops++] = (struct cs_op){ .np = np, .is_status = true, .status = val };
	return 0;
}

static int of_changeset_apply(struct of_changeset *cs)
{
	unsigned int i;

	for (i = 0; i < cs->nops; i++) {
		struct cs_op *op = &cs->ops[i];

		if (op->is_status) {
			op->np->available = !strcmp(op->status, "okay");
		} else {
			const __be32 *vals = op->prop->value;
			int n = (int)(op->prop->length / (2 * sizeof(__be32)));
			int j;

			assert(!strcmp(op->prop->name, "iommus"));
			assert(n >= 1 && n <= 4);
			op->np->iommu_count = n;
			for (j = 0; j < n; j++) {
				op->np->iommus[j] = find_by_phandle(be32_to_cpu(vals[2 * j]));
				assert(op->np->iommus[j]);
				op->np->iommu_args[j] = be32_to_cpu(vals[2 * j + 1]);
			}
			sync_iommus_prop(op->np);
		}
	}
	cs->applied = true;
	assert(nr_applied_cs < ARRAY_SIZE(applied_cs));
	applied_cs[nr_applied_cs++] = cs;
	return 0;
}
''' + helpers + r'''
/*
 * A DART kind whose locked firmware roots apple-dart keeps
 * (apple_dart_keeps_locked_roots()). The J773g's own, apple,t8132-dart, is not
 * one in this tree: see j773g_darts().
 */
#define KEPT_DART "apple,t6041-dart"

struct fixture {
	struct device_node display, disp0_dart, main_dcp;
	struct device_node phy, fw_dart, disp_dart, mbox, piodma, dcpext0;
	struct device_node mem_text, mem_data, mem_heap;
	struct device_node dcpext1;
};

static void register_node(struct device_node *np)
{
	assert(nr_nodes < MAX_NODES);
	all_nodes[nr_nodes++] = np;
}

static void cleanup_applied(void)
{
	unsigned int i;

	for (i = 0; i < nr_applied_cs; i++) {
		of_changeset_destroy(applied_cs[i]);
		free(applied_cs[i]);
	}
	nr_applied_cs = 0;
}

static void check_refs_zero(void)
{
	unsigned int i;

	for (i = 0; i < nr_nodes; i++)
		assert(all_nodes[i]->refcount == 0);
}

static void set_v27_2(struct device_node *np)
{
	np->fw_uuid = APPLE_T8132_DCP_26B123_UUID;
	np->fw_uuid_count = 1;
	np->fw_ver[0] = 27; np->fw_ver[1] = 2; np->fw_ver[2] = 0;
	np->fw_ver_cells = 3;
	np->fw_compat[0] = 27; np->fw_compat[1] = 2; np->fw_compat[2] = 0;
	np->fw_compat_cells = 3;
}

static void init_fixture(struct fixture *f)
{
	cleanup_applied();
	memset(f, 0, sizeof(*f));
	nr_nodes = 0;
	nomodeset = false;
	err_count = info_count = 0;

	/* Deliberately use non-J773g paths to prove resolution is phandle-driven. */
	f->disp0_dart = (struct device_node){
		.path = "/soc/iommu@111100000", .phandle = 10, .available = false,
	};
	f->display = (struct device_node){
		.path = "/soc/display-subsystem", .alias = "disp0",
		.compatible = "apple,t8132-display-subsystem",
		.phandle = 11, .available = true,
		.iommus = { &f->disp0_dart }, .iommu_args = { 0 }, .iommu_count = 1,
	};
	sync_iommus_prop(&f->display);

	f->main_dcp = (struct device_node){
		.path = "/soc/dcp@111e00000", .alias = "dcp",
		.compatible = "apple,t8132-dcp", .phandle = 12, .available = false,
	};

	f->phy = (struct device_node){
		.path = "/soc/phy@999700000", .phandle = 20, .available = false,
	};
	f->fw_dart = (struct device_node){
		.path = "/soc/iommu@999340000", .compatible = KEPT_DART,
		.phandle = 21, .available = false,
	};
	f->disp_dart = (struct device_node){
		.path = "/soc/iommu@999300000", .compatible = KEPT_DART,
		.phandle = 22, .available = false,
	};
	f->mbox = (struct device_node){
		.path = "/soc/mailbox@999e08000", .phandle = 23, .available = false,
	};
	f->piodma = (struct device_node){
		.path = "/soc/dcp@999e00000/piodma", .name = "piodma",
		.phandle = 24, .available = false,
		.iommus = { &f->disp_dart }, .iommu_args = { 16 }, .iommu_count = 1,
	};
	sync_iommus_prop(&f->piodma);

	f->mem_text = (struct device_node){
		.path = "/reserved-memory/text", .compatible = "apple,asc-mem", .available = true,
	};
	f->mem_data = (struct device_node){
		.path = "/reserved-memory/data", .compatible = "apple,asc-mem", .available = true,
	};
	f->mem_heap = (struct device_node){
		.path = "/reserved-memory/heap", .compatible = "apple,asc-mem", .available = true,
	};

	f->dcpext0 = (struct device_node){
		.path = "/soc/dcp@999e00000", .compatible = "apple,t8132-dcpext",
		.phandle = 25, .available = false,
		.phy_names = { "dp-phy" }, .phy_name_count = 1, .phys = { &f->phy },
		.iommus = { &f->fw_dart }, .iommu_args = { 23 }, .iommu_count = 1,
		.mboxes = { &f->mbox }, .mbox_count = 1,
		.mem_regions = { &f->mem_text, &f->mem_data, &f->mem_heap },
		.mem_region_count = 3,
		.piodma_child = &f->piodma,
	};
	sync_iommus_prop(&f->dcpext0);
	set_v27_2(&f->dcpext0);

	/* Second dcpext without dp-phy (Type-C only): must stay untouched. */
	f->dcpext1 = (struct device_node){
		.path = "/soc/dcp@888e00000", .compatible = "apple,t8132-dcpext",
		.phandle = 30, .available = false,
		.phy_names = { "typec0" }, .phy_name_count = 1, .phys = { &f->phy },
	};
	set_v27_2(&f->dcpext1);

	register_node(&f->disp0_dart);
	register_node(&f->display);
	register_node(&f->main_dcp);
	register_node(&f->phy);
	register_node(&f->fw_dart);
	register_node(&f->disp_dart);
	register_node(&f->mbox);
	register_node(&f->piodma);
	register_node(&f->mem_text);
	register_node(&f->mem_data);
	register_node(&f->mem_heap);
	register_node(&f->dcpext0);
	register_node(&f->dcpext1);
}

/* The J773g's dcpext0 DARTs, which this tree's apple-dart does not keep the locked roots of. */
static void j773g_darts(struct fixture *f)
{
	f->fw_dart.compatible = "apple,t8132-dart";
	f->disp_dart.compatible = "apple,t8132-dart";
}

/* Stage 2 published nothing for the dcpexts: no image, placeholders, no display. */
static void no_handover(struct fixture *f)
{
	struct device_node *dcps[] = { &f->dcpext0, &f->dcpext1 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dcps); i++) {
		dcps[i]->fw_uuid = NULL;
		dcps[i]->fw_uuid_count = 0;
		dcps[i]->fw_ver_cells = 0;
		dcps[i]->fw_compat_cells = 0;
	}
	f->mem_text.available = f->mem_data.available = f->mem_heap.available = false;
	f->display.available = false;
}

struct snapshot {
	bool available[MAX_NODES];
	int display_iommus;
	struct device_node *display_iommu[4];
};

static void take_snapshot(struct fixture *f, struct snapshot *snap)
{
	unsigned int i;

	for (i = 0; i < nr_nodes; i++)
		snap->available[i] = all_nodes[i]->available;
	snap->display_iommus = f->display.iommu_count;
	memcpy(snap->display_iommu, f->display.iommus, sizeof(snap->display_iommu));
}

static void expect_unchanged(struct fixture *f, const struct snapshot *snap)
{
	unsigned int i;

	for (i = 0; i < nr_nodes; i++)
		assert(all_nodes[i]->available == snap->available[i]);
	assert(f->display.iommu_count == snap->display_iommus);
	assert(!memcmp(f->display.iommus, snap->display_iommu, sizeof(snap->display_iommu)));
	assert(nr_applied_cs == 0);
}

/*
 * What the GPU sees (asahi_g16_display_prepare()): with the display subsystem
 * disabled it starts on the boot framebuffer instead of waiting for it.
 */
static void expect_gpu_on_boot_framebuffer(struct fixture *f)
{
	assert(!of_device_is_available(&f->display));
	assert(!f->disp0_dart.available);
	assert(f->display.iommu_count == 1 && f->display.iommus[0] == &f->disp0_dart);
}

static void expect_chain(struct fixture *f, bool enabled)
{
	assert(f->phy.available == enabled);
	assert(f->fw_dart.available == enabled);
	assert(f->disp_dart.available == enabled);
	assert(f->mbox.available == enabled);
	assert(f->piodma.available == enabled);
	assert(f->dcpext0.available == enabled);
	assert(!f->dcpext1.available);
}

int main(void)
{
	struct snapshot snap;
	struct fixture f;

	/* 1. Headless T8132 (J773g-like): phandle-derived chain enabled; disp0 iommus replaced. */
	init_fixture(&f);
	gate_t8132_run();
	expect_chain(&f, true);
	assert(f.display.iommu_count == 1);
	assert(f.display.iommus[0] == &f.disp_dart && f.display.iommu_args[0] == 0);
	check_refs_zero();

	/* 2. Laptop T8132 (14" MBP-like): main DCP handed over on 27.2 -> disp0 iommus appended. */
	init_fixture(&f);
	set_v27_2(&f.main_dcp);
	f.main_dcp.has_clock_ids = true;
	gate_t8132_run();
	expect_chain(&f, true);
	assert(f.display.iommu_count == 2);
	assert(f.display.iommus[0] == &f.disp0_dart && f.display.iommu_args[0] == 0);
	assert(f.display.iommus[1] == &f.disp_dart && f.display.iommu_args[1] == 0);
	check_refs_zero();

	/* 3. Main DCP with 27.2 image but no apple,clock-ids (idle) -> replaced, not appended. */
	init_fixture(&f);
	set_v27_2(&f.main_dcp);
	f.main_dcp.has_clock_ids = false;
	gate_t8132_run();
	expect_chain(&f, true);
	assert(f.display.iommu_count == 1 && f.display.iommus[0] == &f.disp_dart);
	check_refs_zero();

	/*
	 * 4-7: a refused chain on a headless T8132 leaves the display subsystem the
	 * boot loader enabled, and its DART, disabled: the GPU starts on the boot
	 * framebuffer.
	 */
	/* 4. Unapproved firmware UUID on dcpext0 -> chain stays disabled, disp0 unchanged. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.dcpext0.fw_uuid = "00000000-0000-0000-0000-000000000000";
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/* 5. Placeholder (disabled) memory-region -> refused whole. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.mem_heap.available = false;
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/* 6. Missing phandle (e.g. no piodma child) -> refused whole. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.dcpext0.piodma_child = NULL;
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/* 7. Any chain node already enabled -> refused whole, DART not modified. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.phy.available = true;
	gate_t8132_run();
	assert(f.phy.available);
	assert(!f.fw_dart.available && !f.disp_dart.available && !f.mbox.available &&
	       !f.piodma.available && !f.dcpext0.available);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/* 8. Disabled display subsystem -> chain stays disabled. */
	init_fixture(&f);
	f.display.available = false;
	gate_t8132_run();
	expect_chain(&f, false);
	assert(f.display.iommu_count == 1 && f.display.iommus[0] == &f.disp0_dart);
	check_refs_zero();

	/*
	 * 9. No fixed dp-phy on any dcpext -> no chain, no error; the display
	 * subsystem has nothing to bind and goes.
	 */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.dcpext0.phy_names[0] = "typec1";
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	assert(err_count == 0 && info_count == 1);
	check_refs_zero();

	/* 10. nomodeset -> no chain; drm/apple does not register, so neither does the display. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	nomodeset = true;
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/*
	 * 11. No handover (Stage 2 published nothing for the dcpexts): no change
	 * from a tree without this gate. Nothing is enabled or disabled, disp0's
	 * iommus stay, and nothing is logged.
	 */
	init_fixture(&f);
	no_handover(&f);
	take_snapshot(&f, &snap);
	gate_t8132_run();
	expect_unchanged(&f, &snap);
	assert(err_count == 0 && info_count == 0);
	check_refs_zero();

	/* 11b. The same with the J773g's DARTs, and under nomodeset. */
	init_fixture(&f);
	no_handover(&f);
	j773g_darts(&f);
	take_snapshot(&f, &snap);
	gate_t8132_run();
	expect_unchanged(&f, &snap);
	nomodeset = true;
	gate_t8132_run();
	expect_unchanged(&f, &snap);
	check_refs_zero();

	/*
	 * 12. The J773g on 27.2 with this tree: Stage 2 handed dcpext0 over and
	 * enabled the display subsystem and disp0's DART, but apple-dart keeps no
	 * locked roots of a T8132 DART. The chain stays off and the display goes,
	 * so the GPU starts on the boot framebuffer as without the handover.
	 */
	init_fixture(&f);
	j773g_darts(&f);
	f.disp0_dart.available = true;
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	assert(err_count == 0);
	check_refs_zero();

	/* 13. Either DART of the chain alone not kept -> the same. */
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.fw_dart.compatible = "apple,t8132-dart";
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	init_fixture(&f);
	f.disp0_dart.available = true;
	f.disp_dart.compatible = "apple,t8132-dart";
	gate_t8132_run();
	expect_chain(&f, false);
	expect_gpu_on_boot_framebuffer(&f);
	check_refs_zero();

	/* 14. Laptop: main DCP handed over on 27.2 -> its display subsystem and DART stay. */
	init_fixture(&f);
	j773g_darts(&f);
	set_v27_2(&f.main_dcp);
	f.main_dcp.has_clock_ids = true;
	f.main_dcp.available = true;
	f.disp0_dart.available = true;
	take_snapshot(&f, &snap);
	gate_t8132_run();
	expect_unchanged(&f, &snap);
	check_refs_zero();

	/* 15. Headless, but the main DCP enabled: the display goes, disp0's DART (its piodma's) stays. */
	init_fixture(&f);
	j773g_darts(&f);
	f.main_dcp.available = true;
	f.disp0_dart.available = true;
	gate_t8132_run();
	expect_chain(&f, false);
	assert(!f.display.available && f.disp0_dart.available);
	check_refs_zero();

	cleanup_applied();
	puts("PASS T8132 fixed-DP display gate: phandle chain, replace vs append disp0 iommus, fail-closed controls");
	puts("PASS T8132 display gate: no handover or no kept DART roots boots as without the gate, GPU on the boot framebuffer");
	return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix='t8132-display-gate-') as tmp:
        src = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test'
        src.write_text(program)
        subprocess.run(
            [os.environ.get('CC', 'clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', str(src), '-o', str(exe)],
            check=True,
        )
        subprocess.run(
            [str(exe)],
            check=True,
            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'},
        )


if __name__ == '__main__':
    main()
