#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check DMA geometry and SPMI interrupt references in compiled board DTBs.

Run on the buildbox after compiling the boards. dt-schema validates each
node's shape; this checks the cross-node channel bounds, including disabled
clients intended for later enablement.
"""

import pathlib
import struct
import sys


def cells(value):
    if len(value) % 4:
        raise ValueError("property length is not a whole number of cells")
    return struct.unpack(">" + "I" * (len(value) // 4), value)


def read_nodes(data):
    header = struct.unpack_from(">10I", data)
    if header[0] != 0xD00DFEED or header[1] > len(data):
        raise ValueError("invalid flattened device tree")
    offset = header[2]
    strings = data[header[3]:header[3] + header[8]]
    stack = []
    nodes = {}
    while True:
        token, = struct.unpack_from(">I", data, offset)
        offset += 4
        if token == 1:
            end = data.index(b"\0", offset)
            stack.append(data[offset:end].decode("ascii"))
            offset = (end + 4) & ~3
            nodes["/".join(stack) or "/"] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, name_offset = struct.unpack_from(">II", data, offset)
            offset += 8
            end = strings.index(b"\0", name_offset)
            name = strings[name_offset:end].decode("ascii")
            nodes["/".join(stack) or "/"][name] = data[offset:offset + size]
            offset = (offset + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return nodes
        else:
            raise ValueError(f"unexpected FDT token {token}")


def check(path):
    nodes = read_nodes(path.read_bytes())
    providers = {}
    for name, props in nodes.items():
        if "phandle" in props:
            providers[cells(props["phandle"])[0]] = (name, props)
    references = 0
    spmi_interrupts = 0
    for name, props in nodes.items():
        specifiers = cells(props.get("dmas", b""))
        offset = 0
        while offset < len(specifiers):
            phandle = specifiers[offset]
            offset += 1
            if phandle not in providers:
                raise ValueError(f"{name}: DMA provider {phandle} is absent")
            provider_name, provider = providers[phandle]
            count = cells(provider.get("#dma-cells", b""))
            if len(count) != 1 or offset + count[0] > len(specifiers):
                raise ValueError(f"{name}: invalid DMA specifier for {provider_name}")
            args = specifiers[offset:offset + count[0]]
            offset += count[0]
            capacity = cells(provider.get("dma-channels", b""))
            if count[0] == 1 and capacity and args[0] >= capacity[0]:
                raise ValueError(
                    f"{name}: channel {args[0]} exceeds {provider_name}'s "
                    f"{capacity[0]} logical channels"
                )
            references += 1
        interrupt_parent = props.get("interrupt-parent")
        if interrupt_parent and "interrupts" in props:
            phandle, = cells(interrupt_parent)
            if phandle not in providers:
                raise ValueError(f"{name}: interrupt provider {phandle} is absent")
            provider_name, provider = providers[phandle]
            compatible = provider.get("compatible", b"").split(b"\0")
            if b"apple,t8103-spmi" in compatible:
                specifiers = cells(props["interrupts"])
                if len(specifiers) % 2:
                    raise ValueError(f"{name}: malformed SPMI interrupt specifier")
                for offset in range(0, len(specifiers), 2):
                    # spmi-apple-controller.c accepts IRQ_TYPE_EDGE_RISING only.
                    if specifiers[offset + 1] != 1:
                        raise ValueError(
                            f"{name}: {provider_name} supports rising-edge interrupts only"
                        )
                    spmi_interrupts += 1
    print(
        f"{path.name}: {references} DMA references within provider geometry; "
        f"{spmi_interrupts} SPMI interrupt specifiers supported by the controller"
    )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: check-dma.py BOARD.dtb [BOARD.dtb ...]")
    for argument in sys.argv[1:]:
        check(pathlib.Path(argument))
