#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the checkout's Wi-Fi disconnect/PM callbacks with mocked transport.

This checks error propagation and command receipts, not firmware sleep semantics.
Run in a bounded host scope; no kernel build or hardware is needed.
"""
import argparse
import os
from pathlib import Path
import re
import resource
import signal
import shlex
import subprocess
import tempfile


def block(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|void) " + re.escape(name) + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise RuntimeError(f"Production function {name} missing")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define GFP_KERNEL 0
#define BUFFER_SIZE 256
#define DATA_TX_SLOTS 8
#define CEN_HOST_EVENTS_LEGACY 85
#define CEN_HOST_EVENTS_FULL 91
#define TX_BUFFER 2048
#define TR_BASE 0
#define dma_rmb() ((void)0)
#define dma_wmb() ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define lockdep_assert_held(l) assert(*(l))
#define msecs_to_jiffies(x) (x)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define time_after_eq(a, b) (!time_before(a, b))
static unsigned long jiffies;
struct centauri_t6050_wifi;
struct pci_dev { int dev; };
struct centauri_t6050 { struct pci_dev *pdev; struct centauri_t6050_wifi *wifi; u8 *bar; bool removing; };
struct wiphy { struct centauri_t6050_wifi *wifi; };
struct net_device { int unused; };
struct cfg80211_wowlan { bool any; };
struct regulatory_request { char alpha2[3]; };
'''

STUBS = r'''
struct centauri_t6050_wifi {
    struct centauri_t6050 *control;
    struct { u8 *window, *bar; uint64_t window_dma; } a;
    int lock, connect_work, link_work, data_tx_queue;
    bool command_failed, stopping, connect_abort, disconnect_pending, connecting, connected;
    bool reply_valid, pm_prepared, pm_wowlan_armed, data_failed, irq_ready;
    bool last_fw_event_valid;
    bool data_tx_busy[DATA_TX_SLOTS];
    bool host_events_fallback_logged;
    bool configured, tx_paused, transport_paused, host_sleep_sent, transport_sleep_sent;
    bool regulatory_deferred, station_signal_valid, disconnect_event;
    u8 country_deferred[3], hw_channels[4];
    int hw_channels_blob, data_rings_open;
    struct net_device *netdev;
    u16 command_head, sequence, reply_capture, reply_sequence, reply_status;
    u32 reply_command;
    u8 *reply_buffer;
    size_t reply_capacity, reply_size;
    struct centauri_wifi_disconnect_receipt pm_disconnect;
};
static const u16 ring_size[5] = {0, 64};
static unsigned int doorbells, drains, notifications, consumer, tx_drain_rounds, info_logs;
static unsigned int reads, index_reads, reports, applies;
static bool rtnl_held, fail_firmware_report;
#define ASSERT_RTNL() assert(rtnl_held)
static void dev_info(const void *dev, const char *fmt, ...)
{ (void)dev; (void)fmt; info_logs++; }
static u16 firmware_status, last_cmd6_tlv280_len, mock_fw_transient_status;
static u32 mock_reply_command = 131073;
static u8 mock_fw_max_events, last_cmd6_tlv280_count;
static bool async_reply, async_event, wrong_reply;
static bool *submission_probe;
static bool private_access_forbidden;
enum { REPLY, TIMEOUT, LOST };
static int transport_mode;
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void *wiphy_priv(struct wiphy *wiphy)
{ assert(!private_access_forbidden); return &wiphy->wifi; }
static void usleep_range(int a, int b) { (void)a; (void)b; jiffies += 100; }
static unsigned int readl(void *address)
{ (void)address; reads++; return transport_mode == LOST ? 0 : 2; }
static void writel(unsigned int value, void *address) { (void)value; (void)address; doorbells++; }
static u16 index_read(struct centauri_t6050_wifi *wifi, int array, int index)
{ assert(wifi->lock); (void)array; (void)index; index_reads++; return consumer; }
static void index_write(struct centauri_t6050_wifi *wifi, int array, int index, u16 value)
{ assert(wifi->lock); (void)array; (void)index; (void)value;
  if (submission_probe) assert(*submission_probe); }
static void put_unaligned_le16(u16 value, void *p) { memcpy(p, &value, 2); }
static void put_unaligned_le32(u32 value, void *p) { memcpy(p, &value, 4); }
static void put_unaligned_le64(uint64_t value, void *p) { memcpy(p, &value, 8); }
static unsigned int recovery_requests;
#define smp_load_acquire(p) (*(p))
static void log_transport_fault(struct centauri_t6050_wifi *wifi, u32 id,
                                u32 stage, const char *reason)
{ (void)wifi; (void)id; (void)stage; (void)reason; }
static void centauri_t6050_request_transport_recover(struct centauri_t6050 *control)
{ (void)control; recovery_requests++; }
static void command_fault(struct centauri_t6050_wifi *wifi, u32 id,
                          u32 stage, const char *reason);
static void append_tlv(u8 *buffer, size_t *size, u16 tag, const void *value, size_t len)
{
    put_unaligned_le16(tag, buffer + *size);
    put_unaligned_le16(len, buffer + *size + 2);
    memcpy(buffer + *size + 4, value, len);
    *size += 4 + len;
    if (tag == 280) {
        last_cmd6_tlv280_len = len;
        last_cmd6_tlv280_count = len ? ((const u8 *)value)[0] : 0;
    }
}
static int command_reply(struct centauri_t6050_wifi *wifi, u32 id,
                         const void *payload, size_t size, void *reply,
                         size_t *reply_size, bool *submitted);
static int command(struct centauri_t6050_wifi *wifi, u32 id, const void *payload,
                   size_t size)
{
    return command_reply(wifi, id, payload, size, NULL, NULL, NULL);
}
static void drain(struct centauri_t6050_wifi *wifi)
{
    assert(wifi->lock);
    drains++;
    if (tx_drain_rounds) {
        tx_drain_rounds--;
        if (!tx_drain_rounds)
            memset(wifi->data_tx_busy, 0, sizeof(wifi->data_tx_busy));
    }
    if (transport_mode != REPLY)
        return;
    if (!wifi->reply_valid) {
        wifi->reply_valid = true;
        wifi->reply_sequence = wifi->reply_capture + wrong_reply;
        const u8 *packet = wifi->a.window + TX_BUFFER +
                           ((wifi->command_head + ring_size[1] - 1) % ring_size[1]) * BUFFER_SIZE;
        u32 command_id;
        memcpy(&command_id, packet + 0x14, sizeof(command_id));
        wifi->reply_command = (mock_reply_command ? mock_reply_command : command_id) |
                              (async_reply ? BIT(30) : 0);
        if (mock_reply_command == 6 && mock_fw_transient_status) {
            wifi->reply_status = mock_fw_transient_status;
            mock_fw_transient_status = 0;
        } else if (mock_reply_command == 6 && mock_fw_max_events &&
                   (last_cmd6_tlv280_len > 1 + mock_fw_max_events ||
                    last_cmd6_tlv280_count > mock_fw_max_events)) {
            wifi->reply_status = 1;
        } else {
            wifi->reply_status = firmware_status;
        }
    } else if (async_event) {
        wifi->disconnect_pending = false;
    }
}
static void cancel_scan(struct centauri_t6050_wifi *wifi) { assert(!wifi->lock); }
static void cancel_work_sync(int *work) { (void)work; }
static void cancel_connect_locked(struct centauri_t6050_wifi *wifi) { assert(wifi->lock); }
static void netif_stop_queue(struct net_device *netdev) { (void)netdev; }
static void netif_carrier_off(struct net_device *netdev) { (void)netdev; }
static void skb_queue_purge(int *queue) { (void)queue; }
static void cfg80211_disconnected(struct net_device *netdev, u16 reason, void *ie,
                                  int size, bool local, int flags)
{ (void)netdev; (void)reason; (void)ie; (void)size; (void)local; (void)flags; notifications++; }
static int alpha_transport_state(struct centauri_t6050_wifi *wifi, bool asleep)
{ assert(wifi->lock && !asleep); return 0; }
static bool netif_running(struct net_device *netdev) { (void)netdev; return false; }
static void netif_wake_queue(struct net_device *netdev) { (void)netdev; }
static void schedule_work(int *work) { (void)work; }
static void firmware_report(struct centauri_t6050_wifi *wifi, u32 id, u8 *buffer, int *blob)
{
    assert(wifi->lock && rtnl_held && id == 19);
    assert(buffer == wifi->hw_channels && blob == &wifi->hw_channels_blob);
    reports++;
    if (fail_firmware_report)
        command_fault(wifi, id, 0, "firmware report");
}
static void channels_apply(struct centauri_t6050_wifi *wifi)
{ assert(wifi->lock && rtnl_held); applies++; }
'''

TESTS = r'''
static struct pci_dev pdev;
static struct centauri_t6050 control;
static struct centauri_t6050_wifi wifi;
static struct wiphy wiphy;
static struct net_device netdev;
static u8 window[32768], bar[8192], control_bar[0x8004];
static void reset(void)
{
    memset(&wifi, 0, sizeof(wifi));
    memset(window, 0, sizeof(window));
    control = (struct centauri_t6050) { .pdev = &pdev, .wifi = &wifi, .bar = control_bar };
    wifi.control = &control;
    wifi.a.window = window;
    wifi.a.bar = bar;
    wifi.connected = true;
    wiphy.wifi = &wifi;
    doorbells = drains = notifications = consumer = tx_drain_rounds = info_logs = 0;
    reads = index_reads = reports = applies = 0;
    rtnl_held = fail_firmware_report = false;
    firmware_status = last_cmd6_tlv280_len = mock_fw_transient_status = 0;
    mock_reply_command = 131073;
    mock_fw_max_events = last_cmd6_tlv280_count = 0;
    async_reply = async_event = wrong_reply = false;
    transport_mode = REPLY;
    jiffies = 0;
    submission_probe = NULL;
    private_access_forbidden = false;
}
static void prepare(void) { reset(); centauri_t6050_wifi_pm_prepare(&control); }
static int disconnect(void) { return wifi_disconnect(&wiphy, &netdev, 3); }
static void expect_error(int expected)
{
    unsigned int before = doorbells;
    assert(wifi.pm_disconnect.error == expected);
    assert(centauri_t6050_wifi_pm_error(&control) == expected);
    assert(call_registered_suspend(&wiphy, NULL) == expected);
    assert(doorbells == before); /* Suspend/error lookup cannot issue a retry. */
    assert(!wifi.lock);
}

static void test_registered_pm_contract(void)
{
    cfg80211_registration_check(&registered_pm_ops);
    assert(registration_warnings == 0);
    for (unsigned retired = 0; retired < 2; retired++) {
        reset();
        wifi.pm_prepared = true;
        wifi.pm_disconnect.error = -EREMOTEIO;
        if (retired) {
            control.wifi = NULL;
            wiphy.wifi = (struct centauri_t6050_wifi *)(uintptr_t)1;
        }
        struct centauri_t6050_wifi snapshot = wifi;
        unsigned before_doorbells = doorbells, before_drains = drains;
        private_access_forbidden = true;
        assert(call_registered_resume(&wiphy) == 0);
        private_access_forbidden = false;
        assert(doorbells == before_doorbells && drains == before_drains);
        assert(memcmp(&snapshot, &wifi, sizeof(wifi)) == 0);
    }
}

static void test_host_sleep_gate(void)
{
    bool published = true;

    reset();
    wifi.host_sleep_sent = true;
    mutex_lock(&wifi.lock);
    assert(command_reply(&wifi, 9, NULL, 0, NULL, NULL, &published) == -EHOSTDOWN);
    mutex_unlock(&wifi.lock);
    assert(!published && !index_reads && !doorbells && !drains && !reads);
    assert(!wifi.sequence && !wifi.command_failed);
}

static void test_deferred_regulatory(void)
{
    struct regulatory_request us = { .alpha2 = "US" };
    struct regulatory_request ca = { .alpha2 = "CA" };
    struct regulatory_request world = { .alpha2 = "00" };

    reset();
    wifi.configured = true;
    wifi.host_sleep_sent = wifi.transport_sleep_sent = true;
    mock_reply_command = 0;
    rtnl_held = true;
    wifi_regulatory(&wiphy, &world);
    wifi_regulatory(&wiphy, &ca);
    wifi_regulatory(&wiphy, &us);
    wifi_regulatory(&wiphy, &world);
    assert(!index_reads && !doorbells && !drains && !reads && !reports && !applies && !wifi.lock);
    assert(wifi.regulatory_deferred && !memcmp(wifi.country_deferred, "US\1", 3));

    mutex_lock(&wifi.lock);
    firmware_status = 1;
    assert(alpha_resume_locked(&wifi) == -EREMOTEIO);
    assert(wifi.host_sleep_sent && wifi.regulatory_deferred && !reports && !applies);
    doorbells = 0;
    firmware_status = 0;
    assert(alpha_resume_locked(&wifi) == 0);
    assert(!wifi.host_sleep_sent && !wifi.regulatory_deferred && !wifi.country_deferred[0]);
    assert(doorbells == 2 && reports == 1 && applies == 1);
    assert(!memcmp(wifi.a.window + TX_BUFFER + (wifi.command_head - 1) * BUFFER_SIZE + 24,
                   "US\1", 3));
    assert(alpha_resume_locked(&wifi) == 0);
    assert(doorbells == 2 && reports == 1 && applies == 1);
    mutex_unlock(&wifi.lock);

    wifi_regulatory(&wiphy, &ca);
    assert(doorbells == 3 && reports == 2 && applies == 2);
    wifi_regulatory(&wiphy, &world);
    assert(doorbells == 3 && reports == 2 && applies == 3);

    wifi.host_sleep_sent = true;
    wifi_regulatory(&wiphy, &world);
    assert(wifi.regulatory_deferred && !wifi.country_deferred[0]);
    mutex_lock(&wifi.lock);
    assert(alpha_resume_locked(&wifi) == 0);
    mutex_unlock(&wifi.lock);
    assert(doorbells == 4 && reports == 2 && applies == 4);

    wifi.host_sleep_sent = wifi.transport_paused = wifi.tx_paused = true;
    wifi_regulatory(&wiphy, &us);
    fail_firmware_report = true;
    mutex_lock(&wifi.lock);
    assert(alpha_resume_locked(&wifi) == -EIO);
    mutex_unlock(&wifi.lock);
    assert(wifi.command_failed && wifi.transport_paused && wifi.tx_paused);
}

int main(void)
{
    test_registered_pm_contract();
    test_host_sleep_gate();
    test_deferred_regulatory();
    prepare();
    assert(!wifi.pm_disconnect.seen);
    expect_error(0); /* Missing transaction remains missing. */
    struct cfg80211_wowlan wowlan = {};
    assert(!centauri_t6050_wifi_wowlan_armed(&control));
    assert(call_registered_suspend(&wiphy, &wowlan) == 1);
    assert(!centauri_t6050_wifi_wowlan_armed(&control));
    wowlan.any = true;
    assert(call_registered_suspend(&wiphy, &wowlan) == 0);
    assert(centauri_t6050_wifi_wowlan_armed(&control));
    assert(!doorbells && !wifi.pm_disconnect.seen && !centauri_t6050_wifi_pm_disconnected(&control));
    wifi.connected = false;
    assert(call_registered_suspend(&wiphy, &wowlan) == 1);
    assert(!centauri_t6050_wifi_wowlan_armed(&control));
    wifi.connected = true;
    wifi.connecting = true;
    assert(call_registered_suspend(&wiphy, &wowlan) == 1);
    wifi.connecting = false;
    wifi.command_failed = true;
    assert(call_registered_suspend(&wiphy, &wowlan) == 1);
    wifi.command_failed = false;
    assert(!doorbells && !wifi.pm_disconnect.seen);

    firmware_status = 326;
    wifi.data_tx_busy[0] = true;
    tx_drain_rounds = 2;
    assert(disconnect() == 0);
    assert(!data_tx_in_flight(&wifi));
    assert(wifi.pm_disconnect.seen && wifi.pm_disconnect.submitted);
    assert(centauri_t6050_wifi_pm_disconnected(&control));
    assert(wifi.pm_disconnect.replied && wifi.pm_disconnect.status == 326);
    assert(wifi.pm_disconnect.command_error == -EREMOTEIO);
    expect_error(0); /* Keep the existing helper normalization. */
    assert(notifications == 1);

    prepare();
    firmware_status = 777;
    assert(disconnect() == -EREMOTEIO);
    expect_error(-EREMOTEIO);
    firmware_status = 0;
    assert(disconnect() == 0);
    assert(wifi.pm_disconnect.status == 777);
    assert(wifi.pm_disconnect.command_error == -EREMOTEIO);
    expect_error(-EREMOTEIO); /* Later success cannot erase the first failure. */
    centauri_t6050_wifi_pm_complete(&control);
    assert(!wifi.pm_prepared && wifi.pm_disconnect.status == 777);
    assert(centauri_t6050_wifi_pm_error(&control) == 0);
    assert(wifi_suspend(&wiphy, NULL) == 0);
    centauri_t6050_wifi_pm_prepare(&control);
    assert(!wifi.pm_disconnect.seen);
    expect_error(0);

    prepare();
    wifi.stopping = true;
    wifi.reply_status = 326; /* Old reply must not become this receipt. */
    assert(disconnect() == 0);
    assert(wifi.pm_disconnect.stopping_skip && wifi.pm_disconnect.seen);
    assert(!wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied);
    assert(!wifi.pm_disconnect.status && !doorbells);
    expect_error(0);

    prepare();
    wifi.command_failed = true;
    wifi.reply_status = 326;
    assert(disconnect() == -EIO);
    assert(!wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied);
    assert(!wifi.pm_disconnect.status && !doorbells);
    expect_error(-EIO);

    prepare();
    consumer = 1; /* Full command ring: fail before publication. */
    assert(disconnect() == -ENOSPC);
    assert(!wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied);
    expect_error(-ENOSPC);

    prepare();
    recovery_requests = 0;
    wifi.stopping = true;
    wifi.configured = true;
    mutex_lock(&wifi.lock);
    command_fault(&wifi, 0, 2, "cancel");
    mutex_unlock(&wifi.lock);
    assert(wifi.command_failed && wifi.data_failed && !wifi.configured);
    assert(recovery_requests == 0);

    prepare();
    transport_mode = LOST;
    assert(disconnect() == -EIO);
    assert(wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied);
    expect_error(-EIO);

    prepare();
    transport_mode = TIMEOUT;
    wifi.reply_status = 326;
    assert(disconnect() == -ETIMEDOUT);
    assert(wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied);
    assert(!wifi.pm_disconnect.status);
    expect_error(-ETIMEDOUT);

    prepare();
    wrong_reply = true;
    firmware_status = 326; /* A mismatched sequence cannot normalize a timeout. */
    assert(disconnect() == -ETIMEDOUT);
    assert(!wifi.pm_disconnect.replied && !wifi.pm_disconnect.status);
    expect_error(-ETIMEDOUT);

    prepare();
    async_reply = true;
    assert(disconnect() == -ETIMEDOUT);
    assert(wifi.pm_disconnect.submitted && wifi.pm_disconnect.replied);
    assert(wifi.pm_disconnect.waited && !wifi.pm_disconnect.command_error);
    assert(wifi.pm_disconnect.reply_command == (131073 | BIT(30)));
    expect_error(-ETIMEDOUT);

    prepare();
    async_reply = async_event = true;
    assert(disconnect() == 0);
    assert(wifi.pm_disconnect.waited);
    expect_error(0); /* Existing async wait result; no causal attribution added. */

    reset();
    firmware_status = 777;
    assert(disconnect() == -EREMOTEIO);
    assert(!wifi.pm_disconnect.seen); /* Non-PM disconnect does not arm PM. */
    assert(wifi_suspend(&wiphy, NULL) == 0);
    control.wifi = NULL;
    centauri_t6050_wifi_pm_prepare(&control);
    assert(centauri_t6050_wifi_pm_error(&control) == 0);
    centauri_t6050_wifi_pm_complete(&control);
    prepare();
    bool published = false;
    wifi.last_fw_event_valid = true;
    submission_probe = &published;
    mutex_lock(&wifi.lock);
    assert(command_reply(&wifi, 131073, NULL, 0, NULL, NULL, &published) == 0);
    mutex_unlock(&wifi.lock);
    assert(published);
    submission_probe = NULL;
    reset();
    unsigned int requests_before = recovery_requests;
    wifi.last_fw_event_valid = true;
    consumer = 1; /* Ring is not empty, but has room for another command. */
    wifi.command_head = 2;
    mutex_lock(&wifi.lock);
    assert(command_reply(&wifi, 9, NULL, 0, NULL, NULL, &published) == -ETIMEDOUT);
    mutex_unlock(&wifi.lock);
    assert(!published && !doorbells && wifi.command_head == 2);
    assert(recovery_requests == requests_before + 1);

    /* Failed keep-alive unwind marks Wi-Fi failed so teardown skips ACIPC. */
    prepare();
    wifi.data_tx_busy[0] = true;
    centauri_t6050_wifi_mark_failed(&control);
    assert(wifi.stopping && wifi.command_failed);
    assert(disconnect() == 0);
    assert(wifi.pm_disconnect.stopping_skip && !doorbells && !drains);

    /*
     * Sleep sends legacy 86-byte TLV 280 without touching fallback state;
     * legacy firmware refuses 91 events on wake (-EREMOTEIO), falls back to 85,
     * and logs the fallback once via dev_info.
     */
    prepare();
    mock_reply_command = 6;
    mock_fw_max_events = 85;
    mutex_lock(&wifi.lock);
    assert(alpha_host_power(&wifi, true) == 0);
    assert(!info_logs && last_cmd6_tlv280_len == 86 &&
           last_cmd6_tlv280_count == 9 && doorbells == 1);
    assert(alpha_host_power(&wifi, false) == 0);
    assert(info_logs == 1 && wifi.host_events_fallback_logged &&
           last_cmd6_tlv280_len == 86 && last_cmd6_tlv280_count == 85 &&
           doorbells == 3);
    /* Subsequent wakes fall back without repeating the one-shot dev_info. */
    assert(alpha_host_power(&wifi, true) == 0);
    assert(last_cmd6_tlv280_len == 86 && last_cmd6_tlv280_count == 9 && doorbells == 4);
    assert(alpha_host_power(&wifi, false) == 0);
    assert(info_logs == 1 && last_cmd6_tlv280_len == 86 &&
           last_cmd6_tlv280_count == 85 && doorbells == 6);
    mutex_unlock(&wifi.lock);

    /* Transient unrelated -EREMOTEIO on wake does not permanently pin 85 events. */
    prepare();
    mock_reply_command = 6;
    mock_fw_max_events = 91;
    mock_fw_transient_status = 777;
    mutex_lock(&wifi.lock);
    assert(alpha_host_power(&wifi, false) == 0);
    assert(info_logs == 1 && last_cmd6_tlv280_len == 86 &&
           last_cmd6_tlv280_count == 85 && doorbells == 2);
    assert(alpha_host_power(&wifi, false) == 0);
    assert(info_logs == 1 && last_cmd6_tlv280_len == 92 &&
           last_cmd6_tlv280_count == 91 && doorbells == 3);
    mutex_unlock(&wifi.lock);

    /* Full-mask firmware keeps 86-byte sleep TLV and accepts 91 events on wake. */
    prepare();
    mock_reply_command = 6;
    mock_fw_max_events = 91;
    mutex_lock(&wifi.lock);
    assert(alpha_host_power(&wifi, true) == 0);
    assert(!info_logs && last_cmd6_tlv280_len == 86 &&
           last_cmd6_tlv280_count == 9 && doorbells == 1);
    assert(alpha_host_power(&wifi, false) == 0);
    assert(!info_logs && last_cmd6_tlv280_len == 92 &&
           last_cmd6_tlv280_count == 91 && doorbells == 2);
    mutex_unlock(&wifi.lock);

    puts("PASS: production disconnect/transport/PM callbacks and first-error retention");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--wifi-source', type=Path)
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[2]
    source = (args.wifi_source or root / "drivers/net/wireless/apple/centauri/t6050/centauri_wifi.c").read_text()
    core = (root / "net/wireless/core.c").read_text()
    receipt = re.search(r"struct centauri_wifi_disconnect_receipt \{.*?\n\};", source, re.S)
    if not receipt:
        raise RuntimeError("Production receipt structure missing")
    names = ["centauri_wait_control_idle", "command_fault", "command_reply", "wait_disconnect",
             "data_tx_in_flight", "drain_tx_in_flight", "wifi_disconnect",
             "centauri_t6050_wifi_pm_prepare", "centauri_t6050_wifi_pm_error",
             "centauri_t6050_wifi_pm_disconnected", "centauri_t6050_wifi_wowlan_armed",
             "centauri_t6050_wifi_mark_failed", "centauri_t6050_wifi_pm_complete",
             "alpha_host_power_send", "alpha_host_power", "country_set",
             "alpha_resume_locked", "wifi_regulatory", "wifi_suspend"]
    has_resume = bool(re.search(r"^static int wifi_resume\(", source, re.M))
    if has_resume:
        names.append("wifi_resume")
    production = "\n\n".join(block(source, name) for name in names)
    recovery = (root / "drivers/net/wireless/apple/centauri/t6050/centauri_recovery.c").read_text()
    production = block(recovery, "centauri_control_idle_ready") + "\n\n" + production
    ops = re.search(r"static const struct cfg80211_ops wifi_ops = \{(.*?)\n\};", source, re.S)
    registration = re.search(r"\.suspend\s*=\s*(\w+)", ops.group(1)) if ops else None
    suspend = registration.group(1) if registration else "NULL"
    resume_registration = re.search(r"\.resume\s*=\s*(\w+)", ops.group(1)) if ops else None
    resume_callback = resume_registration.group(1) if resume_registration else "NULL"
    expected_resume = "wifi_resume" if has_resume else "NULL"
    constraint = re.search(r"^\s*WARN_ON\(ops->suspend && !ops->resume\);", core, re.M)
    if not constraint:
        raise RuntimeError("Actual cfg80211 PM registration constraint changed")
    dispatch = f"""
