"""install_all and uninstall_all end to end, against a fake Mac.

The real functions run; pacman, update-m1n1, curl, findmnt and systemctl are
small stubs that record what happened. The fake update-m1n1 behaves like the
real one where it matters here: it honours M1N1_UPDATE_DISABLED, builds
boot.bin from the installed m1n1 and device trees, and appends /etc/m1n1.conf's
chosen.* lines. pacman -U and the m1n1/kernel installs run it, as the hooks do.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parent.parent / "install-aurora-sep.sh"
# The installer's VERSION names the boot.bin copy it keeps on the EFI partition.
VERSION = re.search(r"^VERSION=(\S+)$", INSTALLER.read_text(), re.M).group(1)

BOARDS = {
    "j516s": ["apple,j516s", "apple,t6030", "apple,arm-platform"],
    "j514s": ["apple,j514s", "apple,t6030", "apple,arm-platform"],
    "j613": ["apple,j613", "apple,t8122", "apple,arm-platform"],
    "j314s": ["apple,j314s", "apple,t6000", "apple,arm-platform"],
}
SWITCHES = b"chosen.asahi,t6030-gpu=1\nchosen.asahi,t6030-dcp=1\nchosen.asahi,t6030-dcpext=1\n"
BRINGUP_FREEZE = (
    "# Added by m3-gpu-work/scripts/install-m3gpu.sh: keep the M3 GPU candidate boot.bin.\n"
    "# Undo with rollback-m3gpu.sh (then run: sudo update-m1n1).\n"
    "M1N1_UPDATE_DISABLED=1\n"
)
OUR_FREEZE = (
    "# >>> aurora-sep: keep this M3's boot.bin as it is (remove with: install-aurora-sep.sh --uninstall)\n"
    "M1N1_UPDATE_DISABLED=1\n"
    "# <<< aurora-sep: keep this M3's boot.bin as it is\n"
)
AURORA3 = "m1n1-aurora-1.6.1.aurora3-1-aarch64.pkg.tar.zst"
AURORA6 = "m1n1-aurora-1.6.1.aurora6-1-aarch64.pkg.tar.zst"
OTHERS = [
    "linux-aurora-7.1.12.aurora2-11.36-aarch64.pkg.tar.zst",
    "linux-aurora-headers-7.1.12.aurora2-11.36-aarch64.pkg.tar.zst",
    "libfprint-1.94.100-1.1-aarch64.pkg.tar.zst",
    "aurora-touchid-20261003-1-any.pkg.tar.zst",
]

PACMAN = r"""#!/bin/bash
echo "pacman $*" >>"$FAKE/log"
op=$1; shift
case $op in
  -Q)
    for p in "$@"; do
      case $p in
        linux-aurora | linux-asahi) grep -qx "$p" "$FAKE/installed" || exit 1 ;;
        m1n1-aurora) grep -q '^m1n1-aurora' "$FAKE/m1n1" || exit 1 ;;
        m1n1) grep -q '^m1n1-stock' "$FAKE/m1n1" || exit 1 ;;
      esac
      echo "$p 1-1"
    done ;;
  -Qq) exit 1 ;;
  -Qlq)
    [[ $1 == linux-aurora ]] && grep -qx linux-aurora "$FAKE/installed" &&
      echo /usr/lib/modules/7.1.12-2-11.36-sep-ARCH/dtbs/fake.dtb
    exit 0 ;;
  -U)
    if [[ -n ${FAKE_FAIL_U:-} ]]; then echo "error: failed to commit transaction" >&2; exit 1; fi
    for f in "$@"; do
      [[ $f == -* || $f == 4 ]] && continue
      b=$(basename "$f")
      case $b in
        m1n1-aurora-*) echo "${b%-aarch64.pkg.tar.zst}" >"$FAKE/m1n1"; printf 'M1N1:%s\n' "${b%-aarch64.pkg.tar.zst}" >"$FAKE/m1n1.bin" ;;
        linux-aurora-headers-*) ;;
        linux-aurora-*) sed -i '/^linux-asahi$/d' "$FAKE/installed"; echo linux-aurora >>"$FAKE/installed" ;;
      esac
    done
    update-m1n1 hook ;;
  -S | -Sy)
    hook=0
    for p in "$@"; do
      case $p in
        m1n1) echo m1n1-stock >"$FAKE/m1n1"; printf 'M1N1:m1n1-stock\n' >"$FAKE/m1n1.bin"; hook=1 ;;
        linux-asahi) sed -i '/^linux-aurora$/d' "$FAKE/installed"; echo linux-asahi >>"$FAKE/installed"; hook=1 ;;
      esac
    done
    if ((hook)); then update-m1n1 hook; fi ;;
