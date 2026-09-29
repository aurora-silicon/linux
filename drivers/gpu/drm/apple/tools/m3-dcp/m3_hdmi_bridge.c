// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S native HDMI bridge power/HPD diagnostic. GPIO API only.
 * ADT dp2hdmi-gpio0: gP0f HDMI power, gP1a converter power; AOP GPIO47 HPD.
 * Native AppleHDMIPortController prefers converter power over reset when both
 * exist. Do not toggle the reset, CEC or force-DFU lines. Retain ownership.
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
static bool power;
module_param(power, bool, 0400);
static int result = -EINPROGRESS;
module_param(result, int, 0400);
static int hpd = -1;
module_param(hpd, int, 0400);
static struct device *owner;
static struct gpio_desc *port, *bridge, *detect;
/* The session owns firmware notifications; this module owns the GPIO. */
int m3_hdmi_bridge_hpd(void);
int m3_hdmi_bridge_hpd(void)
{
 if (result || IS_ERR_OR_NULL(detect)) return -ENODEV;
 return gpiod_get_value_cansleep(detect);
}
EXPORT_SYMBOL_GPL(m3_hdmi_bridge_hpd);
static struct gpiod_lookup_table lines = {
 .dev_id = "m3-hdmi-bridge",
 .table = {
  GPIO_LOOKUP("macsmc-pmu-gpio", 15, "port-power", GPIO_ACTIVE_HIGH),
  GPIO_LOOKUP("macsmc-pmu-gpio", 26, "bridge-power", GPIO_ACTIVE_HIGH),
  GPIO_LOOKUP("374824000.pinctrl", 47, "hpd", GPIO_ACTIVE_HIGH),
  { }
 }
};
static int __init probe(void)
{
 int ret, p, b;
 if (!of_machine_is_compatible("apple,j514s") ||
     !of_machine_is_compatible("apple,t6030")) return -ENODEV;
 owner = root_device_register("m3-hdmi-bridge");
 if (IS_ERR(owner)) return PTR_ERR(owner);
 gpiod_add_lookup_table(&lines);
 port = gpiod_get(owner, "port-power", GPIOD_ASIS);
 if (IS_ERR(port)) {ret=PTR_ERR(port);goto done;}
 bridge = gpiod_get(owner, "bridge-power", GPIOD_ASIS);
 if (IS_ERR(bridge)) {ret=PTR_ERR(bridge);goto done;}
 detect = gpiod_get(owner, "hpd", GPIOD_IN);
 if (IS_ERR(detect)) {ret=PTR_ERR(detect);goto done;}
 if (gpiod_get_direction(port) != 0 || gpiod_get_direction(bridge) != 0) {
  ret=-EINVAL;goto done;
 }
 p=gpiod_get_value_cansleep(port);b=gpiod_get_value_cansleep(bridge);
 hpd=gpiod_get_value_cansleep(detect);
 pr_info("m3_hdmi_bridge: before port=%d bridge=%d hpd=%d\n",p,b,hpd);
 if (p<0 || b<0 || hpd<0) {ret=-EIO;goto done;}
 if (power) {
  ret=gpiod_set_value_cansleep(port, 1);if(ret)goto done;
  ret=gpiod_set_value_cansleep(bridge, 1);if(ret)goto done;
  msleep(500);
  /* The converter can pulse HPD low while starting. Wait for the real
   * connector signal, rather than injecting HPD high into an absent sink. */
  for(unsigned int sample=0,stable=0;sample<100;sample++){
   hpd=gpiod_get_value_cansleep(detect);
   if(hpd<0){ret=hpd;goto done;}
   stable=hpd?stable+1:0;
   if(stable==5)break;
   msleep(50);
   if(sample==99){ret=-ENODEV;goto done;}
  }
  p=gpiod_get_value_cansleep(port);b=gpiod_get_value_cansleep(bridge);
  hpd=gpiod_get_value_cansleep(detect);
  pr_info("m3_hdmi_bridge: powered port=%d bridge=%d hpd=%d\n",p,b,hpd);
 }
 ret=(p<0 || b<0 || hpd<0 || (power && (p!=1 || b!=1))) ? -EIO : 0;
done:
 result=ret;
 pr_info("m3_hdmi_bridge: result=%d power=%u; GPIO ownership retained until reboot\n",ret,power);
 return 0;
}
module_init(probe);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S native HDMI converter power and HPD diagnostic");
