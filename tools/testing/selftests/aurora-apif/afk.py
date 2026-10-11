#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production firmware-27 framing and command-tag helpers."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - AFK v2 framing # SKIP cc unavailable")
        return 4
    header = (ROOT / "drivers/gpu/drm/apple/afk-v2.h").read_text()
    header = re.sub(r"^#include .*\n", "", header, flags=re.M)
    program = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
 struct epic_hdr_v2 h = {.length = cpu_to_le32(16), .category = 0};
 size_t size = 99;
 assert(sizeof(h) == 24 && sizeof(struct epic_cmd_v2) == 32);
 assert(!afk_v2_payload_size(&h, 24, &size) && size == 0);
 h.length = cpu_to_le32(32);
 assert(!afk_v2_payload_size(&h, 48, &size) && size == 16);
 assert(afk_v2_payload_size(&h, 39, &size) == -EINVAL);
 h.length = cpu_to_le32(UINT32_MAX);
 assert(afk_v2_payload_size(&h, 48, &size) == -EINVAL);
 h.length = cpu_to_le32(15);
 assert(afk_v2_payload_size(&h, 48, &size) == -EINVAL);
 h.length = cpu_to_le32(16);
 assert(afk_v2_payload_size(&h, 23, &size) == -EINVAL);
 h.category = 3;
 assert(afk_v2_payload_size(&h, 24, &size) == -EINVAL);
 for (unsigned int generation = 0; generation < 256; generation++)
  for (unsigned int slot = 0; slot < 16; slot++) {
   u16 old = afk_command_tag(false, generation, slot);
   u16 v2 = afk_command_tag(true, generation, slot);
   assert(afk_command_slot(false, old) == slot);
   assert(afk_command_slot(true, v2) == slot);
   assert(old >> 8 == generation && v2 >> 4 == (generation & 15));
   assert(afk_command_tag(true, generation + 1, slot) != v2);
  }
 return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = Path(tmp) / "afk.c", Path(tmp) / "afk"
        source.write_text(program)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
    print("ok 1 - AFK v2 framing and command tags")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
