"""Exercise frozen dependency admission with real pacman and private databases."""
import hashlib
import io
import os
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parents[1] / 'install-aurora-sep.sh'
EMBEDDED = INSTALLER.read_text().split("<<'FROZEN_DEPENDENCIES_PY'\n", 1)[1].split('\nFROZEN_DEPENDENCIES_PY', 1)[0] + '\n'
_HELPER_DIR = tempfile.TemporaryDirectory(prefix='aurora-frozen-helper-')
HELPER = Path(_HELPER_DIR.name) / 'frozen-dependencies.py'
HELPER.write_text(EMBEDDED)

@unittest.skipUnless(all(shutil.which(p) for p in ('pacman', 'repo-add', 'fakeroot', 'bsdtar')), 'pacman fixture tools required')
class FrozenDependencies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='aurora-alpm-')
        self.p = Path(self.tmp.name)
        for d in ('db/local', 'db/sync', 'root', 'cache', 'work', 'repo'):
            (self.p / d).mkdir(parents=True)
        (self.p/'db/local/ALPM_DB_VERSION').write_text('9\n')
        self.conf = self.p/'pacman.conf'
        self.conf.write_text(f'[options]\nRootDir = {self.p}/root\nDBPath = {self.p}/db\nCacheDir = {self.p}/cache\nLogFile = {self.p}/pacman.log\nArchitecture = auto\nIgnorePkg = *\nSigLevel = Never\nLocalFileSigLevel = Never\n[fixture]\nServer = file://{self.p}/repo\n')
        self.repos = []

    def tearDown(self):
        self.tmp.cleanup()

    def pkg(self, name, version='1-1', depends=(), provides=(), conflicts=(), replaces=(), local=False):
        path = self.p/('work' if local else 'repo')/f'{name}-{version}-any.pkg.tar.gz'
        text = f'pkgname = {name}\npkgver = {version}\npkgdesc = fixture\nurl = https://example.com\nbuilddate = 1\npackager = fixture\nsize = 0\narch = any\n'
        for key, values in (('depend', depends), ('provides', provides), ('conflict', conflicts), ('replaces', replaces)):
            text += ''.join(f'{key} = {v}\n' for v in values)
        with tarfile.open(path, 'w:gz') as archive:
            data = text.encode(); entry = tarfile.TarInfo('.PKGINFO'); entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
        if not local:
            self.repos.append(path)
        return path

    def installed(self, name, version='1-1', provides=(), conflicts=()):
        d = self.p/'db/local'/f'{name}-{version}'; d.mkdir()
        fields = {'NAME':[name], 'VERSION':[version], 'ARCH':['any'], 'DESC':['fixture'], 'SIZE':['0'], 'REASON':['0'], 'VALIDATION':['none'], 'PROVIDES':list(provides), 'CONFLICTS':list(conflicts)}
        (d/'desc').write_text(''.join('%'+k+'%\n'+'\n'.join(v)+'\n\n' for k,v in fields.items() if v))
        (d/'files').write_text('%FILES%\n\n')

    def db(self):
        if not self.repos:
            self.pkg('unrelated')
        subprocess.run(['repo-add', '--include-sigs', str(self.p/'db/sync/fixture.db.tar.gz'), *map(str,self.repos)], check=True, capture_output=True)
        path=self.p/'db/sync/fixture.db'; path.unlink()
        path.write_bytes((self.p/'db/sync/fixture.db.tar.gz').read_bytes())

    def call(self, candidate, requires=('fprintd',), extra=(), okay=True, helper=HELPER):
        self.db()
        before=self.conf.read_bytes()
        db_before={str(p.relative_to(self.p/'db/local')):p.read_bytes() for p in (self.p/'db/local').rglob('*') if p.is_file()}
        command=['fakeroot','python3',str(helper),'--config',str(self.conf),'--work',str(self.p/'work'),'--candidate',str(candidate)]
        for requirement in requires: command+=['--require',requirement]
        result=subprocess.run(command+list(extra),text=True,capture_output=True)
        self.assertEqual(result.returncode, 0 if okay else 1, result.stderr+result.stdout)
        self.assertEqual(before,self.conf.read_bytes())
        self.assertEqual(db_before,{str(p.relative_to(self.p/'db/local')):p.read_bytes() for p in (self.p/'db/local').rglob('*') if p.is_file()})
        if okay:
            receipt=json.loads(result.stdout)
            config=Path(receipt['transaction_config']).read_text()
            self.assertIn('IgnorePkg = *',config)
            self.assertNotIn('[fixture]',config)
            resolved=subprocess.run(['pacman-conf','--config',str(self.conf),'-v','LocalFileSigLevel'],capture_output=True,text=True,check=True).stdout
            for line in resolved.splitlines(): self.assertIn(line,config)
            return receipt
        self.assertFalse((self.p/'work/dependency-plan.json').exists())
        self.assertEqual(sorted(p.name for p in (self.p/'work').glob('*.pkg.tar.gz')),[candidate.name])
        return result

    def add_sync_record(self, name, raw):
        original_db = self.db
        def augmented():
            original_db()
            path = self.p/'db/sync/fixture.db'
            with tarfile.open(path) as db:
                records = [(entry, db.extractfile(entry).read() if entry.isfile() else None)
                           for entry in db]
            with tarfile.open(path, 'w') as db:
                for entry, data in records:
                    db.addfile(entry, io.BytesIO(data) if data is not None else None)
                entry = tarfile.TarInfo(name); entry.size = len(raw)
                db.addfile(entry, io.BytesIO(raw))
        self.db = augmented

    def test_all_zero_sync_record_does_not_block_missing_dependency(self):
        self.pkg('fprintd')
        self.add_sync_record('findnewest-0.3-4/desc', b'\0' * 1271)
        receipt = self.call(self.pkg('aurora-touchid', local=True))
        self.assertEqual([p['name'] for p in receipt['dependencies']], ['fprintd'])

    def test_malformed_nonzero_sync_record_still_refused_with_location(self):
        self.pkg('fprintd')
        self.add_sync_record('broken-1-1/desc', b'\0bad')
        result = self.call(self.pkg('aurora-touchid', local=True), okay=False)
        self.assertIn('fixture.db:broken-1-1/desc', result.stderr)

    def test_zero_sync_record_cannot_supply_missing_provider(self):
        self.add_sync_record('fprintd-1-1/desc', b'\0' * 1271)
        result = self.call(self.pkg('aurora-touchid', local=True), okay=False)
        self.assertIn('No unique missing provider for fprintd', result.stderr)

    def test_zero_local_record_still_refused(self):
        self.installed('fprintd')
        (self.p/'db/local/fprintd-1-1/desc').write_bytes(b'\0' * 1271)
        self.call(self.pkg('aurora-touchid', local=True), okay=False)

    def test_missing_transitive_under_full_freeze(self):
        self.pkg('libgusb'); self.pkg('fprintd',depends=['libgusb']); self.pkg('touch-helper')
        candidate=self.pkg('aurora-touchid',depends=['touch-helper'],local=True)
        receipt=self.call(candidate)
        self.assertEqual({p['name'] for p in receipt['dependencies']},{'fprintd','libgusb','touch-helper'})
        for p in receipt['dependencies']:
            self.assertEqual(p['sha256'],hashlib.sha256(Path(p['file']).read_bytes()).hexdigest())

    def candidate_supplied_dependency_fixture(self):
        # The repository carries its own libfprint, but this release ships a newer one.
        self.pkg('libfprint', '1.94.9-1', provides=['libfprint-2.so=2-64'])
        self.pkg('libgusb')
        self.pkg('fprintd', depends=['libfprint', 'libfprint-2.so=2-64', 'libgusb'])
        return self.pkg('libfprint', '1.94.100-1.1', provides=['libfprint-2.so=2-64'], local=True)

    def test_held_image_missing_repo_package_uses_candidate_dependency(self):
        candidate = self.candidate_supplied_dependency_fixture()
        receipt = self.call(candidate)
        self.assertEqual({p['name'] for p in receipt['dependencies']}, {'fprintd', 'libgusb'})
        prepared = subprocess.run(['pacman', '--config', receipt['transaction_config'], '-Up', '--noconfirm',
                                   '--print-format', '%n %v', str(candidate),
                                   *[p['file'] for p in receipt['dependencies']]], capture_output=True, text=True)
        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        self.assertEqual(sorted(prepared.stdout.splitlines()),
                         ['fprintd 1-1', 'libfprint 1.94.100-1.1', 'libgusb 1-1'])

    def test_repository_resolver_still_refuses_incomplete_closure(self):
        candidate = self.candidate_supplied_dependency_fixture()
        source = HELPER.read_text()
        resolved = 'selected = resolve(args.require, candidates, remaining, available)\n'
        self.assertIn(resolved, source)
        incomplete = self.p/'incomplete-helper.py'
        incomplete.write_text(source.replace(resolved, resolved +
                                             '    selected = [p for p in selected if p["name"] != "libgusb"]\n'))
        result = self.call(candidate, okay=False, helper=incomplete)
        self.assertIn(' -Sp ', result.stderr)
        unmet = [line for line in result.stderr.splitlines() if 'unable to satisfy' in line]
        self.assertEqual(unmet, [":: unable to satisfy dependency 'libgusb' required by fprintd"])

    def test_satisfied_root_and_provider_are_excluded(self):
        self.installed('fprintd'); self.installed('provider',provides=['virtual=3'])
        candidate=self.pkg('aurora-touchid',depends=['virtual>=2'],local=True)
        receipt=self.call(candidate)
        self.assertEqual(receipt['dependencies'],[])

    def test_too_old_installed_dependency_is_refused(self):
        self.installed('libgusb','1-1');self.pkg('libgusb','2-1');self.pkg('fprintd',depends=['libgusb>=2'])
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_installed_root_is_not_upgraded(self):
        self.installed('fprintd','1-1');self.pkg('fprintd','2-1')
        receipt=self.call(self.pkg('aurora-touchid',local=True)); self.assertEqual(receipt['dependencies'],[])

    def test_too_old_provider_refused(self):
        self.installed('old-provider',provides=['virtual=1']);self.pkg('new-provider',provides=['virtual=2']);self.pkg('fprintd',depends=['virtual>=2'])
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_kernel_accepts_retained_aurora12_provider_without_bootloader_upgrade(self):
        self.installed('m1n1-aurora', '1.6.1.aurora12-1', provides=['m1n1=1.6.1.aurora12'])
        self.pkg('m1n1-aurora', '1.6.1.aurora17-5', provides=['m1n1=1.6.1.aurora17'])
        candidate = self.pkg('linux-aurora', depends=['m1n1>=1.6.1'], local=True)
        receipt = self.call(candidate, requires=())
        self.assertEqual(receipt['dependencies'], [])
        prepared = subprocess.run(['pacman', '--config', receipt['transaction_config'],
                                   '-Up', '--noconfirm', str(candidate)], capture_output=True, text=True)
        self.assertEqual(prepared.returncode, 0, prepared.stderr+prepared.stdout)

    def test_global_gpu_bootloader_minimum_blocks_retained_aurora12(self):
        self.installed('m1n1-aurora', '1.6.1.aurora12-1', provides=['m1n1=1.6.1.aurora12'])
        self.pkg('m1n1-aurora', '1.6.1.aurora17-5', provides=['m1n1=1.6.1.aurora17'])
        result = self.call(self.pkg('linux-aurora', depends=['m1n1>=1.6.1.aurora15'], local=True),
                           requires=(), okay=False)
        self.assertIn('provider is too old for m1n1>=1.6.1.aurora15', result.stderr)
        self.assertIn('Report this packaging error', result.stderr)
        self.assertNotIn('run the full updater', result.stderr)

    def test_missing_dependency_replacement_refused(self):
        self.installed('unrelated');self.pkg('fprintd',replaces=['unrelated'])
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_candidate_other_installed_conflict_refused(self):
        self.installed('desktop');self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',conflicts=['desktop'],local=True),okay=False)

    def test_reverse_installed_conflict_refused(self):
        self.installed('desktop',conflicts=['aurora-touchid']);self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_candidate_satisfies_own_dependency(self):
        self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',depends=['virtual>=2'],provides=['virtual=2'],local=True))

    def test_no_repo_commit_refuses_new_missing_dependency(self):
        self.pkg('fprintd'); receipt=self.call(self.pkg('aurora-touchid',local=True))
        late=self.pkg('late',depends=['unavailable'],local=True)
        result=subprocess.run(['pacman','--config',receipt['transaction_config'],'-Up','--noconfirm',str(late)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)

    def test_ask1_does_not_unhold_implicit_dependencies(self):
        self.pkg('dep');self.pkg('fprintd',depends=['dep']);self.db()
        for ask in ([],['--ask','1']):
            result=subprocess.run(['pacman','--config',str(self.conf),'-Sp','--noconfirm',*ask,'fprintd'],capture_output=True,text=True)
            self.assertEqual(result.returncode,1)
        result=subprocess.run(['pacman','--config',str(self.conf),'-Sp','--noconfirm','fprintd','dep'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)


    def test_reverse_dependency_conflict_refused(self):
        self.installed('desktop',conflicts=['fprintd']); self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_unsigned_repo_dependency_required_signature_refused(self):
        self.conf.write_text(self.conf.read_text().replace('SigLevel = Never', 'SigLevel = PackageRequired DatabaseNever'))
        self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',local=True),okay=False)

    def test_verify_plan_detects_database_drift(self):
        self.pkg('fprintd'); self.call(self.pkg('aurora-touchid',local=True))
        self.installed('fprintd','2-1')
        r=subprocess.run(['python3',str(HELPER),'--verify-plan',str(self.p/'work/dependency-plan.json')],capture_output=True,text=True)
        self.assertEqual(r.returncode,1); self.assertIn('database changed',r.stderr+r.stdout)

    def test_verify_plan_detects_artifact_drift(self):
        self.pkg('fprintd'); receipt=self.call(self.pkg('aurora-touchid',local=True))
        Path(receipt['dependencies'][0]['file']).write_bytes(b'changed')
        r=subprocess.run(['python3',str(HELPER),'--verify-plan',str(self.p/'work/dependency-plan.json')],capture_output=True,text=True)
        self.assertEqual(r.returncode,1); self.assertIn('input changed',r.stderr)

    def test_verify_plan_accepts_exact_inputs(self):
        self.pkg('fprintd'); receipt=self.call(self.pkg('aurora-touchid',local=True))
        r=subprocess.run(['python3',str(HELPER),'--verify-plan',str(self.p/'work/dependency-plan.json')],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        hook=Path(receipt['hook']).read_text()
        self.assertIn('When = PreTransaction',hook); self.assertIn('AbortOnFail',hook)
        self.assertIn('HookDir = '+str(Path(receipt['hook']).parent),Path(receipt['transaction_config']).read_text())

    def test_ask1_download_skips_held_explicit_targets(self):
        self.pkg('dep'); self.pkg('fprintd',depends=['dep']); self.db()
        for extra, names in ((['--ask','1'],[]), ([],['dep','fprintd'])):
            r=subprocess.run(['fakeroot','pacman','--config',str(self.conf),'-Sw','--noconfirm',*extra,'fprintd','dep'],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(sorted(p.name.split('-1-1-')[0] for p in (self.p/'cache').glob('*.pkg.tar.gz')), names)


    def transaction_fixture(self):
        # Empty archives and a private DB isolate package writes. All system hooks are shadowed.
        hooks=self.p/'hooks'; hooks.mkdir()
        for directory in ('/usr/share/libalpm/hooks','/etc/pacman.d/hooks'):
            for hook in Path(directory).glob('*.hook'):
                target=hooks/hook.name
                if not target.is_symlink(): target.symlink_to('/dev/null')
        self.conf.write_text(self.conf.read_text().replace(f'RootDir = {self.p}/root','RootDir = /').replace('[fixture]',f'HookDir = {hooks}\n[fixture]'))
        # libalpm always invokes ldconfig after commit, even for empty archives. Mock that service only.
        source=self.p/'ldconfig.c'
        source.write_text('#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <string.h>\n#include <unistd.h>\nint execv(const char *p,char *const a[]){int(*real)(const char*,char*const[])=dlsym(RTLD_NEXT,"execv");if(strstr(p,"/ldconfig")){char *args[]={"/usr/bin/true",0};return real(args[0],args);}return real(p,a);}\nint execve(const char *p,char *const a[],char *const e[]){int(*real)(const char*,char*const[],char*const[])=dlsym(RTLD_NEXT,"execve");if(strstr(p,"/ldconfig")){char *args[]={"/usr/bin/true",0};return real(args[0],args,e);}return real(p,a,e); }\n')
        shim=self.p/'ldconfig.so'
        subprocess.run(['cc','-shared','-fPIC','-o',str(shim),str(source),'-ldl'],check=True,capture_output=True)
        return {**os.environ,'LD_PRELOAD':str(shim)}

    def test_real_transaction_hook_accepts_exact_plan(self):
        env=self.transaction_fixture(); self.pkg('fprintd'); candidate=self.pkg('aurora-touchid',local=True)
        receipt=self.call(candidate)
        r=subprocess.run(['fakeroot','pacman','--config',receipt['transaction_config'],'-U','--noconfirm',str(candidate),*[p['file'] for p in receipt['dependencies']]],env=env,capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr+r.stdout)
        self.assertIn('00-aurora-frozen-dependencies.hook',r.stdout)
        self.assertEqual({p.name for p in (self.p/'db/local').iterdir()},{'ALPM_DB_VERSION','aurora-touchid-1-1','fprintd-1-1'})
        self.assertNotIn('command failed',r.stderr)

    def test_real_transaction_hook_refuses_database_drift(self):
        env=self.transaction_fixture(); self.pkg('fprintd'); candidate=self.pkg('aurora-touchid',local=True)
        receipt=self.call(candidate); self.installed('fprintd','2-1')
        r=subprocess.run(['fakeroot','pacman','--config',receipt['transaction_config'],'-U','--noconfirm',str(candidate),*[p['file'] for p in receipt['dependencies']]],env=env,capture_output=True,text=True)
        self.assertEqual(r.returncode,1,r.stderr+r.stdout); self.assertIn('database changed',r.stderr+r.stdout)
        self.assertTrue((self.p/'db/local/fprintd-2-1').exists()); self.assertFalse((self.p/'db/local/aurora-touchid-1-1').exists())
        # The original unguarded command prepares the downgrade rather than refusing it.
        plain=Path(receipt['transaction_config']); stripped=self.p/'unguarded.conf'
        stripped.write_text('\n'.join(line for line in plain.read_text().splitlines() if not line.startswith('HookDir = '))+ '\n')
        old=subprocess.run(['pacman','--config',str(stripped),'-Up','--noconfirm',str(candidate),*[p['file'] for p in receipt['dependencies']]],capture_output=True,text=True)
        self.assertEqual(old.returncode,0,old.stderr)

    def test_real_transaction_hook_refuses_receipt_mutation(self):
        env=self.transaction_fixture(); self.pkg('fprintd'); candidate=self.pkg('aurora-touchid',local=True)
        receipt=self.call(candidate); path=self.p/'work/dependency-plan.json'
        path.write_text(path.read_text()+' ')
        r=subprocess.run(['fakeroot','pacman','--config',receipt['transaction_config'],'-U','--noconfirm',str(candidate),*[p['file'] for p in receipt['dependencies']]],env=env,capture_output=True,text=True)
        self.assertEqual(r.returncode,1,r.stderr+r.stdout); self.assertIn('receipt changed',r.stderr+r.stdout)
        self.assertFalse((self.p/'db/local/aurora-touchid-1-1').exists())

    @unittest.skipUnless(shutil.which('gpg'),'gpg fixture tool required')
    def test_signed_dependencies_keep_required_local_signatures(self):
        keyring=self.p/'keyring'; keyring.mkdir(mode=0o700)
        subprocess.run(['gpg','--homedir',str(keyring),'--batch','--passphrase','','--quick-generate-key','Aurora fixture <fixture@example.invalid>','ed25519','sign','0'],check=True,capture_output=True)
        dep=self.pkg('fprintd'); candidate=self.pkg('aurora-touchid',local=True)
        for path in (dep,candidate):
            subprocess.run(['gpg','--homedir',str(keyring),'--batch','--yes','--detach-sign',str(path)],check=True,capture_output=True)
        self.conf.write_text(self.conf.read_text().replace('SigLevel = Never','SigLevel = PackageRequired TrustAll DatabaseNever').replace('LocalFileSigLevel = Never','LocalFileSigLevel = PackageRequired TrustAll').replace('[fixture]',f'GPGDir = {keyring}\n[fixture]'))
        receipt=self.call(candidate)
        signature=receipt['dependencies'][0]['signature_file']
        self.assertTrue(Path(signature).exists())
        r=subprocess.run(['pacman','--config',receipt['transaction_config'],'-Up','--noconfirm',str(candidate),*[p['file'] for p in receipt['dependencies']]],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)


    def test_removed_provider_cannot_satisfy_dependency(self):
        self.installed('old-boot',provides=['virtual=2']); self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid',depends=['virtual>=2'],conflicts=['old-boot'],local=True),extra=['--allow-remove','old-boot'],okay=False)

    def test_updated_candidate_cannot_reuse_old_provides(self):
        self.installed('aurora-touchid',provides=['virtual=2']); self.pkg('fprintd')
        self.call(self.pkg('aurora-touchid','2-1',depends=['virtual>=2'],local=True),okay=False)

if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(shutil.which('sudo') and os.geteuid() != 0,
                     'unprivileged sudo fixture required')
class RootPrivateTransaction(unittest.TestCase):
    def test_both_checks_accept_root_private_config_and_refuse_missing(self):
        if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode:
            self.skipTest('passwordless sudo fixture required')
        with tempfile.TemporaryDirectory(prefix='aurora-root-config-') as tmp:
            outer = Path(tmp); outer.chmod(0o755)
            private = outer/'private'; config = private/'transaction.conf'
            subprocess.run(['sudo', '-n', 'python3', '-c',
                'import pathlib,sys; p=pathlib.Path(sys.argv[1]); p.mkdir(mode=0o700); (p/"transaction.conf").write_text("[options]\\nIgnorePkg = *\\n")',
                str(private)], check=True)
            try:
                self.assertFalse(config.is_file())
                checks = [line.strip() for line in INSTALLER.read_text().splitlines()
                          if '|| die "missing admitted' in line and 'transaction' in line]
                self.assertEqual(len(checks), 2)
                for check in checks:
                    script = 'sudo="sudo -n"; FROZEN_TRANSACTION_CONFIG=$1; die() { exit 23; }; '+check
                    for path, code in ((config, 0), (private/'missing.conf', 23)):
                        result = subprocess.run(['bash', '-c', script, 'fixture', str(path)],
                                                capture_output=True, text=True)
                        self.assertEqual(result.returncode, code, result.stderr)
                    old = check.replace('$sudo test -f "$FROZEN_TRANSACTION_CONFIG"',
                                        '[[ -f $FROZEN_TRANSACTION_CONFIG ]]')
                    result = subprocess.run(['bash', '-c',
                        'FROZEN_TRANSACTION_CONFIG=$1; die() { exit 23; }; '+old,
                        'fixture', str(config)], capture_output=True)
                    self.assertEqual(result.returncode, 23)
            finally:
                subprocess.run(['sudo', '-n', 'rm', '-rf', '--', str(private)], check=True)
