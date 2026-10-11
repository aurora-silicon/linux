#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check production host GPIO IRQ-bank and aperture validation."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - GPIO IRQ ownership bounds # SKIP cc unavailable")
        return 4
    header = (ROOT / "drivers/pinctrl/pinctrl-apple-irq.h").read_text()
    header = re.sub(r"^#include .*\n", "", header, flags=re.M)
    program = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
"""
    program += header
    program += r"""
int main(void) {
 assert(!apple_gpio_irq_layout(128, 4, 3, 0x990));
 assert(apple_gpio_irq_layout(128, 4, 3, 0x98f) == -EINVAL);
 assert(!apple_gpio_irq_layout(512, 0, 7, 0x9c0));
 assert(apple_gpio_irq_layout(512, 0, 7, 0x9bf) == -EINVAL);
 assert(!apple_gpio_irq_layout(512, 6, 0, 0x800));
 assert(apple_gpio_irq_layout(512, 6, 0, 0x7ff) == -EINVAL);
 assert(apple_gpio_irq_layout(0, 0, 1, UINT64_MAX) == -EINVAL);
 assert(apple_gpio_irq_layout(513, 0, 1, UINT64_MAX) == -EINVAL);
 assert(apple_gpio_irq_layout(32, 7, 0, UINT64_MAX) == -EINVAL);
 assert(apple_gpio_irq_layout(32, 6, 2, UINT64_MAX) == -EINVAL);
 assert(apple_gpio_irq_layout(32, UINT32_MAX, 1, UINT64_MAX) == -EINVAL);
 assert(apple_gpio_irq_layout(32, 0, UINT32_MAX, UINT64_MAX) == -EINVAL);
 return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = Path(tmp) / "gpio.c", Path(tmp) / "gpio"
        source.write_text(program)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
    print("ok 1 - GPIO IRQ banks and apertures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
