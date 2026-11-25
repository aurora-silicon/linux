#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Paired firmware-27 completions carry the retired swap's displayed state to the owner."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'drivers/gpu/drm/apple/dcp-peer-frames-v27.h').read_text()
SHOWN = '''	*shown = s->have_shown[0] && s->shown[0] == done &&
		 s->have_shown[1] && s->shown[1] == done;
'''
assert HEADER.count(SHOWN) == 1
if '--always-shown' in sys.argv:
    HEADER = HEADER.replace(SHOWN, '\t*shown = true;\n')
if '--owner-only' in sys.argv:
    HEADER = HEADER.replace(SHOWN, '\t*shown = s->have_shown[0] && s->shown[0] == done;\n')

TEST = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32; typedef int32_t s32;
''' + HEADER.replace('#include <linux/types.h>', '') + r'''
static struct dcp_peer_frames_v27 paired(u32 first, u32 last)
{
	struct dcp_peer_frames_v27 s = { .active = true };

	for (u32 id = first; id <= last; id++)
		dcp_peer_frame_submit_v27(&s, id);
	return s;
}

int main(void)
{
	struct dcp_peer_frames_v27 s;
	u32 w = 0;
	bool shown = true;

	/* A new adaptive-sync minimum can reuse only the installed mode. */
	s = (struct dcp_peer_frames_v27){ .timing = 81, .color = 86, .active = true };
	assert(dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0, false));
	assert(!dcp_peer_vrr_only_v27(&s, 80, 86, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 85, 0x300000, false));
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, true));
	s.failed = true;
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));
	s.failed = false;
	s.active = false;
	assert(!dcp_peer_vrr_only_v27(&s, 81, 86, 0x300000, false));

	/* First modeset of a pair: both engines display swap 5. */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 5, true, &w, &shown));
	assert(w == 5 && shown);

	/* The peer's half of swap 5 was not displayed. */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 5, false, &w, &shown));
	assert(w == 5 && !shown);

	/* Neither half displayed (the owner's completion came second). */
	s = paired(5, 5);
	assert(!dcp_peer_frame_complete_v27(&s, 1, 5, false, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 0, 5, false, &w, &shown));
	assert(w == 5 && !shown);

	/*
	 * Engines drift: the peer has shown 6 when the owner shows 5. The
	 * retired swap is 5, which the peer's shown 6 does not vouch for;
	 * swap 6 then retires shown.
	 */
	s = paired(5, 6);
	assert(!dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(w == 5 && !shown);
	assert(dcp_peer_frame_complete_v27(&s, 0, 6, true, &w, &shown));
	assert(w == 6 && shown);

	/* A displayed earlier swap does not mark a later, undisplayed one. */
	s = paired(5, 6);
	assert(!dcp_peer_frame_complete_v27(&s, 0, 5, true, &w, &shown));
	assert(!dcp_peer_frame_complete_v27(&s, 0, 6, false, &w, &shown));
	assert(dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	assert(w == 6 && !shown);

	/* A stale or duplicate completion is not a retirement. */
	assert(!dcp_peer_frame_complete_v27(&s, 1, 6, true, &w, &shown));
	return 0;
}
'''

with tempfile.TemporaryDirectory() as t:
    src = Path(t) / 'peer.c'
    src.write_text(TEST)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-fsanitize=address,undefined',
                    str(src), '-o', str(src.with_suffix(''))], check=True)
    if subprocess.run([str(src.with_suffix(''))]).returncode:
        sys.exit(1)
print('PASS paired completion shown state: first modeset, undisplayed half, drift, stale shown, duplicate')
