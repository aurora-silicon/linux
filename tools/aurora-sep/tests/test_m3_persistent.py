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