esac
exit 0
"""

UPDATE_M1N1 = r"""#!/bin/bash
echo "update-m1n1 $*" >>"$FAKE/log"
# As the real one: sourced by sh under set -e (bash in POSIX mode on Arch).
if [[ -f $FAKE_UPDATE_CONF ]]; then
  if ! sh -c 'set -e; . "$1"' _ "$FAKE_UPDATE_CONF" 2>/dev/null; then
    echo "update-m1n1 failed sourcing" >>"$FAKE/log"
    exit 1
  fi
  if sh -c 'set -e; . "$1"; [ -n "${M1N1_UPDATE_DISABLED:-}" ]' _ "$FAKE_UPDATE_CONF" 2>/dev/null; then
    echo "update-m1n1 frozen" >>"$FAKE/log"
    exit 0
  fi
fi
dtbs=$(sh -c 'set -e; DTBS=; [ -f "$1" ] && . "$1"; echo "$DTBS"' _ "$FAKE_UPDATE_CONF")
{
  cat "$FAKE/m1n1.bin"
  printf 'DTBS:%s\n' "${dtbs:-asahi}"
  printf 'UBOOT'
  grep -E '^chosen\.' "$FAKE_M1N1_CONF" 2>/dev/null || true
} >"$FAKE_ESP/m1n1/boot.bin"
echo "update-m1n1 rebuilt" >>"$FAKE/log"
"""

CURL = r"""#!/bin/bash
out= url=
while (($#)); do
  case $1 in -o) out=$2; shift ;; http*) url=$1 ;; esac
  shift
