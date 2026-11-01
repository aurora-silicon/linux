#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the four host-built H16 LOAD sections for one H16G HWX.

H16_LOAD (ABI 3) takes the original HWX plus four sections that the host
builds from it: generic, operation, procedure and text properties. This
derives them from the HWX alone:

- generic: a 0x208-byte header (magic 1, version 0x10, buffer count at
  0x204) and one 0x30-byte entry per surface in HWX segment order. Each
  entry names its buffer ID, its type (0 input, 1 output), its direction
  (1 input, 2 output) and its size. The output gets ID 4 and the inputs
  5, 6, ... in segment order. EXECUTE names buffers by these IDs. A linked
  program (one with a __DATA,__bss scratch segment) gets one more entry,
  last: the scratch, with the next ID, input type and direction and the
  segment size. The kernel owns that buffer; userspace never passes it.
- operation: one record whose BAR references bind every slot the task
  stream references. A slot takes its base from the program descriptor
  qword at +0x10 * (slot + 1); a surface base binds that surface's buffer
  ID, the scratch base (slot 3 in compiler output) binds the scratch ID,
  the __KERN_0 base binds the kernel resource index 1007, and a zero base
  (a config-word slot) stays unbound.
- procedure: the fixed one-procedure form.
- text properties: one segment covering the whole __TEXT,__text stream,
  with one block per task.

The section layouts follow libane's section builder (omarchy-ane, MIT,
Copyright 2026 Joshua Warren) and the H16/H17 program facts; the kernel
validates the generic section against the HWX before LOAD.

  h16-sections.py --hwx program.hwx --out-dir DIR
