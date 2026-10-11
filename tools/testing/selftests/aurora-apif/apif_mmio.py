#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host fault injection against production APIF functions; no firmware calls."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"^.*\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise RuntimeError(f"missing production function {name}")
    start = match.start()
    pos = match.end() - 1
    depth = 0
    for end in range(pos, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if not depth:
                return source[start:end + 1]
    raise RuntimeError(f"unterminated function {name}")


HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uintptr_t phys_addr_t;
#define __packed __attribute__((packed))
#define U64_MAX UINT64_MAX
#define PHYS_ADDR_MAX UINTPTR_MAX
#define SZ_16K 16384UL
#define SZ_4G (1ULL << 32)
#define PAGE_SIZE SZ_16K
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_CACHE 4
#define IOMMU_NOEXEC 8
#define IOMMU_MMIO 16
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define IS_REACHABLE(x) 1
#define GFP_ATOMIC 0
#define kmalloc(n,flags) malloc(n)
#define virt_to_phys(p) ((u64)(uintptr_t)(p))
#define round_down(v,a) ((v) & ~((a)-1))
#define min(a,b) ((a)<(b)?(a):(b))
#define READ_ONCE(v) (v)
#define raw_spin_lock_irqsave(lock,flags) ((void)(lock), (flags)=0)
#define raw_spin_unlock_irqrestore(lock,flags) ((void)(lock), (void)(flags))
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
'''
api = (ROOT / "include/linux/soc/aurora/apif-mmio.h").read_text()
api = "\n".join(line for line in api.splitlines() if not line.startswith("#include"))
HEADER += api + "\n"


def transport_test():
    submit = function("drivers/firmware/aurora-apif-mmio.c", "aurora_apif_submit")
    return HEADER + r'''
struct apif_batch { u32 count, flags; s32 status; u32 done; struct aurora_apif_op ops[3]; };
struct aurora_apif { int lock; bool live; struct apif_batch *batch; u32 max_ops; };
static unsigned rings;
static int mode;
static void apif_ring(struct aurora_apif *apif)
{
    rings++;
    apif->batch->done = apif->batch->count;
    apif->batch->status = 0;
    for (u32 i = 0; i < apif->batch->count; i++) apif->batch->ops[i].ret = U64_MAX;
    if (mode == 1) { apif->batch->done = 1; apif->batch->status = -EINVAL; }
    if (mode == 2) apif->batch->done++;
    if (mode == 3) { apif->batch->done = 0; apif->batch->status = -EINPROGRESS; }
    if (mode == 4) { apif->batch->done = 0; apif->batch->status = 1; }
    if (mode == 5) { apif->batch->done = 1; }
}
''' + submit + r'''
int main(void)
{
    struct apif_batch batch = {};
    struct aurora_apif client = { .live = true, .batch = &batch, .max_ops = 3 };
    struct aurora_apif_op ops[8] = {};
    u32 done;
    assert(!aurora_apif_submit(&client, ops, 8, &done));
    assert(done == 8 && rings == 3 && ops[7].ret == U64_MAX);
    assert(!aurora_apif_submit(&client, NULL, 0, &done) && !done && rings == 3);
    assert(aurora_apif_submit(&client, NULL, 1, &done) == -EINVAL && !done);
    mode = 1;
    assert(aurora_apif_submit(&client, ops, 8, &done) == -EINVAL && done == 1);
    mode = 2;
    assert(aurora_apif_submit(&client, ops, 8, &done) == -EIO && !done);
    mode = 3;
    assert(aurora_apif_submit(&client, ops, 8, &done) == -EIO && !done);
    mode = 4;
    assert(aurora_apif_submit(&client, ops, 8, &done) == -EIO && !done);
    mode = 5;
    assert(aurora_apif_submit(&client, ops, 8, &done) == -EIO && done == 1);
    client.live = false;
    assert(aurora_apif_submit(&client, ops, 1, &done) == -ENODEV && !done);
    return 0;
}
'''


def dart_test():
    path = "drivers/iommu/apple-dart-apif.c"
    source = (ROOT / path).read_text()
    constants = source[source.index("#define APIF_DART_INIT"):source.index("static const struct iommu_ops")]
    funcs = "\n".join(function(path, name) for name in
                      ["apif_force_iovas", "apif_one", "apif_prepare_table", "apif_boot_query", "apif_ring"])
    return HEADER + constants + r'''
struct apif_retained_sid { unsigned root_level; };
struct apple_dart_apif {
    struct aurora_apif *apif;
    struct aurora_apif_op native_ops[2 * APIF_BATCH_MAX_OPS];
    struct aurora_apif_boot_iommu *boot_info;
    u64 *page_lists[APIF_BATCH_MAX_OPS], page_list_pa[APIF_BATCH_MAX_OPS];
    u32 dart_id, num_streams;
    u64 iova_force;
    bool ownership_uncertain;
    struct apif_retained_sid *retained[DART_APIF_MAX_STREAMS];
    struct apif_batch *batch;
};
static struct aurora_apif_op calls[2048];
static unsigned ncalls;
static u64 boot_table, found_table, retype_result = 1, fail_selector;
static int fail_status;
static int native_done = -1;
int aurora_apif_submit(struct aurora_apif *client, struct aurora_apif_op *ops, u32 count, u32 *done)
{
    (void)client;
    if (done) *done = 0;
    for (u32 i = 0; i < count; i++) {
        struct aurora_apif_op *op = &ops[i];
        calls[ncalls++] = *op;
        if (fail_selector && op->selector == fail_selector) return fail_status;
        if (native_done >= 0 && (op->selector >> 32) != AURORA_APIF_SERVICE && i >= (u32)native_done)
            return -EIO;
        op->ret = 0;
        if (op->selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_TRANSLATE))
            op->ret = op->arg[0] + SZ_4G;
        if (op->selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_BOOT_TABLE))
            op->ret = boot_table;
        if (op->selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_FIND_FRAME))
            op->ret = found_table;
        if (op->selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_ALLOC_FRAME))
            op->ret = 0x90000000;
        if (op->selector == AURORA_APIF_SELECTOR(0, 1)) op->ret = retype_result;
        /* A void endpoint is allowed to return nonzero. */
        if (op->selector == AURORA_APIF_SELECTOR(3, 0)) op->ret = U64_MAX;
        if (done) *done = i + 1;
    }
    return 0;
}
''' + funcs + r'''
int main(void)
{
    struct apple_dart_apif dart = { .dart_id = 5, .num_streams = 16 };
    struct aurora_apif_boot_iommu info = {};
    dart.boot_info = &info;
    dart.batch = calloc(1, PAGE_SIZE);
    assert(dart.batch);
    boot_table = 0x80000000;
    assert(!apif_prepare_table(&dart, 3, 0x2000000, 2) && ncalls == 1);
    boot_table = 0; found_table = 0x80000000; ncalls = 0;
    assert(!apif_prepare_table(&dart, 3, 0x2000000, 2) && ncalls == 2);
    found_table = 0; ncalls = 0;
    assert(!apif_prepare_table(&dart, 3, 0x2000000, 2) && ncalls == 6);
    assert(calls[3].selector == AURORA_APIF_SELECTOR(0, 1));
    assert(calls[3].arg[0] == 0x90000000 && calls[3].arg[1] == 11 && calls[3].arg[2] == 24);
    assert(calls[4].selector == AURORA_APIF_SELECTOR(3, 0));
    assert(calls[4].arg[0] == 5 && calls[4].arg[1] == 3 && calls[4].arg[4] == 0x90000000);
    assert(calls[5].selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_TAG_FRAME));
    retype_result = 0; ncalls = 0;
    assert(apif_prepare_table(&dart, 3, 0, 2) == -EIO && ncalls == 5);
    assert(calls[4].selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_FREE_FRAME));
    retype_result = 1; ncalls = 0;
    fail_selector = AURORA_APIF_SELECTOR(3, 0); fail_status = -ETIMEDOUT;
    assert(apif_prepare_table(&dart, 3, 0, 2) == -ETIMEDOUT && dart.ownership_uncertain);
    assert(apif_ring(&dart) == -EIO && !dart.batch->done);
    dart.ownership_uncertain = false; fail_selector = 0; ncalls = 0;
    dart.iova_force = 1ULL << 40;
    dart.batch->count = 2;
    for (unsigned i = 0; i < 2; i++) {
        dart.batch->op[i] = (struct apif_op){ .op = APIF_DART_MAP,
            .arg = { 3, i * 0x4000, 0x800000 + i * 0x4000, 0x4000, 3 | IOMMU_NOEXEC } };
    }
    assert(!apif_ring(&dart) && dart.batch->done == 2 && ncalls == 6);
    assert(calls[4].selector == AURORA_APIF_SELECTOR(3, 2));
    assert(calls[4].arg[2] == (1ULL << 40));
    assert(calls[4].arg[3] == virt_to_phys(dart.page_lists[0]) + SZ_4G);
    assert(dart.page_lists[0][0] == 0x100800000ULL);
    assert(calls[4].arg[5] == 4 && dart.batch->op[0].arg[1] == 0);
    ncalls = 0; native_done = 1;
    assert(apif_ring(&dart) == -EIO && dart.batch->done == 1);
    assert(dart.batch->op[0].arg[1] == 0 && dart.batch->op[1].arg[1] == 0x4000);
    native_done = -1; ncalls = 0;
    dart.batch->op[0].arg[4] = 256;
    assert(apif_ring(&dart) == -EINVAL && !ncalls && !dart.batch->done);
    for (unsigned i = 0; i < APIF_BATCH_MAX_OPS; i++) free(dart.page_lists[i]);
    free(dart.batch);
    return 0;
}
'''


def sart_test():
    return HEADER + r'''
