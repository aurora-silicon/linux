#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Feed RTKit's crashlog parser a container log and malformed logs.

drivers/soc/apple/rtkit-crashlog.c is compiled whole and unchanged, with ASan
and UBSan, against a stand-in for rtkit-internal.h that carries the real
header's crashlog definitions. Every log is allocated at its exact size, so a
read past it is a sanitizer error, and a walk that stops advancing runs into
the line limit.

The container log has the layout of a T6050 DCP crash (a 'CLGH' container,
a nested one, and at 0x2000 the 'CLHE' log of task RTAL with eleven sections
and a footer), with neutral contents. The malformed logs cover a zero and an
oversized section size, a truncated section header, a short mailbox section,
a short and a truncated 'Crl4' section, unterminated strings, short buffers
and broken containers. --log FILE feeds a raw crashlog buffer read from a
device instead, and prints what the parser makes of it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = 'drivers/soc/apple/rtkit-crashlog.c'
HEADER = 'drivers/soc/apple/rtkit-internal.h'

# Each mutation removes one bound; a test case must catch it.
MUTATIONS = {
    'buffer-header': ('if (size < sizeof(header)) {', 'if (0) {'),
    'section-header': ('if (size - offset < 16) {', 'if (0) {'),
    'section-size': ('if (section_size < 16 || section_size > size - offset) {', 'if (0) {'),
    'footer-room': ('if (section_size > size - offset - 16)\n\t\t\tbreak;', ''),
    'mailbox-minimum': ('if (size < 28) {', 'if (0) {'),
    'version-string': ('"RTKit: Version: %.*s", (int)(size - 16), bfr + 16', '"RTKit: Version: %s", bfr + 16'),
    'message-string': ('%.*s", idx,\n\t\t\t\t (int)(end - ptr), ptr', '%s", idx,\n\t\t\t\t ptr'),
}

STUB = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* As linux/build_bug.h has it: the message is optional. */
#undef static_assert
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)
#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
#define __packed __attribute__((packed))
#define BIT(n) (1UL << (n))
#define EINVAL 22
#define EAGAIN 11
struct device { int id; };
struct apple_rtkit { struct device *dev; };
void dev_printk(const char *level, struct device *dev, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
#define dev_warn(dev, ...) dev_printk("warn", dev, __VA_ARGS__)
#define dev_dbg(dev, ...) dev_printk("dbg", dev, __VA_ARGS__)
@DEFINITIONS@
int apple_rtkit_crashlog_locate(const u8 *bfr, size_t size, size_t *log_offset,
				u32 *log_id, unsigned int *depth);
void apple_rtkit_crashlog_dump(struct apple_rtkit *rtk, u8 *bfr, size_t size);
'''

UNALIGNED = r'''
#include <stdint.h>
#include <string.h>
static inline uint32_t get_unaligned_le32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline unsigned long long get_unaligned_le64(const void *p)
{ unsigned long long v; memcpy(&v, p, 8); return v; }
'''

MAIN = r'''
#include "rtkit-internal.h"

#define FOURCC(s) (((u32)(s)[0] << 24) | ((u32)(s)[1] << 16) | ((u32)(s)[2] << 8) | (u32)(s)[3])
#define MAX_LINES 1000

static char lines[MAX_LINES][512];
static unsigned int n_lines;
static struct device device;
static struct apple_rtkit rtk = { .dev = &device };

void dev_printk(const char *level, struct device *dev, const char *fmt, ...)
{
	char *line = lines[n_lines];
	va_list ap;
	size_t n;

	assert(dev == &device);
	/* A walk that does not advance prints without end. */
	assert(n_lines < MAX_LINES - 1);
	n_lines++;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(lines[0]), fmt, ap);
	va_end(ap);
	n = strlen(line);
	while (n && line[n - 1] == '\n')
		line[--n] = 0;
	printf("  %s: %s\n", level, line);
}

static void put32(u8 *b, size_t off, u32 v) { memcpy(b + off, &v, 4); }
static void put64(u8 *b, size_t off, u64 v) { memcpy(b + off, &v, 8); }

/* A section header: its type, a fill word, flags and its size, header included. */
static size_t section(u8 *b, size_t off, const char *type, u32 flags, u32 size)
{
	put32(b, off, FOURCC(type));
	put32(b, off + 4, 0xefefefef);
	put32(b, off + 8, flags);
	put32(b, off + 12, size);
	return off + size;
}

