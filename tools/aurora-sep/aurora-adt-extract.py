#!/usr/bin/env python3
"""Print an allowlist of the boot loader's copy of the Apple device tree (ADT).

m1n1 keeps the ADT in a reserved-memory region with compatible "phram" and label "adt". With the
phram module loaded, Linux shows that region as an MTD device named adt. This tool reads it
through the read-only node /dev/mtdNro, or reads a regular file (tests, offline copies), and
prints only:
  - the nodes under ALLOWED_SUBTREES (GPU, ANE, display, PMP, power manager, CPU clusters), and
  - the properties named in LIMITED_NODES of a few other nodes (/, /arm-io, /product, and the
    reg of the memory cache controller).
Every other node is left out. Inside the printed nodes it drops every property whose name
contains one of DENIED_NAME_PARTS, and every property whose value contains the value of such a
property anywhere in the ADT. The one exception is the uuid of a firmware processor's nub
(iop-*-nub*, FIRMWARE_UUID_NODES): it names the firmware image the boot loader loaded, which is
the same on every Mac with that firmware, and it is printed when it has the form of a UUID.
Numbers are printed in hex. The last line sums up what was dropped.

With --stage2-log it reads m1n1's log of this boot instead (the reserved-memory region labelled
m1n1_stage2.log, which phram shows as the MTD device of that name), with the same checks, and
prints it as text, leaving out every line that names one of LOG_DENIED_PARTS. With --raw as well
it writes the whole region as it is, byte for byte, except that each such line is overwritten
with x (same length), so a ring's layout is kept.

It never writes: every device and file is opened O_RDONLY. Before it reads a device it checks
that the device is the read-only node of the MTD device named adt, and that this MTD device is
the running device tree's reserved-memory region labelled adt, with the same size. It reads at
most MAX_ADT_BYTES, from a device or a file. The output is built in memory and printed only when
everything succeeded.

Usage:
  aurora-adt-extract.py [PATH]          print the allowlist (as root, for a device)
  aurora-adt-extract.py --check [PATH]  check the device and what it is bound to; read nothing
  aurora-adt-extract.py --stage2-log [--check] [--raw] [PATH]
                                        the same for m1n1's stage 2 log (--raw: all of it)
PATH is /dev/mtdNro or a regular file holding an ADT (or the log). Without PATH: the read-only
node of the MTD device named adt (or m1n1_stage2.log).

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
    # Neural engine (ANE), matched by name so a chip this tool has not seen still prints it: the
    # engine (ane, ane0, ane1: windows, interrupts, clock and power gates, iommu-parent, and any
    # iop-*-nub or ascwrap child with its mailbox and segments), its DART (dart-ane*: stream ids,
    # VM range) and the DART's address mapper (mapper-ane*). "ans" is the storage controller and
    # is not matched.
    "/arm-io/ane*",
    "/arm-io/dart-ane*",
    "/arm-io/mapper-ane*",
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
    # The memory cache controller's register windows (the GPU firmware's IO map slot for it).
    "/arm-io/mcc": ("reg",),
}

# Nodes whose "uuid" property is printed: a firmware processor's nub names the firmware image the
# boot loader loaded (gfx, dcp, dcpext0-3, pmp). Matched against the node's own name, inside the
# allowed subtrees, and only when the value is one UUID string (FIRMWARE_UUID_RE).
FIRMWARE_UUID_NODES = ("iop-*-nub*",)
FIRMWARE_UUID_RE = re.compile(rb"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\0")

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

# Lines of m1n1's log left out, matched as case-insensitive parts of the line: what may tell one
# Mac from another. A line that names a uuid is kept when it is about a firmware image
# (LOG_FIRMWARE_PARTS): those UUIDs are the same on every Mac with that firmware.
LOG_DENIED_PARTS = (
    "serial", "ecid", "udid", "uuid", "nonce", "mac-address", "mac address", "macaddr", "unique",
    "imei", "seed", "entropy", "cert", "random",
)
LOG_FIRMWARE_PARTS = ("firmware", "image")
# Log lines longer than this are cut (the rest is left out).
MAX_LOG_LINE = 512

# --- Where the device is ---------------------------------------------------------------------

MTD_CHAR_MAJOR = 90
ADT_LABEL = "adt"
LOG_LABEL = "m1n1_stage2.log"
# The most this reads, from a device region or a file. m1n1 sizes the adt region as the ADT rounded
# up to 16 KiB, and the ADT is well under 1 MiB (the M3 Pro's region is 0x7c000 bytes), so 16 MiB
# leaves wide room while bounding the memory a bad device tree or file can make this use.
MAX_ADT_BYTES = 16 * 1024 * 1024
# The most it reads of m1n1's log (its region is 16 KiB on every M3 so far).
MAX_LOG_BYTES = 1024 * 1024

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


def firmware_uuid(path, prop):
    """Whether prop is the uuid of a firmware processor's nub (FIRMWARE_UUID_NODES), with one UUID
    string as its value: the firmware image's identity. It is printed when its node is (an allowed
    subtree), and it is never a secret for the value check, wherever its nub is."""
    if prop.name != "uuid":
        return False
    segs = _segments(path)
    if not segs or not any(fnmatch.fnmatchcase(segs[-1], p) for p in FIRMWARE_UUID_NODES):
        return False
    return FIRMWARE_UUID_RE.fullmatch(prop.value) is not None


def _secret_values(root):
    """The values of every denied property anywhere in the ADT that are long and varied enough to
    look for in other values. A firmware image's uuid (firmware_uuid) is not one."""
    secrets = set()
    for path, node in walk(root):
        for prop in node.props:
            if denied_part(prop.name) is None or firmware_uuid(path, prop):
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
    firmware_uuids = 0
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
            if part is not None and firmware_uuid(path, prop):
                part = None
                firmware_uuids += 1
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
        f"# summary: printed {shown_nodes} nodes and {shown_props} properties "
        f"({firmware_uuids} firmware image uuids); dropped "
        f"{by_name + by_value} properties ({by_name} by name: {parts}; {by_value} by value); "
        f"omitted {too_large} larger than {MAX_PRINTED_VALUE // 1024} KiB; not printed: "
        f"{hidden_nodes} nodes outside the allowlist and {limited_left_out} properties of "
        f"limited nodes")
    return "\n".join(out) + "\n"


