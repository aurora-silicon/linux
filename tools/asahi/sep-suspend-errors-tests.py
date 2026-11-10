#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile actual SEP commit/error paths with host-only dependency stubs.

Extract current C stop/finish/prepare/noirq, provider poll-one and registration,
Rust wrappers/enter/leave/exit/kick, and the kernel polling macros verbatim.
C fatal paths are observed wholly within C, never by unwinding a Rust frame.
Virtual time/MMIO only test AP control decisions, not firmware timing or safety.
No target access or kernel build occurs.
"""
import argparse
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^[ \t]*(?:(?:pub(?:\([^)]*\))? )?fn|(?:static )?(?:int|bool|void)(?: __maybe_unused)?) ' + re.escape(name) + r'\([^;]*?\)\s*(?:->[^\{]+)?\{',
                      source, re.M)
    if not match:
        raise AssertionError(f'missing function: {name}')
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def macro(source, name):
    match = re.search(r'^#define ' + re.escape(name) + r'\([^\n]*\n(?:.*\\\n)*.*',
                      source, re.M)
    if not match:
        raise AssertionError(f'missing macro: {name}')
    return match.group()


def replace_once(source, before, after):
    # A stale mutation is a broken harness, never a killed behavioral control.
    if source.count(before) != 1:
        raise AssertionError(f'mutation requires one exact match: {before!r}')
    return source.replace(before, after, 1)


# Mutations apply to an extracted real function, not to a mock decision model.
# All must compile and fail a C assertion or a Rust test assertion in remote CI.
MUTATIONS = {
    'unsupported-as-success': ('leave_for_commit', 'Err(Error::from_errno(rc))', 'Ok(())'),
    'read-as-deadline': ('sep_pm_wait_gated', 'if (ret)\n\t\treturn ret;\n\n\treturn !!poll_ret;',
                         'if (ret)\n\t\treturn 1;\n\n\treturn !!poll_ret;'),
    'no-capability-check': ('sep_pm_commit_prepare',
                           ' || !IS_ENABLED(CONFIG_APPLE_SOC_SLEEP) ||\n\t    !is_kernel_in_hyp_mode()', ''),
    'no-transport-check': ('sep_pm_commit_prepare',
                          'if (!apple_mbox_poll_one_supported(mbox))', 'if (false)'),
    'lost-accept': ('sleep_enter', 'pm::SleepGate::Lost => return Err(EIO),',
                    'pm::SleepGate::Lost => return Ok(()),'),
    'nonphysical-wake': ('sleep_exit', 'let real = unsafe { bindings::pm_resume_via_firmware() };',
                         'let real = true;'),
    'wake-before-arm': ('sleep_kick',
                        'self.control.lock().wake_reset();\n        self.sleep_release_rx();',
                        'self.sleep_release_rx();\n        self.control.lock().wake_reset();'),
    'bulk-error-as-success': ('sep_pm_commit_stop', 'if (ret < 0)\n\t\tgoto out;',
                              'if (ret < 0) { ret = 0; goto out; }'),
    'omit-pending-wake': ('sep_pm_commit_stop', 'if (pm_wakeup_pending())', 'if (false)'),
    'omit-before-send': ('sep_pm_commit_stop', 'if (xchg(&sleep_refuse_before_send, false))',
                         'if (false)'),
    'leak-prepared': ('sep_pm_commit_stop', 'if (prepared)\n\t\t\tapple_dart_sleep_restore(c->dart);',
                      'if (false)\n\t\t\tapple_dart_sleep_restore(c->dart);'),
    'sent-error-return': ('sep_pm_commit_stop',
                          'panic("SEP SLEEP failed after send: ret %d reply_valid %d result %#x state %#x",\n\t\t      ret, c->answered, c->reply_result, c->state);',
                          'return ret;'),
    'accept-unanswered-gate': ('sep_pm_commit_stop', 'if (ret >= 0 && !c->answered)',
                               'if (ret >= 0 && !c->answered && !(c->state & SEP_PS_DEV_DISABLE))'),
    'ignore-result': ('sep_pm_commit_stop', 'if (c->answered && c->reply_result)',
                      'if (false && c->answered && c->reply_result)'),
    'ignore-gate-timeout': ('sep_pm_commit_stop', 'if (c->gate_ret)', 'if (false && c->gate_ret)'),
    'finish-without-physical': ('sep_pm_commit_finish', 'if (c->sent && !pm_resume_via_firmware())',
                               'if (false)'),
    'poll-after-callback': ('apple_mbox_poll_one', 'mbox->rx(mbox, msg, mbox->cookie);',
                            'mbox->rx(mbox, msg, mbox->cookie);\n\t\tcontrol = readl_relaxed(mbox->regs + mbox->hw->i2a_control);'),
    'poll-unsupported': ('apple_mbox_poll_one', 'if (!apple_mbox_poll_one_supported(mbox))',
                         'if (false)'),
    'stale-observation': ('sep_pm_commit_prepare', 'c->answered = false;', '/* stale answered */'),
    'drain-two': ('apple_mbox_poll_one', 'ret = 1;',
                  'msg = apple_mbox_read_msg(mbox);\n\t\tmbox->rx(mbox, msg, mbox->cookie);\n\t\tret = 2;'),
    'post-send-wake-refusal': ('sep_pm_commit_stop', 'c->sent = true;',
                              'c->sent = true;\n\tif (pm_wakeup_pending()) return -EBUSY;'),
}

C_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef int64_t s64; typedef long long ktime_t;
#define BIT(n) (1u << (n))
#define SEP_PS_DEV_DISABLE BIT(10)
#define USEC_PER_MSEC 1000
#define NSEC_PER_USEC 1000
#define READ_ONCE(x) (x)
#define IS_ENABLED(x) (config_enabled)
#define CONFIG_APPLE_SOC_SLEEP 1
#define __maybe_unused
#define __iomem
#define unlikely(x) (x)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define might_sleep_if(x) ((void)0)
#define barrier() ((void)0)
#define cpu_relax() ((void)0)
#define guard(kind) (void)
#define xchg(p, v) ({ __typeof__(*(p)) old = *(p); *(p) = (v); old; })
struct device_node { struct device_node *parent; };
struct device { struct device_node *of_node; };
struct resource { unsigned long start; };
struct regmap { int unused; };
struct apple_mbox_msg { u64 msg0; u32 msg1; };
struct apple_mbox_hw {
    bool is_ascwrap_v8, counted_fifo, has_irq_controls; unsigned i2a_control;
};
struct apple_mbox {
    void *regs; struct apple_mbox_hw *hw; bool removing; int rx_lock;
    void (*rx)(struct apple_mbox *, struct apple_mbox_msg, void *); void *cookie;
};
static struct device_node parent, node = { &parent };
static struct device device = { &node }, dart;
static struct regmap regmap;
static struct apple_mbox_hw hw;
static struct apple_mbox mbox;
static int apple_sleep_coprocs_lock;
static void *apple_sleep;
static struct {
    struct device *dev; int (*stop)(void *); void *data;
} apple_sleep_enclave;
static bool config_enabled, hyp, sep_pm_suspending, dart_present, dart_ready, map_ok;
static bool firmware, real_wake, pending_wake, late_wake, sleep_refuse_before_send, tx_ready;
static int domain_error, property_error, address_error, read_error, read_error_after;
static int gate_after, reads, put_refs, maps, unmaps, lookups, prepares, restores;
static int prepare_error, send_error, bulk_error, one_error, sends, closes;
static int mailbox_reads, message_reads, forwarded, lock_depth;
static int resource_bytes, close_gate, early_gate;
static bool message_in_lock, matched_read;
static struct apple_mbox_msg queue[4];
static unsigned queued, consumed;
static ktime_t now;
static bool (*sep_pm_told)(void);
static bool yes(void) { return true; }
static bool no(void) { return false; }
static bool pm_suspend_via_firmware(void) { return firmware; }
static bool pm_resume_via_firmware(void) { return real_wake; }
static bool is_kernel_in_hyp_mode(void) { return hyp; }
static bool pm_wakeup_pending(void) { return pending_wake; }
static int sep_pm_domain(void *p, struct regmap **m, u32 *o) {
    lookups++; *m = &regmap; *o = 0; return domain_error;
}
static int regmap_read(struct regmap *m, u32 o, u32 *v) {
    ++reads; if (read_error && reads >= read_error_after) return read_error;
    *v = gate_after && reads >= gate_after ? SEP_PS_DEV_DISABLE : 0; return 0;
}
static int regmap_write(struct regmap *m, u32 o, u32 v) { return 0; }
static ktime_t ktime_get(void) { return now; }
static ktime_t ktime_add_us(ktime_t t, u64 us) { return t + us; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
static void usleep_range(unsigned long a, unsigned long b) { now += b; }
static void udelay(unsigned long us) { now += us; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int i) {
    lookups++; return domain_error ? NULL : &node;
}
static int of_property_read_u32(struct device_node *n, const char *p, u32 *v) {
    *v = 0; return property_error;
}
static int of_address_to_resource(struct device_node *n, int i, struct resource *r) {
    r->start = 0; return address_error;
}
static void of_node_put(struct device_node *n) {}
static unsigned resource_size(struct resource *r) { return resource_bytes; }
static struct device *sep_pm_dart(struct device *d) { return dart_present ? &dart : NULL; }
static void *dev_get_drvdata(struct device *d) { return dart_ready ? d : NULL; }
static void *ioremap(unsigned long start, unsigned size) { ++maps; return map_ok ? &regmap : NULL; }
static void iounmap(void *p) { ++unmaps; }
static void put_device(struct device *d) { ++put_refs; }
static int apple_dart_sleep_prepare(struct device *d) { ++prepares; return prepare_error; }
static void apple_dart_sleep_restore(struct device *d) { ++restores; }
static void lock_rx(void) {
    assert(lock_depth == 0); lock_depth++; message_in_lock = false;
}
static void unlock_rx(void) { assert(lock_depth == 1); lock_depth--; message_in_lock = false; }
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; lock_rx(); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); unlock_rx(); } while (0)
static void foreign_rx(struct apple_mbox *m, struct apple_mbox_msg msg, void *cookie) {
    assert(cookie == &device); ++forwarded;
}
static u32 readl_relaxed(void *p) {
    // A second status/ACK/read after delivering a message is forbidden.
    assert(!message_in_lock && !matched_read); ++mailbox_reads;
    return queued == consumed;
}
static bool apple_mbox_rx_empty(struct apple_mbox *m, u32 control) { return control != 0; }
static struct apple_mbox_msg apple_mbox_read_msg(struct apple_mbox *m) {
    assert(!message_in_lock && !matched_read); assert(consumed < queued);
    message_in_lock = true; ++message_reads;
    struct apple_mbox_msg msg = queue[consumed++];
    if ((msg.msg0 & 0xffffff) == 0x1234) matched_read = true;
    return msg;
}
static int apple_mbox_poll(struct apple_mbox *m) { return bulk_error; }
static bool apple_mbox_send_ready(struct apple_mbox *m) { return tx_ready; }
static int apple_mbox_send(struct apple_mbox *m, struct apple_mbox_msg msg, bool atomic) {
    assert(atomic); assert(prepares == 1);
    // Production apple_mbox_send has only pre-write negative exits.
    if (send_error) return send_error;
    ++sends;
    if (late_wake) pending_wake = true;
    if (one_error == -ENODEV) m->removing = true;
    if (one_error == -EOPNOTSUPP) m->hw->is_ascwrap_v8 = true;
    return 0;
}
static void apple_mbox_set_recv_enabled(struct apple_mbox *m, bool enabled) {
    assert(!enabled); assert(matched_read); ++closes;
}
static u32 readl(void *p) {
    // PMGR is the sole permitted query after the matched reply/receive close.
    return early_gate || (closes && close_gate) ? SEP_PS_DEV_DISABLE : 0;
}
static jmp_buf fatal_env;
static bool fatal_armed;
static char fatal_message[256];
static _Noreturn void panic_observe(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(fatal_message, sizeof(fatal_message), fmt, ap); va_end(ap);
    assert(fatal_armed && "unexpected C panic (must never unwind across Rust)");
    longjmp(fatal_env, 1);
}
#define panic panic_observe
'''

# Appended after the actual functions/struct: only dependency state and drivers.
C_CONTROLS = r'''
void fixture(void) {
    memset(&sep_pm_commit, 0, sizeof(sep_pm_commit));
    memset(&apple_sleep_enclave, 0, sizeof(apple_sleep_enclave));
    memset(&hw, 0, sizeof(hw));
    mbox = (struct apple_mbox){ .regs = &hw, .hw = &hw, .rx = foreign_rx, .cookie = &device };
    config_enabled = hyp = sep_pm_suspending = dart_present = dart_ready = map_ok = true;
    firmware = tx_ready = true; apple_sleep = &device;
    real_wake = pending_wake = late_wake = sleep_refuse_before_send = false;
    domain_error = property_error = address_error = read_error = gate_after = reads = 0;
    put_refs = maps = unmaps = lookups = prepares = restores = sends = closes = 0;
    prepare_error = send_error = bulk_error = one_error = 0;
    mailbox_reads = message_reads = forwarded = lock_depth = 0;
    resource_bytes = 4; close_gate = 1; early_gate = 0; read_error_after = 1;
    queued = consumed = 0; message_in_lock = matched_read = false; now = 0;
    fatal_armed = false; fatal_message[0] = 0; sep_pm_told = yes;
}
void parameter(int which, int value) {
    switch (which) {
    case 0: config_enabled = value; break; case 1: hyp = value; break;
    case 2: sep_pm_suspending = value; break; case 3: dart_present = value; break;
    case 4: map_ok = value; break; case 5: domain_error = value; break;
    case 6: read_error = value; break; case 7: apple_sleep = value ? &device : NULL; break;
    case 8: gate_after = value; break; case 9: sep_pm_commit.registered = value; break;
    case 10: read_error_after = value; break; case 11: dart_ready = value; break;
    case 12: real_wake = value; break; case 13: hw.is_ascwrap_v8 = value; break;
    case 14: hw.counted_fifo = value; break; case 15: hw.has_irq_controls = value; break;
    case 16: property_error = value; break; case 17: address_error = value; break;
    case 18: resource_bytes = value; break;
    case 19: apple_sleep_enclave.dev = value ? &device : NULL; break;
    case 20: sep_pm_commit.sent = value; break; case 21: firmware = value; break;
    case 22: sep_pm_told = value ? yes : NULL; break;
    }
}
int counter(int which) {
    switch (which) {
    case 0: return put_refs; case 1: return maps; case 2: return unmaps;
    case 3: return sep_pm_commit.registered; case 4: return lookups;
    case 5: return reads; case 6: return sends; case 7: return mailbox_reads;
    default: assert(0); return -1;
    }
}
void *device_pointer(void) { return &device; }
void *mbox_pointer(void) { return &mbox; }
int noirq(void) { return sep_pm_suspend_noirq(&device); }

#ifdef CONTROL_MAIN
static int stage(void) {
    return sep_pm_commit_prepare(&device, &mbox, 0x55, 0, 0x1234, 0xffffff);
}
static void reply(unsigned result) {
    assert(queued < 4); queue[queued++] = (struct apple_mbox_msg){0x1234 | ((u64)result << 24), 0};
}
static void balanced(void) {
    assert(lock_depth == 0); assert(mbox.rx == foreign_rx && mbox.cookie == &device);
}
static void expect_stop_fatal(const char *reason) {
    // This setjmp frame, production callback and panic observer are all C.
    fatal_armed = true;
    if (setjmp(fatal_env) == 0) {
        (void)sep_pm_commit_stop(&sep_pm_commit);
        assert(0 && "sent failure must panic, never return");
    }
    fatal_armed = false;
    assert(strstr(fatal_message, reason)); balanced();
    assert(sep_pm_commit.sent && sends == 1 && restores == 0);
    assert(sep_pm_commit.registered && put_refs == 0 && unmaps == 0);
}
static void test_prepare_and_cleanup(void) {
    // Unsupported RAM guards do no mailbox MMIO, mapping or sending.
    for (int p = 0; p < 3; ++p) {
        fixture(); parameter(p, 0); assert(stage() == -EOPNOTSUPP);
        assert(!maps && !lookups && !sends && !mailbox_reads);
    }
    for (int p = 13; p <= 15; ++p) {
        fixture(); parameter(p, 1); assert(stage() == -EOPNOTSUPP);
        assert(!maps && !lookups && !sends && !mailbox_reads);
    }
    // Offset/resource/property/ref/registration failures retain useful negatives.
    const int cases[][5] = {
        {5,-ENOENT,-ENOENT,0,0}, {16,-EIO,-EIO,0,0}, {17,-EINVAL,-EINVAL,0,0},
        {18,3,-EINVAL,0,0}, {3,0,-ENODEV,0,0}, {11,0,-ENODEV,1,0},
        {4,0,-ENOMEM,1,0}, {7,0,-EOPNOTSUPP,1,1}, {19,1,-EBUSY,1,1},
        {9,1,-EBUSY,0,0},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        fixture(); parameter(cases[i][0], cases[i][1]);
        assert(stage() == cases[i][2]); assert(put_refs == cases[i][3]); assert(unmaps == cases[i][4]);
        assert(!sends && !mailbox_reads);
    }
    fixture(); sep_pm_commit.sent = sep_pm_commit.answered = sep_pm_commit.gate_valid = true;
    sep_pm_commit.reply_result = 0; sep_pm_commit.answer_us = 90; sep_pm_commit.state = 42;
    assert(stage() == 0); assert(!sep_pm_commit.sent && !sep_pm_commit.answered);
    assert(!sep_pm_commit.gate_valid && sep_pm_commit.gate_ret == -EINVAL);
    assert(sep_pm_commit.reply_result == 0xff && !sep_pm_commit.answer_us && !sep_pm_commit.state);
    assert(!sends && !mailbox_reads); assert(sep_pm_commit_finish(&dart) == -ENOENT);
    assert(sep_pm_commit.registered); assert(sep_pm_commit_finish(&device) == 0);
    assert(put_refs == 1 && unmaps == 1 && !apple_sleep_enclave.dev);
    assert(sep_pm_commit_finish(&device) == -ENOENT);
}
static void test_unsent(void) {
    for (int scenario = 0; scenario < 7; ++scenario) {
        fixture(); assert(stage() == 0); reply(0); int expected;
        switch (scenario) {
        case 0: bulk_error = -EIO; expected = -EIO; break;
        case 1: bulk_error = -ETIMEDOUT; expected = -ETIMEDOUT; break;
        case 2: tx_ready = false; expected = -ETIMEDOUT; break;
        case 3: prepare_error = -EIO; expected = -EIO; break;
        case 4: pending_wake = true; expected = -EBUSY; break;
        case 5: sleep_refuse_before_send = true; expected = -ECANCELED; break;
        default: send_error = -ENODEV; expected = -ENODEV; break;
        }
        assert(sep_pm_commit_stop(&sep_pm_commit) == expected); balanced();
        assert(!sends && !sep_pm_commit.sent && !closes && !message_reads);
        assert(!sep_pm_commit.answered && !sep_pm_commit.gate_valid);
        assert(restores == (scenario >= 4)); assert(prepares == (scenario >= 3));
        if (scenario == 5) assert(!sleep_refuse_before_send);
        assert(sep_pm_commit_finish(&device) == 0);
        assert(put_refs == 1 && unmaps == 1 && !apple_sleep_enclave.dev);
    }
}
static void test_sent(void) {
    fixture(); assert(stage() == 0); late_wake = true; bulk_error = 3;
    queue[queued++] = (struct apple_mbox_msg){0x7777, 0}; reply(0);
    assert(sep_pm_commit_stop(&sep_pm_commit) == 0); balanced();
    assert(sends == 1 && closes == 1 && forwarded == 1 && message_reads == 2);
    assert(pending_wake); // A wake after the send decision cannot cancel this stop.
    assert(sep_pm_commit.answered && !sep_pm_commit.reply_result && sep_pm_commit.gate_valid);
    assert(!sep_pm_commit.gate_ret && !restores); real_wake = true;
    assert(sep_pm_commit_finish(&device) == 1); assert(put_refs == 1 && unmaps == 1);

    fixture(); assert(stage() == 0); one_error = -ENODEV; expect_stop_fatal("ret -19 "); assert(!closes);
    fixture(); assert(stage() == 0); one_error = -EOPNOTSUPP;
    expect_stop_fatal("ret -95 "); assert(!closes);
    fixture(); assert(stage() == 0); reply(7); expect_stop_fatal("ret -121 ");
    assert(sep_pm_commit.answered && sep_pm_commit.reply_result == 7 && !closes);
    fixture(); assert(stage() == 0); early_gate = 1;
    expect_stop_fatal("ret -112 "); assert(!sep_pm_commit.answered && !closes && !mailbox_reads);
    fixture(); assert(stage() == 0); expect_stop_fatal("ret -110 ");
    assert(!sep_pm_commit.answered && !closes);
    fixture(); assert(stage() == 0); reply(0); close_gate = 0;
    expect_stop_fatal("did not gate after send: -110");
    assert(closes == 1 && sep_pm_commit.gate_valid && sep_pm_commit.gate_ret == -ETIMEDOUT);

    fixture(); assert(stage() == 0); reply(0); assert(sep_pm_commit_stop(&sep_pm_commit) == 0);
    fatal_armed = true;
    if (setjmp(fatal_env) == 0) {
        (void)sep_pm_commit_finish(&device);
        assert(0 && "sent finish requires physical wake");
    }
    fatal_armed = false; assert(strstr(fatal_message, "without a physical sleep/wake"));
    assert(sep_pm_commit.registered && !put_refs && !unmaps && apple_sleep_enclave.dev);
}
static void test_provider(void) {
    for (int p = 13; p <= 15; ++p) {
        fixture(); parameter(p, 1); assert(!apple_mbox_poll_one_supported(&mbox));
        assert(apple_mbox_poll_one(&mbox) == -EOPNOTSUPP);
        assert(!mailbox_reads && !message_reads && !forwarded); balanced();
    }
    fixture(); mbox.removing = true; assert(apple_mbox_poll_one(&mbox) == -ENODEV);
    assert(!mailbox_reads); balanced();
    fixture(); mbox.rx = NULL; assert(apple_mbox_poll_one(&mbox) == 0); assert(!mailbox_reads);
    fixture(); assert(apple_mbox_poll_one(&mbox) == 0);
    assert(mailbox_reads == 1 && !message_reads && !forwarded); balanced();
    fixture(); queue[queued++] = (struct apple_mbox_msg){0x7777, 0};
    queue[queued++] = (struct apple_mbox_msg){0x8888, 0};
    assert(apple_mbox_poll_one(&mbox) == 1);
    assert(consumed == 1 && forwarded == 1 && mailbox_reads == 1 && message_reads == 1); balanced();
    assert(apple_mbox_poll_one(&mbox) == 1);
    assert(consumed == 2 && forwarded == 2 && mailbox_reads == 2 && message_reads == 2); balanced();
}
static void test_poll_contract(void) {
    fixture(); assert(sep_pm_wait_gated(&device, 5) == 1); int n = reads; assert(n > 1);
    fixture(); gate_after = n; assert(sep_pm_wait_gated(&device, 5) == 0 && reads == n);
    fixture(); gate_after = n + 1; assert(sep_pm_wait_gated(&device, 5) == 1 && reads == n);
    for (int e = 0; e < 2; ++e) {
        int err = e ? -ETIMEDOUT : -EIO;
        fixture(); read_error = err; assert(sep_pm_wait_gated(&device, 5) == err);
        fixture(); read_error = err; read_error_after = n;
        assert(sep_pm_wait_gated(&device, 5) == err && reads == n);
        fixture(); read_error = err; assert(noirq() == err);
    }
    fixture(); domain_error = -ENOENT; assert(sep_pm_wait_gated(&device, 5) == -ENOENT);
    assert(noirq() == 0);
    fixture(); domain_error = -EIO; assert(noirq() == -EIO);
    fixture(); assert(noirq() == -ETIMEDOUT);
    fixture(); gate_after = 1; assert(noirq() == 0);
    fixture(); firmware = false; assert(noirq() == 0 && !lookups);
    fixture(); sep_pm_told = NULL; assert(noirq() == 0 && !lookups);
    fixture(); sep_pm_told = no; assert(noirq() == 0 && !lookups);
}
int main(void) {
    test_prepare_and_cleanup(); test_unsent(); test_provider(); test_sent(); test_poll_contract();
    puts("SEP C commit controls passed"); return 0;
}
#endif
'''

RUST_PREFIX = r'''
#![allow(dead_code, unused_imports, unused_variables)]
use std::cell::{Cell, RefCell, RefMut};
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicU32, Ordering, Ordering::Relaxed};
#[derive(Clone, Copy, Debug, PartialEq, Eq)] struct Error(i32);
impl Error { fn from_errno(n: i32) -> Self { assert!(n < 0); Self(n) } }
type Result<T = ()> = std::result::Result<T, Error>;
const EBUSY: Error = Error(-16); const ENODEV: Error = Error(-19);
const ENOMEM: Error = Error(-12); const EIO: Error = Error(-5);
const ENOENT: Error = Error(-2); const EINVAL: Error = Error(-22);
const ETIMEDOUT: Error = Error(-110); const EOPNOTSUPP: Error = Error(-95);
macro_rules! dev_err { ($($a:tt)*) => {{}}; }
macro_rules! dev_warn { ($($a:tt)*) => {{}}; }
macro_rules! dev_dbg { ($($a:tt)*) => {{}}; }
macro_rules! dev_info { ($($a:tt)*) => {{}}; }
extern "C" {
    fn fixture(); fn parameter(which: i32, value: i32); fn counter(which: i32) -> i32;
    fn device_pointer() -> *mut c_void; fn mbox_pointer() -> *mut c_void;
}
thread_local! {
    static REAL: Cell<bool> = const { Cell::new(false) };
    static EVENTS: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
    static RING: Cell<Result<()>> = const { Cell::new(Ok(())) };
}
fn event(n: u8) { EVENTS.with(|v| v.borrow_mut().push(n)); }
mod bindings { pub unsafe fn pm_resume_via_firmware() -> bool { super::REAL.with(|v| v.get()) } }
mod kernel {
    pub mod device {
        pub struct Device;
        impl Device { pub fn as_raw(&self) -> *mut std::ffi::c_void { unsafe { crate::device_pointer() } } }
    }
    pub mod bindings { #[allow(non_camel_case_types)] pub enum apple_mbox {} }
    pub mod soc { pub mod apple { pub mod mailbox {
        #[derive(Clone, Copy)] pub struct Message { pub msg0: u64, pub msg1: u32 }
    } } }
    pub mod time {
        pub struct Delta; impl Delta { pub fn from_millis(_: i64) -> Self { Self } }
        pub mod delay { pub fn fsleep(_: super::Delta) {} }
    }
}
mod time { pub fn msecs_to_jiffies(ms: u32) -> u64 { ms.into() } }
mod proto {
    pub const EP_CONTROL: u32 = 0; pub const CONTROL_TIMEOUT_MS: u32 = 5000;
    pub const TAG_SLEEP: u32 = 0; pub const SLEEP_REPLY: u64 = 0x1234;
    pub const SLEEP_REPLY_MASK: u64 = 0xffffff;
    pub const WAKE_KICK: crate::kernel::soc::apple::mailbox::Message =
        crate::kernel::soc::apple::mailbox::Message { msg0: 0x99, msg1: 0 };
    pub fn op_sleep() {}
    pub fn encode_control(_: &(), _: u32) -> crate::kernel::soc::apple::mailbox::Message {
        crate::kernel::soc::apple::mailbox::Message { msg0: 0x55, msg1: 0 }
    }
}
mod control {
    pub fn wait_idle<F: FnMut() -> bool, D: FnMut(u32)>(_: u32, _: u32, mut f: F, _: D) -> bool { !f() }
}
struct Lock<T>(RefCell<T>);
impl<T> Lock<T> { fn lock(&self) -> RefMut<'_, T> { self.0.borrow_mut() } }
struct Flag(Cell<bool>);
impl Flag { fn xchg(&self, v: bool, _: Ordering) -> bool { self.0.replace(v) } }
struct Control { busy: bool, wake_ok: bool }
impl Control {
    fn requests_in_flight(&self) -> bool { self.busy }
    fn wake_reset(&mut self) { event(1); }
    fn wake_complete(&self) -> bool { self.wake_ok }
}
enum CondVarTimeoutResult { Woken { jiffies: u64 }, Signal { jiffies: u64 }, Timeout }
struct Wq;
impl Wq {
    fn wait_interruptible_timeout(&self, _: &mut RefMut<'_, Control>, _: u64) -> CondVarTimeoutResult {
        CondVarTimeoutResult::Timeout
    }
}
struct Mbox;
impl Mbox {
    fn as_raw(&self) -> *mut kernel::bindings::apple_mbox { unsafe { mbox_pointer().cast() } }
    fn set_recv_enabled(&self, enabled: bool) { assert!(enabled); event(3); }
    fn hold_rx(&self) { event(5); }
    fn release_rx(&self) { event(2); }
}
const PHASE_READY: u32 = 1;
struct SepData {
    dev: kernel::device::Device, phase: AtomicU32, control: Lock<Control>, mbox: Lock<Option<Mbox>>,
    gate: Cell<pm::SleepGate>, sleep_rx_held: Flag, endpoint: Cell<bool>, control_wq: Wq,
}
impl SepData {
    fn new() -> Self {
        Self { dev: kernel::device::Device, phase: AtomicU32::new(PHASE_READY),
            control: Lock(RefCell::new(Control { busy: false, wake_ok: true })),
            mbox: Lock(RefCell::new(Some(Mbox))), gate: Cell::new(pm::SleepGate::Open),
            sleep_rx_held: Flag(Cell::new(false)), endpoint: Cell::new(true), control_wq: Wq }
    }
    fn endpoint_present(&self, _: u32) -> bool { self.endpoint.get() }
    fn sleep_gate(&self) -> pm::SleepGate { self.gate.get() }
    fn set_sleep_gate(&self, g: pm::SleepGate) { self.gate.set(g); }
    fn ring(_: &Mbox, msg: kernel::soc::apple::mailbox::Message) -> Result<()> {
        assert_eq!(msg.msg0, proto::WAKE_KICK.msg0); event(4); RING.with(|v|v.get())
    }
}
'''

RUST_TESTS = r'''
fn reset() {
    unsafe { fixture() }; REAL.with(|v|v.set(false));
    EVENTS.with(|v|v.borrow_mut().clear()); RING.with(|v|v.set(Ok(())));
}
fn param(n: i32, v: i32) { unsafe { parameter(n, v) } }
fn count(n: i32) -> i32 { unsafe { counter(n) } }
fn events() -> Vec<u8> { EVENTS.with(|v|v.borrow().clone()) }
fn clear_events() { EVENTS.with(|v|v.borrow_mut().clear()); }
fn real_wake() { REAL.with(|v|v.set(true)); param(12,1); }
#[test]
fn staging_errors_preserve_errno_and_cleanup() {
    for (p,v,e,puts,unmaps) in [
        (4,0,ENOMEM,1,0),(3,0,ENODEV,0,0),(5,-2,ENOENT,0,0),
        (9,1,EBUSY,0,0),(11,0,ENODEV,1,0),(16,-5,EIO,0,0),
        (17,-22,EINVAL,0,0),(18,3,EINVAL,0,0),(19,1,EBUSY,1,1),
        (7,0,EOPNOTSUPP,1,1),
    ] {
        reset(); param(p,v); let s=SepData::new();
        assert_eq!(s.sleep_enter(),Err(e)); assert_eq!(s.gate.get(),pm::SleepGate::Open);
        assert!(!s.sleep_rx_held.0.get()); assert_eq!(events(),Vec::<u8>::new());
        assert_eq!(count(0),puts); assert_eq!(count(2),unmaps); assert_eq!(count(6),0);
    }
}
#[test]
fn unsupported_is_refused_without_early_sleep() {
    for p in [0,1,2,13,14,15] {
        reset(); param(p,if p < 3 {0} else {1}); let s=SepData::new();
        assert_eq!(s.sleep_enter(),Err(EOPNOTSUPP));
        assert_eq!(s.gate.get(),pm::SleepGate::Open); assert!(!s.sleep_rx_held.0.get());
        assert_eq!(count(6),0); assert_eq!(count(7),0); assert_eq!(count(1),0);
        assert_eq!(count(4),0); assert!(events().is_empty());
    }
}
#[test]
fn staging_holds_rx_and_unsent_unwind_only_releases() {
    reset(); let s=SepData::new(); assert_eq!(s.sleep_enter(),Ok(()));
    assert_eq!(s.gate.get(),pm::SleepGate::Draining); assert!(s.sleep_rx_held.0.get());
    assert_eq!(count(3),1); assert_eq!(count(6),0); assert_eq!(events(),vec![5]);
    s.sleep_exit(); assert_eq!(s.gate.get(),pm::SleepGate::Open);
    assert!(!s.sleep_rx_held.0.get()); assert_eq!(events(),vec![5,2]);
    assert_eq!(count(0),1); assert_eq!(count(2),1); assert_eq!(count(3),0);
    assert_eq!(pm::commit_finish(&s.dev),None);
    s.sleep_exit(); assert_eq!(events(),vec![5,2]);
}
#[test]
fn admission_and_missing_mailbox_refuse_before_staging() {
    for mode in 0..6 {
        reset(); let s=SepData::new();
        match mode {
            0 => s.phase.store(0,Relaxed), 1 => s.endpoint.set(false),
            2 => s.control.lock().busy=true, 3 => *s.mbox.lock()=None,
            4 => s.gate.set(pm::SleepGate::Draining), _ => s.gate.set(pm::SleepGate::Told),
        }
        assert_eq!(s.sleep_enter(),Err(if mode==3 {ENODEV} else {EBUSY}));
        assert_eq!(count(1),0); assert_eq!(count(6),0); assert!(!s.sleep_rx_held.0.get());
        assert!(events().is_empty());
        if mode < 4 { assert_eq!(s.gate.get(),pm::SleepGate::Open); }
    }
}
#[test]
fn only_physical_wake_runs_the_actual_kick_in_arm_order() {
    reset(); let s=SepData::new(); assert_eq!(s.sleep_enter(),Ok(()));
    // Current-attempt sent guard is exercised only in C, with no Rust frames.
    param(20,1); real_wake(); clear_events(); s.sleep_exit();
    assert_eq!(events(),vec![1,2,3,4]); assert_eq!(s.gate.get(),pm::SleepGate::Open);
    assert!(!s.sleep_rx_held.0.get()); assert_eq!(count(3),0);
    reset(); let s=SepData::new(); s.gate.set(pm::SleepGate::Told);
    s.sleep_rx_held.0.set(true); s.sleep_exit();
    assert_eq!(s.gate.get(),pm::SleepGate::Lost); assert!(s.sleep_rx_held.0.get());
    assert!(events().is_empty()); assert_eq!(count(5),0); assert_eq!(count(0),0);
}
#[test]
fn failed_real_wake_stays_lost_and_refuses_new_firmware_sleep() {
    for mode in 0..4 {
        reset(); let s=SepData::new(); s.gate.set(pm::SleepGate::Told);
        s.sleep_rx_held.0.set(true); real_wake();
        match mode {
            0 => param(6,-5), 1 => s.control.lock().wake_ok=false,
            2 => RING.with(|v|v.set(Err(EIO))), _ => *s.mbox.lock()=None,
        }
        s.sleep_exit(); assert_eq!(s.gate.get(),pm::SleepGate::Lost);
        assert!(s.sleep_rx_held.0.get());
        if mode==0 { assert!(events().is_empty()); } // clear error precedes RX release
        clear_events(); assert_eq!(s.sleep_enter(),Err(EIO)); assert!(events().is_empty());
        assert_eq!(count(6),0);
        REAL.with(|v|v.set(false)); param(12,0); let reads=count(5); let refs=count(0); s.sleep_exit();
        assert_eq!(count(5),reads); assert_eq!(count(0),refs);
        assert_eq!(s.gate.get(),pm::SleepGate::Lost); assert!(s.sleep_rx_held.0.get());
        assert!(events().is_empty());
    }
    reset(); let s=SepData::new(); s.gate.set(pm::SleepGate::Lost);
    s.sleep_rx_held.0.set(true); assert_eq!(s.sleep_enter(),Err(EIO));
    s.sleep_exit(); assert!(events().is_empty()); assert!(s.sleep_rx_held.0.get());
    assert_eq!(count(5),0); assert_eq!(count(0),0);
}
#[test]
fn lost_and_gate_permissions_are_preserved() {
    for gate in [pm::SleepGate::Open,pm::SleepGate::Draining,pm::SleepGate::Told,pm::SleepGate::Lost] {
        assert_eq!(pm::SleepGate::from_u32(gate as u32),gate);
        assert_eq!(gate.may_request(),gate==pm::SleepGate::Open);
        assert_eq!(gate.may_reply(),matches!(gate,pm::SleepGate::Open|pm::SleepGate::Draining));
        assert_eq!(gate.needs_wake(),matches!(gate,pm::SleepGate::Told|pm::SleepGate::Lost));
    }
}
'''


def sources(source, contracts, mutation):
    """Assemble source only. Compilation/execution belongs exclusively to main."""
    pm = (source / 'drivers/soc/apple/pm.rs').read_text()
    sep = (source / 'drivers/soc/apple/sep.rs').read_text()
    shim = (source / 'drivers/soc/apple/pm_shim.c').read_text()
    mailbox = (source / 'drivers/soc/apple/mailbox.c').read_text()
    sleep = (source / 'drivers/soc/apple/sleep.c').read_text()
    iopoll = (contracts / 'include/linux/iopoll.h').read_text()
    names = {
        'shim': ['sep_pm_wait_gated', 'sep_pm_clear_dev_disable', 'sep_pm_dart_restore',
                 'sep_pm_commit_rx', 'sep_pm_commit_take_rx', 'sep_pm_commit_stop',
                 'sep_pm_commit_prepare', 'sep_pm_commit_finish', 'sep_pm_suspend_noirq'],
        'mailbox': ['apple_mbox_poll_one_supported', 'apple_mbox_poll_one'],
        'sleep': ['apple_sleep_commit_stops_coprocs', 'apple_sleep_enclave_register',
                  'apple_sleep_enclave_unregister'],
        'pm': ['leave_for_commit', 'commit_finish', 'clear_dev_disable', 'dart_restore'],
        'sep': ['sleep_enter', 'sleep_leave_for_commit', 'sleep_hold_rx', 'sleep_release_rx',
                'sleep_exit', 'sleep_kick'],
    }
    texts = dict(shim=shim, mailbox=mailbox, sleep=sleep, pm=pm, sep=sep)
    bodies = {n: function(texts[k], n) for k, group in names.items() for n in group}
    if mutation:
        name, before, after = MUTATIONS[mutation]
        bodies[name] = replace_once(bodies[name], before, after)
    macros = ['poll_timeout_us', 'read_poll_timeout', 'poll_timeout_us_atomic',
              'read_poll_timeout_atomic', 'readx_poll_timeout_atomic', 'readl_poll_timeout_atomic']
    c = C_PREFIX + '\n' + '\n'.join(macro(iopoll, n) for n in macros)
    c += '\n' + '\n'.join(re.search(r'^#define ' + n + r'\s+.*$', shim, re.M).group()
                            for n in ['SEP_PM_POLL_US', 'SEP_PM_TIMEOUT_US'])
    start = shim.index('static struct sep_pm_commit {')
    c += '\n' + shim[start:shim.index('} sep_pm_commit;', start) + len('} sep_pm_commit;')]
    # Use the actual provider helper, with only removal/capability dependency state
    # injected by send for the post-send negative-poll scenarios.
    c += '\n' + '\n'.join(bodies[n] for group in ['mailbox', 'sleep', 'shim'] for n in names[group])
    c += '\n' + C_CONTROLS
    gate_start = pm.index('#[derive(Clone, Copy, Debug, PartialEq, Eq)]')
    gate_end = pm.index('/// Stages `request`', gate_start)
    rust = RUST_PREFIX + '\nmod pm {\nuse crate::*;\n'
    rust += '''extern "C" {
        fn sep_pm_commit_prepare(dev:*mut c_void, mbox:*mut c_void, a:u64, b:u32, c:u64, d:u64)->c_int;
        fn sep_pm_commit_finish(dev:*mut c_void)->c_int;
        fn sep_pm_clear_dev_disable(dev:*mut c_void)->c_int;
        fn sep_pm_dart_restore(dev:*mut c_void);
    }
'''
    rust += pm[gate_start:gate_end]
    rust += '\n'.join(bodies[n] for n in names['pm']) + '\n}\nimpl SepData {\n'
    rust += '\n'.join(bodies[n] for n in names['sep']) + '\n}\n' + RUST_TESTS
    return c, rust


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--contracts', type=Path)
    parser.add_argument('--mutation', choices=MUTATIONS)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    args.out.mkdir(parents=True, exist_ok=True)
    c, rust = sources(args.source, args.contracts or args.source, args.mutation)
    cpath, rpath = args.out / 'control.c', args.out / 'control.rs'
    obj, cbinary, binary = args.out / 'control.o', args.out / 'control-c', args.out / 'control'
    cpath.write_text(c)
    rpath.write_text(rust)
    cc = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-parameter']
    # Finish all compilation before executing controls: setup failures cannot kill mutants.
    subprocess.run(cc + ['-c', str(cpath), '-o', str(obj)], check=True)
    subprocess.run(cc + ['-DCONTROL_MAIN', str(cpath), '-o', str(cbinary)], check=True)
    subprocess.run(['rustc', '--edition=2021', '--test', str(rpath), '-C',
                    f'link-arg={obj.resolve()}', '-o', str(binary)], check=True)
    for command in [[str(cbinary.resolve())], [str(binary.resolve()), '--test-threads=1']]:
        result = subprocess.run(command, check=False)
        if result.returncode:
            # Keep the assertion output, without a Python traceback masking a real kill.
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
