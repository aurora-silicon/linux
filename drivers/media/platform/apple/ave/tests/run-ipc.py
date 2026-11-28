#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production AVE channel parser against a firmware that rewrites
the channel descriptors while the host reads them.

An optional argument (slots, addr, name or once) runs only that case after
the well-behaved one.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# python3 -I leaves the script directory off sys.path; run.py sits next to this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import declaration, function, source


def block(text, start):
    begin = text.index(start)
    return text[begin:text.index('\n};', begin) + 3] + '\n'


def main():
    here = Path(__file__).resolve().parent
    src = here.parent
    header = (src / 'ave.h').read_text()
    code = (here / 'shim.h').read_text()
    code += source((src / 'ave-regs.h').read_text())
    for name in ('AVE_IPC_CHANNEL_OFF', 'AVE_IPC_MAX_CHANNELS'):
        match = re.search(r'^#define ' + name + r'\s+[^\n]+', header, re.M)
        if match:
            code += match[0] + '\n'
    code += block(header, 'enum {\n\tAVE_RING_IO,')
    for name in ('ave_codec', 'ave_buf', 'ave_ring', 'ave_image', 'ave_opcode'):
        code += declaration(header, name)
    code += source((src / 'ave-layout.h').read_text())
    code += (here / 'ipc-boundary.h').read_text()
    code += block((src / 'ave-abi.c').read_text(),
                  'static const struct ave_fw_transport ave_transport_6070 =')
    text = (src / 'ave-ipc.c').read_text()
    code += block(text, 'static const struct {\n\tconst char *name;\n\tu32 dir;')
    for name in ('ave_fw_offset', 'ave_ring_id', 'ave_rings_overlap',
                 'ave_ipc_parse_channels'):
        code += function(text, name)
    code += (here / 'ipc-test.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ave-ipc-') as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        '-Wno-pointer-sign',
                        '-Wno-sign-compare', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')] + sys.argv[1:], check=True)


if __name__ == '__main__':
    main()
