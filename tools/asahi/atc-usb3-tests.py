#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host regression tests for the ATCPHY USB3 handoff.

Compile the actual driver functions, defines, enums and mode table. MMIO polling
models a single completed timeout/success per invocation; no timing, electrical
behavior, firmware, real BIST sequence or kernel mutex behavior is simulated.
The actual admission helper restricts the workaround to base M1 machines;
T600x/T8112/T602x still match the T8103 PHY fallback but keep their previous
single-poll/BIST/role behavior. OF compatible lookup is a modeled boundary.
The DT-to-driver match contract is checked from the actual sources.
Hardware qualification remains necessary.
Run --mutations for sensitivity checks.
"""

import argparse
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, marker):
    if marker.startswith("static"):
        # Skip a forward declaration; the next opening brace after a prototype
        # belongs to an unrelated function.
        match = re.search(re.escape(marker) + r"[^;{]*\{", source)
        if match is None:
            raise ValueError("function definition not found: " + marker)
        start = match.start()
    else:
        start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    pos = opening + 1
    while depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    return source[start:pos] + (";" if source[pos:pos + 1] == ";" else "")


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define lockdep_assert_held(...) ((void)0)
#define guard(which) (void)
#define IS_ENABLED(x) 0
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define __iomem
#define FIELD_PREP(mask,value) (((u32)(value) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask,value) (((value) & (mask)) >> __builtin_ctz(mask))
#define fallthrough __attribute__((fallthrough))
#define WARN_ON_ONCE(cond) do { if (cond) warnings++; } while (0)
#define container_of(ptr,type,member) ((type *)((char *)(ptr) - offsetof(type,member)))
'''

STUBS = r'''
struct atcphy_hw { enum atcphy_generation gen;
    bool has_t8140_dp, dp_t8122, has_usb4, park_pipe_unlocked, park_dummy_phy; };
struct device_node { const char *const *compatible; unsigned int count; };
static bool of_device_is_compatible(const struct device_node *root, const char *compatible) {
    if (!root) return false;
    for (unsigned int i = 0; i < root->count; i++)
        if (!strcasecmp(root->compatible[i], compatible)) return true;
    return false;
}
/* ACTUAL_ADMISSION */
struct reset_controller_dev { int unused; };
struct apple_atcphy {
    struct atcphy_hw *hw;
    struct { unsigned char *pipehandler, *core; } regs;
    void *dev;
    int lock;
    struct reset_controller_dev rcdev;
    enum atcphy_mode mode;
    enum atcphy_pipehandler_state pipe_state;
    enum atcphy_mode typec_mode;
    bool pipe_pending, t8103_dummy_pending, t8103_usb3_retry, t8103_usb3_workaround, mode_dirty, usb_off_pending;
    bool host_active, usb3_port_off, dp_only, ss_tunables, fixed_usb2, t8140_dp_tunables, dp_tunables;
};
struct phy { struct apple_atcphy *data; };
struct typec_mux_dev { struct apple_atcphy *data; };
struct typec_altmode { unsigned svid; };
struct typec_mux_state { unsigned long mode; struct typec_altmode *alt; void *data; };
struct enter_usb_data { u32 eudo; };
enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE, PHY_MODE_INVALID };
static unsigned char memory[0x18000] __attribute__((aligned(4)));
static struct atcphy_hw hw;
static struct apple_atcphy p;
static struct phy phy;
static unsigned polls, sleeps, sets, clears, muxes, bist_old, bist_new;
static unsigned configure_calls, restore_calls, unlocks, warnings;
static unsigned dummy_muxes, pending_seen_at_unlock;
static unsigned usb2_power_off_calls;
static unsigned ack_on;
static int check_ret, bist_ret, unlock_ret, configure_ret;
static bool concurrent_override_change;
static u32 readl(const void *ptr) { u32 v; memcpy(&v, ptr, sizeof(v)); return v; }
static void writel(u32 val, void *ptr) { memcpy(ptr, &val, sizeof(val)); }
static void set32(void *ptr, u32 mask) {
    if (ptr == memory + PIPEHANDLER_LOCK_REQ) sets++;
    writel(readl(ptr) | mask, ptr);
}
static void clear32(void *ptr, u32 mask) {
    if (ptr == memory + PIPEHANDLER_LOCK_REQ) clears++;
    writel(readl(ptr) & ~mask, ptr);
}
static void mask32(void *ptr, u32 mask, u32 value) {
    writel((readl(ptr) & ~mask) | value, ptr);
}
static u32 poll_value(void *ptr) {
    assert(ptr == memory + PIPEHANDLER_LOCK_ACK);
    polls++;
    if (concurrent_override_change) {
        set32(memory + PIPEHANDLER_OVERRIDE, BIT(9));
        set32(memory + PIPEHANDLER_OVERRIDE_VALUES, BIT(9));
    }
    return ack_on && polls >= ack_on ? PIPEHANDLER_LOCK_EN : 0;
}
#define readl_poll_timeout(addr,val,cond,delay,timeout) \
    ({ assert((delay) == 10 && (timeout) == 1000); \
       (val) = poll_value(addr); (cond) ? 0 : -ETIMEDOUT; })
static void usleep_range(unsigned lo, unsigned hi) {
    assert(lo == 5000 && hi == 10000); sleeps++;
}
static int atcphy_pipehandler_check(struct apple_atcphy *a) { (void)a; return check_ret; }
static int atcphy_pipehandler_unlock(struct apple_atcphy *a) {
    unlocks++;
    if (a->pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && a->t8103_dummy_pending)
        pending_seen_at_unlock++;
    clear32(a->regs.pipehandler + PIPEHANDLER_LOCK_REQ, PIPEHANDLER_LOCK_EN);
    return unlock_ret;
}
static int atcphy_configure_pipehandler_usb3_host_t8103(struct apple_atcphy *a) {
    (void)a; bist_old++; return bist_ret;
}
static int atcphy_configure_pipehandler_usb3_host_t8122(struct apple_atcphy *a) {
    (void)a; bist_new++; return bist_ret;
}
static void atcphy_pipehandler_set_mux(struct apple_atcphy *a, u32 data, u32 clk) {
    (void)a;
    assert((data == PIPEHANDLER_MUX_CTRL_DATA_USB3 && clk == PIPEHANDLER_MUX_CTRL_CLK_USB3) ||
           (data == PIPEHANDLER_MUX_CTRL_DATA_DUMMY && clk == PIPEHANDLER_MUX_CTRL_CLK_DUMMY) ||
           (data == PIPEHANDLER_MUX_CTRL_DATA_USB4 && clk == PIPEHANDLER_MUX_CTRL_CLK_USB4));
    if (data == PIPEHANDLER_MUX_CTRL_DATA_DUMMY) dummy_muxes++;
    muxes++;
}
static struct apple_atcphy *phy_get_drvdata(struct phy *f) { return f->data; }
static struct apple_atcphy *typec_mux_get_drvdata(struct typec_mux_dev *m) { return m->data; }
static void atcphy_restore_after_pd_off(struct apple_atcphy *a) { (void)a; restore_calls++; }
static int atcphy_usb2_power_off(struct apple_atcphy *a) {
    (void)a; usb2_power_off_calls++; return 0;
}
static int atcphy_configure(struct apple_atcphy *atcphy, enum atcphy_mode mode) {
    /* ACTUAL_CONFIGURE_ENTRY */
    struct apple_atcphy *a = atcphy;
    configure_calls++;
    assert(!a->t8103_dummy_pending);
    if (!configure_ret) { a->mode = mode; a->mode_dirty = false; }
    else a->mode_dirty = true;
    return configure_ret;
}
static void reset(enum atcphy_generation generation, bool t8140) {
    memset(memory, 0, sizeof(memory));
    hw = (struct atcphy_hw){ .gen = generation, .has_t8140_dp = t8140 };
    p = (struct apple_atcphy){ .hw = &hw, .regs = {memory, memory},
        .mode = APPLE_ATCPHY_MODE_OFF, .typec_mode = APPLE_ATCPHY_MODE_USB3,
        .ss_tunables = true };
    const char *const compatibles[] = {generation == ATCPHY_GENERATION_T8103 ? "apple,t8103" : "apple,t8122"};
    const struct device_node root = {compatibles, 1};
    p.t8103_usb3_workaround = atcphy_has_t8103_usb3_workaround(&hw, &root);
    phy.data = &p;
    polls = sleeps = sets = clears = muxes = bist_old = bist_new = 0;
    configure_calls = restore_calls = unlocks = warnings = 0;
    dummy_muxes = pending_seen_at_unlock = 0;
    usb2_power_off_calls = 0;
    ack_on = 1;
    check_ret = bist_ret = unlock_ret = configure_ret = 0;
    concurrent_override_change = false;
}
static void reset_machine(enum atcphy_generation generation, const char *machine) {
    reset(generation, false);
    const char *const compatibles[] = {"apple,synthetic-board", machine};
    const struct device_node root = {compatibles, 2};
    p.t8103_usb3_workaround = atcphy_has_t8103_usb3_workaround(&hw, &root);
}
'''

TESTS = r'''
static unsigned tests;
#define TEST(name) do { name(); tests++; } while (0)
static void live_retry_last(void) {
 reset(ATCPHY_GENERATION_T8103,false); ack_on=10;
 writel(BIT(7),memory+PIPEHANDLER_LOCK_REQ);
 assert(!atcphy_pipehandler_lock_live(&p));
 assert(polls==10 && sleeps==9 && sets==10 && clears==9);
 assert(readl(memory+PIPEHANDLER_LOCK_REQ)==(BIT(7)|PIPEHANDLER_LOCK_EN));
}
static void live_retry_exhausted(void) {
 reset(ATCPHY_GENERATION_T8103,false); ack_on=0;
 assert(atcphy_pipehandler_lock_live(&p)==-ETIMEDOUT);
 assert(polls==10 && sleeps==9 && sets==10 && clears==10);
}
static void request_is_not_ack(void) {
 reset(ATCPHY_GENERATION_T8103,false); ack_on=2;
 writel(PIPEHANDLER_LOCK_EN,memory+PIPEHANDLER_LOCK_REQ);
 assert(!atcphy_pipehandler_lock_live(&p));
 assert(polls==2 && sleeps==1 && sets==1 && clears==1);
}
static void requested_ack_no_writes(void) {
 reset(ATCPHY_GENERATION_T8103,false);
 writel(PIPEHANDLER_LOCK_EN,memory+PIPEHANDLER_LOCK_REQ);
 assert(!atcphy_pipehandler_lock_live(&p)); assert(polls==1 && !sets && !clears && !sleeps);
}
static void later_ack_semantics_unchanged(void) {
 for(unsigned neo=0;neo<2;neo++) {
 reset(ATCPHY_GENERATION_T8122,neo); ack_on=0;
 writel(PIPEHANDLER_LOCK_EN,memory+PIPEHANDLER_LOCK_REQ);
 assert(atcphy_pipehandler_lock_live(&p)==-ETIMEDOUT);
 assert(polls==1 && !sleeps && !sets && clears==1);
 }
}
static void usb4_and_dead_parking_single_poll(void) {
 reset(ATCPHY_GENERATION_T8103,false); ack_on=0;
 assert(!atcphy_configure_pipehandler_usb4(&p)); assert(polls==1 && !sleeps);
 reset(ATCPHY_GENERATION_T8103,false); ack_on=0;
 p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB4;
 assert(!atcphy_park_pipehandler(&p,true)); assert(polls==1 && !sleeps);
}
static void early_host_and_device(void) {
 for(unsigned host=0;host<2;host++) {
 reset(ATCPHY_GENERATION_T8103,false);
 assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
 assert(p.mode==APPLE_ATCPHY_MODE_USB3 && p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
 assert(configure_calls==1 && muxes==1 && !polls && !bist_old && !bist_new);
 assert(p.host_active==host && !p.pipe_pending);
 assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
 assert(configure_calls==1 && muxes==1);
 }
}
static void early_admission_and_failure(void) {
 reset(ATCPHY_GENERATION_T8103,false);p.ss_tunables=false;
 assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-EOPNOTSUPP);assert(!configure_calls&&!muxes);
 reset(ATCPHY_GENERATION_T8103,false);p.dp_only=true;
 assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-EOPNOTSUPP);assert(!configure_calls&&!muxes);
 reset(ATCPHY_GENERATION_T8103,false);configure_ret=-EIO;
 assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-EIO);assert(configure_calls==1&&!muxes);
}
static void later_off_and_bist_unchanged(void) {
 for(unsigned neo=0;neo<2;neo++) {
 reset(ATCPHY_GENERATION_T8122,neo);
 assert(!atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0));assert(p.mode==APPLE_ATCPHY_MODE_OFF&&!configure_calls&&!muxes);
 p.mode=APPLE_ATCPHY_MODE_USB3;
 assert(!atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0));assert(bist_new==1&&!bist_old&&polls==1);
 }
}
static void success_only_all_generations(void) {
 for(unsigned gen=0;gen<2;gen++)for(unsigned mode=0;mode<2;mode++) {
 reset(gen?ATCPHY_GENERATION_T8122:ATCPHY_GENERATION_T8103,false);
 p.mode=mode?APPLE_ATCPHY_MODE_USB4:APPLE_ATCPHY_MODE_USB3;check_ret=-EIO;
 assert(atcphy_configure_pipehandler(&p,true)==-EIO);
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY&&!muxes);
 check_ret=0;assert(!atcphy_configure_pipehandler(&p,true));
 assert(p.pipe_state==atcphy_modes[p.mode].pipehandler_state&&!p.pipe_pending);
 }
}
static void setup_legacy_usb3(void) {
 reset(ATCPHY_GENERATION_T8103,false);p.mode=APPLE_ATCPHY_MODE_USB3;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;
}
static void live_lock_failure_restores_masks(void) {
 setup_legacy_usb3();ack_on=0;concurrent_override_change=true;
 writel(BIT(7),memory+PIPEHANDLER_OVERRIDE);
 writel(BIT(7)|PIPEHANDLER_OVERRIDE_VAL_RXDETECT0,memory+PIPEHANDLER_OVERRIDE_VALUES);
 assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);
 assert(polls==10&&sleeps==9&&!muxes&&!unlocks);
 assert(readl(memory+PIPEHANDLER_OVERRIDE)==(BIT(7)|BIT(9)));
 assert(readl(memory+PIPEHANDLER_OVERRIDE_VALUES)==(BIT(7)|BIT(9)|PIPEHANDLER_OVERRIDE_VAL_RXDETECT0));
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_dummy_pending&&!p.t8103_usb3_retry&&!p.mode_dirty);
}
static void live_park_then_pending_completion(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;
 assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY&&p.t8103_dummy_pending&&p.t8103_usb3_retry);
 assert(muxes==1&&polls==1&&pending_seen_at_unlock==1);
 unsigned savedpolls=polls;
 assert(atcphy_park_pipehandler(&p,true)==-ETIMEDOUT);assert(polls==savedpolls&&muxes==1&&unlocks==2);
 unlock_ret=0;assert(!atcphy_park_pipehandler(&p,true));
 assert(!p.t8103_dummy_pending&&p.t8103_usb3_retry&&unlocks==3&&polls==savedpolls);
 assert(readl(memory+PIPEHANDLER_NONSELECTED_OVERRIDE)&PIPEHANDLER_NATIVE_RESET);
}
static void legacy_live_dispatch_and_generation(void) {
 setup_legacy_usb3();ack_on=0;
 assert(atcphy_park_pipehandler(&p,false)==-ETIMEDOUT);assert(polls==10&&!muxes);
 reset(ATCPHY_GENERATION_T8122,true);p.fixed_usb2=true;p.host_active=true;p.pipe_pending=true;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;ack_on=0;
 assert(atcphy_park_pipehandler(&p,false)==-ETIMEDOUT);assert(polls==1&&!sleeps&&!muxes&&p.mode_dirty);
 assert(atcphy_park_live_t8103_pipehandler(&p)==-EOPNOTSUPP);
}
static void pending_blocks_lane_and_backend(void) {
 setup_legacy_usb3();p.t8103_dummy_pending=true;
 assert(atcphy_configure(&p,APPLE_ATCPHY_MODE_DP)==-EBUSY);assert(!configure_calls);
 assert(atcphy_configure_pipehandler(&p,true)==-EBUSY);assert(!muxes);
}
static void canceled_lane_change_reconnects(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;
 assert(atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP)==-ETIMEDOUT);
 assert(p.mode==APPLE_ATCPHY_MODE_USB3&&p.t8103_dummy_pending&&p.t8103_usb3_retry&&!configure_calls);
 assert(!atcphy_mux_in_mode(&p,APPLE_ATCPHY_MODE_USB3));assert(!atcphy_mux_is_noop(&p,APPLE_ATCPHY_MODE_USB3));
 unlock_ret=0;assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_dummy_pending&&!p.t8103_usb3_retry);
 assert(polls==1&&dummy_muxes==1&&muxes==2&&!configure_calls&&!bist_old&&!bist_new);
}
static void role_finishes_pending(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);
 assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-ETIMEDOUT);assert(muxes==1);
 unlock_ret=0;assert(!atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0));
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_usb3_retry&&!p.t8103_dummy_pending&&muxes==2);
}
static void target_failure_preserves_reconnect(void) {
 setup_legacy_usb3();configure_ret=-EIO;
 assert(atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP)==-EIO);
 assert(p.t8103_usb3_retry&&!p.t8103_dummy_pending&&p.mode_dirty);
 configure_ret=0;assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
 assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_usb3_retry&&!p.mode_dirty);
}
static void nonusb_commit_clears_retry(void) {
 setup_legacy_usb3();assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP));
 assert(p.mode==APPLE_ATCPHY_MODE_DP&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY&&!p.t8103_usb3_retry&&!p.t8103_dummy_pending);
}
static void dp_assignment_d_retains_usb3(void) {
 setup_legacy_usb3();assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3_DP));
 assert(p.mode==APPLE_ATCPHY_MODE_USB3_DP&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!polls&&!muxes&&!warnings);
}
static void modern_safe_refusal_retained(void) {
 setup_legacy_usb3();assert(atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_OFF)==-EBUSY);assert(!polls&&!muxes&&!configure_calls);
}
static void fixed_hub_host_dispatch(void) {
 for(unsigned gen=0;gen<2;gen++) {
 reset(gen?ATCPHY_GENERATION_T8122:ATCPHY_GENERATION_T8103,false);p.fixed_usb2=true;p.host_active=true;
 assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
 assert(p.mode==APPLE_ATCPHY_MODE_USB3&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
 assert(bist_new==gen&&!bist_old&&configure_calls==1&&muxes==1);
 }
}
static void reset_release_retries_before_writes(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);
 u32 aon=BIT(14)|PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN;writel(aon,memory+PIPEHANDLER_AON_GEN);
 assert(atcphy_dwc3_reset_deassert(&p.rcdev,0)==-ETIMEDOUT);assert(readl(memory+PIPEHANDLER_AON_GEN)==aon);
 unlock_ret=0;assert(!atcphy_dwc3_reset_deassert(&p.rcdev,0));assert(!p.t8103_dummy_pending&&p.t8103_usb3_retry);
 assert(readl(memory+PIPEHANDLER_AON_GEN)==((aon&~PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN)|PIPEHANDLER_AON_GEN_DWC3_RESET_N));
 assert(!atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0));assert(!p.t8103_usb3_retry&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
}
static void reset_assert_failure_and_stop(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);
 u32 aon=BIT(14)|PIPEHANDLER_AON_GEN_DWC3_RESET_N;writel(aon,memory+PIPEHANDLER_AON_GEN);
 assert(atcphy_dwc3_reset_assert(&p.rcdev,0)==-ETIMEDOUT);assert(readl(memory+PIPEHANDLER_AON_GEN)==aon&&!usb2_power_off_calls);
 unlock_ret=0;assert(!atcphy_dwc3_reset_assert(&p.rcdev,0));assert(!p.t8103_dummy_pending&&!p.t8103_usb3_retry&&usb2_power_off_calls==1);
}
static void power_off_preserves_dp_and_finishes_pending(void) {
 setup_legacy_usb3();unlock_ret=-ETIMEDOUT;assert(atcphy_park_live_t8103_pipehandler(&p)==-ETIMEDOUT);p.mode=APPLE_ATCPHY_MODE_USB3_DP;
 assert(atcphy_usb3_power_off(&phy)==-ETIMEDOUT);assert(!configure_calls);
 unlock_ret=0;assert(!atcphy_usb3_power_off(&phy));assert(p.mode==APPLE_ATCPHY_MODE_USB3_DP&&!configure_calls&&!p.t8103_usb3_retry&&!p.t8103_dummy_pending);
}
static void setup_known_reset_clears_obligations(void) {
 setup_legacy_usb3();p.t8103_dummy_pending=p.t8103_usb3_retry=true;p.pipe_pending=true;
 atcphy_setup_pipehandler(&p);assert(!p.t8103_dummy_pending&&!p.t8103_usb3_retry&&!p.pipe_pending&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
}
static void target_mode_table_and_dirty(void) {
 reset(ATCPHY_GENERATION_T8103,false);struct typec_mux_state state={.mode=TYPEC_DP_STATE_D};struct typec_altmode alt={.svid=USB_TYPEC_DP_SID};state.alt=&alt;
 assert(atcphy_typec_target_mode(&p,&state)==APPLE_ATCPHY_MODE_USB3_DP);state.mode=TYPEC_DP_STATE_C;
 assert(atcphy_typec_target_mode(&p,&state)==APPLE_ATCPHY_MODE_DP);
 p.mode=APPLE_ATCPHY_MODE_USB3;p.mode_dirty=true;assert(atcphy_usb3_route(&p,true)==-EBUSY);assert(!muxes&&!configure_calls);
 p.mode_dirty=false;p.usb_off_pending=true;assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-EBUSY);
}
static void live_park_finishes_generic_pending(void) {
 setup_legacy_usb3(); p.pipe_pending=true;
 assert(!atcphy_park_pipehandler(&p,false));
 assert(!p.pipe_pending && !p.t8103_dummy_pending && p.t8103_usb3_retry);
 assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP));
 assert(p.mode==APPLE_ATCPHY_MODE_DP&&!p.t8103_usb3_retry);
}
static void fixed_hub_nonusb3_commit_discharges_retry(void) {
 setup_legacy_usb3();p.fixed_usb2=true;p.host_active=true;
 assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP));
 assert(!p.t8103_usb3_retry&&!p.t8103_dummy_pending&&!p.pipe_pending);
 assert(atcphy_mux_in_mode(&p,APPLE_ATCPHY_MODE_DP));
 assert(atcphy_mux_is_noop(&p,APPLE_ATCPHY_MODE_DP));
}
static void actual_dt_fallback_generation_scope(void) {
 for(unsigned i=0;i<sizeof(scope_generations)/sizeof(scope_generations[0]);i++) {
 reset_machine(scope_generations[i],scope_machines[i]);ack_on=0;
 assert(atcphy_pipehandler_lock_live(&p)==-ETIMEDOUT);
 assert(polls==(i==0?10:1) && sleeps==(i==0?9:0));
 }
}
static void admission_requires_machine_and_generation(void) {
 reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");
 assert(p.t8103_usb3_workaround);
 reset_machine(ATCPHY_GENERATION_T8122,"apple,t8103");
 assert(!p.t8103_usb3_workaround);
 reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103-extra");
 assert(!p.t8103_usb3_workaround);
 reset_machine(ATCPHY_GENERATION_T8103,"APPLE,T8103");
 assert(p.t8103_usb3_workaround); /* OF compatible lookup is case-insensitive. */
 assert(!atcphy_has_t8103_usb3_workaround(&hw,NULL));
}
static void fallback_machines_keep_previous_behavior(void) {
 const char *const machines[]={"apple,t6000","apple,t6001","apple,t6002",
     "apple,t8112","apple,t6020","apple,t6021","apple,t6022"};
 for(unsigned i=0;i<sizeof(machines)/sizeof(machines[0]);i++)for(unsigned host=0;host<2;host++) {
  reset_machine(ATCPHY_GENERATION_T8103,machines[i]);
  assert(!p.t8103_usb3_workaround);
  ack_on=0;assert(atcphy_pipehandler_lock_live(&p)==-ETIMEDOUT);
  assert(polls==1&&!sleeps);
  reset_machine(ATCPHY_GENERATION_T8103,machines[i]);
  assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
  assert(p.mode==APPLE_ATCPHY_MODE_OFF&&!configure_calls&&!muxes&&!polls);
  assert(!p.t8103_usb3_retry);
  p.mode=APPLE_ATCPHY_MODE_USB3;
  assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
  assert(bist_old==host&&!bist_new&&polls==host);
  reset_machine(ATCPHY_GENERATION_T8103,machines[i]);
  p.mode=APPLE_ATCPHY_MODE_USB3;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;
  assert(atcphy_park_live_t8103_pipehandler(&p)==-EOPNOTSUPP);
  assert(!polls&&!muxes&&!p.t8103_usb3_retry);
  ack_on=0;assert(!atcphy_park_pipehandler(&p,false));
  assert(polls==1&&!sleeps&&muxes==1&&!p.t8103_dummy_pending&&!p.t8103_usb3_retry);
  reset_machine(ATCPHY_GENERATION_T8103,machines[i]);
  p.mode=APPLE_ATCPHY_MODE_USB3;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;
  assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_DP));
  assert(configure_calls==1&&!polls&&!muxes&&!p.t8103_usb3_retry&&warnings==1);
 }
}
static void early_role_requires_explicit_usb3_request(void) {
 for(unsigned mode=APPLE_ATCPHY_MODE_OFF;mode<=APPLE_ATCPHY_MODE_DP;mode++) {
  for(unsigned host=0;host<2;host++) {
   reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");
   p.typec_mode=mode;
   assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
   bool usb3=mode==APPLE_ATCPHY_MODE_USB3;
   assert(configure_calls==usb3&&muxes==usb3&&!polls&&!bist_old&&!bist_new);
   assert(p.mode==(usb3?APPLE_ATCPHY_MODE_USB3:APPLE_ATCPHY_MODE_OFF));
   assert(p.t8103_usb3_retry==!usb3);
  }
 }
}
static void both_role_and_typec_orders_complete_usb3(void) {
 const enum atcphy_mode targets[]={APPLE_ATCPHY_MODE_USB3,APPLE_ATCPHY_MODE_USB3_DP};
 for(unsigned target=0;target<2;target++)for(unsigned host=0;host<2;host++)for(unsigned first=0;first<2;first++) {
  reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");
  p.typec_mode=APPLE_ATCPHY_MODE_USB2; /* Actual probe default. */
  if(first) {
   assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
   assert(p.mode==APPLE_ATCPHY_MODE_OFF&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
   assert(p.t8103_usb3_retry&&!configure_calls&&!muxes);
  }
  assert(!atcphy_mux_switch(&p,targets[target]));
  if(!first)
   assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
  assert(p.mode==targets[target]&&p.typec_mode==targets[target]);
  assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_usb3_retry&&!p.pipe_pending);
  assert(configure_calls==1&&muxes==1&&!polls&&!bist_old&&!bist_new);
  assert(atcphy_mux_is_noop(&p,targets[target]));
  assert(!atcphy_mux_switch(&p,targets[target]));
  assert(configure_calls==1&&muxes==1);
 }
}
static void nonusb3_commit_discharges_deferred_role(void) {
 const enum atcphy_mode targets[]={APPLE_ATCPHY_MODE_OFF,APPLE_ATCPHY_MODE_USB2,
     APPLE_ATCPHY_MODE_DP,APPLE_ATCPHY_MODE_USB4,APPLE_ATCPHY_MODE_TBT};
 for(unsigned target=0;target<5;target++)for(unsigned host=0;host<2;host++)for(unsigned same=0;same<2;same++) {
  reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");p.typec_mode=APPLE_ATCPHY_MODE_USB2;
  hw.has_usb4=true;
  assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
  assert(p.t8103_usb3_retry&&!configure_calls&&!muxes);
  /* Also exercise a committed same-mode notification with the outstanding role. */
  if(same)p.mode=targets[target];
  assert(!atcphy_mux_switch(&p,targets[target]));
  assert(p.mode==targets[target]&&!p.t8103_usb3_retry&&!p.pipe_pending);
  assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY&&!muxes&&!polls&&!bist_old&&!bist_new);
  assert(atcphy_mux_is_noop(&p,targets[target]));
  unsigned configured=configure_calls;
  assert(!atcphy_mux_switch(&p,targets[target]));assert(configure_calls==configured&&!muxes);
 }
}
static void deferred_role_survives_configuration_failures(void) {
 for(unsigned host=0;host<2;host++)for(unsigned early=0;early<2;early++) {
  reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");
  p.typec_mode=early?APPLE_ATCPHY_MODE_USB3:APPLE_ATCPHY_MODE_USB2;
  configure_ret=early?-EIO:0;
  assert(atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0)==(early?-EIO:0));
  assert(p.t8103_usb3_retry&&!muxes);
  configure_ret=-EIO;
  assert(atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3)==-EIO);
  assert(p.t8103_usb3_retry&&p.mode_dirty&&!muxes);
  configure_ret=0;assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
  assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&!p.t8103_usb3_retry&&!p.mode_dirty);
  assert(muxes==1&&!bist_old&&!bist_new);
 }
 for(unsigned host=0;host<2;host++) {
  reset_machine(ATCPHY_GENERATION_T8103,"apple,t8103");p.typec_mode=APPLE_ATCPHY_MODE_USB2;
  assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
  check_ret=-EIO;assert(atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3)==-EIO);
  assert(p.t8103_usb3_retry&&p.pipe_pending&&configure_calls==1&&!muxes);
  check_ret=0;assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
  assert(!p.t8103_usb3_retry&&!p.pipe_pending&&configure_calls==1&&muxes==1);
 }
}
static void role_after_typec_mux(void) {
 for(unsigned gen=0;gen<2;gen++)for(unsigned host=0;host<2;host++) {
 reset(gen?ATCPHY_GENERATION_T8122:ATCPHY_GENERATION_T8103,false);
 assert(!atcphy_mux_switch(&p,APPLE_ATCPHY_MODE_USB3));
 assert(configure_calls==1&&!muxes&&!polls&&!bist_old&&!bist_new);
 assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_HOST:PHY_MODE_USB_DEVICE,0));
 assert(p.host_active==host&&muxes==1&&bist_new==(gen&&host)&&!bist_old);
 unsigned old_polls=polls;
 assert(!atcphy_usb3_set_mode(&phy,host?PHY_MODE_USB_DEVICE:PHY_MODE_USB_HOST,0));
 assert(p.host_active!=host&&muxes==1&&polls==old_polls&&bist_new==(gen&&host));
 }
}
int main(void) {
 TEST(both_role_and_typec_orders_complete_usb3);
 TEST(nonusb3_commit_discharges_deferred_role);
 TEST(deferred_role_survives_configuration_failures);
 TEST(admission_requires_machine_and_generation);
 TEST(fallback_machines_keep_previous_behavior);
 TEST(early_role_requires_explicit_usb3_request);
 TEST(role_after_typec_mux);
 TEST(actual_dt_fallback_generation_scope);
 TEST(live_park_finishes_generic_pending);TEST(fixed_hub_nonusb3_commit_discharges_retry);
 TEST(live_retry_last);TEST(live_retry_exhausted);TEST(request_is_not_ack);TEST(requested_ack_no_writes);TEST(later_ack_semantics_unchanged);
 TEST(usb4_and_dead_parking_single_poll);TEST(early_host_and_device);TEST(early_admission_and_failure);TEST(later_off_and_bist_unchanged);TEST(success_only_all_generations);
 TEST(live_lock_failure_restores_masks);TEST(live_park_then_pending_completion);TEST(legacy_live_dispatch_and_generation);TEST(pending_blocks_lane_and_backend);
 TEST(canceled_lane_change_reconnects);TEST(role_finishes_pending);TEST(target_failure_preserves_reconnect);TEST(nonusb_commit_clears_retry);TEST(dp_assignment_d_retains_usb3);TEST(modern_safe_refusal_retained);
 TEST(fixed_hub_host_dispatch);TEST(reset_release_retries_before_writes);TEST(reset_assert_failure_and_stop);TEST(power_off_preserves_dp_and_finishes_pending);TEST(setup_known_reset_clears_obligations);TEST(target_mode_table_and_dirty);
 printf("%u ATCPHY USB3 host cases passed\n",tests);
}
'''


def generation_scope(source, root):
    """Resolve real DT fallback compatibles through the driver's actual match table."""
    tables = {}
    for name in ("t8103", "t8122", "t8140", "t8142"):
        text = block(source, "static const struct atcphy_hw atcphy_hw_" + name + " =")
        tables["atcphy_hw_" + name] = re.search(r"\.gen\s*=\s*(ATCPHY_GENERATION_\w+)", text).group(1)
    matches = dict(re.findall(r'\.compatible\s*=\s*"([^"]+)"\s*,\s*\.data\s*=\s*&([a-zA-Z0-9_]+)', source))
    scope = []
    for family, filename, expected in (
            ("t8103", "t8103.dtsi", "ATCPHY_GENERATION_T8103"),
            ("t600x", "t600x-dieX.dtsi", "ATCPHY_GENERATION_T8103"),
            ("t8112", "t8112.dtsi", "ATCPHY_GENERATION_T8103"),
            ("t602x", "t602x-dieX.dtsi", "ATCPHY_GENERATION_T8103"),
            ("t8122", "t8122.dtsi", "ATCPHY_GENERATION_T8122"),
            ("t6030", "t6030.dtsi", "ATCPHY_GENERATION_T8122"),
            ("t6031", "t6031-dieX.dtsi", "ATCPHY_GENERATION_T8122"),
            ("t8140", "t8140.dtsi", "ATCPHY_GENERATION_T8122"),
            ("t8142", "t8142-j813-usb.dtsi", "ATCPHY_GENERATION_T8122")):
        text = (root / "arch/arm64/boot/dts/apple" / filename).read_text()
        rows = re.findall(r'compatible\s*=\s*([^;]*"apple,[^";]*-atcphy"[^;]*);', text)
        assert rows, filename
        for row in rows:
            compatibles = re.findall(r'"([^"]+)"', row)
            selected = next((c for c in compatibles if c in matches), None)
            assert selected is not None, compatibles
            generation = tables[matches[selected]]
            assert generation == expected, (family, selected, generation)
        machine = {"t600x": "t6000", "t602x": "t6020"}.get(family, family)
        scope.append({"family": family, "generation": expected,
                      "machine_compatible": "apple," + machine,
                      "workaround": family == "t8103"})
    assert tables[matches["apple,t8142-atcphy"]] == "ATCPHY_GENERATION_T8122"
    return scope


