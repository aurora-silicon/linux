# SPDX-License-Identifier: GPL-2.0-only OR MIT
#!/usr/bin/env python3
"""Model both probe orders with actual Aurora mux lookup bodies; not hardware proof.

Usage: python3 reproduce-mux-order.py /path/to/aurora-linux
"""
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else Path(__file__).resolve().parents[2]
s = (root / 'drivers/usb/typec/mux.c').read_text()
def fn(name, s=s):
    a = s.index(name + '(')
    a = s.rfind('\n', 0, a) + 1
    return s[a:s.index('\n}', s.index('{', a)) + 2]
pre = r'''
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define EPROBE_DEFER 517
#define ENOMEM 12
#define ENODEV 19
#define TYPEC_MUX_MAX_DEVS 4
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define READ_ONCE(x) (x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define kzalloc_obj(x) calloc(1,sizeof(x))
#define kfree(x) free(x)
static int typec_mux_class,typec_mux_srcu;
struct typec_mux_dev;
struct fwnode_handle {struct fwnode_handle *parent;bool available;bool profile;struct typec_mux_dev *provider;};
struct device {struct fwnode_handle *node;};
struct typec_mux_dev {struct device dev;bool removed;int owner;unsigned sets;};
struct typec_mux_context {int unused;};
struct typec_mux {struct typec_mux_dev *mux_devs[4];unsigned num_mux_devs;struct typec_mux_context *context;};
static int srcu_read_lock(int *s){return 0;}
static void srcu_read_unlock(int *s,int i){}
static bool try_module_get(int owner){return true;}
static void module_put(int owner){}
static void put_device(struct device *d){}
static bool mux_fwnode_match(struct device *d,const void *n){return d->node==n;}
static struct device *class_find_device(int *c,void *start,const struct fwnode_handle *n,bool (*match)(struct device *,const void *)){
 return n->provider ? &n->provider->dev : NULL;
}
#define to_typec_mux_dev(d) ((struct typec_mux_dev *)((char *)(d)-offsetof(struct typec_mux_dev,dev)))
#define fwnode_for_each_parent_node(n,p) for(p=(n)->parent;p;p=p->parent)
static bool fwnode_device_is_available(struct fwnode_handle *n){return n->available;}
static void fwnode_handle_put(struct fwnode_handle *n){}
static bool fwnode_property_present(const struct fwnode_handle *n,const char *id){return !strcmp(id,"apple,j613-25g83-profile") ? n->profile : true;}
static struct typec_mux_context context;
static struct typec_mux_context *typec_mux_context_get(struct fwnode_handle *n){return &context;}
static struct fwnode_handle *targets[2];
typedef void *(*devcon_match_fn_t)(const struct fwnode_handle *, const char *, void *);
#define fwnode_graph_for_each_endpoint(f,ep) for(unsigned i=0;i<2&&((ep)=targets[i],1);i++)
static struct fwnode_handle *fwnode_graph_get_remote_port_parent(struct fwnode_handle *ep){return ep;}
static unsigned int fwnode_graph_devcon_matches(const struct fwnode_handle *,const char *,void *,devcon_match_fn_t,void **,unsigned int);
static int fwnode_connection_find_matches(struct fwnode_handle *n,const char *id,void *data,
 void *(*match)(const struct fwnode_handle *,const char *,void *),void **out,size_t max){
 return fwnode_graph_devcon_matches(n,id,data,match,out,max);
}
static void apply(struct typec_mux *m){for(unsigned i=0;i<m->num_mux_devs;i++)m->mux_devs[i]->sets++;}
'''
prop = (root / 'drivers/base/property.c').read_text()
tipd = (root / 'drivers/usb/typec/tipd/core.c').read_text()
helper = fn('cd321x_display_gate_pending', tipd)
a = tipd.index('/* Do not retain a PHY-only')
b = tipd.index('dev_dbg(tps->dev,', a)
guard = tipd[a:b]
if '--negative-control' in sys.argv:
    guard = '(void)cd321x_display_gate_pending(tps);'
sync = (root / 'drivers/soc/apple/j613-display.c').read_text()
if '--ignore-nomodeset' in sys.argv:
    sync = sync.replace('video_firmware_drivers_only()', 'false')
