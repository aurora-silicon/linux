#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check which co-processors' endpoint 7 messages RTKit serves.

apple_rtkit_rx_dispatch() and the CoreAnalytics handlers are taken from
drivers/soc/apple/rtkit.c unchanged and built with ASan and UBSan. The
endpoint map starts endpoint 7 only for a client that asks for CoreAnalytics
(APPLE_RTKIT_NEEDS_COREANALYTICS) and for registry-v2 PMP firmware, so only
those get a buffer or an acknowledgment there: a buffer request from any
other co-processor, of any size, allocates nothing and is not answered, and
is reported through the rate-limited unknown-endpoint warning.

--old-ref REV runs the same checks on the driver at REV and requires them to
fail there.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/soc/apple/rtkit.c'
HEADER = 'include/linux/soc/apple/rtkit.h'
INTERNAL = 'drivers/soc/apple/rtkit-internal.h'


def function(source, name, optional=False):
    match = re.search(r'^(?:static )?(?:void|int|bool|size_t) ' + name + r'\(', source, re.M)
    if not match and optional:
        return ''
    assert match, name
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def constants(source, header, internal):
    out = [line for line in source.splitlines() if re.match(r'#define APPLE_RTKIT_CA_\w+ ', line)]
    out += [f'#define APPLE_RTKIT_{name} {value}'
            for name, value in re.findall(r'^\tAPPLE_RTKIT_(EP_\w+) = (\w+),', source, re.M)]
    out += [line for line in header.splitlines()
            if re.match(r'#define APPLE_RTKIT_NEEDS_COREANALYTICS\b', line)]
    out += [line for line in internal.splitlines()
            if re.match(r'#define APPLE_RTKIT_APP_ENDPOINT_START\b', line)]
    for name in ('NEEDS_COREANALYTICS', 'EP_COREANALYTICS', 'APP_ENDPOINT_START', 'CA_TYPE'):
        assert any(name in line for line in out), name
    return '\n'.join(out) + '\n'


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define __iomem
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzll(m))
#define FIELD_PREP(m,v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define FIELD_FIT(m,v) (!((((u64)(v)) << __builtin_ctzll(m)) & ~(m)))
#define READ_ONCE(x) (x)
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)warned++)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)

struct apple_rtkit_shmem {
	void *buffer; void *iomem; size_t size; dma_addr_t iova;
	bool is_mapped, needs_dma_sync; void *private;
};
struct apple_rtkit_ops {
	unsigned int flags;
	void (*recv_message)(void *, u8, u64);
	void (*recv_message_timed)(void *, u8, u64, u64);
	int (*shmem_setup)(void *, struct apple_rtkit_shmem *);
	int (*shmem_setup_ep)(void *, u8, struct apple_rtkit_shmem *);
};
struct apple_rtkit {
	void *dev, *cookie;
	const struct apple_rtkit_ops *ops;
	bool shutting_down, sync_syslog_ack, pmp_registry_v2;
	struct { int error; } m5_saved;
	struct apple_rtkit_shmem coreanalytics_buffer;
	int coreanalytics_completion;
};

static unsigned allocations, frees, sent, aux, warned;
static size_t allocated;
static char memory[64];
static int apple_rtkit_alloc_shmem(struct apple_rtkit *rtk, u8 ep, struct apple_rtkit_shmem *b)
{
	(void)rtk; assert(ep == APPLE_RTKIT_EP_COREANALYTICS);
	allocations++; allocated = b->size;
	/* Only the bookkeeping matters here: the memset below gets a stand-in. */
	b->buffer = memory; b->iova = 0x10000;
	return 0;
}
#define memset(p, c, n) ((void)(p), (void)(c), (void)(n))
#define memset_io(p, c, n) ((void)(p), (void)(c), (void)(n))
static void apple_rtkit_free_buffer(struct apple_rtkit *rtk, struct apple_rtkit_shmem *b)
{
	(void)rtk; (void)b; frees++;
}
static int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *c, bool a)
{
	(void)rtk; (void)msg; (void)c; (void)a; assert(ep == APPLE_RTKIT_EP_COREANALYTICS); sent++; return 0;
}
static void complete_all(int *c) { (*c)++; }
#define STUB(n) static void n(struct apple_rtkit *r, u64 m) { (void)r; (void)m; }
STUB(apple_rtkit_management_rx) STUB(apple_rtkit_crashlog_rx) STUB(apple_rtkit_syslog_rx)
STUB(apple_rtkit_ioreport_rx) STUB(apple_rtkit_kdebug_rx) STUB(apple_rtkit_oslog_rx)
static void apple_rtkit_pmp_aux_rx(struct apple_rtkit *r, u64 m) { (void)r; (void)m; aux++; }
'''

MAIN = r'''
static u64 buffer_request(unsigned count, u64 unit)
{
	return FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_BUFFER_REQUEST) |
	       FIELD_PREP(APPLE_RTKIT_CA_COUNT, count) | unit;
}

