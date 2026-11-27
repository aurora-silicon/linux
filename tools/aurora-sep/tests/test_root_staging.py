# SPDX-License-Identifier: GPL-2.0-only
"""install-aurora-sep.sh: what root installs or runs comes from a directory root owns, checked
there as root, and pacman gets the checked archives by name (no glob over the directory). The
same for the M3 Max kit's root run of this script."""
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parents[1] / 'install-aurora-sep.sh'


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Staging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.log = self.root / 'log'
        # Stands in for sudo: records the call and runs it (as this user).
        (self.bin / 'fakesudo').write_text(f'#!/bin/sh\necho "sudo $*" >>"{self.log}"\nexec "$@"\n')
        (self.bin / 'pacman').write_text(f'#!/bin/sh\necho "pacman $*" >>"{self.log}"\n')
        for f in self.bin.iterdir():
            f.chmod(0o755)
        self.tmpdir = self.root / 'tmp'
        self.tmpdir.mkdir()

    def sh(self, body):
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
export TMPDIR='{self.tmpdir}'
die() {{ echo "DIED: $*"; exit 9; }}
{body}
"""
        env = {**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'}
        return subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=env)

    def downloads(self, **files):
        d = Path(tempfile.mkdtemp(dir=self.tmpdir))
        for name, data in files.items():
            (d / name).write_bytes(data)
        return d

    def stage(self, d, entries, then=''):
        lines = '\n'.join(entries)
        return self.sh(f"""
sudo=fakesudo
work='{d}'
candidate_archives=('{d}/a.pkg.tar.zst')
work_to_root <<'EOF'
{lines}
EOF
echo "work=$work"
echo "cand=${{candidate_archives[*]}}"
{then}
""")

    def test_copies_and_checks_into_a_new_directory(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'A', 'tool.sh': b'T'})
        r = self.stage(d, [f'a.pkg.tar.zst {sha(b"A")}', f'tool.sh {sha(b"T")}'])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        work = Path(re.search(r'^work=(.*)$', r.stdout, re.M).group(1))
        self.assertNotEqual(work, d)
        self.assertFalse(d.exists())                         # the user's copy is gone
        self.assertEqual((work / 'a.pkg.tar.zst').read_bytes(), b'A')
        self.assertEqual((work / 'tool.sh').read_bytes(), b'T')
        self.assertEqual(stat.S_IMODE(work.stat().st_mode), 0o755)
        self.assertIn(f'cand={work}/a.pkg.tar.zst', r.stdout)
        self.assertIn('sha256sum', self.log.read_text())     # checked by the root side

    def test_a_file_changed_after_the_check_is_refused(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'changed'})
        r = self.stage(d, [f'a.pkg.tar.zst {sha(b"A")}'])
        self.assertEqual(r.returncode, 9)
        self.assertIn('changed after its checksum was checked', r.stdout)

    def test_a_link_planted_in_place_of_a_download_is_refused(self):
        target = self.root / 'secret'
        target.write_bytes(b'S')
        d = self.downloads()
        (d / 'a.pkg.tar.zst').symlink_to(target)
        r = self.stage(d, [f'a.pkg.tar.zst {sha(b"A")}'])
        self.assertEqual(r.returncode, 9)

    def test_a_failed_copy_leaves_no_directory_behind(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'A', 'b.pkg.tar.zst': b'changed'})
        r = self.sh(f"""
sudo=fakesudo
work='{d}'
FROZEN_PACKAGES=0 NEO_GPU_PREPARED=0 M3_PERSISTENT_TRANSACTION_ACTIVE=0
candidate_archives=()
trap 'm3_install_cleanup' EXIT
work_to_root <<'LIST'
a.pkg.tar.zst {sha(b"A")}
b.pkg.tar.zst {sha(b"B")}
LIST
""")
        self.assertEqual(r.returncode, 9, r.stdout + r.stderr)
        self.assertEqual(sorted(self.tmpdir.iterdir()), [])

    def test_root_files_stay_readable_under_a_private_umask(self):
        # sudo keeps the invoking user's umask. What root writes into its work directory is
        # read back by the user's side, so its mode must not depend on that umask.
        d = self.downloads(**{'a.pkg.tar.zst': b'A'})
        state = self.root / 'state'
        r = self.sh(f"""
