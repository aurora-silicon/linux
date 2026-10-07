#!/usr/bin/env python3
"""Print an allowlist of the boot loader's copy of the Apple device tree (ADT).

m1n1 keeps the ADT in a reserved-memory region with compatible "phram" and label "adt". With the
phram module loaded, Linux shows that region as an MTD device named adt. This tool reads it
through the read-only node /dev/mtdNro, or reads a regular file (tests, offline copies), and
prints only:
  - the nodes under ALLOWED_SUBTREES (GPU, display, PMP, power manager, CPU clusters), and
  - the properties named in LIMITED_NODES of a few container nodes (/, /arm-io, /product).
Every other node is left out. Inside the printed nodes it drops every property whose name
contains one of DENIED_NAME_PARTS, and every property whose value contains the value of such a
property anywhere in the ADT. Numbers are printed in hex. The last line sums up what was dropped.

It never writes: every device and file is opened O_RDONLY. Before it reads a device it checks
that the device is the read-only node of the MTD device named adt, and that this MTD device is
the running device tree's reserved-memory region labelled adt, with the same size. The output is
built in memory and printed only when everything succeeded.

Usage:
  aurora-adt-extract.py [PATH]          print the allowlist (as root, for a device)
  aurora-adt-extract.py --check [PATH]  check the device and what it is bound to; read nothing
PATH is /dev/mtdNro or a regular file holding an ADT. Without PATH: the read-only node of the
MTD device named adt.

Exit status: 0 done, 1 I/O error, 2 usage, 3 refused (not the adt device, or it does not match
its region), 4 not a valid ADT, 5 internal error. Nothing is printed on stdout unless it is 0.
"""

import argparse
import fnmatch
import os
import re
import stat
import struct
import sys

PROG = "aurora-adt-extract"

# --- What is printed -------------------------------------------------------------------------

# Nodes printed whole (minus dropped properties), with every node below them. One fnmatch
# pattern per path segment.
ALLOWED_SUBTREES = (
    # GPU: register windows, interrupts, perf-state tables, core counts and masks, UAT regions.
    "/arm-io/sgx",
    # GPU firmware processor: windows, mailbox interrupts, firmware segments (iop-gfx-nub).
    "/arm-io/gfx-asc",
    # The GPU firmware processor's address mapper, and a GPU DART where a chip has one.
    "/arm-io/mapper-gfx-asc",
    "/arm-io/dart-gfx*",
    # Power manager: power-state registers, device, domain and clock tables, and the voltage
    # and performance-state tables of the GPU and the CPU clusters.
    "/arm-io/pmgr",
    # Power management processor and its DART: windows, interrupts, iop-pmp-nub values.
    "/arm-io/pmp",
    "/arm-io/dart-pmp",
    # Display processors (built-in and external): windows, interrupts, iop-*-nub segments.
    "/arm-io/dcp",
    "/arm-io/dcpext*",
    # Display pipes (disp0, dispext0-3): windows, interrupts, clock and power gates.
    "/arm-io/disp*",
    # Display DARTs (dart-dcp, dart-disp0, dart-dcpext*, dart-dispext*): windows, VM ranges,
    # stream ids.
    "/arm-io/dart-dcp*",
    "/arm-io/dart-disp*",
    # CPU clusters and cores: ids, cluster types, registers.
    "/cpus",
)

# Container nodes printed with these properties only.
LIMITED_NODES = {
    # The cell sizes /arm-io's ranges are written in.
    "/": ("#address-cells", "#size-cells"),
    # How the reg values of the nodes above map to physical addresses.
    "/arm-io": ("name", "compatible", "device_type", "#address-cells", "#size-cells", "ranges",
                "chip-revision"),
    # The model, nothing else.
    "/product": ("name", "product-name", "product-description"),
}

