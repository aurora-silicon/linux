"""The M3 Max boot loader variant (--m3-handoff on a t6031) and the M3 Max test kit (--m3max-kit).

Runs against the fake Mac of test_m3_max (device tree, sysfs, procfs, debugfs, stubbed commands,
the flow harness's pacman, update-m1n1 and curl), set up with the values of Naeem's J516C report
(aurora-recipes reports/m3max/naeem-j516c/report2: model, firmware and boot loader versions, the
command line, the reserved-memory regions, /proc/iomem, the kernel before the kit), plus an EFI
partition with Limine (its EFI binary, limine.conf, the UKI), the EFI variables as plain files and
U-Boot's variable file, as test_air_gpu has them, and stubs for systemctl, shutdown, runuser and
journalctl. Nothing here needs root or an Apple Mac.

VariantTest: --m3-handoff on j514c/j516c installs the variant (m1n1's GPU handoff off), keeps and
records the earlier boot.bin, and --uninstall puts it back; a plain run stays kernel-only; a t6034
refuses; the real T6031 m1n1 package (when at hand) knows both switch names.
KitRefusalTest: the consent screen (yes, anything else, no terminal, --yes) and every Mac the kit
refuses, each with nothing changed.
KitRunTest: the whole run across simulated boots (next_boot plays Limine: it takes the one-shot
or the default entry, sets the command line, runs --mark and --step): the stage state machine,
one-shot arming and disarming, the interim results and the boot loader GPU stage (boot.bin rebuilt
with the GPU handoff on, then off again), the variant or the boot loader GPU stage not coming back
and boot.bin put back from macOS, hung and hung-early stages, a one-shot Limine never took, the
boot cap, a kernel without the GPU start experiment, the kit's Mesa, a GPU success and the jobs
boot, --stop, the --restore drill, the tarball's contents and its masking, and an end-to-end run
on Naeem's values.
"""
from pathlib import Path
from unittest import mock
import hashlib
import os
import re
import shutil
import struct
import subprocess
import tarfile
import unittest

import test_m3_flow as flow
import test_m3_max as mx

SRC = flow.SRC
HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
PLAN = TOOLS / "m3max-kit/m3max-kit.plan"
VERSION = flow.VERSION
KIT_KREL = re.sub(r"^(\d+\.\d+\.\d+)\.aurora(\d+)-(.+)$", r"\1-\2-\3-sep-ARCH", VERSION)
MAX_SWITCHES = re.search(r'^M3_MAX_SWITCHES="([^"]*)"$', SRC, re.M).group(1).split()
MAX_VARIANT = re.search(r'^M3_MAX_VARIANT="([^"]*)"$', SRC, re.M).group(1)
MAX_SAFE = re.search(r'^M3_MAX_SAFE_SWITCHES="([^"]*)"$', SRC, re.M).group(1).split()
MAX_SWITCH_NAMES = [s[len("chosen."):].split("=")[0] for s in MAX_SWITCHES]
MESA_FILE = re.search(r'^M3MAX_KIT_MESA_PACKAGE="(\S+) [0-9a-f]{64}"$', SRC, re.M).group(1)
# The bring-up's packages (the T6031 m1n1 and the kit's Mesa), when at hand: AURORA_M3MAX_ARTIFACTS
# names their directory; the tests that read them are skipped without it.
ARTIFACTS = Path(os.environ.get("AURORA_M3MAX_ARTIFACTS", "/nonexistent"))


def tail(switches):
    return ("\n".join(switches) + "\n").encode()
BLI = "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"
UBOOT = "b2ac5fc9-92b7-4acd-aeac-11e818c3130c"
GLOBAL = "8be4df61-93ca-11d2-aa0d-00e098032b8c"

# Naeem's J516C (report2): what it ran before the kit.
NAEEM = {
    "model": "Apple MacBook Pro (16-inch, M3 Max, 16 CPU cores, Nov 2023)",
    "krel": "7.1.12-2-20261011.15-sep-ARCH",
    "cmdline": ("root=UUID=4f4d5801-524f-4f54-8000-000000000001 rw rootflags=subvol=@,x-systemd.device-timeout=0 "
                "zswap.enabled=0 rootfstype=btrfs quiet splash loglevel=0 systemd.show_status=false "
                "rd.udev.log_level=0 vt.global_cursor_default=0 plymouth.ignore-serial-consoles"),
    "chosen": {"asahi,os-fw-version": "14.7", "asahi,system-fw-version": "27.0",
               "asahi,iboot1-version": "mBoot-20457.1.29", "asahi,iboot2-version": "iBoot-10151.140.19.700.2",
               "asahi,m1n1-stage1-version": "v1.6.1-dirty", "asahi,m1n1-stage2-version": "v1.6.1-omarchy.aurora12",
               "asahi,efi-system-partition": "eabe31e3-497e-4fca-bae8-285a82534004"},
    "adt": (0x10004E74000, 0x8C000),
    "log": (0x10BCA228000, 0x4000),
}
# /proc/iomem's System RAM ranges on Naeem's kernel-only boot (the five DCP __OS_LOG buffers at
# 0x1000507c000 + n * 0x20000 lie inside 10004f60000-10005e2ffff), and on a boot whose boot
# loader reserved them (no-map: carved out of System RAM).
OSLOG = [0x1000507C000 + n * 0x20000 for n in range(5)]
IOMEM_BEFORE = """10000b74000-100014d3fff : reserved
1000223c000-10004f5bfff : System RAM
  10004e74000-10004efffff : reserved
10004f5c000-10004f5ffff : reserved
10004f60000-10005e2ffff : System RAM
10005e30000-1000620ffff : reserved
10006210000-10008513fff : System RAM
10008544000-10b2c3fffff : System RAM
"""
IOMEM_RESERVED = """10000b74000-100014d3fff : reserved
1000223c000-10004f5bfff : System RAM
  10004e74000-10004efffff : reserved
10004f5c000-10004f5ffff : reserved
10004f60000-1000507bfff : System RAM
1000507c000-1000511afff : reserved
1000511b000-10005e2ffff : System RAM
10005e30000-1000620ffff : reserved
10006210000-10008513fff : System RAM
10008544000-10b2c3fffff : System RAM
"""
FW_UUIDS = ("gfx=11111111-2222-3333-4444-555555555555\0dcp=DDF38191-93B3-324A-BC8F-643006F5AC82\0"
            "dcpext0=DDF38191-93B3-324A-BC8F-643006F5AC82\0pmp=2F4EB4C4-0000-0000-0000-000000000000\0")

STEP_STUBS = {
    # The boot finishes starting at once; units are enabled and disabled in the log.
    "systemctl": '#!/bin/sh\necho "systemctl $*" >>"$FAKE/log"\nexit 0\n',
    "shutdown": '#!/bin/sh\necho "shutdown $*" >>"$FAKE/log"\nexit 0\n',
    "runuser": '#!/bin/sh\n[ "$1" = -u ] && shift 2\n[ "$1" = -- ] && shift\nexec "$@"\n',
    "notify-send": '#!/bin/sh\necho "notify-send $*" >>"$FAKE/log"\n',
    "logger": '#!/bin/sh\nexit 0\n',
    # This boot's kernel release (FAKE_KREL), else the real uname.
    "uname": ('#!/bin/sh\n[ "$*" = -n ] && { echo "$FAKE_HOST"; exit 0; }\n'
              '[ "$*" = -r ] && { cat "$FAKE/krel"; exit 0; }\nexec /usr/bin/uname "$@"\n'),
    # This boot's journal (journal.txt); the boot before, found by its tag (prev.json), and its
    # journal (prev-journal.txt).
    "journalctl": r"""#!/bin/bash
case " $* " in
  *" -o json "*) [ -f "$FAKE/prev.json" ] && cat "$FAKE/prev.json" ;;
  *" -b 0 "*) [ -f "$FAKE/journal.txt" ] && cat "$FAKE/journal.txt" ;;
  *" -b "*) [ -f "$FAKE/prev-journal.txt" ] && cat "$FAKE/prev-journal.txt" ;;
  *) [ -f "$FAKE/journal.txt" ] && cat "$FAKE/journal.txt" ;;
esac
exit 0
""",
}


def utf16z(s):
    return s.encode("utf-16-le") + b"\0\0"


def var(attrs, data):
    return attrs.to_bytes(4, "little") + data


def u32(n):
    return struct.pack(">I", n)


def fdt_blob(props):
    """A flattened device tree (version 17): the root with PROPS, a wifi node with this Mac's MAC
    address as raw bytes, and a chosen node with the firmware UUIDs."""
    names, strings = {}, b""

    def name_off(n):
        nonlocal strings
        if n not in names:
            names[n] = len(strings)
            strings += n.encode() + b"\0"
        return names[n]

    def prop(n, v):
        return struct.pack(">III", 3, len(v), name_off(n)) + v + b"\0" * (-len(v) % 4)

    def node(n, body):
        nm = n.encode() + b"\0"
        return struct.pack(">I", 1) + nm + b"\0" * (-len(nm) % 4) + body + struct.pack(">I", 2)

    root = b"".join(prop(k, v) for k, v in props)
    root += node("wifi", prop("local-mac-address", bytes.fromhex(mx.MAC.replace(":", ""))) +
                 prop("compatible", b"pci14e4,4433\0"))
    root += node("chosen", prop("asahi,t6031-fw-uuids", FW_UUIDS.encode()))
    body = node("", root) + struct.pack(">I", 9)
    off_struct = 40 + 16
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    head = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, 40, 17, 16, 0, len(strings), len(body))
    return head + b"\0" * 16 + body + strings


def journal(krel, lines=()):
    head = [f"[    0.000000] {mx.HOST} kernel: Booting Linux on physical CPU 0x0000010100 [0x611f0492]",
            f"[    0.000000] {mx.HOST} kernel: Linux version {krel} (linux-aurora@archlinux)",
            f"[    0.000000] {mx.HOST} kernel: Machine model: {NAEEM['model']}",
            f"[    3.000000] {mx.HOST} kernel: brcmfmac: wlan0 address {mx.MAC}"]
    return "\n".join(head + [f"[    5.{i:06d}] {mx.HOST} kernel: {l}" for i, l in enumerate(lines)]) + "\n"


