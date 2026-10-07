"""aurora-adt-extract.py: the ADT allowlist reader.

Every ADT here is synthetic. build_m3max_adt() makes an ADT shaped like an M3 Max's (the node
names the reader looks for, a 14-inch t6031 or t6034 board), with placeholder values: none of
its numbers describe M3 Max hardware. It plants sensitive properties (serial numbers, MAC
addresses, chip ids, nonces, keys, calibration, UUIDs) inside and outside the allowlist, and
marker strings in nodes outside it, and the tests check that none of them is ever printed.

The device tests use a fake /sys, device tree and /dev in a temporary directory, and a fake
stat() for the character device. Nothing here needs root or an Apple Mac.
"""
from pathlib import Path
import importlib.util
import io
import os
import random
import re
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / "aurora-adt-extract.py"

spec = importlib.util.spec_from_file_location("aurora_adt_extract", TOOL)
adt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adt)


# --- Building ADTs ---------------------------------------------------------------------------

def prop(name, value, placeholder=False):
    if isinstance(value, str):
        value = value.encode() + b"\0"
    elif isinstance(value, int):
        value = struct.pack("<I", value)
    elif isinstance(value, (list, tuple)):
        value = struct.pack(f"<{len(value)}I", *value)
    raw_name = name.encode()
    assert len(raw_name) < 32
    size = len(value) | (0x80000000 if placeholder else 0)
    return (raw_name.ljust(32, b"\0") + struct.pack("<I", size) + value
            + b"\0" * ((-len(value)) & 3))


def node(name, props=(), children=()):
    props = [prop("name", name)] + list(props)
    return struct.pack("<II", len(props), len(children)) + b"".join(props) + b"".join(children)


# Values that must never be printed. Each is long and varied enough for the value check.
SERIAL = "FAKESERIAL0123"
MLB_SERIAL = "FAKEMLBSERIAL98765"
CHIP_ID = bytes.fromhex("8a1c3e5b7d9f0246")
WIFI_MAC = bytes.fromhex("a1b2c3d4e5f6")
BT_MAC = bytes.fromhex("0a1b2c3d4e5f")
NONCE = bytes.fromhex("5e6f7a8b9c0d1e2f3a4b5c6d")
KEY = bytes(range(0x40, 0x60))
CALIBRATION = bytes(range(0x90, 0xd0))
FW_UUID = "11111111-2222-3333-4444-555555555555"
PANEL_SERIAL = "FAKEPANEL24680"
SECRETS = [SERIAL.encode(), MLB_SERIAL.encode(), CHIP_ID, WIFI_MAC, BT_MAC, NONCE, KEY, CALIBRATION,
           FW_UUID.encode(), PANEL_SERIAL.encode()]
# Strings that appear only in nodes outside the allowlist.
OUTSIDE = ["OUTSIDE-wlan", "OUTSIDE-bluetooth", "OUTSIDE-sep", "OUTSIDE-smc", "OUTSIDE-chosen",
           "OUTSIDE-nvme", "OUTSIDE-root-model", "OUTSIDE-product-color"]
SENSITIVE_NAMES = ["serial-number", "mlb-serial-number", "unique-chip-id", "mac-address-wifi0",
                   "local-mac-address", "boot-nonce", "random-seed", "sep-key", "gpu-calibration",
                   "panel-serial-number", "region-info", "ecid", "udid", "imei", "uuid",
                   "wifi-antenna-sku", "bluetooth-taurus", "fuse-revision-row", "device-cert",
                   "entropy-pool"]

BOARDS = {"t6031": ("J514cAP", "Mac15,6"), "t6034": ("J514mAP", "Mac15,8")}


