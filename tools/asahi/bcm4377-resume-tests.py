#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile retained Bluetooth resume callbacks against controlled core/PCI services."""
import argparse, hashlib, json, os, re, resource, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--old-ref');p.add_argument('--mutation',choices=['epoch','close','producer-fence','active-fence','board','board-j616s','cold-gate','cold-windows','cold-running-fw','cold-rti','cold-unquiesce','cold-reset','cold-send','cold-queue','cold-unarmed','cold-rearm','cold-error','cold-sticky','cold-setup-retry']);a=p.parse_args()
r=Path(__file__).resolve().parents[2];o=Path(a.out);o.mkdir(parents=True,exist_ok=False);(o/'tmp').mkdir()
def extract(s,n):
 m=re.search(r'^(?:static )?(?:int|void|bool) '+n+r'\([^;]*?\)\n\{',s,re.M|re.S)
 if not m:raise ValueError(n)
 b=s.index('{',m.start())+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[m.start():b]+'\n'
s=(r/'drivers/bluetooth/hci_bcm4377.c').read_text();core=(r/'net/bluetooth/hci_sync.c').read_text();core_hci=(r/'net/bluetooth/hci_core.c').read_text()
names=['bcm4377_setup_retry_allowed', 'bcm4377_queue_setup_retry', 'bcm4377_setup_retry_work', 'bcm4377_resume_recovery_supported', 'bcm4377_resume_recovery_allowed', 'bcm4377_resume_recovery_invalidate', 'bcm4377_resume_recovery_sync', 'bcm4377_resume_recovery_destroy', 'bcm4377_resume_recovery_work', 'bcm4377_hci_timeout', 'bcm4377_resume_recovery_stop', 'bcm4377_hci_open', 'bcm4377_hci_close', 'bcm4377_hci_setup_once', 'bcm4377_hci_setup', 'bcm4377_hci_post_init', 'bcm4377_hci_send_frame', 'bcm4377_init_cfg', 'bcm4377_hci_unregister_dev', 'bcm4377_suspend', 'bcm4377_resume_cold', 'bcm4377_resume', 'bcm4377_sleep_finish']
bodies={n:extract(s,n) for n in names};shared=[]
if a.old_ref:
 old=subprocess.check_output(['git','show',a.old_ref+':drivers/bluetooth/hci_bcm4377.c'],cwd=r,text=True)
 for n in ['bcm4377_hci_open','bcm4377_hci_close','bcm4377_hci_unregister_dev','bcm4377_suspend','bcm4377_resume']:bodies[n]=extract(old,n)
 shared=[n for n in names if 'resume_recovery' in n or n=='bcm4377_hci_timeout']
mutations={
 'epoch':('bcm4377_resume_recovery_sync','epoch == bcm4377->resume_epoch && ','' ,33),
 'close':('bcm4377_hci_close','bcm4377_resume_recovery_invalidate(bcm4377);','',18),
 'producer-fence':('bcm4377_resume_recovery_stop','cancel_work_sync(&bcm4377->resume_work);','',21),
 'active-fence':('bcm4377_resume_recovery_stop','flush_work(&bcm4377->hdev->cmd_sync_work);','',40),
 'board':('bcm4377_resume_recovery_supported','of_machine_is_compatible("apple,j493")','true',13),
 'board-j616s':('bcm4377_resume_recovery_supported','of_machine_is_compatible("apple,j616s")','true',16),
 # A resume that finds the function reset by the sleep and without firmware.
 'cold-gate':('bcm4377_resume','bcm4377_resume_recovery_supported(bcm4377) &&\n\t       (bcm4377_resume_cold','(bcm4377_resume_cold',54),
 'cold-windows':('bcm4377_resume_cold','if (bcm4377_init_cfg(bcm4377))\n\t\treturn true;','',46),
 'cold-running-fw':('bcm4377_resume_cold','return bootstage != 2 || rti_status != 2;','return true;',50),
 'cold-rti':('bcm4377_resume_cold','bootstage != 2 || rti_status != 2','bootstage != 2 && rti_status != 2',52),
 'cold-unquiesce':('bcm4377_resume','\t\tbcm4377->quiesced = false;\n\t\tbcm4377->needs_reset = true;','\t\tbcm4377->needs_reset = true;',46),
 'cold-reset':('bcm4377_resume','\t\tbcm4377->needs_reset = true;\n','',48),
 'cold-send':('bcm4377_hci_send_frame','READ_ONCE(bcm4377->resume_cold)','false',58),
 'cold-queue':('bcm4377_resume','queue_work(system_freezable_wq, &bcm4377->resume_work);','',46),
 'cold-unarmed':('bcm4377_resume','if (bcm4377->resume_armed) {','if (true) {',48),
 'cold-rearm':('bcm4377_resume','\t\t\tbcm4377->resume_armed = false;\n','',46),
 'cold-error':('bcm4377_resume','\t\treturn 0;\n\t}\n\tif (!ret)','\t}\n\tif (!ret)',47),
 'cold-sticky':('bcm4377_resume',' || READ_ONCE(bcm4377->resume_cold)','',61),
 'cold-setup-retry':('bcm4377_resume','\t\tbcm4377_queue_setup_retry(bcm4377);\n\t\treturn 0;','\t\treturn 0;',62),
}
if a.mutation:
 n,old,new,case=mutations[a.mutation];assert bodies[n].count(old)==1,(n,old);bodies[n]=bodies[n].replace(old,new,1)
