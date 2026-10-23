# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual PHY parking, failure/retry, masked context and OFF controls."""
from pathlib import Path
import re
from common import (block, source_defines, atc_mux_without_pm, ATC_POST_TUNABLE,
                    atc_function, ATC_SPLIT, ATC_T8103_LIVE)

def parking(root):

    atc = (root / 'drivers/phy/apple/atc.c').read_text()
    dp = (root / 'drivers/phy/apple/atc-j700-dp.h').read_text()


    def function(source, name):
        if name == 'atcphy_mux_set' and 'static int atcphy_mux_set_locked(' in source:
            return atc_mux_without_pm(source)
        if name in ATC_SPLIT + ATC_T8103_LIVE:
            return atc_function(source, name)
        return block(source, re.search(r'^static [^\n;]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M).group())


    # Use the production mode table and wrappers too, so SAFE mapping and fast paths
    # are tested rather than mirrored by a second implementation.
    parts = source_defines(atc)
    t8122 = (root / 'drivers/phy/apple/atc-t8122-dp.h').read_text()
    parts += source_defines(t8122)
    for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode',
                 'atcphy_dp_link_rate'):
        parts.append(block(atc, 'enum ' + name + ' {') + ';')
    for name in ('atcphy_mode_configuration', 'atcphy_hw'):
        parts.append(block(atc, 'struct ' + name + ' {') + ';')
    parts.append(r'''
struct apple_atcphy {
    const struct atcphy_hw *hw;
    struct { unsigned char *core, *lpdptx, *pipehandler, *usb2phy; } regs;
    struct reset_controller_dev rcdev;
    void *dev;
    int lock, dp_link_rate;
    bool fixed_usb2, host_active, swap_lanes, ss_tunables, dp_tunables;
    bool aux_power_requested, mode_dirty, dp_spread_spectrum, dp_presets_valid[5];
    u32 dp_presets[5][16];
    unsigned int dp_idle_pmas, dp_owned_pmas;
    unsigned int dp_usb_context_valid, dp_usb_context_dirty;
    u32 dp_usb_context[2][27];
    bool pipe_pending, usb_off_pending, usb_off_drained;
    bool usb3_port_off, park_owed;
    bool t8103_dummy_pending, t8103_usb3_retry, t8103_usb3_workaround;
    bool dp_only, pd_was_off, tunnel_clock_on;
    unsigned int tunnel_rate, tunnel_users;
    struct { bool aux, pll; unsigned int pairs, rate; } dp_t8122;
    enum atcphy_mode mode, typec_mode;
    enum atcphy_pipehandler_state pipe_state;
};
''')
    start = atc.index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
    parts.append(atc[start:atc.index('\n};', start) + 3])
    parts.append(r'''
struct phy_configure_opts_dp {
    unsigned int link_rate, lanes, voltage[4], pre[4];
    bool set_rate, set_lanes, set_voltages, ssc;
};
static u32 registers[0x30000 / 4];
static u32 pipe_regs[0x1000 / 4], usb2_regs[0x1000 / 4];
static unsigned int polls, fail_poll, writes, usb2_on, usb2_writes, reset_writes, mux_writes;
static bool stuck_ack;
static bool hold_unlock_ack;
static const char *poll_caller[256], *fail_caller;
static bool bist_status_stuck;
static unsigned int bist_faults;
static bool require_parked, expected_j700_swap;
static u32 readl(void *address) { return *(u32 *)address; }
static void core_mask32(struct apple_atcphy *p, u32 offset, u32 mask, u32 value)
{
    (void)p;
    assert(offset < sizeof(registers) && offset % 4 == 0);
    if (require_parked) {
        assert(p->pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
        assert(FIELD_GET(p->hw->pipe_mux_data, pipe_regs[PIPEHANDLER_MUX_CTRL / 4]) ==
               PIPEHANDLER_MUX_CTRL_DATA_DUMMY);
        assert(FIELD_GET(p->hw->pipe_mux_clk, pipe_regs[PIPEHANDLER_MUX_CTRL / 4]) ==
               PIPEHANDLER_MUX_CTRL_CLK_DUMMY);
    }
    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;
    writes++;
    if (offset == ATCPHY_POWER_CTRL)
        registers[ATCPHY_POWER_STAT / 4] = registers[offset / 4];
    if (offset == 0x2000) {
        if (value == 0x10000001) {
            registers[offset / 4] |= BIT(1);
            registers[0x7034 / 4] |= BIT(3);
        } else {
            registers[offset / 4] &= ~BIT(1);
            registers[0x7034 / 4] &= ~BIT(3);
        }
    }
}
static void core_set32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, 0, v); }
static void core_clear32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, v, 0); }
static void mask32(void *p, u32 m, u32 v)
{
    *(u32 *)p = (*(u32 *)p & ~m) | v;
    if (p == (void *)&pipe_regs[PIPEHANDLER_LOCK_REQ / 4] && !stuck_ack)
        pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = hold_unlock_ack &&
            !(*(u32 *)p & PIPEHANDLER_LOCK_EN) ? PIPEHANDLER_LOCK_EN :
                                              *(u32 *)p & PIPEHANDLER_LOCK_EN;
    if (p == (void *)&pipe_regs[PIPEHANDLER_AON_GEN / 4]) reset_writes++;
    if (p == (void *)&pipe_regs[PIPEHANDLER_MUX_CTRL / 4]) mux_writes++;
    if ((uintptr_t)p >= (uintptr_t)usb2_regs &&
        (uintptr_t)p < (uintptr_t)usb2_regs + sizeof(usb2_regs)) usb2_writes++;
}
static void writel(u32 value, void *address) { mask32(address, ~0U, value); }
static void set32(void *p, u32 v) { mask32(p, 0, v); }
static void clear32(void *p, u32 v) { mask32(p, v, 0); }
#define readl_poll_timeout_atomic(address, value, condition, interval, timeout) \
    ({ (value) = readl(address); assert(polls < ARRAY_SIZE(poll_caller)); \
       poll_caller[polls++] = __func__; \
       bool bist_fault = bist_status_stuck && (void *)(address) == \
           (void *)&registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2 / 4]; \
       bist_faults += bist_fault; \
       polls == fail_poll || bist_fault || \
       (fail_caller && !strcmp(fail_caller, __func__)) || !(condition) ? -ETIMEDOUT : 0; })
#define readl_poll_timeout readl_poll_timeout_atomic
/* Observation is read-only and separately tested; retain production caller control flow. */
static void atcphy_usb2_power_on(struct apple_atcphy *p) { (void)p; usb2_on++; }
static void atcphy_apply_tunables(struct apple_atcphy *p, enum atcphy_mode mode)
{ (void)p; (void)mode; }
static void atc_tunnel_stop_t8103(struct apple_atcphy *p) { (void)p; }
static void atc_tunnel_restore(struct apple_atcphy *p) { (void)p; }
/* These fixtures exercise host/PIPE ownership with electrical tunnels stopped. */
static int atc_t8122_tunnel_stop(struct apple_atcphy *p)
{ assert(!p->tunnel_clock_on && !p->tunnel_users); return 0; }
static int atcphy_dp_link_stop_t8122(struct apple_atcphy *p)
{ assert(!p->dp_t8122.pll && !p->dp_t8122.pairs); return 0; }
#define __iomem
static bool of_machine_is_compatible(const char *name) { (void)name; return false; }
#ifndef READ_ONCE
#define READ_ONCE(x) (x)
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(v, x) ((v) = (x))
#endif
static void atcphy_restore_after_pd_off(struct apple_atcphy *);
static int atcphy_power_off(struct apple_atcphy *);
static int atcphy_configure(struct apple_atcphy *, enum atcphy_mode);

''')
    for name in ('atc_t8122_dp_mask', 'atc_t8122_dp_aux_on', 'atc_t8122_dp_aux_off', 'atc_t8122_dp_aux_is_on'):
        match = re.search(r'^static inline [^\n]*\b' + name + r'\([^;{}]*\)\s*\{', t8122, re.M)
        assert match, name
        parts.append(block(t8122, match.start()).replace('static inline ', 'static inline __attribute__((unused)) ', 1))
    for name in ('atcphy_dp_aux_on_t8122', 'atcphy_dp_stop_t8122'):
        parts.append(function(atc, name))
    for name in ('atcphy_pipehandler_lock', 'atcphy_pipehandler_lock_live',
                 'atcphy_pipehandler_unlock', 'atcphy_pipehandler_check',
                 'atcphy_pipehandler_set_mux', 'atcphy_clear_nonselected_phy_reset',
                 'atcphy_configure_pipehandler_usb3_host_t8122',
                 'atcphy_configure_pipehandler_usb3_host_t8103',
                 'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
                 'atcphy_enable_dummy_phy', 'atcphy_pipe_live', 'atcphy_needs_dummy_phy',
                 'atcphy_configure_pipehandler_dummy',
                 'atcphy_configure_pipehandler', 'atcphy_park_live_t8103_pipehandler',
                 'atcphy_park_pipehandler',
                 'atcphy_usb2_power_off', '_atcphy_dwc3_reset_assert', 'atcphy_dwc3_reset_assert'):
        parts.append(function(atc, name))
    for name in ('core_read32', 'atcphy_get_mode_config', 'j700_field_update', 'j700_pma_idle'):
        parts.append(function(atc, name))
    parts.append(dp)
    for name in ('atcphy_configure_lanes', 'j700_aux_enable', 'j700_aux_disable',
                 'atcphy_enable_dp_aux', 'atcphy_disable_dp_aux', 'atcphy_power_off',
                 'atcphy_power_on', *ATC_POST_TUNABLE, 'atcphy_configure', 'atcphy_set_orientation',
                 'atcphy_restore_after_pd_off', 'atcphy_sw_set', 'atcphy_mux_set',
                 'atcphy_usb3_route', 'atcphy_usb3_set_mode', 'atcphy_usb3_power_off'):
        parts.append(function(atc, name))
    production = '\n'.join(parts)
    if 'static int j700_usb_capture(' in dp:
        production = '#define HAS_USB_CONTEXT\n' + production
    # With the fixed-hub SuperSpeed handover, dwc3 has left host mode once it
    # powers the USB3 PHY off or asserts its reset, and a fixed-hub host starts
    # even if its PIPE cannot be routed.
    production = ('#define STOPPED_HOST_PARKS ' + str(int('static bool atcphy_pipe_live(' in atc)) +
                  '\n#define FIXED_HOST_STARTS ' + str(int('static int atcphy_usb3_route(' in atc)) +
                  '\n' + production)
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctz(m))
#define field_prep FIELD_PREP
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define hweight32(v) __builtin_popcount(v)
#define dev_info_once(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
static void report(void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define dev_err report
#define lockdep_assert_held(...) ((void)0)
#define WARN_ON_ONCE(v) assert(!(v))
#define CONFIG_PHY_APPLE_ATC_KUNIT_TEST 0
#define IS_ENABLED(option) (option)
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define guard(kind) (void)
#define fallthrough __attribute__((fallthrough))
#define ndelay(n) ((void)(n))
#define udelay(n) ((void)(n))
#define usleep_range(a, b) ((void)(a), (void)(b))
enum { TYPEC_STATE_SAFE, TYPEC_STATE_USB, TYPEC_MODE_USB4,
       TYPEC_DP_STATE_C, TYPEC_DP_STATE_D, TYPEC_DP_STATE_E };
enum { USB_TYPEC_TBT_SID = 0x8087, USB_TYPEC_DP_SID = 0xff01 };
#define EUDO_USB_MODE_MASK 3U
enum { EUDO_USB_MODE_USB2, EUDO_USB_MODE_USB3, EUDO_USB_MODE_USB4 };
enum typec_orientation { TYPEC_ORIENTATION_NONE, TYPEC_ORIENTATION_NORMAL,
                         TYPEC_ORIENTATION_REVERSE };
enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE };
struct enter_usb_data { u32 eudo; };
struct typec_altmode { unsigned int svid; };
struct typec_mux_state { unsigned long mode; struct typec_altmode *alt; void *data; };
struct typec_mux_dev { struct apple_atcphy *data; };
struct typec_switch_dev { struct apple_atcphy *data; };
#define typec_mux_get_drvdata(mux) ((mux)->data)
#define typec_switch_get_drvdata(sw) ((sw)->data)
struct phy { struct apple_atcphy *data; };
#define phy_get_drvdata(phy) ((phy)->data)
struct reset_controller_dev { int unused; };
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
'''
    suffix = r'''
static struct atcphy_hw hw = {
    .has_t8140_dp = true, .gen = ATCPHY_GENERATION_T8122,
    .configure_post_tunable = atcphy_configure_post_tunable_all_modes,
    .aciophy_lane_mode = ACIOPHY_LANE_MODE_T8122,
    .aciophy_crossbar = ACIOPHY_CROSSBAR_T8122,
    .pipe_mux_clk = PIPEHANDLER_MUX_CTRL_CLK_T8103,
    .pipe_mux_data = PIPEHANDLER_MUX_CTRL_DATA_T8103,
};
static void cold_initial(struct apple_atcphy *p, bool fixed, bool swapped)
{
    memset(registers, 0, sizeof(registers));
    memset(pipe_regs, 0, sizeof(pipe_regs));
    memset(usb2_regs, 0, sizeof(usb2_regs));
    registers[0x200 / 4] = 0xee; /* public PMA powered state, synthetic cold fixture */
    registers[AUS_COMMON_DIG_RCAL1 / 4] = AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
    pipe_regs[PIPEHANDLER_AON_GEN / 4] = PIPEHANDLER_AON_GEN_DWC3_RESET_N;
    usb2_regs[USB2PHY_USBCTL / 4] = USB2PHY_USBCTL_RUN;
    *p = (struct apple_atcphy) {
        .t8103_usb3_workaround = hw.gen == ATCPHY_GENERATION_T8103,
        .hw = &hw, .regs.core = (unsigned char *)registers,
        .regs.lpdptx = (unsigned char *)registers,
        .regs.pipehandler = (unsigned char *)pipe_regs,
        .regs.usb2phy = (unsigned char *)usb2_regs,
        .fixed_usb2 = fixed, .host_active = true, .swap_lanes = swapped,
        .ss_tunables = true, .dp_tunables = true, .dp_presets_valid = { true },
        .mode = APPLE_ATCPHY_MODE_USB3_DP, .typec_mode = APPLE_ATCPHY_MODE_USB3_DP,
        .pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY, .mode_dirty = false,
        .dp_owned_pmas = 0, .dp_idle_pmas = 0,
        .aux_power_requested = false,
    };
    polls = fail_poll = writes = usb2_on = usb2_writes = reset_writes = mux_writes = 0;
    stuck_ack = bist_status_stuck = false;
    hold_unlock_ack = false;
    fail_caller = NULL;
    bist_faults = 0;
    require_parked = false;
}
static void initial(struct apple_atcphy *p, bool fixed, bool dirty, bool swapped)
{
    cold_initial(p, fixed, swapped);
#ifdef HAS_USB_CONTEXT
    if (fixed && hw.has_t8140_dp) {
        /* Two-PMA cleanup stress setup: capture before ownership and actual DP writes. */
        assert(j700_usb_capture(p, APPLE_ATCPHY_MODE_DP) == 0);
        assert(atcphy_configure_pipehandler(p, true) == 0);
        assert(j700_dp_start(p, 3, 3, false) == 0);
    } else
#endif
    {
        p->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
        p->dp_owned_pmas = 3; /* legacy synthetic cleanup ownership, no J700 context path */
    }
    p->mode_dirty = dirty;
    p->dp_idle_pmas = dirty ? 1 : 0;
    p->aux_power_requested = true;
    polls = fail_poll = writes = usb2_on = usb2_writes = reset_writes = mux_writes = 0;
}

static void inject_parking(unsigned int fault)
{
    pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
    if (fault)
        fail_poll = 1;
    else
        stuck_ack = true;
}
static void recover(void)
{
    stuck_ack = bist_status_stuck = false;
    hold_unlock_ack = false;
    fail_caller = NULL;
    fail_poll = polls = 0;
}
static int request(struct apple_atcphy *p, bool safe)
{
    struct typec_mux_dev mux = { p };
    struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
    struct typec_mux_state state = {
        .mode = safe ? TYPEC_STATE_SAFE : TYPEC_DP_STATE_C,
        .alt = safe ? NULL : &dp,
    };
    return atcphy_mux_set(&mux, &state);
}
/*
 * Whether a USB3 power-off or controller reset that failed leaves dwc3 a host.
 * With the fixed-hub SuperSpeed handover it has left host mode either way.
 */
