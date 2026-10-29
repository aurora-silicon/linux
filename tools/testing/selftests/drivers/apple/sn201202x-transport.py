#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production SN201202x selection/reads with a bounded SPMI model."""
from pathlib import Path
import re
import resource
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^static int " + name + r"\(", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def main():
    root = Path(__file__).resolve().parents[5]
    path = root / "drivers/usb/typec/tipd/spmi.c"
    if not path.exists():
        print("# SKIP: requires the matching kernel source tree")
        return 4
    source = path.read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
#define TPS_MAX_LEN 64
#define WARN_ON(x) ((void)(x))
#define dev_warn(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
#define min_t(type,a,b) ((type)(a)<(type)(b) ? (type)(a) : (type)(b))
#define msecs_to_jiffies(x) (x)
struct completion { int unused; };
struct device { void *of_node; };
struct tps6598x { struct device *dev; };
struct sn201202x { bool polled, select_polled, atomic; struct completion select_completion; };
struct spmi_device { struct device dev; struct tps6598x tps; };
static struct sn201202x sn;
static u8 selected, advertised;
static unsigned polls, busy_cycles, writes, length_reads, payload_reads, bytes;
static int select_error, status_error, length_error, payload_error;
static bool wrong, forever, completed, length_checked;
static struct tps6598x *spmi_device_get_drvdata(struct spmi_device *s) { return &s->tps; }
static struct sn201202x *tps_to_sn(struct tps6598x *t) { (void)t; return &sn; }
static void reinit_completion(struct completion *c) { (void)c; }
static bool wait_for_completion_timeout(struct completion *c,unsigned n)
{ (void)c; (void)n; return false; }
static void msleep(unsigned n) { (void)n; }
static bool of_device_is_compatible(void *node,const char *name)
{ assert(!strcmp(name,"apple,t8132-sn201202x")); return node != NULL; }
static int spmi_register_zero_write(struct spmi_device *s,u8 reg)
{ (void)s; selected=reg; writes++; return select_error; }
static int spmi_register_read(struct spmi_device *s,u8 reg,u8 *value)
{
    (void)s;
    if(reg==0) {
        if(status_error) return status_error;
        if(wrong) { *value=(selected+1)&0x7f; return 0; }
        if(forever || polls++<busy_cycles) { *value=selected|0x80; return 0; }
        completed=true; *value=selected; return 0;
    }
    assert(reg==0x1f && completed);
    length_reads++;
    if(length_error) return length_error;
    length_checked=true; *value=advertised; return 0;
}
static int spmi_ext_register_read(struct spmi_device *s,unsigned off,void *val,size_t len)
{
    assert(completed);
    if(s->dev.of_node) assert(length_checked);
    assert(len>0 && len<=16 && off==0x20+bytes && off+len<=0x60);
    payload_reads++;
    if(payload_error) return payload_error;
    for(size_t i=0;i<len;i++) ((u8 *)val)[i]=(u8)(off-0x20+i);
    bytes+=len;
    return 0;
}
#define read_poll_timeout(op,ret,condition,delay,bound,sleep_before,...) ({ \
    int poll_ret=-ETIMEDOUT; \
    for(unsigned attempt=0;attempt<8;attempt++) { \
        (ret)=op(__VA_ARGS__); if(condition) { poll_ret=0; break; } \
    } poll_ret; })