done
echo "curl $url" >>"$FAKE/log"
cp "$FAKE_PKGS/$(basename "$url")" "$out"
"""


class M3FlowTest(unittest.TestCase):
    def setUp(self):
        if not shutil.which("bsdtar") or not shutil.which("zstd"):
            self.skipTest("bsdtar and zstd are needed to build the fixture m1n1 packages")
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        for d in ("dt/chosen", "esp/m1n1", "esp/asahi", "etc/default", "state", "pkgs", "bin", "fake"):
            (self.tmp / d).mkdir(parents=True)
        self.boot = self.tmp / "esp/m1n1/boot.bin"
        self.update_conf = self.tmp / "etc/default/update-m1n1"
        self.m1n1_conf = self.tmp / "etc/m1n1.conf"
        self.state = self.tmp / "state"
        self.fake = self.tmp / "fake"
        for name, body in (("pacman", PACMAN), ("update-m1n1", UPDATE_M1N1), ("curl", CURL),
                           ("findmnt", "#!/bin/sh\ncase \"$*\" in *PARTUUID*) echo fake-uuid ;; *) echo vfat ;; esac\n"),
                           ("systemctl", "#!/bin/sh\nexit 0\n")):
            (self.tmp / "bin" / name).write_text(body)
            (self.tmp / "bin" / name).chmod(0o755)
        self.extra_env = {}
        self.shas = {}
        for name in OTHERS:
            self.fixture(name, name.encode())
        self.fixture(AURORA3, None, [])
        self.fixture(AURORA6, None, ["asahi,t6030-gpu", "asahi,t6030-dcp", "asahi,t6030-dcpext"])

    def fixture(self, name, data, strings=None):
        path = self.tmp / "pkgs" / name
        if data is not None:
            path.write_bytes(data)
        else:
            root = self.tmp / ("root-" + name)
            (root / "usr/lib/asahi-boot").mkdir(parents=True)
            (root / "usr/lib/asahi-boot/m1n1.bin").write_bytes(
                b"m1n1\0" + b"\0".join(s.encode() for s in strings) + b"\0end" + bytes(range(256)) * 4096)
            subprocess.run(["bsdtar", "--zstd", "-cf", str(path), "-C", str(root), "usr"], check=True)
        self.shas[name] = hashlib.sha256(path.read_bytes()).hexdigest()

    def mac(self, board, kernel="linux-asahi", m1n1="m1n1-stock", bootbin=b"M1N1:original\n",
            stub="14.8.3", iboot="iBoot-10151.140.19.700.2"):
        (self.tmp / "dt/compatible").write_bytes(b"".join(c.encode() + b"\0" for c in BOARDS[board]))
        (self.tmp / "dt/chosen/asahi,iboot2-version").write_bytes(iboot.encode() + b"\0")
        (self.tmp / "esp/asahi/stub_info.json").write_text(
            '{"system_version": {"ProductVersion": "%s"}}' % stub)
        (self.fake / "installed").write_text(kernel + "\n")
        (self.fake / "m1n1").write_text(m1n1 + "\n")
        (self.fake / "m1n1.bin").write_text("M1N1:" + m1n1 + "\n")
        (self.fake / "log").write_text("")
        self.boot.write_bytes(bootbin)

    def run_sh(self, body, check=True):
        pkgs = "\n".join(f'  "{n} {self.shas[n]}"' for n in OTHERS + [AURORA3])
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
sudo=""
DT='{self.tmp}/dt'
STATE='{self.state}'
M1N1_CONF='{self.m1n1_conf}'
UPDATE_M1N1_CONF='{self.update_conf}'
M3GPU_MARKER='{self.tmp}/etc/default/.update-m1n1.created-by-m3gpu'
MODPROBE_CONF='{self.tmp}/etc/aurora-sep.conf'
M1N1_BIN='{self.fake}/m1n1.bin'
PACKAGES=(
{pkgs}
)
M3_M1N1_PACKAGE="{AURORA6} {self.shas[AURORA6]}"
esp_bootbin() {{ echo '{self.boot}'; }}
version_notice() {{ :; }}; sep_write_notice() {{ :; }}; ane_dkms_notice() {{ :; }}
snapshot() {{ :; }}; add_pin() {{ :; }}; remove_pin() {{ :; }}
calibration() {{ :; }}; sep_policy() {{ :; }}; neo_radio_notice() {{ :; }}
boot_chain() {{ echo limine; }}
{body}
"""
        env = {**os.environ, "NO_COLOR": "1", "PATH": f"{self.tmp}/bin:{os.environ['PATH']}",
               "FAKE": str(self.fake), "FAKE_PKGS": str(self.tmp / "pkgs"),
               "FAKE_ESP": str(self.tmp / "esp"), "FAKE_UPDATE_CONF": str(self.update_conf),
               "FAKE_M1N1_CONF": str(self.m1n1_conf)}
        env.update(self.extra_env)
        proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env)
        if check:
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        return proc

    def install(self, try_=0, check=True, env=""):
        return self.run_sh(f"{env}\nM3_TRY={try_}\ninstall_all", check=check)

    def uninstall(self):
        return self.run_sh("uninstall_all")

    def log(self):
        return (self.fake / "log").read_text()

    def downloaded(self):
        return [l.rsplit("/", 1)[1] for l in self.log().splitlines() if l.startswith("curl ")]

    def kept_copy(self):
        return self.tmp / f"esp/m1n1/boot.bin.before-{VERSION}"

    # M3 Pro on the handoff path

    def test_fresh_m3_pro_gets_the_handoff(self):
        self.mac("j516s")
        out = self.install().stdout
        self.assertIn("built-in display at its native resolution", out)
        self.assertNotIn("run:  aurora-touchid-setup", out)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora6-1\n"), boot)
        self.assertTrue(boot.endswith(SWITCHES), boot)
        self.assertIn(AURORA6, self.downloaded())
        self.assertNotIn(AURORA3, self.downloaded())
        self.assertEqual(self.kept_copy().read_bytes(), b"M1N1:original\n")
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff")
        self.assertFalse(self.update_conf.read_text().count("M1N1_UPDATE_DISABLED"))

        self.uninstall()
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-stock\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertFalse(self.m1n1_conf.exists())
        self.assertFalse(self.state.exists())

    def test_m3_pro_over_an_11_36_kernel_only_install(self):
        self.mac("j516s", kernel="linux-aurora", bootbin=b"M1N1:original\n")
        self.update_conf.write_text(OUR_FREEZE)
        self.install()
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora6-1\n"), boot)
        self.assertTrue(boot.endswith(SWITCHES), boot)
        self.assertNotIn("M1N1_UPDATE_DISABLED", self.update_conf.read_text())

    def test_bringup_m3_pro_and_back(self):
        handbuilt = b"M1N1:bringup\nDTBS:hand\nUBOOT" + SWITCHES
        self.mac("j516s", kernel="linux-asahi", m1n1="m1n1-bringup", bootbin=handbuilt)
        self.update_conf.write_text(BRINGUP_FREEZE)
        marker = self.tmp / "etc/default/.update-m1n1.created-by-m3gpu"
        marker.touch()
        self.install()
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora6-1\n"), boot)
        self.assertTrue(boot.endswith(SWITCHES))
        # The hook ran while the bring-up freeze still held, so the first
        # rebuild is the installer's own, with the new m1n1 in place.
        log = self.log()
        self.assertLess(log.index("update-m1n1 frozen"), log.index("update-m1n1 rebuilt"))
        self.assertFalse(marker.exists())

        self.uninstall()
        self.assertEqual(self.boot.read_bytes(), handbuilt)
        self.assertEqual(self.update_conf.read_text(), BRINGUP_FREEZE)
        self.assertTrue(marker.exists())

    def test_trial_on_an_unlisted_m3_pro(self):
        self.mac("j514s")
        self.install(try_=1)
        self.assertTrue(self.boot.read_bytes().endswith(SWITCHES))
        self.assertTrue(self.kept_copy().exists())

    # M3s that stay kernel-only

    def assert_kernel_only(self, board, **kw):
        self.mac(board, **kw)
        before = self.boot.read_bytes()
        out = self.install().stdout
        self.assertIn("Touch ID is not supported", out)
        self.assertNotIn("run:  aurora-touchid-setup", out)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertIn("boot.bin is unchanged", out)
        self.assertFalse([d for d in self.downloaded() if d.startswith("m1n1-aurora-")])
        self.assertNotIn("update-m1n1 rebuilt", self.log())
        self.assertFalse(self.kept_copy().exists())
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "kernel")
        self.assertIn("M1N1_UPDATE_DISABLED=1", self.update_conf.read_text())

        self.uninstall()
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -Sy --noconfirm --ask 4 linux-asahi linux-asahi-headers libfprint m1n1",
                         self.log())
        self.assertFalse(self.update_conf.exists())

    def test_unlisted_m3_pro_is_kernel_only(self):
        self.assert_kernel_only("j514s")

    def test_m3_is_kernel_only(self):
        self.assert_kernel_only("j613")

    def test_listed_m3_pro_on_another_stub_is_kernel_only(self):
        self.assert_kernel_only("j516s", stub="15.6")

    def test_trial_refused_on_an_m3(self):
        self.mac("j613")
        before = self.boot.read_bytes()
        proc = self.install(try_=1, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertEqual(self.boot.read_bytes(), before)
        self.assertNotIn("pacman -U", self.log())

    # Later runs and failures (review of 5c7daa75)

    def test_rerun_without_the_flag_keeps_a_tried_handoff(self):
        self.mac("j514s")
        self.install(try_=1)
        out = self.install().stdout
        self.assertIn("keeping it", out)
        self.assertEqual((self.state / "m3-mode").read_text().strip(), "handoff")
        self.assertTrue(self.boot.read_bytes().endswith(SWITCHES))
        self.assertNotIn("M1N1_UPDATE_DISABLED", self.update_conf.read_text())
        self.uninstall()
        self.assertTrue(self.boot.read_bytes().startswith(b"M1N1:m1n1-stock\n"))
        self.assertFalse(self.m1n1_conf.exists())

    def test_failed_install_then_uninstall_keeps_the_bringup(self):
        handbuilt = b"M1N1:bringup\nDTBS:hand\nUBOOT" + SWITCHES
        self.mac("j516s", m1n1="m1n1-bringup", bootbin=handbuilt)
        self.update_conf.write_text(BRINGUP_FREEZE)
        marker = self.tmp / "etc/default/.update-m1n1.created-by-m3gpu"
        marker.touch()
        self.extra_env = {"FAKE_FAIL_U": "1"}
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.extra_env = {}
        self.uninstall()
        self.assertEqual(self.boot.read_bytes(), handbuilt)
        self.assertEqual(self.update_conf.read_text(), BRINGUP_FREEZE)
        self.assertTrue(marker.exists())

    def test_failed_install_then_uninstall_keeps_omarchys_config(self):
        own = "export LC_ALL=C\n"
        self.mac("j516s")
        self.update_conf.write_text(own)
        self.extra_env = {"FAKE_FAIL_U": "1"}
        self.assertNotEqual(self.install(check=False).returncode, 0)
        self.extra_env = {}
        self.uninstall()
        self.assertEqual(self.update_conf.read_text(), own)

    def test_uninstall_after_an_11_36_kernel_only_install_and_update(self):
        # 11.36 recorded linux-asahi, then rewrote it to linux-aurora on a re-run.
        self.mac("j516s", kernel="linux-aurora")
        self.update_conf.write_text(OUR_FREEZE)
        (self.state / "previous-package").write_text("linux-aurora 7.1.12.aurora2-11.36-1\n")
        self.install()
        self.uninstall()
        self.assertIn("linux-asahi linux-asahi-headers libfprint m1n1", self.log())
        self.assertNotIn("-S --noconfirm --ask 4 linux-aurora", self.log())

    def test_previous_package_is_kept_across_updates(self):
        self.mac("j314s")
        self.install()
        self.install()
        self.assertTrue((self.state / "previous-package").read_text().startswith("linux-asahi"))

    def test_handoff_refuses_a_boot_bin_that_was_not_rebuilt(self):
        # A freeze update-m1n1 honours but the pattern check would miss, on a
        # hand-built boot.bin that already ends with the switches.
        handbuilt = b"M1N1:bringup\nDTBS:hand\nUBOOT" + SWITCHES
        self.mac("j516s", m1n1="m1n1-bringup", bootbin=handbuilt)
        self.update_conf.write_text(BRINGUP_FREEZE + ": ${M1N1_UPDATE_DISABLED:=1}\n")
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(self.boot.read_bytes(), handbuilt)

    def test_handoff_refuses_a_custom_m1n1_path(self):
        self.mac("j516s")
        self.update_conf.write_text("M1N1=/opt/my-m1n1.bin\n")
        proc = self.install(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Nothing was installed", proc.stderr)
        self.assertNotIn("pacman -U", self.log())

    def test_empty_mode_file_does_not_break_uninstall(self):
        self.mac("j613")
        self.install()
        (self.state / "m3-mode").write_text("")
        self.uninstall()

    # M1/M2 are unchanged: aurora3, no switches, no M3 state

    def test_m1_pro_unchanged(self):
        self.mac("j314s")
        out = self.install().stdout
        self.assertIn("run:  aurora-touchid-setup", out)
        boot = self.boot.read_bytes()
        self.assertTrue(boot.startswith(b"M1N1:m1n1-aurora-1.6.1.aurora3-1\n"), boot)
        self.assertNotIn(b"chosen.", boot)
        self.assertIn(AURORA3, self.downloaded())
        self.assertNotIn(AURORA6, self.downloaded())
        self.assertFalse((self.state / "m3-mode").exists())
        self.assertFalse(self.m1n1_conf.exists())


if __name__ == "__main__":
    unittest.main()
