#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise actual radio SID admission, publication, failure and group code.

These CPU tests do not qualify high-address firmware DMA or radio operation.
"""
from pathlib import Path
import subprocess
import resource
import tempfile


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    root = Path(__file__).resolve().parents[5]
    pci = (root / 'drivers/pci/controller/pcie-apple.c').read_text()
    dart = (root / 'drivers/iommu/apple-dart.c').read_text()
    code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef u64 phys_addr_t;
typedef unsigned long atomic_long_t;
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define SZ_16K 16384
#define GFP_KERNEL 0
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_MMIO 4
#define APPLE_DART2 1
#define DART_T8110_DISABLE_STREAMS 0xc20
#define WRITE_ONCE(x,v) ((x) = (v))
#define READ_ONCE(x) (x)
#define dev_err(dev,...) do { (void)(dev); if (false) fprintf(stderr,__VA_ARGS__); } while (0)
#define dev_warn(dev,...) do { (void)(dev); if (false) fprintf(stderr,__VA_ARGS__); } while (0)
#define dev_err_probe(dev, error, ...) (error)
#define dev_err_ratelimited(...) ((void)0)
#define dev_dbg(dev,...) do { (void)(dev); if (false) fprintf(stderr,__VA_ARGS__); } while (0)
#define dev_info(dev,...) do { (void)(dev); if (false) fprintf(stderr,__VA_ARGS__); } while (0)
#define WARN_ON(x) assert(!(x))
#define BITS_TO_LONGS(n) (((n) + 63) / 64)
#define DART_MAX_STREAMS 256
#define MAX_DARTS_PER_DEVICE 2
#define U32_MAX UINT32_MAX
#define DART_T8110 2
#define DART_T8110_DIAG_LOCK 0x700
#define DART_T8110_DIAG_LOCK_ON_ERR 1
#define DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK 0x200
#define DART_T8110_TCR_BYPASS_DAPF BIT(2)
#define DART_T8110_TCR_BYPASS_DART BIT(1)
#define DART_T8110_TCR_TRANSLATE_ENABLE BIT(0)
#define DART_T8110_TCR_REMAP 0xf00
#define DART_T8110_TCR_REMAP_ENABLE BIT(7)
#define DART_T8110_PROTECT 0x200
#define DART_T8110_PROTECT_LOCK 0x208
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)
#define DART_T8110_ENABLE_STREAMS 0xc00
#define DART_T8110_TLB_CMD 0x80
#define DART_T8110_TLB_CMD_BUSY BIT(31)
#define DART_T8110_ERROR 0x100
#define DART_T8110_ERROR_MASK 0x104
#define DART_T8110_ERROR_FLAG BIT(31)
#define DART_T8110_ERROR_STREAMS 0x1c0
#define FIELD_GET(mask,v) (((v)&(mask)) >> __builtin_ctzl(mask))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define EPROBE_DEFER 517
#define KUNIT_STATIC_STUB_REDIRECT(...) do {} while (0)
#define BITS_TO_U32(n) (((n) + 31) / 32)
#define bitmap_zero(p,n) memset((p),0,BITS_TO_LONGS(n)*sizeof(unsigned long))
#define bitmap_set(p,s,n) do { for (int b=(s); b<(int)((s)+(n)); b++) __set_bit(b,p); } while(0)
#define PORT_RID2SID_VALID BIT(31)
#define PORT_RID2SID_SID_SHIFT 16
#define DART_TCR(d,s) (0x1000 + 4 * (s))
#define DART_TTBR(d,s,i) (0x1400 + 4 * (s) + 0 * (i))
#define for_each_set_bit(n,p,max) for ((n)=0; (n)<(int)(max); (n)++) if ((p)[(n)/64] & BIT((n)%64))
#define __set_bit(n,p) ((p)[(n)/64] |= BIT((n)%64))
#define for_each_set_bit_from(n,p,max) for (; (n)<(int)(max); (n)++) if ((p)[(n)/64] & BIT((n)%64))
#define lockdep_assert_held(lock) assert(*(lock))
#define spin_lock_irqsave(lock,flags) do { assert(!*(lock)); *(lock)=true; (flags)=0; } while (0)
#define spin_unlock_irqrestore(lock,flags) do { assert(*(lock)); *(lock)=false; (void)(flags); } while (0)
struct apple_dart;
struct apple_dart_stream_map;
struct apple_dart_hw {
    int type;
    u32 tcr_disabled, tcr_bypass, ttbr_valid, enable_streams;
    int ttbr_count;
    u32 tcr_enabled, tcr_4level, ttbr_shift, ttbr_addr_field_shift;
    void (*enable_remap)(struct apple_dart *, int, int);
    int (*invalidate_tlb)(struct apple_dart_stream_map *);
    bool gen3;
};
struct apple_dart {
    const struct apple_dart_hw *hw;
    u16 managed_sids, version;
    bool locked, managed_failed, managed_lock, retained, retained_mtp, supports_bypass;
    bool four_level, per_stream, tunneled, fw_mirror, fw_handoff;
    bool pending, preserve_display;
    bool isp_host, cold_ane, live_mtp, display_dark, display_tlb_stale;
    unsigned int display_writers, display_dark_writers;
    bool cold_gen3, cold_gen3_pcie;
    u32 cold_gen3_sids;
    void *tunables;
    unsigned long static_bypass[4];
    void *dcp, *frontend[2];
    struct { bool ready; } iommu;
    void *dev;
    unsigned pgsize, oas;
    int num_streams;
    u64 sid16_page;
    struct io_pgtable_ops *sid16_ops;
    u32 sid16_ttbr;
    u32 sid16_init_tcr;
    u32 sid16_init_ttbr;
    bool sid16_init_enabled;
    u32 regs[0x5000/4];
};
struct apple_dart_stream_map { struct apple_dart *dart; unsigned long sidmap[4]; };
struct apple_dart_atomic_stream_map { struct apple_dart *dart; atomic_long_t sidmap[4]; };
static u32 apple_dart_readl(struct apple_dart *d, unsigned o) { return d->regs[o/4]; }
static unsigned enable_writes, disable_writes, all_writes, guard_calls, device_pins, noidle_puts;
static bool drop_bypass_write, drop_enable_write, drop_disable_write, drop_ttbr_write;
static bool tcr_clear_dapf, sid16_saw_translate;
static unsigned sid16_tcr_writes;
static u32 last_sid16_tcr_write;
static int guard_result, apple_dart_radio_kexec;
static int kexec_block(void *guard)
{ assert(guard==&apple_dart_radio_kexec); guard_calls++; return guard_result; }
static void get_device(void *dev) { assert(dev); device_pins++; }
static u32 last_enable, last_disable;
static void apple_dart_writel(struct apple_dart *d, u32 v, unsigned o)
{
    all_writes++;
    if (o == DART_TCR(d, 16)) {
        sid16_tcr_writes++;
        last_sid16_tcr_write = v;
        if (v & DART_T8110_TCR_TRANSLATE_ENABLE)
            sid16_saw_translate = true;
    }
    if (drop_bypass_write && o==DART_TCR(d,16)) return;
    if (drop_ttbr_write && o==DART_TTBR(d,16,0)) return;
    if (o >= d->hw->enable_streams && o < d->hw->enable_streams + 32) {
        enable_writes++; last_enable=v;
        if (!drop_enable_write)
            d->regs[o/4] |= v;
    } else if (o >= DART_T8110_DISABLE_STREAMS && o < DART_T8110_DISABLE_STREAMS + 32) {
        unsigned en = d->hw->enable_streams + (o - DART_T8110_DISABLE_STREAMS);
        disable_writes++; last_disable=v;
        if (!drop_disable_write)
            d->regs[en/4] &= ~v;
    } else if (tcr_clear_dapf && o >= 0x1000 && o < 0x1000 + 4 * DART_MAX_STREAMS) {
        d->regs[o/4] = v & ~DART_T8110_TCR_BYPASS_DAPF;
    } else d->regs[o/4]=v;
}
static void writel(u32 v, void *p) { (void)v; (void)p; abort(); }
static int find_first_bit(const unsigned long *p, int max)
{ int sid; for_each_set_bit(sid,p,max) return sid; return max; }
static u64 virt_to_phys(void *p) { return (uintptr_t)p; }
static int tlb_result;
static unsigned tlb_flushes;
static unsigned long last_tlb_sidmap;
static int test_invalidate_tlb(struct apple_dart_stream_map *m)
{
    assert(m->sidmap[0] && !(m->sidmap[0] & ~(m->dart->managed_sids | BIT(16))));
    tlb_flushes++;
    last_tlb_sidmap = m->sidmap[0];
    return tlb_result;
}
struct io_pgtable_cfg {
    unsigned pgsize_bitmap, ias, oas;
    int coherent_walk;
    void *iommu_dev;
    struct { int n_ttbrs, n_levels; void *ttbr[1]; } apple_dart_cfg;
};
struct io_pgtable_ops {
    int (*map_pages)(struct io_pgtable_ops *ops, unsigned long iova,
                     phys_addr_t paddr, size_t pgsize, size_t pgcount,
                     int prot, int gfp, size_t *mapped);
    phys_addr_t (*iova_to_phys)(struct io_pgtable_ops *ops, unsigned long iova);
    u64 mapped_iova;
    phys_addr_t mapped_phys;
    size_t mapped_size;
};
static u64 sid16_root_page[1];
static unsigned sid16_allocs, sid16_frees;
static int test_sid16_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
                                phys_addr_t paddr, size_t pgsize, size_t pgcount,
                                int prot, int gfp, size_t *mapped)
{
    (void)gfp;
    assert(pgsize == SZ_16K && pgcount == 1);
    assert(prot == (IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO));
    ops->mapped_iova = iova;
    ops->mapped_phys = paddr;
    ops->mapped_size = pgsize * pgcount;
    *mapped = ops->mapped_size;
    return 0;
}
static phys_addr_t test_sid16_iova_to_phys(struct io_pgtable_ops *ops, unsigned long iova)
{
    return iova == ops->mapped_iova ? ops->mapped_phys : 0;
}
static struct io_pgtable_ops *alloc_io_pgtable_ops(int fmt, struct io_pgtable_cfg *cfg, void *cookie)
{
    (void)cookie;
    assert(fmt == APPLE_DART2 && cfg->pgsize_bitmap == SZ_16K && cfg->ias == 36);
    struct io_pgtable_ops *ops = calloc(1, sizeof(*ops));
    assert(ops);
    ops->map_pages = test_sid16_map_pages;
    ops->iova_to_phys = test_sid16_iova_to_phys;
    cfg->apple_dart_cfg.n_ttbrs = 1;
    cfg->apple_dart_cfg.n_levels = 3;
    cfg->apple_dart_cfg.ttbr[0] = (void *)0x4000;
    (void)sid16_root_page;
    sid16_allocs++;
    return ops;
}
static void free_io_pgtable_ops(struct io_pgtable_ops *ops)
{
    if (!ops)
        return;
    sid16_frees++;
    free(ops);
}
struct io_pgtable { struct io_pgtable_cfg cfg; };
#define io_pgtable_ops_to_pgtable(p) (p)
struct iommu_domain { int unused; };
struct apple_dart_domain {
    struct iommu_domain domain;
    struct io_pgtable *pgtbl_ops;
    struct apple_dart_atomic_stream_map stream_maps[2];
    void *dcp;
    u64 dma_offset;
};
static struct apple_dart_domain *to_dart_domain(struct iommu_domain *p)
{ return (struct apple_dart_domain *)p; }
#define IOMMU_DOMAIN_DMA 3
#define IOMMU_DOMAIN_IDENTITY 4
#define PAGE_SIZE 16384
struct device_node { int unused; };
struct device_driver { int unused; };
static struct { struct device_driver driver; } apple_dart_driver;
struct device {
    bool pci, bound, locked;
    struct device_driver *driver;
    struct device_node *of_node;
    void *data;
};
static void device_lock(struct device *d) { assert(!d->locked); d->locked=true; }
static void device_unlock(struct device *d) { assert(d->locked); d->locked=false; }
static bool device_is_bound(struct device *d) { return d->bound; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static bool apple_dart_fault_pending(struct apple_dart *d) { return d->pending; }
struct apple_dart_master_cfg { struct apple_dart_stream_map stream_maps[2]; bool supports_bypass, locked; };
static struct apple_dart_master_cfg *active_cfg;
static bool board_t8132_air=true;
static void *dev_iommu_priv_get(struct device *d) { (void)d; return active_cfg; }
static bool dev_is_pci(struct device *d) { return d->pci; }
static bool apple_t8132_air(void) { return board_t8132_air; }
static bool apple_dart_master_needs_strict(struct device *d) { assert(!d->of_node); return false; }
static int apple_dart_isp_host_reset(struct apple_dart *d) { (void)d; abort(); }
static void apple_dart_apply_own_tunables(struct apple_dart *d) { (void)d; abort(); }
#define bitmap_weight(p,max) ({ int w=0, n; for_each_set_bit(n,p,max) w++; w; })
#define test_bit(n,p) (!!((p)[(n)/64] & BIT((n)%64)))
/* The radio DART is no J714s/J716c display provider and stays powered. */
static int apple_dart_v27_cfg_enter(struct apple_dart_master_cfg *c) { (void)c; return 0; }
static void apple_dart_v27_cfg_leave(struct apple_dart_master_cfg *c) { (void)c; }
#define APPLE_DART_WRITER_CLOSED 2
static int writer_gate, writer_refs;
static int apple_dart_sleep_writer_get(struct apple_dart *d) { (void)d; writer_refs++; return writer_gate; }
static void apple_dart_sleep_writer_put(struct apple_dart *d, int gate)
{ (void)d; assert(gate==writer_gate && writer_refs>0); writer_refs--; }
static void apple_dart_sleep_release_failed(void) { abort(); }
static u32 readl(const void *p) { (void)p; abort(); }
static int apple_dart_t8110_hw_tlb_command_range(struct apple_dart_stream_map *p,
        int op, bool range, unsigned long a, unsigned long b)
{ (void)p; (void)op; (void)range; (void)a; (void)b; abort(); }
static void apple_dart_hw_clear_all_ttbrs(struct apple_dart_stream_map *p) { (void)p; abort(); }
static void apple_dart_hw_apply_remaps(struct apple_dart *p) { (void)p; abort(); }
static int apple_dart_hw_finish_reset(struct apple_dart *p) { (void)p; abort(); }
static void atomic_long_or(unsigned long v, atomic_long_t *p) { *p |= v; }
static void atomic_long_and(unsigned long v, atomic_long_t *p) { *p &= v; }
static unsigned long atomic_long_read(atomic_long_t *p) { return *p; }
static bool bitmap_empty(unsigned long *p, int max)
{ for (int i=0; i<BITS_TO_LONGS(max); i++) if (p[i]) return false; return true; }
static int pm_refs;
static int pm_runtime_get_sync(void *d) { (void)d; pm_refs++; return 0; }
static int pm_runtime_resume_and_get(void *d) { return pm_runtime_get_sync(d); }
static void pm_runtime_put(void *d) { (void)d; assert(pm_refs>0); pm_refs--; }
static void pm_runtime_put_noidle(void *d) { noidle_puts++;pm_runtime_put(d); }
static int finalize_result;
static int apple_dart_finalize_domain(struct apple_dart_domain *d, struct apple_dart_master_cfg *c)
{ (void)d; (void)c; return finalize_result; }
static int apple_dart_setup_translation_locked(struct apple_dart_domain *d, struct apple_dart_stream_map *m)
{ (void)d; (void)m; abort(); }
static int apple_dart_hw_sync_locked(struct io_pgtable_cfg *c, struct apple_dart_stream_map *m, bool b)
{ (void)c; (void)m; (void)b; abort(); }
static int apple_dart_fw_invalidate(struct apple_dart_stream_map *m, u64 a, u64 b)
{ (void)m; (void)a; (void)b; abort(); }
static int apple_dart_dcp_domain_flush(struct apple_dart_domain *d)
{ (void)d; abort(); }
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
#define for_each_stream_map(i,base,m) for ((i)=0,(m)=&(base)->stream_maps[0]; \
    (i)<MAX_DARTS_PER_DEVICE && (m)->dart; (m)=&(base)->stream_maps[++(i)])
struct apple_pcie_hw { bool tunneled, adopt_link; };
#define PORT_MSICFG 0x124
#define BITS_PER_LONG 64
struct apple_pcie { u16 managed_sids; struct apple_pcie_hw *hw; struct device *dev; };
struct list_head { bool linked; };
struct apple_pcie_port {
    struct apple_pcie *pcie;
    int sid_map_sz;
    u32 entries[16];
    void *base, *domain, *np;
    bool started, needs_stop;
    unsigned long adopted_slots;
    unsigned irq;
    u32 status, link;
    struct list_head entry;
};
#define PORT_INTMSK 0x100
#define PORT_STATUS 0x804
#define PORT_STATUS_READY BIT(0)
#define PORT_LINKSTS 0x208
#define PORT_LINKSTS_UP BIT(0)
#define PORT_LINKSTS_BUSY BIT(2)
static unsigned pci_irq_writes, pci_tunnel_stops;
static void apple_pcie_port_disable_irq(struct apple_pcie_port *p) { (void)p; }
static void apple_pcie_port_writel(struct apple_pcie_port *p, u32 v, unsigned o)
{ (void)p; assert(v==U32_MAX && o==PORT_INTMSK); pci_irq_writes++; }
static void apple_pcie_tunnel_stop(struct apple_pcie_port *p) { (void)p; pci_tunnel_stops++; }
static void apple_pcie_rid2sid_write(struct apple_pcie_port *p, int slot, u32 v) { (void)p; (void)slot; (void)v; abort(); }
static void apple_pcie_port_unregister_irqs(struct apple_pcie_port *p) { (void)p; }
static void irq_set_chained_handler(unsigned i, void *f) { (void)i; (void)f; }
static void synchronize_rcu(void) {}
static void irq_dispose_mapping(unsigned i) { (void)i; }
static void irq_domain_remove(void *d) { (void)d; }
static bool list_empty(struct list_head *p) { return !p->linked; }
static void list_del_init(struct list_head *p) { p->linked=false; }
struct of_phandle_args { struct device_node *np; int args_count; u32 args[1]; };
struct platform_device { struct device dev; };
static struct device_node radio_node, other_node;
static struct platform_device radio_provider;
static bool provider_present=true, mismatched_map;
static int provider_refs, node_refs, map_result[2];
static void of_node_put(void *n) { if (n==&radio_node || n==&other_node) node_refs--; }
static int of_map_iommu_id(struct device_node *n, unsigned rid, struct of_phandle_args *a)
{
    (void)n; assert(rid==0x100 || rid==0x101); unsigned fn=rid-0x100;
    if (map_result[fn]) return map_result[fn];
    a->np=fn && mismatched_map ? &other_node : &radio_node;
    a->args_count=1; a->args[0]=fn+1; node_refs++; return 0;
}
static struct platform_device *of_find_device_by_node(struct device_node *n)
{ assert(n==&radio_node); if (!provider_present) return NULL; provider_refs++; return &radio_provider; }
static void put_device(struct device *d) { assert(d==&radio_provider.dev && provider_refs>0); provider_refs--; }
static unsigned port_rid2sid_offset(struct apple_pcie_port *p, unsigned sid)
{ (void)p; return sid; }
static u32 apple_pcie_port_readl(struct apple_pcie_port *p, unsigned o)
{
    if (o==PORT_STATUS) return p->status;
    if (o==PORT_LINKSTS) return p->link;
    assert(o<16); return p->entries[o];
}
'''
    display = (root / 'drivers/iommu/apple-dart-dcp.h').read_text()
    code += definition(display, 'static bool apple_dart_has_borrowed(') + '\n'
    for name in ('static int apple_pcie_managed_sid_mask(',
                 'static int apple_pcie_managed_word(',
                 'static int apple_pcie_managed_admit('):
        # Skip the forward declaration when extracting the admission definition.
        source = pci[pci.index('/* Native T8132 normal SIDs'):]
        code += definition(source, name) + '\n'
    code += definition(pci, 'static void apple_pcie_port_teardown(') + '\n'
    for name in ('static bool apple_dart_remaps_streams(',
                 'apple_dart_hw_enable_translation(',
                 'static void apple_dart_hw_set_ttbr(',
                 'static void apple_dart_hw_clear_ttbr(',
                 'apple_dart_hw_enable_bypass(',
                 'static void apple_dart_hw_disable_dma(',
                 'static bool apple_dart_managed_static_bypass(',
                 'static bool apple_dart_managed_sid16_narrowed(',
                 'static int apple_dart_managed_narrow_sid16(',
                 'static void apple_dart_managed_unnarrow_sid16(',
                 'static int apple_dart_managed_admit(',
                 'int apple_dart_radio_check_idle(',
                 'static void apple_dart_managed_quarantine_locked(',
                 'static void apple_dart_managed_quarantine(',
                 'static int apple_dart_hw_reset(',
                 'static int apple_dart_t8110_v2_unlock(',
                 'static int apple_dart_def_domain_type(',
                 'apple_dart_mod_streams(',
                 'static int apple_dart_domain_add_streams(',
                 'apple_dart_setup_translation(',
                 'static int apple_dart_attach_dev_paging_unguarded(',
                 'static int apple_dart_attach_dev_paging_v27(',
                 'static int apple_dart_attach_dev_paging(',
                 'static int apple_dart_domain_flush_tlb_range('):
        # This definition has its static int return type on a preceding line.
        body = definition(dart, name)
        if name in ('apple_dart_hw_enable_bypass(', 'apple_dart_hw_enable_translation('):
            body = 'static void\n' + body
        if name in ('apple_dart_mod_streams(', 'apple_dart_setup_translation('):
            body = 'static int\n' + body
        code += body + '\n'
    code += definition(pci, 'static int apple_pcie_radio_check_cold(struct apple_pcie_port *port)\n{') + '\n'
    code += definition(dart, 'static int apple_dart_retain_radio(') + '\n'
    probe = definition(dart, 'static int apple_dart_probe(struct platform_device *pdev)\n{')
    # The managed-SID branch of the reset/acquisition chain, up to its next sibling.
    start = probe.index('} else if (dart->managed_sids) {') + len('} else ')
    end = probe.index('\n\t} else if (', start)
    cleanup = probe[probe.index('err_clk_disable:'):probe.index('\n\tclk_bulk_disable_unprepare', probe.index('err_clk_disable:'))]
    code += 'static int test_probe_start(struct apple_dart *dart) { int ret; void *dev=dart->dev;\n' + probe[start:end] + '\n\t}\nreturn 0;\n' + cleanup + '\nreturn ret; }\n'
    code += r'''
static void test_probe_guard(const struct apple_dart_hw *hw)
{
    for (unsigned warm=0; warm<2; warm++) {
        struct apple_dart d={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                            .num_streams=19,.supports_bypass=true,.dev=(void *)1};
        d.regs[DART_TCR(&d,16)/4]=warm?6:0;
        u32 before[ARRAY_SIZE(d.regs)];memcpy(before,d.regs,sizeof(before));
        all_writes=guard_calls=device_pins=0;guard_result=-EBUSY;pm_refs=1;
        assert(test_probe_start(&d)==-EBUSY);
        assert(guard_calls==1 && !device_pins && !d.retained && !all_writes && !pm_refs);
        assert(!memcmp(before,d.regs,sizeof(before)));
        all_writes=guard_calls=0;guard_result=0;pm_refs=1;
        assert(!test_probe_start(&d) && d.retained && guard_calls==1 && device_pins==1);
        assert(all_writes && pm_refs==1 && d.regs[DART_TCR(&d,16)/4]==6);
        assert((d.regs[hw->enable_streams/4]&6)==6);
        for(unsigned sid=0;sid<19;sid++)
            assert(!d.regs[DART_TTBR(&d,sid,0)/4]);
        pm_refs=0;
    }
    struct apple_dart failed={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                             .num_streams=19,.supports_bypass=true,.dev=(void *)1};
    failed.regs[DART_TCR(&failed,1)/4]=failed.regs[DART_TCR(&failed,2)/4]=1;
    guard_result=0;guard_calls=device_pins=noidle_puts=0;pm_refs=1;
    drop_bypass_write=true;
    assert(test_probe_start(&failed)==-EIO);
    assert(failed.retained && failed.managed_failed && device_pins==1 && guard_calls==1);
    assert(!failed.regs[DART_TCR(&failed,1)/4] && !failed.regs[DART_TCR(&failed,2)/4]);
    assert(noidle_puts==1 && !pm_refs);
    drop_bypass_write=false;
    guard_result=0;

    /*
     * SID16 narrowing + error-path teardown preserves inherited TCR, TTBR, and
     * ENABLE[16] state (with write-one-to-clear DISABLE_STREAMS and T8132
     * non-retained DAPF request modeling) and allows clean rebind without -EBUSY.
     */
    const struct {
        u32 init_tcr, init_ttbr;
        bool init_enabled, clear_dapf;
    } sid16_cases[] = {
        { 0, 0, false, true },
        { 0, 0x123400, false, true },
        { 0, 0x123400, true, true },
        { 2, 0, false, true },
        { 2, 0x5678001, false, true },
        { 2, 0xabcd001, true, true },
        { 6, 0, false, false },
        { 6, 0xabcd001, false, false },
        { 6, 0xabcd001, true, false },
    };
    for (unsigned i=0; i<ARRAY_SIZE(sid16_cases); i++) {
        struct apple_dart narrow={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                  .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                  .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
        tcr_clear_dapf=sid16_cases[i].clear_dapf;
        narrow.regs[DART_TCR(&narrow,16)/4]=sid16_cases[i].init_tcr;
        narrow.regs[DART_TTBR(&narrow,16,0)/4]=sid16_cases[i].init_ttbr;
        narrow.regs[hw->enable_streams/4]=BIT(18)|(sid16_cases[i].init_enabled?BIT(16):0);
        tlb_result=0; tlb_flushes=0; last_tlb_sidmap=0;
        sid16_allocs=sid16_frees=disable_writes=0; pm_refs=1; guard_result=0;
        assert(!test_probe_start(&narrow));
        assert(narrow.sid16_ops && narrow.regs[DART_TCR(&narrow,16)/4]==1);
        assert(narrow.regs[DART_TTBR(&narrow,16,0)/4]==narrow.sid16_ttbr);
        assert(narrow.regs[hw->enable_streams/4]==(BIT(1)|BIT(2)|BIT(16)|BIT(18)));
        assert(tlb_flushes==1 && last_tlb_sidmap==BIT(16));
        assert(sid16_allocs==1 && !sid16_frees);
        bool ib=false;
        assert(!apple_dart_managed_admit(&narrow,&ib));
        narrow.regs[hw->enable_streams/4]&=~BIT(16);
        assert(apple_dart_managed_admit(&narrow,&ib)==-EBUSY);
        narrow.regs[hw->enable_streams/4]|=BIT(16);

        /* Simulated post-narrowing probe failure tears down and frees table. */
        tlb_flushes=0; last_tlb_sidmap=0; disable_writes=0; last_disable=0;
        last_sid16_tcr_write=0;
        apple_dart_managed_quarantine(&narrow);
        apple_dart_managed_unnarrow_sid16(&narrow);
        assert(!narrow.sid16_ops && !narrow.sid16_ttbr && sid16_frees==1);
        assert(last_sid16_tcr_write==
               (sid16_cases[i].init_tcr?hw->tcr_bypass:hw->tcr_disabled));
        assert(narrow.regs[DART_TCR(&narrow,16)/4]==sid16_cases[i].init_tcr);
        assert(narrow.regs[DART_TTBR(&narrow,16,0)/4]==sid16_cases[i].init_ttbr);
        assert(narrow.regs[hw->enable_streams/4]==
               (BIT(1)|BIT(2)|BIT(18)|(sid16_cases[i].init_enabled?BIT(16):0)));
        assert(disable_writes==(sid16_cases[i].init_enabled?0u:1u));
        if (!sid16_cases[i].init_enabled)
            assert(last_disable==BIT(16));
        assert(tlb_flushes==1 && last_tlb_sidmap==BIT(16));

        /* A fresh rebind sees the restored state and admits cleanly. */
        struct apple_dart rebind={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                  .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                  .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
        memcpy(rebind.regs,narrow.regs,sizeof(rebind.regs));
        pm_refs=1;
        assert(!test_probe_start(&rebind));
        assert(rebind.sid16_ops && rebind.regs[DART_TCR(&rebind,16)/4]==1);
        free_io_pgtable_ops(rebind.sid16_ops);
        pm_refs=0;
    }

    /* Entry states that cannot be restored are rejected before narrowing. */
    tcr_clear_dapf=true;
    struct apple_dart bad_entry={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                 .num_streams=19,.supports_bypass=false,.pgsize=SZ_16K,
                                 .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    bad_entry.regs[DART_TCR(&bad_entry,16)/4]=2;
    sid16_allocs=0; all_writes=0;
    assert(apple_dart_managed_narrow_sid16(&bad_entry)==-EBUSY);
    assert(!bad_entry.sid16_ops && !sid16_allocs && !all_writes);
    bad_entry.supports_bypass=true;
    bad_entry.regs[DART_TCR(&bad_entry,16)/4]=0;
    bad_entry.regs[DART_TTBR(&bad_entry,16,0)/4]=hw->ttbr_valid|0x1000;
    pm_refs=1;
    assert(test_probe_start(&bad_entry)==-EBUSY && !pm_refs);
    assert(apple_dart_managed_narrow_sid16(&bad_entry)==-EBUSY);
    assert(!bad_entry.sid16_ops && !sid16_allocs && !all_writes);
    bad_entry.regs[DART_TTBR(&bad_entry,16,0)/4]=0;
    const u32 bad_tcrs[]={1,4,8,0x80};
    for (unsigned i=0; i<ARRAY_SIZE(bad_tcrs); i++) {
        bad_entry.regs[DART_TCR(&bad_entry,16)/4]=bad_tcrs[i];
        pm_refs=1;
        assert(test_probe_start(&bad_entry)==-EBUSY && !pm_refs);
        assert(apple_dart_managed_narrow_sid16(&bad_entry)==-EBUSY);
        assert(!bad_entry.sid16_ops && !sid16_allocs && !all_writes);
    }

    /* Dropped TTBR0 write during narrowing fails before enabling translation. */
    struct apple_dart drop_ttbr={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                 .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                 .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    drop_ttbr.regs[DART_TCR(&drop_ttbr,16)/4]=2;
    drop_ttbr.regs[DART_TTBR(&drop_ttbr,16,0)/4]=0xabcd001;
    drop_ttbr.regs[hw->enable_streams/4]=BIT(16);
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1; noidle_puts=0;
    sid16_saw_translate=false; sid16_tcr_writes=0; last_sid16_tcr_write=0;
    drop_ttbr_write=true;
    assert(test_probe_start(&drop_ttbr)==-EIO);
    drop_ttbr_write=false;
    assert(!sid16_saw_translate);
    assert(drop_ttbr.retained && drop_ttbr.managed_failed && noidle_puts==1 && !pm_refs);
    assert(!drop_ttbr.sid16_ops && !drop_ttbr.sid16_ttbr && sid16_allocs==1 && sid16_frees==1);
    assert(drop_ttbr.regs[DART_TCR(&drop_ttbr,16)/4]==2);
    assert(last_sid16_tcr_write==hw->tcr_bypass);
    assert(drop_ttbr.regs[DART_TTBR(&drop_ttbr,16,0)/4]==0xabcd001);
    assert((drop_ttbr.regs[hw->enable_streams/4]&(BIT(1)|BIT(2)|BIT(16)))==BIT(16));

    /* Dropped ENABLE[16] write during narrowing fails probe and rolls back. */
    struct apple_dart drop_en={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                               .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                               .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    drop_en.regs[DART_TCR(&drop_en,16)/4]=2;
    drop_en.regs[DART_TTBR(&drop_en,16,0)/4]=0xabcd001;
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1; noidle_puts=0;
    last_sid16_tcr_write=0;
    drop_enable_write=true;
    assert(test_probe_start(&drop_en)==-EIO);
    drop_enable_write=false;
    assert(drop_en.retained && drop_en.managed_failed && noidle_puts==1 && !pm_refs);
    assert(!drop_en.sid16_ops && !drop_en.sid16_ttbr && sid16_allocs==1 && sid16_frees==1);
    assert(last_sid16_tcr_write==hw->tcr_bypass);
    assert(drop_en.regs[DART_TCR(&drop_en,16)/4]==2);
    assert(drop_en.regs[DART_TTBR(&drop_en,16,0)/4]==0xabcd001);
    assert(!(drop_en.regs[hw->enable_streams/4]&(BIT(1)|BIT(2)|BIT(16))));

    /* Mismatched TCR readback on restore keeps table unfreed. */
    struct apple_dart mismatch_tcr={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                    .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                    .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    mismatch_tcr.regs[DART_TCR(&mismatch_tcr,16)/4]=6;
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1;
    assert(!test_probe_start(&mismatch_tcr));
    apple_dart_managed_unnarrow_sid16(&mismatch_tcr);
    assert(mismatch_tcr.sid16_ops && sid16_frees==0);
    assert(mismatch_tcr.regs[DART_TCR(&mismatch_tcr,16)/4]==2);
    free_io_pgtable_ops(mismatch_tcr.sid16_ops);
    pm_refs=0;

    /* Failed DISABLE_STREAMS clear refuses to restore bypass and keeps table unfreed. */
    struct apple_dart stuck_en={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    stuck_en.regs[DART_TCR(&stuck_en,16)/4]=2;
    stuck_en.regs[DART_TTBR(&stuck_en,16,0)/4]=0xabcd001;
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1;
    assert(!test_probe_start(&stuck_en));
    drop_disable_write=true;
    apple_dart_managed_unnarrow_sid16(&stuck_en);
    assert(stuck_en.sid16_ops && sid16_frees==0);
    assert(stuck_en.regs[DART_TCR(&stuck_en,16)/4]==1);
    assert(stuck_en.regs[DART_TTBR(&stuck_en,16,0)/4]==stuck_en.sid16_ttbr);
    drop_disable_write=false;
    free_io_pgtable_ops(stuck_en.sid16_ops);
    pm_refs=0;

    /* Failed TCR restore keeps TTBR intact and keeps table unfreed. */
    struct apple_dart stuck_tcr={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                 .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                 .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    stuck_tcr.regs[DART_TCR(&stuck_tcr,16)/4]=2;
    stuck_tcr.regs[DART_TTBR(&stuck_tcr,16,0)/4]=0xabcd001;
    stuck_tcr.regs[hw->enable_streams/4]=BIT(16);
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1;
    assert(!test_probe_start(&stuck_tcr));
    drop_bypass_write=true;
    apple_dart_managed_unnarrow_sid16(&stuck_tcr);
    assert(stuck_tcr.sid16_ops && sid16_frees==0);
    assert(stuck_tcr.regs[DART_TCR(&stuck_tcr,16)/4]==1);
    assert(stuck_tcr.regs[DART_TTBR(&stuck_tcr,16,0)/4]==stuck_tcr.sid16_ttbr);
    drop_bypass_write=false;
    free_io_pgtable_ops(stuck_tcr.sid16_ops);
    pm_refs=0;

    /* Failed TTBR restore keeps table unfreed. */
    struct apple_dart stuck_ttbr={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                                  .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                                  .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    stuck_ttbr.regs[DART_TCR(&stuck_ttbr,16)/4]=2;
    stuck_ttbr.regs[DART_TTBR(&stuck_ttbr,16,0)/4]=0xabcd001;
    stuck_ttbr.regs[hw->enable_streams/4]=BIT(16);
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1;
    assert(!test_probe_start(&stuck_ttbr));
    drop_ttbr_write=true;
    apple_dart_managed_unnarrow_sid16(&stuck_ttbr);
    assert(stuck_ttbr.sid16_ops && sid16_frees==0);
    assert(stuck_ttbr.regs[DART_TTBR(&stuck_ttbr,16,0)/4]==stuck_ttbr.sid16_ttbr);
    drop_ttbr_write=false;
    free_io_pgtable_ops(stuck_ttbr.sid16_ops);
    pm_refs=0;

    /* Failed TLB flush during teardown keeps the page table unfreed. */
    struct apple_dart stuck={.hw=hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,
                             .num_streams=19,.supports_bypass=true,.pgsize=SZ_16K,
                             .oas=36,.sid16_page=0x381204000ULL,.dev=(void *)1};
    stuck.regs[DART_TCR(&stuck,16)/4]=2;
    stuck.regs[hw->enable_streams/4]=BIT(16);
    tlb_result=0; sid16_allocs=sid16_frees=0; pm_refs=1;
    assert(!test_probe_start(&stuck));
    tlb_result=-EIO;
    apple_dart_managed_unnarrow_sid16(&stuck);
    assert(stuck.sid16_ops && sid16_frees==0);
    free_io_pgtable_ops(stuck.sid16_ops);
    tlb_result=0; pm_refs=0; tcr_clear_dapf=false;
}
'''
    code += r'''
int main(void)
{
    u32 ids[] = {1,2}, word=0;
    u16 mask=0;
    assert(apple_pcie_managed_sid_mask(ids,2,&mask)==0 && mask==(BIT(1)|BIT(2)));
    assert(apple_pcie_managed_word(mask,0x100,1,&word)==0 && word==0x80010100);
    assert(apple_pcie_managed_word(mask,0x101,2,&word)==0 && word==0x80020101);
    for (unsigned sid=0; sid<20; sid++)
        if (sid!=1 && sid!=2) assert(apple_pcie_managed_word(mask,0x100,sid,&word)==-EINVAL);
    assert(apple_pcie_managed_word(mask,0,1,&word)==-EINVAL);
    assert(apple_pcie_managed_word(mask,0x10000,1,&word)==-EINVAL);
    ids[1]=1; assert(apple_pcie_managed_sid_mask(ids,2,&mask)==-EINVAL);
    ids[1]=16; assert(apple_pcie_managed_sid_mask(ids,2,&mask)==-EINVAL);
    struct apple_pcie_hw pci_hw={0};
    struct apple_pcie pci={.managed_sids=BIT(1)|BIT(2),.hw=&pci_hw};
    struct apple_pcie_port port={.pcie=&pci};
    port.entries[3]=0x80030300; /* foreign association */
    u32 entries[16]; memcpy(entries,port.entries,sizeof(entries));
    assert(apple_pcie_managed_admit(&port)==0 && port.sid_map_sz==16);
    assert(!memcmp(entries,port.entries,sizeof(entries)));
    for (unsigned sid=0; sid<3; sid++) {
        port.entries[sid]=0x80000100;
        assert(apple_pcie_managed_admit(&port)==-EBUSY);
        port.entries[sid]=0;
    }
    port.base=&port; port.np=&port; port.entries[1]=0x80010100;
    assert(apple_pcie_managed_admit(&port)==-EBUSY);
    apple_pcie_port_teardown(&port);
    assert(!pci_irq_writes && !pci_tunnel_stops && !port.np);
    port.domain=&port; port.irq=1;
    apple_pcie_port_teardown(&port);
    assert(pci_irq_writes==1 && !port.domain && !port.irq);
    const struct apple_dart_hw hw={.type=DART_T8110,.tcr_disabled=0,.tcr_bypass=6,
        .ttbr_count=1,.ttbr_valid=BIT(0),.enable_streams=0xc00,.tcr_enabled=1,
        .tcr_4level=BIT(3),.ttbr_shift=14,.ttbr_addr_field_shift=2,.invalidate_tlb=test_invalidate_tlb};
    test_probe_guard(&hw);
    struct apple_dart d={.hw=&hw,.managed_sids=BIT(1)|BIT(2),.version=0x202,.num_streams=19,.supports_bypass=true};
    bool init_bypass=false;
    /* Firmware reserved roles and another ordinary owner are live. */
    d.regs[DART_TCR(&d,16)/4]=6;
    d.regs[DART_TCR(&d,17)/4]=1; d.regs[DART_TTBR(&d,17,0)/4]=0xf001;
    d.regs[DART_TCR(&d,18)/4]=6;
    d.regs[DART_TCR(&d,3)/4]=1; d.regs[DART_TTBR(&d,3,0)/4]=0x123401;
    u32 before[sizeof(d.regs)/4]; memcpy(before,d.regs,sizeof(before));
    assert(apple_dart_managed_admit(&d,&init_bypass)==0);
    assert(apple_dart_hw_reset(&d)==-EBUSY);
    assert(apple_dart_t8110_v2_unlock(&d)==-EPERM);
    assert(!memcmp(before,d.regs,sizeof(before)));
    /* Masked fault reporting cannot silently defeat owned-stream quarantine. */
    const u32 inherited_masks[]={BIT(0),BIT(31),U32_MAX};
    for (unsigned i=0; i<ARRAY_SIZE(inherited_masks); i++) {
        u32 masked[ARRAY_SIZE(d.regs)];
        d.regs[DART_T8110_ERROR_MASK/4]=inherited_masks[i];
        memcpy(masked,d.regs,sizeof(masked));
        assert(apple_dart_managed_admit(&d,&init_bypass)==-EBUSY);
        assert(!memcmp(masked,d.regs,sizeof(masked)));
    }
    d.managed_sids=0;
    assert(apple_dart_managed_admit(&d,&init_bypass)==0); /* legacy profile unchanged */
    d.managed_sids=BIT(1)|BIT(2); d.regs[DART_T8110_ERROR_MASK/4]=0;
    assert(apple_dart_managed_admit(&d,&init_bypass)==0);
    assert(!memcmp(before,d.regs,sizeof(before)));
    /* DAPF is a request bit: either readable bypass form is inherited. */
    d.regs[DART_TCR(&d,16)/4]=2;
    d.regs[DART_TTBR(&d,16,0)/4]=0xabcd001;
    init_bypass=true;
    assert(apple_dart_managed_admit(&d,&init_bypass)==0 && !init_bypass);
    assert(d.regs[DART_TCR(&d,16)/4]==2);
    assert(d.regs[DART_TTBR(&d,16,0)/4]==0xabcd001);
    for (unsigned tcr=0; tcr<0x100; tcr++)
        assert(apple_dart_managed_static_bypass(tcr)==(tcr==2 || tcr==6));
    d.regs[DART_TCR(&d,16)/4]=6;
    d.regs[DART_TTBR(&d,16,0)/4]=0;
    for (unsigned sid=1; sid<=2; sid++) {
        d.regs[DART_TCR(&d,sid)/4]=6;
        assert(apple_dart_managed_admit(&d,&init_bypass)==-EBUSY);
        d.regs[DART_TCR(&d,sid)/4]=0;
        d.regs[DART_TTBR(&d,sid,0)/4]=hw.ttbr_valid|0x1000;
        assert(apple_dart_managed_admit(&d,&init_bypass)==-EBUSY);
        d.regs[DART_TTBR(&d,sid,0)/4]=0;
    }
    d.regs[DART_TCR(&d,16)/4]=1;
    assert(apple_dart_managed_admit(&d,&init_bypass)==-EBUSY);
    d.regs[DART_TCR(&d,16)/4]=0;
    d.regs[DART_TTBR(&d,16,0)/4]=hw.ttbr_valid|0x1000;
    assert(apple_dart_managed_admit(&d,&init_bypass)==-EBUSY);
    d.regs[DART_TTBR(&d,16,0)/4]=0;
    assert(apple_dart_managed_admit(&d,&init_bypass)==0 && init_bypass);
    struct apple_dart_stream_map bypass={.dart=&d,.sidmap={BIT(16)}};
    apple_dart_hw_enable_bypass(&bypass);
    assert(d.regs[DART_TCR(&d,16)/4]==6);
    assert(!memcmp(before,d.regs,sizeof(before)));
    init_bypass=true;
    assert(apple_dart_managed_admit(&d,&init_bypass)==0 && !init_bypass);
    d.locked=true; assert(apple_dart_managed_admit(&d,&init_bypass)==-EINVAL); d.locked=false;
    d.regs[DART_TCR(&d,1)/4]=1; d.regs[DART_TCR(&d,2)/4]=1;
    apple_dart_managed_quarantine(&d);
    assert(d.managed_failed && !d.managed_lock);
    assert(!memcmp(before,d.regs,sizeof(before))); /* only owned TCRs disabled; no unlock */
    struct device dev={.pci=true};
    struct apple_dart_master_cfg cfg={.stream_maps={{.dart=&d}},.supports_bypass=true};
    active_cfg=&cfg;
    assert(apple_dart_def_domain_type(&dev)==IOMMU_DOMAIN_DMA);
    d.managed_sids=0;
    d.frontend[0]=&d;
    assert(apple_dart_def_domain_type(&dev)==IOMMU_DOMAIN_DMA);
    d.frontend[0]=NULL;
    assert(apple_dart_def_domain_type(&dev)==IOMMU_DOMAIN_IDENTITY); /* T8132 Air PCIe (J713/J715) */
    board_t8132_air=false;
    assert(apple_dart_def_domain_type(&dev)==0); /* other machines retain default */
    d.managed_sids=BIT(1)|BIT(2);
    struct apple_dart_atomic_stream_map domain[2]={{.dart=&d}};
    struct apple_dart_stream_map wifi[2]={{.dart=&d,.sidmap={BIT(1)}}};
    struct apple_dart_stream_map bt[2]={{.dart=&d,.sidmap={BIT(2)}}};
    assert(apple_dart_mod_streams(domain,wifi,true)==0);
    assert(apple_dart_mod_streams(domain,bt,true)==0);
    assert(domain[0].sidmap[0]==(BIT(1)|BIT(2)));
    assert(apple_dart_mod_streams(domain,bt,false)==0);
    assert(domain[0].sidmap[0]==BIT(1)); /* WiFi remains while BT detaches */
    d.managed_failed=false; d.num_streams=64;
    struct io_pgtable pg={.cfg.apple_dart_cfg={.n_ttbrs=1,.n_levels=3,.ttbr={(void *)0x4000}}};
    struct apple_dart_domain attach={.pgtbl_ops=&pg,.stream_maps={{.dart=&d}}};
    d.regs[hw.enable_streams/4]=BIT(3)|BIT(16)|BIT(18);
    d.regs[(hw.enable_streams+4)/4]=BIT(6); /* a foreign SID above 31 */
    memcpy(before,d.regs,sizeof(before));
    cfg.stream_maps[0].sidmap[0]=BIT(1); active_cfg=&cfg;
    enable_writes=0;
    assert(apple_dart_attach_dev_paging(&attach.domain,&dev,NULL)==0);
    assert(enable_writes==1 && last_enable==BIT(1) && !pm_refs);
    assert(attach.stream_maps[0].sidmap[0]==BIT(1));
    assert(d.regs[DART_TCR(&d,1)/4]==1 && d.regs[DART_TCR(&d,2)/4]==0);
    cfg.stream_maps[0].sidmap[0]=BIT(2);
    assert(apple_dart_attach_dev_paging(&attach.domain,&dev,NULL)==0);
    assert(enable_writes==2 && last_enable==BIT(2) && !pm_refs);
    assert(attach.stream_maps[0].sidmap[0]==(BIT(1)|BIT(2)));
    /* A DART closed for its sleep takes no attach and no register write. */
    u32 closed[ARRAY_SIZE(d.regs)]; memcpy(closed,d.regs,sizeof(closed));
    writer_gate=APPLE_DART_WRITER_CLOSED; all_writes=0;
    assert(apple_dart_attach_dev_paging(&attach.domain,&dev,NULL)==-EBUSY);
    assert(!all_writes && !pm_refs && !writer_refs && !memcmp(closed,d.regs,sizeof(closed)));
    writer_gate=0;
    for (unsigned sid=3; sid<64; sid++) {
        assert(d.regs[DART_TCR(&d,sid)/4]==before[DART_TCR(&d,sid)/4]);
        assert(d.regs[DART_TTBR(&d,sid,0)/4]==before[DART_TTBR(&d,sid,0)/4]);
    }
    assert(d.regs[hw.enable_streams/4]==(before[hw.enable_streams/4]|BIT(1)|BIT(2)));
    assert(d.regs[(hw.enable_streams+4)/4]==before[(hw.enable_streams+4)/4]);
    /* Failed second-function publication must not erase WiFi membership. */
    attach.stream_maps[0].sidmap[0]=BIT(1);
    tlb_result=-ETIMEDOUT;
    assert(apple_dart_attach_dev_paging(&attach.domain,&dev,NULL)==-ETIMEDOUT);
    assert(d.managed_failed && !d.managed_lock && !pm_refs);
    assert(attach.stream_maps[0].sidmap[0]==BIT(1));
    assert(!d.regs[DART_TCR(&d,1)/4] && !d.regs[DART_TCR(&d,2)/4]);
    unsigned writes=enable_writes;
    assert(apple_dart_attach_dev_paging(&attach.domain,&dev,NULL)==-EIO);
    assert(enable_writes==writes && attach.stream_maps[0].sidmap[0]==BIT(1));
    /* Later map/unmap synchronization also fails closed on a TLB timeout. */
    d.managed_failed=false;
    d.regs[DART_TCR(&d,1)/4]=1; d.regs[DART_TCR(&d,2)/4]=1;
    assert(apple_dart_domain_flush_tlb_range(&attach,false,0,0)==-ETIMEDOUT);
    assert(d.managed_failed && !d.managed_lock && !pm_refs);
    assert(!d.regs[DART_TCR(&d,1)/4] && !d.regs[DART_TCR(&d,2)/4]);
    for (unsigned sid=3; sid<64; sid++) {
        assert(d.regs[DART_TCR(&d,sid)/4]==before[DART_TCR(&d,sid)/4]);
        assert(d.regs[DART_TTBR(&d,sid,0)/4]==before[DART_TTBR(&d,sid,0)/4]);
    }
    assert(d.regs[(hw.enable_streams+4)/4]==before[(hw.enable_streams+4)/4]);
    /* Actual candidate6 idle state, with stale address bits but VALID clear. */
    d.num_streams=19; d.managed_failed=false; d.iommu.ready=true;
    d.regs[DART_TCR(&d,1)/4]=1; d.regs[DART_TTBR(&d,1,0)/4]=0x0764c8f8;
    d.regs[DART_TCR(&d,2)/4]=1; d.regs[DART_TTBR(&d,2,0)/4]=0x00a62d20;
    radio_provider.dev=(struct device){.bound=true,.driver=&apple_dart_driver.driver,.data=&d};
    pci.dev=&dev; memset(port.entries,0,sizeof(port.entries));
    port.status=0xd; port.link=0xab000200;
    memcpy(before,d.regs,sizeof(before));
    assert(apple_pcie_radio_check_cold(&port)==0);
    assert(!memcmp(before,d.regs,sizeof(before)) && !node_refs && !provider_refs);
    assert(!radio_provider.dev.locked && !d.managed_lock);
    /* Lock freezes protection policy; an unlocked writable state stays usable. */
    d.regs[DART_T8110_PROTECT_LOCK/4]=1;
    assert(apple_pcie_radio_check_cold(&port)==0);
    d.regs[DART_T8110_PROTECT_LOCK/4]=0;
    const unsigned controls[]={DART_T8110_PROTECT,DART_T8110_TLB_CMD,
        DART_T8110_ERROR,DART_T8110_ERROR_STREAMS,DART_T8110_ERROR_MASK};
    const u32 control_bits[]={BIT(0),BIT(31),BIT(31),BIT(1),BIT(0)};
    for (unsigned i=0; i<ARRAY_SIZE(controls); i++) {
        d.regs[controls[i]/4]=control_bits[i];
        assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
        d.regs[controls[i]/4]=0;
    }
    for (unsigned source=0; source<19; source++) {
        u32 old=d.regs[DART_TCR(&d,source)/4];
        for (unsigned target=1; target<=2; target++) {
            d.regs[DART_TCR(&d,source)/4]=BIT(7)|(target<<8);
            assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
        }
        d.regs[DART_TCR(&d,source)/4]=old;
    }
    port.entries[3]=0x80010100; /* an old incorrectly indexed candidate SID */
    assert(apple_pcie_managed_admit(&port)==-EBUSY);
    port.entries[3]=0x80030100; /* candidate RID on a different SID */
    assert(apple_pcie_managed_admit(&port)==-EBUSY);
    port.entries[3]=0x80040300; /* inconsistent foreign index/SID */
    assert(apple_pcie_managed_admit(&port)==-EBUSY);
    port.entries[3]=0x80030300; port.entries[4]=0x80040300;
    assert(apple_pcie_managed_admit(&port)==-EBUSY); /* duplicate RID */
    port.entries[4]=0;
    assert(apple_pcie_managed_admit(&port)==0); /* unrelated consistent owner preserved */
    port.entries[3]=0;
    for (unsigned sid=1; sid<=2; sid++) {
        d.regs[DART_TTBR(&d,sid,0)/4]|=hw.ttbr_valid;
        assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
        d.regs[DART_TTBR(&d,sid,0)/4]&=~hw.ttbr_valid;
        d.regs[DART_TCR(&d,sid)/4]=6;
        assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
        d.regs[DART_TCR(&d,sid)/4]=0;
        assert(apple_pcie_radio_check_cold(&port)==0);
        d.regs[DART_TCR(&d,sid)/4]=1;
    }
    const u32 foreign_modes[]={2,4,6,0x11,0x80,0x100,0x180};
    for (unsigned i=0; i<sizeof(foreign_modes)/sizeof(foreign_modes[0]); i++) {
        d.regs[DART_TCR(&d,1)/4]=foreign_modes[i];
        assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
    }
    d.regs[DART_TCR(&d,1)/4]=1;
    for (unsigned sid=0; sid<16; sid++) {
        port.entries[sid]=PORT_RID2SID_VALID|0x300;
        assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
        port.entries[sid]=0;
    }
    port.link|=PORT_LINKSTS_UP; assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
    port.link&=~PORT_LINKSTS_UP;
    port.link|=PORT_LINKSTS_BUSY; assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
    port.link&=~PORT_LINKSTS_BUSY;
    port.status=0; assert(apple_pcie_radio_check_cold(&port)==-EBUSY); port.status=0xd;
    d.managed_failed=true; assert(apple_pcie_radio_check_cold(&port)==-EIO); d.managed_failed=false;
    d.pending=true; assert(apple_pcie_radio_check_cold(&port)==-EIO); d.pending=false;
    d.iommu.ready=false; assert(apple_pcie_radio_check_cold(&port)==-EPROBE_DEFER); d.iommu.ready=true;
    radio_provider.dev.bound=false; assert(apple_pcie_radio_check_cold(&port)==-EPROBE_DEFER); radio_provider.dev.bound=true;
    radio_provider.dev.driver=NULL; assert(apple_pcie_radio_check_cold(&port)==-EPROBE_DEFER);
    radio_provider.dev.driver=&apple_dart_driver.driver;
    provider_present=false; assert(apple_pcie_radio_check_cold(&port)==-EPROBE_DEFER); provider_present=true;
    mismatched_map=true; assert(apple_pcie_radio_check_cold(&port)==-EINVAL); mismatched_map=false;
    map_result[1]=-ENODEV; assert(apple_pcie_radio_check_cold(&port)==-ENODEV); map_result[1]=0;
    assert(!memcmp(before,d.regs,sizeof(before)) && !node_refs && !provider_refs);
    assert(!radio_provider.dev.locked && !d.managed_lock);
    d.sid16_ops=(void *)1; d.sid16_ttbr=0x4001;
    d.regs[DART_TCR(&d,16)/4]=1; d.regs[DART_TTBR(&d,16,0)/4]=0x4001;
    assert(apple_pcie_radio_check_cold(&port)==0);
    d.regs[hw.enable_streams/4]&=~BIT(16);
    assert(apple_pcie_radio_check_cold(&port)==-EBUSY);
    d.regs[hw.enable_streams/4]|=BIT(16);
    assert(apple_pcie_radio_check_cold(&port)==0);
    d.sid16_ops=NULL; d.sid16_ttbr=0;
    d.regs[DART_TCR(&d,16)/4]=before[DART_TCR(&d,16)/4];
    d.regs[DART_TTBR(&d,16,0)/4]=before[DART_TTBR(&d,16,0)/4];
    puts("radio normal SIDs: admission, scoped publication, foreign preservation, TLB failure rollback, quarantine, group merge and checked cold start PASS");
}
'''
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='radio-sids-') as tmp:
        c=Path(tmp)/'sids.c'; exe=Path(tmp)/'sids'
        assert code.count('if (error_mask)') == 1
        for control in ["production", "ignored-error-mask", "late-kexec-guard", "missing-failure-quarantine"]:
            tested = code
            if control == "ignored-error-mask":
                tested = code.replace('if (error_mask)', 'if (false && error_mask)')
            elif control == "late-kexec-guard":
                block = "\t\tret = apple_dart_retain_radio(dart);\n\t\tif (ret)\n\t\t\tgoto err_clk_disable;\n"
                assert tested.count(block) == 1
                tested = tested.replace(block, '')
                enable = "\t\tapple_dart_writel(dart, dart->managed_sids, dart->hw->enable_streams);"
                assert tested.count(enable) == 1
                tested = tested.replace(enable, enable + "\n" + block)
            elif control == "missing-failure-quarantine":
                block = "\tif (dart->managed_sids && dart->retained && !READ_ONCE(dart->managed_failed))\n\t\tapple_dart_managed_quarantine(dart);\n"
                assert tested.count(block) == 1
                tested = tested.replace(block, '')
            c.write_text(tested)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-Wno-unused-parameter',str(c),'-o',str(exe)],check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if control != "production":
                assert result.returncode and 'Assertion' in result.stderr, result
                print('Managed radio ' + control + ' negative control: detected at runtime')
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())


if __name__ == '__main__':
    main()