# Properties dropped wherever they are, matched as case-insensitive parts of the name.
DENIED_NAME_PARTS = (
    "serial", "mac-address", "ecid", "unique", "uuid", "udid", "nonce", "key", "calibration",
    "cal-data", "region-info", "imei", "wifi", "bluetooth",
    # Also dropped: values that may tell one Mac from another.
    "seed", "random", "entropy", "cert", "fuse",
)

# A dropped property's value counts for the value check from this many bytes and distinct bytes
# on (shorter values match too much ordinary data).
SECRET_MIN_BYTES = 6
SECRET_MIN_DISTINCT = 3

# Values longer than this are not printed (the property is listed as omitted).
MAX_PRINTED_VALUE = 64 * 1024

# --- Where the device is ---------------------------------------------------------------------

MTD_CHAR_MAJOR = 90
ADT_LABEL = "adt"
MAX_FILE_BYTES = 16 * 1024 * 1024

# --- The ADT format --------------------------------------------------------------------------
# A node: u32 property count, u32 child count, the properties, then the children.
# A property: a 32-byte NUL-padded name, a u32 size whose top bit is a flag (a placeholder
# value), then the value, padded to 4 bytes. All little-endian. A node's name is the value of its
# "name" property.

PROP_NAME_BYTES = 32
PROP_HEADER_BYTES = PROP_NAME_BYTES + 4
NODE_HEADER_BYTES = 8
PROP_SIZE_MASK = 0x7FFFFFFF
PROP_FLAG_PLACEHOLDER = 0x80000000
MAX_DEPTH = 64
MAX_NODES = 100000
MAX_PROPS = 1000000


class AdtError(ValueError):
    """The data is not a valid ADT."""


class Refused(Exception):
    """The source is not the device this reads, or it does not match its region."""


class Prop:
    __slots__ = ("name", "value", "placeholder")

    def __init__(self, name, value, placeholder):
        self.name = name
        self.value = value
        self.placeholder = placeholder


class Node:
    __slots__ = ("props", "children")

    def __init__(self, props, children):
        self.props = props
        self.children = children

    @property
    def name(self):
        for prop in self.props:
            if prop.name == "name":
                text = prop.value.split(b"\0", 1)[0]
                if text and all(0x21 <= b <= 0x7E and b != 0x2F for b in text):
                    return text.decode("ascii")
                return ""
        return ""


class _Count:
    def __init__(self):
        self.nodes = 0
        self.props = 0


def _parse_node(buf, off, depth, count):
    if depth > MAX_DEPTH:
        raise AdtError(f"nodes nested deeper than {MAX_DEPTH} levels")
    end = len(buf)
    if off + NODE_HEADER_BYTES > end:
        raise AdtError(f"truncated: node header at offset {off:#x} runs past the end")
    nprops, nchildren = struct.unpack_from("<II", buf, off)
    start = off
    off += NODE_HEADER_BYTES
    if nprops > (end - off) // PROP_HEADER_BYTES:
        raise AdtError(f"node at offset {start:#x} claims {nprops} properties, more than the data holds")
    if nchildren > (end - off) // NODE_HEADER_BYTES:
        raise AdtError(f"node at offset {start:#x} claims {nchildren} children, more than the data holds")
    count.nodes += 1
    if count.nodes > MAX_NODES:
        raise AdtError(f"more than {MAX_NODES} nodes")
    props = []
    for _ in range(nprops):
        if off + PROP_HEADER_BYTES > end:
            raise AdtError(f"truncated: property header at offset {off:#x} runs past the end")
        raw_name = bytes(buf[off:off + PROP_NAME_BYTES])
        nul = raw_name.find(b"\0")
        if nul < 0:
            raise AdtError(f"property name at offset {off:#x} has no terminating NUL")
        if nul == 0:
            raise AdtError(f"property name at offset {off:#x} is empty")
        name_bytes = raw_name[:nul]
        if not all(0x21 <= b <= 0x7E for b in name_bytes):
            raise AdtError(f"property name at offset {off:#x} is not printable ASCII")
        (raw_size,) = struct.unpack_from("<I", buf, off + PROP_NAME_BYTES)
        size = raw_size & PROP_SIZE_MASK
        vstart = off + PROP_HEADER_BYTES
        if size > end - vstart:
            raise AdtError(f"property {name_bytes.decode('ascii')!r} at offset {off:#x}: "
                           f"its {size} bytes run past the end")
        padded = (size + 3) & ~3
        if padded > end - vstart:
            raise AdtError(f"truncated: padding of property at offset {off:#x} runs past the end")
        props.append(Prop(name_bytes.decode("ascii"), bytes(buf[vstart:vstart + size]),
                          bool(raw_size & PROP_FLAG_PLACEHOLDER)))
        count.props += 1
        if count.props > MAX_PROPS:
            raise AdtError(f"more than {MAX_PROPS} properties")
        off = vstart + padded
    children = []
    for _ in range(nchildren):
        child, off = _parse_node(buf, off, depth + 1, count)
        children.append(child)
    return Node(props, children), off


