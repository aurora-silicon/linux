// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6030 internal display enable gate
 *
 * The T6030 device tree describes the internal display (the DCP, its
 * mailbox, the two display DARTs and the display subsystem) with every node
 * disabled. Kernels that cannot take the display over from iBoot therefore
 * keep scanning out through the simple framebuffer, and all of them can
 * share one device tree.
 *
 * This gate enables those five nodes before platform devices are created.
 * It acts only if the boot loader has set apple,t6030-handoff = <1> on the
 * DCP and on the display subsystem, which it does after it has checked and
 * locked the inherited DART mappings. apple_t6030_display.enable=0 on the
 * kernel command line keeps the display on the boot framebuffer instead;
 * apple_t6030_display.dcpext=0 and apple_t6030_display.scanout=0 do the same
 * for the external display processor and its scanout. The
 * targets are found through the DCP's and the display subsystem's
 * phandles, and each must still be disabled. Otherwise nothing is changed.
 *
 * The DCP firmware needs the power management processor (PMP) running,
 * which iBoot leaves loaded but halted. On the J516S, once the display nodes
 * are enabled, the gate also adds the PMP from a built-in overlay: its node,
 * DART and mailbox (disabled), and a PMP report node whose driver sets the
 * display and storage requests and starts the PMP when the display driver
 * asks for it. Before any driver probes, the gate then
 *  - raises the minimum power state of the display domains (display
 *    subsystem, front end and DCP CPU) to "active", so that the PMP can
 *    never power the running DCP down;
 *  - keeps the PMP and PMS_SRAM domains on, as iBoot left them: the PTD the
 *    DCP firmware and the PMP share lives in that SRAM;
 *  - gives the PMP DART and mailbox the interrupt parent of the DCP mailbox.
 * Everything the PMP needs is checked before the display nodes are enabled.
 * On a machine other than the J516S, or if a check fails or the PMP cannot be
 * added, the display nodes stay (or are put back) disabled and the display
 * stays on the boot framebuffer.
 */

#define pr_fmt(fmt) "apple-t6030-display: " fmt

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

enum {
	GATE_DCP_DART,
	GATE_DISP0_DART,
	GATE_DCP_MBOX,
	GATE_DCP,
	GATE_DISPLAY,
	GATE_NR_NODES,
};

enum {
	PMP_PS_DISP_SYS,
	PMP_PS_DISP_FE,
	PMP_PS_DISP_CPU,
	PMP_PS_PMP,
	PMP_PS_PMS_SRAM,
	PMP_PS_NR,
};

static const char *const pmp_ps_labels[PMP_PS_NR] __initconst = {
	"disp_sys", "disp_fe", "disp_cpu", "pmp", "pms_sram",
};

#define PMGR_PS_ACTIVE	15

/* Built-in overlay with the J516S PMP, its DART and mailbox, and its report. */
extern const u8 __dtbo_t6030_j516s_pmp_begin[];
extern const u8 __dtbo_t6030_j516s_pmp_end[];

static bool gate_requested __initdata = true;

/* Kept after a successful apply: the live tree now holds its properties. */
static struct of_changeset gate_cs;
static struct of_changeset gate_pmp_cs;

static int __init gate_setup(char *arg)
{
	return kstrtobool(arg, &gate_requested);
}
early_param("apple_t6030_display.enable", gate_setup);

/* Returns the only node compatible with @compat, or NULL if there are none or several. */
static struct device_node *__init gate_find_one(const char *compat)
{
	struct device_node *np, *found = NULL;