struct cfg80211_ops {{
    int (*suspend)(struct wiphy *, struct cfg80211_wowlan *);
    int (*resume)(struct wiphy *);
}};
static unsigned registration_warnings;
static int mock_warn_on(bool condition)
{{
    registration_warnings += condition;
    return condition;
}}
#define WARN_ON(condition) mock_warn_on(!!(condition))
static void cfg80211_registration_check(const struct cfg80211_ops *ops)
{{
    {constraint.group().strip()}
}}
static const struct cfg80211_ops registered_pm_ops = {{
    .suspend = {suspend},
    .resume = {resume_callback},
}};
static int call_registered_suspend(struct wiphy *wiphy, struct cfg80211_wowlan *wowlan)
{{
    assert(registered_pm_ops.suspend == wifi_suspend);
    return registered_pm_ops.suspend(wiphy, wowlan);
}}
static int call_registered_resume(struct wiphy *wiphy)
{{
    assert(registered_pm_ops.resume && registered_pm_ops.resume == {expected_resume});
    return registered_pm_ops.resume(wiphy);
}}
"""
    production += dispatch
    variants = [("production", production, 0)]
    negative_controls = [
        ("lost-suspend-error", "ret = wifi->pm_disconnect.error;", "ret = 0;"),
        ("overwritten-first-error", "wifi->pm_prepared && !wifi->pm_disconnect.error",
         "wifi->pm_prepared"),
        ("stale-reply", "receipt.replied = receipt.submitted && (!ret || ret == -EREMOTEIO);",
         "receipt.replied = true;"),
        ("wowlan-ignores-trigger", "wowlan->any && ", ""),
        ("sleep-uses-full-len",
         "return alpha_host_power_send(wifi, true, CEN_HOST_EVENTS_LEGACY);",
         "return alpha_host_power_send(wifi, true, CEN_HOST_EVENTS_FULL);"),
        ("silent-fallback-log",
         "if (!ret && !wifi->host_events_fallback_logged)",
         "if (false && !ret && !wifi->host_events_fallback_logged)"),
        ("host-sleep-command", "\tif (wifi->host_sleep_sent)\n\t\treturn -EHOSTDOWN;\n", ""),
        ("regulatory-in-sleep", "if (wifi->host_sleep_sent) {\n\t\t/* Keep the last",
         "if (false) {\n\t\t/* Keep the last"),
        ("lost-deferred-country", "country_set(wifi, wifi->country_deferred);", "(void)wifi;"),
        ("ignored-regulatory-transport-loss",
         "if (wifi->command_failed)\n\t\t\treturn -EIO;", "if (false)\n\t\t\treturn -EIO;"),
    ]
    failures = {
        'lost-suspend-error': 'centauri_t6050_wifi_pm_error(&control) == expected',
        'overwritten-first-error': 'wifi.pm_disconnect.status == 777',
        'stale-reply': '!wifi.pm_disconnect.submitted && !wifi.pm_disconnect.replied',
        'wowlan-ignores-trigger': 'call_registered_suspend(&wiphy, &wowlan) == 1',
        'sleep-uses-full-len': 'alpha_host_power(&wifi, true) == 0',
        'silent-fallback-log': 'info_logs == 1',
        'host-sleep-command': 'command_reply(&wifi, 9, NULL, 0, NULL, NULL, &published) == -EHOSTDOWN',
        'regulatory-in-sleep': '!index_reads && !doorbells && !drains && !reads && !reports && !applies',
        'lost-deferred-country': 'doorbells == 2 && reports == 1 && applies == 1',
        'ignored-regulatory-transport-loss': 'alpha_resume_locked(&wifi) == -EIO',
    }
    for name, before, after in negative_controls if args.negative_controls else []:
        if before not in production:
            raise RuntimeError(f"Production pattern for {name} changed")
        variants.append((name, production.replace(before, after), -signal.SIGABRT))
    if args.negative_controls:
        needle = "\tif (submitted)\n\t\t*submitted = true;\n\tindex_write(wifi, 3, 1, wifi->command_head);"
        if needle not in production:
            raise RuntimeError("Production publication boundary changed")
        late = production.replace(needle, "\tindex_write(wifi, 3, 1, wifi->command_head);\n\tif (submitted)\n\t\t*submitted = true;", 1)
        variants.append(("late-submission", late, -signal.SIGABRT))
        failures["late-submission"] = "*submission_probe"
    if args.negative_controls and has_resume:
        if resume_registration:
            needle = ".resume = " + resume_callback + ","
            if needle not in production:
                raise RuntimeError("Registered resume initializer changed")
            variants.append(("cfg80211-missing-resume", production.replace(needle, ".resume = NULL,", 1), -signal.SIGABRT))
            failures["cfg80211-missing-resume"] = "registration_warnings == 0"
        for name, replacement, failure in (
            ("resume-private-access", "(void)wiphy_priv(wiphy);", "!private_access_forbidden"),
            ("resume-command", "(void)wiphy; writel(1, NULL);", "doorbells == before_doorbells"),
        ):
            needle = "(void)wiphy;"
            if needle not in production:
                raise RuntimeError("No-private-access resume body changed")
            variants.append((name, production.replace(needle, replacement, 1), -signal.SIGABRT))
            failures[name] = failure
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="centauri-wifi-pm-") as directory:
        out = args.out or Path(directory)
        out.mkdir(parents=True, exist_ok=True)
        for name, code, expected in variants:
            harness, binary = out / (name + '.c'), out / name
            harness.write_text(PRELUDE + receipt.group() + STUBS + code + TESTS)
            subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                      str(harness), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            (out / (name + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode != expected or (expected and (
                    'Assertion' not in result.stderr or failures[name] not in result.stderr)):
                raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}\n"
                                   + result.stdout + result.stderr)
            print(f"{name}: {'PASS' if expected == 0 else 'correctly rejected'}")


if __name__ == "__main__":
    main()