core_bodies={n:extract(core_hci,n) for n in ['hci_cmd_timeout','hci_cancel_cmd_sync','hci_suspend_dev','hci_resume_dev']}
# The fixture models UP as a mask; the core uses a bit index.
core_bodies={n:v.replace('test_bit(HCI_UP,', 'test_bit(HCI_UP_INDEX,') for n,v in core_bodies.items()}
fixture=(r/'tools/asahi/bcm4377-resume/fixture.h').read_text()
hw_struct=re.search(r'^struct bcm4377_hw \{.*?^\};',s,re.M|re.S).group()
hw_enum=re.search(r'^enum bcm4377_chip \{.*?^\};',s,re.M|re.S).group()
hw_table=re.search(r'^static const struct bcm4377_hw bcm4377_hw_variants\[\] = \{.*?^\};',s,re.M|re.S).group()
fixture=re.sub(r'^struct bcm4377_hw \{.*?\};', 'typedef uint32_t u32;\n'+hw_struct,fixture,count=1,flags=re.M)
# The variant table, the window setup and the cold-resume probe name registers by macro; take their definitions from the driver.
hw_macros='\n'+''.join(m.group()+'\n' for name in sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b',hw_table+extract(s,'bcm4377_init_cfg')+extract(s,'bcm4377_resume_cold')))) for m in [re.search(r'^#define '+name+r'\b.*$',s,re.M)] if m)
assert 'BAR2_CONTEXT_ADDR_HI' in hw_macros,hw_macros
assert all(name in hw_macros for name in ('PCIECFG_BAR0_WINDOW1','PCIECFG_BAR2_WINDOW_DEFAULT','BAR2_BOOTSTAGE','BAR2_RTI_STATUS')),hw_macros
# The function reset is a fixture that ends the cold state on success; hold the driver to the same single place.
reset=extract(s,'bcm4377_pci_reset')
assert s.count('WRITE_ONCE(bcm4377->resume_cold, false)')==1 and re.search(r'^\tbcm4377->needs_reset = false;\n\tWRITE_ONCE\(bcm4377->resume_cold, false\);$',reset,re.M),'bcm4377_pci_reset'
# bcm4377_suspend() undoes a failed wake arming through bcm4377_resume(), declared ahead of it.
resume_decl=re.search(r'^static int bcm4377_resume\(struct device \*dev\);$',s,re.M).group()+'\n'
fixture=fixture.replace('struct bcm4377_hw *hw;', 'const struct bcm4377_hw *hw;')
table_callbacks='''
static int send_cal(struct bcm4377_data *);
static int send_ptb(struct bcm4377_data *,const struct firmware *);
#define bcm4378_send_calibration send_cal
#define bcm4387_send_calibration send_cal
#define bcm4388_send_calibration send_cal
#define bcm4377_send_ptb send_ptb
#define bcm4378_send_ptb send_ptb
'''
code=fixture+hw_enum+hw_macros+table_callbacks+hw_table+resume_decl+'\n'.join(bodies.values())+'\n'.join(core_bodies.values())+extract(core,'hci_reset_dev_sync')+(r/'tools/asahi/bcm4377-resume/controls.c').read_text()
(o/'production.c').write_text(code);env=dict(os.environ,TMPDIR=str(o/'tmp'))
c=subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-pie','-no-pie',str(o/'production.c'),'-o',str(o/'controls')],env=env,capture_output=True,text=True)
(o/'compile.log').write_text(c.stdout+c.stderr);assert not c.returncode,c.stderr
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
results=[]
for n in ([mutations[a.mutation][3]] if a.mutation else [0,1,2,28] if a.old_ref else range(63)):
 c=subprocess.run([str(o/'controls'),str(n)],capture_output=True,text=True,preexec_fn=no_core,env=dict(env,ASAN_OPTIONS='detect_leaks=1'))
 results.append({'case':n,'exit':c.returncode,'stdout':c.stdout,'stderr':c.stderr})
expected=-6 if a.old_ref or a.mutation else 0
receipt={'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'core_sha256':hashlib.sha256(core.encode()).hexdigest(),'core_hci_sha256':hashlib.sha256(core_hci.encode()).hexdigest(),'core_bodies':{n:hashlib.sha256(v.encode()).hexdigest() for n,v in core_bodies.items()},'actual_hw_struct_sha256':hashlib.sha256(hw_struct.encode()).hexdigest(),'actual_hw_table_sha256':hashlib.sha256(hw_table.encode()).hexdigest(),'table_callback_fixture':'Production variant table compiled unchanged; only calibration/PTB callback names map to controlled send boundaries.', 'extracted_bodies':{n:hashlib.sha256(v.encode()).hexdigest() for n,v in bodies.items()},'shared_new_prerequisites_for_old':shared,'old_ref':a.old_ref,'mutation':a.mutation,'core_fixture_adapter':'Only HCI_UP test_bit index is translated from fixture mask32 to index5; production conditions/order unchanged.', 'source_semantics':'Actual driver callbacks, config-window setup, cold-resume probe, send callback and synchronous close/open helper; core request serialization, PCI config space and BAR registers, the function reset with its firmware boot, and HCI services are controlled fixtures, not whole-kernel or radio execution.','reset_fixture':'bcm4377_pci_reset() is a controlled boundary; the driver source is held to clearing resume_cold only at the end of its success path.','translation_sha256':hashlib.sha256(code.encode()).hexdigest(),'controls':results,'expected_exit':expected}
(o/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');assert all(x['exit']==expected for x in results),results
print(f'{len(results)} production controls passed')