struct device {};
struct mutex { int unused; };
struct sart_apif {
    struct device *dev;
    struct aurora_apif *apif;
    struct mutex lock;
    bool active;
    struct { phys_addr_t address; size_t size; } regions[16];
};
struct apple_sart { struct sart_apif *state; };
#define SART_APIF_MAX_REGIONS 16
#define apple_sart_backend_data(sart) ((sart)->state)
#define dma_to_phys(dev,address) (address)
static struct aurora_apif_op calls[8];
static unsigned ncalls;
static u64 unmap_result;
int aurora_apif_submit(struct aurora_apif *client, struct aurora_apif_op *ops, u32 count, u32 *done)
{
    (void)client; (void)done;
    assert(count == 1);
    calls[ncalls++] = *ops;
    ops->ret = U64_MAX;
    if (ops->selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE, AURORA_APIF_TRANSLATE))
        ops->ret = ops->arg[0] + SZ_4G;
    if (ops->selector == AURORA_APIF_SELECTOR(5, 2)) ops->ret = unmap_result;
    return 0;
}
''' + function("drivers/soc/apple/apple-sart-apif.c", "sart_apif_region") + r'''
int main(void)
{
    struct sart_apif state = {};
    struct apple_sart sart = { &state };
    assert(sart_apif_region(&sart, 0, 0, true) == -EINVAL);
    assert(sart_apif_region(&sart, 1, SZ_16K, true) == -EINVAL);
    assert(!sart_apif_region(&sart, 0x800000, SZ_16K, true));
    assert(ncalls == 3 && state.active && state.regions[0].size == SZ_16K);
    assert(calls[2].selector == AURORA_APIF_SELECTOR(5, 1) && calls[2].arg[0] == 0x100800000ULL);
    ncalls = 0; unmap_result = 1;
    assert(sart_apif_region(&sart, 0x800000, SZ_16K, false) == -EIO);
    assert(state.regions[0].size == SZ_16K);
    ncalls = 0; unmap_result = 0;
    assert(!sart_apif_region(&sart, 0x800000, SZ_16K, false) && !state.regions[0].size);
    assert(sart_apif_region(&sart, 0x800000, SZ_16K, false) == -ENOENT);
    return 0;
}
'''


def nvme_test():
    helpers = "\n".join(function("drivers/nvme/host/apple-apif.h", name) for name in
                         ["apple_nvme_apif_translate", "apple_nvme_apif_request"])
    return HEADER + r'''
