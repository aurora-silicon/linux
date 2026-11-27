#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production brcmfmac PCIe shared RAM, ring and console setup against a fake TCM.

Every TCM access of the fake asserts that it lies inside the mapping, so a firmware-supplied
address the driver does not check makes a control fail. Each --mutation drops one check.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:void|int|bool|s32|u16)\s+' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError('Missing function ' + name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def struct(source, name):
    match = re.search(r'^struct ' + name + r' \{.*?^\};\n', source, re.M | re.S)
    if not match:
        raise ValueError('Missing struct ' + name)
    return match.group(0)


def define(source, name):
    match = re.search(r'^#define\s+' + name + r'\s+(.*)$', source, re.M)
    if not match:
        raise ValueError('Missing define ' + name)
    return '#define %s %s\n' % (name, match.group(1))


PREFIX = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef uint32_t __le32; typedef uint16_t __le16; typedef uint64_t dma_addr_t;
#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define GFP_KERNEL 0
#define brcmf_dbg(...) ((void)0)
#define pr_debug(...) ((void)0)
#define BRCMF_FWCON_ON() 1
#define kzalloc_objs(obj, n) calloc((n), sizeof(obj))
static unsigned errs;
static char last_err[256];
static void brcmf_err(void *bus, const char *fmt, ...)
{ va_list ap; (void)bus; va_start(ap, fmt); vsnprintf(last_err, sizeof(last_err), fmt, ap); va_end(ap); errs++; }
static void __brcmf_err(void *bus, const char *func, const char *fmt, ...) { (void)bus; (void)func; (void)fmt; }
/* DEFINES */
/* STRUCTS */
struct device { int unused; };
struct pci_dev { struct device dev; };
struct brcmf_bus { int unused; };
static struct brcmf_bus bus;
static struct brcmf_bus *dev_get_drvdata(struct device *dev) { (void)dev; return &bus; }
struct brcmf_commonring { u16 r_ptr, w_ptr, depth; };
enum brcmf_pcie_state { BRCMFMAC_PCIE_STATE_DOWN, BRCMFMAC_PCIE_STATE_UP };
struct brcmf_pciedev_info;
struct brcmf_pcie_ringbuf { struct brcmf_commonring commonring; struct brcmf_pciedev_info *devinfo;
 u32 w_idx_addr, r_idx_addr; u16 id; };
struct brcmf_pcie_console { u32 base_addr, buf_addr, bufsize, read_idx, log_idx;
 u8 log_str[256]; u32 scan_idx; u8 scan_len; };
struct brcmf_pcie_shared_info { u32 tcm_base_address, flags; struct brcmf_pcie_ringbuf *commonrings[5];
 struct brcmf_pcie_ringbuf *flowrings; u16 max_rxbufpost, max_flowrings, max_submissionrings,
 max_completionrings; u32 rx_dataoffset, htod_mb_data_addr, dtoh_mb_data_addr, ring_info_addr;
 struct brcmf_pcie_console console; u8 version; bool mb_via_ctl; u32 flags2, flags3; };
struct brcmf_pciedev_info { enum brcmf_pcie_state state; struct pci_dev *pdev; u8 *tcm; size_t tcm_size; struct brcmf_pcie_shared_info shared;
 u8 dma_idx_sz; void *idxbuf; u32 idxbuf_sz; dma_addr_t idxbuf_dmahandle;
 u16 (*read_ptr)(struct brcmf_pciedev_info *devinfo, u32 mem_offset);
 void (*write_ptr)(struct brcmf_pciedev_info *devinfo, u32 mem_offset, u16 value); };

