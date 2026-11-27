#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for T8132 external dual-pipe peer bonding helpers (dcp-m4-peer.h)."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uint32_t __le32;

#define __packed __attribute__((__packed__))
#define struct_group_attr(NAME, ATTR, MEMBERS...) \
	union { \
		struct { MEMBERS } ATTR; \
		struct { MEMBERS } ATTR NAME; \
	}
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#define static_assert _Static_assert

static inline u32 get_unaligned_be32(const void *p)
{
	const u8 *b = p;

	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static inline void put_unaligned_be32(u32 v, void *p)
{
	u8 *b = p;

	b[0] = v >> 24;
	b[1] = v >> 16;
	b[2] = v >> 8;
	b[3] = v;
}

static inline u32 get_unaligned_le32(const void *p)
{
	u32 v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void put_unaligned_le32(u32 v, void *p)
{
	memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le16(u16 v, void *p)
{
	memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le64(u64 v, void *p)
{
	memcpy(p, &v, sizeof(v));
}

#define BIT(n) (1UL << (n))
#define U32_MAX (~(u32)0)
#define U64_MAX (~(u64)0)

struct dcp_m4_profile {
	u32 swap_size;
	u32 panel_width;
	u32 panel_height;
	u32 disabled_color;
	u32 swap_nullable;
	u32 swap_tail;
	u32 swap_nits;
	u32 swap_surface;
	u32 swap_dva;
	u32 swap_intent;
	u32 done_size;
};
static const struct dcp_m4_profile test_pf = {
	.swap_size = 0x1000,
	.panel_width = 2560,
	.panel_height = 1664,
	.disabled_color = 0xff000000,
	.swap_nullable = 0x200,
	.swap_tail = 0x220,
	.swap_nits = 0x360,
	.swap_surface = 0x400,
	.swap_dva = 0xd00,
	.swap_intent = 0x380,
	.done_size = 0x730,
};
static const struct dcp_m4_profile *dcp_m4_pf = &test_pf;

'''

TESTS = r'''
static struct dcp_display_hints make_hints(bool valid, bool tiled, u32 w, u32 h,
					   u32 total, u32 active)
{
	struct dcp_display_hints rec = {};

	rec.hints[0] = (valid ? 1 : 0) | (tiled ? 2 : 0);
	put_unaligned_le32(w, rec.hints + 0x38);
	put_unaligned_le32(h, rec.hints + 0x3c);
	put_unaligned_le32(total, rec.hints + 0x40);
	put_unaligned_le32(active, rec.hints + 0x44);
	return rec;
}

static void test_needs_pair(void)
{
	struct dcp_display_hints h;

	/* 4K@144 (3840x2160, active ~1.194 Gpx/s): single pipe. */
	h = make_hints(true, false, 3840, 2160, 1332750000U, 1194393600U);
	assert(!dcp_m4_peer_needs_pair(&h));

	/* 5K@60 Studio Display (5120x2880, active 884.7 Mpx/s): single pipe. */
	h = make_hints(true, false, 5120, 2880, 960000000U, 884736000U);
	assert(!dcp_m4_peer_needs_pair(&h));

	/* Exact single-pipe ceiling (6144x3456@60 = 1,274,019,840): single pipe. */
	h = make_hints(true, false, 6144, 3456, 1350000000U, 1274019840U);
	assert(!dcp_m4_peer_needs_pair(&h));

	/* AW3926QW 5120x2160@120 (active 1.316 Gpx/s): needs 1 peer pipe. */
	h = make_hints(true, false, 5120, 2160, 1450000000U, 1316044800U);
	assert(dcp_m4_peer_needs_pair(&h));

	/* AW3926QW 5120x2160@165 (exact macOS SetHints payload): needs 1 peer pipe. */
	h = make_hints(true, false, 5120, 2160, 2036750000U, 1824664387U);
	assert(dcp_m4_peer_needs_pair(&h));

	/* Super-ultrawide 7680x2160@60 (width > 6144): needs 1 peer pipe. */
	h = make_hints(true, false, 7680, 2160, 1100000000U, 995328000U);
	assert(dcp_m4_peer_needs_pair(&h));

	/* Invalid, tiled, zero dimensions, or >2-pipe sinks refused. */
	h = make_hints(false, false, 5120, 2160, 2036750000U, 1824664387U);
	assert(!dcp_m4_peer_needs_pair(&h));
	h = make_hints(true, true, 5120, 2160, 2036750000U, 1824664387U);
	assert(!dcp_m4_peer_needs_pair(&h));
	h = make_hints(true, false, 0, 2160, 2036750000U, 1824664387U);
	assert(!dcp_m4_peer_needs_pair(&h));
	h = make_hints(true, false, 12289, 2160, 2036750000U, 1824664387U);
	assert(!dcp_m4_peer_needs_pair(&h));
	h = make_hints(true, false, 5120, 2160, 3000000000U, 2548039681U);
	assert(!dcp_m4_peer_needs_pair(&h));

	puts("PASS dcp_m4_peer_needs_pair boundary checks");
}

static void test_split_timing_and_tiling(void)
{
	u8 timing[0x50] = {};
	struct dcp_peer_tiling primary = {
		.vftg = true, .member = true, .primary = true,
		.width = 2560, .height = 2160, .x = 0, .y = 0,
		.display_id = 101, .display_id_valid = true,
	};
	struct dcp_peer_tiling peer = {
		.vftg = true, .member = true, .primary = false,
		.width = 2560, .height = 2160, .x = 2560, .y = 0,
		.display_id = 102, .display_id_valid = true,
	};
	u32 val = 0;

	put_unaligned_le32(5280, timing + 4);
	put_unaligned_le32(5120, timing + 8);
	put_unaligned_le32(48, timing + 12);
	put_unaligned_le32(32, timing + 16);
	put_unaligned_le32(80, timing + 20);
	put_unaligned_le32((u32)(s32)-16, timing + 24);
	put_unaligned_le32(160, timing + 28);
	put_unaligned_le32(2160, timing + 0x28);
	put_unaligned_le32(2036750000U, timing + 68);

	dcp_m4_peer_split_timing(timing);
	assert(get_unaligned_le32(timing + 4) == 2640);
	assert(get_unaligned_le32(timing + 8) == 2560);
	assert(get_unaligned_le32(timing + 12) == 24);
	assert(get_unaligned_le32(timing + 16) == 16);
	assert(get_unaligned_le32(timing + 20) == 40);
	assert((s32)get_unaligned_le32(timing + 24) == -8);
	assert(get_unaligned_le32(timing + 28) == 80);
	assert(get_unaligned_le32(timing + 0x28) == 2160);
	assert(get_unaligned_le32(timing + 68) == 1018375000U);

	/* Upward-only T8132 peer rule: 1 -> 2 allowed, 2 -> 1 and 0 -> * rejected. */
	assert(dcp_peer_index_allowed_26_6(1, 2));
	assert(!dcp_peer_index_allowed_26_6(2, 1));
	assert(!dcp_peer_index_allowed_26_6(0, 1));
	assert(!dcp_peer_index_allowed_26_6(1, 1));

	/* Tiling state routing across primary and peer. */
	assert(dcp_peer_tiling_get(&primary, &peer, 1, 4, &val) && val == 101);
	assert(dcp_peer_tiling_get(&primary, &peer, 2, 4, &val) && val == 102);
	assert(dcp_peer_tiling_get(&peer, &primary, 1, 5, &val) && val == 2560);
	assert(dcp_peer_tiling_set(&primary, &peer, 1, 10, 2));
	assert(dcp_peer_tiling_get(&peer, &primary, 2, 10, &val) && val == 2);

	puts("PASS split timing, peer index rule, and tiling state");
}

static void test_swap_set_tile(void)
{
	u8 left[0x1000], right[0x1000];
	struct dcp_m4_frame f = {
		.dva = (1ULL << 40),
		.backing_size = 64ULL << 20,
		.width = 5120,
		.height = 2160,
		.stride = 5120 * 4,
		.format = DCP_M4_XRGB8888,
	};

	assert(dcp_m4_swap_encode_on(left, sizeof(left), 42, true, &f, NULL, 0,
				     false, 0, dcp_m4_frame_check_ext, 5120, 2160) == 0);
	assert(dcp_m4_swap_encode_on(right, sizeof(right), 42, true, &f, NULL, 0,
				     false, 0, dcp_m4_frame_check_ext, 5120, 2160) == 0);

	assert(dcp_m4_swap_set_tile(left, sizeof(left), 5120, 0, 2560, 2160) == 0);
	assert(dcp_m4_swap_set_tile(right, sizeof(right), 5120, 2560, 2560, 2160) == 0);

	/* Left tile: src (0,0,2560,2160) -> dst (0,0,2560,2160), surface stays 5120x2160. */
	assert(get_unaligned_le32(left + 0xac) == 0);
	assert(get_unaligned_le32(left + 0xb0) == 0);
	assert(get_unaligned_le32(left + 0xb4) == 2560);
	assert(get_unaligned_le32(left + 0xb8) == 2160);
	assert(get_unaligned_le32(left + 0x10c) == 0);
	assert(get_unaligned_le32(left + 0x110) == 0);
	assert(get_unaligned_le32(left + 0x114) == 2560);
	assert(get_unaligned_le32(left + 0x118) == 2160);
	assert(get_unaligned_le32(left + test_pf.swap_surface + 0x21) == 5120);

	/* Right tile: src (2560,0,2560,2160) -> dst (0,0,2560,2160), surface stays 5120x2160. */
	assert(get_unaligned_le32(right + 0xac) == 2560);
	assert(get_unaligned_le32(right + 0xb0) == 0);
	assert(get_unaligned_le32(right + 0xb4) == 2560);
	assert(get_unaligned_le32(right + 0xb8) == 2160);
	assert(get_unaligned_le32(right + 0x10c) == 0);
	assert(get_unaligned_le32(right + 0x110) == 0);
	assert(get_unaligned_le32(right + 0x114) == 2560);
	assert(get_unaligned_le32(right + 0x118) == 2160);
	assert(get_unaligned_le32(right + test_pf.swap_surface + 0x21) == 5120);

	/* Reject out-of-bounds tiles. */
	assert(dcp_m4_swap_set_tile(right, sizeof(right), 5120, 2561, 2560, 2160) == -EINVAL);
	assert(dcp_m4_swap_set_tile(right, sizeof(right), 5120, 0, 0, 2160) == -EINVAL);
	assert(dcp_m4_swap_set_tile(right, 0x100, 5120, 0, 2560, 2160) == -EINVAL);

	puts("PASS dcp_m4_swap_set_tile left/right tile encoding");
}

static void test_concurrent_oob_transaction(void)
{
	static u8 rpc[DCP_M4_RPC_SIZE];
	struct dcp_m4_transaction state = { .external = true };
	struct dcp_m4_transaction copy;
	u8 mode[8] = { 1, 0, 0, 0, 2, 0, 0, 0 };
	u8 req367[36] = {};
	u8 out367[32] = {};
	u32 out369 = 0, out413 = 0;
	u64 word = 0;
	const u32 tag413 = 'A' | ('4' << 8) | ('1' << 16) | ('3' << 24);
	const u32 tag369 = 'A' | ('3' << 8) | ('6' << 16) | ('9' << 24);
	const u32 tag367 = 'A' | ('3' << 8) | ('6' << 16) | ('7' << 24);

	memset(rpc, 0, sizeof(rpc));

	/* Primary stream-0 A413 in flight on external DCP. */
	assert(dcp_m4_call_begin_stream(&state, rpc, sizeof(rpc), tag413, 0, false,
					mode, sizeof(mode), sizeof(out413), &word) == 0);
	assert(state.calls == 1 && !state.oob && ((word >> 10) & 3) == 0);

	/* Internal DCP must still refuse concurrent stream-1 OOB during stream-0 call. */
	copy = state;
	copy.external = false;
	assert(dcp_m4_call_begin_stream(&copy, rpc, sizeof(rpc), tag369, 1, false,
					NULL, 0, sizeof(out369), &word) == -EINVAL);

	/* External DCP allows one concurrent stream-1 OOB query while A413 is paused. */
	assert(dcp_m4_call_begin_stream(&state, rpc, sizeof(rpc), tag369, 1, false,
					NULL, 0, sizeof(out369), &word) == 0);
	assert(state.calls == 2 && state.oob && ((word >> 10) & 3) == 1);

	/* A second overlapping stream-1 OOB call is refused. */
	copy = state;
	assert(dcp_m4_call_begin_stream(&copy, rpc, sizeof(rpc), tag367, 1, false,
					req367, sizeof(req367), sizeof(out367), &word) == -EINVAL);

	/* Reply to stream-1 A369 delivers output and clears state.oob while A413 stays open. */
	put_unaligned_le32(1, rpc + DCP_M4_STREAM_SIZE + DCP_M4_PACKET_HEADER);
	assert(dcp_m4_call_reply(&state, rpc, sizeof(rpc), 0x442, &out369, sizeof(out369)) == 0);
	assert(out369 == 1 && state.calls == 1 && !state.oob);

	/* A second sequential stream-1 OOB query succeeds while A413 is still open. */
	assert(dcp_m4_call_begin_stream(&state, rpc, sizeof(rpc), tag367, 1, false,
					req367, sizeof(req367), sizeof(out367), &word) == 0);
	assert(state.calls == 2 && state.oob && ((word >> 10) & 3) == 1);
	rpc[DCP_M4_STREAM_SIZE + DCP_M4_PACKET_HEADER + sizeof(req367)] = 0x5a;
	assert(dcp_m4_call_reply(&state, rpc, sizeof(rpc), 0x442, out367, sizeof(out367)) == 0);
	assert(out367[0] == 0x5a && state.calls == 1 && !state.oob);

	/* Outer stream-0 A413 reply completes normally. */
	put_unaligned_le32(0, rpc + DCP_M4_PACKET_HEADER + sizeof(mode));
	assert(dcp_m4_call_reply(&state, rpc, sizeof(rpc), 0x42, &out413, sizeof(out413)) == 0);
	assert(out413 == 0 && state.calls == 0 && !state.oob);

	puts("PASS concurrent stream-1 OOB during paused stream-0 external call");
}

int main(void)
{
	test_needs_pair();
	test_split_timing_and_tiling();
	test_swap_set_tile();
	test_concurrent_oob_transaction();
	return 0;
}
'''


def strip_kernel_includes(text):
    return '\n'.join(
        line for line in text.splitlines()
        if not line.startswith('#include <linux/')
        and not line.startswith('#include "dcp-peer-v26_6.h"')
        and not line.startswith('#include "dcp-m4-profile.h"')
        and not line.startswith('#include "dcp-m4-protocol.h"')
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('width-only', 'no-concurrent-oob'))
    args = parser.parse_args()

    base_hdr = strip_kernel_includes((ROOT / 'drivers/gpu/drm/apple/dcp-peer-v26_6.h').read_text())
    peer_hdr = strip_kernel_includes((ROOT / 'drivers/gpu/drm/apple/dcp-m4-peer.h').read_text())
    frame_hdr = strip_kernel_includes((ROOT / 'drivers/gpu/drm/apple/dcp-m4-frame.h').read_text())
    proto_hdr = strip_kernel_includes((ROOT / 'drivers/gpu/drm/apple/dcp-m4-protocol.h').read_text())
    txn_hdr = strip_kernel_includes((ROOT / 'drivers/gpu/drm/apple/dcp-m4-transaction.h').read_text())
    if args.mutation == 'width-only':
        old = 'if (width <= DCP_M4_EXT_PIPE_MAX_WIDTH &&\n\t    active <= DCP_M4_EXT_PIPE_MAX_ACTIVE_RATE)'
        new = 'if (width <= DCP_M4_EXT_PIPE_MAX_WIDTH)'
        assert old in peer_hdr
        peer_hdr = peer_hdr.replace(old, new, 1)
    elif args.mutation == 'no-concurrent-oob':
        old = 'concurrent_oob = stream == 1 && state->external && !state->oob &&'
        new = 'concurrent_oob = false && stream == 1 && state->external && !state->oob &&'
        assert old in txn_hdr
        txn_hdr = txn_hdr.replace(old, new, 1)

    code = '\n'.join((SHIM, base_hdr, peer_hdr, frame_hdr, proto_hdr, txn_hdr, TESTS))
    with tempfile.TemporaryDirectory(prefix='dcp-m4-peer-') as directory:
        src = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        src.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(src), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)])
        if run.returncode:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
