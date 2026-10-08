"""Optional desktop admission and the installer's package transaction."""
import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('desktop', ROOT / 'desktop-fixes.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
REQUIRED = {
 'omarchy': ['usr/bin/omarchy-session-guard', 'usr/share/omarchy/shell/session-guard.py',
 'usr/share/omarchy/shell/session-guard-install.py', 'usr/share/omarchy/shell/plugins/lock/Service.qml',
 'usr/share/libalpm/hooks/95-omarchy-session-guard.hook'],
 'omarchy-settings': ['usr/lib/systemd/user/wayland-wm@hyprland.desktop.service.d/99-session-lock-recovery.conf',
 'usr/local/share/wayland-sessions/omarchy.desktop',
 'usr/local/share/wayland-sessions/omarchy-guarded-hyprland.desktop',
 'etc/sddm.conf.d/90-session-lock-recovery.conf']}

class Desktop(unittest.TestCase):
 def setUp(self):
  self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
  self.base = Path(self.temp.name); self.root = self.base / 'root'; self.root.mkdir()
  self.bin = self.base / 'bin'; self.bin.mkdir(); self.assets = self.base / 'assets'; self.assets.mkdir()
  self.log = self.base / 'log'; self.state = self.base / 'state.json'; self.manifest = self.base / 'manifest.json'
  self.data = {'schema':'aurora.desktop-fixes/1','packages':{},'omarchy_catalogs':{}}
  baseline = {'usr/bin/omarchy-test':b'baseline', 'usr/share/omarchy/version':b'4.0.0.alpha'}
  current = dict(baseline)
  for name, paths in REQUIRED.items():
   tree = self.base / name; tree.mkdir()
   payload = {p: ('[Autologin]\nRelogin=false\n'.encode() if p.endswith('90-session-lock-recovery.conf') else p.encode()) for p in paths}
   if name == 'omarchy': payload.update(baseline)
   current.update({p:v for p,v in payload.items() if mod.immutable(p)})
   self.package(name, payload, tree)
  self.package('aquamarine',{}, self.base / 'aquamarine')
  for version, files in [('4.0.4-1',baseline),('4.0.4-2',current)]:
   self.data['omarchy_catalogs'][version] = {'source': ('a' if version.endswith('1') else 'b')*40,
      'files':{p:{'sha256':hashlib.sha256(v).hexdigest(),'mode':0o644} for p,v in files.items()}}
  activation={p for paths in REQUIRED.values() for p in paths if not mod.immutable(p)}
  self.data['omarchy_catalogs']['4.0.4-1']['activation']={p:None for p in activation}
  self.data['omarchy_catalogs']['4.0.4-2']['activation']={p:self.data['packages'][n]['payload'][p] for n,paths in REQUIRED.items() for p in paths if p in activation}
  for p,v in baseline.items(): self.put(p,v)
  self.put('etc/pam.d/sddm',b'auth include system-login\n')
  self.put('etc/sddm.conf.d/autologin.conf',b'[Autologin]\nUser=test\n')
  self.installed = {'omarchy':'4.0.4-1','omarchy-settings':'4.0.4-1','aquamarine':'0.15.1-1'}
  self.owned = list(baseline); self.dep_status=0; self.dep_output=''; self.abi='14'
  self.save()
  (self.bin/'pacman').write_text('''#!/usr/bin/env python3
import os,json,sys
s=json.load(open(os.environ['DESKTOP_STATE'])); a=sys.argv[1:]
with open(os.environ['DESKTOP_LOG'],'a') as f:f.write('pacman '+' '.join(a)+'\\n')
if a==['-Q']:
 for n,v in s['installed'].items():print(n,v)
elif a[:1]==['-Qql']:
 for p in s['owned']:print('/'+p)
elif a[:1]==['-Qi']:print('Provides : libaquamarine.so='+s['abi']+'-64')
elif a[:1]==['-T']:
 print(s['dep_output'],end='');sys.exit(s['dep_status'])
'''); (self.bin/'pacman').chmod(0o755)
  (self.bin/'curl').write_text('''#!/usr/bin/env python3
import os,sys,shutil
from pathlib import Path
a=sys.argv[1:];dst=a[a.index('-o')+1]; src=Path(os.environ['DESKTOP_ASSETS'])/a[-1].split('/')[-1]
shutil.copyfile(src,dst)
'''); (self.bin/'curl').chmod(0o755)
  self.env = {**os.environ,'PATH':str(self.bin)+':'+os.environ['PATH'],'DESKTOP_STATE':str(self.state),
     'DESKTOP_LOG':str(self.log),'DESKTOP_ASSETS':str(self.assets),'TMPDIR':str(self.base)}
 def package(self,name,payload,tree):
  tree.mkdir(exist_ok=True)
  meta=f'pkgname = {name}\npkgver = {mod.NAMES[name]}\narch = aarch64\ndepend = glibc\n'
  if name=='omarchy':meta+='depend = omarchy-settings=4.0.4-2\n'
  if name=='aquamarine':meta+='provides = libaquamarine.so=14-64\n'
  self.write(tree/'.PKGINFO',meta.encode())
  for p,v in payload.items():self.write(tree/p,v)
  path=self.assets/f'{name}-{mod.NAMES[name]}-aarch64.pkg.tar.zst'
  subprocess.run(['bsdtar','--zstd','-cf',str(path),'-C',str(tree),'.PKGINFO',*payload],check=True)
  self.data['packages'][name]={'file':path.name,'version':mod.NAMES[name],
    'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'payload':{p:hashlib.sha256(v).hexdigest() for p,v in payload.items()}}
 def write(self,p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(v)
 def put(self,p,v):self.write(self.root/p,v)
 def save(self):
  self.manifest.write_text(json.dumps(self.data))
  self.state.write_text(json.dumps(dict(installed=self.installed,owned=self.owned,
       dep_status=self.dep_status,dep_output=self.dep_output,abi=self.abi)))
 def run_helper(self,mode='plan'):
  self.save();return subprocess.run(['python3',str(ROOT/'desktop-fixes.py'),mode,str(self.manifest),
    '--root',str(self.root),'--directory',str(self.assets)],env=self.env,text=True,capture_output=True)
 def test_matched_pair_and_abi14_selected_and_dependencies_checked(self):
  result=self.run_helper('verify');self.assertEqual(result.returncode,0,result.stderr)
  self.assertEqual(len(result.stdout.splitlines()),3)
  deps=[l for l in self.log.read_text().splitlines() if l.startswith('pacman -T')]
  self.assertEqual(deps,['pacman -T glibc'])
 def test_absent_and_newer_aquamarine_are_preserved(self):
  for installed in ({},{'aquamarine':'0.16.0-1'},{'aquamarine':'0.15.1-1.3'}):
   self.installed=installed;r=self.run_helper();self.assertEqual((r.returncode,r.stdout),(0,''),r.stderr)
 def test_unknown_mixed_dev_and_newer_omarchy_refused(self):
  for installed in ({'omarchy':'4.0.4-1'}, {'omarchy':'4.0.4-1','omarchy-settings':'4.0.4-2'},
      {'omarchy-dev':'4.0.4-1'}, {'omarchy':'4.1.0-1','omarchy-settings':'4.1.0-1'}):
   self.installed=installed;self.assertNotEqual(self.run_helper().returncode,0)
 def test_runtime_changes_and_unowned_files_refused(self):
  self.put('usr/bin/omarchy-test',b'fork');self.assertNotEqual(self.run_helper().returncode,0)
  self.put('usr/bin/omarchy-test',b'baseline');self.put('usr/share/omarchy/unowned',b'extra')
  self.assertNotEqual(self.run_helper().returncode,0)
 def test_symlink_or_writable_parent_and_marker_refused(self):
  parent=self.root/'usr/share/omarchy';parent.chmod(0o777)
  self.assertNotEqual(self.run_helper().returncode,0);parent.chmod(0o755)
  marker=self.root/'etc/sddm.conf.d/90-session-lock-recovery.conf';marker.symlink_to('/dev/null')
  self.assertNotEqual(self.run_helper().returncode,0)
 def test_relogin_override_refused(self):
  self.put('etc/sddm.conf',b'[Autologin]\nRelogin=true\n');self.assertNotEqual(self.run_helper().returncode,0)
 def test_wrong_abi_is_skipped(self):
  self.abi='15';r=self.run_helper();self.assertEqual(r.returncode,0,r.stderr)
  self.assertNotIn('aquamarine',r.stdout)
 def test_missing_activation_leaf_does_not_allow_unsafe_parent(self):
  directory=self.root/'usr/local/share/wayland-sessions';directory.mkdir(parents=True)
  directory.chmod(0o777);self.assertNotEqual(self.run_helper().returncode,0)
 def test_unknown_existing_activation_file_refused(self):
  self.put('usr/local/share/wayland-sessions/omarchy.desktop',b'Exec=other\n')
  self.assertNotEqual(self.run_helper().returncode,0)
 def test_missing_dependencies_and_failed_empty_query_refused(self):
  for output in ('glibc\n',''):
   self.dep_status=127;self.dep_output=output;self.assertNotEqual(self.run_helper('verify').returncode,0)
 def test_artifact_tamper_and_wrong_metadata_refused(self):
  item=self.data['packages']['aquamarine'];p=self.assets/item['file'];p.write_bytes(b'wrong')
  self.assertNotEqual(self.run_helper('verify').returncode,0)
  tree=self.base/'aquamarine';self.package('aquamarine',{},tree)
  self.write(tree/'.PKGINFO',b'pkgname = aquamarine\npkgver = 0.15.1-1.3\narch = x86_64\n')
  subprocess.run(['bsdtar','--zstd','-cf',str(p),'-C',str(tree),'.PKGINFO'],check=True)
  self.data['packages']['aquamarine']['sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
  self.assertNotEqual(self.run_helper('verify').returncode,0)
 def test_installed_guarded_pair_requires_pam_and_full_payload_then_noop(self):
  self.installed={'omarchy':'4.0.4-2','omarchy-settings':'4.0.4-2'}
  for name in REQUIRED:
   subprocess.run(['bsdtar','-xf',str(self.assets/self.data['packages'][name]['file']),'-C',str(self.root)],check=True)
  self.owned=list(self.data['omarchy_catalogs']['4.0.4-2']['files'])
  self.assertNotEqual(self.run_helper().returncode,0)
  self.put('etc/pam.d/sddm',(mod.PAM_LINE+'\n').encode())
  (self.root/'usr/bin/omarchy-session-guard').chmod(0o755)
  self.data['omarchy_catalogs']['4.0.4-2']['files']['usr/bin/omarchy-session-guard']['mode']=0o755
  r=self.run_helper();self.assertEqual((r.returncode,r.stdout),(0,''),r.stderr)
 def shell(self,body,source=None):
  source=source or ROOT/'install-aurora-sep.sh'
  return subprocess.run(['bash','-c',f'AURORA_SEP_SOURCE_ONLY=1 source "{source}"\n'+body],
     env=self.env,text=True,capture_output=True)
 def test_builtin_helper_bytes_and_default_are_unchanged(self):
  r=self.shell('desktop_fixes_builtin');self.assertEqual(r.stdout,(ROOT/'desktop-fixes.py').read_text())
  r=self.shell('desktop_fixes_prepare; desktop_fixes_verify nowhere; desktop_fixes_files')
  self.assertEqual((r.returncode,r.stdout),(0,''),r.stderr);self.assertFalse(self.log.exists())
 def test_actual_argument_parser_requires_install_action(self):
  source=ROOT/'install-aurora-sep.sh';text=source.read_text()
  tail=text[text.index('args=()\n',text.index('# Tests source this file')):]
  shim=self.base/'parser.sh';shim.write_text(f'AURORA_SEP_SOURCE_ONLY=1 source "{source}"\n'
     'preflight() { :; }; install_all() { echo "$DESKTOP_FIXES:$READ_ONLY"; }; uninstall_all() { echo UNINSTALL; };\n'+tail)
  r=subprocess.run(['bash',str(shim),'--desktop-fixes','--read-only'],env=self.env,text=True,capture_output=True)
  self.assertEqual((r.returncode,r.stdout),(0,'1:1\n'),r.stderr)
  r=subprocess.run(['bash',str(shim),'--desktop-fixes','--uninstall'],env=self.env,text=True,capture_output=True)
  self.assertNotEqual(r.returncode,0);self.assertNotIn('UNINSTALL',r.stdout)
 def test_assembler_verifies_optional_three_roles_and_keeps_default_empty(self):
  spec=importlib.util.spec_from_file_location('assembly',ROOT/'assemble-m3-stack.py')
  assembly=importlib.util.module_from_spec(spec);spec.loader.exec_module(assembly)
  self.assertEqual(assembly.desktop_data({},self.assets),'')
  encoded=assembly.desktop_data({'desktop_fixes':self.data},self.assets)
  self.assertEqual(json.loads(base64.b64decode(encoded)),self.data)
  self.data['packages']['aquamarine']['sha256']='0'*64
  with self.assertRaisesRegex(ValueError,'checksum'):
   assembly.desktop_data({'desktop_fixes':self.data},self.assets)
 def install(self,old=False):
  self.save();source=ROOT/'install-aurora-sep.sh'
  if old:
   source=self.base/'old-installer.sh';source.write_bytes(subprocess.check_output(['git','show','05d6db:tools/aurora-sep/install-aurora-sep.sh'],cwd=ROOT))
  names=re.findall(r'^([a-zA-Z0-9_]+)\(\) \{',source.read_text(),re.M)
  keep={'install_all','m3_install_packages','say','warn','die'}|{n for n in names if n.startswith('desktop_fixes_')}
  stubs='\n'.join(n+'() { :; }' for n in names if n not in keep)
  kernel=self.assets/'linux-aurora-candidate-aarch64.pkg.tar.zst';kernel.write_bytes(b'kernel')
  body=stubs+f'''
sudo=""; STATE="{self.base}/state"; DESKTOP_FIXES=1
mkdir -p "$STATE"
DESKTOP_FIXES_DATA='{base64.b64encode(self.manifest.read_bytes()).decode()}'
KERNELPIN='{kernel.name} {hashlib.sha256(kernel.read_bytes()).hexdigest()}'
current_kernel() {{ echo linux-asahi; }}
boot_chain() {{ echo other; }}
packages_for_this_mac() {{ echo "$KERNELPIN"; }}
m3_gpu_files() {{ :; }}; m3_pro_mesa_files() {{ :; }}
m3_plan() {{ M3_MODE=none; }}
for n in is_m3 is_neo is_m3_air m1n1_for_this_mac m3_air_default; do eval "$n() {{ return 1; }}"; done
m1n1_rollback_check() {{ echo FIRST_WRITE >>"$DESKTOP_LOG"; }}
snapshot() {{ echo SNAPSHOT >>"$DESKTOP_LOG"; }}
systemctl() {{ :; }}
desktop_fixes_run() {{ python3 "{ROOT}/desktop-fixes.py" "$1" "{self.manifest}" --root "{self.root}" "${{@:2}}"; }}
install_all
'''
  return self.shell(body,source)
 def test_installer_preflight_refuses_before_first_existing_write(self):
  self.put('usr/bin/omarchy-test',b'unknown');r=self.install()
  self.assertNotEqual(r.returncode,0);self.assertNotIn('FIRST_WRITE',self.log.read_text())
  self.assertNotIn('pacman -U',self.log.read_text())
 def test_selected_packages_join_one_kernel_transaction(self):
  r=self.install();self.assertEqual(r.returncode,0,r.stderr)
  tx=[l for l in self.log.read_text().splitlines() if l.startswith('pacman -U')]
  self.assertEqual(len(tx),1)
  for name in ('linux-aurora-candidate','omarchy-4.0.4-2','omarchy-settings-4.0.4-2','aquamarine-0.15.1-1.3'):
   self.assertIn(name,tx[0])
 def test_original_installer_has_no_desktop_admission_or_transaction(self):
  self.put('usr/bin/omarchy-test',b'unknown');r=self.install(old=True)
  self.assertEqual(r.returncode,0,r.stderr)
  log=self.log.read_text();self.assertIn('FIRST_WRITE',log)
  tx=[l for l in log.splitlines() if l.startswith('pacman -U')]
  self.assertEqual(len(tx),1);self.assertNotIn('omarchy-',tx[0])

if __name__=='__main__':unittest.main()