/* Fake TCM: every access must lie inside the mapping. */
#define TCM_SIZE 0x8000
static u8 tcm[TCM_SIZE];
static unsigned long reads;
static void in_tcm(struct brcmf_pciedev_info *d, u32 a, u32 n)
{ assert(d->tcm_size == TCM_SIZE); assert((u64)a + n <= TCM_SIZE); assert(++reads < 1000000); }
static u32 brcmf_pcie_read_tcm32(struct brcmf_pciedev_info *d, u32 a) { u32 v; in_tcm(d, a, 4); memcpy(&v, tcm + a, 4); return v; }
static u16 brcmf_pcie_read_tcm16(struct brcmf_pciedev_info *d, u32 a) { u16 v; in_tcm(d, a, 2); memcpy(&v, tcm + a, 2); return v; }
static u8 brcmf_pcie_read_tcm8(struct brcmf_pciedev_info *d, u32 a) { in_tcm(d, a, 1); return tcm[a]; }
static void brcmf_pcie_write_tcm32(struct brcmf_pciedev_info *d, u32 a, u32 v) { in_tcm(d, a, 4); memcpy(tcm + a, &v, 4); }
static void brcmf_pcie_write_tcm16(struct brcmf_pciedev_info *d, u32 a, u16 v) { in_tcm(d, a, 2); memcpy(tcm + a, &v, 2); }
static struct brcmf_pciedev_info devinfo;
static void memcpy_fromio(void *dst, const u8 *src, size_t n)
{ assert(src >= tcm); in_tcm(&devinfo, (u32)(src - tcm), (u32)n); memcpy(dst, src, n); }
static void memcpy_toio(u8 *dst, const void *src, size_t n)
{ assert(dst >= tcm); in_tcm(&devinfo, (u32)(dst - tcm), (u32)n); memcpy(dst, src, n); }
static void *dma_alloc_coherent(struct device *dev, size_t n, dma_addr_t *h, int gfp)
{ (void)dev; (void)gfp; *h = 0x10000000; return calloc(1, n); }
/* The real one reads the ring's size and writes its host address in its BRCMF_RING_MEM_SZ slot. */
static struct brcmf_pcie_ringbuf rings_pool[5];
static struct brcmf_pcie_ringbuf *brcmf_pcie_alloc_dma_and_ring(struct brcmf_pciedev_info *d, u32 id, u32 mem)
{ brcmf_pcie_read_tcm16(d, mem + BRCMF_RING_MAX_ITEM_OFFSET); brcmf_pcie_write_tcm32(d, mem + BRCMF_RING_MEM_BASE_ADDR_OFFSET + 4, 0);
  return &rings_pool[id]; }
static void brcmf_pcie_release_ringbuffers(struct brcmf_pciedev_info *d) { (void)d; }
static void brcmf_pcie_ring_mb_ring_bell(void) {}
static void brcmf_pcie_ring_mb_write_rptr(void) {}
static void brcmf_pcie_ring_mb_write_wptr(void) {}
static int brcmf_pcie_ring_mb_update_rptr(void *ctx);
static int brcmf_pcie_ring_mb_update_wptr(void *ctx);
static void brcmf_commonring_register_cb(struct brcmf_commonring *c, void (*a)(void), int (*b)(void *),
 int (*e)(void *), void (*f)(void), void (*g)(void), void *r) { (void)c; (void)a; (void)b; (void)e; (void)f; (void)g; (void)r; }
