#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise PCIe-C admission and s2idle domain-hold policy using production C."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = re.search(r'^(?:static\s+)?(?:bool|int|u16|void)\s+' + name + r'\([^{;]*\)\s*\{',
                      source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
struct device_node {
    const char *compat[3];
    bool kernel_init, tunables;
    int status, refs;
    struct device_node *dart, *child;
};
struct of_device_id { const char *compatible; const void *data; };
struct of_phandle_args { struct device_node *np; };
static int maps;
static bool of_device_is_compatible(const struct device_node *np, const char *compat)
{
    for (int i = 0; np && i < 3 && np->compat[i]; i++)
        if (!strcmp(np->compat[i], compat))
            return true;
    return false;
}
static const struct of_device_id *of_match_node(const struct of_device_id *ids,
                                                struct device_node *np)
{
    for (int i = 0; np && i < 3 && np->compat[i]; i++)
        for (const struct of_device_id *id = ids; id->compatible; id++)
            if (!strcmp(np->compat[i], id->compatible))
                return id;
    return NULL;
}
static bool of_property_read_bool(struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,pciec-kernel-init"));
    return np->kernel_init;
}
static bool of_property_present(struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,tunable"));
    return np->tunables;
}
static int of_property_read_u32(struct device_node *np, const char *name, u32 *v)
{
    assert(!strcmp(name, "apple,pciec-preinit-status"));
    if (np->status < 0)
        return -1;
    *v = np->status;
    return 0;
}
static int of_map_iommu_id(struct device_node *np, int rid, struct of_phandle_args *a)
{
    assert(rid == 0);
    maps++;
    if (!np->dart)
        return -1;
    a->np = np->dart;
    a->np->refs++;
    return 0;
}
static void of_node_put(struct device_node *np) { assert(np->refs > 0); np->refs--; }
#define pr_warn_once(...) ((void)0)
#define for_each_available_child_of_node_scoped(parent, child) \
    for (struct device_node *child = (parent)->child; child; child = NULL)