static void deliver_ep(const struct apple_rtkit_ops *ops, bool registry_v2, u8 ep, u64 msg)
{
	struct apple_rtkit rtk = { .ops = ops, .pmp_registry_v2 = registry_v2 };

	allocations = frees = sent = aux = warned = 0;
	allocated = 0;
	apple_rtkit_rx_dispatch(&rtk, ep, msg, false, 0);
}

static void deliver(const struct apple_rtkit_ops *ops, bool registry_v2, u64 msg)
{
	deliver_ep(ops, registry_v2, APPLE_RTKIT_EP_COREANALYTICS, msg);
}

int main(void)
{
	const struct apple_rtkit_ops plain = { 0 };
	const struct apple_rtkit_ops asks = { .flags = APPLE_RTKIT_NEEDS_COREANALYTICS };
	u64 largest = buffer_request(255, APPLE_RTKIT_CA_UNIT_1M);
	u64 small = buffer_request(4, APPLE_RTKIT_CA_UNIT_4K);
	u64 event = FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_EVENT) |
		    FIELD_PREP(APPLE_RTKIT_CA_EVENT_ID, 9);

	/* A client that asked for CoreAnalytics gets its buffer and its acks. */
	deliver(&asks, false, small);
	assert(allocations == 1 && allocated == 0x4000 && sent == 1 && frees == 0 && !warned);
	deliver(&asks, false, event);
	assert(allocations == 0 && sent == 1 && !warned);

	/* Registry-v2 PMP firmware's endpoint 7 is its auxiliary buffer. */
	deliver(&plain, true, small);
	assert(aux == 1 && allocations == 0 && sent == 0 && !warned);

	/*
	 * Any other co-processor: nothing allocated, nothing answered, and the
	 * message reported through the rate-limited unknown-endpoint warning,
	 * since the firmware decides how often it arrives.
	 */
	deliver(&plain, false, small);
	assert(allocations == 0 && sent == 0 && aux == 0 && warned == 1);
	deliver(&plain, false, largest);
	assert(allocations == 0 && sent == 0 && aux == 0 && warned == 1);
	deliver(&plain, false, event);
	assert(allocations == 0 && sent == 0 && aux == 0 && warned == 1);

	/* A system endpoint nobody serves takes the same warning. */
	deliver_ep(&asks, false, 0x10, small);
	assert(allocations == 0 && sent == 0 && aux == 0 && warned == 1);

	puts("PASS endpoint 7 is served only for the clients the endpoint map starts it for");
	return 0;
}
'''


def read(ref, path):
    if not ref:
        return (ROOT / path).read_text()
    return subprocess.run(['git', '-C', str(ROOT), 'show', f'{ref}:{path}'], check=True,
                          capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='keep build and result files here')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--old-ref', help='require the driver at this revision to fail')
    args = parser.parse_args()
    source = read(args.old_ref, DRIVER)
    code = constants(source, read(args.old_ref, HEADER), read(args.old_ref, INTERNAL)) + PREFIX
    for name in ('apple_rtkit_needs_coreanalytics', 'apple_rtkit_coreanalytics_size',
                 'apple_rtkit_coreanalytics_reply', 'apple_rtkit_coreanalytics_get_buffer',
                 'apple_rtkit_coreanalytics_rx', 'apple_rtkit_rx_dispatch'):
        code += function(source, name)
        if name == 'apple_rtkit_needs_coreanalytics':
            # The one predicate the endpoint map and the dispatcher share; absent before it.
            code += function(source, 'apple_rtkit_ep7_served', optional=True)
            # The one rate-limited warning for unserved endpoints; absent before it.
            code += function(source, 'apple_rtkit_unserved_ep_msg', optional=True)
    code += MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        cfile, exe = out / 'coreanalytics.c', out / 'coreanalytics'
        cfile.write_text(code)
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(cfile), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if args.old_ref:
            if result.returncode == 0:
                raise SystemExit(f'{args.old_ref} passed; the test does not catch the original code')
            print(result.stderr, end='')
            print('old revision fails as required')
            return 0
        print(result.stdout + result.stderr, end='')
        return result.returncode

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='rtkit-coreanalytics-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
