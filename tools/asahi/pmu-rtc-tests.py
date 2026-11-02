#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the PMU RTC callbacks against a byte-addressed W1C regmap model.

C bodies and match data are extracted from the driver. The hardware model has
independent Baku/Abbey addresses. This checks arithmetic, alarm transactions,
shared control preservation, stale events and I/O failures, not physical wake.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MUTATIONS = {
    'ack-clobber': ('control | rtc->hw->event_bit', 'rtc->hw->event_bit'),
    'arm-ack': ('control &= ~rtc->hw->event_bit;', 'control |= rtc->hw->event_bit;'),
    'offset-seconds': ('sign_extend64(offset, 47) * 2', '(s64)offset'),
    'offset-sign': ('sign_extend64(offset, 47) * 2', '(s64)offset * 2'),
    'event-bit': ('.event_bit = BIT(1)', '.event_bit = BIT(0)'),
    'wrong-base': ('.base = 0x2000', '.base = 0xf800'),
}

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t __le32;
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define RTC_TIMESTAMP_END_9999 253402300799LL
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)
#define IS_ERR(x) ((intptr_t)(x) < 0)
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define kfree free
#define guard(x) (void)
#define dev_err(...) ((void)0)
#define PM_SUSPEND_TO_IDLE 1
#define RTC_IRQF 0x80
#define RTC_AF 0x20
#define IRQ_HANDLED 1
#define IRQ_NONE 0
typedef int irqreturn_t;
struct mutex { int unused; };
struct rtc_time { s64 seconds; };
struct rtc_wkalrm { struct rtc_time time; bool enabled, pending; };
struct rtc_device { int events; };
struct device { void *data; bool wake; int wake_events; };
struct nvmem_cell { u8 data[8]; size_t bytes; int error; };
struct regmap { u8 bytes[65536]; unsigned base, event, bit; int ops, fail; };
static int disabled, pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
static inline __attribute__((unused)) s64 sign_extend64(u64 v, int bit) { return (s64)(v << (63 - bit)) >> (63 - bit); }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static void mutex_lock(struct mutex *m) { (void)m; }
static void mutex_unlock(struct mutex *m) { (void)m; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static bool device_may_wakeup(struct device *d) { return d->wake; }
static void pm_wakeup_dev_event(struct device *d, int n, bool hard) {
    assert(n == 0 && hard); d->wake_events++;
}
static void disable_irq_nosync(int irq) { assert(irq == 7); disabled++; }
static void rtc_update_irq(struct rtc_device *r, int n, int flags) {
    assert(n == 1 && flags == (RTC_IRQF | RTC_AF)); r->events++;
}
static void rtc_time64_to_tm(s64 t, struct rtc_time *tm) { tm->seconds = t; }
static s64 rtc_tm_to_time64(const struct rtc_time *tm) { return tm->seconds; }
static void *nvmem_cell_read(struct nvmem_cell *c, size_t *bytes) {
    if (c->error) return (void *)(intptr_t)c->error;
    void *p = malloc(c->bytes); assert(p); memcpy(p, c->data, c->bytes);
    *bytes = c->bytes; return p;
}
static bool io_error(struct regmap *m) { return ++m->ops == m->fail; }
static int regmap_read(struct regmap *m, unsigned reg, unsigned *v) {
    if (io_error(m)) return -EIO;
    *v = m->bytes[reg]; return 0;
}
static int regmap_write(struct regmap *m, unsigned reg, unsigned v) {
    if (io_error(m)) return -EIO;
    if (reg == m->event) {
        /* W1C event, normal R/W bits in the same byte. */
        m->bytes[reg] = (v & ~m->bit) | (m->bytes[reg] & m->bit & ~v);
    } else m->bytes[reg] = v;
    return 0;
}
static int regmap_update_bits(struct regmap *m, unsigned reg, unsigned mask, unsigned val) {
    unsigned old; int ret = regmap_read(m, reg, &old);
    if (ret) return ret;
    return regmap_write(m, reg, (old & ~mask) | (val & mask));
}
static int regmap_bulk_read(struct regmap *m, unsigned reg, void *v, size_t n) {
    if (io_error(m)) return -EIO;
    assert(reg + n <= sizeof(m->bytes)); memcpy(v, m->bytes + reg, n); return 0;
}
static int regmap_bulk_write(struct regmap *m, unsigned reg, const void *v, size_t n) {
    if (io_error(m)) return -EIO;
    /* Comparator programming must happen with the alarm masked and disarmed. */
    assert(reg == m->base + 8 && n == 4);
    assert(!(m->bytes[m->base] & 0x40) && (m->bytes[m->base + 14] & 1));
    memcpy(m->bytes + reg, v, n); return 0;
}
'''

CASES = r'''
static struct regmap map;
static struct nvmem_cell offset;
static struct rtc_device clock_model;
static struct apple_pmu_rtc rtc;
static struct device dev;
static bool abbey;
static void set_counter(u64 ticks) { memcpy(map.bytes + map.base + 2, &ticks, 6); }
static void set_offset(s64 ticks) {
    offset.bytes = abbey ? 6 : 8; memcpy(offset.data, &ticks, offset.bytes);
}
static void reset(bool a) {
    abbey = a; map = (struct regmap){.base = a ? 0x2000 : 0xf800,
        .event = a ? 0x2000 : 0xf80c, .bit = a ? 2 : 1};
    offset = (struct nvmem_cell){0}; clock_model = (struct rtc_device){0};
    rtc = (struct apple_pmu_rtc){.hw = a ? &apple_pmu_rtc_abbey : &apple_pmu_rtc_baku,
        .map = &map, .offset = &offset, .clock = &clock_model};
    dev = (struct device){.data = &rtc, .wake = true}; disabled = 0;
    /* Keep an unrelated writable bit, and Abbey's monitor bit. */
    map.bytes[map.base] = a ? 0x81 : 0x80;
    set_counter(100ULL << 16);
    set_offset(a ? 1000LL * 32768 + 16384 : 1000);
}
static u32 comparator(void) { u32 v; memcpy(&v, map.bytes + map.base + 8, 4); return v; }
static void run_variant(bool a) {
    struct rtc_time time;
    struct rtc_wkalrm alarm = {.time.seconds = 1130, .enabled = true}, readback;
    s64 ticks;
    reset(a);
    assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == 1100);
    set_counter((100ULL << 16) + 49152);
    assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == (a ? 1101 : 1100));
    assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    assert(comparator() == 130 && (map.bytes[map.base] & 0x40));
    assert(map.bytes[map.base] == (a ? 0xc1 : 0xc0));
    assert(!(map.bytes[map.base + 14] & 1));
    assert(!apple_pmu_rtc_read_alarm(&dev, &readback));
    assert(readback.time.seconds == 1130 && readback.enabled && !readback.pending);
    /* Acknowledge without changing ARM, monitor or the unrelated bit. */
    map.bytes[map.event] |= map.bit;
    assert(!apple_pmu_rtc_ack_alarm(&rtc));
    assert(map.bytes[map.base] == (a ? 0xc1 : 0xc0));
    assert(!(map.bytes[map.event] & map.bit));
    /* An ordinary control write must not consume a latched event. */
    map.bytes[map.event] |= map.bit;
    assert(!apple_pmu_rtc_arm(&rtc, false));
    assert(map.bytes[map.event] & map.bit);
    assert(!apple_pmu_rtc_read_alarm(&dev, &readback) && readback.pending);
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    assert(clock_model.events == 1 && dev.wake_events == 1);
    assert(map.bytes[map.base] == (a ? 0x81 : 0x80));
    assert(!(map.bytes[map.event] & map.bit) && (map.bytes[map.base + 14] & 1));
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_NONE && clock_model.events == 1);
    /* Repeat arm/fire; no stuck status or stale IRQ after the first alarm. */
    assert(!apple_pmu_rtc_alarm_enable(&dev, 1));
    map.bytes[map.event] |= map.bit;
    dev.wake = false;
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    assert(clock_model.events == 2 && dev.wake_events == 1);
    assert(!apple_pmu_rtc_alarm_enable(&dev, 0));
    assert(!apple_pmu_rtc_validate_alarm(&rtc, 101));
    assert(apple_pmu_rtc_validate_alarm(&rtc, 100) == -ETIME);
    assert(!apple_pmu_rtc_validate_alarm(&rtc, 3700));
    assert(apple_pmu_rtc_validate_alarm(&rtc, 3701) == -ERANGE);
    alarm.time.seconds = 1100;
    assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -ETIME);
    alarm.time.seconds = 4701;
    assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -ERANGE);
    /* Reject malformed offsets and propagate NVMEM errors. */
    offset.bytes = a ? 8 : 6;
    assert(apple_pmu_rtc_offset(&rtc, &ticks) == -EINVAL);
    offset.error = -EIO;
    assert(apple_pmu_rtc_read_time(&dev, &time) == -EIO);
    reset(a);
    set_offset(-32768);
    if (a) {
        assert(!apple_pmu_rtc_offset(&rtc, &ticks) && ticks == -65536);
        assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == 99);
        alarm.time.seconds = 129;
        assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && comparator() == 130);
    } else assert(apple_pmu_rtc_offset(&rtc, &ticks) == -ERANGE);
    /* Keep a valid firmware alarm and discard a stale one at probe. */
    reset(a); alarm.time.seconds = 1130;
    assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    map.ops = 0;
    assert(!apple_pmu_rtc_init_alarm(&rtc) && (map.bytes[map.base] & 0x40));
    set_counter(131ULL << 16); map.bytes[map.event] |= map.bit;
    assert(!apple_pmu_rtc_init_alarm(&rtc));
    assert(!(map.bytes[map.base] & 0x40) && !(map.bytes[map.event] & map.bit));
    assert(map.bytes[map.base + 14] & 1);
    /* Every failed transaction is reported; later enables fail after IRQ I/O failure. */
    reset(a); assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    int count = map.ops;
    for (int fail = 1; fail <= count; ++fail) {
        reset(a); map.fail = fail;
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -EIO);
    }
    reset(a); map.bytes[map.event] |= map.bit;
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    count = map.ops;
    for (int fail = 1; fail <= count; ++fail) {
        reset(a); map.bytes[map.event] |= map.bit; map.fail = fail;
        assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
        assert(rtc.io_failed && disabled == 1 && dev.wake_events == 1);
        assert(clock_model.events == 0);
        assert(apple_pmu_rtc_alarm_enable(&dev, 1) == -EIO);
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -EIO);
        assert(apple_pmu_rtc_prepare(&dev) == -EBUSY);
    }
    reset(a); assert(!apple_pmu_rtc_prepare(&dev));
    pm_suspend_target_state = 2;
    assert(apple_pmu_rtc_prepare(&dev) == -EBUSY);
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
}
int main(void) {
    run_variant(false); run_variant(true);
    puts("PMU RTC: Baku and Abbey arithmetic, W1C transactions and fault injection passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=MUTATIONS)
    args = parser.parse_args()
    source = (ROOT / 'drivers/rtc/rtc-apple-pmu.c').read_text()
    code = source[source.index('#define APPLE_PMU_RTC_CONTROL'):
                  source.index('static const struct rtc_class_ops')]
    code += source[source.index('static int apple_pmu_rtc_prepare('):
                   source.index('static const struct dev_pm_ops')]
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert old in code, f'missing mutation site: {old}'
        code = code.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='pmu-rtc-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        unit = out / 'pmu-rtc.c'
        binary = out / 'pmu-rtc'
        unit.write_text(PRELUDE + code + CASES)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', str(unit), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
