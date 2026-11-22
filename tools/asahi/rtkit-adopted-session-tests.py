#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check how RTKit imports the endpoint map of a session a boot stage handed over."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/soc/apple/rtkit.c'
INTERNAL = 'drivers/soc/apple/rtkit-internal.h'
HEADER = 'include/linux/soc/apple/rtkit.h'

# Each mutation drops one check; an assertion must then fail.
MUTATIONS = {
    'no-management': ('if (!test_bit(APPLE_RTKIT_EP_MGMT, map))', 'if (0)'),
    'any-system': ('if (test_bit(i, map) && !test_bit(i, known))', 'if (0)'),
    'any-version': ('session->protocol_version < APPLE_RTKIT_MIN_SUPPORTED_VERSION ||', '0 ||'),
    'ignore-ops-version': ('session->protocol_version != rtk->ops->protocol_version', '0'),
    'any-buffer-endpoint': ('rtk->ioreport_buffer.size && !test_bit(APPLE_RTKIT_EP_IOREPORT, map)', '0'),
}


def function(source, name):
    match = re.search(r'^static int ' + name + r'\(', source, re.M)
    assert match, name
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DECLARE_BITMAP(n, bits) bool n[bits]
static void set_bit(unsigned int b, bool *m) { m[b] = true; }
static bool test_bit(unsigned int b, const bool *m) { return m[b]; }
static void bitmap_from_arr32(bool *m, const u32 *a, unsigned int bits)
{ for (unsigned int i = 0; i < bits; i++) m[i] = a[i / 32] >> (i % 32) & 1; }
static void bitmap_copy(bool *d, const bool *s, unsigned int bits) { memcpy(d, s, bits); }
struct apple_rtkit_inherited_buffer { int unused; };
struct apple_rtkit_ops { u16 protocol_version; };
'''

TESTS = r'''
struct apple_rtkit_shmem { unsigned long size; };
struct apple_rtkit {
	const struct apple_rtkit_ops *ops;
	bool endpoints[APPLE_RTKIT_MAX_ENDPOINTS];
	u16 version;
	struct apple_rtkit_shmem crashlog_buffer, syslog_buffer, ioreport_buffer, oslog_buffer;
};
''' + '%s' + r'''
static int run(u16 required, u16 version, const u32 *map, struct apple_rtkit *rtk)
{
	static struct apple_rtkit_ops ops;
	struct apple_rtkit_adopted_session s = { .protocol_version = version };
	struct apple_rtkit_shmem crashlog = rtk->crashlog_buffer, syslog = rtk->syslog_buffer;
	struct apple_rtkit_shmem ioreport = rtk->ioreport_buffer, oslog = rtk->oslog_buffer;

	ops.protocol_version = required;
	memset(rtk, 0, sizeof(*rtk));
	rtk->ops = &ops;
	/* The buffers the session retained, set by the caller. */
	rtk->crashlog_buffer = crashlog;
	rtk->syslog_buffer = syslog;
	rtk->ioreport_buffer = ioreport;
	rtk->oslog_buffer = oslog;
	memcpy(s.epmap, map, sizeof(s.epmap));
	return apple_rtkit_inherit_endpoints(rtk, &s);
}

