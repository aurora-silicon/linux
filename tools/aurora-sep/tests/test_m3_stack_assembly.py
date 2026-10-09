from pathlib import Path
import hashlib
import importlib.util
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('assemble',ROOT/'assemble-m3-stack.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
class Assembly(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.manifest=dict(schema='aurora.m3-matched-stack/1',version='candidate-1',tag='sep-candidate-1',
                           source_commits={'kernel':'a'*40,'m1n1':'b'*40},stage1_25_versions=['v1.6.1-m3next.stage1'],packages={})
        self.binary=b'apple,j613-25g83-mapping-handoff\0apple,j613-25g83-gpu-handoff\0'
        self.manifest['m1n1_bin_sha256']=hashlib.sha256(self.binary).hexdigest()
        for role,name in mod.ROLES.items():
            files={'.PKGINFO':f'pkgname = {name}\npkgver = candidate-1\narch = aarch64\ndepend = glibc\n'.encode()}
            if role=='m1n1':files['usr/lib/asahi-boot/m1n1.bin']=self.binary
            if role=='kernel':files['usr/lib/modules/test/dtbs/apple/t8122-j613-25g83.dtb']=b'apple,j613-25g83-profile\0apple,firmware-compat\0'
            if role=='mesa':
                files.update({'opt/mesa-m3/25g83/share/mesa-m3/profile':b'j613-25g83-gl-only\n',
                              'usr/share/uwsm/env.d/50-mesa-m3':b'. /opt/mesa-m3/libexec/mesa-m3-session-env\n',
                              'opt/mesa-m3/libexec/mesa-m3-session-env':b'profile=j613-25g83-hal200\n',
                              'opt/mesa-m3/libexec/mesa-m3-abi-check':b'helper',
                              'opt/mesa-m3/libexec/mesa-m3-user-setup':b'detector',
                              'opt/mesa-m3/share/mesa-m3/user-setup.list':b'list'})
            self.package(role,name,files)
    def package(self,role,name,files):
        tree=self.root/role;tree.mkdir(exist_ok=True)
        for path,data in files.items():
            p=tree/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        path=self.root/f'{name}-candidate-1-aarch64.pkg.tar.zst'
        subprocess.run(['bsdtar','--zstd','-cf',str(path),'-C',str(tree),*files],check=True)
        self.manifest['packages'][role]=dict(file=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    def assemble(self):return mod.assemble((ROOT/'install-aurora-sep.sh').read_text(),self.manifest,self.root)
    def test_legacy_board_capability_defaults_and_exact_j615_pair(self):
        self.assertIn('M3_PERSISTENT_BOARDS="j613"',self.assemble()[0])
        self.manifest['legacy_gpu_boards']=['j613','j615']
        with self.assertRaisesRegex(ValueError,'kernel consumer'):self.assemble()
        self.manifest['source_commits']['kernel']='debefcbd9d68ecce23df1b22ad6355ce978ea31b'
        with self.assertRaisesRegex(ValueError,'m1n1 producer'):self.assemble()
        self.manifest['source_commits']['m1n1']='74ba6bea52d1f865d204bb3f8168705a148fd5c5'
        self.assertIn('M3_PERSISTENT_BOARDS="j613 j615"',self.assemble()[0])
        for bad in [['j615','j615'],['j504'],[], 'j613 j615']:
            self.manifest['legacy_gpu_boards']=bad
            with self.assertRaises(ValueError):self.assemble()

    def test_complete_artifact_manifest_pins_exact_stack(self):
        script,ident=self.assemble()
        self.assertIn(f'M3_STACK_ID="{ident}"',script)
        self.assertIn('VERSION=candidate-1',script)
        self.assertIn('M1N1_BIN_SHA='+self.manifest['m1n1_bin_sha256'],script)
        self.assertIn('M3_PROFILE_SELECTOR=j613-25g83-hal200',script)
        self.assertNotIn('mesa-m3-26.1.4.m3.1-6',script)
        output=self.root/'candidate.sh';output.write_text(script)
        subprocess.run(['bash','-n',str(output)],check=True)
    def kernel_dtbs(self,paths):
        files={'.PKGINFO':b'pkgname = linux-aurora\npkgver = candidate-1\narch = aarch64\n'}
        files.update({p:b'apple,j613-25g83-profile\0apple,firmware-compat\0' for p in paths})
        self.package('kernel','linux-aurora',files)
    def test_flat_release_recipe_layout_is_accepted(self):
        self.kernel_dtbs(['usr/lib/modules/test/dtbs/t8122-j613-25g83.dtb'])
        self.assemble()
    def test_missing_profile_dtb_is_rejected(self):
        self.kernel_dtbs(['usr/lib/modules/test/dtbs/t8122-j613.dtb'])
        with self.assertRaisesRegex(ValueError,'exactly one'):self.assemble()
    def test_flat_and_nested_duplicates_are_rejected(self):
        self.kernel_dtbs(['usr/lib/modules/test/dtbs/t8122-j613-25g83.dtb',
                          'usr/lib/modules/test/dtbs/apple/t8122-j613-25g83.dtb'])
        with self.assertRaisesRegex(ValueError,'exactly one'):self.assemble()
    def test_profile_outside_modules_dtbs_is_rejected(self):
        self.kernel_dtbs(['boot/apple/t8122-j613-25g83.dtb'])
        with self.assertRaisesRegex(ValueError,'exactly one'):self.assemble()
    def test_missing_artifact_refuses(self):
        (self.root/self.manifest['packages']['mesa']['file']).unlink()
        with self.assertRaises(FileNotFoundError):self.assemble()
    def test_hash_mutation_refuses(self):
        self.manifest['packages']['kernel']['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'hash differs'):self.assemble()
    def test_binary_identity_mismatch_refuses(self):
        self.manifest['m1n1_bin_sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'binary hash differs'):self.assemble()
    def test_missing_stage1_qualification_refuses(self):
        self.manifest['stage1_25_versions']=[]
        with self.assertRaisesRegex(ValueError,'stage1'):self.assemble()
    def mesa_member(self,name,data):
        files={str(p.relative_to(self.root/'mesa')):p.read_bytes()
               for p in (self.root/'mesa').rglob('*') if p.is_file()}
        if data is None:del files[name]
        else:files[name]=data
        self.package('mesa','mesa-m3',files)
    def test_wrong_session_delegation_refuses(self):
        self.mesa_member('usr/share/uwsm/env.d/50-mesa-m3',b'. /tmp/mesa-m3-session-env\n')
        with self.assertRaisesRegex(ValueError,'delegation'):self.assemble()
    def test_commented_delegation_refuses(self):
        self.mesa_member('usr/share/uwsm/env.d/50-mesa-m3',b'# . /opt/mesa-m3/libexec/mesa-m3-session-env\n')
        with self.assertRaisesRegex(ValueError,'delegation'):self.assemble()
    def test_missing_delegated_session_refuses(self):
        self.mesa_member('opt/mesa-m3/libexec/mesa-m3-session-env',None)
        with self.assertRaises(subprocess.CalledProcessError):self.assemble()
    def test_wrong_delegated_selector_refuses(self):
        self.mesa_member('opt/mesa-m3/libexec/mesa-m3-session-env',b'profile=legacy\n')
        with self.assertRaisesRegex(ValueError,'selector'):self.assemble()
