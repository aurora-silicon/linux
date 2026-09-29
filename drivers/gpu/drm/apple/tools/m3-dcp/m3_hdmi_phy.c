// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S HDMI ATC3 power, common/AUX and standalone main-link diagnostic.
 * Addresses: J514S ADT atc-phy3, native 26A428 descriptor capture and T6030 DT.
 * Retain genpd/device/MMIO ownership until reboot, including on failure.
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include "m3_hdmi_control.h"
#include "m3_hdmi_phy_power.h"
#include "m3_hdmi_phy_common.h"
#include "m3_hdmi_phy_dp-main.h"
#include "m3_hdmi_phy_dp-drive.h"
static int result = -EINPROGRESS;
module_param(result, int, 0400);
static bool roundtrip;
module_param(roundtrip, bool, 0400);
static unsigned int main_rate;
module_param(main_rate, uint, 0400);
static bool common;
module_param(common, bool, 0400);
static unsigned int calibration[12];
static unsigned int calibration_count;
module_param_array(calibration, uint, &calibration_count, 0400);
static const u32 offsets[] = {0x800, 0x2224, 0x2230, 0x2240, 0x22b0, 0x22e4,
 0x2334, 0x2248, 0x16008, 0x1600c, 0xa04, 0x260c};
static const u32 masks[] = {0x3f0000, 0xc00, 0x1f, 0x18007f, 0x8700000, 0x60001,
 0xf800000, 0x1ff00, 0x38, 0x1f, 0x7c, 0x3ff};
static struct device *owner;
static void __iomem *core;
static bool phy_active;
bool m3_hdmi_phy_ready(void)
{
 return result==0 && core && phy_active && (readl(core+0x804)&1) &&
  (!!(readl(core+0x7034)&8)==!!main_rate) && readl(core+0x60)==0x492;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_ready);
int m3_hdmi_phy_deactivate(void)
{
 int ret;
 if(!m3_hdmi_phy_ready())return -EHOSTDOWN;
 ret=main_rate?m3_hdmi_atc_dp_main_stop(core,3):0;
 if(ret)return ret;
 m3_hdmi_atc_aux_stop(core);
 m3_hdmi_atc_common_stop(core);
 ret=m3_hdmi_atc_power_down(core);
 pr_info("m3_hdmi_phy: deactivation result=%d\n",ret);
 if(!ret){main_rate=0;phy_active=false;}
 return ret;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_deactivate);
int m3_hdmi_phy_get_rate(void)
{
 return m3_hdmi_phy_ready()?main_rate:-EHOSTDOWN;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_get_rate);
int m3_hdmi_phy_set_rate(unsigned int rate)
{
 int ret;
 if(rate!=0 && rate!=6 && rate!=10 && rate!=20)return -EINVAL;
 if(!m3_hdmi_phy_ready())return -EHOSTDOWN;
 if(rate==main_rate)return 0;
 ret=main_rate?m3_hdmi_atc_dp_main_stop(core,3):0;
 if(!ret && rate)ret=m3_hdmi_atc_dp_main_prepare(core,3,rate,false);
 if(ret)result=ret;else main_rate=rate;
 pr_info("m3_hdmi_phy: set rate=%u result=%d PLL=%#x\n",rate,ret,readl(core+0x7034));
 return ret;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_set_rate);
int m3_hdmi_phy_set_drive(const unsigned int drive[12])
{
 /* 26A428 AppleT8122TypeCPhy fallback at VA0xfffffe000783cac0.
  * Captured J514S ATC3 ADT has no training-table overrides. */
 static const u32 table[16]={0x3f024,0x237013,0x37200b,0x46e004,
  0x3f01c,0x23700b,0x372004,0x3f000,0x3f013,0x237004,0x3f000,0x3f000,
  0x3f004,0x3f000,0x3f000,0x3f000};
 if(!m3_hdmi_phy_ready() || !main_rate)return -EHOSTDOWN;
 for(unsigned int i=0;i<12;i+=3)
  if(drive[i] || drive[i+1]>3 || drive[i+2]>3 || drive[i+1]+drive[i+2]>3)return -EINVAL;
 /* Until native four-lane orientation order is qualified, require equal
  * settings on each pair. This makes both possible pair orders identical. */
 if(memcmp(drive,drive+6,6*sizeof(*drive)))return -EOPNOTSUPP;
 for(unsigned int i=0;i<4;i++){
  u32 preset=table[drive[3*i+1]*4+drive[3*i+2]];
  int ret=m3_hdmi_atc_dp_drive_preset(core,i/2,i&1,preset);
  if(ret){result=ret;return ret;}
  pr_info("m3_hdmi_phy: drive lane=%u voltage=%u emphasis=%u preset=%#x\n",i,drive[3*i+1],drive[3*i+2],preset);
 }
 return 0;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_set_drive);
