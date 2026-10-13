#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject production PIPE and mux callbacks against register mocks."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/phy/apple/atc.c').read_text()


def block(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def function(name):
    return block(re.search(r'^static [^\n]+\b' + name + r'\(', source, re.M).group())


parts = re.findall(r'^#define [^\n]*(?:\\\n[^\n]*)*', source, re.M)
for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode'):
    parts.append(block('enum ' + name + ' {') + ';')
for name in ('atcphy_mode_configuration', 'atcphy_hw'):
    parts.append(block('struct ' + name + ' {') + ';')
parts.append(r'''
struct apple_atcphy {
    const struct atcphy_hw *hw;
    struct { unsigned char *core, *pipehandler; } regs;
    void *dev;
    int lock;
    bool fixed_usb2, host_active, ss_tunables, dp_tunables, mode_dirty;
    unsigned int dp_owned_pmas;
    enum atcphy_mode mode, typec_mode;
    enum atcphy_pipehandler_state pipe_state;
};
''')
start = source.index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
parts.append(source[start:source.index('\n};', start) + 3])
parts.append(r'''
static u32 pipe_regs[0x1000 / 4], core_regs[0x30000 / 4];
static unsigned int polls, fail_poll, mux_writes, lane_calls;
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
#define readl_poll_timeout(address, value, condition, interval, timeout) \
    ({ (value) = readl(address); polls++; \
       polls == fail_poll || !(condition) ? -ETIMEDOUT : 0; })
/* Lane configuration is outside this test: count calls and commit its result. */
static int atcphy_configure(struct apple_atcphy *p, enum atcphy_mode mode)
{ lane_calls++; p->mode = mode; p->mode_dirty = false; return 0; }
''')
for name in ('atcphy_pipehandler_lock', 'atcphy_pipehandler_unlock', 'atcphy_pipehandler_check',
             'atcphy_pipehandler_set_mux', 'atcphy_clear_nonselected_phy_reset',
             'atcphy_configure_pipehandler_usb3_host_t8122',
             'atcphy_configure_pipehandler_usb3_host_t8103',
             'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
             'atcphy_enable_dummy_phy', 'atcphy_configure_pipehandler_dummy',
             'atcphy_configure_pipehandler', 'atcphy_park_pipehandler',
             'atcphy_usb3_set_mode', 'atcphy_mux_set'):
    parts.append(function(name))
production = '\n'.join(parts)
print('Production PIPE/mux SHA256:', hashlib.sha256(production.encode()).hexdigest(), flush=True)
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
#define field_prep FIELD_PREP
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define lockdep_assert_held(...) ((void)0)
#define WARN_ON_ONCE(v) assert(!(v))
#define guard(kind) (void)
#define fallthrough __attribute__((fallthrough))
#define udelay(n) ((void)(n))
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
        stuck_ack = true;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -ETIMEDOUT);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && mux_writes == 0);
        assert(!(pipe_regs[PIPEHANDLER_LOCK_REQ / 4] & PIPEHANDLER_LOCK_EN));
        stuck_ack = false;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == 0);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && mux_writes == 3);
        unsigned int before = polls;
        assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == 0);
        assert(polls == before && mux_writes == 3);
    }
    puts("PASS: both USB3 host BIST generations retain fatal lock ACK and retry");
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
        assert(p.dp_owned_pmas == 3 && mux_writes == 3);
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
    atcphy_park_pipehandler(&p);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && mux_writes == 0);
    stuck_ack = false;
    atcphy_park_pipehandler(&p);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && mux_writes == 3);
    atcphy_park_pipehandler(&p);
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
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "backend")) backend_failure();
    else if (!strcmp(argv[1], "host")) host_lock_failure();
    else if (!strcmp(argv[1], "mux")) mux_retry();
    else if (!strcmp(argv[1], "same")) same_mode_pending();
    else if (!strcmp(argv[1], "park")) parking_and_guards();
    else assert(false);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-atc-pipe-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(path / 'test')], check=True)
    failed = False
    for case in ('backend', 'host', 'mux', 'same', 'park'):
        result = subprocess.run([str(path / 'test'), case])
        if result.returncode:
            print('FAIL:', case, 'exit', result.returncode, flush=True)
            failed = True
    raise SystemExit(1 if failed else 0)
