#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the production OUTPUT picture address against the buffer behind it."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# python3 -I leaves the script directory off sys.path; run.py sits next to this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import declaration, function, source


def main():
    here = Path(__file__).resolve().parent
    src = here.parent
    header = (src / 'ave.h').read_text()
    code = (here / 'shim.h').read_text()
    code += (here / 'input-boundary.h').read_text()
    for name in ('ave_codec', 'ave_buf', 'ave_params', 'ave_slot'):
        code += declaration(header, name)
    code += (here / 'input-session.h').read_text()
    code += function((src / 'ave-fw.c').read_text(), 'ave_free')
    code += function((src / 'ave-fw.c').read_text(), 'ave_alloc_padded')
    code += function((src / 'ave-fw.c').read_text(), 'ave_alloc')
    sess = (src / 'ave-sess.c').read_text()
    code += function(sess, 'ave_note_copy')
    code += function(sess, 'ave_sess_input')
    code += (here / 'input-test.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ave-input-') as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    main()