	for_each_compatible_node(np, NULL, compat) {
		if (found) {
			of_node_put(np);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(np);
	}

	return found;
}

static bool __init gate_marked(const struct device_node *np)
{
	u32 val;

	return !of_property_read_u32(np, "apple,t6030-handoff", &val) && val == 1;
}

static bool __init gate_disabled(const struct device_node *np)
{
	const char *status;

	return !of_property_read_string(np, "status", &status) &&
	       !strcmp(status, "disabled");
}

/*
 * Returns the node named by the single entry of @list in @np, if that entry
 * has @nargs cells (the first equal to @arg0) and the node is compatible
 * with @compat. Returns NULL otherwise.
 */
static struct device_node *__init gate_target(const struct device_node *np,
					      const char *list,
					      const char *cells_name,
					      int nargs, u32 arg0,
					      const char *compat)
{
	struct of_phandle_args args;

	if (of_count_phandle_with_args(np, list, cells_name) != 1)
		return NULL;
	if (of_parse_phandle_with_args(np, list, cells_name, 0, &args))
		return NULL;

	if (args.args_count != nargs || (nargs && args.args[0] != arg0) ||
	    !of_device_is_compatible(args.np, compat)) {
		of_node_put(args.np);
		return NULL;
	}

	return args.np;
}

static int __init gate_resolve(struct device_node **np)
{
	int i;

	np[GATE_DCP] = gate_find_one("apple,t6030-dcp");
	if (!np[GATE_DCP]) {
		pr_warn("not enabling: need exactly one apple,t6030-dcp node\n");
		return -ENODEV;
	}

	np[GATE_DISPLAY] = gate_find_one("apple,t6030-display-subsystem");
	if (!np[GATE_DISPLAY]) {
		pr_warn("not enabling: need exactly one apple,t6030-display-subsystem node\n");
		return -ENODEV;
	}

	if (!gate_marked(np[GATE_DCP]) || !gate_marked(np[GATE_DISPLAY])) {
		pr_warn("not enabling: the boot loader did not set apple,t6030-handoff\n");
		return -EPERM;
	}

	np[GATE_DCP_DART] = gate_target(np[GATE_DCP], "iommus", "#iommu-cells",
					1, 5, "apple,t8110-dart");
	if (!np[GATE_DCP_DART]) {
		pr_warn("not enabling: %pOF iommus is not one apple,t8110-dart stream 5\n",
			np[GATE_DCP]);
		return -EINVAL;
	}

	np[GATE_DISP0_DART] = gate_target(np[GATE_DISPLAY], "iommus",
					  "#iommu-cells", 1, 0,
					  "apple,t8110-dart");
	if (!np[GATE_DISP0_DART]) {
		pr_warn("not enabling: %pOF iommus is not one apple,t8110-dart stream 0\n",
			np[GATE_DISPLAY]);
		return -EINVAL;
	}

	np[GATE_DCP_MBOX] = gate_target(np[GATE_DCP], "mboxes", "#mbox-cells",
					0, 0, "apple,asc-mailbox-v4");
	if (!np[GATE_DCP_MBOX]) {
		pr_warn("not enabling: %pOF mboxes is not one apple,asc-mailbox-v4\n",
			np[GATE_DCP]);
		return -EINVAL;
	}

	if (np[GATE_DCP_DART] == np[GATE_DISP0_DART]) {
		pr_warn("not enabling: the DCP and the display subsystem share %pOF\n",
			np[GATE_DCP_DART]);
		return -EINVAL;
	}

	for (i = 0; i < GATE_NR_NODES; i++) {
		if (!gate_disabled(np[i])) {
			pr_warn("not enabling: %pOF is not disabled\n", np[i]);
			return -EBUSY;
		}
	}

	return 0;
}

static int __init gate_apply(struct device_node **np)
{
	int i, ret = 0;

	of_changeset_init(&gate_cs);

	for (i = 0; i < GATE_NR_NODES; i++) {
		ret = of_changeset_update_prop_string(&gate_cs, np[i], "status",
						      "okay");
		if (ret)
			break;
	}
	if (!ret)
		ret = of_changeset_apply(&gate_cs);

	if (ret) {
		of_changeset_destroy(&gate_cs);
		pr_err("not enabling: changeset failed: %d\n", ret);
		return ret;
	}

	pr_info("enabled %pOF, %pOF, %pOF, %pOF and %pOF\n",
		np[GATE_DCP_DART], np[GATE_DISP0_DART], np[GATE_DCP_MBOX],
		np[GATE_DCP], np[GATE_DISPLAY]);

	return 0;
}

/* Disables the display nodes again when the PMP could not be added. */
static void __init gate_revert(void)
{
	int ret = of_changeset_revert(&gate_cs);

	if (ret) {
		pr_err("could not disable the display nodes again: %d\n", ret);
		return;
	}
	of_changeset_destroy(&gate_cs);
	pr_info("display nodes disabled again, display stays on the boot framebuffer\n");
}

/* The single power domain of @np, if it is a T6030 power state. */
static struct device_node *__init gate_ps_parent(struct device_node *np)
{
	return gate_target(np, "power-domains", "#power-domain-cells", 0, 0,
			   "apple,t6030-pmgr-pwrstate");
}

static bool __init gate_ps_is(const struct device_node *np, const char *label)
{
	const char *name;

	return np && of_device_is_compatible(np, "apple,t6030-pmgr-pwrstate") &&
	       !of_property_read_string(np, "label", &name) && !strcmp(name, label);
}

/* The only power state below @pmgr labelled @label. */
static struct device_node *__init gate_ps_child(struct device_node *pmgr, const char *label)
{
	struct device_node *child, *found = NULL;