int main(void)
{
	/*
	 * An M2 Mac mini HDMI DCP on the 27.2 firmware, as its boot stage
	 * describes it: system endpoints 0-4 and 8, application endpoints
	 * 0x20-0x2c, 0x31 and 0x37.
	 */
	const u32 dcpext[8] = { 0x0000011f, 0x00821fff };
	struct apple_rtkit r = {};
	u32 m[8];
	int i;

	assert(run(0, 12, dcpext, &r) == 0);
	for (i = 0; i < APPLE_RTKIT_MAX_ENDPOINTS; i++) {
		bool want = i <= 4 || i == 8 || (i >= 0x20 && i <= 0x2c) ||
			    i == 0x31 || i == 0x37;
		assert(r.endpoints[i] == want);
	}
	assert(r.version == 12);

	/* A driver that needs one protocol version takes only that one. */
	assert(run(12, 12, dcpext, &r) == 0);
	assert(run(11, 12, dcpext, &r) == -EINVAL);
	/* Versions RTKit does not speak are refused. */
	assert(run(0, APPLE_RTKIT_MIN_SUPPORTED_VERSION - 1, dcpext, &r) == -EINVAL);
	assert(run(0, APPLE_RTKIT_MAX_SUPPORTED_VERSION + 1, dcpext, &r) == -EINVAL);

	/* Without management there is no session to adopt. */
	memcpy(m, dcpext, sizeof(m));
	m[0] &= ~1u;
	assert(run(0, 12, m, &r) == -EINVAL);
	/* A system endpoint RTKit does not implement is not silently ignored. */
	memcpy(m, dcpext, sizeof(m));
	m[0] |= 1u << 5;
	assert(run(0, 12, m, &r) == -EINVAL);
	/* Tracekit (0xa) is one RTKit knows. */
	memcpy(m, dcpext, sizeof(m));
	m[0] |= 1u << 0xa;
	assert(run(0, 12, m, &r) == 0 && r.endpoints[0xa]);
	/* Every known system endpoint and every application endpoint. */
	for (i = 0; i < 8; i++)
		m[i] = i ? ~0u : 0x59fu;
	assert(run(0, 11, m, &r) == 0 && r.endpoints[0x20] && r.endpoints[0xff]);
	/* Management alone. */
	memset(m, 0, sizeof(m));
	m[0] = 1;
	assert(run(0, 12, m, &r) == 0 && r.endpoints[0] && !r.endpoints[0x37]);

	/*
	 * The J473 retains crashlog, syslog and ioreport buffers: all three
	 * endpoints are in its map. A buffer for an endpoint the map lacks
	 * is refused.
	 */
	memset(&r, 0, sizeof(r));
	r.crashlog_buffer.size = 0x10000;
	r.syslog_buffer.size = 0x4000;
	r.ioreport_buffer.size = 0x4000;
	assert(run(0, 12, dcpext, &r) == 0);
	memcpy(m, dcpext, sizeof(m));
	m[0] &= ~(1u << 4);
	assert(run(0, 12, m, &r) == -EINVAL);
	memset(&r, 0, sizeof(r));
	r.oslog_buffer.size = 0x4000;
	assert(run(0, 12, dcpext, &r) == 0);
	memcpy(m, dcpext, sizeof(m));
	m[0] &= ~(1u << 8);
	assert(run(0, 12, m, &r) == -EINVAL);

	puts("PASS adopted session endpoint map, protocol version and system endpoints");
	return 0;
}
'''


def build_source(mutation):
    driver = (ROOT / DRIVER).read_text()
    internal = (ROOT / INTERNAL).read_text()
    header = (ROOT / HEADER).read_text()
    defines = re.findall(r'^#define APPLE_RTKIT_(?:MIN_SUPPORTED_VERSION|MAX_SUPPORTED_VERSION|'
                         r'APP_ENDPOINT_START|MAX_ENDPOINTS)\b.*$', driver + internal, re.M)
    assert len(defines) == 4, defines
    words = re.search(r'^#define APPLE_RTKIT_ADOPTED_EPMAP_WORDS.*$', header, re.M)
    session = re.search(r'^struct apple_rtkit_adopted_session \{.*?^\};', header, re.M | re.S)
    endpoints = re.search(r'enum \{\s*APPLE_RTKIT_EP_MGMT = 0,.*?\};', driver, re.S)
    assert words and session and endpoints
    imported = function(driver, 'apple_rtkit_inherit_endpoints')
    if mutation:
        old, new = MUTATIONS[mutation]
        assert old in imported, mutation
        imported = imported.replace(old, new)
    # The firmware's map replaces the assumed one before RX can deliver
    # traffic for its endpoints.
    init = re.search(r'^static struct apple_rtkit \*__apple_rtkit_init\(.*?^\}', driver, re.M | re.S)
    assert init
    body = init.group(0)
    assert (body.index('apple_rtkit_mark_running(rtk)') <
            body.index('apple_rtkit_inherit_endpoints(') <
            body.index('apple_rtkit_claim_rx('))
    return (PREFIX + '\n'.join(defines) + '\n' + words.group(0) + '\n' + endpoints.group(0) +
            '\n' + session.group(0) + '\n' + TESTS % imported)


def run(out, cc, mutation):
    source = out / 'rtkit-adopted-session.c'
    binary = out / 'rtkit-adopted-session'
    source.write_text(build_source(mutation))
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(source), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)]).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='keep build and result files here')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--mutation', choices=sorted(MUTATIONS),
                        help='drop one check (the run must fail an assertion)')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        status = run(out, args.cc, args.mutation)
    raise SystemExit(status)


if __name__ == '__main__':
    main()
