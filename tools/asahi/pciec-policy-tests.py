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
    start = re.search(r'^(?:static\s+)?(?:bool|int|u16|u32|unsigned long|void)\s+' + name + r'\([^{;]*\)\s*\{',
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
#define WRITE_ONCE(x, val) ((x) = (val))
#define smp_mb() ((void)0)
#define smp_mb__after_atomic() ((void)0)
#define CONFIG_PCIEASPM 1
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define kfree(p) free(p)
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define GENPD_NOTIFY_PRE_OFF 0
#define GENPD_NOTIFY_ON 1
#define PM_SUSPEND_ON 0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
#define PCI_VENDOR_ID           0x00
#define PCI_PM_CTRL             4
#define PCI_PM_CTRL_STATE_MASK  0x0003
#define PCI_PM_CTRL_NO_SOFT_RESET 0x0008
#define PCI_DEV_FLAGS_NO_PM_RESET (1U << 7)
#define PCIE_RESET_READY_POLL_MS 60000
#define PCI_PM_D3_WAIT          10
#define PCI_PM_D2_DELAY         200
#define PCI_CAP_ID_EXP          0x10
#define PCI_EXP_DEVCTL          0x08
#define PCI_EXP_LNKCTL          0x10
#define PCI_EXP_SLTCTL          0x18
#define PCI_EXP_RTCTL           0x1c
#define PCI_EXP_DEVCTL2         0x28
#define PCI_EXP_LNKCTL2         0x30
#define PCI_EXP_SLTCTL2         0x38
#define PCI_EXP_LNKCTL_ASPM_L0S 0x0001
#define PCI_EXP_LNKCTL_ASPM_L1  0x0002
#define PCI_EXP_LNKCTL_ASPMC    0x0003
#define PCI_L1SS_CTL1           0x08
#define PCI_L1SS_CTL2           0x0c
#define PCI_L1SS_CTL1_PCIPM_L1_2 0x00000001
#define PCI_L1SS_CTL1_PCIPM_L1_1 0x00000002
#define PCI_L1SS_CTL1_ASPM_L1_2  0x00000004
#define PCI_L1SS_CTL1_ASPM_L1_1  0x00000008
#define PCI_L1SS_CTL1_L1SS_MASK 0x0000000f
#define PCI_L1SS_CTL1_L1_2_MASK 0x00000005
#define PCI_EXT_CAP_ID_L1SS     0x001e
typedef int pci_power_t;
#define PCI_D0          0
#define PCI_D1          1
#define PCI_D2          2
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
    int rpm_usage;
    unsigned int rpm_barriers;
    void (*rpm_inflight_cb)(struct device *dev);
    unsigned int add_calls, remove_calls;
};
static int pci_rescan_remove_depth;
static int pci_bus_sem_depth;
static void pm_runtime_get_noresume(struct device *dev)
{
    dev->rpm_usage++;
}
static int pm_runtime_put(struct device *dev)
{
    assert(dev->rpm_usage > 0);
    dev->rpm_usage--;
    return 0;
}
static int pm_runtime_barrier(struct device *dev)
{
    assert(pci_rescan_remove_depth == 0 && pci_bus_sem_depth == 0);
    assert(dev->rpm_usage > 0);
    dev->rpm_barriers++;
    if (dev->rpm_inflight_cb) {
        void (*cb)(struct device *) = dev->rpm_inflight_cb;

        dev->rpm_inflight_cb = NULL;
        cb(dev);
    }
    return 0;
}
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
static void list_del_init(struct list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    list_init(entry);
}
static void list_splice_init(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list)) {
        struct list_head *first = list->next;
        struct list_head *last = list->prev;
        struct list_head *at = head->next;

        first->prev = head;
        head->next = first;
        last->next = at;
        at->prev = last;
        list_init(list);
    }
}
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
#define list_first_entry_or_null(ptr, type, member) \
    (!list_empty(ptr) ? container_of((ptr)->next, type, member) : NULL)
