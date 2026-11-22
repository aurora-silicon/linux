"""The boot-partition tools' compatibility lock in /tmp: taken only when it is already there as a
regular file of the tool's own user (root), never created and never followed through a link.
The /run/lock lock is taken as before."""
from pathlib import Path
import fcntl
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
ONESHOT = TOOLS / 'air-gpu/air-gpu-oneshot.sh'
INSTALLER = TOOLS / 'install-aurora-sep.sh'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ESP = load('esp_history_locks', TOOLS / 'esp-history.py')
M3 = load('m3_boot_profile_locks', TOOLS / 'm3-boot-profile.py')


def held_by_someone(path):
    """True when another open file description holds an exclusive flock on path."""
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    finally:
        os.close(fd)
    return False


class PythonLocks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.boot = self.root / 'run/boot-partition.lock'
        self.compat = self.root / 'tmp/limine-global.lock'
        self.compat.parent.mkdir()

    def lockers(self):
        return (('esp-history', lambda: ESP.lock([self.boot], [self.compat])),
                ('m3-boot-profile', lambda: M3.locks([self.boot], 0.2, [self.compat])))

    def test_defaults_name_the_compat_lock_separately(self):
        self.assertEqual(ESP.BOOT_LOCK, Path('/run/lock/boot-partition.lock'))
        self.assertEqual(ESP.COMPAT_LOCK, Path('/tmp/limine-global.lock'))
        text = (TOOLS / 'm3-boot-profile.py').read_text()
        self.assertIn("args.lock = [Path('/run/lock/boot-partition.lock')]", text)
        self.assertIn("args.compat_lock = args.compat_lock or [Path('/tmp/limine-global.lock')]", text)

    def test_missing_compat_lock_is_not_created(self):
        for name, locker in self.lockers():
            with self.subTest(name):
                with locker():
                    self.assertTrue(held_by_someone(self.boot))
                self.assertFalse(self.compat.exists())

    def test_symlinked_compat_lock_is_not_followed(self):
        target = self.root / 'victim'
        self.compat.symlink_to(target)
        for name, locker in self.lockers():
            with self.subTest(name):
                with locker():
                    pass
                self.assertFalse(target.exists())
        target.write_text('x')
        for name, locker in self.lockers():
            with self.subTest(name + ' existing target'):
                with locker():
                    self.assertFalse(held_by_someone(target))

    def test_compat_lock_of_another_user_is_ignored_even_when_held(self):
        self.compat.touch()
        holder = os.open(self.compat, os.O_RDONLY)
        self.addCleanup(os.close, holder)
        fcntl.flock(holder, fcntl.LOCK_EX)
        # The file belongs to someone else when the tool's own uid is another one.
        for module in (ESP, M3):
            with self.subTest(module.__name__), patch.object(module.os, 'geteuid',
                                                             return_value=os.geteuid() + 1):
                ctx = ESP.lock([self.boot], [self.compat]) if module is ESP else \
                    M3.locks([self.boot], 0.2, [self.compat])
                with ctx:
                    self.assertTrue(held_by_someone(self.boot))

    def test_own_compat_lock_is_taken_and_a_holder_still_blocks(self):
        self.compat.touch()
        os.chmod(self.compat, 0o600)
        for name, locker in self.lockers():
            with self.subTest(name):
                with locker():
                    self.assertTrue(held_by_someone(self.compat))
                self.assertFalse(held_by_someone(self.compat))
        holder = os.open(self.compat, os.O_RDONLY)
        self.addCleanup(os.close, holder)
        fcntl.flock(holder, fcntl.LOCK_EX)
        with self.assertRaises(BlockingIOError):
            with ESP.lock([self.boot], [self.compat]):
                pass
        with self.assertRaises(TimeoutError):
            with M3.locks([self.boot], 0.2, [self.compat]):
                pass

    def test_a_fifo_is_not_a_lock(self):
        os.mkfifo(self.compat)
        for name, locker in self.lockers():
            with self.subTest(name):
                with locker():
                    pass

    def test_installer_copies_match(self):
        installer = INSTALLER.read_text()
        for tag, path in (('ESP_HISTORY_PY', 'esp-history.py'), ('M3_BOOT_PROFILE_PY', 'm3-boot-profile.py')):
            embedded = installer.split(f"<<'{tag}'\n", 1)[1].split(f'\n{tag}\n', 1)[0] + '\n'
            self.assertEqual(embedded, (TOOLS / path).read_text(), path)
        self.assertIn('M3MAX_KIT_LOCKS="/run/lock/boot-partition.lock"\n', installer)
        self.assertIn('M3MAX_KIT_COMPAT_LOCKS="/tmp/limine-global.lock"\n', installer)
        self.assertIn('  COMPAT_LOCKS=$M3MAX_KIT_COMPAT_LOCKS\n', installer)


@unittest.skipUnless(shutil.which('flock') and shutil.which('stat'), 'flock and stat are needed')
class ShellLocks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.boot = self.root / 'run/boot-partition.lock'
        self.compat = self.root / 'tmp/limine-global.lock'
        self.compat.parent.mkdir()

    def take(self, then='true'):
        """Source the one-shot script, take its locks, then run `then` while they are held."""
        script = f"""
set -euo pipefail
AIR_GPU_SOURCE_ONLY=1 source '{ONESHOT}'
LOCKS='{self.boot}'
COMPAT_LOCKS='{self.compat}'
refuse() {{ echo "REFUSED: $*"; exit 7; }}
take_locks
echo "held ${{#LOCK_FDS[@]}}"
{then}
"""
        return subprocess.run(['bash', '-c', script], capture_output=True, text=True)

    def probe(self, path):
        return f"flock -n '{path}' true && echo free || echo busy"

    def test_defaults(self):
        text = ONESHOT.read_text()
        self.assertIn('\nLOCKS="/run/lock/boot-partition.lock"\n', text)
        self.assertIn('\nCOMPAT_LOCKS="/tmp/limine-global.lock"\n', text)

    def test_missing_compat_lock_is_not_created(self):
        r = self.take()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('held 1', r.stdout)
        self.assertFalse(self.compat.exists())

    def test_symlinked_compat_lock_is_not_used(self):
        target = self.root / 'victim'
        target.write_text('x')
        self.compat.symlink_to(target)
        r = self.take(self.probe(target))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('held 1', r.stdout)
        self.assertIn('free', r.stdout)
        self.compat.unlink()
        self.compat.symlink_to(self.root / 'missing')
        r = self.take()
        self.assertIn('held 1', r.stdout)
        self.assertFalse((self.root / 'missing').exists())

    def test_own_regular_compat_lock_is_held(self):
        self.compat.write_text('')
        r = self.take(self.probe(self.compat))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('held 2', r.stdout)
        self.assertIn('busy', r.stdout)

    def test_a_fifo_is_not_a_lock(self):
        os.mkfifo(self.compat)
        r = self.take()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('held 1', r.stdout)


if __name__ == '__main__':
    unittest.main()
