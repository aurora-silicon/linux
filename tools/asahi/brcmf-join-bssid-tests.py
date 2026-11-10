#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check which BSS brcmfmac names when a station join succeeds or fails.

A failed join is reported to cfg80211 against profile->bssid, and
wpa_supplicant puts that BSS on its ignore list.  The profile must hold the
BSS the join asked for, not the one the interface was last associated with.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/'

# Each must make the controls fail.
MUTATIONS = {
    'stale-target': ('\t\tmemcpy(profile->bssid, bssid, ETH_ALEN);\n', '\t\t;\n'),
    'zero-named': ('\t\tif (!is_zero_ether_addr(profile->bssid))\n'
                   '\t\t\tconn_params.links[0].bssid = profile->bssid;\n',
                   '\t\tconn_params.links[0].bssid = profile->bssid;\n'),
    'no-call': ('\tbrcmf_connect_set_target(profile, params->bssid);\n', ''),
    'lost-bss-success': ('\t\t\t\tcompleted = false;\n', '\t\t\t\t;\n'),
}

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define ETH_ALEN 6
#define GFP_KERNEL 0
#define brcmf_dbg(...) ((void)0)
enum { WLAN_STATUS_SUCCESS = 0, WLAN_STATUS_AUTH_TIMEOUT = 16 };
enum { BRCMF_E_SET_SSID = 0, BRCMF_E_PSK_SUP = 46 };
enum { BRCMF_E_STATUS_SUCCESS = 0, BRCMF_E_STATUS_TIMEOUT = 2, BRCMF_E_STATUS_FWSUP_COMPLETED = 6 };
enum { BRCMF_VIF_STATUS_CONNECTING, BRCMF_VIF_STATUS_CONNECTED,
       BRCMF_VIF_STATUS_EAP_SUCCESS, BRCMF_VIF_STATUS_ASSOC_SUCCESS };
enum { BRCMF_PROFILE_FWSUP_NONE, BRCMF_PROFILE_FWSUP_PSK, BRCMF_PROFILE_FWSUP_1X,
       BRCMF_PROFILE_FWSUP_SAE };
struct brcmf_cfg80211_profile { u8 bssid[ETH_ALEN]; int use_fwsup; };
struct brcmf_cfg80211_vif { struct brcmf_cfg80211_profile profile; unsigned long sme_state; };
struct brcmf_if { struct brcmf_cfg80211_vif *vif; };
struct net_device { struct brcmf_if *ifp; };
struct brcmf_cfg80211_connect_info { u8 *req_ie, *resp_ie; u32 req_ie_len, resp_ie_len; };
struct brcmf_cfg80211_info { struct brcmf_cfg80211_connect_info conn; };
struct brcmf_event_msg { u32 event_code, status; u8 addr[ETH_ALEN]; };
struct cfg80211_connect_resp_params {
 int status; const u8 *req_ie, *resp_ie; size_t req_ie_len, resp_ie_len;
 struct { const u8 *bssid; } links[1];
};
static struct brcmf_if *netdev_priv(struct net_device *n) { return n->ifp; }
static struct brcmf_cfg80211_connect_info *cfg_to_conn(struct brcmf_cfg80211_info *c) { return &c->conn; }
static void set_bit(int b, unsigned long *w) { *w |= 1UL << b; }
static void clear_bit(int b, unsigned long *w) { *w &= ~(1UL << b); }
static bool test_bit(int b, unsigned long *w) { return *w & (1UL << b); }
static bool test_and_clear_bit(int b, unsigned long *w) { bool r = test_bit(b, w); clear_bit(b, w); return r; }
static bool is_zero_ether_addr(const u8 *a) { static const u8 z[ETH_ALEN]; return !memcmp(a, z, ETH_ALEN); }
static void eth_zero_addr(u8 *a) { memset(a, 0, ETH_ALEN); }
static int reports, authorized;
static int reported_status;
static u8 reported_bssid[ETH_ALEN];
static bool reported_none;
static void brcmf_get_assoc_ies(struct brcmf_cfg80211_info *c, struct brcmf_if *i) { (void)c; (void)i; }
#define BRCMF_C_DISASSOC 52
static s32 bss_info_err;
static bool bss_known = true;
static int disassocs;
static s32 brcmf_update_bss_info(struct brcmf_cfg80211_info *c, struct brcmf_if *i) { (void)c; (void)i; return bss_info_err; }
static bool brcmf_bss_known(struct brcmf_cfg80211_info *c, const u8 *b) { (void)c; (void)b; return bss_known; }
static s32 brcmf_fil_cmd_data_set(struct brcmf_if *i, u32 cmd, void *d, u32 l)
{ (void)i; (void)d; (void)l; if (cmd == BRCMF_C_DISASSOC) disassocs++; return 0; }
static void cfg80211_connect_done(struct net_device *n, struct cfg80211_connect_resp_params *p, int gfp)
{
 (void)n; (void)gfp; reports++; reported_status = p->status;
 reported_none = !p->links[0].bssid;
 if (p->links[0].bssid) memcpy(reported_bssid, p->links[0].bssid, ETH_ALEN);
}
static void cfg80211_port_authorized(struct net_device *n, const u8 *b, const u8 *x, int l, int g)
{ (void)n; (void)b; (void)x; (void)l; (void)g; authorized++; }
'''

MAIN = r'''
static const u8 OLD5[ETH_ALEN] = { 0xd2, 0x21, 0xf9, 0x33, 0x59, 0xed };
static const u8 TRY24[ETH_ALEN] = { 0xd0, 0x21, 0xf9, 0x53, 0x59, 0xec };
static struct brcmf_cfg80211_vif vif;
static struct brcmf_if ifp = { &vif };
static struct net_device ndev = { &ifp };
static struct brcmf_cfg80211_info cfg;