	for_each_child_of_node(pmgr, child) {
		if (!gate_ps_is(child, label))
			continue;
		if (found) {
			of_node_put(child);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(child);
	}

	return found;
}

/*
 * The display domains are the DCP's power domain and its parents; the PMP
 * and PMS_SRAM domains are found by label beside them.
 */
static int __init gate_pmp_resolve(struct device_node **np, struct device_node **ps,
				   struct device_node **aic)
{
	struct device_node *pmgr, *other;
	int i;

	if (!of_machine_is_compatible("apple,j516s")) {
		pr_warn("PMP not added: its description is for the J516S only\n");
		return -ENODEV;
	}

	other = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
	if (!other)
		other = of_find_compatible_node(NULL, NULL, "apple,t6030-pmp-v2-report");
	if (other) {
		pr_warn("PMP not added: %pOF already exists\n", other);
		of_node_put(other);
		return -EEXIST;
	}

	/* The overlay's DCP fragment names this path. */
	if (strcmp(of_node_full_name(np[GATE_DCP]), "dcp@28ec00000") ||
	    !of_node_name_eq(np[GATE_DCP]->parent, "soc") ||
	    !of_node_is_root(np[GATE_DCP]->parent->parent)) {
		pr_warn("PMP not added: the DCP is %pOF, not /soc/dcp@28ec00000\n",
			np[GATE_DCP]);
		return -EINVAL;
	}

	ps[PMP_PS_DISP_CPU] = gate_ps_parent(np[GATE_DCP]);
	if (ps[PMP_PS_DISP_CPU])
		ps[PMP_PS_DISP_FE] = gate_ps_parent(ps[PMP_PS_DISP_CPU]);
	if (ps[PMP_PS_DISP_FE])
		ps[PMP_PS_DISP_SYS] = gate_ps_parent(ps[PMP_PS_DISP_FE]);
	pmgr = of_get_parent(ps[PMP_PS_DISP_CPU]);
	if (pmgr) {
		ps[PMP_PS_PMP] = gate_ps_child(pmgr, "pmp");
		ps[PMP_PS_PMS_SRAM] = gate_ps_child(pmgr, "pms_sram");
		of_node_put(pmgr);
	}
	for (i = 0; i < PMP_PS_NR; i++) {
		if (!gate_ps_is(ps[i], pmp_ps_labels[i])) {
			pr_warn("PMP not added: no %s power state where expected\n",
				pmp_ps_labels[i]);
			return -ENODEV;
		}
	}
	for (i = PMP_PS_DISP_SYS; i <= PMP_PS_DISP_CPU; i++) {
		if (!of_property_read_bool(ps[i], "apple,inherited-on")) {
			pr_warn("PMP not added: %pOF is not apple,inherited-on\n", ps[i]);
			return -EINVAL;
		}
	}

	*aic = of_parse_phandle(np[GATE_DCP_MBOX], "interrupt-parent", 0);
	if (!*aic || !of_property_read_bool(*aic, "interrupt-controller")) {
		pr_warn("PMP not added: %pOF has no interrupt parent\n", np[GATE_DCP_MBOX]);
		return -EINVAL;
	}

