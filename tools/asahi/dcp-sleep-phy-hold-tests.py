#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual DCP sleep PHY hold over every PHY topology.

A DCP whose firmware keeps running through a sleep keeps driving the DP PHY
of its output. genpd powers the PHY's domain off at noirq unless the PHY's
device is on the wakeup path, and cutting it under the running firmware
resets the J473 (firmware 27.2) on s2idle. dcp_sleep_hold_phys() puts the
provider device of the DCP's own dp-phy and of its active Type-C route's PHY
on the wakeup path. The fixture builds the real dcp_phy_set_wakeup_path() and
dcp_sleep_hold_phys() from drivers/gpu/drm/apple/dcp-fabric.c against a small
driver-core model and checks which devices end up on the wakeup path.

--mutation names a change to the production functions that the matrix must
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
DCP = "drivers/gpu/drm/apple/dcp-fabric.c"

MUTATIONS = {
    # Forget the Type-C route's PHY (the USB-C output of a primary DCP).
    "no-route": ("\tif (route)\n\t\tdcp_phy_set_wakeup_path(route->phy);\n", ""),
    # Forget the DCP's own dp-phy (the HDMI output of a dcpext).
    "no-own-phy": ("\tdcp_phy_set_wakeup_path(dcp->phy);\n", ""),
    # Mark the PHY class device instead of the provider genpd sees.
    "class-device": ("device_set_wakeup_path(phy->dev.parent);", "device_set_wakeup_path(&phy->dev);"),
    # Trust an error pointer.
    "no-err-check": ("!IS_ERR_OR_NULL(phy) && ", "phy && "),
}

HARNESS = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static volatile int cur_line;
static bool dcp_typec_fabric_lock;
static bool *lock_acquire(bool *held)
{
	if (*held)
		abort();
	*held = true;
	return held;
}
static void lock_release(bool **held) { **held = false; }
#define guard(kind) bool *__attribute__((cleanup(lock_release))) held = lock_acquire
static void on_segv(int sig) { char b[64]; int n = snprintf(b, sizeof(b), "FAIL line %d (signal %d)\n", cur_line, sig); write(1, b, n); _exit(1); }
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline bool IS_ERR_OR_NULL(const void *p) { return !p || IS_ERR_VALUE((unsigned long)p); }
struct device { struct device *parent; bool wakeup_path; };
static void device_set_wakeup_path(struct device *dev)
{
	if (!dcp_typec_fabric_lock) {
		printf("FAIL line %d wakeup mark without fabric lock\n", cur_line);
		exit(1);
	}
	if (IS_ERR_VALUE((unsigned long)dev)) { printf("FAIL line %d error pointer\n", cur_line); exit(1); }
	dev->wakeup_path = true;
}
struct phy { struct device dev; };
struct apple_dcp_typec_route { struct phy *phy; };
struct apple_dcp { struct phy *phy; struct apple_dcp_typec_route *active_typec_route; };
@@FUNCS@@
int main(void)
{
	struct device own_prov = {0}, route_prov = {0}, other_prov = {0};
	struct phy own = { .dev = { .parent = &own_prov } };
	struct phy rphy = { .dev = { .parent = &route_prov } };
	struct phy orphan = { .dev = { .parent = NULL } };
	struct apple_dcp_typec_route route = { .phy = &rphy }, noroute_phy = { .phy = NULL };
	struct apple_dcp d;
	int fail = 0;

	signal(SIGSEGV, on_segv);
#define RESET() do { own_prov.wakeup_path = route_prov.wakeup_path = other_prov.wakeup_path = false; \
	own.dev.wakeup_path = rphy.dev.wakeup_path = orphan.dev.wakeup_path = false; } while (0)
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL line %d %s\n", __LINE__, name); fail++; } else printf("ok %s\n", name); } while (0)
#define HOLD(dcp) do { cur_line = __LINE__; fflush(stdout); dcp_sleep_hold_phys(dcp); } while (0)
	/* dcpext: its own dp-phy, no Type-C route */
	RESET(); d.phy = &own; d.active_typec_route = NULL; HOLD(&d);
	CHECK("dcpext dp-phy provider held", own_prov.wakeup_path && !own.dev.wakeup_path);
	/* primary with a Type-C route and no own phy */
	RESET(); d.phy = NULL; d.active_typec_route = &route; HOLD(&d);
	CHECK("route phy provider held", route_prov.wakeup_path && !rphy.dev.wakeup_path && !own_prov.wakeup_path);
	/* both */
	RESET(); d.phy = &own; d.active_typec_route = &route; HOLD(&d);
	CHECK("both held", own_prov.wakeup_path && route_prov.wakeup_path);
	/* nothing to hold */
	RESET(); d.phy = NULL; d.active_typec_route = NULL; HOLD(&d);
	CHECK("none", !own_prov.wakeup_path && !route_prov.wakeup_path && !other_prov.wakeup_path);
	/* an optional phy lookup error and a route without a phy */
	RESET(); d.phy = (struct phy *)(intptr_t)-19; d.active_typec_route = &noroute_phy; HOLD(&d);
	CHECK("error phy and phy-less route ignored", !own_prov.wakeup_path && !route_prov.wakeup_path);
	/* a phy without a provider parent */
	RESET(); d.phy = &orphan; d.active_typec_route = NULL; HOLD(&d);
	CHECK("parentless phy ignored", !orphan.dev.wakeup_path);
	printf("%d failures\n", fail);
	return fail != 0;
}
'''

def extract(src, name):
    m = re.search(r'^(?:static )?void %s\(.*?^}\n' % re.escape(name), src, re.S | re.M)
    if not m:
        sys.exit(f"{name} not found in {DCP}")
    return m.group(0)

def run(mutation):
    src = (ROOT / DCP).read_text()
    funcs = extract(src, "dcp_phy_set_wakeup_path") + "\n" + extract(src, "dcp_sleep_hold_phys")
    if mutation:
        old, new = MUTATIONS[mutation]
        if funcs.count(old) != 1:
            sys.exit(f"mutation {mutation} does not apply exactly once")
        funcs = funcs.replace(old, new)
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "t.c"
        exe = Path(tmp) / "t"
        c.write_text(HARNESS.replace("@@FUNCS@@", funcs))
        flags = ["-std=gnu11", "-Wall"] + ([] if mutation else ["-Werror"])
        subprocess.run([os.environ.get("CC", "cc"), *flags, "-o", exe, c], check=True)
        r = subprocess.run([exe], capture_output=True, text=True)
        out = r.stdout + r.stderr
        bad = r.returncode != 0
        return bad, out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutation", choices=sorted(MUTATIONS))
    a = ap.parse_args()
    bad, out = run(a.mutation)
    print(out, end="")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
