// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hibernation on Apple Silicon under an aura boot object.
 *
 * A hibernation image restores every saved page to the frame it was taken from, and swsusp saves
 * reserved pages as long as they are mapped. Under aura the memory below Linux's own allocations
 * holds what the boot loader keeps alive across the hand-off: aura itself (resident at EL2), its
 * heap with the firmware buffers it handed over, the device tree, the initramfs, and the ranges
 * the boot firmware placed for the co-processors. Restoring old contents there would overwrite
 * the running boot loader and the live firmware state of the resuming boot.
 *
 * aura marks a boot whose layout is the same on every boot with /chosen/aura,hibernate-layout
 * (start, end of the range it keeps from Linux). On such a boot every /memreserve/ range and every
 * non-reusable /reserved-memory range is registered as nosave, except the kernel image itself.
 * Without the property, hibernation and resume are refused, since the reserved ranges move.
 */

#define pr_fmt(fmt) "aura-hibernate: " fmt

#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/pfn.h>
#include <linux/suspend.h>
#include <linux/soc/apple/aura-hibernate.h>
#include <asm/memory.h>
#include <asm/sections.h>

static bool aura_layout_fixed;

/*
 * Ranges kept out of the image, in page frames. Filled once at late_initcall,
 * read by arm64's pfn_is_nosave() for reserved pages. A static table avoids
 * register_nosave_region(), which allocates from memblock and is meant for
 * setup_arch() time.
 */
#define AURA_NOSAVE_MAX 128
static struct { unsigned long start, end; } aura_nosave[AURA_NOSAVE_MAX];
static unsigned int aura_nosave_count;

bool aura_hibernate_layout_fixed(void)
{
	return aura_layout_fixed;
}

bool aura_hibernate_pfn_nosave(unsigned long pfn)
{
	unsigned int i;

	for (i = 0; i < aura_nosave_count; i++)
		if (pfn >= aura_nosave[i].start && pfn < aura_nosave[i].end)
			return true;
	return false;
}

static void __init aura_nosave_add(unsigned long s, unsigned long e)
{
	if (s >= e)
		return;
	if (aura_nosave_count == AURA_NOSAVE_MAX) {
		pr_warn("more than %d reserved ranges; hibernation stays disabled\n", AURA_NOSAVE_MAX);
		aura_nosave_count++;
		return;
	}
	if (aura_nosave_count < AURA_NOSAVE_MAX) {
		aura_nosave[aura_nosave_count].start = s;
		aura_nosave[aura_nosave_count].end = e;
		aura_nosave_count++;
	}
}

static void __init aura_nosave_range(u64 start, u64 end, unsigned long kstart, unsigned long kend,
				     const char *what)
{
	unsigned long s = PFN_DOWN(start), e = PFN_UP(end);

	if (s >= e)
		return;
	/* The kernel image is Linux's own and is saved; split the range around it. */
	if (s < kend && e > kstart) {
		if (s < kstart)
			aura_nosave_add(s, kstart);
		if (e > kend)
			aura_nosave_add(kend, e);
	} else {
		aura_nosave_add(s, e);
	}
	pr_debug("nosave %s [%#llx, %#llx)\n", what, start, end);
}

static int __init aura_hibernate_init(void)
{
	unsigned long kstart = PFN_DOWN(__pa_symbol(_text)), kend = PFN_UP(__pa_symbol(_end));
	struct device_node *chosen, *rmem, *child;
	unsigned int count = 0;
	const void *fdt = initial_boot_params;
	u64 layout[2];
	int i, n;

	/* Only a boot loader that keeps the layout fixed publishes it. */
	chosen = of_find_node_by_path("/chosen");
	if (!chosen ||
	    of_property_read_u64_array(chosen, "aura,hibernate-layout", layout, 2)) {
		of_node_put(chosen);
		return 0;
	}
	of_node_put(chosen);

	if (fdt) {
		n = fdt_num_mem_rsv(fdt);
		for (i = 0; i < n; i++) {
			u64 addr, size;

			if (fdt_get_mem_rsv(fdt, i, &addr, &size) || !size)
				continue;
			aura_nosave_range(addr, addr + size, kstart, kend, "memreserve");
			count++;
		}
	}

	rmem = of_find_node_by_path("/reserved-memory");
	for_each_available_child_of_node(rmem, child) {
		struct resource r;

		if (of_property_read_bool(child, "reusable"))
			continue;
		if (of_address_to_resource(child, 0, &r))
			continue;
		aura_nosave_range(r.start, r.end + 1, kstart, kend, "reserved-memory");
		count++;
	}
	of_node_put(rmem);

	if (aura_nosave_count > AURA_NOSAVE_MAX) {
		aura_nosave_count = 0;
		return 0;
	}
	aura_layout_fixed = true;
	pr_info("fixed boot layout [%#llx, %#llx), %u reserved ranges kept out of the image\n",
		layout[0], layout[1], count);
	return 0;
}
late_initcall(aura_hibernate_init);
