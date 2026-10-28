#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Build the exact kernel ledger/payload helpers as an offline host test library.

No kernel driver, device file or hardware is opened. Minimal headers implement
only the API primitives the pure ledger uses. KUnit separately exercises the
same source against real kernel primitives. --vendor uses a pinned source copy
for the MIT daemon's portable tests; --kernel reads the Linux tree directly.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess

parser=argparse.ArgumentParser()
source=parser.add_mutually_exclusive_group(required=True)
source.add_argument('--kernel',type=Path)
source.add_argument('--vendor',type=Path)
parser.add_argument('--out',required=True,type=Path)
args=parser.parse_args()
out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
if args.kernel:
    root=args.kernel.resolve()
    files={
        'speaker-energy.c':root/'sound/soc/apple/speaker-energy.c',
        'speaker-energy.h':root/'sound/soc/apple/speaker-energy.h',
        'apple-admac-payload.h':root/'drivers/dma/apple-admac-payload.h',
        'apple-admac.h':root/'include/linux/dma/apple-admac.h',
        'apple-speaker.h':root/'include/uapi/sound/apple-speaker.h',
        'gate-host.c':Path(__file__).with_name('gate-host.c'),
    }
else:
    pinned=json.loads((args.vendor/'SOURCE.json').read_text())
    for name,identity in pinned.items():
        assert hashlib.sha256((args.vendor/name).read_bytes()).hexdigest()==identity['sha256'],name
    files={name:args.vendor/name for name in ['speaker-energy.c','speaker-energy.h',
        'apple-admac-payload.h','apple-admac.h','apple-speaker.h','gate-host.c']}
headers={
'linux/types.h':r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint8_t __u8; typedef uint32_t __le32; typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define cpu_to_le32(x) ((uint32_t)(x))
#define cpu_to_le64(x) ((uint64_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))
#define le64_to_cpu(x) ((uint64_t)(x))
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "host model needs LE");
''',
'linux/spinlock.h':r'''#pragma once
#include <pthread.h>
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(p) pthread_mutex_init((p),0)
#define spin_lock_irqsave(p,f) do {(f)=0;pthread_mutex_lock(p);}while(0)
#define spin_unlock_irqrestore(p,f) do {(void)(f);pthread_mutex_unlock(p);}while(0)
''',
'linux/errno.h':'#pragma once\n#include <asm-generic/errno.h>\n',
'linux/kconfig.h':'#pragma once\n#define IS_REACHABLE(x) 0\n',
'linux/dmaengine.h':'#pragma once\n#include <linux/types.h>\nstruct dma_chan;\n',
'linux/module.h':'#pragma once\n#define MODULE_LICENSE(x)\n#define EXPORT_SYMBOL_GPL(x)\n',
'linux/overflow.h':'#pragma once\n#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)\n',
'linux/string.h':'#pragma once\n#include <string.h>\n',
'linux/ktime.h':r'''#pragma once
#include <linux/types.h>
extern _Thread_local u64 apple_test_clock_ns;
static inline u64 ktime_get_boottime_ns(void) {return apple_test_clock_ns;}
''',
'linux/math.h':r'''#pragma once
#include <linux/types.h>
#define min(a,b) ({__typeof__(a) _a=(a);__typeof__(b) _b=(b);_a<_b?_a:_b;})
#define abs(a) ({__typeof__(a) _a=(a);_a<0?-_a:_a;})
#define DIV_ROUND_UP_ULL(a,b) (((u64)(a)+(b)-1)/(b))
static inline u64 int_sqrt64(u64 x) {
    u64 lo=0,hi=1ULL<<32;
    while(lo+1<hi) {u64 mid=(lo+hi)/2;if(mid<=x/mid)lo=mid;else hi=mid;}
    return lo;
}
static inline s32 sign_extend32(u32 x,unsigned bit) {
    u32 m=1U<<bit;return (s32)((x&m)?(x|(~0U<<bit)):(x&((m<<1)-1)));
}
''',
'linux/math64.h':'#pragma once\n#include <linux/types.h>\nstatic inline u64 div_u64(u64 x,u32 y){return x/y;}\n',
'linux/unaligned.h':r'''#pragma once
#include <linux/types.h>
#include <string.h>
static inline u16 get_unaligned_le16(const void *p){u16 v;memcpy(&v,p,2);return v;}
static inline u32 get_unaligned_le32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static inline void put_unaligned_le16(u16 v,void *p){memcpy(p,&v,2);}
static inline void put_unaligned_le32(u32 v,void *p){memcpy(p,&v,4);}
''',
'sound/pcm.h':r'''#pragma once
/* Values are from the public ALSA syscall ABI. */
#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_FORMAT_S24_LE 6
#define SNDRV_PCM_FORMAT_S32_LE 10
''',
}
for name,text in headers.items():
    p=out/'shim'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
for name,path in files.items():shutil.copy2(path,out/name)
(out/'shim/linux/dma').mkdir(parents=True,exist_ok=True)
shutil.copy2(files['apple-admac.h'],out/'shim/linux/dma/apple-admac.h')
manifest={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in files.items()}
(out/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
subprocess.run([os.environ.get('HOST_CC','cc'),'-std=gnu11','-O2','-g','-fPIC','-shared',
    '-pthread','-Wall','-Wextra','-Wno-unused-parameter','-I'+str(out/'shim'),'-I'+str(out),
    str(out/'speaker-energy.c'),str(out/'gate-host.c'),'-lm','-o',str(out/'libapple-speaker-energy.so')],check=True)
