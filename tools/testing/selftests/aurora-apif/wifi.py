#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production frame bounds, completion spans and key ownership."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from pmp import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - N1 Wi-Fi frame and payload ownership # SKIP cc unavailable")
        return 4
    source = (ROOT / "drivers/net/wireless/apple/centauri/centauri_wifi.c").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define DATA_TX_SLOTS 2048
#define ETH_HLEN 14
struct centauri_wifi { int unused; };
static const u16 ring_size[5] = {0, 64, 192, 96, 192};
static u16 tail;
static unsigned int barriers;
static u16 index_read(struct centauri_wifi *w, int a, int i) { return tail; }
static void dma_rmb(void) { barriers++; }
static u16 get_unaligned_le16(const void *p) {
    const u8 *v = p; return v[0] | v[1] << 8;
}
'''
    for name in ["data_rx_frame", "data_tx_completion_span", "command_payload_consumed", "link_signal_read"]:
        program += block(source, "^static bool " + name + r"\(")
    program += r'''
int main(void) {
    u8 packet[4096] = {0}; unsigned int offset, length, first; u16 count;
    assert(!data_rx_frame(packet, 4096, 100, 0, &offset, &length));
    assert(!data_rx_frame(packet, 4096, 4097, BIT(7), &offset, &length));
    assert(!data_rx_frame(packet, 4096, 13, BIT(7), &offset, &length));
    assert(data_rx_frame(packet, 4096, 100, BIT(7) | (4 << 12), &offset, &length));
    assert(offset == 4 && length == 96);
    packet[2] = 0xff; packet[3] = 0xff;
    assert(!data_rx_frame(packet, 4096, 100, BIT(7) | BIT(24), &offset, &length));
    packet[2] = 32; packet[3] = 0;
    assert(data_rx_frame(packet, 4096, 100, BIT(7) | BIT(24), &offset, &length));
    assert(offset == 32 && length == 100);
    assert(!data_tx_completion_span(0, 0, 0, &first, &count));
    assert(!data_tx_completion_span(2049, 0, 0, &first, &count));
    assert(!data_tx_completion_span(1, 2048, 1, &first, &count));
    assert(!data_tx_completion_span(1, 2, 0, &first, &count));
    assert(data_tx_completion_span(1, 2, 1, &first, &count));
    assert(first == 2047 && count == 2);
    assert(data_tx_completion_span(2048, 0, 0, &first, &count));
    assert(first == 2047 && count == 1);
    u8 status[160] = {0}; s8 signal;
    assert(!link_signal_read(status, 31, &signal));
    status[32] = 12; status[33] = 1; status[34] = 115;
    status[37] = (u8)-42;
    assert(link_signal_read(status, 151, &signal) && signal == -42);
    assert(!link_signal_read(status, 150, &signal));
    status[37] = 0;
    assert(!link_signal_read(status, 151, &signal));
    struct centauri_wifi w;
    tail = 63;
    assert(!command_payload_consumed(&w, 63) && !barriers);
    tail = 62;
    assert(!command_payload_consumed(&w, 63) && !barriers);
    tail = 0;
    assert(command_payload_consumed(&w, 63) && barriers == 1);
    assert(!command_payload_consumed(&w, 64) && barriers == 1);
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - N1 Wi-Fi frame and payload ownership")
        return 1
    print("ok 1 - N1 Wi-Fi frame and payload ownership")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