static int phy_start(bool lanes)
{
 int ret=m3_hdmi_atc_power_up_reset(core);
 if(ret)return ret;
  for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
   m3_hdmi_atc_component_update(core, offsets[i], masks[i], calibration[i]);
   if ((readl(core + offsets[i]) & masks[i]) != calibration[i]) { ret = -EIO; return ret; }
  }
  if (lanes) {
   /* Native mode3 is four-lane DP, fixed HDMI orientation. */
   m3_hdmi_atc_component_update(core, 0x60, 0xfff, 0x492);
   m3_hdmi_atc_component_update(core, 0x64, 0x1f, 0x14);
   m3_hdmi_atc_component_update(core, 0x64, 0x1ffe0, 0x22000);
  }
  /* Native phy_init_partC releases bit4, waits100us, then cmn_init. */
  udelay(10);
  m3_hdmi_atc_component_update(core, 0x20000, 0, BIT(4));
  udelay(100);
  ret = m3_hdmi_atc_common_start(core);
  if (ret) return ret;
  m3_hdmi_atc_aux_start(core);
  if ((readl(core + 0x16400) & 0x30f) != 0x10f || (readl(core + 0x16000) & 1)) ret = -EIO;
  pr_info("m3_hdmi_phy: common ready=%#x AUX ctrl=%#x cfg=%#x result=%d\n",
   readl(core + 0x804), readl(core + 0x16400), readl(core + 0x16000), ret);
  if (!ret && main_rate) {
   ret = m3_hdmi_atc_dp_main_prepare(core, 3, main_rate, false);
   pr_info("m3_hdmi_phy: main rate=%u result=%d request=%#x PLL=%#x lanes=%#x xbar=%#x\n",
    main_rate, ret, readl(core + 0x2000), readl(core + 0x7034),
    readl(core + 0x60), readl(core + 0x64));
  }
 return ret;
}
int m3_hdmi_phy_activate(void)
{
 int ret;
 if(result || !core || !common)return -EHOSTDOWN;
 if(phy_active)return m3_hdmi_phy_ready()?0:-EIO;
 /* DPTX will choose the link rate; restore common/AUX and four-lane mode. */
 main_rate=0;
 ret=phy_start(true);
 if(!ret)phy_active=true;else result=ret;
 pr_info("m3_hdmi_phy: reactivation result=%d\n",ret);
 return ret;
}
EXPORT_SYMBOL_GPL(m3_hdmi_phy_activate);
static int __init probe(void)
{
 struct of_phandle_args args = {};
 const char *label;
 int ret;
 if (main_rate && (!common || main_rate != 10)) return -EINVAL;
 if (common) {
  if (roundtrip || calibration_count != ARRAY_SIZE(offsets)) return -EINVAL;
  for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++)
   if (calibration[i] & ~masks[i]) return -EINVAL;
 }
 if (!of_machine_is_compatible("apple,j514s") ||
     !of_machine_is_compatible("apple,t6030")) return -ENODEV;
 args.np = of_find_node_by_path("/soc/power-management@350700000/power-controller@4f8");
 if (!args.np) return -ENODEV;
 if (of_property_read_string(args.np, "label", &label) || strcmp(label, "atc3_common")) {
  of_node_put(args.np); return -EINVAL;
 }
 owner = root_device_register("m3-hdmi-phy");
 if (IS_ERR(owner)) { of_node_put(args.np); return PTR_ERR(owner); }
 ret = of_genpd_add_device(&args, owner);
 of_node_put(args.np);
 if (ret) { root_device_unregister(owner); return ret; }
 pm_runtime_set_suspended(owner);
 pm_runtime_enable(owner);
 ret = pm_runtime_resume_and_get(owner);
 if (ret < 0) goto done;
 if (!request_mem_region(0x1303000000ULL, 0x30000, "m3-hdmi-phy")) {
  ret = -EBUSY; goto done;
 }
 core = ioremap_np(0x1303000000ULL, 0x30000);
 if (!core) { ret = -ENOMEM; goto done; }
 pr_info("m3_hdmi_phy: ATC3 ctrl=%#x status=%#x misc=%#x\n",
  readl(core + 0x20000), readl(core + 0x20004), readl(core + 0x20008));
 ret = m3_hdmi_atc_power_idle(core);
 if (ret || (!roundtrip && !common)) goto done;
 if(roundtrip)ret = m3_hdmi_atc_power_up_reset(core);
 if (!ret && roundtrip) ret = m3_hdmi_atc_power_down(core);
 if (!ret && common) {
  ret=phy_start(!!main_rate);
 }
done:
 phy_active=!ret && common;
 result = ret;
 pr_info("m3_hdmi_phy: result=%d roundtrip=%u; resources retained until reboot\n", ret, roundtrip);
 return 0;
}
module_init(probe);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S HDMI ATC3 power and common/AUX diagnostic");