#define FAILED_STOP_HOST (!STOPPED_HOST_PARKS)
/* A host start whose PIPE cannot be routed fails, except on a fixed-hub port with the handover. */
#define HOST_START_ERROR(p, err) (FIXED_HOST_STARTS && (p)->fixed_usb2 ? 0 : (err))
static void unchanged(struct apple_atcphy *p, bool dirty, bool host)
{
    bool expected_dirty = dirty || (p->hw->has_t8140_dp && p->fixed_usb2 && host);
    assert(p->mode == APPLE_ATCPHY_MODE_USB3_DP && p->mode_dirty == expected_dirty);
    assert(p->pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && p->host_active == host);
    assert(p->dp_owned_pmas == 3 && p->dp_idle_pmas == (dirty ? 1U : 0U));
    assert(p->aux_power_requested);
    assert(writes == 0 && usb2_on == 0 && usb2_writes == 0 && reset_writes == 0);
    assert(mux_writes == 0);
    assert(pipe_regs[PIPEHANDLER_AON_GEN / 4] == PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    assert(usb2_regs[USB2PHY_USBCTL / 4] == USB2PHY_USBCTL_RUN);
    if (expected_dirty) {
        struct phy_configure_opts_dp training = {
            .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
        };
        assert(j700_dp_configure(p, &training) == -EBUSY && writes == 0);
    }
}
static void observed(const char *caller, int result)
{
    printf("%s: result=%d core_writes=%u usb2_writes=%u reset_writes=%u\n",
           caller, result, writes, usb2_writes, reset_writes);
    fflush(stdout);
}
static void mux_parking(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    for (unsigned int dirty = 0; dirty < 2; dirty++)
    for (unsigned int swapped = 0; swapped < 2; swapped++)
    for (unsigned int host = 0; host < 2; host++)
    for (unsigned int safe = 0; safe < 2; safe++)
    for (unsigned int fault = 0; fault < 2; fault++) {
        initial(&p, true, dirty, swapped);
        p.host_active = host;
        inject_parking(fault);
        int ret = request(&p, safe);
        observed("mux parking failure", ret);
        assert(ret == -ETIMEDOUT);
        unchanged(&p, dirty, host);
        if (!fault) {
            assert(request(&p, safe) == -ETIMEDOUT);
            unchanged(&p, dirty, host);
        }
        recover();
        assert(request(&p, safe) == 0);
        assert(p.mode == (safe ? APPLE_ATCPHY_MODE_USB2 : APPLE_ATCPHY_MODE_DP));
        assert(!p.mode_dirty && p.host_active == host);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !p.dp_owned_pmas);
        assert(!p.dp_idle_pmas && p.aux_power_requested == !safe);
        assert(usb2_on == 1 && usb2_writes == 0 && reset_writes == 0);
        assert(usb2_regs[USB2PHY_USBCTL / 4] == USB2PHY_USBCTL_RUN);
        unsigned int before = writes;
        assert(request(&p, safe) == 0 && writes == before && usb2_on == 1);
        cases++;
    }
    printf("PASS: %u actual mux parking/retry scenarios preserve fixed USB2 and ownership\n", cases);
}
/* Explicit full-PHY teardown for tests of OFF/epoch handling. USB-only stop
 * is separately checked to preserve DP; it no longer requests this full OFF. */
static int full_power_off(struct phy *phy) {
    int ret=atcphy_usb3_power_off(phy);
    if (ret) return ret;
    return phy->data->mode != APPLE_ATCPHY_MODE_OFF || phy->data->mode_dirty ?
        atcphy_configure(phy->data, APPLE_ATCPHY_MODE_OFF) : 0;
}
static void power_parking(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    for (unsigned int fixed = 0; fixed < 2; fixed++)
    for (unsigned int dirty = 0; dirty < 2; dirty++)
    for (unsigned int swapped = 0; swapped < 2; swapped++)
    for (unsigned int fault = 0; fault < 2; fault++) {
        initial(&p, fixed, dirty, swapped);
        struct phy phy = { &p };
        inject_parking(fault);
        int ret = atcphy_usb3_power_off(&phy);
        observed("power-off parking failure", ret);
        assert(ret == -ETIMEDOUT);
        unchanged(&p, dirty, FAILED_STOP_HOST);
        if (!fault) {
            assert(atcphy_usb3_power_off(&phy) == -ETIMEDOUT);
            unchanged(&p, dirty, FAILED_STOP_HOST);
        }
        recover();
        unsigned int owned=p.dp_owned_pmas,idle=p.dp_idle_pmas; bool aux=p.aux_power_requested;
        assert(atcphy_usb3_power_off(&phy) == 0);
        assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.mode_dirty == !!dirty && !p.host_active);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
        assert(p.dp_owned_pmas==owned && p.dp_idle_pmas==idle && p.aux_power_requested==aux);
        assert(usb2_writes == 0 && reset_writes == 0);
        unsigned int before = writes;
        assert(atcphy_usb3_power_off(&phy) == 0 && writes == before);
        cases++;
    }
    printf("PASS: %u actual power-off parking/retry scenarios\n", cases);
}
static void reset_parking(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    for (unsigned int fixed = 0; fixed < 2; fixed++)
    for (unsigned int dirty = 0; dirty < 2; dirty++)
    for (unsigned int fault = 0; fault < 2; fault++) {
        initial(&p, fixed, dirty, false);
        inject_parking(fault);
        int ret = atcphy_dwc3_reset_assert(&p.rcdev, 0);
        observed("reset parking failure", ret);
        assert(ret == -ETIMEDOUT);
        unchanged(&p, dirty, FAILED_STOP_HOST);
        if (!fault) {
            assert(atcphy_dwc3_reset_assert(&p.rcdev, 0) == -ETIMEDOUT);
            unchanged(&p, dirty, FAILED_STOP_HOST);
        }
        recover();
        assert(atcphy_dwc3_reset_assert(&p.rcdev, 0) == 0);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !p.host_active);
        assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.mode_dirty == (dirty || (fixed && FAILED_STOP_HOST)));
        assert(p.dp_owned_pmas == 3 && p.dp_idle_pmas == (dirty ? 1U : 0U));
        assert(p.aux_power_requested && writes == 0);
        assert(reset_writes == 2 && usb2_writes > 0);
        assert(!(pipe_regs[PIPEHANDLER_AON_GEN / 4] & PIPEHANDLER_AON_GEN_DWC3_RESET_N));
        assert(pipe_regs[PIPEHANDLER_AON_GEN / 4] & PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
        assert(!(usb2_regs[USB2PHY_USBCTL / 4] & USB2PHY_USBCTL_RUN));
        cases++;
    }
    printf("PASS: %u actual reset parking/retry scenarios retain DP ownership\n", cases);
}
static void power_cleanup(void)
{
    struct apple_atcphy p;
    unsigned int phases = 0;
    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        initial(&p, fixed, false, false);
        p.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
        struct phy phy = { &p };
        fail_poll = 1; /* The first DP stop ACK cannot complete. */
        int ret = full_power_off(&phy);
        observed("power-off DP cleanup failure", ret);
        assert(ret == -ETIMEDOUT && p.mode_dirty && p.host_active == FAILED_STOP_HOST);
        assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.dp_owned_pmas == 3);
        assert(p.aux_power_requested && usb2_writes == 0 && reset_writes == 0);
        struct phy_configure_opts_dp training = {
            .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
        };
        unsigned int before = writes;
        assert(j700_dp_configure(&p, &training) == -EBUSY && writes == before);
        recover();
        assert(full_power_off(&phy) == 0);
        assert(!p.mode_dirty && !p.host_active && !p.dp_owned_pmas && !p.aux_power_requested);
        /* A failed initial transition can leave OFF recorded with dirty resources. */
        initial(&p, fixed, true, false);
        p.mode = APPLE_ATCPHY_MODE_OFF;
        p.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
        fail_poll = 1;
        assert(full_power_off(&phy) == -ETIMEDOUT);
        assert(p.mode_dirty && p.host_active == FAILED_STOP_HOST && p.dp_owned_pmas == 3);
        recover();
        assert(full_power_off(&phy) == 0);
        assert(p.mode == APPLE_ATCPHY_MODE_OFF && !p.mode_dirty && !p.host_active);
        assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
        for (unsigned int fault = 1; fault <= 2; fault++) {
            initial(&p, fixed, false, false);
            p.mode = APPLE_ATCPHY_MODE_USB2;
            p.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
            p.dp_owned_pmas = p.dp_idle_pmas = 0;
            p.aux_power_requested = false;
            fail_poll = fault; /* BIG/SMALL sleep ACK. */
            assert(full_power_off(&phy) == -ETIMEDOUT);
            assert(p.mode == APPLE_ATCPHY_MODE_USB2 && p.mode_dirty && p.host_active == FAILED_STOP_HOST);
            recover();
            assert(full_power_off(&phy) == 0 && !p.mode_dirty && !p.host_active);
        }
        /* Inject every poll in DP stop, pending PMA idle, AUX and domain sleep. */
        initial(&p, fixed, true, false);
        p.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
        assert(full_power_off(&phy) == 0);
        unsigned int total = polls;
        for (unsigned int fault = 1; fault <= total; fault++) {
            initial(&p, fixed, true, false);
            p.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
            fail_poll = fault;
            assert(full_power_off(&phy) == -ETIMEDOUT);
            assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.mode_dirty && p.host_active == FAILED_STOP_HOST);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
            assert(usb2_writes == 0 && reset_writes == 0);
            unsigned int owned = p.dp_owned_pmas, idle = p.dp_idle_pmas;
            bool aux = p.aux_power_requested;
            unsigned int before = writes;
            assert(j700_dp_configure(&p, &training) == -EBUSY && writes == before);
            assert(p.dp_owned_pmas == owned && p.dp_idle_pmas == idle);
            assert(p.aux_power_requested == aux);
            recover();
            assert(full_power_off(&phy) == 0);
            assert(p.mode == APPLE_ATCPHY_MODE_OFF && !p.mode_dirty && !p.host_active);
            assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
            phases++;
        }
    }
    printf("PASS: %u cleanup poll faults retain retry/ownership; dirty OFF retries cleanup\n", phases);
}
static int select_mode(struct apple_atcphy *p, enum atcphy_mode mode)
{
    struct typec_mux_dev mux = { p };
    struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
    struct typec_mux_state state = { .mode = TYPEC_STATE_SAFE };

    if (mode == APPLE_ATCPHY_MODE_USB3)
        state.mode = TYPEC_STATE_USB;
    else if (mode == APPLE_ATCPHY_MODE_USB3_DP || mode == APPLE_ATCPHY_MODE_DP) {
        state.alt = &dp;
        state.mode = mode == APPLE_ATCPHY_MODE_DP ? TYPEC_DP_STATE_C : TYPEC_DP_STATE_D;
    }
    return atcphy_mux_set(&mux, &state);
}
static void orient(struct apple_atcphy *p, bool reverse)
{
    struct typec_switch_dev sw = { p };
    assert(atcphy_sw_set(&sw, reverse ? TYPEC_ORIENTATION_REVERSE :
                                     TYPEC_ORIENTATION_NORMAL) == 0);
}
static void orientation_initial(struct apple_atcphy *p, bool fixed)
{
    cold_initial(p, fixed, false);
    p->mode = p->typec_mode = fixed ? APPLE_ATCPHY_MODE_USB2 : APPLE_ATCPHY_MODE_OFF;
    p->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
    p->dp_owned_pmas = p->dp_idle_pmas = 0;
    p->aux_power_requested = false;
    p->dp_link_rate = -1;
    p->host_active = fixed;
    registers[ACIOPHY_TOP_PHY_STAT / 4] = ACIOPHY_TOP_PHY_STAT_LN0_UNK0;
    registers[ATCPHY_MISC / 4] = BIT(7) | ATCPHY_MISC_LANE_SWAP;
    expected_j700_swap = true;
    registers[hw.aciophy_lane_mode / 4] = 0xa5000000;
    registers[hw.aciophy_crossbar / 4] = 0x80000000;
    pipe_regs[PIPEHANDLER_MUX_CTRL / 4] =
        FIELD_PREP(hw.pipe_mux_data, PIPEHANDLER_MUX_CTRL_DATA_DUMMY) |
        FIELD_PREP(hw.pipe_mux_clk, PIPEHANDLER_MUX_CTRL_CLK_DUMMY);
}
static void check_route(struct apple_atcphy *p, enum atcphy_mode mode, bool reverse)
{
    assert(p->mode == mode && p->swap_lanes == reverse && !p->mode_dirty);
    assert(registers[ATCPHY_MISC / 4] & BIT(7));
    /* Fixed public route expectations, independent of the production mode table. */
    assert(mode == APPLE_ATCPHY_MODE_USB3 || mode == APPLE_ATCPHY_MODE_USB3_DP ||
           mode == APPLE_ATCPHY_MODE_DP);
    u32 lane = registers[hw.aciophy_lane_mode / 4];
    u32 crossbar = registers[hw.aciophy_crossbar / 4];
    u32 mask = hw.has_t8140_dp && mode != APPLE_ATCPHY_MODE_DP ? 0x1ffff : 0x3ffff;
    assert((lane & 0xfff) == (mode == APPLE_ATCPHY_MODE_DP ? 0x492 :
                                                            (reverse ? 0x252 : 0x489)));
    assert((lane & 0xfffff000) == 0xa5000000);
    assert((crossbar & mask) == (mode == APPLE_ATCPHY_MODE_DP ?
                                (reverse ? 0x114 : 0x22014) : (0x110 | reverse)));
    assert(crossbar & 0x80000000);
    /* J700 preserves the boot swap bit; generic routes apply their explicit swap. */
    assert(!!(registers[ATCPHY_MISC / 4] & ATCPHY_MISC_LANE_SWAP) ==
           (hw.has_t8140_dp ? expected_j700_swap :
                              (mode != APPLE_ATCPHY_MODE_DP && reverse)));
    assert(p->pipe_state == (p->fixed_usb2 && mode != APPLE_ATCPHY_MODE_DP ?
                            ATCPHY_PIPEHANDLER_STATE_USB3 : ATCPHY_PIPEHANDLER_STATE_DUMMY));
}
static void orientation_roundtrip(void)
{
    struct apple_atcphy p;
    const enum atcphy_mode modes[] = {
        APPLE_ATCPHY_MODE_USB3, APPLE_ATCPHY_MODE_USB3_DP, APPLE_ATCPHY_MODE_DP,
    };
    unsigned int cases = 0;

    for (unsigned int generation = 0; generation < 4; generation++) {
        hw.gen = generation == 0 ? ATCPHY_GENERATION_T8103 : ATCPHY_GENERATION_T8122;
        hw.configure_post_tunable = generation == 0 ? atcphy_configure_post_tunable_t8103 :
                                    generation == 1 ? atcphy_configure_post_tunable_t8122 :
                                    atcphy_configure_post_tunable_all_modes;
        hw.has_t8140_dp = generation == 2;
        hw.has_usb4 = !hw.has_t8140_dp;
        hw.aciophy_lane_mode = generation == 0 ? ACIOPHY_LANE_MODE_T8103 :
                                                ACIOPHY_LANE_MODE_T8122;
        hw.aciophy_crossbar = generation == 0 ? ACIOPHY_CROSSBAR_T8103 :
                                               ACIOPHY_CROSSBAR_T8122;
        hw.pipe_mux_clk = generation == 3 ? PIPEHANDLER_MUX_CTRL_CLK_T8142 :
                                           PIPEHANDLER_MUX_CTRL_CLK_T8103;
        hw.pipe_mux_data = generation == 3 ? PIPEHANDLER_MUX_CTRL_DATA_T8142 :
                                            PIPEHANDLER_MUX_CTRL_DATA_T8103;
        for (unsigned int fixed = 0; fixed < 2; fixed++) {
            for (unsigned int m = 0; m < ARRAY_SIZE(modes); m++) {
                for (unsigned int first = 0; first < 2; first++) {
                    orientation_initial(&p, fixed);
                    for (unsigned int turn = 0; turn < 3; turn++) {
                        bool reverse = first ^ (turn & 1);
                        orient(&p, reverse);
                        if (turn) {
                            assert(p.mode_dirty);
                            struct phy phy = { &p };
                            unsigned int before = writes;
                            assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == HOST_START_ERROR(&p, -EBUSY));
                            assert(writes == before);
                        }
                        require_parked = true;
                        assert(select_mode(&p, modes[m]) == 0);
                        require_parked = false;
                        check_route(&p, modes[m], reverse);
                        unsigned int before = writes, before_mux = mux_writes;
                        orient(&p, reverse);
                        assert(select_mode(&p, modes[m]) == 0);
                        assert(writes == before && mux_writes == before_mux);
                        struct typec_switch_dev sw = { &p };
                        assert(atcphy_sw_set(&sw, TYPEC_ORIENTATION_NONE) == 0);
                        assert(!p.mode_dirty && p.swap_lanes == reverse);
                        cases++;
                    }
                    /* A USB3 protocol change also rebuilds the live backend. */
                    if (m == 0) {
                        require_parked = true;
                        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
                        require_parked = false;
                        check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, first);
                    }
                    assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == 0);
                    assert(p.mode == (fixed ? APPLE_ATCPHY_MODE_USB2 : APPLE_ATCPHY_MODE_OFF));
                    assert(!p.mode_dirty && !p.aux_power_requested && !p.dp_idle_pmas);
                    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
                }
            }
        }
    }
    printf("PASS: %u production orientation routes across T8103/T8122/T8140/T8142, fixed/ordinary ports, both flip roundtrips\n", cases);
}
static void orientation_faults(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;

    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        for (unsigned int first = 0; first < 2; first++) {
            orientation_initial(&p, fixed);
            orient(&p, first);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            struct phy_configure_opts_dp rate = {
                .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
            };
            assert(j700_dp_configure(&p, &rate) == 0);
            assert(p.dp_owned_pmas == BIT(!first));
            orient(&p, !first);
            unsigned int before = writes;
            struct phy_configure_opts_dp training = { .set_voltages = true, .lanes = 2 };
            assert(j700_dp_configure(&p, &training) == -EBUSY && writes == before);
            assert(j700_dp_configure(&p, &rate) == -EBUSY && writes == before);
            /* Derive all actual poll phases, then inject each in a fresh transition. */
            polls = 0;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            unsigned int total = polls;
            bool advisory[256] = { false };
            for (unsigned int i = 0; i < total; i++) {
                const char *caller = poll_caller[i];
                /* Every observed BIST and USB3 handoff failure is now fatal. */
                if (fixed && i + 1 == total) {
                    assert(!strcmp(caller, "atcphy_pipehandler_unlock"));
                    advisory[i] = false;
                } else if (!strcmp(caller, "atcphy_configure_pipehandler_usb3_host_t8122")) {
                    assert(fixed);
                    advisory[i] = false;
                } else {
                    assert(!strcmp(caller, "j700_dp_wait") ||
                           !strcmp(caller, "j700_field_update") ||
                           !strcmp(caller, "j700_usb_field_update") ||
                           !strcmp(caller, "atcphy_power_on") ||
                           !strcmp(caller, "atcphy_configure") ||
                           (fixed && (!strcmp(caller, "atcphy_pipehandler_lock") ||
                                      !strcmp(caller, "atcphy_pipehandler_unlock"))));
                }
            }
            for (unsigned int fault = 1; fault <= total; fault++) {
                orientation_initial(&p, fixed);
                orient(&p, first);
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
                assert(j700_dp_configure(&p, &rate) == 0);
                unsigned int owned = p.dp_owned_pmas;
                orient(&p, !first);
                polls = 0;
                fail_poll = fault;
                int ret = select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP);
                /* A fatal phase must never be accepted as advisory success. */
                assert((ret == 0) == advisory[fault - 1]);
                if (!ret) {
                    check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, !first);
                    assert(!p.dp_owned_pmas && !p.dp_idle_pmas);
                    cases++;
                    continue;
                }
                assert(ret == -ETIMEDOUT);
                if (p.mode_dirty || p.pipe_pending) {
                    before = writes;
                    assert(j700_dp_configure(&p, &training) == -EBUSY && writes == before);
                    assert(j700_dp_configure(&p, &rate) == -EBUSY && writes == before);
                    if (fault == 1 && !fixed)
                        assert(p.dp_owned_pmas == owned && p.aux_power_requested);
                } else {
                    assert(fixed && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
                }
                recover();
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
                check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, !first);
                assert(!p.dp_owned_pmas && !p.dp_idle_pmas);
                assert(j700_dp_configure(&p, &rate) == 0);
                assert(p.dp_owned_pmas == BIT(first));
                assert(j700_dp_configure(&p, &training) == 0);
                cases++;
            }
            /* A fatal parking check preserves the old owner and all lane words. */
            if (fixed) {
                orientation_initial(&p, true);
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
                assert(j700_dp_configure(&p, &rate) == 0);
                orient(&p, true);
                before = writes;
                inject_parking(0);
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
                assert(writes == before && p.dp_owned_pmas == 2 && p.mode_dirty);
                assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && p.host_active);
                recover();
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
                check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, true);
            }
            /* Reversing a pending flip cannot erase cleanup obligations. */
            orientation_initial(&p, fixed);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(j700_dp_configure(&p, &rate) == 0);
            orient(&p, true);
            orient(&p, false);
            assert(p.mode_dirty && p.dp_owned_pmas == 2);
            p.ss_tunables = false;
            before = writes;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -EOPNOTSUPP);
            assert(writes == before && p.mode_dirty && p.dp_owned_pmas == 2);
            p.ss_tunables = true;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, false);
            assert(!p.dp_owned_pmas && !p.dp_idle_pmas);
        }
    }
    printf("PASS: %u orientation transition poll injections, dirty training guard, ownership and retry controls\n", cases);
}
static void backend_change(void)
{
    struct apple_atcphy p;

    orientation_initial(&p, true);
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3) == 0);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    require_parked = true;
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    require_parked = false;
    check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, false);
    puts("PASS: a live fixed-hub USB3 backend parks before same-orientation protocol changes");
}