def build_m3max_adt(chip="t6031"):
    """An ADT shaped like a 14-inch M3 Max's. Placeholder values only."""
    target, model = BOARDS[chip]
    cpus = [node(f"cpu{i}", [prop("cpu-id", i), prop("cluster-id", i // 4),
                             prop("cluster-type", "E" if i < 4 else "P"),
                             prop("reg", [0x10000 + i, 0]), prop("compatible", "apple,cpu")])
            for i in range(16)]
    sgx = node("sgx", [
        prop("compatible", f"gpu,{chip}"),
        prop("reg", [0x11110000, 0x1, 0x01000000, 0x0, 0x22220000, 0x1, 0x00010000, 0x0]),
        prop("interrupts", [1, 2, 3, 4, 5, 6, 7]),
        prop("gpu-num-cores", 0x28),
        prop("perf-states", [0x10, 0x20, 0x30, 0x40, 0x50, 0x60]),
        prop("perf-state-count", 3),
        prop("gfx-shared-region-base", [0x33330000, 0x1]),
        prop("gpu-calibration", CALIBRATION),
        prop("unique-id", CHIP_ID),
        # an ordinary-looking name whose value holds the serial number: dropped by value
        prop("diag-info", "board " + SERIAL + " rev 2"),
        prop("unused-slot", b"\0" * 8, placeholder=True),
    ])
    gfx_asc = node("gfx-asc", [prop("compatible", "iop,ascwrap-v6"),
                               prop("reg", [0x44440000, 0x1, 0x4000, 0x0])],
                   [node("iop-gfx-nub", [prop("segment-names", b"__TEXT\0__DATA\0"),
                                         prop("segment-ranges", [0x1000, 0, 0x2000, 0]),
                                         prop("uuid", FW_UUID)])])
    pmgr = node("pmgr", [prop("compatible", "pmgr1,t6031"),
                         prop("ps-regs", [0, 0x1000, 0x4000, 1, 0x2000, 0x4000]),
                         prop("voltage-states1", [0x100, 0x200, 0x300, 0x400]),
                         prop("voltage-states5", [0x500, 0x600, 0x700, 0x800]),
                         prop("fuse-revision-row", [0xabcdef01, 0x23456789])])
    pmp = node("pmp", [prop("compatible", "pmp,t6031"), prop("reg", [0x55550000, 0x1, 0x4000, 0])],
               [node("iop-pmp-nub", [prop("ptd-range", [0, 0x100, 0x200, 0x300]),
                                     prop("uuid", FW_UUID)])])
    dcp = node("dcp", [prop("compatible", "dcp,t6031"), prop("reg", [0x66660000, 0x1, 0x4000, 0])],
               [node("iop-dcp-nub", [prop("segment-names", b"__TEXT\0__OS_LOG\0"),
                                     prop("asc-dram-mask", [0, 0]),
                                     prop("panel-serial-number", PANEL_SERIAL),
                                     prop("uuid", FW_UUID)])])
    dcpext = [node(f"dcpext{i}", [prop("compatible", "dcpext,t6031"),
                                  prop("reg", [0x67670000 + i * 0x10000, 0x1, 0x4000, 0])])
              for i in range(4)]
    disp = [node("disp0", [prop("compatible", "disp0,t6031"), prop("clock-gates", [1, 2])])]
    disp += [node(f"dispext{i}", [prop("compatible", "dispext,t6031"), prop("power-gates", [i])])
             for i in range(4)]
    darts = [node("dart-disp0", [prop("vm-base", [0, 0]), prop("vm-size", [0, 0x10])],
                  [node("mapper-disp0", [prop("reg", 0)])]),
             node("dart-dcp", [prop("real-time", 1)], [node("mapper-dcp", [prop("reg", 5)])]),
             node("dart-dcpext0", [prop("real-time", 1)]),
             node("dart-dispext0", [prop("vm-size", [0, 0x20])]),
             node("dart-pmp", [prop("reg", [0x77770000, 0x1, 0x4000, 0])]),
             node("mapper-gfx-asc", [prop("reg", 7)])]
    outside = [
        node("wlan", [prop("local-mac-address", WIFI_MAC), prop("compatible", "OUTSIDE-wlan")]),
        node("bluetooth", [prop("local-mac-address", BT_MAC), prop("model", "OUTSIDE-bluetooth")]),
        node("sep", [prop("sep-key", KEY), prop("compatible", "OUTSIDE-sep")]),
        node("smc", [prop("compatible", "OUTSIDE-smc"), prop("random-seed", NONCE)]),
        node("nvme", [prop("compatible", "OUTSIDE-nvme")]),
        # a node whose name looks like an allowed one at the wrong depth
        node("pmgr-mini", [prop("compatible", "OUTSIDE-smc")]),
    ]
    arm_io = node("arm-io", [
        prop("compatible", f"arm-io,{chip}"), prop("device_type", "soc"),
        prop("#address-cells", 2), prop("#size-cells", 2),
        prop("ranges", [0, 0x2, 0, 0x2, 0, 0x2]),
        prop("chip-revision", 0x11),
        prop("ecid", CHIP_ID), prop("debug-name", "OUTSIDE-root-model"),
    ], [sgx, gfx_asc, pmgr, pmp, dcp] + dcpext + disp + darts + outside)
    chosen = node("chosen", [prop("unique-chip-id", CHIP_ID), prop("boot-nonce", NONCE),
                             prop("mac-address-wifi0", WIFI_MAC), prop("board-id", 0x99),
                             prop("debug-tag", "OUTSIDE-chosen"), prop("udid", "OUTSIDE-chosen")])
    product = node("product", [prop("product-name", "MacBook Pro (14-inch, Nov 2023)"),
                               prop("product-description", "MacBook Pro (14-inch, Nov 2023)"),
                               prop("device-color", "OUTSIDE-product-color"),
                               prop("serial-number", SERIAL), prop("imei", "OUTSIDE-product-color")])
    root = node("device-tree", [
        prop("#address-cells", 2), prop("#size-cells", 2),
        prop("compatible", b"%s\0AppleARM\0" % target.encode()),
        prop("model", model), prop("target-type", "OUTSIDE-root-model"),
        prop("serial-number", SERIAL), prop("mlb-serial-number", MLB_SERIAL),
        prop("region-info", "LL/A FAKE"),
        prop("wifi-antenna-sku", "OUTSIDE-wlan"), prop("bluetooth-taurus", "OUTSIDE-bluetooth"),
        prop("device-cert", KEY), prop("entropy-pool", NONCE),
    ], [chosen, arm_io, node("cpus", [prop("#address-cells", 2)], cpus), product])
    return root


def padded(blob, size=0x40000):
    """As in the reserved region: the ADT, then zeros to the region's size."""
    return blob + b"\0" * (size - len(blob))


def render(blob):
    root, used, nodes, props = adt.parse_adt(blob)
    return adt.render(root, "test", used, nodes, props)


def run(argv, host=None):
    out, err = io.StringIO(), io.StringIO()
    code = adt.main(argv, host=host, stdout=out, stderr=err)
    return code, out.getvalue(), err.getvalue()


# --- Parsing and the allowlist ---------------------------------------------------------------

class AllowlistTest(unittest.TestCase):
    def check_private(self, text):
        hexes = []
        for secret in SECRETS:
            self.assertNotIn(secret.decode("latin-1"), text)
            hexes.append(secret.hex())
            if len(secret) % 4 == 0:
                words = struct.unpack(f"<{len(secret) // 4}I", secret)
                for w in words:
                    if w not in (0,):
                        self.assertNotIn(f"0x{w:08x}", text, secret)
        flat = re.sub(r"[^0-9a-f]", "", text.lower())
        for h in hexes:
            self.assertNotIn(h, flat)
        for marker in OUTSIDE:
            self.assertNotIn(marker, text)
        printed = re.findall(r"^  (\S+) \[", text, re.M)
        for name in printed:
            self.assertIsNone(adt.denied_part(name), name)
        for name in SENSITIVE_NAMES:
            self.assertNotIn(name, printed)

    def test_m3max_shapes(self):
        for chip in BOARDS:
            with self.subTest(chip=chip):
                text = render(padded(build_m3max_adt(chip)))
                self.check_private(text)
                nodes = re.findall(r"^(/\S*)$", text, re.M)
                self.assertEqual(nodes[:3], ["/", "/arm-io", "/arm-io/sgx"])
                for want in ["/arm-io/gfx-asc/iop-gfx-nub", "/arm-io/pmgr", "/arm-io/pmp/iop-pmp-nub",
                             "/arm-io/dcp/iop-dcp-nub", "/arm-io/dcpext3", "/arm-io/disp0",
                             "/arm-io/dispext3", "/arm-io/dart-disp0/mapper-disp0",
                             "/arm-io/dart-dcp/mapper-dcp", "/arm-io/dart-dcpext0", "/arm-io/dart-pmp",
                             "/arm-io/mapper-gfx-asc", "/cpus", "/cpus/cpu15", "/product"]:
                    self.assertIn(want, nodes)
                for no in ["/chosen", "/arm-io/wlan", "/arm-io/bluetooth", "/arm-io/sep", "/arm-io/smc",
                           "/arm-io/nvme", "/arm-io/pmgr-mini"]:
                    self.assertNotIn(no, nodes)
                self.assertIn(f'  compatible [10] = "gpu,{chip}"', text)
                self.assertIn("  gpu-num-cores [4] = <0x00000028>", text)
                self.assertIn("  voltage-states5 [16] = <0x00000500 0x00000600 0x00000700 0x00000800>",
                              text)
                self.assertIn("  unused-slot [8] (placeholder) = <0x00000000 0x00000000>", text)
                self.assertIn('  segment-names [14] = "__TEXT", "__DATA"', text)

    def test_limited_nodes(self):
        text = render(build_m3max_adt())
        block = lambda path: re.search(rf"^{re.escape(path)}\n((?:  .*\n)*)", text, re.M).group(1)
        self.assertEqual(re.findall(r"^  (\S+)", block("/"), re.M), ["#address-cells", "#size-cells"])
        self.assertEqual(re.findall(r"^  (\S+)", block("/product"), re.M),
                         ["name", "product-name", "product-description"])
        self.assertEqual(re.findall(r"^  (\S+)", block("/arm-io"), re.M),
                         ["name", "compatible", "device_type", "#address-cells", "#size-cells",
                          "ranges", "chip-revision"])

    def test_dropped_and_summary(self):
        text = render(build_m3max_adt())
        self.assertIn("# dropped: /arm-io/sgx gpu-calibration (name contains 'calibration')", text)
        self.assertIn("# dropped: /arm-io/sgx unique-id (name contains 'unique')", text)
        self.assertIn("# dropped: /arm-io/sgx diag-info (value holds a dropped property's value)", text)
        self.assertIn("# dropped: /arm-io/dcp/iop-dcp-nub uuid (name contains 'uuid')", text)
        self.assertIn("# dropped: /arm-io/pmgr fuse-revision-row (name contains 'fuse')", text)
        summary = text.rstrip("\n").splitlines()[-1]
        self.assertTrue(summary.startswith("# summary: printed "), summary)
        self.assertRegex(summary, r"dropped 8 properties \(7 by name: calibration 1, fuse 1, "
                                  r"serial 1, unique 1, uuid 3; 1 by value\)")
        self.assertRegex(summary, r"not printed: \d+ nodes outside the allowlist and \d+ properties")

    def test_value_check_needs_long_varied_values(self):
        # a short or flat denied value does not knock out ordinary data
        root = node("device-tree", [prop("#address-cells", 2), prop("key-flag", 1),
                                    prop("serial-pad", b"\0" * 16)],
                    [node("arm-io", [], [node("sgx", [prop("count", 1), prop("zeros", b"\0" * 16)])])])
        text = render(root)
        self.assertIn("  count [4] = <0x00000001>", text)
        self.assertIn("  zeros [16] = <0x00000000 0x00000000 0x00000000 0x00000000>", text)

    def test_large_value_omitted(self):
        root = node("device-tree", [], [node("arm-io", [], [node("sgx", [prop("big", b"\x01" * 70000)])])])
        text = render(root)
        self.assertIn("  big [70000] (omitted: larger than 64 KiB)", text)
        self.assertNotIn("0x01010101", text)

    def test_odd_sizes_and_strings(self):
        self.assertEqual(adt.format_value(b""), "<>")
        self.assertEqual(adt.format_value(b"\x01\x02\x03"), "[01 02 03]")
        self.assertEqual(adt.format_value(b"AB\0\0"), "<0x00004241>")
        self.assertEqual(adt.format_value(b'a"b\0c\0'), '"a\\"b", "c"')

    def test_patterns(self):
        self.assertTrue(adt.in_allowed_subtree("/arm-io/dispext2/x"))
        self.assertTrue(adt.in_allowed_subtree("/arm-io/dart-dcpext1"))
        self.assertFalse(adt.in_allowed_subtree("/arm-io"))
        self.assertFalse(adt.in_allowed_subtree("/arm-io/pmgr-mini"))
        self.assertFalse(adt.in_allowed_subtree("/sgx"))
        self.assertFalse(adt.in_allowed_subtree("/chosen/arm-io/sgx"))

    def test_node_names_with_a_slash_are_not_matched(self):
        root = node("device-tree", [], [node("arm-io", [], [node("sgx/evil", [prop("x", "y")])])])
        text = render(root)
        self.assertNotIn("/arm-io/sgx", text)
        self.assertNotIn('"y"', text)


# --- Malformed input -------------------------------------------------------------------------

class MalformedTest(unittest.TestCase):
    def refused(self, blob, msg=None):
        with self.assertRaises(adt.AdtError) as ctx:
            adt.parse_adt(blob)
        if msg:
            self.assertIn(msg, str(ctx.exception))

    def test_cases(self):
        good = build_m3max_adt()
        self.refused(b"", "truncated")
        self.refused(b"\0" * 4, "truncated")
        self.refused(b"\0" * 64, "root node is empty")
        self.refused(struct.pack("<II", 1, 0), "properties, more than the data holds")
        self.refused(struct.pack("<II", 0, 0xFFFFFFFF) + b"\0" * 64, "children, more than")
        self.refused(struct.pack("<II", 0xFFFFFFFF, 0) + b"\0" * 64, "properties, more than")
        self.refused(struct.pack("<II", 1, 0) + b"x" * 32 + struct.pack("<I", 0), "no terminating NUL")
        self.refused(struct.pack("<II", 1, 0) + b"\0" * 32 + struct.pack("<I", 0), "is empty")
        self.refused(struct.pack("<II", 1, 0) + b"n\x01m".ljust(32, b"\0") + struct.pack("<I", 0),
                     "not printable")
        self.refused(struct.pack("<II", 1, 0) + b"name".ljust(32, b"\0") + struct.pack("<I", 100)
                     + b"abc\0", "run past the end")
        self.refused(struct.pack("<II", 1, 0) + b"name".ljust(32, b"\0") + struct.pack("<I", 0x7FFFFFFF),
                     "run past the end")
        self.refused(struct.pack("<II", 1, 0) + b"name".ljust(32, b"\0") + struct.pack("<I", 3) + b"abc",
                     "padding")
        self.refused(good[:-1], None)
        self.refused(good[:len(good) // 2], None)

    def test_depth_bomb(self):
        blob = node("leaf")
        for _ in range(100):
            blob = node("n", children=[blob])
        self.refused(blob, "deeper than 64")

    def test_truncations_and_flips_never_crash(self):
        good = build_m3max_adt()
        cases = [good[:n] for n in range(0, len(good), 7)]
        rng = random.Random(20261007)
        for _ in range(600):
            b = bytearray(good)
            for _ in range(rng.randint(1, 8)):
                b[rng.randrange(len(b))] = rng.randrange(256)
            cases.append(bytes(b))
        for i in range(200):
            b = bytearray(good)
            pos = rng.randrange(0, len(b) - 4) & ~3
            b[pos:pos + 4] = struct.pack("<I", rng.choice([0, 1, 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000,
                                                           rng.randrange(1 << 32)]))
            cases.append(bytes(b))
        parsed = 0
        for blob in cases:
            try:
                root, used, nodes, props = adt.parse_adt(blob)
            except adt.AdtError:
                continue
            parsed += 1
            adt.render(root, "fuzz", used, nodes, props)
        self.assertGreater(parsed, 0)

    def test_cli_refuses_cleanly(self):
        good = build_m3max_adt()
        with tempfile.TemporaryDirectory() as tmp:
            for i, blob in enumerate([b"", good[:100], b"\xff" * 4096, good[:-3]]):
                path = Path(tmp, f"bad{i}.bin")
                path.write_bytes(blob)
                proc = subprocess.run([sys.executable, str(TOOL), str(path)], capture_output=True, text=True)
                self.assertEqual(proc.returncode, 4, proc.stderr)
                self.assertEqual(proc.stdout, "")
                self.assertEqual(len(proc.stderr.splitlines()), 1, proc.stderr)
                self.assertTrue(proc.stderr.startswith("aurora-adt-extract: not a valid ADT: "))
                self.assertNotIn("Traceback", proc.stderr)


# --- Files and the command line --------------------------------------------------------------

class FileTest(unittest.TestCase):
    def test_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "adt.bin")
            path.write_bytes(padded(build_m3max_adt("t6034")))
            proc = subprocess.run([sys.executable, str(TOOL), str(path)], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stderr, "")
            self.assertIn("# source: file adt.bin (262144 bytes)", proc.stdout)
            self.assertNotIn(tmp, proc.stdout)
            self.assertIn('  compatible [10] = "gpu,t6034"', proc.stdout)
            AllowlistTest.check_private(self, proc.stdout)
            proc = subprocess.run([sys.executable, str(TOOL), "--check", str(path)], capture_output=True,
                                  text=True)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("a regular file, no device to check", proc.stdout)

    def test_never_opens_for_writing(self):
        source = TOOL.read_text()
        for bad in ["O_WRONLY", "O_RDWR", "O_CREAT", "O_TRUNC", "O_APPEND", "open("]:
            self.assertNotIn(bad, source.replace("os.open(", "").replace("_read_small(", ""), bad)
        seen = []
        real_open = os.open

        def guarded(path, flags, *a, **k):
            seen.append(flags)
            assert flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC) == 0
            return real_open(path, flags, *a, **k)

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "adt.bin")
            path.write_bytes(build_m3max_adt())
            adt.os.open = guarded
            try:
                code, out, err = run([str(path)])
            finally:
                adt.os.open = real_open
        self.assertEqual(code, 0, err)
        self.assertTrue(seen)


