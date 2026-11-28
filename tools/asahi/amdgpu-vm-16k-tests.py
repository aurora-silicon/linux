#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test 16 KiB amdgpu_vm_update_range sub-page mapping and non-posted ROM/BAR0 ioremap."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def extract_block(path, start_marker, end_marker):
    lines = path.read_text().splitlines(keepends=True)
    s = next(i for i, line in enumerate(lines) if start_marker in line)
    e = next(i for i in range(s + 1, len(lines)) if end_marker in lines[i])
    return "".join(lines[s:e])


def extract_func(path, declaration):
    lines = path.read_text().splitlines(keepends=True)
    s, = (i for i, l in enumerate(lines) if l.startswith(declaration))
    e = next(i for i in range(s + 1, len(lines)) if lines[i] == "}\n")
    return "".join(lines[s:e + 1])


def mutate(code, name):
    if name == "drop-subpage-offset":
        old = "addr = pages_addr[cursor.start >> PAGE_SHIFT] +\n\t\t\t\t\t(cursor.start & ~PAGE_MASK);"
        new = "addr = pages_addr[cursor.start >> PAGE_SHIFT];"
        assert old in code, "subpage offset target missing"
        return code.replace(old, new, 1)
    if name == "drop-spanned-contiguity":
        old = (
            "if (sub_entries + num_entries >\n"
            "\t\t\t    AMDGPU_GPU_PAGES_IN_CPU_PAGE) {"
        )
        new = "if (num_entries > AMDGPU_GPU_PAGES_IN_CPU_PAGE) {"
        assert old in code, "spanned contiguity target missing"
        return code.replace(old, new, 1)
    if name == "drop-rom-nonposted":
        old = (
            "\tif (res->flags & IORESOURCE_MEM_NONPOSTED)\n"
            "\t\trom = ioremap_np(start, *size);\n"
            "\telse\n"
            "\t\trom = ioremap(start, *size);"
        )
        new = "\trom = ioremap(start, *size);"
        assert old in code, "rom nonposted target missing"
        return code.replace(old, new, 1)
    raise ValueError(f"unknown mutation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mutation", choices=("drop-subpage-offset",
                                               "drop-spanned-contiguity",
                                               "drop-rom-nonposted"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    vm_loop = extract_block(
        ROOT / "drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c",
        "\tamdgpu_res_first(pages_addr ? NULL : res, offset,",
        "\tr = vm->update_funcs->commit(&params, fence);",
    )
    rom_fn = extract_func(
        ROOT / "drivers/pci/rom.c",
        "void __iomem *pci_map_rom(",
    )
    bar0_fn = extract_func(
        ROOT / "drivers/gpu/drm/amd/amdgpu/amdgpu_device.c",
        "static int amdgpu_device_read_fb_via_bar0(",
    )

    code = (
        "static int run_vm_update_loop(struct amdgpu_vm_update_params *p_in,\n"
        "\t\t\t      struct amdgpu_device *adev,\n"
        "\t\t\t      struct ttm_resource *res,\n"
        "\t\t\t      dma_addr_t *pages_addr,\n"
        "\t\t\t      uint64_t offset, uint64_t start,\n"
        "\t\t\t      uint64_t last, uint64_t flags,\n"
        "\t\t\t      uint64_t vram_base)\n"
        "{\n"
        "\tstruct amdgpu_vm_update_params params = *p_in;\n"
        "\tstruct amdgpu_res_cursor cursor;\n"
        "\tint r = 0;\n"
        + vm_loop +
        "\treturn 0;\n"
        "error_free:\n"
        "\treturn r;\n"
        "}\n\n"
        + rom_fn + "\n\n"
        + bar0_fn + "\n"
    )
    if args.mutation:
        code = mutate(code, args.mutation)
    (args.out / "extracted.h").write_text(code)

    binary = args.out / "amdgpu-vm-16k"
    fixture = Path(__file__).with_name("amdgpu-vm-16k") / "fixture.c"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(args.out), str(fixture), "-o", str(binary)],
                   check=True)
    res = subprocess.run([str(binary)], check=False)
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
