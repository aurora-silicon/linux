#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check QSPIMC opcode admission and bounded NVRAM write sequences."""
from pathlib import Path
import argparse
import contextlib
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef uint64_t resource_size_t;
struct device_node { const char *compatible; };
static const char *machine;
static bool of_device_is_compatible(struct device_node *node,const char *compat){return !strcmp(node->compatible,compat);}
static bool of_machine_is_compatible(const char *compat){return !strcmp(machine,compat);}
#define EOPNOTSUPP 95
#define GFP_KERNEL 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define SPI_MEM_DATA_IN 1
#define SPI_MEM_DATA_OUT 2
#define SPI_MEM_NO_DATA 0
#define APPLE_QSPI_MAX_DATA 4096
#define SPI_MEM_OP_CMD(o,w) { .opcode=o,.nbytes=1,.buswidth=w }
#define SPI_MEM_OP_ADDR(n,a,w) { .val=a,.nbytes=n,.buswidth=w }
#define SPI_MEM_OP_NO_ADDR {0}
#define SPI_MEM_OP_NO_DUMMY {0}
#define SPI_MEM_OP_DATA_IN(n,b,w) { .nbytes=n,.buf.in=b,.buswidth=w,.dir=SPI_MEM_DATA_IN }
#define SPI_MEM_OP_NO_DATA {0}
#define SPI_MEM_OP(c,a,d,v) { .cmd=c,.addr=a,.dummy=d,.data=v }
static void discard_log(const char *format,...){(void)format;}
#define dev_err(device,...) discard_log(__VA_ARGS__)
#define dev_warn_ratelimited(device,...) discard_log(__VA_ARGS__)
#define mutex_lock(p) assert(!(*(p))++)
#define mutex_unlock(p) assert((*(p))-- == 1)
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a,b) ((long)((a)-(b))>=0)
#define usleep_range(a,b) ((void)(a),(void)(b),jiffies+=100)
static unsigned long jiffies;
static bool runtime_ready, write_fault_latched, read_only=true;
static int apple_qspi_policy_lock;
struct kernel_param { void *arg; };
static int kstrtobool(const char*s,bool*v){if(!strcmp(s,"Y"))*v=true;else if(!strcmp(s,"N"))*v=false;else return -EINVAL;return 0;}
static int param_get_bool(char*b,const struct kernel_param*p){b[0]=*(bool*)p->arg?'Y':'N';return 1;}
struct apple_qspi {
 bool nvram_policy, nvram_write_candidate, write_fault, nvram_window;
 u64 window_start, window_end;
 u8 jedec[3]; u8 jedec_alt[3]; u8 bp_ok;
 int irq;
};
struct spi_controller;
struct device { struct device *parent; struct spi_controller *controller; };
struct spi_controller { struct apple_qspi *q; bool auto_runtime_pm; struct device dev; };
struct spi_device { struct spi_controller *controller; unsigned int cs; int dev; };
struct spi_mem { struct spi_device *spi; };
struct spi_mem_op {
 struct { unsigned int opcode,nbytes,buswidth,dtr; } cmd;
 struct { unsigned int nbytes,buswidth,dtr; u64 val; } addr;
 struct { unsigned int nbytes,buswidth,dtr; } dummy;
 struct { unsigned int nbytes,buswidth,dtr,ecc,swap16,dir; union {const void*out;void*in;}buf; } data;
 unsigned int max_freq;
};
struct apple_qspi_op_shape {u8 width,shared_count,multi_count,turnaround;};
static struct apple_qspi *spi_controller_get_devdata(struct spi_controller*c){return c->q;}
static unsigned int spi_get_chipselect(struct spi_device*s,int i){return s->cs;}
static bool spi_get_csgpiod(struct spi_device*s,int i){return false;}
static bool spi_mem_default_supports_op(struct spi_mem*m,const struct spi_mem_op*o){return true;}
static bool deny_alloc;
static void *kmalloc(size_t n,int flags){return deny_alloc?NULL:malloc(n);}
#define kfree free
static u8 flash[0x800000], sr[3], jedec[3]={0xef,0x65,0x17};
static unsigned int wire[4096],wire_count,fail_opcode;
static bool corrupt_readback,stuck_busy,programmed;
static unsigned int pm_refs,busy_calls,busy_wire;
static void pm_runtime_mark_last_busy(struct device*d){
 assert(d&&pm_refs);busy_calls++;busy_wire=wire_count;
}
static int apple_qspi_transfer_op(struct spi_mem*m,const struct spi_mem_op*o){
 if(m->spi->controller->auto_runtime_pm)assert(pm_refs);
 assert(wire_count<ARRAY_SIZE(wire));wire[wire_count++]=o->cmd.opcode;
 if(o->cmd.opcode==fail_opcode)return -EIO;
 switch(o->cmd.opcode){
 case 0x9f:memcpy(o->data.buf.in,jedec,o->data.nbytes);break;
 case 0x05:*(u8*)o->data.buf.in=sr[0]|(stuck_busy&&programmed?1:0);break;
 case 0x35:assert(jedec[0]!=0xc2);*(u8*)o->data.buf.in=sr[1];break;
 case 0x15:*(u8*)o->data.buf.in=sr[2];break;
 case 0x03:
  assert(o->addr.val+o->data.nbytes<=sizeof(flash));
  memcpy(o->data.buf.in,flash+o->addr.val,o->data.nbytes);
  if(corrupt_readback&&programmed)((u8*)o->data.buf.in)[0]^=1;
  break;
 case 0x06:assert(!(sr[0]&3));sr[0]|=2;break;
 case 0x04:sr[0]&=~2;break;
 case 0x02:
  assert(sr[0]&2);assert(o->addr.val>=0x700000&&o->addr.val+o->data.nbytes<=0x800000);
  for(unsigned int i=0;i<o->data.nbytes;i++)flash[o->addr.val+i]&=((const u8*)o->data.buf.out)[i];
  sr[0]&=~2;programmed=true;break;
 case 0x20:assert(sr[0]&2);assert(o->addr.val>=0x700000&&o->addr.val+4096<=0x800000);
  memset(flash+o->addr.val,0xff,4096);sr[0]&=~2;programmed=true;break;
 default:assert(!"forbidden opcode reached the wire");
 }return 0;
}
'''

CONTROLS = r'''
static struct apple_qspi q;
static struct spi_controller ctl={&q};
static struct device parent;
static struct spi_device spi={&ctl};
static struct spi_mem mem={&spi};
static struct kernel_param parameter={&read_only};
static void reset(void){
 memset(flash,0xff,sizeof(flash));memset(sr,0,sizeof(sr));q=(struct apple_qspi){.nvram_policy=true,.nvram_write_candidate=true,.window_start=0x700000,.window_end=0x800000,.jedec={0xef,0x65,0x17}};
 read_only=true;runtime_ready=false;write_fault_latched=false;wire_count=0;fail_opcode=0;spi.cs=0;
 corrupt_readback=false;stuck_busy=false;programmed=false;deny_alloc=false;jiffies=0;
 jedec[0]=0xef;jedec[1]=0x65;jedec[2]=0x17;
 ctl.auto_runtime_pm=false;ctl.dev.parent=&parent;
 pm_refs=busy_calls=busy_wire=0;
}
static struct spi_mem_op program(u64 addr,unsigned int n,const u8*b){
 struct spi_mem_op o=SPI_MEM_OP(SPI_MEM_OP_CMD(0x02,1),SPI_MEM_OP_ADDR(3,addr,1),SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 o.data.nbytes=n;o.data.dir=SPI_MEM_DATA_OUT;o.data.buswidth=1;o.data.buf.out=b;return o;
}
static void arm(void){runtime_ready=true;assert(!apple_qspi_set_read_only("N",&parameter));}
static bool sent(unsigned int o){for(unsigned int i=0;i<wire_count;i++)if(wire[i]==o)return true;return false;}
static void denied(struct spi_mem_op o){wire_count=0;assert(apple_qspi_exec_op(&mem,&o)<0);assert(!wire_count);}
int main(void){
 const char *boards[]={"apple,j613","apple,j713","apple,j700","apple,j616s","apple,j714"};
 const char *controllers[]={"apple,t8122-qspi","apple,t8132-qspi","apple,t8140-qspi","apple,t6041-qspi"};
 const u64 bases[]={0x2a1118000ULL,0x3ad118000ULL,0x385110000ULL,0x429118000ULL,0xad118000ULL,0x3ad118004ULL,0x429118800ULL};
 for(unsigned int b=0;b<ARRAY_SIZE(boards);b++){
  machine=boards[b];
  for(unsigned int c=0;c<ARRAY_SIZE(controllers);c++){
   struct device_node node={controllers[c]};
   for(unsigned int a=0;a<ARRAY_SIZE(bases);a++)
    assert(!!apple_qspi_nvram_write_board(&node,bases[a])==(b<4&&b==c&&c==a));
  }
 }
 const struct apple_qspi_board *qb;
 struct device_node t6041={"apple,t6041-qspi"},t8140={"apple,t8140-qspi"};
 machine="apple,j616s";qb=apple_qspi_nvram_write_board(&t6041,0x429118000ULL);
 assert(qb&&qb->nvram_start==0x700000&&qb->nvram_size==0x100000&&qb->jedec[0]==0xc2&&qb->jedec[1]==0x2b&&qb->jedec[2]==0x27&&!qb->bp_ok);
 assert(qb->jedec_alt[0]==0xef&&qb->jedec_alt[1]==0x65&&qb->jedec_alt[2]==0x17);
 machine="apple,j700";qb=apple_qspi_nvram_write_board(&t8140,0x385110000ULL);
 assert(qb&&qb->nvram_start==0x24000&&qb->nvram_size==0x1d0000&&qb->jedec[2]==0x15&&qb->bp_ok==0x1c);
 puts("PASS J613/J713/J700/J616s board, controller and full-width aperture qualification (140 combinations)");
 u8 data[256];memset(data,0x55,sizeof(data));reset();
 struct spi_mem_op wren=SPI_MEM_OP(SPI_MEM_OP_CMD(0x06,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 /* T8140 (J700) shares the verified policy at its DT nvram window; its data
  * reads pass without the opt-in. This synthetic window shares the flash
  * model's range; KUnit covers the J700 partition's exact bounds. */
 q.nvram_window=true;q.bp_ok=0x1c;
 u8 rdbuf[4];struct spi_mem_op t8140_read=SPI_MEM_OP(SPI_MEM_OP_CMD(0x03,1),SPI_MEM_OP_ADDR(3,0x10,1),SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_DATA_IN(4,rdbuf,1));
 assert(!apple_qspi_mem_exec_op(&mem,&t8140_read) && wire_count==1);
 wire_count=0;assert(apple_qspi_mem_exec_op(&mem,&wren)==-EACCES && !wire_count);
 struct spi_mem_op window_program=program(0x700000,256,data);
 assert(apple_qspi_mem_exec_op(&mem,&window_program)==-EACCES && !wire_count);
 arm();sr[0]=4;
 assert(!apple_qspi_mem_exec_op(&mem,&wren) && !wire_count);
 assert(!apple_qspi_mem_exec_op(&mem,&window_program) && sent(0x02) && sr[0]==4);
 assert(!memcmp(flash+0x700000,data,256));
 const u64 outside_window[]={0,0x6fffff,0x800000,0x100700000ULL};
 for(unsigned int i=0;i<ARRAY_SIZE(outside_window);i++)denied(program(outside_window[i],1,data));
 struct spi_mem_op status_write=wren;status_write.cmd.opcode=0x01;denied(status_write);
 q.window_start=q.window_end=0;denied(program(0x700000,1,data));
 q.window_start=0x700000;q.window_end=0x800000;q.bp_ok=0;wire_count=0;
 window_program=program(0x700100,1,data);
 assert(apple_qspi_exec_op(&mem,&window_program)==-EACCES&&!sent(0x06));
 puts("PASS T8140 read pass-through, opt-in, verified programs, tolerated BP bits, rejected control writes and empty partition");
 reset();
 assert(apple_qspi_mem_exec_op(&mem,&wren)==-EACCES && !wire_count);
 arm();
 assert(!apple_qspi_mem_exec_op(&mem,&wren) && !wire_count && !sr[0]);
 reset();
 assert(apple_qspi_set_read_only("N",&parameter)==-EPERM&&read_only);
 struct spi_mem_op o=program(0x700000,256,data);denied(o);
 u8 id[3];struct spi_mem_op rd=SPI_MEM_OP(SPI_MEM_OP_CMD(0x9f,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_DATA_IN(3,id,1));
 assert(!apple_qspi_exec_op(&mem,&rd)&&!memcmp(id,jedec,3));
 arm();wire_count=0;assert(!apple_qspi_exec_op(&mem,&o));
 assert(wire_count==14&&wire[0]==0x9f&&wire[4]==0x03&&wire[5]==0x06&&wire[7]==0x02&&wire[9]==0x04&&wire[13]==0x03);
 assert(!memcmp(flash+0x700000,data,256)&&!sr[0]);
 wire_count=0;assert(!apple_qspi_exec_op(&mem,&o)&&!sent(0x06)&&!sent(0x02));
 memset(data,0xff,sizeof(data));wire_count=0;assert(apple_qspi_exec_op(&mem,&o)==-EINVAL&&!sent(0x06));
 reset();arm();memset(data,0x55,sizeof(data));
 const u64 bad[]={0,0x6fffff,0x800000,0x100700000ULL,UINT64_MAX};
 for(unsigned int i=0;i<ARRAY_SIZE(bad);i++)denied(program(bad[i],1,data));
 denied(program(0x7fffff,2,data));denied(program(0x7000ff,2,data));denied(program(0x700000,257,data));denied(program(0x700000,0,data));
 o=program(0x700000,1,data);o.addr.nbytes=4;denied(o);o=program(0x700000,1,data);o.data.buswidth=4;denied(o);
 o=program(0x700000,1,data);o.dummy.nbytes=1;o.dummy.buswidth=1;denied(o);o=program(0x700000,1,data);spi.cs=1;denied(o);spi.cs=0;
 q.nvram_write_candidate=false;denied(o);q.nvram_write_candidate=true;
 for(unsigned int c=0;c<256;c++){
  if(c==2||c==4||c==6||c==0x20)continue;
  struct spi_mem_op cmd=SPI_MEM_OP(SPI_MEM_OP_CMD(c,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);denied(cmd);
 }
 for(unsigned int c=2;c<=0x20;c+=0x1e){struct spi_mem_op cmd=SPI_MEM_OP(SPI_MEM_OP_CMD(c,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);denied(cmd);}
 for(unsigned int c=4;c<=6;c+=2){struct spi_mem_op cmd=SPI_MEM_OP(SPI_MEM_OP_CMD(c,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);wire_count=0;assert(!apple_qspi_exec_op(&mem,&cmd)&&!wire_count);}
 for(unsigned int i=0;i<3;i++){
  reset();arm();sr[i]=i==0?4:i==1?0x40:4;wire_count=0;o=program(0x700000,1,data);
  assert(apple_qspi_exec_op(&mem,&o)==-EACCES&&!sent(0x06));
 }
 reset();arm();jedec[2]^=1;o=program(0x700000,1,data);assert(apple_qspi_exec_op(&mem,&o)==-ENODEV&&!sent(0x06));
 reset();arm();deny_alloc=true;assert(apple_qspi_exec_op(&mem,&o)==-ENOMEM&&!wire_count);
 reset();arm();memset(flash+0x7ff000,0,4096);
 struct spi_mem_op erase=SPI_MEM_OP(SPI_MEM_OP_CMD(0x20,1),SPI_MEM_OP_ADDR(3,0x7ff000,1),SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 assert(!apple_qspi_exec_op(&mem,&erase));for(unsigned int i=0;i<4096;i++)assert(flash[0x7ff000+i]==0xff);
 erase.addr.val=0x7ff001;denied(erase);erase.addr.val=0x800000;denied(erase);erase.addr.val=0x6ff000;denied(erase);
 for(unsigned int fault=0;fault<4;fault++){
  reset();arm();o=program(0x700000,1,data);
  if(!fault)fail_opcode=0x06;else if(fault==1)fail_opcode=0x02;else if(fault==2)corrupt_readback=true;else stuck_busy=true;
  assert(apple_qspi_exec_op(&mem,&o)<0&&read_only&&q.write_fault&&write_fault_latched);
  assert(sent(0x04)&&apple_qspi_set_read_only("N",&parameter)==-EPERM);denied(o);
 }
 puts("PASS read-only startup, exact opcode/range/page/erase admission, WREN ordering, readback and fault latch");
 /* J616s Macronix part: 35h (EQIO there) never reaches the wire. */
 reset();jedec[0]=0xc2;jedec[1]=0x2b;jedec[2]=0x27;memcpy(q.jedec,jedec,3);arm();memset(data,0x55,sizeof(data));
 o=program(0x700000,256,data);wire_count=0;assert(!apple_qspi_exec_op(&mem,&o)&&sent(0x02)&&!sent(0x35)&&sent(0x15));
 assert(!memcmp(flash+0x700000,data,256)&&!sr[0]);
 erase.addr.val=0x701000;
 wire_count=0;assert(!apple_qspi_exec_op(&mem,&erase)&&sent(0x20)&&!sent(0x35));
 struct spi_mem_op rdcr2=SPI_MEM_OP(SPI_MEM_OP_CMD(0x35,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_DATA_IN(1,id,1));
 wire_count=0;assert(apple_qspi_exec_op(&mem,&rdcr2)<0&&!sent(0x35));
 for(unsigned int bp=0x04;bp<=0x20;bp<<=1){reset();jedec[0]=0xc2;jedec[1]=0x2b;jedec[2]=0x27;memcpy(q.jedec,jedec,3);arm();
  sr[0]=bp;wire_count=0;o=program(0x700000,1,data);assert(apple_qspi_exec_op(&mem,&o)==-EACCES&&!sent(0x06)&&!sent(0x35));}
 reset();memcpy(q.jedec,(u8[]){0xc2,0x2b,0x27},3);arm();wire_count=0;o=program(0x700000,1,data);
 assert(apple_qspi_exec_op(&mem,&o)==-ENODEV&&!sent(0x06));
 puts("PASS Macronix J616s part: no 35h on the wire, SR/CR checks, BP0-BP3 refusal and JEDEC mismatch");
 /* J616s W25Q64 part: the same board entry, and 35h is its SR2 read. */
 reset();memcpy(q.jedec,(u8[]){0xc2,0x2b,0x27},3);memcpy(q.jedec_alt,jedec,3);arm();memset(data,0x55,sizeof(data));
 o=program(0x700000,256,data);wire_count=0;assert(!apple_qspi_exec_op(&mem,&o)&&sent(0x02)&&sent(0x35));
 assert(!memcmp(flash+0x700000,data,256)&&!sr[0]);
 wire_count=0;assert(!apple_qspi_exec_op(&mem,&rdcr2)&&sent(0x35));
 /* A second-slot match keeps the Winbond protection checks. */
 sr[1]=0x40;wire_count=0;o=program(0x700100,1,data);assert(apple_qspi_exec_op(&mem,&o)==-EACCES&&!sent(0x06));
 sr[1]=0;jedec[2]^=1;wire_count=0;assert(apple_qspi_exec_op(&mem,&o)==-ENODEV&&!sent(0x06));
 /* An empty second slot admits nothing, even on a bus that reads zeroes. */
 reset();memset(jedec,0,3);arm();wire_count=0;o=program(0x700000,1,data);
 assert(apple_qspi_exec_op(&mem,&o)==-ENODEV&&!sent(0x06));
 puts("PASS W25Q64 J616s part: second ID admitted with SR2 read and checks, third ID and empty slot refused");
 /* Model spi-mem's outer reference. Every private JEDEC/status/WREN,
  * completion poll, WRDI and readback must finish before the idle stamp.
  * Hardware tests must still prove genpd and the actual suspend timer. */
 reset();q.nvram_window=true;ctl.auto_runtime_pm=true;pm_refs=1;
 assert(!apple_qspi_mem_exec_op(&mem,&t8140_read));
 assert(busy_calls==1&&busy_wire==wire_count&&wire_count==1);
 wire_count=0;assert(apple_qspi_mem_exec_op(&mem,&wren)==-EACCES);
 assert(busy_calls==2&&!wire_count);
 arm();o=program(0x700000,256,data);stuck_busy=false;
 assert(!apple_qspi_mem_exec_op(&mem,&o));
 assert(busy_calls==3&&busy_wire==wire_count&&wire[wire_count-1]==0x03);
 wire_count=0;fail_opcode=0x03;
 assert(apple_qspi_mem_exec_op(&mem,&t8140_read)==-EIO);
 assert(busy_calls==4&&busy_wire==wire_count);
 fail_opcode=0x02;o.addr.val=0x700100;
 assert(apple_qspi_mem_exec_op(&mem,&o)==-EIO&&q.write_fault&&read_only);
 assert(busy_calls==5&&busy_wire==wire_count&&wire[wire_count-1]==0x04);
 pm_refs=0;
 reset();assert(!apple_qspi_mem_exec_op(&mem,&t8140_read));assert(!busy_calls);
 puts("PASS runtime-PM idle stamp after read, denied write, full verified program and transfer error; other SoCs unchanged");
 return 0;
}
'''


def run(code, path):
    path.write_text(code)
    command = ['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                    '-Wno-missing-field-initializers', '-Wno-sign-compare',
                    '-fsanitize=address,undefined',
                    str(path), '-o', str(path.with_suffix(''))]
    path.with_suffix('.command.json').write_text(json.dumps(command) + '\n')
    subprocess.run(command, check=True)
    result = subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)
    path.with_suffix('.stdout').write_text(result.stdout)
    path.with_suffix('.stderr').write_text(result.stderr)
    path.with_suffix('.exit').write_text(str(result.returncode) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='retain generated C and test binaries')
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'drivers/spi/spi-apple-qspi.c').read_text()
    policy = (ROOT / 'drivers/spi/spi-apple-qspi-policy.h').read_text()
    boards = (''.join(m.group(0) + '\n' for m in re.finditer(r'#define APPLE_QSPI_JEDEC_\w+.*', source)) +
              function(source, 'struct apple_qspi_board {').rstrip() + ';\n' +
              function(source, 'static const struct apple_qspi_board apple_qspi_boards[]').rstrip() + ';\n')
    signatures = ['static int apple_qspi_set_read_only(', 'static int apple_qspi_get_read_only(',
                  'static const struct apple_qspi_board *\napple_qspi_nvram_write_board(',
                  'static bool apple_qspi_opcode_is_read(', 'static int apple_qspi_width_mode(',
                  'static int apple_qspi_shape_op(', 'static bool apple_qspi_supports_op(',
                  'static int apple_qspi_read_register(', 'static int apple_qspi_command(',
                  'static int apple_qspi_read_array(', 'static int apple_qspi_status(',
                  'static int apple_qspi_nvram_modify(', 'static int apple_qspi_exec_op(',
                  'static int apple_qspi_mem_exec_op(']
    code = PREFIX + policy + boards + '\n'.join(function(source, s) for s in signatures) + CONTROLS
    output = contextlib.nullcontext(str(args.output)) if args.output else tempfile.TemporaryDirectory(prefix='qspi-nvram-')
    with output as directory:
        current = run(code, Path(directory) / 'current.c')
        print(current.stdout, end='')
        if current.returncode:
            raise SystemExit(current.stderr)
        for name, old, new in [
            ('controller', 'of_device_is_compatible(node, board->controller) &&', ''),
            ('board', 'of_machine_is_compatible(board->machine) &&', ''),
            ('aperture', 'base == board->base', 'base != 0'),
            ('j616s-aperture', '"apple,j616s", 0x429118000ULL', '"apple,j616s", 0x429118800ULL'),
            ('j616s-partition', '"apple,j616s", 0x429118000ULL,\n\t  0x700000', '"apple,j616s", 0x429118000ULL,\n\t  0x680000'),
            ('range', 'addr >= start &&', 'addr >= 1 &&'),
            ('bp-ok', '!(sr1 & 0x1c & ~bp_ok)', '1'),
            ('mx-bp3', '!(sr & 0x3c & ~bp_ok)', '!(sr & 0x1c & ~bp_ok)'),
            ('mx-eqio', 'opcode = macronix;', 'opcode = winbond;'),
            ('mx-eqio-read', '!ret && id[0] == APPLE_QSPI_MFR_MACRONIX', '0'),
            ('jedec-alt', 'memcmp(id, qspi->jedec_alt, sizeof(id))', '1'),
            ('jedec-alt-empty', '!qspi->jedec_alt[0] || ', ''),
            ('read-only', 'read_only || !qspi->nvram_write_candidate || qspi->write_fault',
             '!qspi->nvram_write_candidate || qspi->write_fault'),
            ('fault-latch', 'write_fault_latched = true;', 'write_fault_latched = false;'),
            ('pm-last-busy', 'pm_runtime_mark_last_busy(ctlr->dev.parent);', '(void)ctlr;')]:
            assert old in code
            mutant = run(code.replace(old, new), Path(directory) / (name + '.c'))
            if not mutant.returncode:
                raise SystemExit(name + ' changed-guard control did not fail')
            print('PASS ' + name + ' changed-guard control rejects the write sequence')


if __name__ == '__main__':
    main()