if '--drop-failed-completion' in sys.argv:
    sync = sync.replace('j613_display_state = ret ? J613_DISPLAY_FALLBACK : J613_DISPLAY_HANDOFF;', 'if (!ret) j613_display_state = J613_DISPLAY_HANDOFF;')
producer = (root / 'drivers/gpu/drm/apple/j613_dcp_activate.c').read_text()
pre += r'''
#include <string.h>
#include <errno.h>
#define CONFIG_DRM_APPLE drm_built
#define IS_ENABLED(x) (x)
#define __init
#define DEFINE_MUTEX(n) int n
#define mutex_lock(n) ((void)(n))
#define mutex_unlock(n) ((void)(n))
struct device_node { bool profile, done; };
struct tps6598x { struct device *dev; };
static struct device_node dcp_node;
static bool j613 = true, drm_built = true, firmware_only;
static int core_state = -EPROBE_DEFER;
static bool of_machine_is_compatible(const char *s) { return j613; }
static struct device_node *of_find_node_by_path(const char *s) { return &dcp_node; }
static bool of_property_present(struct device_node *n,const char *s) { return n->profile; }
static void of_node_put(struct device_node *n) {}
static bool video_firmware_drivers_only(void) { return firmware_only; }
static int driver_deferred_probe_check_state(struct device *d) { return core_state; }
enum j613_display_state {J613_DISPLAY_PENDING,J613_DISPLAY_HANDOFF,J613_DISPLAY_FALLBACK};
static DEFINE_MUTEX(j613_display_lock);
static enum j613_display_state j613_display_state;
static bool j613_display_expired, j613_display_timer_started;
static struct device *j613_display_consumers[2];
struct work_struct {int unused;};
static int j613_display_timeout_work;
#define HZ 100
static unsigned scheduled,attached;
static struct device *get_device(struct device *d){return d;}
static bool device_is_registered(struct device *d){return true;}
static int device_attach(struct device *d){attached++;return 0;}
#define dev_dbg(...) ((void)0)
static void schedule_delayed_work(int *w,unsigned ticks){assert(ticks==10*HZ);scheduled++;}
'''
pre += '\n'.join(fn(n, sync) for n in ['j613_display_timeout', 'apple_j613_display_begin', 'apple_j613_display_end', 'apple_j613_display_fail', 'apple_j613_display_pending', 'apple_j613_display_skip'])
pre += helper + '\nstatic int probe(struct fwnode_handle *f) {\n struct tps6598x tps_value = {}; struct tps6598x *tps = &tps_value;\n' + guard + r'''
 struct typec_mux *m = fwnode_typec_mux_get(f);
 if (IS_ERR(m)) return PTR_ERR(m);
 int count = m->num_mux_devs;
 free(m);
 return count;
}
'''
# probe calls the production lookup defined below.
pre = pre.replace(helper, 'static struct typec_mux *fwnode_typec_mux_get(struct fwnode_handle *f);\n' + helper)

