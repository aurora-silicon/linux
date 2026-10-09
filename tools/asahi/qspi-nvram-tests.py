#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check QSPIMC opcode admission and bounded NVRAM write sequences."""
from pathlib import Path
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
struct apple_qspi { bool nvram_write_candidate, write_fault; };
struct spi_controller { struct apple_qspi *q; };
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
static int apple_qspi_transfer_op(struct spi_mem*m,const struct spi_mem_op*o){
 assert(wire_count<ARRAY_SIZE(wire));wire[wire_count++]=o->cmd.opcode;
 if(o->cmd.opcode==fail_opcode)return -EIO;
 switch(o->cmd.opcode){
 case 0x9f:memcpy(o->data.buf.in,jedec,o->data.nbytes);break;
 case 0x05:*(u8*)o->data.buf.in=sr[0]|(stuck_busy&&programmed?1:0);break;
 case 0x35:*(u8*)o->data.buf.in=sr[1];break;
 case 0x15:*(u8*)o->data.buf.in=sr[2];break;
 case 0x03:
  assert(o->addr.val+o->data.nbytes<=sizeof(flash));
  memcpy(o->data.buf.in,flash+o->addr.val,o->data.nbytes);
  if(corrupt_readback&&programmed)((u8*)o->data.buf.in)[0]^=1;
  break;
 case 0x06:assert(!sr[0]);sr[0]|=2;break;
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
static struct spi_device spi={&ctl};
static struct spi_mem mem={&spi};
static struct kernel_param parameter={&read_only};
static void reset(void){
 memset(flash,0xff,sizeof(flash));memset(sr,0,sizeof(sr));q=(struct apple_qspi){.nvram_write_candidate=true};
 read_only=true;runtime_ready=false;write_fault_latched=false;wire_count=0;fail_opcode=0;spi.cs=0;
 corrupt_readback=false;stuck_busy=false;programmed=false;deny_alloc=false;jiffies=0;
 jedec[0]=0xef;jedec[1]=0x65;jedec[2]=0x17;
}
static struct spi_mem_op program(u64 addr,unsigned int n,const u8*b){
 struct spi_mem_op o=SPI_MEM_OP(SPI_MEM_OP_CMD(0x02,1),SPI_MEM_OP_ADDR(3,addr,1),SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 o.data.nbytes=n;o.data.dir=SPI_MEM_DATA_OUT;o.data.buswidth=1;o.data.buf.out=b;return o;
}
static void arm(void){runtime_ready=true;assert(!apple_qspi_set_read_only("N",&parameter));}
static bool sent(unsigned int o){for(unsigned int i=0;i<wire_count;i++)if(wire[i]==o)return true;return false;}
static void denied(struct spi_mem_op o){wire_count=0;assert(apple_qspi_exec_op(&mem,&o)<0);assert(!wire_count);}
int main(void){
 u8 data[256];memset(data,0x55,sizeof(data));reset();
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
 return 0;
}
'''


def run(code, path):
    path.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                    '-Wno-missing-field-initializers', '-Wno-sign-compare',
                    '-fsanitize=address,undefined',
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    return subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)


def main():
    source = (ROOT / 'drivers/spi/spi-apple-qspi.c').read_text()
    policy = (ROOT / 'drivers/spi/spi-apple-qspi-policy.h').read_text()
    signatures = ['static int apple_qspi_set_read_only(', 'static int apple_qspi_get_read_only(',
                  'static bool apple_qspi_opcode_is_read(', 'static int apple_qspi_width_mode(',
                  'static int apple_qspi_shape_op(', 'static bool apple_qspi_supports_op(',
                  'static int apple_qspi_read_register(', 'static int apple_qspi_command(',
                  'static int apple_qspi_read_array(', 'static int apple_qspi_status(',
                  'static int apple_qspi_nvram_modify(', 'static int apple_qspi_exec_op(']
    code = PREFIX + policy + '\n'.join(function(source, s) for s in signatures) + CONTROLS
    with tempfile.TemporaryDirectory(prefix='qspi-nvram-') as directory:
        current = run(code, Path(directory) / 'current.c')
        print(current.stdout, end='')
        if current.returncode:
            raise SystemExit(current.stderr)
        for name, old, new in [
            ('range', 'addr >= APPLE_QSPI_NVRAM_START', 'addr >= 1'),
            ('read-only', 'read_only || !qspi->nvram_write_candidate || qspi->write_fault',
             '!qspi->nvram_write_candidate || qspi->write_fault'),
            ('fault-latch', 'write_fault_latched = true;', 'write_fault_latched = false;')]:
            assert old in code
            mutant = run(code.replace(old, new), Path(directory) / (name + '.c'))
            if not mutant.returncode:
                raise SystemExit(name + ' changed-guard control did not fail')
            print('PASS ' + name + ' changed-guard control rejects the write sequence')


if __name__ == '__main__':
    main()
