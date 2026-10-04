// SPDX-License-Identifier: GPL-2.0-only
/* Read-only J514S Type-C owner snapshot through the exported cache API.
 * No private TIPD layout dependency, second bus owner, or PHY writes.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spmi.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_cd321x.h>
#include "../../../../../usb/typec/tipd/tps6598x.h"

static bool right_dp_ready, right_dp_partner, any_dp_ready, any_dp_partner;
static const char *controller_names[]={"1-0c","1-0a","2-08"};
static struct spmi_device *controllers[3];
static unsigned int dp_partner_mask, dp_ready_mask;
static int state_get(char *buffer, const struct kernel_param *parameter)
{
 struct cd321x_dp_snapshot state;
 bool partner,ready;
 int ret;
 int (*snapshot)(struct device *, struct cd321x_dp_snapshot *);
 snapshot = symbol_get(cd321x_get_dp_state);
 if (!snapshot) return -EOPNOTSUPP;
 right_dp_ready=right_dp_partner=any_dp_ready=any_dp_partner=false;
 dp_partner_mask=dp_ready_mask=0;
 for(unsigned int i=0;i<ARRAY_SIZE(controllers);i++){
  if(!controllers[i])continue;
  ret=snapshot(&controllers[i]->dev,&state);
  if(ret){symbol_put(cd321x_get_dp_state);return ret;}
  partner=(state.status & TPS_STATUS_PLUG_PRESENT) &&
   (state.data_status & TPS_DATA_STATUS_DP_CONNECTION) &&
   TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT(state.data_status)==TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_D;
  ready=partner && (state.dp_status & DP_STATUS_HPD_STATE);
  any_dp_partner|=partner;any_dp_ready|=ready;
  if(partner)dp_partner_mask|=BIT(i);
  if(ready)dp_ready_mask|=BIT(i);
  if(i==2){right_dp_partner=partner;right_dp_ready=ready;}
 }
 symbol_put(cd321x_get_dp_state);
 if(parameter->arg==&dp_partner_mask || parameter->arg==&dp_ready_mask)
  return param_get_uint(buffer, parameter);
 return param_get_bool(buffer, parameter);
}
static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(right_dp_ready, &state_ops, &right_dp_ready, 0400);
module_param_cb(right_dp_partner, &state_ops, &right_dp_partner, 0400);
module_param_cb(any_dp_ready, &state_ops, &any_dp_ready, 0400);
module_param_cb(any_dp_partner, &state_ops, &any_dp_partner, 0400);
module_param_cb(dp_partner_mask, &state_ops, &dp_partner_mask, 0400);
module_param_cb(dp_ready_mask, &state_ops, &dp_ready_mask, 0400);

static int __init m3_typec_probe(void)
{
 struct device_node *node;
 int ret, count = 0;
 int (*snapshot)(struct device *, struct cd321x_dp_snapshot *);

 if (!of_machine_is_compatible("apple,j514s")) return -ENODEV;
 snapshot = symbol_get(cd321x_get_dp_state);
 if (!snapshot) return -EOPNOTSUPP;
 for_each_compatible_node(node, NULL, "apple,sn201202x") {
  struct spmi_device *sdev = spmi_find_device_by_of_node(node);
  struct cd321x_dp_snapshot state;
  if (!sdev) continue;
  ret = snapshot(&sdev->dev, &state);
  if (!ret) {
   pr_info("m3_typec_probe: %s status=%#x data=%#x dp_status=%#x IRQs=%llu\n",
           dev_name(&sdev->dev), state.status, state.data_status,
           state.dp_status, (unsigned long long)state.hpd_irqs);
   for(unsigned int i=0;i<ARRAY_SIZE(controllers);i++)
    if(!strcmp(dev_name(&sdev->dev),controller_names[i])){
     get_device(&sdev->dev);controllers[i]=sdev;break;
    }
   count++;
  }
  spmi_device_put(sdev);
 }
 symbol_put(cd321x_get_dp_state);
 return count ? 0 : -ENODEV;
}
static void __exit m3_typec_exit(void)
{
 for(unsigned int i=0;i<ARRAY_SIZE(controllers);i++)
  if(controllers[i])spmi_device_put(controllers[i]);
}
module_init(m3_typec_probe);
module_exit(m3_typec_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J514S read-only cached Type-C negotiation snapshot");
