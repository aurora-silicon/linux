#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production retained-active PMGR contract with a register test double."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[4]
p=Path(sys.argv[1]) if len(sys.argv)>1 else root/'drivers/pmdomain/apple/pmgr-pwrstate.c'
s=p.read_text();start=s.index('static int apple_pmgr_ps_set(');brace=s.index('{',start);end=brace+1;depth=1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
fn=s[start:end]
defines='\n'.join(l for l in s.splitlines() if l.startswith('#define APPLE_PMGR_'))
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U>>(31-(h)))&(~0U<<(l)))
#define FIELD_GET(mask,v) (((v)&(mask))>>__builtin_ctz(mask))
#define FIELD_PREP(mask,v) (((v)<<__builtin_ctz(mask))&(mask))
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
static bool diagnostic_trace;
''' + defines + r'''
struct device {};
struct generic_pm_domain { const char *name; };
struct regmap { u32 value; unsigned writes; u32 written[8]; int fail_read; };
struct apple_pmgr_ps { struct device *dev; struct generic_pm_domain genpd; struct regmap *regmap; u32 offset; bool force_disable,force_reset,externally_clocked,preserve_active_control,strict_state; };
#define genpd_to_apple_pmgr_ps(g) ((struct apple_pmgr_ps *)((char *)(g)-offsetof(struct apple_pmgr_ps,genpd)))
static int regmap_read(struct regmap *m,u32 offset,u32 *value) {
 assert(offset==0x180);if(m->fail_read)return m->fail_read;*value=m->value;return 0;
}
static int regmap_write(struct regmap *m,u32 offset,u32 value) {
 assert(offset==0x180&&m->writes<8);m->written[m->writes++]=value;
 /* Test double: control transitions reach their requested actual state. */
 m->value=(value&~APPLE_PMGR_PS_ACTUAL)|FIELD_PREP(APPLE_PMGR_PS_ACTUAL,FIELD_GET(APPLE_PMGR_PS_TARGET,value));return 0;
}
#define regmap_read_poll_timeout_atomic(map,offset,cur,condition,delay,timeout) \
 ({ int err=regmap_read(map,offset,&cur);err?err:((condition)?0:-110); })
''' + fn + r'''
int main(void) {
 struct regmap m={.value=0x510302ff};
 struct apple_pmgr_ps ps={.genpd={.name="retained"},.regmap=&m,.offset=0x180,.preserve_active_control=true};
 u32 original=m.value;
 assert(!apple_pmgr_ps_set(&ps.genpd,15,true));assert(m.writes==0&&m.value==original);
 ps.preserve_active_control=false;m=(struct regmap){.value=original};
 assert(!apple_pmgr_ps_set(&ps.genpd,15,true));assert(m.writes==2);
 ps.preserve_active_control=true;m=(struct regmap){.value=0};
 assert(!apple_pmgr_ps_set(&ps.genpd,15,true));assert(m.writes==2);
 assert(FIELD_GET(APPLE_PMGR_PS_TARGET,m.written[0])==15);
 m=(struct regmap){.value=original};
 assert(!apple_pmgr_ps_set(&ps.genpd,0,false));assert(m.writes==1);
 assert(FIELD_GET(APPLE_PMGR_PS_TARGET,m.written[0])==0);
 for(unsigned i=0;i<4;i++) {
  const u32 invalid[]={APPLE_PMGR_BUSY,APPLE_PMGR_RESET,APPLE_PMGR_PS_RESET,APPLE_PMGR_DEV_DISABLE};
  m=(struct regmap){.value=original|invalid[i]};
  assert(!apple_pmgr_ps_set(&ps.genpd,15,true));assert(m.writes==2);
  assert(!(m.written[0]&(APPLE_PMGR_BUSY|APPLE_PMGR_FLAGS|APPLE_PMGR_AUTO_ENABLE)));
 }
 m=(struct regmap){.value=original,.fail_read=-5};
 assert(apple_pmgr_ps_set(&ps.genpd,15,true)==-5);assert(m.writes==0);
 puts("PMGR retained-active and real-transition contracts pass");
}
'''
with tempfile.TemporaryDirectory(prefix='m5-pmgr-host-') as tmp:
 t=Path(tmp);(t/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(t/'test.c'),'-o',str(t/'test')],check=True)
 subprocess.run([str(t/'test')],check=True)
