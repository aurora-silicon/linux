#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host unit tests and negative controls for Centauri Wi-Fi/BT scanning."""
import argparse
import os
from pathlib import Path
import re
import resource
import shlex
import signal
import subprocess
import tempfile


def extract_fn(source, name):
    match = re.search(
        r"^(?:static )?(?:int|void|bool) " + re.escape(name) + r"\([^;]*?\)\n\{",
        source,
        re.M,
    )
    if not match:
        raise RuntimeError(f"missing production function: {name}")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


WIFI_PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n) (1U << (n))
#define BIT_ULL(n) (1ULL << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define CEN_SCAN_CHANNELS_MAX 59
#define SCAN_ACTIVE_MIN_MS 40
#define SCAN_LISTEN_MS 110
#define CHANNEL_PASSIVE BIT(4)
#define CHANNEL_RADAR BIT(6)
#define IEEE80211_CHAN_DISABLED BIT(0)
#define IEEE80211_CHAN_NO_IR BIT(1)
#define IEEE80211_CHAN_RADAR BIT(2)
#define HZ 1000
#define msecs_to_jiffies(x) ((unsigned long)(x))
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define time_after_eq(a, b) (!time_before(a, b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define NL80211_BAND_2GHZ 0
#define NL80211_BAND_5GHZ 1
#define NL80211_BAND_6GHZ 2
#define NL80211_STA_INFO_CONNECTED_TIME 0
#define NL80211_STA_INFO_RX_BYTES64 1
#define NL80211_STA_INFO_TX_BYTES64 2
#define NL80211_STA_INFO_RX_PACKETS 3
#define NL80211_STA_INFO_TX_PACKETS 4
#define NL80211_STA_INFO_SIGNAL 5
#define lockdep_assert_held(l) assert(*(l))
#define dev_warn(...) ((void)0)

struct device { int dummy; };
struct pci_dev { struct device dev; };
struct centauri_t6050 { struct pci_dev *pdev; };
struct ieee80211_channel { int band; u32 center_freq; u32 flags; };
struct cfg80211_ssid { u8 ssid[32]; u8 ssid_len; };
struct cfg80211_scan_request {
	u32 n_channels;
	u32 n_ssids;
	struct cfg80211_ssid *ssids;
	struct ieee80211_channel *channels[8];
};
struct net_device_stats {
	u64 rx_bytes, tx_bytes, rx_packets, tx_packets;
};
struct net_device { struct net_device_stats stats; };
struct station_info {
	u64 filled;
	u32 connected_time;
	u64 rx_bytes, tx_bytes, rx_packets, tx_packets;
	s8 signal;
};
struct centauri_t6050_wifi {
	struct centauri_t6050 *control;
	struct { u8 *bar; } a;
	struct net_device *netdev;
	struct cfg80211_scan_request *scan_request;
	int lock;
	u32 scans_done;
	u16 reply_status, scan_status;
	u8 scan_sequence;
	bool stopping, command_failed, host_sleep_sent, scan_abort;
	s8 station_signal;
	bool station_signal_valid;
	unsigned long station_signal_at, connected_at;
	struct net_device_stats station_start;
};

static unsigned long jiffies;
static unsigned int bar_reads, sleep_calls, link_queries, stop_commands;
static unsigned int stop_event_delay, complete_after_drains, drains_seen;
static unsigned int sleep_step_ms, sleep_sets_host_sleep_at;
static unsigned int sleep_completes_on_drain_at;
static bool stop_refuse_342, stop_refuse_other, sleep_sets_abort;
static bool sleep_sets_host_sleep, sleep_checks_station, station_ran_in_sleep;
static u32 bar_state;
static u8 fake_bar[32];
static struct centauri_t6050_wifi *g_wifi;

static void fill_station(struct centauri_t6050_wifi *wifi,
			 struct station_info *sinfo);

static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static u32 readl(void *addr) { (void)addr; bar_reads++; return bar_state; }
static int firmware_band(int band) { return band; }
static int ieee80211_frequency_to_channel(u32 freq)
{
	return freq == 2412 ? 1 : 36;
}
static void command_refused(struct centauri_t6050_wifi *w, const char *s, int r)
{
	(void)w; (void)s; (void)r;
}
static int query_link_status(struct centauri_t6050_wifi *wifi)
{
	assert(wifi->lock);
	link_queries++;
	wifi->station_signal = -52;
	wifi->station_signal_valid = true;
	wifi->station_signal_at = jiffies;
	return 0;
}
static int command(struct centauri_t6050_wifi *wifi, u32 id,
		   const void *payload, size_t size)
{
	(void)payload; (void)size;
	assert(wifi->lock);
	if (id == 65537) {
		stop_commands++;
		if (stop_refuse_342) {
			wifi->reply_status = 342;
			return -EREMOTEIO;
		}
		if (stop_refuse_other) {
			wifi->reply_status = 500;
			return -EREMOTEIO;
		}
		wifi->reply_status = 0;
		return 0;
	}
	return 0;
}
static void drain(struct centauri_t6050_wifi *wifi)
{
	assert(wifi->lock);
	drains_seen++;
	if (complete_after_drains && drains_seen >= complete_after_drains) {
		wifi->scans_done++;
		complete_after_drains = 0;
	}
	if (stop_commands && stop_event_delay) {
		stop_event_delay--;
		if (!stop_event_delay)
			wifi->scans_done++;
	}
}
static void usleep_range(unsigned long a, unsigned long b)
{
	(void)a; (void)b;
	sleep_calls++;
	jiffies += sleep_step_ms ? sleep_step_ms : 250;
	/* Must not hold wifi->lock across scan wait sleeps. */
	assert(!g_wifi->lock);
	if (sleep_checks_station && !station_ran_in_sleep) {
		struct station_info sinfo = {};

		mutex_lock(&g_wifi->lock);
		fill_station(g_wifi, &sinfo);
		mutex_unlock(&g_wifi->lock);
		assert(link_queries == 0);
		assert(sinfo.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL));
		assert(sinfo.signal == -61);
		station_ran_in_sleep = true;
	}
	if (sleep_sets_abort) {
		g_wifi->scan_abort = true;
		sleep_sets_abort = false;
	}
	if (sleep_sets_host_sleep) {
		g_wifi->host_sleep_sent = true;
		sleep_sets_host_sleep = false;
	}
	if (sleep_sets_host_sleep_at && sleep_calls == sleep_sets_host_sleep_at)
		g_wifi->host_sleep_sent = true;
	if (sleep_completes_on_drain_at &&
	    sleep_calls == sleep_completes_on_drain_at)
		complete_after_drains = drains_seen + 1;
}
'''

WIFI_TESTS = r'''
static struct pci_dev pdev;
static struct centauri_t6050 ctrl = { .pdev = &pdev };
static struct net_device ndev;
static struct centauri_t6050_wifi wifi;
static struct ieee80211_channel ch1 = { .band = NL80211_BAND_2GHZ, .center_freq = 2412 };
static struct cfg80211_scan_request req;

static void reset_wifi(void)
{
	memset(&wifi, 0, sizeof(wifi));
	memset(&ndev, 0, sizeof(ndev));
	memset(&req, 0, sizeof(req));
	wifi.control = &ctrl;
	wifi.a.bar = fake_bar;
	wifi.netdev = &ndev;
	req.n_channels = 1;
	req.channels[0] = &ch1;
	wifi.scan_request = &req;
	wifi.station_signal = -61;
	wifi.station_signal_valid = true;
	wifi.station_signal_at = 0;
	g_wifi = &wifi;
	jiffies = 2000;
	bar_state = 2;
	bar_reads = sleep_calls = link_queries = stop_commands = 0;
	stop_event_delay = complete_after_drains = drains_seen = 0;
	sleep_step_ms = sleep_sets_host_sleep_at = 0;
	sleep_completes_on_drain_at = 0;
	stop_refuse_342 = stop_refuse_other = false;
	sleep_sets_abort = sleep_sets_host_sleep = false;
	sleep_checks_station = station_ran_in_sleep = false;
}

int main(void)
{
	/* 1. Normal scan unlocks during sleep; concurrent fill_station skips FW cmd 20. */
	reset_wifi();
	complete_after_drains = 3;
	sleep_checks_station = true;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == 0);
	mutex_unlock(&wifi.lock);
	assert(sleep_calls >= 1 && station_ran_in_sleep && link_queries == 0);

	/* Outside a scan, fill_station refreshes link status normally. */
	wifi.scan_request = NULL;
	{
		struct station_info sinfo = {};

		mutex_lock(&wifi.lock);
		fill_station(&wifi, &sinfo);
		mutex_unlock(&wifi.lock);
		assert(link_queries == 1 && sinfo.signal == -52);
	}

	/* 2. Scan abort sends 65537 and waits for 0x80000005 completion. */
	reset_wifi();
	sleep_sets_abort = true;
	stop_event_delay = 2;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == -ECANCELED);
	mutex_unlock(&wifi.lock);
	assert(stop_commands == 1 && wifi.scans_done == 1);

	/* 3. Scan abort with firmware status 342 (already ended) returns cleanly. */
	reset_wifi();
	wifi.scan_abort = true;
	stop_refuse_342 = true;
	mutex_lock(&wifi.lock);
	assert(scan_stop_chunk(&wifi, 0) == 0);
	mutex_unlock(&wifi.lock);
	assert(stop_commands == 1);

	/* 4. Scan timeout stops chunk and waits for 0x80000005 before returning. */
	reset_wifi();
	stop_event_delay = 2;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == -ETIMEDOUT);
	mutex_unlock(&wifi.lock);
	assert(stop_commands == 1 && wifi.scans_done == 1);

	/* 5. Host sleep entered while unlocked aborts without BAR read or stop cmd. */
	reset_wifi();
	sleep_sets_host_sleep = true;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == -EIO);
	mutex_unlock(&wifi.lock);
	assert(bar_reads == 2 && stop_commands == 0);

	/* 6. scan_stop_chunk() entry guard refuses to ring doorbell when stopped. */
	reset_wifi();
	wifi.host_sleep_sent = true;
	mutex_lock(&wifi.lock);
	assert(scan_stop_chunk(&wifi, 0) == -EIO);
	mutex_unlock(&wifi.lock);
	assert(stop_commands == 0 && bar_reads == 0);

	/* 7. Host sleep set during the final sleep as deadline expires skips 65537. */
	reset_wifi();
	sleep_step_ms = 9000;
	sleep_sets_host_sleep_at = 1;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == -EIO);
	mutex_unlock(&wifi.lock);
	assert(sleep_calls == 1 && stop_commands == 0);

	/* 8. Completion arriving during final sleep as deadline expires succeeds cleanly. */
	reset_wifi();
	sleep_step_ms = 9000;
	sleep_completes_on_drain_at = 1;
	mutex_lock(&wifi.lock);
	assert(scan_profile27(&wifi) == 0);
	mutex_unlock(&wifi.lock);
	assert(sleep_calls == 1 && stop_commands == 0 && wifi.scans_done == 1);

	puts("PASS: Centauri T6050 Wi-Fi scan lock-release and stop synchronization");
	return 0;
}
'''

SHARED_WIFI_PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define SCAN_PASSIVE_DWELL_MS 110
#define SCAN_LISTEN_MS 110
#define SCAN_STATUS_INVALID_HANDLE 342
#define msecs_to_jiffies(x) ((unsigned long)(x))
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define time_after(a, b) time_before(b, a)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(type, x, y) ((type)(x) < (type)(y) ? (type)(x) : (type)(y))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
static inline void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static void dev_dbg(void *d, const char *fmt, ...) { (void)d; (void)fmt; }

struct centauri_board { bool scan_tlvs; };
static const struct centauri_board *centauri_board(void) { return NULL; }

struct device { int dummy; };
struct pci_dev { struct device dev; };
struct centauri { struct pci_dev *pdev; };
struct work_struct { int dummy; };
struct cfg80211_scan_request { int dummy; };
struct cfg80211_scan_info { bool aborted; };
struct centauri_wifi {
	struct centauri *control;
	struct { u8 *bar; } a;
	struct work_struct scan_work;
	struct cfg80211_scan_request *scan_request;
	bool scan_abort;
	int lock;
	u8 scan_sequence;
	u16 reply_status;
	u32 channels_seen, scans_done, beacons_seen;
	bool stopping, transport_paused;
};
static int run_scan_tlvs(struct centauri_wifi *w, struct cfg80211_scan_request *r)
{
	(void)w; (void)r;
	return 0;
}

static unsigned long jiffies;
static unsigned int scan_commands, stop_commands, sleep_calls, drains_seen;
static unsigned int sleep_step_ms, sleep_sets_paused_at, sleep_sets_abort_at;
static unsigned int complete_on_stop_drain, channels_per_drain;
static bool stop_never_completes, last_scan_aborted, scan_done_called;
static bool pause_between_chunks;
static u32 bar_state;
static u8 fake_bar[32];
static struct centauri_wifi *g_swifi;

static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static u32 readl(void *addr) { (void)addr; return bar_state; }
static unsigned int recovery_requests;
static void centauri_recovery_request(struct centauri *c, const char *why)
{
	(void)c; (void)why; recovery_requests++;
}
static void cfg80211_scan_done(struct cfg80211_scan_request *r,
			       struct cfg80211_scan_info *info)
{
	(void)r;
	scan_done_called = true;
	last_scan_aborted = info->aborted;
}
static int command(struct centauri_wifi *wifi, u32 id,
		   const void *payload, size_t size)
{
	(void)payload; (void)size;
	assert(wifi->lock);
	if (id == 65551) {
		scan_commands++;
		return 0;
	}
	if (id == 65537) {
		stop_commands++;
		wifi->reply_status = 0;
		return 0;
	}
	return 0;
}
static void drain(struct centauri_wifi *wifi)
{
	assert(wifi->lock);
	drains_seen++;
	if (channels_per_drain)
		wifi->channels_seen += channels_per_drain;
	if (stop_commands && !stop_never_completes && complete_on_stop_drain) {
		complete_on_stop_drain--;
		if (!complete_on_stop_drain) {
			wifi->scans_done++;
			if (pause_between_chunks)
				wifi->transport_paused = true;
		}
	}
}
static void usleep_range(unsigned long a, unsigned long b)
{
	(void)a; (void)b;
	sleep_calls++;
	jiffies += sleep_step_ms ? sleep_step_ms : 250;
	assert(!g_swifi->lock);
	if (sleep_sets_paused_at && sleep_calls == sleep_sets_paused_at)
		g_swifi->transport_paused = true;
	if (sleep_sets_abort_at && sleep_calls == sleep_sets_abort_at)
		g_swifi->scan_abort = true;
}
'''

