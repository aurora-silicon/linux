from pathlib import Path
import hashlib
import importlib.util
import subprocess
import tempfile
import unittest
from unittest import mock
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
            if role=='kernel':
                files['.PKGINFO'] += b'depend = m1n1>=1.6.1\n'
                files['usr/lib/modules/test/dtbs/apple/t8122-j613-25g83.dtb']=b'apple,j613-25g83-profile\0apple,firmware-compat\0'
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
    def test_kernel_dependency_preserves_existing_bootloaders(self):
        self.assemble()
        base = b'pkgname = linux-aurora\npkgver = candidate-1\narch = aarch64\n'
        for dependencies in ([], ['m1n1'], ['m1n1>=1.6.1.aurora15'],
                             ['m1n1>=1.6.1', 'm1n1>=1.6.1.aurora15'],
                             ['m1n1>=1.6.1', 'm1n1>=1.6.1'],
                             ['m1n1>=1.6.1', 'm1n1-aurora'],
                             ['m1n1>=1.6.1', 'm1n1-neo>=1']):
            with self.subTest(dependencies=dependencies):
                self.package('kernel', 'linux-aurora', {'.PKGINFO': base +
                             ''.join('depend = '+d+'\n' for d in dependencies).encode()})
                with self.assertRaisesRegex(ValueError, 'kernel'):
                    self.assemble()
    def test_legacy_board_capability_defaults_and_exact_j615_pair(self):
        self.assertIn('M3_PERSISTENT_BOARDS="j613"',self.assemble()[0])
        self.manifest['legacy_gpu_boards']=['j613','j615']
        with self.assertRaisesRegex(ValueError,'kernel consumer'):self.assemble()
        self.manifest['source_commits']['kernel']='a4d7ff4acdefcbce7daa7f57866413f21f05fb75'
        with self.assertRaisesRegex(ValueError,'m1n1 producer'):self.assemble()
        self.manifest['source_commits']['m1n1']='74ba6bea52d1f865d204bb3f8168705a148fd5c5'
        self.assertIn('M3_PERSISTENT_BOARDS="j613 j615"',self.assemble()[0])
        for bad in [['j615','j615'],['j504'],[], 'j613 j615']:
            self.manifest['legacy_gpu_boards']=bad
            with self.assertRaises(ValueError):self.assemble()

    def standard_boot_files(self):
        return {'.PKGINFO': b'pkgname = m1n1-aurora\npkgver = candidate-1\narch = aarch64\n',
                'usr/lib/asahi-boot/m1n1.bin': self.binary,
                'usr/share/m1n1-aurora/source': (mod.STANDARD_M1N1_SOURCE+'\n').encode(),
                'usr/share/m1n1-aurora/build-config': b'release=1\nchainloading=0\nj613_esp_stage1=0\ntag=v1.6.1-omarchy.aurora17\n'}

    def test_standard_boot_source_and_build_markers(self):
        self.manifest['source_commits']['m1n1'] = mod.STANDARD_M1N1_SOURCE
        files=self.standard_boot_files()
        self.package('m1n1','m1n1-aurora',files)
        self.assemble()
        for field in ('source','build-config'):
            key='usr/share/m1n1-aurora/'+field
            for value in (None,b'',b'foreign\n'):
                with self.subTest(field=field,value=value):
                    changed=dict(files)
                    if value is None:del changed[key]
                    else:changed[key]=value
                    self.package('m1n1','m1n1-aurora',changed)
                    with self.assertRaisesRegex(ValueError,'standard m1n1'):self.assemble()
        for old,new in ((b'release=1',b'release=0'),(b'chainloading=0',b'chainloading=1'),
                        (b'j613_esp_stage1=0',b'j613_esp_stage1=1'),(b'aurora17',b'aurora16')):
            changed=dict(files)
            changed['usr/share/m1n1-aurora/build-config']=changed['usr/share/m1n1-aurora/build-config'].replace(old,new)
            self.package('m1n1','m1n1-aurora',changed)
            with self.assertRaisesRegex(ValueError,'configuration'):self.assemble()

    def test_new_standard_boot_keeps_legacy_j615_admission(self):
        self.manifest['legacy_gpu_boards']=['j613','j615']
        self.manifest['source_commits'].update(kernel=mod.NEO_KERNELS[-1],m1n1=mod.STANDARD_M1N1_SOURCE)
        self.package('m1n1','m1n1-aurora',self.standard_boot_files())
        self.assertIn('M3_PERSISTENT_BOARDS="j613 j615"', self.assemble()[0])
        self.manifest['source_commits']['m1n1']='e'*40
        with self.assertRaisesRegex(ValueError,'m1n1 producer'):self.assemble()

    def test_previous_standard_boot_keeps_legacy_j615_admission(self):
        self.manifest['legacy_gpu_boards']=['j613','j615']
        self.manifest['source_commits'].update(kernel=mod.NEO_PREVIOUS_FW_ROOT_PAIR['kernel'],
                                              m1n1=mod.PREVIOUS_STANDARD_M1N1_SOURCE)
        files=self.standard_boot_files()
        files['usr/share/m1n1-aurora/source']=(mod.PREVIOUS_STANDARD_M1N1_SOURCE+'\n').encode()
        self.package('m1n1','m1n1-aurora',files)
        self.assertIn('M3_PERSISTENT_BOARDS="j613 j615"', self.assemble()[0])
        files['usr/share/m1n1-aurora/build-config']=b'release=0\n'
        self.package('m1n1','m1n1-aurora',files)
        with self.assertRaisesRegex(ValueError,'standard m1n1'):self.assemble()

    def neo_pair(self):
        self.manifest['source_commits']['kernel']=mod.NEO_KERNEL
        neo=dict(profile='j700-g17p-hal200', source_commits={'mesa':'c'*40,'m1n1':'d'*40}, packages={})
        self.manifest['neo']=neo
        for role,name in [('mesa','mesa-neo'),('m1n1','m1n1-neo')]:
            files={'.PKGINFO':f'pkgname = {name}\narch = aarch64\n'.encode()}
            if role=='m1n1':
                files.update({'usr/lib/m1n1-neo/m1n1.bin':b'own Neo binary',
                    'usr/share/m1n1-neo/source':('d'*40+'\n').encode(),
                    'usr/share/m1n1-neo/profile':b'j700-g17p-hal200\n',
                    'usr/share/m1n1-neo/build-config.h':b'#define RELEASE\n#define CHAINLOADING\n#define J700_ESP_STAGE2\n'})
            else:
                files.update({'opt/mesa-neo/share/mesa-neo/profile':b'j700-g17p-hal200\n',
                    'opt/mesa-neo/share/mesa-neo/capabilities':b'opengl=native-experimental\nvulkan=honeykrisp-experimental\n',
                    'usr/share/uwsm/env.d/51-mesa-neo':b'    . /opt/mesa-neo/libexec/mesa-neo-session-env\n',
                    'opt/mesa-neo/libexec/mesa-neo-session-env':(ROOT/'neo/mesa-neo-session-env').read_bytes(),
                    'opt/mesa-neo/libexec/mesa-neo-env':(ROOT/'neo/mesa-neo-env').read_bytes(),
                    'opt/mesa-neo/bin/mesa-neo-probe':(ROOT/'neo/mesa-neo-probe-wrapper').read_bytes()})
                for name2 in ('libexec/mesa-neo-abi-check','libexec/mesa-neo-loadcheck','libexec/mesa-neo-probe.real','share/vulkan/icd.d/asahi_icd.aarch64.json'):
                    files['opt/mesa-neo/'+name2]=b'fixture'
            self.package('neo-'+role,name,files)
            neo['packages'][role]=self.manifest['packages'].pop('neo-'+role)
        neo['m1n1_bin_sha256']=hashlib.sha256(b'own Neo binary').hexdigest()
        return neo
    def test_neo_matched_pair_selects_separate_packages(self):
        neo=self.neo_pair()
        script,_=self.assemble()
        for role in ('mesa','m1n1'):
            self.assertIn('NEO_'+role.upper()+'_PACKAGE="'+neo['packages'][role]['file'],script)
        self.assertIn('M1N1_PACKAGE="m1n1-aurora-',script)
        self.assertIn('NEO_M1N1_BIN_SHA="'+neo['m1n1_bin_sha256']+'"',script)
    def test_new_kernel_keeps_both_air_and_neo_pairs(self):
        neo=self.neo_pair()
        pair=dict(kernel=mod.NEO_FW_ROOT_PAIR['kernel'],m1n1=neo['source_commits']['m1n1'],m1n1_bin_sha256=neo['m1n1_bin_sha256'])
        for name in ('NEO_FW_ROOT_PAIR', 'NEO_PREVIOUS_RELEASE_FW_ROOT_PAIR',
                     'NEO_RELEASED_FW_ROOT_PAIR', 'NEO_PREVIOUS_FW_ROOT_PAIR',
                     'NEO_TUNNEL_LIVE_FW_ROOT_PAIR', 'NEO_NIC_LIVE_FW_ROOT_PAIR',
                     'NEO_DISPLAY_READY_LIVE_FW_ROOT_PAIR', 'NEO_RELEASE14_LIVE_FW_ROOT_PAIR',
                     'NEO_RELEASE15_LIVE_FW_ROOT_PAIR', 'NEO_RELEASE16_LIVE_FW_ROOT_PAIR',
                     'NEO_RELEASE18_LIVE_FW_ROOT_PAIR', 'NEO_RELEASE19_LIVE_FW_ROOT_PAIR'):
            changed=dict(pair,kernel=getattr(mod,name)['kernel'])
            patch=mock.patch.object(mod,name,changed);patch.start();self.addCleanup(patch.stop)
        self.manifest['legacy_gpu_boards']=['j613','j615']
        self.manifest['source_commits']['m1n1']='74ba6bea52d1f865d204bb3f8168705a148fd5c5'
        for kernel in mod.NEO_KERNELS:
            with self.subTest(kernel=kernel):
                self.manifest['source_commits']['kernel']=kernel
                self.assertIn('M3_PERSISTENT_BOARDS="j613 j615"',self.assemble()[0])
        self.manifest['source_commits']['kernel']='e'*40
        with self.assertRaisesRegex(ValueError,'kernel consumer'):self.assemble()

    def test_new_neo_kernel_requires_reserved_table_producer(self):
        neo=self.neo_pair()
        self.manifest['source_commits']['kernel']=mod.NEO_FW_ROOT_PAIR['kernel']
        pair=dict(kernel=mod.NEO_FW_ROOT_PAIR['kernel'],m1n1=neo['source_commits']['m1n1'],m1n1_bin_sha256=neo['m1n1_bin_sha256'])
        with mock.patch.object(mod,'NEO_FW_ROOT_PAIR',pair):
            self.assertIn('NEO_M1N1_BIN_SHA="'+neo['m1n1_bin_sha256']+'"',self.assemble()[0])
        for field,word in [('m1n1','source'),('m1n1_bin_sha256','binary')]:
            changed=dict(pair);changed[field]='e'*len(pair[field])
            with self.subTest(field=field), mock.patch.object(mod,'NEO_FW_ROOT_PAIR',changed):
                with self.assertRaisesRegex(ValueError,'firmware tables.*'+word):self.assemble()
        changed=dict(pair);changed['m1n1']=None
        with mock.patch.object(mod,'NEO_FW_ROOT_PAIR',changed):
            with self.assertRaisesRegex(ValueError,'recorded boot source'):self.assemble()

    def test_previous_reserved_table_kernel_keeps_exact_boot_binding(self):
        neo=self.neo_pair()
        self.manifest['source_commits']['kernel']=mod.NEO_PREVIOUS_FW_ROOT_PAIR['kernel']
        pair=dict(kernel=mod.NEO_PREVIOUS_FW_ROOT_PAIR['kernel'],
                  m1n1=neo['source_commits']['m1n1'],m1n1_bin_sha256=neo['m1n1_bin_sha256'])
        with mock.patch.object(mod,'NEO_PREVIOUS_FW_ROOT_PAIR',pair):
            self.assemble()
        for field,word in [('m1n1','source'),('m1n1_bin_sha256','binary')]:
            changed=dict(pair);changed[field]='e'*len(pair[field])
            with self.subTest(field=field), mock.patch.object(mod,'NEO_PREVIOUS_FW_ROOT_PAIR',changed):
                with self.assertRaisesRegex(ValueError,'firmware tables.*'+word):self.assemble()

    def test_live_and_previous_boot_sources_keep_separate_binary_bindings(self):
        old, new = mod.NEO_FW_ROOT_PAIR, mod.NEO_LIVE_FW_ROOT_PAIR
        self.assertEqual(old['kernel'], new['kernel'])
        for pair in (old, new):
            mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':pair['m1n1']},
                                         pair['m1n1_bin_sha256'])
            other = new if pair is old else old
            with self.assertRaisesRegex(ValueError, 'matched boot binary'):
                mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':pair['m1n1']},
                                             other['m1n1_bin_sha256'])
        for pair in (mod.NEO_PREVIOUS_RELEASE_FW_ROOT_PAIR,
                     mod.NEO_RELEASED_FW_ROOT_PAIR, mod.NEO_PREVIOUS_FW_ROOT_PAIR):
            with self.assertRaisesRegex(ValueError, 'matched boot source'):
                mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':new['m1n1']},
                                             new['m1n1_bin_sha256'])

    def test_current_and_released_neo_kernels_require_their_exact_boot_pair(self):
        for pair in (mod.NEO_FW_ROOT_PAIR, mod.NEO_PREVIOUS_RELEASE_FW_ROOT_PAIR,
                     mod.NEO_RELEASED_FW_ROOT_PAIR,
                     mod.NEO_PREVIOUS_FW_ROOT_PAIR):
            with self.subTest(kernel=pair['kernel']):
                mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':pair['m1n1']},
                                             pair['m1n1_bin_sha256'])
                with self.assertRaisesRegex(ValueError,'matched boot source'):
                    mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':'e'*40},
                                                 pair['m1n1_bin_sha256'])
                with self.assertRaisesRegex(ValueError,'matched boot binary'):
                    mod.check_neo_firmware_tables(pair['kernel'], {'m1n1':pair['m1n1']}, 'e'*64)

    def test_tunnel_kernel_keeps_native25_and_neo_boot_bindings(self):
        for pair, neo in ((mod.J615_TUNNEL_NATIVE25_PAIR, mod.NEO_TUNNEL_LIVE_FW_ROOT_PAIR),
                          (mod.J615_NIC_NATIVE25_PAIR, mod.NEO_NIC_LIVE_FW_ROOT_PAIR),
                          (mod.J615_DISPLAY_READY_NATIVE25_PAIR, mod.NEO_DISPLAY_READY_LIVE_FW_ROOT_PAIR),
                          (mod.J615_RELEASE14_NATIVE25_PAIR, mod.NEO_RELEASE14_LIVE_FW_ROOT_PAIR),
                          (mod.J615_RELEASE15_NATIVE25_PAIR, mod.NEO_RELEASE15_LIVE_FW_ROOT_PAIR)):
            self.assertEqual(pair['kernel'], neo['kernel'])
            self.assertIn(pair['kernel'], mod.NEO_KERNELS)
            members = {'opt/mesa-m3/share/mesa-m3/native25-boards': b'j613\nj615-experimental\n',
                       'opt/mesa-m3/libexec/mesa-m3-session-env': b'asahi,j615-25g83-experimental'}
            sources = {'kernel': pair['kernel'], 'm1n1': pair['m1n1']}
            with mock.patch.object(mod, 'optional_member', side_effect=lambda p,n: members[n]), \
                 mock.patch.object(mod, 'member', side_effect=lambda p,n: members[n]):
                mod.check_j615_native25(sources, pair['m1n1_bin_sha256'], Path('mesa'))
                with self.assertRaisesRegex(ValueError, 'matched J615 m1n1 source'):
                    mod.check_j615_native25(dict(sources, m1n1=neo['m1n1']),
                                           pair['m1n1_bin_sha256'], Path('mesa'))
                with self.assertRaisesRegex(ValueError, 'matched J615 m1n1 binary'):
                    mod.check_j615_native25(sources, neo['m1n1_bin_sha256'], Path('mesa'))
            mod.check_neo_firmware_tables(neo['kernel'], {'m1n1': neo['m1n1']},
                                         neo['m1n1_bin_sha256'])
            for wrong in (pair, mod.NEO_FW_ROOT_PAIR):
                with self.assertRaisesRegex(ValueError, 'matched boot source'):
                    mod.check_neo_firmware_tables(neo['kernel'], {'m1n1': wrong['m1n1']},
                                                 wrong['m1n1_bin_sha256'])
            with self.assertRaisesRegex(ValueError, 'matched boot binary'):
                mod.check_neo_firmware_tables(neo['kernel'], {'m1n1': neo['m1n1']},
                                             mod.NEO_FW_ROOT_PAIR['m1n1_bin_sha256'])
    def test_new_kernel_keeps_exact_native25_boot_binding(self):
        pair=mod.J615_NATIVE25_PAIR
        self.assertEqual(pair['kernel'],mod.NEO_FW_ROOT_PAIR['kernel'])
        sources={'kernel':pair['kernel'],'m1n1':pair['m1n1']}
        members={'opt/mesa-m3/share/mesa-m3/native25-boards':b'j613\nj615-experimental\n',
                 'opt/mesa-m3/libexec/mesa-m3-session-env':b'asahi,j615-25g83-experimental'}
        with mock.patch.object(mod,'optional_member',side_effect=lambda p,n:members[n]), \
             mock.patch.object(mod,'member',side_effect=lambda p,n:members[n]):
            mod.check_j615_native25(sources,pair['m1n1_bin_sha256'],Path('mesa'))
            for field,why in [('kernel','matched J615 kernel'),('m1n1','matched J615 m1n1 source')]:
                with self.subTest(field=field), self.assertRaisesRegex(ValueError,why):
                    mod.check_j615_native25(dict(sources,**{field:'e'*40}),pair['m1n1_bin_sha256'],Path('mesa'))
            with self.assertRaisesRegex(ValueError,'matched J615 m1n1 binary'):
                mod.check_j615_native25(sources,'e'*64,Path('mesa'))

    def test_released_native25_kernel_keeps_exact_boot_binding(self):
        pair=mod.J615_PREVIOUS_NATIVE25_PAIR
        sources={'kernel':pair['kernel'],'m1n1':pair['m1n1']}
        members={'opt/mesa-m3/share/mesa-m3/native25-boards':b'j613\nj615-experimental\n',
                 'opt/mesa-m3/libexec/mesa-m3-session-env':b'asahi,j615-25g83-experimental'}
        with mock.patch.object(mod,'optional_member',side_effect=lambda p,n:members[n]), \
             mock.patch.object(mod,'member',side_effect=lambda p,n:members[n]):
            mod.check_j615_native25(sources,pair['m1n1_bin_sha256'],Path('mesa'))
            with self.assertRaisesRegex(ValueError,'matched J615 m1n1 source'):
                mod.check_j615_native25(dict(sources,m1n1='e'*40),pair['m1n1_bin_sha256'],Path('mesa'))
            with self.assertRaisesRegex(ValueError,'matched J615 m1n1 binary'):
                mod.check_j615_native25(sources,'e'*64,Path('mesa'))

    def test_generic_esp_stage2_requires_stage2_build(self):
        neo=self.neo_pair()
        tree=self.root/'neo-m1n1'
        files={str(p.relative_to(tree)):p.read_bytes() for p in tree.rglob('*') if p.is_file()}
        key='usr/share/m1n1-neo/build-config.h'
        files[key]=files[key].replace(b'J700_ESP_STAGE2',b'ESP_STAGE2')
        self.package('neo-m1n1','m1n1-neo',files)
        neo['packages']['m1n1']=self.manifest['packages'].pop('neo-m1n1')
        self.assemble()
        files[key]=files[key].replace(b'#define ESP_STAGE2\n',b'')
        self.package('neo-m1n1','m1n1-neo',files)
        neo['packages']['m1n1']=self.manifest['packages'].pop('neo-m1n1')
        with self.assertRaisesRegex(ValueError,'Neo ESP configuration'):self.assemble()

    def test_neo_helper_bytes_are_bound_after_valid_package_hash(self):
        neo=self.neo_pair()
        tree=self.root/'neo-mesa'
        original={str(p.relative_to(tree)):p.read_bytes() for p in tree.rglob('*') if p.is_file()}
        for name in ('libexec/mesa-neo-env','libexec/mesa-neo-session-env','bin/mesa-neo-probe'):
            with self.subTest(name=name):
                files=dict(original)
                files['opt/mesa-neo/'+name]=b'#!/bin/sh\nexit 0\n'
                self.package('neo-mesa','mesa-neo',files)
                neo['packages']['mesa']=self.manifest['packages'].pop('neo-mesa')
                with self.assertRaisesRegex(ValueError,'package helper differs'):self.assemble()

    def test_neo_pair_rejects_incomplete_or_mismatched_artifacts(self):
        neo=self.neo_pair()
        original=neo['packages']['mesa']['sha256']
        neo['packages']['mesa']['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'Neo package hash'): self.assemble()
        neo['packages']['mesa']['sha256']=original
        del neo['packages']['m1n1']
        with self.assertRaisesRegex(ValueError,'both matched'): self.assemble()
    def test_neo_pair_rejects_wrong_kernel_and_boot_binary(self):
        neo=self.neo_pair()
        self.manifest['source_commits']['kernel']='a'*40
        with self.assertRaisesRegex(ValueError,'T8140 kernel'): self.assemble()
        self.manifest['source_commits']['kernel']=mod.NEO_KERNEL
        neo['m1n1_bin_sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'Neo bootloader hash'): self.assemble()

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
        files={'.PKGINFO':b'pkgname = linux-aurora\npkgver = candidate-1\narch = aarch64\ndepend = m1n1>=1.6.1\n'}
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
    def test_native25_default_is_j613_and_otherwise_unchanged(self):
        script,ident=self.assemble()
        template=(ROOT/'install-aurora-sep.sh').read_text()
        # Omitted, the two new fields keep the template's own values.
        for line in ('M3_NATIVE25_BOARDS="j613"','M3_STAGE1_25_J615_VERSIONS=""'):
            self.assertIn('\n'+line+'\n',template);self.assertIn('\n'+line+'\n',script)
        self.manifest['native25_boards']=['j613'];self.manifest['stage1_25_j615_versions']=['ignored']
        explicit,other=self.assemble()
        self.assertEqual(explicit.replace(other,ident),script)
    def kernel_files(self,dtbs):
        files={'.PKGINFO':b'pkgname = linux-aurora\npkgver = candidate-1\narch = aarch64\ndepend = m1n1>=1.6.1\n'}
        files.update(dtbs)
        self.package('kernel','linux-aurora',files)
    J615_KERNEL='c'*40
    J615_M1N1='d'*40
    def j615_pair(self,**over):
        pair=dict(kernel=self.J615_KERNEL,m1n1=self.J615_M1N1,m1n1_bin_sha256=self.manifest['m1n1_bin_sha256'],**over)
        patch=mock.patch.object(mod,'J615_NATIVE25_PAIR',pair);patch.start();self.addCleanup(patch.stop)
    def j615_release(self):
        self.manifest['native25_boards']=['j613','j615']
        self.manifest['stage1_25_j615_versions']=['v1.6.1-m3air25.stage1']
        self.manifest['source_commits'].update(kernel=self.J615_KERNEL,m1n1=self.J615_M1N1)
        self.binary+=b'asahi,j615-25g83-experimental\0'
        self.manifest['m1n1_bin_sha256']=hashlib.sha256(self.binary).hexdigest()
        self.package('m1n1','m1n1-aurora',{'.PKGINFO':b'pkgname = m1n1-aurora\npkgver = candidate-1\narch = aarch64\n',
                                           'usr/lib/asahi-boot/m1n1.bin':self.binary})
        self.mesa_member('opt/mesa-m3/libexec/mesa-m3-session-env',
                         b'profile=j613-25g83-hal200\n/proc/device-tree/chosen/asahi,j615-25g83-experimental\n')
        self.mesa_member('opt/mesa-m3/share/mesa-m3/native25-boards',b'j613\nj615-experimental\n')
        self.j615_pair()
        profile=b'apple,j613-25g83-profile\0apple,firmware-compat\0'
        self.kernel_files({'usr/lib/modules/test/dtbs/apple/t8122-j613-25g83.dtb':profile,
                          'usr/lib/modules/test/dtbs/apple/t8122-j615-25g83.dtb':b'apple,j615\0'+profile})
    def test_j615_native25_release_is_filled_in(self):
        self.j615_release()
        script=self.assemble()[0]
        self.assertIn('M3_NATIVE25_BOARDS="j613 j615"\n',script)
        self.assertIn('M3_STAGE1_25_J615_VERSIONS="v1.6.1-m3air25.stage1"\n',script)
        self.assertIn('M3_PERSISTENT_BOARDS="j613"\n',script)
    def test_j615_native25_needs_dtb_switch_and_stage1(self):
        for case,why in [('no dtb','one separate J61525'),('two dtbs','one separate J61525'),('no j615 compatible','J61525 DTB profile'),
                         ('no profile marker','J61525 DTB profile'),('no switch','J615 25G83 switch'),
                         ('no stage1','stage1_25_j615_versions'),('bad stage1','stage1_25_j615_versions')]:
            with self.subTest(case=case):
                self.setUp();self.j615_release()
                profile=b'apple,j613-25g83-profile\0apple,firmware-compat\0'
                j613={'usr/lib/modules/test/dtbs/apple/t8122-j613-25g83.dtb':profile}
                if case=='no dtb':self.kernel_files(j613)
                elif case=='two dtbs':self.kernel_files({**j613,'usr/lib/modules/test/dtbs/apple/t8122-j615-25g83.dtb':b'apple,j615\0'+profile,
                                                        'usr/lib/modules/test/dtbs/t8122-j615-25g83.dtb':b'apple,j615\0'+profile})
                elif case=='no j615 compatible':self.kernel_files({**j613,'usr/lib/modules/test/dtbs/apple/t8122-j615-25g83.dtb':profile})
                elif case=='no profile marker':self.kernel_files({**j613,'usr/lib/modules/test/dtbs/apple/t8122-j615-25g83.dtb':b'apple,j615\0apple,firmware-compat\0'})
                elif case=='no switch':
                    self.binary=self.binary.replace(b'asahi,j615-25g83-experimental\0',b'')
                    self.manifest['m1n1_bin_sha256']=hashlib.sha256(self.binary).hexdigest()
                    self.package('m1n1','m1n1-aurora',{'.PKGINFO':b'pkgname = m1n1-aurora\npkgver = candidate-1\narch = aarch64\n',
                                                       'usr/lib/asahi-boot/m1n1.bin':self.binary})
                    self.j615_pair()
                elif case=='no stage1':self.manifest['stage1_25_j615_versions']=[]
                else:self.manifest['stage1_25_j615_versions']=['v1.6.1 m3air25']
                with self.assertRaisesRegex(ValueError,why):self.assemble()
    def test_native25_board_list_is_checked(self):
        for bad in [['j615'],['j613','j613'],['j613','j504'],[],'j613 j615']:
            with self.subTest(bad=bad):
                self.manifest['native25_boards']=bad
                with self.assertRaisesRegex(ValueError,'native25'):self.assemble()
    def test_j615_native25_needs_the_recorded_boot_kernel_pair(self):
        self.j615_release()
        self.assemble()
        with mock.patch.object(mod,'J615_NATIVE25_PAIR',None):
            with self.assertRaisesRegex(ValueError,'boot/kernel pair'):self.assemble()
        for field,why in [('kernel','matched J615 kernel'),('m1n1','matched J615 m1n1 source'),
                          ('m1n1_bin_sha256','matched J615 m1n1 binary')]:
            with self.subTest(field=field):
                pair=dict(mod.J615_NATIVE25_PAIR);pair[field]='e'*len(pair[field])
                with mock.patch.object(mod,'J615_NATIVE25_PAIR',pair):
                    with self.assertRaisesRegex(ValueError,why):self.assemble()
        pair=dict(mod.J615_NATIVE25_PAIR);pair['stage1']='x'
        with mock.patch.object(mod,'J615_NATIVE25_PAIR',pair):
            with self.assertRaisesRegex(ValueError,'boot/kernel pair'):self.assemble()
    def test_j615_native25_needs_a_mesa_that_admits_it(self):
        for case,name,data,why in [
                ('no capability','opt/mesa-m3/share/mesa-m3/native25-boards',None,'session capability'),
                ('j613 only','opt/mesa-m3/share/mesa-m3/native25-boards',b'j613\n','session capability'),
                ('substring','opt/mesa-m3/share/mesa-m3/native25-boards',b'j613\nj615-experimental-no\n','session capability'),
                ('old hook','opt/mesa-m3/libexec/mesa-m3-session-env',b'profile=j613-25g83-hal200\n','J615 25G83 admission')]:
            with self.subTest(case=case):
                self.setUp();self.j615_release()
                self.mesa_member(name,data)
                with self.assertRaisesRegex(ValueError,why):self.assemble()
    def test_j613_release_needs_no_j615_capability(self):
        # The released (pkgrel 3) Mesa has neither the list nor the J615 admission: J613 assembly is unchanged.
        with mock.patch.object(mod,'J615_NATIVE25_PAIR',None):
            self.assertIn('M3_NATIVE25_BOARDS="j613"\n',self.assemble()[0])
        self.mesa_member('opt/mesa-m3/share/mesa-m3/native25-boards',b'j613\nj615-experimental\n')
        self.assertIn('M3_NATIVE25_BOARDS="j613"\n',self.assemble()[0])