def harness(source):
    # Preserve continuation lines: a line-only regexp can swallow the next enum
    # as part of a trailing backslash macro and silently change its meaning.
    definitions = []
    continuing = False
    for line in source.splitlines():
        if continuing or line.startswith("#define "):
            definitions.append(line)
            continuing = line.endswith("\\")
    defines = "\n".join(definitions)
    root = Path(__file__).resolve().parents[2]
    alt_header = (root / "include/linux/usb/typec_altmode.h").read_text()
    dp_header = (root / "include/linux/usb/typec_dp.h").read_text()
    usb_types = "\n".join((block(alt_header, "enum {\n\tTYPEC_STATE_SAFE"),
                            block(alt_header, "enum {\n\tTYPEC_MODE_USB2"),
                            block(dp_header, "enum {\n\tTYPEC_DP_STATE_A")))
    usb_defines = []
    for name in ("pd.h", "typec_dp.h", "typec_tbt.h"):
        for line in (root / "include/linux/usb" / name).read_text().splitlines():
            if re.match(r"#define\s+(EUDO_USB_MODE_|USB_TYPEC_(DP_SID|TBT_SID|VENDOR_INTEL))", line):
                usb_defines.append(line)
    types = usb_types + "\n" + "\n".join(block(source, "enum " + name + " {") for name in (
        "atcphy_generation", "atcphy_lane_mode", "atcphy_pipehandler_state",
        "atcphy_mode", "atcphy_lane"))
    types += "\n" + block(source, "struct atcphy_mode_configuration {")
    start = source.index("static const struct {\n\tconst struct atcphy_mode_configuration normal;")
    end = source.index("\n};", source.index("} atcphy_modes[] = {", start)) + 3
    table = source[start:end]
    function_specs = [
        ("int", "atcphy_pipehandler_lock"),
        ("int", "atcphy_pipehandler_lock_live"),
        ("int", "atcphy_configure_pipehandler_usb3"),
        ("int", "atcphy_configure_pipehandler_usb4"),
        ("void", "atcphy_enable_dummy_phy"),
        ("bool", "atcphy_pipe_live"),
        ("bool", "atcphy_needs_dummy_phy"),
        ("int", "atcphy_configure_pipehandler_dummy"),
        ("int", "atcphy_configure_pipehandler"),
        ("int", "atcphy_park_live_t8103_pipehandler"),
        ("int", "atcphy_park_pipehandler"),
        ("int", "atcphy_usb3_route"),
        ("int", "atcphy_usb3_set_mode"),
        ("bool", "atcphy_usb3_port_ready"),
        ("enum atcphy_mode", "atcphy_typec_target_mode"),
        ("bool", "atcphy_mux_in_mode"),
        ("bool", "atcphy_mux_is_noop"),
        ("int", "atcphy_mux_check"),
        ("int", "atcphy_mux_switch"),
        ("int", "atcphy_usb3_power_off"),
        ("void", "_atcphy_dwc3_reset_assert"),
        ("int", "atcphy_dwc3_reset_assert"),
        ("int", "atcphy_dwc3_reset_deassert"),
        ("void", "atcphy_setup_pipehandler"),
    ]
    scope = generation_scope(source, root)
    scope_c = "static const enum atcphy_generation scope_generations[] = {" + ",".join(r["generation"] for r in scope) + "};\n"
    functions = "\n".join(block(source, "static " + kind + " " + name + "(")
                          for kind, name in function_specs)
    configure = block(source, "static int atcphy_configure(")
    guard_start = configure.index("lockdep_assert_held")
    guard_end = configure.index("if (atcphy->usb_off_pending")
    stubs = STUBS.replace("/* ACTUAL_CONFIGURE_ENTRY */", configure[guard_start:guard_end])
    stubs = stubs.replace("/* ACTUAL_ADMISSION */",
                          block(source, "static bool atcphy_has_t8103_usb3_workaround("))
    scope_c += "static const char *const scope_machines[] = {" + ",".join(
        json.dumps(row["machine_compatible"]) for row in scope) + "};\n"
    return PRELUDE + defines + "\n" + "\n".join(usb_defines) + "\n" + types + "\n" + table + stubs + functions + scope_c + TESTS


