#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise DMA-heap allocation, unpublished descriptors and copy failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def digest(s):
    return hashlib.sha256(s.encode()).hexdigest()


def function(source, name):
    match = re.search(r'^\s*(?:static\s+)?(?:struct dma_buf\s*\*|long|int|void)\s*' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    brace = source.index('{', match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <linux/types.h>
#include "dma-heap-uapi.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define __user
#define GFP_KERNEL 0
#define PAGE_ALIGN(x) (((x)+PAGE_SIZE-1)&~(uint64_t)(PAGE_SIZE-1))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define max(a,b) ((a)>(b)?(a):(b))
#define max3(a,b,c) max(max(a,b),c)
#define IOC_IN (_IOC_WRITE << _IOC_DIRSHIFT)
#define IOC_OUT (_IOC_READ << _IOC_DIRSHIFT)
#define array_index_nospec(x,n) (x)
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>=(uintptr_t)-4095)
#define PTR_ERR(x) ((intptr_t)(x))
struct file { void *private_data; };
struct dma_buf { struct file *file; };
struct dma_heap;
struct dma_heap_ops { struct dma_buf *(*allocate)(struct dma_heap *,size_t,u32,u64); };
struct dma_heap { const struct dma_heap_ops *ops; };
static struct file buffer_file;
static struct dma_buf buffer={&buffer_file};
/* 0 unused, 1 reserved, 2 installed buffer, 3 another file. */
static int slots[32], refs, allocations, reserves, installs, buffer_puts, unreserves, traces;
static int copy_in_fail, copy_out_fail, allocation_fail, reserve_fail, kmalloc_fail;
static int copy_calls, copy_done, observe_copy, race_after_install, race_after_unreserve, foreign_closes;
static size_t allocated_len;
static struct dma_heap_allocation_data user;
static int lastfd;
static void dma_buf_put(struct dma_buf *buf) { assert(buf==&buffer); assert(refs==1); refs--; buffer_puts++; }
static int get_unused_fd_flags(int flags) {
    (void)flags; reserves++;
    if(reserve_fail) return -EMFILE;
    for(int i=4;i<32;i++) if(!slots[i]) { slots[i]=1;lastfd=i;return i; }
    return -EMFILE;
}
static void put_unused_fd(int fd) {
    assert(slots[fd]==1);slots[fd]=0;unreserves++;
    if(race_after_unreserve) { assert(refs==1);slots[fd]=3; }
}
static void close_fd(int fd) {
    if(slots[fd]==2) dma_buf_put(&buffer);
    else if(slots[fd]==3) foreign_closes++;
    slots[fd]=0;
}
static void fd_install(int fd,struct file *file) {
    assert(file==&buffer_file);assert(slots[fd]==1);slots[fd]=2;installs++;
    if(race_after_install) { close_fd(fd);slots[fd]=3; }
}
#define trace_dma_buf_fd 0
#define DMA_BUF_TRACE(event,buf,fd) do { (void)(event);assert((buf)==&buffer);assert((fd)==lastfd);traces++; } while(0)
static struct dma_buf *allocate(struct dma_heap *heap,size_t len,u32 flags,u64 heap_flags) {
    (void)heap;(void)flags;(void)heap_flags;allocations++;allocated_len=len;
    if(allocation_fail)return ERR_PTR(-ENOMEM);
    assert(!refs);refs=1;return &buffer;
}
static size_t copy_from_user(void *dst,const void *src,size_t len) {
    if(copy_in_fail)return len;
    memcpy(dst,src,len);return 0;
}
static size_t copy_to_user(void *dst,const void *src,size_t len) {
    copy_calls++;
    if(observe_copy) {
        assert(installs==0);assert(slots[lastfd]==1);
        /* An observer cannot acquire or reuse the reserved descriptor. */
        int reserved=lastfd;int another=get_unused_fd_flags(0);assert(another!=reserved);
        put_unused_fd(another);lastfd=((const struct dma_heap_allocation_data *)src)->fd;
    }
    if(copy_out_fail) {
        if(copy_out_fail==2)memcpy(dst,src,len);
        return len ? len : 1;
    }
    memcpy(dst,src,len);copy_done=1;return 0;
}
static void *kmalloc(size_t n,int flags) { (void)flags;return kmalloc_fail?NULL:malloc(n); }
static void kfree(void *p) { free(p); }
'''

TESTS = r'''
static const struct dma_heap_ops ops={allocate};
static struct dma_heap heap={&ops};
static struct file heapfile={&heap};
static int cases;
static void reset(void) {
    for(int i=0;i<32;i++)if(slots[i]==2)close_fd(i);
    assert(!refs);memset(slots,0,sizeof(slots));
    allocations=reserves=installs=buffer_puts=unreserves=traces=0;
    copy_in_fail=copy_out_fail=allocation_fail=reserve_fail=kmalloc_fail=0;
    copy_calls=copy_done=observe_copy=race_after_install=race_after_unreserve=foreign_closes=0;
    allocated_len=0;lastfd=4;
    user=(struct dma_heap_allocation_data){.len=PAGE_SIZE,.fd_flags=O_RDWR};
}
static long run(void) {cases++;return dma_heap_ioctl(&heapfile,DMA_HEAP_IOCTL_ALLOC,(unsigned long)&user);}
static void no_resources(void) {assert(!refs);for(int i=0;i<32;i++)assert(!slots[i]);assert(!installs);}
static void success(void) {assert(run()==0);assert(user.fd==4);assert(refs==1);assert(installs==1);assert(traces==1);assert(copy_done);assert(slots[4]==2);}
int main(int argc,char **argv) {
    reset();
    if(argc>1 && !strcmp(argv[1],"published-reuse")) {
        race_after_install=1;assert(run()==0);assert(slots[4]==3);
        assert(!foreign_closes&&!refs);assert(installs==1&&traces==1);return 0;
    }
    if(argc>1) {copy_out_fail=1;assert(run()==-EFAULT);no_resources();return 0;}
    int flags[]={0,O_RDONLY,O_WRONLY,O_RDWR,O_CLOEXEC,O_CLOEXEC|O_WRONLY,O_CLOEXEC|O_RDWR};
    for(unsigned i=0;i<ARRAY_SIZE(flags);i++){reset();user.fd_flags=flags[i];success();assert(allocated_len==PAGE_SIZE);}
    reset();user.len=1;success();assert(allocated_len==PAGE_SIZE);
    reset();user.len=PAGE_SIZE+1;success();assert(allocated_len==2*PAGE_SIZE);
    reset();user.len=0;assert(run()==-EINVAL);no_resources();assert(!allocations);
    reset();user.len=UINT64_MAX;assert(run()==-EINVAL);no_resources();assert(!allocations);
    reset();user.len=UINT64_MAX-(PAGE_SIZE-2);assert(run()==-EINVAL);no_resources();
    reset();user.fd=1;assert(run()==-EINVAL);no_resources();assert(!allocations);
    reset();user.fd_flags=O_NONBLOCK;assert(run()==-EINVAL);no_resources();assert(!allocations);
    reset();user.heap_flags=1;assert(run()==-EINVAL);no_resources();assert(!allocations);
    reset();allocation_fail=1;assert(run()==-ENOMEM);no_resources();assert(!reserves&&!buffer_puts);
    reset();reserve_fail=1;assert(run()==-EMFILE);no_resources();assert(buffer_puts==1);
    reset();copy_in_fail=1;assert(run()==-EFAULT);no_resources();assert(!allocations);
    reset();copy_out_fail=1;assert(run()==-EFAULT);no_resources();assert(buffer_puts==1&&unreserves==1&&!traces);
    reset();copy_out_fail=2;assert(run()==-EFAULT);no_resources();assert(user.fd==4);assert(buffer_puts==1&&unreserves==1);
    reset();observe_copy=1;success();assert(unreserves==1);
    reset();observe_copy=1;copy_out_fail=1;assert(run()==-EFAULT);no_resources();assert(buffer_puts==1&&unreserves==2);
    reset();race_after_install=1;assert(run()==0);assert(slots[4]==3);assert(!foreign_closes&&!refs);assert(installs==1&&traces==1);
    reset();race_after_unreserve=1;copy_out_fail=1;assert(run()==-EFAULT);assert(slots[4]==3);assert(!foreign_closes&&!refs&&!installs);assert(buffer_puts==1&&unreserves==1);
    reset();assert(dma_heap_ioctl(&heapfile,_IOWR('H',99,struct dma_heap_allocation_data),(unsigned long)&user)==-EINVAL);no_resources();cases++;
    reset();unsigned char large[256]={0};memcpy(large,&user,sizeof(user));
    kmalloc_fail=1;assert(dma_heap_ioctl(&heapfile,_IOC(_IOC_READ|_IOC_WRITE,'H',0,sizeof(large)),(unsigned long)large)==-ENOMEM);no_resources();cases++;
    reset();memset(large,0xa5,sizeof(large));memcpy(large,&user,sizeof(user));assert(dma_heap_ioctl(&heapfile,_IOC(_IOC_READ|_IOC_WRITE,'H',0,sizeof(large)),(unsigned long)large)==0);assert(refs==1&&installs==1);for(unsigned i=sizeof(user);i<sizeof(large);i++)assert(large[i]==0xa5);cases++;
    reset();printf("%d cases PAGE_SIZE=%d PASS\n",cases,PAGE_SIZE);return 0;
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--ref')
    ap.add_argument('--old-ref',required=True)
    ap.add_argument('--cc',default=os.environ.get('CC','clang'))
    args=ap.parse_args()
    root=Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True,exist_ok=True)
    def read(path,ref=None):
        return subprocess.check_output(['git','show',f'{ref}:{path}'],cwd=root,text=True) if ref else (root/path).read_text()
    heap=read('drivers/dma-buf/dma-heap.c',args.ref)
    buf=read('drivers/dma-buf/dma-buf.c',args.ref)
    uapi=read('include/uapi/linux/dma-heap.h',args.ref)
    (args.out/'dma-heap-uapi.h').write_text(uapi)
    def bodies(h,b):
        names=['dma_heap_ioctl_allocate','dma_heap_ioctl']
        if 'static int dma_heap_buffer_alloc(' in h:names.insert(0,'dma_heap_buffer_alloc')
        pieces={n:function(h,n) for n in names}
        pieces['dma_buf_fd']=function(b,'dma_buf_fd')
        if 'void dma_buf_fd_install(' in b:pieces['dma_buf_fd_install']=function(b,'dma_buf_fd_install')
        cmds=re.search(r'static unsigned int dma_heap_ioctl_cmds\[\] = \{.*?\};',h,re.S).group()
        body=pieces['dma_buf_fd']+'\n'+pieces.get('dma_buf_fd_install','')+'\n'+pieces.get('dma_heap_buffer_alloc','')+'\n'+pieces['dma_heap_ioctl_allocate']+'\n'+cmds+'\n'+pieces['dma_heap_ioctl']
        return body,{n:digest(s) for n,s in pieces.items()}
    current,binding=bodies(heap,buf)
    old,old_binding=bodies(read('drivers/dma-buf/dma-heap.c',args.old_ref),read('drivers/dma-buf/dma-buf.c',args.old_ref))
    sources={'positive':current,'original':old}
    changes={
        'early-publication':('fd = ((struct dma_heap_allocation_data *)kdata)->fd;', 'fd = ((struct dma_heap_allocation_data *)kdata)->fd;\n\t\tdma_buf_fd_install(dmabuf, fd);'),
        'leaked-reservation':('put_unused_fd(fd);',';'),
        'leaked-buffer':('''put_unused_fd(fd);
			dma_buf_put(dmabuf);''','put_unused_fd(fd);'),
        'close-reused-fd':('dma_buf_fd_install(dmabuf, fd);','dma_buf_fd_install(dmabuf, fd); close_fd(fd);'),
        'missing-trace':('DMA_BUF_TRACE(trace_dma_buf_fd, dmabuf, fd);',';'),
    }
    for name,(before,after) in changes.items():
        assert before in current,name
        changed=current.replace(before,after)
        if name=='early-publication':
            changed=changed.replace('''} else {
			dma_buf_fd_install(dmabuf, fd);
		}''','}')
        sources[name]=changed
    results={}
    for page in (4096,16384):
        for name,body in sources.items():
            source=args.out/f'{name}-{page}.c';source.write_text(HARNESS+body+TESTS)
            binary=source.with_suffix('')
            command=[args.cc,'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',f'-DPAGE_SIZE={page}',str(source),'-o',str(binary)]
            subprocess.run(command,check=True)
            mode=['old-negative'] if name=='original' else ['published-reuse'] if name=='close-reused-fd' else []
            run=subprocess.run([str(binary)]+mode,capture_output=True,text=True)
            (args.out/f'{name}-{page}.log').write_text(run.stdout+run.stderr)
            assert (run.returncode==0)==(name=='positive'),(name,page,run.returncode)
            results[f'{name}-{page}']={'exit':run.returncode,'compiled_source_sha256':digest(source.read_text()),'stdout':run.stdout.strip()}
    receipt={'results':results,'actual_bodies_sha256':binding,'old_bodies_sha256':old_binding,'uapi_sha256':digest(uapi),'scope':'Actual allocator/ioctl, existing fd exporter and new installer bodies; controlled user-copy, fd table, heap allocation, trace and refcount boundaries. Scheduled observers run before publication, after reservation return and after close/reuse; no kernel syscall, hardware DMA or real concurrent stress.'}
    (args.out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))

if __name__=='__main__':main()
