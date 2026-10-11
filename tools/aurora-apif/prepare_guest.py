#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Migrate a boot-producer DTB to native APIF; never infer hardware ownership."""
import argparse
from pathlib import Path
import struct

import libfdt

APIF_IPA = 0x61FF0000
PAGE_SIZE = 0x4000
LEGACY_DART = "apple,dart-x1n1"
LEGACY_SART = "apple,sart-x1n1"
LEGACY_NVME = "apple,t6050-nvme-sptm"
LEGACY_PCIE = "apple,x1n1-pcie"


def property_bytes(tree, path, name):
    value = tree.getprop(tree.path_offset(path), name, quiet=(libfdt.NOTFOUND,))
    return None if isinstance(value, int) else bytes(value)


def strings(tree, path, name):
    data = property_bytes(tree, path, name)
    if data is None:
        return []
    if not data.endswith(b"\0"):
        raise ValueError(f"{path}: malformed {name}")
    return data[:-1].decode("ascii").split("\0")


def cells(tree, path, name):
    data = property_bytes(tree, path, name)
    if data is None or len(data) % 4:
        raise ValueError(f"{path}: missing or malformed {name}")
    return list(struct.unpack(f">{len(data) // 4}I", data))


def number(tree, path, name):
    values = cells(tree, path, name)
    if len(values) != 1:
        raise ValueError(f"{path}: {name} must be one cell")
    return values[0]


def paths(tree):
    offset, depth = 0, 0
    while offset >= 0 and depth >= 0:
        yield tree.get_path(offset)
        offset, depth = tree.next_node(offset, depth, quiet=(libfdt.NOTFOUND,))


def put(tree, path, name, value):
    tree.setprop(tree.path_offset(path), name, value)


def put_cells(tree, path, name, values):
    put(tree, path, name, struct.pack(f">{len(values)}I", *values))


def put_strings(tree, path, name, values):
    put(tree, path, name, b"\0".join(v.encode("ascii") for v in values) + b"\0")


def delete(tree, path, name):
    tree.delprop(tree.path_offset(path), name, quiet=(libfdt.NOTFOUND,))


def reservation(tree, phandle):
    offset = tree.node_offset_by_phandle(phandle)
    path = tree.get_path(offset)
    parent = path.rsplit("/", 1)[0]
    if parent != "/reserved-memory" or property_bytes(tree, parent, "ranges") != b"" or property_bytes(tree, path, "no-map") is None:
        raise ValueError(f"{path}: queue page must be no-map reserved memory")
    ac, sc = number(tree, parent, "#address-cells"), number(tree, parent, "#size-cells")
    if ac not in (1, 2) or sc not in (1, 2):
        raise ValueError("unsupported reserved-memory cell widths")
    reg = cells(tree, path, "reg")
    if len(reg) != ac + sc:
        raise ValueError(f"{path}: queue page must have exactly one extent")
    address = int.from_bytes(struct.pack(f">{ac}I", *reg[:ac]), "big")
    size = int.from_bytes(struct.pack(f">{sc}I", *reg[ac:]), "big")
    if address % PAGE_SIZE or size != PAGE_SIZE:
        raise ValueError(f"{path}: queue page must be aligned and 16 KiB sized")
    return address


def nvme_handoff(tree, path):
    if property_bytes(tree, path, "asahi,rtkit-quiesced") is None:
        raise ValueError(f"{path}: producer must quiesce RTKit before migration")
    if property_bytes(tree, path, "dma-coherent") is None:
        raise ValueError(f"{path}: coherent handoff required")
    names = strings(tree, path, "memory-region-names")
    if names != ["admin-sq", "admin-cq", "io-sq", "io-cq"]:
        raise ValueError(f"{path}: four named pinned queue pages required")
    handles = cells(tree, path, "memory-region")
    if len(handles) != 4 or len(set(reservation(tree, h) for h in handles)) != 4:
        raise ValueError(f"{path}: queue reservations must be distinct")


def pci_layout(tree, path, layout):
    if property_bytes(tree, path, "apple,preinitialized") is None:
        raise ValueError(f"{path}: stopped preinitialized producer handoff required")
    if layout != "t6050-gen3-x1":
        raise ValueError(f"{path}: explicitly select the producer's PCIe layout")
    if strings(tree, path, "reg-names") != ["ecam", "port", "phy", "perst", "intr2axi"]:
        raise ValueError(f"{path}: matched PCIe register resources required")
    if number(tree, path, "max-link-speed") != 3 or number(tree, path, "num-lanes") != 1:
        raise ValueError(f"{path}: only the matched Gen3 x1 layout supports port cycling")
    routes = cells(tree, path, "apple,rid-to-sid")
    if not routes or len(routes) % 3 or len(routes) > 45:
        raise ValueError(f"{path}: explicit RID-to-SID tuples required")
    return ["apple,t6050-pcie-apif", "apple,apif-pcie"]