typedef struct { int v; } atomic_t;
static void atomic_set(atomic_t *a, int v) { a->v = v; }
static int atomic_read(const atomic_t *a) { return a->v; }
static void atomic_inc(atomic_t *a) { a->v++; }
static void atomic_dec(atomic_t *a) { assert(a->v > 0); a->v--; }
typedef struct { bool held; } spinlock_t;
#define DEFINE_SPINLOCK(x) static spinlock_t x = { false }
static int atomic_ctx_depth;
static bool spin_is_locked(const spinlock_t *l) { return l->held; }
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; \
    assert(!(lock)->held); \
    (lock)->held = true; \
    atomic_ctx_depth++; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    (void)(flags); \
    assert((lock)->held && atomic_ctx_depth > 0); \
    (lock)->held = false; \
    atomic_ctx_depth--; \
} while (0)
struct mutex {
    bool held;
};
static void mutex_init(struct mutex *lock)
{
    lock->held = false;
}
struct work_struct {
    void (*func)(struct work_struct *work);
    struct work_struct *next;
    bool pending;
};
struct workqueue_struct { int id; };
static struct workqueue_struct system_freezable_wq_obj = { 1 };
static __attribute__((unused)) struct workqueue_struct system_dfl_wq_obj = { 2 };
#define system_freezable_wq (&system_freezable_wq_obj)
#define system_dfl_wq (&system_dfl_wq_obj)
#define INIT_WORK(w, f) do { (w)->func = (f); (w)->next = NULL; (w)->pending = false; } while (0)
static struct work_struct *work_head, *work_tail;
static unsigned int queued_works;
static bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    assert(wq == system_freezable_wq);
    if (work->pending)
        return false;
    work->pending = true;
    work->next = NULL;
    if (work_tail)
        work_tail->next = work;
    else
        work_head = work;
    work_tail = work;
    queued_works++;
    return true;
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
    struct list_head hib_l1ss_rpm;
    struct mutex hib_l1ss_lock;
    struct work_struct hib_l1ss_work;
    atomic_t hib_l1ss_pending_cnt;
    bool hib_l1ss_closing;
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
struct pci_cap_saved_data {
    u32 data[7];
};
struct pci_cap_saved_state {
    struct pci_cap_saved_data cap;
};
struct pcie_link_state;
struct pci_dev {
    struct list_head bus_list;
    struct pci_bus *bus;
    struct pci_bus *subordinate;
    struct pcie_link_state *link_state;
    struct work_struct aspm_pm_work;
    u8 aspm_pcipm_deferred;
    u8 aspm_l1ss_resync;
    struct device dev;
    void *driver;
    pci_power_t current_state;
    u32 dev_flags;
    u8 devfn;
    u8 pm_cap;
    u16 l1ss;
    u16 lnkctl;
    u16 pmcsr;
    bool d1_support, d2_support;
    bool refuse_low_power;
    bool downstream_port;
    bool disconnected;
    bool power_up_fail;
    int cfg_read_err;
    unsigned int refs;
    unsigned int restore_calls;
    u32 cfg[128];
    struct pci_cap_saved_state saved_l1ss;
    struct pci_cap_saved_state saved_exp;
    u32 first_ctl1_write;
    bool saw_ctl1_write;
    unsigned int lnkctl_write_calls;
    unsigned int fail_lnkctl_write_on_call;
    unsigned int fail_lnkctl_write_count;
    unsigned int cfg_read_calls;
    unsigned int fail_cfg_read_on_call;
    unsigned int cfg_write_calls;
    unsigned int fail_cfg_write_on_call;
};
#define to_pci_dev(d) container_of(d, struct pci_dev, dev)
struct pci_host_bridge {
    struct pci_bus *bus;
    void *priv;
    void (*pm_state_change)(struct pci_host_bridge *bridge, struct pci_dev *dev);
};
static int pci_bus_sem;
static struct mutex aspm_lock;
DEFINE_SPINLOCK(aspm_l1ss_hw_lock);
static bool aspm_disabled;
static bool require_topology_lock;
static bool require_hw_lock_on_lnkctl;
static bool require_hw_lock_on_lnkctl_read;
static bool require_hw_lock_on_pmcsr;
static bool require_hw_lock_on_l1ss_read;
static void (*aspm_lock_pre_hook)(void);
static void (*cfg_read_hook)(struct pci_dev *pdev, int pos);
static void (*cfg_write_hook)(struct pci_dev *pdev, int pos);
static void (*pmcsr_write_hook)(struct pci_dev *pdev);
static void pci_lock_rescan_remove(void)
{
    assert(atomic_ctx_depth == 0);
    assert(pci_rescan_remove_depth == 0);
    assert(pci_bus_sem_depth == 0 && !aspm_lock.held);
    pci_rescan_remove_depth++;
}
static void pci_unlock_rescan_remove(void)
{
    assert(pci_rescan_remove_depth == 1);
    pci_rescan_remove_depth--;
}
static void down_read(int *sem)
{
    assert(atomic_ctx_depth == 0);
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
static void mutex_lock(struct mutex *lock)
{
    assert(atomic_ctx_depth == 0);
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
}
static void run_queued_work(void)
{
    assert(pci_rescan_remove_depth == 0 && pci_bus_sem_depth == 0 &&
           !aspm_lock.held && atomic_ctx_depth == 0);
    while (work_head) {
        struct work_struct *work = work_head;

        work_head = work->next;
        if (!work_head)
            work_tail = NULL;
        work->next = NULL;
        work->pending = false;
        work->func(work);
    }
}
static bool pci_dev_is_disconnected(const struct pci_dev *pdev)
{
    return pdev->disconnected;
}
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
static struct pci_cap_saved_state *pci_find_saved_cap(struct pci_dev *dev, char cap)
{
    assert(cap == PCI_CAP_ID_EXP);
    return &dev->saved_exp;
}
static struct pci_cap_saved_state *pci_find_saved_ext_cap(struct pci_dev *dev, u16 cap)
{
    assert(cap == PCI_EXT_CAP_ID_L1SS);
    return dev->l1ss ? &dev->saved_l1ss : NULL;
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
static int pci_read_config_word(struct pci_dev *pdev, int pos, u16 *val)
{
    assert(pdev->pm_cap && pos == pdev->pm_cap + PCI_PM_CTRL);
    *val = pdev->pmcsr;
    return 0;
}
static int pci_write_config_word(struct pci_dev *pdev, int pos, u16 val)
{
    assert(pdev->pm_cap && pos == pdev->pm_cap + PCI_PM_CTRL);
    if (require_hw_lock_on_pmcsr)
        assert(spin_is_locked(&aspm_l1ss_hw_lock));
    if (pmcsr_write_hook)
        pmcsr_write_hook(pdev);
    if (!pdev->refuse_low_power)
        pdev->pmcsr = val;
    return 0;
}
static int pci_read_config_dword(struct pci_dev *pdev, int pos, u32 *val)
{
    if (require_topology_lock)
        assert(pci_rescan_remove_depth > 0);
    if (require_hw_lock_on_l1ss_read && pdev->l1ss &&
        (pos == pdev->l1ss + PCI_L1SS_CTL1 || pos == pdev->l1ss + PCI_L1SS_CTL2))
        assert(spin_is_locked(&aspm_l1ss_hw_lock));
    if (cfg_read_hook)
        cfg_read_hook(pdev, pos);
    assert((pos & 3) == 0 && (size_t)(pos >> 2) < ARRAY_SIZE(pdev->cfg));
    pdev->cfg_read_calls++;
    if (pdev->fail_cfg_read_on_call &&
        pdev->cfg_read_calls == pdev->fail_cfg_read_on_call)
        return pdev->cfg_read_err ? pdev->cfg_read_err : -EIO;
    *val = pdev->cfg[pos >> 2];
    return 0;
}
static int pci_write_config_dword(struct pci_dev *pdev, int pos, u32 val)
{
    if (require_topology_lock)
        assert(pci_rescan_remove_depth > 0);
    if (cfg_write_hook)
        cfg_write_hook(pdev, pos);
    assert((pos & 3) == 0 && (size_t)(pos >> 2) < ARRAY_SIZE(pdev->cfg));
    pdev->cfg_write_calls++;
    if (pdev->l1ss && pos == pdev->l1ss + PCI_L1SS_CTL1 && !pdev->saw_ctl1_write) {
        pdev->first_ctl1_write = val;
        pdev->saw_ctl1_write = true;
    }
    if (pdev->fail_cfg_write_on_call &&
        pdev->cfg_write_calls == pdev->fail_cfg_write_on_call)
        return -EIO;
    pdev->cfg[pos >> 2] = val;
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
static int pcie_capability_read_word(struct pci_dev *pdev, int pos, u16 *val)
{
    if (pos == PCI_EXP_LNKCTL) {
        if (require_hw_lock_on_lnkctl_read)
            assert(spin_is_locked(&aspm_l1ss_hw_lock));
        *val = pdev->lnkctl;
    } else {
        *val = 0;
    }
    return 0;
}
static int pcie_capability_write_word(struct pci_dev *pdev, int pos, u16 val)
{
    if (pos == PCI_EXP_LNKCTL) {
        if (require_hw_lock_on_lnkctl)
            assert(spin_is_locked(&aspm_l1ss_hw_lock));
        pdev->lnkctl_write_calls++;
        if (pdev->fail_lnkctl_write_on_call) {
            unsigned int n = pdev->fail_lnkctl_write_count ? pdev->fail_lnkctl_write_count : 1;
            if (pdev->lnkctl_write_calls >= pdev->fail_lnkctl_write_on_call &&
                pdev->lnkctl_write_calls < pdev->fail_lnkctl_write_on_call + n)
                return -EIO;
        }
        pdev->lnkctl = val;
    }
    return 0;
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
static void pci_save_ltr_state(struct pci_dev *dev) { (void)dev; }
static void pci_restore_ltr_state(struct pci_dev *dev) { (void)dev; }
static void pci_bridge_reconfigure_ltr(struct pci_dev *dev) { (void)dev; }
static bool pci_is_pcie(const struct pci_dev *dev) { (void)dev; return true; }
static int pci_dev_reset_iommu_prepare(struct pci_dev *dev) { (void)dev; return 0; }
static void pci_dev_reset_iommu_done(struct pci_dev *dev) { (void)dev; }
static int pci_dev_wait(struct pci_dev *dev, char *reset_type, int timeout)
{
    (void)dev;
    (void)reset_type;
    (void)timeout;
    return 0;
}
static int pci_save_pcie_state(struct pci_dev *dev);
static void pci_restore_pcie_state(struct pci_dev *dev);
static void pci_update_aspm_saved_state(struct pci_dev *pdev) { (void)pdev; }
static void pci_dev_d3_sleep(struct pci_dev *dev) { (void)dev; }
static void udelay(unsigned long usecs) { (void)usecs; }
static const char *pci_power_name(pci_power_t state)
{
    return state == PCI_D0 ? "D0" : "D3hot";
}
#define PCI_POSSIBLE_ERROR(val) ((val) == (typeof(val))~0U)
#define pci_dbg(dev, fmt, ...) ((void)(dev))
#define pci_err(dev, fmt, ...) ((void)(dev))
#define pci_info_ratelimited(dev, fmt, ...) ((void)(dev))
static void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev)
{
    if (!pci_dev->power_up_fail)
        pci_dev->current_state = PCI_D0;
    else
        pci_dev->current_state = PCI_D3cold;
}
static void pci_restore_state(struct pci_dev *pci_dev)
{
    pci_dev->restore_calls++;
    pci_restore_pcie_state(pci_dev);
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
    list_init(&pcie->hib_l1ss_rpm);
    mutex_init(&pcie->hib_l1ss_lock);
    INIT_WORK(&pcie->hib_l1ss_work, apple_pcie_hib_l1ss_work_fn);
    atomic_set(&pcie->hib_l1ss_pending_cnt, 0);
    pcie->hib_l1ss_closing = false;
    s2idle_keep_link = true;
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
    suspend_via_firmware = false;
    link_healthy = true;
    radio_prepare_calls = 0;
    radio_complete_calls = 0;
    warn_calls = 0;
    aspm_disabled = false;
    pci_bus_sem_depth = 0;
    pci_rescan_remove_depth = 0;
    atomic_ctx_depth = 0;
    require_topology_lock = false;
    require_hw_lock_on_lnkctl = false;
    require_hw_lock_on_lnkctl_read = false;
    require_hw_lock_on_pmcsr = false;
    require_hw_lock_on_l1ss_read = false;
    aspm_lock.held = false;
    aspm_l1ss_hw_lock.held = false;
    work_head = NULL;
    work_tail = NULL;
    queued_works = 0;
    aspm_lock_pre_hook = NULL;
    cfg_read_hook = NULL;
    cfg_write_hook = NULL;
    pmcsr_write_hook = NULL;
}
/* Concurrent events, injected at the point the real code would race. */
static struct pci_dev *race_pdev;
static struct pcie_link_state *race_link;
static struct apple_pcie *race_pcie;
static struct pci_dev *race_dev;
static bool race_saw_lock;
static bool race_saw_rescan_lock;
static int race_pending_cnt_on_retry;
static void race_free_link_hook(void)
{
    memset(race_link, 0, sizeof(*race_link));
    race_pdev->link_state = NULL;
}
static void race_d0_cfg_write_hook(struct pci_dev *pdev, int pos)
{
    if (pdev != race_dev || pos != pdev->l1ss + PCI_L1SS_CTL1 ||
        pdev->cfg_write_calls < 2)
        return;
    cfg_write_hook = NULL;
    race_dev->current_state = PCI_D0;
    apple_pcie_pm_state_change(race_pcie->bridge, race_dev);
}
static void race_snapshot_read_hook(struct pci_dev *pdev, int pos)
{
    if (!pdev->l1ss || pos != pdev->l1ss + PCI_L1SS_CTL2)
        return;
    cfg_read_hook = NULL;
    /* Verify hib_l1ss_lock and rescan_remove lock are held before reading CTL2/CTL1. */
    race_saw_lock = race_pcie->hib_l1ss_lock.held;
    race_saw_rescan_lock = (pci_rescan_remove_depth > 0);
}
static void race_retry_d0_read_hook(struct pci_dev *pdev, int pos)
{
    /* Fire when worker checks link1 (ep1) while link0 (fn0) was moved to retry. */
    if (pdev != race_pdev || pos != PCI_VENDOR_ID)
        return;
    cfg_read_hook = NULL;
    race_pending_cnt_on_retry = atomic_read(&race_pcie->hib_l1ss_pending_cnt);
    race_dev->current_state = PCI_D0;
    apple_pcie_pm_state_change(race_pcie->bridge, race_dev);
}
static u32 inflight_rpm_saved_ctl1;
static struct pci_dev *inflight_rpm_pdev;
static struct pci_bus *hotplug_root_bus;
static struct pci_dev *hotplug_rp;
static struct pci_dev *hotplug_chain_rps[8];
static unsigned int hotplug_chain_next;
static void inflight_rpm_save_ctl1(struct device *dev)
{
    (void)dev;
    inflight_rpm_saved_ctl1 =
        inflight_rpm_pdev->cfg[(inflight_rpm_pdev->l1ss + PCI_L1SS_CTL1) >> 2];
}
static void hotplug_during_barrier(struct device *dev)
{
    (void)dev;
    /* Simulate pciehp enumerating a new L1SS port+endpoint while rescan lock is dropped. */
    list_add_tail(&hotplug_rp->bus_list, &hotplug_root_bus->devices);
}
static void hotplug_chain_during_barrier(struct device *dev)
{
    (void)dev;
    if (hotplug_chain_next < 8)
        list_add_tail(&hotplug_chain_rps[hotplug_chain_next++]->bus_list,
                      &hotplug_root_bus->devices);
}
static void race_pmcsr_check_hook(struct pci_dev *pdev)
{
    /* Verify dev->current_state is already updated out of D0 before writing PMCSR. */
    assert(pdev->current_state == PCI_D3hot);
    assert(spin_is_locked(&aspm_l1ss_hw_lock));
    pmcsr_write_hook = NULL;
    require_hw_lock_on_pmcsr = false;
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
        fn0.pm_cap = 0x48; \
        ((u16 *)&fn0.saved_exp.cap.data[0])[1] = 0x0042; \
        pcie_aspm_init_dev(&rp0); \
        pcie_aspm_init_dev(&fn0); \
        pcie_aspm_init_dev(&fn1); \
        list_add_tail(&rp0.bus_list, &root_bus.devices); \
        list_add_tail(&fn0.bus_list, &sub0_bus.devices); \
        list_add_tail(&fn1.bus_list, &sub0_bus.devices); \
        if (two_links) { \
            INIT_DEV(rp1, 0x08, 0x150, &sub1_bus, true, 0x106b, 0x0000005f, 0x14, 0x0042); \
            INIT_DEV(ep1, 0x00, 0x160, NULL, false, 0x14e4, 0x0000005f, 0x14, 0x0042); \
            rp1.bus = &root_bus; \
            ep1.bus = &sub1_bus; \
            ep1.pm_cap = 0x48; \
            ((u16 *)&ep1.saved_exp.cap.data[0])[1] = 0x0042; \
            pcie_aspm_init_dev(&rp1); \
            pcie_aspm_init_dev(&ep1); \
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

    /* Suspend recheck clears L1SS replayed by pci_restore_state() after prepare(). */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    /* Simulate runtime resume during pci_pm_suspend() replaying saved L1SS enables. */
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    require_topology_lock = true;
    assert(apple_pcie_suspend(&dev) == 0);
    require_topology_lock = false;
    assert(pci_rescan_remove_depth == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    require_topology_lock = true;
    apple_pcie_complete(&dev);
    require_topology_lock = false;
    assert(pci_rescan_remove_depth == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Complete clears live CTL1 enables before writing saved timing, and masks PCI-PM if not D0. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    /* Firmware/resume left different timing bits in live CTL1 and endpoint in D3hot. */
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000a5;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000b5;
    rp0.saw_ctl1_write = false;
    fn0.saw_ctl1_write = false;
    fn0.current_state = PCI_D3hot;
    require_topology_lock = true;
    apple_pcie_complete(&dev);
    require_topology_lock = false;
    assert(fn0.saw_ctl1_write && fn0.first_ctl1_write == 0x000000b0);
    assert(rp0.saw_ctl1_write && rp0.first_ctl1_write == 0x000000a0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL2) >> 2] == 0x28);
    apple_pcie_hib_free_l1ss(&pcie);
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* One-shot failure on child's final CTL1 write unwinds L1SS symmetrically and retains recovery record. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.cfg_write_calls = 0;
    /* enable writes on child: 1=clear CTL1, 2=CTL2, 3=CTL1 sans L1.2, 4=final CTL1 with L1.2 */
    fn0.fail_cfg_write_on_call = 4;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(warn_calls == 1);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    /* Retry via D0 notification recovers L1SS cleanly and frees the pending record. */
    fn0.fail_cfg_write_on_call = 0;
    apple_pcie_pm_state_change(&bridge, &fn0);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Removed link (cleared subordinate or unlinked child) is skipped safely during complete(). */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    rp0.subordinate = NULL;
    rp0.cfg_write_calls = 0;
    fn0.cfg_write_calls = 0;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg_write_calls == 0 && fn0.cfg_write_calls == 0);
    assert(rp0.refs == 1 && fn0.refs == 1);

    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    list_del(&fn0.bus_list);
    rp0.cfg_write_calls = 0;
    fn0.cfg_write_calls = 0;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg_write_calls == 0 && fn0.cfg_write_calls == 0);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Suspend recheck failure on one-shot ASPM restore write restores pre-recheck ASPM before returning error. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 2; /* 1=clear ASPM, 2=restore_aspm fails once */
    assert(apple_pcie_suspend(&dev) == -EIO);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    apple_pcie_complete(&dev);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Suspend recheck catches a link whose L1SS was off during prepare() and enabled before suspend(). */
    RESET_L1SS(false);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(list_empty(&pcie.hib_l1ss));
    /* Endpoint runtime-resumed during pci_pm_suspend() and replayed L1SS enables. */
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    assert(apple_pcie_suspend(&dev) == 0);
    assert(!list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Suspend recheck propagates config read errors on a present link instead of skipping it. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.cfg_read_calls = 0;
    fn0.fail_cfg_read_on_call = 1; /* PCI_VENDOR_ID read error */
    assert(apple_pcie_suspend(&dev) == -EIO);
    fn0.fail_cfg_read_on_call = 0;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Unmanaged fallback: D3hot endpoint syncs saved state, defers PCI-PM, and restores via freezable work. */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    rp0.saved_l1ss.cap.data[0] = 0x28;
    rp0.saved_l1ss.cap.data[1] = 0x000000ff;
    fn0.saved_l1ss.cap.data[0] = 0x28;
    fn0.saved_l1ss.cap.data[1] = 0x000000ff;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(rp0.saved_l1ss.cap.data[1] == 0x000000f0 && fn0.saved_l1ss.cap.data[1] == 0x000000f0);
    /* A PCI restore during the sleep window (even from atomic context!) must not sleep or replay stale PCI-PM enables. */
    atomic_ctx_depth = 1;
    require_hw_lock_on_lnkctl = true;
    pci_save_aspm_l1ss_state(&fn0);
    pci_restore_state(&fn0);
    require_hw_lock_on_lnkctl = false;
    atomic_ctx_depth = 0;
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(rp0.saved_l1ss.cap.data[1] == 0x000000fc && fn0.saved_l1ss.cap.data[1] == 0x000000fc);
    /* Runtime resume under pci_bus_sem queues hib_l1ss_work without taking locks inline. */
    pci_bus_sem_depth = 1;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    pci_bus_sem_depth = 0;
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    require_topology_lock = true;
    require_hw_lock_on_lnkctl = true;
    run_queued_work();
    require_hw_lock_on_lnkctl = false;
    require_topology_lock = false;
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.saved_l1ss.cap.data[1] == 0x000000ff && fn0.saved_l1ss.cap.data[1] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* pci_set_low_power_state() holds aspm_l1ss_hw_lock across current_state + PMCSR write and notifies on refused D0. */
    RESET_L1SS(false);
    fn0.current_state = PCI_D0;
    fn0.pmcsr = PCI_D0;
    require_hw_lock_on_pmcsr = true;
    pmcsr_write_hook = race_pmcsr_check_hook;
    assert(pci_set_low_power_state(&fn0, PCI_D3hot, false) == 0);
    pmcsr_write_hook = NULL;
    require_hw_lock_on_pmcsr = false;
    assert(fn0.current_state == PCI_D3hot && (fn0.pmcsr & PCI_PM_CTRL_STATE_MASK) == PCI_D3hot);
    /* Refused D3hot leaving device in D0 triggers pm_state_change notification. */
    fn0.current_state = PCI_D0;
    fn0.pmcsr = PCI_D0;
    fn0.refuse_low_power = true;
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    fn0.current_state = PCI_D0;
    queued_works = 0;
    assert(pci_set_low_power_state(&fn0, PCI_D3hot, false) == 0);
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    run_queued_work();
    assert(atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Unmanaged fallback across two sleeps in D3hot preserves original PCI-PM bits. */
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
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Snapshot vs D0-worker & hot-removal race: rescan lock + hib_l1ss_lock are held before reading CTL1. */
    RESET_L1SS(false);
    aspm_disabled = true;
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f3;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f3;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    race_pcie = &pcie;
    race_dev = &fn0;
    race_saw_lock = false;
    race_saw_rescan_lock = false;
    cfg_read_hook = race_snapshot_read_hook;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(race_saw_lock && race_saw_rescan_lock);
    assert(!list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    fn0.current_state = PCI_D0;
    apple_pcie_complete(&dev);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f3);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f3);

    /* Worker retry-list race: hib_l1ss_pending_cnt stays >0 while a D3hot record sits on retry so D0 is never lost. */
    RESET_L1SS(true);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    ep1.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(atomic_read(&pcie.hib_l1ss_pending_cnt) == 2);
    /* Wake ep1 first; while worker handles ep1 and fn0 is on retry, wake fn0 and fire pm_state_change! */
    ep1.current_state = PCI_D0;
    race_pcie = &pcie;
    race_pdev = &ep1;
    race_dev = &fn0;
    race_pending_cnt_on_retry = -1;
    cfg_read_hook = race_retry_d0_read_hook;
    queued_works = 0;
    apple_pcie_pm_state_change(&bridge, &ep1);
    run_queued_work();
    assert(race_pending_cnt_on_retry == 2);
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp1.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x0000005f);
    assert(ep1.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x0000005f);
    assert(rp0.refs == 1 && fn0.refs == 1 && rp1.refs == 1 && ep1.refs == 1);

    /* BUS_NOTIFY_DEL_DEVICE purges BOTH hib_l1ss and hib_l1ss_pending cleanly; hib_l1ss_rpm pins release on complete/free. */
    RESET_L1SS(false);
    aspm_disabled = true;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(!list_empty(&pcie.hib_l1ss) && rp0.refs == 3 && fn0.refs == 3);
    assert(rp0.dev.rpm_usage == 1 && fn0.dev.rpm_usage == 1 && fn1.dev.rpm_usage == 1);
    assert(apple_pcie_hib_l1ss_pci_notify(&pcie.hib_l1ss_pci_nb, BUS_NOTIFY_DEL_DEVICE, &fn0.dev) == NOTIFY_DONE);
    assert(list_empty(&pcie.hib_l1ss) && rp0.refs == 2 && fn0.refs == 2 && fn1.refs == 2);
    apple_pcie_complete(&dev);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending) && rp0.refs == 2 && fn0.refs == 2);
    assert(apple_pcie_hib_l1ss_pci_notify(&pcie.hib_l1ss_pci_nb, BUS_NOTIFY_DEL_DEVICE, &fn0.dev) == NOTIFY_DONE);
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Core-managed link tests: sleep suppression, D3hot deferral, coalesced freezable work, restore sync, timing error retention. */
    struct pcie_link_state link0;
    #define ATTACH_LINK(l, up, dn, sup, en, def, dis) do { \
        memset(&(l), 0, sizeof(l)); \
        (l).pdev = (up); \
        (l).downstream = (dn); \
        (l).root = &(l); \
        (l).aspm_support = (sup); \
        (l).aspm_capable = (sup); \
        (l).aspm_enabled = (en); \
        (l).aspm_hw_l1ss = (en) & PCIE_LINK_STATE_L1SS; \
        (l).aspm_default = (def); \
        (l).aspm_disable = (dis); \
        (up)->link_state = &(l); \
        (up)->aspm_pcipm_deferred = 0; \
    } while (0)
    #define ALL_L1_STATES (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1SS)

    /* Core-managed normal D0 sleep/resume preserves policy and blocks mid-sleep restore replay. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(link0.aspm_enabled == PCIE_LINK_STATE_L1);
    assert(rp0.saved_l1ss.cap.data[1] == 0x000000f0 && fn0.saved_l1ss.cap.data[1] == 0x000000f0);
    /* Even if stale saved_l1ss had enables, pci_restore_state() masks them during sleep (and is atomic-safe). */
    rp0.saved_l1ss.cap.data[1] = 0x000000ff;
    fn0.saved_l1ss.cap.data[1] = 0x000000ff;
    atomic_ctx_depth = 1;
    pci_restore_state(&fn0);
    atomic_ctx_depth = 0;
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(link0.aspm_enabled == PCIE_LINK_STATE_L1);
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.aspm_pcipm_deferred == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    /* When aspm_pcipm_deferred == 0, runtime resume queues ZERO ASPM work items. */
    queued_works = 0;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(queued_works == 0);

    /* Core-managed D3hot endpoint sets aspm_pcipm_deferred, coalesces work, and avoids desync with pci_restore_state(). */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.current_state = PCI_D3hot;
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0);
    assert(rp0.aspm_pcipm_deferred == 1);
    assert(link0.aspm_enabled == (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_1 | PCIE_LINK_STATE_L1_2));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000fc);
    /* Failed power-up leaves device out of D0 and queues no work. */
    fn0.power_up_fail = true;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(fn0.current_state == PCI_D3cold && queued_works == 0);
    fn0.power_up_fail = false;
    fn0.restore_calls = 0;
    /* Error recovery runtime resume with pci_bus_sem held queues preallocated coalesced work. */
    pci_bus_sem_depth = 1;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    pci_bus_sem_depth = 0;
    assert(fn0.current_state == PCI_D0 && fn0.restore_calls == 2);
    assert(queued_works == 1 && rp0.refs == 2);
    run_queued_work();
    assert(rp0.refs == 1 && fn0.refs == 1 && rp0.aspm_pcipm_deferred == 0);
    assert(link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    /* If a later pci_restore_state() replays stale masked CTL1, atomic restore sets aspm_l1ss_resync (without mutating bitfields) and queues deferred recheck! */
    rp0.saved_l1ss.cap.data[1] = 0x000000fc;
    fn0.saved_l1ss.cap.data[1] = 0x000000fc;
    queued_works = 0;
    pci_restore_state(&fn0);
    assert(link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.aspm_l1ss_resync == 1 && rp0.aspm_pcipm_deferred == 1 && queued_works == 1);
    run_queued_work();
    assert(link0.aspm_enabled == ALL_L1_STATES && rp0.aspm_l1ss_resync == 0 && rp0.aspm_pcipm_deferred == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Core-managed transient vendor-ID -EIO with both ends ALREADY in D0 queues bounded retry automatically! */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(rp0.dev.rpm_usage == 1 && fn0.dev.rpm_usage == 1 && fn1.dev.rpm_usage == 1);
    fn0.cfg_read_calls = 0;
    fn0.fail_cfg_read_on_call = 1; /* transient PCI_VENDOR_ID -EIO while both ends are already D0 */
    queued_works = 0;
    apple_pcie_complete(&dev);
    assert(warn_calls == 1);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 1); /* bounded retry queued without requiring another D0 transition */
    fn0.fail_cfg_read_on_call = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* Config-read -ENODEV on a still-present link retains recovery and keeps aspm_sleep_disable armed! */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.cfg_read_calls = 0;
    fn0.cfg_read_err = -ENODEV;
    fn0.fail_cfg_read_on_call = 1;
    queued_works = 0;
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 1);
    fn0.fail_cfg_read_on_call = 0;
    fn0.cfg_read_err = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && link0.aspm_sleep_disable == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Still-enumerated D3cold endpoint returning 0xffffffff retains recovery until D0 resume! */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.current_state = PCI_D3cold;
    fn0.cfg[PCI_VENDOR_ID >> 2] = U32_MAX;
    queued_works = 0;
    apple_pcie_complete(&dev);
    assert(warn_calls == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 0);
    /* Endpoint powers back up to D0 and becomes readable again. */
    fn0.cfg[PCI_VENDOR_ID >> 2] = 0x14e4;
    assert(pci_pm_runtime_resume(&fn0.dev) == 0);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Unmanaged failed prepare LNKCTL restore + transient rollback failure preserves rollback intent in worker! */
    RESET_L1SS(false);
    aspm_disabled = true;
    /* Link0 prepare fails on parent ASPM restore (leaves rp0.lnkctl == 0x0040); rollback fails on VID read! */
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 2; /* 1=clear ASPM, 2=prepare restore_aspm fails */
    fn0.cfg_read_calls = 0;
    fn0.fail_cfg_read_on_call = 4; /* 1=prepare VID, 2=CTL2, 3=CTL1, 4=rollback VID fails */
    queued_works = 0;
    assert(apple_pcie_prepare(&dev) == -EIO);
    assert(rp0.lnkctl == 0x0040);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 1);
    fn0.fail_cfg_read_on_call = 0;
    rp0.fail_lnkctl_write_on_call = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* pci_save_pcie_state() and prepare/recheck L1SS snapshots hold aspm_l1ss_hw_lock across LNKCTL + CTL2/CTL1 reads! */
    RESET_L1SS(false);
    require_hw_lock_on_lnkctl_read = true;
    require_hw_lock_on_l1ss_read = true;
    assert(pci_save_pcie_state(&fn0) == 0);
    assert(apple_pcie_prepare(&dev) == 0);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    assert(apple_pcie_suspend(&dev) == 0);
    require_hw_lock_on_lnkctl_read = false;
    require_hw_lock_on_l1ss_read = false;
    apple_pcie_complete(&dev);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* pci_pm_reset() serializes D3hot entry under aspm_l1ss_hw_lock and notifies D0 when ready again! */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    fn0.current_state = PCI_D0;
    fn0.pmcsr = PCI_D0;
    queued_works = 0;
    require_hw_lock_on_pmcsr = true;
    pmcsr_write_hook = race_pmcsr_check_hook;
    assert(pci_pm_reset(&fn0, false) == 0);
    pmcsr_write_hook = NULL;
    require_hw_lock_on_pmcsr = false;
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    run_queued_work();
    assert(atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* Suspend recheck partial error after clearing L1SS on a link missed by prepare() MUST preserve recovery snapshot! */
    RESET_L1SS(false);
    aspm_disabled = true;
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(list_empty(&pcie.hib_l1ss));
    /* Endpoint replayed L1SS after prepare; suspend recheck clears child+parent CTL1 then fails on ASPM restore! */
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000ff;
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 2; /* 1=clear ASPM, 2=restore_aspm fails once */
    assert(apple_pcie_suspend(&dev) == -EIO);
    assert(!list_empty(&pcie.hib_l1ss)); /* recovery snapshot MUST NOT be dropped after modifying hardware! */
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    rp0.fail_lnkctl_write_on_call = 0;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* In-flight runtime-suspend callback is drained by pm_runtime_barrier() OUTSIDE PCI locks BEFORE L1SS is cleared! */
    RESET_L1SS(false);
    inflight_rpm_saved_ctl1 = 0;
    inflight_rpm_pdev = &fn0;
    fn0.dev.rpm_inflight_cb = inflight_rpm_save_ctl1;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(fn0.dev.rpm_barriers == 1 && rp0.dev.rpm_barriers == 1 && fn1.dev.rpm_barriers == 1);
    assert(inflight_rpm_saved_ctl1 == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* One-shot LNKCTL restore failure in complete() retries parent and downstream function ASPM. */
    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 2;
    apple_pcie_complete(&dev);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    apple_pcie_hib_free_l1ss(&pcie);

    RESET_L1SS(false);
    assert(apple_pcie_prepare(&dev) == 0);
    fn1.lnkctl_write_calls = 0;
    fn1.fail_lnkctl_write_on_call = 2;
    apple_pcie_complete(&dev);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    apple_pcie_hib_free_l1ss(&pcie);

    /* Failed prepare rollback ALSO retains recovery record and keeps aspm_sleep_disable armed until recovery! */
    RESET_L1SS(true);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    /* Fail link1 prepare -> triggers rollback of link0, whose CTL2 timing write fails transiently. */
    rp1.fail_lnkctl_write_on_call = 2;
    fn0.cfg_write_calls = 0;
    fn0.fail_cfg_write_on_call = 3; /* 1=prepare clear CTL1, 2=rollback clear CTL1, 3=rollback CTL2 write */
    assert(apple_pcie_prepare(&dev) == -EIO);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    fn0.fail_cfg_write_on_call = 0;
    apple_pcie_pm_state_change(&bridge, &fn0);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(rp0.refs == 1 && fn0.refs == 1 && rp1.refs == 1 && ep1.refs == 1);

    /* Core-managed timing restore failure keeps aspm_sleep_disable armed and retains recovery record! */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.cfg_write_calls = 0;
    fn0.fail_cfg_write_on_call = 2; /* fail child CTL2 timing write */
    apple_pcie_complete(&dev);
    assert(warn_calls == 1);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(link0.aspm_enabled == PCIE_LINK_STATE_L1);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    /* A policy recheck must NOT enable L1.2 against incomplete timing. */
    pcie_aspm_pm_state_change(&rp0, false);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    /* Subsequent D0 work retries timing restore, clears aspm_sleep_disable, and enables full L1SS. */
    fn0.fail_cfg_write_on_call = 0;
    apple_pcie_pm_state_change(&bridge, &fn0);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL2) >> 2] == 0x28);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* Core-managed link starting with L1SS disabled still installs sleep mask against mid-sleep enable. */
    RESET_L1SS(false);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, PCIE_LINK_STATE_L1, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    assert(!list_empty(&pcie.hib_l1ss) && link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000f0);
    apple_pcie_complete(&dev);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
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

    /* A link freed between queueing and running the work item is skipped, not dereferenced. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    rp0.aspm_pcipm_deferred = 1;
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

    /* Removal of the link between unlocked hint and aspm_lock must not be dereferenced. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    race_pdev = &rp0;
    race_link = &link0;
    aspm_lock_pre_hook = race_free_link_hook;
    pcie_aspm_pm_state_change(&rp0, false);
    assert(rp0.link_state == NULL && aspm_lock_pre_hook == NULL && !aspm_lock.held);

    /* Unmanaged D0 transition mid-restore during complete() is caught by post-publish D0 recheck. */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    race_pcie = &pcie;
    race_dev = &fn0;
    cfg_write_hook = race_d0_cfg_write_hook;
    apple_pcie_complete(&dev);
    assert(cfg_write_hook == NULL && fn0.current_state == PCI_D0);
    assert(queued_works == 1);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_pending));
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && !pcie.hib_l1ss_lock.held);

    /* Hotplugging a new L1SS link during the unlocked barrier window re-pins and drains it before clearing L1SS. */
    RESET_L1SS(true);
    list_del(&rp1.bus_list);
    hotplug_root_bus = &root_bus;
    hotplug_rp = &rp1;
    fn0.dev.rpm_inflight_cb = hotplug_during_barrier;
    assert(apple_pcie_prepare(&dev) == 0);
    assert(rp1.dev.rpm_usage == 1 && rp1.dev.rpm_barriers == 1);
    assert(ep1.dev.rpm_usage == 1 && ep1.dev.rpm_barriers == 1);
    assert(rp1.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x00000050);
    assert(ep1.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x00000050);
    apple_pcie_complete(&dev);
    assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
    assert(rp1.dev.rpm_usage == 0 && ep1.dev.rpm_usage == 0);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1 && rp1.refs == 1 && ep1.refs == 1);

    /* R4-1: Double-failed LNKCTL restore during normal complete() retains intended ASPM and recovers on retry. */
    RESET_L1SS(false);
    aspm_disabled = true;
    assert(apple_pcie_prepare(&dev) == 0);
    /* Resume policy kept L1 (0x0042) on rp0 and L0s+L1 (0x0043) on fn1. */
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 2; /* 1=clear, 2=first restore, 3=immediate retry */
    rp0.fail_lnkctl_write_count = 2;
    apple_pcie_complete(&dev);
    /* Both restore attempts failed so rp0 LNKCTL is temporarily cleared, but record is retained and queued. */
    assert(rp0.lnkctl == 0x0040);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 1);
    rp0.fail_lnkctl_write_on_call = 0;
    rp0.fail_lnkctl_write_count = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042 && fn1.lnkctl == 0x0043);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);

    /* R4-2: System resume_noirq / restore_noirq (pci_pm_default_resume_early) wakes deferred unmanaged + core work. */
    RESET_L1SS(false);
    aspm_disabled = true;
    fn0.current_state = PCI_D3hot;
    assert(apple_pcie_prepare(&dev) == 0);
    apple_pcie_complete(&dev);
    assert(!list_empty(&pcie.hib_l1ss_pending) && queued_works == 0);
    pci_pm_default_resume_early(&fn0);
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* R4-3: D3cold 0xffffffff reads in pci_save_aspm_l1ss_state_locked() must not corrupt valid saved timing. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    pci_save_aspm_l1ss_state(&fn0);
    assert(fn0.saved_l1ss.cap.data[0] == 0x28 && fn0.saved_l1ss.cap.data[1] == 0x000000ff);
    assert(rp0.saved_l1ss.cap.data[0] == 0x28 && rp0.saved_l1ss.cap.data[1] == 0x000000ff);
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0xffffffffu;
    pci_save_aspm_l1ss_state(&fn0);
    assert(fn0.saved_l1ss.cap.data[0] == 0x28 && fn0.saved_l1ss.cap.data[1] == 0x000000ff);
    assert(rp0.saved_l1ss.cap.data[0] == 0x28 && rp0.saved_l1ss.cap.data[1] == 0x000000ff);

    /* R4-4: Core-managed pcie_config_aspm_l1ss() child CTL1 write failure unwinds, re-arms, retains, and retries. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    /* During complete(): driver writes child CTL1(clear), CTL2, CTL1(timing) = 3 writes;
     * core pcie_config_aspm_l1ss() writes child CTL1(disable) = 4, child CTL1(enable) = 5. */
    fn0.cfg_write_calls = 0;
    fn0.fail_cfg_write_on_call = 5;
    apple_pcie_complete(&dev);
    /* Core unwound parent CTL1, re-armed aspm_sleep_disable, and driver retained record + queued worker. */
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 1);
    assert(queued_works == 1);
    fn0.fail_cfg_write_on_call = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0);
    assert(link0.aspm_enabled == ALL_L1_STATES && link0.aspm_hw_l1ss == PCIE_LINK_STATE_L1SS);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.refs == 1 && fn0.refs == 1);

    /* R4-5: Stale saved_l1ss restoring ASPM L1.1 when policy permits plain L1 only forces resync reprogramming. */
    RESET_L1SS(false);
    rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] = 0x000000f0;
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, PCIE_LINK_STATE_L1, PCIE_LINK_STATE_L1, 0);
    rp0.saved_l1ss.cap.data[0] = 0x28;
    rp0.saved_l1ss.cap.data[1] = 0x000000f0 | PCI_L1SS_CTL1_ASPM_L1_1;
    fn0.saved_l1ss.cap.data[0] = 0x28;
    fn0.saved_l1ss.cap.data[1] = 0x000000f0 | PCI_L1SS_CTL1_ASPM_L1_1;
    queued_works = 0;
    atomic_ctx_depth = 1;
    pci_restore_aspm_l1ss_state(&fn0);
    atomic_ctx_depth = 0;
    assert(rp0.aspm_l1ss_resync == 1 && rp0.aspm_pcipm_deferred == 1 && queued_works == 1);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == PCI_L1SS_CTL1_ASPM_L1_1);
    run_queued_work();
    assert(rp0.aspm_l1ss_resync == 0 && rp0.aspm_pcipm_deferred == 0);
    assert(link0.aspm_enabled == PCIE_LINK_STATE_L1 && link0.aspm_hw_l1ss == 0);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);

    /* R5-1: D3hot policy recheck after atomic restore masked PCI-PM must NOT re-enable PCI-PM in D3hot. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    pci_save_aspm_l1ss_state(&fn0);
    fn0.current_state = PCI_D3hot;
    atomic_ctx_depth = 1;
    pci_restore_aspm_l1ss_state(&fn0);
    atomic_ctx_depth = 0;
    assert(rp0.aspm_l1ss_resync == 1 && rp0.aspm_pcipm_deferred == 1);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & APPLE_PCIE_L1SS_PCIPM_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & APPLE_PCIE_L1SS_PCIPM_MASK) == 0);
    /* Policy recheck while fn0 is still in D3hot must keep PCI-PM off and keep aspm_pcipm_deferred armed. */
    pcie_aspm_pm_state_change(&rp0, false);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & APPLE_PCIE_L1SS_PCIPM_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & APPLE_PCIE_L1SS_PCIPM_MASK) == 0);
    assert(rp0.aspm_pcipm_deferred == 1);
    /* Once fn0 enters D0, deferred D0 notification restores PCI-PM L1.1/L1.2 cleanly. */
    queued_works = 0;
    pci_pm_default_resume_early(&fn0);
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    run_queued_work();
    assert(rp0.aspm_pcipm_deferred == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* R5-2: Transient CTL1 error in deferred D0 worker preserves aspm_pcipm_deferred for the next D0 notify. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    fn0.current_state = PCI_D3hot;
    apple_pcie_complete(&dev);
    assert(list_empty(&pcie.hib_l1ss_pending) && rp0.aspm_pcipm_deferred == 1);
    queued_works = 0;
    pci_pm_default_resume_early(&fn0);
    assert(fn0.current_state == PCI_D0 && queued_works == 1);
    /* Inject transient failure on child CTL1 enable write during deferred D0 worker. */
    fn0.cfg_write_calls = 0;
    fn0.fail_cfg_write_on_call = 2; /* 1=disable, 2=enable */
    run_queued_work();
    assert(rp0.aspm_l1ss_resync == 1 && rp0.aspm_pcipm_deferred == 1);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    /* Second D0 notification must still trigger worker and succeed once error clears. */
    fn0.fail_cfg_write_on_call = 0;
    queued_works = 0;
    pci_pm_default_resume_early(&fn0);
    assert(queued_works == 1);
    run_queued_work();
    assert(rp0.aspm_pcipm_deferred == 0 && rp0.aspm_l1ss_resync == 0);
    assert(link0.aspm_enabled == ALL_L1_STATES && link0.aspm_hw_l1ss == PCIE_LINK_STATE_L1SS);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);

    /* R5-3: Core LNKCTL double-failure after pcie_config_aspm_l1ss() succeeded unwinds CTL1 child-first under suppression. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0, ALL_L1_STATES, ALL_L1_STATES, ALL_L1_STATES, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    /* During complete(): driver writes rp0 LNKCTL 2x (clear+restore); core writes rp0 LNKCTL clear (3), restore (4), retry (5). */
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 4;
    rp0.fail_lnkctl_write_count = 2;
    apple_pcie_complete(&dev);
    /* Both CTL1 registers must be unwound to 0 while aspm_sleep_disable is re-armed, and driver retains record. */
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_L1SS_MASK) == 0);
    assert(link0.aspm_sleep_disable == PCIE_LINK_STATE_L1SS);
    assert(!list_empty(&pcie.hib_l1ss_pending) && queued_works == 1);
    rp0.fail_lnkctl_write_on_call = 0;
    rp0.fail_lnkctl_write_count = 0;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(link0.aspm_sleep_disable == 0 && link0.aspm_enabled == ALL_L1_STATES);
    assert(rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] == 0x000000ff);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042);

    /* R6-1: Preserve aspm_l1ss_resync across sleep_end() retry when endpoint enters D3hot after LNKCTL failure. */
    RESET_L1SS(false);
    ATTACH_LINK(link0, &rp0, &fn0,
                PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_2_PCIPM,
                PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_2_PCIPM,
                PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_2_PCIPM, 0);
    assert(apple_pcie_prepare(&dev) == 0);
    /* Fail core's parent LNKCTL restore (calls 4, 5, and unwind restore call 7) so rp0 ends at 0x0040. */
    rp0.lnkctl_write_calls = 0;
    rp0.fail_lnkctl_write_on_call = 4;
    rp0.fail_lnkctl_write_count = 4; /* 4=restore, 5=retry, 6=unwind clear, 7=unwind restore */
    apple_pcie_complete(&dev);
    assert(rp0.lnkctl == 0x0040 && rp0.aspm_l1ss_resync == 1);
    assert(!list_empty(&pcie.hib_l1ss_pending) && queued_works == 1);
    /* Endpoint enters D3hot before worker retry; masking PCI-PM leaves state == PCIE_LINK_STATE_L1 == aspm_enabled. */
    rp0.fail_lnkctl_write_on_call = 0;
    rp0.fail_lnkctl_write_count = 0;
    fn0.current_state = PCI_D3hot;
    run_queued_work();
    assert(list_empty(&pcie.hib_l1ss_pending) && atomic_read(&pcie.hib_l1ss_pending_cnt) == 0);
    assert(rp0.lnkctl == 0x0042 && fn0.lnkctl == 0x0042);
    assert(rp0.aspm_pcipm_deferred == 1 && rp0.aspm_l1ss_resync == 0);
    /* Returning to D0 completes PCI-PM L1.2 enable. */
    queued_works = 0;
    pci_pm_default_resume_early(&fn0);
    assert(queued_works == 1);
    run_queued_work();
    assert(rp0.aspm_pcipm_deferred == 0);
    assert((rp0.cfg[(0x150 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_PCIPM_L1_2) != 0);
    assert((fn0.cfg[(0x160 + PCI_L1SS_CTL1) >> 2] & PCI_L1SS_CTL1_PCIPM_L1_2) != 0);

    /* Sustained hotplug churn exhausting the 8-pass pin/drain limit returns -EBUSY (not -EAGAIN). */
    {
        struct pci_bus ch_sub[8];
        struct pci_dev ch_rp[8], ch_ep[8];
        RESET_L1SS(false);
        hotplug_root_bus = &root_bus;
        hotplug_chain_next = 0;
        for (int ci = 0; ci < 8; ci++) {
            list_init(&ch_sub[ci].devices);
            INIT_DEV(ch_rp[ci], (u8)(0x10 + ci * 8), 0x150, &ch_sub[ci], true, 0x106b, 0x0000005f, 0x14, 0x0042);
            INIT_DEV(ch_ep[ci], 0x00, 0x160, NULL, false, 0x14e4, 0x0000005f, 0x14, 0x0042);
            ch_rp[ci].bus = &root_bus;
            ch_ep[ci].bus = &ch_sub[ci];
            list_add_tail(&ch_ep[ci].bus_list, &ch_sub[ci].devices);
            ch_ep[ci].dev.rpm_inflight_cb = hotplug_chain_during_barrier;
            hotplug_chain_rps[ci] = &ch_rp[ci];
        }
        fn0.dev.rpm_inflight_cb = hotplug_chain_during_barrier;
        assert(apple_pcie_prepare(&dev) == -EBUSY);
        assert(hotplug_chain_next == 8 && radio_complete_calls == 1);
        assert(list_empty(&pcie.hib_l1ss) && list_empty(&pcie.hib_l1ss_rpm));
        assert(rp0.dev.rpm_usage == 0 && fn0.dev.rpm_usage == 0 && fn1.dev.rpm_usage == 0);
        assert(rp0.refs == 1 && fn0.refs == 1 && fn1.refs == 1);
        for (int ci = 0; ci < 8; ci++) {
            assert(ch_rp[ci].dev.rpm_usage == 0 && ch_ep[ci].dev.rpm_usage == 0);
            assert(ch_rp[ci].refs == 1 && ch_ep[ci].refs == 1);
        }
    }

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
static const struct apple_dpin_policy apple_dpin_m1, apple_dpin_m2;
struct apple_cio_hw {
    bool sleep_removes_domains;
    bool restart_on_wake;
    bool phy_shared_with_dwc3;
    bool psd_power_cycle;
    bool stop_at_reboot;
    bool sleep_router_on_stop;
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
    static const char *const t6020_compat[] = {
        "apple,t6020-usb4-acio", "apple,t8103-usb4-acio", NULL
    };
    const struct apple_cio_hw *t8122 = match_of(t8122_compat);
    const struct apple_cio_hw *t8103 = match_of(t8103_compat);
    const struct apple_cio_hw *t6020 = match_of(t6020_compat);
    assert(t8122 == &apple_cio_t8122_hw);
    assert(t8103 == &apple_cio_t8103_hw);
    assert(t6020 == &apple_cio_t6020_hw);
    assert(t8122->sleep_removes_domains && t8103->sleep_removes_domains);
    assert(t8122->restart_on_wake && t8103->restart_on_wake);
    assert(t8122->phy_shared_with_dwc3 && t8103->phy_shared_with_dwc3);
    assert(t8103->psd_power_cycle && t8103->stop_at_reboot && t8103->sleep_router_on_stop);
    assert(t8122->dp == &apple_dpin_m1 && t8103->dp == &apple_dpin_m1);

    struct apple_cio acio_t8122 = { .hw = t8122 };
    struct apple_cio acio_t8103 = { .hw = t8103 };
    struct apple_cio acio_t6020 = { .hw = t6020 };

    /* mem_sleep=deep + mem tears down T8103 and T8122 ACIO, leaves T6020 unchanged. */
    default_s2idle = false;
    no_platform = false;
    assert(apple_cio_sleep_removes_domains(&acio_t8122));
    assert(apple_cio_sleep_removes_domains(&acio_t8103));
    assert(!apple_cio_sleep_removes_domains(&acio_t6020));

    /* s2idle keeps the port up. */
    default_s2idle = true;
    no_platform = false;
    assert(!apple_cio_sleep_removes_domains(&acio_t8122));
    assert(!apple_cio_sleep_removes_domains(&acio_t8103));

    /* freeze keeps the port up even when mem_sleep=deep is the default. */
    default_s2idle = false;
    no_platform = true;
    assert(!apple_cio_sleep_removes_domains(&acio_t8122));
    assert(!apple_cio_sleep_removes_domains(&acio_t8103));

    /* Shared ATC PHY: ACIO PM notifier must run before DWC3's. */
    struct dwc3_apple dwc = { 0 };
    add_dwc(&dwc);
    add_cio(&acio_t8122);
    for (int i = 0; i < count; i++)
        assert(!chain[i]->notifier_call());
    assert(pci_off && phy_off);

    puts("PASS: T8103/T8122 ACIO deep-sleep teardown, freeze/s2idle preservation, and DWC3 PM order");
    return 0;
}
'''


def test_t8122_cio_sleep(out, acio):
    dwc = (ROOT / 'drivers/usb/dwc3/dwc3-apple.c').read_text()
    hw_defs = []
    for sym in ('apple_cio_t8103_hw', 'apple_cio_t8122_hw', 'apple_cio_t6020_hw'):
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
                   if c in ('apple,t8122-usb4-acio', 'apple,t6020-usb4-acio',
                            'apple,t8103-usb4-acio')]
    assert len(match_lines) == 3, match_lines
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
        't8103-drop-sleep-removes-domains': (
            'static const struct apple_cio_hw apple_cio_t8103_hw = {\n'
            '\t.sleep_removes_domains = true,\n',
            'static const struct apple_cio_hw apple_cio_t8103_hw = {\n'),
        't8122-drop-sleep-removes-domains': (
            'static const struct apple_cio_hw apple_cio_t8122_hw = {\n'
            '\t.sleep_removes_domains = true,\n',
            'static const struct apple_cio_hw apple_cio_t8122_hw = {\n'),
        't8122-drop-phy-shared': (
            'static const struct apple_cio_hw apple_cio_t8122_hw = {\n'
            '\t.sleep_removes_domains = true,\n'
            '\t.restart_on_wake = true,\n'
            '\t.phy_shared_with_dwc3 = true,\n',
            'static const struct apple_cio_hw apple_cio_t8122_hw = {\n'
            '\t.sleep_removes_domains = true,\n'
            '\t.restart_on_wake = true,\n'),
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


CIO_STOP_FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define ENODEV 19
#define EIO 5
#define READ_ONCE(x) (x)
#define lockdep_assert_held(l) ((void)(l))
#define scoped_guard(type, lock) if ((void)(lock), 1)
#define dev_err(dev, ...) ((void)(dev), errs++)
#define dev_warn(dev, ...) ((void)(dev))
#define readl(addr) ((void)(addr), 0u)
#define APPLE_CIO_M3_CTRL 0
#define APPLE_CIO_LSTX_CTRL_LEVEL 1u
struct device { int unused; };
struct platform_device { struct device dev; };
struct apple_cio_hw { bool sleep_router_on_stop; bool lstx_after_boot; };
struct apple_cio {
    const struct apple_cio_hw *hw;
    int lock, pcie_tunnel_lock, dp_wq;
    struct device *dev;
    struct platform_device *nhi_pdev;
    unsigned int current_cable_info, target_cable_info;
    bool pcie_tunnel_populated, pcie_quiesce_pending;
    bool resume_link_failed, link_lost_restart;
    void *rtk;
    char *rc_base;
};
static int errs, sleep_ret, sleeps, reopens, destroyed, depopulated;
static int poweroffs, freed, released;
static bool apple_cio_stop_pcie_tunnel(struct apple_cio *acio) { (void)acio; return true; }
static int apple_cio_sleep_router_for_stop(struct apple_cio *acio) { (void)acio; sleeps++; return sleep_ret; }
static void apple_cio_reopen_pcie_tunnel(struct apple_cio *acio, bool pcie) { (void)acio; assert(pcie); reopens++; }
static void of_platform_device_destroy(struct device *dev, void *data) { (void)dev; (void)data; destroyed++; }
static void flush_workqueue(int wq) { (void)wq; }
static void of_platform_depopulate(struct device *dev) { (void)dev; depopulated++; }
static int apple_rtkit_poweroff(void *rtk) { (void)rtk; poweroffs++; return 0; }
static void apple_rtkit_mailbox_off(void *rtk) { (void)rtk; }
static void apple_rtkit_free(void *rtk) { (void)rtk; freed++; }
static void apple_cio_cpu_halt(struct apple_cio *acio) { (void)acio; }
static void apple_cio_lstx_hold(struct apple_cio *acio, bool high) { (void)acio; (void)high; }
static void apple_cio_test_log_guards(struct apple_cio *acio) { (void)acio; }
static void apple_cio_release_domains(struct apple_cio *acio) { (void)acio; released++; }
/* PRODUCTION */
static struct device dev;
static struct platform_device nhi;
static int rtk;
static int stop(const struct apple_cio_hw *hw, int ret, enum apple_cio_awake awake,
                struct apple_cio *acio)
{
    *acio = (struct apple_cio){ .hw = hw, .dev = &dev, .nhi_pdev = &nhi,
                                .current_cable_info = 0x51, .rtk = &rtk };
    errs = sleeps = reopens = destroyed = depopulated = poweroffs = freed = released = 0;
    sleep_ret = ret;
    return apple_cio_stop(acio, awake);
}
int main(void)
{
    static const struct apple_cio_hw sleeps_first = { .sleep_router_on_stop = true };
    static const struct apple_cio_hw plain = { 0 };
    struct apple_cio acio;

    /* A router that sleeps: the complex goes off whatever the caller. */
    for (int awake = APPLE_CIO_AWAKE_KEEP; awake <= APPLE_CIO_AWAKE_FORCE; awake++) {
        assert(stop(&sleeps_first, 0, awake, &acio) == 0);
        assert(sleeps == 1 && destroyed == 1 && depopulated == 1 && !reopens);
        assert(poweroffs == 1 && freed == 1 && released == 1 && !acio.current_cable_info);
    }
    /* No sleep needed: unchanged. */
    assert(stop(&plain, -EIO, APPLE_CIO_AWAKE_KEEP, &acio) == 0);
    assert(!sleeps && destroyed == 1 && released == 1 && !acio.current_cable_info);

    /* Sleep or cable change: the port stays up with its NHI and domains. */
    assert(stop(&sleeps_first, -EIO, APPLE_CIO_AWAKE_KEEP, &acio) == -EIO);
    assert(reopens == 1 && !destroyed && !depopulated && !poweroffs && !freed && !released);
    assert(acio.current_cable_info == 0x51 && acio.rtk == &rtk && errs);

    /* Reboot: the children go, the co-processor and the domains stay. */
    assert(stop(&sleeps_first, -ENODEV, APPLE_CIO_AWAKE_HOLD, &acio) == -ENODEV);
    assert(!reopens && destroyed == 1 && depopulated == 1);
    assert(!poweroffs && !freed && !released && acio.rtk == &rtk && errs);

    /* Removal: the device and its domains go. */
    assert(stop(&sleeps_first, -EIO, APPLE_CIO_AWAKE_FORCE, &acio) == 0);
    assert(!reopens && destroyed == 1 && released == 1 && freed == 1 && errs);

    puts("PASS: ACIO stop keeps the port or its domains up for a router that does not sleep");
    return 0;
}
'''


def test_cio_stop(out, acio):
    enum = re.search(r'^enum apple_cio_awake \{.*?\n\};', acio, re.M | re.S)
    assert enum
    production = '\n'.join((enum.group(),
                            function(acio, 'apple_cio_remove_children'),
                            function(acio, 'apple_cio_stop')))
    code = CIO_STOP_FIXTURE.replace('/* PRODUCTION */', production)
    muts = {
        'cio-stop-ignore-sleep-error': (
            'ret = apple_cio_sleep_router_for_stop(acio);\n'
            '\t\tif (ret && awake == APPLE_CIO_AWAKE_KEEP) {',
            'ret = apple_cio_sleep_router_for_stop(acio);\n'
            '\t\tif (false) {'),
        'cio-stop-no-reopen': (
            'apple_cio_reopen_pcie_tunnel(acio, pcie);', '(void)pcie;'),
        'cio-stop-hold-powers-off': (
            'if (ret && awake == APPLE_CIO_AWAKE_HOLD) {', 'if (false) {'),
        'cio-stop-keep-powers-off': (
            'if (ret && awake == APPLE_CIO_AWAKE_KEEP) {\n'
            '\t\tdev_err(acio->dev, "the port stays up',
            'if (false) {\n'
            '\t\tdev_err(acio->dev, "the port stays up'),
    }
    cases = {'cio-stop-production': code}
    for name, (old, new) in muts.items():
        assert code.count(old) == 1, name
        cases[name] = code.replace(old, new)
    run_cases(out, cases, extra_flags=('-Wextra', '-Wno-unused-parameter'))


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
static void pcie_aspm_restore_l1ss_mask(struct pci_dev *parent,
                                        struct pci_dev *pdev,
                                        u32 *pl_ctl1, u32 *cl_ctl1);
static void pcie_aspm_restore_l1ss_sync(struct pci_dev *parent,
                                        struct pci_dev *pdev,
                                        u32 pl_ctl1, u32 cl_ctl1);
static void pcie_aspm_pm_state_change_deferred(struct pci_dev *pdev);
static void apple_pcie_hib_l1ss_work_fn(struct work_struct *work);
'''
    aspm_prod = '\n'.join((aspm_struct, aspm_helpers,
                           *(function(aspm, n) for n in (
                               'pcie_aspm_l1ss_hw_lock_irqsave',
                               'pcie_aspm_l1ss_hw_unlock_irqrestore',
                               'pci_save_aspm_l1ss_state_locked',
                               'pci_save_aspm_l1ss_state',
                               'pci_restore_aspm_l1ss_state_locked',
                               'pci_restore_aspm_l1ss_state',
                               'pcie_aspm_l1ss_ctl1_to_state',
                               'pcie_aspm_restore_l1ss_mask',
                               'pcie_aspm_restore_l1ss_sync',
                               'pcie_config_aspm_dev',
                               'pcie_l1ss_ctl1_update',
                               'pcie_config_aspm_l1ss',
                               'pcie_config_aspm_link_locked',
                               'pcie_config_aspm_link',
                               'pcie_config_aspm_path',
                               'pcie_aspm_pm_state_change',
                               'pcie_aspm_pm_work_fn',
                               'pcie_aspm_init_dev',
                               'pcie_aspm_pm_state_change_deferred',
                               'pcie_aspm_l1ss_hw_exec',
                               'pcie_aspm_l1ss_sleep_begin_locked',
                               'pcie_aspm_l1ss_sleep_end')),
                           function(pci_core, 'pci_pm_notify_host'),
                           function(pci_core, 'pci_pm_notify_state_change'),
                           function(pci_core, 'pci_pm_notify_resume'),
                           function(pci_core, 'pci_set_low_power_state'),
                           function(pci_core, 'pci_save_pcie_state'),
                           function(pci_core, 'pci_restore_pcie_state'),
                           function(pci_core, 'pci_pm_reset'),
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
                                 'apple_pcie_hib_link_present',
                                 'apple_pcie_hib_rpm_add',
                                 'apple_pcie_hib_rpm_pin_cb',
                                 'apple_pcie_hib_rpm_release',
                                 'apple_pcie_hib_restore_aspm',
                                 'apple_pcie_link_set_l1ss_hw',
                                 'apple_pcie_link_set_l1ss',
                                 'apple_pcie_hib_disable_l1ss_hw',
                                 'apple_pcie_hib_disable_l1ss_link',
                                 'apple_pcie_hib_restore_l1ss_link',
                                 'apple_pcie_hib_disable_l1ss_cb',
                                 'apple_pcie_hib_free_l1ss',
                                 'apple_pcie_hib_retain_failed',
                                 'apple_pcie_hib_restore_l1ss_locked',
                                 'apple_pcie_hib_restore_l1ss',
                                 'apple_pcie_hib_l1ss_work_fn',
                                 'apple_pcie_pm_state_change',
                                 'apple_pcie_hib_l1ss_pci_notify',
                                 'apple_pcie_hib_recheck_l1ss_hw',
                                 'apple_pcie_hib_recheck_l1ss_link',
                                 'apple_pcie_hib_recheck_l1ss_cb',
                                 'apple_pcie_hib_recheck_l1ss',
                                 'apple_pcie_s2r_reinit',
                                 'apple_pcie_prepare',
                                 'apple_pcie_suspend',
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
        'drop-suspend-l1ss-recheck': (
            'if (pcie && apple_pcie_s2r_reinit(pcie) &&\n'
            '\t    pm_suspend_target_state != PM_SUSPEND_TO_IDLE &&\n'
            '\t    pm_suspend_target_state != PM_SUSPEND_ON)\n'
            '\t\treturn apple_pcie_hib_recheck_l1ss(pcie);',
            'if (false)\n'
            '\t\treturn apple_pcie_hib_recheck_l1ss(pcie);'),
        'drop-recheck-aspm-restore': (
            '\tif (ret && hw->restore_aspm_on_err)\n'
            '\t\tapple_pcie_hib_restore_aspm(parent, saved);',
            '\tif (false)\n'
            '\t\tapple_pcie_hib_restore_aspm(parent, saved);'),
        'drop-recheck-vid-err': (
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret) {\n'
            '\t\twalk->err = ret;\n'
            '\t\treturn 1;\n'
            '\t}\n'
            '\tif (id == U32_MAX)\n'
            '\t\treturn 0;\n\n'
            '\trctx = (struct apple_pcie_l1ss_recheck_ctx){',
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret || id == U32_MAX)\n'
            '\t\treturn 0;\n\n'
            '\trctx = (struct apple_pcie_l1ss_recheck_ctx){'),
        'drop-pcipm-d0-mask': (
            '\t\t\tif (((pl_ctl1 | cl_ctl1) & APPLE_PCIE_L1SS_PCIPM_MASK) &&\n'
            '\t\t\t    (READ_ONCE(parent->current_state) != PCI_D0 ||\n'
            '\t\t\t     READ_ONCE(child->current_state) != PCI_D0)) {',
            '\t\t\tif (false) {'),
        'drop-live-ctl1-clear': (
            'ret = apple_pcie_cfg_write_u32(child, child->l1ss + PCI_L1SS_CTL1,\n'
            '\t\t\t\t\t       cur_cl_ctl1 & ~PCI_L1SS_CTL1_L1SS_MASK);',
            'ret = apple_pcie_cfg_write_u32(child, child->l1ss + PCI_L1SS_CTL1,\n'
            '\t\t\t\t\t       cl_ctl1 & ~PCI_L1SS_CTL1_L1SS_MASK);'),
        'drop-partial-enable-unwind': (
            'unwind_l1ss:\n'
            '\t/* Keep both ends symmetrically disabled (child-first) if enable failed partway. */\n'
            '\tapple_pcie_cfg_write_u32(child, child->l1ss + PCI_L1SS_CTL1,\n'
            '\t\t\t\t cl_ctl1 & ~PCI_L1SS_CTL1_L1SS_MASK);\n'
            '\tapple_pcie_cfg_write_u32(parent, parent->l1ss + PCI_L1SS_CTL1,\n'
            '\t\t\t\t pl_ctl1 & ~PCI_L1SS_CTL1_L1SS_MASK);',
            'unwind_l1ss:'),
        'drop-restore-rescan-lock': (
            '\tpci_lock_rescan_remove();\n'
            '\tapple_pcie_hib_restore_l1ss_locked(pcie, rollback);',
            '\tapple_pcie_hib_restore_l1ss_locked(pcie, rollback);'),
        'drop-restore-link-present': (
            '\tif (!apple_pcie_hib_link_present(parent, child))\n'
            '\t\treturn -ENODEV;',
            '\tif (!parent->subordinate)\n'
            '\t\treturn -ENODEV;'),
        'drop-restore-vid-err-retain': (
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret)\n'
            '\t\treturn ret == -ENODEV ? -EIO : ret;',
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret)\n'
            '\t\treturn -ENODEV;'),
        'drop-config-enodev-remap': (
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret)\n'
            '\t\treturn ret == -ENODEV ? -EIO : ret;\n'
            '\tif (id == U32_MAX)',
            '\tret = pcibios_err_to_errno(pci_read_config_dword(child, PCI_VENDOR_ID, &id));\n'
            '\tif (ret)\n'
            '\t\treturn ret;\n'
            '\tif (id == U32_MAX)'),
        'drop-d3cold-ff-retain': (
            '\tif (id == U32_MAX)\n'
            '\t\treturn -EAGAIN;\n\n'
            '\t/*',
            '\tif (id == U32_MAX)\n'
            '\t\treturn -ENODEV;\n\n'
            '\t/*'),
        'drop-d0-timing-requeue': (
            '\t\t\tsmp_mb__after_atomic();\n'
            '\t\t\tif (READ_ONCE(saved->parent->current_state) == PCI_D0 &&\n'
            '\t\t\t    READ_ONCE(saved->child->current_state) == PCI_D0)\n'
            '\t\t\t\trequeue_d0 = true;',
            '\t\t\t(void)requeue_d0;'),
        'drop-worker-rollback-intent': (
            '\t\tctx = (struct apple_pcie_l1ss_restore_ctx){\n'
            '\t\t\t.saved = saved,\n'
            '\t\t\t.rollback = saved->rollback_pending,\n'
            '\t\t\t.from_work = true,\n'
            '\t\t};',
            '\t\tctx = (struct apple_pcie_l1ss_restore_ctx){\n'
            '\t\t\t.saved = saved,\n'
            '\t\t\t.from_work = true,\n'
            '\t\t};'),
        'drop-rollback-retain-timing': (
            '\tif (!READ_ONCE(pcie->hib_l1ss_closing)) {\n'
            '\t\tsaved->timing_pending = true;\n'
            '\t\tif (ctx->rollback)\n'
            '\t\t\tsaved->rollback_pending = true;',
            '\tif (!ctx->rollback && !READ_ONCE(pcie->hib_l1ss_closing)) {\n'
            '\t\tsaved->timing_pending = true;\n'
            '\t\tif (ctx->rollback)\n'
            '\t\t\tsaved->rollback_pending = true;'),
        'drop-core-d0-check': (
            '\tif (READ_ONCE(parent->current_state) != PCI_D0 ||\n'
            '\t    READ_ONCE(child->current_state) != PCI_D0) {\n'
            '\t\tu32 deferred_state;',
            '\tif (false) {\n'
            '\t\tu32 deferred_state;'),
        'copy-stale-pcipm-on-d3hot-resync': (
            '\t\tdeferred_state = state & ~PCIE_LINK_STATE_L1_SS_PCIPM;\n'
            '\t\tif (wanted & PCIE_LINK_STATE_L1_SS_PCIPM) {',
            '\t\tdeferred_state = (state & ~PCIE_LINK_STATE_L1_SS_PCIPM) |\n'
            '\t\t\t\t (link->aspm_enabled & PCIE_LINK_STATE_L1_SS_PCIPM);\n'
            '\t\tif ((wanted & PCIE_LINK_STATE_L1_SS_PCIPM) & ~deferred_state) {'),
        'drop-worker-err-rearm-deferred': (
            '\t\tif (!(READ_ONCE(link->aspm_sleep_disable) &\n'
            '\t\t      PCIE_LINK_STATE_L1SS))\n'
            '\t\t\tWRITE_ONCE(parent->aspm_pcipm_deferred, 1);',
            '\t\tif (false)\n'
            '\t\t\tWRITE_ONCE(parent->aspm_pcipm_deferred, 1);'),
        'drop-lnkctl-err-l1ss-unwind': (
            '\t\tif (l1ss_programmed) {',
            '\t\tif (false && l1ss_programmed) {'),
        'clear-resync-before-sleep-end-retry': (
            '\t\tlink->aspm_enabled &= ~PCIE_LINK_STATE_L1SS;\n'
            '\t\tWRITE_ONCE(link->aspm_hw_l1ss, 0);\n'
            '\t\tif (!ret) {',
            '\t\tlink->aspm_enabled &= ~PCIE_LINK_STATE_L1SS;\n'
            '\t\tWRITE_ONCE(link->aspm_hw_l1ss, 0);\n'
            '\t\tWRITE_ONCE(pdev->aspm_l1ss_resync, 0);\n'
            '\t\tif (!ret) {'),
        'drop-aspm-sleep-disable-mask': (
            'state &= (link->aspm_capable & ~link->aspm_disable &\n'
            '\t\t  ~READ_ONCE(link->aspm_sleep_disable));',
            'state &= (link->aspm_capable & ~link->aspm_disable);'),
        'drop-default-resume-early-notify': (
            '\tpci_pme_restore(pci_dev);\n'
            '\tpci_pm_notify_resume(pci_dev);',
            '\tpci_pme_restore(pci_dev);'),
        'sync-default-resume-early-notify': (
            '\tpci_pme_restore(pci_dev);\n'
            '\tpci_pm_notify_resume(pci_dev);',
            '\tpci_pme_restore(pci_dev);\n'
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
        'drop-restore-l1ss-sync': (
            '\tpcie_aspm_restore_l1ss_sync(parent, pdev, pl_ctl1 | pl_l1_2_enable,\n'
            '\t\t\t\t    cl_ctl1 | cl_l1_2_enable);',
            ''),
        'drop-restore-l1ss-mask': (
            '\tpcie_aspm_restore_l1ss_mask(parent, pdev, &pl_ctl1, &cl_ctl1);',
            ''),
        'drop-deferred-flag-gate': (
            '\tif (aspm_disabled || !READ_ONCE(pdev->aspm_pcipm_deferred))\n'
            '\t\treturn;',
            '\tif (aspm_disabled || !READ_ONCE(pdev->link_state))\n'
            '\t\treturn;'),
        'drop-freezable-wq': (
            '\tif (!queue_work(system_freezable_wq, &pdev->aspm_pm_work))',
            '\tif (!queue_work(system_dfl_wq, &pdev->aspm_pm_work))'),
        'clear-sleep-disable-on-timing-err': (
            '\t\t} else {\n'
            '\t\t\tWRITE_ONCE(link->aspm_sleep_disable,\n'
            '\t\t\t\t   READ_ONCE(link->aspm_sleep_disable) |\n'
            '\t\t\t\t   PCIE_LINK_STATE_L1SS);',
            '\t\t} else {\n'
            '\t\t\tWRITE_ONCE(link->aspm_sleep_disable,\n'
            '\t\t\t\t   READ_ONCE(link->aspm_sleep_disable) &\n'
            '\t\t\t\t   ~PCIE_LINK_STATE_L1SS);'),
        'drop-retain-timing-pending': (
            '\tif (!READ_ONCE(pcie->hib_l1ss_closing)) {\n'
            '\t\tsaved->timing_pending = true;',
            '\tif (false) {\n'
            '\t\tsaved->timing_pending = true;'),
        'drop-notify-purge-hib-l1ss': (
            '\tlist_for_each_entry_safe(saved, tmp, &pcie->hib_l1ss, entry) {\n'
            '\t\tif (saved->parent != pdev && saved->child != pdev)\n'
            '\t\t\tcontinue;\n'
            '\t\tlist_del_init(&saved->entry);\n'
            '\t\tpci_dev_put(saved->child);\n'
            '\t\tpci_dev_put(saved->parent);\n'
            '\t\tkfree(saved);\n'
            '\t}',
            ''),
        'drop-work-rescan-lock': (
            '\tpci_lock_rescan_remove();\n'
            '\tfor (;;) {\n'
            '\t\tstruct pci_dev *parent, *child;\n'
            '\t\tstruct apple_pcie_l1ss_restore_ctx ctx;\n\n'
            '\t\t/*',
            '\tfor (;;) {\n'
            '\t\tstruct pci_dev *parent, *child;\n'
            '\t\tstruct apple_pcie_l1ss_restore_ctx ctx;\n\n'
            '\t\t/*'),
        'drop-retry-pending-cnt': (
            '\t\tsaved = list_first_entry_or_null(&pcie->hib_l1ss_pending,\n'
            '\t\t\t\t\t\t struct apple_pcie_hib_l1ss,\n'
            '\t\t\t\t\t\t entry);\n'
            '\t\tif (saved)\n'
            '\t\t\tlist_del_init(&saved->entry);\n'
            '\t\tmutex_unlock(&pcie->hib_l1ss_lock);',
            '\t\tsaved = list_first_entry_or_null(&pcie->hib_l1ss_pending,\n'
            '\t\t\t\t\t\t struct apple_pcie_hib_l1ss,\n'
            '\t\t\t\t\t\t entry);\n'
            '\t\tif (saved) {\n'
            '\t\t\tlist_del_init(&saved->entry);\n'
            '\t\t\tatomic_dec(&pcie->hib_l1ss_pending_cnt);\n'
            '\t\t}\n'
            '\t\tmutex_unlock(&pcie->hib_l1ss_lock);'),
        'drop-pmcsr-hw-lock': (
            '\tflags = pcie_aspm_l1ss_hw_lock_irqsave();\n'
            '\tWRITE_ONCE(dev->current_state, state);\n'
            '\tpci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);\n'
            '\tpcie_aspm_l1ss_hw_unlock_irqrestore(flags);',
            '\t(void)flags;\n'
            '\tpci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);'),
        'drop-pm-reset-d0-interlock': (
            '\tflags = pcie_aspm_l1ss_hw_lock_irqsave();\n'
            '\tWRITE_ONCE(dev->current_state, PCI_D3hot);\n'
            '\tcsr &= ~PCI_PM_CTRL_STATE_MASK;\n'
            '\tcsr |= PCI_D3hot;\n'
            '\tpci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);\n'
            '\tpcie_aspm_l1ss_hw_unlock_irqrestore(flags);',
            '\t(void)flags;\n'
            '\tcsr &= ~PCI_PM_CTRL_STATE_MASK;\n'
            '\tcsr |= PCI_D3hot;\n'
            '\tpci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);'),
        'drop-save-pcie-hw-lock': (
            '\tflags = pcie_aspm_l1ss_hw_lock_irqsave();\n'
            '\tpcie_capability_read_word(dev, PCI_EXP_LNKCTL, &cap[i++]);\n'
            '\tpci_save_aspm_l1ss_state_locked(dev);\n'
            '\tpcie_aspm_l1ss_hw_unlock_irqrestore(flags);',
            '\t(void)flags;\n'
            '\tpcie_capability_read_word(dev, PCI_EXP_LNKCTL, &cap[i++]);\n'
            '\tpci_save_aspm_l1ss_state(dev);'),
        'drop-restore-pcie-hw-lock': (
            '\tpcie_capability_write_word(dev, PCI_EXP_DEVCTL, cap[i++]);\n'
            '\tpcie_capability_write_word(dev, PCI_EXP_LNKCTL, cap[i++]);\n'
            '\tpcie_aspm_l1ss_hw_unlock_irqrestore(flags);',
            '\tpcie_aspm_l1ss_hw_unlock_irqrestore(flags);\n'
            '\tpcie_capability_write_word(dev, PCI_EXP_DEVCTL, cap[i++]);\n'
            '\tpcie_capability_write_word(dev, PCI_EXP_LNKCTL, cap[i++]);'),
        'mutex-in-save-aspm-l1ss': (
            '\tspin_lock_irqsave(&aspm_l1ss_hw_lock, flags);\n'
            '\tpci_save_aspm_l1ss_state_locked(pdev);\n'
            '\tspin_unlock_irqrestore(&aspm_l1ss_hw_lock, flags);',
            '\t(void)flags;\n'
            '\tmutex_lock(&aspm_lock);\n'
            '\tpci_save_aspm_l1ss_state_locked(pdev);\n'
            '\tmutex_unlock(&aspm_lock);'),
        'drop-disable-hw-lock': (
            '\tret = pcie_aspm_l1ss_hw_exec(child, true, apple_pcie_hib_disable_l1ss_hw,\n'
            '\t\t\t\t     &dhw);',
            '\tret = apple_pcie_hib_disable_l1ss_hw(&dhw);'),
        'drop-complete-parent-aspm-retry': (
            '\t\tif (r && enable)\n'
            '\t\t\tr = apple_pcie_lnkctl_write(parent, restore_plnkctl);',
            '\t\tif (false)\n'
            '\t\t\tr = apple_pcie_lnkctl_write(parent, restore_plnkctl);'),
        'drop-complete-fn-aspm-retry': (
            '\t\t\tif (r && enable)\n'
            '\t\t\t\tr = apple_pcie_lnkctl_write(fns[i], restore_fn_lnkctl[i]);',
            '\t\t\tif (false)\n'
            '\t\t\t\tr = apple_pcie_lnkctl_write(fns[i], restore_fn_lnkctl[i]);'),
        'drop-complete-aspm-lost-save': (
            '\tif (hw.aspm_lost) {\n'
            '\t\tsaved->pl_lnkctl = hw.restore_plnkctl;',
            '\tif (false) {\n'
            '\t\tsaved->pl_lnkctl = hw.restore_plnkctl;'),
        'drop-save-l1ss-validate': (
            '\t    PCI_POSSIBLE_ERROR(cl_ctl2) || PCI_POSSIBLE_ERROR(cl_ctl1) ||\n'
            '\t    PCI_POSSIBLE_ERROR(pl_ctl2) || PCI_POSSIBLE_ERROR(pl_ctl1))',
            '\t    false)'),
        'drop-core-enable-err-retain': (
            '\t\tif (ret && ret != -ENODEV && !ctx.keep_pending)\n'
            '\t\t\tapple_pcie_hib_retain_failed(pcie, saved, &ctx, ret);\n'
            '\t\tif (!ctx.keep_pending) {',
            '\t\t(void)ret;\n'
            '\t\tif (!ctx.keep_pending) {'),
        'drop-core-l1ss-err-rearm': (
            '\t\t\tif (ret) {\n'
            '\t\t\t\tif (ret == -ENODEV)\n'
            '\t\t\t\t\tret = -EIO;\n'
            '\t\t\t\tWRITE_ONCE(link->aspm_sleep_disable,\n'
            '\t\t\t\t\t   READ_ONCE(link->aspm_sleep_disable) |\n'
            '\t\t\t\t\t   PCIE_LINK_STATE_L1SS);\n'
            '\t\t\t\tWRITE_ONCE(pdev->aspm_pcipm_deferred, 0);\n'
            '\t\t\t}',
            '\t\t\tif (false) {\n'
            '\t\t\t\tWRITE_ONCE(link->aspm_sleep_disable,\n'
            '\t\t\t\t\t   READ_ONCE(link->aspm_sleep_disable) |\n'
            '\t\t\t\t\t   PCIE_LINK_STATE_L1SS);\n'
            '\t\t\t\tWRITE_ONCE(pdev->aspm_pcipm_deferred, 0);\n'
            '\t\t\t}'),
        'drop-resync-force-reprogram': (
            '\tif (!resync && link->aspm_enabled == state)\n'
            '\t\treturn 0;',
            '\tif (link->aspm_enabled == state)\n'
            '\t\treturn 0;'),
        'drop-hib-rpm-get': (
            '\t\t\t\tpci_walk_bus(bridge->bus,\n'
            '\t\t\t\t\t     apple_pcie_hib_rpm_pin_cb, &walk);',
            '\t\t\t\t(void)rpm;'),
        'drop-hib-rpm-barrier': (
            '\t\t\t\t\tif (!rpm->drained) {\n'
            '\t\t\t\t\t\tpm_runtime_barrier(&rpm->pdev->dev);\n'
            '\t\t\t\t\t\trpm->drained = true;\n'
            '\t\t\t\t\t}',
            '\t\t\t\t\trpm->drained = true;'),
        'drop-hib-rpm-recheck-loop': (
            '\t\t\t\tif (!walk.added)\n'
            '\t\t\t\t\tbreak;',
            '\t\t\t\tif (tries > 0 || !walk.added)\n'
            '\t\t\t\t\tbreak;'),
        'exhaust-returns-eagain': (
            '\t\t\tif (walk.added) {\n'
            '\t\t\t\tapple_pcie_hib_rpm_release(pcie);\n'
            '\t\t\t\tapple_radio_complete(dev);\n'
            '\t\t\t\treturn -EBUSY;\n'
            '\t\t\t}',
            '\t\t\tif (walk.added) {\n'
            '\t\t\t\tapple_pcie_hib_rpm_release(pcie);\n'
            '\t\t\t\tapple_radio_complete(dev);\n'
            '\t\t\t\treturn -EAGAIN;\n'
            '\t\t\t}'),
        'drop-recheck-keep-snapshot-on-err': (
            '\tif (rhw.had_l1ss) {\n'
            '\t\tif (!saved) {',
            '\tif (!ret && rhw.had_l1ss) {\n'
            '\t\tif (!saved) {'),
        'drop-d0-recheck-after-publish': (
            '\t\t\t/* Pair with smp_mb() in pci_pm_notify_host(). */\n'
            '\t\t\tsmp_mb__after_atomic();\n'
            '\t\t\tif (READ_ONCE(parent->current_state) == PCI_D0 &&\n'
            '\t\t\t    READ_ONCE(child->current_state) == PCI_D0)\n'
            '\t\t\t\trequeue_d0 = true;',
            ''),
        'drop-zero-l1ss-sleep-suppress': (
            '\tif (!((saved->pl_ctl1 | saved->cl_ctl1) & PCI_L1SS_CTL1_L1SS_MASK)) {\n'
            '\t\tsaved->active = dhw->core_managed || dhw->pend_rollback;',
            '\tif (!((saved->pl_ctl1 | saved->cl_ctl1) & PCI_L1SS_CTL1_L1SS_MASK)) {\n'
            '\t\tsaved->active = false;'),
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
        test_cio_stop(out, acio)
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
