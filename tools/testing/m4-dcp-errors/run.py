#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Manual source-checkout regressions for retained J713 display error paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

from harness import callback_unit, header_unit, irq_unit, pair_teardown_unit, pending_unit, unmap_unit
from module_guard import check_module_guard


SOURCES = {
    "dart": "drivers/iommu/apple-dart.c",
    "dcp": "drivers/iommu/apple-dart-dcp.h",
    "format": "drivers/iommu/apple-dart-dcp-format.h",
    "callback": "drivers/gpu/drm/apple/iomfb_m4.c",
    "callbacks": "drivers/gpu/drm/apple/dcp-m4-callback.h",
    "header": "include/linux/soc/apple/dart.h",
}


def run_source(source_root, output, compiler, negative=False):
    source = {key: (source_root / path).read_text() for key, path in SOURCES.items()}
    cases = {
        "v2-irq": irq_unit(source["dart"], source["dcp"]),
        "v2-admission": pending_unit(source["dart"]),
        "shared-irq-teardown": pair_teardown_unit(source["dart"]),
        "quarantined-unmap": unmap_unit(source["dcp"], source["format"]),
        "async-callback": callback_unit(source["callback"], source["callbacks"]),
        "module-public-stubs": header_unit(source["header"]),
    }
    result = {"source_hashes": {
        path: hashlib.sha256(source[key].encode()).hexdigest()
        for key, path in SOURCES.items()
    }, "cases": {}}
    output.mkdir(parents=True, exist_ok=True)
    for name, unit in cases.items():
        c = output / (name + ".c")
        binary = output / name
        c.write_text(unit)
        command = compiler + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-fsanitize=undefined", "-fno-sanitize-recover=all", str(c), "-o", str(binary),
        ]
        build = subprocess.run(command, capture_output=True, text=True)
        (output / (name + "-compile.txt")).write_text(build.stdout + build.stderr)
        case = {"command": command, "compile_exit": build.returncode}
        if build.returncode == 0:
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            (output / (name + "-result.txt")).write_text(run.stdout + run.stderr)
            case["run_exit"] = run.returncode
        passed = case["compile_exit"] == 0 and case.get("run_exit") == 0
        case["expected_failure"] = negative
        case["passed"] = not passed if negative else passed
        result["cases"][name] = case
        print(f"{'negative' if negative else 'production'} {name}: "
              f"{'PASS' if case['passed'] else 'FAIL'}", flush=True)
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def run_negative_controls(source_root, output, compiler):
    dart=(source_root / SOURCES['dart']).read_text()
    dcp=(source_root / SOURCES['dcp']).read_text()
    irq=irq_unit(dart,dcp)
    peer=pair_teardown_unit(dart)
    pending=pending_unit(dart)
    controls=[
        ('display-quarantine',irq,'apple_dart_quarantine_borrowed(dart);','(void)dart;',2),
        ('radio-quarantine',irq,'apple_dart_managed_quarantine(dart);','(void)dart;',2),
        ('display-no-unlock',irq,' && !apple_dart_has_borrowed(dart)','',1),
        ('ordinary-unlock',irq,'if (ret == IRQ_HANDLED && !dart->managed_sids','if (false && !dart->managed_sids',1),
        ('shared-peer-quarantine',peer,'apple_dart_quarantine_borrowed(apple_dart_display_irq_peers[i]);','(void)0;',1),
        ('unmasked-admission',pending,'readl(dart->regs + DART_T8110_ERROR_MASK)','0',1),
    ]
    output.mkdir(parents=True)
    results={}
    for name,unit,needle,replacement,count in controls:
        assert unit.count(needle)==count,(name,unit.count(needle))
        c=output/(name+'.c');binary=output/name
        c.write_text(unit.replace(needle,replacement))
        command=compiler+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-Wno-unused-parameter','-Wno-unused-variable','-Wno-unused-but-set-variable',
            '-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(binary)]
        build=subprocess.run(command,capture_output=True,text=True)
        (output/(name+'-compile.txt')).write_text(build.stdout+build.stderr)
        case=dict(command=command,compile_exit=build.returncode,passed=False)
        if build.returncode==0:
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            (output/(name+'-result.txt')).write_text(run.stdout+run.stderr)
            case.update(run_exit=run.returncode,passed=run.returncode!=0)
        results[name]=case
        print(f"mutation {name}: {'DETECTED' if case['passed'] else 'FAIL'}",flush=True)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    return results


def run(args, output):
    compiler = shlex.split(args.cc)
    if not compiler or not shutil.which(compiler[0]):
        raise RuntimeError("HOSTCC/--cc must identify an available host compiler")
    results = {"source_root": str(args.source_root), "compiler": compiler}
    results["production"] = run_source(args.source_root, output / "production", compiler)
    results["module_guard"] = check_module_guard(
        args.source_root, output / "module-guard", compiler, args.kconfig_conf)
    results["mutations"] = run_negative_controls(args.source_root, output / "mutations", compiler)
    if args.negative_source_root:
        results["negative"] = run_source(
            args.negative_source_root, output / "negative", compiler, negative=True)
    passed = all(case["passed"] for case in results["production"]["cases"].values())
    passed = passed and results["module_guard"]["passed"]
    passed = passed and all(case["passed"] for case in results["mutations"].values())
    if "negative" in results:
        passed = passed and all(case["passed"] for case in results["negative"]["cases"].values())
    results["passed"] = passed
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"Artifacts: {output}", flush=True)
    if not passed:
        raise RuntimeError("One or more source regressions failed; inspect result/compile logs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output-dir", type=Path, help="Preserve generated C, binaries, commands and logs")
    parser.add_argument("--cc", default=os.environ.get("HOSTCC", "cc"), help="Host C compiler command")
    parser.add_argument("--kconfig-conf", type=Path, default=os.environ.get("KCONFIG_CONF"),
                        help="Existing host scripts/kconfig/conf binary; enables the optional dependency matrix")
    parser.add_argument("--negative-source-root", type=Path,
                        help="Optional old source snapshot that must fail every regression control")
    args = parser.parse_args()
    args.source_root = args.source_root.resolve()
    if args.negative_source_root:
        args.negative_source_root = args.negative_source_root.resolve()
    if args.kconfig_conf:
        args.kconfig_conf = args.kconfig_conf.resolve()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
        run(args, output)
    else:
        with tempfile.TemporaryDirectory(prefix="m4-dcp-errors-") as directory:
            run(args, Path(directory))


if __name__ == "__main__":
    main()