SHARED_WIFI_TESTS = r'''
static struct pci_dev spdev;
static struct centauri sctrl = { .pdev = &spdev };
static struct cfg80211_scan_request sreq;
static struct centauri_wifi swifi;

static void reset_swifi(void)
{
	memset(&swifi, 0, sizeof(swifi));
	swifi.control = &sctrl;
	swifi.a.bar = fake_bar;
	swifi.scan_request = &sreq;
	g_swifi = &swifi;
	jiffies = 2000;
	bar_state = 2;
	scan_commands = stop_commands = sleep_calls = drains_seen = 0;
	sleep_step_ms = sleep_sets_paused_at = sleep_sets_abort_at = 0;
	complete_on_stop_drain = channels_per_drain = 0;
	stop_never_completes = last_scan_aborted = scan_done_called = false;
	pause_between_chunks = false;
}

int main(void)
{
	/* 1. Entry guard in scan_stop_chunk() skips command(65537) when paused. */
	reset_swifi();
	swifi.transport_paused = true;
	mutex_lock(&swifi.lock);
	assert(scan_stop_chunk(&swifi, 0) == -EIO);
	mutex_unlock(&swifi.lock);
	assert(stop_commands == 0);

	/* 2. Transport paused during final sleep at deadline skips 65537 and next chunk. */
	reset_swifi();
	sleep_step_ms = 9000;
	sleep_sets_paused_at = 1;
	scan_worker(&swifi.scan_work);
	assert(scan_done_called && last_scan_aborted);
	assert(scan_commands == 1 && stop_commands == 0);

	/* 3. Timeout whose stop times out propagates error and does NOT start chunk 1. */
	reset_swifi();
	sleep_step_ms = 9000;
	stop_never_completes = true;
	scan_worker(&swifi.scan_work);
	assert(scan_commands == 1 && stop_commands == 1);
	assert(scan_done_called && last_scan_aborted);

	/* 4. Transport paused between chunks stops before starting chunk 1. */
	reset_swifi();
	sleep_step_ms = 9000;
	complete_on_stop_drain = 1;
	pause_between_chunks = true;
	scan_worker(&swifi.scan_work);
	assert(scan_done_called && last_scan_aborted);
	assert(scan_commands == 1 && stop_commands == 1);

	/* 5. Abort arriving after 65537 already sent (stopped=true) still waits for 0x05. */
	reset_swifi();
	channels_per_drain = 13;
	sleep_sets_abort_at = 1;
	complete_on_stop_drain = 3;
	scan_worker(&swifi.scan_work);
	assert(scan_done_called && last_scan_aborted);
	assert(scan_commands == 1 && stop_commands == 1 && swifi.scans_done == 1);

	puts("PASS: Centauri shared Wi-Fi scan stop/abort/chunk state checks");
	return 0;
}
'''