umask 077
sudo=fakesudo
work='{d}'
candidate_archives=()
work_to_root <<'LIST'
a.pkg.tar.zst {sha(b"A")}
LIST
echo text | work_write "$work/dependency-result.json"
m3_gpu_check_plan() {{ :; }}
STATE='{state}' M3_GPU_CHECK='{self.root}/sbin/aurora-m3-gpu-check'
m3_gpu_check_install
stat -c 'mode %a %n' "$work" "$work"/*
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        modes = {name: mode for mode, name in re.findall(r'^mode (\d+) \S*/([^/\n]+)$', r.stdout, re.M)}
        self.assertEqual(modes.pop('a.pkg.tar.zst'), '644')
        self.assertEqual(modes.pop('dependency-result.json'), '644')
        self.assertEqual(modes.pop('aurora-m3-gpu-check'), '644')
        self.assertEqual(list(modes.values()), ['755'], r.stdout)
        self.assertEqual((self.root / 'sbin/aurora-m3-gpu-check').stat().st_mode & 0o777, 0o755)

    def test_root_writes_into_its_work_directory_set_the_mode(self):
        text = INSTALLER.read_text()
        self.assertEqual(re.findall(r'^[^#\n]*\btee "\$work/[^\n]*', text, re.M), [])
        for name in ('migrate-pro-session.py', 'frozen-dependencies.py', 'dependency-result.json',
                     'dependency-files', 'aurora-m3-gpu-check'):
            self.assertRegex(text, rf'work_write "\$work/{re.escape(name)}"')

    def test_odd_names_are_refused(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'A'})
        for name in ('../a.pkg.tar.zst', '.hidden', 'x/y'):
            with self.subTest(name):
                r = self.stage(d, [f'{name} {sha(b"A")}'])
                self.assertEqual(r.returncode, 9, r.stdout)

    def test_without_sudo_nothing_moves(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'A'})
        r = self.sh(f"sudo=''\nwork='{d}'\ncandidate_archives=()\nwork_to_root </dev/null\necho \"work=$work\"")
        self.assertIn(f'work={d}', r.stdout)

    def test_pacman_gets_the_checked_archives_by_name(self):
        d = self.downloads(**{'a.pkg.tar.zst': b'A', 'b.pkg.tar.zst': b'B', 'extra.pkg.tar.zst': b'X'})
        (d / 'm3-pro').mkdir()
        r = self.sh(f"""
sudo=fakesudo
work='{d}'
FROZEN_PACKAGES=0 M3_GPU_PERSISTENT=0 M3_PRO_PERF_SELECTED=0 M3_PRO_MESA_PACKAGE=''
INSTALL_ARCHIVES=('{d}/a.pkg.tar.zst' '{d}/b.pkg.tar.zst' '{d}/gone.pkg.tar.zst' '{d}/notes.txt')
chain=grub
m3_install_packages
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        calls = [l for l in self.log.read_text().splitlines() if l.startswith('pacman ')]
        self.assertEqual(calls, [f'pacman -U --noconfirm --ask 4 {d}/a.pkg.tar.zst {d}/b.pkg.tar.zst'])

    def test_no_root_step_reads_a_file_the_user_wrote_after_staging(self):
        text = INSTALLER.read_text()
        self.assertIsNone(re.search(r'^[^#\n]*\$sudo pacman -U[^\n]*"\$work"/\*', text, re.M))
        def body(name):
            return text.split(f'\n{name}() {{', 1)[1].split('\n}\n', 1)[0]
        # Every write into $work after the download loop goes through root.
        for name in ('install_all', 'm3_pro_mesa_set_aside', 'frozen_dependency_prepare',
                     'm3_pro_perf_activate', 'm3_gpu_check_install', 'm3_install_packages'):
            for line in body(name).splitlines():
                if line.lstrip().startswith('#'):
                    continue
                self.assertIsNone(re.search(r'(?<![<>&|])>\s*"\$work/', line), f'{name}: {line}')
                self.assertIsNone(re.search(r'^\s*(mkdir|mv|cp|cat)\b[^|]*"?\$work/', line), f'{name}: {line}')
        install_all = body('install_all')
        self.assertIn('work_to_root < <(', install_all)
        self.assertLess(install_all.index('work_to_root'), install_all.index('frozen_dependency_prepare'))
        self.assertLess(install_all.index('work_to_root'), install_all.index('m3_install_packages'))


class PythonIsolation(unittest.TestCase):
    def test_every_root_python_is_isolated(self):
        text = INSTALLER.read_text()
        calls = re.findall(r'\$sudo python3\b[^\n]*', text)
        self.assertGreater(len(calls), 10)
        for call in calls:
            self.assertTrue(call.startswith('$sudo python3 -I '), call)
        self.assertIn('"Exec = /usr/bin/python3 -I "', text)
        self.assertIn('python3 -I - "$1" "$d/install-aurora-sep.sh"', text)


class KitRootCopy(unittest.TestCase):
    """--m3max-kit under sudo: root runs its own checked copy, from its own directory."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        (self.bin / 'fakesudo').write_text('#!/bin/sh\nwhile [ "${1#--}" != "$1" ]; do shift; done\nexec "$@"\n')
        (self.bin / 'fakesudo').chmod(0o755)
        self.tmpdir = self.root / 'tmp'
        self.tmpdir.mkdir()
        # The installer's own identity, and a stand-in copy that reports it when sourced
        # (as the root side checks it) and says how it was run otherwise.
        ident = subprocess.run(['bash', '-c', f"AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}' && m3max_kit_identity"],
                               capture_output=True, text=True, check=True).stdout
        (self.root / 'identity').write_text(ident)
        (self.root / 'other-identity').write_text('another release|')
        self.copy = self.root / 'copy.sh'
        self.copy.write_text(self.stand_in('identity'))

    def stand_in(self, ident):
        return (f"#!/bin/bash\nm3max_kit_identity() {{ cat '{self.root}/{ident}'; }}\n"
                "if [[ ${AURORA_SEP_SOURCE_ONLY:-} == 1 ]]; then return 0; fi\n"
                'echo "ran $0 $*"\necho "mode $(stat -c %a "$0")"\n')

    def setup(self, tamper='', before=''):
        script = f"""
set -euo pipefail
{before}
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
export TMPDIR='{self.tmpdir}'
die() {{ echo "DIED: $*"; exit 9; }}
sudo=fakesudo
release_source() {{ :; }}
require_supported_soc() {{ :; }}
is_m3_max() {{ true; }}
m3max_kit_self() {{
  cp '{self.copy}' "$M3MAX_KIT_WORK/install-aurora-sep.sh"
  M3MAX_KIT_SELF_SHA=$(sha256sum "$M3MAX_KIT_WORK/install-aurora-sep.sh" | cut -d' ' -f1)
  echo "user-work=$M3MAX_KIT_WORK"
  {tamper}
}}
m3max_kit_setup --yes
"""
        env = {**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'}
        return subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=env)

    def test_root_runs_its_own_copy(self):
        r = self.setup()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        user = re.search(r'^user-work=(.*)$', r.stdout, re.M).group(1)
        m = re.search(r'^ran (\S+)/install-aurora-sep\.sh --m3max-kit --kit-work (\S+) --yes$', r.stdout, re.M)
        self.assertIsNotNone(m, r.stdout)
        self.assertEqual(m.group(1), m.group(2))
        self.assertNotEqual(m.group(1), user)
        self.assertFalse(Path(user).exists())

    def test_a_changed_copy_is_not_run(self):
        r = self.setup(tamper='echo "echo evil" >>"$M3MAX_KIT_WORK/install-aurora-sep.sh"')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('ran ', r.stdout)
        self.assertIn('could not take the kit script', r.stderr)

    def test_root_checks_the_identity_of_its_own_copy(self):
        # The user-side checks saw a script that passed; the file root copies (its sha256 taken
        # on that same content) is another script. Root's own identity check refuses it.
        self.copy.write_text(self.stand_in('other-identity'))
        r = self.setup()
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('ran ', r.stdout)
        self.assertIn('is not the script that runs now', r.stderr)

    def test_the_identity_check_of_roots_copy_runs_as_the_invoking_user(self):
        # The check sources the script. With root's id differing from the invoking user's, it
        # is run through runuser for that user, reading root's copy from its standard input.
        log = self.root / 'runuser.log'
        (self.bin / 'id').write_text('#!/bin/sh\nif [ "$*" = "-u" ]; then echo 0; else exec /usr/bin/id "$@"; fi\n')
        (self.bin / 'runuser').write_text(
            f'#!/bin/sh\necho "$*" >>"{log}"\nreadlink /proc/self/fd/0 >>"{log}"\nshift 3\nexec "$@"\n')
        for name in ('id', 'runuser'):
            (self.bin / name).chmod(0o755)
        r = self.setup()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        m = re.search(r'^ran (\S+)/install-aurora-sep\.sh ', r.stdout, re.M)
        self.assertIsNotNone(m, r.stdout)
        call, stdin = log.read_text().splitlines()
        user = subprocess.run(['/usr/bin/id', '-un'], capture_output=True, text=True).stdout.strip()
        self.assertTrue(call.startswith(f'-u {user} -- env AURORA_SEP_SOURCE_ONLY=1 bash -c source /dev/stdin '), call)
        self.assertEqual(stdin, f'{m.group(1)}/install-aurora-sep.sh')

    def test_the_installer_reports_its_identity_from_standard_input(self):
        with open(INSTALLER) as f:
            r = subprocess.run(['bash', '-c', 'AURORA_SEP_SOURCE_ONLY=1 source /dev/stdin >/dev/null 2>&1 && m3max_kit_identity'],
                               stdin=f, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, (self.root / 'identity').read_text())
        self.assertGreater(r.stdout.count('|'), 8)

    def test_roots_copy_is_readable_under_a_private_umask(self):
        r = self.setup(before='umask 077')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('\nmode 755\n', r.stdout)

    def test_a_link_is_not_followed(self):
        r = self.setup(tamper=f'cp "$M3MAX_KIT_WORK/install-aurora-sep.sh" "{self.root}/real"; '
                              f'ln -sf "{self.root}/real" "$M3MAX_KIT_WORK/install-aurora-sep.sh"')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('ran ', r.stdout)

    def test_root_refuses_a_work_directory_it_does_not_own(self):
        d = Path(tempfile.mkdtemp(dir=self.tmpdir))
        os.chmod(d, 0o777)
        script = f"""
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
die() {{ echo "DIED: $*"; exit 9; }}
M3MAX_KIT_WORK='{d}'
m3max_kit_work_is_root
echo ok
"""
        r = subprocess.run(['bash', '-c', script], capture_output=True, text=True)
        self.assertIn("is not root's own", r.stdout)
        os.chmod(d, 0o700)
        r = subprocess.run(['bash', '-c', script], capture_output=True, text=True)
        self.assertIn('ok', r.stdout)


class RootConfiguration(unittest.TestCase):
    """What root installs as update-m1n1's or m1n1's configuration is passed to it by the shell
    that made it, and does not sit in a temporary file of the invoking user's on the way."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.log = self.root / 'log'
        (self.bin / 'fakesudo').write_text(f'#!/bin/sh\necho "sudo $*" >>"{self.log}"\nexec "$@"\n')
        # An install that finds its source changed since the shell wrote it: any file under the
        # temporary directory that root is asked to install by name has other content by then.
        (self.bin / 'install').write_text(
            '#!/bin/bash\nfor a in "$@"; do\n'
            f'  if [[ -f $a && $a == "{self.root}/tmp/"* ]]; then echo CHANGED=1 >"$a"; fi\n'
            'done\nexec /usr/bin/install "$@"\n')
        for f in self.bin.iterdir():
            f.chmod(0o755)
        (self.root / 'tmp').mkdir()
        self.etc = self.root / 'etc'
        self.etc.mkdir()
        self.state = self.root / 'state'
        self.state.mkdir()
        self.conf = self.etc / 'update-m1n1'
        self.m1n1 = self.etc / 'm1n1.conf'

    def sh(self, body):
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
export TMPDIR='{self.root}/tmp'
die() {{ echo "DIED: $*"; exit 9; }}
umask 077
sudo=fakesudo
UPDATE_M1N1_CONF='{self.conf}' M1N1_CONF='{self.m1n1}' STATE='{self.state}'
{body}
"""
        env = {**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'}
        r = subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # Nothing of the user's is left in the temporary directory, and none in /etc.
        self.assertEqual(list((self.root / 'tmp').iterdir()), [])
        self.assertEqual(sorted(p.name for p in self.etc.iterdir() if '.' in p.name and p.name != 'm1n1.conf'), [])
        return r

    def mode(self, path):
        return stat.S_IMODE(path.stat().st_mode)

    def test_no_user_temporary_file_is_installed_as_root(self):
        text = INSTALLER.read_text()
        self.assertEqual(re.findall(r'^[^#\n]*\$sudo install\b[^\n]*"\$tmp"[^\n]*', text, re.M), [])
        for dest in ('"$UPDATE_M1N1_CONF"', '"$M1N1_CONF"', '"$conf"', '"$STATE/update-m1n1.default.saved"'):
            self.assertIn(f'| root_put {dest}', text)

    def test_freeze_and_unfreeze(self):
        self.conf.write_text('KEEP=1\n\n')
        self.sh('m3_freeze; echo "by=$M3_FROZEN_BY"')
        text = self.conf.read_text()
        self.assertTrue(text.startswith('KEEP=1\n\n# >>> aurora-sep: keep this M3'), text)
        self.assertTrue(text.endswith('\nM1N1_UPDATE_DISABLED=1\n# <<< aurora-sep: keep this M3\'s boot.bin as it is\n'), text)
        self.assertNotIn('CHANGED', text)
        self.assertEqual(self.mode(self.conf), 0o644)
        self.sh('m3_unfreeze')
        self.assertEqual(self.conf.read_text(), 'KEEP=1\n\n')
        self.assertEqual(self.mode(self.conf), 0o644)

    def test_unfreeze_removes_a_file_that_held_only_the_freeze(self):
        self.sh('m3_freeze')
        self.assertIn('M1N1_UPDATE_DISABLED=1', self.conf.read_text())
        self.sh('m3_unfreeze')
        self.assertFalse(self.conf.exists())

    def test_switches_write_and_remove(self):
        self.m1n1.write_text('other=1\n')
        stubs = 'is_m3_air() { false; }; is_m3_max() { false; }; m3_switches() { echo chosen.a=1 chosen.b=0; }\n'
        self.sh(stubs + 'm3_switches_write')
        lines = self.m1n1.read_text().splitlines()
        self.assertEqual(lines[0], 'other=1')
        self.assertEqual(lines[2:4], ['chosen.a=1', 'chosen.b=0'])
        self.assertEqual(len(lines), 5)
        self.assertEqual(self.mode(self.m1n1), 0o644)
        self.sh(stubs + 'm3_switches_remove')
        self.assertEqual(self.m1n1.read_text(), 'other=1\n')

    def test_boot_state_snapshot_and_neo_block(self):
        self.conf.write_text('M1N1=/x/m1n1.bin\n')
        self.sh('m3_freeze; snapshot_boot_state || true')
        self.assertEqual((self.state / 'update-m1n1.default.saved').read_text(), 'M1N1=/x/m1n1.bin\n')
        self.assertEqual(self.mode(self.state / 'update-m1n1.default.saved'), 0o644)
        self.sh('m3_unfreeze; neo_gpu_boot_config; neo_gpu_boot_config')
        text = self.conf.read_text()
        self.assertTrue(text.startswith('M1N1=/x/m1n1.bin\n# >>> aurora-sep: Neo stage2\n'), text)
        self.assertEqual(text.count('# >>> aurora-sep: Neo stage2'), 1)
        self.assertTrue(text.endswith('# <<< aurora-sep: Neo stage2\n'), text)
        self.assertNotIn('CHANGED', text)

    def test_a_failed_write_keeps_the_old_file(self):
        self.conf.write_text('KEEP=1\n')
        (self.bin / 'mv').write_text('#!/bin/sh\nexit 1\n')
        (self.bin / 'mv').chmod(0o755)
        script = f"""
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
sudo=''
echo new | root_put '{self.conf}' && echo put-ok
"""
        env = {**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'}
        r = subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=env)
        self.assertNotIn('put-ok', r.stdout)
        self.assertEqual(self.conf.read_text(), 'KEEP=1\n')
        self.assertEqual([p.name for p in self.etc.iterdir()], ['update-m1n1'])


class AdtReaderRootCopy(unittest.TestCase):
    """The report's ADT reader: under sudo root runs its own copy, checked as root."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        (self.bin / 'fakesudo').write_text('#!/bin/sh\nexec "$@"\n')
        (self.bin / 'fakesudo').chmod(0o755)
        (self.root / 'tmp').mkdir()
        self.work = self.root / 'work'
        self.work.mkdir()
        self.reader = self.work / 'aurora-adt-extract.py'
        self.reader.write_bytes(b'print("reader")\n')

    def sh(self, body, sudo='fakesudo', pin=None):
        pin = sha(b'print("reader")\n') if pin is None else pin
        script = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{INSTALLER}'
export TMPDIR='{self.root}/tmp'
umask 077
sudo={sudo!r}
M3_ADT_READER={('aurora-adt-extract.py ' + pin) if pin else ''!r}
M3_WORK='{self.work}'
{body}
"""
        env = {**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'}
        return subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=env)

    def test_root_runs_its_own_checked_copy(self):
        r = self.sh(f"""
m3_adt_reader_root '{self.reader}'
echo "run=$M3_ADT_RUN"
echo CHANGED >'{self.reader}'
python3 -I "$M3_ADT_RUN"
stat -c 'mode %a' "$M3_ADT_RUN"
m3_adt_reader_root_done
echo "root=[$M3_ADT_ROOT]"
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        run = Path(re.search(r'^run=(.*)$', r.stdout, re.M).group(1))
        self.assertEqual(run.name, 'aurora-adt-extract.py')
        self.assertNotEqual(run.parent, self.work)
        self.assertIn('\nreader\nmode 644\nroot=[]\n', r.stdout)
        self.assertFalse(run.parent.exists())

    def test_a_copy_that_differs_from_the_pinned_reader_is_refused(self):
        r = self.sh(f"""
echo 'print("other")' >'{self.reader}'
if m3_adt_reader_root '{self.reader}'; then echo accepted; else echo "why=$M3_ADT_WHY"; fi
m3_report_cleanup
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('accepted', r.stdout)
        self.assertIn('why=aurora-adt-extract.py changed after its checksum was checked, so the ADT was not read', r.stdout)
        self.assertEqual(list((self.root / 'tmp').iterdir()), [])

    def test_without_sudo_the_reader_runs_where_it_is(self):
        r = self.sh(f"m3_adt_reader_root '{self.reader}'; echo \"run=$M3_ADT_RUN root=[$M3_ADT_ROOT]\"", sudo='')
        self.assertIn(f'run={self.reader} root=[]', r.stdout)

    def test_the_report_runs_only_roots_copy(self):
        text = INSTALLER.read_text()
        for name in ('m3_report_adt', 'm3_report_stage2_log'):
            body = text.split(f'\n{name}() {{', 1)[1].split('\n}\n', 1)[0]
            runs = re.findall(r'\$sudo python3 -I "\$(\w+)"', body)
            self.assertGreaterEqual(len(runs), 2, name)
        adt = text.split('\nm3_report_adt() {', 1)[1].split('\n}\n', 1)[0]
        self.assertEqual(set(re.findall(r'\$sudo python3 -I "\$(\w+)"', adt)), {'M3_ADT_RUN'})
        self.assertIn('m3_report_stage2_log "$1" "$M3_ADT_RUN" "$log"', adt)
        self.assertLess(adt.index('m3_adt_reader_root "$reader"'), adt.index('$sudo python3 -I'))


if __name__ == '__main__':
    unittest.main()