	return 0;
}

/* Sets the u32 property @name of @np, adding it if it is missing. */
static int __init gate_set_u32(struct of_changeset *cs, struct device_node *np,
			       const char *name, u32 val)
{
	struct property *prop;
	__be32 *value;

	/* Kept for good: the live tree refers to it once applied. */
	prop = kzalloc_obj(*prop);
	value = kmalloc_obj(*value);
	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		return -ENOMEM;
	}
	*value = cpu_to_be32(val);
	prop->name = (char *)name;
	prop->length = sizeof(*value);
	prop->value = value;

	return of_changeset_update_property(cs, np, prop);
}

static int __init gate_pmp_apply(struct device_node *dcp, struct device_node **ps,
				 struct device_node *aic)
{
	const size_t size = __dtbo_t6030_j516s_pmp_end - __dtbo_t6030_j516s_pmp_begin;
	struct device_node *report, *disp = NULL, *pmp = NULL, *dart = NULL, *mbox = NULL;
	int ovcs_id = 0, ret, i;

	ret = of_overlay_fdt_apply(__dtbo_t6030_j516s_pmp_begin, size, &ovcs_id, NULL);
	if (ret) {
		pr_err("PMP not added: overlay failed: %d\n", ret);
		return ret;
	}

	report = of_find_compatible_node(NULL, NULL, "apple,t6030-pmp-v2-report");
	if (report) {
		struct device_node *child;

		pmp = of_parse_phandle(report, "apple,pmp", 0);
		/* The display request entry: DISP is PMP device 7. */
		for_each_child_of_node(report, child) {
			if (!strcmp(of_node_full_name(child), "report@7")) {
				disp = child;
				break;
			}
		}
	}
	if (pmp) {
		dart = of_parse_phandle(pmp, "iommus", 0);
		mbox = of_parse_phandle(pmp, "mboxes", 0);
	}

	of_changeset_init(&gate_pmp_cs);
	ret = dart && mbox && disp && disp->phandle ? 0 : -ENODEV;
	/* The DCP driver starts once the PMP acknowledges this request. */
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, dcp, "apple,pmp-report",
						disp->phandle);
	/* The DART and mailbox interrupt parent lives in the base tree. */
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, dart, "interrupt-parent",
						aic->phandle);
	if (!ret)
		ret = of_changeset_add_prop_u32(&gate_pmp_cs, mbox, "interrupt-parent",
						aic->phandle);
	/* The display domains' floor, applied by the PMGR driver when it probes. */
	for (i = PMP_PS_DISP_SYS; i <= PMP_PS_DISP_CPU && !ret; i++)
		ret = gate_set_u32(&gate_pmp_cs, ps[i], "apple,min-state", PMGR_PS_ACTIVE);
	/* Linux would otherwise power these off as unused. */
	for (i = PMP_PS_PMP; i <= PMP_PS_PMS_SRAM && !ret; i++) {
		if (!of_property_read_bool(ps[i], "apple,always-on"))
			ret = of_changeset_add_prop_bool(&gate_pmp_cs, ps[i], "apple,always-on");
	}
	if (!ret)
		ret = of_changeset_apply(&gate_pmp_cs);

	of_node_put(mbox);
	of_node_put(dart);
	of_node_put(pmp);
	of_node_put(disp);
	of_node_put(report);

	if (ret) {
		of_changeset_destroy(&gate_pmp_cs);
		of_overlay_remove(&ovcs_id);
		pr_err("PMP not added: changeset failed: %d\n", ret);
		return ret;
	}

	pr_info("added the PMP and its display and storage report; minimum power state of %s, %s and %s raised to active; %s and %s kept on\n",
		pmp_ps_labels[PMP_PS_DISP_SYS], pmp_ps_labels[PMP_PS_DISP_FE],
		pmp_ps_labels[PMP_PS_DISP_CPU], pmp_ps_labels[PMP_PS_PMP],
		pmp_ps_labels[PMP_PS_PMS_SRAM]);
	return 0;
}

