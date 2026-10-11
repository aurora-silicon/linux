#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Production ADMAC cyclic preparation rejects malformed ranges before allocation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def function(source, name):
    at = source.index(name + "(")
    start = source.rfind("\n", 0, at) + 1
    opening = source.index("{", at)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    cc = os.environ.get("CC", "cc")
    if not shutil.which(cc):
        print("SKIP: C compiler required")
        return 4
    source = (ROOT / "drivers/dma/apple-admac.c").read_text()
    functions = "\n".join(function(source, name) for name in (
        "admac_cyclic_range_valid", "admac_prep_dma_cyclic"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define U32_MAX UINT32_MAX
#define DMA_BIT_MASK(n) ((UINT64_C(1) << (n)) - 1)
#define GFP_NOWAIT 0
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
typedef uint64_t dma_addr_t;
enum dma_transfer_direction { DMA_MEM_TO_DEV, DMA_DEV_TO_MEM };
struct dma_chan { int unused; };
struct dma_async_tx_descriptor {
    struct dma_chan *chan;
    int (*tx_submit)(struct dma_async_tx_descriptor *);
    int (*desc_free)(struct dma_async_tx_descriptor *);
};
struct admac_chan { struct dma_chan chan; int no; };
struct admac_tx {
    struct dma_async_tx_descriptor tx;
    bool cyclic;
    dma_addr_t buf_addr, buf_end;
    size_t buf_len, period_len, submitted_pos, reclaimed_pos;
};
static int allocations;
static void *allocate(size_t bytes) { allocations++; return calloc(1, bytes); }
#define kzalloc_obj(object, flags) allocate(sizeof(object))
static enum dma_transfer_direction admac_chan_direction(int channel)
{ return channel & 1 ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV; }
static int admac_tx_submit(struct dma_async_tx_descriptor *tx) { (void)tx; return 0; }
static int admac_desc_free(struct dma_async_tx_descriptor *tx) { free(tx); return 0; }
static void dma_async_tx_descriptor_init(struct dma_async_tx_descriptor *tx, struct dma_chan *chan)
{ tx->chan = chan; }
FUNCTIONS
int main(void)
{
    struct admac_chan channel = {.no = 4}; /* Qualified base-ns TX2 numbering. */
    const uint64_t edge = UINT64_C(1) << 42;
    struct { uint64_t address; size_t bytes, period; bool valid; } cases[] = {
        {0x4000, 768000, 384000, true},
        {0x4000, 0x200000, 0x10000, true},
        {edge - 0x4000, 0x4000, 0x4000, true},
        {0x4000, 0, 1, false}, {0x4000, 1, 0, false},
        {0x4000, 10, 3, false}, {0x4000, 4, 8, false},
        {0x4000, UINT64_C(1) << 32, UINT64_C(1) << 32, false},
        {edge, 1, 1, false}, {edge - 4, 8, 4, false},
        {UINT64_MAX - 1, 8, 4, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = allocations;
        struct dma_async_tx_descriptor *descriptor = admac_prep_dma_cyclic(
            &channel.chan, cases[i].address, cases[i].bytes, cases[i].period,
            DMA_MEM_TO_DEV, 0);
        assert(!!descriptor == cases[i].valid);
        assert(allocations == before + cases[i].valid);
        if (descriptor) {
            struct admac_tx *tx = container_of(descriptor, struct admac_tx, tx);
            assert(tx->cyclic && tx->buf_addr == cases[i].address);
            assert(tx->buf_end == cases[i].address + cases[i].bytes);
            assert(tx->period_len == cases[i].period && !tx->submitted_pos && !tx->reclaimed_pos);
            descriptor->desc_free(descriptor);
        }
    }
    assert(!admac_prep_dma_cyclic(&channel.chan, 0x4000, 0x4000, 0x4000, DMA_DEV_TO_MEM, 0));
    printf("12 production cyclic preparation cases passed; invalid ranges allocate nothing\n");
    return 0;
}
'''.replace("FUNCTIONS", functions).replace("#include <assert.h>", "#include <assert.h>\n#include <stdio.h>")
    with tempfile.TemporaryDirectory(prefix="admac-descriptor-range-") as directory:
        directory = Path(directory)
        c, binary = directory / "fixtures.c", directory / "fixtures"
        c.write_text(harness)
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        str(c), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