static void failed_flip_return(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    struct phy_configure_opts_dp rate = {
        .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
    };

    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        for (unsigned int first = 0; first < 2; first++) {
            orientation_initial(&p, fixed);
            orient(&p, first);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(j700_dp_configure(&p, &rate) == 0);
            orient(&p, !first);
            polls = 0;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            unsigned int route_fault = 0;
            for (unsigned int i = 0; i < polls; i++)
                if (!strcmp(poll_caller[i], "j700_field_update")) route_fault = i + 1;
            assert(route_fault);

            orientation_initial(&p, fixed);
            orient(&p, first);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(j700_dp_configure(&p, &rate) == 0);
            orient(&p, !first);
            polls = 0;
            fail_poll = route_fault;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
            /* The route changed before PMA idle completed; completed mode is still old. */
            assert((registers[0x60 / 4] & 0xfff) == (first ? 0x489 : 0x252));
            assert(p.mode_dirty && p.dp_idle_pmas == BIT(first) && !p.dp_owned_pmas);
            orient(&p, first);
            assert(p.mode_dirty && p.dp_idle_pmas == BIT(first));
            unsigned int before = writes;
            assert(j700_dp_configure(&p, &rate) == -EBUSY && writes == before);
            recover();
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, first);
            assert(!p.dp_idle_pmas && !p.dp_owned_pmas);
            assert(j700_dp_configure(&p, &rate) == 0);
            assert(p.dp_owned_pmas == BIT(!first));
            cases++;
        }
    }
    printf("PASS: %u partially applied flip cancellations preserve pending PMA ownership\n", cases);
}

static void safe_first_recovery(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;

    for (unsigned int first = 0; first < 2; first++) {
        orientation_initial(&p, true);
        orient(&p, first);
        bist_status_stuck = true;
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3) == -ETIMEDOUT);
        assert(bist_faults == 2 && p.mode_dirty && p.pipe_pending);
        bist_status_stuck = false;
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3) == 0);
        check_route(&p, APPLE_ATCPHY_MODE_USB3, first);
        polls = 0;
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
        unsigned int host_lock_poll = 0;
        for (unsigned int i = 0; i < polls; i++)
            if (!strcmp(poll_caller[i], "atcphy_pipehandler_lock")) host_lock_poll = i + 1;
        assert(host_lock_poll > 1); /* Inject the host lock, after acknowledged parking. */
        orientation_initial(&p, true);
        orient(&p, first);
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3) == 0);
        polls = 0;
        fail_poll = host_lock_poll;
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
        assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && !p.mode_dirty);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && p.host_active);
        assert(p.aux_power_requested);

        /* Persistent AUX cleanup failure blocks SAFE before orientation publication. */
        fail_caller = "j700_field_update";
        u32 route = registers[0x60 / 4], crossbar = registers[0x64 / 4];
        for (unsigned int attempt = 0; attempt < 3; attempt++) {
            assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == -ETIMEDOUT);
            assert(p.swap_lanes == first && p.mode_dirty && p.aux_power_requested);
            assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.host_active);
            assert(registers[0x60 / 4] == route && registers[0x64 / 4] == crossbar);
            assert(usb2_regs[USB2PHY_USBCTL / 4] == USB2PHY_USBCTL_RUN);
        }
        /* Returning the plug cannot clear an unavailable cleanup handshake. */
        assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == -ETIMEDOUT);
        assert(p.swap_lanes == first && p.mode_dirty);
        recover(); /* Model hardware readiness returning; production performs the retry. */
        assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == 0);
        assert(p.mode == APPLE_ATCPHY_MODE_USB2 && !p.mode_dirty && !p.aux_power_requested);
        /* SAFE uses the existing generic USB2 route and clears its swap control. */
        assert(!(registers[ATCPHY_MISC / 4] & ATCPHY_MISC_LANE_SWAP));
        expected_j700_swap = false;
        orient(&p, !first);
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
        check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, !first);

        /* Real reconnect order: SAFE must succeed before the new orientation is published. */
        struct phy_configure_opts_dp rate = {
            .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
        };
        assert(j700_dp_configure(&p, &rate) == 0);
        assert(p.dp_owned_pmas == BIT(first));
        assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == 0);
        assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
        orient(&p, first);
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
        check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, first);
        assert(j700_dp_configure(&p, &rate) == 0);
        assert(p.dp_owned_pmas == BIT(!first));
        unsigned int before = writes, before_mux = mux_writes;
        orient(&p, first);
        assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
        assert(writes == before && mux_writes == before_mux);
        cases++;
    }
    printf("PASS: %u chained BIST/host-ACK failure, persistent SAFE failure and SAFE-first roundtrip controls\n", cases);
}

static void live_parking_handshake(void)
{
    struct apple_atcphy p;
    struct phy_configure_opts_dp rate = {
        .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
    };
    struct phy_configure_opts_dp training = { .set_voltages = true, .lanes = 2 };
    unsigned int cases = 0;
    for (unsigned int reverse = 0; reverse < 2; reverse++) {
        for (unsigned int fault = 0; fault < 3; fault++) {
            orientation_initial(&p, true);
            orient(&p, reverse);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(j700_dp_configure(&p, &rate) == 0);
            u32 lanes = registers[0x60 / 4], route = registers[0x64 / 4];
            u32 native = pipe_regs[PIPEHANDLER_NONSELECTED_OVERRIDE / 4];
            unsigned int before = writes, owned = p.dp_owned_pmas;
            if (fault == 2) {
                /* A previous host unlock left ACK asserted before parking. */
                pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
                hold_unlock_ack = true;
            } else if (fault == 1) {
                hold_unlock_ack = true;
            } else {
                stuck_ack = true;
            }
            for (unsigned int retry = 0; retry < 3; retry++) {
                unsigned int before_mux = mux_writes;
                assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == -ETIMEDOUT);
                assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.mode_dirty);
                assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && p.host_active);
                assert(p.dp_owned_pmas == owned && p.aux_power_requested);
                assert(writes == before && registers[0x60 / 4] == lanes && registers[0x64 / 4] == route);
                assert(pipe_regs[PIPEHANDLER_NONSELECTED_OVERRIDE / 4] == native);
                if (fault != 1) assert(mux_writes == before_mux);
                /* A failed SAFE cannot be forgotten by returning to the cached mode. */
                assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
                assert(j700_dp_configure(&p, &training) == -EBUSY && writes == before);
                assert(!usb2_writes && !reset_writes);
            }
            recover();
            assert(select_mode(&p, APPLE_ATCPHY_MODE_OFF) == 0);
            assert(p.mode == APPLE_ATCPHY_MODE_USB2 && !p.mode_dirty && p.host_active);
            assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
            expected_j700_swap = false;
            orient(&p, !reverse);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            check_route(&p, APPLE_ATCPHY_MODE_USB3_DP, !reverse);
            assert(j700_dp_configure(&p, &rate) == 0 && p.dp_owned_pmas == BIT(reverse));
            cases++;
        }
    }
    printf("PASS: %u live parking entry-ACK/LOCK/UNLOCK failure, ownership, write ordering and retry roundtrips\n", cases);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "mux")) mux_parking();
    else if (!strcmp(argv[1], "power")) power_parking();
    else if (!strcmp(argv[1], "reset")) reset_parking();
    else if (!strcmp(argv[1], "cleanup")) power_cleanup();
    else if (!strcmp(argv[1], "orientation")) orientation_roundtrip();
    else if (!strcmp(argv[1], "orientation-faults")) orientation_faults();
    else if (!strcmp(argv[1], "backend-change")) backend_change();
    else if (!strcmp(argv[1], "safe-first")) safe_first_recovery();
    else if (!strcmp(argv[1], "live-park")) live_parking_handshake();
    else if (!strcmp(argv[1], "failed-flip-return")) failed_flip_return();
    else assert(false);
    return 0;
}
'''
    return {**locals(), "block": block}

def adverse(root):

    # Preserve continued source macros; the parking fixture does not use the
    # continued PLL clock macro exercised by the legacy rate helper here.
    ns = parking(root)
    production = ns['production'] + '\n' + ns['function'](ns['atc'], 'atcphy_usb2_init')
    # An admitted base-M1 host start may skip the native BIST, and the lock it holds.
    dispatch = ns['function'](ns['atc'], 'atcphy_configure_pipehandler')
    production = ('#define T8103_HOST_BIST ' + str(int('ATCPHY_GENERATION_T8103' not in dispatch and
                  't8103_usb3_workaround' not in dispatch)) +
                  '\n' + production)
    production = production.replace('bool pipe_pending, usb_off_pending, usb_off_drained;',
                                    'bool pipe_pending, usb_off_pending, usb_off_drained, tunnel_saved;', 1)
    production += '\n' + ns['block'](ns['atc'], 'enum atcphy_lane {') + ';'
    production += '\n' + ns['block'](ns['atc'], 'struct atcphy_dp_link_rate_configuration {') + ';'
    production += '\n' + ns['block'](ns['atc'], 'static const struct atcphy_dp_link_rate_configuration dp_lr_config[] = {') + ';'
    # These leaf operations are outside the pending/cache gate under test.
    production += r'''
static int atcphy_auspll_apb_command(struct apple_atcphy *p, u32 command)
{ (void)p; (void)command; assert(false); return -EIO; }
static int atcphy_dp_configure_lane(struct apple_atcphy *p, enum atcphy_lane lane,
                                  const struct atcphy_dp_link_rate_configuration *cfg)
{ (void)p; (void)lane; (void)cfg; assert(false); return -EIO; }
'''
    production += '\n' + ns['function'](ns['atc'], 'atcphy_dp_configure')
    for profile in ('t8103', 't8122', 't8140', 't8142'):
        production += '\n' + ns['block'](ns['atc'],
            'static const struct atcphy_hw atcphy_hw_' + profile + ' = {') + ';'
    production = production.replace('static unsigned int polls,', 'static int poll_errors[256];\nstatic unsigned int polls,', 1)
    production = production.replace('polls == fail_poll || bist_fault ||',
                                    'poll_errors[polls] ? poll_errors[polls] : polls == fail_poll || bist_fault ||', 1)
    production = production.replace('static u32 readl(void *address) { return *(u32 *)address; }',
                                    '''static unsigned int mmio_reads;