#define SZ_4K 4096UL
#define APPLE_NVME_APIF_PAGES 257
#define APPLE_NVME_APIF_LIST_SIZE SZ_4K
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define IS_ALIGNED(n,a) (!((n)&((a)-1)))
#define lockdep_assert_held(p) ((void)(p))
#define dma_wmb() ((void)0)
#define cpu_to_le16(v) (v)
#define le64_to_cpu(v) (v)
typedef uint64_t __le64;
struct command { struct { struct {u64 prp1, prp2;} dptr; u32 command_id; } common; };
struct apple_nvmmu_tcb { u32 pad; unsigned short length; char rest[122]; };
struct apple_nvme_iod { struct command cmd; bool apif_registered; __le64 *prps; };
struct request { struct apple_nvme_iod iod; u32 bytes; };
struct apple_nvme_hw { u32 max_queue_depth; };
struct apple_nvme { void *dev; int lock; struct aurora_apif *apif; struct apple_nvme_hw *hw;
    struct aurora_apif_op *apif_ops; void *apif_scratch; u64 apif_scratch_dma; bool apif_uncertain; };
struct apple_nvme_queue { struct apple_nvme *anv; bool is_adminq; struct apple_nvmmu_tcb tcbs[64]; };
static struct apple_nvme *queue_to_apple_nvme(struct apple_nvme_queue *q) {return q->anv;}
static struct apple_nvme_iod *blk_mq_rq_to_pdu(struct request *r) {return &r->iod;}
static u32 nvme_tag_from_cid(u32 tag) {return tag;}
static u32 blk_rq_payload_bytes(struct request *r) {return r->bytes;}
static void **apple_nvme_iod_list(struct request *r) {return (void **)&r->iod.prps;}
static u64 dma_to_phys(void *dev, u64 p) {(void)dev; return p;}
static int map_calls, translate_calls, mode;
static struct aurora_apif_op mapped;
int aurora_apif_submit(struct aurora_apif *apif, struct aurora_apif_op *ops, u32 n, u32 *done) {
    (void)apif; if(done) *done=n;
    for(u32 i=0;i<n;i++) {
        if(ops[i].selector == AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,AURORA_APIF_TRANSLATE)) {
            translate_calls++; ops[i].ret=ops[i].arg[0]+(mode==3?SZ_16K:0);
        } else if(ops[i].selector == AURORA_APIF_SELECTOR(6,1)) {
            mapped=ops[i]; map_calls++; ops[i].ret=U64_MAX;
            if(mode==1) return -EIO;
        } else { assert(ops[i].selector==AURORA_APIF_SELECTOR(6,2)); ops[i].ret=mode==2?0:1; }
    }
    return 0;
}
''' + helpers + r'''
int main(void) {
    struct apple_nvme_hw hw={64};
    struct aurora_apif_op ops[257];
    unsigned char scratch[SZ_16K];
    struct apple_nvme anv={.hw=&hw,.apif_ops=ops,.apif_scratch=scratch,.apif_scratch_dma=0x800000};
    struct apple_nvme_queue q={.anv=&anv,.is_adminq=true};
    struct request r={.iod.cmd.common={.dptr={0x100000,0},.command_id=3}};
    /* A no-payload Create SQ still admits its queue page. MAP is void. */
    assert(!apple_nvme_apif_request(&q,&r,true));
    assert(map_calls==1 && mapped.arg[4]==1 && mapped.arg[1]==3);
    assert(mapped.arg[2]==0x801000 && mapped.arg[3]==0x800000 && r.iod.apif_registered);
    mode=2; assert(apple_nvme_apif_request(&q,&r,false)==-EIO && r.iod.apif_registered);
    mode=0; assert(!apple_nvme_apif_request(&q,&r,false) && !r.iod.apif_registered);
    mode=1; assert(apple_nvme_apif_request(&q,&r,true)==-EIO);
    assert(r.iod.apif_registered && anv.apif_uncertain);
    int maps=map_calls; assert(apple_nvme_apif_request(&q,&r,true)==-EIO && map_calls==maps);
    mode=0; assert(!apple_nvme_apif_request(&q,&r,false)); anv.apif_uncertain=false;
    mode=3; assert(apple_nvme_apif_request(&q,&r,true)==-EINVAL && map_calls==maps);
    mode=0; __le64 prps[256]; for(int i=0;i<256;i++) prps[i]=0x200000+(u64)i*SZ_4K;
    r.bytes=1024*1024; r.iod.prps=prps; r.iod.cmd.common.dptr.prp1=0x100004;
    assert(!apple_nvme_apif_request(&q,&r,true) && mapped.arg[4]==257);
    assert(((struct apple_nvmmu_tcb *)(scratch+SZ_4K))->length==255);
    assert(translate_calls>=257);
    return 0;
}
'''


def main():
    compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
    print("TAP version 13\n1..4", flush=True)
    with tempfile.TemporaryDirectory(prefix="apif-selftest-") as directory:
        for i, (name, test) in enumerate([( "transport", transport_test),
                                        ("dart", dart_test), ("sart", sart_test), ("nvme", nvme_test)], 1):
            path = Path(directory) / name
            path.with_suffix(".c").write_text(test())
            subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                       str(path.with_suffix(".c")), "-o", str(path)], check=True)
            subprocess.run([str(path)], check=True)
            print(f"ok {i} - {name} production function", flush=True)


if __name__ == "__main__":
    main()
