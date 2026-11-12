#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the PMU RTC callbacks against a byte-addressed W1C regmap model.

C bodies and match data are extracted from the driver. The hardware model has
independent Baku, Abbey, Stowe and Maverick addresses. This checks arithmetic, alarm transactions,
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
    'wrong-control': ('.control = 0x2000', '.control = 0xf800'),
    'maverick-event': ('.event = 0x14c0', '.event = 0x140c'),
    'control-event-clear': ('control &= ~rtc->hw->control_event_bit;',
                            'control |= rtc->hw->control_event_bit;'),
    'stowe-offset': ('static const struct apple_pmu_rtc_hw apple_pmu_rtc_stowe = {',
                     'static const struct apple_pmu_rtc_hw apple_pmu_rtc_stowe = {\n\t.offset_in_seconds = true,'),
    'baku-limit': ('.max_delay = U32_MAX,', '.max_delay = APPLE_PMU_RTC_MAX_DELAY,'),
    'set-no-round': ('(counter + BIT(APPLE_PMU_RTC_HZ_SHIFT - 1))', '(counter)'),
    'set-negative': ('if (offset < 0 || offset', 'if (offset'),
    'restore-unrestored': ('if (pm_hibernate_is_restored())',
                           'if (pm_hibernate_is_restored() || true)'),
    'restore-nothing': ('if (pm_hibernate_is_restored())',
                        'if (pm_hibernate_is_restored() && false)'),
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
typedef uint64_t __le64;
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define RTC_TIMESTAMP_END_9999 253402300799LL
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)
#define cpu_to_le64(x) (x)
#define IS_ERR(x) ((intptr_t)(x) < 0)
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define kfree free
#define guard(x) (void)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define PM_SUSPEND_ON 0
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
struct nvmem_cell { u8 data[8]; size_t bytes; int error, write_error; };
struct regmap {
    u8 bytes[65536];
    unsigned ctrl, counter, compare, mask, event, bit, control_event;
    int ops, fail;
};
static int disabled, pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
static bool firmware_suspend, restored;
static bool pm_suspend_via_firmware(void) { return firmware_suspend; }
static bool pm_hibernate_is_restored(void) { return restored; }
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
static int nvmem_cell_write(struct nvmem_cell *c, void *buf, size_t bytes) {
    if (c->write_error) return c->write_error;
    assert(bytes == c->bytes); memcpy(c->data, buf, bytes); return bytes;
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
    } else if (reg == m->ctrl && m->control_event) {
        /* Maverick's control-side event latch is cleared by writing zero. */
        m->bytes[reg] = (v & ~m->control_event) |
                        (m->bytes[reg] & m->control_event & v);
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
    assert(reg == m->compare && n == 4);
    assert(!(m->bytes[m->ctrl] & 0x40) && (m->bytes[m->mask] & 1));
    memcpy(m->bytes + reg, v, n); return 0;
}
static bool fire_alarm(struct regmap *m) {
    assert(m->bytes[m->ctrl] & 0x40);
    m->bytes[m->ctrl] &= ~0x40;
    if (m->control_event) {
        /* A stale control latch suppresses the next separate-event edge. */
        if (m->bytes[m->ctrl] & m->control_event) return false;
        m->bytes[m->ctrl] |= m->control_event;
    }
    m->bytes[m->event] |= m->bit;
    return true;
}
'''

CASES = r'''
static struct regmap map;
static struct nvmem_cell offset;
static struct rtc_device clock_model;
static struct apple_pmu_rtc rtc;
static struct device dev;
enum { BAKU, ABBEY, STOWE, MAVERICK };
static bool abbey; /* offset in 1/32768 s and monitor bit */
static void set_counter(u64 ticks) { memcpy(map.bytes + map.counter, &ticks, 6); }
static void set_offset(s64 ticks) {
    offset.bytes = abbey ? 6 : 8; memcpy(offset.data, &ticks, offset.bytes);
}
static int variant;
static void reset(int v) {
    static const struct regmap layout[] = {
        [BAKU] = {.ctrl = 0xf800, .counter = 0xf802, .compare = 0xf808, .mask = 0xf80e,
                  .event = 0xf80c, .bit = 1},
        [ABBEY] = {.ctrl = 0x2000, .counter = 0x2002, .compare = 0x2008, .mask = 0x200e,
                   .event = 0x2000, .bit = 2},
        [STOWE] = {.ctrl = 0xf800, .counter = 0xf802, .compare = 0xf808, .mask = 0xf80e,
                   .event = 0xf80c, .bit = 1},
        [MAVERICK] = {.ctrl = 0x1401, .counter = 0x1406, .compare = 0x140c, .mask = 0x14c2,
                      .event = 0x14c0, .bit = 1, .control_event = 2},
    };
    static const struct apple_pmu_rtc_hw *hw[] = {
        [BAKU] = &apple_pmu_rtc_baku, [ABBEY] = &apple_pmu_rtc_abbey,
        [STOWE] = &apple_pmu_rtc_stowe, [MAVERICK] = &apple_pmu_rtc_maverick,
    };
    bool a = v != BAKU;
    variant = v; abbey = a; map = layout[v];
    offset = (struct nvmem_cell){0}; clock_model = (struct rtc_device){0};
    rtc = (struct apple_pmu_rtc){.hw = hw[v],
        .map = &map, .offset = &offset, .clock = &clock_model};
    dev = (struct device){.data = &rtc, .wake = true}; disabled = 0;
    /* Keep an unrelated writable bit, and each variant's monitor bit. */
    map.bytes[map.ctrl] = a ? 0x81 : 0x80;
    if (v == MAVERICK) map.bytes[map.ctrl] = 0x03;
    set_counter(100ULL << 16);
    set_offset(a ? 1000LL * 32768 + 16384 : 1000);
}
static u32 comparator(void) { u32 v; memcpy(&v, map.bytes + map.compare, 4); return v; }
static void run_variant(int v) {
    bool a = v != BAKU;
    struct rtc_time time;
    struct rtc_wkalrm alarm = {.time.seconds = 1130, .enabled = true}, readback;
    s64 ticks;
    reset(v);
    if (v == MAVERICK) {
        /* A stale 0x03 latch suppresses a new edge until control bit 1 clears. */
        assert(map.bytes[map.ctrl] == 0x03 && !(map.bytes[map.event] & map.bit));
        map.bytes[map.ctrl] |= 0x40;
        assert(!fire_alarm(&map));
        assert(map.bytes[map.ctrl] == 0x03 && !(map.bytes[map.event] & map.bit));
    }
    assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == 1100);
    set_counter((100ULL << 16) + 49152);
    assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == (a ? 1101 : 1100));
    assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    assert(comparator() == 130 && (map.bytes[map.ctrl] & 0x40));
    assert(map.bytes[map.ctrl] == (v == MAVERICK ? 0x41 : (a ? 0xc1 : 0xc0)));
    assert(!(map.bytes[map.mask] & 1));
    assert(!apple_pmu_rtc_read_alarm(&dev, &readback));
    assert(readback.time.seconds == 1130 && readback.enabled && !readback.pending);
    /* Acknowledge without changing ARM, monitor or the unrelated bit. */
    assert(fire_alarm(&map));
    if (v == MAVERICK) assert(map.bytes[map.ctrl] == 0x03);
    assert(!apple_pmu_rtc_ack_alarm(&rtc));
    assert(map.bytes[map.ctrl] == (a ? (v == MAVERICK ? 0x03 : 0x81) : 0x80));
    assert(!(map.bytes[map.event] & map.bit));
    assert(!apple_pmu_rtc_arm(&rtc, true));
    /* An ordinary control write must not consume a latched event. */
    map.bytes[map.event] |= map.bit;
    assert(!apple_pmu_rtc_arm(&rtc, false));
    assert(map.bytes[map.event] & map.bit);
    assert(!apple_pmu_rtc_read_alarm(&dev, &readback) && readback.pending);
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    assert(clock_model.events == 1 && dev.wake_events == 1);
    assert(map.bytes[map.ctrl] == (v == MAVERICK ? 0x01 : (a ? 0x81 : 0x80)));
    assert(!(map.bytes[map.event] & map.bit) && (map.bytes[map.mask] & 1));
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_NONE && clock_model.events == 1);
    /* Repeat arm/fire; no stuck status or stale IRQ after the first alarm. */
    assert(!apple_pmu_rtc_alarm_enable(&dev, 1));
    assert(fire_alarm(&map));
    dev.wake = false;
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    assert(clock_model.events == 2 && dev.wake_events == 1);
    assert(!apple_pmu_rtc_alarm_enable(&dev, 0));
    assert(!apple_pmu_rtc_validate_alarm(&rtc, 101));
    assert(apple_pmu_rtc_validate_alarm(&rtc, 100) == -ETIME);
    assert(!apple_pmu_rtc_validate_alarm(&rtc, 3700));
    alarm.time.seconds = 1100;
    assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -ETIME);
    alarm.time.seconds = 4701;
    if (a) {
        /* One hour, where the alarm only ends a suspend-to-idle. */
        assert(apple_pmu_rtc_validate_alarm(&rtc, 3701) == -ERANGE);
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -ERANGE);
    } else {
        /* Baku: as far as the 32-bit comparator reaches. */
        assert(!apple_pmu_rtc_validate_alarm(&rtc, 3701));
        assert(!apple_pmu_rtc_validate_alarm(&rtc, U32_MAX));
        assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && comparator() == 3701);
        alarm.time.seconds = 86400 * 7 + 1000;
        assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && comparator() == 86400 * 7);
        alarm.time.seconds = 1000 + (s64)U32_MAX + 1;
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -ERANGE);
        assert(!apple_pmu_rtc_alarm_enable(&dev, 0));
    }
    /* Reject malformed offsets and propagate NVMEM errors. */
    offset.bytes = a ? 8 : 6;
    assert(apple_pmu_rtc_offset(&rtc, &ticks) == -EINVAL);
    offset.error = -EIO;
    assert(apple_pmu_rtc_read_time(&dev, &time) == -EIO);
    if (!a) {
        /* Baku sets its whole-second offset, rounding the counter to the second. */
        reset(v);
        time.seconds = 5000;
        set_counter((100ULL << 16) + 32767);
        assert(!apple_pmu_rtc_set_time(&dev, &time));
        assert(get_unaligned_le64(offset.data) == 4900);
        assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == 5000);
        time.seconds = 5000;
        set_counter((100ULL << 16) + 32768);
        assert(!apple_pmu_rtc_set_time(&dev, &time));
        assert(get_unaligned_le64(offset.data) == 4899);
        /* Alarms follow the new offset. */
        alarm.time.seconds = 5030;
        assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && comparator() == 131);
        assert(!apple_pmu_rtc_alarm_enable(&dev, 0));
        /* Before the counter's own time, and past what the offset may hold. */
        time.seconds = 99;
        assert(apple_pmu_rtc_set_time(&dev, &time) == -ERANGE);
        time.seconds = RTC_TIMESTAMP_END_9999;
        assert(apple_pmu_rtc_set_time(&dev, &time) == -ERANGE);
        assert(get_unaligned_le64(offset.data) == 4899);
        time.seconds = 5000;
        offset.write_error = -EIO;
        assert(apple_pmu_rtc_set_time(&dev, &time) == -EIO);
        offset.write_error = 0;
        map.ops = 0; map.fail = 1;
        assert(apple_pmu_rtc_set_time(&dev, &time) == -EIO);
        assert(get_unaligned_le64(offset.data) == 4899);
    }
    reset(v);
    set_offset(-32768);
    if (a) {
        assert(!apple_pmu_rtc_offset(&rtc, &ticks) && ticks == -65536);
        assert(!apple_pmu_rtc_read_time(&dev, &time) && time.seconds == 99);
        alarm.time.seconds = 129;
        assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && comparator() == 130);
    } else assert(apple_pmu_rtc_offset(&rtc, &ticks) == -ERANGE);
    /* Keep a valid firmware alarm and discard a stale one at probe. */
    reset(v); alarm.time.seconds = 1130;
    assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    map.ops = 0;
    assert(!apple_pmu_rtc_init_alarm(&rtc) && (map.bytes[map.ctrl] & 0x40));
    set_counter(131ULL << 16); assert(fire_alarm(&map));
    assert(!apple_pmu_rtc_init_alarm(&rtc));
    assert(!(map.bytes[map.ctrl] & 0x40) && !(map.bytes[map.event] & map.bit));
    assert(map.bytes[map.mask] & 1);
    /* Every failed transaction is reported; later enables fail after IRQ I/O failure. */
    reset(v); assert(!apple_pmu_rtc_set_alarm(&dev, &alarm));
    int count = map.ops;
    for (int fail = 1; fail <= count; ++fail) {
        reset(v); map.fail = fail;
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -EIO);
    }
    reset(v); map.bytes[map.event] |= map.bit;
    assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
    count = map.ops;
    for (int fail = 1; fail <= count; ++fail) {
        reset(v); map.bytes[map.event] |= map.bit; map.fail = fail;
        assert(apple_pmu_rtc_interrupt(7, &dev) == IRQ_HANDLED);
        assert(rtc.io_failed && disabled == 1 && dev.wake_events == 1);
        assert(clock_model.events == 0);
        assert(apple_pmu_rtc_alarm_enable(&dev, 1) == -EIO);
        assert(apple_pmu_rtc_set_alarm(&dev, &alarm) == -EIO);
        assert(apple_pmu_rtc_prepare(&dev) == -EBUSY);
    }
    reset(v); assert(!apple_pmu_rtc_prepare(&dev));
    pm_suspend_target_state = 2;
    assert(apple_pmu_rtc_prepare(&dev) == -EBUSY);
    /* A firmware-driven suspend programs the wake itself. */
    firmware_suspend = true;
    assert(!apple_pmu_rtc_prepare(&dev));
    firmware_suspend = false;
    /* Hibernation is accepted. A platform power-off stops the alarm, and goes on if that fails. */
    pm_suspend_target_state = PM_SUSPEND_ON;
    assert(!apple_pmu_rtc_prepare(&dev));
    assert(!apple_pmu_rtc_set_alarm(&dev, &alarm) && (map.bytes[map.ctrl] & 0x40));
    assert(!apple_pmu_rtc_poweroff(&dev));
    assert(!(map.bytes[map.ctrl] & 0x40) && (map.bytes[map.mask] & 1));
    map.ops = 0; map.fail = 1;
    assert(!apple_pmu_rtc_poweroff(&dev));
    pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
    /*
     * After a resume from disk the RTC core goes through its alarm again, without
     * a PMU access of the driver's own; a hibernation given up before its image
     * leaves the alarm alone.
     */
    reset(v);
    assert(!apple_pmu_rtc_restore(&dev) && clock_model.events == 0 && map.ops == 0);
    restored = true;
    assert(!apple_pmu_rtc_restore(&dev) && clock_model.events == 1 && map.ops == 0);
    restored = false;
}
int main(void) {
    run_variant(BAKU); run_variant(ABBEY); run_variant(STOWE); run_variant(MAVERICK);
    puts("PMU RTC: Baku, Abbey, Stowe and Maverick arithmetic, W1C transactions, Baku clock setting and fault injection passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=MUTATIONS)
    args = parser.parse_args()
    source = (ROOT / 'drivers/rtc/rtc-apple-pmu.c').read_text()
    code = source[source.index('#define APPLE_PMU_RTC_ARM'):
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
        result = subprocess.run([str(binary)])
        raise SystemExit(1 if result.returncode else 0)


if __name__ == '__main__':
    main()
