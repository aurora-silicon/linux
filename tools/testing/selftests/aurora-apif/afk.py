#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production firmware-27 framing and command-tag helpers."""
from pathlib import Path
import re
import shutil
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - AFK v2 framing # SKIP cc unavailable")
        return 4
    header = (ROOT / "drivers/gpu/drm/apple/afkv2-codec.h").read_text()
    header = re.sub(r"^#include .*\n", "", header, flags=re.M)
    program = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define __packed __attribute__((packed))
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#else
#define cpu_to_le32(x) __builtin_bswap32(x)
#define le32_to_cpu(x) __builtin_bswap32(x)
#endif
"""
    program += header
    program += r"""
int main(void) {
 u8 raw[80] = {}, payload[32] = {};
 struct afkv2_frame frame;
 struct afkv2_command command;
 size_t written;
 assert(!afkv2_encode_open(raw, sizeof(raw), 7, 3, &written));
 assert(written == 24 && !afkv2_decode_frame(raw, written, false, &frame));
 assert(frame.interface == 3 && frame.type == AFKV2_OPEN && frame.payload_size == 0);
 assert(afkv2_decode_frame(raw, 23, false, &frame) == -EINVAL);
 afkv2_put32(raw + 4, UINT32_MAX);
 assert(afkv2_decode_frame(raw, 24, false, &frame) == -EINVAL);
 assert(!afkv2_encode_oob_command(payload, sizeof(payload), 0x7a,
                                 0x10000, 16, 0x20000, 32));
 assert(!afkv2_encode_frame(raw, sizeof(raw), 8, 3, 0xabc, 0xc0,
                           AFKV2_COMMAND, 0, payload, sizeof(payload), &written));
 assert(!afkv2_decode_frame(raw, written, false, &frame));
 assert(!afkv2_decode_command(frame.payload, frame.payload_size, &command));
 assert(command.id == 0x7a && command.rx_address == 0x10000 && command.tx_size == 32);
 assert(afkv2_decode_frame(raw, written + 1, false, &frame) == -EINVAL);
 assert(!afkv2_decode_frame(raw, written + 1, true, &frame));
 raw[17] = 3;
 assert(afkv2_decode_frame(raw, written, false, &frame) == -EPROTO);
 assert(afkv2_encode_oob_command(payload, sizeof(payload), 0, UINT64_MAX, 2, 0, 0) == -EINVAL);
 for (unsigned int generation = 0; generation < 256; generation++)
  for (unsigned int slot = 0; slot < 16; slot++)
   assert(afkv2_command_id(generation, slot) == (((generation & 15) << 4) | slot));
 assert(afkv2_owned_span(0x10000, 32, 0x10008, 16));
 assert(!afkv2_owned_span(0x10000, 32, 0x10008, 32));
 return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = Path(tmp) / "afk.c", Path(tmp) / "afk"
        source.write_text(program)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                       check=True)
        # stdbuf injects LD_PRELOAD into the runner, ahead of the ASan runtime.
        # Only this isolated sanitizer child needs a clean loader environment.
        env = {**os.environ, "LD_PRELOAD": ""}
        subprocess.run([str(binary)], env=env, check=True)
    print("ok 1 - AFK v2 framing and command tags")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