def parse_adt(buf):
    """Parses the ADT at the start of buf. Returns (root, bytes used, nodes, properties).
    Bytes after the root node (the region's padding) are ignored. Raises AdtError."""
    if not isinstance(buf, (bytes, bytearray, memoryview)):
        raise TypeError("parse_adt takes bytes")
    count = _Count()
    root, used = _parse_node(memoryview(buf), 0, 0, count)
    if not root.props and not root.children:
        raise AdtError("the root node is empty")
    return root, used, count.nodes, count.props


def walk(node, path="/"):
    """Yields (path, node) for node and everything below it, in ADT order."""
    yield path, node
    for child in node.children:
        name = child.name or "?"
        yield from walk(child, (path.rstrip("/") + "/" + name))


# --- The allowlist -----------------------------------------------------------------------------

def _segments(path):
    return [s for s in path.split("/") if s]


def in_allowed_subtree(path):
    segs = _segments(path)
    for pattern in ALLOWED_SUBTREES:
        pat = _segments(pattern)
        if len(segs) >= len(pat) and all(fnmatch.fnmatchcase(s, p) for s, p in zip(segs, pat)):
            return True
    return False


def denied_part(name):
    """The DENIED_NAME_PARTS entry name contains, or None."""
    low = name.lower()
    for part in DENIED_NAME_PARTS:
        if part in low:
            return part
    return None


def _secret_values(root):
    """The values of every denied property anywhere in the ADT that are long and varied enough to
    look for in other values."""
    secrets = set()
    for _, node in walk(root):
        for prop in node.props:
            if denied_part(prop.name) is None:
                continue
            value = prop.value.rstrip(b"\0")
            if len(value) >= SECRET_MIN_BYTES and len(set(value)) >= SECRET_MIN_DISTINCT:
                secrets.add(value)
    return secrets


def _is_strings(value):
    if len(value) < 2 or value[-1] != 0:
        return False
    parts = value[:-1].split(b"\0")
    return all(part and all(0x20 <= b <= 0x7E for b in part) for part in parts)


