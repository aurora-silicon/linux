#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host checks of production G15 admission and pure recovery/buffer logic.

No GPU access or kernel build. Kernel services are mocked; this cannot establish
that a GPU boots, that cache maintenance works, or that the driver compiles.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[6]
G15 = ROOT / "drivers/gpu/drm/asahi/g15"

MEMORY_PREFIX = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define PAGE_SIZE 0x4000UL
#define PAGE_SHIFT 14
#define IS_ALIGNED(x, a) (!((x) & ((a) - 1)))
#define PHYS_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))
#define check_add_overflow(a, b, out) __builtin_add_overflow(a, b, out)
#define IORESOURCE_SYSTEM_RAM 1
#define IORES_DESC_NONE 0
#define REGION_INTERSECTS 1
#define REGION_DISJOINT 0
#define MEMREMAP_WB 1
#define MEMREMAP_WC 4
struct page { bool valid, ram, reserved; };
static struct page pages[8];
static bool pfn_valid(unsigned long pfn) { return pfn < 8 && pages[pfn].valid; }
static bool page_is_ram(unsigned long pfn) { return pfn < 8 && pages[pfn].ram; }
static struct page *pfn_to_page(unsigned long pfn) { return &pages[pfn]; }
#define PageReserved(p) ((p)->reserved)
/* Model the documented API: RAM plus undefined holes is INTERSECTS. */
static int region_intersects(u64 start, u64 size, int flags, int desc)
{
    unsigned long first = PHYS_PFN(start);
    (void)flags; (void)desc;
    if (!size || start + size < start)
        return REGION_DISJOINT;
    for (unsigned long p = first; p < 8 && p <= PHYS_PFN(start + size - 1); p++)
        if (pages[p].ram)
            return REGION_INTERSECTS;
    return REGION_DISJOINT;
}
'''

MEMORY_CASES = r'''
static int failures;
static void check(const char *name, u64 start, u64 size, bool expected)
{
    bool actual = asahi_g15_ram_within(start, size);
    printf("%s: %s\n", name, actual == expected ? "PASS" : "FAIL");
    failures += actual != expected;
}
static void mode(const char *name, u64 start, u64 size, unsigned int expected)
{
    unsigned int actual = asahi_g15_memory_mode(start, size);
    printf("%s: %s\n", name, actual == expected ? "PASS" : "FAIL");
    failures += actual != expected;
}
int main(void)
{
    for (int i = 0; i < 8; i++)
        pages[i] = (struct page){true, true, true};
    check("reserved RAM", PAGE_SIZE, 3 * PAGE_SIZE, true);
    mode("reserved mapping is WB", PAGE_SIZE, 3 * PAGE_SIZE, MEMREMAP_WB);
    mode("non-RAM mapping is WC", 8 * PAGE_SIZE, PAGE_SIZE, MEMREMAP_WC);
    mode("mapping rejects empty range", PAGE_SIZE, 0, 0);
    mode("mapping rejects unaligned address", PAGE_SIZE + 1, PAGE_SIZE, 0);
    mode("mapping rejects unaligned size", PAGE_SIZE, PAGE_SIZE - 1, 0);
    mode("mapping rejects overflow", UINT64_MAX & ~(PAGE_SIZE - 1), PAGE_SIZE, 0);
    check("last complete page", 7 * PAGE_SIZE, PAGE_SIZE, true);
    check("empty range", PAGE_SIZE, 0, false);
    check("unaligned address", PAGE_SIZE + 1, PAGE_SIZE, false);
    check("unaligned size", PAGE_SIZE, PAGE_SIZE - 1, false);
    check("overflow", UINT64_MAX & ~(PAGE_SIZE - 1), PAGE_SIZE, false);
    pages[2].ram = false;
    check("RAM plus undefined hole", PAGE_SIZE, 3 * PAGE_SIZE, false);
    mode("mapping rejects RAM plus hole", PAGE_SIZE, 3 * PAGE_SIZE, 0);
    pages[2].ram = true;
    pages[2].reserved = false;
    check("allocator-owned page", PAGE_SIZE, 3 * PAGE_SIZE, false);
    mode("mapping rejects allocator page", PAGE_SIZE, 3 * PAGE_SIZE, 0);
    pages[2].reserved = true;
    pages[2].valid = false;
    check("invalid PFN", PAGE_SIZE, 3 * PAGE_SIZE, false);
    mode("mapping rejects invalid PFN", PAGE_SIZE, 3 * PAGE_SIZE, 0);
    return failures ? 1 : 0;
}
'''

RUST_PREFIX = r'''
#![allow(dead_code)]
extern crate self as kernel;
pub mod prelude {
    pub type Error = i32;
    pub type Result<T = ()> = std::result::Result<T, Error>;
    pub const EINVAL: Error = -22;
    pub const EFAULT: Error = -14;
    pub use crate::{pr_warn, static_assert};
}
#[macro_export]
macro_rules! pr_warn { ($($arg:tt)*) => { let _ = format_args!($($arg)*); } }
#[macro_export]
macro_rules! static_assert { ($e:expr) => { const _: () = assert!($e); } }
pub mod mmu {
    use crate::prelude::*;
    pub const UAT_PGSZ: usize = 0x4000;
    pub const UAT_PGMSK: usize = UAT_PGSZ - 1;
    pub struct Vm(pub Vec<u8>);
    impl Vm {
        pub fn read_user(&self, va: u64, out: &mut [u8]) -> Result {
            let start = usize::try_from(va).map_err(|_| EFAULT)?;
            let end = start.checked_add(out.len()).ok_or(EFAULT)?;
            out.copy_from_slice(self.0.get(start..end).ok_or(EFAULT)?);
            Ok(())
        }
    }
}
mod command {
    use crate::{mmu, prelude::*};
    pub const USC_WINDOW: u64 = 1 << 32;
    pub fn cover(vm: &mmu::Vm, start: u64, end: u64, _: bool, _: bool) -> Result {
        let mut bytes = vec![0; usize::try_from(end - start).map_err(|_| EINVAL)?];
        vm.read_user(start, &mut bytes)
    }
}
mod params { pub fn stream_gate() -> bool { true } }
'''

GATE_CASES = r'''
#[cfg(test)]
mod gate_regressions {
    use crate::{gate, mmu, prelude::*};
    fn vm(words: &[u32]) -> mmu::Vm {
        let mut bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        bytes.resize(bytes.len() + 256, 0);
        mmu::Vm(bytes)
    }
    #[test]
    fn terminator_and_known_barrier() {
        assert_eq!(gate::cdm(&vm(&[0x40000000]), 0, 0), Ok(()));
        assert_eq!(gate::cdm(&vm(&[0x60000160, 0x40000000]), 0, 0), Ok(()));
    }
    #[test]
    fn cyclic_links_are_rejected() {
        assert_eq!(gate::cdm(&vm(&[0x20000000, 0]), 0, 0), Err(EINVAL));
    }
    #[test]
    fn record_budget_requires_a_terminator() {
        assert_eq!(gate::cdm(&vm(&vec![0x60000160; 8192]), 0, 0), Err(EINVAL));
    }
    #[test]
    fn final_record_may_terminate() {
        let mut words = vec![0x60000160; 8192];
        words[8191] = 0x40000000;
        assert_eq!(gate::cdm(&vm(&words), 0, 0), Ok(()));
    }
    #[test]
    fn malformed_and_unreadable_streams_are_rejected() {
        assert_eq!(gate::cdm(&vm(&[0xffffffff]), 0, 0), Err(EINVAL));
        assert_eq!(gate::cdm(&vm(&[0x20000000, 2]), 0, 0), Err(EINVAL));
        assert_eq!(gate::cdm(&mmu::Vm(vec![]), 0, 0), Err(EFAULT));
    }
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="asahi-g15-host-") as temp:
        temp = Path(temp)
        glue = (G15 / "glue.c").read_text()
        functions = []
        for signature in ["bool asahi_g15_ram_disjoint", "bool asahi_g15_ram_within",
                          "unsigned int asahi_g15_memory_mode"]:
            marker = signature + "(u64 start, u64 size)\n{"
            start = glue.index(marker)
            end = glue.index("\n}", start) + 2
            functions.append(glue[start:end])
        c_source = temp / "memory.c"
        c_source.write_text(MEMORY_PREFIX + "\n".join(functions) + MEMORY_CASES)
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", str(c_source),
                        "-o", str(temp / "memory")], check=True)
        memory = subprocess.run([str(temp / "memory")]).returncode

        start = glue.index("bool asahi_g15_reserved_uat(u32 index, u64 start, u64 size)\n{")
        end = glue.index("\n}", start) + 2
        (temp / "uat-production.h").write_text(glue[start:end])
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-I", str(temp),
                        str(Path(__file__).with_name("uat-owner.c")),
                        "-o", str(temp / "uat-owner")], check=True)
        subprocess.run([str(temp / "uat-owner")], check=True)

        pmp = []
        for name in ["prepare", "get"]:
            start = glue.index("int asahi_g15_pmp_" + name + "(struct device *dev)\n{")
            end = glue.index("\n}", start) + 2
            pmp.append(glue[start:end])
        (temp / "pmp-production.h").write_text("\n".join(pmp))
        # This module has one maintained PMP bridge, not SF's old alternate stack.
        # Supplier-readiness behavior is covered with the maintained PMP implementation's
        # own tests; this fixture verifies the consumer's gate, defer, ACK and vote contract.
        output = temp / "pmp-maintained"
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", "-I", str(temp),
                        str(Path(__file__).with_name("pmp-bridge.c")), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)

        # Native wire layout is a private per-backend facade. Its independent host test
        # checks that canonical normal/G17 UAPI remains unchanged; there is no global
        # Rust bindgen header selector in this integration.
        harness = RUST_PREFIX + f'\n#[path = "{G15 / "gate.rs"}"] mod gate;\n'
        harness += f'#[path = "{G15 / "recovery/logic.rs"}"] mod recovery;\n'
        harness += f'#[path = "{G15 / "power/idle.rs"}"] mod idle;\n'
        harness += f'#[path = "{G15 / "power/pstate.rs"}"] mod pstate;\n'
        harness += ("mod firmware_identity {\n" +
                    f'#[path = "{G15 / "identity/matching.rs"}"] pub mod matching;\n' +
                    f'#[path = "{G15 / "identity/lookup.rs"}"] pub mod lookup;\n' + "}\n")
        # The T6031 board-data builders (the J516C fixture cases stay ignored: they need
        # off-tree machine data).
        harness += ("mod board {\n" +
                    f'#[path = "{G15 / "board/template.rs"}"] pub mod template;\n' +
                    "pub(crate) use template::{Line, Template};\n" +
                    f'#[path = "{G15 / "board/data.rs"}"] pub mod data;\n' +
                    f'#[path = "{G15 / "board/words.rs"}"] pub mod words;\n' +
                    f'#[path = "{G15 / "board/record.rs"}"] pub mod record;\n' + "}\n")
        harness += ("mod g15 { pub mod fw {\n" +
                    f'#[path = "{G15 / "fw/channels.rs"}"] pub mod channels;\n' +
                    f'#[path = "{G15 / "fw/buffer.rs"}"] pub mod buffer;\n' +
                    "} pub mod object {\n" +
                    f'#[path = "{G15 / "object/geometry.rs"}"] pub mod geometry;\n' +
                    "} pub mod pools {\n" +
                    f'#[path = "{G15 / "pools/growth.rs"}"] pub mod growth;\n' + "}}\n")
        rust_source = temp / "host.rs"
        rust_source.write_text(harness + GATE_CASES)
        subprocess.run([os.environ.get("HOSTRUSTC", "rustc"), "--edition=2021", "--test",
                        str(rust_source), "-o", str(temp / "host")], check=True)
        rust = subprocess.run([str(temp / "host")]).returncode
        return int(bool(memory or rust))


if __name__ == "__main__":
    raise SystemExit(main())
