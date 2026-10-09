"""Actual embedded checker and owned-file lifecycle on synthetic desktop files."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import types
import unittest
import test_m3_handoff
import test_m3_persistent

SOURCE = test_m3_handoff.INSTALLER.read_text()
CODE = SOURCE.split("cat <<'AURORA_M3_GPU_CHECK_PY'\n", 1)[1].split('\nAURORA_M3_GPU_CHECK_PY', 1)[0]
CHECK = types.ModuleType('gpu_check')
exec(compile(CODE, 'aurora-m3-gpu-check', 'exec'), CHECK.__dict__)

class Runtime(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.uid = os.geteuid() or 1000
        self.boot = '12345678-1234-1234-1234-123456789abc'
        self.env = {'MESA_M3_HOOK_RUN_ID':'hook1', 'MESA_M3_SESSION':'/opt/mesa-m3','XDG_SESSION_ID':'2'}
        self.state = dict(schema='aurora.mesa-m3-session/2',decision='active',reason='active',uid=str(self.uid),
                          boot_id=self.boot,hook_run_id='hook1',session_id='2',prefix='/opt/mesa-m3',fallback='none')
        self.put('proc/sys/kernel/random/boot_id', self.boot)
        self.put('proc/device-tree/compatible', b'apple,j613\0apple,t8122\0')
        self.put('sys/class/drm/renderD128/device/of_node/compatible', b'apple,agx-t8122\0')
        driver=self.root/'sys/class/drm/renderD128/device/driver'; driver.symlink_to(self.root/'drivers/asahi')
        self.put('dev/dri/renderD128', '')
        self.put('etc/mesa-m3/t8122-gpu-experiment','1\n')
        self.abi='profile=legacy HAL0 USC3 match=1'; self.abi_status=0; self.probe_status=0
        self.report = dict(result='pass',exit='0',**{'gl.renderer':'zink Vulkan 1.4(Apple M3)','gl.platform':'wayland',
            'gl.swap':'pass','gl.render':'pass left=255,0,0,255 right=0,0,255,255 glerror=0x0',
            'vk.device':'Apple M3','vk.device_type':'integrated-gpu','vk.job':'pass words=65536 wrong=0',
            'env.MESA_M3_HOOK_RUN_ID':'hook1','env.MESA_M3_SESSION':'/opt/mesa-m3'})
        self.map='map class=implementation path=/opt/mesa-m3/lib/libgallium.so deleted=0 same_file=1'
        self.fd='fd.dri fd=4 node=/dev/dri/renderD128'; self.calls=[]
    def put(self,path,data):
        p=self.root/path; p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes(data if isinstance(data,bytes) else data.encode()); return p
    def process(self,cmd,**kwargs):
        self.calls.append(cmd)
        if cmd[0].endswith('mesa-m3-abi-check'):return subprocess.CompletedProcess(cmd,self.abi_status,self.abi,'')
        text='pid=123 starttime=999 boot_id='+self.boot+'\n'+ '\n'.join(k+'='+v for k,v in self.report.items())+'\n'+self.fd+'\n'+self.map+'\n'
        return subprocess.CompletedProcess(cmd,self.probe_status,text,'')
    def verify(self):
        p=self.put(f'run/user/{self.uid}/mesa-m3-session.state','\n'.join(k+'='+v for k,v in self.state.items()))
        if os.geteuid()==0:os.chown(p,self.uid,-1)
        return CHECK.verify(self.root,self.env,self.uid,self.process)
    def test_legacy_actual_readbacks_required(self):
        self.assertEqual(len(self.verify()[0]),2); self.assertEqual(len(self.calls),2)
        self.assertEqual(self.calls[0][1],'legacy')
    def test_air15_and_pro_legacy(self):
        for board,soc in [('j615','t8122'),('j516s','t6030')]:
            self.put('proc/device-tree/compatible',f'apple,{board}\0apple,{soc}\0')
            self.put('sys/class/drm/renderD128/device/of_node/compatible',f'apple,agx-{soc}\0')
            self.assertIn('PASS Apple GPU OpenGL',self.verify()[0][0])
    def native(self,board='j613'):
        self.put('etc/mesa-m3/t8122-profile','j613-25g83-hal200\n')
        self.put('proc/device-tree/compatible',f'apple,{board}\0apple,t8122\0')
        self.put('sys/class/drm/renderD128/device/of_node/apple,firmware-compat',bytes.fromhex('0000001a0000000600000002'))
        self.put('sys/class/drm/renderD128/device/of_node/apple,j613-25g83-gpu-handoff',bytes.fromhex('00000001'))
        self.put('opt/mesa-m3/25g83/share/mesa-m3/profile','j613-25g83-gl-only\n')
        for d in (self.state,self.env,self.report):
            key='prefix' if d is self.state else 'MESA_M3_SESSION' if d is self.env else 'env.MESA_M3_SESSION'
            d[key]='/opt/mesa-m3/25g83'
        self.abi='profile=j613-25g83-hal200 HAL200 USC3 match=1'
        self.report.update({'gl.renderer':'Apple M3','vk.capability':'unavailable profile=j613-25g83'})
        self.map=self.map.replace('/opt/mesa-m3/lib','/opt/mesa-m3/25g83/lib')
    def test_native_truthful_gl_only(self):
        self.native();self.assertIn('Vulkan unavailable',self.verify()[0][1])
    def test_native_air15_refused(self):
        self.native('j615')
        with self.assertRaises(CHECK.CheckError):self.verify()
    def test_inactive_stale_or_software_never_runs_probe(self):
        for key,val in [('decision','inactive'),('boot_id','old'),('uid','9999'),('hook_run_id','old'),('prefix','other'),('fallback','software')]:
            with self.subTest(key=key):
                old=self.state[key];self.state[key]=val;self.calls=[]
                with self.assertRaises(CHECK.CheckError):self.verify()
                self.assertEqual(self.calls,[]);self.state[key]=old
    def test_abi_refusal_stops_readback(self):
        for abi,status in [('HAL0 USC3 match=0',0),('HAL200 USC3 match=1',0),('HAL0 USC3 match=1',1)]:
            self.abi=abi;self.abi_status=status;self.calls=[]
            with self.assertRaises(CHECK.CheckError):self.verify()
            self.assertEqual(len(self.calls),1)
    def test_report_mutations_fail(self):
        mutations={'result':'fail','exit':'1','gl.renderer':'llvmpipe (Apple M3)', 'gl.platform':'surfaceless',
                   'gl.swap':'fail','gl.render':'pass left=254,0,0,255 right=0,0,255,255 glerror=0x0',
                   'vk.device':'llvmpipe','vk.device_type':'cpu','vk.job':'pass words=65536 wrong=1',
                   'env.MESA_M3_HOOK_RUN_ID':'old','env.MESA_M3_SESSION':'/other'}
        for key,val in mutations.items():
            with self.subTest(key=key):
                old=self.report[key];self.report[key]=val
                with self.assertRaises(CHECK.CheckError):self.verify()
                self.report[key]=old
    def test_deleted_foreign_native_maps_and_wrong_node_refused(self):
        for attr,val in [('map',self.map.replace('same_file=1','same_file=0')),('map',self.map.replace('deleted=0','deleted=1')),
                         ('map',self.map.replace('/opt/mesa-m3/lib','/usr/lib')),('map',self.map.replace('/opt/mesa-m3/lib','/opt/mesa-m3/25g83/lib')),
                         ('fd','fd.dri fd=4 node=/dev/dri/renderD129')]:
            with self.subTest(val=val):
                old=getattr(self,attr);setattr(self,attr,val)
                with self.assertRaises(CHECK.CheckError):self.verify()
                setattr(self,attr,old)
    def test_native_wrong_hal_and_claimed_vk_refused(self):
        self.native();self.abi='HAL0 USC3 match=1'
        with self.assertRaises(CHECK.CheckError):self.verify()
        self.abi='HAL200 USC3 match=1';self.report['vk.capability']='available'
        with self.assertRaises(CHECK.CheckError):self.verify()
    def test_removed_air_intent_refused(self):
        (self.root/'etc/mesa-m3/t8122-gpu-experiment').unlink()
        with self.assertRaises(CHECK.CheckError):self.verify()
    def test_no_gpu_reports_this_boot_startup_without_reboot_loop(self):
        self.state.update(decision='inactive',reason='no-gpu')
        with self.assertRaises(CHECK.CheckError) as error:self.verify()
        self.assertIn('journalctl -b -k',error.exception.remedy)
        self.assertIn('issue #35',error.exception.remedy)
        self.assertNotIn('Reboot into',error.exception.remedy)
        self.assertEqual(self.calls,[])

    def test_root_refused(self):
        with self.assertRaises(CHECK.CheckError):CHECK.verify(self.root,self.env,0,self.process)

class Ownership(unittest.TestCase):
    setUp = test_m3_handoff.M3PathTest.setUp
    run_sh = test_m3_handoff.M3PathTest.run_sh
    selection = test_m3_persistent.Persistent.selection
    transaction_paths = test_m3_persistent.Persistent.transaction_paths
    def helper(self):return self.tmp/'aurora-m3-gpu-check'
    def shell(self,body,check=True):return self.run_sh(self.selection()+f'work="{self.tmp}/work"\nmkdir -p "$work"\n'+body,check=check)
    def test_helper_install_remove_exact_owned(self):
        self.shell('m3_gpu_check_install')
        self.assertEqual((self.state/'m3-gpu-check').read_text().strip(),hashlib.sha256(self.helper().read_bytes()).hexdigest())
        self.shell('m3_gpu_check_remove');self.assertFalse(self.helper().exists())
    def test_foreign_helper_refused_untouched(self):
        self.helper().write_text('foreign')
        self.assertNotEqual(self.shell('m3_gpu_check_install',False).returncode,0)
        self.assertEqual(self.helper().read_text(),'foreign')
    def test_changed_owned_helper_retained(self):
        self.shell('m3_gpu_check_install');self.helper().write_text('changed')
        self.shell('m3_gpu_check_remove');self.assertEqual(self.helper().read_text(),'changed')
    def test_symlink_helper_refused(self):
        target=self.tmp/'foreign';target.write_text('foreign');self.helper().symlink_to(target)
        self.assertNotEqual(self.shell('m3_gpu_check_install',False).returncode,0)
        self.shell('m3_gpu_check_remove');self.assertEqual(target.read_text(),'foreign')
    def test_new_helper_rollback_restores_absence(self):
        self.run_sh(self.transaction_paths()+'m3_persistent_transaction_begin limine\nm3_gpu_check_install\nm3_persistent_transaction_rollback')
        self.assertFalse(self.helper().exists());self.assertFalse((self.state/'m3-gpu-check').exists())
    def test_owned_helper_rollback_restores_prior_bytes(self):
        self.shell('m3_gpu_check_install');(self.tmp/'work/aurora-m3-gpu-check').unlink();(self.tmp/'work').rmdir();old=self.helper().read_bytes();record=(self.state/'m3-gpu-check').read_bytes()
        self.run_sh(self.transaction_paths()+'m3_persistent_transaction_begin limine\nprintf successor > "$M3_GPU_CHECK"\nprintf changed > "$STATE/m3-gpu-check"\nm3_persistent_transaction_rollback')
        self.assertEqual(self.helper().read_bytes(),old);self.assertEqual((self.state/'m3-gpu-check').read_bytes(),record)