BT_PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define GFP_KERNEL 0
#define CEN_RX 0x8000
#define CEN_TX 0xa000
#define CEN_FOOTER 260
#define CEN_STRIDE (16 + CEN_FOOTER)
#define CEN_RX_SLOTS 17
#define CEN_TX_SLOTS 5
#define CEN_LE_RX 0xc000
#define CEN_LE_TX 0xe000
#define CEN_LE_FOOTER 256
#define CEN_LE_STRIDE (16 + CEN_LE_FOOTER)
#define CEN_LE_SLOTS 17
#define HCI_EVENT_HDR_SIZE 2U
#define HCI_EVENT_PKT 4
#define HCI_EV_CMD_STATUS 0x0f
#define HCI_EV_CONN_COMPLETE 0x03
#define HCI_OP_CREATE_CONN 0x0405
#define CEN_OP_CREATE_CONN 0xff7b
#define ACL_LINK 1
#define dma_rmb() ((void)0)
#define dev_err(...) ((void)0)
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

struct hci_ev_conn_complete { u8 dummy[11]; };
struct cen_aux_spec { u16 id, slots, footer; bool receive; };
static const struct cen_aux_spec cen_aux_specs[] = {
	{ 2, 9, 2592, false }, { 3, 9, 724, false }, { 5, 9, 260, false },
	{ 8, 9, 2592, true },  { 9, 9, 724, true },  { 11, 9, 268, true },
};
struct cen_aux_state { u32 offset; u16 head, post; bool opened; };
struct work_struct { int dummy; };
struct sk_buff_head { unsigned int len; };
#define HCI_EV_DISCONN_COMPLETE 0x05
struct hci_ev_disconn_complete { u8 raw[4]; };
struct sk_buff { u8 data[64]; u32 len; u8 pkt_type; };
struct pci_dev { int dev; };
struct hci_dev { struct { u32 byte_rx, err_rx; } stat; };
struct cen_bt {
	struct pci_dev *pdev;
	struct hci_dev *hdev;
	u8 *bar;
	u8 *arena;
	int lock;
	struct work_struct io_work;
	struct sk_buff_head txq, acl_txq, le_txq, sco_txq, iso_txq;
	u16 acl_tx_head, le_tx_head, sco_tx_head, iso_tx_head;
	u16 tx_head, rx_head, rx_post;
	u16 le_rx_head, le_rx_post;
	struct cen_aux_state aux[ARRAY_SIZE(cen_aux_specs)];
	bool running, transport_paused;
	bool connection_command_supported, connection_status_pending;
	u16 sync_pending_wire, sync_pending_orig;
};

