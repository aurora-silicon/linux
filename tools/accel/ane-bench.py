#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Collect ANE workload output, system power and idle recovery in one receipt."""
import argparse
import errno
import grp
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


def check_device(node):
    try:
        fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
    except OSError as exc:
        if exc.errno in (errno.EACCES, errno.EPERM):
            try:
                group = grp.getgrgid(Path(node).stat().st_gid).gr_name
            except (OSError, KeyError):
                group = "the device's owning group"
            raise RuntimeError(
                f"Permission denied opening {node}: request access through "
                f"the installed device-access rule or membership in {group}; "
                "start a new login after changing group membership."
            ) from exc
        raise RuntimeError(f"Cannot open {node}: {exc.strerror}") from exc
    else:
        os.close(fd)


def power_sensor():
    for hwmon in Path("/sys/class/hwmon").glob("hwmon*"):
        try:
            if (hwmon / "name").read_text().strip() != "macsmc_hwmon":
                continue
            for label in hwmon.glob("power*_label"):
                if label.read_text().strip().lower() == "total system power":
                    return label.with_name(label.name.replace("_label", "_input"))
        except OSError:
            continue
    raise RuntimeError("Total System Power sensor unavailable in macsmc_hwmon")


def snapshot(device):
    result = {}
    for name in ("performance_level", "memory_vote", "performance_error",
                 "power/runtime_status", "ane_stats"):
        try:
            result[name] = (device / name).read_text().strip()
        except OSError:
            result[name] = None
    return result


def sample_power(sensor, seconds=None, process=None):
    samples = []
    start = time.monotonic()
    while ((process is None and time.monotonic() - start < seconds) or
           (process is not None and process.poll() is None)):
        samples.append({"seconds": time.monotonic() - start,
                        "watts": int(sensor.read_text()) / 1_000_000})
        time.sleep(0.5)
    values = [s["watts"] for s in samples]
    return {"duration_s": time.monotonic() - start, "samples": samples,
            "mean_W": statistics.mean(values) if values else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/accel/accel0")
    parser.add_argument("--idle-seconds", type=float, default=10)
    parser.add_argument("--check", action="store_true", help="check device access only")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="-- followed by an installed ANE workload command")
    args = parser.parse_args()
    if args.idle_seconds <= 0:
        parser.error("--idle-seconds must be positive")
    try:
        check_device(args.device)
        if args.check:
            print(f"Access to {args.device}: OK")
            return 0
        command = args.command
        if command and command[0] == "--":
            command = command[1:]
        if not command:
            parser.error("supply a workload command after --, or use --check")
        sensor = power_sensor()
        time.sleep(3)  # Let the access check's runtime-PM reference expire.
        device = Path("/sys/class/accel") / Path(args.device).name / "device"
        receipt = {"schema": 1, "kernel": platform.release(), "command": command,
                   "power_sensor": str(sensor), "before": snapshot(device)}
        receipt["idle_before"] = sample_power(sensor, args.idle_seconds)
        # A file avoids a full stdout pipe stalling a long-running workload.
        with tempfile.TemporaryFile(mode="w+") as output:
            with subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT) as proc:
                try:
                    receipt["load"] = sample_power(sensor, process=proc)
                    receipt["returncode"] = proc.wait()
                except BaseException:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                    raise
            output.seek(0)
            receipt["workload_output"] = output.read()
        receipt["power_comparison_valid"] = len(receipt["load"]["samples"]) >= 6
        if not receipt["power_comparison_valid"]:
            receipt["power_note"] = "Fewer than 6 load samples; repeat with a longer workload."
        receipt["after_load"] = snapshot(device)
        time.sleep(3)  # Allow the default runtime-PM delay to expire.
        receipt["idle_after"] = sample_power(sensor, args.idle_seconds)
        receipt["after_idle"] = snapshot(device)
        print(json.dumps(receipt, indent=2))
        return 0 if receipt["returncode"] == 0 else 1
    except (RuntimeError, OSError) as exc:
        print(f"ane-bench: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
