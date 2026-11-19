#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production brcmfmac PCIe firmware death detection against a fake dongle RAM."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:void|int|bool|s32)\s+' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError('Missing function ' + name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


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
typedef int32_t s32; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef unsigned int uint;
#define HZ 100
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define xchg(p,v) ({ __typeof__(*(p)) o_ = *(p); *(p) = (v); o_; })
#define time_after(a,b) ((long)((b) - (a)) < 0)
#define brcmf_dbg(...) ((void)0)
#define bphy_err(d, ...) ((void)(d))
static char last_err[256];
static unsigned errs;
static void brcmf_err(void *bus, const char *fmt, ...)
{ va_list ap; (void)bus; va_start(ap, fmt); vsnprintf(last_err, sizeof(last_err), fmt, ap); va_end(ap); errs++; }
enum brcmf_bus_state { BRCMF_BUS_DOWN, BRCMF_BUS_UP };
enum brcmf_pcie_state { BRCMFMAC_PCIE_STATE_DOWN, BRCMFMAC_PCIE_STATE_UP };
/* DEFINES */
static unsigned long jiffies = 1000;
struct device { int unused; };
struct pci_dev { struct device dev; };
struct brcmf_pcie_console { u32 base_addr, buf_addr, bufsize, read_idx;
 u32 scan_idx; u8 scan_line[8]; u8 scan_len; };
struct brcmf_pcie_shared_info { u32 tcm_base_address; struct brcmf_pcie_console console; };
struct brcmf_pciedev_info { enum brcmf_pcie_state state; struct pci_dev *pdev;
 struct brcmf_pcie_shared_info shared; bool fw_dead, reset_wanted; u32 recoveries; unsigned long recovery_window; };
struct pcie_priv { struct brcmf_pciedev_info *devinfo; };
struct brcmf_bus { enum brcmf_bus_state state; struct device *dev; struct { struct pcie_priv *pcie; } bus_priv; };
struct brcmf_proto { void *pd; };
struct brcmf_pub { struct brcmf_bus *bus_if; struct brcmf_proto *proto; };
struct sk_buff { u8 data[64]; u32 len; };
static struct brcmf_bus bus;
static struct brcmf_bus *dev_get_drvdata(struct device *dev) { (void)dev; return &bus; }

/* Fake dongle RAM. */
static u8 tcm[0x4000];
static bool link_down;
static u32 tcm_reads;
static u32 brcmf_pcie_read_tcm32(struct brcmf_pciedev_info *d, u32 a)
{ (void)d; tcm_reads++; if (link_down) return ~0U; assert(a + 4 <= sizeof(tcm)); u32 v; memcpy(&v, tcm + a, 4); return v; }
static u8 brcmf_pcie_read_tcm8(struct brcmf_pciedev_info *d, u32 a)
{ (void)d; tcm_reads++; if (link_down) return 0xff; assert(a < sizeof(tcm)); return tcm[a]; }
static void put32(u32 a, u32 v) { memcpy(tcm + a, &v, 4); }

static unsigned crashes, state_downs;
static void brcmf_fw_crashed(struct device *dev) { (void)dev; crashes++; }
static void brcmf_bus_change_state(struct brcmf_bus *b, enum brcmf_bus_state s)
{ b->state = s; if (s == BRCMF_BUS_DOWN) state_downs++; }

/* msgbuf side */
struct brcmf_msgbuf { u32 ctl_stalls; bool ctl_completed; void *rx_pktids; u32 ioctl_resp_pktid,
 ioctl_resp_ret_len; int ioctl_resp_status; struct brcmf_pub *drvr; };
