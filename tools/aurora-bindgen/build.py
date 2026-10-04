#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch pinned source, apply generic fixes, and build/test a private bindgen."""
if not __debug__:
    raise SystemExit("optimized Python is unsupported: validation assertions must remain enabled")

import argparse,hashlib,json,subprocess,tarfile,urllib.request,shutil,os,sys
from pathlib import Path
P=Path(__file__).resolve().parent;ap=argparse.ArgumentParser();ap.add_argument('--output-dir',required=True,type=Path);ap.add_argument('--archive',type=Path);ap.add_argument('--jobs',type=int,default=2);a=ap.parse_args();assert 1<=a.jobs<=2
O=a.output_dir.resolve();assert not O.exists(),'refusing to overwrite an existing source or tool';O.mkdir(parents=True)
m=json.loads((P/'sources.json').read_text());sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();archive=O/'source.tar.gz'
if a.archive:shutil.copyfile(a.archive,archive)
else:
 with urllib.request.urlopen(m['source_url'],timeout=60) as response, archive.open('wb') as f:shutil.copyfileobj(response,f)
assert sha(archive)==m['source_sha256'],'source digest mismatch'
with tarfile.open(archive) as t:t.extractall(O,filter='data')
S=O/m['source_directory']
for name,h in m['patches'].items():
 p=P/'patches'/name;assert sha(p)==h,name;subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(p)],cwd=S,check=True)
assert sha(S/'Cargo.lock')==m['cargo_lock_sha256']
for f,h in m['patched_files'].items():assert sha(S/f)==h,f
cmd=['cargo','build','--locked','--release','-p','bindgen-cli','-j'+str(a.jobs)]
with (O/'build.log').open('w') as log:subprocess.run(cmd,cwd=S,stdout=log,stderr=subprocess.STDOUT,check=True)
binary=S/'target/release/bindgen';subprocess.run([sys.executable,str(P/'test.py'),'--bindgen',str(binary),'--output-dir',str(O/'tests')],check=True)
r={'status':'BUILT_AND_COMPILE_TESTED','binary':str(binary),'binary_sha256':sha(binary),'source_manifest_sha256':sha(P/'sources.json'),'build_command':cmd,'rustc_version':subprocess.check_output(['rustc','-vV'],text=True),'cargo_version':subprocess.check_output(['cargo','-vV'],text=True),'tests_manifest_sha256':sha(O/'tests/result.json'),'installed_globally':False};(O/'result.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2));print('Use make BINDGEN='+str(binary)+' ...; keep full kernel layout assertions enabled.')