static bool only_power_mmio;
static u32 readl(void *address)
{
    mmio_reads++;
    if (only_power_mmio && (uintptr_t)address >= (uintptr_t)registers &&
        (uintptr_t)address < (uintptr_t)registers + sizeof(registers)) {
        uintptr_t offset=(uintptr_t)address-(uintptr_t)registers;
        assert(offset==ATCPHY_POWER_CTRL || offset==ATCPHY_POWER_STAT || offset==ATCPHY_MISC);
    }
    return *(u32 *)address;
}''', 1)
    production = production.replace('    (void)p;\n    assert(offset < sizeof(registers)',
                                    '    (void)p;\n    if (only_power_mmio) assert(offset==ATCPHY_POWER_CTRL || offset==ATCPHY_MISC);\n    assert(offset < sizeof(registers)', 1)
    controls = r'''
static void legacy_setup(struct apple_atcphy *p, unsigned int gen, bool fixed)
{
    hw = gen==0 ? atcphy_hw_t8103 : gen==1 ? atcphy_hw_t8122 :
         gen==2 ? atcphy_hw_t8140 : atcphy_hw_t8142;
    cold_initial(p, fixed, false);
    p->mode = p->typec_mode = APPLE_ATCPHY_MODE_USB3;
    registers[ACIOPHY_TOP_PHY_STAT/4] = ACIOPHY_TOP_PHY_STAT_LN0_UNK0;
    memset(poll_errors, 0, sizeof(poll_errors));
}
static void shared_bist(void)
{
    for (unsigned int gen=0; gen<2; gen++) {
        unsigned int count=gen ? 5 : 3;
        for (unsigned int fault=1; fault<=count; fault++) {
            struct apple_atcphy p;
            legacy_setup(&p,gen,false);
            poll_errors[fault]=-EIO;
            if (fault<count) poll_errors[fault+1]=-ETIMEDOUT;
            int ret=gen ? atcphy_configure_pipehandler_usb3_host_t8122(&p) :
                          atcphy_configure_pipehandler_usb3_host_t8103(&p);
            assert(ret==-EIO && polls==count);
            if (gen) assert(!(registers[ACIOPHY_BIST_CFG1/4] &
                              (ACIOPHY_BIST_CFG1_CLOCK_EN|ACIOPHY_BIST_CFG1_USB_EN)));
        }
    }
    puts("PASS: every T8103/T8122 BIST poll propagates first error through full cleanup");
}
#if !T8103_HOST_BIST
static bool bist_polled(unsigned int from)
{
    for (unsigned int i=from; i<polls; i++)
        if (!strcmp(poll_caller[i],"atcphy_configure_pipehandler_usb3_host_t8103") ||
            !strcmp(poll_caller[i],"atcphy_configure_pipehandler_usb3_host_t8122"))
            return true;
    return false;
}
static bool only_released(unsigned int from)
{
    for (unsigned int i=from; i<polls; i++)
        if (strcmp(poll_caller[i],"atcphy_pipehandler_unlock"))
            return false;
    return true;
}
/*
 * A T8103-generation host start runs no native BIST and holds no lock. Its
 * remaining PIPE handshake is the release of a stale lock in
 * atcphy_pipehandler_check(), so that release is where the fault goes: not
 * acknowledged (unlock) or a failed poll. The start fails at that first poll,
 * before the PIPE moves and with the lanes untouched. A lock that stays held
 * fails the next start the same way. Once it lets go, the next start parks
 * the PIPE and routes USB3, and no BIST poll runs in the whole sequence.
 */
static void t8103_release_fault(struct apple_atcphy *p, struct phy *phy, bool fixed,
                                bool unlock, bool mux)
{
    const bool starts = FIXED_HOST_STARTS && fixed;
    unsigned int before=writes;

    pipe_regs[PIPEHANDLER_LOCK_ACK/4]=PIPEHANDLER_LOCK_EN;
    if (unlock) hold_unlock_ack=true;
    else poll_errors[1]=-EIO;
    int ret=atcphy_usb3_set_mode(phy,PHY_MODE_USB_HOST,0);
    printf("t8103 fixed=%u unlock=%u mux=%u ret=%d polls=%u first=%s\n",
           fixed,unlock,mux,ret,polls,polls ? poll_caller[0] : "none");fflush(stdout);
    assert(polls>=1 && !strcmp(poll_caller[0],"atcphy_pipehandler_unlock"));
    if (starts) {
        /* The fixed-hub host starts anyway; its park repeats the release. */
        assert(ret==0);
        if (unlock) {
            assert(polls==2 && only_released(0) && p->pipe_pending && mux_writes==0);
        } else {
            assert(polls==3 && !strcmp(poll_caller[1],"atcphy_pipehandler_lock") &&
                   !strcmp(poll_caller[2],"atcphy_pipehandler_unlock"));
            assert(!p->pipe_pending && mux_writes==3);
        }
    } else {
        assert(ret==(unlock ? -ETIMEDOUT : -EIO) && polls==1);
        assert(p->pipe_pending && mux_writes==0);
    }
    assert(p->pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY && !p->mode_dirty && writes==before);

    /* A lock that stays held fails the next start at the same release. */
    hold_unlock_ack=false;
    memset(poll_errors,0,sizeof(poll_errors));
    pipe_regs[PIPEHANDLER_LOCK_ACK/4]=PIPEHANDLER_LOCK_EN;
    stuck_ack=true;
    unsigned int first=polls, moved=mux_writes;
    ret=atcphy_usb3_set_mode(phy,PHY_MODE_USB_HOST,0);
    assert(ret==(starts ? 0 : -ETIMEDOUT) && polls>first && only_released(first));
    assert(mux_writes==moved && p->pipe_pending);
    assert(p->pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY && !p->mode_dirty);

    /* The lanes were never touched: the callback rebuilds nothing. */
    stuck_ack=false;
    ret=mux ? select_mode(p,APPLE_ATCPHY_MODE_USB3) : atcphy_usb2_init(phy);
    assert(ret==0 && writes==before && !p->mode_dirty);
    assert(atcphy_usb3_set_mode(phy,PHY_MODE_USB_HOST,0)==0);
    assert(p->pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3 && !p->pipe_pending);
    assert(!p->mode_dirty && writes==before);
    assert(!(pipe_regs[PIPEHANDLER_LOCK_REQ/4] & PIPEHANDLER_LOCK_EN) && !bist_polled(0));
}
#endif
static void retry_callbacks(bool mux)
{
    for (unsigned int gen=0; gen<4; gen++)
    for (unsigned int fixed=0; fixed<2; fixed++)
    for (unsigned int unlock=0; unlock<2; unlock++) {
        struct apple_atcphy p;
        legacy_setup(&p,gen,fixed);
        struct phy phy={&p};
#if !T8103_HOST_BIST
        if (gen==0) {
            t8103_release_fault(&p,&phy,fixed,unlock,mux);
            continue;
        }
#endif
        if (unlock) hold_unlock_ack=true;
        else poll_errors[2]=-EIO;
#if FIXED_HOST_STARTS
        if (fixed) {
            /* A fixed-hub host starts, with its PIPE parked if the lock lets go. */
            assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==0);
            assert(p.mode_dirty && p.pipe_pending==(bool)unlock);
            if (!unlock) {
                /* Parked: the next mux update restores the lanes and routes the PIPE. */
                assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
                memset(poll_errors,0,sizeof(poll_errors));
                assert((mux ? select_mode(&p,APPLE_ATCPHY_MODE_USB3) : atcphy_usb2_init(&phy))==0);
                assert(!p.mode_dirty && !p.pipe_pending);
                assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==0);
                assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3 && !p.pipe_pending);
                continue;
            }
        } else
#endif
        assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==
               (unlock ? -ETIMEDOUT : -EIO));
        assert(p.mode_dirty && p.pipe_pending);
        unsigned int before=writes, on=usb2_on;
        hold_unlock_ack=false;
        memset(poll_errors,0,sizeof(poll_errors));
        stuck_ack=true;
        int ret=mux ? select_mode(&p,APPLE_ATCPHY_MODE_USB3) : atcphy_usb2_init(&phy);
        printf("gen=%u fixed=%u unlock=%u mux=%u ret=%d new_core_writes=%u\n",
               gen,fixed,unlock,mux,ret,writes-before);fflush(stdout);
        assert(ret==-ETIMEDOUT && writes==before && usb2_on==on);
        assert(p.pipe_pending && p.mode_dirty);
        stuck_ack=false;
        assert((mux ? select_mode(&p,APPLE_ATCPHY_MODE_USB3) : atcphy_usb2_init(&phy))==0);
        assert(!p.mode_dirty && !p.pipe_pending);
        assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==0);
        assert(p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3 && !p.pipe_pending);
    }
    puts("PASS: all matched hardware profiles fixed/ordinary BIST and unlock failures require acknowledged parking before callback lane recovery");
}
static void successful_scope(void)
{
    for (unsigned int gen=0; gen<4; gen++)
    for (unsigned int fixed=0; fixed<2; fixed++)
    for (unsigned int host=0; host<2; host++) {
        struct apple_atcphy p;
        legacy_setup(&p,gen,fixed);
        struct phy phy={&p};
        assert(atcphy_usb3_set_mode(&phy,host ? PHY_MODE_USB_HOST : PHY_MODE_USB_DEVICE,0)==0);
        unsigned int before=polls, w=writes, m=mux_writes;
        assert(atcphy_usb3_set_mode(&phy,host ? PHY_MODE_USB_HOST : PHY_MODE_USB_DEVICE,0)==0);
        assert(polls==before && writes==w && mux_writes==m);
        if (!host) assert(before==0);
        assert(!p.mode_dirty && !p.pipe_pending);
    }
    puts("PASS: all matched hardware profiles fixed/ordinary host/device success and duplicate cache controls");
}

static void post_tunable_profiles(void)
{
    /*
     * Asahi 7.2 per-SoC setup after the tunables: T8103 sets its overrides in
     * every mode and never waits for RCAL; T8122 wakes its common block and
     * now also wakes/calibrates DP for its T8122 DP backend; T8140/T8142 do both in every
     * mode, DisplayPort included, as on 7.1.
     */
    const enum atcphy_mode modes[]={APPLE_ATCPHY_MODE_USB3,APPLE_ATCPHY_MODE_USB3_DP,
                                    APPLE_ATCPHY_MODE_DP};
    for (unsigned int gen=0; gen<4; gen++)
    for (unsigned int m=0; m<ARRAY_SIZE(modes); m++) {
        struct apple_atcphy p;
        legacy_setup(&p,gen,false);
        unsigned int before=polls, rcal=0;
        assert(atcphy_configure(&p,modes[m])==0);
        for (unsigned int i=before; i<polls; i++)
            rcal+=!strcmp(poll_caller[i],"atcphy_configure");
        bool usb3=atcphy_modes[modes[m]].pipehandler_state==ATCPHY_PIPEHANDLER_STATE_USB3;
        bool wake=gen>=1;
        if(gen==1 && !usb3) assert(p.dp_t8122.aux && atc_t8122_dp_aux_is_on(p.regs.core));
        assert(rcal==wake);
        assert(!!(registers[AUS_COMMON_SHIM_BLK_BIAS_REG/4]&AUS_COMMON_SHIM_BLK_BIAS_REG_BGBIAS_OV)==wake);
        assert(!!(registers[ACIOPHY_SLEEP_CTRL/4]&ACIOPHY_SLEEP_CTRL_TX_CLAMP_OV)==(gen==0));
    }
    puts("PASS: actual per-SoC post-tunable callbacks and RCAL wait for every hardware profile");
}

static void pending_gate(void)
{
    for (unsigned int gen=0;gen<2;gen++) {
        struct apple_atcphy p;
        legacy_setup(&p,gen,false);
        struct phy phy={&p};
        const char *fault="atcphy_pipehandler_lock";
#if !T8103_HOST_BIST
        /* T8103 host start takes no lock: fail the release of a stale one instead. */
        if (gen==0) {
            pipe_regs[PIPEHANDLER_LOCK_ACK/4]=PIPEHANDLER_LOCK_EN;
            fault="atcphy_pipehandler_unlock";
        }
#endif
        stuck_ack=true;
        assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-ETIMEDOUT);
        assert(polls==1 && !strcmp(poll_caller[0],fault));
        assert(p.pipe_pending && !p.mode_dirty);
        unsigned int w=writes,r=mmio_reads;
        int ret=atcphy_configure(&p,APPLE_ATCPHY_MODE_USB3);
        printf("pending configure gen=%u ret=%d new_writes=%u new_reads=%u\n",
               gen,ret,writes-w,mmio_reads-r);fflush(stdout);
        assert(ret==-EBUSY && writes==w && mmio_reads==r);
        p.dp_link_rate=ATCPHY_DP_LINK_RATE_HBR3;
        assert(atcphy_dp_configure(&p,ATCPHY_DP_LINK_RATE_HBR3)==-EBUSY);
        assert(writes==w && mmio_reads==r);
    }
    puts("PASS: shared configure rejects pending PIPE before any core MMIO");
}
static void off_both_pmas(void)
{
    hw=atcphy_hw_t8140;
    unsigned int shutdown_poll[2],count=0;
    struct apple_atcphy p;
    initial(&p,true,false,false);
    struct phy phy={&p};
    assert(p.dp_usb_context_valid==3 && p.dp_usb_context_dirty==3);
    assert(full_power_off(&phy)==0);
    for (unsigned int i=0;i<polls;i++)
        if (!strcmp(poll_caller[i],"atcphy_power_off")) shutdown_poll[count++]=i+1;
    assert(count==2);
    for (unsigned int phase=0;phase<2;phase++) {
        initial(&p,true,false,false);
        u32 saved[2][27];memcpy(saved,p.dp_usb_context,sizeof(saved));
        fail_poll=shutdown_poll[phase];
        assert(full_power_off(&phy)==-ETIMEDOUT);
        assert(p.usb_off_pending && p.usb_off_drained && p.mode_dirty);
        assert(p.dp_usb_context_valid==3 && p.dp_usb_context_dirty==3);
        assert(!memcmp(saved,p.dp_usb_context,sizeof(saved)));
        assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
        unsigned int w=writes,r=mmio_reads;
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_OFF)==0);
        assert(w==writes && r==mmio_reads);
        p.pipe_pending=true;
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_OFF)==-EBUSY);
        p.pipe_pending=false;p.dp_owned_pmas=1;
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_OFF)==-EBUSY);
        p.dp_owned_pmas=0;
        u32 marker[2]={0x159a7531,0x246bc842};
        registers[0x9244/4]=marker[0];registers[0x10244/4]=marker[1];
        fail_poll=0;
        only_power_mmio=true;
        assert(full_power_off(&phy)==0);
        only_power_mmio=false;
        assert(registers[0x9244/4]==marker[0] && registers[0x10244/4]==marker[1]);
        assert(!p.dp_usb_context_valid && !p.dp_usb_context_dirty && !p.usb_off_pending);
        w=writes;r=mmio_reads;
        assert(full_power_off(&phy)==0);
        assert(w==writes && r==mmio_reads);
    }
    puts("PASS: simultaneous both-PMA drain, big/small OFF failures retain shadows, drained retry gates/no PMA MMIO/no replay, completed OFF is zero MMIO");
}


static void dp_cache_gate(void)
{
    for (unsigned int gen=0;gen<2;gen++) {
        struct apple_atcphy p;
        legacy_setup(&p,gen,false);
        struct phy phy={&p};
        const char *fault="atcphy_pipehandler_lock";
#if !T8103_HOST_BIST
        /* T8103 host start takes no lock: fail the release of a stale one instead. */
        if (gen==0) {
            pipe_regs[PIPEHANDLER_LOCK_ACK/4]=PIPEHANDLER_LOCK_EN;
            fault="atcphy_pipehandler_unlock";
        }
#endif
        stuck_ack=true;
        assert(atcphy_usb3_set_mode(&phy,PHY_MODE_USB_HOST,0)==-ETIMEDOUT);
        assert(polls==1 && !strcmp(poll_caller[0],fault));
        assert(p.pipe_pending && !p.mode_dirty);
        p.dp_link_rate=ATCPHY_DP_LINK_RATE_HBR3;
        unsigned int w=writes,r=mmio_reads;
        int ret=atcphy_dp_configure(&p,ATCPHY_DP_LINK_RATE_HBR3);
        printf("pending legacy DP cache gen=%u ret=%d\n",gen,ret);fflush(stdout);
        assert(ret==-EBUSY && writes==w && mmio_reads==r);
    }
    puts("PASS: legacy DP rate cache cannot accept pending PIPE as ready");
}

