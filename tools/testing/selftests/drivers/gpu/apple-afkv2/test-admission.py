#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the actual inline admission helpers against host-only kernel mocks."""
from pathlib import Path
import subprocess
import tempfile
import argparse
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-root", type=Path,
                    default=Path(os.environ.get("AFKV2_SOURCE_ROOT",
                                             str(Path(__file__).resolve().parents[6]))),
                    help="kernel source tree (defaults to this script's tree)")
root = parser.parse_args().source_root.resolve()
def extract(file, name):
    s = (root / 'drivers/gpu/drm/apple' / file).read_text()
    endname = s.index(name + '(')
    start = s.rfind('static inline', 0, endname)
    brace = s.index('{', endname)
    depth = 1
    end = brace + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef int64_t s64;
typedef uint32_t u32;
#define MAX_PENDING_CMDS 16
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags)=0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define WARN_ON(x) (x)
#define smp_store_release(p,v) (*(p)=(v))
#define smp_load_acquire(p) (*(p))
#define WRITE_ONCE(p,v) ((p)=(v))
#define bitmap_empty(p, n) (!*(p))
struct apple_epic_service;
struct apple_dcp_afkep { int unused; };
struct apple_epic_service_ops {
 bool reusable;
 int (*prepare)(struct apple_epic_service *,const char *,const char *,s64);
 void (*teardown)(struct apple_epic_service *);
};
struct apple_epic_service {
 int lock; unsigned int users; u32 channel; bool enabled, torndown;
 void *cookie; const struct apple_epic_service_ops *ops;
 struct apple_dcp_afkep *ep; unsigned long cmd_map[1];
};
struct dptx_port {
 bool enabled, connected, closed; int enable_completion; u32 unit;
 struct apple_epic_service *service;
};
static void reinit_completion(int *p) { *p = 0; }
static unsigned retry_calls;
static void afk_service_retry(struct apple_dcp_afkep *ep) { retry_calls++; }
'''
functions = [('afk.h', n) for n in ['afk_service_get','afk_service_put','afk_service_matches','afk_service_reclaimable','afk_service_reinit','afk_service_discard','afk_service_publish']]
functions += [('dptx-service.h', n) for n in ['dptx_service_port','dptx_service_available','dptx_service_bind','dptx_service_publish','dptx_service_unbind']]
body = r'''
static int prepare(struct apple_epic_service *s,const char *n,const char *c,s64 u) { return 0; }
int main(void) {
 const char *n="dcpdptx-port-epic", *c="AppleDCPDPTXRemotePort";
 assert(dptx_service_port(n,c,3,true,3,1)==0);
 assert(dptx_service_port(n,c,0,true,3,1)==-EINVAL);
 assert(dptx_service_port(n,c,-1,true,3,1)==-EINVAL);
 assert(dptx_service_port(n,c,3,true,3,2)==-EINVAL);
 assert(dptx_service_port(NULL,c,3,true,3,1)==-EINVAL);
 assert(dptx_service_port(n,NULL,3,true,3,1)==-EINVAL);
 assert(dptx_service_port("bad",c,3,true,3,1)==-EINVAL);
 assert(dptx_service_port(n,"bad",3,true,3,1)==-EINVAL);
 assert(dptx_service_port(n,c,1,false,0,1)==1);
 struct apple_epic_service_ops ops={.prepare=prepare,.teardown=dptx_service_unbind};
 struct apple_epic_service_ops legacy={0};
 struct apple_epic_service a={0}, b={0}, old={0}; struct dptx_port p={0};
 assert(afk_service_reinit(&old,NULL,&legacy,8));
 assert(afk_service_matches(&old,8));
 assert(afk_service_reinit(&a,NULL,&ops,1));
 assert(!afk_service_get(&a) && !afk_service_matches(&a,1));
 afk_service_discard(&a);
 assert(afk_service_reinit(&a,NULL,&ops,1));
 assert(dptx_service_bind(&a,&p,3)==0);
 assert(!dptx_service_available(&p) && !p.service && a.cookie==&p);
 assert(!afk_service_get(&a));
 afk_service_discard(&a); /* failed OPEN */
 assert(!p.enabled && !p.service);
 assert(afk_service_reinit(&a,NULL,&ops,1));
 assert(dptx_service_bind(&a,&p,3)==0);
 afk_service_publish(&a);
 assert(!p.enabled && !p.service);
 dptx_service_publish(&a);
 assert(dptx_service_available(&p) && p.service==&a && p.service->cookie==&p);
 assert(afk_service_get(&a)==&a); afk_service_put(&a); assert(retry_calls==1 && !a.users);
 assert(afk_service_reinit(&b,NULL,&ops,2));
 assert(dptx_service_bind(&b,&p,3)==-EBUSY);
 afk_service_discard(&b); assert(p.enabled && p.service==&a);
 p.connected=true;
 a.enabled=false; a.torndown=true; dptx_service_unbind(&a);
 assert(p.closed && !p.enabled && !p.connected && a.cookie==&p);
 assert(!afk_service_reinit(&a,NULL,&ops,3));
 assert(afk_service_reinit(&b,NULL,&ops,3));
 assert(dptx_service_bind(&b,&p,3)==-EBUSY);
 afk_service_discard(&b);
 assert(p.service==&a && !afk_service_get(p.service) && !p.enabled);
 assert(afk_service_reinit(&b,NULL,&ops,4));
 afk_service_publish(&b); assert(!b.cookie && afk_service_get(&b)==&b);
 afk_service_put(&b);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'test.c'
    source.write_text(preamble+'\n'.join(extract(*x) for x in functions)+body)
    subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(source),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
print('PASS: host-extracted admission/identity/lifetime assertions (kernel mocks, no target kernel build)')
