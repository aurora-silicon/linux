#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production WoWLAN sleep-filter builders and check the pkt_filter_add wire bytes."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:void|int|s32)\s+' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError('Missing function ' + name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def block(source, pattern):
    match = re.search(pattern, source, re.S | re.M)
    if not match:
        raise ValueError('Missing ' + pattern)
    return match[0] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint32_t __be32;
#define __packed __attribute__((packed))
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define ETH_ALEN 6
#define NDOL_MAX_ENTRIES 8
#define IPPROTO_UDP 17
#define IPPROTO_ICMP 1
#define IPPROTO_ICMPV6 58
#define GFP_KERNEL 0
#define WARN_ON(x) (x)
#define brcmf_dbg(...) do { } while (0)
#define kzalloc(n, f) calloc(1, (n))
#define kfree free
struct in6_addr { u8 s6_addr[16]; };
struct net_device { u8 dev_addr[ETH_ALEN]; };
struct brcmf_if { struct net_device *ndev; struct in6_addr ipv6_addr_tbl[NDOL_MAX_ENTRIES]; u8 ipv6addr_idx; };
'''

STUBS = r'''
struct brcmf_cfg80211_wowl { u32 lpas_filter_ids[BRCMF_LPAS_MAX_FILTERS]; u8 n_lpas_filters; };
static u8 sent[32][256]; static size_t sent_len[32]; static int n_sent, enabled, fail_from = 99;
static int brcmf_fil_iovar_data_set(struct brcmf_if *ifp, const char *name, const void *data, u32 len)
{
	(void)ifp; assert(!strcmp(name, "pkt_filter_add")); assert(len <= 256 && n_sent < 32);
	if (n_sent >= fail_from) return -EBADE;
	memcpy(sent[n_sent], data, len); sent_len[n_sent++] = len; return 0;
}
static int brcmf_fil_iovar_int_set(struct brcmf_if *ifp, const char *name, u32 v) { (void)ifp; (void)name; (void)v; return 0; }
static int brcmf_lpas_filter_enable(struct brcmf_if *ifp, u32 id, bool en) { (void)ifp; (void)id; assert(en); enabled++; return 0; }
'''

MAIN = r'''
int main(int argc, char **argv)
{
	static struct net_device nd = { { 0x3a, 0x72, 0x4e, 0x2c, 0xfb, 0x24 } };
	static struct brcmf_if ifp = { .ndev = &nd, .ipv6addr_idx = 2 };
	struct brcmf_cfg80211_wowl w = { 0 };
	__be32 ip4[2] = { 0x9b01010a, 0x0202a8c0 };
	int i, j;

	fail_from = argc > 1 ? atoi(argv[1]) : 99;
	for (i = 0; i < 16; i++) { ifp.ipv6_addr_tbl[0].s6_addr[i] = 0xfe - i; ifp.ipv6_addr_tbl[1].s6_addr[i] = 0x20 + i; }
	brcmf_lpas_ucast_filters_install(&ifp, &w, ip4, 2);
	assert(w.n_lpas_filters == n_sent && enabled == n_sent);
	for (i = 0; i < n_sent; i++) {
		printf("%u ", w.lpas_filter_ids[i]);
		for (j = 0; j < (int)sent_len[i]; j++) printf("%02x", sent[i][j]);
		printf("\n");
	}
	return 0;
}
'''

MAC = bytes.fromhex('3a724e2cfb24')
PREFIX_BYTES = b'\xff' * 6 + MAC
IP4 = [bytes.fromhex('0a01019b'), bytes.fromhex('c0a80202')]
IP6 = [bytes(0xfe - i for i in range(16)), bytes(0x20 + i for i in range(16))]
# Public DHD wl_pkt_filter_base values and the type 2 (pattern list) layout.
ETH_H, ETH_D, IP4_H, IP6_H, UDP_D = 4, 5, 8, 10, 15


def el(base, rel, data, neg=False):
    return struct.pack('<HHHH', rel, base, len(data), 1 if neg else 0) + b'\xff' * len(data) + data


def flt(fid, *els):
    body = b''.join(els)
    return struct.pack('<III', fid, 2, 0) + struct.pack('<BBH', len(els), 0, 4 + len(body)) + body


def expected():
    out = [(102, flt(102, el(ETH_H, 12, b'\x08\x42'), el(ETH_D, 0, PREFIX_BYTES)))]
    for i, a in enumerate(IP4):
        out.append((103 + i, flt(103 + i, el(IP4_H, 16, a), el(IP4_H, 9, b'\x11'), el(UDP_D, 0, PREFIX_BYTES))))
    for i, a in enumerate(IP6):
        out.append((105 + i, flt(105 + i, el(IP6_H, 24, a), el(IP6_H, 6, b'\x11'), el(UDP_D, 0, PREFIX_BYTES))))
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=['base-order', 'mask-last', 'totsize', 'no-track'])
    args = parser.parse_args()
    cfg = (DRIVER / 'cfg80211.c').read_text()
    hdr = (DRIVER / 'cfg80211.h').read_text()
    types = (DRIVER / 'fwil_types.h').read_text()
    defs = ''.join(re.findall(r'^#define BRCMF_LPAS_\w+(?:.*\\\n)*.*\n', cfg, re.M))
    defs += ''.join(re.findall(r'^#define BRCMF_LPAS_MAX_\w+.*\n', hdr, re.M))
    defs += ''.join(re.findall(r'^#define BRCMF_PKT_FILTER_\w+.*\n', types, re.M))
    body = (block(types, r'^struct brcmf_pkt_filter_pattern_le \{.*?\n\};')
            + block(types, r'^struct brcmf_pkt_filter_le \{.*?\n\};')
            + block(types, r'^enum brcmf_pkt_filter_base \{.*?\n\};')
            + block(types, r'^struct brcmf_pkt_filter_listel_le \{.*?\n\} __packed;')
            + block(types, r'^struct brcmf_pkt_filter_list_le \{.*?\n\} __packed;')
            + STUBS
            + block(cfg, r'^struct brcmf_lpas_el \{.*?\n\};')
            + function(cfg, 'brcmf_lpas_filter_track')
            + function(cfg, 'brcmf_lpas_list_filter_install')
            + function(cfg, 'brcmf_lpas_ucast_filters_install'))
    if args.mutation == 'base-order':
        body = body.replace('BRCMF_PKT_FILTER_BASE_D11_D,\n', '')
    elif args.mutation == 'mask-last':
        body = body.replace('memset(e->mask_and_data, 0xff, el[i].len);',
                            'memset(e->mask_and_data + el[i].len, 0xff, el[i].len);')
        body = body.replace('memcpy(e->mask_and_data + el[i].len, el[i].data, el[i].len);',
                            'memcpy(e->mask_and_data, el[i].data, el[i].len);')
    elif args.mutation == 'totsize':
        body = body.replace('list->totsize = cpu_to_le16(size);', 'list->totsize = cpu_to_le16(size - sizeof(*list));')
    elif args.mutation == 'no-track':
        body = body.replace('brcmf_lpas_filter_track(wowl, id);', '(void)id;')
    code = PREFIX + defs + body + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / 'filters.c'
        source.write_text(code)
        exe = out / 'filters'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(source), '-o', str(exe)], check=True)
        fails = []
        full = subprocess.run([str(exe)], capture_output=True, text=True)
        got = [(int(l.split()[0]), bytes.fromhex(l.split()[1])) for l in full.stdout.split('\n') if l]
        if full.returncode or got != expected():
            fails.append('full set: exit %d, %d filters, mismatch' % (full.returncode, len(got)))
        # The firmware runs out of filter slots after the third: the rest are skipped, the first three stay tracked.
        part = subprocess.run([str(exe), '3'], capture_output=True, text=True)
        got = [(int(l.split()[0]), bytes.fromhex(l.split()[1])) for l in part.stdout.split('\n') if l]
        if part.returncode or got != expected()[:3]:
            fails.append('slot exhaustion: exit %d, %d filters' % (part.returncode, len(got)))
        for n, fail in enumerate(fails, 1):
            print('FAIL line %d: %s' % (n, fail))
        receipt = {'exit': 1 if fails else 0, 'fails': fails,
                   'limits': 'Host stubs; checks the pkt_filter_add bytes only, not firmware matching.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))
        return 1 if fails else 0

    if args.out:
        raise SystemExit(run(args.out))
    with tempfile.TemporaryDirectory(prefix='brcmf-wowl-filter-') as temp:
        raise SystemExit(run(Path(temp)))


if __name__ == '__main__':
    main()