int main(int argc,char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"dp-cache")) dp_cache_gate();
    else if (!strcmp(argv[1],"off")) off_both_pmas();
    else if (!strcmp(argv[1],"gate")) pending_gate();
    else if (!strcmp(argv[1],"bist")) shared_bist();
    else if (!strcmp(argv[1],"init")) retry_callbacks(false);
    else if (!strcmp(argv[1],"mux")) retry_callbacks(true);
    else if (!strcmp(argv[1],"success")) successful_scope();
    else if (!strcmp(argv[1],"post-tunable")) post_tunable_profiles();
    else assert(false);
    return 0;
}
'''
    return ns['prefix'] + '\ntypedef uint8_t u8;\n' + production + ns['suffix'].replace('int main(', 'int original_fixture_main(', 1) + controls

def hardening(root):
    ns = parking(root)
    production=(ns['production']+'\n'+ns['function'](ns['atc'],'atcphy_usb2_init')).replace('static u32 registers[','static u32 drop_offset;\nstatic unsigned int mmio_reads;\nstatic u32 registers[',1)
    production=production.replace('static u32 readl(void *address) { return *(u32 *)address; }','static u32 readl(void *address) { mmio_reads++; return *(u32 *)address; }',1)
    production=production.replace('    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;','    if (drop_offset && offset == drop_offset) { writes++; return; }\n    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;',1)
    controls=r'''
static const struct phy_configure_opts_dp rate={.set_rate=true,.set_lanes=true,.link_rate=8100,.lanes=2};
static void setup(struct apple_atcphy *p, unsigned int first)
{
    orientation_initial(p,true);
    for (unsigned int o=0x9000;o<0x15000;o+=4) registers[o/4]=0x125a7000^o;
    orient(p,first);
    assert(select_mode(p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(j700_dp_configure(p,&rate)==0);
}
/* A full PHY OFF request, after USB host teardown, ends the saved epoch. */
static int epoch_power_off(struct phy *phy) {
    int ret=atcphy_usb3_power_off(phy);
    return ret ? ret : (phy->data->mode != APPLE_ATCPHY_MODE_OFF || phy->data->mode_dirty ? atcphy_configure(phy->data, APPLE_ATCPHY_MODE_OFF) : 0);
}
static void epoch_controls(void)
{
    for(unsigned int first=0;first<2;first++) {
        struct apple_atcphy p;setup(&p,first);
        unsigned int lane=!first;
        u32 saved[27];memcpy(saved,p.dp_usb_context[lane],sizeof(saved));
        struct phy phy={&p};
        unsigned int preserved=p.dp_usb_context_valid;
        assert(atcphy_usb3_power_off(&phy)==0 && p.mode==APPLE_ATCPHY_MODE_USB3_DP);
        assert(p.dp_usb_context_valid==preserved);
        assert(epoch_power_off(&phy)==0);
        assert(p.mode==APPLE_ATCPHY_MODE_OFF&&!p.dp_usb_context_valid&&!p.dp_usb_context_dirty);
        assert(!p.usb_off_pending&&!p.usb_off_drained);
        for(unsigned int i=0;i<27;i++) {
            u32 o,m;j700_usb_field(i,lane,&o,&m);
            assert((registers[o/4]&m)==saved[i]);
        }
        /* Model a different post-OFF native calibration, not a canonical hardware claim. */
        for(unsigned int i=0;i<27;i++) {
            u32 o,m;j700_usb_field(i,lane,&o,&m);
            if(i==1||i==3)continue;
            registers[o/4]=(registers[o/4]&~m)|((0xa5639000^o)&m);
        }
        registers[0x200/4]=0xee;registers[0xc/4]=0;
        assert(atcphy_usb2_init(&phy)==0);
        assert(p.dp_usb_context_valid&BIT(lane));
        assert(p.dp_usb_context[lane][8]!=saved[8]);
        assert(j700_dp_configure(&p,&rate)==0);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);orient(&p,!first);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        u32 o,m;j700_usb_field(8,lane,&o,&m);
        assert((registers[o/4]&m)==((0xa5639000^o)&m));
        setup(&p,first);phy.data=&p;
        unsigned int valid=p.dp_usb_context_valid,dirty=p.dp_usb_context_dirty;
        memcpy(saved,p.dp_usb_context[lane],sizeof(saved));
        drop_offset=0x9244+0x7000*lane;
        assert(epoch_power_off(&phy)==-EIO);
        assert(p.usb_off_pending&&!p.usb_off_drained&&p.mode_dirty);
        assert(p.dp_usb_context_valid==valid&&p.dp_usb_context_dirty==dirty);
        assert(!memcmp(saved,p.dp_usb_context[lane],sizeof(saved)));
        unsigned int w=writes,r=mmio_reads;
        assert(atcphy_configure(&p,APPLE_ATCPHY_MODE_USB2)==-EBUSY);
        assert(writes==w&&mmio_reads==r);
        drop_offset=0;assert(epoch_power_off(&phy)==0);
        setup(&p,first);phy.data=&p;
        fail_caller="atcphy_power_off";
        assert(epoch_power_off(&phy)==-ETIMEDOUT);
        assert(p.usb_off_pending&&p.usb_off_drained&&p.dp_usb_context_valid);
        u32 marker=0x458abcde;registers[(0x9244+0x7000*lane)/4]=marker;
        w=writes;r=mmio_reads;
        assert(atcphy_configure(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(writes==w&&mmio_reads==r);
        fail_caller=NULL;
        assert(epoch_power_off(&phy)==0);
        assert(registers[(0x9244+0x7000*lane)/4]==marker); /* no old replay on partial-OFF retry */
    }
    puts("PASS: both-PMA clean drain/new-calibration epoch, retained failed drain, partial OFF gate/no stale replay");
}
static void bist_controls(void)
{
    for(unsigned int first=0;first<2;first++) {
        struct apple_atcphy p;orientation_initial(&p,true);orient(&p,first);
        bist_status_stuck=true;registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]=BIT(7);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
        assert(p.mode_dirty&&p.pipe_pending&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
        unsigned int w=writes,r=mmio_reads;
        assert(j700_dp_configure(&p,&rate)==-EBUSY);
        assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(writes==w&&mmio_reads==r);
        stuck_ack=true;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT);
        assert(p.pipe_pending&&p.mode_dirty);
        /* Remove injected causes explicitly; no production readiness override exists. */
        stuck_ack=bist_status_stuck=false;
        registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]=0;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(!p.mode_dirty&&!p.pipe_pending&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
        orientation_initial(&p,true);orient(&p,first);hold_unlock_ack=true;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
        assert(p.mode_dirty&&p.pipe_pending&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
        w=writes;r=mmio_reads;
        assert(atcphy_configure(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY);
        assert(writes==w&&mmio_reads==r);
        hold_unlock_ack=false;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(!p.mode_dirty&&!p.pipe_pending);
    }
    puts("PASS: both-PMA BIST failure/no trained cache, pending DUMMY zero-MMIO guards, failed parking, unlock and retry");
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"epoch")) { epoch_controls(); return 0; }
    if(argc>1 && !strcmp(argv[1],"bist")) { bist_controls(); return 0; }
    if(argc>1)return original_fixture_main(argc,argv);
    epoch_controls();bist_controls();return 0;
}
'''
    return ns['prefix'] + production + ns['suffix'].replace('int main(', 'int original_fixture_main(', 1) + controls

def warm_reset(root):

    ns = parking(root)
    production = ns['production']
    reset_block = '''\t/*
\t * Fixed-hub SAFE keeps USB2 up rather than taking the block OFF.
\t * With PIPE parked and DP owners released, reinitialize USB3 before
\t * programming its lanes; the existing sequence releases this reset.
\t */
\tif (atcphy->hw->has_t8140_dp && atcphy->fixed_usb2)
\t\tcore_clear32(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_PHY_RESET_N);

'''
    assert production.count(reset_block) == 1
    production = production.replace('static u32 registers[', '''static unsigned int warm_reset_edges, warm_unreset_lanes, warm_ownership_errors;
static u32 registers[''', 1)
    production = production.replace('    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', '''    if (offset == ATCPHY_POWER_CTRL && (mask & ATCPHY_POWER_PHY_RESET_N) &&
        !(value & ATCPHY_POWER_PHY_RESET_N)) {
        warm_reset_edges++;
        if (p->fixed_usb2 && p->hw->has_t8140_dp &&
            (p->pipe_state != ATCPHY_PIPEHANDLER_STATE_DUMMY || p->dp_owned_pmas ||
             p->dp_idle_pmas || p->aux_power_requested)) warm_ownership_errors++;
    }
    if (offset == p->hw->aciophy_lane_mode && p->fixed_usb2 && p->hw->has_t8140_dp &&
        (registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N)) warm_unreset_lanes++;
    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;''', 1)
    controls = r'''
static void warm_initial(struct apple_atcphy *p)
{
    orientation_initial(p, true);
    registers[ATCPHY_POWER_CTRL / 4] |= ATCPHY_POWER_PHY_RESET_N;
    warm_reset_edges = warm_unreset_lanes = warm_ownership_errors = 0;
}
static void warm_roundtrip(void)
{
    bool failed = false;
    for (unsigned int first = 0; first < 2; first++) {
        struct apple_atcphy p;
        const struct phy_configure_opts_dp rate = {
            .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
        };
        warm_initial(&p);
        for (unsigned int attach = 0; attach < 3; attach++) {
            orient(&p, first ^ (attach & 1));
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && !p.mode_dirty);
            assert(j700_dp_configure(&p, &rate) == 0);
            unsigned int edges = warm_reset_edges;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(warm_reset_edges == edges); /* cached same-mode must not reset */
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == 0);
            assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && p.host_active);
            assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
            assert(registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N);
            assert(!reset_writes && !usb2_writes);
        }
        assert(usb2_on == 6); /* no extra USB2 power-on call introduced */
        printf("first=%u reset_assertions=%u unreset_lane_writes=%u ownership_errors=%u\n",
               first, warm_reset_edges, warm_unreset_lanes, warm_ownership_errors);
        failed |= warm_reset_edges != 6 || warm_unreset_lanes || warm_ownership_errors;
    }
    fflush(stdout);
    assert(!failed);
}
static void warm_failure_controls(void)
{
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate = {
        .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 2,
    };
    warm_initial(&p);
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(j700_dp_configure(&p, &rate) == 0);
    unsigned int edges = warm_reset_edges, owned = p.dp_owned_pmas;
    stuck_ack = true;
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == -ETIMEDOUT);
    assert(warm_reset_edges == edges && p.dp_owned_pmas == owned && p.mode_dirty);
    assert(!usb2_writes && !reset_writes);

    warm_initial(&p);
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(j700_dp_configure(&p, &rate) == 0);
    edges = warm_reset_edges; owned = p.dp_owned_pmas;
    fail_caller = "j700_field_update";
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == -ETIMEDOUT);
    assert(warm_reset_edges == edges && p.dp_owned_pmas == owned && p.mode_dirty);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert(!usb2_writes && !reset_writes);

    warm_initial(&p);
    fail_caller = "atcphy_configure"; /* existing RCAL poll, after reset release */
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
    assert(warm_reset_edges == 1 && !warm_ownership_errors && p.mode_dirty);
    assert(registers[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    struct phy phy = { &p };
    unsigned int before = writes;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == HOST_START_ERROR(&p, -EBUSY));
    assert(j700_dp_configure(&p, &rate) == -EBUSY && writes == before);
    fail_caller = NULL;
    assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    assert(!p.mode_dirty && !warm_ownership_errors);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(!usb2_writes && !reset_writes);
    puts("PASS: park/DP-cleanup failures precede reset; released-reset RCAL failure stays dirty and retryable");
}
static void warm_scope_controls(void)
{
    struct apple_atcphy p;
    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        for (unsigned int t8140 = 0; t8140 < 2; t8140++) {
            orientation_initial(&p, fixed);
            hw.has_t8140_dp = t8140;
            registers[ATCPHY_POWER_CTRL / 4] |= ATCPHY_POWER_PHY_RESET_N;
            warm_reset_edges = warm_unreset_lanes = warm_ownership_errors = 0;
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
            assert(warm_reset_edges == (fixed && t8140));
            assert(!warm_ownership_errors && !usb2_writes && !reset_writes);
        }
    }
    hw.has_t8140_dp = true;
    puts("PASS: reset scope requires both fixed hub and T8140; ordinary/legacy paths unchanged");
}
int main(int argc, char **argv)
{
    if (argc > 1) return original_fixture_main(argc, argv);
    warm_roundtrip(); warm_failure_controls(); warm_scope_controls();
    puts("PASS: warm reset source ordering; no electrical recovery is modeled");
    return 0;
}
'''
    suffix = ns['suffix'].replace('int main(', 'int original_fixture_main(', 1) + controls
    return ns['prefix'] + production + suffix

def usb_context(root):
    ns = parking(root)
    production=ns['production'].replace('static u32 registers[', 'static u32 blocked_offset;\nstatic unsigned int mmio_reads;\nstatic bool track_dp_writes;\nstatic u32 dp_write_masks[0x30000 / 4];\nstatic u32 registers[', 1)
    production=production.replace('static u32 readl(void *address) { return *(u32 *)address; }', 'static u32 readl(void *address) { mmio_reads++; return *(u32 *)address; }', 1)
    production=production.replace('    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', '    if (track_dp_writes) dp_write_masks[offset / 4] |= mask | value;\n    if (blocked_offset && offset == blocked_offset) { writes++; return; }\n    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', 1)
    controls = r'''
