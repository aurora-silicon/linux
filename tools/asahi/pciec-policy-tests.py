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
    start = re.search(r'^(?:static\s+)?(?:bool|int|void)\s+' + name + r'\(',
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
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define READ_ONCE(x) (x)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define GENPD_NOTIFY_PRE_OFF 0
#define GENPD_NOTIFY_ON 1
#define PM_SUSPEND_ON 0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
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
struct device {
    struct apple_pcie *pcie;
    struct notifier_block *genpd_nb;
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
static void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}
static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    entry->next = head;
    entry->prev = head->prev;
    head->prev->next = entry;
    head->prev = entry;
}
struct apple_pcie_port {
    struct list_head entry;
    unsigned int idx;
    bool link_failed;
    u8 regs[0x900];
};
struct apple_pcie {
    struct device *dev;
    const struct hw_info *hw;
    u8 *base;
    struct list_head ports;
    bool power_retained, kernel_init, tunnel_cold_init;
    bool bus_stopped, resume_failed, link_kept;
    struct apple_pcie_pd *pds;
    unsigned int num_pds, pd_held;
    struct apple_dart_cio *cio_dart;
    bool cio_prepared, neo_sleep_armed, t6050_down;
    unsigned int managed_sids;
    void *t6050_port, *t6050_dart;
    u32 t6050_parked;
};
static bool s2idle_keep_link = true;
static int pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
static bool suspend_via_firmware;
static bool link_healthy = true;
static unsigned int radio_prepare_calls, warn_calls;
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
    s2idle_keep_link = true;
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
    suspend_via_firmware = false;
    link_healthy = true;
    radio_prepare_calls = 0;
    warn_calls = 0;
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

    puts("PASS: T6050 PCIe-C s2idle domain hold, release, deep sleep and prepare refusal");
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
    acio = (ROOT / 'drivers/thunderbolt/apple.c').read_text()
    defines = '\n'.join(re.findall(r'^#define (?:[^\n]*\\\n)*[^\n]*$', pci, re.M))
    info = pci[pci.index('struct hw_info {'):pci.index('static const struct of_device_id apple_pcie_of_match[];')]
    matches = re.search(r'static const struct of_device_id apple_pcie_of_match\[\] = \{.*?\n\};', pci, re.S)[0]
    production = '\n'.join((defines, info, matches,
                            function(pci, 'apple_pcie_tunnel_needs_cold_init'),
                            function(acio, 'apple_cio_pciec_eligible'),
                            function(acio, 'apple_cio_pcie_tunnel_is_preinitialized')))
    code = FIXTURE.replace('/* PRODUCTION */', production)
    s2idle_prod = '\n'.join((defines, info, matches,
                             *(function(pci, n) for n in (
                                 'apple_pcie_pd_notify',
                                 'apple_pcie_release_domains',
                                 'apple_pcie_hold_domains',
                                 'apple_pcie_can_keep_link',
                                 'apple_pcie_keep_link',
                                 'apple_pcie_adopted_links_ready',
                                 'apple_pcie_adopted_sleep_host',
                                 'apple_pcie_prepare'))))
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
    }
    s2idle_cases = {'s2idle-production': s2idle_code}
    for name, (old, new) in s2idle_muts.items():
        assert s2idle_code.count(old) == 1, name
        s2idle_cases[name] = s2idle_code.replace(old, new)
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