/* A 'CLHE' header (or footer) of a log of `size` bytes at `off`. */
static void clhe(u8 *b, size_t off, u32 size, u32 flags)
{
	put32(b, off, FOURCC("CLHE"));
	put32(b, off + 4, 0x303);
	put32(b, off + 8, size);
	put32(b, off + 12, flags);
}

/* A 'CLGH' container at `off` with `count` (id, offset) records. */
static void clgh(u8 *b, size_t off, u32 size, const char *current, unsigned int count,
		 const char *const *ids, const u32 *offsets)
{
	put32(b, off, FOURCC("CLGH"));
	put32(b, off + 4, 0x202);
	put32(b, off + 8, size);
	put32(b, off + 12, FOURCC(current));
	put32(b, off + 16, count);
	for (unsigned int i = 0; i < count; i++) {
		put64(b, off + 0x14 + i * 0x18, FOURCC(ids[i]));
		put32(b, off + 0x14 + i * 0x18 + 16, offsets[i]);
	}
}

/* Runs the dump on an exact-size copy of `log` and returns the number of lines. */
static unsigned int dump(const char *name, const u8 *log, size_t size)
{
	u8 *copy = malloc(size ? size : 1);

	assert(copy);
	memcpy(copy, log, size);
	n_lines = 0;
	printf("%s (%zu bytes)\n", name, size);
	apple_rtkit_crashlog_dump(&rtk, copy, size);
	free(copy);
	return n_lines;
}

static bool printed(const char *text)
{
	for (unsigned int i = 0; i < n_lines; i++)
		if (strstr(lines[i], text))
			return true;
	return false;
}

static bool ended(void)
{
	return n_lines && !strcmp(lines[n_lines - 1], "RTKit: End of crashlog reached");
}

#define DCP_SIZE 0x17000
#define DCP_LOG 0x2000
#define DCP_LOG_SIZE 0x26e0
#define MAILBOX_ENTRIES 128

/* The layout of a T6050 DCP crashlog: the RTAL task's log, two containers deep. */
static void dcp_log(u8 *b)
{
	static const char *const top[] = { "CL4U", "CL4K" }, *const tasks[] = { "root", "RTAL" };
	static const u32 top_offsets[] = { 0x44, 0x12000 }, task_offsets[] = { 0x44, 0x1fbc };
	static const u32 pattern[8] = { 0x202, 0x242, 0x302, 0x342, 0x302, 0x342, 0x202, 0x242 };
	size_t off = DCP_LOG + 0x20, body;

	memset(b, 0, DCP_SIZE);
	clgh(b, 0, DCP_SIZE, "CL4U", 2, top, top_offsets);
	clgh(b, 0x44, 0x11fbc, "RTAL", 2, tasks, task_offsets);
	clhe(b, DCP_LOG, DCP_LOG_SIZE, 0x820);
	put32(b, DCP_LOG + 0x1c, FOURCC("RTAL"));

	body = off + 16;
	off = section(b, off, "Cstr", 0x101, 0x60);
	put32(b, body, 1);
	strcpy((char *)b + body + 4, "crash message [0x87aa9a0]");
	off = section(b, off, "Cver", 0x300, 0xa0);
	body = off + 16;
	off = section(b, off, "Ctim", 0x100, 0x20);
	put64(b, body, 87481704354ULL);
	off = section(b, off, "Ccst", 0x400, 0x70);
	off = section(b, off, "Crtk", 0x300, 0x17c0);
	body = off + 16;
	off = section(b, off, "Cmbx", 0x300, 0xc30);
	for (unsigned int i = 0; i < MAILBOX_ENTRIES; i++) {
		size_t e = body + 28 + i * 0x18;

		put64(b, e, 0x37);
		put64(b, e + 8, pattern[i % 8]);
		put32(b, e + 16, 0xda8656a9 + i * 0x1000);
	}
	off = section(b, off, "Cpct", 0x200, 0x50);
	off = section(b, off, "Csta", 0x100, 0x30);
	off = section(b, off, "Cced", 0x100, 0x50);
	off = section(b, off, "Chex", 0x100, 0x20);
	off = section(b, off, "Csta", 0x100, 0x30);
	assert(off == DCP_LOG + DCP_LOG_SIZE - 0x20);
	clhe(b, off, DCP_LOG_SIZE, 0x820);
}

