"""Matched persistent activation and exact 25 profile on fixture files."""
from pathlib import Path
import json
import os
import subprocess
import test_m3_handoff

class Persistent(test_m3_handoff.M3PathTest):
    def setup_profile(self, profile='legacy', stage1='v1.6.1-dirty'):
        self.mac('j613', stage1=stage1, stub='14.8.3' if profile=='legacy' else '26.6.2')
        (self.dt/'chosen/asahi,os-fw-version').write_bytes(b'14.8.3\0' if profile=='legacy' else b'26.6.2\0')
        return f'M3_GPU_PERSISTENT=1\nM3_GPU_PROFILE={profile}\nM3_TRY=1\nM3_STACK_ID={"a"*64}\nM3_STAGE1_25_VERSIONS=source-built-25\n'

    def test_persistent14_plan_and_switches(self):
        out=self.run_sh(self.setup_profile()+'m3_plan\nm3_persistent_preflight\necho "$M3_MODE"\nm3_switches\nm3_variant').stdout
        self.assertIn('handoff',out)
        self.assertIn('chosen.asahi,t8122-gpu=1',out)
        self.assertIn('chosen.asahi,t8122-gpu-fuse-leakage=1',out)
        self.assertIn('air-gpu-persistent-legacy',out)

    def test_clean161_only_exact_current14_j613(self):
        setup=self.setup_profile(stage1='v1.6.1')
        self.assertEqual(self.run_sh(setup+'m3_stage1_problem').stdout,'')
        (self.dt/'chosen/asahi,m1n1-oslog-overlap').write_bytes(bytes(16))
        self.assertIn('stage 1 is v1.6.1',self.run_sh(setup+'m3_stage1_problem').stdout)
        (self.dt/'chosen/asahi,m1n1-oslog-overlap').unlink()
        (self.dt/'chosen/asahi,os-fw-version').unlink()
        self.assertIn('stage 1 is v1.6.1',self.run_sh(setup+'m3_stage1_problem').stdout)

    def test_clean161_never_qualifies25(self):
        setup=self.setup_profile('j613-25g83','v1.6.1')
        self.assertIn('stage 1 is v1.6.1',self.run_sh(setup+'m3_stage1_problem').stdout)

    def test_scott_dirty_stage1_stub1483_os147_current14_admitted(self):
        setup=self.setup_profile(stage1='v1.6.1-dirty')
        (self.dt/'chosen/asahi,os-fw-version').write_bytes(b'14.7\0')
        out=self.run_sh(setup+'m3_plan\nm3_persistent_preflight\necho "$M3_MODE"').stdout
        self.assertIn('handoff',out)
        self.assertEqual(self.run_sh(setup+'m3_stub_problem\nm3_stage1_problem').stdout,'')

    def test_persistent25_uses_own_fw_and_sourcebuilt_stage1(self):
        setup=self.setup_profile('j613-25g83','source-built-25')
        out=self.run_sh(setup+'m3_plan\nm3_persistent_preflight\necho "$M3_MODE"\nm3_switches').stdout
        self.assertIn('profile j613-25g83',out)
        self.assertIn('handoff',out)
        self.assertIn('chosen.asahi,t8122-gpu=1',out)
        self.assertNotIn('power-standin=1',out)
        self.assertNotIn('14.8.3 stub',out)

    def test25_refuses_linux14_before_any_intent(self):
        setup=self.setup_profile('legacy')+'M3_GPU_PROFILE=j613-25g83\n'
        result=self.run_sh(setup+'m3_plan',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Firmware migration is separate',result.stderr)

    def test_persistent_needs_manifest_and_owner_intent(self):
        setup=self.setup_profile()
        result=self.run_sh(setup+'m3_plan\nM3_STACK_ID=""\nm3_persistent_preflight',check=False)
        self.assertNotEqual(result.returncode,0)
        (self.etc/'m1n1.conf').write_text('chosen.asahi,t8122-gpu=0\n')
        result=self.run_sh(setup+'m3_plan\nm3_persistent_preflight',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('explicit owner switch-off',result.stderr)

    def selection(self):
        return f'M3_GPU_OPTIN="{self.etc}/intent"\nM3_GPU_PROFILE_FILE="{self.etc}/profile"\nM3_MESA_NATIVE_MARKER="{self.tmp}/native-marker"\nchain=grub\n'

    def test_marker_failure_does_not_publish_intent_or_state(self):
        setup=self.setup_profile('j613-25g83','source-built-25')+self.selection()
        result=self.run_sh(setup+'m3_persistent_select',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((self.etc/'intent').exists())
        self.assertFalse((self.etc/'profile').exists())
        self.assertFalse((self.state/'m3-gpu-persistent').exists())

    def test_exact25_marker_maps_cli_to_hal200_selector(self):
        (self.tmp/'native-marker').write_text('j613-25g83-gl-only\n')
        self.run_sh(self.setup_profile('j613-25g83','source-built-25')+self.selection()+'m3_persistent_select')
        self.assertEqual((self.etc/'profile').read_text(),'j613-25g83-hal200\n')
        self.assertEqual((self.etc/'intent').read_text(),'1\n')
        self.assertEqual((self.state/'m3-gpu-persistent').read_text(),'j613-25g83\n')

    def test_legacy_selection_stays_independent_of_native_marker(self):
        (self.etc/'profile').write_text('j613-25g83-hal200\n')
        self.run_sh(self.setup_profile()+self.selection()+'m3_persistent_select')
        self.assertFalse((self.etc/'profile').exists())
        self.assertEqual((self.etc/'intent').read_text(),'1\n')

    def test_matched_mesa_stays_in_common_package_transaction(self):
        work=self.tmp/'work';work.mkdir()
        mesa=work/'mesa.pkg.tar.zst';mesa.write_bytes(b'fixture')
        self.run_sh(self.setup_profile()+f'work="{work}"\nM3_PRO_MESA_PACKAGE="mesa.pkg.tar.zst {"a"*64}"\nm3_pro_mesa_set_aside')
        self.assertTrue(mesa.exists())
        self.assertFalse((work/'m3-pro').exists())

    def test_plain_update_retains_explicit_profile(self):
        self.setup_profile('j613-25g83','source-built-25')
        (self.state/'m3-gpu-persistent').write_text('j613-25g83\n')
        out=self.run_sh('M3_STAGE1_25_VERSIONS=source-built-25\nm3_plan\necho "$M3_GPU_PERSISTENT:$M3_GPU_PROFILE:$M3_MODE"').stdout
        self.assertIn('1:j613-25g83:handoff',out)

    def test_existing_grub_fallback_is_explicitly_gpu_off(self):
        entry=self.tmp/'previous-entry'
        entry.write_text("#!/bin/sh\ncat <<'MENU'\nlinux /vmlinuz-aurora-sep-previous root=UUID=abc asahi.t8122-start=1 mesa_m3=on\ninitrd /initramfs-aurora-sep-previous.img\nMENU\n")
        self.run_sh(f'm3_grub_fallback_off "{entry}"')
        text=entry.read_text()
        self.assertNotIn('asahi.t8122-start=1',text)
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',text)

    def transaction_paths(self):
        defaults=self.etc/'default/limine'
        defaults.write_text('KERNEL_CMDLINE[default]="root=UUID=abc rw"\n')
        (self.etc/'m1n1.conf').write_text('# owner setting\n')
        work=self.tmp/'work';work.mkdir()
        (work/'matched.pkg.tar.zst').touch()
        return self.selection()+f'''
chain=limine
work="{work}"
M3_LIMINE_DEFAULTS="{defaults}"
M3_GRUB_DEFAULTS="{self.etc}/default/grub"
M3_PROFILE_HOOK="{self.etc}/hooks/profile.hook"
M3_PROFILE_UPDATE="{self.etc}/libexec/profile-update"
trap 'm3_install_cleanup' EXIT
'''

    def test_pacman_failure_restores_owned_switches_and_bootbin(self):
        import test_m3_boot_profile
        fixture=test_m3_boot_profile.BootProfile();fixture.setUp();self.addCleanup(fixture.doCleanups)
        fixture.run_helper('retain')
        setup=self.setup_profile()+self.transaction_paths()
        bootbin=self.esp/'m1n1/boot.bin';before=bootbin.read_bytes()
        result=self.run_sh(setup+f'''
m3_persistent_transaction_begin "$chain"
m3_switches_write
printf 'package hook changed boot.bin' > "{bootbin}"
pacman() {{ return 23; }}
m3_install_packages
''',check=False)
        self.assertEqual(result.returncode,23,result.stderr)
        self.assertEqual((self.etc/'m1n1.conf').read_text(),'# owner setting\n')
        self.assertEqual(bootbin.read_bytes(),before)
        self.assertNotIn('asahi.t8122_start=1',(self.etc/'default/limine').read_text())
        self.assertFalse((self.state/'m3-gpu-persistent').exists())
        self.assertFalse((self.state/'m3-persistent-transaction.json').exists())
        self.assertIn('/Aurora previous (GPU off)',fixture.conf.read_text())
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',fixture.conf.read_text())

    def test_native_marker_after_package_failure_rolls_back_before_arming(self):
        setup=self.setup_profile('j613-25g83','source-built-25')+self.transaction_paths()
        result=self.run_sh(setup+'''
m3_persistent_transaction_begin "$chain"
m3_switches_write
pacman() { return 0; }
m3_install_packages
''',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertNotIn('asahi.t8122_start=1',(self.etc/'default/limine').read_text())
        self.assertFalse((self.etc/'intent').exists())
        self.assertFalse((self.etc/'profile').exists())

    def test_remove_cleans_owned_defaults_without_final_state(self):
        self.run_sh(self.transaction_paths()+'''
m3_persistent_cmdline limine
m3_persistent_remove
''')
        self.assertNotIn('asahi.t8122_start=1',(self.etc/'default/limine').read_text())
        self.assertFalse((self.state/'m3-gpu-persistent').exists())

    def test_post_generation_publish_lock_failure_rolls_back_real_helper(self):
        import fcntl
        import test_m3_boot_profile
        fixture=test_m3_boot_profile.BootProfile();fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.run_helper('retain')
        setup=self.setup_profile()+self.transaction_paths()
        # Execute the production helper after a package hook replaces the main
        # UKI. Its custom EFI fallback survives that regeneration.
        fixture.regenerate()
        fixture.lock2.touch()
        with fixture.lock2.open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            result=self.run_sh(setup+f'''
m3_persistent_transaction_begin "$chain"
m3_switches_write
pacman() {{ return 0; }}
m3_persistent_keep_entry() {{
  python3 "{test_m3_boot_profile.HELPER}" "$1" --esp "{fixture.esp}" --state "{fixture.state}" --defaults "{fixture.defaults}" --lock "{fixture.lock1}" --lock "{fixture.lock2}"
}}
m3_install_packages
''',check=False)
        self.assertNotEqual(result.returncode,0)
        text=fixture.conf.read_text()
        self.assertIn('/Aurora previous (GPU off)',text)
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',text)
        self.assertNotIn('asahi.t8122_start=1',text)
        self.assertNotIn('asahi.t8122_start=1',(self.etc/'default/limine').read_text())
        self.assertEqual((self.etc/'m1n1.conf').read_text(),'# owner setting\n')
        self.assertFalse((self.etc/'intent').exists())
        self.assertFalse((self.state/'m3-gpu-persistent').exists())

    def test_matched_transaction_commits14_and25_only_after_validation(self):
        import test_m3_boot_profile
        for profile in ('legacy','j613-25g83'):
            with self.subTest(profile=profile):
                fixture=test_m3_boot_profile.BootProfile();fixture.setUp()
                try:
                    fixture.run_helper('retain');fixture.regenerate()
                    setup=self.setup_profile(profile,'source-built-25' if profile!='legacy' else 'v1.6.1-dirty')
                    # Reuse isolated transaction files across the two cases.
                    work=self.tmp/'work'
                    if work.exists():
                        import shutil
                        shutil.rmtree(work)
                    setup+=self.transaction_paths()
                    (self.tmp/'native-marker').write_text('j613-25g83-gl-only\n')
                    self.run_sh(setup+f'''
m3_persistent_transaction_begin "$chain"
m3_switches_write
pacman() {{ return 0; }}
m3_persistent_keep_entry() {{
  python3 "{test_m3_boot_profile.HELPER}" "$1" --esp "{fixture.esp}" --state "{fixture.state}" --defaults "{fixture.defaults}" --lock "{fixture.lock1}" --lock "{fixture.lock2}"
}}
m3_install_packages
[[ -f "$STATE/m3-persistent-transaction.json" ]]
# The caller commits only after its boot.bin and remaining install checks.
m3_persistent_transaction_commit
''')
                    self.assertEqual((self.etc/'intent').read_text(),'1\n')
                    self.assertEqual((self.state/'m3-gpu-persistent').read_text(),profile+'\n')
                    self.assertFalse((self.state/'m3-persistent-transaction.json').exists())
                    self.assertIn('asahi.t8122_start=1',(self.etc/'default/limine').read_text())
                    self.assertIn('asahi.t8122_start=1',fixture.conf.read_text())
                    self.assertIn('/Aurora previous (GPU off)',fixture.conf.read_text())
                    if profile=='j613-25g83': self.assertEqual((self.etc/'profile').read_text(),'j613-25g83-hal200\n')
                finally: fixture.doCleanups()

    def test_failure_after_publication_disarms_main_and_restores_intent(self):
        import test_m3_boot_profile
        fixture=test_m3_boot_profile.BootProfile();fixture.setUp();self.addCleanup(fixture.doCleanups)
        fixture.run_helper('retain');fixture.regenerate()
        result=self.run_sh(self.setup_profile()+self.transaction_paths()+f'''
m3_persistent_transaction_begin "$chain"
pacman() {{ return 0; }}
m3_persistent_keep_entry() {{
  python3 "{test_m3_boot_profile.HELPER}" "$1" --esp "{fixture.esp}" --state "{fixture.state}" --defaults "{fixture.defaults}" --lock "{fixture.lock1}" --lock "{fixture.lock2}"
}}
m3_install_packages
# Model a later boot.bin validation failure, before the caller's commit.
false
''',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertNotIn('asahi.t8122_start=1',fixture.conf.read_text())
        self.assertIn('/Aurora previous (GPU off)',fixture.conf.read_text())
        self.assertFalse((self.etc/'intent').exists())
        self.assertFalse((self.state/'m3-gpu-persistent').exists())
        self.assertFalse((self.etc/'hooks/profile.hook').exists())
