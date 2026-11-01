#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check sensor identities in a compiled J700 DTB without accessing hardware.

The SMC hwmon binding numbers temperature children in their flattened-tree
order. Looking only at a board's source misses inherited and merged children.
Set APPLE_J700_DTB or pass --dtb after building apple/t8140-j700.dtb.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ZONES = {
    "cpu-e0-thermal": "Te05",
    "cpu-e1-thermal": "Te0S",
    "cpu-p0-thermal": "Tp01",
    "cpu-p1-thermal": "Tp05",
    "cpu-p2-thermal": "Tp09",
    "cpu-p3-thermal": "Tp0D",
    "smc-tp0t-thermal": "Tp0t",
    "charger-thermal": "TCMz",
    "chassis1-thermal": "Ts0A",
    "chassis2-thermal": "Ts0I",
    "chassis3-thermal": "Ts0M",
    "smc-tp02-thermal": "Tp02",
    "smc-tp06-thermal": "Tp06",
    "smc-tp0e-thermal": "Tp0E",
    "smc-tpx1-thermal": "Tpx1",
    "smc-tpx3-thermal": "Tpx3",
}


class Tree:
    def __init__(self, path):
        self.path = path

    def read(self, *args):
        result = subprocess.run(["fdtget", *args[:2], str(self.path), *args[2:]],
                                check=True, capture_output=True, text=True)
        return result.stdout.strip()

    def strings(self, node, prop):
        return self.read("-t", "s", node, prop).split()

    def cells(self, node, prop):
        return [int(value, 16) for value in
                self.read("-t", "x", node, prop).split()]

    def children(self, node):
        result = subprocess.run(["fdtget", "-l", str(self.path), node],
                                check=True, capture_output=True, text=True)
        return result.stdout.split()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_policy(tree, zone):
    path = f"/thermal-zones/{zone}"
    if zone == "charger-thermal":
        temps = [80000, 83000, 88000, 92000, 95000]
    elif zone.startswith("chassis"):
        temps = [70000, 75000, 80000, 85000, 88000]
    else:
        temps = [85000, 88000, 92000, 95000, 98000]
    require(tree.cells(path, "polling-delay") == [100], "polling too slow")
    require(tree.cells(path, "polling-delay-passive") == [100], "passive polling too slow")
    require(tree.strings(path, "critical-action") == ["shutdown"], "wrong critical action")
    gpu_nodes = [f"/soc/{n}" for n in tree.children("/soc") if n.startswith("gpu@")]
    require(len(gpu_nodes) == 1, "expected one GPU")
    gpu = gpu_nodes[0]
    require(tree.cells(gpu, "#cooling-cells") == [2], "GPU cooling admission missing")
    gpu_phandle, = tree.cells(gpu, "phandle")
    for index, state in enumerate([25, 50, 75, 100]):
        trip = f"{path}/trips/cap-{index}"
        require(tree.cells(trip, "temperature") == [temps[index]], "wrong cap temperature")
        require(tree.cells(trip, "hysteresis") == [5000], "wrong cap hysteresis")
        require(tree.strings(trip, "type") == ["active"], "non-power-actor ladder required")
        phandle, = tree.cells(trip, "phandle")
        mapping = f"{path}/cooling-maps/map{index}"
        require(tree.cells(mapping, "trip") == [phandle], "wrong trip reference")
        require(tree.cells(mapping, "cooling-device") == [gpu_phandle, state, state],
                "wrong GPU or non-exact cap request")
    require(tree.cells(f"{path}/trips/trip-hot", "temperature") == [temps[2]],
            "wrong hot temperature")
    require(tree.strings(f"{path}/trips/trip-hot", "type") == ["hot"], "missing hot trip")
    require(tree.cells(f"{path}/trips/trip-hot", "hysteresis") == [5000],
            "wrong hot hysteresis")
    critical = f"{path}/trips/trip-critical"
    require(tree.cells(critical, "temperature") == [temps[4]], "wrong critical temperature")
    require(tree.strings(critical, "type") == ["critical"], "missing critical shutdown")
    require(tree.cells(critical, "hysteresis") == [0], "wrong critical hysteresis")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dtb", type=Path, default=os.environ.get("APPLE_J700_DTB"))
    args = parser.parse_args()
    print("TAP version 13")
    if args.dtb is None or shutil.which("fdtget") is None:
        print("1..0 # SKIP set APPLE_J700_DTB and install dtc/fdtget")
        return 4

    tree = Tree(args.dtb)
    print(f"1..{len(ZONES) + 1}")
    try:
        require("apple,j700" in tree.strings("/", "compatible"), "not a J700 DTB")
        # Find the SMC by its public node name rather than assuming its address.
        providers = [f"/soc/{node}/hwmon" for node in tree.children("/soc")
                     if node.startswith("smc@")]
        require(len(providers) == 1, "expected one SMC provider")
        provider = providers[0]
        require(tree.strings(provider, "compatible") == ["apple,smc-hwmon"],
                "wrong hwmon provider")
        require(tree.cells(provider, "#thermal-sensor-cells") == [1],
                "wrong sensor-cell count")
        phandle, = tree.cells(provider, "phandle")
        channels = [node for node in tree.children(provider)
                    if node.startswith("temperature-")]
        keys = [tree.strings(f"{provider}/{node}", "apple,key-id")
                for node in channels]
        require(all(key == [node.removeprefix("temperature-")]
                    for key, node in zip(keys, channels)), "sensor/key mismatch")
        require(len({key[0] for key in keys}) == len(keys), "duplicate sensor keys")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"not ok 1 - DTB setup: {str(error).splitlines()[0]}")
        for number, zone in enumerate(ZONES, 2):
            print(f"ok {number} - {zone} # SKIP DTB setup failed")
        return 1

    print("ok 1 - J700 indexed SMC temperature provider")
    failed = False
    for number, (zone, key) in enumerate(ZONES.items(), 2):
        try:
            reference = tree.cells(f"/thermal-zones/{zone}", "thermal-sensors")
            require(len(reference) == 2, "expected one sensor with one argument")
            owner, channel = reference
            require(owner == phandle, "wrong sensor provider")
            require(channel < len(channels), "sensor channel out of range")
            require(keys[channel] == [key], f"channel {channel} is {keys[channel]}, expected {key}")
            if not zone.startswith("cpu-"):
                check_policy(tree, zone)
            print(f"ok {number} - {zone} reads {key} with expected policy")
        except (ValueError, OSError, subprocess.CalledProcessError) as error:
            print(f"not ok {number} - {zone}: {str(error).splitlines()[0]}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