/* An external handoff describes memory only; firmware startup is a later,
 * explicit driver operation. Older kernels leave all these nodes disabled. */
static bool gate_dcpext_requested __initdata = true;
static struct of_changeset gate_dcpext_cs;

static int __init gate_dcpext_setup(char *arg)
{
	return kstrtobool(arg, &gate_dcpext_requested);
}
early_param("apple_t6030_display.dcpext", gate_dcpext_setup);

static bool gate_scanout_requested __initdata = true;
static struct of_changeset gate_scanout_cs;

static int __init gate_scanout_setup(char *arg)
{
	return kstrtobool(arg, &gate_scanout_requested);
}
early_param("apple_t6030_display.scanout", gate_scanout_setup);

/*
 * A no-map DT reservation is recorded by the reserved-memory parser, but
 * need not appear in memblock.reserved: no-map marks memblock.memory, and
 * the bootloader's carveouts can be outside the usable-memory list entirely.
 * memblock_is_region_reserved() only checks intersection, not full coverage.
 */
static bool __init gate_scanout_table_reserved(struct device_node *region,
					     const struct resource *table)
{
	struct reserved_mem *rmem;
	struct memblock_region *m;
	u64 size;

	if (!region || !region->parent ||
	    !of_node_name_eq(region->parent, "reserved-memory") ||
	    !of_node_is_root(region->parent->parent) ||
	    !of_device_is_available(region) ||
	    !of_property_read_bool(region, "no-map") ||
	    of_property_read_bool(region, "reusable") || table->end < table->start)
		return false;
	size = resource_size(table);
	if (size < SZ_16K || size > SZ_1M || !IS_ALIGNED(table->start, SZ_16K) ||
	    !IS_ALIGNED(size, SZ_16K))
		return false;
	rmem = of_reserved_mem_lookup(region);
	if (!rmem || rmem->base != table->start || rmem->size != size)
		return false;
	/* Reject even partially allocatable or linearly mapped RAM. */
	for_each_mem_region(m) {
		if (m->size && m->base <= table->end &&
		    (table->start < m->base || table->start - m->base < m->size) &&
		    !memblock_is_nomap(m))
			return false;
	}
	return true;
}

