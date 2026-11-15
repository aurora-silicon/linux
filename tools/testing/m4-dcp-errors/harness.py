# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Production C extraction and synthetic host boundaries for DCP error tests."""
import re


def block(source, name):
    for match in re.finditer(r'\b' + re.escape(name) + r'\(', source):
        start = source.rfind('\n', 0, match.start()) + 1
        # Include a split return type line where applicable.
        if source[start:match.start()].strip() == '':
            start = source.rfind('\n', 0, start - 1) + 1
        paren = source.index('(', match.start())
        depth, end = 1, paren + 1
        while depth:
            depth += (source[end] == '(') - (source[end] == ')')
            end += 1
        brace = end
        while source[brace].isspace():
            brace += 1
        if source[brace] != '{':
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end] + '\n'
    raise ValueError(name)


def defines(source, prefix):
    selected = []
    lines = iter(source.splitlines())
    for line in lines:
        if not line.startswith('#define ' + prefix):
            continue
        selected.append(line)
        while line.endswith(chr(92)):
            line = next(lines)
            selected.append(line)
    return '\n'.join(selected) + '\n'


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t phys_addr_t;
typedef unsigned int gfp_t;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define BIT(n) (1U << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define GENMASK_ULL(h,l) ((UINT64_MAX << (l)) & (UINT64_MAX >> (63-(h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctz(mask))
#define FIELD_PREP(mask,v) (((v) << __builtin_ctz(mask)) & (mask))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define KUNIT_STATIC_STUB_REDIRECT(...) do {} while(0)
#define ilog2(n) (31 - __builtin_clz(n))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
typedef int irqreturn_t;
static char last_error[1024];
static unsigned errors;
static void error_log(void *dev, const char *fmt, ...) {
    (void)dev; va_list ap; va_start(ap,fmt);
    vsnprintf(last_error,sizeof(last_error),fmt,ap); va_end(ap); errors++;
}
#define dev_err_ratelimited(...) error_log(__VA_ARGS__)
#define dev_err(...) error_log(__VA_ARGS__)
#define dev_dbg(...) do {} while(0)
#define for_each_set_bit(bit,word,size) \
    for ((bit)=0; (unsigned)(bit)<(size); (bit)++) if ((*(word)) & (1UL << (bit)))
#define __set_bit(bit,word) ((word)[(bit)/64] |= 1UL << ((bit)%64))
#define test_bit(bit,word) (!!((word)[(bit)/64] & (1UL << ((bit)%64))))
#define lockdep_assert_held(lock) assert(*(lock))
#define spin_lock_irqsave(lock,flags) do { (flags)=0; assert(!*(lock)); *(lock)=1; } while(0)
#define spin_unlock_irqrestore(lock,flags) do { (void)(flags); assert(*(lock)); *(lock)=0; } while(0)
'''


def irq_unit(dart, dcp):
    unit = COMMON + defines(dart, 'DART_') + r'''
struct apple_dart_dcp { bool failed; };
struct apple_dart {
    void *dev; u8 *regs; unsigned num_streams, pgsize; u16 version;
    bool commands_gated, managed_failed;
    u16 managed_sids;
    int lock, managed_lock;
    struct apple_dart_dcp *dcp, *frontend[2];
};
struct apple_dart_stream_map { struct apple_dart *dart; unsigned long sidmap[4]; };
struct action { char kind; u32 offset, value; };
static struct action actions[100];
static unsigned action_count, legacy_calls;
static u8 *active_regs;
static void record(char kind, u32 offset, u32 value) {
    assert(action_count < ARRAY_SIZE(actions));
    actions[action_count++] = (struct action){kind, offset, value};
}
static u32 readl(const void *p) {
    u32 value; memcpy(&value,p,4); record('r',(const u8 *)p-active_regs,value); return value;
}
static void writel(u32 value, void *p) {
    u32 offset = (u8 *)p-active_regs;
    record('w',offset,value);
    if ((offset >= 0x4000 && offset < 0x4020) || offset == DART_T8110_BLOCK_ERROR) {
        u32 old; memcpy(&old,p,4); old &= ~value; memcpy(p,&old,4);
    } else {
        memcpy(p,&value,4);
    }
    if (offset == DART_T8110_TLB_CMD) {
        u32 zero=0;
        memcpy(active_regs+DART_T8110_DIAG_LOCK,&zero,4);
    }
}
#define dsb(x) record('b',0,0)
#define dma_wmb() record('d',0,0)
static u32 apple_dart_readl(struct apple_dart *d, u32 offset) { return readl(d->regs+offset); }
static void apple_dart_writel(struct apple_dart *d,u32 value,u32 offset) { writel(value,d->regs+offset); }
#define read_poll_timeout_atomic(fn,val,cond,delay,timeout,sleep,d,off) \
    (((val)=fn(d,off)), (cond) ? 0 : -ETIMEDOUT)
static irqreturn_t apple_dart_t8110_legacy_irq(int irq,void *dev) {
    (void)irq; (void)dev; legacy_calls++; return IRQ_NONE;
}
'''
    unit += block(dcp, 'apple_dart_has_borrowed')
    unit += block(dcp, 'apple_dart_borrowed_sid')
    unit += r'''
/* DMA-disable MMIO is separately covered by the radio SID tests. */
static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *stream) {
    record('q', 0, stream->sidmap[0]);
}
'''
    unit += block(dart, 'apple_dart_managed_quarantine_locked')
    unit += block(dart, 'apple_dart_managed_quarantine')
    unit += block(dart, 'apple_dart_t8110_hw_tlb_command_range')
    unit += block(dart, 'apple_dart_t8110_v2_unlock')
    unit += block(dart, 'apple_dart_t8110_v2_fault_name')
    if 'static void apple_dart_quarantine_borrowed(' in dart:
        unit += block(dart, 'apple_dart_quarantine_borrowed')
    for name in ['apple_dart_t8110_v2_block_pending', 'apple_dart_t8110_v2_irq',
                 'apple_dart_dcp_irq', 'apple_dart_t8110_irq']:
        unit += block(dart,name)
    unit += r'''
static void setreg(u8 *regs,u32 offset,u32 value) { memcpy(regs+offset,&value,4); }
static int find_action(char kind,u32 offset,u32 value) {
    for(unsigned i=0;i<action_count;i++)
        if(actions[i].kind==kind && actions[i].offset==offset && actions[i].value==value) return i;
    return -1;
}
static void inject_fault(u8 *regs, unsigned sid) {
    setreg(regs,DART_T8110_EXCEPTION(0),BIT(sid));
    setreg(regs,DART_T8110_ERR_SID_STATUS(sid),DART_T8110_ERR_SID_STATUS_WRITE|1);
    setreg(regs,DART_T8110_EXCEPTION_ADDR_LO(sid),0x12345000);
    setreg(regs,DART_T8110_EXCEPTION_ADDR_HI(sid),0x100);
    setreg(regs,DART_T8110_DIAG_LOCK,DART_T8110_DIAG_LOCK_ON_ERR);
}
int main(void) {
    static u8 regs[0x10000];
    struct apple_dart_dcp owner={0}, fe={0};
    struct apple_dart d={.regs=regs,.num_streams=32,.pgsize=16384};
    const u32 unlock=FIELD_PREP(DART_T8110_TLB_CMD_OP,DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK);
    active_regs=regs;
    /* Both retained profiles quarantine owned and foreign faults, without unlock. */
    for(unsigned radio=0;radio<2;radio++) for(unsigned version=0x202;version<=0x203;version++) {
        d.version=version;
        d.managed_sids=radio ? BIT(1)|BIT(2) : 0;
        d.dcp=radio ? NULL : &owner; d.frontend[0]=radio ? NULL : &fe;
        for(unsigned foreign=0;foreign<2;foreign++) {
            unsigned sid=foreign ? 5 : radio ? 1 : 23;
            memset(regs,0,sizeof(regs)); action_count=0;
            owner.failed=fe.failed=d.managed_failed=false;
            assert(apple_dart_t8110_irq(678,&d)==IRQ_NONE);
            assert(!owner.failed && !fe.failed && !d.managed_failed);
            action_count=0; inject_fault(regs,sid);
            assert(apple_dart_t8110_irq(678,&d)==IRQ_HANDLED);
            assert(d.managed_failed==!!radio);
            assert(owner.failed==!radio && fe.failed==!radio);
            assert((find_action('q',0,BIT(1)|BIT(2))>=0)==!!radio);
            int ack=find_action('w',DART_T8110_EXCEPTION(0),BIT(sid));
            int barrier=find_action('b',0,0);
            int readback=find_action('r',DART_T8110_EXCEPTION(0),0);
            assert(ack>=0 && barrier>ack && readback>barrier);
            assert(find_action('w',DART_T8110_TLB_CMD,unlock)==-1);
            assert(find_action('r',DART_T8110_ERROR,0)==-1);
            if(version==0x202) assert(find_action('r',DART_T8110_BLOCK_ERROR,0)==-1);
        }
        if(version==0x203) {
            memset(regs,0,sizeof(regs)); action_count=0;
            owner.failed=fe.failed=d.managed_failed=false;
            setreg(regs,DART_T8110_BLOCK_ERROR,7);
            setreg(regs,DART_T8110_DIAG_LOCK,DART_T8110_DIAG_LOCK_ON_ERR);
            assert(apple_dart_t8110_irq(678,&d)==IRQ_HANDLED);
            assert(d.managed_failed==!!radio && owner.failed==!radio && fe.failed==!radio);
            assert(find_action('w',DART_T8110_BLOCK_ERROR,7)>=0);
            assert(find_action('w',DART_T8110_TLB_CMD,unlock)==-1);
        }
    }
    /* An ordinary domain retains ACK/barrier/readback/FLUSH_UNLOCK ordering. */
    memset(regs,0,sizeof(regs)); action_count=0;
    d.version=0x202; d.managed_sids=0; d.dcp=NULL; d.frontend[0]=NULL;
    inject_fault(regs,3);
    assert(apple_dart_t8110_irq(678,&d)==IRQ_HANDLED);
    int ack=find_action('w',DART_T8110_EXCEPTION(0),BIT(3));
    int barrier=find_action('b',0,0), readback=find_action('r',DART_T8110_EXCEPTION(0),0);
    assert(ack>=0 && barrier>ack && readback>barrier);
    assert(find_action('w',DART_T8110_TLB_CMD,unlock)>readback);
    memset(regs,0,sizeof(regs)); action_count=0;
    assert(apple_dart_t8110_irq(678,&d)==IRQ_NONE);
    d.version=0x201;
    assert(apple_dart_t8110_irq(678,&d)==IRQ_NONE && legacy_calls==1);
    return 0;
}
'''
    return unit


def pending_unit(dart):
    unit = COMMON + defines(dart,'DART_') + r'''
struct apple_dart { u8 *regs; unsigned num_streams; u16 version; bool preserve_display; };
static unsigned reads,writes;
static u32 readl(const void *p) { u32 value;memcpy(&value,p,4);reads++;return value; }
'''
    if 'static bool apple_dart_fault_pending(' in dart:
        unit += block(dart,'apple_dart_t8110_v2_block_pending')
        unit += block(dart,'apple_dart_fault_pending')
    start=dart.index('/* Refuse an already pending inherited fault before joining the shared IRQ. */')
    start=dart.index('if (',start)+4
    depth,end=1,start
    while depth:
        depth += (dart[end]=='(') - (dart[end]==')')
        end+=1
    condition=dart[start:end-1]
    unit+='static bool probe_refuses(struct apple_dart *dart) { return '+condition+'; }\n'
    unit+=r'''
int main(void) {
    static u8 regs[0x10000]; u32 bit=BIT(23);
    struct apple_dart d={.regs=regs,.num_streams=32,.version=0x0202,.preserve_display=true};
    memcpy(regs+DART_T8110_EXCEPTION(0),&bit,4);
    assert(probe_refuses(&d));
    assert(reads==1 && writes==0);
    u32 value;memcpy(&value,regs+DART_T8110_EXCEPTION(0),4);assert(value==bit);
    memset(regs,0,sizeof(regs)); reads=0;
    assert(!probe_refuses(&d) && reads==2 && writes==0);
    bit=1;memcpy(regs+DART_T8110_ERROR_MASK,&bit,4);reads=0;
    assert(probe_refuses(&d) && reads==2 && writes==0);
    memset(regs,0,sizeof(regs));
    d.version=0x0203;bit=7;memcpy(regs+DART_T8110_BLOCK_ERROR,&bit,4);
    assert(probe_refuses(&d) && writes==0);
    return 0;
}
'''
    return unit


def unmap_unit(dcp, fmt):
    unit=COMMON+defines(fmt,'DCP_DART_')+r'''
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_NOEXEC 8
#define IOMMU_CACHE 4
#define dma_wmb() do {} while(0)
#define PAGE_SIZE 16384UL
#define PHYS_PFN(x) ((x)/PAGE_SIZE)
#define page_folio(x) (x)
struct apple_dart { void *dev; };
struct apple_dart_dcp {
    struct apple_dart *dart;unsigned sid;int lock;bool failed;
    u64 *root,*table[2048],*expected[2048],entry[2048];
    unsigned long foreign[32],shared_roots[32],arena[32],claimed[32];
    unsigned long retained;
};
struct iommu_iotlb_gather { size_t retired; };
static unsigned pin_count[3];
static unsigned fail_pin;
static int flush_error;
static bool pfn_valid(unsigned long pfn) { return pfn>=100 && pfn<=102; }
struct page { unsigned long pfn; };
static struct page pages[3];
static struct page *pfn_to_page(unsigned long pfn) {
    assert(pfn_valid(pfn)); pages[pfn-100].pfn=pfn; return &pages[pfn-100];
}
static bool PageReserved(struct page *p) { (void)p; return false; }
static bool folio_test_slab(struct page *p) { (void)p; return false; }
static unsigned page_count(struct page *p) { (void)p; return 1; }
static unsigned folio_try_get(struct page *p) {
    unsigned long pfn=p->pfn;
    assert(pfn_valid(pfn)); if(pfn==fail_pin)return 0;pin_count[pfn-100]++;return 1;
}
static int apple_dart_dcp_flush(struct apple_dart_dcp *d) { (void)d;return flush_error; }
static void iommu_iotlb_gather_add_range(struct iommu_iotlb_gather *g,u64 addr,size_t size) {
    (void)addr;g->retired+=size;
}
static unsigned long get_zeroed_page(gfp_t gfp) { (void)gfp;return (unsigned long)calloc(1,PAGE_SIZE); }
static void free_page(unsigned long p) { free((void *)p); }
static u64 virt_to_phys(void *p) { (void)p;return 0x500000; }
static u64 cmpxchg64(u64 *p,u64 old,u64 new) {u64 value=*p;if(value==old)*p=new;return value;}
'''
    unit+=block(fmt,'dcp_dart_pte_phys')
    unit+=block(dcp,'apple_dart_dcp_iova')
    unit+=block(dcp,'apple_dart_dcp_retain')
    unit+=block(dcp,'apple_dart_dcp_retain_span')
    unit+=block(dcp,'apple_dart_dcp_unmap')
    unit+=block(dcp,'apple_dart_dcp_map_owned')
    unit+=r'''
static u64 pte(unsigned long pfn) { return ((pfn*PAGE_SIZE)>>4)|1; }
int main(void) {
    struct apple_dart dart={0};
    struct apple_dart_dcp *d=calloc(1,sizeof(*d));
    static u64 root[2048], a[2048], b[2048], ae[2048],be[2048];
    d->dart=&dart;d->sid=0;d->root=root;d->table[0]=a;d->table[1]=b;
    d->expected[0]=ae;d->expected[1]=be;d->entry[0]=root[0]=1;d->entry[1]=root[1]=2;
    a[2047]=ae[2047]=pte(100);b[0]=be[0]=pte(101);
    struct iommu_iotlb_gather g={0};
    d->failed=true;
    u64 iova=DCP_DART_ALIAS+2047*PAGE_SIZE;
    assert(apple_dart_dcp_unmap(d,iova,PAGE_SIZE,2,&g)==2*PAGE_SIZE);
    assert(pin_count[0]==1 && pin_count[1]==1 && d->retained==2);
    assert(a[2047]==pte(100) && b[0]==pte(101) && g.retired==2*PAGE_SIZE);
    size_t mapped=0;
    assert(apple_dart_dcp_map_owned(d,iova,100*PAGE_SIZE,PAGE_SIZE,1,
                                  IOMMU_READ|IOMMU_WRITE,0,&mapped,false)==-EIO);
    assert(!mapped && a[2047]==pte(100));
    /* Failed pin never reports successful logical retirement. */
    fail_pin=100;g.retired=0;
    assert(!apple_dart_dcp_unmap(d,iova,PAGE_SIZE,1,&g) && !g.retired);
    fail_pin=0;d->failed=false;flush_error=0;g.retired=0;
    assert(apple_dart_dcp_unmap(d,iova,PAGE_SIZE,1,&g)==PAGE_SIZE);
    assert(!a[2047] && !ae[2047] && !d->failed);
    /* Failed flush restores the PTE and pins backing before logical retirement. */
    a[2047]=ae[2047]=pte(102);flush_error=-EIO;g.retired=0;
    assert(apple_dart_dcp_unmap(d,iova,PAGE_SIZE,1,&g)==PAGE_SIZE);
    assert(d->failed && a[2047]==pte(102) && ae[2047]==pte(102) && pin_count[2]==1);
    free(d);return 0;
}
'''
    return unit


def callback_unit(source, callbacks):
    unit=COMMON+r'''
#define dma_rmb() do {} while(0)
#define DCP_M4_RPC_SIZE 0xc0000
#define DCP_M4_PACKET_HEADER 12
struct dcp_m4_packet { u32 tag,input_size,output_size;bool remote,callback_flag; };
struct iomfb_m4 {void *dev,*rpc;int transaction;};
struct dcp_m4_runtime {struct iomfb_m4 *dcp;unsigned callbacks;};
static int dcp_m4_runtime_ops;
static int dispatch_error=-EOPNOTSUPP,finish_error,send_error;
static unsigned finish_calls,send_calls;
static u32 get_unaligned_le32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static int dcp_m4_callback_begin(int *t,void *rpc,unsigned size,u64 word,struct dcp_m4_packet *p){
    (void)t;(void)rpc;(void)size;(void)word;
    *p=(struct dcp_m4_packet){.tag=0x37363544,.input_size=128,.output_size=4};return 0;
}
static int dcp_m4_packet_offset(struct dcp_m4_packet *p,bool cb){(void)p;(void)cb;return 0;}
#define GFP_KERNEL 0
static unsigned snapshots;
static void *kmemdup(const void *p,size_t size,gfp_t flags) {
    (void)flags;void *copy=malloc(size);assert(copy);memcpy(copy,p,size);snapshots++;return copy;
}
static void kfree(void *p) { assert(snapshots);snapshots--;free(p); }
static void dcp_m4_runtime_bad_word(struct dcp_m4_runtime *r,u64 word,int ret){(void)r;(void)word;(void)ret;}
static int dcp_m4_callback_dispatch(int *ops,void *r,u32 tag,bool remote,void *input,u32 in,void *out,u32 size){
    (void)ops;(void)r;(void)tag;(void)remote;(void)input;(void)in;(void)out;(void)size;return dispatch_error;
}
static int dcp_m4_callback_end(int *t,void *rpc,unsigned size,u64 *ack){
    (void)t;(void)rpc;(void)size;*ack=0x42;finish_calls++;return finish_error;
}
static int dcp_m4_runtime_send(struct dcp_m4_runtime *r,u64 word){(void)r;(void)word;send_calls++;return send_error;}
'''
    start=callbacks.index('struct dcp_m4_callback_shape {')
    unit+=callbacks[start:callbacks.index('};',start)+2]+'\n'
    start=callbacks.index('static const struct dcp_m4_callback_shape dcp_m4_callbacks[]')
    unit+=callbacks[start:callbacks.index('};',start)+2]+'\n'
    unit+=block(callbacks,'dcp_m4_callback_method')
    unit+=block(callbacks,'dcp_m4_callback_shape')
    unit+=block(source,'dcp_m4_runtime_callback')
    unit+=r'''
int main(void){
    u8 rpc[256]={0};struct iomfb_m4 d={.rpc=rpc};struct dcp_m4_runtime r={.dcp=&d};
    assert(dcp_m4_runtime_callback(&r,2)==-EOPNOTSUPP);
    assert(errors==1 && !finish_calls && !send_calls);
    assert(strstr(last_error,"D567") && strstr(last_error,"37363544") && strstr(last_error,"128/4"));
    char error_value[24];snprintf(error_value,sizeof(error_value),"%d",-EOPNOTSUPP);
    assert(strstr(last_error,"dispatch failed") && strstr(last_error,error_value));
    dispatch_error=0;finish_error=-EPROTO;
    assert(dcp_m4_runtime_callback(&r,2)==-EPROTO);
    assert(errors==2 && finish_calls==1 && !send_calls && strstr(last_error,"finish failed"));
    finish_error=0;send_error=-EIO;
    assert(dcp_m4_runtime_callback(&r,2)==-EIO && strstr(last_error,"ack failed"));
    send_error=0;unsigned before=errors;
    assert(!dcp_m4_runtime_callback(&r,2) && errors==before && !snapshots);
    return 0;
}
'''
    return unit


def header_unit(header):
    unit=COMMON+r'''
#define IS_ENABLED(x) x
#define CONFIG_APPLE_DART 1
#define CONFIG_APPLE_DART_MODULE 1
#define CONFIG_APPLE_DART_J713_DCP 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
typedef u64 dma_addr_t;
'''
    unit+='\n'.join(line for line in header.splitlines() if not line.startswith('#include '))+'\n'
    unit+=r'''
int main(void){
    assert(apple_dart_dcp_check(NULL)==-EOPNOTSUPP);
    assert(apple_dart_dcp_check_buffer(NULL,0,0)==-EOPNOTSUPP);
    assert(apple_dart_dcp_alloc(NULL,0,NULL)==ERR_PTR(-EOPNOTSUPP));
    assert(apple_dart_dcp_shared_alloc(NULL,NULL,0,NULL,NULL)==-EOPNOTSUPP);
    assert(apple_dart_dcp_shared_map(NULL,NULL,0,0,0)==-EOPNOTSUPP);
    assert(apple_dart_dcp_scanout_alloc(NULL,0,NULL)==ERR_PTR(-EOPNOTSUPP));
    return 0;
}
'''
    return unit

def pair_teardown_unit(dart):
    unit=COMMON+r'''
struct apple_dart_dcp { bool failed; };
struct apple_dart {
    int irq; bool preserve_display;
    struct apple_dart_dcp *dcp, *frontend[2];
};
static int apple_dart_display_irq_lock, apple_dart_display_shared_irq=-1;
static struct apple_dart *apple_dart_display_irq_peers[2];
#define guard(kind) (void)
static unsigned freed;
static void free_irq(int irq,void *data) {
    assert(irq>=0);freed++;
    if(data==apple_dart_display_irq_peers) {
        assert(irq==apple_dart_display_shared_irq);
        for(unsigned i=0;i<2;i++) {
            struct apple_dart *d=apple_dart_display_irq_peers[i];
            assert(d);
            if(d->dcp)assert(d->dcp->failed);
            for(unsigned j=0;j<2;j++)if(d->frontend[j])assert(d->frontend[j]->failed);
        }
    }
}
'''
    unit+=block(dart,'apple_dart_quarantine_borrowed')
    unit+=block(dart,'apple_dart_free_irq')
    unit+=r'''
int main(void) {
    struct apple_dart_dcp firmware={0}, frontend={0}, piodma={0};
    struct apple_dart a={.irq=678,.preserve_display=true,.dcp=&firmware};
    struct apple_dart b={.irq=678,.preserve_display=true,.frontend={&frontend,&piodma}};
    apple_dart_display_irq_peers[0]=&a;apple_dart_display_irq_peers[1]=&b;
    apple_dart_display_shared_irq=678;
    apple_dart_free_irq(&a);
    assert(freed==1 && firmware.failed && frontend.failed && piodma.failed);
    assert(!apple_dart_display_irq_peers[0] && apple_dart_display_irq_peers[1]==&b);
    assert(apple_dart_display_shared_irq==-1);
    apple_dart_free_irq(&b);
    assert(freed==1 && !apple_dart_display_irq_peers[1]);
    /* A single partially admitted peer has no shared action to free. */
    apple_dart_display_irq_peers[0]=&a;
    apple_dart_free_irq(&a);
    assert(freed==1 && !apple_dart_display_irq_peers[0]);
    a.preserve_display=false;
    apple_dart_free_irq(&a);assert(freed==2);
    a.irq=-1;apple_dart_free_irq(&a);assert(freed==2);
    return 0;
}
'''
    return unit
