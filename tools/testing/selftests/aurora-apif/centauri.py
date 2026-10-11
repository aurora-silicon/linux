#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the production FTAB parser and DMA shutdown predicate."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from pmp import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - N1 firmware and DMA ownership # SKIP cc unavailable")
        return 4
    ipc = (ROOT / "drivers/net/wireless/apple/centauri/centauri_ipc.c").read_text()
    pci = (ROOT / "drivers/net/wireless/apple/centauri/centauri_pci.c").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define SZ_64M (64 * 1024 * 1024)
#define PCI_COMMAND 4
#define PCI_COMMAND_MASTER 4
struct pci_dev { int dummy; };
static int config_error, pending_stopped = 1;
static u16 command;
static void pci_clear_master(struct pci_dev *p) {}
static int pci_read_config_word(struct pci_dev *p, int reg, u16 *v) {
    *v = command; return config_error;
}
static int pci_wait_for_pending_transaction(struct pci_dev *p) { return pending_stopped; }
static u32 get_unaligned_le32(const void *p) {
    const u8 *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
}
static void put32(u32 v, void *p) {
    u8 *b = p;
    for (int i = 0; i < 4; i++) b[i] = v >> (8 * i);
}
'''
    program += block(ipc, r"^static int ftab_entry\(")
    program += block(pci, r"^bool centauri_stop_dma\(")
    program += r'''
int main(void) {
    u8 image[256] = {0}; u32 off, len;
    memcpy(image + 0x24, "ftab", 4); put32(1, image + 0x28);
    memcpy(image + 0x30, "mswc", 4); put32(0x40, image + 0x34); put32(16, image + 0x38);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, false) == 0);
    assert(off == 0x40 && len == 16);
    assert(ftab_entry(image, 47, "mswc", &off, &len, false) == -EINVAL);
    assert(ftab_entry(image, sizeof(image), "none", &off, &len, false) == -ENOENT);
    put32(257, image + 0x28);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, false) == -EINVAL);
    put32(1, image + 0x28); put32(0x30, image + 0x34);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, false) == -EINVAL);
    put32(0x1000, image + 0x34);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, false) == -EINVAL);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, true) == 0);
    put32(SZ_64M, image + 0x38);
    assert(ftab_entry(image, sizeof(image), "mswc", &off, &len, true) == -EINVAL);
    struct pci_dev p;
    assert(centauri_stop_dma(&p));
    config_error = 1; assert(!centauri_stop_dma(&p));
    config_error = 0; command = PCI_COMMAND_MASTER; assert(!centauri_stop_dma(&p));
    command = 0; pending_stopped = 0; assert(!centauri_stop_dma(&p));
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - N1 firmware and DMA ownership")
        return 1
    print("ok 1 - N1 firmware and DMA ownership")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