#define SN201202X_ATOMIC_SELECT_MS 10
#define read_poll_timeout_atomic read_poll_timeout
''' + function(source, "regmap_sn201202x_select_reg") + function(source, "regmap_sn201202x_read") + r'''
static void clean(struct spmi_device *s)
{
    memset(s,0,sizeof(*s)); s->dev.of_node=s; s->tps.dev=&s->dev;
    sn=(struct sn201202x){ .select_polled=true };
    selected=0; advertised=4; busy_cycles=2;
    polls=writes=length_reads=payload_reads=bytes=0;
    select_error=status_error=length_error=payload_error=0;
    wrong=forever=completed=length_checked=false;
}
int main(void)
{
    struct spmi_device s;
    u8 reg=0x5f, out[64];
    clean(&s); memset(out,0xa5,sizeof(out));
    assert(!regmap_sn201202x_read(&s,&reg,1,out,4));
    assert(writes==1 && polls==3 && completed && length_reads==1 && payload_reads==1);
    assert(bytes==4 && out[0]==0 && out[3]==3 && out[4]==0xa5);
    clean(&s); sn.atomic=true; memset(out,0xa5,sizeof(out));
    assert(!regmap_sn201202x_read(&s,&reg,1,out,4));
    assert(writes==1 && polls==3 && completed && length_reads==1 && payload_reads==1);
    assert(bytes==4 && out[0]==0 && out[3]==3 && out[4]==0xa5);
    puts("ok 1 - exact selector completion precedes length and payload");
    for(unsigned length=0;length<=255;length++) for(unsigned request=0;request<=64;request++) {
        clean(&s); advertised=length; memset(out,0xa5,sizeof(out));
        int ret=regmap_sn201202x_read(&s,&reg,1,out,request);
        bool valid=length>=request && length<=64;
        if(valid) {
            assert(!ret && bytes==request && payload_reads==(request+15)/16);
            for(unsigned i=0;i<request;i++) assert(out[i]==i);
        } else {
            assert(ret==-EIO && !payload_reads && !bytes);
            for(unsigned i=0;i<64;i++) assert(out[i]==0xa5);
        }
    }
    puts("ok 2 - all bounded lengths, short responses and oversized payloads");
    clean(&s); select_error=-EREMOTEIO;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-EREMOTEIO && !length_reads && !payload_reads);
    clean(&s); status_error=-EIO;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-EIO && !length_reads && !payload_reads);
    clean(&s); wrong=true;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-ETIMEDOUT && !length_reads && !payload_reads);
    clean(&s); forever=true;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-ETIMEDOUT && !length_reads && !payload_reads);
    puts("ok 3 - failed, wrong or busy selection never reaches payload");
    clean(&s); length_error=-EREMOTEIO;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-EREMOTEIO && !payload_reads);
    clean(&s); payload_error=-EREMOTEIO;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-EREMOTEIO && payload_reads==1 && !bytes);
    clean(&s);
    assert(regmap_sn201202x_read(&s,&reg,2,out,4)==-EINVAL && !writes);
    assert(regmap_sn201202x_read(&s,&reg,1,out,65)==-EINVAL && !writes);
    reg=0x80;
    assert(regmap_sn201202x_read(&s,&reg,1,out,4)==-EINVAL && !writes);
    puts("ok 4 - transport errors and invalid transfer bounds propagate");
    clean(&s); s.dev.of_node=NULL; reg=0x5f; advertised=0;
    assert(!regmap_sn201202x_read(&s,&reg,1,out,4) && !length_reads && bytes==4);
    puts("ok 5 - older SN201202x transport behavior preserved");
    return 0;
}
'''
    busy = program.replace("err || val == reg", "err || (val & 0x7f) == reg")
    unchecked = program.replace(
        'if (of_device_is_compatible(sdev->dev.of_node, "apple,t8132-sn201202x"))',
        'if (false)', 1)
    oversized = program.replace("size > TPS_MAX_LEN", "false", 1)
    assert all(text != program for text in [busy, unchecked, oversized])
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    print("TAP version 13\n1..6", flush=True)
    with tempfile.TemporaryDirectory(prefix="sn201202x-transport-") as directory:
        tmp = Path(directory)
        for name, text in [("production", program), ("busy", busy),
                           ("unchecked", unchecked), ("oversized", oversized)]:
            c, binary = tmp / (name + ".c"), tmp / name
            c.write_text(text)
            options = [] if name == "production" else ["-Wno-unused-function"]
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror"] + options +
                           [str(c), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=name != "production")
            if name == "production":
                result.check_returncode()
            elif result.returncode == 0:
                raise RuntimeError(f"negative control passed: {name}")
    print("ok 6 - busy, missing-length and oversize negative controls rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
