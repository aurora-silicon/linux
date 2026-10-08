#!/usr/bin/env python3
"""Compile actual USB4 lifecycle functions with controlled host dependencies.

This exercises provider callback lifetime, PCIe bind readiness, chained IRQ
draining and restore ordering. Hardware, device-core scheduling and MMIO are replaced by fixtures;
the selected production function bodies are extracted without rewriting.
"""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(r"(?m)^(?:static )?(?:void|int|bool|struct typec_thunderbolt_switch \*)\s*" + name
                      + r"\([^;]*?\)\n\{", source)
    if not match:
        raise ValueError(name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define IS_ERR_OR_NULL(p) (!(p))
#define READ_ONCE(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) ((struct delayed_work *)(p))
#define lockdep_assert_held(p) ((void)(p))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err_probe(d,e,...) (e)
static pthread_mutex_t *lock_mutex(pthread_mutex_t *p) {
    assert(!pthread_mutex_lock(p)); return p;
}
static void unlock_mutex(pthread_mutex_t **p) {
    if (*p) assert(!pthread_mutex_unlock(*p));
}
static pthread_rwlock_t *lock_rwsem_read(pthread_rwlock_t *p) {
    assert(!pthread_rwlock_rdlock(p)); return p;
}
static pthread_rwlock_t *lock_rwsem_write(pthread_rwlock_t *p) {
    assert(!pthread_rwlock_wrlock(p)); return p;
}
static void unlock_rwsem_read(pthread_rwlock_t **p) {
    if (*p) assert(!pthread_rwlock_unlock(*p));
}
static void unlock_rwsem_write(pthread_rwlock_t **p) {
    if (*p) assert(!pthread_rwlock_unlock(*p));
}
#define guard(n) __attribute__((cleanup(unlock_##n),unused)) __auto_type held_##n = lock_##n
#define scoped_guard(n,p) for (__attribute__((cleanup(unlock_##n))) __auto_type held_##n = lock_##n(p); held_##n; unlock_##n(&held_##n), held_##n = NULL)
'''

SWITCH = r'''
struct module { int puts, gets; bool unload; };
struct device { bool unregistered; void *driver; };
struct typec_thunderbolt_switch_data { int state; };
struct typec_thunderbolt_switch_dev {
    struct device dev;
    pthread_rwlock_t set_lock;
    struct module *owner;
    int (*set)(struct typec_thunderbolt_switch_dev *,
               const struct typec_thunderbolt_switch_data *);
};
struct typec_thunderbolt_switch { struct typec_thunderbolt_switch_dev *sw_dev; };
static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_cv = PTHREAD_COND_INITIALIZER;
static bool entered, leave;
static int calls, device_puts;
static void device_unregister(struct device *d) { d->unregistered = true; }
static void put_device(struct device *d) { (void)d; device_puts++; }
static void module_put(struct module *m) { m->puts++; }
#define kfree free
struct fwnode_handle { struct typec_thunderbolt_switch_dev *provider; };
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define ERR_CAST(p) (p)
#define kzalloc_obj(x) calloc(1, sizeof(x))
static int typec_thunderbolt_switch_match;
static struct typec_thunderbolt_switch_dev *getting;
static void *fwnode_connection_find_match(struct fwnode_handle *f, const char *s,
                                        void *x, int fn) {
    (void)s; (void)x; (void)fn; getting=f->provider; return getting;
}
static bool try_module_get(struct module *m) {
    /* Acquiring module ownership must serialize with callback revocation. */
    assert(pthread_rwlock_trywrlock(&getting->set_lock) == EBUSY);
    m->gets++; return !m->unload;
}
static int callback(struct typec_thunderbolt_switch_dev *sw,
                    const struct typec_thunderbolt_switch_data *data) {
    assert(!sw->dev.unregistered);
    pthread_mutex_lock(&callback_lock);
    calls++; entered = true; pthread_cond_broadcast(&callback_cv);
    while (!leave) pthread_cond_wait(&callback_cv, &callback_lock);
    pthread_mutex_unlock(&callback_lock);
    return data->state;
}
'''

SWITCH_MAIN = r'''
static void *call_thread(void *p) {
    struct typec_thunderbolt_switch_data data = { 17 };
    assert(typec_thunderbolt_switch_set(p, &data) == 17);
    return NULL;
}
static void *remove_thread(void *p) {
    typec_thunderbolt_switch_unregister(p); return NULL;
}
int main(void) {
    struct module owner = {0};
    struct typec_thunderbolt_switch_dev provider = { .owner = &owner, .set = callback };
    struct typec_thunderbolt_switch handle = { .sw_dev = &provider };
    struct typec_thunderbolt_switch_data data = { 0 };
    struct fwnode_handle node = { &provider };
    pthread_t caller, remover;
    pthread_rwlock_init(&provider.set_lock, NULL);
    struct typec_thunderbolt_switch *fresh = fwnode_typec_thunderbolt_switch_get(&node);
    assert(!IS_ERR(fresh) && fresh->sw_dev == &provider && owner.gets == 1);
    typec_thunderbolt_switch_put(fresh);
    owner.unload = true;
    assert(fwnode_typec_thunderbolt_switch_get(&node) == ERR_PTR(-ENODEV));
    owner.unload = false;
    assert(typec_thunderbolt_switch_set(NULL, &data) == 0);
    typec_thunderbolt_switch_unregister(NULL);
    pthread_create(&caller, NULL, call_thread, &handle);
    pthread_mutex_lock(&callback_lock);
    while (!entered) pthread_cond_wait(&callback_cv, &callback_lock);
    /* The executing callback holds the production read-side gate. */
    assert(pthread_rwlock_trywrlock(&provider.set_lock) == EBUSY);
    pthread_create(&remover, NULL, remove_thread, &provider);
    leave = true; pthread_cond_broadcast(&callback_cv);
    pthread_mutex_unlock(&callback_lock);
    pthread_join(caller, NULL); pthread_join(remover, NULL);
    assert(provider.dev.unregistered && provider.set == NULL && calls == 1);
    assert(typec_thunderbolt_switch_set(&handle, &data) == -ENODEV);
    assert(calls == 1);
    int gets = owner.gets;
    assert(fwnode_typec_thunderbolt_switch_get(&node) == ERR_PTR(-ENODEV));
    assert(owner.gets == gets);
    /* The parent driver is gone; dropping an old handle uses the cached owner. */
    provider.dev.driver = NULL;
    struct typec_thunderbolt_switch *allocated = malloc(sizeof(*allocated));
    *allocated = handle;
    typec_thunderbolt_switch_put(allocated);
    assert(owner.puts == 2 && device_puts == 4);
    puts("PASS callback drain, stale handle/get refusal, guarded module get/release");
    return 0;
}
'''

READINESS = r'''
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct hw_info { bool tunneled; };
struct apple_pcie_port { bool started, needs_stop, link_failed, enabled; struct apple_pcie_port *next; };
struct list_head { struct apple_pcie_port *first; };
struct apple_pcie { struct hw_info *hw; bool resume_failed, bus_stopped; struct list_head ports; };
struct pci_host_bridge { void *bus; struct apple_pcie pcie; };
struct device { pthread_mutex_t lock; bool bound; void *driver, *data; };
struct platform_device { struct device dev; };
struct pd_list { unsigned int num_pds; void *pd_links[4]; };
struct apple_cio {
    pthread_mutex_t lock, pcie_tunnel_lock;
    struct device *dev;
    void *pcie_tunnel_np, *tbt_switch, *nhi_pdev, *rtk;
    bool pcie_tunnel_preinitialized, pcie_tunnel_populated;
    bool pcie_tunnel_stopping, pcie_pm_prepared, pcie_quiesce_pending;
    bool pcie_tunnel_requested, pcie_resume_expected;
    unsigned int pcie_bind_retries;
    struct pd_list *pd_list;
    struct delayed_work pcie_tunnel_work;
};
#define APPLE_CIO_PCIE_BIND_RETRIES 20
#define APPLE_CIO_PCIE_BIND_WAIT_MS 250
#define list_for_each_entry(p,h,m) for (p=(h)->first;p;p=p->next)
#define list_empty(h) (!(h)->first)
static struct platform_device host;
static struct pci_host_bridge bridge;
static struct apple_pcie_port port;
static struct hw_info hw = { .tunneled = true };
static struct pd_list pd;
static int ready, fault, queued, refs, scans, populations, start_error, dart_error;
static bool host_exists;
static void mutex_lock(pthread_mutex_t *p) { pthread_mutex_lock(p); }
static void mutex_unlock(pthread_mutex_t *p) { pthread_mutex_unlock(p); }
static pthread_mutex_t *lock_device(struct device *d) { return lock_mutex(&d->lock); }
static void unlock_device(pthread_mutex_t **p) { unlock_mutex(p); }
static bool device_is_bound(struct device *d) { return d->bound; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *pci_host_bridge_priv(struct pci_host_bridge *b) { return &b->pcie; }
static void put_device(struct device *d) { (void)d; refs--; assert(refs>=0); }
static struct platform_device *apple_cio_find_pcie_tunnel(struct apple_cio *c) {
    if (!c->pcie_tunnel_populated || !host_exists) return NULL;
    refs++; return &host;
}
static int apple_pcie_tunnel_stop(struct apple_pcie_port *p) { p->needs_stop=false; return 0; }
static int apple_pcie_tunnel_start(struct apple_pcie_port *p) {
    if (!start_error) p->started=true; return start_error;
}
static void apple_pcie_port_enable_irq(struct apple_pcie_port *p) { p->enabled=true; }
static int apple_dart_resume_commands;
static int apple_pcie_walk_tunnel_darts(struct apple_pcie *p, int fn, bool x) {
    (void)p; (void)fn; (void)x; return dart_error;
}
static void apple_pcie_tunnel_wait_ready(struct apple_pcie *p) { (void)p; }
static void pci_lock_rescan_remove(void) { }
static void pci_unlock_rescan_remove(void) { }
static void pci_rescan_bus(void *b) { (void)b; assert(port.enabled); scans++; }
static int apple_pcie_tunnel_keep_d0;
static void pci_walk_bus(void *b, int fn, void *p) { (void)b; (void)fn; (void)p; }
static int apple_cio_populate_pcie_tunnel(struct apple_cio *c) { (void)c; populations++; return 0; }
static int apple_cio_quiesce_pcie_tunnel_locked(struct apple_cio *c) { c->pcie_quiesce_pending=false; return 0; }
static void *system_freezable_wq;
static int msecs_to_jiffies(int x) { return x; }
static void mod_delayed_work(void *q, struct delayed_work *w, int delay) {
    (void)q; (void)w; assert(delay==APPLE_CIO_PCIE_BIND_WAIT_MS); queued++;
}
static void typec_thunderbolt_switch_notify_ready(void *p) { (void)p; ready++; }
static void typec_thunderbolt_switch_notify(void *p) { (void)p; fault++; }
static bool apple_rtkit_is_crashed(void *p) { (void)p; return false; }
'''

READINESS_MAIN = r'''
static void reset(struct apple_cio *c) {
    static bool initialized;
    if (initialized) {
        assert(!pthread_mutex_destroy(&host.dev.lock));
        assert(!pthread_mutex_destroy(&c->pcie_tunnel_lock));
    }
    initialized=true;
    memset(c, 0, sizeof(*c)); memset(&host,0,sizeof(host));
    memset(&bridge,0,sizeof(bridge)); memset(&port,0,sizeof(port));
    ready=fault=queued=refs=scans=populations=start_error=dart_error=0;
    host_exists=true; pd.num_pds=4; pd.pd_links[3]=&pd;
    host.dev.bound=true; host.dev.driver=&hw; host.dev.data=&bridge;
    pthread_mutex_init(&host.dev.lock,NULL);
    pthread_mutex_init(&c->pcie_tunnel_lock,NULL);
    c->pcie_tunnel_np=&pd; c->pcie_tunnel_requested=true;
    c->pcie_tunnel_preinitialized=true; c->pcie_bind_retries=20;
    c->pd_list=&pd; c->nhi_pdev=&host;
    bridge.bus=&bridge; bridge.pcie.hw=&hw; bridge.pcie.ports.first=&port;
    port.started=true;
}
static void work(struct apple_cio *c) { apple_cio_pcie_tunnel_work(&c->pcie_tunnel_work.work); assert(refs==0); }
int main(void) {
    struct apple_cio c;
    reset(&c); host.dev.bound=false; host.dev.driver=NULL; host.dev.data=NULL;
    work(&c); assert(!ready && !fault && queued==1 && populations==1);
    assert(apple_cio_check_connection(&c)==-EAGAIN);
    host.dev.bound=true; host.dev.driver=&hw; host.dev.data=&bridge;
    work(&c); assert(ready==1 && !fault && populations==1);
    assert(apple_cio_check_connection(&c)==0);
    reset(&c); host_exists=false;
    for (int i=0;i<21;i++) work(&c);
    assert(!ready && fault==1 && queued==20 && c.pcie_bind_retries==0);
    assert(apple_cio_check_connection(&c)==-ENODEV);
    reset(&c); port.link_failed=true; work(&c);
    assert(!ready && fault==1 && !queued);
    reset(&c); bridge.pcie.resume_failed=true; work(&c);
    assert(!ready && fault==1 && !queued);
    reset(&c); c.pcie_pm_prepared=true; work(&c);
    assert(!ready && !fault && !queued && !populations);
    reset(&c); c.pcie_tunnel_stopping=true; work(&c);
    assert(!ready && !fault && !queued && !populations);
    reset(&c); c.pcie_tunnel_requested=false; work(&c);
    assert(!ready && !fault && !queued && !populations);
    reset(&c); bridge.pcie.bus_stopped=true; port.started=false;
    work(&c); assert(ready==1 && scans==1 && !bridge.pcie.bus_stopped);
    reset(&c); bridge.pcie.bus_stopped=true; port.started=false; start_error=-EIO;
    work(&c); assert(!ready && fault==1 && !port.enabled && !scans);
    reset(&c); bridge.pcie.bus_stopped=true; dart_error=-EIO;
    work(&c); assert(!ready && fault==1 && !port.enabled && !scans);
    reset(&c); c.pcie_tunnel_preinitialized=false; work(&c);
    assert(!ready && fault==1 && !populations);
    puts("PASS 12 bind, timeout, failure, PM, cancellation and restore scenarios");
    return 0;
}
'''


IRQ = r'''
struct apple_pcie_port {
    int irq;
    void *domain;
    pthread_mutex_t irq_lock;
    bool irq_disabled, clock_live, parent_masked;
    unsigned long status, mask;
};
struct irq_chip { int unused; };
struct irq_desc { struct apple_pcie_port *port; struct irq_chip chip; };
static struct apple_pcie_port *irq_port;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cv = PTHREAD_COND_INITIALIZER;
static bool pause_read, in_read, let_read, mask_seen;
static int reads, dispatched, enters, exits;
#define PORT_INTSTAT 0
#define PORT_INTMSK 1
#define raw_spin_lock(p) assert(!pthread_mutex_lock(p))
#define raw_spin_unlock(p) assert(!pthread_mutex_unlock(p))
#define raw_spin_lock_irqsave(p,f) do { (f)=0; raw_spin_lock(p); } while (0)
#define raw_spin_unlock_irqrestore(p,f) do { (void)(f); raw_spin_unlock(p); } while (0)
#define for_each_set_bit(i,p,n) for (i=0;i<(n);i++) if (*(p)&(1ul<<i))
static struct apple_pcie_port *irq_desc_get_handler_data(struct irq_desc *d) { return d->port; }
static struct irq_chip *irq_desc_get_chip(struct irq_desc *d) { return &d->chip; }
static void chained_irq_enter(struct irq_chip *c, struct irq_desc *d) { (void)c; (void)d; enters++; }
static void chained_irq_exit(struct irq_chip *c, struct irq_desc *d) { (void)c; (void)d; exits++; }
static unsigned long apple_pcie_port_readl(struct apple_pcie_port *p, int reg) {
    assert(p->clock_live); reads++;
    if (reg==PORT_INTSTAT && pause_read) {
        pthread_mutex_lock(&event_lock);
        in_read=true; pthread_cond_broadcast(&event_cv);
        while (!let_read) pthread_cond_wait(&event_cv,&event_lock);
        pthread_mutex_unlock(&event_lock);
    }
    return reg==PORT_INTSTAT ? p->status : p->mask;
}
static void generic_handle_domain_irq(void *d, int bit) {
    assert(d && bit==2); dispatched++;
}
static void disable_irq_nosync(int irq) {
    assert(irq==irq_port->irq);
    pthread_mutex_lock(&event_lock);
    irq_port->parent_masked=true; mask_seen=true;
    pthread_cond_broadcast(&event_cv);
    pthread_mutex_unlock(&event_lock);
}
static void enable_irq(int irq) {
    assert(irq==irq_port->irq && !irq_port->irq_disabled && irq_port->clock_live);
    irq_port->parent_masked=false;
}
'''
IRQ_MAIN = r'''
static void *handler(void *p) { apple_port_irq_handler(p); return NULL; }
static void *disable(void *p) {
    apple_pcie_port_disable_irq(p);
    ((struct apple_pcie_port *)p)->clock_live=false;
    return NULL;
}
int main(void) {
    struct apple_pcie_port port = { .irq=1, .domain=&port, .clock_live=true,
                                   .status=(1ul<<2)|(1ul<<3), .mask=1ul<<3 };
    struct irq_desc desc = { .port=&port };
    pthread_t running, stopper;
    irq_port=&port; pthread_mutex_init(&port.irq_lock,NULL);
    apple_port_irq_handler(&desc);
    assert(reads==2 && dispatched==1 && enters==exits);
    pause_read=true;
    pthread_create(&running,NULL,handler,&desc);
    pthread_mutex_lock(&event_lock);
    while (!in_read) pthread_cond_wait(&event_cv,&event_lock);
    /* The real handler holds the drain lock while touching MMIO. */
    assert(pthread_mutex_trylock(&port.irq_lock)==EBUSY);
    pthread_create(&stopper,NULL,disable,&port);
    while (!mask_seen) pthread_cond_wait(&event_cv,&event_lock);
    assert(port.parent_masked && port.clock_live);
    let_read=true; pthread_cond_broadcast(&event_cv);
    pthread_mutex_unlock(&event_lock);
    pthread_join(running,NULL); pthread_join(stopper,NULL);
    assert(port.irq_disabled && !port.clock_live && port.parent_masked);
    int before=reads;
    apple_port_irq_handler(&desc);
    assert(reads==before && enters==exits);
    apple_pcie_port_disable_irq(&port);
    port.clock_live=true; pause_read=false;
    apple_pcie_port_enable_irq(&port);
    assert(!port.irq_disabled && !port.parent_masked);
    apple_port_irq_handler(&desc);
    assert(reads==before+2 && dispatched==3 && enters==exits);
    puts("PASS masked events, running-handler drain, gated MMIO and restore ordering");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    paths = ['drivers/usb/typec/mux.c', 'drivers/pci/controller/pcie-apple.c',
             'drivers/thunderbolt/apple.c']
    mux, pci, cio = [(args.source / p).read_text() for p in paths]
    switch = COMMON + SWITCH + '\n'.join(function(mux, n) for n in
        ['typec_thunderbolt_switch_set', 'typec_thunderbolt_switch_unregister',
         'typec_thunderbolt_switch_put', 'fwnode_typec_thunderbolt_switch_get']) + SWITCH_MAIN
    readiness = COMMON + READINESS + '\n'.join([
        function(pci, 'apple_pcie_tunnel_restore'),
        function(pci, 'apple_pcie_tunnel_check_state'),
        function(cio, 'apple_cio_pcie_tunnel_ready_locked'),
        function(cio, 'apple_cio_activate_pcie_tunnel_locked'),
        function(cio, 'apple_cio_pcie_tunnel_work'),
        function(cio, 'apple_cio_check_connection')]) + READINESS_MAIN
    irq = COMMON + IRQ + '\n'.join(function(pci, n) for n in
        ['apple_port_irq_handler', 'apple_pcie_port_disable_irq',
         'apple_pcie_port_enable_irq']) + IRQ_MAIN
    cases = [('switch-lifetime', switch, True), ('host-readiness', readiness, True),
             ('irq-clock-gate', irq, True)]
    if args.negative_controls:
        cases.extend([
            ('disabled-mmio', irq.replace('if (port->irq_disabled)\n\t\tgoto out;',
                                         'if (false) goto out;'), False),
            ('ignored-event-mask', irq.replace('stat &= ~apple_pcie_port_readl(port, PORT_INTMSK);', '(void)stat;'), False),
            ('stale-callback', switch.replace('sw_dev->set = NULL;',
                                              '(void)sw_dev;'), False),
            ('undrained-callback', switch.replace(
                'guard(rwsem_read)(&sw->sw_dev->set_lock);', ''), False),
            ('unguarded-module-get', switch.replace(
                'scoped_guard(rwsem_read, &sw_dev->set_lock)', 'if (true)'), False),
            ('premature-ready', readiness.replace(
                'return apple_cio_pcie_tunnel_ready_locked(acio);', 'return 0;'), False),
            ('unbounded-bind', readiness.replace('acio->pcie_bind_retries--;', ''), False),
            ('early-irq-open', readiness.replace(
                'if (ret)\n\t\treturn ret;\n\n\tapple_pcie_tunnel_wait_ready(pcie);',
                '(void)ret;\n\n\tapple_pcie_tunnel_wait_ready(pcie);'), False)])
    results = []
    with tempfile.TemporaryDirectory(prefix='pr14-host-') as tmp:
        tmp = Path(tmp)
        for name, source, expected_pass in cases:
            path = tmp / (name + '.c')
            path.write_text(source)
            exe = tmp / name
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-Wno-misleading-indentation',
                            '-fsanitize=undefined', str(path), '-o', str(exe)],
                           check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    timeout=15)
            if expected_pass != (result.returncode == 0):
                raise AssertionError((name, result.returncode, result.stderr))
            results.append({'name': name, 'expected_pass': expected_pass,
                            'exit': result.returncode,
                            'output': result.stdout.strip(),
                            'fixture_sha256': hashlib.sha256(source.encode()).hexdigest()})
    print(json.dumps({'source_sha256': {p: hashlib.sha256((args.source / p).read_bytes()).hexdigest()
                                      for p in paths}, 'controls': results}, indent=2))


if __name__ == '__main__':
    main()