static u16 mock_tail[16];
static bool advance_rx_tail_on_post, advance_tx_tail_after_tx;
static unsigned int rx_frames, reschedules;
static struct sk_buff fake_skb;

static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l); *l = 0; }
static u32 readl(void *p) { (void)p; return 2; }
static u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static bool skb_queue_empty(const struct sk_buff_head *q) { return q->len == 0; }
static void schedule_work(struct work_struct *w) { (void)w; reschedules++; }
static u16 cen_tail(struct cen_bt *bt, u16 ring) { (void)bt; return mock_tail[ring]; }
static int cen_aux_drain(struct cen_bt *bt, bool repost) { (void)bt; (void)repost; return 0; }
static int cen_receive_acl(struct cen_bt *bt, const u8 *d, u32 l) { (void)bt; (void)d; (void)l; return 0; }
static void cen_le_rx_buffer(struct cen_bt *bt, u16 slot) { (void)bt; (void)slot; }
static void cen_rx_buffer(struct cen_bt *bt, u16 slot) { (void)bt; (void)slot; }
static void cen_fixup_scan_response(u8 *e, size_t l) { (void)e; (void)l; }
static void cen_hrb_track_add(struct cen_bt *bt, u16 h, const u8 a[6]) { (void)bt; (void)h; (void)a; }
static void cen_hrb_track_del(struct cen_bt *bt, u16 h) { (void)bt; (void)h; }
static struct sk_buff *bt_skb_alloc(u32 len, int flags)
{
	(void)flags;
	assert(len <= sizeof(fake_skb.data));
	fake_skb.len = 0;
	return &fake_skb;
}
static void skb_put_data(struct sk_buff *skb, const void *d, u32 len)
{
	memcpy(skb->data, d, len);
	skb->len = len;
}
#define hci_skb_pkt_type(skb) ((skb)->pkt_type)
static void hci_recv_frame(struct hci_dev *h, struct sk_buff *s) { (void)h; (void)s; rx_frames++; }
static void cen_post(struct cen_bt *bt, u16 ring, u16 val)
{
	(void)bt; (void)val;
	if (ring == 7 && advance_rx_tail_on_post) {
		advance_rx_tail_on_post = false;
		mock_tail[7] = (mock_tail[7] + 1) % CEN_RX_SLOTS;
	}
}
static int cen_transmit(struct cen_bt *bt, struct sk_buff_head *q, u16 ring,
			u16 slots, u32 off, u16 footer, u16 *head)
{
	(void)bt; (void)off; (void)footer;
	if (ring == 1 && advance_tx_tail_after_tx && q->len) {
		/* Simulate ring momentarily full during transmit, then freed right after. */
		*head = (mock_tail[1] + slots - 1) % slots;
		mock_tail[1] = (mock_tail[1] + 1) % slots;
		advance_tx_tail_after_tx = false;
	}
	return 0;
}
'''

BT_TESTS = r'''
static u8 arena[0x10000];
static u8 fake_bar[32];
static struct pci_dev pdev;
static struct hci_dev hdev;
static struct cen_bt bt;

