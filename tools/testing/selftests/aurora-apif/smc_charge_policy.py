#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Ensure T6050 battery discovery cannot reset charging policy."""
from pathlib import Path
import subprocess, tempfile
from common import block

ROOT=Path(__file__).resolve().parents[4]
source=(ROOT/'drivers/power/supply/macsmc-power.c').read_text()
guarded=block(source,r'^\t\tif \(!of_device_is_compatible\(dev->parent->of_node, "apple,t6050-smc"\)\)')
probe=block(source,r'^static int macsmc_power_probe\(')
assert probe.count('apple_smc_write_')==guarded.count('apple_smc_write_')==5
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
struct device { struct device *parent; bool *of_node; };
struct macsmc_power { bool has_chte,has_ch0c,has_ch0i; };
static int calls;
static bool of_device_is_compatible(bool *node,const char *name){assert(!strcmp(name,"apple,t6050-smc"));return *node;}
#define SMC_KEY(k) #k
static void apple_smc_write_u32(void *smc,const char *key,uint32_t value){(void)smc;assert(value==0);assert(!strcmp(key,"CHTE"));calls++;}
static void apple_smc_write_u8(void *smc,const char *key,uint8_t value){(void)smc;(void)key;assert(value==0);calls++;}
static void policy(struct device *dev, struct macsmc_power *power){void *smc=0;
''' + guarded + r'''
}
int main(void) {
 bool t6050=true;struct device parent={.of_node=&t6050},dev={.parent=&parent};
 struct macsmc_power p={true,true,true};policy(&dev,&p);assert(calls==0);
 t6050=false;policy(&dev,&p);assert(calls==4);
 calls=0;p.has_chte=false;policy(&dev,&p);assert(calls==4);
 calls=0;p.has_ch0c=false;p.has_ch0i=false;policy(&dev,&p);assert(calls==2);
}
'''
with tempfile.TemporaryDirectory(prefix='smc-charge-') as directory:
 d=Path(directory);(d/'test.c').write_text(program)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('T6050 probe preserves charge policy; older-platform reset behavior unchanged')
