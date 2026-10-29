#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check QSPI/NOR support and migration refusal with an incomplete config."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("dtc"):
        print("ok 1 - guest config # SKIP dtc unavailable")
        return 4
    try:
        import libfdt
    except ImportError:
        print("ok 1 - guest config # SKIP Python libfdt unavailable")
        return 4
    tool = ROOT / "tools/aurora-apif/check_guest_config.py"
    spec = importlib.util.spec_from_file_location("check_guest_config", tool)
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    required = ["SPI", "SPI_MEM", "SPI_APPLE_QSPI", "MTD", "MTD_SPI_NOR", "MTD_OF_PARTS"]
    fragment = check.parse_config((ROOT / "arch/arm64/configs/aurora-apif.config").read_text())
    assert all(fragment.get("CONFIG_" + symbol) == "y" for symbol in required)
    source = r'''/dts-v1/;
/ {
 #address-cells=<2>; #size-cells=<2>;
 spi@62630000 {
  compatible="apple,t6050-qspi","apple,t8140-qspi";
  reg=<0 0x62630000 0 0x4000>; #address-cells=<1>; #size-cells=<0>;
  flash@0 {compatible="jedec,spi-nor"; reg=<0>;
   partitions {compatible="fixed-partitions";};};
 };
 iommu@61f34000 {compatible="apple,dart-x1n1";
  reg=<0 0x61f34000 0 0x4000>; #iommu-cells=<1>; apple,x1n1-dart-id=<12>;};
 disabled {status="disabled"; child {compatible="apple,sn201202x";};};
 opaque {compatible="example,not-in-map";};
};'''
    values = required + ["AURORA_APIF_MMIO", "APPLE_DART_APIF"]
    good = "\n".join("CONFIG_" + symbol + "=y" for symbol in values) + "\n"
    bad = good.replace("CONFIG_SPI_APPLE_QSPI=y\n", "CONFIG_SPI_APPLE=y\n")
    with tempfile.TemporaryDirectory(prefix="apif-config-") as temporary:
        tmp = Path(temporary)
        (tmp / "input.dts").write_text(source)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(tmp / "input.dtb"),
                        str(tmp / "input.dts")], check=True)
        blob = (tmp / "input.dtb").read_bytes()
        report = check.audit(blob, bad)
        assert report["missing"] == [{"node": "/spi@62630000", "symbol": "CONFIG_SPI_APPLE_QSPI", "value": "n"}]
        assert not check.validate(blob, good)["missing"]
        assert any(item["node"] == "/opaque" for item in report["unaudited"])
        modular = good.replace("CONFIG_SPI_APPLE_QSPI=y", "CONFIG_SPI_APPLE_QSPI=m")
        assert not check.audit(blob, modular)["missing"]
        assert check.audit(blob, modular, builtin=True)["missing"]
        assert check.audit(blob, good.replace("CONFIG_MTD_SPI_NOR=y\n", ""))["missing"]
        migration = ROOT / "tools/aurora-apif/prepare_guest.py"
        (tmp / "bad.config").write_text(bad)
        failed = subprocess.run([sys.executable, str(migration), str(tmp / "input.dtb"),
                                 str(tmp / "rejected.dtb"), "--kernel-config", str(tmp / "bad.config")],
                                capture_output=True, text=True)
        assert failed.returncode != 0 and "CONFIG_SPI_APPLE_QSPI" in failed.stderr
        assert not (tmp / "rejected.dtb").exists()
        (tmp / "good.config").write_text(good)
        subprocess.run([sys.executable, str(migration), str(tmp / "input.dtb"),
                        str(tmp / "accepted.dtb"), "--kernel-config", str(tmp / "good.config")], check=True)
        assert (tmp / "accepted.dtb").exists()
    print("ok 1 - QSPI/NOR dependencies, disabled parents, modules and migration config gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
