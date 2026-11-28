#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the checkout's T6050 Wi-Fi 6 GHz avoid helper against a mock cfg80211 BSS list.

The mock keeps cfg80211's set behaviour (unlinking any member of a beacon's
multiple-BSSID set unlinks the whole set), entries cfg80211 does not return
(expired) and entries it does not unlink. No kernel build or hardware is needed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

SRC = "drivers/net/wireless/apple/centauri/t6050/centauri_wifi.c"


def block(source, kind, name):
    """The production function (kind: bool, void) or struct `name`, braces balanced."""
    if kind == "struct":
        match = re.search(r"^struct " + re.escape(name) + r" \{", source, re.M)
    else:
        match = re.search(r"^static " + kind + r" " + re.escape(name) + r"\(", source, re.M)
    if not match:
        raise RuntimeError(f"production {kind} {name} missing")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if kind == "struct":
        end = source.index(";", end) + 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define WLAN_EID_SSID 0
#define NL80211_BAND_5GHZ 1
#define NL80211_BAND_6GHZ 3
#define IEEE80211_BSS_TYPE_ANY 0
#define IEEE80211_PRIVACY_ANY 0
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
#define list_for_each_entry(pos, head, member) \
	for (pos = container_of((head)->next, __typeof__(*pos), member); \
	     &pos->member != (head); \
	     pos = container_of(pos->member.next, __typeof__(*pos), member))
static unsigned long jiffies = 1000;
struct ieee80211_channel { int band; int center_freq; };
struct element { u8 id, datalen; u8 data[IEEE80211_MAX_SSID_LEN]; };
struct cfg80211_bss {
	u8 bssid[ETH_ALEN];
	struct ieee80211_channel *channel;
	struct cfg80211_bss *transmitted_bss;
	struct list_head nontrans_list;
	/* the mock's own */
	struct element ssid;
	bool linked, expired, sticky;
};
struct wiphy { int unused; };
struct centauri_t6050_wifi {
	struct wiphy *wiphy;
	bool connecting, connected, avoid_valid;
	u8 ssid[IEEE80211_MAX_SSID_LEN], bssid[ETH_ALEN];
	size_t ssid_len;
	u8 avoid_ssid[IEEE80211_MAX_SSID_LEN];
	size_t avoid_ssid_len;
	unsigned long avoid_until;
};
static bool ether_addr_equal(const u8 *a, const u8 *b) { return !memcmp(a, b, ETH_ALEN); }
static void ether_addr_copy(u8 *d, const u8 *s) { memcpy(d, s, ETH_ALEN); }

#define MAX_BSS 16
static struct cfg80211_bss bsses[MAX_BSS];
static int n_bss, walks;
static const struct element *ieee80211_bss_get_elem(const struct cfg80211_bss *b, int id)
{
	return id == WLAN_EID_SSID ? &b->ssid : NULL;
}
static void cfg80211_bss_iter(struct wiphy *w, void *chandef,
			      void (*iter)(struct wiphy *, struct cfg80211_bss *, void *), void *data)
{
	walks++;
	for (int i = 0; i < n_bss; i++)
		if (bsses[i].linked)
			iter(w, &bsses[i], data);
}
static struct cfg80211_bss *cfg80211_get_bss(struct wiphy *w, struct ieee80211_channel *ch,
					     const u8 *bssid, const u8 *ssid, size_t ssid_len,
					     int type, int privacy)
{
	for (int i = 0; i < n_bss; i++) {
		struct cfg80211_bss *b = &bsses[i];
		if (b->linked && !b->expired && b->channel == ch && ether_addr_equal(b->bssid, bssid) &&
		    (!ssid || (b->ssid.datalen == ssid_len && !memcmp(b->ssid.data, ssid, ssid_len))))
			return b;
	}
	return NULL;
}
/* As cfg80211: a member takes its whole set down; a sticky entry stays. */
static void unlink_one(struct cfg80211_bss *b) { if (!b->sticky) b->linked = false; }
static void cfg80211_unlink_bss(struct wiphy *w, struct cfg80211_bss *b)
{
	struct cfg80211_bss *set = b->transmitted_bss ? b->transmitted_bss : b;
	for (int i = 0; i < n_bss; i++)
		if (&bsses[i] == set || bsses[i].transmitted_bss == set)
			unlink_one(&bsses[i]);
}
static void cfg80211_put_bss(struct wiphy *w, struct cfg80211_bss *b) { (void)w; (void)b; }
'''

TESTS = r'''
static struct ieee80211_channel c6a = { NL80211_BAND_6GHZ, 6135 }, c6b = { NL80211_BAND_6GHZ, 6455 },
				c5 = { NL80211_BAND_5GHZ, 5180 };
