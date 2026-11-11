#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production report-validation KUnit vectors on the host.

Only KUnit registration/assertion/allocation and Linux integer/unaligned helpers
are host boundaries. The validation header and all test case bodies are verbatim.
This complements execution with the kernel KUnit runtime; it does not emulate it.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define U32_MAX UINT32_MAX
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK_ULL(h, l) ((~0ULL >> (63 - (h))) & (~0ULL << (l)))
#define FIELD_PREP(mask, val) (((u64)(val) << __builtin_ctzll(mask)) & (mask))
#define GFP_KERNEL 0
static inline unsigned int __ffs64(u64 n) { assert(n); return __builtin_ctzll(n); }
static inline u32 get_unaligned_le32(const void *p)
{
    const u8 *b = p;
    return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}
static inline void put_unaligned_le32(u32 n, void *p)
{
    u8 *b = p;
    for (unsigned int i = 0; i < 4; i++) b[i] = n >> (8 * i);
}
struct kunit { void *allocs[32]; unsigned int count; };
static void *kunit_kzalloc(struct kunit *test, size_t len, int flags)
{
    (void)flags;
    assert(test->count < 32);
    return test->allocs[test->count++] = calloc(1, len);
}
#define KUNIT_EXPECT_TRUE(t, x) do { (void)(t); assert(x); } while (0)
#define KUNIT_EXPECT_FALSE(t, x) KUNIT_EXPECT_TRUE(t, !(x))
#define KUNIT_ASSERT_NOT_NULL(t, x) KUNIT_EXPECT_TRUE(t, (x) != NULL)
#define KUNIT_ASSERT_TRUE(t, x) KUNIT_EXPECT_TRUE(t, x)
#define KUNIT_EXPECT_EQ(t, x, y) KUNIT_EXPECT_TRUE(t, (x) == (y))
struct kunit_case { void (*run)(struct kunit *); const char *name; };
#define KUNIT_CASE(f) {f, #f}
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define kunit_test_suite(suite) \
int main(void) { \
    unsigned int count = 0; \
    for (struct kunit_case *c = suite.test_cases; c->run; c++, count++) { \
        struct kunit test = {0}; \
        c->run(&test); \
        for (unsigned int i = 0; i < test.count; i++) free(test.allocs[i]); \
        printf("%s: PASS\n", c->name); \
    } \
    printf("%s: %u host-executed KUnit vectors PASS\n", suite.name, count); \
    return 0; \
}
#define MODULE_DESCRIPTION(s)
#define MODULE_LICENSE(s)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    path = ROOT / 'drivers/pmdomain/apple'
    validation = (path / 'pmp-report-validation.h').read_text()
    cases = (path / 'pmp-report-test.c').read_text()
    # Drop platform includes, retaining production definitions and KUnit cases.
    source = re.sub(r'^#include .*\n', '', validation + '\n' + cases, flags=re.M)
    generated = args.out / 'pmp-validation.c'
    generated.write_text(SHIM + source)
    executable = args.out / 'pmp-validation'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        str(generated), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
