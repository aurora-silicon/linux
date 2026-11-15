"""Execute the packaged Pro admission, environment and batching helpers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import test_m3_flow as flow

PRO = Path(__file__).resolve().parent.parent / 'pro'
IDENTITY = 'profile=legacy chip=0x6030 G15S rev=0x11 dies=1 clusters=2 USC3 HAL0 params=600 match=1'

@unittest.skipUnless(shutil.which('bwrap'), 'bubblewrap is required')
class ProProfile(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.write('bin/uname', '#!/bin/sh\necho test-kernel\n', True)
        self.write('bin/journalctl', '#!/bin/sh\nexit 1\n', True)
        self.write('proc/cmdline', 'root=test\n')
        self.write('modules/test-kernel/aurora-m3pro-profile', 't6030-perf-v1\nsource=d49021440d8ca02bbc8ecc17c19ce76c048b6236\n')
        self.write('identity', IDENTITY + '\n')
        self.write('opt/mesa-m3/libexec/mesa-m3-abi-check', '#!/bin/sh\ncat /tmp/fixture/identity\n', True)
        for profile, marker, source in (
            ('native-pro', 't6030-native-gl-legacy-hal', 'd616d0650d6fa80f72a750a0121442ab917ed6cf'),
            ('pro-vulkan', 't6030-pro-vulkan-legacy-hal', 'b0f56952b7f06955b143fbfa90c07de4c5864964')):
            self.write(f'opt/mesa-m3/{profile}/share/mesa-m3/profile', marker + '\n')
            self.write(f'opt/mesa-m3/{profile}/share/mesa-m3/capabilities', 'source=' + source + '\n')
        for name in ('mesa-m3-pro-check', 'mesa-m3-pro-performance', 'mesa-m3-pro-session-env'):
            self.write('opt/mesa-m3/native-pro/libexec/' + name, (PRO / name).read_text(), True)
        for name, value in [('m3_consistent_perf', '0'), ('m3_pipeline_depth', '0'),
                            ('m3_render_batch_override', '0'), ('m3_compute_batch_override', '0'),
                            ('m3_mixed_batch', '0')]:
            self.write('sys/module/asahi/parameters/' + name, value)
        self.write('sys/kernel/debug/asahi-m3/completion_wait', '0')
        self.write('sys/kernel/debug/asahi-m3/submit_overlap', '0')
        for directory in ('etc', 'home'):
            (self.root / directory).mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text, executable=False):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if executable:
            path.chmod(0o755)

    def run_helper(self, *command, no_home=False):
        arguments = ['bwrap', '--ro-bind', '/', '/', '--tmpfs', '/tmp', '--dev', '/dev', '--bind', str(self.root), '/tmp/fixture']
        for source, target in [('opt', '/opt'), ('etc', '/etc'), ('sys', '/sys'),
                               ('proc', '/proc'), ('modules', '/usr/lib/modules')]:
            arguments += ['--bind', str(self.root / source), target]
        arguments += ['--setenv', 'HOME', '/tmp/fixture/home', '--setenv', 'PATH', '/tmp/fixture/bin:/usr/bin', *command]
        if no_home:
            arguments[-len(command):-len(command)] = ['--unsetenv', 'HOME', '--unsetenv', 'XDG_CONFIG_HOME']
        return subprocess.run(arguments, capture_output=True, text=True)

    def check(self):
        return self.run_helper('/opt/mesa-m3/native-pro/libexec/mesa-m3-pro-check')

    def test_matching_profile_and_performance(self):
        self.assertEqual(self.check().returncode, 0)
        run = self.run_helper('/opt/mesa-m3/native-pro/libexec/mesa-m3-pro-performance')
        self.assertEqual(run.returncode, 0, run.stderr)
        for name, value in [('m3_render_batch_override', '4'), ('m3_compute_batch_override', '4'),
                            ('m3_mixed_batch', '1'), ('m3_pipeline_depth', '0'), ('m3_consistent_perf', '0')]:
            self.assertEqual((self.root / ('sys/module/asahi/parameters/' + name)).read_text(), value)
        self.assertEqual((self.root / 'sys/kernel/debug/asahi-m3/completion_wait').read_text(), '25')
        self.assertEqual((self.root / 'sys/kernel/debug/asahi-m3/submit_overlap').read_text(), '1')

    def test_system_service_without_home(self):
        run = self.run_helper('/opt/mesa-m3/native-pro/libexec/mesa-m3-pro-performance', no_home=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual((self.root / 'sys/module/asahi/parameters/m3_mixed_batch').read_text(), '1')

    def test_environment_selects_both_packages(self):
        run = self.run_helper('/bin/sh', '-c', '. /opt/mesa-m3/native-pro/libexec/mesa-m3-pro-session-env; env')
        self.assertEqual(run.returncode, 0, run.stderr)
        values = dict(line.split('=', 1) for line in run.stdout.splitlines() if '=' in line)
        self.assertEqual(values['LIBGL_DRIVERS_PATH'], '/opt/mesa-m3/native-pro/lib/dri')
        self.assertEqual(values['VK_DRIVER_FILES'], '/opt/mesa-m3/pro-vulkan/share/vulkan/icd.d/asahi_icd.aarch64.json')
        for flag in ('HK_M3_COMPRESSED_RGBA16F', 'HK_M3_COMPRESSED_R8', 'HK_M3_COMPRESSED_SPLIT_DEPTH'):
            self.assertEqual(values[flag], '1')
        self.assertEqual(values['HK_M3_COMPRESSED_DEPTH'], '0')

    def test_other_abis_refused_without_writes(self):
        for identity in (IDENTITY.replace('0x6030', '0x8122'), IDENTITY.replace('HAL0', 'HAL200'),
                         IDENTITY.replace('rev=0x11', 'rev=0x20')):
            with self.subTest(identity=identity):
                self.write('identity', identity + '\n')
                self.assertNotEqual(self.check().returncode, 0)
                run = self.run_helper('/opt/mesa-m3/native-pro/libexec/mesa-m3-pro-performance')
                self.assertEqual(run.returncode, 0)
                self.assertEqual((self.root / 'sys/module/asahi/parameters/m3_mixed_batch').read_text(), '0')

    def test_release16_18_and_19_kernels_admitted_others_refused(self):
        for source, admitted in (('d49021440d8ca02bbc8ecc17c19ce76c048b6236', True),
                                 ('6f63677751854d57bcfac172314957bc2197c05f', True),
                                 ('5ec1028c81526e1ed31db2b97e47edf8663e83cb', True),
                                 ('f240960cc84cfda6f73e188050ccadb9435e938f', False),
                                 ('0000000000000000000000000000000000000000', False)):
            with self.subTest(source=source):
                self.write('modules/test-kernel/aurora-m3pro-profile', 't6030-perf-v1\nsource=' + source + '\n')
                self.assertEqual(self.check().returncode == 0, admitted)

    def test_previous_native_source_is_not_activated(self):
        for source in ('99d5b9d16e779ad9a0879a71578da0a6536cf006',
                       'f287f282940ec0c8b0de50db2df1f98d423e84fa',
                       'd0c0cdb0624ab4b438b694808ac1776551be4bca'):
            with self.subTest(source=source):
                self.write('opt/mesa-m3/native-pro/share/mesa-m3/capabilities',
                           'source=' + source + '\n')
                self.assertNotEqual(self.check().returncode, 0)
                run = self.run_helper('/opt/mesa-m3/native-pro/libexec/mesa-m3-pro-performance')
                self.assertEqual(run.returncode, 0)
                self.assertEqual((self.root / 'sys/module/asahi/parameters/m3_mixed_batch').read_text(), '0')

    def test_opt_out_and_incomplete_pair(self):
        for path, content in [('proc/cmdline', 'root=test mesa_m3=off'),
                              ('etc/mesa-m3/disable', ''), ('etc/mesa-m3/pro-disable', ''),
                              ('home/.config/mesa-m3/disable', ''),
                              ('opt/mesa-m3/native-pro/share/mesa-m3/capabilities', 'source=2dc61e4b462639d29ca846a3ad2dfd1f28629c4e\n'),
                              ('opt/mesa-m3/pro-vulkan/share/mesa-m3/profile', 'wrong\n')]:
            original = (self.root / path).read_text() if (self.root / path).exists() else None
            with self.subTest(path=path):
                self.write(path, content)
                self.assertNotEqual(self.check().returncode, 0)
            if original is None:
                (self.root / path).unlink()
            else:
                self.write(path, original)

class ProInstallFlow(flow.M3FlowBase):
    def setUp(self):
        super().setUp()
        self.pair = {}
        for name in ('mesa-m3-native-pro', 'mesa-m3-pro-vulkan'):
            filename = name + '-candidate-1-aarch64.pkg.tar.zst'
            self.pro_mesa_fixture(filename, name, 'candidate-1', 'opt/' + name)
            self.pair[name] = filename

    def pro_install(self, board, optout=False, corrupt=False, wrong_name=False, fail=False):
        self.mac(board)
        native, vulkan = (self.pair[name] for name in ('mesa-m3-native-pro', 'mesa-m3-pro-vulkan'))
        if fail:
            self.extra_env['FAKE_FAIL_U_FOR'] = 'mesa-m3-native-pro-*'
        if wrong_name:
            self.pro_mesa_fixture(native, 'unrelated-package', 'candidate-1', 'opt/wrong')
        digest = '0' * 64 if corrupt else self.shas[native]
        return self.install(check=False, env=f'''
M3_PRO_NATIVE_PACKAGE="{native} {digest}"
M3_PRO_VULKAN_PACKAGE="{vulkan} {self.shas[vulkan]}"
M3_PRO_PERF_MIGRATOR=fixture
M3_PRO_MESA={0 if optout else 1}
m3_pro_perf_activate() {{ ((M3_PRO_PERF_SELECTED)) || return 0; echo pro-activated >>"$FAKE/log"; }}
''')

    def test_fresh_pro_gets_kernel_and_all_mesa_in_one_transaction(self):
        run = self.pro_install('j516s')
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
        transactions = [line for line in self.log().splitlines() if line.startswith('pacman -U ')]
        self.assertEqual(len(transactions), 1, self.log())
        for filename in (self.pro_mesa, *self.pair.values()):
            self.assertIn(filename, transactions[0])
        self.assertIn('pro-activated', self.log())

    def test_other_macs_and_optout_do_not_download_pro_pair(self):
        for board in ('j613', 'j313', 'j414s', 'j516c', 'j700'):
            with self.subTest(board=board):
                self.fresh_state()
                run = self.pro_install(board)
                self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
                self.assertFalse(set(self.pair.values()) & set(self.downloaded()))
                self.assertNotIn('pro-activated', self.log())
        self.fresh_state()
        run = self.pro_install('j516s', optout=True)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
        self.assertFalse(set(self.pair.values()) & set(self.downloaded()))

    def test_bad_artifact_is_rejected_before_package_transaction(self):
        for options in ({'corrupt': True}, {'wrong_name': True}):
            with self.subTest(options=options):
                self.fresh_state()
                run = self.pro_install('j516s', **options)
                self.assertNotEqual(run.returncode, 0)
                self.assertNotIn('pacman -U ', self.log())
                self.assertNotIn('pro-activated', self.log())

    def test_transaction_failure_does_not_activate_profile(self):
        run = self.pro_install('j516s', fail=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertNotIn('pro-activated', self.log())

if __name__ == '__main__':
    unittest.main()