static void test_dcp_log(void)
{
	static const char *const expect[] = {
		"RTKit: Crashlog container: log 'RTAL' at 0x2000 (depth 2)",
		"RTKit: Message (id=1): crash message [0x87aa9a0]",
		"RTKit: Version: ",
		"RTKit: Crash time: 87481704354",
		"RTKit: Unknown crashlog section: 43637374",
		"RTKit: Unknown crashlog section: 4372746b",
		"RTKit: Mailbox history (type = 0, index = 0)",
		"RTKit: Unknown crashlog section: 43706374",
		"RTKit: Unknown crashlog section: 43737461",
		"RTKit: Unknown crashlog section: 43636564",
		"RTKit: Unknown crashlog section: 43686578",
		"RTKit: Unknown crashlog section: 43737461",
		"RTKit: End of crashlog reached",
	};
	static u8 b[DCP_SIZE];
	unsigned int n, i, j = 0, entries = 0, dumped = 0;
	size_t log_offset;
	unsigned int depth;
	u32 id;

	dcp_log(b);
	assert(!apple_rtkit_crashlog_locate(b, DCP_SIZE, &log_offset, &id, &depth));
	assert(log_offset == DCP_LOG && id == FOURCC("RTAL") && depth == 2);
	n = dump("T6050 DCP container log", b, DCP_SIZE);
	for (i = 0; i < n; i++) {
		if (!strncmp(lines[i], "RTKit:  #", 9)) {
			entries++;
			continue;
		}
		/* The payload of a section without a decoder. */
		if (!strncmp(lines[i], "RTKit:  + ", 10)) {
			dumped++;
			continue;
		}
		assert(j < sizeof(expect) / sizeof(expect[0]));
		assert(!strcmp(lines[i], expect[j]));
		j++;
	}
	assert(j == sizeof(expect) / sizeof(expect[0]));
	assert(entries == MAILBOX_ENTRIES);
	/* Seven such sections: a name line each, and 32 bytes to a line, 256 at most. */
	assert(dumped == 7 + 3 + 8 + 2 + 1 + 2 + 1 + 1);
	assert(printed("RTKit:  + 'Ccst', 96 bytes") && printed("RTKit:  + 'Crtk', 6064 bytes"));
	assert(printed("RTKit:  #000@da8656a9: 0000000000000037 0000000000000202"));
	assert(printed("RTKit:  #127@da8e46a9: 0000000000000037 0000000000000242"));

	/* Still being written: not a complete log. */
	put32(b, DCP_LOG + 12, 0x820 | 0x100);
	assert(apple_rtkit_crashlog_locate(b, DCP_SIZE, &log_offset, &id, &depth) == -EAGAIN);
	dump("log still being written", b, DCP_SIZE);
	assert(printed("holds no complete log (-11)"));
	put32(b, DCP_LOG + 12, 0x820);

	/* A record past the buffer, and a container that names itself. */
	put32(b, 0x44 + 0x14 + 0x18 + 16, DCP_SIZE);
	assert(apple_rtkit_crashlog_locate(b, DCP_SIZE, &log_offset, &id, &depth) == -EINVAL);
	dump("record past the buffer", b, DCP_SIZE);
	assert(printed("holds no complete log (-22)"));
	put32(b, 0x44 + 0x14 + 0x18 + 16, 0);
	assert(apple_rtkit_crashlog_locate(b, DCP_SIZE, &log_offset, &id, &depth) == -EINVAL);
	/* The log's section walk is bounded by the log, not the buffer. */
	dcp_log(b);
	put32(b, DCP_LOG + 8, 0x20 + 0x60);
	dump("log cut after its first section", b, DCP_SIZE);
	assert(printed("End of crashlog reached but no footer present") && !printed("Message"));
}

/* A bare log: a header, then sections built by the caller, then optionally a footer. */
static u8 logbuf[0x1000];

static size_t bare(void)
{
	memset(logbuf, 0, sizeof(logbuf));
	return 0x20;
}

static unsigned int finish(const char *name, size_t off, bool footer)
{
	size_t size = off + (footer ? 0x20 : 0);

	if (footer)
		clhe(logbuf, off, size, 0);
	clhe(logbuf, 0, size, 0);
	return dump(name, logbuf, size);
}

