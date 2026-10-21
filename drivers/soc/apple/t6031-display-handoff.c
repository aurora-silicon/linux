// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6031 (M3 Max: J514C, J516C) internal display handoff.
 *
 * The boot loader leaves the internal DCP running with the panel lit. On
 * these boards it describes that state in the device tree without enabling
 * anything: the DCP, its mailbox, the two display DARTs and the display
 * subsystem stay "disabled", and the DCP, the display subsystem and the
 * PIODMA child carry apple,t6031-handoff = <1> once the inherited DART
 * mappings have been checked and locked. The firmware's memory sits in
 * no-map reservations named by memory-region. This Mac's PMP values (board
 * and DRAM ids, the PMP tunables) are in /chosen/asahi,t6031-pmp.
 *
 * Before devices are populated, and only with apple_t6031_display.enable=1
 * on the command line, this file completes that description for the common
 * Apple display driver:
 *  - it adds the PMP, its DART and its mailbox under /soc with this Mac's
 *    values, as the T6030 handoff does from the ADT, and keeps the PMP and
 *    PMS_SRAM power domains on; pmp-core.rs then starts the PMP and
 *    pmp-display.c holds the display chain and seeds the DISP request;
 *  - it enables the five display nodes.
 * Everything is one changeset: if any check fails nothing changes and the
 * Mac keeps the boot framebuffer. apple_t6031_display.pmp=0 enables the
 * display nodes without adding the PMP; the display driver then adopts the
 * DCP without a PMP acknowledgement (an experiment, see dcp.c).
 *
 * Without the option the handoff is only described in the log.
 */
#define pr_fmt(fmt) "apple-t6031-display: " fmt

#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-display.h>
#include <linux/string.h>

/* The T6031 PMP image, as the J516C ADT names it (iop-pmp-nub). */
#define T6031_PMP_UUID	"07FF4C5B-CB2E-3DE6-95FF-3B107D19B7AB"

#define T6031_HANDOFF	"apple,t6031-handoff"
#define T6031_PMP_PATH	"/soc/pmp@290700000"

static bool t6031_display_enable __initdata;
static bool t6031_display_pmp __initdata = true;

static int __init t6031_display_enable_param(char *arg)
{
	return kstrtobool(arg, &t6031_display_enable);
}
early_param("apple_t6031_display.enable", t6031_display_enable_param);

static int __init t6031_display_pmp_param(char *arg)
{
	return kstrtobool(arg, &t6031_display_pmp);
}
early_param("apple_t6031_display.pmp", t6031_display_pmp_param);

/* The only node compatible with @compat, or NULL when there is none or several. */
static struct device_node *__init t6031_unique(const char *compat)
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

/* The only T6031 power state labelled @label, in any PMGR block. */
static struct device_node *__init t6031_pwrstate(const char *label)
{
	struct device_node *np, *found = NULL;
	const char *name;

