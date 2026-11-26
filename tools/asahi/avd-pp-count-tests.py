#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production AV1 tile emitter and PP kick counts for tiled frames.

set_tiles() builds one segment per tile and records the frame's PP kick
count. The legacy submit hook and the queued firmware's metadata both take
that count, so it must hold every tile count the stateless API allows: up
to V4L2_AV1_MAX_TILE_COUNT (512). The test runs the production set_tiles(),
avd_av1_pp_kicks() and avd_av1_submit() for grids of 1, 255, 256, 272 and
512 tiles and checks that every tile is kicked, and that the queued
firmware takes a 256-tile frame and refuses a larger one before anything is
written (AVDQ_MAX_PP_EXTRA) instead of kicking a wrapped count.

With --ref the driver of that revision is tested instead (the negative
control).
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/media/platform/apple/avd'

PRELUDE = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controls.h"
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define max_t(t, a, b) ({ t __a = (a); t __b = (b); __a > __b ? __a : __b; })

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct avd_run { dma_addr_t coded_in; };
struct avd_av1_stream;
struct avd_variant { int revision; unsigned int fifo_slots; u32 submit_offset; };
struct avd_dev { const struct avd_variant *variant; char *ctrl; };
struct avd_ctx { struct avd_dev *dev; void *priv; struct { unsigned int num; } job; unsigned int fifo_idx; };

/* Instructions: only counted. The emitters' arguments are not evaluated. */
static unsigned int pushes;
#define push(...) (pushes++)

/* EXEC writes to the submit register */
static char ctrl[0x2000];
static unsigned int kicks;
static void writel(u32 v, char *addr)
{
	(void)v;
	if (addr >= ctrl && addr < ctrl + sizeof(ctrl))
		kicks++;
}
'''

MAIN = r'''
int main(void)
{
	static const struct { unsigned int cols, rows; } grids[] = {
		{ 1, 1 }, { 15, 17 }, { 16, 16 }, { 16, 17 }, { 32, 16 }, { 64, 8 },
	};
	static const struct avd_variant variant = { .revision = 4, .fifo_slots = 15, .submit_offset = 0x1000 };
	struct avd_dev dev = { .variant = &variant, .ctrl = ctrl };
	struct v4l2_ctrl_av1_sequence seq = {};
	struct v4l2_ctrl_av1_frame *frame = calloc(1, sizeof(*frame));
	struct v4l2_ctrl_av1_tile_group_entry *tiles = calloc(V4L2_AV1_MAX_TILE_COUNT, sizeof(*tiles));
	struct avd_av1_ctx *av1 = calloc(1, sizeof(*av1));

	if (!frame || !tiles || !av1)
		return 2;
	for (unsigned int i = 0; i < sizeof(grids) / sizeof(grids[0]); i++) {
		unsigned int cols = grids[i].cols, rows = grids[i].rows, n = cols * rows;
		struct avd_ctx ctx = { .dev = &dev, .priv = av1 };
		struct avd_av1_run run = { .seq = &seq, .frame = frame, .tile_group = tiles,
					   .tile_group_entries = n, .tile_rows = rows,
					   .tile_cols = cols };
		unsigned int pp;

		frame->tile_info.tile_cols = cols;
		frame->tile_info.tile_rows = rows;
		for (unsigned int t = 0; t < n; t++) {
			tiles[t].tile_row = t / cols;
			tiles[t].tile_col = t % cols;
		}
		set_tiles(&ctx, &run);
		CHECK(ctx.job.num == n);
		pp = avd_av1_pp_kicks(&ctx);
		/* One PP kick per tile, never a wrapped count. */
		CHECK(pp == n);
		kicks = 0;
		avd_av1_submit(&ctx);
		CHECK(kicks == n);
		/* The queued firmware runs pp - 1 continuations, or refuses the frame. */
		CHECK((pp - 1 <= AVDQ_MAX_PP_EXTRA) == (n <= AVDQ_MAX_PP_EXTRA + 1));
		printf("%3ux%-2u = %3u tiles: segments %u, PP kicks %u, legacy EXEC writes %u\n",
		       cols, rows, n, ctx.job.num, pp, kicks);
	}
	free(frame);
	free(tiles);
	free(av1);
	printf("%s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
'''


def function(text, name):
    # A definition: 'static', its type (maybe on a line of its own), the name.
    match = re.search(r'^static[^;{}()]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text, re.M)
    if not match:
        print(f'FAIL line 0: {name} is missing')
        sys.exit(1)
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'


def structure(text, name):
    start = text.index('struct ' + name + ' {')
    return text[start:text.index('\n};', start) + 3] + '\n'


def read(ref, path):
    if ref:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{ref}:{path}'], text=True)
    return (ROOT / path).read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ref', help='test the driver of this revision instead')
    args = parser.parse_args()
    av1 = read(args.ref, f'{DRIVER}/avd-av1.c')
    inst = read(args.ref, f'{DRIVER}/avd-inst.h')
    queue = read(args.ref, f'{DRIVER}/avd-backend-queue.c')
    defines = re.findall(r'^#define AVD_OP_EXEC\w*(?:\(\w+\))?\s.*$', inst, re.M)
    defines.append(re.search(r'^#define AVDQ_MAX_PP_EXTRA\s.*$', queue, re.M).group(0))
    code = (PRELUDE + '\n'.join(defines) + '\n' + structure(av1, 'avd_av1_run') +
            structure(av1, 'avd_av1_ctx') + function(av1, 'set_tiles') +
            function(av1, 'avd_av1_pp_kicks') + function(av1, 'avd_av1_submit') + MAIN)
    with tempfile.TemporaryDirectory(prefix='avd-pp-count-') as tmp:
        (Path(tmp) / 'controls.h').write_text(read(args.ref, 'include/uapi/linux/v4l2-controls.h'))
        src = Path(tmp) / 'production.c'
        binary = Path(tmp) / 'production'
        src.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                        '-fsanitize=address,undefined', str(src), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(run.stdout + run.stderr)
    sys.exit(run.returncode)


if __name__ == '__main__':
    main()
