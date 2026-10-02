// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S reservation/power audit. Explicit load under the M3 watchdog guard.
 * No power, DART, mailbox or PHY writes. Read DART only if the relevant
 * real PMGR domains are on. no_ps ADT entries have no register to inspect.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

static unsigned int port;
module_param(port, uint, 0400);
static bool read_dart;
module_param(read_dart, bool, 0400);
MODULE_PARM_DESC(read_dart, "Inspect powered DART after validating reservations");
static bool read_cpu;
module_param(read_cpu, bool, 0400);

static int __init audit(void)
{
	const u32 power[2][3] = {{0x3b8, 0x3d8, 0x3e0}, {0x3c0, 0x3f0, 0x3f8}};
	const phys_addr_t coproc[] = {0x2d2c00000ULL, 0x2d6c00000ULL};
	const phys_addr_t dart[] = {0x2d130c000ULL, 0x2d530c000ULL};
	const phys_addr_t scanout[] = {0x2d1304000ULL, 0x2d5304000ULL};
	struct device_node *node;
	struct regmap *pmgr;
	struct resource resource;
	char alias[16];
	u32 marker, value;
	bool powered = true;
	int ret;

	if (port > 1 || !of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	snprintf(alias, sizeof(alias), "dcpext%u", port);
	node = of_find_node_by_path(alias);
	if (!node)
		return -ENODEV;
	ret = of_property_read_u32(node, "apple,j514s-dcpext-reservations", &marker);
	if (ret || marker != 1 || of_device_is_available(node) ||
	    !of_device_is_compatible(node, "apple,t6030-dcpext") ||
	    of_address_to_resource(node, 0, &resource) || resource.start != coproc[port] ||
	    of_count_phandle_with_args(node, "memory-region", NULL) != 7) {
		of_node_put(node);
		return -EINVAL;
	}
	for (unsigned int i = 0; i < 7; i++) {
		struct device_node *memory = of_parse_phandle(node, "memory-region", i);

		if (!memory || !of_property_read_bool(memory, "no-map") ||
		    of_address_to_resource(memory, 0, &resource)) {
			of_node_put(memory);
			of_node_put(node);
			return -EINVAL;
		}
		pr_info("M3 DCPEXT%u protected region %u: %pr\n", port, i, &resource);
		of_node_put(memory);
	}
	of_node_put(node);
	node = of_find_node_by_path("/soc/power-management@350700000");
	if (!node)
		return -ENODEV;
	pmgr = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(pmgr))
		return PTR_ERR(pmgr);
	for (unsigned int i = 0; i < 3; i++) {
		ret = regmap_read(pmgr, power[port][i], &value);
		if (ret)
			return ret;
		pr_info("M3 DCPEXT%u PMGR +%#x = %#x\n", port, power[port][i], value);
		/* CPU auto-gating is normal on T6030; require the parent
		 * SYS/FE fabric to remain active before optional ASC reads.
		 */
		if ((value & 0xff) != 0xff ||
		    value & (BIT(31) | BIT(12) | BIT(11) | BIT(10)) ||
		    (i < 2 && (value & BIT(28))))
			powered = false;
	}
	if ((!read_dart && !read_cpu) || !powered) {
		pr_info("M3 DCPEXT%u no DART reads: requested=%u powered=%u\n",
			port, read_dart, powered);
		return 0;
	}
	if (read_cpu) {
		void __iomem *regs = ioremap(coproc[port], 0x4000);

		if (!regs)
			return -ENOMEM;
		pr_info("M3 DCPEXT%u reading ASC control/status\n", port);
		msleep(20);
		pr_info("M3 DCPEXT%u ASC control=%#x status=%#x\n",
			port, readl(regs + 0x44), readl(regs + 0x48));
		iounmap(regs);
	}
	for (unsigned int i = 0; read_dart && i < 3; i++) {
		unsigned int sid = i == 0 ? 5 : i == 1 ? 0 : 4;
		phys_addr_t base = i == 0 ? dart[port] : scanout[port];
		const unsigned int offsets[] = {0x200, 0x1000 + sid * 4, 0x1400 + sid * 4};
		void __iomem *regs = ioremap(base, 0x4000);

		if (!regs)
			return -ENOMEM;
		for (unsigned int n = 0; n < ARRAY_SIZE(offsets); n++) {
			pr_info("M3 DCPEXT%u reading %pap + %#x\n", port, &base, offsets[n]);
			msleep(20);
			pr_info("M3 DCPEXT%u DART %pap + %#x = %#x\n",
				port, &base, offsets[n], readl(regs + offsets[n]));
		}
		iounmap(regs);
	}
	return 0;
}

static void __exit done(void) {}
module_init(audit);
module_exit(done);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S external display reservation and scalar register audit");
