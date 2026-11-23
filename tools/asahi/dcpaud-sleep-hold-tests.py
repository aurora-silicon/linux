#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual dcp-dp-audio late sleep callback over every DCP topology.

genpd powers the dp-audio domain off at noirq unless the device is on the
wakeup path, and cutting it under running DCP firmware resets the J473 on
s2idle. dcpaud_suspend_late() puts dp-audio on the wakeup path when the DCP
linked by its OF graph exists and is unbound or kept its firmware running
through the sleep (dcp_sleep_keeps_firmware()); a DCP that stopped its
firmware (S2R, the 27.x sleep, an M4 stop) leaves the domain to genpd. The
fixture builds the real function and the real dcpaud_pm_ops (with the
kernel's PM_OPS macros) from drivers/gpu/drm/apple/audio.c, and the real
dcp_sleep_keeps_firmware() from dcp.c, against an OF graph and driver-core
model that counts every node and device reference and checks that the
DCP's driver data is read under its device lock.

--mutation names a change to a production function that the matrix must
kill. --ref reads audio.c from an older revision: before the fix the late
slot was pm_runtime_force_suspend, which never holds the domain, so the
matrix fails (the old-code negative control).
"""
import argparse
import json
import os
import re
import resource
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUDIO = "drivers/gpu/drm/apple/audio.c"
DCP = "drivers/gpu/drm/apple/dcp.c"
PM = "include/linux/pm.h"

# (file, old, new): each must apply exactly once and the matrix must kill it.
MUTATIONS = {
    # Hold only for a bound DCP that kept its firmware; an unbound DCP loses the domain.
    "bound-only": ("dcp", "runs = !dev->driver || (dcp && dcp->sleep_fw_runs);",
                   "runs = dcp && dcp->sleep_fw_runs;"),
    # Hold for any bound DCP, also one that stopped its firmware for the sleep.
    "stopped-holds": ("dcp", "(dcp && dcp->sleep_fw_runs)", "(dcp != NULL)"),
    # Hold for a bound DCP that stopped its firmware, not for one that kept it.
    "invert-runs": ("dcp", "(dcp && dcp->sleep_fw_runs)", "(dcp && !dcp->sleep_fw_runs)"),
    # Read the driver data without the device lock.
    "no-lock": ("dcp", "\tdevice_lock(dev);\n", ""),
    # Leak the DCP device reference from of_find_device_by_node().
    "leak-device": ("audio", "\t\tput_device(&dcp_pdev->dev);\n", ""),
    # Leak the DCP node reference from of_graph_get_remote_port_parent().
    "leak-node": ("audio", "\tof_node_put(dcp_node);\n", ""),
    # Back to the wakeup-path test that also held for a stopped DCP.
    "ignore-dcp": ("audio", "if (dcp_sleep_keeps_firmware(&dcp_pdev->dev))", "if (true)"),
}


def block(text, start):
    begin = text.index("{", start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return end


def function(text, name):
    match = re.search(r"^static [^\n;]*\bint " + name + r"\(struct device \*dev\)\n\{",
                      text, re.M)
    if not match:
        return None
    return text[match.start():block(text, match.start())]


def dcp_function(text):
    match = re.search(r"^bool dcp_sleep_keeps_firmware\(struct device \*dev\)\n\{", text, re.M)
    if not match:
        return None
    return text[match.start():block(text, match.start())]


def pm_ops(text):
    start = text.index("static const struct dev_pm_ops dcpaud_pm_ops = {")
    return text[start:block(text, start)] + ";\n"


def macro(text, name):
    match = re.search(r"^#define " + name + r"\(.*?[^\\]\n", text, re.M | re.S)
    assert match, name
    return match.group()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--ref", help="read audio.c from this revision")
    parser.add_argument("--mutation", choices=sorted(MUTATIONS))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    if args.ref:
        audio, dcp = (subprocess.check_output(["git", "show", f"{args.ref}:{path}"],
                                              cwd=ROOT, text=True) for path in (AUDIO, DCP))
    else:
        audio, dcp = ((ROOT / path).read_text() for path in (AUDIO, DCP))
    pm = (ROOT / PM).read_text()
    late = function(audio, "dcpaud_suspend_late")
    keeps = dcp_function(dcp)
    if args.mutation:
        where, old, new = MUTATIONS[args.mutation]
        body = late if where == "audio" else keeps
        if not body or body.count(old) != 1:
            raise RuntimeError(f"mutation {args.mutation} did not apply exactly once")
        if where == "audio":
            late = late.replace(old, new)
        else:
            keeps = keeps.replace(old, new)

    macros = "".join(macro(pm, name) for name in
                     ("SYSTEM_SLEEP_PM_OPS", "LATE_SYSTEM_SLEEP_PM_OPS", "RUNTIME_PM_OPS"))
    code = (Path(__file__).with_suffix(".c").read_text()
            .replace("/* ACTUAL_PM_MACROS */", macros)
            .replace("/* ACTUAL_DCP_FUNCTION */", keeps or "")
            .replace("/* ACTUAL_FUNCTION */", late or "")
            .replace("/* ACTUAL_PM_OPS */", pm_ops(audio)))

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source, binary = out / "dcpaud-sleep-hold.c", out / "dcpaud-sleep-hold"
        source.write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-function",
                        "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all"]
                       + (["-DHAVE_SUSPEND_LATE"] if late else [])
                       + [str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / "results.json").write_text(json.dumps({
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "ref": args.ref,
            "mutation": args.mutation,
            "limits": "OF graph, driver core and runtime PM are host models; no genpd, no DCP.",
        }, indent=2) + "\n")
        print(result.stdout + result.stderr, end="")
        return result.returncode != 0

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.out:
        return run(args.out.resolve())
    with tempfile.TemporaryDirectory(prefix="dcpaud-sleep-hold-") as out:
        return run(Path(out))


if __name__ == "__main__":
    raise SystemExit(main())