# --- The device and its region ----------------------------------------------------------------

class FakeMac:
    """A fake /sys, device tree and /dev: nvram as mtd0, the adt region as mtd1."""

    def __init__(self, tmp, size=0x7c000, base=0x10003528000):
        self.root = Path(os.path.realpath(tmp))
        self.sys = self.root / "sys"
        self.dt = self.sys / "firmware/devicetree/base"
        self.dev = self.root / "dev"
        self.rdevs = {}
        rm = self.dt / "reserved-memory"
        rm.mkdir(parents=True)
        (rm / "#address-cells").write_bytes(struct.pack(">I", 2))
        (rm / "#size-cells").write_bytes(struct.pack(">I", 2))
        self.region = self.add_region("flash@%x" % base, b"phram\0", b"adt\0", base, size)
        self.add_region("flash@108d994c000", b"phram\0", b"m1n1_stage2.log\0", 0x108d994c000, 0x4000)
        self.dev.mkdir()
        self.add_mtd(0, "nvram", "nor", 0x100000, None)
        self.add_mtd(1, "adt", "ram", size, self.region)
        # The device holds the ADT and the region's padding (only the ADT for a region too large
        # to read, which the reader must refuse before it opens the device).
        self.blob = padded(build_m3max_adt(), min(size, 0x7c000))
        (self.dev / "mtd1ro").write_bytes(self.blob)

    def add_region(self, name, compatible, label, base, size):
        node = self.dt / "reserved-memory" / name
        node.mkdir()
        (node / "compatible").write_bytes(compatible)
        (node / "label").write_bytes(label)
        (node / "reg").write_bytes(struct.pack(">4I", base >> 32, base & 0xFFFFFFFF, size >> 32,
                                               size & 0xFFFFFFFF))
        return node

    def add_mtd(self, index, name, typ, size, of_node):
        cls = self.sys / "class/mtd"
        for suffix, minor in (("", index * 2), ("ro", index * 2 + 1)):
            d = cls / f"mtd{index}{suffix}"
            d.mkdir(parents=True)
            (d / "dev").write_text(f"90:{minor}\n")
            self.rdevs[str(self.dev / f"mtd{index}{suffix}")] = os.makedev(90, minor)
            if not suffix:
                (d / "name").write_text(name + "\n")
                (d / "type").write_text(typ + "\n")
                (d / "size").write_text(f"{size}\n")
                if of_node is not None:
                    (d / "of_node").symlink_to(of_node)

    def host(self):
        def fake_stat(path):
            st = os.stat(path)
            rdev = self.rdevs.get(str(path))
            return st if rdev is None else FakeStat(rdev)

        def fake_fstat(fd):
            path = os.readlink(f"/proc/self/fd/{fd}")
            return fake_stat(path)

        return adt.Host(sys_root=str(self.sys), dt_root=str(self.dt), dev_root=str(self.dev),
                        stat_fn=fake_stat, fstat_fn=fake_fstat)