static void seed_usb(struct apple_atcphy *p, unsigned int first)
{
    orientation_initial(p, true);
    registers[0x200 / 4] = 0xee;
    registers[0xc / 4] = 0;
    /* Synthetic USB context, never a private factory table or captured register image. */
    for (unsigned int offset = 0x9000; offset < 0x15000; offset += 4)
        registers[offset / 4] = 0x5a5a1234 ^ offset;
    registers[0x8 / 4] = 0xa5000000;
    registers[0x10c / 4] = 0xa5000000;
    orient(p, first);
    assert(select_mode(p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
}

static void mask_coverage(void)
{
    struct apple_atcphy p;
    memset(dp_write_masks, 0, sizeof(dp_write_masks));
    seed_usb(&p, 1);
    for (unsigned int rate = 0; rate < 4; rate++) {
        track_dp_writes = true;
        assert(j700_dp_start(&p, 1, rate, false) == 0);
        j700_dp_equalize(&p, 0, true, 0x123);
        j700_dp_equalize(&p, 0, false, 0x123);
        assert(j700_dp_stop(&p) == 0);
        track_dp_writes = false;
    }
    unsigned int entries = 0;
    for (unsigned int offset = 0; offset < sizeof(dp_write_masks); offset += 4) {
        if (!((offset >= 0x9000 && offset <= 0xdfff) || offset == 8 ||
              offset == 0xc || offset == 0x10c || offset == 0x200)) continue;
        u32 expected = 0;
        for (unsigned int i = 0; i < ARRAY_SIZE(j700_usb_fields); i++)
            if (j700_usb_fields[i].offset == offset) expected = j700_usb_fields[i].mask;
        assert(dp_write_masks[offset / 4] == expected);
        entries += !!expected;
    }
    assert(entries == 27);
    puts("PASS: 27 masks equal the actual public DP start/voltage/stop write union at every rate");
}
static void masked_roundtrip(void)
{
    const struct phy_configure_opts_dp rate = {
        .set_rate=true, .set_lanes=true, .link_rate=8100, .lanes=2,
    };
    bool failure=false;
    for (unsigned int first=0; first<2; first++) {
        struct apple_atcphy p;
        seed_usb(&p, first);
        unsigned int old_dp=!first;
#ifdef PROPOSAL
        assert(p.dp_usb_context_valid & BIT(old_dp));
        assert(p.dp_usb_context[old_dp][1] == 0);
        assert(p.dp_usb_context[old_dp][3] == (0xeU << (4 * old_dp)));
        assert(((registers[0x200 / 4] >> (4 * old_dp)) & 0xf) == 1);
#endif
        u32 saved[27], outside[27];
        for (unsigned int i=0;i<27;i++) {
            u32 offset,mask;
            j700_usb_field(i,old_dp,&offset,&mask);
            saved[i]=core_read32(&p,offset)&mask;
        }
        /* Power/receiver were saved before route idle, not at this post-route sample. */
        saved[1]=0; saved[3]=0xeU<<(4*old_dp);
        assert(j700_dp_configure(&p,&rate)==0);
        unsigned int edges_before=writes;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(writes==edges_before); /* same mode never resets or recaptures */
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
        for(unsigned int i=0;i<27;i++) {
            u32 offset,mask;j700_usb_field(i,old_dp,&offset,&mask);
            u32 other_offset,other_mask; j700_usb_field(i,!old_dp,&other_offset,&other_mask);
            outside[i]=registers[offset/4]&~(mask | (i<4 ? other_mask : 0));
        }
        orient(&p,!first);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        unsigned int mismatch=0;
        for(unsigned int i=0;i<27;i++) {
            u32 offset,mask;j700_usb_field(i,old_dp,&offset,&mask);
            if((registers[offset/4]&mask)!=saved[i]) mismatch++;
            u32 other_offset,other_mask; j700_usb_field(i,!old_dp,&other_offset,&other_mask);
            assert((registers[offset/4]&~(mask | (i<4 ? other_mask : 0)))==outside[i]);
        }
        printf("first=%u former_dp_pma=%u masked_mismatches=%u receiver=%#x power=%#x\n",
               first,old_dp,mismatch,registers[0xc/4],registers[0x200/4]);
        failure |= mismatch!=0;
        assert(!usb2_writes&&!reset_writes);
        assert(j700_dp_configure(&p,&rate)==0);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
        orient(&p,first);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(!p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
    }
    fflush(stdout);assert(!failure);
}
static void restore_controls(void)
{
#ifdef PROPOSAL
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate={.set_rate=true,.set_lanes=true,.link_rate=8100,.lanes=2};
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    unsigned int valid=p.dp_usb_context_valid,dirty=p.dp_usb_context_dirty;
    u32 snapshot[2][27];memcpy(snapshot,p.dp_usb_context,sizeof(snapshot));
    stuck_ack=true;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT);
    assert(p.dp_owned_pmas==1&&p.dp_usb_context_valid==valid&&p.dp_usb_context_dirty==dirty);
    assert(!memcmp(snapshot,p.dp_usb_context,sizeof(snapshot)));
    recover();
    fail_caller="j700_field_update";
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT);
    assert(p.dp_owned_pmas==1&&p.dp_usb_context_valid==valid);
    assert(!memcmp(snapshot,p.dp_usb_context,sizeof(snapshot)));
    recover();assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    p.dp_owned_pmas=1;
    unsigned int before=writes;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.dp_owned_pmas=0;p.dp_idle_pmas=1;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.dp_idle_pmas=0;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.pipe_state=ATCPHY_PIPEHANDLER_STATE_DUMMY;
    fail_caller="j700_usb_field_update";
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
    assert(p.dp_usb_context_valid&1);
    assert(p.dp_usb_context_dirty&1);
    recover();assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(!(p.dp_usb_context_valid&1)&&!(p.dp_usb_context_dirty&1));
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    u32 old_context[27];memcpy(old_context,p.dp_usb_context[0],sizeof(old_context));
    fail_caller="j700_usb_field_update";
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
    assert(p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert((p.dp_usb_context_valid&1)&&(p.dp_usb_context_dirty&1));
    assert(!memcmp(old_context,p.dp_usb_context[0],sizeof(old_context)));
    before=writes;
    assert(j700_dp_configure(&p,&rate)==-EBUSY&&writes==before);
    recover();assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(!p.mode_dirty&&!(p.dp_usb_context_valid&1));
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    blocked_offset=0x9244;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EIO);
    assert(p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert((p.dp_usb_context_valid&1)&&(p.dp_usb_context_dirty&1));
    before=writes;
    assert(j700_dp_configure(&p,&rate)==-EBUSY&&writes==before);
    blocked_offset=0;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0&&!p.mode_dirty);
    puts("PASS: actual mux static/power restore failures keep dirty route/context, block training and retry");
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]=BIT(7);
    bist_status_stuck=true; bist_faults=0;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
    assert(p.mode_dirty && p.pipe_pending);
    assert(bist_faults==2 && (registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]&BIT(7)));
    assert((registers[0x200/4]&0xf)==0xe && !(registers[0xc/4]&0x44));
    stuck_ack=true;
    before=writes;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT && writes==before);
    puts("PASS: restoration never clears simulated BIST/ACK faults or forces readiness");
    for(unsigned int fixed=0;fixed<2;fixed++) {
        seed_usb(&p,0);p.fixed_usb2=fixed;hw.has_t8140_dp=false;
        before=writes;
        assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==0&&writes==before);
        hw.has_t8140_dp=true;
    }
    puts("PASS: park/DP-idle failure retention, immutable snapshot, live/pending-owner restore guards, power readback failure/retry, legacy exclusions");
#endif
}

static void inactive_host_gates(void)
{
#ifdef PROPOSAL
    const struct phy_configure_opts_dp rate = {
        .set_rate=true, .set_lanes=true, .link_rate=8100, .lanes=2,
    };
    unsigned int violations = 0;
    for (unsigned int backend = ATCPHY_PIPEHANDLER_STATE_USB3;
         backend <= ATCPHY_PIPEHANDLER_STATE_USB4; backend++) {
        for (unsigned int entry = 0; entry < 3; entry++) {
            struct apple_atcphy p;
            seed_usb(&p, 1);
            assert(j700_dp_configure(&p, &rate) == 0);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == 0);
            orient(&p, 0);
            p.host_active = false;
            p.pipe_state = backend;
            unsigned int before_writes = writes, before_reads = mmio_reads;
            unsigned int valid = p.dp_usb_context_valid, dirty = p.dp_usb_context_dirty;
            u32 snapshot[2][27]; memcpy(snapshot, p.dp_usb_context, sizeof(snapshot));
            int ret = entry == 0 ? j700_usb_capture(&p, APPLE_ATCPHY_MODE_USB3_DP) :
                      entry == 1 ? j700_usb_restore(&p, APPLE_ATCPHY_MODE_USB3_DP) :
                                   atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3_DP);
            bool blocked = ret == -EBUSY && writes == before_writes &&
                           mmio_reads == before_reads && valid == p.dp_usb_context_valid &&
                           dirty == p.dp_usb_context_dirty &&
                           !memcmp(snapshot, p.dp_usb_context, sizeof(snapshot));
            printf("inactive-host backend=%u entry=%u ret=%d new_writes=%u new_reads=%u blocked=%u\n",
                   backend, entry, ret, writes-before_writes, mmio_reads-before_reads, blocked);
            violations += !blocked;
        }
    }
    fflush(stdout); assert(!violations);
    puts("PASS: six inactive-host/live-PIPE entry gates retain snapshots and perform zero MMIO");
#endif
}
static void failure_diagnostics(void)
{
#ifdef PROPOSAL
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate={.set_rate=true,.set_lanes=true,.link_rate=8100,.lanes=2};
    const char *stages[] = { "stage=static", "stage=power", "stage=receiver" };
    for (unsigned int stage=0; stage<3; stage++) {
        seed_usb(&p,1); assert(j700_dp_configure(&p,&rate)==0);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0); orient(&p,0);
        diagnostic_records=0; last_diagnostic[0]=0;
        blocked_offset=stage==0 ? 0x9244 : stage==1 ? 0x200 : 0xc;
        int ret=select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP);
        assert(ret==(stage==0 ? -EIO : -ETIMEDOUT));
        assert(diagnostic_records==1 && strstr(last_diagnostic,stages[stage]));
        assert(strstr(last_diagnostic,"pma=0") && strstr(last_diagnostic,"reg="));
        assert(strstr(last_diagnostic,"mask=") && strstr(last_diagnostic,"expected="));
        assert(strstr(last_diagnostic,"observed="));
        assert(p.mode_dirty && (p.dp_usb_context_valid&1) && (p.dp_usb_context_dirty&1));
        blocked_offset=0;
    }
    puts("PASS: static/power/receiver errors preserve state and report stage/PMA/register/mask/expected/final observed");
#endif
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"inactive-host")) { inactive_host_gates(); return 0; }
    if(argc>1)return original_fixture_main(argc,argv);
    mask_coverage();masked_roundtrip();restore_controls();inactive_host_gates();failure_diagnostics();
    puts("PASS: masked restoration roundtrips; no BIST readiness or USB speed is modeled");
    return 0;
}
'''
    logger = r'''
#include <stdarg.h>
static unsigned int diagnostic_records;
static char last_diagnostic[512];
static void record_debug(const char *format, ...)
{
    if (!strstr(format, "USB restore stage=")) return;
    va_list args; va_start(args,format);
    vsnprintf(last_diagnostic,sizeof(last_diagnostic),format,args);
    va_end(args); diagnostic_records++;
    fputs(last_diagnostic,stdout);
}
#undef dev_dbg
#define dev_dbg(dev, format, ...) record_debug(format, ##__VA_ARGS__)
'''
    return ns["prefix"] + logger + "#define PROPOSAL\n" + production + ns["suffix"].replace("int main(", "int original_fixture_main(", 1) + controls

def pipe(root):

    source = (root / 'drivers/phy/apple/atc.c').read_text()
    dp = (root / 'drivers/phy/apple/atc-j700-dp.h').read_text()


    def block(marker):
        start = source.index(marker)
        opening = source.index('{', start)
        end, depth = opening + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]


    def function(name):
        if name == 'atcphy_mux_set' and 'static int atcphy_mux_set_locked(' in source:
            return atc_mux_without_pm(source)
        if name in ATC_SPLIT + ATC_T8103_LIVE:
            return atc_function(source, name)
        return block(re.search(r'^static [^\n;]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M).group())


    parts = source_defines(source)
    for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode'):
        parts.append(block('enum ' + name + ' {') + ';')
    for name in ('atcphy_mode_configuration', 'atcphy_hw'):
        parts.append(block('struct ' + name + ' {') + ';')
    parts.append(r'''
struct apple_atcphy {
    bool dp_only, pd_was_off, tunnel_clock_on;
    unsigned int tunnel_rate, tunnel_users;
    struct { bool aux, pll; unsigned int pairs, rate; } dp_t8122;
    const struct atcphy_hw *hw;
    struct { unsigned char *core, *pipehandler; } regs;
    void *dev;
    int lock;
    bool fixed_usb2, host_active, ss_tunables, dp_tunables, mode_dirty;
    unsigned int dp_owned_pmas, dp_idle_pmas;
    unsigned int dp_usb_context_valid, dp_usb_context_dirty;
    u32 dp_usb_context[2][27];
    bool pipe_pending, usb_off_pending, usb_off_drained;
    bool usb3_port_off, park_owed;
    bool t8103_dummy_pending, t8103_usb3_retry, t8103_usb3_workaround;
    bool swap_lanes;
    enum atcphy_mode mode, typec_mode;
    enum atcphy_pipehandler_state pipe_state;
};
''')
    start = source.index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
    parts.append(source[start:source.index('\n};', start) + 3])
    parts.append(r'''
static u32 pipe_regs[0x1000 / 4], core_regs[0x30000 / 4];
static unsigned int polls, fail_poll, mux_writes, lane_calls;
static const char *last_poll;
static bool stuck_ack;
static u32 readl(void *address) { return *(u32 *)address; }
static void mask32(void *address, u32 mask, u32 value)
{
    *(u32 *)address = (*(u32 *)address & ~mask) | value;
    if (address == (void *)&pipe_regs[PIPEHANDLER_LOCK_REQ / 4] && !stuck_ack)
        pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = *(u32 *)address & PIPEHANDLER_LOCK_EN;
    if (address == (void *)&pipe_regs[PIPEHANDLER_MUX_CTRL / 4])
        mux_writes++;
}
static void set32(void *p, u32 v) { mask32(p, 0, v); }
static void clear32(void *p, u32 v) { mask32(p, v, 0); }
static void writel(u32 v, void *p) { mask32(p, ~0U, v); }
static void core_mask32(struct apple_atcphy *p, u32 o, u32 m, u32 v)
{ assert(o < sizeof(core_regs) && o % 4 == 0); mask32(p->regs.core + o, m, v); }
static void core_set32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, 0, v); }
static void core_clear32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, v, 0); }
#define readl_poll_timeout_atomic readl_poll_timeout
#define readl_poll_timeout(address, value, condition, interval, timeout) \
    ({ (value) = readl(address); polls++; last_poll = __func__; \
       polls == fail_poll || !(condition) ? -ETIMEDOUT : 0; })
/* Lane configuration is outside this test: count calls and commit its result. */
/* Observation is read-only and separately tested; retain production caller control flow. */
static void __attribute__((unused)) atcphy_restore_after_pd_off(struct apple_atcphy *p) { assert(!p->pd_was_off); }
static int atcphy_configure(struct apple_atcphy *p, enum atcphy_mode mode);
''')
    for name in ('core_read32', 'atcphy_get_mode_config'):
        parts.append(function(name))
    # Actual restoration functions and gates; analog lane setup stays outside this PIPE unit.
    parts.append(dp[dp.index('/* Masks are the union'):dp.index('/* Retry only the idle operations')])
    parts.append(r'''