static void test_malformed(void)
{
	size_t off, body;
	unsigned int i;

	off = section(logbuf, bare(), "Ctim", 0x100, 0x20);
	section(logbuf, off, "Csta", 0x100, 0);
	finish("zero section size", off + 0x30, true);
	assert(printed("section 43737461 at 0x40 has a bad size 0x0") && n_lines == 2);

	off = section(logbuf, bare(), "Ctim", 0x100, 0x20);
	section(logbuf, off, "Cmbx", 0x300, 0x10000);
	finish("oversized section", off + 0x40, true);
	assert(printed("section 436d6278 at 0x40 has a bad size 0x10000"));

	off = bare();
	put32(logbuf, off, FOURCC("Ctim"));
	finish("truncated section header", off + 8, false);
	assert(printed("section header at 0x20 is truncated") && n_lines == 1);

	off = section(logbuf, bare(), "Ctim", 0x100, 0x20);
	finish("section, then the footer", off, true);
	assert(printed("Crash time: 0") && ended() && n_lines == 2);

	/* Without a footer the last section may not be decoded: its decoder reads 16 bytes on. */
	body = bare();
	off = section(logbuf, body, "Cstr", 0x101, 0x40);
	put32(logbuf, body + 16, 1);
	memset(logbuf + body + 20, 'm', 0x40 - 20);
	finish("section reaching the end of a log without a footer", off, false);
	assert(printed("End of crashlog reached but no footer present") && n_lines == 1);

	for (u32 size = 16; size < 28; size++) {
		off = section(logbuf, bare(), "Cmbx", 0x300, size);
		finish("short mailbox section", off, true);
		assert(printed("Mailbox section too small") && ended());
	}
	off = section(logbuf, bare(), "Cmbx", 0x300, 28 + 2 * 0x18);
	finish("two-entry mailbox section", off, true);
	assert(printed("RTKit:  #001@") && !printed("#002") && ended());

	body = bare();
	off = section(logbuf, body, "Crl4", 0x100, 0x378);
	put32(logbuf, body + 0x14, 0xd8);
	put64(logbuf, body + 0x110, 0xfffffe0007001234ULL);
	put64(logbuf, body + 0x360, 0xdead0000ULL);
	for (i = 0; i < 28; i++)
		put64(logbuf, body + 0x18 + i * 8, 0x1000 + i);
	finish("whole Crl4 section", off, true);
	assert(printed("Exception dump (Crl4):") && printed("PC     = 0xfffffe0007001234") &&
	       printed("FAR    = 0xdead0000") &&
	       printed("x24-x27 = 0000000000001018 0000000000001019 000000000000101a 000000000000101b") &&
	       ended());

	off = section(logbuf, bare(), "Crl4", 0x100, 0x378);
	finish("truncated Crl4 section", 0x20 + 0x100, true);
	assert(printed("section 43726c34 at 0x20 has a bad size 0x378"));

	off = section(logbuf, bare(), "Crl4", 0x100, 0x40);
	finish("short Crl4 section", off, true);
	assert(printed("Crl4 section too small: 0x40") && ended());

	body = bare();
	off = section(logbuf, body, "Crl4", 0x100, 0x378);
	put32(logbuf, body + 0x10, 1 << 16);
	put32(logbuf, body + 0x14, 0xd8);
	finish("undecodable Crl4 frame", off, true);
	assert(printed("Crl4 frame not decodable") && ended());

	/* Strings without a terminator, against the end of the log. */
	body = bare();
	off = section(logbuf, body, "Cver", 0x300, 0x40);
	memset(logbuf + body + 32, 'v', 0x40 - 32);
	put32(logbuf, off, FOURCC("CLHE"));
	memset(logbuf + off + 4, 0x55, 12);
	clhe(logbuf, 0, off + 16, 0);
	dump("unterminated version", logbuf, off + 16);
	assert(!strcmp(lines[0], "RTKit: Version: vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvEHLCUUUUUUUUUUUU"));

	body = bare();
	off = section(logbuf, body, "Cstr", 0x101, 0x40);
	put32(logbuf, body + 16, 1);
	memset(logbuf + body + 20, 'm', 0x40 - 20);
	put32(logbuf, off, FOURCC("CLHE"));
	memset(logbuf + off + 4, 0x55, 12);
	clhe(logbuf, 0, off + 16, 0);
	dump("unterminated message", logbuf, off + 16);
	assert(!strcmp(lines[0], "RTKit: Message (id=1): mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmEHLCUUUUUUUUUUUU"));

	body = bare();
	off = section(logbuf, body, "Cstr", 0x101, 0x40);
	put32(logbuf, body + 16, 2);
	strcpy((char *)logbuf + body + 20, "first\nsecond\n");
	finish("message lines", off, true);
	assert(printed("Message (id=2): first") && printed("Message (id=2): second") && ended());

	/* Buffers too short for a header, and logs larger than their buffer. */
	for (size_t size = 0; size < 0x20; size += 4) {
		memset(logbuf, 0, sizeof(logbuf));
		clhe(logbuf, 0, 0x20, 0);
		dump("short buffer", logbuf, size);
		assert(printed("Crashlog buffer too small"));
	}
	clhe(logbuf, 0, 0x40, 0);
	dump("log larger than its buffer", logbuf, 0x20);
	assert(printed("Crashlog size (40) is too large"));
	memset(logbuf, 0, sizeof(logbuf));
	put32(logbuf, 0, FOURCC("CLGH"));
	dump("container header only", logbuf, 0x20);
	assert(printed("holds no complete log (-22)"));
}