class KitBase(mx.MaxBase):
    """test_m3_max's fake Mac as Naeem's J516C on its kernel-only install, with Limine."""

    PARAMS_WITH_START = [b"apple_t6031_display.enable", b"asahi.t6031_start", b"asahi.t6031_pstate_cap",
                         b"asahi.m3_expose", b"softlockup_panic", b"hung_task_panic"]
    PARAMS_NO_START = [b"apple_t6031_display.enable", b"asahi.m3_expose", b"softlockup_panic", b"hung_task_panic"]

    def setUp(self):
        super().setUp()
        patcher = mock.patch.dict(flow.BOARDS, mx.EXTRA_BOARDS)
        patcher.start()
        self.addCleanup(patcher.stop)
        for name, body in STEP_STUBS.items():
            (self.tmp / "bin" / name).write_text(body)
            (self.tmp / "bin" / name).chmod(0o755)
        # This release's m1n1 knows the M3 Max variant's switches too.
        self.fixture(flow.M1N1_PKG, None, flow.SWITCH_NAMES + MAX_SWITCH_NAMES)
        # The kit's files, as the release serves them.
        for src in (TOOLS / "air-gpu/air-gpu-oneshot.sh", TOOLS / "air-gpu/air-gpu-job.sh",
                    TOOLS / "aurora-adt-extract.py"):
            shutil.copy(src, self.tmp / "pkgs" / src.name)
        self.plan(PLAN.read_text())
        self.mesa_fixture()
        self.kit = self.tmp / "kit-state"
        self.libexec = self.tmp / "libexec"
        self.units = self.tmp / "units"
        self.kitbin = self.tmp / "kitbin/aurora-m3max-kit"
        self.efivars = self.tmp / "efivars"
        self.esp = self.tmp / "esp"
        self.conf = self.esp / "limine.conf"
        self.varfile = self.esp / "ubootefi.var"
        self.uki = self.esp / "EFI/Linux/omarchy_linux-aurora.efi"
        self.rundir = self.tmp / "run-user"
        self.home_alice = self.tmp / "home-alice"
        for d in (self.efivars, self.esp / "EFI/BOOT", self.esp / "EFI/Linux", self.rundir, self.home_alice,
                  self.tmp / "run"):
            d.mkdir(parents=True, exist_ok=True)
        self.tty = self.tmp / "tty"
        self.tty.write_text("yes\n")
        self.extra_env.pop("SUDO_USER", None)

    # ---- the fake Mac ------------------------------------------------------------------------------

    def fresh_state(self):
        self.state.mkdir(exist_ok=True)
        super().fresh_state()

    def plan(self, text):
        (self.tmp / "pkgs/m3max-kit.plan").write_text(text)
        self.plan_sha = hashlib.sha256(text.encode()).hexdigest()

    def mesa_fixture(self, files=("opt/mesa-m3-g15c/bin/g15c-first-job",), depends=("glibc>=2.43", "libdrm")):
        """The kit's Mesa package, as the release would serve it (a stand-in by the shipped name)."""
        root = self.tmp / "root-mesa"
        shutil.rmtree(root, ignore_errors=True)
        for f in files:
            (root / f).parent.mkdir(parents=True, exist_ok=True)
            (root / f).write_text("stand-in\n")
        (root / ".PKGINFO").write_text("pkgname = mesa-m3-g15c\npkgver = 26.1.4.g15c1-2\narch = aarch64\n"
                                       + "".join(f"depend = {d}\n" for d in depends))
        path = self.tmp / "pkgs" / MESA_FILE
        subprocess.run(["bsdtar", "--zstd", "-cf", str(path), "-C", str(root), ".PKGINFO",
                        *sorted({f.split("/")[0] for f in files})], check=True)
        self.mesa_sha = hashlib.sha256(path.read_bytes()).hexdigest()

    def naeem(self, board="j516c", params=None):
        """Naeem's J516C as report2 shows it: kernel-only, update-m1n1 frozen by this script."""
        # An earlier run's boot loader facts go (a test may set the Mac up twice).
        for f in (self.tmp / "dt/chosen").glob("asahi,t6031-*"):
            shutil.rmtree(f) if f.is_dir() else f.unlink()
        self.max_mac(board)
        # His report's chosen/ is empty: no T6031 facts before the boot loader variant.
        (self.tmp / f"dt/chosen/asahi,{flow.BOARDS[board][1].split(',')[1]}-facts").unlink()
        (self.tmp / "dt/model").write_bytes(NAEEM["model"].encode() + b"\0")
        for k, v in NAEEM["chosen"].items():
            self.w(f"dt/chosen/{k}", v + "\0")
        for name in list((self.tmp / "dt/reserved-memory").iterdir()):
            if name.is_dir():
                shutil.rmtree(name)
        for (addr, size), label in ((NAEEM["adt"], "adt"), (NAEEM["log"], "m1n1_stage2.log")):
            n = self.node(f"reserved-memory/flash@{addr:x}", "phram", label=label)
            (n / "reg").write_bytes(u32(addr >> 32) + u32(addr & 0xFFFFFFFF) + u32(0) + u32(size))
            self.platform(f"{addr:x}.flash", f"reserved-memory/flash@{addr:x}")
        self.extra_env.update({"FAKE_ADT_NODE": f"{NAEEM['adt'][0]:x}", "FAKE_ADT_SIZE": str(NAEEM["adt"][1])})
        self.w("proc/iomem", IOMEM_BEFORE)
        self.w("proc/cmdline", NAEEM["cmdline"] + "\n")
        self.w("proc/sys/kernel/random/boot_id", "0f0e0d0c-0b0a-0908-0706-050403020100\n")
        self.w("proc/uptime", "12.34 5.0\n")
        (self.fake / "krel").write_text(NAEEM["krel"] + "\n")
        (self.fake / "installed").write_text("linux-aurora\n")
        (self.fake / "m1n1").write_text("m1n1-aurora-1.6.1.aurora12-1\n")
        (self.state / "m3-mode").write_text("kernel\n")
        self.update_conf.write_text(flow.OUR_FREEZE)
        self.boot_before = self.boot.read_bytes()
        self.w("fake/journal.txt", journal(NAEEM["krel"]))
        self.w("fdt", fdt_blob([("model", NAEEM["model"].encode() + b"\0"),
                                ("serial-number", mx.SERIAL.encode() + b"\0")]))
        (self.tmp / "var-log-journal").mkdir(exist_ok=True)
        self.limine(NAEEM["krel"], self.PARAMS_NO_START if params is None else params)

    def limine(self, krel, params):
        """Limine on the EFI partition: its binary, the UKI of krel, limine.conf, the variables."""
        (self.esp / "EFI/BOOT/BOOTAA64.EFI").write_bytes(b"\x7fLIMINE...limine.conf...\0")
        self.uki.write_bytes(b"MZ" + b"\0".join(params) + b"\0" + krel.encode() + b"\0tail")
        self.path = "boot():/EFI/Linux/omarchy_linux-aurora.efi#" + hashlib.blake2b(self.uki.read_bytes()).hexdigest()
        self.conf.write_text(
            "timeout: 3\ndefault_entry: 2\n/+Omarchy\ncomment: machine-id=x order-priority=50\n"
            "  //linux-aurora\n  ### This kernel entry is auto-generated by limine-entry-tool\n"
            f"  comment: Kernel version: {krel}\n  comment: kernel-id=linux-aurora\n  protocol: efi\n"
            f"  path: {self.path}\n  cmdline: {NAEEM['cmdline']}\n")
        self.setvar("LoaderInfo", BLI, var(6, utf16z("Limine 12.9.0")))
        self.setvar("RTStorageVolatile", UBOOT, var(6, b"ubootefi.var\0"))
        self.setvar("VarToFile", UBOOT, var(6, b"\0" * 8 + b"UbEfiVa\0"))
        self.setvar("SecureBoot", GLOBAL, var(6, b"\0"))
        self.varfile.write_bytes(self.store())
        self.w("etc/limine", 'ENABLE_UKI=yes\nKERNEL_CMDLINE[default]="x"\n')

    def setvar(self, name, guid, data):
        (self.efivars / f"{name}-{guid}").write_bytes(data)

    def oneshot_var(self):
        p = self.efivars / f"LoaderEntryOneShot-{BLI}"
        return p.read_bytes() if p.exists() else None

    def store(self):
        out = b"\0" * 8 + b"UbEfiVa\0"
        for f in sorted(self.efivars.iterdir()):
            if not f.name.startswith("VarToFile-"):
                out += b"HDR" + utf16z(f.name.rsplit("-", 5)[0]) + f.read_bytes()[4:]
        return out

    def kit_block(self):
        m = re.search(r"^# >>> m3max-kit:.*?^# <<< m3max-kit$", self.conf.read_text(), re.M | re.S)
        return m.group(0) if m else None

    def variant_facts(self, reserved=True):
        """What the boot loader variant publishes with its GPU handoff off: the T6031 facts, PMP
        values and firmware UUIDs, the five log reservations; the GPU node stays disabled."""
        self.w("dt/chosen/asahi,t6031-fw-uuids", FW_UUIDS)
        facts = self.tmp / "dt/chosen/asahi,t6031-facts"
        facts.mkdir(parents=True, exist_ok=True)
        (facts / "schema-version").write_bytes(u32(1))
        (facts / "disp0-clock-frequencies").write_bytes(u32(712000000))
        pmp = self.tmp / "dt/chosen/asahi,t6031-pmp"
        pmp.mkdir(parents=True, exist_ok=True)
        (pmp / "apple,board-id").write_bytes(u32(0x2c))
        (pmp / "apple,tunable-uuid").write_bytes(b"F00DCAFE-0000-0000-0000-000000000000\0")
        self.w("dt/chosen/asahi,m1n1-stage2-version", "v1.6.1-omarchy.aurora17.t6031.1\0")
        gpu = self.node("soc/gpu@40a400000", "apple,agx-t6031", "disabled")
        (gpu / "apple,mtr-fuse-leakage").write_bytes(b"\x12\x34\x56\x78")
        self.w("dt/aliases/gpu", "/soc/gpu@40a400000\0")
        opp = self.node("opp-table-gpu", "operating-points-v2")
        self.node("opp-table-gpu/opp00", None, None, opp_hz=u32(338000000))
        self.w("dt/aliases/dcp", "/soc/dcp@386c00000\0")
        dcp = self.node("soc/dcp@386c00000", "apple,t6031-dcp", "disabled")
        (dcp / "apple,t6031-handoff").write_bytes(u32(1))
        self.node("soc/dcp@386c00000/piodma", "apple,dcp-piodma", "disabled")
        if reserved:
            for a in OSLOG:
                n = self.node(f"reserved-memory/dcp-oslog@{a:x}", "apple,asc-mem")
                (n / "reg").write_bytes(u32(a >> 32) + u32(a & 0xFFFFFFFF) + u32(0) + u32(0x1F000))
                (n / "no-map").write_bytes(b"")
            self.w("proc/iomem", IOMEM_RESERVED)

    def gpu_handoff_facts(self, result=1, restored=True):
        """What the variant publishes with m1n1's GPU handoff on (the loader boot)."""
        ident = self.tmp / "dt/chosen/asahi,t6031-gpu-powered-identity"
        ident.mkdir(parents=True, exist_ok=True)
        (ident / "id-version").write_bytes(b"\x07\x04\x12\x00")
        (ident / "core-mask-0").write_bytes(u32(0xFFFFFFFF))
        (ident / "core-mask-1").write_bytes(u32(0xFF))
        (ident / "probe-result").write_bytes(u32(result))
        (ident / "pmgr-before").write_bytes(u32(0xF) + u32(0x4000000F))
        (ident / "pmgr-after").write_bytes(u32(0xF) + u32(0x4000000F) if restored else u32(0xF) + u32(0xF))
        img = self.tmp / "dt/chosen/asahi,t6031-gpu-image"
        img.mkdir(parents=True, exist_ok=True)
        (img / "firmware-family").write_bytes(b"14.8.3\0")
        (img / "image-rtkit-version").write_bytes(b"RTKit-2419.140.12.release\0")
        gpu = self.tmp / "dt/soc/gpu@40a400000"
        (gpu / "status").write_bytes(b"okay\0")
        (gpu / "apple,firmware-compat").write_bytes(u32(14) + u32(8) + u32(3))
        (gpu / "asahi,t6031-gpu-standin").write_bytes(b"apple,core-leak-coef\0opp-microwatt\0")

    def gpu_handoff_off(self):
        """The variant's boot loader with its GPU handoff off again."""
        shutil.rmtree(self.tmp / "dt/chosen/asahi,t6031-gpu-powered-identity", ignore_errors=True)
        shutil.rmtree(self.tmp / "dt/chosen/asahi,t6031-gpu-image", ignore_errors=True)
        (self.tmp / "dt/soc/gpu@40a400000/status").write_bytes(b"disabled\0")

    def put_back_from_macos(self):
        """What the macOS restore steps do: the kept boot.bin over boot.bin."""
        shutil.copy(self.kept_copy(), self.boot)
        (self.tmp / "dt/chosen/asahi,m1n1-stage2-version").write_bytes(b"v1.6.1-omarchy.aurora12\0")

    # ---- running it ----------------------------------------------------------------------------

    def kit_body(self):
        t = self.tmp
        return f"""{self.env_body()}
M3MAX_KIT_STATE='{self.kit}'
M3MAX_KIT_LIBEXEC='{self.libexec}'
M3MAX_KIT_BIN='{self.kitbin}'
M3MAX_KIT_UNIT_DIR='{self.units}'
M3MAX_KIT_ONESHOT='{self.libexec}/air-gpu-oneshot.sh'
M3MAX_KIT_JOB='{self.libexec}/air-gpu-job.sh'
M3MAX_KIT_PLAN_FILE='{self.kit}/plan'
M3MAX_KIT_PLAN="m3max-kit.plan {self.plan_sha}"
M3MAX_KIT_MESA_PACKAGE="{MESA_FILE} {self.mesa_sha}"
M3MAX_KIT_EFIVARS='{self.efivars}'
M3MAX_KIT_CMDLINE='{t}/proc/cmdline'
M3MAX_KIT_LOCKS='{t}/run/boot-partition.lock {t}/run/limine-global.lock'
M3MAX_KIT_LIMINE_DEFAULTS='{t}/etc/limine'
M3MAX_KIT_TTY='{self.tty}'
M3MAX_KIT_SETTLE=0
M3MAX_KIT_DISPLAY_SETTLE=0
M3MAX_KIT_FDT='{t}/fdt'
M3MAX_KIT_JOURNAL_DIR='{t}/var-log-journal'
M3MAX_KIT_DEBUGFS='{t}/debug'
M3MAX_KIT_RUNDIR='{self.rundir}'
M3_BOOT_PROFILE_HELPER='{t}/libexec-profile'
m3max_kit_home() {{ if [[ $1 == alice ]]; then echo '{self.home_alice}'; else echo /nonexistent; fi; }}
m3_keep_limine_entry() {{ echo "keep-limine-entry $*" >>"$FAKE/log"; }}
m3max_kit_oneshot_hook() {{
  ESP='{self.esp}'
  is_root() {{ true; }}
  uboot_store() {{
    printf '\\0\\0\\0\\0\\0\\0\\0\\0UbEfiVa\\0'
    local f n
    for f in "$EFIVARS"/*; do
      [[ ${{f##*/}} == VarToFile-* ]] && continue
      n=${{f##*/}}; n=${{n%-*-*-*-*-*}}
      printf 'HDR'; printf '%s' "$n" | iconv -f UTF-8 -t UTF-16LE; printf '\\0\\0'; tail -c +5 "$f"
    done
  }}
}}
"""

    def kit_sh(self, body, check=True):
        return self.run_sh(self.kit_body() + body, check=check)

    def setup_kit(self, args="--yes", check=True):
        return self.kit_sh(f"m3max_kit_setup {args}", check=check)

    def step(self, action="--step", check=True):
        return self.kit_sh(f"m3max_kit_runner {action}", check=check)

    def status(self):
        return dict(l.split("=", 1) for l in (self.kit / "status").read_text().splitlines() if "=" in l)

    def outcome(self, stage):
        p = next(self.kit.glob(f"stages/{stage}/outcome.txt"), None)
        return p.read_text().strip() if p else None

    def install_new_kernel(self, params=None):
        """What the kit's install leaves for the next boot: the test kernel's UKI and entry."""
        self.limine(KIT_KREL, self.PARAMS_NO_START if params is None else params)

    def next_boot(self, *, collect=True, mark=True, consume=True, lines=(), krel=KIT_KREL):
        """One boot: Limine takes the one-shot (when consume) or the default entry, the boot's
        command line follows, the mark unit runs (when mark), then the kit's step (when collect)."""
        armed = self.oneshot_var()
        cmd = NAEEM["cmdline"]
        if armed is not None and consume:
            (self.efivars / f"LoaderEntryOneShot-{BLI}").unlink()
            self.varfile.write_bytes(self.store())
            block = self.kit_block()
            self.assertIsNotNone(block, "a one-shot is set, but limine.conf has no m3max-kit entry")
            cmd = re.search(r"^    cmdline: (.*)$", block, re.M).group(1)
        self.w("proc/cmdline", cmd + "\n")
        (self.fake / "krel").write_text(krel + "\n")
        self.w("fake/journal.txt", journal(krel, lines))
        if mark and "m3max_kit.boot=" in cmd:
            self.step("--mark")
        if collect:
            return self.step()
        return None

    def kit_log(self):
        return (self.kit / "log").read_text()

    def esp_restore(self):
        return self.esp / "AURORA-M3MAX-KIT-RESTORE.txt"

    def esp_interim(self):
        return self.esp / "m1n1/aurora-m3max-kit-interim.tgz"

    def through_display(self, display_lines=("apple-t6031-display: enabled /soc/dcp@386c00000",)):
        """The data boot and the display boot: after the display boot the interim results are
        packed and boot.bin is rebuilt with the GPU handoff on."""
        self.next_boot()
        self.next_boot(lines=list(display_lines))
        self.gpu_handoff_facts()

    def tarball(self):
        st = self.status()
        path = Path(st["tarball"])
        self.assertTrue(path.exists(), st)
        with tarfile.open(path) as t:
            for m in t.getmembers():
                self.assertEqual((m.uid, m.gid), (0, 0), m.name)
            return {m.name.removeprefix("./"): t.extractfile(m).read() for m in t.getmembers() if m.isfile()}


