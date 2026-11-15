#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Manual source-checkout regression for J713 OF-to-FDT policy mapping."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess
import tempfile


def block(source, name):
    match = re.search(r"^static [^\n]+\b" + name + r"\(", source, re.M)
    if not match:
        raise RuntimeError(f"Production function {name} not found")
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def build_dtb(source_root, output, relative_source, compiler):
    pp = output.with_suffix(".pp.dts")
    subprocess.run(compiler + [
        "-E", "-nostdinc", "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
        "-I", str(source_root / "scripts/dtc/include-prefixes"),
        "-I", str(source_root / "include"),
        str(source_root / relative_source), "-o", str(pp),
    ], check=True)
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(output), str(pp)], check=True)


def legacy_fixture(native, output):
    # Model the old loader ABI without changing the source policy or ancestry.
    # An actual captured legacy DTB can be supplied instead with --legacy-dtb.
    import libfdt
    fdt = libfdt.Fdt(native.read_bytes())
    offsets = []
    offset, depth = 0, 0
    while offset >= 0 and depth >= 0:
        prop = fdt.getprop(offset, "compatible", quiet=(libfdt.NOTFOUND,))
        if not isinstance(prop, int) and b"apple,t8132-pmgr-pwrstate\0" in bytes(prop):
            offsets.append(offset)
        offset, depth = fdt.next_node(offset, depth, quiet=(libfdt.NOTFOUND,))
    # Delete later nodes first, retaining validity of the earlier offsets.
    for offset in reversed(offsets):
        for name in ["status", "apple,firmware-owned", "apple,inherited-on"]:
            fdt.delprop(offset, name, quiet=(libfdt.NOTFOUND,))
    output.write_bytes(fdt.as_bytearray())


def run(args, output):
    source_root = args.source_root.resolve()
    path = source_root / "drivers/soc/apple/t8132-platform-handoff.c"
    production = path.read_text()
    policy = block(production, "t8132_pmgr_policy")
    helper = block(production, "t8132_fdt_node_offset")
    bad_lookup = "int parent = t8132_fdt_node_offset(data, pmgr);"
    if policy.count(bad_lookup) != 1:
        raise RuntimeError("Production PMGR lookup changed; update the negative control")
    # Restore the exact old failure: component used as absolute/alias path, with
    # a failed lookup then treated as an unknown parent for every child.
    negative = policy.replace(bad_lookup, "int parent = fdt_path_offset(data, pmgr->full_name);")
    guard = re.search(r"\n\t\tif \(parent < 0\) \{\n.*?\n\t\t\}\n", negative, re.S)
    if not guard:
        raise RuntimeError("Production parent-error guard changed; update the negative control")
    negative = negative[:guard.start()] + "\n" + negative[guard.end():]
    template = (Path(__file__).parent / "harness.c").read_text()
    compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
    env = dict(os.environ, TMPDIR=str(output))
    policy_dtb = output / "policy.dtb"
    native_dtb = output / "native.dtb"
    legacy_dtb = output / "legacy.dtb"
    if args.policy_dtb:
        shutil.copyfile(args.policy_dtb, policy_dtb)
    else:
        build_dtb(source_root, policy_dtb, "drivers/soc/apple/t8132-platform-data.dts", compiler)
    if args.native_dtb:
        shutil.copyfile(args.native_dtb, native_dtb)
    else:
        build_dtb(source_root, native_dtb, "arch/arm64/boot/dts/apple/t8132-j713.dts", compiler)
    if args.legacy_dtb:
        shutil.copyfile(args.legacy_dtb, legacy_dtb)
    else:
        legacy_fixture(native_dtb, legacy_dtb)
    results = {
        "source": str(path), "source_sha256": hashlib.sha256(production.encode()).hexdigest(),
        "legacy_fixture": str(args.legacy_dtb) if args.legacy_dtb else "synthesized from native DTB",
    }
    for mode, code in [("negative-control", negative), ("production", helper + "\n" + policy)]:
        c = output / (mode + ".c")
        binary = output / mode
        c.write_text(template.replace("/* PRODUCTION_CODE */", code))
        command = compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
        command += ["-DCORRECTED"] if mode == "production" else ["-Wno-unused-function"]
        subprocess.run(command + [str(c), "-lfdt", "-o", str(binary)], check=True, env=env)
        result = subprocess.run([str(binary), str(policy_dtb), str(legacy_dtb), str(native_dtb)],
                                capture_output=True, text=True, env=env)
        (output / (mode + ".txt")).write_text(result.stdout + result.stderr)
        results[mode] = {"exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
        print(f"{mode}: exit {result.returncode}\n{result.stdout}{result.stderr}", end="")
        expected = 0 if mode == "production" else 1
        if result.returncode != expected:
            raise RuntimeError(f"{mode} returned {result.returncode}, expected {expected}")
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--policy-dtb", type=Path, help="Existing embedded policy DTB; otherwise build from source")
    parser.add_argument("--native-dtb", type=Path, help="Existing native J713 DTB; otherwise build from source")
    parser.add_argument("--legacy-dtb", type=Path, help="Captured legacy live DTB; otherwise synthesize missing policy properties")
    parser.add_argument("--output-dir", type=Path, help="Preserve generated host sources, binaries and results")
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.output_dir:
        args.output_dir = args.output_dir.resolve()
        args.output_dir.mkdir(parents=True, exist_ok=True)
        run(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="m4-pmgr-policy-") as temp:
            run(args, Path(temp))


if __name__ == "__main__":
    main()