def migrate(blob, pcie_layout=None, enable_pmp=False):
    tree = libfdt.Fdt(bytearray(blob))
    tree.resize(len(blob) * 2 + 16384)
    inventory = list(paths(tree))
    compatible = {p: strings(tree, p, "compatible") for p in inventory}
    targets = [p for p, c in compatible.items() if any(v in c for v in
               [LEGACY_DART, LEGACY_SART, LEGACY_NVME, LEGACY_PCIE])]
    # Reject direct CPU-space collisions; buses without ranges are not CPU
    # address spaces. PCI child BAR encodings remain owned by the producer.
    for path in inventory:
        if path == "/" or property_bytes(tree, path, "reg") is None:
            continue
        parent = path.rsplit("/", 1)[0] or "/"
        pa, ps = property_bytes(tree, parent, "#address-cells"), property_bytes(tree, parent, "#size-cells")
        if pa is None or ps is None:
            continue
        ca, cs = number(tree, parent, "#address-cells"), number(tree, parent, "#size-cells")
        if ca not in (1, 2) or cs not in (1, 2):
            continue
        # Identity CPU buses are common in generated guest descriptions.
        bus = parent
        while bus != "/" and property_bytes(tree, bus, "ranges") == b"":
            bus = bus.rsplit("/", 1)[0] or "/"
        if bus != "/":
            continue
        reg = cells(tree, path, "reg")
        if len(reg) % (ca + cs):
            raise ValueError(f"{path}: malformed CPU register extents")
        for i in range(0, len(reg), ca + cs):
            address = int.from_bytes(struct.pack(f">{ca}I", *reg[i:i+ca]), "big")
            size = int.from_bytes(struct.pack(f">{cs}I", *reg[i+ca:i+ca+cs]), "big")
            if size and address < APIF_IPA + PAGE_SIZE and APIF_IPA < address + size:
                raise ValueError(f"{path}: native APIF page overlaps a CPU resource")
    if not targets:
        raise ValueError("no version-1 providers or consumers to migrate")
    # The native page uses the root's CPU physical address space.
    ac, sc = number(tree, "/", "#address-cells"), number(tree, "/", "#size-cells")
    if ac not in (1, 2) or sc not in (1, 2):
        raise ValueError("unsupported root address/size cell widths")
    handles = {}
    for path in inventory:
        for name in ["phandle", "linux,phandle"]:
            data = property_bytes(tree, path, name)
            if data is not None:
                handle = number(tree, path, name)
                if not handle or handle == 0xFFFFFFFF or (handle in handles and handles[handle] != path):
                    raise ValueError("invalid or duplicate phandle")
                handles[handle] = path
    handle = max(handles, default=0) + 1
    if handle >= 0xFFFFFFFF:
        raise ValueError("no available phandle")
    if any("aurora,apif-mmio" in c for c in compatible.values()):
        raise ValueError("tree already contains native APIF; do not migrate it twice")
    apif = "/apif@61ff0000"
    tree.add_subnode(0, apif[1:])
    put_strings(tree, apif, "compatible", ["aurora,apif-mmio"])
    put_cells(tree, apif, "phandle", [handle])
    put(tree, apif, "reg", APIF_IPA.to_bytes(4 * ac, "big") + PAGE_SIZE.to_bytes(4 * sc, "big"))
    renamed = {}
    for path in targets:
        c = compatible[path]
        if LEGACY_DART in c:
            unit = number(tree, path, "apple,x1n1-dart-id")
            if unit > 255:
                raise ValueError(f"{path}: invalid native DART unit")
            put_strings(tree, path, "compatible", ["apple,dart-apif"])
            for old, new in [("dart-id", "dart-id"), ("num-streams", "num-streams"),
                             ("iova-force", "iova-force"), ("retained-sids", "retained-sids")]:
                data = property_bytes(tree, path, "apple,x1n1-" + old)
                if data is not None:
                    put(tree, path, "apple,apif-" + new, data)
                    delete(tree, path, "apple,x1n1-" + old)
            delete(tree, path, "apple,x1n1-selftest")
            count = number(tree, path, "apple,apif-num-streams") if property_bytes(tree, path, "apple,apif-num-streams") else 256
            if not 1 <= count <= 256:
                raise ValueError(f"{path}: invalid stream count")
            retained = cells(tree, path, "apple,apif-retained-sids") if property_bytes(tree, path, "apple,apif-retained-sids") else []
            if len(retained) != len(set(retained)) or any(sid >= count for sid in retained):
                raise ValueError(f"{path}: invalid retained streams")
            renamed[path] = path.rsplit("/", 1)[0] + f"/iommu-{unit:x}"
            delete(tree, path, "reg")
        elif LEGACY_SART in c:
            put_strings(tree, path, "compatible", ["apple,sart-apif"])
            renamed[path] = path.rsplit("/", 1)[0] + "/sart"
            delete(tree, path, "reg")
        elif LEGACY_NVME in c:
            nvme_handoff(tree, path)
            put_strings(tree, path, "compatible", ["apple,nvme-apif"])
            # Version-1 has a third transport window; native APIF is a phandle.
            names = strings(tree, path, "reg-names")
            reg = cells(tree, path, "reg")
            parent = path.rsplit("/", 1)[0] or "/"
            width = number(tree, parent, "#address-cells") + number(tree, parent, "#size-cells")
            if names != ["nvme", "ans", "sptm"] or len(reg) != 3 * width:
                raise ValueError(f"{path}: expected version-1 NVMe resources")
            put_cells(tree, path, "reg", reg[:2 * width])
            put_strings(tree, path, "reg-names", ["nvme", "ans"])
        else:
            put_strings(tree, path, "compatible", pci_layout(tree, path, pcie_layout))
        if LEGACY_PCIE not in c:
            put_cells(tree, path, "aurora,apif", [handle])
    # Identify the legacy RTC layout only on matched T6050 guests.
    if "apple,t6050" in compatible.get("/", []):
        for path, c in compatible.items():
            if "apple,pmu-rtc" in c:
                names = strings(tree, path, "nvmem-cell-names")
                if names != ["counter", "rtc_offset", "alarm", "alarm_ctrl", "irq_mask"]:
                    raise ValueError(f"{path}: unknown PMU RTC cell layout")
                put_strings(tree, path, "compatible", ["apple,abbey-pmu-rtc"])
    # The old driver applied a hidden guard to the matched PMP consumer.
    # Record that existing ABI rule explicitly on its provider instead.
    for path, c in compatible.items():
        if "apple,t6050-pmp-v2" not in c:
            continue
        iommu = cells(tree, path, "iommus")
        if len(iommu) != 2 or iommu[0] not in handles:
            raise ValueError(f"{path}: one described PMP DART stream required")
        provider = handles[iommu[0]]
        if strings(tree, provider, "compatible") != ["apple,dart-apif"]:
            raise ValueError(f"{path}: PMP requires its APIF DART provider")
        put(tree, provider, "apple,apif-reserve-last-page", b"")
        if enable_pmp:
            put_strings(tree, path, "status", ["okay"])
    # Retain every provider phandle, and repair standard path references.
    if len(set(renamed.values())) != len(renamed):
        raise ValueError("provider names would collide")
    for old, new in renamed.items():
        if new in inventory and new != old:
            raise ValueError(f"{new}: provider name collision")
        tree.set_name(tree.path_offset(old), new.rsplit("/", 1)[1])
    for metadata in ["/aliases", "/__symbols__"]:
        offset = tree.path_offset(metadata, quiet=(libfdt.NOTFOUND,))
        if offset < 0:
            continue
        props = []
        po = tree.first_property_offset(offset, quiet=(libfdt.NOTFOUND,))
        while po >= 0:
            prop = tree.get_property_by_offset(po)
            props.append((prop.name, bytes(prop)))
            po = tree.next_property_offset(po, quiet=(libfdt.NOTFOUND,))
        for name, data in props:
            for old, new in renamed.items():
                if data == old.encode() + b"\0":
                    put_strings(tree, metadata, name, [new])
    tree.pack()
    return bytes(tree.as_bytearray())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pcie-layout", choices=["t6050-gen3-x1"])
    parser.add_argument("--enable-pmp", action="store_true")
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve() or args.output.exists():
        parser.error("output must be a new file; preserve the producer's original DTB")
    try:
        output = migrate(args.input.read_bytes(), args.pcie_layout, args.enable_pmp)
    except (ValueError, libfdt.FdtException) as error:
        parser.error(str(error))
    with args.output.open("xb") as stream:
        stream.write(output)


if __name__ == "__main__":
    main()