class VariantTest(KitBase):
    """G3: the opt-in M3 Max boot loader variant through --m3-handoff."""

    def test_max_handoff_installs_the_variant_and_uninstall_puts_boot_bin_back(self):
        for board in ("j516c", "j514c"):
            with self.subTest(board=board):
                self.fresh_state()
                self.naeem(board)
                proc = self.install(try_=1)
                self.assertIn(f"M3 Max boot loader variant ({MAX_VARIANT})", proc.stderr)
                self.assertIn("GPU handoff stays off (chosen.asahi,t6031-gpu=0)", proc.stderr)
                boot = self.boot.read_bytes()
                self.assertTrue(boot.startswith(b"M1N1:" + flow.M1N1_BASE.encode() + b"\n"), boot[:60])
                # m1n1's GPU handoff (the identity read) stays off in the variant.
                self.assertEqual(MAX_SAFE, ["chosen.asahi,t6031-dcp=1", "chosen.asahi,t6031-gpu=0"])
                self.assertTrue(boot.endswith(tail(MAX_SAFE)), boot[-300:])
                self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {MAX_VARIANT}")
                kept = self.kept_copy()
                self.assertEqual(kept.read_bytes(), self.boot_before)
                rec = (self.state / "m3max-bootbin-backup").read_text().split()
                self.assertEqual(rec, [str(kept), hashlib.sha256(self.boot_before).hexdigest()])
                self.assertIn("# >>> aurora-sep: M3 Max boot loader variant", self.m1n1_conf.read_text())
                # The restore steps from macOS and Linux are printed.
                self.assertIn(f"boot.bin.before-{VERSION}", proc.stderr)
                self.assertIn("diskutil", proc.stderr)
                self.assertIn("NEXT STEPS: M3 MAX BOOT LOADER VARIANT", proc.stdout)
                self.assertIn("--m3-report", proc.stdout)
                out = self.uninstall()
                self.assertEqual(self.boot.read_bytes(), self.boot_before)
                self.assertFalse(self.m1n1_conf.exists())
                # This release's T6031 m1n1 stays installed, so update-m1n1 stays frozen: a
                # rebuild without the switches would run the GPU handoff at every boot.
                self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
                self.assertIn("update-m1n1 stays frozen", out.stdout + out.stderr)
                self.assertFalse(self.state.exists())

    def test_uninstall_puts_back_the_images_own_update_m1n1_configuration(self):
        self.naeem("j516c")
        own = "DTBS=/boot/dtbs/image-own/*.dtb\n"
        self.update_conf.write_text(own + flow.OUR_FREEZE)
        self.install(try_=1)
        self.assertIn("# aurora-sep: build m1n1", self.update_conf.read_text())
        self.uninstall()
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), own + flow.OUR_FREEZE)

    def test_a_kernel_only_max_uninstall_still_lifts_the_freeze(self):
        # Its m1n1 is the image's own (no T6031 handoff), as before the variant existed.
        self.naeem("j516c")
        self.install()
        out = self.uninstall()
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertFalse(self.update_conf.exists())
        self.assertNotIn("update-m1n1 stays frozen", out.stdout + out.stderr)

    def test_the_switch_names_are_the_boot_loaders(self):
        # The bootloader workstream's names (CONTRACT-CHANGES.md): the display and GPU handoffs.
        self.assertEqual(MAX_SWITCHES, ["chosen.asahi,t6031-dcp=1", "chosen.asahi,t6031-gpu=1"])
        self.assertNotIn("gpu-diag", SRC.split("M3_MAX_SWITCHES=", 1)[1].split("\n", 1)[0])

    def test_the_real_t6031_m1n1_package_knows_both_switches(self):
        pkg = ARTIFACTS / "m1n1-aurora-1.6.1.aurora17.t6031.1-1-aarch64.pkg.tar.zst"
        if not pkg.exists():
            self.skipTest(f"{pkg} is not at hand")
        self.naeem()
        for gpu in (0, 1):
            with self.subTest(gpu_handoff=gpu):
                proc = self.run_sh(f"M3_MAX_GPU_HANDOFF={gpu}\nm1n1_pkg_has_handoff '{pkg}' && echo yes", check=False)
                self.assertEqual(proc.stdout.strip(), "yes", proc.stderr)
        # The provisional names it does not have would refuse it.
        proc = self.run_sh(f"M3_MAX_SAFE_SWITCHES='chosen.asahi,t6031-gpu-diag=1'\nm1n1_pkg_has_handoff '{pkg}' || echo no",
                           check=False)
        self.assertEqual(proc.stdout.strip(), "no")

    def test_a_plain_run_stays_kernel_only(self):
        self.naeem("j516c")
        out = self.install().stdout
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertIn("NEXT STEPS: M3 BRING-UP", out)
        self.assertFalse((self.state / "m3max-bootbin-backup").exists())
        # is_m3_kernel_only_chip is unchanged: t6031 and t6034 stay kernel-only chips.
        self.assertIn("grep -Eqx 'apple,(t6031|t6034)' && return 0", SRC)

    def test_a_plain_run_keeps_the_variant_it_has(self):
        self.naeem("j516c")
        self.install(try_=1)
        boot = self.boot.read_bytes()
        out = self.install().stdout
        self.assertIn("this Mac has m1n1's M3 Max boot loader variant", out)
        self.assertEqual(self.boot.read_bytes(), boot)
        # The record still names the boot.bin from before the variant.
        self.assertEqual((self.state / "m3max-bootbin-backup").read_text().split()[1],
                         hashlib.sha256(self.boot_before).hexdigest())

    def test_an_older_variant_needs_a_new_opt_in(self):
        self.naeem("j516c")
        self.install(try_=1)
        (self.state / "m3-mode").write_text("handoff t6031-data-0\n")
        before = self.boot.read_bytes()
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("this release's is another one", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)

    def test_t6034_and_other_m3s_still_refuse(self):
        for board in ("j514m", "j516m", "j504"):
            with self.subTest(board=board):
                self.fresh_state()
                self.mac(board)
                before = self.boot.read_bytes()
                proc = self.install(try_=1, check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("has no display and GPU handoff in m1n1 yet", proc.stderr)
                self.assertEqual(self.boot.read_bytes(), before)

    def test_an_m1n1_without_the_switches_is_refused(self):
        self.fixture(flow.M1N1_PKG, None, flow.SWITCH_NAMES)
        self.naeem("j516c")
        proc = self.install(try_=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("has no M3 Max boot loader variant", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertNotIn("pacman -U", self.log())

    def test_another_stub_or_stage1_is_refused(self):
        for kw, why in (({"stub": "15.6"}, "stub is 15.6"), ({"stage1": "v1.7.0"}, "stage 1 is v1.7.0")):
            with self.subTest(kw=kw):
                self.fresh_state()
                self.naeem("j516c")
                self.mac("j516c", **kw)
                proc = self.install(try_=1, check=False)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)


class KitRefusalTest(KitBase):
    """The consent screen and the Macs the kit refuses: each with nothing changed."""

    def assert_unchanged(self, proc, why):
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertIn(why, proc.stderr)
        self.assertIn("Nothing was changed" if "stopped" not in why else "nothing was changed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertFalse(self.kit.exists())
        self.assertFalse(self.libexec.exists())
        self.assertNotIn("pacman -U", self.log())
        self.assertIsNone(self.oneshot_var())
        self.assertFalse(self.esp_restore().exists())

    def test_consent_yes_on_the_screen(self):
        self.naeem()
        proc = self.setup_kit("")
        self.assertIn("M3 MAX TEST KIT (j516c)", proc.stdout)
        self.assertIn("Type yes and press Enter", proc.stdout)
        self.assertIn("sudo aurora-m3max-kit --restore", proc.stdout)
        self.assertEqual(self.status()["phase"], "running")

    def test_consent_screen_says_what_happens(self):
        self.naeem()
        proc = self.setup_kit("")
        screen = proc.stdout.split("M3 MAX TEST KIT (j516c)")[1].split("Type yes")[0]
        for want in ("writes a report of this Mac as it is now", f"({KIT_KREL})", MAX_VARIANT,
                     "Aurora previous (GPU off)", "restarts the Mac by itself up to 9 times",
                     "hold the power", "the boot loader GPU test changes boot.bin itself",
                     "results so far", "put the boot loader back from macOS",
                     "aurora-m3max-kit-j516c-<date>.tgz", "--status", "--stop", "--restore",
                     "aurora-m3max-kit-RESTORE.txt"):
            self.assertIn(want, screen)

    def test_anything_but_yes_stops(self):
        for answer in ("no\n", "\n", "y\n", ""):
            with self.subTest(answer=answer):
                self.fresh_state()
                shutil.rmtree(self.kit, ignore_errors=True)
                self.naeem()
                self.tty.write_text(answer)
                self.assert_unchanged(self.setup_kit("", check=False), "stopped: nothing was changed")

    def test_no_terminal_and_no_yes(self):
        self.naeem()
        self.tty.unlink()
        self.assert_unchanged(self.setup_kit("", check=False), "has no terminal to ask in")

    def test_unknown_option(self):
        self.naeem()
        proc = self.setup_kit("--yes --fast", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("unknown option --fast after --m3max-kit", proc.stderr)

    def test_other_macs_refuse(self):
        for board, why in (("j516s", "for the 16-core M3 Max"), ("j613", "for the 16-core M3 Max"),
                           ("j314s", "for the 16-core M3 Max"), ("j514m", "needs its own ADT first"),
                           ("j516m", "needs its own ADT first")):
            with self.subTest(board=board):
                self.fresh_state()
                self.mac(board)
                self.boot_before = self.boot.read_bytes()
                self.assert_unchanged(self.setup_kit(check=False), why)

    def test_grub_is_refused(self):
        self.naeem()
        self.assert_unchanged(self.kit_sh("boot_chain() { echo grub; }\nm3max_kit_setup --yes", check=False),
                              "through the Limine boot menu")

    def test_a_mac_that_cannot_arm_a_one_shot(self):
        for what, change, why in (
                ("secure boot", lambda: self.setvar("SecureBoot", GLOBAL, var(6, b"\1")), "Secure Boot is on"),
                ("no BLI", lambda: (self.efivars / f"LoaderInfo-{BLI}").unlink(), "no Boot Loader Interface"),
                ("remember", lambda: self.conf.write_text(self.conf.read_text().replace(
                    "timeout: 3", "timeout: 3\nremember_last_entry: yes")), "remember_last_entry"),
                ("enrolled", lambda: self.w("etc/limine", "ENABLE_ENROLL_LIMINE_CONFIG=yes\n"), "ENABLE_ENROLL")):
            with self.subTest(what=what):
                self.fresh_state()
                self.naeem()
                change()
                self.assert_unchanged(self.setup_kit(check=False), why)

    def test_a_bad_plan_or_a_bad_download(self):
        self.naeem()
        self.plan("format 1\nchip t6031\nboards j516c\nboot data data\nboot g gpu root=/dev/sda\n")
        self.assert_unchanged(self.setup_kit(check=False), "root is not a parameter the kit sets")
        self.plan("format 1\nchip t6031\nboards j514c\nboot data data\n")
        self.assert_unchanged(self.setup_kit(check=False), "the kit's plan is for j514c, not this j516c")
        self.plan(PLAN.read_text())
        (self.tmp / "pkgs/air-gpu-oneshot.sh").write_text("#!/bin/bash\nexit 0\n")
        self.assert_unchanged(self.setup_kit(check=False), "air-gpu-oneshot.sh does not match its published checksum")

    def test_a_run_in_progress_is_refused(self):
        self.naeem()
        self.setup_kit()
        before = self.boot.read_bytes()
        proc = self.setup_kit(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("a test kit run is in progress on this Mac (running)", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)

    def test_low_space_is_refused(self):
        self.naeem()
        self.assert_unchanged(self.kit_sh("M3MAX_KIT_MIN_MB=999999999\nm3max_kit_setup --yes", check=False),
                              "MB free for its results")

    def test_the_kit_checks_run_before_anything_changes(self):
        # Every refusal of the preflight comes before the consent screen and the first change.
        body = SRC.split("m3max_kit_setup() {", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(body.index("m3max_kit_preflight\n  m3max_kit_consent"), body.index("# From here on, the Mac changes."))


class KitRunTest(KitBase):
    """The run across boots."""

    def start(self, params=None, plan=None, initial_params=None):
        self.naeem(params=initial_params)
        if plan is not None:
            self.plan(plan)
        proc = self.setup_kit()
        self.install_new_kernel(params)
        self.variant_facts()
        return proc

    def test_setup(self):
        proc = self.start()
        st = self.status()
        self.assertEqual(st["phase"], "running")
        self.assertEqual(st["next"], "0")
        self.assertEqual(st["kernel"], KIT_KREL)
        self.assertEqual(st["user"], "alice")
        self.assertEqual(st["bootbin"], hashlib.sha256(self.boot.read_bytes()).hexdigest())
        # The baseline report, before the install: Naeem's kernel-only boot.
        self.assertEqual(self.outcome("00-baseline").split()[0], "collected")
        base = self.kit / "stages/00-baseline"
        self.assertIn(f"kernel: {NAEEM['krel']}", (base / "report/system.txt").read_text())
        self.assertIn("dcp-oslog=0/5", (base / "checks.txt").read_text())
        self.assertTrue((base / "report/m1n1-stage2-log.txt").exists())
        self.assertTrue((base / "report/m1n1-stage2-log.raw").exists())
        self.assertTrue((base / "report/adt-allowlist.txt").exists())
        # The variant, with m1n1's GPU handoff off, and its backup; the previous kernel's entry was
        # kept before the packages changed.
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        log = self.log()
        self.assertLess(log.index("keep-limine-entry retain"), log.index("pacman -U"))
        self.assertTrue((self.state / "m3max-bootbin-backup").exists())
        # No start experiment in the kernel that ran the setup: no Mesa.
        self.assertIn("so the kit's Mesa (for GPU jobs) is not installed", proc.stdout)
        self.assertNotIn(MESA_FILE, " ".join(l for l in log.splitlines() if l.startswith("pacman -U")))
        # The runner, its units, the restore steps (home and EFI partition); a restart in 2 minutes.
        self.assertTrue((self.libexec / "install-aurora-sep.sh").read_bytes() == flow.INSTALLER.read_bytes())
        for f in ("air-gpu-oneshot.sh", "air-gpu-job.sh", "aurora-adt-extract.py"):
            self.assertEqual((self.libexec / f).read_bytes(), (self.tmp / "pkgs" / f).read_bytes())
        self.assertIn(f"exec {self.libexec}/install-aurora-sep.sh --m3max-kit-runner", self.kitbin.read_text())
        unit = (self.units / "aurora-m3max-kit.service").read_text()
        self.assertIn(f"ExecStart={self.kitbin} --step", unit)
        self.assertIn("Type=exec", unit)
        self.assertIn("ConditionKernelCommandLine=m3max_kit.boot",
                      (self.units / "aurora-m3max-kit-mark.service").read_text())
        self.assertIn("systemctl enable aurora-m3max-kit.service aurora-m3max-kit-mark.service", log)
        self.assertIn("shutdown -r +2", log)
        for restore in ((self.home_alice / "aurora-m3max-kit-RESTORE.txt").read_text(), self.esp_restore().read_text()):
            self.assertIn(f"boot.bin.before-{VERSION}", restore)
            self.assertIn("sudo aurora-m3max-kit --restore", restore)
            self.assertIn("m1n1/aurora-m3max-kit-interim.tgz", restore)
        self.assertIn("STARTED (j516c)", proc.stdout)
        self.assertIn("  | If the Mac stops in the boot loader", proc.stdout)
        # Nothing is armed yet: the first boot is the default entry.
        self.assertIsNone(self.oneshot_var())
        self.assertIsNone(self.kit_block())

    def test_kernel_without_the_start_experiment(self):
        # This round: the test kernel has no asahi.t6031_start.
        self.start()
        # S1: the data boot (GPU handoff off); then the display boot is armed.
        self.next_boot()
        self.assertEqual(self.outcome("01-data"),
                         "collected t6031-facts=3 dcp-oslog=5/5 in-system-ram=0 gpu-identity=absent cpus=1")
        data = self.kit / "stages/01-data"
        checks = (data / "checks.txt").read_text()
        self.assertIn("asahi,t6031-fw-uuids: gfx=11111111-2222-3333-4444-555555555555 | dcp=DDF38191", checks)
        self.assertIn("dcp-oslog@1000507c000: 0x1000507c000+0x1f000, outside System RAM", checks)
        self.assertIn("== boot.bin: the kit's", checks)
        self.assertIn("m1n1's T6031 lines of this boot", checks)
        # The boot loader's facts, raw, whatever the kernel does.
        dt = data / "dt"
        self.assertEqual((dt / "chosen/asahi,t6031-fw-uuids").read_text(), FW_UUIDS)
        self.assertEqual((dt / "chosen/asahi,t6031-pmp/apple,board-id").read_bytes(), u32(0x2c))
        self.assertTrue((dt / "chosen/asahi,t6031-pmp/apple,tunable-uuid").exists())
        self.assertEqual((dt / "chosen/asahi,t6031-facts/schema-version").read_bytes(), u32(1))
        self.assertTrue((dt / f"reserved-memory/dcp-oslog@{OSLOG[4]:x}/reg").exists())
        self.assertTrue((dt / f"reserved-memory/flash@{NAEEM['log'][0]:x}/reg").exists())
        self.assertEqual((dt / "nodes/dcp/apple,t6031-handoff").read_bytes(), u32(1))
        self.assertTrue((dt / "nodes/dcp/piodma/compatible").exists())
        self.assertTrue((dt / "nodes/opp-table-gpu/opp00/opp-hz").exists())
        self.assertFalse((dt / "nodes/gpu/apple,mtr-fuse-leakage").exists())
        self.assertIn("left out: /soc/gpu@40a400000/apple,mtr-fuse-leakage", (dt / "README.txt").read_text())
        self.assertIn("gpu /soc/gpu@40a400000", (dt / "nodes/aliases.txt").read_text())
        # The handed-over tree, its serial number and raw MAC address zeroed, everything else as it was.
        fdt, orig = (dt / "fdt").read_bytes(), (self.tmp / "fdt").read_bytes()
        self.assertEqual(len(fdt), len(orig))
        self.assertNotIn(mx.SERIAL.encode(), fdt)
        self.assertNotIn(bytes.fromhex(mx.MAC.replace(":", "")), fdt)
        self.assertIn(FW_UUIDS.encode(), fdt)
        self.assertIn(NAEEM["model"].encode(), fdt)
        self.assertEqual((dt / "fdt.txt").read_text(), "2 properties zeroed\n")
        self.assertNotIn(b"serial-number", fdt)
        self.assertIn(b"xxxxxx-xxxxxx\0", fdt)
        self.assertIn("== journal: persistent", checks)
        self.assertTrue((data / "report/m1n1-stage2-log.raw").exists())
        block = self.kit_block()
        cmd = re.search(r"^    cmdline: (.*)$", block, re.M).group(1)
        self.assertTrue(cmd.startswith(NAEEM["cmdline"] + " apple_t6031_display.enable=1 softlockup_panic=1 "
                                       "hung_task_panic=1 systemd.watchdog_sec=30 panic=10 m3max_kit.boot="), cmd)
        self.assertIn("comment: M3 Max test kit, one boot (display ", block)
        self.assertEqual(self.oneshot_var(), var(7, utf16z("m3max-kit")))
        self.assertIn(utf16z("LoaderEntryOneShot") + utf16z("m3max-kit"), self.varfile.read_bytes())
        self.assertEqual(self.status()["boots"], "1")
        self.assertIn("shutdown -r +1", self.log())
        # S2: the display boot, collected. Then, before any GPU stage: the interim results (home
        # and EFI partition), the restore steps again, and boot.bin rebuilt with m1n1's GPU
        # handoff on; the restart into it is announced 3 minutes ahead. No one-shot.
        self.next_boot(lines=["apple-t6031-display: enabled /soc/dcp@386c00000, /soc/display-subsystem and /soc/dcp/piodma",
                              "[drm] Initialized apple 1.0.0 for display-subsystem"])
        self.assertEqual(self.outcome("02-display"), "collected native-display=no success=yes")
        self.assertIn("apple-t6031-display: enabled /soc/dcp@386c00000",
                      (self.kit / "stages/02-display/checks.txt").read_text())
        st = self.status()
        interim = Path(st["interim"])
        self.assertEqual(interim.parent, self.home_alice)
        self.assertTrue(interim.name.endswith("-interim.tgz"))
        self.assertEqual(self.esp_interim().read_bytes(), interim.read_bytes())
        with tarfile.open(interim) as t:
            names = {m.name.removeprefix("./") for m in t.getmembers()}
        self.assertIn("stages/02-display/checks.txt", names)
        self.assertIn("kit-status.txt", names)
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SWITCHES)))
        self.assertIn("chosen.asahi,t6031-gpu=1", self.m1n1_conf.read_text())
        self.assertEqual(st["loader"], "on")
        self.assertTrue(st["armed"].startswith("loader:"))
        self.assertIsNone(self.oneshot_var())
        self.assertIsNone(self.kit_block())
        self.assertIn("shutdown -r +3", self.log())
        self.assertEqual(st["boots"], "2")
        # S3a: the loader boot (default entry, boot.bin with the GPU handoff on). The GPU start is
        # skipped (no start experiment), and so are the jobs; boot.bin goes back to the GPU
        # handoff off, and the kit restarts into a normal boot to finish on it.
        self.gpu_handoff_facts()
        self.next_boot()
        self.assertEqual(self.outcome("03-gpu-handoff"), "collected probe-result=1 power-restored=yes family=14.8.3 gpu-node=okay")
        loader = (self.kit / "stages/03-gpu-handoff/checks.txt").read_text()
        self.assertIn("asahi,t6031-gpu-standin: apple,core-leak-coef opp-microwatt", loader)
        for stage in ("04-gpu-1", "05-gpu-2", "06-gpu-3", "07-gpu-4"):
            self.assertEqual(self.outcome(stage),
                             "skipped kernel has no start experiment (the test kernel has no asahi.t6031_start)")
        self.assertEqual(self.outcome("08-jobs"),
                         "skipped kernel has no start experiment, so there is no GPU to run jobs on")
        st = self.status()
        self.assertEqual(st["phase"], "finishing")
        self.assertEqual(st["loader"], "off")
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        # The last boot, on the variant with the GPU handoff off: collect, pack, upload.
        self.gpu_handoff_off()
        self.next_boot()
        st = self.status()
        self.assertEqual(st["phase"], "done")
        self.assertIn("the kit's, GPU handoff off", self.outcome("99-final"))
        self.assertEqual(Path(st["tarball"]).parent, self.home_alice)
        self.assertIn("systemctl disable aurora-m3max-kit.service aurora-m3max-kit-mark.service", self.log())
        # The end state: the variant with the GPU handoff off stays; the kit's EFI files go.
        self.assertEqual((self.state / "m3-mode").read_text().strip(), f"handoff {MAX_VARIANT}")
        self.assertFalse(self.esp_restore().exists())
        self.assertFalse(self.esp_interim().exists())
        status = self.step("--status").stdout
        self.assertIn(f"Please upload this file (drag it into a comment on the issue): {st['tarball']}", status)
        self.assertRegex(status, r"04 gpu-1 +gpu +skipped kernel has no start experiment")
        # Restarts after the install: into display, into the loader boot, into the final boot.
        self.assertEqual(self.log().count("shutdown -r +1"), 2)
        self.assertEqual(self.log().count("shutdown -r +3"), 1)
        self.next_boot()
        self.assertIn("the kit is done; nothing to do", self.kit_log())

    def test_the_display_boot_collects_debugfs_and_the_backlight(self):
        self.start()
        self.w("sys/class/backlight/apple-panel-bl/max_brightness", "500\n")
        self.w("sys/class/backlight/apple-panel-bl/brightness", "321\n")
        self.w("sys/class/backlight/apple-panel-bl/actual_brightness", "250\n")
        self.w("debug/dcp-j516c/started", "1\n")
        self.w("debug/dri/1/name", "apple dev=display-subsystem\n")
        self.w("debug/dri/1/state", "plane[31]: plane-0\n")
        self.next_boot()
        self.next_boot(lines=["apple-t6031-display: enabled /soc/dcp@386c00000"])
        checks = (self.kit / "stages/02-display/checks.txt").read_text()
        self.assertIn("-- dcp-j516c/started\n1", checks)
        self.assertIn("-- dri/1/state\nplane[31]: plane-0", checks)
        self.assertIn("== backlight apple-panel-bl: set 250 of 500, actual 250; put back 321", checks)
        self.assertEqual((self.tmp / "sys/class/backlight/apple-panel-bl/brightness").read_text(), "321\n")

    def test_a_volatile_journal_is_said(self):
        self.start()
        shutil.rmtree(self.tmp / "var-log-journal")
        self.next_boot()
        self.assertIn("== journal: NOT persistent", (self.kit / "stages/01-data/checks.txt").read_text())

    def test_the_loader_boot_does_not_come_back(self):
        self.start()
        self.through_display()
        # The Mac did not come back with the GPU handoff on: boot.bin put back from macOS.
        self.put_back_from_macos()
        self.gpu_handoff_off()
        self.next_boot()
        out = self.outcome("03-gpu-handoff")
        self.assertTrue(out.startswith("hung the Mac did not come back with the boot loader's GPU handoff on"), out)
        self.assertEqual(self.outcome("04-gpu-1"), "skipped the boot loader's GPU handoff stage did not come back")
        st = self.status()
        self.assertEqual(st["phase"], "done")
        # The end state after a boot-loader-level failure: the boot.bin from before the kit, and
        # the kernel-only state around it.
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertFalse((self.state / "m3max-bootbin-backup").exists())
        self.assertFalse(self.esp_interim().exists())
        # The variant's m1n1 itself booted: it is not recorded as failed.
        self.assertFalse((self.state / "m1n1-failed").exists())
        self.assertIn("the boot.bin from before the kit", self.outcome("99-final"))
        files = self.tarball()
        self.assertIn("stages/02-display/checks.txt", files)

    def test_the_variant_does_not_boot(self):
        self.start()
        # The first boot after the install did not come up; boot.bin was put back from macOS.
        self.put_back_from_macos()
        self.next_boot()
        out = self.outcome("01-data")
        self.assertTrue(out.startswith("hung the boot loader variant did not boot"), out)
        self.assertEqual(self.outcome("02-display"), "skipped the boot loader variant did not boot")
        self.assertEqual(self.status()["phase"], "done")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        # Its m1n1 is recorded as failed on this Mac, so no later run puts it back.
        self.assertIn(self.bin_shas[self.m1n1_pkg], (self.state / "m1n1-failed").read_text())
        self.assertIsNone(self.oneshot_var())

    def test_gpu_success_then_jobs_with_a_mesa(self):
        self.start(params=self.PARAMS_WITH_START, initial_params=self.PARAMS_WITH_START)
        # The test kernel has the start experiment: the kit's Mesa goes on, in its own transaction.
        self.assertTrue([l for l in self.log().splitlines() if l.startswith("pacman -U") and MESA_FILE in l])
        self.assertEqual(self.status()["mesa"], "mesa-m3-g15c")
        prefix = self.tmp / "opt-mesa"
        (prefix / "share/vulkan/icd.d").mkdir(parents=True)
        (prefix / "share/vulkan/icd.d/asahi_icd.aarch64.json").write_text("{}")
        (prefix / "bin").mkdir()
        (prefix / "bin/g15c-first-job").write_text(
            '#!/bin/bash\necho "first-job $* G15C=$ASAHI_M3_G15C" >>"$FAKE/log"\nmkdir -p "$1"\n'
            'printf "VERDICT compute-check exit=0 pass\\nVERDICT vk-render-check exit=0 pass\\n'
            'VERDICT gl-render-check exit=0 pass\\nasahi: G15C admitted\\n" >"$1/summary.txt"\n'
            'echo log >"$1/compute-check.log"\n')
        (prefix / "bin/g15c-first-job").chmod(0o755)
        (self.kit / "plan").write_text((self.kit / "plan").read_text().replace("/opt/mesa-m3-g15c", str(prefix)))
        self.through_display()
        self.next_boot()                                              # the loader boot
        self.assertEqual(self.status()["loader_ok"], "1")
        self.assertIn("asahi.t6031_start=1 asahi.t6031_pstate_cap=1 softlockup_panic", self.kit_block())
        # The start's status file and crash log, copied by the mark unit as the boot comes up.
        self.w("debug/asahi-t6031/status", "version=1 phase=firmware-running initdata_version=0xc08e21e83800490 "
                                           "io_mappings=12 crashlog_bytes=0 outcome=0\n")
        self.w("sys/class/devcoredump/devcd1/data", b"crash bytes")
        self.next_boot(lines=["asahi 40a400000.gpu: M3 G15C start: armed (asahi.t6031_start=1, one attempt this boot)",
                              "asahi 40a400000.gpu: M3 G15C verdict: firmware-running: the firmware accepted the InitData"])
        early = self.kit / "stages/04-gpu-1/early"
        self.assertIn("phase=firmware-running", (early / "asahi-t6031-status.txt").read_text())
        self.assertEqual((early / "devcd1.bin").read_bytes(), b"crash bytes")
        self.assertEqual(self.outcome("04-gpu-1"), "collected gpu-start=success phase=firmware-running")
        self.assertIn("M3 G15C start: armed", (self.kit / "stages/04-gpu-1/checks.txt").read_text())
        self.assertEqual(self.status()["gpu_ok"], "3")
        for stage in ("05-gpu-2", "06-gpu-3", "07-gpu-4"):
            self.assertEqual(self.outcome(stage), "skipped GPU boot 04-gpu-1 succeeded already")
        cmd = re.search(r"^    cmdline: (.*)$", self.kit_block(), re.M).group(1)
        self.assertIn(" asahi.t6031_start=1 asahi.t6031_pstate_cap=1 asahi.m3_expose=1 softlockup_panic=1", cmd)
        self.next_boot(lines=["asahi 40a400000.gpu: M3 G15C verdict: job-completed: the first compute job finished, 8000 ns of GPU time"])
        jobs = (self.kit / "stages/08-jobs/checks.txt").read_text()
        self.assertIn("job 1 first-job: 3 passed;", jobs)
        self.assertIn("job 2 render: skipped (no desktop session of alice", jobs)
        self.assertIn("gpu gpu-start=success", jobs)
        self.assertRegex(self.log(), r"(?m)^first-job \S+/mesa 5 G15C=1$")
        self.assertIn("G15C=1", [l for l in self.log().splitlines() if l.startswith("first-job")][0])
        self.assertTrue((self.kit / "stages/08-jobs/mesa/summary.txt").exists())
        self.assertEqual(self.outcome("08-jobs"), "collected passed=1 failed=0 skipped=1")
        # Back to the GPU handoff off, then the final boot.
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        self.gpu_handoff_off()
        self.next_boot()
        self.assertEqual(self.status()["phase"], "done")

    def test_the_kits_mesa_needs_what_is_installed(self):
        # A dependency this Mac does not have at that version: the Mesa is left out, nothing upgraded.
        self.mesa_fixture(depends=("glibc>=9.99",))
        proc = self.start(params=self.PARAMS_WITH_START, initial_params=self.PARAMS_WITH_START)
        self.assertIn("the kit's Mesa needs glibc>=9.99", proc.stderr)
        self.assertNotIn(MESA_FILE, " ".join(l for l in self.log().splitlines() if l.startswith("pacman -U")))
        # A package with a file outside /opt is left out too.
        self.fresh_state()
        shutil.rmtree(self.kit)
        self.mesa_fixture(files=("opt/mesa-m3-g15c/bin/x", "usr/lib/libGL.so"))
        proc = self.start(params=self.PARAMS_WITH_START, initial_params=self.PARAMS_WITH_START)
        self.assertIn("has files outside /opt", proc.stderr)

    def test_gpu_sets_stop_at_the_first_success(self):
        # The shipped sweep: the defaults first, then one knob at a time.
        params = self.PARAMS_WITH_START + [b"asahi.t6031_initdata_version", b"asahi.t6031_clkgen", b"asahi.t6031_fender"]
        self.start(params=params)
        self.through_display()
        self.next_boot()                                             # loader
        self.next_boot(lines=["M3 G15C verdict: initdata-rejected (ETIMEDOUT): the firmware did not acknowledge"])
        self.assertEqual(self.outcome("04-gpu-1"), "collected gpu-start=no-success")
        self.assertIn("asahi.t6031_pstate_cap=1 asahi.t6031_initdata_version=0x0c89c357839204b8", self.kit_block())
        # A check that failed after the firmware ran is not a success.
        self.next_boot(lines=["M3 G15C verdict: firmware-running-check-failed (EIO): the firmware accepted the InitData"])
        self.assertEqual(self.outcome("05-gpu-2"), "collected gpu-start=no-success")
        self.assertIn("asahi.t6031_clkgen=e1c", self.kit_block())
        self.next_boot(lines=["M3 G15C verdict: firmware-running: the firmware accepted the InitData (version 0x1)"])
        self.assertEqual(self.outcome("06-gpu-3"), "collected gpu-start=success")
        self.assertEqual(self.outcome("07-gpu-4"), "skipped GPU boot 06-gpu-3 succeeded already")
        self.assertEqual(self.outcome("08-jobs"), "skipped no Mesa with a Vulkan driver in /opt/mesa-m3-g15c")

    def test_a_failed_gpu_handoff_skips_the_start(self):
        self.start(params=self.PARAMS_WITH_START)
        self.through_display()
        self.gpu_handoff_facts(result=3)
        self.next_boot()
        self.assertEqual(self.outcome("03-gpu-handoff"), "collected probe-result=3 power-restored=yes family=14.8.3 gpu-node=okay")
        self.assertEqual(self.outcome("04-gpu-1"), "skipped the boot loader's GPU handoff did not pass (see the loader stage)")
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))

    def test_hung_stages_are_recorded_and_the_kit_goes_on(self):
        plan = re.sub(r"(?m)^boot gpu-[234] .*\n", "", PLAN.read_text())
        self.start(params=self.PARAMS_WITH_START, plan=plan)
        self.next_boot()
        armed_id = self.status()["armed"].split()[0]
        # The display boot hangs after it reached userspace (the mark unit ran), then the power
        # button: the next boot is the normal entry. Its journal kept that boot.
        self.next_boot(collect=False)
        self.assertTrue((self.kit / f"reached-{armed_id}").exists())
        (self.fake / "prev.json").write_text(
            '{"MESSAGE": "Kernel command line: root=x m3max_kit.boot=%s", "_BOOT_ID": "abc123"}\n' % armed_id)
        (self.fake / "prev-journal.txt").write_text(journal(KIT_KREL, ["dcp: hang"]))
        self.next_boot()
        out = self.outcome("02-display")
        self.assertTrue(out.startswith("hung the boot reached userspace"), out)
        self.assertIn("its journal is in previous-boot-kernel-log.txt", out)
        self.assertIn("dcp: hang", (self.kit / "stages/02-display/previous-boot-kernel-log.txt").read_text())
        # It went on: the loader boot is armed, with the interim results.
        self.assertTrue(self.status()["armed"].startswith("loader:"))
        self.gpu_handoff_facts()
        self.next_boot()
        # The GPU boot hangs before userspace, and leaves no journal.
        (self.fake / "prev.json").unlink()
        self.next_boot(collect=False, mark=False)
        self.next_boot()
        out = self.outcome("04-gpu-1")
        self.assertTrue(out.startswith("hung-early no sign that the boot reached userspace"), out)
        self.assertIn("it left no journal", out)
        self.assertEqual(self.outcome("05-jobs"), "skipped no GPU boot succeeded")
        self.assertEqual(self.status()["phase"], "finishing")
        self.gpu_handoff_off()
        self.next_boot()
        self.assertEqual(self.status()["phase"], "done")
        files = self.tarball()
        self.assertIn("stages/02-display/previous-boot-kernel-log.txt", files)
        self.assertIn(b"] host kernel: dcp: hang", files["stages/02-display/previous-boot-kernel-log.txt"])

    def test_a_one_shot_limine_never_took_stops_the_kit(self):
        self.start(params=self.PARAMS_WITH_START)
        self.next_boot()
        # Limine booted the default entry and left the variable set.
        self.next_boot(consume=False)
        self.assertTrue(self.outcome("02-display").startswith("not-booted the one-shot was still set"))
        self.assertEqual(self.outcome("03-gpu-handoff"), "skipped the kit's one-shot boots do not work on this Mac")
        self.assertIsNone(self.oneshot_var())
        self.assertIsNone(self.kit_block())
        self.assertEqual(self.status()["phase"], "done")
        # The variant (GPU handoff off) stays: nothing failed at the boot loader level.
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))

    def test_the_boot_cap(self):
        plan = PLAN.read_text().replace("max-boots 8", "max-boots 1")
        self.start(params=self.PARAMS_WITH_START, plan=plan)
        self.next_boot()
        self.next_boot()
        self.assertEqual(self.outcome("03-gpu-handoff"), "skipped the plan's limit of 1 experimental boots was reached")
        self.assertEqual(self.outcome("04-gpu-1"), "skipped the plan's limit of 1 experimental boots was reached")
        self.assertEqual(self.status()["boots"], "1")
        self.assertEqual(self.status()["phase"], "finishing")
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        # The code's own ceiling, whatever a plan says.
        proc = self.kit_sh(f"printf 'format 1\\nchip t6031\\nboards j516c\\nmax-boots 13\\nboot data data\\n' >'{self.tmp}/p'\n"
                           f"m3max_kit_plan_check '{self.tmp}/p'", check=False)
        self.assertIn("max-boots must be 1 to 12", proc.stderr)

    def test_a_parameter_the_kernel_lacks_costs_no_boot(self):
        plan = PLAN.read_text().replace("boot display display apple_t6031_display.enable=1",
                                        "boot display display apple_t6031_display.enable=1 apple_t6031_display.notch=1")
        self.start(params=self.PARAMS_WITH_START, plan=plan)
        self.next_boot()
        self.assertEqual(self.outcome("02-display"), "skipped the test kernel has no apple_t6031_display.notch")
        self.assertTrue(self.status()["armed"].startswith("loader:"))
        self.assertEqual(self.status()["boots"], "1")

    def test_another_kernel_from_the_menu_waits(self):
        self.start()
        self.next_boot(krel=NAEEM["krel"])
        self.assertIn("not the kit's kernel", self.kit_log())
        self.assertEqual(self.status()["next"], "0")
        self.assertIsNone(self.kit_block())
        self.next_boot()
        self.assertEqual(self.outcome("01-data").split()[0], "collected")

    def test_stop(self):
        self.start()
        self.next_boot()
        proc = self.step("--stop")
        self.assertIn("Stopped: nothing more is armed", proc.stdout)
        self.assertEqual(self.outcome("02-display"), "cancelled the kit was stopped before this boot ran")
        self.assertIsNone(self.oneshot_var())
        self.assertIsNone(self.kit_block())
        self.assertIn("shutdown -c", self.log())
        st = self.status()
        self.assertEqual(st["phase"], "stopped")
        self.assertTrue(Path(st["tarball"]).exists())
        # The variant stays; later boots do nothing.
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        self.next_boot()
        self.assertEqual(self.status()["phase"], "stopped")

    def test_stop_while_the_gpu_handoff_is_on(self):
        self.start()
        self.through_display()
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SWITCHES)))
        self.step("--stop")
        # boot.bin goes back to the variant with the GPU handoff off.
        self.assertTrue(self.boot.read_bytes().endswith(tail(MAX_SAFE)))
        self.assertEqual(self.status()["loader"], "off")
        self.assertEqual(self.outcome("03-gpu-handoff"), "cancelled the kit was stopped before this boot ran")

    def test_restore_drill(self):
        self.start()
        self.next_boot()                                    # the data boot; the display boot is armed
        proc = self.step("--restore")
        self.assertIn("Restored: the kit is off", proc.stdout)
        # The boot.bin from before the kit, byte for byte, and the kernel-only state around it.
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        self.assertNotIn("M3 Max boot loader variant", self.m1n1_conf.read_text() if self.m1n1_conf.exists() else "")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertFalse((self.state / "m3max-bootbin-backup").exists())
        self.assertFalse((self.state / "m1n1-installed").exists())
        self.assertFalse(self.esp_restore().exists())
        # No one-shot, no kit entry, no units, no scheduled restart; the results are packed.
        self.assertIsNone(self.oneshot_var())
        self.assertIsNone(self.kit_block())
        self.assertNotIn(b"m3max-kit", self.varfile.read_bytes())
        log = self.log()
        self.assertIn("systemctl disable aurora-m3max-kit.service aurora-m3max-kit-mark.service", log)
        self.assertIn("shutdown -c", log)
        st = self.status()
        self.assertEqual(st["phase"], "restored")
        self.assertTrue(Path(st["tarball"]).exists())
        self.assertEqual(self.outcome("02-display"), "cancelled the kit was stopped before this boot ran")
        # The next boot is a plain kernel-only boot: the kit does nothing, and a plain install
        # keeps boot.bin as it is.
        self.next_boot()
        self.assertEqual(self.status()["phase"], "restored")
        self.install()
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        # A second --restore changes nothing.
        proc = self.step("--restore")
        self.assertIn("boot.bin: nothing to put back", proc.stdout)
        self.assertEqual(self.boot.read_bytes(), self.boot_before)

    def test_restore_while_the_gpu_handoff_is_on(self):
        self.start()
        self.through_display()
        self.step("--restore")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        self.assertFalse(self.esp_interim().exists())
        self.assertEqual(self.status()["phase"], "restored")

    def test_restore_from_the_release_script_without_the_runner(self):
        self.start()
        shutil.rmtree(self.libexec)
        shutil.rmtree(self.kit)
        self.kit_sh("m3max_kit_from_release --restore")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        self.assertFalse(self.esp_restore().exists())

    def test_restore_refuses_a_changed_backup(self):
        self.start()
        self.next_boot()
        self.kept_copy().write_bytes(b"something else")
        proc = self.step("--restore", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("missing or changed", proc.stderr)
        self.assertNotEqual(self.boot.read_bytes(), b"something else")
        # The kit itself is off: nothing armed, no restart.
        self.assertIsNone(self.oneshot_var())

    def test_uninstall_stops_the_kit_and_puts_boot_bin_back(self):
        self.start()
        self.next_boot()
        # The installed runner, as sudo would run it: this script's --stop.
        self.kitbin.write_text("#!/bin/bash\nexit 0\n")
        self.kit_sh("m3max_kit_runner --stop")
        self.kit_sh("uninstall_all")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertFalse(self.libexec.exists())
        self.assertFalse((self.units / "aurora-m3max-kit.service").exists())
        self.assertTrue((self.kit / "status").exists())             # the results stay
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)

    def test_restore_then_uninstall_keeps_update_m1n1_frozen(self):
        self.start()
        self.next_boot()
        self.step("--restore")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.kitbin.write_text("#!/bin/bash\nexit 0\n")
        out = self.kit_sh("uninstall_all")
        self.assertEqual(self.boot.read_bytes(), self.boot_before)
        self.assertEqual(self.update_conf.read_text(), flow.OUR_FREEZE)
        self.assertIn("update-m1n1 stays frozen", out.stdout + out.stderr)

    def test_tarball_contents_and_masking(self):
        self.start()
        self.next_boot()
        self.next_boot(lines=["apple-t6031-display: enabled /soc/dcp@386c00000", f"some line from {mx.FQDN} for bob {mx.SERIAL}"])
        self.gpu_handoff_facts()
        self.next_boot()
        self.gpu_handoff_off()
        self.next_boot()
        files = self.tarball()
        names = set(files)
        for want in ("README.txt", "kit-status.txt", "plan.txt", "kit-log.txt",
                     "stages/00-baseline/report/system.txt", "stages/00-baseline/checks.txt",
                     "stages/01-data/report/adt-allowlist.txt", "stages/01-data/report/m1n1-stage2-log.txt",
                     "stages/01-data/report/m1n1-stage2-log.raw", "stages/01-data/report/iomem.txt",
                     "stages/01-data/dt/chosen/asahi,t6031-fw-uuids", "stages/01-data/checks.txt",
                     "stages/02-display/report/kernel-log.txt", "stages/02-display/meta.txt",
                     "stages/03-gpu-handoff/dt/chosen/asahi,t6031-gpu-powered-identity/probe-result",
                     "stages/03-gpu-handoff/dt/chosen/asahi,t6031-gpu-image/firmware-family",
                     "stages/03-gpu-handoff/checks.txt",
                     "stages/04-gpu-1/outcome.txt", "stages/99-final/report/display.txt"):
            self.assertIn(want, names)
        status = files["kit-status.txt"].decode()
        self.assertIn("M3 Max test kit (j516c): done", status)
        self.assertRegex(status, r"01 data +data +collected t6031-facts=3")
        self.assertRegex(status, r"03 gpu-handoff +loader +collected probe-result=1")
        self.assertIn("boot.bin: sha256", files["stages/03-gpu-handoff/report/system.txt"].decode())
        # Nothing of the host, the users, the serial numbers or the MAC addresses.
        for name, data in files.items():
            for secret in mx.SECRETS:
                self.assertNotIn(secret.encode(), data, name)
        self.assertIn(b"] host kernel: some line from host for user SERIAL",
                      files["stages/02-display/report/kernel-log.txt"])
        # The firmware image UUIDs stay.
        self.assertIn(b"DDF38191-93B3-324A-BC8F-643006F5AC82", files["stages/01-data/dt/chosen/asahi,t6031-fw-uuids"])

    def test_end_to_end_on_naeems_values(self):
        """Naeem's J516C, as his second report shows it, through the whole default plan."""
        proc = self.start()
        self.assertIn("up to 9 times", proc.stdout)
        self.next_boot()
        data = (self.kit / "stages/01-data/checks.txt").read_text()
        for a in OSLOG:
            self.assertIn(f"dcp-oslog@{a:x}: 0x{a:x}+0x1f000, outside System RAM", data)
        base = (self.kit / "stages/00-baseline/report/reserved-memory.txt").read_text()
        self.assertIn("flash@10004e74000\tcompatible=phram\tlabel=adt\treg=0x10004e74000+0x8c000", base)
        self.assertIn("flash@10bca228000\tcompatible=phram\tlabel=m1n1_stage2.log\treg=0x10bca228000+0x4000", base)
        system = (self.kit / "stages/01-data/report/system.txt").read_text()
        self.assertIn("m1n1-stage2-version: v1.6.1-omarchy.aurora17.t6031.1", system)
        self.assertIn(f"m3-mode: handoff {MAX_VARIANT}", system)
        self.next_boot(lines=["apple-t6031-display: enabled /soc/dcp@386c00000"])
        self.gpu_handoff_facts()
        self.next_boot()
        self.gpu_handoff_off()
        self.next_boot()
        st = self.status()
        self.assertEqual(st["phase"], "done")
        self.assertEqual(st["boots"], "2")
        files = self.tarball()
        self.assertEqual(sorted({n.split("/")[1] for n in files if n.startswith("stages/")}),
                         ["00-baseline", "01-data", "02-display", "03-gpu-handoff", "04-gpu-1", "05-gpu-2", "06-gpu-3",
                          "07-gpu-4", "08-jobs", "99-final"])
        self.assertIn(b"model: Apple MacBook Pro (16-inch, M3 Max, 16 CPU cores, Nov 2023)",
                      files["stages/01-data/report/system.txt"])