def log_denied_part(line):
    """The LOG_DENIED_PARTS entry a log line (text) names, or None."""
    low = line.lower()
    part = next((p for p in LOG_DENIED_PARTS if p in low), None)
    if part == "uuid" and any(f in low for f in LOG_FIRMWARE_PARTS) and \
            next((p for p in LOG_DENIED_PARTS if p != "uuid" and p in low), None) is None:
        return None
    return part


def raw_log(data):
    """m1n1's log region as bytes, its length kept: each line that log_denied_part names is
    overwritten with x, its newline kept."""
    out = []
    for line in bytes(data).split(b"\n"):
        text = line.decode("latin-1")
        out.append(b"x" * len(line) if log_denied_part(text) else line)
    return b"\n".join(out)


def render_log(data, source):
    """m1n1's stage 2 log as text, one string: the region's bytes up to its trailing NULs, every
    other byte that is not printable ASCII, a tab or a newline shown as '.', lines longer than
    MAX_LOG_LINE cut, and every line naming one of LOG_DENIED_PARTS left out."""
    text = bytes(data).rstrip(b"\0")
    chars = "".join(chr(b) if (0x20 <= b <= 0x7E or b in (0x09, 0x0A)) else "." for b in text)
    out = [f"# {PROG}: m1n1's stage 2 log of this boot ({LOG_LABEL}); read-only",
           f"# source: {source}"]
    kept = dropped = cut = 0
    by_part = {}
    for line in chars.split("\n"):
        part = log_denied_part(line)
        if part is not None:
            dropped += 1
            by_part[part] = by_part.get(part, 0) + 1
            continue
        if len(line) > MAX_LOG_LINE:
            line = line[:MAX_LOG_LINE] + " (cut)"
            cut += 1
        out.append(line)
        kept += 1
    parts = ", ".join(f"{part} {n}" for part, n in sorted(by_part.items())) or "none"
    out.append(f"# summary: printed {kept} lines of {len(text)} bytes; left out {dropped} lines "
               f"({parts}); cut {cut} longer than {MAX_LOG_LINE} characters")
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
    def __init__(self, dev, rdev, mtd, region, label=ADT_LABEL):
        self.dev = dev
        self.rdev = rdev
        self.mtd = mtd
        self.region = region
        self.label = label

    def describe(self):
        return (f"{self.dev}: MTD device {self.mtd} named {self.label!r}, {self.region.size} bytes, "
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


def _limit(label):
    """The most this reads of the region labelled label, and what the region holds."""
    return (MAX_LOG_BYTES, "a log") if label == LOG_LABEL else (MAX_ADT_BYTES, "an ADT")


def find_region(host, label=ADT_LABEL):
    """The running device tree's reserved-memory region labelled label (adt, or m1n1_stage2.log),
    with compatible phram."""
    limit, holds = _limit(label)
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
        if b"phram" in compatible and (_dt_prop(node, "label") or b"").rstrip(b"\0") == label.encode():
            found.append(node)
    if not found:
        raise Refused(f"the device tree has no reserved-memory region labelled {label} (this boot's "
                      "m1n1 reserved none)")
    if len(found) > 1:
        raise Refused(f"the device tree has {len(found)} reserved-memory regions labelled {label}")
    node = found[0]
    status = (_dt_prop(node, "status") or b"okay").rstrip(b"\0")
    if status not in (b"okay", b"ok"):
        raise Refused(f"the {label} region {os.path.basename(node)} is not enabled")
    reg = _dt_prop(node, "reg")
    if reg is None or len(reg) != 4 * (ac + sc):
        raise Refused(f"the {label} region {os.path.basename(node)} does not have exactly one reg entry")
    cells = struct.unpack(f">{ac + sc}I", reg)
    base = 0
    for c in cells[:ac]:
        base = (base << 32) | c
    size = 0
    for c in cells[ac:]:
        size = (size << 32) | c
    if size == 0:
        raise Refused(f"the {label} region {os.path.basename(node)} has size 0")
    if size > limit:
        raise Refused(f"the {label} region {os.path.basename(node)} has {size} bytes, more than {holds} "
                      f"({limit})")
    return Region(os.path.realpath(node), base, size)


def _sys_attr(path):
    try:
        return _read_small(path).decode("ascii", "replace").strip()
    except OSError:
        return None


def find_adt_mtd(host, label=ADT_LABEL):
    """The name (mtdN) of the one MTD device named label (adt, or m1n1_stage2.log)."""
    cls = os.path.join(host.sys_root, "class", "mtd")
    names = []
    if os.path.isdir(cls):
        for entry in sorted(os.listdir(cls)):
            if re.fullmatch(r"mtd[0-9]+", entry) and _sys_attr(os.path.join(cls, entry, "name")) == label:
                names.append(entry)
    if not names:
        raise Refused(f"no MTD device is named {label} (is the phram module loaded?)")
    if len(names) > 1:
        raise Refused(f"{len(names)} MTD devices are named {label}: {' '.join(names)}")
    return names[0]


def check_binding(path, host, label=ADT_LABEL):
    """Checks that path (default: the read-only node of the MTD device named label) is the read-only
    node of the MTD device named label (adt, or m1n1_stage2.log), and that this device is the device
    tree's region of that label. Returns a Binding; raises Refused."""
    region = find_region(host, label)
    if path is None:
        path = os.path.join(host.dev_root, find_adt_mtd(host, label) + "ro")
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
    if name != label:
        raise Refused(f"{path} belongs to {mtd}, named {name!r}, not {label!r}")
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
    return Binding(real, st.st_rdev, mtd, region, label)


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
    limit, holds = _limit(binding.label)
    if not 0 < binding.region.size <= limit:
        raise Refused(f"the {binding.label} region has {binding.region.size} bytes, more than {holds} ({limit})")
    fd = os.open(binding.dev, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        st = host.fstat(fd)
        if not stat.S_ISCHR(st.st_mode) or st.st_rdev != binding.rdev:
            raise Refused(f"{binding.dev} changed while it was opened")
        return _read_exact(fd, binding.region.size)
    finally:
        os.close(fd)


def read_file(path, label=ADT_LABEL):
    limit, holds = _limit(label)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise Refused(f"{path} is not a regular file")
        if st.st_size > limit:
            raise Refused(f"{path} has {st.st_size} bytes, more than {holds} ({limit})")
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
    parser.add_argument("--stage2-log", action="store_true",
                        help="read m1n1's stage 2 log of this boot (the m1n1_stage2.log region) "
                             "instead of the ADT")
    parser.add_argument("--raw", action="store_true",
                        help="with --stage2-log: write the whole region, with the lines that may "
                             "name this Mac overwritten")
    parser.add_argument("path", nargs="?",
                        help="/dev/mtdNro, or a regular file holding an ADT (default: the read-only "
                             "node of the MTD device named adt, or m1n1_stage2.log)")
    args = parser.parse_args(argv)
    label = LOG_LABEL if args.stage2_log else ADT_LABEL
    if args.raw and not args.stage2_log:
        parser.error("--raw goes with --stage2-log")

    def fail(code, what):
        print(f"{PROG}: {what}", file=stderr)
        return code

    try:
        if args.path is not None and _is_regular_file(args.path, host):
            if args.check:
                print(f"{args.path}: a regular file, no device to check", file=stdout)
                return 0
            real = os.path.realpath(args.path)
            data = read_file(real, label)
            source = f"file {os.path.basename(args.path)} ({len(data)} bytes)"
        else:
            binding = check_binding(args.path, host, label)
            if args.check:
                print(binding.describe(), file=stdout)
                return 0
            data = read_device(binding, host)
            source = binding.describe()
        if args.stage2_log and args.raw:
            text = raw_log(data)
        elif args.stage2_log:
            text = render_log(data, source)
        else:
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
    if isinstance(text, bytes):
        out = getattr(stdout, "buffer", None)
        if out is None:
            stdout.write(text.decode("latin-1"))
        else:
            stdout.flush()
            out.write(text)
            out.flush()
        return 0
    stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
