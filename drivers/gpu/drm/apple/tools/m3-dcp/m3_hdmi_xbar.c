// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S DCPEXT0->ATC3 crossbar. Acquire ATC3_COMMON; the caller also holds
 * DCPEXT0 power. Retain all ownership until reboot, including failed writes. */
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
#include "m3_hdmi_xbar.h"
static int result = -EINPROGRESS;
module_param(result, int, 0400);
static bool roundtrip;
module_param(roundtrip, bool, 0400);
static struct device *owner;
static struct m3_hdmi_dpxbar xbar;
int m3_hdmi_xbar_deactivate(void)
{
 if(result || !xbar.regs)return -EHOSTDOWN;
 return m3_hdmi_dpxbar_down(&xbar);
}
EXPORT_SYMBOL_GPL(m3_hdmi_xbar_deactivate);
int m3_hdmi_xbar_activate(void)
{
 if(result || !xbar.regs)return -EHOSTDOWN;
 return xbar.selected?0:m3_hdmi_dpxbar_up(&xbar);
}
EXPORT_SYMBOL_GPL(m3_hdmi_xbar_activate);
static int __init probe(void)
{
 struct of_phandle_args args = {};
 const char *label;
 int ret;
 if (!of_machine_is_compatible("apple,j514s") ||
     !of_machine_is_compatible("apple,t6030")) return -ENODEV;
 args.np = of_find_node_by_path("/soc/power-management@350700000/power-controller@4f8");
 if (!args.np) return -ENODEV;
 if (of_property_read_string(args.np, "label", &label) || strcmp(label, "atc3_common")) {
  of_node_put(args.np); return -EINVAL;
 }
 owner = root_device_register("m3-hdmi-xbar");
 if (IS_ERR(owner)) { of_node_put(args.np); return PTR_ERR(owner); }
 ret = of_genpd_add_device(&args, owner);
 of_node_put(args.np);
 if (ret) { root_device_unregister(owner); return ret; }
 pm_runtime_set_suspended(owner);
 pm_runtime_enable(owner);
 ret = pm_runtime_resume_and_get(owner);
 if (ret < 0) goto done;
 if (!request_mem_region(0x130304c000ULL, 0x4000, "m3-hdmi-xbar")) {
  ret = -EBUSY; goto done;
 }
 xbar.regs = ioremap_np(0x130304c000ULL, 0x4000);
 if (!xbar.regs) { ret = -ENOMEM; goto done; }
 ret = m3_hdmi_dpxbar_up(&xbar);
 if (!ret && roundtrip) ret = m3_hdmi_dpxbar_down(&xbar);
done:
 result = ret;
 pr_info("m3_hdmi_xbar: result=%d; resources retained until reboot\n", ret);
 return 0;
}
module_init(probe);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S HDMI ATC3 display crossbar diagnostic");