	for_each_compatible_node(np, NULL, "apple,t6031-pmgr-pwrstate") {
		if (of_property_read_string(np, "label", &name) || strcmp(name, label))
			continue;
		if (found) {
			of_node_put(np);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(np);
	}
	return found;
}

static bool __init t6031_marked(const struct device_node *np)
{
	u32 val;

	return np && !of_property_read_u32(np, T6031_HANDOFF, &val) && val == 1;
}

static bool __init t6031_disabled(const struct device_node *np)
{
	const char *status;

	return np && !of_property_read_string(np, "status", &status) &&
	       !strcmp(status, "disabled");
}

/*
 * The node a one-entry phandle list of @np names, if its argument cells are
 * @nargs with the first equal to @arg0 and it is compatible with @compat.
 */
static struct device_node *__init t6031_linked(const struct device_node *np,
					       const char *list, const char *cells,
					       int nargs, u32 arg0, const char *compat)
{
	struct of_phandle_args args;

	if (of_count_phandle_with_args(np, list, cells) != 1 ||
	    of_parse_phandle_with_args(np, list, cells, 0, &args))
		return NULL;
	if (args.args_count != nargs || (nargs && args.args[0] != arg0) ||
	    !of_device_is_compatible(args.np, compat)) {
		of_node_put(args.np);
		return NULL;
	}
	return args.np;
}

static phandle __init t6031_free_phandle(void)
{
	struct device_node *np;
	phandle max = 0;

	for_each_of_allnodes(np)
		if (np->phandle > max)
			max = np->phandle;
	return max + 1;
}

/* A property copied byte for byte; the live tree keeps it. */
static int __init t6031_copy_prop(struct of_changeset *cs, struct device_node *np,
				  const struct property *src)
{
	struct property *prop = kzalloc_obj(*prop);

	if (!prop)
		return -ENOMEM;
	prop->name = kstrdup(src->name, GFP_KERNEL);
	prop->value = kmemdup(src->value, src->length ?: 1, GFP_KERNEL);
	prop->length = src->length;
	if (!prop->name || !prop->value)
		return -ENOMEM;
	return of_changeset_add_property(cs, np, prop);
}

/* The boot loader's values the PMP node needs: ids, capacity and tunables. */
static bool __init t6031_pmp_value(const struct property *prop)
{
	if (!strcmp(prop->name, "apple,board-id") ||
	    !strcmp(prop->name, "apple,dram-vendor-id") ||
	    !strcmp(prop->name, "apple,dram-capacity"))
		return prop->length == sizeof(u32);
	return strstarts(prop->name, "apple,tunable-");
}

/*
 * The PMP, its DART and its mailbox, with the J516C ADT's addresses and
 * interrupts: SRAM 0x2_9070_0000 (1 MiB), ASC 0x2_90c0_0000, mailbox at
 * ASC + 0x8000 (IRQs 1180-1183), DART 0x2_9030_0000 (IRQ 1184). Returns the
 * number of values copied from the boot loader, or an error.
 */
static int __init t6031_add_pmp(struct of_changeset *cs, struct device_node *soc,
				struct device_node *values, u32 irq_parent,
				struct device_node **nodes, phandle first)
{
	static const char *const dart_compat[] = { "apple,t6031-dart", "apple,t8110-dart" };
	static const char *const mbox_compat[] = { "apple,t6031-asc-mailbox",
						   "apple,asc-mailbox-v4" };
	static const char *const mbox_irq_names[] = { "send-empty", "send-not-empty",
						      "recv-empty", "recv-not-empty" };
	static const char *const pmp_compat[] = { "apple,t6031-pmp-v2", "apple,t6000-pmp-v2" };
	static const char *const pmp_reg_names[] = { "pmp", "asc" };
	const u32 dart_reg[] = { 0x2, 0x90300000, 0x0, 0x4000 };
	const u32 dart_irq[] = { 0, 0, 1184, 4 };
	const u32 mbox_reg[] = { 0x2, 0x90c08000, 0x0, 0x4000 };
	const u32 mbox_irq[] = { 0, 0, 1180, 4, 0, 0, 1181, 4, 0, 0, 1182, 4, 0, 0, 1183, 4 };
	const u32 pmp_reg[] = { 0x2, 0x90700000, 0x0, 0x100000, 0x2, 0x90c00000, 0x0, 0x4000 };
	const u32 iommus[] = { first, 0 };
	struct device_node *dart, *mbox, *pmp;
	struct property *src;
	int ret, copied = 0;

	dart = of_changeset_create_node(cs, soc, "iommu@290300000");
	mbox = of_changeset_create_node(cs, soc, "mailbox@290c08000");
	pmp = of_changeset_create_node(cs, soc, "pmp@290700000");
	if (!dart || !mbox || !pmp)
		return -ENOMEM;
	nodes[0] = dart;
	nodes[1] = mbox;
	nodes[2] = pmp;