static struct wiphy wiphy;
static struct centauri_t6050_wifi wifi;

static void reset(const char *avoid)
{
	memset(bsses, 0, sizeof(bsses)); n_bss = 0; walks = 0;
	memset(&wifi, 0, sizeof(wifi)); wifi.wiphy = &wiphy;
	wifi.avoid_valid = true; wifi.avoid_until = jiffies + 120;
	wifi.avoid_ssid_len = strlen(avoid); memcpy(wifi.avoid_ssid, avoid, wifi.avoid_ssid_len);
}
/* One entry; tx: its set's transmitting entry, or NULL when it transmits. */
static struct cfg80211_bss *add(u8 last, struct ieee80211_channel *ch, const char *ssid, struct cfg80211_bss *tx)
{
	struct cfg80211_bss *b = &bsses[n_bss++];
	u8 a[ETH_ALEN] = { 2, 0, 0, 0, 0, last };
	memcpy(b->bssid, a, ETH_ALEN); b->channel = ch; b->linked = true;
	b->ssid.datalen = strlen(ssid); memcpy(b->ssid.data, ssid, b->ssid.datalen);
	b->nontrans_list.next = b->nontrans_list.prev = &b->nontrans_list;
	b->transmitted_bss = tx;
	if (tx) {	/* list_add_tail(&b->nontrans_list, &tx->nontrans_list) */
		struct list_head *h = &tx->nontrans_list;
		b->nontrans_list.prev = h->prev; b->nontrans_list.next = h;
		h->prev->next = &b->nontrans_list; h->prev = &b->nontrans_list;
	}
	return b;
}
static int linked(void) { int n = 0; for (int i = 0; i < n_bss; i++) n += bsses[i].linked; return n; }