static int test_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t size;
	u8 *b;

	assert(f);
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	b = malloc(size);
	assert(b && fread(b, 1, size, f) == size);
	fclose(f);
	dump(path, b, size);
	free(b);
	return 0;
}

int main(int argc, char **argv)
{
	assert(FOURCC("CLHE") == APPLE_RTKIT_CRASHLOG_HEADER_FOURCC);
	assert(FOURCC("CLGH") == APPLE_RTKIT_CRASHLOG_CONTAINER_FOURCC);
	if (argc > 1)
		return test_file(argv[1]);
	test_dcp_log();
	test_malformed();
	printf("PASS rtkit crashlog: container log decoded; malformed logs refused without a loop or an over-read\n");
	return 0;
}
'''


def definitions(header):
    """The crashlog fourccs and header struct of rtkit-internal.h."""
    found = re.findall(r'^#define APPLE_RTKIT_CRASHLOG_\w+_FOURCC .*$', header, re.M)
    found += re.findall(r'^struct apple_rtkit_crashlog_header \{.*?^\};\n(?:\nstatic_assert\(.*?\);)?',
                        header, re.M | re.S)
    assert len(found) == 3, found
    return '\n'.join(found)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ref', help='Git revision of the parser to exercise')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    parser.add_argument('--log', type=Path, help='a raw crashlog buffer to dump')
    args = parser.parse_args()

    def read(path):
        if args.ref:
            return subprocess.check_output(['git', 'show', f'{args.ref}:{path}'], cwd=ROOT, text=True)
        return (ROOT / path).read_text()
    source, header = read(SOURCE), read(HEADER)
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        # A mutation that no longer matches the parser would test nothing.
        assert source.count(old) == 1, 'stale mutation: ' + args.mutation
        source = source.replace(old, new)

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        (out / 'linux').mkdir(exist_ok=True)
        (out / 'rtkit-crashlog.c').write_text(source)
        (out / 'rtkit-internal.h').write_text(STUB.replace('@DEFINITIONS@', definitions(header)))
        (out / 'linux/unaligned.h').write_text(UNALIGNED)
        (out / 'main.c').write_text(MAIN)
        exe = out / 'crashlog-tests'
        compiler = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                   '-I', str(out), str(out / 'rtkit-crashlog.c'), str(out / 'main.c'),
                                   '-o', str(exe)], capture_output=True, text=True)
        (out / 'compile.log').write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode:
            print(compiler.stdout + compiler.stderr)
            compiler.check_returncode()
        env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:detect_leaks=0', UBSAN_OPTIONS='print_stacktrace=1')
        result = subprocess.run([str(exe)] + ([str(args.log.resolve())] if args.log else []),
                                capture_output=True, text=True, env=env)
        receipt = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'ref': args.ref,
                   'mutation': args.mutation, 'log': str(args.log) if args.log else None,
                   'exit': result.returncode}
        (out / 'results.json').write_text(json.dumps(dict(receipt, stdout=result.stdout,
                                                          stderr=result.stderr), indent=2) + '\n')
        print(result.stdout + result.stderr)
        print(json.dumps(receipt))
        return result.returncode != 0

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='rtkit-crashlog-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
