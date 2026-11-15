# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Source-connected accounting regressions retaining adverse-review assertions.

SRCU, module and hardware leaves are models; the transaction, DCP and mux core
functions are extracted from the selected public source. No device is accessed.
"""
import quiesce
import transaction
from common import block

def srcu_index(ROOT):
    unit=quiesce.make_unit(ROOT)
    main=block(unit,'int main(')
    unit=unit.replace(main,main.replace('int main(','int __attribute__((unused)) prior_main(',1),1)
    old_lock=block(unit,'static int srcu_read_lock(')
    old_unlock=block(unit,'static void srcu_read_unlock(')
    old_mutex_unlock=block(unit,'static void mutex_unlock(')
    leaves=r'''
    static pthread_mutex_t schedule_mutex=PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t schedule_cond=PTHREAD_COND_INITIALIZER;
    static int srcu_bank,bank_readers[2],released_index=-1;
    static bool pause_unlock,pause_seen,resume_unlock;
    static _Thread_local int expected_index=-1;
    static int srcu_read_lock(int *s) {
     (void)s;pthread_mutex_lock(&schedule_mutex);int i=srcu_bank;bank_readers[i]++;expected_index=i;pthread_mutex_unlock(&schedule_mutex);return i;
    }
    static void srcu_read_unlock(int *s,int i) {
     (void)s;pthread_mutex_lock(&schedule_mutex);released_index=i;
     fprintf(stderr,"SRCU unlock: acquired=%d released=%d bank0=%d bank1=%d\n",expected_index,i,bank_readers[0],bank_readers[1]);
     assert(i==expected_index&&bank_readers[i]>0);bank_readers[i]--;expected_index=-1;
     pthread_cond_broadcast(&schedule_cond);pthread_mutex_unlock(&schedule_mutex);
    }
    static void mutex_unlock(struct mutex *m) {
     assert(!pthread_mutex_unlock(&m->value));
     if(m==&shared_context.lock&&pause_unlock) {
      pthread_mutex_lock(&schedule_mutex);pause_seen=true;pause_unlock=false;pthread_cond_broadcast(&schedule_cond);
      while(!resume_unlock)pthread_cond_wait(&schedule_cond,&schedule_mutex);
      pthread_mutex_unlock(&schedule_mutex);
     }
    }
    '''
    unit=unit.replace(old_lock,'',1).replace(old_unlock,'',1).replace(old_mutex_unlock,leaves,1)
    unit+=r'''
    static void *first_actor(void *p) {
     struct typec_mux *m=p;struct typec_mux_state safe={.mode=TYPEC_STATE_SAFE};assert(typec_mux_set(m,&safe)==0);return NULL;
    }
    int main(void) {
     struct cd321x c;initial(&c);mux.num_mux_devs=0;pause_unlock=true;
     pthread_t thread;assert(!pthread_create(&thread,NULL,first_actor,&mux));
     pthread_mutex_lock(&schedule_mutex);while(!pause_seen)pthread_cond_wait(&schedule_cond,&schedule_mutex);
     srcu_bank=1;pthread_mutex_unlock(&schedule_mutex);
     struct typec_mux_state safe={.mode=TYPEC_STATE_SAFE};assert(typec_mux_begin(&mux,&safe)==0);
     assert(mux.srcu_index==1&&bank_readers[0]==1&&bank_readers[1]==1);
     pthread_mutex_lock(&schedule_mutex);resume_unlock=true;pthread_cond_broadcast(&schedule_cond);pthread_mutex_unlock(&schedule_mutex);
     assert(!pthread_join(thread,NULL));typec_mux_abort(&mux);assert(bank_readers[0]==0&&bank_readers[1]==0);
     puts("PASS same-handle SRCU read-index balance across context unlock");
    }
    '''
    return unit

def resources(ROOT):
    actual=next(u for n,u,c,f in transaction.controls(ROOT) if n=='actual-dcp-normal-safe')
    main=block(actual,'int main(')
    actual=actual.replace(main,main.replace('int main(','int __attribute__((unused)) prior_route_main(',1),1)
    actual=actual.replace('struct mux_control { int unused; };',r'''
    struct mux_control;
    struct mux_ops { int (*set)(struct mux_control *,int); };
    struct mux_chip { const struct mux_ops *ops; };
    struct mux_control { int lock,idle_state,cached_state; unsigned last_change;struct mux_chip *chip; };
    ''',1)
    muxcore=(ROOT/'drivers/mux/core.c').read_text()
    resource=r'''
    #define MUX_IDLE_AS_IS -1
    #define MUX_CACHE_UNKNOWN -2
    static int hw_calls,hw_failures,fixed_restore_calls,fixed_restore_failures;
    static unsigned ktime_get(void) { return 0; }
    static void up(int *s) { (*s)++; }
    static int hw_set(struct mux_control *m,int state) { (void)m;(void)state;hw_calls++;return hw_calls<=hw_failures?-EIO:0; }
    '''
    resource+=block(muxcore,'static int mux_control_set(')+'\n'+block(muxcore,'int mux_control_deselect(')+'\n'
    actual=actual.replace(block(actual,'static int mux_control_deselect('),resource,1)
    actual=actual.replace(block(actual,'static int mux_control_select('),r'''static int mux_control_select(struct mux_control *m,unsigned n) { (void)m;(void)n;fixed_restore_calls++;return fixed_restore_calls<=fixed_restore_failures?-EIO:0; }''',1)
    actual=actual.replace('static int gpiod_get_value_cansleep(void *p) { (void)p;return 0; }', 'static int gpio_level;\nstatic int gpiod_get_value_cansleep(void *p) { (void)p;return gpio_level; }',1)
    actual+=r'''
    static int peer_error;
    static int peer_finish(struct typec_mux_dev *p,struct typec_mux_state *s) { (void)p;(void)s;return peer_error; }
    int main(int argc,char **argv) {
     (void)argc;struct cd321x c;initial(&c);
     struct mux_ops ops={.set=hw_set};struct mux_chip chip={.ops=&ops};
     struct mux_control xbar={.lock=0,.idle_state=0,.cached_state=1,.chip=&chip};
     struct mux_control fixed_xbar={.lock=1,.idle_state=0,.cached_state=0,.chip=&chip};
     route.xbar=route.active_xbar=&xbar;
     struct typec_mux_state safe={.mode=TYPEC_STATE_SAFE};
     if(argv[1][0]=='n') {
      assert(typec_mux_set(&mux,&safe)==0&&xbar.lock==1&&!port.owner&&!port.safe_pending);
      puts("PASS actual DCP finalizer + real mux deselect resource balance");return 0;
     }
     if(argv[1][0]=='l') {
      phy_provider.finish=peer_finish;mux.mux_devs[0]=&display_provider;mux.mux_devs[1]=&phy_provider;peer_error=-EIO;
      assert(typec_mux_set(&mux,&safe)==-EIO&&xbar.lock==1&&port.owner==&route&&port.safe_pending&&port.safe_finish_ready);
      peer_error=0;assert(typec_mux_set(&mux,&safe)==0&&xbar.lock==1&&!port.owner&&!port.safe_pending);
      puts("PASS successful DCP finalizer is not repeated after a later provider failure");return 0;
     }
     if(argv[1][0]=='h')hw_failures=1;
     if(argv[1][0]=='f') {display.fixed_connector_type=2;display.hdmi_hpd=(void *)1;gpio_level=1;display.xbar=display.fixed_xbar=&fixed_xbar;fixed_restore_failures=1;}
     assert(typec_mux_set(&mux,&safe)==-EIO&&xbar.lock==1&&port.owner==&route&&port.safe_pending&&!port.safe_finish_ready);
     bool selected_after_error=route.selected;
     int ret=typec_mux_set(&mux,&safe);
     fprintf(stderr,"finalizer retry: kind=%c ret=%d permits=%d selected_after_error=%d owner=%p pending=%d hw_calls=%d fixed_calls=%d\n",argv[1][0],ret,xbar.lock,selected_after_error,(void *)port.owner,port.safe_pending,hw_calls,fixed_restore_calls);
     assert(xbar.lock==1);
     if(argv[1][0]=='h')assert(ret==-EIO&&hw_calls==1&&port.owner==&route&&port.safe_pending);
     if(argv[1][0]=='f')assert(ret==0&&!port.owner&&!port.safe_pending&&fixed_restore_calls==2);
     return 0;
    }
    '''
    return actual

def getter_unit(ROOT):
    getter=transaction.lifetime(ROOT)
    main=block(getter,'int main(')
    renamed=main.replace('int main(','int __attribute__((unused)) prior_lifetime_main(',1)
    renamed=renamed[:-1]+'return 0;\n}'
    getter=getter.replace(main,renamed,1)
    getter=getter.replace('struct typec_mux_dev {', 'struct module { unsigned refs;bool going; };\nstruct device { unsigned refs; };\nstruct typec_mux_dev { struct module *owner;struct device dev;',1)
    getter=getter.replace(block(getter,'static void typec_mux_put('),'void typec_mux_put(struct typec_mux *mux);',1)
    getter=getter.replace('#include <stdlib.h>',r'''
    #include <stdlib.h>
    static int allocation_count,allocation_failure;
    static void *review_alloc(size_t size) { if(++allocation_count==allocation_failure)return NULL;return calloc(1,size); }
    ''',1)
    getter=getter.replace('#define kzalloc_obj(x) calloc(1,sizeof(x))','#define kzalloc_obj(x) review_alloc(sizeof(x))',1)
    getter+=r'''
    #undef IS_ERR
    #define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
    #define PTR_ERR(p) ((int)(intptr_t)(p))
    #define TYPEC_MUX_MAX_DEVS 3
    #define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
    static struct typec_mux_dev graph_providers[2];
    static struct module modules[2];
    static bool graph_second_error;
    static void *typec_mux_match(const struct fwnode_handle *f,const char *id,void *data) { (void)f;(void)id;(void)data;return NULL; }
    static int fwnode_connection_find_matches(struct fwnode_handle *f,const char *id,void *data,void *(*fn)(const struct fwnode_handle *,const char *,void *),void **matches,unsigned max) {
     (void)f;(void)id;(void)data;(void)fn;assert(max>=2);
     graph_providers[0].dev.refs++;matches[0]=&graph_providers[0];
     if(graph_second_error)matches[1]=ERR_PTR(-EPROBE_DEFER);
     else {graph_providers[1].dev.refs++;matches[1]=&graph_providers[1];}
     return 2;
    }
    static bool try_module_get(struct module *m) { if(!m)return true;if(m->going)return false;m->refs++;return true; }
    static void module_put(struct module *m) { if(m){assert(m->refs);m->refs--;} }
    static void put_device(struct device *d) { assert(d->refs);d->refs--; }
    '''
    getter='#define EPROBE_DEFER 517\n'+getter
    mux=(ROOT/'drivers/usb/typec/mux.c').read_text()
    getter+='\n'+block(mux,'struct typec_mux *fwnode_typec_mux_get(')+'\n'+block(mux,'void typec_mux_put(')+'\n'
    getter+=r'''
    static void graph_setup(void) {
     graph_second_error=false;allocation_count=allocation_failure=0;
     for(unsigned i=0;i<2;i++){modules[i]=(struct module){0};graph_providers[i]=(struct typec_mux_dev){.owner=&modules[i],.set=phy_set};}
    }
    static void graph_balanced(struct fwnode_handle *f) {
     assert(!f->refs&&typec_mux_contexts.next==&typec_mux_contexts);
     for(unsigned i=0;i<2;i++)assert(!modules[i].refs&&!graph_providers[i].dev.refs);
    }
    int main(void) {
     struct fwnode_handle f={0};graph_setup();
     struct typec_mux *a=fwnode_typec_mux_get(&f),*b=fwnode_typec_mux_get(&f);
     assert(!IS_ERR(a)&&!IS_ERR(b)&&a->context==b->context&&a->context->users==2&&f.refs==1);
     for(unsigned i=0;i<2;i++)assert(modules[i].refs==2&&graph_providers[i].dev.refs==2);
     typec_mux_put(a);typec_mux_put(b);graph_balanced(&f);
     graph_setup();modules[1].going=true;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-ENODEV);graph_balanced(&f);
     graph_setup();graph_second_error=true;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-EPROBE_DEFER);graph_balanced(&f);
     graph_setup();allocation_failure=2;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-ENOMEM);graph_balanced(&f);
     graph_setup();a=fwnode_typec_mux_get(&f);graph_providers[0].removed=true;
     struct typec_mux_state safe={.mode=TYPEC_STATE_SAFE};unsigned before=phy_calls;
     assert(typec_mux_set(a,&safe)==-ENODEV&&phy_calls==before);typec_mux_put(a);graph_balanced(&f);
     puts("PASS actual getter/context/module/device balance: same fwnode, failed module conversion, deferred provider, context allocation and stale-provider handle");
     return 0;
    }
    '''
    return getter

def controls(root):
    clock = transaction.clock_order(root).replace('if(parked)return 0;if(!backend_clock)', 'if(!backend_clock)', 1)
    clock = clock.replace('assert(!port.owner&&!port.safe_pending);', 'assert(!port.owner&&!port.safe_pending&&phy_calls==1&&role_calls==2);', 1)
    return [
        ('same-handle-srcu-index', srcu_index(root), ((),), ('-pthread',)),
        ('mux-release-stages', resources(root), (('normal',), ('late',), ('hardware',), ('fixed',)), ('-pthread',)),
        ('single-safe-before-role-exit', clock, ((),), ('-pthread',)),
        ('mux-get-put-balance', getter_unit(root), ((),), ('-pthread',)),
        ('lookup-unregister-module-lifetime', lookup_lifetime(root), ((),), ('-pthread',)),
        ('kernel-module-conversion-balance', module_conversion(root), ((),), ('-pthread',)),
    ]

def lookup_lifetime(ROOT):
    getter = getter_unit(ROOT)
    mux = (ROOT / "drivers/usb/typec/mux.c").read_text()
    module_unit=getter.replace(block(getter,'int main('),block(getter,'int main(').replace('int main(','int __attribute__((unused)) prior_getter_main(',1),1)
    module_unit=module_unit.replace('struct module { unsigned refs;bool going; };','struct module { unsigned refs,refcnt;bool going; };',1)
    module_unit=module_unit.replace('struct device { unsigned refs; };','struct device { unsigned refs;bool unregistered; };',1)
    sync=r'''
    static pthread_mutex_t conversion_mutex=PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t conversion_cond=PTHREAD_COND_INITIALIZER;
    static bool lookup_ready,lookup_resume;
    static unsigned active_readers;
    static void synchronize_srcu(int *s) { (void)s;pthread_mutex_lock(&conversion_mutex);assert(active_readers==0);pthread_mutex_unlock(&conversion_mutex); }
    static void device_unregister(struct device *d) { d->unregistered=true;assert(d->refs);d->refs--; }
    static bool module_is_live(struct module *m) { return !m->going; }
    static bool atomic_inc_not_zero(unsigned *p) { if(!*p)return false;(*p)++;return true; }
    #define likely(x) (x)
    #define trace_module_get(...) ((void)0)
    #define _RET_IP_ 0
    '''
    pos=module_unit.index('static bool try_module_get(')
    module_unit=module_unit[:pos]+sync+module_unit[pos:]
    modulemain=(ROOT/'kernel/module/main.c').read_text()
    module_unit=module_unit.replace(block(module_unit,'static bool try_module_get('),block(modulemain,'bool try_module_get('),1)
    needle='return 2;\n    }'
    assert module_unit.count(needle)==1
    module_unit=module_unit.replace(needle,r'''
     pthread_mutex_lock(&conversion_mutex);lookup_ready=true;pthread_cond_broadcast(&conversion_cond);
     while(!lookup_resume)pthread_cond_wait(&conversion_cond,&conversion_mutex);
     pthread_mutex_unlock(&conversion_mutex);return 2;
    }''',1)

    declarations = r'''
static pthread_mutex_t conversion_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t conversion_cond=PTHREAD_COND_INITIALIZER;
static bool lookup_ready,lookup_resume,draining;
static unsigned active_readers;
#define WRITE_ONCE(x,v) ((x)=(v))
'''
    start = module_unit.index('static pthread_mutex_t conversion_mutex=')
    end = module_unit.index('static void synchronize_srcu(', start)
    module_unit = module_unit[:start] + module_unit[end:]
    pos = module_unit.index('static int srcu_read_lock(')
    module_unit = module_unit[:pos] + declarations + module_unit[pos:]
    module_unit = module_unit.replace(block(module_unit, 'static int srcu_read_lock('), r'''
static int srcu_read_lock(int *s) { (void)s;pthread_mutex_lock(&conversion_mutex);active_readers++;pthread_mutex_unlock(&conversion_mutex);return 0; }
''', 1)
    module_unit = module_unit.replace(block(module_unit, 'static void srcu_read_unlock('), r'''
static void srcu_read_unlock(int *s,int index) { (void)s;assert(index==0);pthread_mutex_lock(&conversion_mutex);assert(active_readers);active_readers--;pthread_cond_broadcast(&conversion_cond);pthread_mutex_unlock(&conversion_mutex); }
''', 1)
    module_unit = module_unit.replace(block(module_unit, 'static void synchronize_srcu('), r'''
static void synchronize_srcu(int *s) { (void)s;pthread_mutex_lock(&conversion_mutex);draining=true;pthread_cond_broadcast(&conversion_cond);while(active_readers)pthread_cond_wait(&conversion_cond,&conversion_mutex);pthread_mutex_unlock(&conversion_mutex); }
''', 1)
    module_unit += '\n' + block(mux, 'void typec_mux_unregister(') + r'''
static struct typec_mux *lookup_result;
static void *getter_actor(void *node) { lookup_result=fwnode_typec_mux_get(node);return NULL; }
static void *unregister_actor(void *provider) { typec_mux_unregister(provider);return NULL; }
int main(void) {
 struct fwnode_handle f={0};graph_setup();
 struct module *departing=calloc(1,sizeof(*departing));departing->going=true;graph_providers[0].owner=departing;graph_providers[0].dev.refs=1;
 pthread_t getter_thread,remove_thread;assert(!pthread_create(&getter_thread,NULL,getter_actor,&f));
 pthread_mutex_lock(&conversion_mutex);while(!lookup_ready)pthread_cond_wait(&conversion_cond,&conversion_mutex);
 assert(active_readers==1&&graph_providers[0].dev.refs==2);pthread_mutex_unlock(&conversion_mutex);
 assert(!pthread_create(&remove_thread,NULL,unregister_actor,&graph_providers[0]));
 pthread_mutex_lock(&conversion_mutex);while(!draining)pthread_cond_wait(&conversion_cond,&conversion_mutex);
 assert(active_readers==1&&!graph_providers[0].dev.unregistered);
 lookup_resume=true;pthread_cond_broadcast(&conversion_cond);pthread_mutex_unlock(&conversion_mutex);
 assert(!pthread_join(getter_thread,NULL));assert(!pthread_join(remove_thread,NULL));
 assert(PTR_ERR(lookup_result)==-ENODEV&&graph_providers[0].dev.unregistered&&graph_providers[0].dev.refs==0&&active_readers==0);
 assert(graph_providers[1].dev.refs==0&&!f.refs);
 free(departing);
 puts("PASS lookup-to-module conversion holds a reader until removed-provider rejection and balanced cleanup");
 return 0;
}
'''
    return module_unit


def module_conversion(ROOT):
    unit = getter_unit(ROOT)
    main = block(unit, 'int main(')
    unit = unit.replace(main, main.replace('int main(', 'int __attribute__((unused)) prior_getter_main(', 1), 1)
    unit = unit.replace('struct module { unsigned refs;bool going; };', 'struct module { unsigned refs,refcnt;bool going; };', 1)
    source = (ROOT / 'kernel/module/main.c').read_text()
    pos = unit.index('static bool try_module_get(')
    leaves = r"""
