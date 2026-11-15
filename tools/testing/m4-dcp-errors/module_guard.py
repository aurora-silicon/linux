# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Retained-DART feature preprocessing and optional production Kconfig matrix."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def config_block(source, name):
    start = source.index("config " + name + "\n")
    match = re.search(r"^config ", source[start + 1:], re.M)
    end = start + 1 + match.start() if match else len(source)
    return source[start:end]


def check_kconfig(source_root, output, conf):
    dart = (source_root / "drivers/iommu/Kconfig").read_text()
    drm = (source_root / "drivers/gpu/drm/apple/Kconfig").read_text()
    blocks = "\n".join(
        [config_block(dart, name) for name in ["APPLE_DART", "APPLE_DART_J713_DCP"]] +
        [config_block(drm, name) for name in ["DRM_APPLE", "DRM_APPLE_DCP_T8132"]])
    prefix = 'config MODULES\n\tbool "modules"\n\tmodules\n\tdefault y\n'
    for name, kind, value in [
            ("ARCH_APPLE", "bool", "y"), ("COMPILE_TEST", "bool", "n"),
            ("GENERIC_ATOMIC64", "bool", "n"), ("ARM64", "bool", "y"),
            ("ARM64_16K_PAGES", "bool", "y"), ("DRM", "tristate", "y"),
            ("OF", "bool", "y"), ("APPLE_RTKIT", "tristate", "y"),
            ("OF_ADDRESS", "bool", "y"), ("BACKLIGHT_CLASS_DEVICE", "tristate", "y"),
            ("APPLE_PMP", "tristate", "y")]:
        prefix += f'config {name}\n\t{kind} "{name}"\n\tdefault {value}\n'
    for name in sorted(set(re.findall(r"^\s*select\s+(\w+)", blocks, re.M))):
        prefix += f"config {name}\n\tbool\n"
    # The producer blocks are extracted unchanged. Only unrelated dependency
    # producers are synthetic; this is reachability, not full kernel Kconfig.
    cases = {
        "ordinary-module": ("m", "m", "y", "y", "y", "y", "n", "n"),
        "native-builtin": ("y", "y", "y", "y", "y", "y", "y", "y"),
        "native-modular-drm": ("y", "m", "y", "y", "y", "m", "y", "y"),
        "unsupported-page-size": ("y", "y", "y", "y", "n", "y", "n", "n"),
        "ordinary-builtin": ("y", "y", "n", "y", "y", "y", "n", "n"),
    }
    results = {}
    for name, values in cases.items():
        dart_mode, drm_mode, feature, dcp, pages, pmp, expected_feature, expected_dcp = values
        requested = {"APPLE_DART": dart_mode, "DRM_APPLE": drm_mode,
                     "APPLE_DART_J713_DCP": feature, "DRM_APPLE_DCP_T8132": dcp,
                     "ARM64_16K_PAGES": pages, "APPLE_PMP": pmp}
        expected = {"APPLE_DART": dart_mode, "DRM_APPLE": drm_mode,
                    "APPLE_DART_J713_DCP": expected_feature, "DRM_APPLE_DCP_T8132": expected_dcp}
        case_dir = output / name
        case_dir.mkdir(parents=True)
        kconfig = case_dir / "Kconfig"
        kconfig.write_text(prefix + "\n" + blocks)
        config = case_dir / ".config"
        config.write_text("CONFIG_MODULES=y\n" + "".join(
            "CONFIG_" + key + "=" + value + "\n" for key, value in requested.items()))
        env = dict(os.environ, KCONFIG_CONFIG=str(config))
        result = subprocess.run([str(conf), "--olddefconfig", str(kconfig)],
                                cwd=case_dir, env=env, capture_output=True, text=True)
        (case_dir / "result.txt").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Kconfig failed for {name}: {result.stderr}")
        actual = dict(re.findall(r"^CONFIG_(\w+)=(\w+)$", config.read_text(), re.M))
        passed = all(actual.get(symbol, "n") == value for symbol, value in expected.items())
        results[name] = {"requested": requested, "expected": expected,
                         "actual": {symbol: actual.get(symbol, "n") for symbol in expected},
                         "passed": passed}
        print(f"Kconfig {name}: {'PASS' if passed else 'FAIL'}", flush=True)
    return results


def check_module_guard(source_root, output, compiler, conf=None):
    source_root = Path(source_root)
    output.mkdir(parents=True, exist_ok=True)
    source = (source_root / "drivers/iommu/apple-dart.c").read_text()
    hashes = {"drivers/iommu/apple-dart.c": hashlib.sha256(source.encode()).hexdigest()}

    def expand_include(match):
        name = match[1]
        if name.startswith("apple-dart-dcp"):
            relative = Path("drivers/iommu") / name
            text = (source_root / relative).read_text()
            hashes[str(relative)] = hashlib.sha256(text.encode()).hexdigest()
            return text
        return ""

    # Expand the private implementation fragments inside their real guard.
    # Kernel header contents are unnecessary for conditional preprocessing.
    for unused in range(8):
        if not re.search(r"^\s*#include ", source, re.M):
            break
        source = re.sub(r'^\s*#include [<"]([^>"]+)[>"]\s*$',
                        expand_include, source, flags=re.M)
    c = output / "dart-module-input.c"
    c.write_text(source)
    command = compiler + ["-E", "-P", "-x", "c", "-DIS_ENABLED(x)=x", "-DIS_BUILTIN(x)=x",
                          "-DCONFIG_APPLE_DART_J713_DCP=0", "-DMODULE=1", str(c)]
    result = subprocess.run(command, capture_output=True, text=True)
    (output / "preprocess.log").write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f"DART feature preprocessing failed: {result.stderr}")
    (output / "dart-module-expanded.c").write_text(result.stdout)
    symbols = ["arch_dma_prep_coherent", "dma_common_pages_remap", "dma_pgprot"]
    references = {name: len(re.findall(r"\b" + name + r"\b", result.stdout)) for name in symbols}
    defconfig_path = "arch/arm64/configs/asahi.config"
    defconfig = (source_root / defconfig_path).read_text()
    hashes[defconfig_path] = hashlib.sha256(defconfig.encode()).hexdigest()
    native_opt_in = "CONFIG_APPLE_DART_J713_DCP=y\n" in defconfig
    summary = {"command": command, "source_hashes": hashes,
               "private_dma_references": references, "native_graphics_fragment_opt_in": native_opt_in,
               "passed": not any(references.values()) and native_opt_in}
    print(f"module-private-DMA guard: {'PASS' if summary['passed'] else 'FAIL'}", flush=True)
    if conf:
        summary["kconfig"] = check_kconfig(source_root, output / "kconfig", conf)
        summary["passed"] = summary["passed"] and all(
            case["passed"] for case in summary["kconfig"].values())
    else:
        summary["kconfig"] = {"skipped": True, "reason": "pass --kconfig-conf or set KCONFIG_CONF"}
        print("Kconfig dependency matrix: SKIP (no existing conf binary supplied)", flush=True)
    (output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary
