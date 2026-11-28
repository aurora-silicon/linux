#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test nouveau non-posted BAR helpers, VRAM pushbuf I/O, and 16 KiB VMM DMA offsets."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def extract_func(path, declaration):
    lines = path.read_text().splitlines(keepends=True)
    s, = (i for i, l in enumerate(lines) if l.startswith(declaration))
    e = next(i for i in range(s + 1, len(lines)) if lines[i] == "}\n")
    return "".join(lines[s:e + 1])


def extract_block(path, start_marker, end_marker):
    lines = path.read_text().splitlines(keepends=True)
    s = next(i for i, l in enumerate(lines) if start_marker in l)
    e = next(i for i in range(s + 1, len(lines)) if end_marker in lines[i])
    return "".join(lines[s:e])


def mutate(code, name):
    if name == "old-page-mask":
        old = "\t\tmap->off  = map->offset & ~PAGE_MASK;"
        new = "\t\tmap->off  = map->offset & PAGE_MASK;"
        assert old in code, "vmm page mask target missing"
        return code.replace(old, new, 1)
    if name == "drop-addr-nonposted":
        old = "\t\tif (len && addr >= start && addr - start < len &&\n\t\t    size <= len - (addr - start))\n\t\t\treturn true;"
        new = "\t\t(void)start; (void)len; (void)addr; (void)size;"
        assert old in code, "addr nonposted target missing"
        return code.replace(old, new, 1)
    if name == "drop-nonposted-fault-migrate":
        old = "\t\tif (nvkm_device_bar_nonposted(device, NVKM_BAR1_FB) &&\n\t\t    !bo->pin_count &&\n\t\t    (nvbo->valid_domains & NOUVEAU_GEM_DOMAIN_GART)) {"
        new = "\t\tif (0) {"
        assert old in code, "fault migrate target missing"
        return code.replace(old, new, 1)
    if name == "drop-push-iomem":
        old = (
            "\tif (_p->is_iomem)                                        \\\n"
            "\t\tmemcpy_toio((void __force __iomem *)_p->cur, (d), _s << 2); \\\n"
            "\telse                                                     \\\n"
            "\t\tmemcpy(_p->cur, (d), _s << 2);                   \\"
        )
        new = "\tmemcpy(_p->cur, (d), _s << 2);                           \\"
        assert old in code, "push iomem target missing"
        return code.replace(old, new, 1)
    raise ValueError(f"unknown mutation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mutation", choices=(
        "old-page-mask",
        "drop-addr-nonposted",
        "drop-nonposted-fault-migrate",
        "drop-push-iomem",
    ))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    nv = ROOT / "drivers/gpu/drm/nouveau"
    dev_helpers = (
        extract_func(nv / "include/nvkm/core/device.h",
                     "nvkm_device_bar_nonposted(") + "\n" +
        extract_func(nv / "include/nvkm/core/device.h",
                     "nvkm_device_addr_nonposted(")
    )
    dev_helpers = (
        "static inline bool\n" + dev_helpers.replace(
            "\nnvkm_device_addr_nonposted(",
            "\nstatic inline bool\nnvkm_device_addr_nonposted(",
        )
    )
    fault_fn = extract_func(nv / "nouveau_bo.c",
                            "vm_fault_t nouveau_ttm_fault_reserve_notify(")
    vmm_lines = (nv / "nvkm/subdev/mmu/vmm.c").read_text().splitlines(keepends=True)
    idx = next(i for i, l in enumerate(vmm_lines) if "map->dma += map->offset >> PAGE_SHIFT;" in l)
    vmm_snippet = (
        "static void run_vmm_dma_offset(struct nvkm_vmm_map *map)\n{\n"
        + vmm_lines[idx]
        + vmm_lines[idx + 1]
        + "}\n"
    )
    push_hdr = extract_block(
        nv / "include/nvif/push.h",
        "#define PUSH_DATA__(",
        "#define PUSH_1(",
    )

    code = dev_helpers + "\n\n" + vmm_snippet + "\n\n" + fault_fn + "\n\n" + push_hdr + "\n"
    if args.mutation:
        code = mutate(code, args.mutation)
    (args.out / "extracted.h").write_text(code)

    binary = args.out / "nouveau-bar-16k"
    fixture = Path(__file__).with_name("nouveau-bar-16k") / "fixture.c"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(args.out), str(fixture), "-o", str(binary)],
                   check=True)
    res = subprocess.run([str(binary)], check=False)
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
