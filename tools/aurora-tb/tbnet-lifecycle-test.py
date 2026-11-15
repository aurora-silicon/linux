#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Exercise real Thunderbolt net setup/unwind with synthetic host boundaries.

Error-checking pthread mutexes detect recursive ownership. DMA-owned pages
cannot be freed until their ring stops. No hardware or network is accessed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess


FUNCTIONS = (
    'tbnet_free_buffers', 'tbnet_alloc_rx_buffers', 'tbnet_disable_napi',
    'tbnet_connect_failed_locked', 'tbnet_connected_work',
)


def function(source, name):
    match = re.search(r'(?m)^static (?:void|int) ' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError('missing production function: ' + name)
    opening = source.index('{', match.start())
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREFIX = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define TBNET_RING_SIZE 4
#define TBNET_FRAME_SIZE 4096
#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define netdev_dbg(...) ((void)0)
#define netdev_err(...) ((void)0)
#define trace_tbnet_free_frame(...) ((void)0)
#define trace_tbnet_alloc_rx_frame(...) ((void)0)
struct mutex { pthread_mutex_t native; bool held; pthread_t owner; };
static void mutex_init(struct mutex *m)
{
    pthread_mutexattr_t a;
    assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK));
    assert(!pthread_mutex_init(&m->native, &a));
    assert(!pthread_mutexattr_destroy(&a));
}
static void mutex_lock(struct mutex *m)
{
    assert(!pthread_mutex_lock(&m->native));
    m->held = true;
    m->owner = pthread_self();
}
static void assert_held(struct mutex *m)
{
    assert(m->held && pthread_equal(m->owner, pthread_self()));
}
static void mutex_unlock(struct mutex *m)
{
    assert_held(m);
    m->held = false;
    assert(!pthread_mutex_unlock(&m->native));
}
#define lockdep_assert_held(m) assert_held(m)
#define spin_lock_bh(m) mutex_lock(m)
#define spin_unlock_bh(m) mutex_unlock(m)
struct tbnet;
struct tb_ring;
struct device { struct tb_ring *ring; };
struct page { bool queued; struct tb_ring *ring; };
struct page_pool { struct tb_ring *ring; };
struct ring_frame { uintptr_t buffer_phy; };
struct net_device { bool carrier, queue; };
struct tbnet_frame {
    struct page *page;
    struct ring_frame frame;
    struct net_device *dev;
};
struct tb_ring {
    bool running;
    int hop;
    struct device dma;
    struct tbnet *net;
};
struct tbnet_ring {
    unsigned int prod, cons;
    struct tbnet_frame frames[TBNET_RING_SIZE];
    struct tb_ring *ring;
    struct page_pool *pool;
};
struct work_struct { int unused; };
struct napi_struct { bool enabled; };
struct tb_xdomain { int allocated; struct tbnet *net; };
struct tbnet {
    struct tb_xdomain *xd;
    struct net_device *dev;
    struct napi_struct napi;
    bool napi_enabled, rings_started, login_sent, login_received;
    struct mutex connection_lock, poll_lock;
    struct work_struct connected_work;
    int remote_transmit_path, local_transmit_path;
    struct tbnet_ring rx_ring, tx_ring;
};
static int hop_result, rx_budget, rx_allocations, live_pages;
static int enqueue_error, enqueue_calls, tx_error, path_error;
static int rearmed, released_hops, unmapped;
static struct page *new_page(struct tb_ring *r)
{
    struct page *p = calloc(1, sizeof(*p));
    assert(p);
    p->ring = r;
    live_pages++;
    return p;
}
static struct page *page_pool_dev_alloc_pages(struct page_pool *p)
{
    assert(!p->ring->net->napi_enabled);
    if (rx_allocations++ >= rx_budget)
        return NULL;
    return new_page(p->ring);
}
static uintptr_t page_pool_get_dma_addr(struct page *p)
{
    return (uintptr_t)p;
}
static void page_pool_put_full_page(struct page_pool *pool, struct page *p, bool direct)
{
    (void)direct;
    assert(p->ring == pool->ring);
    assert(!p->queued || !p->ring->running);
    live_pages--;
    free(p);
}
static struct device *tb_ring_dma_device(struct tb_ring *r) { return &r->dma; }
static void dma_unmap_page(struct device *d, uintptr_t address, size_t size, int direction)
{
    (void)size;
    assert(!d->ring->running && address && direction == DMA_TO_DEVICE);
    unmapped++;
}
static void __free_page(struct page *p)
{
    assert(!p->ring->running);
    live_pages--;
    free(p);
}
static bool netif_carrier_ok(struct net_device *d) { return d->carrier; }
static void netif_carrier_on(struct net_device *d) { d->carrier = true; }
static void netif_start_queue(struct net_device *d) { d->queue = true; }
static int tb_xdomain_alloc_in_hopid(struct tb_xdomain *xd, int requested)
{
    (void)requested;
    assert_held(&xd->net->connection_lock);
    assert(xd->allocated == -1);
    if (hop_result >= 0)
        xd->allocated = hop_result;
    return hop_result;
}
static void tb_xdomain_release_in_hopid(struct tb_xdomain *xd, int hop)
{
    assert(xd->allocated == hop);
    xd->allocated = -1;
    released_hops++;
}
static void tb_ring_start(struct tb_ring *r)
{
    assert_held(&r->net->connection_lock);
    assert(!r->running);
    r->running = true;
}
static void tb_ring_stop(struct tb_ring *r)
{
    assert(!r->net->napi_enabled);
    assert(r->running);
    r->running = false;
}
static int tb_ring_rx(struct tb_ring *r, struct ring_frame *f)
{
    struct tbnet_frame *tf = container_of(f, struct tbnet_frame, frame);
    assert(r->running && !r->net->napi_enabled);
    if (++enqueue_calls == enqueue_error)
        return -ESHUTDOWN;
    tf->page->queued = true;
    return 0;
}
static int tbnet_alloc_tx_buffers(struct tbnet *net)
{
    struct tbnet_frame *tf = &net->tx_ring.frames[0];
    tf->page = new_page(net->tx_ring.ring);
    tf->frame.buffer_phy = tx_error ? 0 : (uintptr_t)tf->page;
    return tx_error ? -ENOMEM : 0;
}
static void napi_enable(struct napi_struct *n) { assert(!n->enabled); n->enabled = true; }
static void napi_disable(struct napi_struct *n) { assert(n->enabled); n->enabled = false; }
static void tb_ring_poll_complete(struct tb_ring *r)
{
    assert(r->net->napi_enabled && r->net->rx_ring.prod == TBNET_RING_SIZE);
    rearmed++;
}
static int tb_xdomain_enable_paths(struct tb_xdomain *xd, int local, int tx,
                                  int remote, int rx)
{
    (void)local; (void)tx; (void)remote; (void)rx;
    assert(xd->net->napi_enabled && rearmed == 1);
    return path_error ? -EIO : 0;
}
'''


SUFFIX = r'''
int main(int argc, char **argv)
{
    struct tbnet net = { 0 };
    struct net_device dev = { 0 };
    struct tb_xdomain xd = { .allocated = -1, .net = &net };
    struct tb_ring rx = { .net = &net }, tx = { .net = &net };
    struct page_pool pool = { .ring = &rx };
    bool ignored = false;
    int expected_unmaps = 0;

    assert(argc == 2);
    rx.dma.ring = &rx;
    tx.dma.ring = &tx;
    net.xd = &xd;
    net.dev = &dev;
    net.rx_ring.ring = &rx;
    net.tx_ring.ring = &tx;
    net.rx_ring.pool = &pool;
    net.remote_transmit_path = 5;
    net.local_transmit_path = 3;
    net.login_sent = net.login_received = true;
    hop_result = 5;
    rx_budget = TBNET_RING_SIZE;
    mutex_init(&net.connection_lock);
    mutex_init(&net.poll_lock);

    if (!strcmp(argv[1], "negative-hop")) hop_result = -ENOMEM;
    else if (!strcmp(argv[1], "mismatched-hop")) hop_result = 7;
    else if (!strcmp(argv[1], "rx-oom")) rx_budget = 2;
    else if (!strcmp(argv[1], "rx-enqueue")) enqueue_error = 2;
    else if (!strcmp(argv[1], "tx-oom")) tx_error = 1;
    else if (!strcmp(argv[1], "path-error")) {
        path_error = 1;
        expected_unmaps = 1;
    } else if (!strcmp(argv[1], "already-up")) {
        ignored = true;
        dev.carrier = true;
    } else if (!strcmp(argv[1], "await-login")) {
        ignored = true;
        net.login_sent = false;
    } else if (!strcmp(argv[1], "await-peer")) {
        ignored = true;
        net.login_received = false;
    } else assert(!strcmp(argv[1], "success"));

    tbnet_connected_work(&net.connected_work);
    assert(!net.connection_lock.held && !net.poll_lock.held);
    mutex_lock(&net.connection_lock);
    mutex_unlock(&net.connection_lock);
    if (!strcmp(argv[1], "success")) {
        assert(dev.carrier && dev.queue && net.rings_started && net.napi_enabled);
        assert(xd.allocated == 5 && net.login_received && net.login_sent);
        assert(net.rx_ring.prod == TBNET_RING_SIZE);
        assert(live_pages == TBNET_RING_SIZE + 1);
        tbnet_disable_napi(&net);
        tb_ring_stop(&rx);
        tb_ring_stop(&tx);
        tbnet_free_buffers(&net.rx_ring);
        tbnet_free_buffers(&net.tx_ring);
        tb_xdomain_release_in_hopid(&xd, 5);
    } else {
        assert(!dev.queue && !rx.running && !tx.running);
        assert(!net.napi_enabled && !net.rings_started && xd.allocated == -1);
        assert(!live_pages);
        if (!ignored) {
            assert(!net.login_sent && net.login_received);
            assert(released_hops == (hop_result < 0 ? 0 : 1));
            assert(unmapped == expected_unmaps);
        }
    }
    assert(!live_pages);
    assert(!pthread_mutex_destroy(&net.connection_lock.native));
    assert(!pthread_mutex_destroy(&net.poll_lock.native));
    return 0;
}
'''


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('negative control target is ambiguous or missing')
    return text.replace(before, after, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    original = (args.source / 'drivers/net/thunderbolt/main.c').read_text()
    functions = {name: function(original, name) for name in FUNCTIONS}
    production = '\n\n'.join(functions.values())
    notice = original.split('#include', 1)[0]
    args.output.mkdir(parents=True, exist_ok=True)
    results = []

    def compile_unit(name, body):
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        source = directory / 'host.c'
        source.write_text(notice + PREFIX + '\n' + body + '\n' + SUFFIX)
        command = shlex.split(os.environ.get('HOSTCC', 'clang')) + [
            '-std=gnu11', '-Wall', '-Werror', '-O1', '-g', '-pthread',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            str(source), '-o', str(directory / 'host'),
        ]
        (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (directory / 'compile.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        return directory

    def execute(directory, case, should_pass):
        result = subprocess.run([str(directory / 'host'), case], capture_output=True,
                                text=True, timeout=10)
        (directory / (case + '.log')).write_text(result.stdout + result.stderr)
        passed = (result.returncode == 0) == should_pass
        results.append(dict(name=directory.name + '/' + case, passed=passed,
                            rc=result.returncode, negative_control=not should_pass))
        print(('ok ' if passed else 'not ok ') + str(len(results)) + ' - '
              + results[-1]['name'], flush=True)

    cases = ('negative-hop', 'mismatched-hop', 'rx-oom', 'rx-enqueue', 'tx-oom',
             'path-error', 'success', 'already-up', 'await-login', 'await-peer')
    print('TAP version 13', flush=True)
    print('1..' + str(len(cases) + (4 if args.negative_controls else 0)), flush=True)
    positive = compile_unit('production', production)
    for case in cases:
        execute(positive, case, True)
    if args.negative_controls:
        mutations = [
            ('recursive-lock', 'negative-hop',
             '\tlockdep_assert_held(&net->connection_lock);',
             '\tmutex_lock(&net->connection_lock);'),
            ('live-ring-free', 'rx-oom', '\ttb_ring_stop(net->rx_ring.ring);',
             '\t/* Negative control: omit RX stop. */'),
            ('free-on-oom', 'rx-oom', '\t\tif (!page)\n\t\t\treturn -ENOMEM;',
             '\t\tif (!page) { tbnet_free_buffers(ring); return -ENOMEM; }'),
            ('lose-peer-login', 'negative-hop', '\tnet->login_sent = false;',
             '\tnet->login_sent = false;\n\tnet->login_received = false;'),
        ]
        for name, case, before, after in mutations:
            directory = compile_unit(name, replace_once(production, before, after))
            execute(directory, case, False)
    record = dict(source_sha256=hashlib.sha256(original.encode()).hexdigest(),
                  function_sha256={name: hashlib.sha256(body.encode()).hexdigest()
                                   for name, body in functions.items()}, results=results)
    (args.output / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    return int(any(not result['passed'] for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
