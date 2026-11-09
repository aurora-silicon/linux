#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise brcmfmac's capability-gated PCIe in-band deep-sleep protocol."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:void|int)\s+' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError('Missing function ' + name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define lockdep_assert_held(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define wake_up_all(x) ((void)(x))
#define wake_up(x) ((void)(x))
#define WARN_ON(x) (x)
#define brcmf_dbg(...) ((void)0)
#define BRCMF_PCIE_MBDATA_TIMEOUT 1
#define BRCMF_D2H_DEV_D3_ACK 0x00000001
#define BRCMF_D2H_DEV_DS_ENTER_REQ 0x00000002
#define BRCMF_D2H_DEV_DS_EXIT_NOTE 0x00000004
#define BRCMF_D2H_DEV_HOST_SLEEP_EXIT_ACK 0x00000008
#define BRCMF_D2H_DEV_FWHALT 0x10000000
#define BRCMF_H2D_HOST_DS_ACK 0x00000002
#define BRCMF_H2D_DEVICE_WAKE_DEASSERT 0x00000020
#define BRCMF_H2D_DEVICE_WAKE_ASSERT 0x00000040
#define BRCMF_HOSTCAP_DS_NO_OOB_DW 0x00001000
#define BRCMF_HOSTCAP_DS_INBAND_DW 0x00002000
#define BRCMF_PCIE_SHARED_INBAND_DS 0x40000000
struct device { int unused; };
struct pci_dev { struct device dev; };
struct brcmf_pcie_shared_info { u32 flags; };
struct brcmf_pciedev_info {
 bool inband_ds, skip_ds_ack, mbdata_completed;
 enum brcmf_pcie_ds_state ds_state;
 unsigned host_active;
 int ds_lock, ds_wait, mbdata_resp_wait;
 struct pci_dev *pdev;
 struct brcmf_pcie_shared_info shared;
};
static struct brcmf_pciedev_info *wait_dev;
static bool deliver_exit;
static bool mailbox_db1;
static bool fw_asleep;
static u32 sent[32];
static unsigned sent_count, crashes;
static u32 fail_message;
static void wait_step(void) {
 if (deliver_exit && wait_dev && wait_dev->ds_state == BRCMF_PCIE_DS_WAKE_WAIT)
  wait_dev->ds_state = BRCMF_PCIE_DS_AWAKE;
}
#define wait_event_timeout(q,c,t) (wait_step(), (c) ? 1L : 0L)
static int brcmf_pcie_send_mb_data(struct brcmf_pciedev_info *d, u32 data) {
 (void)d;sent[sent_count++]=data;
 if (data==BRCMF_H2D_HOST_DS_ACK) fw_asleep=true;
 if (data==BRCMF_H2D_DEVICE_WAKE_ASSERT && fw_asleep && mailbox_db1)
  deliver_exit=true;
 return data == fail_message ? -EIO : 0;
}
static void brcmf_fw_crashed(struct device *d) {(void)d;crashes++;}
'''


MAIN = r'''
static void reset(struct brcmf_pciedev_info *d, struct pci_dev *p, bool capable) {
 memset(d,0,sizeof(*d));memset(p,0,sizeof(*p));memset(sent,0,sizeof(sent));
 d->pdev=p;d->inband_ds=capable;d->ds_state=BRCMF_PCIE_DS_ACTIVE;
 wait_dev=d;deliver_exit=false;mailbox_db1=SOURCE_MAILBOX_DB1;fw_asleep=false;
 sent_count=crashes=0;fail_message=0xffffffff;
}
int main(void) {
 struct brcmf_pciedev_info d;struct pci_dev p;unsigned cases=0;
 assert(SOURCE_RING_FENCE);assert(SOURCE_D3_ORDER);
 assert(SOURCE_SETUP_ERROR_SAFE);cases+=3;

 reset(&d,&p,false);d.inband_ds=d.skip_ds_ack=true;d.host_active=3;
 d.ds_state=BRCMF_PCIE_DS_ERROR;
 assert(negotiate(&d,0)==BRCMF_HOSTCAP_DS_NO_OOB_DW && !d.inband_ds &&
        !d.skip_ds_ack && !d.host_active && d.ds_state==BRCMF_PCIE_DS_ACTIVE);
 assert(negotiate(&d,BRCMF_PCIE_SHARED_INBAND_DS)==
        (BRCMF_HOSTCAP_DS_NO_OOB_DW|BRCMF_HOSTCAP_DS_INBAND_DW));
 assert(d.inband_ds && d.ds_state==BRCMF_PCIE_DS_ACTIVE);cases++;

 reset(&d,&p,false);brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_DS_ENTER_REQ);
 assert(sent_count==1 && sent[0]==BRCMF_H2D_HOST_DS_ACK &&
        d.ds_state==BRCMF_PCIE_DS_ACTIVE);cases++;

 reset(&d,&p,true);assert(!brcmf_pcie_ds_begin(&d));
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_DS_ENTER_REQ);
 assert(d.ds_state==BRCMF_PCIE_DS_SLEEP_PENDING && d.host_active==1 && !sent_count);
 brcmf_pcie_ds_end(&d);
 assert(d.ds_state==BRCMF_PCIE_DS_SLEEP && !d.host_active &&
        sent_count==1 && sent[0]==BRCMF_H2D_HOST_DS_ACK);cases++;

 /* Model the observed attach failure: firmware accepts DS_ACK and then only
  * consumes DEVICE_WAKE_ASSERT when its TCM mailbox is notified through DB1.
  */
 reset(&d,&p,true);
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_DS_ENTER_REQ);
 assert(d.ds_state==BRCMF_PCIE_DS_SLEEP && fw_asleep && sent_count==1 &&
        sent[0]==BRCMF_H2D_HOST_DS_ACK);
 assert(!brcmf_pcie_ds_begin(&d));
 assert(d.ds_state==BRCMF_PCIE_DS_AWAKE && d.host_active==1 &&
        sent_count==2 && sent[1]==BRCMF_H2D_DEVICE_WAKE_ASSERT);
 brcmf_pcie_ds_end(&d);
 assert(d.ds_state==BRCMF_PCIE_DS_ACTIVE && !d.host_active &&
        sent_count==3 && sent[2]==BRCMF_H2D_DEVICE_WAKE_DEASSERT);cases++;

 reset(&d,&p,true);d.ds_state=BRCMF_PCIE_DS_SLEEP;
 assert(brcmf_pcie_ds_begin(&d)==-ETIMEDOUT);
 assert(d.ds_state==BRCMF_PCIE_DS_ERROR && !d.host_active && crashes==1);cases++;

 reset(&d,&p,true);assert(!brcmf_pcie_ds_prepare_suspend(&d));
 assert(d.skip_ds_ack && d.ds_state==BRCMF_PCIE_DS_HOST_SLEEP_WAIT);
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_DS_ENTER_REQ);
 assert(!sent_count);
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_D3_ACK);
 assert(d.ds_state==BRCMF_PCIE_DS_HOST_SLEEP && d.mbdata_completed);
 brcmf_pcie_ds_resume_start(&d);
 assert(d.ds_state==BRCMF_PCIE_DS_HOST_WAKE_WAIT);
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_HOST_SLEEP_EXIT_ACK);
 assert(d.ds_state==BRCMF_PCIE_DS_ACTIVE && d.skip_ds_ack);
 brcmf_pcie_ds_resume_complete(&d);
 assert(d.ds_state==BRCMF_PCIE_DS_ACTIVE && !d.skip_ds_ack);cases++;

 reset(&d,&p,true);d.ds_state=BRCMF_PCIE_DS_SLEEP;
 brcmf_pcie_handle_mb_data(&d,BRCMF_D2H_DEV_DS_EXIT_NOTE);
 assert(d.ds_state==BRCMF_PCIE_DS_ACTIVE && sent_count==1 &&
        sent[0]==BRCMF_H2D_DEVICE_WAKE_DEASSERT);cases++;

 reset(&d,&p,true);assert(!brcmf_pcie_ds_prepare_suspend(&d));
 brcmf_pcie_ds_suspend_abort(&d);
 assert(!d.skip_ds_ack && d.ds_state==BRCMF_PCIE_DS_ACTIVE);
 assert(!brcmf_pcie_ds_begin(&d));brcmf_pcie_ds_end(&d);cases++;

 printf("%u in-band DS capability, enter/exit, timeout and D3 controls passed\n",cases);
 return 0;
}
'''


def mutate(source, msgbuf, name):
    if name == 'capability-gate':
        source = source.replace('if (shared->flags & BRCMF_PCIE_SHARED_INBAND_DS) {',
                                'if (true) {', 1)
    elif name == 'early-ack':
        source = source.replace('if (!devinfo->host_active) {', 'if (true) {', 1)
    elif name == 'exit-note':
        source = source.replace(
            '} else if (devinfo->ds_state == BRCMF_PCIE_DS_SLEEP) {',
            '} else if (false) {', 1)
    elif name == 'wake-timeout':
        source = source.replace(
            'if (!timeout || devinfo->ds_state != BRCMF_PCIE_DS_AWAKE) {',
            'if (false) {', 1)
    elif name == 'd3-race':
        source = source.replace('devinfo->skip_ds_ack = true;',
                                'devinfo->skip_ds_ack = false;', 1)
    elif name == 'unwind':
        start = source.index('static void brcmf_pcie_ds_suspend_abort')
        end = source.index('\n}', start)
        body = source[start:end]
        source = source[:start] + body.replace('devinfo->skip_ds_ack = false;',
                                                'devinfo->skip_ds_ack = true;', 1) + source[end:]
    elif name == 'ring-fence':
        msgbuf = msgbuf.replace('err = brcmf_commonring_prepare_write(commonring);',
                                'err = 0;', 1)
    elif name == 'd3-order':
        source = source.replace('ret = brcmf_pcie_ds_prepare_suspend(devinfo);',
                                'ret = 0;', 1)
    elif name == 'mailbox-db1':
        source = source.replace('BRCMF_PCIE_PCIE2REG_H2D_MAILBOX_1, 1);',
                                'BRCMF_PCIE_REG_SBMBX, 1);', 1)
    elif name == 'setup-error-reset':
        source = source.replace('\tdevice_release_driver(dev);',
                                '\tbrcmf_fw_crashed(dev);\n\tdevice_release_driver(dev);', 1)
    return source, msgbuf


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=('capability-gate', 'early-ack', 'exit-note',
                                               'wake-timeout', 'd3-race', 'unwind',
                                               'ring-fence', 'd3-order', 'mailbox-db1',
                                               'setup-error-reset'))
    args = parser.parse_args()
    source = (DRIVER / 'pcie.c').read_text()
    msgbuf = (DRIVER / 'msgbuf.c').read_text()
    if args.mutation:
        source, msgbuf = mutate(source, msgbuf, args.mutation)

    enum = re.search(r'enum brcmf_pcie_ds_state \{.*?\};', source, re.S)[0]
    init = function(source, 'brcmf_pcie_init_share_ram_info')
    cap = re.search(r'\tdevinfo->inband_ds = false;\n.*?'
                    r'\thost_cap \|= BRCMF_HOSTCAP_DS_NO_OOB_DW;\n'
                    r'\tif \(.*?\n\t\}', init, re.S)[0]
    negotiate = ('static u32 negotiate(struct brcmf_pciedev_info *devinfo, u32 flags) {\n'
                 ' struct brcmf_pcie_shared_info *shared=&devinfo->shared;u32 host_cap=0;\n'
                 ' shared->flags=flags;\n' + cap + '\n return host_cap;\n}\n')
    names = ('brcmf_pcie_ds_send_locked', 'brcmf_pcie_ds_begin', 'brcmf_pcie_ds_end',
             'brcmf_pcie_ds_prepare_suspend', 'brcmf_pcie_ds_suspend_abort',
             'brcmf_pcie_ds_resume_start', 'brcmf_pcie_ds_resume_complete',
             'brcmf_pcie_handle_mb_data')
    body = ''.join(function(source, name) for name in names)

    send_mb = function(source, 'brcmf_pcie_send_mb_data')
    mailbox_db1 = ('brcmf_pcie_write_tcm32' in send_mb and
                   'BRCMF_PCIE_PCIE2REG_H2D_MAILBOX_1' in send_mb and
                   send_mb.index('brcmf_pcie_write_tcm32') <
                   send_mb.index('BRCMF_PCIE_PCIE2REG_H2D_MAILBOX_1'))
    setup = function(source, 'brcmf_pcie_setup')
    setup_fail = setup[setup.rindex('\nfail:'):]
    setup_error_safe = 'brcmf_fw_crashed' not in setup_fail

    tx_ioctl = function(msgbuf, 'brcmf_msgbuf_tx_ioctl')
    ring_fence = 'brcmf_commonring_prepare_write' in tx_ioctl
    if ring_fence:
        ring_fence = (tx_ioctl.index('brcmf_commonring_prepare_write') <
                      tx_ioctl.index('brcmf_commonring_lock') <
                      tx_ioctl.index('brcmf_commonring_write_complete') <
                      tx_ioctl.rindex('brcmf_commonring_unlock') <
                      tx_ioctl.rindex('brcmf_commonring_finish_write'))
    enter = function(source, 'brcmf_pcie_pm_enter_D3')
    leave = function(source, 'brcmf_pcie_pm_leave_D3')
    d3_order = ('brcmf_pcie_ds_prepare_suspend' in enter and
                enter.index('brcmf_pcie_ds_prepare_suspend') <
                enter.index('brcmf_bus_change_state(bus, BRCMF_BUS_DOWN)') <
                enter.index('BRCMF_H2D_HOST_D3_INFORM') and
                leave.index('BRCMF_H2D_HOST_D0_INFORM') <
                leave.index('brcmf_pcie_intr_enable') < leave.index('brcmf_pcie_hostready') <
                leave.index('brcmf_pcie_ds_resume_complete') <
                leave.index('brcmf_bus_change_state(bus, BRCMF_BUS_UP)'))
    code = (enum + '\n#define SOURCE_MAILBOX_DB1 ' + str(int(mailbox_db1)) +
            '\n' + PREFIX + negotiate + body +
            f'\n#define SOURCE_RING_FENCE {int(ring_fence)}\n'
            f'#define SOURCE_D3_ORDER {int(d3_order)}\n'
            f'#define SOURCE_SETUP_ERROR_SAFE {int(setup_error_safe)}\n' + MAIN)

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        cfile = out / 'controls.c'
        cfile.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(cfile), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt = {'exit': result.returncode, 'stdout': result.stdout,
                   'stderr': result.stderr,
                   'limits': 'Host model; no firmware, IRQ concurrency or hardware timing proof.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))
        return result.returncode

    if args.out:
        raise SystemExit(run(args.out))
    with tempfile.TemporaryDirectory(prefix='brcmf-inband-ds-') as temp:
        raise SystemExit(run(Path(temp)))


if __name__ == '__main__':
    main()
