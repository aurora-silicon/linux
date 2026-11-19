#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual router-state transfer against a fault-injected config transport.

This verifies framing, opaque state lifetime and bounded command polling. It
does not qualify firmware commands, SRAM retention or any power transition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef int64_t ktime_t;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ROUTER_CS_9 9
#define ROUTER_CS_26 26
#define ROUTER_CS_26_OV BIT(31)
#define ROUTER_CS_26_ONS BIT(30)
#define ROUTER_CS_26_STATUS_MASK (0x3fU << 24)
#define ROUTER_CS_26_STATUS_SHIFT 24
#define TB_CFG_SWITCH 2
struct tb { int lock; };
struct tb_switch { struct tb *tb; unsigned route; };
#define lockdep_assert_held(p) assert(*(p) == 1)
static unsigned tb_route(struct tb_switch *sw) { return sw->route; }
static ktime_t now;
static ktime_t ktime_get(void) { return now; }
static ktime_t ktime_add_ms(ktime_t t, unsigned ms) { return t + 1000 * ms; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
static void usleep_range(unsigned lo, unsigned hi) { assert(lo == 500 && hi >= lo); now += lo; }
static unsigned calls, fail_at, op, reads, busy_reads;
static bool unsupported, stuck;
static u32 reply_status, submitted[17];
static u32 opaque(unsigned i) { return 0x927f3100U ^ (i * 0x1020304U); }
static int tb_sw_write(struct tb_switch *sw, const void *data, int space,
                       unsigned offset, unsigned count)
{
 assert(sw && space == TB_CFG_SWITCH);
 if (++calls == fail_at) return -EPIPE;
 if (offset == ROUTER_CS_9) {
  assert(count == 17);
  memcpy(submitted, data, sizeof(submitted));
  assert(submitted[16] == 0);
 } else {
  assert(offset == ROUTER_CS_26 && count == 1);
  u32 command = *(const u32 *)data;
  assert(command == 0x80008000 || command == 0x80008001);
  op = command & 0xffff;
  for (unsigned i = 0; i < 16; i++)
   assert(submitted[i] == (op == 0x8000 ? 0 : opaque(i)));
 }
 return 0;
}
static int tb_sw_read(struct tb_switch *sw, void *data, int space,
                      unsigned offset, unsigned count)
{
 assert(sw && space == TB_CFG_SWITCH);
 if (++calls == fail_at) return -EPIPE;
 if (offset == ROUTER_CS_26) {
  assert(count == 1);
  reads++;
  *(u32 *)data = (stuck || reads <= busy_reads) ? ROUTER_CS_26_OV :
                (unsupported ? ROUTER_CS_26_ONS : reply_status << 24);
 } else {
  assert(offset == ROUTER_CS_9 && count == 17 && op);
  for (unsigned i = 0; i < 16; i++)
   ((u32 *)data)[i] = op == 0x8000 ? opaque(i) : 0xbad00000 + i;
  ((u32 *)data)[16] = 0x12345678;
 }
 return 0;
}
'''

FOOTER = r'''
static void reset_io(void)
{
 now = 0; calls = fail_at = op = reads = busy_reads = 0;
 unsupported = stuck = false; reply_status = 0;
 memset(submitted, 0xa5, sizeof(submitted));
}
static void check_saved(struct apple_router_sleep *s)
{
 assert(s->valid);
 for (unsigned i = 0; i < 16; i++) assert(s->data[i] == opaque(i));
}
int main(void)
{
 struct tb tb = {.lock = 1};
 struct tb_switch sw = {.tb = &tb};
 struct apple_router_sleep s = {};
 unsigned cases = 0;
 assert(apple_router_restore_sleep(&sw, &s) == -ENODATA && !calls); cases++;
 busy_reads = 3;
 assert(!apple_router_save_sleep(&sw, &s));
 assert(op == 0x8000 && reads == 4); check_saved(&s); cases++;
 unsigned before = calls;
 assert(apple_router_save_sleep(&sw, &s) == -EBUSY && calls == before); cases++;
 reset_io();
 assert(!apple_router_restore_sleep(&sw, &s) && !s.valid && op == 0x8001);
 for (unsigned i = 0; i < 16; i++) assert(s.data[i] == opaque(i));
 cases++;
 for (unsigned fail = 1; fail <= 4; fail++) {
  reset_io(); fail_at = fail;
  memset(&s, 0, sizeof(s));
  assert(apple_router_save_sleep(&sw, &s) == -EPIPE && !s.valid);
  assert(calls == fail); cases++;
  reset_io(); assert(!apple_router_save_sleep(&sw, &s));
  reset_io(); fail_at = fail;
  assert(apple_router_restore_sleep(&sw, &s) == -EPIPE);
  assert(calls == fail); check_saved(&s); cases++;
 }
 for (unsigned kind = 0; kind < 3; kind++) {
  reset_io(); memset(&s, 0, sizeof(s));
  stuck = kind == 0; unsupported = kind == 1; reply_status = kind == 2;
  int expected = kind == 0 ? -ETIMEDOUT : kind == 1 ? -EOPNOTSUPP : -EIO;
  assert(apple_router_save_sleep(&sw, &s) == expected && !s.valid);
  assert(reads <= 2001 && now <= 1000000); cases++;
  reset_io(); assert(!apple_router_save_sleep(&sw, &s));
  reset_io();
  stuck = kind == 0; unsupported = kind == 1; reply_status = kind == 2;
  assert(apple_router_restore_sleep(&sw, &s) == expected);
  check_saved(&s); assert(reads <= 2001 && now <= 1000000); cases++;
 }
 reset_io(); memset(&s, 0, sizeof(s)); sw.route = 1;
 assert(apple_router_save_sleep(&sw, &s) == -EINVAL && !calls && !s.valid); cases++;
 sw.route = 0;
 for (unsigned status = 2; status < 64; status++) {
  reset_io(); memset(&s, 0, sizeof(s)); reply_status = status;
  assert(!apple_router_save_sleep(&sw, &s)); check_saved(&s);
  assert(!apple_router_restore_sleep(&sw, &s) && !s.valid); cases++;
 }
 reset_io(); memset(&s, 0, sizeof(s));
 for (unsigned cycle = 0; cycle < 3; cycle++) {
  assert(!apple_router_save_sleep(&sw, &s)); check_saved(&s);
  assert(!apple_router_restore_sleep(&sw, &s) && !s.valid); cases++;
 }
 printf("PASS %u router transfer cases\n", cases);
}
'''

MUTATIONS = {
    'missing-validity': ('state->valid = true;', 'state->valid = false;'),
    'consume-failed-restore': ('ret = apple_router_sleep_op(sw, APPLE_ROUTER_RESTORE_SLEEP, data);',
                             'state->valid = false;\n ret = apple_router_sleep_op(sw, APPLE_ROUTER_RESTORE_SLEEP, data);'),
    'stale-save-input': ('u32 data[ARRAY_SIZE(state->data)] = {};',
                        'u32 data[ARRAY_SIZE(state->data)] = {1};'),
    'wrong-restore-op': ('#define APPLE_ROUTER_RESTORE_SLEEP\t0x8001',
                         '#define APPLE_ROUTER_RESTORE_SLEEP\t0x8000'),
    'ignore-unsupported': ('if (command & ROUTER_CS_26_ONS)', 'if (false)'),
    'ignore-status': ('if (status == APPLE_ROUTER_NOT_READABLE)', 'if (status == 64)'),
    'missing-restore-admission': ('if (!state->valid)', 'if (false)'),
    'overwrite-valid-save': ('if (state->valid)', 'if (false)'),
    'short-payload': ('ARRAY_SIZE(words));', 'ARRAY_SIZE(words) - 1);'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = ['drivers/thunderbolt/apple-router-sleep.h',
             'drivers/thunderbolt/apple-router-sleep.c']
    sources = {p: (ROOT / p).read_text() for p in paths}
    production = '\n'.join(re.sub(r'^#include.*\n', '', s, flags=re.M)
                           for s in sources.values())
    variants = {'real': production}
    for name, (old, new) in MUTATIONS.items():
        assert old in production, name
        variants[name] = production.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='apple-router-sleep-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = {}
        for name, source in variants.items():
            unit, binary = out / (name + '.c'), out / name
            unit.write_text(HEADER + source + FOOTER)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-variable', '-g', '-fsanitize=address,undefined',
                            str(unit), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            results[name] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            if name == 'real':
                assert run.returncode == 0, results[name]
                print(run.stdout, end='')
            else:
                assert run.returncode != 0 and 'Assertion' in run.stderr, results[name]
        (out / 'receipt.json').write_text(json.dumps({
            'scope': __doc__, 'source_sha256': {
                p: hashlib.sha256(s.encode()).hexdigest() for p, s in sources.items()},
            'results': results}, indent=2) + '\n')
        print(f'PASS {len(MUTATIONS)} negative controls')


if __name__ == '__main__':
    main()
