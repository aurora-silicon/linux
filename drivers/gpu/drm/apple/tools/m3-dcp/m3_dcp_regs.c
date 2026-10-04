// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S readl-only diagnostic: never use vector loads on DART registers. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>
static int __init probe(void)
{
 const phys_addr_t bases[] = {0x28d30c000ULL, 0x28d304000ULL};
 const unsigned int sid[] = {5, 0, 4};
 if (!of_machine_is_compatible("apple,j514s")) return -ENODEV;
 for (unsigned int i=0; i<3; i++) {
  phys_addr_t base=bases[i != 0];
  const unsigned int offsets[] = {0x200,0x1000+4*sid[i],0x1400+4*sid[i]};
  void __iomem *regs=ioremap(base,0x4000);
  if (!regs) return -ENOMEM;
  for (unsigned int n=0; n<3; n++) {
   pr_info("M3 DCP reading %pap + %#x using readl\n", &base, offsets[n]);
   msleep(100);
   pr_info("M3 DCP register %pap + %#x = %#x\n", &base, offsets[n], readl(regs+offsets[n]));
  }
  iounmap(regs);
 }
 {
  /* Display clock registers from J514S ADT clock-frequencies-regs entries
   * 92/156, plus the three real display power-state registers. Parent
   * LW00/LS0/IOA0/AFI and display leaf entries have no_ps: do not map them.
   */
  const phys_addr_t addresses[] = {0x28ec00044ULL,0x28ec00048ULL,
   0x3507001c0ULL,0x350700258ULL,0x350710000ULL,
   0x350040064ULL,0x350040164ULL,0x28e850000ULL,0x28e850004ULL};
  for (unsigned int i=0; i<ARRAY_SIZE(addresses); i++) {
   void __iomem *r=ioremap(addresses[i]&~0x3fffULL,0x4000);
   if (!r) return -ENOMEM;
   pr_info("M3 DCP lifecycle %pap = %#x\n", &addresses[i],readl(r+(addresses[i]&0x3fff)));
   iounmap(r);
  }
 }
 {
  /* Qualified on J514S by the 26A428 ApplePMGR getRegMap(8,0)
   * capture and matching retained ADT ptd-range entries. Read aperture
   * entries are 16 bytes: value then timestamp/status. Never memcpy MMIO.
   */
  const unsigned int entries[] = {1,8,9,10,11,280,281,282,283,
   284,285,286,287,288,289,290,291,292,293,294,295,296,297,
   298,299,300,301,302,303};
  void __iomem *ptd = ioremap(0x3503c0000ULL, 0x4000);
  if (!ptd) return -ENOMEM;
  for (unsigned int i=0; i<ARRAY_SIZE(entries); i++) {
   u64 value = readq(ptd + entries[i] * 16);
   u64 metadata = readq(ptd + entries[i] * 16 + 8);
   pr_info("M3 DCP PTD index=%u value=%#llx metadata=%#llx\n",
    entries[i], value, metadata);
  }
  iounmap(ptd);
 }
 return 0;
}
static void __exit done(void) {}
module_init(probe); module_exit(done);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S readl-only display clock, power and DART audit");
