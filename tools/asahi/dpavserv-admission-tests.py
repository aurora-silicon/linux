#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual DP AV service admission check over the output states.

A DCP takes a DP AV service only while the attachment it was announced for
is current. Every sleep and DPMS cycle moves the DCP's generation on; a
Type-C output then waits for its cable again. A fixed output that no Type-C
display has borrowed (the M2 Mac mini's HDMI port) has no Type-C cable: once
the generation had moved, its converter's AV service was refused for good,
so HDMI lost its EDID and stayed dark after the first sleep. The fixture
builds the real dcpavserv_admission_current() from epic/dpavservep.c and the
real dcp_is_typec_output() from dcp-fabric.c against a small model.

--mutation names a change to the production function that the matrix must
kill.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DPAV = "drivers/gpu/drm/apple/epic/dpavservep.c"
FABRIC = "drivers/gpu/drm/apple/dcp-fabric.c"

MUTATIONS = {
    # The old check: a fixed output waits for a Type-C cable it never gets.
    "old-cable-wait": (DPAV, " || !dcp_is_typec_output(dcp))", ")"),
    # Admit a stale announcement from before a sleep.
    "ignore-generation": (DPAV, "generation == dcp->typec_generation &&", ""),
    # Admit on a borrowed (Type-C) route without its cable.
    "ignore-cable": (DPAV, "dcp->typec_cable_connected ||", "true ||"),
    # Admit while the DCP is quiescing.
    "ignore-quiescing": (DPAV, "!READ_ONCE(dcp->quiescing) && ", ""),
}

HARNESS = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define lockdep_assert_held(x) ((void)(x))
#define DRM_MODE_CONNECTOR_HDMIA 11
#define DRM_MODE_CONNECTOR_USB 20
struct apple_dcp {
	int hpd_mutex;
	bool quiescing, external_physically_absent, typec_cable_connected;
	u64 typec_generation;
	void *active_typec_route;
	int fixed_connector_type;
};
@@FUNCS@@
int main(void)
{
	int route, fail = 0;
	struct apple_dcp d;
#define CASE(name, expect, ...) do { d = (struct apple_dcp){ __VA_ARGS__ }; \
	bool got = dcpavserv_admission_current(&d, d.typec_generation); \
	if (got != (expect)) { printf("FAIL line %d %s: got %d\n", __LINE__, name, got); fail++; } \
	else printf("ok %s\n", name); } while (0)
#define STALE(name, ...) do { d = (struct apple_dcp){ __VA_ARGS__ }; \
	bool got = dcpavserv_admission_current(&d, d.typec_generation - 1); \
	if (got) { printf("FAIL line %d %s: stale admitted\n", __LINE__, name); fail++; } \
	else printf("ok %s\n", name); } while (0)
	CASE("hdmi at boot", true, .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA);
	CASE("hdmi after a sleep", true, .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA, .typec_generation = 3);
	STALE("hdmi stale announcement", .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA, .typec_generation = 3);
	CASE("hdmi quiescing", false, .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA, .typec_generation = 3, .quiescing = true);
	CASE("hdmi physically absent", false, .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA, .external_physically_absent = true);
	CASE("typec before first attach", true, .active_typec_route = &route);
	CASE("typec unplugged", false, .active_typec_route = &route, .typec_generation = 2);
	CASE("typec plugged", true, .active_typec_route = &route, .typec_generation = 2, .typec_cable_connected = true);
	STALE("typec stale announcement", .active_typec_route = &route, .typec_generation = 2, .typec_cable_connected = true);
	CASE("hdmi borrowed by typec, unplugged", false, .fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA, .active_typec_route = &route, .typec_generation = 2);
	CASE("fixed usb-c output unplugged", false, .fixed_connector_type = DRM_MODE_CONNECTOR_USB, .typec_generation = 2);
	printf("%d failures\n", fail);
	return fail != 0;
}
'''

def extract(path, sig):
    src = (ROOT / path).read_text()
    m = re.search(r'^%s\(struct apple_dcp \*dcp[^)]*\)\n\{.*?^\}\n' % re.escape(sig), src, re.S | re.M)
    if not m:
        sys.exit(f"{sig} not found in {path}")
    return path, m.group(0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutation", choices=sorted(MUTATIONS))
    a = ap.parse_args()
    parts = [extract(FABRIC, "bool dcp_is_typec_output"),
             extract(DPAV, "static bool dcpavserv_admission_current")]
    code = []
    for path, text in parts:
        if a.mutation and MUTATIONS[a.mutation][0] == path:
            old, new = MUTATIONS[a.mutation][1:]
            if text.count(old) != 1:
                sys.exit(f"mutation {a.mutation} does not apply exactly once")
            text = text.replace(old, new)
        code.append(text)
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "t.c"
        exe = Path(tmp) / "t"
        c.write_text(HARNESS.replace("@@FUNCS@@", "\n".join(code)))
        flags = ["-std=gnu11", "-Wall"] + ([] if a.mutation else ["-Werror"])
        subprocess.run([os.environ.get("CC", "cc"), *flags, "-o", exe, c], check=True)
        r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout + r.stderr, end="")
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