/* Read-only verification before Linux may attach the external display DART. */
static void __init gate_dispext_scanout(struct device_node *dcp)
{
	struct device_node *scanout = NULL, *dart = NULL, *region = NULL;
	struct resource regs, table, first_table = {};
	struct of_phandle_args spec;
	void __iomem *mmio = NULL;
	void *root;
	u32 state[6], marker;
	u64 phys;
	int i, ret = -EINVAL;

	if (!gate_scanout_requested)
		return;
	scanout = of_get_child_by_name(dcp, "scanout");
	if (!scanout || !gate_disabled(scanout) ||
	    !of_device_is_compatible(scanout, "apple,t6030-dispext-scanout") ||
	    of_property_read_u32(scanout, "apple,t6030-dispext-handoff", &marker) || marker != 1)
		goto out;
	if (of_count_phandle_with_args(scanout, "iommus", "#iommu-cells") != 1 ||
	    of_parse_phandle_with_args(scanout, "iommus", "#iommu-cells", 0, &spec))
		goto out;
	dart = spec.np;
	if (spec.args_count != 1 || spec.args[0] != 0 || !gate_disabled(dart) ||
	    !of_device_is_compatible(dart, "apple,t8110-dart") ||
	    of_property_read_u32(dart, "apple,t6030-dispext-handoff", &marker) || marker != 1 ||
	    of_property_count_u32_elems(dart, "apple,inherited-dart-state") != 6 ||
	    of_property_read_u32_array(dart, "apple,inherited-dart-state", state, 6) ||
	    of_count_phandle_with_args(dart, "memory-region", NULL) != 2 ||
	    of_property_match_string(dart, "memory-region-names", "sid0-page-tables") != 0 ||
	    of_property_match_string(dart, "memory-region-names", "sid4-page-tables") != 1 ||
	    state[0] != 0 || state[3] != 4 ||
	    of_address_to_resource(dart, 0, &regs) ||
	    regs.start != 0x2d1304000ULL || resource_size(&regs) != SZ_16K)
		goto out;
	mmio = ioremap(regs.start, resource_size(&regs));
	if (!mmio || !(readl(mmio + 0x200) & BIT(0)))
		goto out;
	for (i = 0; i < 2; i++) {
		u32 sid = state[3 * i], tcr = state[3 * i + 1], ttbr = state[3 * i + 2];

		/* A valid SID0 root must not turn a later SID4 refusal into success. */
		ret = -EINVAL;
		if ((tcr & (BIT(0) | BIT(1) | BIT(3))) != BIT(0) || !(ttbr & BIT(0)) ||
		    (ttbr & ~(GENMASK(29, 2) | BIT(0))) ||
		    readl(mmio + 0x1000 + 4 * sid) != tcr ||
		    readl(mmio + 0x1400 + 4 * sid) != ttbr)
			goto out;
		/* T8110: address bits29:2, 16KiB page shift (42-bit output PA). */
		phys = (u64)(ttbr & GENMASK(29, 2)) << 12;
		region = of_parse_phandle(dart, "memory-region", i);
		if (!region ||
		    of_address_to_resource(region, 0, &table) ||
		    !gate_scanout_table_reserved(region, &table) ||
		    phys < table.start || phys > table.end - SZ_16K + 1 ||
		    (i && table.start <= first_table.end && first_table.start <= table.end))
			goto out;
		if (!i)
			first_table = table;
		/* arch_initcall runs after vmalloc initialization. On arm64,
		 * MEMREMAP_WB uses ioremap_cache for no-map/outside-usable RAM;
		 * the validation above excludes an allocatable/direct-map alias.
		 */
		root = memremap(phys, SZ_16K, MEMREMAP_WB);
		if (!root)
			goto out;
		ret = memchr_inv(root, 0, SZ_16K) ? -EBUSY : 0;
		memunmap(root);
		if (ret)
			goto out;
		of_node_put(region);
		region = NULL;
	}
	of_changeset_init(&gate_scanout_cs);
	ret = gate_set_u32(&gate_scanout_cs, scanout, "apple,t6030-scanout-verified", 1);
	if (!ret)
		ret = of_changeset_update_prop_string(&gate_scanout_cs, dart, "status", "okay");
	if (!ret)
		ret = of_changeset_update_prop_string(&gate_scanout_cs, scanout, "status", "okay");
	if (!ret)
		ret = of_changeset_apply(&gate_scanout_cs);
	if (ret)
		of_changeset_destroy(&gate_scanout_cs);
	else
		pr_info("external scanout DART verified locked with empty SID0/SID4 roots; enabled SID0 consumer\n");
out:
	if (ret)
		pr_warn("external scanout remains disabled: handoff/root validation failed (%d)\n", ret);
	if (mmio)
		iounmap(mmio);
	of_node_put(region);
	of_node_put(dart);
	of_node_put(scanout);
}