class KitPlanTest(KitBase):
    def check(self, text):
        p = self.tmp / "p.plan"
        p.write_text(text)
        return self.kit_sh(f"m3max_kit_plan_check '{p}'", check=False)

    def test_the_shipped_plan(self):
        proc = self.check(PLAN.read_text())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        boots = [l.split()[1:3] for l in proc.stdout.splitlines() if l.startswith("boot ")]
        self.assertEqual(boots, [["data", "data"], ["display", "display"], ["gpu-handoff", "loader"],
                                 ["gpu-1", "gpu"], ["gpu-2", "gpu"], ["gpu-3", "gpu"], ["gpu-4", "gpu"], ["jobs", "jobs"]])
        self.assertIn("max-boots 8", proc.stdout)
        self.assertIn("success gpu M3 G15C verdict: (firmware-running|job-completed):", proc.stdout)
        self.assertIn("safety softlockup_panic=1 hung_task_panic=1 systemd.watchdog_sec=30", proc.stdout)
        self.assertIn("mesa-prefix /opt/mesa-m3-g15c", proc.stdout)
        self.assertIn("job-env ASAHI_M3_EXPERIMENTAL=1 ASAHI_M3_G15C=1", proc.stdout)
        self.assertIn("jobs first-job render", proc.stdout)
        self.assertIn("loader-delay 180", proc.stdout)
        # The release pins the plan's bytes.
        sha = hashlib.sha256(PLAN.read_bytes()).hexdigest()
        self.assertRegex(SRC, rf'(?m)^M3MAX_KIT_PLAN="m3max-kit\.plan {sha}"$')

    def test_refusals(self):
        head = "format 1\nchip t6031\nboards j514c j516c\n"
        for body, why in (
                ("boot data data\nboot g gpu init=/bin/sh", "init is not a parameter the kit sets"),
                ("boot data data\nboot g gpu systemd.unit=rescue.target", "systemd.unit is not a parameter"),
                ("boot data data\nboot g gpu m3max_kit.boot=1", "m3max_kit.boot is not a parameter"),
                ("boot data data\nboot g gpu asahi.x=1;reboot", "the value must be"),
                ("boot data data\nboot g gpu", "a gpu boot needs parameters"),
                ("boot data data\nboot data data", "boot data is given twice"),
                ("boot data data\nboot j jobs @gpu\nboot g gpu asahi.x=1", "the jobs boot comes after every other boot"),
                ("boot data data\nboot g display @gpu", "@gpu belongs to a jobs boot only"),
                ("boot data data\nboot l loader asahi.x=1", "the loader boot takes no parameters"),
                ("boot data data\nboot l loader\nboot m loader", "only one loader boot"),
                ("boot data data\nboot g gpu asahi.x=1\nboot l loader", "the gpu and jobs boots come after the loader boot"),
                ("boot data data\nloader-delay 5", "loader-delay must be 30 to 900"),
                ("boot data data\nsuccess gpu (", "does not compile"),
                ("boot data data\nmesa-prefix /usr", "mesa-prefix must be one path under /opt"),
                ("boot data data\nreboot-delay 5", "reboot-delay must be 30 to 900"),
                ("boot data data\njobs compute fork", "jobs takes 1 to 6"),
                ("boot data data\nwhatever 1", "unknown directive"),
                ("", "it names no boots")):
            with self.subTest(body=body):
                proc = self.check(head + body + "\n")
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(why, proc.stderr)

    def test_the_real_mesa_package(self):
        # The Mesa workstream's package, when at hand: every file under /opt, the first-job and
        # probe programs, by the name and sha256 the installer pins.
        pkg = ARTIFACTS / MESA_FILE
        if not pkg.exists():
            self.skipTest(f"{pkg} is not at hand")
        sha = re.search(r'^M3MAX_KIT_MESA_PACKAGE="\S+ ([0-9a-f]{64})"$', SRC, re.M).group(1)
        self.assertEqual(hashlib.sha256(pkg.read_bytes()).hexdigest(), sha)
        names = subprocess.run(["bsdtar", "-tf", str(pkg)], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual([n for n in names if not n.startswith(".") and not n.startswith("opt/")], [])
        self.assertIn("opt/mesa-m3-g15c/bin/g15c-first-job", names)
        self.assertIn("opt/mesa-m3-g15c/bin/mesa-m3-probe", names)


class ReportAndSmcTest(KitBase):
    """G1 in the report, and the t6034 SMC line."""

    def test_the_report_has_m1n1s_log(self):
        self.naeem()
        _, files = self.report_files()
        log = files["m1n1-stage2-log.txt"].decode()
        self.assertIn("m1n1's stage 2 log", log)
        self.assertNotIn(mx.HOST, log)
        self.assertIn("m1n1-stage2-log.raw", files)
        check = files["adt-check.txt"].decode()
        self.assertIn("--stage2-log", check)
        self.assertIn("log: done", check)
        self.assertIn("log raw: done", check)
        self.assertIn("m1n1-stage2-log.txt", files["README.txt"].decode())

    def test_no_log_without_the_adt_step(self):
        self.naeem()
        self.extra_env["FAKE_NO_PHRAM"] = "1"
        _, files = self.report_files()
        self.assertIn("m1n1's log was not read: no phram module", files["m1n1-stage2-log.txt"].decode())
        self.assertNotIn("m1n1-stage2-log.raw", files)


if __name__ == "__main__":
    unittest.main()