/* PRODUCTION */
static void expect(struct device_node *host, bool cold, bool admitted)
{
    struct device_node parent = { .child = host };
    maps = 0;
    assert(apple_pcie_tunnel_needs_cold_init(host) == cold);
    assert(apple_cio_pcie_tunnel_is_preinitialized(&parent) == admitted);
    assert(!host->dart || host->dart->refs == 0);
    if (!host->kernel_init || host->status == 1)
        assert(maps == 0);
}
int main(void)
{
    struct device_node dart = { .tunables = true };
    const char *intr2axi[] = { "apple,t8112-pciec", "apple,t6000-pciec",
                              "apple,t8142-pciec" };
    for (unsigned int i = 0; i < 3; i++) {
        /* M2 must win over its T8103 fallback, without a board allowlist. */
        struct device_node host = { .compat = { intr2axi[i], i < 2 ? "apple,t8103-pciec" : NULL },
                                   .status = -1, .dart = &dart };
        const struct of_device_id *id = of_match_node(apple_pcie_of_match, &host);
        assert(id && ((const struct hw_info *)id->data)->tunnel_intr2axi);
        expect(&host, false, false);  /* No implicit cold reset. */
        host.kernel_init = true;
        expect(&host, true, true);
        dart.tunables = false;
        expect(&host, false, false); /* Do not let the fallback bypass DART admission. */
        host.status = 1;
        expect(&host, false, true);  /* Preserve a firmware handoff. */
        host.status = 0;
        expect(&host, false, false);
        dart.tunables = true;
        expect(&host, true, true);  /* Failed/missing handoff can be initialized. */
        host.dart = NULL;
        expect(&host, false, false);
    }
    struct device_node m1 = { .compat = { "apple,t8103-pciec" }, .status = -1 };
    expect(&m1, false, false);
    m1.kernel_init = true;
    expect(&m1, true, true);        /* Original M1 does not need T8110 DART tunables. */
    m1.status = 1;
    expect(&m1, false, true);
    struct device_node regular = { .compat = { "apple,pcie" }, .kernel_init = true,
                                  .status = -1 };
    expect(&regular, false, false);
    regular.compat[0] = "apple,unknown-pciec";
    expect(&regular, false, false);
    puts("PASS: M1, M1 Pro, M2, native T8142, firmware handoff, missing DART and DT opt-in");
    return 0;
}
'''

S2IDLE_FIXTURE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_GET(mask, reg) (((reg) & (mask)) >> __builtin_ctz(mask))
#define U16_MAX ((u16)~0U)
#define U32_MAX ((u32)~0U)
#define READ_ONCE(x) (x)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define GFP_ATOMIC 0
#define kmalloc_obj(x, gfp) calloc(1, sizeof(x))
#define kfree(p) free(p)
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define GENPD_NOTIFY_PRE_OFF 0
#define GENPD_NOTIFY_ON 1
#define PM_SUSPEND_ON 0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
#define PCI_VENDOR_ID           0x00
#define PCI_EXP_LNKCTL          0x10
#define PCI_EXP_LNKCTL_ASPM_L0S 0x0001
#define PCI_EXP_LNKCTL_ASPM_L1  0x0002
#define PCI_EXP_LNKCTL_ASPMC    0x0003
#define PCI_L1SS_CTL1           0x08
#define PCI_L1SS_CTL2           0x0c
#define PCI_L1SS_CTL1_PCIPM_L1_2 0x00000001
#define PCI_L1SS_CTL1_PCIPM_L1_1 0x00000002
#define PCI_L1SS_CTL1_ASPM_L1_2  0x00000004
#define PCI_L1SS_CTL1_ASPM_L1_1  0x00000008
#define PCI_L1SS_CTL1_L1_2_MASK  0x00000005
#define PCI_L1SS_CTL1_L1SS_MASK  0x0000000f
typedef int pci_power_t;
#define PCI_D0          0
#define PCI_D3hot       3
#define PCI_D3cold      4
#define BUS_NOTIFY_DEL_DEVICE 0x0002
#define NOTIFY_DONE     0x0000
#define pci_fixup_resume_early 0
#define PCIE_LINK_STATE_L0S_UP     BIT(0)
#define PCIE_LINK_STATE_L0S_DW     BIT(1)
#define PCIE_LINK_STATE_L0S        (PCIE_LINK_STATE_L0S_UP | PCIE_LINK_STATE_L0S_DW)
#define PCIE_LINK_STATE_L1         BIT(2)
#define PCIE_LINK_STATE_L1_1       BIT(3)
#define PCIE_LINK_STATE_L1_2       BIT(4)
#define PCIE_LINK_STATE_L1_1_PCIPM BIT(5)
#define PCIE_LINK_STATE_L1_2_PCIPM BIT(6)
#define PCIE_LINK_STATE_L1_SS_PCIPM (PCIE_LINK_STATE_L1_1_PCIPM | PCIE_LINK_STATE_L1_2_PCIPM)
#define PCIE_LINK_STATE_L1SS       (PCIE_LINK_STATE_L1_1 | PCIE_LINK_STATE_L1_1_PCIPM | \
                                    PCIE_LINK_STATE_L1_2 | PCIE_LINK_STATE_L1_2_PCIPM)
static inline int notifier_from_errno(int err)
{
    return err ? (NOTIFY_STOP_MASK | (NOTIFY_OK - err)) : NOTIFY_OK;
}
static inline int notifier_to_errno(int ret)
{
    ret &= ~NOTIFY_STOP_MASK;
    return ret > NOTIFY_OK ? NOTIFY_OK - ret : 0;
}
struct of_device_id { const char *compatible; const void *data; };
struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb,
                             unsigned long action, void *data);
struct notifier_block { notifier_fn_t notifier_call; };
struct device;
struct dev_pm_ops {
    int (*runtime_resume)(struct device *dev);
};
struct device_driver {
    const struct dev_pm_ops *pm;
};
struct device {
    struct apple_pcie *pcie;
    struct notifier_block *genpd_nb;
    const struct device_driver *driver;
    bool genpd_on;
    int genpd_add_ret;
    unsigned int add_calls, remove_calls;
};
struct apple_pcie_pd {
    struct device *dev;
    struct notifier_block nb;
    unsigned int vetoed;
};
struct apple_dart_cio { int dummy; };
struct hw_info;
struct list_head {
    struct list_head *next, *prev;
};
#define list_for_each_entry(pos, head, member)     for (pos = container_of((head)->next, typeof(*pos), member);          &pos->member != (head);          pos = container_of(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member)     for (pos = container_of((head)->next, typeof(*pos), member),          n = container_of(pos->member.next, typeof(*pos), member);          &pos->member != (head);          pos = n, n = container_of(n->member.next, typeof(*n), member))
static void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}
static bool list_empty(const struct list_head *head)
{
    return head->next == head;
}
static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    entry->next = head;
    entry->prev = head->prev;
    head->prev->next = entry;
    head->prev = entry;
}
static void list_del(struct list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = NULL;
    entry->prev = NULL;
}
struct mutex {
    bool held;
};
static void mutex_init(struct mutex *lock)
{
    lock->held = false;
}
struct apple_pcie_port {
    struct list_head entry;
    unsigned int idx;
    bool link_failed;
    u8 regs[0x900];
};
struct pci_host_bridge;
struct apple_pcie {
    struct device *dev;
    const struct hw_info *hw;
    u8 *base;
    struct list_head ports;
    struct list_head hib_l1ss;
    struct list_head hib_l1ss_pending;
    struct mutex hib_l1ss_lock;
    struct notifier_block hib_l1ss_pci_nb;
    struct pci_host_bridge *bridge;
    bool power_retained, kernel_init, tunnel_cold_init;
    bool bus_stopped, resume_failed, link_kept;
    struct apple_pcie_pd *pds;
    unsigned int num_pds, pd_held;
    struct apple_dart_cio *cio_dart;
    bool cio_prepared, neo_sleep_armed, t6050_down;
    unsigned int managed_sids;
    void *t6050_port, *t6050_dart, *piodma_supplier;
    u32 t6050_parked;
};
static bool s2idle_keep_link = true;
static int pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
static bool suspend_via_firmware;
static bool link_healthy = true;
static unsigned int radio_prepare_calls, radio_complete_calls, warn_calls;
static u32 readl(const void *addr)
{
    u32 v;

    memcpy(&v, addr, sizeof(v));
    return v;
}
static void writel(u32 v, void *addr)
{
    memcpy(addr, &v, sizeof(v));
}
static u32 apple_pcie_port_readl(struct apple_pcie_port *port, u32 reg)
{
    assert(reg + sizeof(u32) <= sizeof(port->regs));
    return readl(port->regs + reg);
}
static bool pm_suspend_no_platform(void)
{
    return pm_suspend_target_state == PM_SUSPEND_TO_IDLE;
}
static bool pm_suspend_via_firmware(void)
{
    return suspend_via_firmware;
}
static struct apple_pcie *apple_pcie_lookup(struct device *dev)
{
    return dev ? dev->pcie : NULL;
}
static bool apple_pcie_tunnel_link_healthy(struct apple_pcie *pcie)
{
    (void)pcie;
    return link_healthy;
}
static int apple_radio_prepare(struct device *dev)
{
    (void)dev;
    radio_prepare_calls++;
    return 0;
}
static void apple_radio_complete(struct device *dev)
{
    (void)dev;
    radio_complete_calls++;
}
struct pci_host_bridge;
struct pci_bus {
    struct list_head devices;
    struct pci_dev *self;
    struct pci_host_bridge *bridge;
};
struct pcie_link_state;
struct pci_dev {
    struct list_head bus_list;
    struct pci_bus *bus;
    struct pci_bus *subordinate;
    struct pcie_link_state *link_state;
    struct device dev;
    void *driver;
    pci_power_t current_state;
    u8 devfn;
    u16 l1ss;
    u16 lnkctl;
    bool downstream_port;
    bool disconnected;
    bool power_up_fail;
    unsigned int refs;
    unsigned int restore_calls;
    u32 cfg[128];
    u32 saved_l1ss_ctl1;
    unsigned int lnkctl_write_calls;
    unsigned int fail_lnkctl_write_on_call;
    unsigned int cfg_write_calls;
    unsigned int fail_cfg_write_on_call;
};
#define to_pci_dev(d) container_of(d, struct pci_dev, dev)
static int pci_bus_sem;
static struct mutex aspm_lock;
static int pci_bus_sem_depth;
static bool aspm_disabled;
static void down_read(int *sem)
{
    assert(sem == &pci_bus_sem);
    assert(pci_bus_sem_depth == 0);
    pci_bus_sem_depth++;
}
static void up_read(int *sem)
{
    assert(sem == &pci_bus_sem);
    assert(pci_bus_sem_depth == 1);
    pci_bus_sem_depth--;
}
static void lockdep_assert_held_read(int *sem)
{
    assert(sem == &pci_bus_sem && pci_bus_sem_depth > 0);
}
static void (*aspm_lock_pre_hook)(void);
static void (*mutex_unlock_hook)(struct mutex *lock);
static void (*cfg_write_hook)(struct pci_dev *pdev);
static void mutex_lock(struct mutex *lock)
{
    if (lock == &aspm_lock && aspm_lock_pre_hook) {
        void (*hook)(void) = aspm_lock_pre_hook;

        aspm_lock_pre_hook = NULL;
        hook();
    }
    assert(!lock->held);
    lock->held = true;
}
static void mutex_unlock(struct mutex *lock)
{
    assert(lock->held);
    lock->held = false;
    if (mutex_unlock_hook)
        mutex_unlock_hook(lock);
}
/* Single-threaded workqueue: work runs only when the test drains it. */
struct work_struct {
    void (*func)(struct work_struct *work);
    struct work_struct *next;
};
struct workqueue_struct { int unused; };
static struct workqueue_struct system_dfl_wq_obj;
#define system_dfl_wq (&system_dfl_wq_obj)
#define INIT_WORK(w, f) do { (w)->func = (f); (w)->next = NULL; } while (0)
static struct work_struct *work_head, *work_tail;
static unsigned int queued_works;
static bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    assert(wq == system_dfl_wq);
    work->next = NULL;
    if (work_tail)
        work_tail->next = work;
    else
        work_head = work;
    work_tail = work;
    queued_works++;
    return true;
}
static void run_queued_work(void)
{
    /* Work items run from process context with no PCI lock held. */
    assert(pci_bus_sem_depth == 0 && !aspm_lock.held);
    while (work_head) {
        struct work_struct *work = work_head;

        work_head = work->next;
        if (!work_head)
            work_tail = NULL;
        work->func(work);
    }
}
struct pci_host_bridge {
    struct pci_bus *bus;
    void *priv;
    void (*pm_state_change)(struct pci_host_bridge *bridge, struct pci_dev *dev);
};
static struct pci_host_bridge *pci_host_bridge_from_priv(struct apple_pcie *pcie)
{
    return pcie->bridge;
}
static void *pci_host_bridge_priv(struct pci_host_bridge *bridge)
{
    return bridge->priv;
}
static struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
    while (bus && bus->self && bus->self->bus)
        bus = bus->self->bus;
    return bus ? bus->bridge : NULL;
}
static bool pci_dev_is_disconnected(const struct pci_dev *pdev)
{
    return pdev->disconnected;
}
static bool pcie_downstream_port(const struct pci_dev *pdev)
{
    return pdev->downstream_port;
}
static struct pci_dev *pci_dev_get(struct pci_dev *pdev)
{
    if (pdev)
        pdev->refs++;
    return pdev;
}
static void pci_dev_put(struct pci_dev *pdev)
{
    if (pdev) {
        assert(pdev->refs > 0);
        pdev->refs--;
    }
}
static int pcibios_err_to_errno(int err)
{
    return err;
}
static int pci_read_config_dword(struct pci_dev *pdev, int pos, u32 *val)
{
    assert((pos & 3) == 0 && (size_t)(pos >> 2) < ARRAY_SIZE(pdev->cfg));
    *val = pdev->cfg[pos >> 2];
    return 0;
}
static int pci_write_config_dword(struct pci_dev *pdev, int pos, u32 val)
{
    if (cfg_write_hook) {
        void (*hook)(struct pci_dev *) = cfg_write_hook;

        cfg_write_hook = NULL;
        hook(pdev);
    }
    assert((pos & 3) == 0 && (size_t)(pos >> 2) < ARRAY_SIZE(pdev->cfg));
    pdev->cfg_write_calls++;
    if (pdev->fail_cfg_write_on_call &&
        pdev->cfg_write_calls == pdev->fail_cfg_write_on_call)
        return -EIO;
    pdev->cfg[pos >> 2] = val;
    return 0;
}
static int pcie_capability_read_word(struct pci_dev *pdev, int pos, u16 *val)
{
    assert(pos == PCI_EXP_LNKCTL);
    *val = pdev->lnkctl;
    return 0;
}
static int pcie_capability_write_word(struct pci_dev *pdev, int pos, u16 val)
{
    assert(pos == PCI_EXP_LNKCTL);
    pdev->lnkctl_write_calls++;
    if (pdev->fail_lnkctl_write_on_call &&
        pdev->lnkctl_write_calls == pdev->fail_lnkctl_write_on_call)
        return -EIO;
    pdev->lnkctl = val;
    return 0;
}
static int pci_clear_and_set_config_dword(struct pci_dev *pdev, int pos,
                                          u32 clear, u32 set)
{
    u32 val;
    int ret = pci_read_config_dword(pdev, pos, &val);

    if (ret)
        return ret;
    val = (val & ~clear) | set;
    return pci_write_config_dword(pdev, pos, val);
}
static int pcie_capability_clear_and_set_word(struct pci_dev *pdev, int pos,
                                              u16 clear, u16 set)
{
    u16 val;
    int ret = pcie_capability_read_word(pdev, pos, &val);

    if (ret)
        return ret;
    val = (val & ~clear) | set;
    return pcie_capability_write_word(pdev, pos, val);
}
static void pci_save_aspm_l1ss_state(struct pci_dev *pdev)
{
    struct pci_dev *parent = pdev && pdev->bus ? pdev->bus->self : NULL;

    if (!pdev || pdev->downstream_port || !parent || !pdev->l1ss || !parent->l1ss)
        return;
    pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1,
                          &pdev->saved_l1ss_ctl1);
    pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
                          &parent->saved_l1ss_ctl1);
}
static void pci_update_aspm_saved_state(struct pci_dev *pdev)
{
    (void)pdev;
}
static void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev)
{
    if (!pci_dev->power_up_fail)
        pci_dev->current_state = PCI_D0;
    else
        pci_dev->current_state = PCI_D3cold;
}
static void pci_restore_state(struct pci_dev *pci_dev)
{
    struct pci_dev *parent = pci_dev->bus ? pci_dev->bus->self : NULL;

    pci_dev->restore_calls++;
    if (!pci_dev->downstream_port && parent && pci_dev->l1ss && parent->l1ss) {
        pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
                               parent->saved_l1ss_ctl1);
        pci_write_config_dword(pci_dev, pci_dev->l1ss + PCI_L1SS_CTL1,
                               pci_dev->saved_l1ss_ctl1);
    }
}
static void pci_pme_restore(struct pci_dev *pci_dev) { (void)pci_dev; }
static void pci_resume_ptm(struct pci_dev *pci_dev) { (void)pci_dev; }
static void pci_fixup_device(int pass, struct pci_dev *pci_dev) { (void)pass; (void)pci_dev; }
static void pci_pm_default_resume(struct pci_dev *pci_dev) { (void)pci_dev; }
static void pci_pm_bridge_power_up_actions(struct pci_dev *pci_dev) { (void)pci_dev; }
static void pci_warn(const struct pci_dev *pdev, const char *fmt, ...)
{
    (void)pdev;
    (void)fmt;
    warn_calls++;
}
static bool pci_walk_bus_rec(struct pci_bus *top,
                             int (*cb)(struct pci_dev *, void *), void *userdata)
{
    struct pci_dev *dev;

    list_for_each_entry(dev, &top->devices, bus_list) {
        if (cb(dev, userdata))
            return true;
        if (dev->subordinate && pci_walk_bus_rec(dev->subordinate, cb, userdata))
            return true;
    }
    return false;
}
static void pci_walk_bus(struct pci_bus *top,
                         int (*cb)(struct pci_dev *, void *), void *userdata)
{
    down_read(&pci_bus_sem);
    pci_walk_bus_rec(top, cb, userdata);
    up_read(&pci_bus_sem);
}
static int dev_pm_genpd_add_notifier(struct device *dev,
                                     struct notifier_block *nb)
{
    dev->add_calls++;
    if (dev->genpd_add_ret)
        return dev->genpd_add_ret;
    assert(!dev->genpd_nb);
    dev->genpd_nb = nb;
    return 0;
}
static int dev_pm_genpd_remove_notifier(struct device *dev)
{
    dev->remove_calls++;
    assert(dev->genpd_nb != NULL);
    dev->genpd_nb = NULL;
    return 0;
}
static bool dev_pm_genpd_is_on(struct device *dev) { return dev->genpd_on; }
static inline void dev_dbg(struct device *dev, const char *fmt, ...)
{
    (void)dev;
    (void)fmt;
}
static inline void dev_warn(struct device *dev, const char *fmt, ...)
{
    (void)dev;
    (void)fmt;
    warn_calls++;
}
/* PRODUCTION */
static const struct hw_info *match_compat(const char *compat)
{
    for (const struct of_device_id *id = apple_pcie_of_match;
         id->compatible; id++)
        if (!strcmp(id->compatible, compat))
            return id->data;
    return NULL;
}
static void reset_host(struct device *dev, struct apple_pcie *pcie,
                       struct apple_pcie_pd *pd, const struct hw_info *hw,
                       struct apple_dart_cio *cio_dart)
{
    memset(dev, 0, sizeof(*dev));
    memset(pcie, 0, sizeof(*pcie));
    memset(pd, 0, sizeof(*pd));
    dev->pcie = pcie;
    dev->genpd_on = true;
    pd->dev = dev;
    pcie->dev = dev;
    pcie->hw = hw;
    pcie->kernel_init = true;
    pcie->pds = pd;
    pcie->num_pds = 1;
    pcie->cio_dart = cio_dart;
    list_init(&pcie->ports);
    list_init(&pcie->hib_l1ss);
    list_init(&pcie->hib_l1ss_pending);
    mutex_init(&pcie->hib_l1ss_lock);
    s2idle_keep_link = true;
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
    suspend_via_firmware = false;
    link_healthy = true;
    radio_prepare_calls = 0;
    radio_complete_calls = 0;
    warn_calls = 0;
    aspm_disabled = false;
    pci_bus_sem_depth = 0;
    aspm_lock.held = false;
}
/* Concurrent events, injected at the point the real code would race. */
static struct pci_dev *race_pdev;
static struct pcie_link_state *race_link;
static struct apple_pcie *race_pcie;
static struct pci_dev *race_dev;
static bool race_blocked;
/* free_link_state() ran before this thread got aspm_lock. */
static void race_free_link_hook(void)
{
    memset(race_link, 0, sizeof(*race_link));
    race_pdev->link_state = NULL;
}
/* An endpoint reaches D0 mid-restore; its notification waits for hib_l1ss_lock. */
static void race_d0_cfg_hook(struct pci_dev *pdev)
{
    (void)pdev;
    race_dev->current_state = PCI_D0;
    if (race_pcie->hib_l1ss_lock.held)
        race_blocked = true;
    else
        apple_pcie_pm_state_change(race_pcie->bridge, race_dev);
}
static void race_unlock_hook(struct mutex *lock)
{
    if (race_blocked && lock == &race_pcie->hib_l1ss_lock) {
        race_blocked = false;
        apple_pcie_pm_state_change(race_pcie->bridge, race_dev);
    }
}
int main(void)
{
    struct apple_dart_cio cio_dart = { 1 };
    struct device dev;
    struct apple_pcie pcie;
    struct apple_pcie_pd pd;
    const struct hw_info *t6050 = match_compat("apple,t6050-pciec");
    const struct hw_info *t8132 = match_compat("apple,t8132-pciec");
    const struct hw_info *t6040 = match_compat("apple,t6040-pciec");
    const struct hw_info *t8103 = match_compat("apple,t8103-pciec");
    int nret;

    /* T6050 PCIe-C opts into s2idle_hold_domain like T8132 and T6040. */
    assert(t6050 && t6050->tunneled && t6050->s2idle_hold_domain);
    assert(t8132 && t8132->s2idle_hold_domain);
    assert(t6040 && t6040->s2idle_hold_domain);
    assert(t8103 && t8103->tunneled && !t8103->s2idle_hold_domain);

    /* Non-retained T6050 cio_dart host (tunnel_cold_init) holds domain in s2idle and releases. */
    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.kernel_init = false;
    pcie.tunnel_cold_init = true;
    assert(apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == 0);
    assert(radio_prepare_calls == 1 && warn_calls == 0);
    assert(apple_pcie_keep_link(&pcie));
    assert(pcie.pd_held == 1 && dev.genpd_nb == &pd.nb);
    assert(dev.add_calls == 1 && dev.remove_calls == 0);
    assert(dev.genpd_nb->notifier_call(dev.genpd_nb, GENPD_NOTIFY_ON, NULL) == NOTIFY_OK);
    assert(pd.vetoed == 0);
    nret = dev.genpd_nb->notifier_call(dev.genpd_nb, GENPD_NOTIFY_PRE_OFF, NULL);
    assert(notifier_to_errno(nret) == -EBUSY && pd.vetoed == 1);
    apple_pcie_release_domains(&pcie);
    assert(pcie.pd_held == 0 && dev.genpd_nb == NULL && dev.remove_calls == 1);

    /* Non-retained T8132 cio_dart host (kernel_init) also passes prepare and holds domain. */
    reset_host(&dev, &pcie, &pd, t8132, &cio_dart);
    assert(apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(apple_pcie_keep_link(&pcie) && pcie.pd_held == 1 && dev.add_calls == 1);
    apple_pcie_release_domains(&pcie);
    assert(pcie.pd_held == 0 && dev.genpd_nb == NULL && dev.remove_calls == 1);

    /* Domain already off or unhealthy link passes prepare and cleans up notifier in keep_link. */
    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.kernel_init = false;
    pcie.tunnel_cold_init = true;
    dev.genpd_on = false;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(!apple_pcie_keep_link(&pcie));
    assert(pcie.pd_held == 0 && dev.genpd_nb == NULL && dev.remove_calls == 1);

    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.kernel_init = false;
    pcie.tunnel_cold_init = true;
    link_healthy = false;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(!apple_pcie_keep_link(&pcie));
    assert(pcie.pd_held == 0 && dev.genpd_nb == NULL && dev.remove_calls == 1);

    /* Deep sleep is untouched: unprepared refuses early, prepared passes without holding. */
    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pm_suspend_target_state = PM_SUSPEND_MEM;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    pcie.cio_prepared = true;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.genpd_nb == NULL);

    /* PM_SUSPEND_ON (non-sleep) does not refuse in prepare even without cio_prepared. */
    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pm_suspend_target_state = PM_SUSPEND_ON;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    /* Every pre-suspend refusal term in apple_pcie_can_keep_link() refuses early in prepare. */
    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    s2idle_keep_link = false;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.kernel_init = false;
    pcie.tunnel_cold_init = false;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.bus_stopped = true;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    reset_host(&dev, &pcie, &pd, t6050, &cio_dart);
    pcie.resume_failed = true;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    /* Non-retained cio_dart host without s2idle_hold_domain refuses early in prepare. */
    reset_host(&dev, &pcie, &pd, t8103, &cio_dart);
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    /* Non-cio_dart host is not refused in prepare (noirq falls back to stop_for_sleep). */
    reset_host(&dev, &pcie, &pd, t8103, NULL);
    pcie.bus_stopped = true;
    assert(!apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(!apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    /* Retained power domain on a non-holding host passes prepare and keeps link without hold. */
    reset_host(&dev, &pcie, &pd, t8103, &cio_dart);
    pcie.power_retained = true;
    assert(apple_pcie_can_keep_link(&pcie));
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(apple_pcie_keep_link(&pcie) && pcie.pd_held == 0 && dev.add_calls == 0);

    /* Adopted T6050 host: s2idle and firmware-suspend refusals in prepare. */
    const struct hw_info *t6050_adopt = match_compat("apple,t6050-pcie");
    assert(t6050_adopt && t6050_adopt->adopt_link);

    u8 rc_regs[0x100];
    struct apple_pcie_port p0, p1;
    int dummy_port = 1, dummy_dart = 1;

    #define RESET_ADOPTED() do {         reset_host(&dev, &pcie, &pd, t6050_adopt, NULL);         memset(rc_regs, 0, sizeof(rc_regs));         pcie.base = rc_regs;         writel(CORE_RC_CTL_RUN, rc_regs + CORE_RC_CTL);         writel(CORE_RC_STAT_READY, rc_regs + CORE_RC_STAT);         memset(&p0, 0, sizeof(p0));         p0.idx = 0;         writel(PORT_STATUS_READY, p0.regs + PORT_STATUS);         writel(PORT_LINKSTS_UP, p0.regs + PORT_LINKSTS);         list_add_tail(&p0.entry, &pcie.ports);         memset(&p1, 0, sizeof(p1));         p1.idx = 1;         writel(PORT_STATUS_READY, p1.regs + PORT_STATUS);         writel(PORT_LINKSTS_UP, p1.regs + PORT_LINKSTS);         list_add_tail(&p1.entry, &pcie.ports);     } while (0)

    /* Healthy adopted host passes s2idle prepare. */
    RESET_ADOPTED();
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    /* Non-s2idle without firmware suspend refuses with -EBUSY in prepare. */
    RESET_ADOPTED();
    pm_suspend_target_state = PM_SUSPEND_MEM;
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);

    /* Root complex not running or not ready refuses with -ENOLINK. */
    RESET_ADOPTED();
    writel(0, rc_regs + CORE_RC_CTL);
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    writel(0, rc_regs + CORE_RC_STAT);
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    /* Unparked port link_failed / not ready / link down / interrupt refuses with -ENOLINK. */
    RESET_ADOPTED();
    p1.link_failed = true;
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    writel(0, p1.regs + PORT_STATUS);
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    writel(PORT_LINKSTS_UP | PORT_LINKSTS_BUSY, p1.regs + PORT_LINKSTS);
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    writel(BIT(PORT_INT_LINK_DOWN), p1.regs + PORT_INTSTAT);
    assert(apple_pcie_prepare(&dev) == -ENOLINK && radio_prepare_calls == 0 && warn_calls == 1);

    /* Parked port with dead link registers is skipped and passes prepare. */
    RESET_ADOPTED();
    p1.link_failed = true;
    writel(0, p1.regs + PORT_STATUS);
    writel(0, p1.regs + PORT_LINKSTS);
    writel(BIT(PORT_INT_LINK_DOWN), p1.regs + PORT_INTSTAT);
    pcie.t6050_parked = BIT(p1.idx);
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    /* Firmware suspend refuses without both t6050_port and t6050_dart, passes with both. */
    RESET_ADOPTED();
    pm_suspend_target_state = PM_SUSPEND_MEM;
    suspend_via_firmware = true;
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    pm_suspend_target_state = PM_SUSPEND_MEM;
    suspend_via_firmware = true;
    pcie.t6050_port = &dummy_port;
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    pm_suspend_target_state = PM_SUSPEND_MEM;
    suspend_via_firmware = true;
    pcie.t6050_dart = &dummy_dart;
    assert(apple_pcie_prepare(&dev) == -EBUSY && radio_prepare_calls == 0 && warn_calls == 1);

    RESET_ADOPTED();
    pm_suspend_target_state = PM_SUSPEND_MEM;
    suspend_via_firmware = true;
    pcie.t6050_port = &dummy_port;
    pcie.t6050_dart = &dummy_dart;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    /* Already-down, managed-radio, or NEO/CIO-prepared adopted host bypasses adopted check. */
    RESET_ADOPTED();
    writel(0, rc_regs + CORE_RC_CTL);
    pcie.t6050_down = true;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    RESET_ADOPTED();
    writel(0, rc_regs + CORE_RC_CTL);
    pcie.managed_sids = 1;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);

    #undef RESET_ADOPTED

    /* Conventional reinit host: L1SS disable/restore and one-shot ASPM restore write failure. */
    const struct hw_info *t8103_conv = match_compat("apple,pcie");
    assert(t8103_conv && !t8103_conv->tunneled && !t8103_conv->root_bus_only);

    struct pci_host_bridge bridge;
    struct pci_bus root_bus, sub0_bus, sub1_bus;
    struct pci_dev rp0, fn0, fn1, rp1, ep1;

    #define INIT_DEV(d, dfn, l1off, sub, dsp, vid, ctl1, ctl2, lctl) do { \
        memset(&(d), 0, sizeof(d)); \
        (d).devfn = (dfn); \
        (d).l1ss = (l1off); \
        (d).subordinate = (sub); \
        (d).downstream_port = (dsp); \
        (d).refs = 1; \
        (d).lnkctl = (lctl); \
        (d).cfg[PCI_VENDOR_ID >> 2] = (vid); \
        if (l1off) { \
            (d).cfg[((l1off) + PCI_L1SS_CTL1) >> 2] = (ctl1); \
            (d).cfg[((l1off) + PCI_L1SS_CTL2) >> 2] = (ctl2); \
        } \
    } while (0)

    #define RESET_L1SS(two_links) do { \
        reset_host(&dev, &pcie, &pd, t8103_conv, NULL); \
        pm_suspend_target_state = PM_SUSPEND_MEM; \
        list_init(&root_bus.devices); \
        list_init(&sub0_bus.devices); \
        list_init(&sub1_bus.devices); \
        root_bus.self = NULL; \
        root_bus.bridge = &bridge; \
        sub0_bus.self = &rp0; \
        sub0_bus.bridge = NULL; \
        sub1_bus.self = &rp1; \
        sub1_bus.bridge = NULL; \
        bridge.bus = &root_bus; \
        bridge.priv = &pcie; \
        bridge.pm_state_change = apple_pcie_pm_state_change; \
        pcie.bridge = &bridge; \
        pcie.hib_l1ss_pci_nb.notifier_call = apple_pcie_hib_l1ss_pci_notify; \
        INIT_DEV(rp0, 0x00, 0x150, &sub0_bus, true, 0x106b, 0x000000ff, 0x28, 0x0042); \
        INIT_DEV(fn0, 0x00, 0x160, NULL, false, 0x14e4, 0x000000ff, 0x28, 0x0042); \
        INIT_DEV(fn1, 0x01, 0, NULL, false, 0x14e4, 0, 0, 0x0043); \
        rp0.bus = &root_bus; \
        fn0.bus = &sub0_bus; \
        fn1.bus = &sub0_bus; \
        list_add_tail(&rp0.bus_list, &root_bus.devices); \
        list_add_tail(&fn0.bus_list, &sub0_bus.devices); \
        list_add_tail(&fn1.bus_list, &sub0_bus.devices); \
        if (two_links) { \
            INIT_DEV(rp1, 0x08, 0x150, &sub1_bus, true, 0x106b, 0x0000005f, 0x14, 0x0042); \
            INIT_DEV(ep1, 0x00, 0x160, NULL, false, 0x14e4, 0x0000005f, 0x14, 0x0042); \
            rp1.bus = &root_bus; \
            ep1.bus = &sub1_bus; \
            list_add_tail(&rp1.bus_list, &root_bus.devices); \
            list_add_tail(&ep1.bus_list, &sub1_bus.devices); \
        } \
    } while (0)

    /* s2idle leaves L1SS and Link Control untouched on a conventional reinit host. */
    RESET_L1SS(false);
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    apple_pcie_complete(&dev);
    assert(radio_complete_calls == 1);

    /* Deep sleep clears L1SS on prepare, preserves ASPM, and restores L1SS on complete. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    assert(!list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);
    assert(radio_complete_calls == 1 && warn_calls == 0);

    /* Normal completion preserves PCI core's resume-time ASPM policy instead of replaying saved bits. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    rp0.lnkctl = 0x0041;
    fn0.lnkctl = 0x0041;
    fn1.lnkctl = 0x0042;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.lnkctl == 0x0041 && fn0.lnkctl == 0x0041 && fn1.lnkctl == 0x0042);

    /* Normal completion also preserves ASPM disabled (0) set during resume. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0 && radio_prepare_calls == 1 && warn_calls == 0);
    rp0.lnkctl = 0x0040;
    fn0.lnkctl = 0x0040;
    fn1.lnkctl = 0x0040;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.lnkctl == 0x0040 && fn0.lnkctl == 0x0040 && fn1.lnkctl == 0x0040);

    /* One-shot failure on parent ASPM restore write: rollback must restore original ASPM and L1SS. */
    RESET_L1SS(false);
    rp0.fail_lnkctl_write_on_call = 2; /* 1 = ASPM clear succeeds, 2 = restore_aspm write fails once */
    assert(apple_pcie_prepare(&dev) == -EIO);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);
    assert(radio_complete_calls == 1 && warn_calls == 0);

    /* One-shot failure on downstream function ASPM restore write: rollback restores original ASPM. */
    RESET_L1SS(false);
    fn1.fail_lnkctl_write_on_call = 2; /* 1 = ASPM clear succeeds, 2 = restore_aspm write fails once */
    assert(apple_pcie_prepare(&dev) == -EIO);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);
    assert(radio_complete_calls == 1 && warn_calls == 0);

    /* Multi-link one-shot restore failure on second link unwinds both links cleanly. */
    RESET_L1SS(true);
    rp1.fail_lnkctl_write_on_call = 2;
    assert(apple_pcie_prepare(&dev) == -EIO);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp1.lnkctl == 0x0042 && ep1.lnkctl == 0x0042);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp1.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x0000005f);
    assert(ep1.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x0000005f);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1 && rp1.refs == 1 && ep1.refs == 1);
    assert(radio_complete_calls == 1 && warn_calls == 0);

    /* Unmanaged fallback: runtime-suspended D3hot endpoint syncs saved state, defers PCI-PM, and restores on real pci_pm_runtime_resume(). */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    rp0.saved_l1ss_ctl1 = 0x000000ff;
    fn0.saved_l1ss_ctl1 = 0x000000ff;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(rp0.saved_l1ss_ctl1 == 0x000000f0 && fn0.saved_l1ss_ctl1 == 0x000000f0);
    /* A PCI restore during the sleep window must not replay stale PCI-PM enables. */
    pci_restore_state(&fn0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(!list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(rp0.saved_l1ss_ctl1 == 0x000000fc && fn0.saved_l1ss_ctl1 == 0x000000fc);
    /* Real runtime resume restores masked config (0xfc) then applies deferred PCI-PM enables (0xff). */
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(fn0.current_state == PCI_D0);
    assert(list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.saved_l1ss_ctl1 == 0x000000ff && fn0.saved_l1ss_ctl1 == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* Unmanaged fallback across two sleeps in D3hot preserves original PCI-PM bits and cleans up on remove/del. */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending));
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending));
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Unmanaged pending record drops cleanly on device removal or host free without leaking refs. */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.refs == 2 && fn0.refs == 2);
    assert(apple_pcie_hib_l1ss_pci_notify(&pcie.hib_l1ss_pci_nb, BUS_NOTIFY_DEL_DEVICE, &fn0.dev) == NOTIFY_DONE);
    assert(list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.refs == 1 && fn0.refs == 1);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending));
    apple_pcie_hib_free_l1ss(&pcie);
    assert(list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Core-managed link: normal D0 sleep/resume preserves policy and syncs aspm_enabled. */
    struct pcie_link_state link0;
    #define ATTACH_LINK(l, up, dn, sup, en, def, dis) do { \
        memset(&(l), 0, sizeof(l)); \
        (l).pdev = (up); \
        (l).downstream = (dn); \
        (l).root = &(l); \
        (l).aspm_support = (sup); \
        (l).aspm_capable = (sup); \
        (l).aspm_enabled = (en); \
        (l).aspm_default = (def); \
        (l).aspm_disable = (dis); \
        (up)->link_state = &(l); \
    } while (0)
    #define ALL_L1_STATES (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1SS)

    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(link0.aspm_enabled == PCIE_LINK_STATE_L1);
    assert(link0.aspm_default == ALL_L1_STATES && link0.aspm_disable == 0);
    assert(rp0.saved_l1ss_ctl1 == 0x000000f0 && fn0.saved_l1ss_ctl1 == 0x000000f0);
    /* Mid-sleep D0 transition notification must keep L1SS suppressed until complete(). */
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0);
    assert(link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.saved_l1ss_ctl1 == 0x000000ff && fn0.saved_l1ss_ctl1 == 0x000000ff);

    /* Core-managed link: D3hot endpoint defers PCI-PM L1.1/L1.2 until endpoint enters D0. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.current_state = PCI_D3hot;
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0);
    assert(link0.aspm_enabled == (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_1 | PCIE_LINK_STATE_L1_2));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(rp0.saved_l1ss_ctl1 == 0x000000fc && fn0.saved_l1ss_ctl1 == 0x000000fc);
    /* Subsequent D3hot->D0 transition via real pci_pm_runtime_resume() enables deferred PCI-PM L1.1/L1.2. */
    fn0.power_up_fail = true;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(fn0.current_state == PCI_D3cold && queued_works == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    fn0.power_up_fail = false;
    fn0.restore_calls = 0;
    /*
     * Error recovery resumes devices inside pci_walk_bus(), which holds
     * pci_bus_sem for read. Runtime resume must not take it again: the
     * fixture's down_read() asserts on recursion.
     */
    pci_bus_sem_depth = 1;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    pci_bus_sem_depth = 0;
    assert(fn0.current_state == PCI_D0 && fn0.restore_calls == 1);
    /* The ASPM update is queued, pinning the port, and not yet applied. */
    assert(queued_works == 1 && rp0.refs == 2);
    assert(link0.aspm_enabled != ALL_L1_STATES);
    run_queued_work();
    assert(rp0.refs == 1 && fn0.refs == 1);
    assert(link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.saved_l1ss_ctl1 == 0x000000ff && fn0.saved_l1ss_ctl1 == 0x000000ff);

    /* Core-managed link starting with L1SS disabled still installs sleep mask against mid-sleep enable. */
    RESET_L1SS(false);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, PCIE_LINK_STATE_L1, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(!list_empty(&pcie.hib_l1ss));
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0);
    assert(link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Core-managed link: parent port not in D0 also defers PCI-PM L1.1/L1.2 until both in D0. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    rp0.current_state = PCI_D3hot;
    apple_pcie_complete(&dev);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    rp0.current_state = PCI_D0;
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Core-managed link: pre-existing aspm_disable bits stay disabled after resume. */
    RESET_L1SS(false);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000fe;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000fe;
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES,
                ALL_L1_STATES & ~PCIE_LINK_STATE_L1_2_PCIPM,
                ALL_L1_STATES, PCIE_LINK_STATE_L1_2_PCIPM);
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(link0.aspm_disable == PCIE_LINK_STATE_L1_2_PCIPM);
    assert(link0.aspm_enabled == (ALL_L1_STATES & ~PCIE_LINK_STATE_L1_2_PCIPM));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fe);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fe);
    assert(pci_bus_sem_depth == 0 && !aspm_lock.held && !pcie.hib_l1ss_lock.held);

    /* A link freed between queueing and running the work item is skipped, not used. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    queued_works = 0;
    fn0.current_state = PCI_D0;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(queued_works == 1 && rp0.refs == 2);
    race_pdev = &rp0;
    race_link = &link0;
    aspm_lock_pre_hook = race_free_link_hook;
    run_queued_work();
    assert(rp0.link_state == NULL && rp0.refs == 1 && !aspm_lock.held);
    assert(aspm_lock_pre_hook == NULL);

    /* Removal of the link between the unlocked hint and aspm_lock must not be dereferenced. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    race_pdev = &rp0;
    race_link = &link0;
    aspm_lock_pre_hook = race_free_link_hook;
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.link_state == NULL && aspm_lock_pre_hook == NULL);
    assert(pci_bus_sem_depth == 0 && !aspm_lock.held);
    /* Same with the caller already holding pci_bus_sem. */
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    pci_bus_sem_depth = 1;
    aspm_lock_pre_hook = race_free_link_hook;
    pcie_aspm_pm_state_change(&rp0, true);
    pci_bus_sem_depth = 0;
    assert(rp0.link_state == NULL && !aspm_lock.held);

    /*
     * Unmanaged link: an endpoint reaching D0 right after the restore masked
     * PCI-PM L1.1/L1.2 but before the pending record exists must still get
     * its enables. The notification blocks on hib_l1ss_lock, so it only runs
     * once the record is published.
     */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    race_pcie = &pcie;
    race_dev = &fn0;
    race_blocked = false;
    cfg_write_hook = race_d0_cfg_hook;
    mutex_unlock_hook = race_unlock_hook;
    apple_pcie_complete(&dev);
    mutex_unlock_hook = NULL;
    assert(cfg_write_hook == NULL && !race_blocked);
    assert(fn0.current_state == PCI_D0);
    assert(list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.saved_l1ss_ctl1 == 0x000000ff && fn0.saved_l1ss_ctl1 == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && !pcie.hib_l1ss_lock.held);

    /* Unmanaged link that is already in D0 at restore applies everything at once. */
    RESET_L1SS(false);
    aspm_disabled = true;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    #undef ALL_L1_STATES
    #undef ATTACH_LINK
    #undef RESET_L1SS
    #undef INIT_DEV

    puts("PASS: T6050 PCIe-C s2idle domain hold, release, deep sleep, L1SS rollback and prepare refusal");
    return 0;
}
'''


