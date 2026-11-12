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
struct fwnode_handle {struct fwnode_handle *parent;bool available;struct typec_mux_dev *provider;};
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
static bool fwnode_property_present(const struct fwnode_handle *n,const char *id){return true;}
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
    guard = '(void)cd321x_display_gate_pending();'
pre += r'''
#include <string.h>
#define CONFIG_DRM_APPLE 1
#define IS_ENABLED(x) (x)
struct device_node { bool profile, done; };
static struct device_node dcp_node;
static bool j613 = true;
static bool of_machine_is_compatible(const char *s) { return j613; }
static struct device_node *of_find_node_by_path(const char *s) { return &dcp_node; }
static bool of_property_present(struct device_node *n,const char *s) {
 return strstr(s,"coldplug-done") ? n->done : n->profile;
}
static void of_node_put(struct device_node *n) {}
'''
pre += helper + '\nstatic int probe(struct fwnode_handle *f) {\n' + guard + r'''
 struct typec_mux *m = fwnode_typec_mux_get(f);
 if (IS_ERR(m)) return PTR_ERR(m);
 int count = m->num_mux_devs;
 free(m);
 return count;
}
'''
# probe calls the production lookup defined below.
pre = pre.replace(helper, 'static struct typec_mux *fwnode_typec_mux_get(struct fwnode_handle *f);\n' + helper)

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
 dcp_node.profile=true; dcp_node.done=false;
 processor.available=false; route.available=false; route.provider=NULL;
 assert(probe(&connector)==-EPROBE_DEFER);
 processor.available=true; route.available=true;
 assert(probe(&connector)==-EPROBE_DEFER); /* changeset not complete */
 dcp_node.done=true;
 assert(probe(&connector)==-EPROBE_DEFER); /* provider not registered */
 route.provider=&dcp; assert(probe(&connector)==2);
 route.provider=NULL; route.available=false; processor.available=false;
 assert(probe(&connector)==1); /* gate refusal still permits USB */
 dcp_node.done=false; dcp_node.profile=false;
 assert(probe(&connector)==1); /* ordinary J613 without the changeset */
 dcp_node.profile=true; j613=false;
 assert(probe(&connector)==1); /* another board never waits */
}
'''
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / 'mux.c'
    p.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    result = subprocess.run([str(p.with_suffix(''))])
    if result.returncode:
        sys.exit(1)
print('PASS early J613 defers; enabled/unregistered defers; registered has both muxes; refused/no-profile/other-board skip; original lookup: unavailable route or ancestor produces an early handle omits disabled DCP and stays ATC-only after coldplug/register; available-unregistered defers; late handle includes both')
