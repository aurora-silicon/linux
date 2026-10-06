"""--m3-report: one file for an M3 test report, read-only, without private data.

Runs against the fake Mac of test_m3_flow, with a fake dmesg and lsusb.
"""
import tarfile
import unittest

import test_m3_flow as flow

Base = getattr(flow, "M3FlowBase", None)
if Base is None:
    Base = type("Base", (flow.M3FlowTest,),
                {n: None for n in dir(flow.M3FlowTest) if n.startswith("test")})

DMESG = """\
[    0.000000] OF: reserved mem: 0x00000103fff70000..0x00000103fff73fff (16 KiB) map non-reusable uat-handoff
[    0.055969] [drm] Initialized simpledrm 1.0.0 for 103e1c94000.framebuffer on minor 0
[    2.100000] usb 1-1: SerialNumber: ABC123SECRET
[    3.000000] brcmfmac: wlan0 address 12:34:56:78:9a:bc
[    3.100000] usb 2-1: new SuperSpeed USB device, mac 12:34:56:78:9a:bc
[    4.000000] some unrelated line
"""


class M3ReportTest(Base):
    def setUp(self):
        super().setUp()
        (self.tmp / "bin/dmesg").write_text("#!/bin/sh\ncat <<'EOF'\n" + DMESG + "EOF\n")
        (self.tmp / "bin/dmesg").chmod(0o755)
        (self.tmp / "bin/lsusb").write_text("#!/bin/sh\necho '/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=xhci-hcd/1p, 5000M'\n")
        (self.tmp / "bin/lsusb").chmod(0o755)
        self.out = self.tmp / "out"
        self.out.mkdir()

    def report(self):
        proc = self.run_sh(f"cd '{self.out}'\nm3_report")
        files = list(self.out.glob("aurora-m3-report-*.tgz"))
        self.assertEqual(len(files), 1, proc.stdout + proc.stderr)
        with tarfile.open(files[0]) as t:
            return proc, {m.name.lstrip("./"): (t.extractfile(m).read() if m.isfile() else None)
                          for m in t.getmembers()}

    def test_air_report(self):
        self.mac("j613")
        node = self.tmp / "dt/chosen/asahi,t8122-gpu-powered-identity"
        node.mkdir(parents=True)
        (node / "id-version").write_bytes(b"\x07\x02\x20\x00")
        (self.tmp / "dt/chosen/asahi,t8122-dcp").write_bytes(b"1\0")
        (self.tmp / "dt/chosen/asahi,os-fw-version").write_bytes(b"14.7\0")
        proc, files = self.report()
        self.assertIn("aurora-m3-report-j613-", proc.stdout)
        self.assertIn("board: j613 soc: t8122", files["system.txt"].decode())
        self.assertIn("os-fw-version: 14.7", files["system.txt"].decode())
        self.assertEqual(files["chosen/asahi,t8122-gpu-powered-identity/id-version"], b"\x07\x02\x20\x00")
        self.assertIn("chosen/asahi,t8122-dcp", files)
        dmesg = files["dmesg-m3.txt"].decode()
        self.assertIn("uat-handoff", dmesg)
        self.assertIn("simpledrm", dmesg)
        self.assertNotIn("ABC123SECRET", dmesg)
        self.assertNotIn("12:34:56:78:9a:bc", dmesg)
        self.assertIn("xx:xx:xx:xx:xx:xx", dmesg)
        self.assertNotIn("unrelated", dmesg)
        self.assertIn("5000M", files["usb-display.txt"].decode())

    def test_report_changes_nothing(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        self.report()
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())
        self.assertNotIn("update-m1n1", self.log())

    def test_report_names_the_m1n1_by_its_bytes(self):
        self.mac("j516s")
        self.install()
        sha = self.bin_shas[self.m1n1_pkg]
        _, files = self.report()
        text = files["system.txt"].decode()
        self.assertIn(f"installed m1n1.bin: sha256 {sha}", text)
        self.assertRegex(text, rf"boot.bin's first \d+ bytes: sha256 {sha}")
        self.assertIn(f"m1n1-installed: {sha} ", text)

    def test_report_needs_no_preflight(self):
        src = flow.INSTALLER.read_text()
        self.assertIn("--agent-prompt | --reset-touchid | --m3-report) return 1", src)


if __name__ == "__main__":
    unittest.main()
