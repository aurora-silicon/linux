#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# kernel-digest.py libagx_shaders.c: sha256 of the parts of the generated
# kernel file that an M3 (G15) device uses: every portable NIR array (*_nir)
# and the text outside the arrays (the program tables). The precompiled G13
# binaries (M1/M2 only) are left out: their info header carries uninitialised
# padding, so they differ from run to run.
import hashlib, re, sys
s = open(sys.argv[1]).read()
arr = re.compile(r'static const uint32_t (\w+)\[(\d+)\] = \{(.*?)\};', re.S)
h = hashlib.sha256()
n = 0
for m in arr.finditer(s):
    if m.group(1).endswith('_nir'):
        h.update(f'{m.group(1)}[{m.group(2)}]={"".join(m.group(3).split())}\n'.encode())
        n += 1
h.update(arr.sub('', s).encode())
print(h.hexdigest(), n)
