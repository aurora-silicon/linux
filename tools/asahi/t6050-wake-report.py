#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode one production pending/mask snapshot; no hardware or hold qualification."""
import argparse
import json
from pathlib import Path
import re
import struct


def parse(text):
    if 'wake register snapshot unavailable' in text:
        raise ValueError('explicitly unavailable wake snapshot')
    words = {}
    for label in ('pending', 'masks'):
        matches = re.findall(r'\bwake ' + label + r' AP ([0-9a-fA-F ]+) GFX (0x[0-9a-fA-F]+|0)\s*$', text, re.M)
        if len(matches) != 1:
            raise ValueError('require exactly one ' + label + ' record in this row interval')
        raw = bytes.fromhex(matches[0][0])
        gfx = int(matches[0][1], 0)
        if len(raw) != 40 or not 0 <= gfx <= 0xffffffff:
            raise ValueError('incomplete AP10/GFX1 word snapshot')
        # The reporting candidate is J714s little-endian ARM64, like the retained TEST dump.
        words[label] = list(struct.unpack('<10I', raw)) + [gfx]

    def ids(values):
        return [32 * word + bit for word, value in enumerate(values)
                for bit in range(32) if value & (1 << bit)]

    pending, masks = words['pending'], words['masks']
    return {'pending_words': pending, 'mask_words': masks,
            'raw_pending_ids': ids(pending),
            'enabled_pending_ids': ids([value & ~mask for value, mask in zip(pending, masks)]),
            'masked_pending_ids': ids([value & mask for value, mask in zip(pending, masks)]),
            'scope': 'sequential early-wake pending/mask observation; not cause, final-commit, GPIO, SMC or hold proof'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('row_kernel_log', type=Path)
    args = parser.parse_args()
    print(json.dumps(parse(args.row_kernel_log.read_text()), indent=2))