'''

MAIN = r'''
#define SHARED 0x100
#define CONS 0x200
#define BUF 0x400
#define RINGINFO 0x600
#define RINGMEM 0x700
#define IDX 0x800
static struct pci_dev pdev;
static void put32(u32 a, u32 v) { memcpy(tcm + a, &v, 4); }
static void put16(u32 a, u16 v) { memcpy(tcm + a, &v, 2); }
/* A version 6 firmware with TCM ring indices, a console and two flowrings. */
static void reset(void)
{
 free(devinfo.shared.flowrings); free(devinfo.idxbuf);
 memset(tcm, 0, sizeof(tcm)); memset(&devinfo, 0, sizeof(devinfo)); memset(rings_pool, 0, sizeof(rings_pool));
 devinfo.pdev = &pdev; devinfo.tcm = tcm; devinfo.tcm_size = TCM_SIZE; errs = 0; reads = 0;
 put32(SHARED + BRCMF_SHARED_FLAGS_OFFSET, 6);
 put32(SHARED + BRCMF_SHARED_CONSOLE_ADDR_OFFSET, CONS);
 put32(SHARED + BRCMF_SHARED_HTOD_MB_DATA_ADDR_OFFSET, 0xa00);
 put32(SHARED + BRCMF_SHARED_DTOH_MB_DATA_ADDR_OFFSET, 0xa04);
 put32(SHARED + BRCMF_SHARED_RING_INFO_ADDR_OFFSET, RINGINFO);
 put32(CONS + BRCMF_CONSOLE_BUFADDR_OFFSET, BUF); put32(CONS + BRCMF_CONSOLE_BUFSIZE_OFFSET, 64);
 put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, 3); memcpy(tcm + BUF, "ok\n", 3);
 struct brcmf_pcie_dhi_ringinfo *ri = (void *)(tcm + RINGINFO);
 ri->ringmem = RINGMEM; ri->h2d_w_idx_ptr = IDX; ri->h2d_r_idx_ptr = IDX + 0x40;
 ri->d2h_w_idx_ptr = IDX + 0x80; ri->d2h_r_idx_ptr = IDX + 0xc0;
 ri->max_flowrings = 2; ri->max_submissionrings = 4; ri->max_completionrings = 3;
}
static int init(void) { return brcmf_pcie_init_share_ram_info(&devinfo, SHARED); }
static struct brcmf_pcie_dhi_ringinfo *ringinfo(void) { return (void *)(tcm + RINGINFO); }
/* Set up the rings, then touch every ring index as the data path does. */
static int rings(void)
{
 int ret = brcmf_pcie_init_ringbuffers(&devinfo);
 if (ret)
  return ret;
 for (int i = 0; i < 5; i++) {
  devinfo.read_ptr(&devinfo, devinfo.shared.commonrings[i]->w_idx_addr);
  devinfo.read_ptr(&devinfo, devinfo.shared.commonrings[i]->r_idx_addr);
 }
 for (int i = 0; i < devinfo.shared.max_flowrings; i++) {
  devinfo.read_ptr(&devinfo, devinfo.shared.flowrings[i].w_idx_addr);
  devinfo.read_ptr(&devinfo, devinfo.shared.flowrings[i].r_idx_addr);
 }
 return 0;
}
int main(void)
{
 unsigned cases = 0;
 /* A well-formed firmware sets up and its console is read. */
 reset(); assert(!init() && devinfo.shared.console.bufsize == 64 && devinfo.shared.console.read_idx == 3);
 assert(!rings() && !errs); cases++;
 /* Shared RAM whose fields run past the TCM fails setup. */
 reset(); put32(TCM_SIZE - 8 + BRCMF_SHARED_FLAGS_OFFSET, 6);
 assert(brcmf_pcie_init_share_ram_info(&devinfo, TCM_SIZE - 8) == -EINVAL); cases++;
 /* Mailbox and ring info addresses outside the TCM fail setup. */
 reset(); put32(SHARED + BRCMF_SHARED_HTOD_MB_DATA_ADDR_OFFSET, TCM_SIZE - 2); assert(init() == -EINVAL); cases++;
 reset(); put32(SHARED + BRCMF_SHARED_DTOH_MB_DATA_ADDR_OFFSET, 0xfffffffe); assert(init() == -EINVAL); cases++;
 reset(); put32(SHARED + BRCMF_SHARED_RING_INFO_ADDR_OFFSET, TCM_SIZE - 8); assert(init() == -EINVAL); cases++;
 /* So does ring memory or a ring index array outside the TCM. */
 reset(); assert(!init()); ringinfo()->ringmem = TCM_SIZE - 0x20; assert(rings() == -EINVAL); cases++;
 reset(); assert(!init()); ringinfo()->h2d_w_idx_ptr = TCM_SIZE - 8; assert(rings() == -EINVAL); cases++;
 reset(); assert(!init()); ringinfo()->h2d_r_idx_ptr = 0xfffffffc; assert(rings() == -EINVAL); cases++;
 reset(); assert(!init()); ringinfo()->d2h_w_idx_ptr = TCM_SIZE - 4; assert(rings() == -EINVAL); cases++;
 reset(); assert(!init()); ringinfo()->d2h_r_idx_ptr = TCM_SIZE; assert(rings() == -EINVAL); cases++;
 /* The flowring indices count against the TCM too. */
 reset(); assert(!init()); ringinfo()->h2d_w_idx_ptr = TCM_SIZE - 4 * 4; ringinfo()->max_flowrings = 3;
 assert(rings() == -EINVAL); cases++;
 /* Host memory ring indices: one per ring the driver uses, in a buffer sized from the ring counts. */
 reset(); put32(SHARED + BRCMF_SHARED_FLAGS_OFFSET, 6 | BRCMF_PCIE_SHARED_DMA_INDEX | BRCMF_PCIE_SHARED_DMA_2B_IDX);
 assert(!init() && devinfo.dma_idx_sz == 2 && !rings() && !errs); cases++;
 /* Fewer submission or completion rings than the driver uses fail setup. */
 reset(); put32(SHARED + BRCMF_SHARED_FLAGS_OFFSET, 6 | BRCMF_PCIE_SHARED_DMA_INDEX);
 assert(!init()); ringinfo()->max_submissionrings = 2; assert(rings() == -EINVAL); cases++;
 reset(); put32(SHARED + BRCMF_SHARED_FLAGS_OFFSET, 6 | BRCMF_PCIE_SHARED_DMA_INDEX);
 assert(!init()); ringinfo()->max_completionrings = 0; assert(rings() == -EINVAL); cases++;
 /* Ring pointers from the dongle are taken only inside the ring; the last good one stays. */
 reset(); devinfo.state = BRCMFMAC_PCIE_STATE_UP; assert(!init() && !rings());
 struct brcmf_pcie_ringbuf *rb = devinfo.shared.commonrings[2]; rb->devinfo = &devinfo; rb->commonring.depth = 64;
 put16(rb->r_idx_addr, 63); put16(rb->w_idx_addr, 10);
 assert(!brcmf_pcie_ring_mb_update_rptr(rb) && !brcmf_pcie_ring_mb_update_wptr(rb));
 assert(rb->commonring.r_ptr == 63 && rb->commonring.w_ptr == 10);
 put16(rb->r_idx_addr, 64); put16(rb->w_idx_addr, 0xffff);
 assert(brcmf_pcie_ring_mb_update_rptr(rb) == -EIO && brcmf_pcie_ring_mb_update_wptr(rb) == -EIO);
 assert(rb->commonring.r_ptr == 63 && rb->commonring.w_ptr == 10); cases++;
 /* A console descriptor or buffer outside the TCM leaves the console off; setup goes on. */
 reset(); put32(SHARED + BRCMF_SHARED_CONSOLE_ADDR_OFFSET, TCM_SIZE - 4);
 assert(!init() && !devinfo.shared.console.base_addr); cases++;
 reset(); put32(CONS + BRCMF_CONSOLE_BUFADDR_OFFSET, TCM_SIZE - 16);
 assert(!init() && !devinfo.shared.console.base_addr && !devinfo.shared.console.bufsize); cases++;
 reset(); put32(CONS + BRCMF_CONSOLE_BUFSIZE_OFFSET, 0xffffffff);
 assert(!init() && !devinfo.shared.console.base_addr); cases++;
 /* No console reported: nothing is read at address 0, and a partial line is dropped. */
 reset(); assert(!init()); devinfo.shared.console.log_idx = 5; put32(SHARED + BRCMF_SHARED_CONSOLE_ADDR_OFFSET, 0);
 reads = 0; brcmf_pcie_bus_console_init(&devinfo);
 assert(reads == 1 && !devinfo.shared.console.base_addr && !devinfo.shared.console.log_idx); cases++;
 /* A write index at or past the buffer size is not followed. */
 reset(); put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, 64); assert(!init() && devinfo.shared.console.read_idx == 0); cases++;
 reset(); put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, 0x10000); assert(!init()); cases++;
 /* A new firmware with a smaller console starts reading it from the beginning. */
 reset(); assert(!init()); devinfo.shared.console.read_idx = 60; put32(CONS + BRCMF_CONSOLE_BUFSIZE_OFFSET, 16);
 put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, 3); assert(!init() && devinfo.shared.console.read_idx == 3); cases++;
 reset();
 printf("%u shared RAM bounds controls passed\n", cases);
 return 0;
}
'''

MUTATIONS = {
    'shared': ('\tif (!brcmf_pcie_tcm_range_valid(devinfo->tcm_size, sharedram_addr,\n'
               '\t\t\t\t\tBRCMF_SHARED_SIZE)) {', '\tif (false) {'),
    'htod-mb': ('\tif (!brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\tshared->htod_mb_data_addr, 4) ||',
                '\tif (false ||'),
    'dtoh-mb': ('\t    !brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\tshared->dtoh_mb_data_addr, 4) ||',
                '\t    false ||'),
    'ring-info': ('\t    !brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\tshared->ring_info_addr,\n'
                  '\t\t\t\t\tsizeof(struct brcmf_pcie_dhi_ringinfo))) {', '\t    false) {'),
    'ring-mem': ('\tif (!brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\tle32_to_cpu(ringinfo.ringmem),',
                 '\tif (0 && !brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\tle32_to_cpu(ringinfo.ringmem),'),
    'ring-idx': ('\t\tif (!brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\t\th2d_w_idx_ptr, h2d_idx_len) ||',
                 '\t\tif (0 && !brcmf_pcie_tcm_range_valid(devinfo->tcm_size,\n\t\t\t\t\t\th2d_w_idx_ptr, h2d_idx_len) ||'),
    'flowring-idx': ('\t\th2d_idx_len = (BRCMF_NROF_H2D_COMMON_MSGRINGS + max_flowrings) *',
                     '\t\th2d_idx_len = (BRCMF_NROF_H2D_COMMON_MSGRINGS) *'),
    'dma-counts': ('\t\tif (max_submissionrings <\n\t\t    BRCMF_NROF_H2D_COMMON_MSGRINGS + max_flowrings ||\n'
                   '\t\t    max_completionrings < BRCMF_NROF_D2H_COMMON_MSGRINGS) {', '\t\tif (false) {'),
    'rptr-depth': ('\tptr = devinfo->read_ptr(devinfo, ring->r_idx_addr);\n\tif (ptr >= commonring->depth) {',
                   '\tptr = devinfo->read_ptr(devinfo, ring->r_idx_addr);\n\tif (false) {'),
    'wptr-depth': ('\tptr = devinfo->read_ptr(devinfo, ring->w_idx_addr);\n\tif (ptr >= commonring->depth) {',
                   '\tptr = devinfo->read_ptr(devinfo, ring->w_idx_addr);\n\tif (false) {'),
    'console-desc': ('\t    !brcmf_pcie_tcm_range_valid(devinfo->tcm_size, console->base_addr,\n'
                     '\t\t\t\t\tBRCMF_CONSOLE_WRITEIDX_OFFSET + 4)) {', '\t    false) {'),
    'console-none': ('\tif (!console->base_addr ||\n', '\tif (false ||\n'),
    'console-buf': ('\t    !brcmf_pcie_tcm_range_valid(devinfo->tcm_size, console->buf_addr,\n'
                    '\t\t\t\t\tconsole->bufsize)) {', '\t    false) {'),
    'console-widx': ('\tif (newidx >= console->bufsize || console->read_idx >= console->bufsize)\n\t\treturn;\n', ''),
    'console-reinit': ('\tconsole->bufsize = 0;\n\tconsole->read_idx = 0;\n', '\tconsole->bufsize = 0;\n'),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    pcie = (DRIVER / 'pcie.c').read_text()
    defines = ''.join(define(pcie, n) for n in (
        'BRCMF_PCIE_SHARED_VERSION_7', 'BRCMF_PCIE_MIN_SHARED_VERSION', 'BRCMF_PCIE_MAX_SHARED_VERSION', 'BRCMF_PCIE_SHARED_VERSION_MASK',
        'BRCMF_PCIE_SHARED_DMA_INDEX', 'BRCMF_PCIE_SHARED_DMA_2B_IDX', 'BRCMF_PCIE_SHARED_USE_MAILBOX',
        'BRCMF_PCIE_SHARED_HOSTRDY_DB1', 'BRCMF_PCIE_SHARED_DAR', 'BRCMF_HOSTCAP_H2D_ENABLE_HOSTRDY',
        'BRCMF_HOSTCAP_H2D_DAR', 'BRCMF_HOSTCAP_DS_NO_OOB_DW', 'BRCMF_DEF_MAX_RXBUFPOST',
        'BRCMF_SHARED_FLAGS_OFFSET', 'BRCMF_SHARED_MAX_RXBUFPOST_OFFSET', 'BRCMF_SHARED_RX_DATAOFFSET_OFFSET',
        'BRCMF_SHARED_CONSOLE_ADDR_OFFSET', 'BRCMF_SHARED_HTOD_MB_DATA_ADDR_OFFSET',
        'BRCMF_SHARED_DTOH_MB_DATA_ADDR_OFFSET', 'BRCMF_SHARED_RING_INFO_ADDR_OFFSET',
        'BRCMF_SHARED_FLAGS2_OFFSET', 'BRCMF_SHARED_HOST_CAP_OFFSET', 'BRCMF_SHARED_FLAGS3_OFFSET',
        'BRCMF_SHARED_HOST_CAP2_OFFSET', 'BRCMF_SHARED_SIZE', 'BRCMF_CONSOLE_BUFADDR_OFFSET',
        'BRCMF_CONSOLE_BUFSIZE_OFFSET', 'BRCMF_CONSOLE_WRITEIDX_OFFSET', 'BRCMF_RING_MEM_SZ',
        'BRCMF_RING_MAX_ITEM_OFFSET', 'BRCMF_RING_MEM_BASE_ADDR_OFFSET'))
    bus_h = (DRIVER / 'bus.h').read_text()
    defines += ''.join(define(bus_h, n) for n in (
        'BRCMF_H2D_MSGRING_FLOWRING_IDSTART', 'BRCMF_NROF_H2D_COMMON_MSGRINGS', 'BRCMF_NROF_D2H_COMMON_MSGRINGS'))
    defines += '#define BRCMF_NROF_COMMON_MSGRINGS (BRCMF_NROF_H2D_COMMON_MSGRINGS + BRCMF_NROF_D2H_COMMON_MSGRINGS)\n'
    structs = struct((DRIVER / 'msgbuf.h').read_text(), 'msgbuf_buf_addr') + struct(pcie, 'brcmf_pcie_dhi_ringinfo')
    body = function((DRIVER / 'pcie.h').read_text(), 'brcmf_pcie_tcm_range_valid') + ''.join(
        function(pcie, n) for n in ('brcmf_pcie_read_idx', 'brcmf_pcie_write_idx',
                                    'brcmf_pcie_ring_mb_update_rptr', 'brcmf_pcie_ring_mb_update_wptr',
                                    'brcmf_pcie_bus_console_init', 'brcmf_pcie_bus_console_read',
                                    'brcmf_pcie_init_ringbuffers', 'brcmf_pcie_init_share_ram_info'))
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        if old not in body:
            raise ValueError('mutation site missing: ' + args.mutation)
        body = body.replace(old, new)
    code = PREFIX.replace('/* DEFINES */', defines).replace('/* STRUCTS */', structs) + body + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / 'controls.c'
        source.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-Wno-incompatible-function-pointer-types',
                        str(source), '-o', str(exe)], check=True)
        env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
        receipt = {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'limits': 'Fake TCM and dongle tables; no firmware, PCIe link or DMA proof.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))
        return result.returncode

    if args.out:
        raise SystemExit(run(args.out))
    with tempfile.TemporaryDirectory(prefix='brcmf-shared-tcm-') as temp:
        raise SystemExit(run(Path(temp)))


if __name__ == '__main__':
    main()
