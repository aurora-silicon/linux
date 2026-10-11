#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual reserved-region allocation and DART PTE permission code."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[5]


def function(source, name):
    start = source.index("static dart_iopte " + name)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    compiler = os.environ.get("CC", "cc")
    if shutil.which(compiler) is None:
        print("SKIP: a host C compiler is required")
        return 4
    iommu = (ROOT / "include/linux/iommu.h").read_text()
    dart = (ROOT / "drivers/iommu/io-pgtable-dart.c").read_text()
    flags = "\n".join(line for line in iommu.splitlines()
                      if re.match(r"#define IOMMU_(READ|WRITE|CACHE)\s", line))
    flags += "\n" + "\n".join(line for line in dart.splitlines()
                              if re.match(r"#define APPLE_DART[12]_PTE_PROT_", line))
    enum = re.search(r"enum iommu_resv_type\s*\{.*?\n\};", iommu, re.S).group()
    fmt = re.search(r"enum io_pgtable_fmt\s*\{.*?\n\};",
                    (ROOT / "include/linux/io-pgtable.h").read_text(), re.S).group()
    header = json.dumps(str(ROOT / "drivers/iommu/of_iommu_firmware.h"))
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libfdt.h>
#define BIT(n) (1UL << (n))
#define GFP_KERNEL 0
typedef uint64_t phys_addr_t;
typedef uint64_t dart_iopte;
FLAGS
ENUM
FMT
struct iommu_resv_region { phys_addr_t phys, iova; size_t length; int prot; enum iommu_resv_type type; };
static struct iommu_resv_region recorded;
static struct iommu_resv_region *iommu_alloc_resv_region_tr(
    phys_addr_t phys, phys_addr_t iova, size_t length, int prot, enum iommu_resv_type type, int gfp)
{
    (void)gfp;
    recorded = (struct iommu_resv_region){phys, iova, length, prot, type};
    return &recorded;
}
static struct iommu_resv_region *iommu_alloc_resv_region(
    phys_addr_t iova, size_t length, int prot, enum iommu_resv_type type, int gfp)
{
    return iommu_alloc_resv_region_tr(iova, iova, length, prot, type, gfp);
}
struct device_node { bool asc, read_only; };
static bool of_device_is_compatible(struct device_node *node, const char *name)
{
    assert(!strcmp(name, "apple,asc-mem"));
    return node->asc;
}
static bool of_property_read_bool(struct device_node *node, const char *name)
{
    assert(!strcmp(name, "apple,firmware-read-only"));
    return node->read_only;
}
#include HEADER
struct dart_io_pgtable { struct { enum io_pgtable_fmt fmt; } iop; };
DART_FUNCTION
int main(int argc, char **argv)
{
    unsigned count = 0;
    for (int asc = 0; asc < 2; asc++)
      for (int ro = 0; ro < 2; ro++)
       for (int coherent = 0; coherent < 2; coherent++)
        for (int translated = 0; translated < 2; translated++) {
            struct device_node node = {asc, ro};
            enum iommu_resv_type type = translated ? IOMMU_RESV_TRANSLATED : IOMMU_RESV_DIRECT;
            struct iommu_resv_region *region = of_iommu_alloc_firmware_region(
                0x12340000, 0x10000000000ULL, 0x4000, type, coherent,
                of_iommu_firmware_read_only(&node));
            assert(region->type == type && region->length == 0x4000);
            assert(region->iova == 0x10000000000ULL);
            assert(region->phys == (translated ? 0x12340000 : region->iova));
            assert(region->prot & IOMMU_READ);
            assert(!!(region->prot & IOMMU_WRITE) == !(asc && ro));
            assert(!!(region->prot & IOMMU_CACHE) == !!coherent);
            for (int version = 0; version < 2; version++) {
                struct dart_io_pgtable data = {.iop.fmt = version ? APPLE_DART2 : APPLE_DART};
                dart_iopte pte = dart_prot_to_pte(&data, region->prot);
                dart_iopte no_write = version ? APPLE_DART2_PTE_PROT_NO_WRITE : APPLE_DART1_PTE_PROT_NO_WRITE;
                dart_iopte no_read = version ? APPLE_DART2_PTE_PROT_NO_READ : APPLE_DART1_PTE_PROT_NO_READ;
                assert(!!(pte & no_write) == !!(asc && ro));
                assert(!(pte & no_read));
                if (version)
                    assert(!!(pte & APPLE_DART2_PTE_PROT_NO_CACHE) == !coherent);
                count++;
            }
        }
    printf("%u allocator/PTE permission paths passed\n", count);
    if (argc == 2) {
        FILE *input = fopen(argv[1], "rb"); assert(input);
        unsigned char blob[32768];
        size_t bytes = fread(blob, 1, sizeof(blob), input); fclose(input);
        assert(bytes > 0 && !fdt_check_header(blob));
        int memory = fdt_path_offset(blob, "/reserved-memory"); assert(memory >= 0);
        int node, found = 0;
        fdt_for_each_subnode(node, blob, memory) {
            int length;
            const fdt64_t *reg = fdt_getprop(blob, node, "reg", &length);
            assert(reg && length == 16);
            const fdt32_t *mapping = fdt_getprop(blob, node, "iommu-addresses", &length);
            assert(mapping && length == 20);
            phys_addr_t phys = fdt64_to_cpu(reg[0]);
            phys_addr_t iova = (uint64_t)fdt32_to_cpu(mapping[1]) << 32 |
                fdt32_to_cpu(mapping[2]);
            size_t size = fdt64_to_cpu(reg[1]);
            struct device_node ofnode = {
                !fdt_node_check_compatible(blob, node, "apple,asc-mem"),
                fdt_getprop(blob, node, "apple,firmware-read-only", NULL) != NULL,
            };
            enum iommu_resv_type type = phys == iova ? IOMMU_RESV_DIRECT : IOMMU_RESV_TRANSLATED;
            struct iommu_resv_region *region = of_iommu_alloc_firmware_region(
                phys, iova, size, type, false, of_iommu_firmware_read_only(&ofnode));
            struct dart_io_pgtable data = {.iop.fmt = APPLE_DART2};
            dart_iopte pte = dart_prot_to_pte(&data, region->prot);
            assert(!!(pte & APPLE_DART2_PTE_PROT_NO_WRITE) == ofnode.read_only);
            assert(!(pte & APPLE_DART2_PTE_PROT_NO_READ));
            found++;
        }
        assert(found == 2);
        printf("actual producer DT: %d DART2 text/data mapping permissions passed\n", found);
    }
    return 0;
}
'''.replace("FLAGS", flags).replace("ENUM", enum).replace("FMT", fmt).replace(
        "HEADER", header).replace("DART_FUNCTION", function(dart, "dart_prot_to_pte"))
    with tempfile.TemporaryDirectory(prefix="apple-firmware-permissions-") as folder:
        folder = Path(folder)
        c = folder / "fixtures.c"
        binary = folder / "fixtures"
        c.write_text(source)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c), "-lfdt", "-o", str(binary)], check=True)
        return subprocess.run([str(binary), *sys.argv[1:]]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