static int atcphy_configure(struct apple_atcphy *p, enum atcphy_mode mode)
{
    int ret;
    lane_calls++; p->mode_dirty = true;
    ret = j700_usb_capture(p, mode);
    if (!ret) ret = j700_usb_restore(p, mode);
    if (ret) return ret;
    p->mode = mode; p->mode_dirty = false; return 0;
}
''')
    for name in ('atcphy_pipehandler_lock', 'atcphy_pipehandler_lock_live',
                 'atcphy_pipehandler_unlock', 'atcphy_pipehandler_check',
                 'atcphy_pipehandler_set_mux', 'atcphy_clear_nonselected_phy_reset',
                 'atcphy_configure_pipehandler_usb3_host_t8122',
                 'atcphy_configure_pipehandler_usb3_host_t8103',
                 'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
                 'atcphy_enable_dummy_phy', 'atcphy_pipe_live', 'atcphy_needs_dummy_phy',
                 'atcphy_configure_pipehandler_dummy',
                 'atcphy_configure_pipehandler', 'atcphy_park_live_t8103_pipehandler',
                 'atcphy_park_pipehandler',
                 'atcphy_usb3_route', 'atcphy_usb3_set_mode', 'atcphy_mux_set'):
        parts.append(function(name))
    production = '\n'.join(parts)
    # An admitted base-M1 host start may skip the native BIST, and the lock it holds.
    production = ('#define T8103_HOST_BIST ' + str(int('ATCPHY_GENERATION_T8103' not in
                  function('atcphy_configure_pipehandler') and 't8103_usb3_workaround' not in
                  function('atcphy_configure_pipehandler'))) + '\n' + production)
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctz(m))
#define field_prep FIELD_PREP
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define lockdep_assert_held(...) ((void)0)
#define WARN_ON_ONCE(v) assert(!(v))
#define CONFIG_PHY_APPLE_ATC_KUNIT_TEST 0
#define IS_ENABLED(option) (option)
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define guard(kind) (void)
#define fallthrough __attribute__((fallthrough))
#define usleep_range(...) ((void)0)
#define udelay(n) ((void)(n))
#define __iomem
enum { TYPEC_STATE_SAFE, TYPEC_STATE_USB, TYPEC_MODE_USB4,
       TYPEC_DP_STATE_C, TYPEC_DP_STATE_D, TYPEC_DP_STATE_E };
enum { USB_TYPEC_TBT_SID = 0x8087, USB_TYPEC_DP_SID = 0xff01 };
#define EUDO_USB_MODE_MASK 3U
enum { EUDO_USB_MODE_USB2, EUDO_USB_MODE_USB3, EUDO_USB_MODE_USB4 };
enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE, PHY_MODE_INVALID };
struct enter_usb_data { u32 eudo; };
struct typec_altmode { unsigned int svid; };
struct typec_mux_state { unsigned long mode; struct typec_altmode *alt; void *data; };
struct typec_mux_dev { struct apple_atcphy *data; };
struct phy { struct apple_atcphy *data; };
#define typec_mux_get_drvdata(mux) ((mux)->data)
#define phy_get_drvdata(phy) ((phy)->data)
'''
    suffix = r'''
static struct atcphy_hw hw = {
    .gen = ATCPHY_GENERATION_T8122, .has_t8140_dp = true,
    .pipe_mux_clk = PIPEHANDLER_MUX_CTRL_CLK_T8103,
    .pipe_mux_data = PIPEHANDLER_MUX_CTRL_DATA_T8103,
};
static void initial(struct apple_atcphy *p, enum atcphy_mode mode,
                    enum atcphy_pipehandler_state backend)
{
    memset(pipe_regs, 0, sizeof(pipe_regs));
    memset(core_regs, 0, sizeof(core_regs));
    core_regs[ACIOPHY_TOP_PHY_STAT / 4] = ACIOPHY_TOP_PHY_STAT_LN0_UNK0;
    *p = (struct apple_atcphy) {
        .t8103_usb3_workaround = hw.gen == ATCPHY_GENERATION_T8103,
        .hw = &hw, .regs.core = (unsigned char *)core_regs,
        .regs.pipehandler = (unsigned char *)pipe_regs,
        .mode = mode, .typec_mode = mode, .pipe_state = backend,
        .ss_tunables = true, .dp_tunables = true,
    };
    polls = fail_poll = mux_writes = lane_calls = 0;
    stuck_ack = false;
}
static int request(struct apple_atcphy *p, enum atcphy_mode mode)
{
    struct typec_mux_dev mux = { p };
    struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
    struct enter_usb_data data = { .eudo = EUDO_USB_MODE_USB4 };
    struct typec_mux_state state = { .mode = TYPEC_STATE_SAFE };
    if (mode == APPLE_ATCPHY_MODE_USB3)
        state.mode = TYPEC_STATE_USB;
    else if (mode == APPLE_ATCPHY_MODE_USB3_DP || mode == APPLE_ATCPHY_MODE_DP) {
        state.mode = mode == APPLE_ATCPHY_MODE_DP ? TYPEC_DP_STATE_C : TYPEC_DP_STATE_D;
        state.alt = &dp;
    } else if (mode == APPLE_ATCPHY_MODE_USB4) {
        state.mode = TYPEC_MODE_USB4;
        state.data = &data;
    }
    return atcphy_mux_set(&mux, &state);
}
static void backend_failure(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    for (enum atcphy_mode mode = APPLE_ATCPHY_MODE_OFF; mode <= APPLE_ATCPHY_MODE_DP; mode++) {
        enum atcphy_pipehandler_state target = atcphy_modes[mode].pipehandler_state;
        enum atcphy_pipehandler_state old = target == ATCPHY_PIPEHANDLER_STATE_DUMMY ?
            ATCPHY_PIPEHANDLER_STATE_USB3 : ATCPHY_PIPEHANDLER_STATE_DUMMY;
        for (unsigned int host = 0; host < 2; host++) {
            initial(&p, mode, old);
            /* A previous lock ACK cannot be cleared: fatal before mux writes. */
            pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
            stuck_ack = true;
            assert(atcphy_configure_pipehandler(&p, host) == -ETIMEDOUT);
            assert(p.pipe_state == old && mux_writes == 0);
            stuck_ack = false;
            assert(atcphy_configure_pipehandler(&p, host) == 0);
            assert(p.pipe_state == target && mux_writes == 3);
            const unsigned int data[] = { PIPEHANDLER_MUX_CTRL_DATA_DUMMY,
                PIPEHANDLER_MUX_CTRL_DATA_USB3, PIPEHANDLER_MUX_CTRL_DATA_USB4 };
            const unsigned int clk[] = { PIPEHANDLER_MUX_CTRL_CLK_DUMMY,
                PIPEHANDLER_MUX_CTRL_CLK_USB3, PIPEHANDLER_MUX_CTRL_CLK_USB4 };
            u32 mux = pipe_regs[PIPEHANDLER_MUX_CTRL / 4];
            assert(FIELD_GET(hw.pipe_mux_data, mux) == data[target]);
            assert(FIELD_GET(hw.pipe_mux_clk, mux) == clk[target]);
            cases++;
        }
    }
    printf("PASS: %u backend/role check-failure and retry cases\n", cases);
}
static void host_lock_failure(void)
{
    struct apple_atcphy p;
    for (unsigned int gen = 0; gen < 2; gen++) {
        hw.gen = gen ? ATCPHY_GENERATION_T8122 : ATCPHY_GENERATION_T8103;
        initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_DUMMY);
        struct phy phy = { &p };
        const char *fault = "atcphy_pipehandler_lock";
#if !T8103_HOST_BIST
        /* T8103 host start takes no lock: fail the release of a stale one instead. */
        if (!gen) {
            pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
            fault = "atcphy_pipehandler_unlock";
        }
#endif
        stuck_ack = true;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -ETIMEDOUT);
        assert(polls == 1 && !strcmp(last_poll, fault));
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && mux_writes == 0);
        assert(!(pipe_regs[PIPEHANDLER_LOCK_REQ / 4] & PIPEHANDLER_LOCK_EN));
        stuck_ack = false;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == 0);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && mux_writes == 6);
        unsigned int before = polls;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == 0);
        assert(polls == before && mux_writes == 6);
    }
#if T8103_HOST_BIST
    puts("PASS: both USB3 host BIST generations retain fatal lock ACK and retry");
#else
    puts("PASS: T8122 host lock and T8103 stale-lock release faults stay fatal; both retry once acknowledged");
#endif
}
static void mux_retry(void)
{
    struct apple_atcphy p;
    const enum atcphy_mode modes[] = { APPLE_ATCPHY_MODE_USB3, APPLE_ATCPHY_MODE_USB3_DP };
    for (unsigned int m = 0; m < 2; m++) {
        initial(&p, APPLE_ATCPHY_MODE_USB2, ATCPHY_PIPEHANDLER_STATE_DUMMY);
        p.fixed_usb2 = p.host_active = true;
        stuck_ack = true;
        assert(request(&p, modes[m]) == -ETIMEDOUT);
        assert(p.mode == modes[m] && !p.mode_dirty && lane_calls == 1);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && mux_writes == 0);
        p.dp_owned_pmas = 3; /* PIPE retry must not reconfigure an owned DP mode. */
        assert(request(&p, modes[m]) == -ETIMEDOUT);
        assert(lane_calls == 1 && p.dp_owned_pmas == 3 && mux_writes == 0);
        stuck_ack = false;
        assert(request(&p, modes[m]) == 0);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && lane_calls == 1);
        assert(p.dp_owned_pmas == 3 && mux_writes == 6);
        unsigned int before = polls;
        assert(request(&p, modes[m]) == 0 && polls == before && lane_calls == 1);
    }
    puts("PASS: fixed-hub same-mode retry preserves lanes and DP ownership");
}
static void same_mode_pending(void)
{
    struct apple_atcphy p;
    initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_DUMMY);
    p.fixed_usb2 = p.host_active = true;
    stuck_ack = true;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3) == -ETIMEDOUT);
    assert(lane_calls == 0 && mux_writes == 0);
    stuck_ack = false;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3) == 0);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && lane_calls == 0);
    puts("PASS: same-mode pending PIPE setup independently retries");
}
static void parking_and_guards(void)
{
    struct apple_atcphy p;
    initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_USB3);
    pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
    stuck_ack = true;
    assert(atcphy_park_pipehandler(&p, false) == -ETIMEDOUT);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && mux_writes == 0);
    stuck_ack = false;
    assert(atcphy_park_pipehandler(&p, false) == 0);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && mux_writes == 3);
    assert(atcphy_park_pipehandler(&p, false) == 0);
    assert(mux_writes == 3);
    initial(&p, APPLE_ATCPHY_MODE_USB2, ATCPHY_PIPEHANDLER_STATE_DUMMY);
    p.fixed_usb2 = p.host_active = true;
    assert(request(&p, APPLE_ATCPHY_MODE_OFF) == 0);
    assert(p.mode == APPLE_ATCPHY_MODE_USB2 && lane_calls == 0 && mux_writes == 0);
    p.mode_dirty = true;
    assert(request(&p, APPLE_ATCPHY_MODE_OFF) == 0 && lane_calls == 1 && !p.mode_dirty);
    /* Inactive hosts and ordinary ports retain their deferred PIPE setup. */
    initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_DUMMY);
    p.fixed_usb2 = true;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3) == 0 && mux_writes == 0);
    p.fixed_usb2 = false;
    p.host_active = true;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3) == 0 && mux_writes == 0);
    /* Capability rejection must still precede mode configuration. */
    initial(&p, APPLE_ATCPHY_MODE_USB2, ATCPHY_PIPEHANDLER_STATE_DUMMY);
    p.fixed_usb2 = p.host_active = true;
    p.ss_tunables = false;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3) == -EOPNOTSUPP && lane_calls == 0);
    p.ss_tunables = true;
    p.dp_tunables = false;
    assert(request(&p, APPLE_ATCPHY_MODE_USB3_DP) == -EOPNOTSUPP && lane_calls == 0);
    assert(request(&p, APPLE_ATCPHY_MODE_USB4) == -EOPNOTSUPP && lane_calls == 0);
    puts("PASS: parking retry, SAFE fixed USB2, dirty-mode and capability guards");
}
static void requested_lock(void)
{
    struct apple_atcphy p;
    initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_DUMMY);
    pipe_regs[PIPEHANDLER_LOCK_REQ / 4] = PIPEHANDLER_LOCK_EN;
    stuck_ack = true;
    assert(atcphy_pipehandler_lock(&p) == -ETIMEDOUT && polls == 1);
    assert(!(pipe_regs[PIPEHANDLER_LOCK_REQ / 4] & PIPEHANDLER_LOCK_EN));
    pipe_regs[PIPEHANDLER_LOCK_REQ / 4] = PIPEHANDLER_LOCK_EN;
    pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
    assert(atcphy_pipehandler_lock(&p) == 0 && polls == 2);
    puts("PASS: an existing LOCK request must still have actual ACK before success");
}
static void live_policy_scope(void)
{
    struct apple_atcphy p;
    for (unsigned int t8140 = 0; t8140 < 2; t8140++) {
        for (unsigned int fixed = 0; fixed < 2; fixed++) {
            for (unsigned int active = 0; active < 2; active++) {
                hw.has_t8140_dp = t8140;
                initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_USB3);
                p.fixed_usb2 = fixed;
                p.host_active = active;
                stuck_ack = true; /* Request exists, but ACK never asserts. */
                int ret = atcphy_park_pipehandler(&p, false);
                if (t8140 && fixed && active) {
                    assert(ret == -ETIMEDOUT && p.mode_dirty && mux_writes == 0);
                    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
                    assert(!pipe_regs[PIPEHANDLER_NONSELECTED_OVERRIDE / 4]);
                } else {
                    assert(ret == 0 && !p.mode_dirty && mux_writes == 3);
                    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
                }
                /* Entry ACK release is fatal everywhere, but dirties only live T8140 parking. */
                initial(&p, APPLE_ATCPHY_MODE_USB3, ATCPHY_PIPEHANDLER_STATE_USB3);
                p.fixed_usb2 = fixed;
                p.host_active = active;
                pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
                stuck_ack = true;
                assert(atcphy_park_pipehandler(&p, false) == -ETIMEDOUT);
                assert(p.mode_dirty == (t8140 && fixed && active));
                assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && mux_writes == 0);
            }
        }
    }
    puts("PASS: eight live/inactive, fixed/ordinary and T8140/legacy LOCK and entry-ACK policy controls");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "backend")) backend_failure();
    else if (!strcmp(argv[1], "host")) host_lock_failure();
    else if (!strcmp(argv[1], "mux")) mux_retry();
    else if (!strcmp(argv[1], "same")) same_mode_pending();
    else if (!strcmp(argv[1], "park")) parking_and_guards();
    else if (!strcmp(argv[1], "requested")) requested_lock();
    else if (!strcmp(argv[1], "scope")) live_policy_scope();
    else assert(false);
    return 0;
}
'''
    return prefix + production + suffix