static void reset_bt(void)
{
	memset(&bt, 0, sizeof(bt));
	memset(arena, 0, sizeof(arena));
	memset(mock_tail, 0, sizeof(mock_tail));
	bt.pdev = &pdev;
	bt.hdev = &hdev;
	bt.bar = fake_bar;
	bt.arena = arena;
	bt.running = true;
	bt.rx_post = CEN_RX_SLOTS - 1;
	bt.le_rx_post = CEN_LE_SLOTS - 1;
	advance_rx_tail_on_post = advance_tx_tail_after_tx = false;
	rx_frames = reschedules = 0;
}

static void put_rx_event(u16 slot)
{
	u8 *d = arena + CEN_RX + slot * CEN_STRIDE;
	u32 hdr = 1 | (4U << 8);

	memcpy(d, &hdr, 4);
	d[16] = 0x3e;
	d[17] = 2;
	d[18] = 0x0d;
	d[19] = 0;
}

int main(void)
{
	/* 1. Coalesced RX arrival during drain triggers io_work reschedule. */
	reset_bt();
	put_rx_event(0);
	put_rx_event(1);
	mock_tail[7] = 1;
	advance_rx_tail_on_post = true;
	cen_io_work(&bt.io_work);
	assert(rx_frames == 1 && bt.rx_head == 1 && reschedules == 1);

	/* Second pass drains remaining event and stays idle (no spin). */
	reschedules = 0;
	cen_io_work(&bt.io_work);
	assert(rx_frames == 2 && bt.rx_head == 2 && reschedules == 0);

	/* 2. Pending TX queue with newly freed TX ring slot reschedules io_work. */
	reset_bt();
	bt.txq.len = 1;
	advance_tx_tail_after_tx = true;
	cen_io_work(&bt.io_work);
	assert(reschedules == 1);

	/* 3. Idle rings do not reschedule. */
	reset_bt();
	cen_io_work(&bt.io_work);
	assert(reschedules == 0);

	puts("PASS: Centauri Bluetooth RX/TX ring drain reschedule");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--source-root", type=Path)
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[2]
    wifi_src = (
        root / "drivers/net/wireless/apple/centauri/t6050/centauri_wifi.c"
    ).read_text()
    shared_src = (
        root / "drivers/net/wireless/apple/centauri/centauri_wifi.c"
    ).read_text()
    bt_src = (root / "drivers/bluetooth/btcentauri_t6050.c").read_text()

    wifi_prod = "\n\n".join(
        extract_fn(wifi_src, name)
        for name in (
            "scan_transport_stopped",
            "scan_stop_chunk",
            "scan_profile27",
            "fill_station",
        )
    )
    shared_prod = "\n\n".join(
        extract_fn(shared_src, name)
        for name in (
            "scan_transport_stopped",
            "scan_wait_stop",
            "scan_stop_chunk",
            "run_scan_legacy",
            "scan_worker",
        )
    )
    bt_prod = "\n\n".join(
        extract_fn(bt_src, name)
        for name in ("cen_rx_pending", "cen_tx_ready", "cen_io_work")
    )

    wifi_variants = [("wifi-production", WIFI_PRELUDE + wifi_prod + WIFI_TESTS, 0, None)]
    shared_variants = [
        ("shared-wifi-production", SHARED_WIFI_PRELUDE + shared_prod + SHARED_WIFI_TESTS, 0, None)
    ]
    bt_variants = [("bt-production", BT_PRELUDE + bt_prod + BT_TESTS, 0, None)]

    if args.negative_controls:
        wifi_controls = [
            (
                "scan-holds-lock-in-sleep",
                "\t\t\tmutex_unlock(&wifi->lock);\n\t\t\tusleep_range(1000, 2000);\n\t\t\tmutex_lock(&wifi->lock);",
                "\t\t\tusleep_range(1000, 2000);",
                "!g_wifi->lock",
            ),
            (
                "station-queries-fw-during-scan",
                "if (!wifi->scan_request &&\n\t    time_after_eq(jiffies, wifi->station_signal_at + HZ))",
                "if (time_after_eq(jiffies, wifi->station_signal_at + HZ))",
                "link_queries == 0",
            ),
            (
                "timeout-skips-stop-wait",
                "\t\t\tret = scan_stop_chunk(wifi, done);\n\t\t\treturn ret == -EIO ? -EIO : -ETIMEDOUT;",
                "\t\t\tcommand(wifi, 65537, NULL, 0);\n\t\t\treturn -ETIMEDOUT;",
                "wifi.scans_done == 1",
            ),
            (
                "abort-ignores-342",
                "if (ret == -EREMOTEIO && wifi->reply_status == 342)\n\t\treturn 0;",
                "if (false && ret == -EREMOTEIO && wifi->reply_status == 342)\n\t\treturn 0;",
                "scan_stop_chunk(&wifi, 0) == 0",
            ),
            (
                "t6050-stop-skips-entry-guard",
                "\tlockdep_assert_held(&wifi->lock);\n\tif (scan_transport_stopped(wifi))\n\t\treturn -EIO;",
                "\tlockdep_assert_held(&wifi->lock);",
                "stop_commands == 0",
            ),
        ]
        for name, old, new, expected_msg in wifi_controls:
            if old not in wifi_prod:
                raise RuntimeError(f"missing pattern for {name}")
            wifi_variants.append(
                (name, WIFI_PRELUDE + wifi_prod.replace(old, new, 1) + WIFI_TESTS,
                 -signal.SIGABRT, expected_msg)
            )

        shared_controls = [
            (
                "shared-timeout-drops-stop-ret",
                "\t\t\t\t\tret = stopped ? -ETIMEDOUT :\n\t\t\t\t\t      scan_stop_chunk(wifi, done);",
                "\t\t\t\t\tif (!stopped)\n\t\t\t\t\t\tscan_stop_chunk(wifi, done);",
                "scan_commands == 1",
            ),
            (
                "shared-abort-skips-inflight-stop-wait",
                "\t\t\t\tint stop_ret = stopped ? scan_wait_stop(wifi, done) :\n\t\t\t\t\t       scan_stop_chunk(wifi, done);",
                "\t\t\t\tint stop_ret = stopped ? 0 :\n\t\t\t\t\t       scan_stop_chunk(wifi, done);",
                "swifi.scans_done == 1",
            ),
        ]
        for name, old, new, expected_msg in shared_controls:
            if old not in shared_prod:
                raise RuntimeError(f"missing pattern for {name}")
            shared_variants.append(
                (name, SHARED_WIFI_PRELUDE + shared_prod.replace(old, new, 1) + SHARED_WIFI_TESTS,
                 -signal.SIGABRT, expected_msg)
            )

        bt_controls = [
            (
                "bt-drops-rx-reschedule",
                "if (cen_rx_pending(bt) ||",
                "if ((false && cen_rx_pending(bt)) ||",
                "reschedules == 1",
            ),
            (
                "bt-drops-tx-reschedule",
                "cen_tx_ready(bt, &bt->txq, 1, CEN_TX_SLOTS, bt->tx_head) ||",
                "(false && cen_tx_ready(bt, &bt->txq, 1, CEN_TX_SLOTS, bt->tx_head)) ||",
                "reschedules == 1",
            ),
        ]
        for name, old, new, expected_msg in bt_controls:
            if old not in bt_prod:
                raise RuntimeError(f"missing pattern for {name}")
            bt_variants.append(
                (name, BT_PRELUDE + bt_prod.replace(old, new, 1) + BT_TESTS,
                 -signal.SIGABRT, expected_msg)
            )

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="centauri-scan-test-") as directory:
        out = args.out or Path(directory)
        out.mkdir(parents=True, exist_ok=True)
        for name, code, expected, msg in wifi_variants + shared_variants + bt_variants:
            harness, binary = out / (name + ".c"), out / name
            harness.write_text(code)
            subprocess.run(
                compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(harness), "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            (out / (name + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode != expected or (
                expected and ("Assertion" not in result.stderr or msg not in result.stderr)
            ):
                raise RuntimeError(
                    f"{name}: exit {result.returncode}, expected {expected}\n"
                    + result.stdout + result.stderr
                )
            print(f"{name}: {'PASS' if expected == 0 else 'correctly rejected'}")


if __name__ == "__main__":
    main()
