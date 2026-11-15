# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the unchanged production PMP headers with userspace I/O shims."""
import os
from pathlib import Path
import shlex
import subprocess


def build(root, out, source):
    shim = out / 'thermal-host-include' / 'linux'
    shim.mkdir(parents=True, exist_ok=True)
    headers = {
        'types.h': '#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n'
                   'typedef uint8_t u8; typedef uint32_t u32;\n'
                   'typedef int32_t s32; typedef int64_t s64;\n',
        'io.h': '#pragma once\n#include <string.h>\n#define __iomem\n'
                '#define memcpy_fromio memcpy\n',
        'kernel.h': '#pragma once\n#include <errno.h>\n#include <linux/types.h>\n'
                    '#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))\n'
                    '#define DIV_ROUND_CLOSEST(x, d) '
                    '(((x) < 0 ? (x) - (d) / 2 : (x) + (d) / 2) / (d))\n',
        'string.h': '#pragma once\n#include <string.h>\n',
        'unaligned.h': '#pragma once\n#include <linux/types.h>\n'
                       'static inline u32 get_unaligned_le32(const void *p) {\n'
                       ' const u8 *b = p; return (u32)b[0] | (u32)b[1] << 8 |\n'
                       ' (u32)b[2] << 16 | (u32)b[3] << 24; }\n',
    }
    for name, text in headers.items():
        (shim / name).write_text(text)
    binary = out / Path(source).stem
    command = shlex.split(os.environ.get('CC', 'cc'))
    command += ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(shim.parent), '-I', str(root / 'include'),
                str(source), '-o', str(binary)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    return binary
