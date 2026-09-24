#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real private reader against synthetic, non-executable DATA."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'drivers/soc/apple/pmp-v2-sample.h').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint16_t u16;
#define BIT(x) (1U<<(x))
#define BIT_ULL(x) (1ULL<<(x))
#define GENMASK_ULL(h,l) ((~0ULL>>(63-(h)))&(~0ULL<<(l)))
#define round_down(v,a) ((v)&~((a)-1))
#define max(a,b) ((a)>(b)?(a):(b))
#define __ffs(v) __builtin_ctz(v)
static int sign_extend32(unsigned x,unsigned sign)
{ return (int)(x<<(31-sign))>>(31-sign); }
static unsigned char memory[0x5c000];
struct pmp_v2 { int lock; };
struct pmp_data_field { const char *name; unsigned offset,width; };
static const struct pmp_data_field pmp_data_timer[]={
    {"frequency",0x20,4},{"offset",0x38,8},{"subtract",0x5c,1}};
static bool live=true,busy;
static unsigned reads;
static u64 counter=1000000;
static bool mutex_trylock(int *lock) { (void)lock; return !busy; }
static void mutex_unlock(int *lock) { (void)lock; }
static bool pmp_data_live(struct pmp_v2 *pmp) { (void)pmp; return live; }
static unsigned arch_timer_get_cntfrq(void) { return 1000000000; }
static u64 __arch_counter_get_cntpct(void) { return counter; }
static int pmp_sample_age(u64 c,u64 t,u64 *age)
{ if(t>UINT64_MAX/125 || t*125/3>c || c-t*125/3>=5000000) return -ETIME;
  *age=c-t*125/3; return 0; }
static int pmp_v2_heap_range(u64 va,unsigned size)
{ return va<0x105e3c0 || va>0x10963c0-size || (va&3) ? -ERANGE:0; }
static int pmp_data_read(struct pmp_v2 *pmp,u64 va,unsigned width,u64 *out)
{
    (void)pmp; reads++;
    if(va<0x103c000 || width>8 || va>0x1098000-width) return -ERANGE;
    *out=0;
    for(unsigned i=0;i<width;i++) *out|=(u64)memory[va-0x103c000+i]<<(8*i);
    return 0;
}
static int pmp_data_tuple(struct pmp_v2 *pmp,u64 va,
                          const struct pmp_data_field *f,unsigned count,u64 *out)
{ for(unsigned i=0;i<count;i++) if(pmp_data_read(pmp,va+f[i].offset,f[i].width,out+i)) return -ERANGE;
  return 0; }
static void put(u64 va,u64 value,unsigned width)
{ assert(va>=0x103c000 && va+width<=0x1098000);
  for(unsigned i=0;i<width;i++) memory[va-0x103c000+i]=value>>(8*i); }
static void init(void)
{
    memset(memory,0,sizeof(memory)); live=true; busy=false; reads=0;
    put(0x104ffb8+0x20,24000000,4); put(0x1050028,64,4);
    for(unsigned i=0;i<2;i++) {
        u64 provider=i?0x10541f0:0x1053ce0;
        u64 config=i?0x104afb0:0x104abf8;
        u64 cache=0x1060000+i*256;
        put(provider+0x38,8,8); put(provider+0x50,cache,8);
        put(provider+0x180,config,8); put(provider+0x2a4,1,4); put(provider+0x2a8,1,4);
        put(config+0x64,i?360:352,4); put(config+0x68,i?16:8,4);
        put(cache+0x30,0x8c808c80,8);
        put(cache+0x38,BIT_ULL(54)|(counter*3/125-10),8);
    }
}
'''
code += source + r'''
static void expect(int result,unsigned reason)
{
    struct pmp_v2 pmp={}; struct pmp_sample_diagnostic d;
    u64 t[2]={},age[2]={}; int temp,cluster[2];
    assert(pmp_v2_sample(&pmp,t,age,&temp,cluster,&d)==result);
    assert(d.reason==reason);
    if(!result) assert(temp==50000 && t[0] && t[1]);
}
int main(void)
{
    init(); expect(0,0);
    init(); put(0x104abf8+0x68,0,4); expect(-EAGAIN,PMP_SAMPLE_COUNT_PENDING);
    put(0x104abf8+0x68,8,4); expect(0,0);
    init(); put(0x1053ce0+0x50,0,8); expect(-EAGAIN,PMP_SAMPLE_CACHE_PENDING);
    put(0x1053ce0+0x50,0x1060000,8); expect(0,0);
    init(); put(0x1053ce0+0x2a8,0,4); expect(-EAGAIN,PMP_SAMPLE_ACTIVITY_PENDING);
    put(0x1053ce0+0x2a8,1,4); expect(0,0);
    init(); put(0x1060038,0,8); expect(-EAGAIN,PMP_SAMPLE_DELIVERY_PENDING);
    init(); put(0x1060030,0,8); expect(-EAGAIN,PMP_SAMPLE_DELIVERY_PENDING);
    init(); put(0x1053ce0+0x50,0,8); put(0x10541f0+0x2b2,1,1);
    expect(-ENODATA,PMP_SAMPLE_IDENTITY); /* Pending must not hide hard P error. */
    init(); put(0x104abf8+0x68,0,4); put(0x1053ce0+0x50,1,8);
    expect(-ENODATA,PMP_SAMPLE_POINTER);
    init(); put(0x104abf8+0x68,4,4); expect(-ENODATA,PMP_SAMPLE_CONFIG);
    init(); put(0x1060038,BIT_ULL(55),8); expect(-ENODATA,PMP_SAMPLE_METADATA);
    init(); put(0x1060038,BIT_ULL(54)|BIT_ULL(55),8); expect(-ENODATA,PMP_SAMPLE_AGE);
    init(); put(0x1060038,BIT_ULL(54)|BIT_ULL(55)|(counter*3/125-10),8);
    expect(-EAGAIN,PMP_SAMPLE_DROPPED_PENDING);
    put(0x10541f0+0x50,1,8); expect(-ENODATA,PMP_SAMPLE_POINTER);
    init(); put(0x1060038,BIT_ULL(54)|BIT_ULL(55)|(counter*3/125-10),8);
    put(0x1060030,0,8); expect(-ENODATA,PMP_SAMPLE_METADATA);
    init(); put(0x1060038,BIT_ULL(54)|(counter*3/125+1),8); expect(-ENODATA,PMP_SAMPLE_AGE);
    init(); counter=10000000; put(0x1060038,BIT_ULL(54)|1,8);
    expect(-ENODATA,PMP_SAMPLE_AGE); counter=1000000;
    init(); put(0x1060030,0xb200b200,8); expect(-ENODATA,PMP_SAMPLE_VALUE);
    init(); put(0x1050028,0,4); expect(-ENODATA,PMP_SAMPLE_TIMER);
    init(); live=false; expect(-ENODATA,PMP_SAMPLE_OWNER);
    init(); busy=true; expect(-EBUSY,0); assert(!reads);
}
'''
with tempfile.TemporaryDirectory(prefix='neo-pmp-ready-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-x', 'c', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-o', str(binary), '-'],
                   input=code, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: real reader readiness whitelist, safe pointers and hard-error precedence')
