#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the right USB4 port's DP IN resources in the compiled T6050 trees."""

import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BOARDS = ("t6050-j714s", "t6050-k114s", "t6050-k114c")
DP_INS = range(4)


def read(dtb, node, prop, kind="s", default=None):
    options = ["-d", default] if default is not None else []
    result = subprocess.run(["fdtget", "-t", kind, *options, dtb, node, prop],
                            capture_output=True, text=True)
    assert result.returncode == 0, f"{dtb.name}: {node}/{prop}: {result.stderr.strip()}"
    return result.stdout.split()


def symbol(dtb, name):
    path, = read(dtb, "/__symbols__", name)
    return path


def check(dtb):
    phy = symbol(dtb, "atc2_phy")
    router = symbol(dtb, "acio2")
    for node in (phy, router):
        assert read(dtb, node, "status", default="okay") in (["okay"], ["ok"]), f"{dtb.name}: {node} is disabled"

    names = read(dtb, phy, "reg-names")
    regs = read(dtb, phy, "reg", "x")
    assert len(regs) == 4 * len(names), f"{dtb.name}: ATC2 register/name count mismatch"
    assert names.count("dp-clock-source") == 1, f"{dtb.name}: missing or duplicate DP clock source"
    clock = 4 * names.index("dp-clock-source")
    assert tuple(int(word, 16) for word in regs[clock:clock + 4]) == (2, 0x80000068, 0, 16), (
        f"{dtb.name}: expected four DP IN clock source registers"
    )

    domains = read(dtb, phy, "power-domains", "x")
    names = read(dtb, phy, "power-domain-names")
    assert len(domains) == len(names), f"{dtb.name}: ATC2 domain/name count mismatch"
    for dpin in DP_INS:
        name = f"dp{dpin}"
        provider = symbol(dtb, f"ps_atc2_common_dp_{dpin}")
        assert names.count(name) == 1, f"{dtb.name}: missing or duplicate {name} power domain"
        assert [domains[names.index(name)]] == read(dtb, provider, "phandle", "x"), (
            f"{dtb.name}: {name} points at the wrong power provider"
        )
        assert read(dtb, provider, "status") == ["okay"], f"{dtb.name}: {name} provider is disabled"

    assert read(dtb, router, "interrupt-names") == [f"dpin{i}" for i in DP_INS], (
        f"{dtb.name}: ATC2 DP IN interrupt names changed"
    )
    # AIC type, die, IRQ number, trigger. Each slot must wake its own DP IN.
    interrupts = tuple(int(word) for word in read(dtb, router, "interrupts", "u"))
    assert interrupts == tuple(cell for irq in (1667, 1668, 1673, 1674)
                               for cell in (0, 0, irq, 4)), f"{dtb.name}: wrong ATC2 DP IN IRQs"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dtc", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    for board in BOARDS:
        source = ROOT / "arch/arm64/boot/dts/apple" / f"{board}.dts"
        dtb = args.output / f"{board}.dtb"
        text = subprocess.check_output([
            "cpp", "-nostdinc", "-undef", "-D__DTS__", "-I", str(ROOT / "scripts/dtc/include-prefixes"),
            "-x", "assembler-with-cpp", str(source),
        ])
        subprocess.run([args.dtc, "-@", "-q", "-I", "dts", "-O", "dtb", "-o", dtb], input=text, check=True)
        check(dtb)
        print(f"{board}: four DP IN clocks, power domains and interrupts", flush=True)


if __name__ == "__main__":
    main()
