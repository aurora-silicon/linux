"""Exercise the Neo installer and session selector using isolated files."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NeoGPU(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="neo-fixture-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.dt = self.tmp / 'dt'
        self.dt.mkdir()
        self.mac('j700', 't8140')
        self.state = self.tmp / 'state'
        self.state.mkdir()
        self.conf = self.tmp / 'update-m1n1'
        self.original_m1n1 = self.tmp / 'original-m1n1.bin'
        self.original_m1n1.write_bytes(b'original own stage2')
        self.conf.write_text(f'U_BOOT=/opt/neo/u-boot.bin\nM1N1={self.original_m1n1}\nOTHER=retained\n')
        self.boot = self.tmp / 'boot.bin'
        self.boot.write_bytes(b'original stage2 and dtbs')
        self.config = self.tmp / 'config'
        self.config.mkdir()
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        tool = self.bin / 'update-m1n1'
        tool.write_text('#!/bin/sh\nexit 0\n')
        tool.chmod(0o755)
        self.prefix = self.tmp / 'prefix'

    def mac(self, board, soc):
        (self.dt / 'compatible').write_bytes(f'apple,{board}\0apple,{soc}\0'.encode())

    def run_sh(self, body):
        setup = f"""
set -euo pipefail
AURORA_SEP_SOURCE_ONLY=1 source '{ROOT / 'install-aurora-sep.sh'}'
sudo=''
DT='{self.dt}'
STATE='{self.state}'
UPDATE_M1N1_CONF='{self.conf}'
NEO_GPU_CONFIG='{self.config}'
NEO_MESA_PREFIX='{self.prefix}'
NEO_MESA_PACKAGE='mesa-neo-test-aarch64.pkg.tar.zst {'a' * 64}'
NEO_M1N1_PACKAGE='m1n1-neo-test-aarch64.pkg.tar.zst {'b' * 64}'
NEO_M1N1_BIN_SHA={'c' * 64}
esp_bootbin() {{ echo '{self.boot}'; }}
{body}
"""
        return subprocess.run(['bash', '-c', setup], text=True, capture_output=True,
                              env={**os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}'})

    def test_neo_plan_selects_separate_pair(self):
        result = self.run_sh('NEO_GPU=1; neo_gpu_plan; m1n1_for_this_mac; echo "$M1N1_PACKAGE:$M1N1_BIN"; neo_gpu_files')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('m1n1-neo-test', result.stdout)
        self.assertIn('/usr/lib/m1n1-neo/m1n1.bin', result.stdout)
        self.assertIn('mesa-neo-test', result.stdout)
        self.assertIn('U_BOOT=/opt/neo/u-boot.bin', self.conf.read_text())

    def test_update_does_not_rearm_removed_owner_intent(self):
        (self.state/'neo-gpu').write_text('j700-g17p-hal200\n')
        (self.config/'profile').write_text('j700-g17p-hal200\n')
        result=self.run_sh('neo_gpu_plan; echo selected=$NEO_GPU')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('selected=0',result.stdout)
        (self.config/'gpu-experiment').touch()
        result=self.run_sh('neo_gpu_plan; echo selected=$NEO_GPU')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('selected=1',result.stdout)

    def test_m3_and_m1_cannot_select_neo(self):
        for board, soc in [('j613', 't8122'), ('j514s', 't6030'), ('j313', 't8103')]:
            self.mac(board, soc)
            result = self.run_sh('NEO_GPU=1; neo_gpu_plan')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('nothing was installed', result.stderr)

    def test_unpinned_or_owner_disabled_pair_refused(self):
        result = self.run_sh("NEO_GPU=1; NEO_MESA_PACKAGE=''; neo_gpu_plan")
        self.assertNotEqual(result.returncode, 0)
        self.conf.write_text('M1N1_UPDATE_DISABLED=1\n')
        result = self.run_sh('NEO_GPU=1; neo_gpu_plan')
        self.assertNotEqual(result.returncode, 0)

    def test_transaction_restores_boot_config_and_intent(self):
        original = self.conf.read_bytes()
        (self.config / 'profile').write_text('prior profile\n')
        result = self.run_sh('''NEO_GPU=1
neo_gpu_transaction_begin
grep -q 'U_BOOT=/opt/neo/u-boot.bin' "$UPDATE_M1N1_CONF"
grep -q 'M1N1=/usr/lib/m1n1-neo/m1n1.bin' "$UPDATE_M1N1_CONF"
printf changed > "$(esp_bootbin)"
printf changed > "$NEO_GPU_CONFIG/profile"
touch "$NEO_GPU_CONFIG/gpu-experiment"
neo_gpu_restore "$STATE/neo-transaction.json"
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.conf.read_bytes(), original)
        self.assertEqual(self.boot.read_bytes(), b'original stage2 and dtbs')
        self.assertEqual((self.config / 'profile').read_text(), 'prior profile\n')
        self.assertFalse((self.config / 'gpu-experiment').exists())
        self.original_m1n1.unlink()
        result = self.run_sh('neo_gpu_restore \"$STATE/neo-before.json\"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.original_m1n1.read_bytes(), b'original own stage2')

    def hook(self, intent=True, profile='j700-g17p-hal200', abi=0):
        for path in ['libexec', 'share/mesa-neo']:
            (self.prefix / path).mkdir(parents=True, exist_ok=True)
        (self.prefix / 'share/mesa-neo/profile').write_text('j700-g17p-hal200\n')
        for name, rc in [('mesa-neo-abi-check', abi), ('mesa-neo-loadcheck', 0)]:
            f = self.prefix / 'libexec' / name
            f.write_text(f'#!/bin/sh\nexit {rc}\n')
            f.chmod(0o755)
        (self.config / 'profile').write_text(profile + '\n')
        if intent:
            (self.config / 'gpu-experiment').touch()
        render = self.tmp / 'sys/renderD128/device/of_node'
        render.mkdir(parents=True, exist_ok=True)
        (render / 'compatible').write_bytes(b'apple,agx-t8140\0')
        (self.tmp / 'dev').mkdir(exist_ok=True)
        (self.tmp / 'dev/renderD128').touch()
        source = (ROOT / 'neo/mesa-neo-session-env').read_text()
        for old, new in [('/proc/device-tree', self.dt), ('/sys/class/drm', self.tmp / 'sys'),
                         ('/dev/dri', self.tmp / 'dev'), ('/etc/mesa-neo', self.config),
                         ('/opt/mesa-neo', self.prefix)]:
            source = source.replace(old, str(new))
        script = self.tmp / 'hook'
        script.write_text(source)
        env = {**os.environ, 'HOME': str(self.tmp), 'VK_DRIVER_FILES': '/stale/m3.json',
               'VK_ICD_FILENAMES': '/stale/m3.json', 'GALLIUM_DRIVER': 'zink',
               'LD_LIBRARY_PATH': '/opt/mesa-m3/lib:/opt/mesa-m3/25g83/lib:/keep/lib'}
        result = subprocess.run(['sh', '-c', '. "$1"; env', '_', str(script)], env=env,
                                text=True, capture_output=True, check=True)
        return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def test_hook_native_gl_and_vk_paths_clear_stale_m3(self):
        env = self.hook()
        self.assertEqual(env['GALLIUM_DRIVER'], 'asahi')
        self.assertEqual(env['MESA_NEO_PROFILE'], 'j700-g17p-hal200')
        self.assertEqual(env['VK_DRIVER_FILES'], str(self.prefix / 'share/vulkan/icd.d/asahi_icd.aarch64.json'))
        self.assertEqual(env['LD_LIBRARY_PATH'], str(self.prefix / 'lib') + ':/keep/lib')
        self.assertNotIn('LIBGL_ALWAYS_SOFTWARE', env)

    def test_hook_requires_intent_exact_token_and_runtime_abi(self):
        for args in [dict(intent=False), dict(profile='j613-25g83-hal200'), dict(abi=1)]:
            with self.subTest(args=args):
                env = self.hook(**args)
                self.assertEqual(env['LIBGL_ALWAYS_SOFTWARE'], '1')
                self.assertNotIn('VK_DRIVER_FILES', env)
                self.assertNotIn('MESA_NEO_PROFILE', env)
                self.assertEqual(env['LD_LIBRARY_PATH'], '/keep/lib')

    def test_hook_leaves_other_macs_unchanged(self):
        self.mac('j613', 't8122')
        env = self.hook()
        self.assertEqual(env['GALLIUM_DRIVER'], 'zink')
        self.assertEqual(env['VK_DRIVER_FILES'], '/stale/m3.json')


if __name__ == '__main__':
    unittest.main()
