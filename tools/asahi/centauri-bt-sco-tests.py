#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host unit tests and negative controls for Centauri Bluetooth HRB SCO tracking."""
import argparse
import os
from pathlib import Path
import re
import resource
import shlex
import signal
import subprocess
import tempfile


def extract_fn(source, name):
    match = re.search(
        r"^static (?:void|bool|int) " + re.escape(name) + r"\([^;]*?\)\n\{",
        source,
        re.M,
    )
    if not match:
        raise RuntimeError(f"missing production function: {name}")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define CEN_MAX_HRB_CONNS 16

struct cen_hrb_conn {
	bool valid;
	u16 handle;
	u8 bdaddr[6];
};

struct cen_bt {
	bool hrb_status_pending;
	u16 hrb_sync_pending_wire, hrb_sync_pending_orig;
	struct cen_hrb_conn hrb_conns[CEN_MAX_HRB_CONNS];
};
'''

TESTS = r'''
int main(void)
{
	struct cen_bt bt;
	const u8 addr_a[6] = { 0x7d, 0xc5, 0x79, 0x86, 0x77, 0x74 };
	const u8 addr_b[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	const u8 addr_c[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

	memset(&bt, 0, sizeof(bt));
	assert(!cen_hrb_has_handle(&bt, 0x000b));
	assert(!cen_hrb_has_bdaddr(&bt, addr_a));

	/* Track extended link handles with 12-bit handle masking. */
	cen_hrb_track_add(&bt, 0xf00b, addr_a);
	assert(cen_hrb_has_handle(&bt, 0x000b));
	assert(cen_hrb_has_handle(&bt, 0x200b));
	assert(cen_hrb_has_bdaddr(&bt, addr_a));
	assert(!cen_hrb_has_bdaddr(&bt, addr_b));

	/* Updating the same handle refreshes its address without duplicating slots. */
	cen_hrb_track_add(&bt, 0x000b, addr_b);
	assert(cen_hrb_has_handle(&bt, 0x000b));
	assert(!cen_hrb_has_bdaddr(&bt, addr_a));
	assert(cen_hrb_has_bdaddr(&bt, addr_b));

	/* Multiple concurrent extended connections and disconnect cleanup. */
	cen_hrb_track_add(&bt, 0x000c, addr_c);
	assert(cen_hrb_has_handle(&bt, 0x000c));
	assert(cen_hrb_has_bdaddr(&bt, addr_c));
	cen_hrb_track_del(&bt, 0x100b);
	assert(!cen_hrb_has_handle(&bt, 0x000b));
	assert(!cen_hrb_has_bdaddr(&bt, addr_b));
	assert(cen_hrb_has_handle(&bt, 0x000c));

	/* Reset clears tracked handles and pending status fields. */
	bt.hrb_status_pending = true;
	bt.hrb_sync_pending_wire = 0xfdc7;
	bt.hrb_sync_pending_orig = 0x0428;
	cen_hrb_clear(&bt);
	assert(!cen_hrb_has_handle(&bt, 0x000c));
	assert(!bt.hrb_status_pending);
	assert(bt.hrb_sync_pending_wire == 0 && bt.hrb_sync_pending_orig == 0);

	puts("PASS: Centauri Bluetooth HRB SCO tracking");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--source-root", type=Path)
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[2]
    c_src = (root / "drivers/bluetooth/btcentauri.c").read_text()
    t6050_src = (root / "drivers/bluetooth/btcentauri_t6050.c").read_text()

    # Verify both drivers define the same extended SCO/voice constants.
    for sym in (
        "CEN_OP_HRB_SETUP_SYNC_CONN 0xfdc7",
        "CEN_OP_HRB_ACCEPT_SYNC_CONN 0xfdf8",
        "CEN_OP_WRITE_VOICE_CONFIG 0xfe10",
        "CEN_VOICE_TRANSPARENT_16BIT 0x0063",
    ):
        assert sym in c_src, f"missing {sym} in btcentauri.c"
        assert sym in t6050_src, f"missing {sym} in btcentauri_t6050.c"

    prod = "\n\n".join(
        extract_fn(c_src, name)
        for name in (
            "cen_hrb_clear",
            "cen_hrb_track_add",
            "cen_hrb_track_del",
            "cen_hrb_has_handle",
            "cen_hrb_has_bdaddr",
        )
    )

    variants = [("bt-sco-production", PRELUDE + prod + TESTS, 0, None)]
    if args.negative_controls:
        controls = [
            (
                "missing-handle-mask",
                "handle &= 0x0fff;\n\tfor (i = 0; i < CEN_MAX_HRB_CONNS; i++) {\n\t\tif (bt->hrb_conns[i].valid && bt->hrb_conns[i].handle == handle)\n\t\t\treturn true;",
                "for (i = 0; i < CEN_MAX_HRB_CONNS; i++) {\n\t\tif (bt->hrb_conns[i].valid && bt->hrb_conns[i].handle == handle)\n\t\t\treturn true;",
                "cen_hrb_has_handle(&bt, 0x200b)",
            ),
            (
                "missing-clear-wire",
                "bt->hrb_sync_pending_wire = 0;",
                "/* omit clearing pending wire opcode */",
                "hrb_sync_pending_wire == 0",
            ),
        ]
        for name, old, new, expected_msg in controls:
            if old not in prod:
                raise RuntimeError(f"missing pattern for {name}")
            variants.append(
                (
                    name,
                    PRELUDE + prod.replace(old, new, 1) + TESTS,
                    -signal.SIGABRT,
                    expected_msg,
                )
            )

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="centauri-bt-sco-test-") as directory:
        out = args.out or Path(directory)
        out.mkdir(parents=True, exist_ok=True)
        for name, code, expected, msg in variants:
            harness, binary = out / (name + ".c"), out / name
            harness.write_text(code)
            subprocess.run(
                compiler
                + [
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(harness),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            (out / (name + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode != expected or (
                expected and ("Assertion" not in result.stderr or msg not in result.stderr)
            ):
                raise RuntimeError(
                    f"{name}: exit {result.returncode}, expected {expected}\n"
                    + result.stdout
                    + result.stderr
                )
            print(f"{name}: {'PASS' if expected == 0 else 'correctly rejected'}")


if __name__ == "__main__":
    main()