/* Associated with OLD5 earlier, then a join is started for @target. */
static void start(const u8 *target, int fwsup)
{
 memset(&vif, 0, sizeof(vif));
 memcpy(vif.profile.bssid, OLD5, ETH_ALEN);
 vif.profile.use_fwsup = fwsup;
 reports = authorized = 0; reported_status = -1; reported_none = false;
 memset(reported_bssid, 0xee, ETH_ALEN);
 brcmf_connect_set_target(&vif.profile, target);
 set_bit(BRCMF_VIF_STATUS_CONNECTING, &vif.sme_state);
}

int main(void)
{
 int controls = 0;

 /* The firmware timed out joining TRY24: name TRY24, never OLD5. */
 for (int fwsup = 0; fwsup <= BRCMF_PROFILE_FWSUP_SAE; fwsup++) {
  start(TRY24, fwsup);
  brcmf_bss_connect_done(&cfg, &ndev, NULL, false);
  assert(reports == 1 && reported_status == WLAN_STATUS_AUTH_TIMEOUT);
  assert(!reported_none && !memcmp(reported_bssid, TRY24, ETH_ALEN));
  assert(!authorized && !test_bit(BRCMF_VIF_STATUS_CONNECTED, &vif.sme_state));
  controls++;
 }

 /* A join that asked for no BSS and failed names none. */
 start(NULL, BRCMF_PROFILE_FWSUP_PSK);
 brcmf_bss_connect_done(&cfg, &ndev, NULL, false);
 assert(reports == 1 && reported_status == WLAN_STATUS_AUTH_TIMEOUT && reported_none);
 controls++;

 /* Success names the BSS the firmware reports joining. */
 for (int target = 0; target < 2; target++) {
  start(target ? TRY24 : NULL, BRCMF_PROFILE_FWSUP_NONE);
  struct brcmf_event_msg e = { BRCMF_E_SET_SSID, BRCMF_E_STATUS_SUCCESS,
                               { 0xd0, 0x21, 0xf9, 0x53, 0x59, 0xec } };
  assert(brcmf_is_linkup(&vif, &e));
  brcmf_bss_connect_done(&cfg, &ndev, &e, true);
  assert(reports == 1 && reported_status == WLAN_STATUS_SUCCESS);
  assert(!reported_none && !memcmp(reported_bssid, e.addr, ETH_ALEN));
  assert(test_bit(BRCMF_VIF_STATUS_CONNECTED, &vif.sme_state));
  controls++;
 }

 /* Firmware supplicant: link up only after both events; then authorised. */
 start(TRY24, BRCMF_PROFILE_FWSUP_PSK);
 struct brcmf_event_msg ssid = { BRCMF_E_SET_SSID, BRCMF_E_STATUS_SUCCESS,
                                 { 0xd0, 0x21, 0xf9, 0x53, 0x59, 0xec } };
 struct brcmf_event_msg psk = { BRCMF_E_PSK_SUP, BRCMF_E_STATUS_FWSUP_COMPLETED, { 0 } };
 assert(!brcmf_is_linkup(&vif, &ssid) && brcmf_is_linkup(&vif, &psk));
 brcmf_bss_connect_done(&cfg, &ndev, &psk, true);
 assert(reports == 1 && !memcmp(reported_bssid, TRY24, ETH_ALEN) && authorized == 1);
 controls++;

 /* A link-up after the join was torn down: no BSS from the firmware or cfg80211. */
 for (int fwsup = 0; fwsup <= BRCMF_PROFILE_FWSUP_SAE; fwsup++) {
  start(TRY24, fwsup);
  bss_info_err = -22; bss_known = false; disassocs = 0;
  brcmf_bss_connect_done(&cfg, &ndev, &ssid, true);
  assert(reports == 1 && reported_status == WLAN_STATUS_AUTH_TIMEOUT);
  assert(!authorized && !test_bit(BRCMF_VIF_STATUS_CONNECTED, &vif.sme_state));
  assert(disassocs == 1);
  controls++;
 }

 /* The firmware lost the record but cfg80211 knows the BSS: still connected. */
 start(TRY24, BRCMF_PROFILE_FWSUP_PSK);
 bss_info_err = -22; bss_known = true; disassocs = 0;
 brcmf_bss_connect_done(&cfg, &ndev, &psk, true);
 assert(reports == 1 && reported_status == WLAN_STATUS_SUCCESS && authorized == 1);
 assert(test_bit(BRCMF_VIF_STATUS_CONNECTED, &vif.sme_state) && !disassocs);
 bss_info_err = 0;
 controls++;

 /* Only one report per join. */
 brcmf_bss_connect_done(&cfg, &ndev, NULL, false);
 assert(reports == 1);
 controls++;

 printf("%d join BSSID controls passed\n", controls);
 return 0;
}
'''


def function(source, name):
    match = re.search(r'^static (?:s32|void|bool)\s*\n?' + name + r'\(', source, re.M)
    if not match:
        raise ValueError(name)
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ref', help='Git source revision to exercise')
    parser.add_argument('--out', type=Path, help='retain executable and results')
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()

    if args.ref:
        source = subprocess.check_output(['git', 'show', args.ref + ':' + DRIVER + 'cfg80211.c'],
                                         cwd=ROOT, text=True)
    else:
        source = (ROOT / DRIVER / 'cfg80211.c').read_text()
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert source.count(old) == 1, args.mutation
        source = source.replace(old, new)

    # The connect handler must record its target before it asks for the join.
    connect = function(source, 'brcmf_cfg80211_connect')
    hint = connect.find('params->bssid = params->bssid_hint;')
    record = connect.find('brcmf_connect_set_target(profile, params->bssid);')
    join = connect.find('"join"')
    if not (0 <= hint < record < join):
        print('Assertion failed: brcmf_cfg80211_connect() does not record the join target '
              'after the BSSID hint and before the join')
        return 1

    bodies = [function(source, name) for name in
              ('brcmf_connect_set_target', 'brcmf_is_linkup', 'brcmf_bss_connect_done')]
    code = PREFIX + ''.join(bodies) + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        src = out / 'controls.c'
        src.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(src), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt = {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'mutation': args.mutation,
                   'limits': 'cfg80211 and firmware are host stubs; no hardware join.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        sys.stdout.write(result.stdout or result.stderr)
        return result.returncode != 0

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='brcmf-join-bssid-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