# Extract the entire coldplug producer, including all early exits and unwind.
pre += r'''
#define pr_info(...) ((void)0)
#define __iomem
#define BIT(n) (1U<<(n))
#define J615_25G83_OPT_IN "optin"
#define J613_25G83_HANDOFF "handoff"
typedef uint32_t u32;
enum {J613_PMP_REPORT,J613_PMP_DART,J613_PMP_MBOX,J613_PMP,J613_DCP_DART,J613_DISP0_DART,J613_DCP_MBOX,J613_DISPLAY,J613_PIODMA,J613_DCP,J613_NR_NODES};
struct of_changeset {int unused;};
static struct of_changeset gate, external_gate;
static bool external_gate_applied, refuse;
static unsigned int display_clock_hz, apply_calls, reverts;
static int allocation_error, application_error;
static struct device_node producer_nodes[J613_NR_NODES];
static bool of_device_is_compatible(struct device_node *n,const char *s){return true;}
static bool apple_t8122_25g83_board(void){return !refuse;}
static int of_property_count_u32_elems(struct device_node *n,const char *s){return strstr(s,"clock-adt")?5:strstr(s,"firmware-compat")?3:1;}
static int of_property_read_u32(struct device_node *n,const char *s,u32 *v){*v=1;return 0;}
static int of_property_read_u32_array(struct device_node *n,const char *s,u32 *v,int count){for(int i=0;i<count;i++)v[i]=1;return 0;}
static int of_property_read_string(struct device_node *n,const char *s,const char **v){*v="disabled";return 0;}
static bool j613_25g83_identity(bool a,u32 p,u32 *v,int c,const char *s){return true;}
static bool j613_25g83_clock(u32 *v,int c){return true;}
static bool j613_25g83_mappings(u32 *v,int c){return true;}
static void j613_resolve(struct device_node **n){for(int i=0;i<J613_DCP;i++)n[i]=&producer_nodes[i];}
static int j613_power_ready(void){return 0;}
static void *of_iomap(struct device_node *n,int i){return n;}
static unsigned readl(void *p){return BIT(4);}
static void iounmap(void *p){}
static void of_changeset_init(struct of_changeset *c){}
static int j613_pmp_values(struct device_node *n){return 0;}
static int of_changeset_update_prop_string(struct of_changeset *c,struct device_node *n,const char *k,const char *v){return allocation_error;}
static int of_changeset_apply(struct of_changeset *c){apply_calls++;return application_error;}
static void of_changeset_revert(struct of_changeset *c){reverts++;}
static void of_changeset_destroy(struct of_changeset *c){}
static int j613_external_gate(struct device_node *n){external_gate_applied=true;return 0;}
'''
pre += fn('apple_j613_25g83_coldplug', producer)
code = pre + fn('fwnode_graph_devcon_matches', prop) + '\n'.join(fn(n) for n in ['typec_mux_ancestors_available', 'typec_mux_match', 'fwnode_typec_mux_get']) + r'''
int main(void){
 struct fwnode_handle processor={.available=false},route={.parent=&processor,.available=true},phy={.available=true},connector={.available=true};
 struct typec_mux_dev atc={.dev.node=&phy},dcp={.dev.node=&route};phy.provider=&atc;targets[0]=&phy;targets[1]=&route;
 struct typec_mux *early=fwnode_typec_mux_get(&connector);assert(early&&!IS_ERR(early)&&early->num_mux_devs==1);
 /* A disabled route node is skipped even before mux_match sees its ancestor. */
 processor.available=true;route.available=false;
 struct typec_mux *own_off=fwnode_typec_mux_get(&connector);
 assert(own_off&&!IS_ERR(own_off)&&own_off->num_mux_devs==1);
 route.available=true;
 processor.available=true; /* J613 coldplug OF activation, provider not registered yet. */
 struct typec_mux *defer=fwnode_typec_mux_get(&connector);assert(IS_ERR(defer)&&PTR_ERR(defer)==-EPROBE_DEFER);
 route.provider=&dcp; /* DCP probe registers its route after activation. */
 apply(early);apply(own_off);assert(atc.sets==2&&dcp.sets==0); /* Later enable/register never refreshes this handle. */
 struct typec_mux *late=fwnode_typec_mux_get(&connector);assert(late&&!IS_ERR(late)&&late->num_mux_devs==2);
 apply(late);assert(atc.sets==3&&dcp.sets==1);
 free(early);free(own_off);free(late);
 /* Run the connector's actual preflight guard before the graph lookup. */
 dcp_node.profile=true; j613_display_state=J613_DISPLAY_PENDING;
 processor.available=false; route.available=false; route.provider=NULL;
 assert(probe(&connector)==-EPROBE_DEFER);
 processor.available=true; route.available=true;
 assert(probe(&connector)==-EPROBE_DEFER); /* changeset not complete */
 assert(apple_j613_display_begin()); apple_j613_display_end(0);
 assert(probe(&connector)==-EPROBE_DEFER); /* provider not registered */
 route.provider=&dcp; assert(probe(&connector)==2);
 route.provider=NULL; route.available=false; processor.available=false;
 assert(probe(&connector)==1); /* gate refusal still permits USB */
 j613_display_state=J613_DISPLAY_PENDING; dcp_node.profile=false;
 assert(probe(&connector)==1); /* ordinary J613 without the changeset */
 dcp_node.profile=true; j613=false;
 assert(probe(&connector)==1); /* another board never waits */
 j613=true; dcp_node.profile=true; processor.profile=true;
 firmware_only=true; assert(video_firmware_drivers_only()); j613_display_state=J613_DISPLAY_PENDING;
 assert(probe(&connector)==1); assert(j613_display_state==J613_DISPLAY_FALLBACK);
 assert(!apple_j613_display_begin()); /* module loaded later cannot reopen */
 firmware_only=false; drm_built=false; j613_display_state=J613_DISPLAY_PENDING;
 assert(probe(&connector)==1); drm_built=true;
 /* A refusal/error publishes fallback with no property allocation at all. */
 for (int error=1;error<=3;error++) {
  j613_display_state=J613_DISPLAY_PENDING;
  assert(apple_j613_display_begin()); apple_j613_display_end(-error);
  assert(probe(&connector)==1); assert(!apple_j613_display_begin());
 }
 /* Module absent or probe failure: use the driver core's terminal retry. */
 for (int accepted=0;accepted<=1;accepted++) {
  j613_display_state=accepted ? J613_DISPLAY_HANDOFF : J613_DISPLAY_PENDING;
  core_state=-ETIMEDOUT; processor.available=true;route.available=true;
  assert(probe(&connector)==1); assert(j613_display_state==J613_DISPLAY_FALLBACK);
 }
 apple_j613_display_fail();
 assert(j613_display_state==J613_DISPLAY_FALLBACK);
 /* An already registered handoff is retained after the timeout. */
 j613_display_state=J613_DISPLAY_HANDOFF;route.provider=&dcp;
 assert(probe(&connector)==2);assert(j613_display_state==J613_DISPLAY_HANDOFF);
 route.provider=NULL;
 /* Fallback never hides a missing PHY provider. */
 j613_display_state=J613_DISPLAY_FALLBACK;phy.provider=NULL;
 assert(probe(&connector)==-EPROBE_DEFER);phy.provider=&atc;
 /* The independent deadline retries both controllers even with no core timeout. */
 core_state=-EPROBE_DEFER; j613_display_expired=false;
 j613_display_state=J613_DISPLAY_PENDING;
 struct device consumer={};j613_display_consumers[0]=&consumer;
 j613_display_timeout(NULL);assert(j613_display_expired && attached==1);
 assert(!j613_display_consumers[0]);
 assert(j613_display_state==J613_DISPLAY_FALLBACK);
 assert(probe(&connector)==1);assert(!apple_j613_display_begin());
 processor.available=false;route.available=false;
 /* Exercise producer allocation/application failures, not a fake done bit. */
 for (int failure=0;failure<4;failure++) {
  j613_display_state=J613_DISPLAY_PENDING;
  external_gate_applied=false;
  refuse=failure==1;allocation_error=failure==2?-ENOMEM:0;
  application_error=failure==3?-EIO:0;
  int ret=apple_j613_25g83_coldplug();
  assert((ret==0)==(failure==0));
  assert(j613_display_state==(failure?J613_DISPLAY_FALLBACK:J613_DISPLAY_HANDOFF));
  if(failure)assert(probe(&connector)==1);
 }
 assert(reverts==1); /* failed application unwinds the external gate */
 allocation_error=0;application_error=0;refuse=false;
 j613_display_state=J613_DISPLAY_FALLBACK;
 unsigned before=apply_calls;
 assert(apple_j613_25g83_coldplug()==-ENODEV && apply_calls==before);
}
'''
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / 'mux.c'
    p.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    result = subprocess.run([str(p.with_suffix(''))])
    if result.returncode:
        sys.exit(1)
print('PASS production coldplug success/refusal/allocation/application failures; nomodeset/no-driver/late-module/probe-timeout fallback; early J613 defers; enabled/unregistered defers; registered has both muxes; refused/no-profile/other-board skip; original lookup: unavailable route or ancestor produces an early handle omits disabled DCP and stays ATC-only after coldplug/register; available-unregistered defers; late handle includes both')