static void __init gate_dcpext(void)
{
	struct device_node *np[3] = {};
	struct device_node *cpu = NULL, *fe = NULL, *sys = NULL;
	struct device_node *report = NULL, *entry = NULL, *domain = NULL, *child;
	bool applied = false;
	u32 ready, id;
	int i, ret;

	if (!gate_dcpext_requested)
		return;
	np[0] = gate_find_one("apple,t6030-dcpext");
	if (!np[0] || of_property_read_u32(np[0], "apple,t6030-dcpext-memory-ready", &ready) ||
	    ready != 1 || !of_property_present(np[0], "memory-region")) {
		/* The boot loader did not hand the external processor over. */
		pr_info("dcpext not handed off by the boot loader, left disabled\n");
		goto put;
	}
	np[1] = gate_target(np[0], "iommus", "#iommu-cells", 1, 5, "apple,t8110-dart");
	np[2] = gate_target(np[0], "mboxes", "#mbox-cells", 0, 0, "apple,asc-mailbox-v4");
	for (i = 0; i < 3; i++)
		if (!np[i] || !gate_disabled(np[i]))
			goto out;

	/* Do not enable a CPU whose power ownership is not fully described. */
	cpu = gate_ps_parent(np[0]);
	if (!gate_ps_is(cpu, "dispext0_cpu"))
		goto out;
	fe = gate_ps_parent(cpu);
	if (!gate_ps_is(fe, "dispext0_fe"))
		goto out;
	sys = gate_ps_parent(fe);
	if (!gate_ps_is(sys, "dispext0_sys"))
		goto out;
	for (i = 1; i < 3; i++) {
		domain = gate_ps_parent(np[i]);
		if (domain != cpu)
			goto out;
		of_node_put(domain);
		domain = NULL;
	}
	report = gate_find_one("apple,t6030-pmp-v2-report");
	if (!report || !of_device_is_available(report))
		goto out;
	for_each_child_of_node(report, child) {
		if (!strcmp(of_node_full_name(child), "report@8")) {
			entry = child;
			break;
		}
	}
	if (!entry || !gate_disabled(entry) || !entry->phandle ||
	    !of_device_is_compatible(entry, "apple,t6000-pmp-v2-report-entry") ||
	    of_property_read_u32(entry, "reg", &id) || id != 8 ||
	    !of_property_read_bool(entry, "apple,always-on") ||
	    of_property_read_bool(entry, "apple,no-ack"))
		goto out;

	of_changeset_init(&gate_dcpext_cs);
	/* PMGR applies the floor at probe, before the PMP can manage this CPU. */
	ret = gate_set_u32(&gate_dcpext_cs, cpu, "apple,min-state", PMGR_PS_ACTIVE);
	/* The driver must wait for this report's acknowledgement before RUN. */
	if (!ret)
		ret = gate_set_u32(&gate_dcpext_cs, np[0], "apple,pmp-report", entry->phandle);
	if (!ret)
		ret = of_changeset_update_prop_string(&gate_dcpext_cs, entry, "status", "okay");
	for (i = 0; i < 3 && !ret; i++)
		ret = of_changeset_update_prop_string(&gate_dcpext_cs, np[i], "status", "okay");
	if (!ret)
		ret = of_changeset_apply(&gate_dcpext_cs);
	if (ret) {
		of_changeset_destroy(&gate_dcpext_cs);
		pr_warn("external memory gate failed: %d\n", ret);
	} else {
		applied = true;
		pr_info("enabled dcpext memory devices and PMP DISPEXT0 request with CPU power floor; external CPU startup remains manual\n");
		gate_dispext_scanout(np[0]);
	}
out:
	if (!applied)
		pr_warn("dcpext remains disabled: power and memory prerequisites were not applied\n");
put:
	of_node_put(domain);
	of_node_put(entry);
	of_node_put(report);
	of_node_put(sys);
	of_node_put(fe);
	of_node_put(cpu);
	for (i = 0; i < 3; i++)
		of_node_put(np[i]);
}

/* Runs before of_platform_default_populate_init() at arch_initcall_sync. */
static int __init apple_t6030_display_gate(void)
{
	struct device_node *np[GATE_NR_NODES] = {};
	struct device_node *ps[PMP_PS_NR] = {};
	struct device_node *aic = NULL;
	int i;

	if (!of_machine_is_compatible("apple,t6030"))
		return 0;

	if (!gate_requested) {
		pr_info("disabled on the command line, display stays on the boot framebuffer\n");
		return 0;
	}

	if (!gate_resolve(np) && !gate_pmp_resolve(np, ps, &aic) && !gate_apply(np)) {
		if (gate_pmp_apply(np[GATE_DCP], ps, aic))
			gate_revert();
		else
			gate_dcpext();
	}
	of_node_put(aic);
	for (i = 0; i < PMP_PS_NR; i++)
		of_node_put(ps[i]);
	for (i = 0; i < GATE_NR_NODES; i++)
		of_node_put(np[i]);

	return 0;
}
arch_initcall(apple_t6030_display_gate);
