"""aurora-adt-extract.py: the ADT allowlist reader.

Every ADT here is synthetic. build_m3max_adt() makes an ADT shaped like an M3 Max's (the node
names the reader looks for, a 14-inch t6031 or t6034 board), with placeholder values: none of
its numbers describe M3 Max hardware. It plants sensitive properties (serial numbers, MAC
addresses, chip ids, nonces, keys, calibration, per-Mac UUIDs) inside and outside the allowlist,
and marker strings in nodes outside it, and the tests check that none of them is ever printed.
The firmware image UUIDs of the firmware processors' nubs (iop-*-nub*) are the exception: they
are printed (FirmwareUuidTest). StageLogTest covers --stage2-log, m1n1's log of this boot.

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
# The firmware image UUIDs the boot loader loaded: printed for a nub's "uuid".
FW_UUID = "11111111-2222-3333-4444-555555555555"
DCP_UUID = "DDF38191-93B3-324A-BC8F-643006F5AC82"
# A UUID that names this Mac (not a firmware image): never printed.
MAC_UUID = "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"
PANEL_SERIAL = "FAKEPANEL24680"
SECRETS = [SERIAL.encode(), MLB_SERIAL.encode(), CHIP_ID, WIFI_MAC, BT_MAC, NONCE, KEY, CALIBRATION,
           MAC_UUID.encode(), PANEL_SERIAL.encode()]
# Strings that appear only in nodes outside the allowlist.
OUTSIDE = ["OUTSIDE-wlan", "OUTSIDE-bluetooth", "OUTSIDE-sep", "OUTSIDE-smc", "OUTSIDE-chosen",
           "OUTSIDE-nvme", "OUTSIDE-root-model", "OUTSIDE-product-color", "OUTSIDE-mcc"]
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
        # a uuid that is not a firmware nub's: dropped, and a secret for the value check
        prop("uuid", MAC_UUID),
        prop("boot-volume", "volume " + MAC_UUID),
        # holds a firmware image's uuid: not dropped by value
        prop("fw-note", "image " + FW_UUID),
    ])
    gfx_asc = node("gfx-asc", [prop("compatible", "iop,ascwrap-v6"),
                               prop("reg", [0x44440000, 0x1, 0x4000, 0x0])],
                   [node("iop-gfx-nub0", [prop("segment-names", b"__TEXT\0__DATA\0"),
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
                                     prop("uuid", DCP_UUID),
                                     # not one UUID string: dropped
                                     prop("tunable-uuid", MAC_UUID)])])
    dcpext = [node(f"dcpext{i}", [prop("compatible", "dcpext,t6031"),
                                  prop("reg", [0x67670000 + i * 0x10000, 0x1, 0x4000, 0])],
                   [node(f"iop-dcpext{i}-nub", [prop("uuid", DCP_UUID if i < 3 else b"\x01" * 16)])])
              for i in range(4)]
    # The memory cache controller: only its reg is printed. The Neural Engine is an allowed
    # subtree: printed whole, with its nub.
    mcc = node("mcc", [prop("compatible", "mcc,t6031"), prop("reg", [0x20000000, 0x2, 0x95000, 0]),
                       prop("dcs-count", 8), prop("debug-tag", "OUTSIDE-mcc")])
    ane = node("ane", [prop("compatible", "ane,t6031"), prop("reg", [0x0945c000, 0x3, 0x4000, 0]),
                       prop("debug-tag", "ane-tag")], [node("iop-ane-nub", [prop("uuid", FW_UUID)])])
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
    ], [sgx, gfx_asc, pmgr, pmp, dcp, mcc, ane] + dcpext + disp + darts + outside)
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
        # The one denied name printed: a firmware nub's uuid, and only with a UUID as its value.
        for name in printed:
            if name != "uuid":
                self.assertIsNone(adt.denied_part(name), name)
        for line in re.findall(r"^  uuid \[.*$", text, re.M):
            self.assertRegex(line, r'^  uuid \[37\] = "[0-9A-F-]{36}"$')
        for name in SENSITIVE_NAMES:
            if name != "uuid":
                self.assertNotIn(name, printed)

    def test_m3max_shapes(self):
        for chip in BOARDS:
            with self.subTest(chip=chip):
                text = render(padded(build_m3max_adt(chip)))
                self.check_private(text)
                nodes = re.findall(r"^(/\S*)$", text, re.M)
                self.assertEqual(nodes[:3], ["/", "/arm-io", "/arm-io/sgx"])
                for want in ["/arm-io/gfx-asc/iop-gfx-nub0", "/arm-io/pmgr", "/arm-io/pmp/iop-pmp-nub",
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
        self.assertEqual(block("/arm-io/mcc"),
                         "  reg [16] = <0x20000000 0x00000002 0x00095000 0x00000000>\n")
        # The Neural Engine is not limited: printed whole, its nub with the firmware image uuid.
        self.assertIn("  reg [16] = <0x0945c000 0x00000003 0x00004000 0x00000000>\n", block("/arm-io/ane"))
        self.assertIn('  debug-tag [8] = "ane-tag"', block("/arm-io/ane"))
        self.assertIn(f'  uuid [37] = "{FW_UUID}"', block("/arm-io/ane/iop-ane-nub"))

    def test_dropped_and_summary(self):
        text = render(build_m3max_adt())
        self.assertIn("# dropped: /arm-io/sgx gpu-calibration (name contains 'calibration')", text)
        self.assertIn("# dropped: /arm-io/sgx unique-id (name contains 'unique')", text)
        self.assertIn("# dropped: /arm-io/sgx diag-info (value holds a dropped property's value)", text)
        self.assertIn("# dropped: /arm-io/dcp/iop-dcp-nub tunable-uuid (name contains 'uuid')", text)
        self.assertIn("# dropped: /arm-io/sgx uuid (name contains 'uuid')", text)
        self.assertIn("# dropped: /arm-io/sgx boot-volume (value holds a dropped property's value)", text)
        self.assertIn("# dropped: /arm-io/dcpext3/iop-dcpext3-nub uuid (name contains 'uuid')", text)
        self.assertIn("# dropped: /arm-io/pmgr fuse-revision-row (name contains 'fuse')", text)
        self.assertNotIn("iop-dcp-nub uuid (", text)
        summary = text.rstrip("\n").splitlines()[-1]
        self.assertTrue(summary.startswith("# summary: printed "), summary)
        self.assertRegex(summary, r"\(7 firmware image uuids\)")
        self.assertRegex(summary, r"dropped 9 properties \(7 by name: calibration 1, fuse 1, "
                                  r"serial 1, unique 1, uuid 3; 2 by value\)")
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


class FirmwareUuidTest(unittest.TestCase):
    """The uuid of a firmware processor's nub names the firmware image, the same on every Mac with
    that firmware: printed, when it is one UUID string, in a nub of an allowed node."""

    def test_nub_uuids_are_printed(self):
        text = render(build_m3max_adt())
        block = lambda path: re.search(rf"^{re.escape(path)}\n((?:  .*\n)*)", text, re.M).group(1)
        self.assertIn(f'  uuid [37] = "{FW_UUID}"', block("/arm-io/gfx-asc/iop-gfx-nub0"))
        self.assertIn(f'  uuid [37] = "{FW_UUID}"', block("/arm-io/pmp/iop-pmp-nub"))
        self.assertIn(f'  uuid [37] = "{DCP_UUID}"', block("/arm-io/dcp/iop-dcp-nub"))
        for i in range(3):
            self.assertIn(f'  uuid [37] = "{DCP_UUID}"', block(f"/arm-io/dcpext{i}/iop-dcpext{i}-nub"))
        # Not a UUID string: dropped.
        self.assertNotIn("uuid", block("/arm-io/dcpext3/iop-dcpext3-nub"))
        # A firmware uuid is no secret (the ANE's nub holds the same one): another value that
        # holds it stays.
        self.assertIn(f'  fw-note [43] = "image {FW_UUID}"', block("/arm-io/sgx"))

    def test_only_a_nubs_own_uuid(self):
        nub = lambda name, props: node("dcp", [prop("compatible", "dcp,t6031")], [node(name, props)])
        cases = {
            # outside the allowlist: never printed (and no secret either: see test_nub_uuids_are_printed)
            "/arm-io/smc/iop-smc-nub": node("arm-io", [], [node("smc", [], [node("iop-smc-nub", [prop("uuid", FW_UUID)])])]),
            # a node whose name only looks like a nub
            "/arm-io/dcp/nub-iop": node("arm-io", [], [nub("nub-iop", [prop("uuid", FW_UUID)])]),
            # a nub's other uuid properties
            "/arm-io/dcp/iop-dcp-nub": node("arm-io", [], [nub("iop-dcp-nub", [prop("volume-uuid", FW_UUID),
                                                                              prop("UUID", FW_UUID)])]),
            # the value is not exactly one UUID string
            "/arm-io/dcp/iop-dcp-nub ": node("arm-io", [], [nub("iop-dcp-nub", [prop("uuid", FW_UUID + "x")])]),
        }
        for what, arm_io in cases.items():
            with self.subTest(what=what):
                text = render(node("device-tree", [], [arm_io]))
                self.assertNotIn(FW_UUID, text)
                self.assertRegex(text, r"\(0 firmware image uuids\)")

    def test_mac_uuid_still_drops_by_value(self):
        text = render(build_m3max_adt())
        self.assertNotIn(MAC_UUID, text)
        self.assertNotIn(MAC_UUID.lower(), text.lower())


class StageLogTest(unittest.TestCase):
    """--stage2-log: m1n1's log of this boot, from the m1n1_stage2.log region, read-only, with the
    same checks as the ADT, as text, with the lines that may name this Mac left out."""

    LOG = (b"m1n1 v1.6.1-omarchy.aurora13\n"
           b"Chip: 0x6031 rev 0x12\n"
           b"ECID: 0x0123456789abcdef\n"
           b"Serial number: C02FAKE1234\n"
           b"preboot uuid " + MAC_UUID.encode() + b"\n"
           b"t6031: reserved dcp-oslog@1000507c000 (0x1f000)\n"
           b"FDT: GPU: T6031 image " + FW_UUID.encode() + b", RTKit-2419.140.12.release\n"
           b"FDT: T6031: 7 of 7 firmware image UUIDs published\n"
           b"a\x01b\x7fc\ttab\n"
           + b"x" * 600 + b"\n"
           + b"last line")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mac = FakeMac(self.tmp.name)
        log = self.mac.dt / "reserved-memory/flash@108d994c000"
        self.mac.add_mtd(2, "m1n1_stage2.log", "ram", 0x4000, log)
        (self.mac.dev / "mtd2ro").write_bytes(self.LOG + b"\0" * (0x4000 - len(self.LOG)))

    def tearDown(self):
        self.tmp.cleanup()

    def check_text(self, out):
        self.assertTrue(out.startswith("# aurora-adt-extract: m1n1's stage 2 log of this boot"), out)
        self.assertIn("m1n1 v1.6.1-omarchy.aurora13\n", out)
        self.assertIn("Chip: 0x6031 rev 0x12\n", out)
        self.assertIn("t6031: reserved dcp-oslog@1000507c000 (0x1f000)\n", out)
        # Lines about a firmware image keep their UUIDs; a per-Mac one goes.
        self.assertIn(f"FDT: GPU: T6031 image {FW_UUID}, RTKit-2419.140.12.release\n", out)
        self.assertIn("FDT: T6031: 7 of 7 firmware image UUIDs published\n", out)
        self.assertIn("a.b.c\ttab\n", out)
        self.assertIn("x" * 512 + " (cut)\n", out)
        self.assertIn("last line\n", out)
        for gone in ("ECID", "0123456789abcdef", "C02FAKE1234", MAC_UUID, "\0"):
            self.assertNotIn(gone, out)
        self.assertRegex(out.rstrip("\n").splitlines()[-1],
                         r"^# summary: printed \d+ lines of \d+ bytes; left out 3 lines \(ecid 1, serial 1, uuid 1\); cut 1 ")

    def test_read_by_name(self):
        code, out, err = run(["--stage2-log"], self.mac.host())
        self.assertEqual(code, 0, err)
        self.assertIn("MTD device mtd2 named 'm1n1_stage2.log', 16384 bytes, read-only node; reserved-memory "
                      "node flash@108d994c000", out)
        self.check_text(out)

    def test_read_by_node_and_check(self):
        code, out, err = run(["--stage2-log", str(self.mac.dev / "mtd2ro")], self.mac.host())
        self.assertEqual(code, 0, err)
        self.check_text(out)
        code, out, err = run(["--stage2-log", "--check", str(self.mac.dev / "mtd2ro")], self.mac.host())
        self.assertEqual(code, 0, err)
        self.assertIn("named 'm1n1_stage2.log'", out)
        self.assertNotIn("m1n1 v1.6.1", out)

    def test_the_adt_device_is_refused(self):
        code, out, err = run(["--stage2-log", str(self.mac.dev / "mtd1ro")], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertEqual(out, "")
        self.assertIn("belongs to mtd1, named 'adt', not 'm1n1_stage2.log'", err)
        # And the other way round.
        code, out, err = run([str(self.mac.dev / "mtd2ro")], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertIn("named 'm1n1_stage2.log', not 'adt'", err)

    def test_size_and_binding(self):
        (self.mac.sys / "class/mtd/mtd2/size").write_text("8192\n")
        code, out, err = run(["--stage2-log"], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertIn("has 8192 bytes, but its region flash@108d994c000 has 16384", err)
        (self.mac.sys / "class/mtd/mtd2/size").write_text("16384\n")
        link = self.mac.sys / "class/mtd/mtd2/of_node"
        link.unlink()
        link.symlink_to(self.mac.region)
        code, out, err = run(["--stage2-log"], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertIn("is not bound to the region flash@108d994c000", err)

    def test_no_log_device(self):
        (self.mac.sys / "class/mtd/mtd2/name").write_text("other\n")
        code, out, err = run(["--stage2-log"], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertIn("no MTD device is named m1n1_stage2.log", err)

    def test_oversized_log_region_refused(self):
        big = adt.MAX_LOG_BYTES + 0x4000
        reg = self.mac.dt / "reserved-memory/flash@108d994c000/reg"
        reg.write_bytes(struct.pack(">4I", 0x108, 0xd994c000, 0, big))
        code, out, err = run(["--stage2-log"], self.mac.host())
        self.assertEqual(code, 3, err)
        self.assertIn(f"has {big} bytes, more than a log ({adt.MAX_LOG_BYTES})", err)

    def test_raw(self):
        # All of the region, byte for byte, but the lines that may name this Mac, overwritten.
        region = (self.mac.dev / "mtd2ro").read_bytes()
        proc = subprocess.run([sys.executable, "-c",
                               "import sys, importlib.util as u; s = u.spec_from_file_location('a', sys.argv[1]); "
                               "m = u.module_from_spec(s); s.loader.exec_module(m); "
                               "sys.stdout.buffer.write(m.raw_log(open(sys.argv[2], 'rb').read()))",
                               str(TOOL), str(self.mac.dev / "mtd2ro")], capture_output=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        raw = proc.stdout
        self.assertEqual(len(raw), len(region))
        for gone in (b"ECID", b"0123456789abcdef", b"C02FAKE1234", MAC_UUID.encode()):
            self.assertNotIn(gone, raw)
        self.assertIn(b"x" * len(b"ECID: 0x0123456789abcdef"), raw)
        self.assertIn(b"a\x01b\x7fc\ttab\n", raw)
        self.assertIn(FW_UUID.encode(), raw)
        self.assertTrue(raw.endswith(b"last line" + b"\0" * (0x4000 - len(self.LOG))))
        # Through the command line: the device's checks, then the same bytes.
        code, out, err = run(["--stage2-log", "--raw"], self.mac.host())
        self.assertEqual(code, 0, err)
        self.assertEqual(out.encode("latin-1"), raw)

    def test_raw_needs_stage2_log(self):
        proc = subprocess.run([sys.executable, str(TOOL), "--raw"], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("--raw goes with --stage2-log", proc.stderr)

    def test_file(self):
        path = Path(self.tmp.name, "log.bin")
        path.write_bytes(self.LOG)
        proc = subprocess.run([sys.executable, str(TOOL), "--stage2-log", str(path)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("# source: file log.bin", proc.stdout)
        self.check_text(proc.stdout)


# --- The neural engine -----------------------------------------------------------------------

def build_ane_adt(units=("",)):
    """An ADT with one ANE per suffix in units, a DART and mapper for each, and look-alikes that
    must not print."""
    arm_io = []
    for u in units:
        arm_io.append(node(f"ane{u}", [
            prop("compatible", "ane,t6031"), prop("reg", [0x88880000, 0x2, 0x100000, 0x0]),
            prop("interrupts", [0x4c0, 0x4c1]), prop("interrupt-parent", 0xa1),
            prop("clock-gates", [0x120]), prop("power-gates", [0x121, 0x122]),
            prop("iommu-parent", 0x9f), prop("ane-type", 1),
            prop("serial-number", SERIAL),
        ], [node(f"iop-ane{u}-nub", [prop("compatible", "iop-nub,rtbuddy-v2"),
                                      prop("segment-names", b"__TEXT\0__DATA\0"),
                                      prop("uuid", FW_UUID), prop("volume-uuid", MAC_UUID)])]))
        arm_io.append(node(f"dart-ane{u}", [prop("sids", [0, 1, 2]), prop("vm-size", [0, 0x20])],
                           [node(f"mapper-ane{u}", [prop("compatible", "iommu-mapper,ane")])]))
    arm_io += [
        node("ans", [prop("compatible", "OUTSIDE-ans")], [node("iop-ans-nub", [prop("x", "OUTSIDE-ans")])]),
        node("sart-ans", [prop("compatible", "OUTSIDE-ans")]),
        node("plane-info", [prop("compatible", "OUTSIDE-plane")]),
        node("pmgr", [prop("ane-dpe", 0), prop("ane-tvm", 1)]),
    ]
    return node("device-tree", [], [node("arm-io", [prop("compatible", "arm-io,t6031")], arm_io)])


class AneTest(unittest.TestCase):
    def test_ane_nodes_print_for_any_unit_count(self):
        for units in [("",), ("0",), ("0", "1"), ("", "1", "2", "3")]:
            with self.subTest(units=units):
                text = render(build_ane_adt(units))
                nodes = re.findall(r"^(/\S*)$", text, re.M)
                for u in units:
                    for want in [f"/arm-io/ane{u}", f"/arm-io/ane{u}/iop-ane{u}-nub",
                                 f"/arm-io/dart-ane{u}", f"/arm-io/dart-ane{u}/mapper-ane{u}"]:
                        self.assertIn(want, nodes)
                for no in ["/arm-io/ans", "/arm-io/ans/iop-ans-nub", "/arm-io/sart-ans",
                           "/arm-io/plane-info"]:
                    self.assertNotIn(no, nodes)
                self.assertNotIn("OUTSIDE", text)

    def test_ane_properties_and_pmgr_entries(self):
        text = render(build_ane_adt())
        self.assertIn("  interrupts [8] = <0x000004c0 0x000004c1>", text)
        self.assertIn("  power-gates [8] = <0x00000121 0x00000122>", text)
        self.assertIn("  iommu-parent [4] = <0x0000009f>", text)
        self.assertIn("  sids [12] = <0x00000000 0x00000001 0x00000002>", text)
        self.assertIn('  segment-names [14] = "__TEXT", "__DATA"', text)
        self.assertIn("  ane-dpe [4] = <0x00000000>", text)
        self.assertIn("  ane-tvm [4] = <0x00000001>", text)

    def test_ane_nodes_keep_the_private_value_rules(self):
        text = render(build_ane_adt())
        self.assertNotIn(SERIAL, text)
        self.assertNotIn(MAC_UUID, text)
        self.assertIn("# dropped: /arm-io/ane serial-number", text)
        self.assertIn("# dropped: /arm-io/ane/iop-ane-nub volume-uuid", text)
        # The nub's own uuid names the firmware image: printed, as for the other nubs.
        self.assertIn(f'  uuid [37] = "{FW_UUID}"', text)

    def test_patterns(self):
        for path in ["/arm-io/ane", "/arm-io/ane0", "/arm-io/ane1/iop-ane1-nub",
                     "/arm-io/dart-ane0", "/arm-io/dart-ane/mapper-ane", "/arm-io/mapper-ane0"]:
            self.assertTrue(adt.in_allowed_subtree(path), path)
        for path in ["/arm-io/ans", "/arm-io/sart-ans", "/arm-io/plane", "/ane", "/chosen/arm-io/ane",
                     "/arm-io/dart-ans"]:
            self.assertFalse(adt.in_allowed_subtree(path), path)

    @unittest.skipUnless(os.environ.get("AURORA_ADT_FIXTURE"), "set AURORA_ADT_FIXTURE to a real ADT file")
    def test_real_adt_prints_its_ane_nodes(self):
        blob = Path(os.environ["AURORA_ADT_FIXTURE"]).read_bytes()
        root, used, nodes, props = adt.parse_adt(blob)
        text = adt.render(root, "fixture", used, nodes, props)
        want = [path for path, _ in adt.walk(root) if re.match(r"/arm-io/(dart-|mapper-)?ane", path)]
        self.assertTrue(want, "the fixture has no ANE node")
        printed = re.findall(r"^(/\S*)$", text, re.M)
        for path in want:
            self.assertIn(path, printed)


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
