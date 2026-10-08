from pathlib import Path
import fcntl
import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
HELPER=Path(__file__).resolve().parent.parent/'m3-boot-profile.py'
class BootProfile(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.esp=self.root/'esp';self.state=self.root/'state'
        (self.esp/'EFI/BOOT').mkdir(parents=True);(self.esp/'EFI/Linux').mkdir()
        (self.esp/'EFI/BOOT/BOOTAA64.EFI').write_bytes(b'limine.conf++CONFIG_B2SUM_SIGNATURE++'+b'0'*128)
        self.defaults=self.root/'defaults';self.defaults.write_text('ENABLE_ENROLL_LIMINE_CONFIG=no\n')
        self.uki=self.esp/'EFI/Linux/omarchy_linux-aurora.efi';self.uki.write_bytes(b'fixture UKI6.12-test\0')
        self.conf=self.esp/'EFI/BOOT/limine.conf'
        digest=hashlib.blake2b(self.uki.read_bytes()).hexdigest()
        self.original='/+Omarchy\n  //linux-aurora\n    protocol: efi\n    path: boot():/EFI/Linux/omarchy_linux-aurora.efi#'+digest+'\n    cmdline: root=UUID=abc rw\n'
        self.conf.write_text(self.original)
        modules=self.root/'modules/6.12-test';modules.mkdir(parents=True)
        (modules/'pkgbase').write_text('linux-aurora\n');(modules/'modules.dep').write_text('fixture\n')
        (modules/'dtbs').mkdir();(modules/'dtbs/wrong.dtb').touch()
        self.lock1=self.root/'lock1';self.lock2=self.root/'lock2'
        # Model the documented custom-EFI API, without mounting a FAT ESP or
        # invoking the host's boot tool. Kernel regeneration below preserves
        # this top-level entry, as the production tool's tree writer does.
        self.tool=self.root/'entry-tool'
        self.tool.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
args=sys.argv[1:]
assert args[0]=='--add-efi' and '--no-mutex' in args and '--no-hooks' in args
assert '--overwrite' in args and args[args.index('--priority')+1]=='90'
esp=Path(os.environ['FIXTURE_ESP']); conf=esp/'EFI/BOOT/limine.conf'
text=conf.read_text(); name='/'+args[1]
if name in text: text=text[:text.index(name)].rstrip()+'\\n'
path=Path(args[2]).relative_to(esp)
conf.write_text(text+'\\n'+name+'\\n    protocol: efi\\n    path: boot():/'+str(path)+'\\n')
''')
        self.tool.chmod(0o755)
    def run_helper(self,action,ok=True):
        import os
        result=subprocess.run(['python3',str(HELPER),action,'--esp',str(self.esp),'--state',str(self.state),'--defaults',str(self.defaults),'--modules',str(self.root/'modules'),'--release','6.12-test','--entry-tool',str(self.tool),'--lock',str(self.lock1),'--lock',str(self.lock2)],capture_output=True,text=True,env={**os.environ,'FIXTURE_ESP':str(self.esp)})
        if ok:self.assertEqual(result.returncode,0,result.stderr)
        else:self.assertNotEqual(result.returncode,0)
        return result
    def test_retains_bytes_modules_and_gpu_off_entry(self):
        self.run_helper('retain')
        saved=json.loads((self.state/'m3-known-entry.json').read_text())
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',saved['cmdline'])
        path=self.esp/saved['path'].split('#')[0].removeprefix('boot():/')
        self.assertEqual(path.read_bytes(),self.uki.read_bytes())
        self.assertFalse((self.state/'modules-6.12-test/dtbs').exists())
        self.assertIn('/Aurora previous (GPU off)', self.conf.read_text())
        self.assertIn('asahi.t8122_start=0 mesa_m3=off', self.conf.read_text())
        self.assertNotIn('asahi.t8122_start=1', self.conf.read_text())
        self.run_helper('publish');self.run_helper('publish')
        text=self.conf.read_text()
        self.assertEqual(text.count('/Aurora previous (GPU off)'),1)
        self.assertIn('root=UUID=abc rw asahi.t8122_start=1',text)
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',text)
    def test_hash_mutation_refuses_before_configuration_change(self):
        self.uki.write_bytes(b'mutated')
        self.run_helper('retain',False)
        self.assertEqual(self.conf.read_text(),self.original)
        self.assertFalse((self.state/'m3-known-entry.json').exists())
    def test_enrolled_limine_refuses(self):
        (self.esp/'EFI/BOOT/BOOTAA64.EFI').write_bytes(b'limine.conf++CONFIG_B2SUM_SIGNATURE++'+b'a'*128)
        self.run_helper('retain',False)
        self.assertEqual(self.conf.read_text(),self.original)
    def test_locked_or_symlink_lock_refuses(self):
        self.lock1.touch()
        with self.lock1.open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.run_helper('retain',False)
        self.lock1.unlink();self.lock1.symlink_to(self.defaults)
        self.run_helper('retain',False)
    def test_wrong_running_kernel_refuses(self):
        self.uki.write_bytes(b'wrong6.13\0')
        digest=hashlib.blake2b(self.uki.read_bytes()).hexdigest()
        self.conf.write_text(self.original.split('#')[0]+'#'+digest+'\n    cmdline: root=UUID=abc rw\n')
        self.run_helper('retain',False)
    def test_new_kernel_hook_restores_retained_entry(self):
        self.run_helper('retain');self.run_helper('publish')
        self.regenerate()
        self.run_helper('publish')
        self.assertIn('/Aurora previous (GPU off)',self.conf.read_text())
        self.assertIn('asahi.t8122_start=1',self.conf.read_text())

    def regenerate(self):
        fallback=self.conf.read_text().split('/Aurora previous (GPU off)',1)[1]
        self.uki.write_bytes(b'new UKI6.13\0');digest=hashlib.blake2b(self.uki.read_bytes()).hexdigest()
        self.conf.write_text(self.original.split('#')[0]+'#'+digest+'\n    cmdline: root=UUID=abc rw\n\n/Aurora previous (GPU off)'+fallback)

    def test_post_generation_second_lock_failure_keeps_reachable_fallback(self):
        self.run_helper('retain')
        self.regenerate()
        before=self.conf.read_bytes()
        self.lock2.touch()
        with self.lock2.open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.run_helper('publish',False)
        self.assertEqual(self.conf.read_bytes(),before)
        self.assertIn('/Aurora previous (GPU off)',self.conf.read_text())
        self.assertIn('asahi.t8122_start=0 mesa_m3=off',self.conf.read_text())
        self.assertNotIn('asahi.t8122_start=1',self.conf.read_text())

    def test_missing_custom_entry_refuses_activation(self):
        self.run_helper('retain')
        self.conf.write_text(self.original)
        self.run_helper('publish',False)
        self.assertNotIn('asahi.t8122_start=1',self.conf.read_text())

    def test_disarm_keeps_both_main_and_fallback_gpu_off(self):
        self.run_helper('retain');self.run_helper('publish')
        self.run_helper('disarm')
        self.assertNotIn('asahi.t8122_start=1',self.conf.read_text())
        self.assertEqual(self.conf.read_text().count('asahi.t8122_start=0 mesa_m3=off'),2)
