#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check migration boundaries, metadata preservation and failed handoffs."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("dtc"):
        print("ok 1 - guest migration # SKIP dtc unavailable")
        return 4
    try:
        import libfdt
    except ImportError:
        print("ok 1 - guest migration # SKIP Python libfdt unavailable")
        return 4
    spec = importlib.util.spec_from_file_location("prepare_guest", ROOT / "tools/aurora-apif/prepare_guest.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = r'''/dts-v1/;
/ {
    #address-cells = <2>; #size-cells = <2>;
    aliases { pmp-iommu = "/iommu@61f34000"; };
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        asq: queue@80000000 {reg=<0 0x80000000 0 0x4000>; no-map;};
        acq: queue@80004000 {reg=<0 0x80004000 0 0x4000>; no-map;};
        isq: queue@80008000 {reg=<0 0x80008000 0 0x4000>; no-map;};
        icq: queue@8000c000 {reg=<0 0x8000c000 0 0x4000>; no-map;};
    };
    dart: iommu@61f34000 {
        compatible="apple,dart-x1n1"; reg=<0 0x61f34000 0 0x4000>;
        #iommu-cells=<1>; apple,x1n1-dart-id=<12>;
        apple,x1n1-num-streams=<16>; apple,x1n1-retained-sids=<0 1>;
        apple,dma-range=<0x100 0 0x10 0>;
    };
    sart: sart@61f04000 {compatible="apple,sart-x1n1"; reg=<0 0x61f04000 0 0x4000>;};
    nvme@60400000 {
        compatible="apple,t6050-nvme-sptm";
        reg=<0 0x60400000 0 0x40000>,<0 0x61000000 0 0x4000>,<0 0x61f00000 0 0x4000>;
        reg-names="nvme","ans","sptm"; dma-coherent; asahi,rtkit-quiesced;
        memory-region=<&asq &acq &isq &icq>;
        memory-region-names="admin-sq","admin-cq","io-sq","io-cq";
        apple,sart=<&sart>;
    };
    pmp@300500000 {compatible="apple,t6050-pmp-v2"; iommus=<&dart 0>; status="disabled";};
    gpu {compatible="example,opaque-gpu"; unchanged=[de ad be ef];};
};'''
    with tempfile.TemporaryDirectory(prefix="apif-guest-") as directory:
        path = Path(directory)
        (path / "input.dts").write_text(source)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(path / "input.dtb"),
                        str(path / "input.dts")], check=True)
        blob = (path / "input.dtb").read_bytes()
        before = libfdt.Fdt(bytearray(blob))
        after = libfdt.Fdt(bytearray(module.migrate(blob)))
        assert module.strings(after, "/iommu-c", "compatible") == ["apple,dart-apif"]
        for old, new in [("apple,x1n1-retained-sids", "apple,apif-retained-sids"),
                         ("apple,dma-range", "apple,dma-range"), ("phandle", "phandle")]:
            assert module.property_bytes(before, "/iommu@61f34000", old) == module.property_bytes(after, "/iommu-c", new)
        assert module.property_bytes(after, "/iommu-c", "apple,apif-reserve-last-page") == b""
        assert module.strings(after, "/aliases", "pmp-iommu") == ["/iommu-c"]
        assert module.strings(after, "/pmp@300500000", "status") == ["disabled"]
        assert module.property_bytes(after, "/gpu", "unchanged") == module.property_bytes(before, "/gpu", "unchanged")
        assert len(module.cells(after, "/nvme@60400000", "reg")) == 8
        assert module.cells(after, "/apif@61ff0000", "reg") == [0, module.APIF_IPA, 0, module.PAGE_SIZE]
        for mutation in ["live", "duplicate", "collision"]:
            bad = libfdt.Fdt(bytearray(blob)); bad.resize(len(blob)+2048)
            if mutation == "live":
                module.delete(bad, "/nvme@60400000", "asahi,rtkit-quiesced")
            elif mutation == "duplicate":
                handles = module.cells(bad, "/nvme@60400000", "memory-region")
                handles[1] = handles[0]
                module.put_cells(bad, "/nvme@60400000", "memory-region", handles)
            else:
                bad.add_subnode(0, "device@61ff0000")
                module.put_cells(bad, "/device@61ff0000", "reg", [0, module.APIF_IPA, 0, 4])
            bad.pack()
            try:
                module.migrate(bytes(bad.as_bytearray()))
                raise AssertionError(f"accepted {mutation} handoff")
            except ValueError:
                pass
    print("ok 1 - checked guest migration and preserved producer metadata")
    return 0


if __name__ == "__main__":
    sys.exit(main())
