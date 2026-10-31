/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Minimal CPU/DMA test doubles; no device IO or Linux kernel build required. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8, __u8;
typedef int8_t s8, __s8;
typedef uint16_t u16, __u16;
typedef int16_t s16, __s16;
typedef uint32_t u32, __u32, __le32;
typedef int32_t s32, __s32;
typedef uint64_t u64, __u64, dma_addr_t;
typedef int64_t s64, __s64;
typedef int irqreturn_t;
#define BIT(x) (1UL << (x))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define FIELD_PREP(m,v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ALIGN(v,a) (((v)+(a)-1)&~((typeof(v))(a)-1))
#define ALIGN_DOWN(v,a) ((v)&~((typeof(v))(a)-1))
#define DIV_ROUND_UP(v,d) (((v)+(d)-1)/(d))
#define clamp_val(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define S16_MIN INT16_MIN
#define S16_MAX INT16_MAX
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((u64)(x)>>32))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define GFP_KERNEL 0
#define WARN_ON(x) (!!(x))
#define kzalloc(n,f) calloc(1,n)
#define kfree(p) free(p)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static u16 get_unaligned_le16(const void *p) { const u8 *b=p; return b[0]|b[1]<<8; }
static u32 get_unaligned_le32(const void *p) { const u8 *b=p; return b[0]|b[1]<<8|b[2]<<16|(u32)b[3]<<24; }
static void put_unaligned_le16(u16 x, void *p) { u8 *b=p; b[0]=x; b[1]=x>>8; }
static void put_unaligned_le32(u32 x, void *p) { u8 *b=p; for(int i=0;i<4;i++) b[i]=x>>(i*8); }
typedef struct { unsigned int n; } refcount_t;
static void refcount_set(refcount_t *r,unsigned int n) {r->n=n;}
static void refcount_inc(refcount_t *r) {assert(r->n);r->n++;}
static bool refcount_dec_and_test(refcount_t *r) {assert(r->n);return !--r->n;}
static int test_and_set_bit(unsigned int n,unsigned long *p) {int b=!!(*p&BIT(n));*p|=BIT(n);return b;}
static void clear_bit(unsigned int n,unsigned long *p) {*p&=~BIT(n);}
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(h) ((h)->next=(h)->prev=(h))
static void list_add_tail(struct list_head *n,struct list_head *h) {n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head *n) {n->prev->next=n->next;n->next->prev=n->prev;}
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry(p,h,m) for(p=list_entry((h)->next,typeof(*p),m); &p->m!=(h);p=list_entry(p->m.next,typeof(*p),m))
#define list_for_each_entry_safe(p,q,h,m) for(p=list_entry((h)->next,typeof(*p),m),q=list_entry(p->m.next,typeof(*p),m);&p->m!=(h);p=q,q=list_entry(q->m.next,typeof(*q),m))
#define V4L2_CID_CODEC_STATELESS_BASE 0