def format_value(value):
    """The value as text: quoted strings, else little-endian u32 words, else bytes, in hex."""
    if not value:
        return "<>"
    if _is_strings(value):
        quoted = []
        for part in value[:-1].split(b"\0"):
            text = part.decode("ascii").replace("\\", "\\\\").replace('"', '\\"')
            quoted.append(f'"{text}"')
        return ", ".join(quoted)
    if len(value) % 4 == 0:
        words = struct.unpack(f"<{len(value) // 4}I", value)
        items = [f"0x{w:08x}" for w in words]
        open_, close = "<", ">"
    else:
        items = [f"{b:02x}" for b in value]
        open_, close = "[", "]"
    per_line = 8 if open_ == "<" else 16
    lines = [" ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return open_ + "\n      ".join(lines) + close


def render(root, source, used, nodes, props):
    """The allowlist report, as one string."""
    secrets = _secret_values(root)
    out = [
        f"# {PROG}: an allowlist of the Apple device tree (ADT); read-only",
        f"# source: {source}",
        f"# ADT: {nodes} nodes, {props} properties, {used} bytes",
    ]
    dropped = []
    by_part = {}
    by_value = 0
    too_large = 0
    shown_nodes = 0
    shown_props = 0
    hidden_nodes = 0
    limited_left_out = 0
    for path, node in walk(root):
        limited = LIMITED_NODES.get(path)
        if limited is None and not in_allowed_subtree(path):
            hidden_nodes += 1
            continue
        shown_nodes += 1
        out.append(path)
        for prop in node.props:
            if limited is not None and prop.name not in limited:
                limited_left_out += 1
                continue
            part = denied_part(prop.name)
            if part is not None:
                by_part[part] = by_part.get(part, 0) + 1
                dropped.append(f"# dropped: {path} {prop.name} (name contains {part!r})")
                continue
            if any(secret in prop.value for secret in secrets):
                by_value += 1
                dropped.append(f"# dropped: {path} {prop.name} (value holds a dropped property's value)")
                continue
            flag = " (placeholder)" if prop.placeholder else ""
            if len(prop.value) > MAX_PRINTED_VALUE:
                too_large += 1
                out.append(f"  {prop.name} [{len(prop.value)}]{flag} (omitted: larger than "
                           f"{MAX_PRINTED_VALUE // 1024} KiB)")
                continue
            shown_props += 1
            out.append(f"  {prop.name} [{len(prop.value)}]{flag} = {format_value(prop.value)}")
    out.extend(dropped)
    parts = ", ".join(f"{part} {n}" for part, n in sorted(by_part.items())) or "none"
    by_name = sum(by_part.values())
    out.append(
        f"# summary: printed {shown_nodes} nodes and {shown_props} properties; dropped "
        f"{by_name + by_value} properties ({by_name} by name: {parts}; {by_value} by value); "
        f"omitted {too_large} larger than {MAX_PRINTED_VALUE // 1024} KiB; not printed: "
        f"{hidden_nodes} nodes outside the allowlist and {limited_left_out} properties of "
        f"limited nodes")
    return "\n".join(out) + "\n"


# --- The device and its region ------------------------------------------------------------------

class Host:
    """Where the running system's files are. Tests use a fake one."""

    def __init__(self, sys_root="/sys", dt_root="/sys/firmware/devicetree/base", dev_root="/dev",
                 stat_fn=os.stat, fstat_fn=os.fstat):
        self.sys_root = sys_root
        self.dt_root = dt_root
        self.dev_root = dev_root
        self.stat = stat_fn
        self.fstat = fstat_fn


class Region:
    def __init__(self, node, base, size):
        self.node = node
        self.base = base
        self.size = size


class Binding:
    def __init__(self, dev, rdev, mtd, region):
        self.dev = dev
        self.rdev = rdev
        self.mtd = mtd
        self.region = region

    def describe(self):
        return (f"{self.dev}: MTD device {self.mtd} named {ADT_LABEL!r}, {self.region.size} bytes, "
                f"read-only node; reserved-memory node {os.path.basename(self.region.node)} "
                f"at {self.region.base:#x}, {self.region.size:#x} bytes")


def _read_small(path, limit=4096):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        return os.read(fd, limit)
    finally:
        os.close(fd)


def _dt_prop(node, name):
    path = os.path.join(node, name)
    if not os.path.isfile(path):
        return None
    return _read_small(path, 65536)


def _be_cells(raw, what):
    if raw is None or len(raw) != 4:
        raise Refused(f"the device tree has no usable {what}")
    return struct.unpack(">I", raw)[0]


def find_region(host):
    """The running device tree's reserved-memory region labelled adt, with compatible phram."""
    rm = os.path.join(host.dt_root, "reserved-memory")
    if not os.path.isdir(rm):
        raise Refused("the device tree has no reserved-memory node")
    ac = _be_cells(_dt_prop(rm, "#address-cells"), "reserved-memory #address-cells")
    sc = _be_cells(_dt_prop(rm, "#size-cells"), "reserved-memory #size-cells")
    if not (1 <= ac <= 2 and 1 <= sc <= 2):
        raise Refused(f"reserved-memory cell sizes {ac}/{sc} are not 1 or 2")
    found = []
    for entry in sorted(os.listdir(rm)):
        node = os.path.join(rm, entry)
        if not os.path.isdir(node):
            continue
        compatible = (_dt_prop(node, "compatible") or b"").split(b"\0")
        label = (_dt_prop(node, "label") or b"").rstrip(b"\0")
        if b"phram" in compatible and label == ADT_LABEL.encode():
            found.append(node)
    if not found:
        raise Refused("the device tree has no reserved-memory region labelled adt (this boot's "
                      "m1n1 reserved none)")
    if len(found) > 1:
        raise Refused(f"the device tree has {len(found)} reserved-memory regions labelled adt")
    node = found[0]
    status = (_dt_prop(node, "status") or b"okay").rstrip(b"\0")
    if status not in (b"okay", b"ok"):
        raise Refused(f"the adt region {os.path.basename(node)} is not enabled")
    reg = _dt_prop(node, "reg")
    if reg is None or len(reg) != 4 * (ac + sc):
        raise Refused(f"the adt region {os.path.basename(node)} does not have exactly one reg entry")
    cells = struct.unpack(f">{ac + sc}I", reg)
    base = 0
    for c in cells[:ac]:
        base = (base << 32) | c
    size = 0
    for c in cells[ac:]:
        size = (size << 32) | c
    if size == 0:
        raise Refused(f"the adt region {os.path.basename(node)} has size 0")
    return Region(os.path.realpath(node), base, size)


def _sys_attr(path):
    try:
        return _read_small(path).decode("ascii", "replace").strip()
    except OSError:
        return None


def find_adt_mtd(host):
    """The name (mtdN) of the one MTD device named adt."""
    cls = os.path.join(host.sys_root, "class", "mtd")
    names = []
    if os.path.isdir(cls):
        for entry in sorted(os.listdir(cls)):
            if re.fullmatch(r"mtd[0-9]+", entry) and _sys_attr(os.path.join(cls, entry, "name")) == ADT_LABEL:
                names.append(entry)
    if not names:
        raise Refused("no MTD device is named adt (is the phram module loaded?)")
    if len(names) > 1:
        raise Refused(f"{len(names)} MTD devices are named adt: {' '.join(names)}")
    return names[0]


def check_binding(path, host):
    """Checks that path (default: the read-only node of the MTD device named adt) is the read-only
    node of the MTD device named adt, and that this device is the device tree's adt region. Returns
    a Binding; raises Refused."""
    region = find_region(host)
    if path is None:
        path = os.path.join(host.dev_root, find_adt_mtd(host) + "ro")
    real = os.path.realpath(path)
    try:
        st = host.stat(real)
    except FileNotFoundError:
        raise Refused(f"{path} does not exist") from None
    if not stat.S_ISCHR(st.st_mode):
        raise Refused(f"{path} is not a character device")
    major, minor = os.major(st.st_rdev), os.minor(st.st_rdev)
    if major != MTD_CHAR_MAJOR:
        raise Refused(f"{path} is not an MTD device (major {major})")
    if minor % 2 == 0:
        raise Refused(f"{path} is a writable MTD node; this reads only the read-only node /dev/mtdNro")
    mtd = f"mtd{minor >> 1}"
    cls = os.path.join(host.sys_root, "class", "mtd")
    if _sys_attr(os.path.join(cls, mtd + "ro", "dev")) != f"{MTD_CHAR_MAJOR}:{minor}":
        raise Refused(f"{path} is not the node of {mtd}ro in sysfs")
    name = _sys_attr(os.path.join(cls, mtd, "name"))
    if name != ADT_LABEL:
        raise Refused(f"{path} belongs to {mtd}, named {name!r}, not {ADT_LABEL!r}")
    mtd_type = _sys_attr(os.path.join(cls, mtd, "type"))
    if mtd_type != "ram":
        raise Refused(f"{mtd} is of type {mtd_type!r}, not 'ram' (phram)")
    size = _sys_attr(os.path.join(cls, mtd, "size"))
    if size != str(region.size):
        raise Refused(f"{mtd} has {size} bytes, but its region {os.path.basename(region.node)} has "
                      f"{region.size}")
    of_node = os.path.join(cls, mtd, "of_node")
    if not os.path.islink(of_node):
        raise Refused(f"{mtd} has no device-tree node")
    if os.path.realpath(of_node) != region.node:
        raise Refused(f"{mtd} is not bound to the region {os.path.basename(region.node)}")
    return Binding(real, st.st_rdev, mtd, region)


def _read_exact(fd, size):
    chunks = []
    left = size
    while left:
        chunk = os.read(fd, min(left, 1 << 20))
        if not chunk:
            raise OSError(f"short read: {size - left} of {size} bytes")
        chunks.append(chunk)
        left -= len(chunk)
    return b"".join(chunks)


def read_device(binding, host):
    fd = os.open(binding.dev, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        st = host.fstat(fd)
        if not stat.S_ISCHR(st.st_mode) or st.st_rdev != binding.rdev:
            raise Refused(f"{binding.dev} changed while it was opened")
        return _read_exact(fd, binding.region.size)
    finally:
        os.close(fd)


def read_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise Refused(f"{path} is not a regular file")
        if st.st_size > MAX_FILE_BYTES:
            raise Refused(f"{path} has {st.st_size} bytes, more than an ADT ({MAX_FILE_BYTES})")
        return _read_exact(fd, st.st_size)
    finally:
        os.close(fd)


def _is_regular_file(path, host):
    try:
        return stat.S_ISREG(host.stat(os.path.realpath(path)).st_mode)
    except OSError:
        return False


# --- Main ----------------------------------------------------------------------------------------

def main(argv=None, host=None, stdout=None, stderr=None):
    host = host or Host()
    stdout = stdout or sys.stdout
    stderr = stderr or sys.stderr
    parser = argparse.ArgumentParser(
        prog=PROG, description="Print an allowlist of the boot loader's copy of the Apple device tree.")
    parser.add_argument("--check", action="store_true",
                        help="check the device and what it is bound to; read nothing")
    parser.add_argument("path", nargs="?",
                        help="/dev/mtdNro, or a regular file holding an ADT (default: the read-only "
                             "node of the MTD device named adt)")
    args = parser.parse_args(argv)

    def fail(code, what):
        print(f"{PROG}: {what}", file=stderr)
        return code

    try:
        if args.path is not None and _is_regular_file(args.path, host):
            if args.check:
                print(f"{args.path}: a regular file, no device to check", file=stdout)
                return 0
            real = os.path.realpath(args.path)
            data = read_file(real)
            source = f"file {os.path.basename(args.path)} ({len(data)} bytes)"
        else:
            binding = check_binding(args.path, host)
            if args.check:
                print(binding.describe(), file=stdout)
                return 0
            data = read_device(binding, host)
            source = binding.describe()
        root, used, nodes, props = parse_adt(data)
        text = render(root, source, used, nodes, props)
    except Refused as error:
        return fail(3, f"refused: {error}")
    except AdtError as error:
        return fail(4, f"not a valid ADT: {error}")
    except PermissionError as error:
        return fail(1, f"{error.strerror}: {error.filename or ''} (reading the device needs root)")
    except OSError as error:
        return fail(1, f"I/O error: {error}")
    except Exception as error:  # noqa: BLE001 - one line, never a traceback
        return fail(5, f"internal error ({type(error).__name__}); nothing was printed")
    stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
