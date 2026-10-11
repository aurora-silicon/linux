#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check production Bluetooth event fixup and shared-supplier readiness."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from pmp import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - N1 Bluetooth report and supplier bounds # SKIP cc unavailable")
        return 4
    bt = (ROOT / "drivers/bluetooth/btcentauri.c").read_text()
    pci = (ROOT / "drivers/net/wireless/apple/centauri/centauri_pci.c").read_text()
    header = (ROOT / "include/net/bluetooth/hci.h").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint16_t __le16;
typedef struct { u8 b[6]; } bdaddr_t;
#define __counted_by(x)
#define HCI_EVENT_HDR_SIZE 2
#define HCI_EV_LE_META 0x3e
#define HCI_EV_LE_EXT_ADV_REPORT 0x0d
#define LE_EXT_ADV_EVT_TYPE_MASK 0x7f
#define LE_EXT_ADV_LEGACY_PDU 0x10
#define LE_EXT_ADV_SCAN_RSP 0x08
#define LE_EXT_ADV_SCAN_IND 0x02
#define READ_ONCE(v) (v)
#define smp_load_acquire(p) (*(p))
struct pci_driver { int unused; };
static struct pci_driver centauri_driver, other_driver;
struct centauri { bool removing, ready; };
struct pci_dev { struct pci_driver *driver; struct centauri *data; };
static void *pci_get_drvdata(struct pci_dev *p) { return p->data; }
static u16 get_unaligned_le16(const void *p) {
    const u8 *v = p; return v[0] | v[1] << 8;
}
static void put_unaligned_le16(u16 v, void *p) { u8 *b = p; b[0] = v; b[1] = v >> 8; }
'''
    program += block(header, r"^struct hci_ev_le_ext_adv_info\b") + " __attribute__((packed));\n"
    program += block(bt, r"^static void cen_fixup_scan_response\(")
    program += block(pci, r"^bool apple_centauri_control_ready\(")
    program += r'''
int main(void) {
    u8 event[64] = {HCI_EV_LE_META, 0, HCI_EV_LE_EXT_ADV_REPORT, 1};
    struct hci_ev_le_ext_adv_info *info = (void *)(event + 4);
    size_t length = 4 + sizeof(*info) + 3;
    put_unaligned_le16(0x2518, &info->type); info->length = 3;
    event[length - 1] = 0xab;
    cen_fixup_scan_response(event, length);
    assert(get_unaligned_le16(&info->type) == 0x251a && event[length - 1] == 0xab);
    put_unaligned_le16(0x2508, &info->type);
    cen_fixup_scan_response(event, length);
    assert(get_unaligned_le16(&info->type) == 0x2508);
    put_unaligned_le16(0x2538, &info->type);
    cen_fixup_scan_response(event, length);
    assert(get_unaligned_le16(&info->type) == 0x2538);
    put_unaligned_le16(0x2518, &info->type); info->length = 4;
    cen_fixup_scan_response(event, length);
    assert(get_unaligned_le16(&info->type) == 0x2518);
    cen_fixup_scan_response(event, 3);
    cen_fixup_scan_response(event, 4);
    struct centauri c = {false, true};
    struct pci_dev p = {&centauri_driver, &c};
    assert(apple_centauri_control_ready(&p));
    c.ready = false; assert(!apple_centauri_control_ready(&p));
    c.ready = true; c.removing = true; assert(!apple_centauri_control_ready(&p));
    c.removing = false; p.driver = &other_driver; assert(!apple_centauri_control_ready(&p));
    p.driver = &centauri_driver; p.data = NULL; assert(!apple_centauri_control_ready(&p));
}
'''
    try:
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    except subprocess.CalledProcessError:
        print("not ok 1 - N1 Bluetooth report and supplier bounds")
        return 1
    print("ok 1 - N1 Bluetooth report and supplier bounds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
