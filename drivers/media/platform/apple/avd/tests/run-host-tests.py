#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile production CPU code with DMA/V4L2 test doubles, then run with ASan/UBSan.
The UAPI definitions come from this checkout; git show also supports sparse trees.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parent

def without_includes(text):
    return re.sub(r'^#include .*\n', '', text, flags=re.M)

uapi = subprocess.check_output(['git', '-C', str(DRIVER), 'show',
                               'HEAD:include/uapi/linux/v4l2-controls.h'], text=True)
uapi = uapi.split('/* Stateless AV1 controls */')[1].split('/* MPEG-compression')[0]
with tempfile.TemporaryDirectory(prefix='avd-av1-test-') as tmp:
    out = Path(tmp)
    source = (HERE / 'host-shim.h').read_text() + uapi
    source += without_includes((DRIVER / 'avd-av1-fp.h').read_text())
    source += (HERE / 'host-driver.h').read_text()
    source += without_includes((DRIVER / 'avd-queue.h').read_text())
    # Compile the production transport selector, not a duplicate test rule.
    header = (DRIVER / 'avd.h').read_text()
    source += re.search(r'static inline const struct avd_codec_frame_ops \*\n'
                        r'avd_get_frame_ops.*?\n}', header, re.S).group(0) + '\n'

    inst = without_includes((DRIVER / 'avd-inst.h').read_text())
    inst = inst[:inst.index('static inline void push(')] + '\n#endif\n'
    source += inst
    source += without_includes((DRIVER / 'avd-av1-fp.c').read_text())
    source += without_includes((DRIVER / 'avd-av1.c').read_text())
    # Freeze the pre-conversion arithmetic/packing as an independent oracle.
    legacy = (HERE / 'avd-av1-legacy.c').read_text()
    legacy = legacy[legacy.index('static const short div_lut'):legacy.index('/* End of packing oracle,')]
    names = ['div_lut', 'avd_av1_dec_get_msb', 'avd_av1_dec_resolve_divisor_32',
             'avd_av1_dec_get_shear_params', 'set_refs', 'avd_av1_get_dist',
             'is_valid_ref_frame', 'avd_select_refs', 'set_ref_hints', 'set_ref_hdr',
             'calc_upscale', 'set_header', 'set_tiles']
    for name in names:
        legacy = re.sub(r'\b' + name + r'\b', 'legacy_' + name, legacy)
    legacy = legacy.replace('struct avd_av1_ctx *av1_ctx = ctx->priv;', '')
    legacy = legacy.replace('av1_ctx->bufs.', 'run->bufs.')
    legacy = legacy.replace('run->bufs.probs.addr', 'run->addresses.probs_in')
    legacy = re.sub(r'av1_ctx->submit_num = .*?;', '', legacy)
    legacy = legacy.replace('push_comp(ctx, ', 'test_legacy_comp(ctx, ')
    source += '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wpointer-sign\"\n'
    source += (HERE / 'legacy-emitter.h').read_text() + legacy
    source += '#pragma GCC diagnostic pop\n'
    source += (HERE / 'host-test.c').read_text()
    (out / 'test.c').write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-Werror', str(out / 'test.c'),
                    '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