int main(void)
{
	/* An avoided entry cfg80211 does not return, ahead of two that it does: both go. */
	reset("guest");
	struct cfg80211_bss *x = add(1, &c6a, "guest", NULL), *y = add(2, &c6b, "guest", NULL),
			    *z = add(3, &c6b, "guest", NULL);
	x->expired = true;
	avoid_unlink(&wifi, NULL);
	assert(x->linked && !y->linked && !z->linked);
	/* The same with one that cfg80211 does not unlink. */
	reset("guest");
	x = add(1, &c6a, "guest", NULL); y = add(2, &c6b, "guest", NULL); z = add(3, &c6b, "guest", NULL);
	x->sticky = true;
	avoid_unlink(&wifi, NULL);
	assert(x->linked && !y->linked && !z->linked);
	puts("PASS passed-over entries: each later avoided entry is still unlinked");

	/* Two avoided entries, the first in the joined BSS's set: it stays, the second goes. */
	reset("guest");
	struct cfg80211_bss *t1 = add(0x10, &c6a, "main", NULL), *g1 = add(0x11, &c6a, "guest", t1),
			    *t2 = add(0x20, &c6b, "other", NULL), *g2 = add(0x21, &c6b, "guest", t2),
			    *g5 = add(0x30, &c5, "guest", NULL);
	wifi.connected = true; memcpy(wifi.bssid, t1->bssid, ETH_ALEN);
	memcpy(wifi.ssid, "guest", 5); wifi.ssid_len = 5;
	avoid_unlink(&wifi, wifi.bssid);
	assert(t1->linked && g1->linked && !t2->linked && !g2->linked && g5->linked);
	puts("PASS keep: the joined BSS's set stays, another AP's set goes with its avoided profile, 5 GHz untouched");

	/* The current network's set stays though it holds the avoided profile; others go. */
	reset("guest");
	t1 = add(0x10, &c6a, "main", NULL); g1 = add(0x11, &c6a, "guest", t1);
	t2 = add(0x20, &c6b, "main", NULL); g2 = add(0x21, &c6b, "guest", t2);
	struct cfg80211_bss *t3 = add(0x40, &c6b, "lab", NULL), *g3 = add(0x41, &c6b, "guest", t3);
	wifi.connected = true; memcpy(wifi.bssid, (u8[]){ 2, 0, 0, 0, 0, 0x99 }, ETH_ALEN);
	memcpy(wifi.ssid, "main", 4); wifi.ssid_len = 4;
	avoid_unlink(&wifi, wifi.bssid);
	assert(t1->linked && g1->linked && t2->linked && g2->linked && !t3->linked && !g3->linked);
	/* Not connecting or connected: no current network, all three sets go. */
	reset("guest");
	t1 = add(0x10, &c6a, "main", NULL); g1 = add(0x11, &c6a, "guest", t1);
	t2 = add(0x20, &c6b, "main", NULL); g2 = add(0x21, &c6b, "guest", t2);
	memcpy(wifi.ssid, "main", 4); wifi.ssid_len = 4;
	avoid_unlink(&wifi, NULL);
	assert(!t1->linked && !g1->linked && !t2->linked && !g2->linked);
	/* The avoided network as the current one keeps nothing but the joined BSS's set. */
	reset("guest");
	t1 = add(0x10, &c6a, "guest", NULL); t2 = add(0x20, &c6b, "guest", NULL);
	wifi.connecting = true; memcpy(wifi.bssid, t1->bssid, ETH_ALEN);
	memcpy(wifi.ssid, "guest", 5); wifi.ssid_len = 5;
	avoid_unlink(&wifi, wifi.bssid);
	assert(t1->linked && !t2->linked);
	puts("PASS current network: its sets stay while connecting or connected, unless it is the avoided one");

	/* The window over: nothing is unlinked. */
	reset("guest");
	t1 = add(0x10, &c6a, "guest", NULL);
	jiffies += 1000;
	avoid_unlink(&wifi, NULL);
	assert(t1->linked && !wifi.avoid_valid && linked() == 1);
	puts("PASS window: after it ends nothing is unlinked");
	return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--negative-control", action="store_true",
                    help="put back the loop that re-reads a passed-over entry: the first case must fail")
    args = ap.parse_args()
    source = (Path(args.root) / SRC).read_text()
    parts = [block(source, "bool", "avoided"), block(source, "struct", "avoid_find"),
             block(source, "bool", "avoid_keep_bss"), block(source, "bool", "avoid_keep_set"),
             block(source, "void", "avoid_find_one"), block(source, "void", "avoid_unlink")]
    if args.negative_control:
        fixed = ("\t\tif (find.count < count)\n\t\t\tcontinue;\n"
                 "\t\t/* Not unlinked: pass it over and look up the entry after it. */\n"
                 "\t\tfind.skip++;\n\t\tfind.count = 0;\n"
                 "\t\tcfg80211_bss_iter(wifi->wiphy, NULL, avoid_find_one, &find);\n")
        if fixed not in parts[-1]:
            raise RuntimeError("negative control: the pass-over walk is not in avoid_unlink()")
        parts[-1] = parts[-1].replace(fixed, "\t\tif (find.count >= count)\n\t\t\tfind.skip++;\n")
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "avoid.c"
        c.write_text(PRELUDE + "\n\n".join(parts) + "\n" + TESTS)
        exe = Path(tmp) / "avoid"
        subprocess.run([args.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined", str(c), "-o", str(exe)],
                       check=True)
        rc = subprocess.run([str(exe)], timeout=60).returncode
    if rc:
        print(f"FAIL: the helper broke a case above (exit {rc})")
        sys.exit(1)


if __name__ == "__main__":
    main()
