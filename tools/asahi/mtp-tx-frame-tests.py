#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Send DockChannel HID frames from two interfaces at once and parse the FIFO.

Runs the production dchid_send() (and dchid_send_frame()) in two threads, one
per interface, as when the actuator enable (0xb4 on comm) and the trackpad's
dimension query (0xd9 on multi-touch) go out together after multi-touch
reports ready. The stub FIFO records every dockchannel_send() write and
yields between writes. Parsing it back with the production
dchid_verify_checksum() must give whole, checksum-valid frames, all of them,
in per-interface order. The old transport must compile and fail this at
runtime, as the J613 MTP firmware did ("[dock ch tl]: xsum err").
"""
import argparse
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TRANSPORT = 'drivers/hid/dockchannel-hid/dockchannel-hid.c'


def function(source, name):
    for kind in ('int', 'void', 'u32', 'bool'):
        prefix = 'static ' + kind + ' ' + name + '('
        if prefix in source:
            start = source.index(prefix)
            return source[start:source.index('\n}', start)+2]
    raise ValueError(name)


def struct(source, name):
    m = re.search(r'struct ' + name + r' \{.*?\} __packed;', source, re.S)
    if not m:
        raise ValueError(name)
    return m.group(0)


PREFIX = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define __packed __attribute__((packed))
#define U16_MAX 0xffff
#define U32_MAX 0xffffffffu
#define round_down(x, y) ((x) & ~((y) - 1))
#define round_up(x, y) ((((x) + (y) - 1) / (y)) * (y))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define WARN_ON_ONCE(x) assert(!(x))
#define DCHID_CHANNEL_CMD 0x11
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
struct mutex { pthread_mutex_t m; };
static void mutex_lock(struct mutex *l) { assert(!pthread_mutex_lock(&l->m)); }
static void mutex_unlock(struct mutex *l) { assert(!pthread_mutex_unlock(&l->m)); }

/* The shared TX FIFO: each dockchannel_send() lands whole, as its words do. */
static u8 fifo[1 << 20];
static size_t fifo_len;
static pthread_mutex_t fifo_lock = PTHREAD_MUTEX_INITIALIZER;
struct dockchannel { int unused; };
static int dockchannel_send(struct dockchannel *dc, const void *buf, size_t count) {
    pthread_mutex_lock(&fifo_lock);
    assert(fifo_len + count <= sizeof(fifo));
    memcpy(fifo + fifo_len, buf, count);
    fifo_len += count;
    pthread_mutex_unlock(&fifo_lock);
    usleep(200);  /* the next write of this frame comes later */
    return (int)count;
}
struct dockchannel_hid { struct dockchannel *dc; struct mutex tx_mutex; };
struct dchid_iface { struct dockchannel_hid *dchid; int index; u8 tx_seq; };
'''

CASES = r'''
#define FRAMES 40
static struct dockchannel dc;
static struct dockchannel_hid dchid = { .dc = &dc };
static struct dchid_iface comm = { .dchid = &dchid, .index = 0 }, mt = { .dchid = &dchid, .index = 1 };
static volatile int go;

struct sender { struct dchid_iface *iface; u8 msg[16]; size_t size; u32 flags; int sent; };
static void *run(void *p) {
    struct sender *s = p;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
        sched_yield();
    for (int i = 0; i < FRAMES; i++) {
        s->msg[s->size - 1] = (u8)i;
        assert(dchid_send(s->iface, s->flags, s->msg, s->size) == 0);
        s->iface->tx_seq++;
        s->sent++;
        sched_yield();
    }
    return NULL;
}

int main(void) {
    pthread_mutex_init(&dchid.tx_mutex.m, NULL);
    /* 0xb4 iface: enable (2 bytes, a tail word) vs 0xd9 GET_REPORT (1 byte) and a 9-byte frame. */
    struct sender a = { .iface = &comm, .msg = { 0xb4, 4 }, .size = 2, .flags = 0x80 };
    struct sender b = { .iface = &mt, .msg = { 0xd9, 1, 2, 3, 4, 5, 6, 7, 0 }, .size = 9, .flags = 0x41 };
    pthread_t ta, tb;
    assert(!pthread_create(&ta, NULL, run, &a) && !pthread_create(&tb, NULL, run, &b));
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(ta, NULL) && !pthread_join(tb, NULL));

    int seen[2] = { 0, 0 };
    size_t o = 0;
    while (o < fifo_len) {
        struct dchid_hdr hdr;
        assert(o + sizeof(hdr) <= fifo_len);
        memcpy(&hdr, fifo + o, sizeof(hdr));
        assert(hdr.hdr_len == sizeof(hdr) && hdr.channel == DCHID_CHANNEL_CMD && "frame header");
        assert(hdr.iface <= 1);
        size_t body = hdr.length + 4;
        assert(o + sizeof(hdr) + body <= fifo_len);
        u32 sum;
        assert(dchid_verify_checksum(&hdr, fifo + o + sizeof(hdr), &sum) && "frame checksum (firmware: xsum err)");
        struct dchid_subhdr sub;
        memcpy(&sub, fifo + o + sizeof(hdr), sizeof(sub));
        struct sender *s = hdr.iface ? &b : &a;
        assert(sub.length == s->size && sub.flags == s->flags);
        assert(hdr.seq == (u8)seen[hdr.iface] && "per-interface frame order");
        assert(fifo[o + sizeof(hdr) + sizeof(sub) + s->size - 1] == (u8)seen[hdr.iface]);
        seen[hdr.iface]++;
        o += sizeof(hdr) + body;
    }
    assert(seen[0] == FRAMES && seen[1] == FRAMES);
    printf("MTP TX frames: %d + %d frames from two interfaces, all whole and checksum-valid\n", seen[0], seen[1]);
    return 0;
}
'''


def check_callers(source):
    """Every FIFO write is inside dchid_send_frame(), which only dchid_send() calls."""
    frame = function(source, 'dchid_send_frame')
    if source.count('dockchannel_send(') != frame.count('dockchannel_send('):
        raise SystemExit('dockchannel_send() is called outside dchid_send_frame()')
    if len(re.findall(r'\bdchid_send_frame\(', source)) != 2:
        raise SystemExit('dchid_send_frame() has a caller other than dchid_send()')
    if 'mutex_init(&dchid->tx_mutex)' not in source:
        raise SystemExit('tx_mutex is never initialised')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out')
    p.add_argument('--old-transport')
    args = p.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = ((ROOT / TRANSPORT).read_text() if not args.old_transport else subprocess.check_output(
        ['git', 'show', args.old_transport + ':' + TRANSPORT], cwd=ROOT, text=True))
    body = (struct(source, 'dchid_hdr') + '\n' + struct(source, 'dchid_subhdr') + '\n' +
            function(source, 'dchid_checksum') + '\n' + function(source, 'dchid_verify_checksum') + '\n')
    if args.old_transport:
        body += function(source, 'dchid_send') + '\n'
    else:
        check_callers(source)
        body += function(source, 'dchid_send_frame') + '\n' + function(source, 'dchid_send') + '\n'
    with tempfile.TemporaryDirectory(prefix='mtp-tx-frame-') as tmp:
        out = Path(args.out or tmp)
        out.mkdir(parents=True, exist_ok=True)
        c, exe = out/'mtp-tx-frame.c', out/'mtp-tx-frame'
        c.write_text(PREFIX + body + CASES)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-Wno-unused-function', '-pthread',
                        str(c), '-o', str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        (out/'run.log').write_text(r.stdout+r.stderr)
        if args.old_transport:
            if r.returncode != -6 or 'Assertion' not in r.stderr:
                raise SystemExit('old transport did not fail its runtime assertion:\n' + r.stdout + r.stderr)
            print('MTP TX frames: old transport compiled and interleaved two interfaces\' frames')
        elif r.returncode:
            raise SystemExit(r.stdout+r.stderr)
        else:
            print(r.stdout, end='')


if __name__ == '__main__':
    main()