class FakeStat:
    def __init__(self, rdev):
        self.st_mode = stat.S_IFCHR | 0o600
        self.st_rdev = rdev


class DeviceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mac = FakeMac(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def refused(self, argv, msg):
        code, out, err = run(argv, self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertEqual(out, "")
        self.assertIn(msg, err)
        return err

    def test_found_by_name_and_read(self):
        code, out, err = run([], self.mac.host())
        self.assertEqual(code, 0, err)
        self.assertIn("# source: %s/mtd1ro: MTD device mtd1 named 'adt', 507904 bytes, read-only node; "
                      "reserved-memory node flash@10003528000 at 0x10003528000, 0x7c000 bytes" % self.mac.dev,
                      out)
        self.assertIn("/arm-io/sgx", out)
        AllowlistTest.check_private(self, out)

    def test_check(self):
        code, out, err = run(["--check", str(self.mac.dev / "mtd1ro")], self.mac.host())
        self.assertEqual(code, 0, err)
        self.assertIn("MTD device mtd1 named 'adt'", out)
        self.assertNotIn("/arm-io", out)

    def test_check_reads_nothing(self):
        (self.mac.dev / "mtd1ro").write_bytes(b"garbage")
        code, out, err = run(["--check"], self.mac.host())
        self.assertEqual(code, 0, err)

    def test_writable_node_refused(self):
        (self.mac.dev / "mtd1").write_bytes(self.mac.blob)
        self.refused([str(self.mac.dev / "mtd1")], "is a writable MTD node")
        (self.mac.dev / "mtd/by-name").mkdir(parents=True)
        (self.mac.dev / "mtd/by-name/adt").symlink_to("../../mtd1")
        self.refused([str(self.mac.dev / "mtd/by-name/adt")], "is a writable MTD node")

    def test_wrong_device(self):
        (self.mac.dev / "mtd0ro").write_bytes(b"x")
        self.refused([str(self.mac.dev / "mtd0ro")], "belongs to mtd0, named 'nvram'")
        self.refused([str(self.mac.dev / "nothere")], "does not exist")

    def test_not_a_char_device(self):
        other = self.mac.root / "dev/sda"
        other.write_bytes(b"x")
        self.mac.rdevs[str(other)] = os.makedev(8, 1)
        self.refused([str(other)], "not an MTD device")

    def test_size_mismatch(self):
        (self.mac.sys / "class/mtd/mtd1/size").write_text("4096\n")
        self.refused([], "has 4096 bytes, but its region flash@10003528000 has 507904")

    def test_of_node_mismatch(self):
        link = self.mac.sys / "class/mtd/mtd1/of_node"
        link.unlink()
        link.symlink_to(self.mac.dt / "reserved-memory/flash@108d994c000")
        self.refused([], "is not bound to the region flash@10003528000")
        link.unlink()
        self.refused([], "has no device-tree node")

    def test_no_or_two_regions(self):
        (self.mac.dt / "reserved-memory/flash@20000000000").mkdir()
        (self.mac.dt / "reserved-memory/flash@20000000000/compatible").write_bytes(b"phram\0")
        (self.mac.dt / "reserved-memory/flash@20000000000/label").write_bytes(b"adt\0")
        self.refused([], "2 reserved-memory regions labelled adt")
        for d in ("flash@20000000000", "flash@10003528000"):
            (self.mac.dt / "reserved-memory" / d / "label").write_bytes(b"other\0")
        self.refused([], "no reserved-memory region labelled adt")

    def test_disabled_region(self):
        (self.mac.region / "status").write_bytes(b"disabled\0")
        self.refused([], "is not enabled")

    def test_bad_reg(self):
        (self.mac.region / "reg").write_bytes(b"\0" * 32)
        self.refused([], "does not have exactly one reg entry")

    def test_no_adt_mtd(self):
        (self.mac.sys / "class/mtd/mtd1/name").write_text("other\n")
        self.refused([], "no MTD device is named adt")

    def test_two_adt_mtds(self):
        self.mac.add_mtd(2, "adt", "ram", 0x7c000, self.mac.region)
        self.refused([], "2 MTD devices are named adt")

    def test_sysfs_dev_mismatch(self):
        (self.mac.sys / "class/mtd/mtd1ro/dev").write_text("90:5\n")
        self.refused([str(self.mac.dev / "mtd1ro")], "is not the node of mtd1ro in sysfs")

    def test_not_ram(self):
        (self.mac.sys / "class/mtd/mtd1/type").write_text("nor\n")
        self.refused([], "not 'ram'")

    def test_short_device(self):
        (self.mac.dev / "mtd1ro").write_bytes(self.mac.blob[:1000])
        code, out, err = run([], self.mac.host())
        self.assertEqual(code, 1, err)
        self.assertIn("short read", err)
        self.assertEqual(out, "")


class RegionSizeTest(unittest.TestCase):
    """The device region is capped at MAX_ADT_BYTES before the device is opened."""

    def opens(self, mac, argv):
        seen = []
        real_open = os.open

        def tracking(path, flags, *a, **k):
            seen.append(os.fspath(path))
            return real_open(path, flags, *a, **k)

        adt.os.open = tracking
        try:
            result = run(argv, mac.host())
        finally:
            adt.os.open = real_open
        return result, [p for p in seen if p.startswith(str(mac.dev))]

    def test_oversized_region_refused_before_open(self):
        size = 17 * 1024 * 1024
        self.assertGreater(size, adt.MAX_ADT_BYTES)
        with tempfile.TemporaryDirectory() as tmp:
            mac = FakeMac(tmp, size=size)
            for argv in ([], [str(mac.dev / "mtd1ro")], ["--check"]):
                with self.subTest(argv=argv):
                    (code, out, err), opened = self.opens(mac, argv)
                    self.assertEqual(code, 3, err)
                    self.assertEqual(out, "")
                    self.assertIn(f"the adt region flash@10003528000 has {size} bytes, more than an ADT "
                                  f"({adt.MAX_ADT_BYTES})", err)
                    self.assertEqual(opened, [])

    def test_huge_region_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            mac = FakeMac(tmp, size=0xFFFFFFFF00000000)
            code, out, err = run([], mac.host())
            self.assertEqual(code, 3, err)
            self.assertIn("more than an ADT", err)

    def test_cap_itself_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            mac = FakeMac(tmp, size=adt.MAX_ADT_BYTES)
            (code, out, err), opened = self.opens(mac, ["--check"])
            self.assertEqual(code, 0, err)
            self.assertIn(f"{adt.MAX_ADT_BYTES} bytes", out)
            self.assertEqual(opened, [])

    def test_read_device_checks_the_cap_too(self):
        region = adt.Region("/nonexistent", 0, adt.MAX_ADT_BYTES + 1)
        binding = adt.Binding("/nonexistent-device", 0, "mtd1", region)
        with self.assertRaises(adt.Refused):
            adt.read_device(binding, adt.Host())

    def test_oversized_file_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "big.bin")
            with open(path, "wb") as f:
                f.truncate(adt.MAX_ADT_BYTES + 1)
            code, out, err = run([str(path)])
            self.assertEqual(code, 3, err)
            self.assertEqual(out, "")
            self.assertIn("more than an ADT", err)


if __name__ == "__main__":
    unittest.main()
