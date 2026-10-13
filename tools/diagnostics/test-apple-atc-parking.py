#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject production PIPE parking, mux, power-off and reset callbacks."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
atc = (root / 'drivers/phy/apple/atc.c').read_text()
dp = (root / 'drivers/phy/apple/atc-j700-dp.h').read_text()


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def function(source, name):
    return block(source, re.search(r'^static [^\n]+\b' + name + r'\(', source, re.M).group())


# Use the production mode table and wrappers too, so SAFE mapping and fast paths
# are tested rather than mirrored by a second implementation.
parts = re.findall(r'^#define [^\n]*(?:\\\n[^\n]*)*', atc, re.M)
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
static void mask32(void *p, u32 m, u32 v)
{
    *(u32 *)p = (*(u32 *)p & ~m) | v;
    if (p == (void *)&pipe_regs[PIPEHANDLER_LOCK_REQ / 4] && !stuck_ack)
        pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = *(u32 *)p & PIPEHANDLER_LOCK_EN;
    if (p == (void *)&pipe_regs[PIPEHANDLER_AON_GEN / 4]) reset_writes++;
    if (p == (void *)&pipe_regs[PIPEHANDLER_MUX_CTRL / 4]) mux_writes++;
    if ((uintptr_t)p >= (uintptr_t)usb2_regs &&
        (uintptr_t)p < (uintptr_t)usb2_regs + sizeof(usb2_regs)) usb2_writes++;
}
static void writel(u32 value, void *address) { mask32(address, ~0U, value); }
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
''')
for name in ('atcphy_pipehandler_lock', 'atcphy_pipehandler_unlock', 'atcphy_pipehandler_check',
             'atcphy_pipehandler_set_mux', 'atcphy_clear_nonselected_phy_reset',
             'atcphy_configure_pipehandler_usb3_host_t8122',
             'atcphy_configure_pipehandler_usb3_host_t8103',
             'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
             'atcphy_enable_dummy_phy', 'atcphy_configure_pipehandler_dummy',
             'atcphy_configure_pipehandler', 'atcphy_park_pipehandler',
             'atcphy_usb2_power_off', '_atcphy_dwc3_reset_assert', 'atcphy_dwc3_reset_assert'):
    parts.append(function(atc, name))
for name in ('atcphy_get_mode_config', 'j700_field_update', 'j700_pma_idle'):
    parts.append(function(atc, name))
parts.append(dp)
for name in ('atcphy_configure_lanes', 'j700_aux_enable', 'j700_aux_disable',
             'atcphy_enable_dp_aux', 'atcphy_disable_dp_aux', 'atcphy_power_off',
             'atcphy_power_on', 'atcphy_configure', 'atcphy_mux_set', 'atcphy_usb3_power_off'):
    parts.append(function(atc, name))
production = '\n'.join(parts)
print('Production parking/caller SHA256:', hashlib.sha256(production.encode()).hexdigest(), flush=True)
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
struct phy { struct apple_atcphy *data; };
#define phy_get_drvdata(phy) ((phy)->data)
struct reset_controller_dev { int unused; };
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
'''
suffix = r'''
static struct atcphy_hw hw = {
    .has_t8140_dp = true, .gen = ATCPHY_GENERATION_T8122,
    .aciophy_lane_mode = ACIOPHY_LANE_MODE_T8122,
    .aciophy_crossbar = ACIOPHY_CROSSBAR_T8122,
    .pipe_mux_clk = PIPEHANDLER_MUX_CTRL_CLK_T8103,
    .pipe_mux_data = PIPEHANDLER_MUX_CTRL_DATA_T8103,
};
static void initial(struct apple_atcphy *p, bool fixed, bool dirty, bool swapped)
{
    memset(registers, 0, sizeof(registers));
    memset(pipe_regs, 0, sizeof(pipe_regs));
    memset(usb2_regs, 0, sizeof(usb2_regs));
    registers[AUS_COMMON_DIG_RCAL1 / 4] = AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
    pipe_regs[PIPEHANDLER_AON_GEN / 4] = PIPEHANDLER_AON_GEN_DWC3_RESET_N;
    usb2_regs[USB2PHY_USBCTL / 4] = USB2PHY_USBCTL_RUN;
    *p = (struct apple_atcphy) {
        .hw = &hw, .regs.core = (unsigned char *)registers,
        .regs.lpdptx = (unsigned char *)registers,
        .regs.pipehandler = (unsigned char *)pipe_regs,
        .regs.usb2phy = (unsigned char *)usb2_regs,
        .fixed_usb2 = fixed, .host_active = true, .swap_lanes = swapped,
        .ss_tunables = true, .dp_tunables = true, .dp_presets_valid = { true },
        .mode = APPLE_ATCPHY_MODE_USB3_DP, .typec_mode = APPLE_ATCPHY_MODE_USB3_DP,
        .pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3, .mode_dirty = dirty,
        .dp_owned_pmas = 3, .dp_idle_pmas = dirty ? 1 : 0,
        .aux_power_requested = true,
    };
    polls = fail_poll = writes = usb2_on = usb2_writes = reset_writes = mux_writes = 0;
    stuck_ack = false;
}
static void inject_parking(unsigned int fault)
{
    pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = PIPEHANDLER_LOCK_EN;
    if (fault)
        fail_poll = 1;
    else
        stuck_ack = true;
}
static void recover(void) { stuck_ack = false; fail_poll = polls = 0; }
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
static void unchanged(struct apple_atcphy *p, bool dirty, bool host)
{
    assert(p->mode == APPLE_ATCPHY_MODE_USB3_DP && p->mode_dirty == dirty);
    assert(p->pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && p->host_active == host);
    assert(p->dp_owned_pmas == 3 && p->dp_idle_pmas == (dirty ? 1U : 0U));
    assert(p->aux_power_requested);
    assert(writes == 0 && usb2_on == 0 && usb2_writes == 0 && reset_writes == 0);
    assert(mux_writes == 0);
    assert(pipe_regs[PIPEHANDLER_AON_GEN / 4] == PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    assert(usb2_regs[USB2PHY_USBCTL / 4] == USB2PHY_USBCTL_RUN);
    if (dirty) {
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
        unchanged(&p, dirty, true);
        if (!fault) {
            assert(atcphy_usb3_power_off(&phy) == -ETIMEDOUT);
            unchanged(&p, dirty, true);
        }
        recover();
        assert(atcphy_usb3_power_off(&phy) == 0);
        assert(p.mode == APPLE_ATCPHY_MODE_OFF && !p.mode_dirty && !p.host_active);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
        assert(!p.dp_owned_pmas && !p.dp_idle_pmas && !p.aux_power_requested);
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
        unchanged(&p, dirty, true);
        if (!fault) {
            assert(atcphy_dwc3_reset_assert(&p.rcdev, 0) == -ETIMEDOUT);
            unchanged(&p, dirty, true);
        }
        recover();
        assert(atcphy_dwc3_reset_assert(&p.rcdev, 0) == 0);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !p.host_active);
        assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && p.mode_dirty == dirty);
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
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "mux")) mux_parking();
    else if (!strcmp(argv[1], "power")) power_parking();
    else if (!strcmp(argv[1], "reset")) reset_parking();
    else assert(false);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-atc-parking-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(path / 'test')], check=True)
    failed = False
    for case in ('mux', 'power', 'reset'):
        result = subprocess.run([str(path / 'test'), case])
        if result.returncode:
            print('FAIL:', case, 'exit', result.returncode, flush=True)
            failed = True
    raise SystemExit(1 if failed else 0)
