#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject production J700 mode, mux, AUX and PMA transitions (no hardware)."""
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
    struct { unsigned char *core, *lpdptx; } regs;
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
static int atcphy_park_pipehandler(struct apple_atcphy *p)
{ p->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY; return 0; }
static int atcphy_configure_pipehandler(struct apple_atcphy *p, bool host)
{ (void)host; p->pipe_state = atcphy_modes[p->mode].pipehandler_state; return 0; }
''')
for name in ('atcphy_get_mode_config', 'j700_field_update', 'j700_pma_idle'):
    parts.append(function(atc, name))
parts.append(dp)
for name in ('atcphy_configure_lanes', 'j700_aux_enable', 'j700_aux_disable',
             'atcphy_enable_dp_aux', 'atcphy_disable_dp_aux', 'atcphy_power_off',
             'atcphy_power_on', 'atcphy_configure', 'atcphy_mux_set'):
    parts.append(function(atc, name))
production = '\n'.join(parts)
print('Production transition SHA256:', hashlib.sha256(production.encode()).hexdigest())
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
    .aciophy_lane_mode = 0x60, .aciophy_crossbar = 0x64,
};
static void initial(struct apple_atcphy *p, bool fixed, bool swapped)
{
    memset(registers, 0, sizeof(registers));
    registers[AUS_COMMON_DIG_RCAL1 / 4] = AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
    *p = (struct apple_atcphy) {
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
with tempfile.TemporaryDirectory(prefix='apple-atc-unwind-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
