"""air-gpu-collect.sh hands its tgz to the user privately: the file is created by the user's own
process with mode 0600, and no copy is left in the temp directory (or anywhere world-readable)
when the user's home cannot be written."""
import os
from pathlib import Path
import stat
import unittest

import test_air_gpu as A
from test_air_gpu import ARMED, COLLECT, RUNNING, k1


class CollectHandOverTest(unittest.TestCase):
    def setUp(self):
        # CollectRunTest's fake Mac, journal and home, without its tests.
        rig = A.CollectRunTest("test_this_boot")
        rig.setUp()
        self.addCleanup(rig.doCleanups)
        self.mac, self.home, self.statedir = rig.mac, rig.home, rig.statedir
        self.rig = rig
        self.tmpdir = self.mac.tmp / "tmpdir"
        self.tmpdir.mkdir()
        self.mac.cmdline.write_text(ARMED + "\n")
        rig.journal(0, ARMED, RUNNING + k1("job-completed", ": the first compute job finished"))

    def collect(self):
        t = self.mac.tmp
        body = f"""
export FAKE='{self.rig.fake}'
export TMPDIR='{self.tmpdir}'
DEBUGFS='{t}/debug'
SYS='{t}/sys'
BOOT_ID='{self.rig.boot_id}'
STATE_DIR='{self.statedir}'
EXTRA_HOMES=''
user_home() {{ echo '{self.home}'; }}
main
"""
        return self.mac.run(COLLECT, body, check=True)

    def leftovers(self):
        return sorted(p.name for p in self.tmpdir.iterdir())

    def test_the_tgz_is_private_and_nothing_is_left_behind(self):
        old = os.umask(0o022)
        try:
            self.collect()
        finally:
            os.umask(old)
        tgz = list(self.home.glob("air-gpu-collect-*.tgz"))
        self.assertEqual(len(tgz), 1)
        mode = stat.S_IMODE(tgz[0].stat().st_mode)
        self.assertEqual(mode & 0o077, 0, oct(mode))
        self.assertEqual(tgz[0].stat().st_uid, os.getuid())
        self.assertEqual(self.leftovers(), [])

    def test_an_unwritable_home_keeps_a_root_only_copy(self):
        os.chmod(self.home, 0o500)
        self.addCleanup(os.chmod, self.home, 0o700)
        if os.access(self.home, os.W_OK):
            self.skipTest("running with DAC override (root)")
        proc = self.collect()
        self.assertEqual(list(self.home.glob("air-gpu-collect-*.tgz")), [])
        self.assertEqual(self.leftovers(), [])
        kept = list((self.statedir / "collect").glob("air-gpu-collect-*.tgz"))
        self.assertEqual(len(kept), 1, proc.stdout)
        self.assertEqual(stat.S_IMODE(kept[0].stat().st_mode) & 0o077, 0)
        self.assertEqual(stat.S_IMODE((self.statedir / "collect").stat().st_mode) & 0o077, 0)
        self.assertIn("readable by root only", proc.stdout)

    def test_no_world_readable_hand_over_in_the_script(self):
        text = (Path(__file__).resolve().parents[1] / "air-gpu/air-gpu-collect.sh").read_text()
        self.assertNotIn("chmod 0644", text)
        self.assertNotIn("mktemp --tmpdir", text)


if __name__ == "__main__":
    unittest.main()