static bool module_is_live(struct module *m) { return !m->going; }
static bool atomic_inc_not_zero(unsigned *p) { if(!*p)return false;(*p)++;return true; }
static int atomic_dec_if_positive(unsigned *p) { if(!*p)return -1;return --(*p); }
#define likely(x) (x)
#define trace_module_get(...) ((void)0)
#define trace_module_put(...) ((void)0)
#define preempt_disable() ((void)0)
#define preempt_enable() ((void)0)
#define WARN_ON(v) assert(!(v))
#define _RET_IP_ 0
"""
    unit = unit[:pos] + leaves + unit[pos:]
    unit = unit.replace(block(unit, 'static bool try_module_get('), block(source, 'bool try_module_get('), 1)
    unit = unit.replace(block(unit, 'static void module_put('), block(source, 'void module_put('), 1)
    old = block(unit, 'static int srcu_read_lock(') + '\n' + block(unit, 'static void srcu_read_unlock(')
    unit = unit.replace(old, r"""
static unsigned conversion_readers;
static int srcu_read_lock(int *s) { (void)s;conversion_readers++;return 0; }
static void srcu_read_unlock(int *s,int i) { (void)s;assert(i==0&&conversion_readers);conversion_readers--; }
""", 1)
    unit += r"""
static void live_graph(void) { graph_setup();for(unsigned i=0;i<2;i++)modules[i].refcnt=1; }
static void module_balance(struct fwnode_handle *f) { graph_balanced(f);assert(!conversion_readers);for(unsigned i=0;i<2;i++)assert(modules[i].refcnt==1); }
int main(void) {
 struct fwnode_handle f={0};live_graph();
 struct typec_mux *a=fwnode_typec_mux_get(&f),*b=fwnode_typec_mux_get(&f);
 assert(!IS_ERR(a)&&!IS_ERR(b)&&a->context==b->context&&!conversion_readers);
 for(unsigned i=0;i<2;i++)assert(modules[i].refcnt==3&&graph_providers[i].dev.refs==2);
 typec_mux_put(a);typec_mux_put(b);module_balance(&f);
 live_graph();modules[1].going=true;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-ENODEV);module_balance(&f);
 live_graph();allocation_failure=2;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-ENOMEM);module_balance(&f);
 live_graph();graph_second_error=true;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-EPROBE_DEFER);module_balance(&f);
 live_graph();graph_providers[0].removed=true;a=fwnode_typec_mux_get(&f);assert(PTR_ERR(a)==-ENODEV);module_balance(&f);
 puts("PASS actual kernel module get/put and mux get/put: shared handles and every failed conversion unwind");
 return 0;
}
"""
    return unit