	ret = of_changeset_add_prop_string_array(cs, dart, "compatible", dart_compat,
						 ARRAY_SIZE(dart_compat)) ?:
	      of_changeset_add_prop_u32_array(cs, dart, "reg", dart_reg, ARRAY_SIZE(dart_reg)) ?:
	      of_changeset_add_prop_u32(cs, dart, "#iommu-cells", 1) ?:
	      of_changeset_add_prop_u32(cs, dart, "interrupt-parent", irq_parent) ?:
	      of_changeset_add_prop_u32_array(cs, dart, "interrupts", dart_irq,
					      ARRAY_SIZE(dart_irq)) ?:
	      of_changeset_add_prop_u32(cs, dart, "phandle", first);
	if (ret)
		return ret;
	ret = of_changeset_add_prop_string_array(cs, mbox, "compatible", mbox_compat,
						 ARRAY_SIZE(mbox_compat)) ?:
	      of_changeset_add_prop_u32_array(cs, mbox, "reg", mbox_reg, ARRAY_SIZE(mbox_reg)) ?:
	      of_changeset_add_prop_u32(cs, mbox, "interrupt-parent", irq_parent) ?:
	      of_changeset_add_prop_u32_array(cs, mbox, "interrupts", mbox_irq,
					      ARRAY_SIZE(mbox_irq)) ?:
	      of_changeset_add_prop_string_array(cs, mbox, "interrupt-names", mbox_irq_names,
						 ARRAY_SIZE(mbox_irq_names)) ?:
	      of_changeset_add_prop_u32(cs, mbox, "#mbox-cells", 0) ?:
	      of_changeset_add_prop_u32(cs, mbox, "phandle", first + 1);
	if (ret)
		return ret;
	ret = of_changeset_add_prop_string_array(cs, pmp, "compatible", pmp_compat,
						 ARRAY_SIZE(pmp_compat)) ?:
	      of_changeset_add_prop_u32_array(cs, pmp, "reg", pmp_reg, ARRAY_SIZE(pmp_reg)) ?:
	      of_changeset_add_prop_string_array(cs, pmp, "reg-names", pmp_reg_names,
						 ARRAY_SIZE(pmp_reg_names)) ?:
	      of_changeset_add_prop_u32_array(cs, pmp, "iommus", iommus, ARRAY_SIZE(iommus)) ?:
	      of_changeset_add_prop_u32(cs, pmp, "mboxes", first + 1) ?:
	      of_changeset_add_prop_string(cs, pmp, "mbox-names", "mbox") ?:
	      of_changeset_add_prop_u32(cs, pmp, "phandle", first + 2);
	if (ret)
		return ret;
	for_each_property_of_node(values, src) {
		if (!t6031_pmp_value(src))
			continue;
		ret = t6031_copy_prop(cs, pmp, src);
		if (ret)
			return ret;
		copied++;
	}
	return copied;
}

static int __init t6031_display_handoff(void)
{
	struct device_node *dcp = NULL, *display = NULL, *piodma = NULL;
	struct device_node *mbox = NULL, *dcp_dart = NULL, *disp_dart = NULL;
	struct device_node *soc = NULL, *aic = NULL, *values = NULL, *existing;
	struct device_node *ps_pmp = NULL, *ps_sram = NULL, *ps, *parent;
	struct device_node *added[3] = {};
	struct device_node *enable[5];
	struct of_changeset *cs = NULL;
	const char *uuid = NULL, *pmp_uuid = NULL;
	u32 compat[3] = {}, cells, v;
	phandle first = 0;
	int ret = -ENODEV, regions, i, copied = 0;

	if (!apple_pmp_t6031_board())
		return 0;

	dcp = t6031_unique("apple,t6031-dcp");
	display = t6031_unique("apple,t6031-display-subsystem");
	piodma = dcp ? of_get_child_by_name(dcp, "piodma") : NULL;
	if (!dcp || !display) {
		pr_info("no T6031 display description in the device tree\n");
		goto out;
	}
	if (!t6031_marked(dcp) || !t6031_marked(display) || !t6031_marked(piodma)) {
		pr_info("the boot loader did not hand the display over (%s missing)\n",
			T6031_HANDOFF);
		goto out;
	}
	of_property_read_string(dcp, "apple,firmware-uuid", &uuid);
	of_property_read_u32_array(dcp, "apple,firmware-compat", compat, 3);
	regions = of_count_phandle_with_args(dcp, "memory-region", NULL);
	pr_info("handoff: DCP firmware %u.%u.%u %s, %d memory regions\n",
		compat[0], compat[1], compat[2], uuid ?: "(no uuid)", regions);

	if (!t6031_display_enable) {
		pr_info("native display not requested; add apple_t6031_display.enable=1 to take it over, the boot framebuffer stays\n");
		ret = 0;
		goto out;
	}
	if (compat[0] != 14 || compat[1] != 7 || compat[2] != 0 || regions < 1 ||
	    of_count_phandle_with_args(display, "memory-region", NULL) < 1) {
		pr_warn("not enabling: the display driver knows DCP firmware 14.7.0 with memory regions only\n");
		ret = -EINVAL;
		goto out;
	}

	mbox = t6031_linked(dcp, "mboxes", "#mbox-cells", 0, 0, "apple,asc-mailbox-v4");
	dcp_dart = t6031_linked(dcp, "iommus", "#iommu-cells", 1, 5, "apple,t8110-dart");
	disp_dart = t6031_linked(display, "iommus", "#iommu-cells", 1, 0, "apple,t8110-dart");
	if (!mbox || !dcp_dart || !disp_dart || dcp_dart == disp_dart) {
		pr_warn("not enabling: the DCP's mailbox and the two display DARTs are not as expected\n");
		ret = -EINVAL;
		goto out;
	}
	enable[0] = dcp_dart;
	enable[1] = disp_dart;
	enable[2] = mbox;
	enable[3] = dcp;
	enable[4] = display;
	for (i = 0; i < ARRAY_SIZE(enable); i++) {
		if (!t6031_disabled(enable[i])) {
			pr_warn("not enabling: %pOF is not disabled\n", enable[i]);
			ret = -EBUSY;
			goto out;
		}
	}

	/* The display's power chain must be the boot loader's, left on. */
	ps = t6031_linked(dcp, "power-domains", "#power-domain-cells", 0, 0,
			  "apple,t6031-pmgr-pwrstate");
	for (i = 0; ps && i < 3; i++) {
		if (!of_property_read_bool(ps, "apple,inherited-on")) {
			pr_warn("not enabling: %pOF is not apple,inherited-on\n", ps);
			of_node_put(ps);
			ret = -EINVAL;
			goto out;
		}
		parent = t6031_linked(ps, "power-domains", "#power-domain-cells", 0, 0,
				      "apple,t6031-pmgr-pwrstate");
		of_node_put(ps);
		ps = parent;
	}
	of_node_put(ps);

	cs = kzalloc_obj(*cs);
	if (!cs) {
		ret = -ENOMEM;
		goto out;
	}
	of_changeset_init(cs);

	if (t6031_display_pmp) {
		values = of_find_node_by_path("/chosen/asahi,t6031-pmp");
		if (!values ||
		    of_property_read_u32(values, "apple,board-id", &v) ||
		    of_property_read_u32(values, "apple,dram-vendor-id", &v) ||
		    of_property_read_string(values, "apple,tunable-uuid", &pmp_uuid)) {
			pr_warn("not enabling: the boot loader passed no PMP values for this Mac\n");
			ret = -ENODEV;
			goto out;
		}
		if (strcmp(pmp_uuid, T6031_PMP_UUID)) {
			pr_warn("not enabling: this Mac's PMP firmware is %s, not %s\n",
				pmp_uuid, T6031_PMP_UUID);
			ret = -EINVAL;
			goto out;
		}
		existing = of_find_node_by_path(T6031_PMP_PATH);
		if (!existing)
			existing = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
		if (existing) {
			pr_warn("not enabling: %pOF already describes the PMP\n", existing);
			of_node_put(existing);
			ret = -EEXIST;
			goto out;
		}
		soc = of_find_node_by_path("/soc");
		aic = t6031_unique("apple,t6031-aic3");
		if (!soc || !aic || !aic->phandle ||
		    of_property_read_u32(aic, "#interrupt-cells", &cells) || cells != 4) {
			pr_warn("not enabling: no /soc or no four-cell T6031 interrupt controller\n");
			ret = -EINVAL;
			goto out;
		}
		ps_pmp = t6031_pwrstate("pmp");
		ps_sram = t6031_pwrstate("pms_sram");
		if (!ps_pmp || !ps_sram) {
			pr_warn("not enabling: no unique pmp and pms_sram power states\n");
			ret = -ENODEV;
			goto out;
		}
		first = t6031_free_phandle();
		ret = t6031_add_pmp(cs, soc, values, aic->phandle, added, first);
		if (ret < 0)
			goto out;
		copied = ret;
		/* Linux would otherwise power these off as unused. */
		ret = 0;
		if (!of_property_read_bool(ps_pmp, "apple,always-on"))
			ret = of_changeset_add_prop_bool(cs, ps_pmp, "apple,always-on");
		if (!ret && !of_property_read_bool(ps_sram, "apple,always-on"))
			ret = of_changeset_add_prop_bool(cs, ps_sram, "apple,always-on");
		if (ret)
			goto out;
	}

	for (i = 0; i < ARRAY_SIZE(enable); i++) {
		ret = of_changeset_update_prop_string(cs, enable[i], "status", "okay");
		if (ret)
			goto out;
	}
	ret = of_changeset_apply(cs);
	if (ret)
		goto out;
	/*
	 * A node attached by the changeset takes its phandle from the
	 * properties it has at that moment, before the changeset adds them.
	 */
	if (added[0]) {
		added[0]->phandle = first;
		added[1]->phandle = first + 1;
		added[2]->phandle = first + 2;
	}
	pr_info("enabled %pOF, %pOF, %pOF, %pOF and %pOF\n", enable[0], enable[1],
		enable[2], enable[3], enable[4]);
	if (added[0])
		pr_info("added the PMP, its DART and mailbox with %d of this Mac's values; pmp and pms_sram kept on\n",
			copied);
	else
		pr_info("no PMP added (apple_t6031_display.pmp=0): the DCP is adopted without a PMP acknowledgement\n");
	cs = NULL;
out:
	if (ret && cs) {
		/* Nothing was applied; the changeset's nodes and properties are dropped. */
		of_changeset_destroy(cs);
		kfree(cs);
		pr_info("display stays on the boot framebuffer (%d)\n", ret);
	}
	of_node_put(ps_sram);
	of_node_put(ps_pmp);
	of_node_put(aic);
	of_node_put(soc);
	of_node_put(values);
	of_node_put(disp_dart);
	of_node_put(dcp_dart);
	of_node_put(mbox);
	of_node_put(piodma);
	of_node_put(display);
	of_node_put(dcp);
	return 0;
}

/* Before of_platform_default_populate() at arch_initcall_sync. */
arch_initcall(t6031_display_handoff);
