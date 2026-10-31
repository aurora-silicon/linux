#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run all six AVD conformance suites, preserving failures and vector annotations."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DECODERS = {"H.264": "GStreamer-H.264-V4L2SL", "H.265": "GStreamer-H.265-V4L2SL",
            "VP9": "GStreamer-VP9-V4L2SL", "AV1": "GStreamer-AV1-V4L2SL"}


def format_marks(codec, output_format):
    """Implementation limits, distinct from proven hardware limits."""
    marks = []
    if "444" in output_format:
        marks.append("driver-unsupported: 4:4:4")
    if "12" in output_format:
        marks.append("driver-unsupported: 12-bit")
    if output_format == "yuv422p10le":
        marks.append("driver-unsupported: P210 capture format is not exposed")
    if codec == "AV1" and output_format.startswith("gray"):
        marks.append("driver-unsupported: AV1 monochrome")
    return marks


def load_suites(directory, baseline):
    found = {}
    for path in sorted(directory.rglob("*.json")):
        suite = json.loads(path.read_text())
        name = suite.get("name")
        if name in baseline["suites"]:
            if name in found:
                raise ValueError(f"duplicate suite: {name}")
            found[name] = (path, suite)
    missing = set(baseline["suites"]) - found.keys()
    if missing:
        raise ValueError(f"missing suites: {', '.join(sorted(missing))}")
    for name, (_, suite) in found.items():
        expected = baseline["suites"][name]["total"]
        digest = baseline["suites"][name].get("catalogue_sha256")
        if digest and hashlib.sha256(found[name][0].read_bytes()).hexdigest() != digest:
            raise ValueError(f"{name}: catalogue hash changed; review baseline")
        if len(suite["test_vectors"]) != expected:
            raise ValueError(f"{name}: catalogue changed from {expected} vectors; review baseline")
    return found


def command(args, suite, codec, results):
    decoder = args.decoder.get(codec, DECODERS[codec])
    return [sys.executable, str(args.fluster / "fluster.py"),
            "--test-suites-dir", str(args.fluster / "test_suites"),
            "--resources", str(args.resources), "--output", str(results / suite / "decoded"),
            "run", "--jobs", str(args.jobs), "--timeout", str(args.timeout),
            "--testsuites", suite, "--decoders", decoder, "--keep", "--verbose",
            "--format", "csv", "--summary-output", str(results / suite / "results.csv")]


def probe(path, ffprobe, timeout):
    cmd = [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
           "stream=width,height,pix_fmt,field_order", "-of", "json", str(path)]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
        if result.returncode:
            return {"error": result.stderr.strip(), "rc": result.returncode}
        return json.loads(result.stdout)
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        return {"error": str(error)}


def annotations(args, suites, baseline, results):
    rows = []
    evidence = {}
    for name, (_, suite) in suites.items():
        for vector in suite["test_vectors"]:
            key = name + "/" + vector["name"]
            marks = format_marks(suite["codec"], vector["output_format"])
            marks += baseline.get("vector_marks", {}).get(key, {}).get("marks", [])
            state = "catalogue only; interlace, slice groups and size not inspected"
            if args.probe:
                path = args.resources / name / vector["name"] / vector["input_file"]
                info = probe(path, args.ffprobe, args.timeout)
                evidence[key] = info
                if "error" in info or not info.get("streams"):
                    state = "probe-error"
                else:
                    state = "first stream header inspected; later parameter sets and slice groups unclassified"
                    stream = info["streams"][0]
                    marks += format_marks(suite["codec"], stream.get("pix_fmt", ""))
                    if suite["codec"] == "H.264":
                        if stream.get("field_order") in ("tt", "bb", "tb", "bt"):
                            marks.append("hw-unsupported: H.264 interlace")
                        if max(stream.get("width", 0), stream.get("height", 0)) > 4096:
                            marks.append("hw-unsupported: H.264 above 4096x4096")
            rows.append([name, vector["name"], vector["output_format"],
                         "; ".join(sorted(set(marks))), state])
    with (results / "vectors.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["suite", "vector", "output_format", "unsupported_marks", "inspection"])
        writer.writerows(rows)
    if args.probe:
        (results / "probes.json").write_text(json.dumps(evidence, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fluster", type=Path, required=True, help="Fluster checkout")
    parser.add_argument("--resources", type=Path, required=True, help="already downloaded Fluster resources")
    parser.add_argument("--results", type=Path, required=True, help="new directory, never overwritten")
    parser.add_argument("--baseline", type=Path, default=HERE / "baseline.json")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=30, help="per-vector seconds")
    parser.add_argument("--decoder", action="append", default=[], metavar="CODEC=NAME")
    parser.add_argument("--probe", action="store_true", help="annotate local streams using ffprobe")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--plan", action="store_true", help="print commands without running tests")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("jobs and timeout must be positive")
    try:
        args.decoder = dict(item.split("=", 1) for item in args.decoder)
    except ValueError:
        parser.error("decoder must be CODEC=NAME")
    if args.decoder.keys() - DECODERS.keys():
        parser.error("unknown codec in decoder override")
    args.fluster = args.fluster.resolve()
    args.resources = args.resources.resolve()
    args.results = args.results.resolve()
    baseline = json.loads(args.baseline.read_text())
    suites = load_suites(args.fluster / "test_suites", baseline)
    if not (args.fluster / "fluster.py").is_file():
        parser.error("missing fluster.py")
    if args.plan:
        for name, (_, suite) in suites.items():
            print(shlex.join(command(args, name, suite["codec"], args.results)))
        return 0
    args.results.mkdir(parents=True, exist_ok=False)
    annotations(args, suites, baseline, args.results)
    manifest = {"baseline": baseline, "jobs": args.jobs, "timeout": args.timeout, "runs": []}
    failed = False
    for name, (path, suite) in suites.items():
        directory = args.results / name
        directory.mkdir()
        cmd = command(args, name, suite["codec"], args.results)
        print(shlex.join(cmd), flush=True)
        with (directory / "run.log").open("w") as log:
            try:
                rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=False).returncode
            except OSError as error:
                log.write(str(error) + "\n")
                rc = 127
        failed |= rc != 0
        manifest["runs"].append({"suite": name, "command": cmd, "rc": rc,
                                  "catalogue_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        # Persist after each suite, even when a decoder fails or times out.
        (args.results / "run.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
