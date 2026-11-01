#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Build an H13 recovery fixture from a pinned public MIT encoder (no device IO)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

COMMIT = "ef2dbceefedff38ecce4f3dc593e148c5e7a6d54"
GUARD = "chunks.size() != 1 || chunks.front() > 8"
DRIVER = r'''
#include "H13Program.h"
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char **argv) {
    using namespace ane::h13;
    ConvShape shape{1, 1, 2, 1, false, {128, 56, 56}, {256, 28, 28}};
    if (argc == 1) {
        auto why = convGenericRefusal(shape);
        std::cout << why << "\n";
        return why.find("8 interleaved lanes") == std::string::npos;
    }
    // Nonzero, exactly representable fp16 weights; no private compiler output.
    std::vector<std::uint8_t> weights(256 * 128 * 2);
    for (unsigned i = 0; i < weights.size() / 2; ++i) {
        const unsigned half = 0x2400 + (i * 17 % 0x400);
        weights[2 * i] = half; weights[2 * i + 1] = half >> 8;
    }
    auto bytes = encodeANEC(encodeConvGeneric(shape, weights.data(), weights.size(), nullptr, 0));
    std::ofstream(argv[1], std::ios::binary).write((const char *)bytes.data(), bytes.size());
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("compiler", type=Path, help="clean mil-hwx-compiler checkout at pinned PR 2 commit")
    p.add_argument("output", type=Path)
    p.add_argument("--cxx", default="c++")
    args = p.parse_args()
    source = args.compiler.resolve()
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if head != COMMIT:
        p.error(f"compiler HEAD must be {COMMIT}")
    directory = source / "plugins/H13"
    # Compare all encoder inputs with the licensed public commit. The bypass is
    # confined to a temporary translation unit; the checkout stays unmodified.
    for item in directory.iterdir():
        if item.suffix in (".cpp", ".h", ".inc"):
            original = subprocess.check_output(["git", "-C", str(source), "show", f"HEAD:plugins/H13/{item.name}"])
            if item.read_bytes() != original:
                p.error(f"modified encoder input: {item.name}")
    program = (directory / "H13Program.cpp").read_text()
    if program.count(GUARD) != 1:
        p.error("expected refusal changed")
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ane-hang-fixture-") as temporary:
        tmp = Path(temporary)
        (tmp / "fixture.cpp").write_text(DRIVER)
        def build(unit):
            subprocess.run([args.cxx, "-std=c++17", "-O2", "-I", str(directory),
                            str(unit), str(directory / "H13ANEC.cpp"), str(tmp / "fixture.cpp"),
                            "-o", str(tmp / "fixture")], check=True)
        build(directory / "H13Program.cpp")
        subprocess.run([str(tmp / "fixture")], check=True)  # normal compiler must refuse
        (tmp / "H13Program.cpp").write_text(program.replace(GUARD, "chunks.size() != 1"))
        build(tmp / "H13Program.cpp")
        subprocess.run([str(tmp / "fixture"), str(args.output)], check=True)
    receipt = {"compiler": COMMIT, "shape": "1x1 s2 128->256 56->28", "bias": False,
               "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
               "bytes": args.output.stat().st_size}
    args.output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