MUTATIONS = {'no-live-retry': ('atcphy_pipehandler_lock_live', '? 10 : 1;', '? 1 : 1;'),
 'retry-later-generations': ('atcphy_pipehandler_lock_live', '? 10 : 1;', '? 10 : 10;'),
 'sleep-after-last': ('atcphy_pipehandler_lock_live', 'if (attempt + 1 < attempts)', 'if (true)'),
 'missing-ack-check': ('atcphy_pipehandler_lock',
                       'if (requested & PIPEHANDLER_LOCK_EN) {',
                       'if (requested & PIPEHANDLER_LOCK_EN) return 0;\n'
                       '\tif (requested & PIPEHANDLER_LOCK_EN) {'),
 'usb4-live-retry': ('atcphy_configure_pipehandler_usb4',
                     'atcphy_pipehandler_lock(atcphy)',
                     'atcphy_pipehandler_lock_live(atcphy)'),
 'dead-park-live-retry': ('atcphy_configure_pipehandler_dummy',
                          'atcphy_pipehandler_lock(atcphy)',
                          'atcphy_pipehandler_lock_live(atcphy)'),
 'early-missing-tunables': ('atcphy_usb3_route',
                            'if (atcphy->dp_only || !atcphy->ss_tunables)',
                            'if (atcphy->dp_only)'),
 'early-missing-dp-only': ('atcphy_usb3_route',
                           'if (atcphy->dp_only || !atcphy->ss_tunables)',
                           'if (!atcphy->ss_tunables)'),
 'early-all-generations': ('atcphy_usb3_route',
                           'atcphy->t8103_usb3_workaround &&',
                           'true &&'),
 'early-ignores-typec-request': ('atcphy_usb3_route',
                                'atcphy->typec_mode != APPLE_ATCPHY_MODE_USB3',
                                'false'),
 'early-role-not-remembered': ('atcphy_usb3_route',
                              'atcphy->t8103_usb3_retry = true;',
                              '(void)0;'),
 'early-configure-error-loses-obligation': ('atcphy_usb3_route',
                                           'atcphy->t8103_usb3_retry = true;',
                                           'if (atcphy->typec_mode != APPLE_ATCPHY_MODE_USB3) atcphy->t8103_usb3_retry = true;'),
 'same-mode-nonusb3-keeps-role-obligation': ('atcphy_mux_switch',
                                            '\t\t\tatcphy->t8103_usb3_retry = false;\n\t\t}\n\t\t/* Retry a failed PIPE setup',
                                            '\t\t\t(void)0;\n\t\t}\n\t\t/* Retry a failed PIPE setup'),
 'admission-ignores-machine': ('bool atcphy_has_t8103_usb3_workaround',
                              'of_device_is_compatible(root, "apple,t8103")',
                              'true'),
 'admission-ignores-generation': ('bool atcphy_has_t8103_usb3_workaround',
                                 'hw->gen == ATCPHY_GENERATION_T8103 &&',
                                 'true &&'),
 'bist-restored-t8103': ('atcphy_configure_pipehandler',
                         'host && !atcphy->t8103_usb3_workaround',
                         'host'),
 'bist-skipped-later': ('atcphy_configure_pipehandler',
                        'host && !atcphy->t8103_usb3_workaround',
                        'false'),
 'bist-skipped-fallback-machines': ('atcphy_configure_pipehandler',
                                   'host && !atcphy->t8103_usb3_workaround',
                                   'host && atcphy->hw->gen != ATCPHY_GENERATION_T8103'),
 'failed-state-published': ('atcphy_configure_pipehandler', 'if (!ret) {', 'if (true) {'),
 'live-generation-gate-removed': ('atcphy_park_live_t8103_pipehandler',
                                  'if (!atcphy->t8103_usb3_workaround)',
                                  'if (false)'),
 'live-park-dispatch-all-fallbacks': ('atcphy_park_pipehandler',
                                    '!stopped && atcphy->t8103_usb3_workaround &&',
                                    '!stopped && atcphy->hw->gen == ATCPHY_GENERATION_T8103 &&'),
 'mux-live-park-all-fallbacks': ('atcphy_mux_switch',
                               'if (atcphy->t8103_usb3_workaround)',
                               'if (atcphy->hw->gen == ATCPHY_GENERATION_T8103)'),
 'no-values-restore': ('atcphy_park_live_t8103_pipehandler',
                       'mask32(pipe + PIPEHANDLER_OVERRIDE_VALUES, value_mask,\n'
                       '\t\t\t       values & value_mask);',
                       '(void)values;'),
 'no-overrides-restore': ('atcphy_park_live_t8103_pipehandler',
                          'mask32(pipe + PIPEHANDLER_OVERRIDE, override_mask,\n'
                          '\t\t\t       overrides & override_mask);',
                          '(void)overrides;'),
 'clobber-unrelated-overrides': ('atcphy_park_live_t8103_pipehandler',
                                 'mask32(pipe + PIPEHANDLER_OVERRIDE, override_mask,\n'
                                 '\t\t\t       overrides & override_mask);',
                                 'writel(overrides, pipe + PIPEHANDLER_OVERRIDE);'),
 'no-physical-mux-state': ('atcphy_park_live_t8103_pipehandler',
                           'atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;',
                           '(void)0;'),
 'no-pending-after-move': ('atcphy_park_live_t8103_pipehandler',
                           'atcphy->t8103_dummy_pending = true;',
                           '(void)0;'),
 'relock-pending': ('atcphy_park_live_t8103_pipehandler',
                    'if (!atcphy->t8103_dummy_pending)',
                    'if (true)'),
 'no-reconnect-obligation': ('atcphy_park_live_t8103_pipehandler',
                             'atcphy->t8103_usb3_retry = true;',
                             '(void)0;'),
 'pending-cleared-before-ack': ('atcphy_park_live_t8103_pipehandler',
                                'ret = atcphy_pipehandler_unlock(atcphy);',
                                'atcphy->t8103_dummy_pending = false;\n'
                                '\tret = atcphy_pipehandler_unlock(atcphy);'),
 'park-bypasses-pending': ('atcphy_park_pipehandler',
                           'if (atcphy->t8103_dummy_pending ||',
                           'if (false ||'),
 'lanes-ignore-pending': ('atcphy_configure', 'if (atcphy->t8103_dummy_pending)', 'if (false)'),
 'backend-ignores-pending': ('atcphy_configure_pipehandler',
                             'if (atcphy->t8103_dummy_pending)',
                             'if (false)'),
 'role-ignores-pending': ('atcphy_usb3_route', 'if (atcphy->t8103_dummy_pending)', 'if (false)'),
 'mux-ignores-pending': ('atcphy_mux_switch', 'if (atcphy->t8103_dummy_pending)', 'if (false)'),
 'canceled-not-reconnected': ('atcphy_mux_switch',
                              'if (atcphy_modes[target_mode].pipehandler_state == ATCPHY_PIPEHANDLER_STATE_USB3)\n\t\t\t\treturn atcphy_configure_pipehandler',
                              'if (false)\n\t\t\t\treturn atcphy_configure_pipehandler'),
 'retry-cleared-at-unlock': ('atcphy_park_live_t8103_pipehandler',
                             'atcphy->t8103_dummy_pending = false;',
                             'atcphy->t8103_dummy_pending = false; atcphy->t8103_usb3_retry = '
                             'false;'),
 'retry-not-cleared-after-usb3': ('atcphy_configure_pipehandler',
                                  'atcphy->t8103_usb3_retry = false;',
                                  '(void)0;'),
 'retry-not-cleared-after-nonusb3': ('atcphy_mux_switch',
                                     'return atcphy_configure_pipehandler(atcphy, false);\n'
                                     '\t\tatcphy->t8103_usb3_retry = false;',
                                     'return atcphy_configure_pipehandler(atcphy, false);\n'
                                     '\t\t(void)0;'),
 'usb3-dp-wrongly-parked': ('atcphy_mux_switch',
                            'atcphy_modes[target_mode].pipehandler_state == '
                            'ATCPHY_PIPEHANDLER_STATE_DUMMY) {',
                            'true) {'),
 'reset-release-ignores-pending': ('atcphy_dwc3_reset_deassert',
                                   'if (atcphy->t8103_dummy_pending)',
                                   'if (false)'),
 'reset-release-permanent-ebusy': ('atcphy_dwc3_reset_deassert',
                                   'if (atcphy->t8103_dummy_pending) {',
                                   'if (atcphy->t8103_dummy_pending) return -EBUSY;\n'
                                   '\tif (atcphy->t8103_dummy_pending) {'),
 'reset-release-writes-early': ('atcphy_dwc3_reset_deassert',
                                'if (atcphy->t8103_dummy_pending) {',
                                'clear32(atcphy->regs.pipehandler + PIPEHANDLER_AON_GEN, '
                                'PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);\n'
                                '\tif (atcphy->t8103_dummy_pending) {'),
 'reset-no-retry-clear': ('atcphy_dwc3_reset_assert',
                          'atcphy->t8103_usb3_retry = false;',
                          '(void)0;'),
 'poweroff-no-retry-clear': ('atcphy_usb3_power_off',
                             'atcphy->t8103_usb3_retry = false;',
                             '(void)0;'),
 'known-reset-no-retry-clear': ('void atcphy_setup_pipehandler',
                                'atcphy->t8103_usb3_retry = false;',
                                '(void)0;'),
 'live-completion-leaves-generic-pending': ('atcphy_park_live_t8103_pipehandler',
                                            'atcphy->pipe_pending = false;',
                                            '(void)0;'),
 'fixedhub-nonusb3-keeps-retry': ('atcphy_mux_switch',
                                  'if (!ret && pipe_state != ATCPHY_PIPEHANDLER_STATE_USB3)\n'
                                  '\t\t\tatcphy->t8103_usb3_retry = false;',
                                  '(void)0;'),
 'outer-noop-ignores-new-obligations': ('bool atcphy_mux_in_mode',
                                        'atcphy->t8103_dummy_pending || atcphy->t8103_usb3_retry',
                                        'false')}


