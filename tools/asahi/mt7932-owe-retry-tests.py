#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the connect worker's OWE retry decision and reset with synthetic peers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LINK = Path('drivers/net/wireless/mediatek/mt7932/link.c')

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define dev_dbg(...) ((void)0)
struct completion { bool done; };
struct mt7932 {
 int command_mutex,response_lock,connect_error;
 bool owe_cold_prepared,connect_owe,connect_cancelled,disconnecting,link_failed,rf_ready;
 bool disconnect_seen,disconnect_event_received,assoc_request_seen,owner;
 unsigned assoc_request_ie_len,connect_center,connection_generation;
 struct completion assoc_start,assoc_done,disconnect_done;
 unsigned char connect_chandef[32];
};
static unsigned locked, attempts, retirements, snapshots, preparations;
static int first_error, second_error, retire_error, prepare_error;
static bool initial_auth, initial_submitted, cancel_before, cancel_during, rf_before, rf_during;
static bool first_disconnecting, late_error;
static void mutex_lock(int *m) { assert(!locked); locked=1; }
static void mutex_unlock(int *m) { assert(locked); locked=0; }
static bool mt_rf_allowed(struct mt7932 *m) { return m->rf_ready; }
static void reinit_completion(struct completion *c) { c->done=false; }
static void mt_transport_snapshot(struct mt7932 *m) { snapshots++; }
static int mt_connect_prepare(struct mt7932 *m, bool *owned)
{
 assert(locked && !m->owner);preparations++;
 if (prepare_error) return prepare_error;
 *owned=m->owner=true;
 return 0;
}
static int mt_connect_attempt(struct mt7932 *m, bool *submitted, bool *owned, bool *auth_submitted, bool retry)
{
 assert(locked && attempts<2 && retry==(attempts==1));
 if (retry || preparations) {
  assert(!m->owner && !m->connect_error && !m->disconnecting);
  assert(!m->disconnect_seen && !m->disconnect_event_received);
  assert(!m->assoc_start.done && !m->assoc_done.done && !m->disconnect_done.done);
  assert(!m->assoc_request_seen && !m->assoc_request_ie_len && !m->connect_center);
  for (unsigned i=0;i<sizeof(m->connect_chandef);i++) assert(!m->connect_chandef[i]);
  assert(m->connection_generation==9+preparations+retry);
 } else {
  m->connect_cancelled=cancel_before;
  m->rf_ready=!rf_before;
  m->disconnecting=first_disconnecting;
  if (late_error) m->connect_error=first_error;
 }
 *submitted=retry || initial_submitted;
 *auth_submitted=retry || initial_auth;
 *owned=true;
 m->owner=true;
 attempts++;
 return retry ? second_error : first_error;
}
static int mt_retire_connection(struct mt7932 *m)
{
 assert(!locked && m->disconnecting && m->owner);
 retirements++;
 if (retire_error) { m->link_failed=true; return retire_error; }
 m->owner=false;
 m->connect_error=-ECONNRESET; /* Expected typed disconnect event. */
 m->connect_cancelled=cancel_during;
 m->rf_ready=!rf_during;
 return 0;
}
RETIRE_HELPER
static int worker(struct mt7932 *m)
{
 unsigned long flags;
 bool submitted=false,peer_owned=false,retry=false,retirement_attempted=false,auth_submitted=false;
 int ret;
WORKER
report:
 assert(locked);
 // Mirror the final report's cleanup decision, so a failed retirement cannot
 // be repeated or release an uncertain peer through the unsubmitted path.
 if (ret && submitted && !retirement_attempted) retirements++;
 else if (ret && peer_owned && !submitted && !retirement_attempted) assert(!retirement_attempted);
 mutex_unlock(&m->command_mutex);
 return ret;
}
static struct mt7932 fresh(void)
{
 struct mt7932 m={.owe_cold_prepared=true,.connect_owe=true,.rf_ready=true,.disconnect_seen=true,
  .disconnect_event_received=true,.assoc_request_seen=true,.assoc_request_ie_len=8,
  .connect_center=11,.connection_generation=9,
  .assoc_start={true},.assoc_done={true},.disconnect_done={true}};
 memset(m.connect_chandef,0xa5,sizeof(m.connect_chandef));
 attempts=retirements=snapshots=locked=preparations=0;
 first_error=-ETIMEDOUT;second_error=retire_error=prepare_error=0;initial_auth=initial_submitted=true;
 cancel_before=cancel_during=rf_before=rf_during=first_disconnecting=late_error=false;
 return m;
}
int main(void)
{
 struct mt7932 m;unsigned cases=0;
 int retry_errors[]={-ETIMEDOUT,-ECONNREFUSED,-ECONNRESET};
 for (unsigned i=0;i<3;i++) {
  m=fresh();first_error=retry_errors[i];late_error=true;first_disconnecting=(i==2);
  assert(worker(&m)==0 && attempts==2 && retirements==1 && snapshots==1);cases++;
 }
 m=fresh();second_error=-ETIMEDOUT;
 assert(worker(&m)==-ETIMEDOUT && attempts==2 && retirements==2);cases++;
 int no_retry_errors[]={0,-EPROTO,-ECANCELED,-EIO,-ENOMEM};
 for (unsigned i=0;i<5;i++) {
  m=fresh();first_error=no_retry_errors[i];
  assert(worker(&m)==first_error && attempts==1 && snapshots==0);cases++;
 }
 for (unsigned i=0;i<7;i++) {
  m=fresh();
  switch(i) {
  case 0:m.connect_owe=false;break;
  case 1:initial_auth=initial_submitted=false;break;
  case 2:cancel_before=true;break;
  case 3:rf_before=true;break;
  case 4:m.link_failed=true;break;
  case 5:initial_auth=false;break;
  default:m.rf_ready=false;rf_before=true;break;
  }
  assert(worker(&m)==-ETIMEDOUT && attempts==1 && snapshots==0);cases++;
 }
 m=fresh();retire_error=-EIO;
 assert(worker(&m)==-EIO && attempts==1 && retirements==1 && m.owner);cases++;
 m=fresh();cancel_during=true;
 assert(worker(&m)==-ECANCELED && attempts==1 && retirements==1 && !m.owner);cases++;
 m=fresh();rf_during=true;
 assert(worker(&m)==-EIO && attempts==1 && retirements==1 && !m.owner);cases++;
 // Cold cleanup precedes the first association and is not repeated once armed.
 m=fresh();m.owe_cold_prepared=false;first_error=0;
 assert(!worker(&m)&&attempts==1&&preparations==1&&retirements==1&&m.owe_cold_prepared);cases++;
 m=fresh();m.owe_cold_prepared=false;
 assert(!worker(&m)&&attempts==2&&preparations==1&&retirements==2);cases++;
 m=fresh();m.owe_cold_prepared=false;prepare_error=-EINVAL;
 assert(worker(&m)==-EINVAL&&!attempts&&!retirements);cases++;
 m=fresh();m.owe_cold_prepared=false;retire_error=-EIO;
 assert(worker(&m)==-EIO&&!attempts&&retirements==1&&m.owner);cases++;
 m=fresh();m.owe_cold_prepared=false;cancel_during=true;
 assert(worker(&m)==-ECANCELED&&!attempts&&retirements==1&&!m.owner);cases++;
 m=fresh();m.owe_cold_prepared=false;rf_during=true;
 assert(worker(&m)==-EIO&&!attempts&&retirements==1&&!m.owner);cases++;
 printf("PASS %u OWE cold cleanup, retry, cancellation and ownership controls\n",cases);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    text = (args.source_root / LINK).read_text()
    start = text.index('void mt_connect_work(')
    start = text.index('\tmutex_lock(&m->command_mutex);', start)
    end = text.index('report:\n', start)
    worker = text[start:end]
    helper_start = text.index('static int mt_owe_retire_attempt(')
    helper_end = text.index('\n/* command_mutex held on entry and return. D7', helper_start)
    helper = text[helper_start:helper_end]
    args.out.mkdir(parents=True, exist_ok=True)
    candidates = {'production': (helper, worker),
                  'mutant-repeat-retirement': (helper.replace(
                      '*retirement_attempted = true;', '*retirement_attempted = false;'), worker),
                  'mutant-cancel-reset': (helper.replace(
                      'if (m->connect_cancelled)', 'if (false)'), worker),
                  'mutant-skip-retirement': (helper.replace(
                      'ret = mt_retire_connection(m);', 'ret = 0;'), worker)}
    results = {}
    for name, (retire_helper, code) in candidates.items():
        source = args.out / (name + '.c')
        binary = args.out / name
        source.write_text(HARNESS.replace('RETIRE_HELPER', retire_helper).replace('WORKER', code))
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-parameter',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (args.out / (name + '.log')).write_text(result.stdout + result.stderr)
        results[name] = {
            'exit': result.returncode,
            'worker_sha256': hashlib.sha256(code.encode()).hexdigest(),
            'retire_helper_sha256': hashlib.sha256(retire_helper.encode()).hexdigest(),
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        }
        (args.out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(name, result.returncode, result.stdout.strip())
        assert (result.returncode == 0) == (name == 'production')


if __name__ == '__main__':
    main()