def unwind(root):

    atc = (root / 'drivers/phy/apple/atc.c').read_text()
    dp = (root / 'drivers/phy/apple/atc-j700-dp.h').read_text()


    def function(source, name):
        if name == 'atcphy_mux_set' and 'static int atcphy_mux_set_locked(' in source:
            return atc_mux_without_pm(source)
        if name in ATC_SPLIT + ATC_T8103_LIVE:
            return atc_function(source, name)
        return block(source, re.search(r'^static [^\n;]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M).group())


    # Use the production mode table and wrappers too, so SAFE mapping and fast paths
    # are tested rather than mirrored by a second implementation.
    parts = source_defines(atc)
    for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode',
                 'atcphy_dp_link_rate'):
        parts.append(block(atc, 'enum ' + name + ' {') + ';')
    for name in ('atcphy_mode_configuration', 'atcphy_hw'):
        parts.append(block(atc, 'struct ' + name + ' {') + ';')
    parts.append(r'''
struct apple_atcphy {
    bool dp_only, pd_was_off, tunnel_clock_on;
    unsigned int tunnel_rate, tunnel_users;
    struct { bool aux, pll; unsigned int pairs, rate; } dp_t8122;
    const struct atcphy_hw *hw;
    struct { unsigned char *core, *lpdptx; } regs;
    void *dev;
    int lock, dp_link_rate;
    bool fixed_usb2, host_active, swap_lanes, ss_tunables, dp_tunables;
    bool aux_power_requested, mode_dirty, dp_spread_spectrum, dp_presets_valid[5];
    u32 dp_presets[5][16];
    unsigned int dp_idle_pmas, dp_owned_pmas;
    unsigned int dp_usb_context_valid, dp_usb_context_dirty;
    u32 dp_usb_context[2][27];
    bool pipe_pending, usb_off_pending, usb_off_drained;
    bool usb3_port_off, park_owed;
    bool t8103_dummy_pending, t8103_usb3_retry, t8103_usb3_workaround;
    enum atcphy_mode mode, typec_mode;
    enum atcphy_pipehandler_state pipe_state;
};
''')
    start = atc.index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
    parts.append(atc[start:atc.index('\n};', start) + 3])
    parts.append(r'''
struct phy_configure_opts_dp {
    unsigned int link_rate, lanes, voltage[4], pre[4];
    bool set_rate, set_lanes, set_voltages, ssc;
};
static u32 registers[0x30000 / 4];
static unsigned int polls, fail_poll, writes, usb2_on;
static u32 readl(void *address) { return *(u32 *)address; }
static void core_mask32(struct apple_atcphy *p, u32 offset, u32 mask, u32 value)
{
    (void)p;
    assert(offset < sizeof(registers) && offset % 4 == 0);
    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;
    writes++;
    if (offset == ATCPHY_POWER_CTRL)
        registers[ATCPHY_POWER_STAT / 4] = registers[offset / 4];
    if (offset == 0x2000) {
        if (value == 0x10000001)
            registers[offset / 4] |= BIT(1);
        else {
            registers[offset / 4] &= ~BIT(1);
            registers[0x7034 / 4] &= ~BIT(3);
        }
    }
}
static void core_set32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, 0, v); }
static void core_clear32(struct apple_atcphy *p, u32 o, u32 v) { core_mask32(p, o, v, 0); }
static void writel(u32 value, void *address) { *(u32 *)address = value; }
static void mask32(void *p, u32 m, u32 v) { * (u32 *)p = (*(u32 *)p & ~m) | v; }
static void set32(void *p, u32 v) { mask32(p, 0, v); }
static void clear32(void *p, u32 v) { mask32(p, v, 0); }
#define readl_poll_timeout_atomic(address, value, condition, interval, timeout) \
    ({ (value) = readl(address); polls++; \
       polls == fail_poll || !(condition) ? -ETIMEDOUT : 0; })
#define readl_poll_timeout readl_poll_timeout_atomic
static void atcphy_usb2_power_on(struct apple_atcphy *p) { (void)p; usb2_on++; }
static void atcphy_apply_tunables(struct apple_atcphy *p, enum atcphy_mode mode)
{ (void)p; (void)mode; }
static void atc_tunnel_stop_t8103(struct apple_atcphy *p) { (void)p; }
static void atc_tunnel_restore(struct apple_atcphy *p) { (void)p; }
/* These fixtures exercise host/PIPE ownership with electrical tunnels stopped. */
static int atc_t8122_tunnel_stop(struct apple_atcphy *p)
{ assert(!p->tunnel_clock_on && !p->tunnel_users); return 0; }
static void atcphy_dp_stop_t8122(struct apple_atcphy *p)
{ assert(!p->dp_t8122.aux && !p->dp_t8122.pll); }
static void atcphy_dp_aux_on_t8122(struct apple_atcphy *p)
{ assert(!p->hw->dp_t8122); }
static bool of_machine_is_compatible(const char *name) { (void)name; return false; }
#ifndef READ_ONCE
#define READ_ONCE(x) (x)
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(v, x) ((v) = (x))
#endif
static int atcphy_power_off(struct apple_atcphy *);
static int atcphy_configure(struct apple_atcphy *, enum atcphy_mode);

static int atcphy_park_pipehandler(struct apple_atcphy *p, bool keep_dp)
{ (void)keep_dp; p->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY; return 0; }
/* The T8140 profile never owes a T8103-generation live PIPE park. */
static int __attribute__((unused)) atcphy_park_live_t8103_pipehandler(struct apple_atcphy *p)
{ (void)p; assert(!"T8103 live PIPE park"); return -EINVAL; }
/* Observation is read-only and separately tested; retain production caller control flow. */
static int atcphy_configure_pipehandler(struct apple_atcphy *p, bool host)
{ (void)host; p->pipe_state = atcphy_modes[p->mode].pipehandler_state; return 0; }
''')
    for name in ('core_read32', 'atcphy_get_mode_config', 'j700_field_update', 'j700_pma_idle'):
        parts.append(function(atc, name))
    parts.append(dp)
    # The fixture hw below is the T8140 profile: only its post-tunable callback.
    for name in ('atcphy_configure_lanes', 'j700_aux_enable', 'j700_aux_disable',
                 'atcphy_enable_dp_aux', 'atcphy_disable_dp_aux', 'atcphy_power_off',
                 'atcphy_power_on', 'atcphy_wake_common_t8122',
                 'atcphy_configure_post_tunable_all_modes', 'atcphy_configure', 'atcphy_mux_set'):
        parts.append(function(atc, name))
    production = '\n'.join(parts)
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctz(m))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define hweight32(v) __builtin_popcount(v)
#define dev_info_once(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
static void report(void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define dev_err report
#define lockdep_assert_held(...) ((void)0)
#define WARN_ON_ONCE(v) assert(!(v))
#define CONFIG_PHY_APPLE_ATC_KUNIT_TEST 0
#define IS_ENABLED(option) (option)
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define guard(kind) (void)
#define fallthrough __attribute__((fallthrough))
#define ndelay(n) ((void)(n))
#define udelay(n) ((void)(n))
#define usleep_range(a, b) ((void)(a), (void)(b))
enum { TYPEC_STATE_SAFE, TYPEC_STATE_USB, TYPEC_MODE_USB4,
       TYPEC_DP_STATE_C, TYPEC_DP_STATE_D, TYPEC_DP_STATE_E };
enum { USB_TYPEC_TBT_SID = 0x8087, USB_TYPEC_DP_SID = 0xff01 };
#define EUDO_USB_MODE_MASK 3U
enum { EUDO_USB_MODE_USB2, EUDO_USB_MODE_USB3, EUDO_USB_MODE_USB4 };
struct enter_usb_data { u32 eudo; };
struct typec_altmode { unsigned int svid; };
struct typec_mux_state { unsigned long mode; struct typec_altmode *alt; void *data; };
struct typec_mux_dev { struct apple_atcphy *data; };
#define typec_mux_get_drvdata(mux) ((mux)->data)
'''
    suffix = r'''
static struct atcphy_hw hw = {
    .has_t8140_dp = true, .gen = ATCPHY_GENERATION_T8122,
    .configure_post_tunable = atcphy_configure_post_tunable_all_modes,
    .aciophy_lane_mode = 0x60, .aciophy_crossbar = 0x64,
};
static void initial(struct apple_atcphy *p, bool fixed, bool swapped)
{
    memset(registers, 0, sizeof(registers));
    registers[AUS_COMMON_DIG_RCAL1 / 4] = AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
    *p = (struct apple_atcphy) {
        .t8103_usb3_workaround = hw.gen == ATCPHY_GENERATION_T8103,
        .hw = &hw, .regs.core = (unsigned char *)registers,
        .regs.lpdptx = (unsigned char *)registers,
        .fixed_usb2 = fixed, .swap_lanes = swapped,
        .ss_tunables = true, .dp_tunables = true, .dp_presets_valid = { true },
        .mode = fixed ? APPLE_ATCPHY_MODE_USB2 : APPLE_ATCPHY_MODE_OFF,
        .pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY,
    };
    polls = fail_poll = writes = usb2_on = 0;
}
static int safe(struct apple_atcphy *p)
{
    struct typec_mux_dev mux = { p };
    struct typec_mux_state state = { .mode = TYPEC_STATE_SAFE };
    return atcphy_mux_set(&mux, &state);
}
int main(void)
{
    struct apple_atcphy p;
    unsigned int cases = 0;
    for (unsigned int fixed = 0; fixed < 2; fixed++) {
        for (unsigned int swapped = 0; swapped < 2; swapped++) {
            const enum atcphy_mode modes[] = {
                APPLE_ATCPHY_MODE_DP, APPLE_ATCPHY_MODE_USB3_DP, APPLE_ATCPHY_MODE_USB3,
            };
            for (unsigned int m = 0; m < ARRAY_SIZE(modes); m++) {
                unsigned int aux = modes[m] != APPLE_ATCPHY_MODE_USB3 ? 4 : 0;
                unsigned int idle = modes[m] == APPLE_ATCPHY_MODE_DP ? 12 : 6;
                for (unsigned int fault = 3; fault <= 2 + aux + idle; fault++) {
                    enum atcphy_mode old = fixed ? APPLE_ATCPHY_MODE_USB2 : APPLE_ATCPHY_MODE_OFF;
                    initial(&p, fixed, swapped);
                    fail_poll = fault;
                    assert(atcphy_configure(&p, modes[m]) == -ETIMEDOUT);
                    assert(p.mode == old && p.mode_dirty);
                    if (fault <= 2 + aux)
                        assert(p.aux_power_requested && !p.dp_idle_pmas);
                    else
                        assert(p.dp_idle_pmas);
                    bool owned_aux = p.aux_power_requested;
                    unsigned int owned_idle = p.dp_idle_pmas;
                    polls = 0;
                    fail_poll = 1;
                    usb2_on = 0;
                    assert(safe(&p) == -ETIMEDOUT);
                    assert(p.mode == old && p.mode_dirty);
                    assert(p.aux_power_requested == owned_aux && p.dp_idle_pmas == owned_idle);
                    assert(usb2_on == 0); /* Cleanup failure must not continue into power changes. */
                    polls = fail_poll = 0;
                    assert(safe(&p) == 0);
                    assert(p.mode == old && !p.mode_dirty);
                    assert(!p.aux_power_requested && !p.dp_idle_pmas && !p.dp_owned_pmas);
                    assert(usb2_on == fixed); /* Fixed-hub SAFE restores USB2, never powers it off. */
                    polls = writes = 0;
                    assert(safe(&p) == 0 && polls == 0 && writes == 0);
                    cases++;
                }
            }
        }
    }
    /* A failed route change must not expose the old DP mode to link training. */
    initial(&p, false, false);
    p.mode = APPLE_ATCPHY_MODE_DP;
    p.aux_power_requested = true;
    p.dp_owned_pmas = 3;
    assert(atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
    unsigned int last_idle_poll = polls - 1; /* The final poll is RCAL, after route idle. */
    initial(&p, false, false);
    p.mode = APPLE_ATCPHY_MODE_DP;
    p.aux_power_requested = true;
    p.dp_owned_pmas = 3;
    fail_poll = last_idle_poll;
    assert(atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
    assert(p.mode == APPLE_ATCPHY_MODE_DP && p.mode_dirty && p.dp_idle_pmas);
    struct phy_configure_opts_dp rate = {
        .set_rate = true, .set_lanes = true, .link_rate = 8100, .lanes = 4,
    };
    struct phy_configure_opts_dp training = { .set_voltages = true, .lanes = 4 };
    unsigned int writes_before = writes, idle_before = p.dp_idle_pmas;
    unsigned int owned_before = p.dp_owned_pmas;
    assert(j700_dp_configure(&p, &rate) == -EBUSY);
    assert(j700_dp_configure(&p, &training) == -EBUSY);
    assert(writes == writes_before && p.dp_idle_pmas == idle_before);
    assert(p.dp_owned_pmas == owned_before && p.aux_power_requested && p.mode_dirty);
    /* Explicit stop is allowed, but must leave the route cleanup obligation. */
    struct phy_configure_opts_dp stop = { .set_rate = true };
    assert(j700_dp_configure(&p, &stop) == 0);
    assert(p.dp_idle_pmas == idle_before && p.mode_dirty);
    fail_poll = polls = 0;
    assert(safe(&p) == 0 && !p.mode_dirty && !p.dp_idle_pmas && !p.aux_power_requested);
    /* If teardown itself fails, reject training without dropping live ownership. */
    initial(&p, false, false);
    p.mode = APPLE_ATCPHY_MODE_DP;
    p.aux_power_requested = true;
    p.dp_owned_pmas = 3;
    fail_poll = 1;
    assert(atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3_DP) == -ETIMEDOUT);
    assert(p.mode_dirty && p.dp_owned_pmas == 3);
    writes_before = writes;
    assert(j700_dp_configure(&p, &rate) == -EBUSY);
    assert(j700_dp_configure(&p, &training) == -EBUSY);
    assert(writes == writes_before && p.dp_owned_pmas == 3 && p.aux_power_requested);
    fail_poll = polls = 0;
    assert(safe(&p) == 0 && !p.mode_dirty && !p.dp_owned_pmas && !p.aux_power_requested);
    /* A cleanup failure also dirties an initially successful mode transition. */
    initial(&p, false, false);
    p.mode = APPLE_ATCPHY_MODE_DP;
    p.aux_power_requested = true;
    fail_poll = 1;
    assert(safe(&p) == -ETIMEDOUT && p.mode_dirty && p.aux_power_requested);
    fail_poll = polls = 0;
    assert(safe(&p) == 0 && !p.mode_dirty && !p.aux_power_requested);
    /* Existing main-link ownership survives a failed cleanup of a dirty mode. */
    initial(&p, true, false);
    p.mode_dirty = p.aux_power_requested = true;
    p.dp_owned_pmas = 3;
    fail_poll = 1;
    assert(safe(&p) == -ETIMEDOUT);
    assert(p.mode_dirty && p.aux_power_requested && p.dp_owned_pmas == 3);
    fail_poll = polls = 0;
    assert(safe(&p) == 0 && !p.mode_dirty && !p.aux_power_requested && !p.dp_owned_pmas);
    /* OFF is not committed when domain sleep fails, and SAFE retries it. */
    initial(&p, false, false);
    p.mode = APPLE_ATCPHY_MODE_USB2;
    fail_poll = 1;
    assert(safe(&p) == -ETIMEDOUT && p.mode == APPLE_ATCPHY_MODE_USB2 && p.mode_dirty);
    fail_poll = polls = 0;
    assert(safe(&p) == 0 && p.mode == APPLE_ATCPHY_MODE_OFF && !p.mode_dirty);
    printf("PASS: %u AUX/PMA fault scenarios, failed cleanup, dirty training guard, SAFE retry, fixed USB2, OFF commit\n", cases);
    return 0;
}
'''
    return prefix + production + suffix

def aux(root):

    source = root / 'drivers/phy/apple/atc.c'
    text = source.read_text()
    parts = [line for line in text.splitlines() if line.startswith(('#define J700_AUX_', '#define J700_PMA_'))]
    for name in ('j700_field_update', 'j700_pma_idle', 'j700_aux_enable', 'j700_aux_disable'):
        start = text.index('static int ' + name + '(')
        begin = text.index('{', start)
        depth = 1
        end = begin + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        parts.append(text[start:end])
    production = '\n'.join(parts)
    prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
struct apple_atcphy {
    bool dp_only, pd_was_off, tunnel_clock_on;
    unsigned int tunnel_rate, tunnel_users;
    struct { bool aux, pll; unsigned int pairs, rate; } dp_t8122; struct { unsigned char *core; } regs; bool aux_power_requested; };
struct event { u32 offset, mask, value; } events[32];
static u32 registers[0x18000 / 4];
static unsigned int writes, reject;
static void core_mask32(struct apple_atcphy *phy, u32 offset, u32 mask, u32 value)
{
    assert(offset < sizeof(registers) && offset % 4 == 0 && writes < 32);
    assert(!(value & ~mask));
    events[writes] = (struct event){offset, mask, value};
    writes++;
    if (writes != reject)
        registers[offset / 4] = (registers[offset / 4] & ~mask) | value;
    (void)phy;
}
static void usleep_range(unsigned int low, unsigned int high)
{
    assert(low && high >= low);
}
#define readl_poll_timeout_atomic(address, value, condition, interval, timeout) \
    ({ (value) = *(u32 *)(address); (condition) ? 0 : -110; })
'''
    suffix = r'''
static void initial(struct apple_atcphy *phy)
{
    memset(registers, 0, sizeof(registers));
    memset(events, 0, sizeof(events));
    registers[J700_AUX_SUPPLY_CONTROL / 4] = 0x80000200;
    registers[J700_AUX_ANALOG_CONTROL / 4] = 0x40000001;
    *phy = (struct apple_atcphy){ .regs.core = (unsigned char *)registers };
    writes = reject = 0;
}
int main(void)
{
    struct apple_atcphy phy;
    initial(&phy);
    assert(j700_aux_disable(&phy) == 0 && writes == 0);
    assert(j700_aux_enable(&phy) == 0 && phy.aux_power_requested && writes == 4);
    assert(registers[J700_AUX_SUPPLY_CONTROL / 4] == 0x8000010f);
    assert(registers[J700_AUX_ANALOG_CONTROL / 4] == 0x40000000);
    assert(events[0].mask == 3 && events[1].mask == 12);
    assert(events[2].mask == 0x300 && events[2].value == 0x100);
    assert(events[3].offset == J700_AUX_ANALOG_CONTROL);
    writes = 0;
    assert(j700_aux_disable(&phy) == 0 && !phy.aux_power_requested && writes == 4);
    assert(events[0].offset == J700_AUX_ANALOG_CONTROL);
    assert(events[1].mask == 0x300 && events[2].mask == 8 && events[3].mask == 2);
    assert(registers[J700_AUX_SUPPLY_CONTROL / 4] == 0x80000305);
    assert(registers[J700_AUX_ANALOG_CONTROL / 4] == 0x40000001);
    for (unsigned int fault = 1; fault <= 4; fault++) {
        initial(&phy);
        reject = fault;
        assert(j700_aux_enable(&phy) == -110 && phy.aux_power_requested);
        reject = 0;
        writes = 0;
        assert(j700_aux_disable(&phy) == 0 && !phy.aux_power_requested);
        initial(&phy);
        assert(j700_aux_enable(&phy) == 0);
        writes = 0;
        reject = fault;
        assert(j700_aux_disable(&phy) == -110 && phy.aux_power_requested);
    }
    initial(&phy);
    assert(j700_pma_idle(&phy, 2) == -EINVAL && writes == 0);
    for (unsigned int lane = 0; lane < 2; lane++) {
        initial(&phy);
        registers[J700_PMA_POWER_CONTROL / 4] = 0x800000ee;
        registers[J700_PMA_RECEIVER_CONTROL / 4] = 0x5000;
        assert(j700_pma_idle(&phy, lane) == 0 && writes == 6);
        assert(((registers[J700_PMA_POWER_CONTROL / 4] >> (4 * lane)) & 15) == 1);
        assert(((registers[J700_PMA_POWER_CONTROL / 4] >> (4 * (1 - lane))) & 15) == 14);
        assert(registers[J700_PMA_POWER_CONTROL / 4] & 0x80000000);
        assert(registers[J700_PMA_RECEIVER_CONTROL / 4] == (0x5000U | (0x44U << lane)));
        for (unsigned int fault = 1; fault <= 6; fault++) {
            initial(&phy);
            registers[J700_PMA_POWER_CONTROL / 4] = 0xee;
            reject = fault;
            assert(j700_pma_idle(&phy, lane) == -110);
        }
    }
    puts("PASS: sequencing, unrelated bits, every failed phase, retained cleanup ownership");
    return 0;
}
'''
    return prefix + production + suffix