def run(code, cc, directory, name):
    cfile = directory / (name + ".c")
    binary = directory / name
    cfile.write_text(code)
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-Wall", "-Wextra",
                            "-Wno-unused-parameter", "-Wno-unused-function",
                            str(cfile), "-o", str(binary)], text=True, capture_output=True)
    if build.returncode:
        raise RuntimeError("compile failed for " + name + ":\n" + build.stderr)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1")
    return subprocess.run([str(binary)], text=True, capture_output=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "drivers/phy/apple/atc.c")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--out", type=Path, help="retain generated C, binaries and receipt")
    args = parser.parse_args()
    code = harness(args.source.read_text())
    results = {}
    context = tempfile.TemporaryDirectory(prefix="atc-usb3-tests-") if args.out is None else None
    directory = Path(context.name) if context else args.out
    directory.mkdir(parents=True, exist_ok=True)
    result = run(code, args.cc, directory, "actual")
    (directory / "actual.stdout").write_text(result.stdout)
    (directory / "actual.stderr").write_text(result.stderr)
    if result.returncode:
        raise SystemExit(result.stderr or result.stdout)
    print(result.stdout, end="")
    if args.mutations:
        for name, mutation in MUTATIONS.items():
            function, before, after = mutation
            declaration = function if " " in function else "int " + function
            old_scope = block(code, "static " + declaration + "(")
            if old_scope.count(before) != 1:
                raise RuntimeError(name + ": expected one mutation site")
            changed = old_scope.replace(before, after, 1)
            mutated = code.replace(old_scope, changed, 1)
            result = run(mutated, args.cc, directory, name)
            (directory / (name+".stdout")).write_text(result.stdout)
            (directory / (name+".stderr")).write_text(result.stderr)
            if result.returncode == 0:
                raise RuntimeError("mutation survived: " + name)
            if "Assertion" not in result.stderr and "runtime error:" not in result.stderr:
                raise RuntimeError("mutant failed without an assertion or sanitizer diagnostic: " + name)
            results[name] = {"class": "KILLED", "returncode": result.returncode}
        print(json.dumps({"mutations": results}, sort_keys=True))
    after = run(code, args.cc, directory, "restored-control")
    (directory/"restored-control.stdout").write_text(after.stdout)
    (directory/"restored-control.stderr").write_text(after.stderr)
    if after.returncode:
        raise RuntimeError("post-mutation control failed: " + after.stderr)
    receipt = {"source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
               "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "generated_sha256": hashlib.sha256(code.encode()).hexdigest(),
               "generation_scope": generation_scope(args.source.read_text(),Path(__file__).resolve().parents[2]),
               "control_stdout": (directory/"actual.stdout").read_text(),
               "after_control_stdout": after.stdout,
               "mutations": results,
               "scope": "actual extracted driver bodies; MMIO, polling, BIST, lane configuration and power restoration boundary models"}
    (directory/"receipt.json").write_text(json.dumps(receipt, indent=2)+"\n")
    if context: context.cleanup()



if __name__ == "__main__":
    main()