def run_cases(out, cases, extra_flags=()):
    for name, body in cases.items():
        source = out / (name + '.c')
        source.write_text(body)
        binary = out / name
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                        '-Wno-unused-function', *extra_flags,
                        str(source), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        if name.endswith('production'):
            assert run.returncode == 0, run.stderr
            print(run.stdout, end='')
        else:
            assert run.returncode < 0 and 'Assertion' in run.stderr, (
                name, run.returncode, run.stderr)
            print('PASS negative control:', name)


def pm_registration(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first) + len(end)]


PM_FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
struct notifier_block { int (*notifier_call)(void); int priority; };
struct apple_cio_hw { bool phy_shared_with_dwc3; };
struct apple_cio { const struct apple_cio_hw *hw; struct notifier_block pm_notifier; };
struct dwc3_apple { struct notifier_block pm_nb; };
static const struct apple_cio_hw apple_cio_t8142_hw = { .phy_shared_with_dwc3 = true };
static struct notifier_block *chain[2];
static int count;
static bool busy, phy_off, pci_off;
static int register_pm_notifier(struct notifier_block *nb)
{
    int i = count++;
    assert(count <= 2);
    while (i && chain[i - 1]->priority < nb->priority) {
        chain[i] = chain[i - 1];
        i--;
    }
    chain[i] = nb;
    return 0;
}
static int apple_cio_pm_notify(void)
{
    /* An open disk must be rejected while its PCIe link still works. */
    assert(!phy_off);
    if (busy)
        return 1;
    pci_off = true;
    return 0;
}
static int dwc3_apple_pm_notify(void)
{
    assert(pci_off);
    phy_off = true;
    return 0;
}
static void add_cio(struct apple_cio *acio)
{
    /* ACIO_REGISTRATION */
}
static void add_dwc(struct dwc3_apple *appledwc)
{
    int ret;
    /* DWC_REGISTRATION */
    assert(!ret);
}
int main(void)
{
    struct apple_cio acio = { .hw = &apple_cio_t8142_hw };
    struct dwc3_apple dwc = { 0 };
    /* DWC3 probes first on J813. Equal priority reproduces the failure. */
    add_dwc(&dwc);
    add_cio(&acio);
    for (int open = 0; open < 2; open++) {
        busy = open;
        phy_off = pci_off = false;
        for (int i = 0; i < count; i++)
            if (chain[i]->notifier_call())
                break;
        assert(phy_off == !busy && pci_off == !busy);
    }
    puts("PASS: T8142 refuses an open disk before PHY shutdown; idle PCIe stops first");
    return 0;
}
'''


def test_pm_order(out, acio):
    dwc = (ROOT / 'drivers/usb/dwc3/dwc3-apple.c').read_text()
    code = PM_FIXTURE.replace('/* ACIO_REGISTRATION */', pm_registration(
        acio, 'acio->pm_notifier.notifier_call = apple_cio_pm_notify;',
        'register_pm_notifier(&acio->pm_notifier);'))
    code = code.replace('/* DWC_REGISTRATION */', pm_registration(
        dwc, 'appledwc->pm_nb.notifier_call = dwc3_apple_pm_notify;',
        'ret = register_pm_notifier(&appledwc->pm_nb);'))
    mutation = code.replace('acio->pm_notifier.priority = 40;',
                            'acio->pm_notifier.priority = 0;')
    assert mutation != code
    for name, body in [('pm-order', code), ('phy-before-pcie', mutation)]:
        source, binary = out / (name + '.c'), out / name
        source.write_text(body)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', str(source),
                        '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        if name == 'pm-order':
            assert run.returncode == 0, run.stderr
            print(run.stdout, end='')
        else:
            assert run.returncode < 0 and 'Assertion' in run.stderr
            print('PASS negative control:', name)


T8122_SLEEP_FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct apple_dpin_policy { int unused; };
static const struct apple_dpin_policy apple_dpin_m1;
struct apple_cio_hw {
    bool sleep_removes_domains;
    bool restart_on_wake;
    bool phy_shared_with_dwc3;
    const struct apple_dpin_policy *dp;
};
struct notifier_block { int (*notifier_call)(void); int priority; };
struct apple_cio { const struct apple_cio_hw *hw; struct notifier_block pm_notifier; };
struct dwc3_apple { struct notifier_block pm_nb; };
struct match_entry { const char *compatible; const struct apple_cio_hw *data; };
static bool default_s2idle, no_platform;
static bool pm_suspend_default_s2idle(void) { return default_s2idle; }
static bool pm_suspend_no_platform(void) { return no_platform; }
/* PRODUCTION_HW */
static const struct match_entry match_table[] = {
/* MATCH_ENTRIES */
    { NULL, NULL },
};
/* PRODUCTION_FN */
static const struct apple_cio_hw *match_of(const char *const *compat)
{
    for (size_t i = 0; compat[i]; i++)
        for (size_t j = 0; match_table[j].compatible; j++)
            if (!strcmp(compat[i], match_table[j].compatible))
                return match_table[j].data;
    return NULL;
}
static struct notifier_block *chain[2];
static int count;
static bool phy_off, pci_off;
static int register_pm_notifier(struct notifier_block *nb)
{
    int i = count++;
    assert(count <= 2);
    while (i && chain[i - 1]->priority < nb->priority) {
        chain[i] = chain[i - 1];
        i--;
    }
    chain[i] = nb;
    return 0;
}
static int apple_cio_pm_notify(void)
{
    assert(!phy_off);
    pci_off = true;
    return 0;
}
static int dwc3_apple_pm_notify(void)
{
    assert(pci_off);
    phy_off = true;
    return 0;
}
static void add_cio(struct apple_cio *acio)
{
    /* ACIO_REGISTRATION */
}
static void add_dwc(struct dwc3_apple *appledwc)
{
    int ret;
    /* DWC_REGISTRATION */
    assert(!ret);
}
int main(void)
{
    static const char *const t8122_compat[] = {
        "apple,t8122-usb4-acio", "apple,t8103-usb4-acio", NULL
    };
    static const char *const t8103_compat[] = {
        "apple,t8103-usb4-acio", NULL
    };
    const struct apple_cio_hw *t8122 = match_of(t8122_compat);
    const struct apple_cio_hw *t8103 = match_of(t8103_compat);
    assert(t8122 == &apple_cio_t8122_hw);
    assert(t8103 == &apple_cio_t8103_hw);
    assert(t8122->sleep_removes_domains);
    assert(t8122->restart_on_wake);
    assert(t8122->phy_shared_with_dwc3);
    assert(t8122->dp == &apple_dpin_m1);

    struct apple_cio acio_t8122 = { .hw = t8122 };
    struct apple_cio acio_t8103 = { .hw = t8103 };

    /* mem_sleep=deep + mem tears down T8122 ACIO, leaves T8103 unchanged. */
    default_s2idle = false;
    no_platform = false;
    assert(apple_cio_sleep_removes_domains(&acio_t8122));
    assert(!apple_cio_sleep_removes_domains(&acio_t8103));

    /* s2idle keeps the port up. */
    default_s2idle = true;
    no_platform = false;
    assert(!apple_cio_sleep_removes_domains(&acio_t8122));

    /* freeze keeps the port up even when mem_sleep=deep is the default. */
    default_s2idle = false;
    no_platform = true;
    assert(!apple_cio_sleep_removes_domains(&acio_t8122));

    /* Shared ATC PHY: ACIO PM notifier must run before DWC3's. */
    struct dwc3_apple dwc = { 0 };
    add_dwc(&dwc);
    add_cio(&acio_t8122);
    for (int i = 0; i < count; i++)
        assert(!chain[i]->notifier_call());
    assert(pci_off && phy_off);

    puts("PASS: T8122 ACIO deep-sleep teardown, freeze/s2idle preservation, and DWC3 PM order");
    return 0;
}
'''


