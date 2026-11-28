#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test drm/xe 16 KiB host-page fixes and IORESOURCE_MEM_NONPOSTED BAR handling."""

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


def extract_line(path, needle):
    for line in path.read_text().splitlines(keepends=True):
        if needle in line:
            return line
    raise RuntimeError(f"missing {needle} in {path}")


def mutate(code, name):
    if name == "ring-ctl-page-size":
        old = "((size) - SZ_4K)"
        new = "((size) - PAGE_SIZE)"
        assert old in code
        return code.replace(old, new, 1)
    if name == "guc-page-shift":
        old = ">> XE_PTE_SHIFT;"
        new = ">> PAGE_SHIFT;"
        assert old in code
        return code.replace(old, new, 1)
    if name == "emit-pte-page-size":
        old = "addr = xe_res_dma(cur) & ~(u64)XE_PTE_MASK;"
        new = "addr = xe_res_dma(cur) & PAGE_MASK;"
        assert old in code
        return code.replace(old, new, 1)
    if name == "query-min-align-4k":
        old = "xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K : PAGE_SIZE;"
        new = "xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K : SZ_4K;"
        assert old in code
        return code.replace(old, new, 1)
    if name == "drop-xe-nonposted-io":
        old = "if (pci_resource_flags(to_pci_dev(xe->drm.dev), LMEM_BAR) &\n\t\t    IORESOURCE_MEM_NONPOSTED) {"
        new = "if (0) {"
        assert old in code
        return code.replace(old, new, 1)
    if name == "drop-xe-fault-migrate":
        old = "if ((bo->flags & XE_BO_FLAG_SYSTEM) &&\n\t    !xe_bo_is_pinned(bo) &&\n\t    (pci_resource_flags(pdev, LMEM_BAR) & IORESOURCE_MEM_NONPOSTED))"
        new = "(void)pdev; if (0)"
        assert old in code
        return code.replace(old, new, 1)
    raise ValueError(f"unknown mutation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mutation", choices=(
        "ring-ctl-page-size",
        "guc-page-shift",
        "emit-pte-page-size",
        "query-min-align-4k",
        "drop-xe-nonposted-io",
        "drop-xe-fault-migrate",
    ))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    xe = ROOT / "drivers/gpu/drm/xe"
    ring_ctl = extract_line(xe / "regs/xe_engine_regs.h", "#define   RING_CTL_SIZE(")
    guc_ads_fn = extract_func(xe / "xe_guc.c", "static u32 guc_ctl_ads_flags(")
    emit_pte_fn = extract_func(xe / "xe_migrate.c", "static void emit_pte(")
    io_res_fn = extract_func(xe / "xe_bo.c", "static int xe_ttm_io_mem_reserve(")
    mig_fn = extract_func(xe / "xe_bo.c", "static bool should_migrate_to_smem(")
    qline = extract_line(xe / "xe_query.c", "xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K :")
    query_helper = (
        "static u64 test_query_min_alignment(struct xe_device *xe)\n{\n"
        "\treturn " + qline.strip() + "\n}\n"
    )

    code = (
        ring_ctl + "\n" +
        guc_ads_fn + "\n\n" +
        emit_pte_fn + "\n\n" +
        io_res_fn + "\n\n" +
        mig_fn + "\n\n" +
        query_helper + "\n"
    )
    if args.mutation:
        code = mutate(code, args.mutation)
    (args.out / "extracted.h").write_text(code)

    binary = args.out / "xe-16k-nonposted"
    fixture = Path(__file__).with_name("xe-16k-nonposted") / "fixture.c"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(args.out), str(fixture), "-o", str(binary)],
                   check=True)
    res = subprocess.run([str(binary)], check=False)
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
