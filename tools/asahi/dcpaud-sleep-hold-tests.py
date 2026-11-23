#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual dcp-dp-audio late sleep callback over every DCP topology.

genpd powers the dp-audio domain off at noirq unless the device is on the
wakeup path, and cutting it under running DCP firmware resets the J473 on
s2idle. dcpaud_suspend_late() puts dp-audio on the wakeup path when the DCP
linked by its OF graph exists and is unbound or on its own wakeup path. The
fixture builds the real function and the real dcpaud_pm_ops (with the
kernel's PM_OPS macros) from drivers/gpu/drm/apple/audio.c against an OF
graph and driver-core model that counts every node and device reference.

--mutation names a change to the production function that the matrix must
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
PM = "include/linux/pm.h"

MUTATIONS = {
    # Hold only for a DCP on its wakeup path; an unbound DCP loses the domain.
    "bound-only": ("!device_is_bound(&dcp_pdev->dev) || device_wakeup_path(",
                   "device_wakeup_path("),
    # Leak the DCP device reference from of_find_device_by_node().
    "leak-device": ("\t\tput_device(&dcp_pdev->dev);\n", ""),
    # Leak the DCP node reference from of_graph_get_remote_port_parent().
    "leak-node": ("\tof_node_put(dcp_node);\n", ""),
    # Hold for a bound DCP that is NOT on its wakeup path.
    "invert-wakeup": ("|| device_wakeup_path(", "|| !device_wakeup_path("),
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
        audio = subprocess.check_output(["git", "show", f"{args.ref}:{AUDIO}"],
                                        cwd=ROOT, text=True)
    else:
        audio = (ROOT / AUDIO).read_text()
    pm = (ROOT / PM).read_text()
    late = function(audio, "dcpaud_suspend_late")
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        if not late or late.count(old) != 1:
            raise RuntimeError(f"mutation {args.mutation} did not apply exactly once")
        late = late.replace(old, new)

    macros = "".join(macro(pm, name) for name in
                     ("SYSTEM_SLEEP_PM_OPS", "LATE_SYSTEM_SLEEP_PM_OPS", "RUNTIME_PM_OPS"))
    code = (Path(__file__).with_suffix(".c").read_text()
            .replace("/* ACTUAL_PM_MACROS */", macros)
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