static int tx_error, reply;
static u32 stall_calls, last_stalls;
static void brcmf_bus_ctl_stall(struct brcmf_bus *b, u32 stalls) { (void)b; stall_calls++; last_stalls = stalls; }
static int brcmf_msgbuf_tx_ioctl(struct brcmf_pub *d, int i, uint c, void *b, uint l)
{ (void)d; (void)i; (void)c; (void)b; (void)l; return tx_error; }
static int brcmf_msgbuf_ioctl_resp_wait(struct brcmf_msgbuf *m) { (void)m; return reply; }
static struct sk_buff skb;
static struct sk_buff *brcmf_msgbuf_get_pktid(void *d, void *p, u32 n) { (void)d; (void)p; (void)n; return &skb; }
static void brcmu_pkt_buf_free_skb(struct sk_buff *s) { (void)s; }
'''

MAIN = r'''
#define SHARED 0x100
#define CONS 0x200
#define BUF 0x400
#define BUFSZ 64
static struct pci_dev pdev;
static struct brcmf_pciedev_info devinfo;
static struct pcie_priv priv = { &devinfo };
static void reset(void)
{
 memset(tcm, 0, sizeof(tcm)); link_down = false;
 memset(&devinfo, 0, sizeof(devinfo)); devinfo.pdev = &pdev; devinfo.state = BRCMFMAC_PCIE_STATE_UP;
 devinfo.shared.tcm_base_address = SHARED; put32(SHARED + BRCMF_SHARED_FLAGS_OFFSET, 0x10007);
 devinfo.shared.console.base_addr = CONS; devinfo.shared.console.buf_addr = BUF;
 devinfo.shared.console.bufsize = BUFSZ; put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, 0);
 bus.state = BRCMF_BUS_UP; bus.bus_priv.pcie = &priv;
 crashes = state_downs = errs = 0; last_err[0] = 0;
}
/* Append console text at the firmware write index, wrapping like the ring. */
static void con(const char *s)
{
 u32 w = brcmf_pcie_read_tcm32(&devinfo, CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET);
 for (; *s; s++) { tcm[BUF + w] = (u8)*s; w = (w + 1) % BUFSZ; }
 put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, w);
}
static void stall(u32 n) { brcmf_pcie_ctl_stall(&pdev.dev, n); }
static bool died(const char *why) { return crashes == 1 && bus.state == BRCMF_BUS_DOWN && devinfo.fw_dead && devinfo.reset_wanted && strstr(last_err, why); }
int main(void)
{
 unsigned cases = 0;
 /* A silent dongle is dead after BRCMF_PCIE_CTL_STALL_MAX stalls in a row, not before. */
 reset();
 for (u32 i = 1; i < BRCMF_PCIE_CTL_STALL_MAX; i++) { stall(i); assert(!crashes && bus.state == BRCMF_BUS_UP && !devinfo.fw_dead); }
 stall(BRCMF_PCIE_CTL_STALL_MAX); assert(died("no response")); cases++;
 /* Only once per firmware instance. */
 stall(BRCMF_PCIE_CTL_STALL_MAX + 1); stall(1); assert(crashes == 1);
 /* A halt notice racing the stall check does not reset twice. */
 brcmf_pcie_fw_died(&devinfo, "firmware halt notice"); assert(crashes == 1); cases++;
 /* Trap flag in shared RAM: dead at the first stall. */
 reset(); put32(SHARED, 0x10007 | BRCMF_PCIE_SHARED_TRAP); put32(SHARED + BRCMF_SHARED_TRAP_ADDR_OFFSET, 0x800);
 put32(0x800, 4); put32(0x804, 0x1234); stall(1); assert(died("trap in shared RAM")); cases++;
 /* Assert only counts when the firmware was built with asserts. */
 reset(); put32(SHARED, 0x10007 | BRCMF_PCIE_SHARED_ASSERT); stall(1); assert(!crashes); cases++;
 reset(); put32(SHARED, 0x10007 | BRCMF_PCIE_SHARED_ASSERT | BRCMF_PCIE_SHARED_ASSERT_BUILT);
 stall(1); assert(died("assert")); cases++;
 /* A device that reads back all ones is gone. */
 reset(); link_down = true; stall(1); assert(died("not responding")); cases++;
 /* Console trap marker at a line start. */
 reset(); con("wl0: fatal error, reinitializing\n"); stall(1); assert(!crashes);
 con("THREADX TRAP INFO:\n"); stall(1); assert(!crashes);
 con("\nTRAP 4(930b00): pc 7594be\n"); stall(1); assert(died("console")); cases++;
 /* Marker split across the end of the ring. */
 reset(); con("0123456789012345678901234567890123456789012345678901234567890\n");
 stall(1); assert(!crashes); con("TRAP 4\n"); stall(1); assert(died("console")); cases++;
 /* Marker split across two scans. */
 reset(); con("\nTR"); stall(1); assert(!crashes); con("AP 4\n"); stall(1); assert(died("console")); cases++;
 /* Console text written before the scan start (an earlier firmware) is ignored. */
 reset(); con("TRAP 4 old\n"); devinfo.shared.console.scan_idx = brcmf_pcie_read_tcm32(&devinfo, CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET);
 stall(1); assert(!crashes); cases++;
 /* A garbage write index does not run the scan off the ring. */
 reset(); put32(CONS + BRCMF_CONSOLE_WRITEIDX_OFFSET, BUFSZ + 5); stall(1); assert(!crashes); cases++;
 /* Nothing happens while the bus is down. */
 reset(); devinfo.state = BRCMFMAC_PCIE_STATE_DOWN; put32(SHARED, 0x10007 | BRCMF_PCIE_SHARED_TRAP);
 tcm_reads = 0; stall(BRCMF_PCIE_CTL_STALL_MAX); assert(!crashes && !tcm_reads); cases++;
 /* At most BRCMF_PCIE_RECOVERY_MAX resets per window, then the driver gives up until it passes. */
 reset();
 for (u32 i = 1; i <= BRCMF_PCIE_RECOVERY_MAX; i++) {
  devinfo.fw_dead = devinfo.reset_wanted = false; bus.state = BRCMF_BUS_UP; jiffies += 10 * HZ;
  stall(BRCMF_PCIE_CTL_STALL_MAX);
  assert(crashes == i && devinfo.reset_wanted);
 }
 devinfo.fw_dead = devinfo.reset_wanted = false; bus.state = BRCMF_BUS_UP; jiffies += 10 * HZ;
 stall(BRCMF_PCIE_CTL_STALL_MAX);
 /* A death past the limit asks for no reset, so the end of a system sleep replays none. */
 assert(crashes == BRCMF_PCIE_RECOVERY_MAX && bus.state == BRCMF_BUS_UP && !devinfo.reset_wanted &&
        strstr(last_err, "not resetting"));
 devinfo.fw_dead = false; jiffies += BRCMF_PCIE_RECOVERY_WINDOW_S * HZ; stall(BRCMF_PCIE_CTL_STALL_MAX);
 assert(crashes == BRCMF_PCIE_RECOVERY_MAX + 1 && bus.state == BRCMF_BUS_DOWN); cases++;
 /* msgbuf counts stalled control requests in a row and clears the count on a reply. */
 struct brcmf_msgbuf mb = { 0 }; struct brcmf_proto proto = { &mb }; struct brcmf_pub drvr = { &bus, &proto };
 mb.drvr = &drvr; u8 buf[8]; int fwerr; u32 rl;
 stall_calls = 0; tx_error = 0; reply = 0;
 for (u32 i = 1; i <= 3; i++) { assert(brcmf_msgbuf_query_dcmd_len(&drvr, 0, 1, buf, 8, &fwerr, &rl) == -EIO); assert(last_stalls == i); }
 tx_error = -ENOMEM; assert(brcmf_msgbuf_query_dcmd_len(&drvr, 0, 1, buf, 8, &fwerr, &rl) == -ENOMEM && last_stalls == 4);
 tx_error = -EIO; assert(brcmf_msgbuf_query_dcmd_len(&drvr, 0, 1, buf, 8, &fwerr, &rl) == -EIO && stall_calls == 4);
 tx_error = 0; reply = 1; assert(!brcmf_msgbuf_query_dcmd_len(&drvr, 0, 1, buf, 8, &fwerr, &rl) && stall_calls == 4);
 reply = 0; assert(brcmf_msgbuf_query_dcmd_len(&drvr, 0, 1, buf, 8, &fwerr, &rl) == -EIO && last_stalls == 1); cases++;
 printf("%u firmware death detection controls passed\n", cases);
 return 0;
}
'''

MUTATIONS = {
    'stall-limit': ('pcie', '} else if (stalls >= BRCMF_PCIE_CTL_STALL_MAX) {', '} else if (stalls >= 1) {'),
    'trap-flag': ('pcie', '} else if (flags & BRCMF_PCIE_SHARED_TRAP) {', '} else if (false) {'),
    'console': ('pcie', '\t\tif (++console->scan_len == sizeof(marker) - 1)\n\t\t\ttrapped = true;',
                '\t\t++console->scan_len;'),
    'line-start': ('pcie', '\t\t\tconsole->scan_len = sizeof(marker) - 1;\n\t\t\tcontinue;',
                   '\t\t\tconsole->scan_len = 0;\n\t\t\tcontinue;'),
    'once': ('pcie', '\tif (xchg(&devinfo->fw_dead, true))\n\t\treturn;', '\tdevinfo->fw_dead = true;'),
    'rate-limit': ('pcie', '\tif (++devinfo->recoveries > BRCMF_PCIE_RECOVERY_MAX) {', '\tif (false) {'),
    'reset-wanted': ('pcie', '\tWRITE_ONCE(devinfo->reset_wanted, true);\n', ''),
    'wanted-over-limit': ('pcie', '\tif (xchg(&devinfo->fw_dead, true))\n\t\treturn;\n',
                          '\tif (xchg(&devinfo->fw_dead, true))\n\t\treturn;\n'
                          '\tWRITE_ONCE(devinfo->reset_wanted, true);\n'),
    'stall-reset': ('msgbuf', '\tmsgbuf->ctl_stalls = 0;\n', ''),
    'ring-full': ('msgbuf', '\t\tif (err == -ENOMEM)\n', '\t\tif (false)\n'),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    sources = {name: (DRIVER / (name + '.c')).read_text() for name in ('pcie', 'msgbuf')}
    defines = ''.join(define(sources['pcie'], n) for n in (
        'BRCMF_PCIE_SHARED_ASSERT_BUILT', 'BRCMF_PCIE_SHARED_ASSERT', 'BRCMF_PCIE_SHARED_TRAP',
        'BRCMF_SHARED_FLAGS_OFFSET', 'BRCMF_SHARED_TRAP_ADDR_OFFSET', 'BRCMF_CONSOLE_WRITEIDX_OFFSET',
        'BRCMF_PCIE_CTL_STALL_MAX', 'BRCMF_PCIE_RECOVERY_MAX', 'BRCMF_PCIE_RECOVERY_WINDOW_S'))
    bodies = {
        'pcie': ''.join(function(sources['pcie'], n) for n in (
            'brcmf_pcie_fw_died', 'brcmf_pcie_console_trapped', 'brcmf_pcie_ctl_stall')),
        'msgbuf': function(sources['msgbuf'], 'brcmf_msgbuf_query_dcmd_len'),
    }
    if args.mutation:
        group, old, new = MUTATIONS[args.mutation]
        if old not in bodies[group]:
            raise ValueError('mutation site missing: ' + args.mutation)
        bodies[group] = bodies[group].replace(old, new)
    code = PREFIX.replace('/* DEFINES */', defines) + bodies['pcie'] + bodies['msgbuf'] + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / 'controls.c'
        source.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(source), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt = {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'limits': 'Fake dongle RAM and console; no firmware, PCIe link or reset path proof.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))
        return result.returncode

    if args.out:
        raise SystemExit(run(args.out))
    with tempfile.TemporaryDirectory(prefix='brcmf-recovery-') as temp:
        raise SystemExit(run(Path(temp)))


if __name__ == '__main__':
    main()