"""

import argparse
import pathlib
import struct
import sys

GENERIC_MAGIC = 1
GENERIC_VERSION = 0x10
GENERIC_HEADER = 0x208
GENERIC_ENTRY = 0x30
OPERATION_RECORD = 0x40c
OPERATION_REFS_MAX = (OPERATION_RECORD - 0x0c) // 8
RESOURCE_KERNEL = 1007
FIRST_OUTPUT_ID = 4
SECTION_NAMES = ("generic.bin", "operation.bin", "procedure.bin", "text-properties.bin")
# Byte ranges of the H16 register image that task records may name.
REGISTERS = ((0x0000, 0x005c), (0x4100, 0x41a4), (0x44d0, 0x44d4), (0x4500, 0x453c),
             (0x4900, 0x4930), (0x4d00, 0x4e44), (0x5100, 0x5154), (0x5500, 0x5620),
             (0x5900, 0x5930))


class HwxError(ValueError):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def cstr(b):
    return b.split(b"\0", 1)[0].decode()


def parse(hwx):
    """Surfaces in segment order, the text and kernel sections, the program descriptor."""
    if len(hwx) < 32 or u32(hwx, 0) != 0xbeefface or u32(hwx, 8) != 7:
        raise HwxError("not an H16G HWX")
    surfaces, text, kernel, descriptor, tensors, scratch = [], None, None, None, {}, None
    pos = 32
    for _ in range(u32(hwx, 16)):
        cmd, size = u32(hwx, pos), u32(hwx, pos + 4)
        if size < 8 or pos + size > len(hwx):
            raise HwxError("truncated load command")
        if cmd == 0x19:
            segment = cstr(hwx[pos + 8:pos + 24])
            vm, vmsize = u64(hwx, pos + 24), u64(hwx, pos + 32)
            prot = u32(hwx, pos + 60)
            for i in range(u32(hwx, pos + 64)):
                at = pos + 72 + 80 * i
                name = cstr(hwx[at:at + 16])
                svm, ssize, soff = u64(hwx, at + 32), u64(hwx, at + 40), u32(hwx, at + 48)
                if segment == "__FVMLIB":
                    if prot not in (1, 2):
                        raise HwxError("surface segment is neither input nor output")
                    surfaces.append({"vm": svm, "allocation": vmsize, "direction": prot})
                elif segment == "__TEXT" and name == "__text":
                    text = (svm, ssize, soff)
                elif segment == "__KERN_0" and name == "__kern_0":
                    kernel = (svm, ssize, soff)
                elif segment == "__DATA" and name == "__bss":
                    if prot != 3:
                        raise HwxError("scratch segment is not read/write")
                    scratch = (svm, ssize)
        elif cmd == 4 and u32(hwx, pos + 8) == 4:
            descriptor = pos
        elif cmd == 4 and u32(hwx, pos + 8) == 3:
            shortoff = u32(hwx, pos + 0x7c)
            n, batch = u32(hwx, pos + 0x28), u64(hwx, pos + 0x50)
            tensors[cstr(hwx[pos + shortoff:pos + size])] = (u32(hwx, pos + 0x14), n * batch)
        elif cmd == 0x40:
            vm, name = u64(hwx, pos + 16), cstr(hwx[pos + 24:pos + size])
            for s in surfaces:
                if s["vm"] == vm:
                    s["name"] = name
        pos += size
    if text is None or descriptor is None or len(surfaces) < 2:
        raise HwxError("HWX lacks a task stream, program descriptor or surfaces")
    for s in surfaces:
        direction, span = tensors.get(s.get("name"), (None, None))
        if direction != s["direction"] or not span or span > s["allocation"]:
            raise HwxError("surface %s has no matching tensor" % s.get("name"))
        s["span"] = span
    if sum(s["direction"] == 2 for s in surfaces) != 1:
        raise HwxError("an H16 program has exactly one output")
    return surfaces, text, kernel, descriptor, scratch


def register_valid(word):
    return any(start <= word * 4 < end for start, end in REGISTERS)


def walk(stream):
    """Count the tasks and collect the BAR slots their records reference."""
    if len(stream) < 16 or len(stream) % 4 or u32(stream, 0) != 1 or any(stream[4:16]):
        raise HwxError("task stream lacks the H16 prologue")
    pos, tasks, slots = 16, 0, set()
    while pos < len(stream):
        words = (u32(stream, pos) >> 16) & 0x7ff
        if not words:
            if any(stream[pos:pos + 16]):
                raise HwxError("nonzero padding in the task stream")
            pos += 16
            continue
        end = pos + 4 * words
        header = 10 if (u32(stream, pos + 32) & 3) == 3 else 9
        if words < header or end > len(stream):
            raise HwxError("task overruns the stream")
        rec = pos + 4 * header
        while rec < end:
            h = u32(stream, rec)
            rec += 4
            if h & (1 << 31):
                count = 1 + bin((h >> 15) & 0xffff).count("1")
            else:
                count = ((h >> 15) & 0x3f) + 1
                if h & (1 << 29):
                    slots.add((h >> 23) & 0x3f)
            if not register_valid(h & 0x7fff) or rec + 4 * count > end:
                raise HwxError("task record outside the register image")
            rec += 4 * count
        pos = (end + 15) & ~15
        tasks += 1
    if not tasks:
        raise HwxError("task stream has no task")
    return tasks, slots


def build(hwx):
    """Return the four sections, in H16_LOAD kind order, and the buffer IDs."""
    surfaces, (text_vm, text_size, text_off), kernel, descriptor, scratch = parse(hwx)
    stream = hwx[text_off:text_off + text_size]
    tasks, slots = walk(stream)

    inputs = iter(range(FIRST_OUTPUT_ID + 1, 0x3d))
    for s in surfaces:
        s["id"] = FIRST_OUTPUT_ID if s["direction"] == 2 else next(inputs)
    entries = [(s["id"], s["direction"], s["allocation"]) for s in surfaces]
    scratch_id = None
    if scratch:
        scratch_id = next(inputs)
        entries.append((scratch_id, 1, scratch[1]))

    generic = bytearray(GENERIC_HEADER + GENERIC_ENTRY * len(entries))
    struct.pack_into("<II", generic, 0, GENERIC_MAGIC, GENERIC_VERSION)
    struct.pack_into("<I", generic, 0x204, len(entries))
    for k, (ident, direction, size) in enumerate(entries):
        e = GENERIC_HEADER + GENERIC_ENTRY * k
        struct.pack_into("<III", generic, e, 1, ident, direction - 1)
        struct.pack_into("<I", generic, e + 0x10, direction)
        struct.pack_into("<Q", generic, e + 0x20, size)
        struct.pack_into("<I", generic, e + 0x28, 0xffff)

    refs = []
    for slot in sorted(slots):
        base = u64(hwx, descriptor + 0x10 * (slot + 1))
        if not base:
            continue
        hit = [s["id"] for s in surfaces if s["vm"] == base]
        if hit:
            refs.append((slot, hit[0]))
        elif scratch and base == scratch[0]:
            refs.append((slot, scratch_id))
        elif kernel and base == kernel[0]:
            refs.append((slot, RESOURCE_KERNEL))
        else:
            raise HwxError("BAR slot %d base %#x names no surface or kernel" % (slot, base))
    if not refs or len(refs) > OPERATION_REFS_MAX:
        raise HwxError("program binds no buffer through a BAR slot")
    operation = bytearray(4 + OPERATION_RECORD)
    struct.pack_into("<I", operation, 0, 1)
    struct.pack_into("<I", operation, 4 + 0x08, len(refs))
    for j, (slot, tag) in enumerate(refs):
        struct.pack_into("<II", operation, 4 + 0x0c + 8 * j, slot, tag)

    procedure = bytearray(0x38)
    struct.pack_into("<IIQQ", procedure, 0, 1, 0, 0x18, 0x20)
    struct.pack_into("<8I", procedure, 0x18, 1, 3, 0, 0, 1, 0, 0xffffffff, 4)

    properties = bytearray(0x28)
    struct.pack_into("<IIII", properties, 0, 1, 0, 0, tasks)
    struct.pack_into("<QQQ", properties, 0x10, 0, 0, text_size)

    sections = [bytes(generic), bytes(operation), bytes(procedure), bytes(properties)]
    ids = {s["name"]: s["id"] for s in surfaces}
    if scratch:
        ids["scratch"] = scratch_id
    return sections, ids, refs, tasks


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hwx", type=pathlib.Path, required=True)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        sections, ids, refs, tasks = build(args.hwx.read_bytes())
    except HwxError as e:
        print(f"FAIL: {e}")
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for name, data in zip(SECTION_NAMES, sections):
        (args.out_dir / name).write_bytes(data)
    print("buffer IDs", ", ".join(f"{n}={i}" for n, i in ids.items()))
    print("BAR refs", ", ".join(f"slot {s}->{t}" for s, t in refs), f"tasks {tasks}")
    print("sizes", ", ".join(f"{n} {len(d):#x}" for n, d in zip(SECTION_NAMES, sections)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