def test_t8122_cio_sleep(out, acio):
    dwc = (ROOT / 'drivers/usb/dwc3/dwc3-apple.c').read_text()
    hw_defs = []
    for sym in ('apple_cio_t8103_hw', 'apple_cio_t8122_hw'):
        m = re.search(r'^static const struct apple_cio_hw ' + sym +
                      r' = \{.*?\n\};', acio, re.M | re.S)
        assert m, sym
        hw_defs.append(m.group())
    match_block = re.search(
        r'static const struct of_device_id apple_acio_match\[\] = \{.*?\n\};',
        acio, re.S)
    assert match_block
    entries = re.findall(
        r'\.compatible\s*=\s*"([^"]+)"\s*,\s*\.data\s*=\s*(&apple_cio_\w+_hw)',
        match_block.group())
    match_lines = [f'    {{ "{c}", {d} }},' for c, d in entries
                   if c in ('apple,t8122-usb4-acio', 'apple,t8103-usb4-acio')]
    assert len(match_lines) == 2, match_lines
    code = T8122_SLEEP_FIXTURE
    code = code.replace('/* PRODUCTION_HW */', '\n'.join(hw_defs))
    code = code.replace('/* MATCH_ENTRIES */', '\n'.join(match_lines))
    code = code.replace('/* PRODUCTION_FN */',
                        function(acio, 'apple_cio_sleep_removes_domains'))
    code = code.replace('/* ACIO_REGISTRATION */', pm_registration(
        acio, 'acio->pm_notifier.notifier_call = apple_cio_pm_notify;',
        'register_pm_notifier(&acio->pm_notifier);'))
    code = code.replace('/* DWC_REGISTRATION */', pm_registration(
        dwc, 'appledwc->pm_nb.notifier_call = dwc3_apple_pm_notify;',
        'ret = register_pm_notifier(&appledwc->pm_nb);'))
    muts = {
        't8122-drop-match': ('    { "apple,t8122-usb4-acio", &apple_cio_t8122_hw },', ''),
        't8122-drop-sleep-removes-domains': ('\t.sleep_removes_domains = true,\n', ''),
        't8122-drop-phy-shared': ('\t.phy_shared_with_dwc3 = true,\n', ''),
    }
    cases = {'t8122-cio-sleep': code}
    for name, (old, new) in muts.items():
        assert code.count(old) == 1, name
        cases[name] = code.replace(old, new)
    for name, body in cases.items():
        source, binary = out / (name + '.c'), out / name
        source.write_text(body)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / (name + '.log')).write_text(run.stdout + run.stderr)
        if name == 't8122-cio-sleep':
            assert run.returncode == 0, run.stderr
            print(run.stdout, end='')
        else:
            assert run.returncode < 0 and 'Assertion' in run.stderr, (
                name, run.returncode, run.stderr)
            print('PASS negative control:', name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    pci = (ROOT / 'drivers/pci/controller/pcie-apple.c').read_text()
    aspm = (ROOT / 'drivers/pci/pcie/aspm.c').read_text()
    pci_core = (ROOT / 'drivers/pci/pci.c').read_text()
    pci_drv = (ROOT / 'drivers/pci/pci-driver.c').read_text()
    acio = (ROOT / 'drivers/thunderbolt/apple.c').read_text()
    defines = '\n'.join(re.findall(r'^#define (?:[^\n]*\\\n)*[^\n]*$', pci, re.M))
    info = pci[pci.index('struct hw_info {'):pci.index('static const struct of_device_id apple_pcie_of_match[];')]
    matches = re.search(r'static const struct of_device_id apple_pcie_of_match\[\] = \{.*?\n\};', pci, re.S)[0]
    production = '\n'.join((defines, info, matches,
                            function(pci, 'apple_pcie_tunnel_needs_cold_init'),
                            function(acio, 'apple_cio_pciec_eligible'),
                            function(acio, 'apple_cio_pcie_tunnel_is_preinitialized')))
    code = FIXTURE.replace('/* PRODUCTION */', production)
    aspm_struct = re.search(r'^struct pcie_link_state \{.*?\n\};', aspm, re.M | re.S)[0]
    aspm_helpers = '''
static int policy_to_aspm_state(struct pcie_link_state *link)
{
    return link->aspm_default;
}
static void pcie_update_aspm_capable(struct pcie_link_state *root)
{
    /* A link freed before aspm_lock was taken has no root left. */
    assert(root && aspm_lock.held);
}
'''
    aspm_work_struct = re.search(r'^struct pcie_aspm_pm_work \{.*?\n\};', aspm,
                                 re.M | re.S)[0]
    aspm_prod = '\n'.join((aspm_struct, aspm_work_struct, aspm_helpers,
                           *(function(aspm, n) for n in (
                               'pcie_config_aspm_dev',
                               'pcie_config_aspm_l1ss',
                               'pcie_config_aspm_link',
                               'pcie_config_aspm_path',
                               'pcie_aspm_pm_state_change',
                               'pcie_aspm_pm_work_fn',
                               'pcie_aspm_pm_state_change_deferred',
                               'pcie_aspm_l1ss_sleep_begin_locked',
                               'pcie_aspm_l1ss_sleep_end')),
                           function(pci_core, 'pci_pm_notify_host'),
                           function(pci_core, 'pci_pm_notify_state_change'),
                           function(pci_core, 'pci_pm_notify_resume'),
                           function(pci_drv, 'pci_pm_default_resume_early'),
                           function(pci_drv, 'pci_pm_runtime_resume')))
    l1ss_structs = pci[pci.index('struct apple_pcie_hib_l1ss {'):
                       pci.index('static int apple_pcie_cfg_read_u32(')]
    s2idle_prod = '\n'.join((defines, info, matches, aspm_prod, l1ss_structs,
                             *(function(pci, n) for n in (
                                 'apple_pcie_pd_notify',
                                 'apple_pcie_release_domains',
                                 'apple_pcie_hold_domains',
                                 'apple_pcie_can_keep_link',
                                 'apple_pcie_keep_link',
                                 'apple_pcie_adopted_links_ready',
                                 'apple_pcie_adopted_sleep_host',
                                 'apple_pcie_cfg_read_u32',
                                 'apple_pcie_cfg_write_u32',
                                 'apple_pcie_lnkctl_read',
                                 'apple_pcie_lnkctl_write',
                                 'apple_pcie_hib_save_lnkctl',
                                 'apple_pcie_hib_merge_aspm',
                                 'apple_pcie_hib_orig_fn_lnkctl',
                                 'apple_pcie_link_set_l1ss',
                                 'apple_pcie_hib_disable_l1ss_link',
                                 'apple_pcie_hib_restore_l1ss_link',
                                 'apple_pcie_hib_disable_l1ss_cb',
                                 'apple_pcie_hib_restore_l1ss',
                                 'apple_pcie_pm_state_change',
                                 'apple_pcie_hib_l1ss_pci_notify',
                                 'apple_pcie_hib_free_l1ss',
                                 'apple_pcie_s2r_reinit',
                                 'apple_pcie_prepare',
                                 'apple_pcie_complete'))))
    s2idle_code = S2IDLE_FIXTURE.replace('/* PRODUCTION */', s2idle_prod)
    t6050_old = '\t.mps_timers\t\t= true,\n\t.s2idle_hold_domain\t= true,\n'
    s2idle_muts = {
        'drop-t6050-hold': (t6050_old, '\t.mps_timers\t\t= true,\n'),
        'drop-prepare-guard': (
            'pm_suspend_target_state != PM_SUSPEND_ON &&\n'
            '\t    !apple_pcie_can_keep_link(pcie)',
            'pm_suspend_target_state != PM_SUSPEND_ON &&\n\t    false'),
        'drop-keep-link-param': (
            'if (!READ_ONCE(s2idle_keep_link) || !pm_suspend_no_platform())',
            'if (!pm_suspend_no_platform())'),
        'drop-no-platform-check': (
            'if (!READ_ONCE(s2idle_keep_link) || !pm_suspend_no_platform())',
            'if (!READ_ONCE(s2idle_keep_link))'),
        'drop-init-check': (
            'if (!(pcie->kernel_init || pcie->tunnel_cold_init))',
            'if (false)'),
        'drop-bus-stopped-check': (
            'if (pcie->bus_stopped || pcie->resume_failed)',
            'if (pcie->resume_failed)'),
        'drop-resume-failed-check': (
            'if (pcie->bus_stopped || pcie->resume_failed)',
            'if (pcie->bus_stopped)'),
        'drop-domain-policy-check': (
            'if (!pcie->power_retained && !pcie->hw->s2idle_hold_domain)',
            'if (false)'),
        'skip-domain-release': (
            'dev_pm_genpd_remove_notifier(pd->dev);', '(void)pd;'),
        'ignore-domain-off': (
            'if (!dev_pm_genpd_is_on(pd->dev)) {', 'if (false) {'),
        'drop-adopted-prepare-guard': (
            'if (pcie && pm_suspend_target_state != PM_SUSPEND_ON &&\n'
            '\t    apple_pcie_adopted_sleep_host(pcie)) {',
            'if (false) {'),
        'drop-adopted-fw-sleep-guard': (
            'ret = pcie->t6050_port && pcie->t6050_dart ? 0 : -EBUSY;',
            'ret = 0;'),
        'drop-adopted-parked-skip': (
            'if (pcie->t6050_parked & BIT(port->idx))\n\t\t\tcontinue;',
            'if (false)\n\t\t\tcontinue;'),
        'drop-l1ss-parent-lnkctl-retain': (
            'restore_plnkctl =\n'
            '\t\t\t\tapple_pcie_hib_merge_aspm(plnkctl, saved->pl_lnkctl);',
            'restore_plnkctl = plnkctl;'),
        'drop-l1ss-fn-lnkctl-retain': (
            'restore_fn_lnkctl[nfn] =\n'
            '\t\t\t\t\tapple_pcie_hib_merge_aspm(fn_lnkctl[nfn], orig);',
            '(void)orig;\n\t\t\t\trestore_fn_lnkctl[nfn] = fn_lnkctl[nfn];'),
        'merge-l1ss-parent-aspm-on-complete': (
            'if (rollback)\n'
            '\t\t\trestore_plnkctl =\n'
            '\t\t\t\tapple_pcie_hib_merge_aspm(plnkctl, saved->pl_lnkctl);',
            'if (true)\n'
            '\t\t\trestore_plnkctl =\n'
            '\t\t\t\tapple_pcie_hib_merge_aspm(plnkctl, saved->pl_lnkctl);'),
        'merge-l1ss-fn-aspm-on-complete': (
            'if (rollback) {\n'
            '\t\t\t\tu16 orig = apple_pcie_hib_orig_fn_lnkctl(saved,',
            'if (true) {\n'
            '\t\t\t\tu16 orig = apple_pcie_hib_orig_fn_lnkctl(saved,'),
        'drop-fallback-d0-check': (
            'if (defer_pcipm) {\n'
            '\t\t\t\tpl_ctl1 &= ~APPLE_PCIE_L1SS_PCIPM_MASK;',
            'if (false) {\n'
            '\t\t\t\tpl_ctl1 &= ~APPLE_PCIE_L1SS_PCIPM_MASK;'),
        'drop-core-d0-check': (
            'if (parent->current_state != PCI_D0 || child->current_state != PCI_D0) {\n'
            '\t\tstate &= ~PCIE_LINK_STATE_L1_SS_PCIPM;',
            'if (false) {\n'
            '\t\tstate &= ~PCIE_LINK_STATE_L1_SS_PCIPM;'),
        'drop-aspm-sleep-disable-mask': (
            'state &= (link->aspm_capable & ~link->aspm_disable &\n'
            '\t\t  ~link->aspm_sleep_disable);',
            'state &= (link->aspm_capable & ~link->aspm_disable);'),
        'drop-sleep-begin-clear-enabled': (
            '\t\tlink->aspm_sleep_disable |= PCIE_LINK_STATE_L1SS;\n'
            '\t\tlink->aspm_enabled &= ~PCIE_LINK_STATE_L1SS;',
            '\t\tlink->aspm_sleep_disable |= PCIE_LINK_STATE_L1SS;'),
        'drop-runtime-resume-aspm-notify': (
            '\tpci_pm_default_resume_early(pci_dev);\n'
            '\tpci_resume_ptm(pci_dev);\n'
            '\tpci_pm_notify_resume(pci_dev);',
            '\tpci_pm_default_resume_early(pci_dev);\n'
            '\tpci_resume_ptm(pci_dev);'),
        'sync-runtime-resume-aspm-notify': (
            '\tpci_resume_ptm(pci_dev);\n'
            '\tpci_pm_notify_resume(pci_dev);',
            '\tpci_resume_ptm(pci_dev);\n'
            '\tpci_pm_notify_state_change(pci_dev, false);'),
        'prelock-link-read': [
            ('\tstruct pcie_link_state *link;\n\n'
             '\t/* Unlocked hint only; pdev->link_state is validated under aspm_lock. */\n'
             '\tif (aspm_disabled || !READ_ONCE(pdev->link_state))',
             '\tstruct pcie_link_state *link = pdev->link_state;\n\n'
             '\tif (aspm_disabled || !link)'),
            ('\t/* free_link_state() clears pdev->link_state under aspm_lock. */\n'
             '\tlink = pdev->link_state;\n',
             ''),
        ],
        'drop-unmanaged-pending-keep': (
            '\tif (defer) {\n'
            '\t\tlist_add_tail(&saved->entry, &pcie->hib_l1ss_pending);\n'
            '\t\tctx->keep_pending = true;\n'
            '\t}',
            '\tif (defer)\n'
            '\t\tctx->keep_pending = false;'),
        'publish-pending-after-masking': (
            '\tmutex_lock(&pcie->hib_l1ss_lock);\n'
            '\tdefer = ((saved->pl_ctl1 | saved->cl_ctl1) & APPLE_PCIE_L1SS_PCIPM_MASK) &&\n'
            '\t\t(parent->current_state != PCI_D0 || child->current_state != PCI_D0);\n'
            '\tret = apple_pcie_link_set_l1ss(saved, true, ctx->rollback, false, defer);\n'
            '\tif (ret) {\n'
            '\t\tmutex_unlock(&pcie->hib_l1ss_lock);\n'
            '\t\tpci_warn(child, "failed to restore PCIe L1SS after sleep: %d\\n",\n'
            '\t\t\t ret);\n'
            '\t\treturn ret;\n'
            '\t}\n'
            '\n'
            '\tpci_save_aspm_l1ss_state(child);\n',
            '\tdefer = ((saved->pl_ctl1 | saved->cl_ctl1) & APPLE_PCIE_L1SS_PCIPM_MASK) &&\n'
            '\t\t(parent->current_state != PCI_D0 || child->current_state != PCI_D0);\n'
            '\tret = apple_pcie_link_set_l1ss(saved, true, ctx->rollback, false, defer);\n'
            '\tif (ret) {\n'
            '\t\tpci_warn(child, "failed to restore PCIe L1SS after sleep: %d\\n",\n'
            '\t\t\t ret);\n'
            '\t\treturn ret;\n'
            '\t}\n'
            '\n'
            '\tpci_save_aspm_l1ss_state(child);\n'
            '\tmutex_lock(&pcie->hib_l1ss_lock);\n'),
        'drop-unmanaged-sleep-save-state': (
            '\tif (!ret && !core_managed)\n'
            '\t\tpci_save_aspm_l1ss_state(child);',
            ''),
        'drop-zero-l1ss-sleep-suppress': (
            '\tif (!((saved->pl_ctl1 | saved->cl_ctl1) & PCI_L1SS_CTL1_L1SS_MASK)) {\n'
            '\t\tsaved->active = core_managed;\n'
            '\t\treturn 0;\n'
            '\t}',
            '\tif (!((saved->pl_ctl1 | saved->cl_ctl1) & PCI_L1SS_CTL1_L1SS_MASK)) {\n'
            '\t\tsaved->active = false;\n'
            '\t\treturn 0;\n'
            '\t}'),
    }
    s2idle_cases = {'s2idle-production': s2idle_code}
    for name, edits in s2idle_muts.items():
        mutated = s2idle_code
        for old, new in ([edits] if isinstance(edits, tuple) else edits):
            assert mutated.count(old) == 1, name
            mutated = mutated.replace(old, new)
        s2idle_cases[name] = mutated
    with tempfile.TemporaryDirectory(prefix='pciec-policy-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        test_pm_order(out, acio)
        test_t8122_cio_sleep(out, acio)
        cases = {
            'production': code,
            'm2-fallback': code.replace(
                '{ .compatible = "apple,t8112-pciec",\t.data = &t8112_pciec_hw },', ''),
            't8142-admission': code.replace(
                'of_device_is_compatible(np, "apple,t8142-pciec")', 'false'),
            'dart-bypass': code.replace(
                'tunables = of_property_present(iommu_spec.np, "apple,tunable");',
                'tunables = true;'),
        }
        run_cases(out, cases)
        run_cases(out, s2idle_cases,
                  extra_flags=('-Wextra', '-Wno-unused-parameter', '-g',
                               '-fsanitize=address,undefined'))


if __name__ == '__main__':
    main()
