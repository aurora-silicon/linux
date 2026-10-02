#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Physical station E2E: explicit Wi-Fi route, gateway round trips, DNS/TLS data.

Manual hardware test; activates and selects a band on the supplied saved profile.
Run once per named attempt. Output contains no credentials or unit addresses.
The saved NetworkManager profile supplies credentials on the target only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--expected-release', required=True)
parser.add_argument('--profile', required=True, help='Unique existing NetworkManager profile name')
parser.add_argument('--connect', action='store_true')
parser.add_argument('--attempt', default='e2e')
parser.add_argument('--band', choices=['bg', 'a'], default='bg')
args = parser.parse_args()
if not re.fullmatch(r'[a-z0-9-]{1,48}', args.attempt):
    parser.error('Invalid attempt name')
out = Path('/tmp/neo-wifi-network-' + args.attempt)
result = {'result': 'NOT_STARTED', 'steps': [], 'samples': []}
fault = re.compile(r'translation fault|stale exception latched|Kernel panic|Internal error:|'
                   r'SError Interrupt|BUG:|Unable to handle kernel|WIFI_TERMINAL|WIFI_STARTUP_FAILED|'
                   r'qualification stopped:|CAL_PROCEDURE_FAILED|D7_CALIBRATION_FAILED|'
                   r'SCAN_INTERFACE_SETUP_FAILED|recovery-required=1', re.I)


def run(argv, timeout=30):
    return subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)


def require(value, reason):
    if not value:
        raise RuntimeError(reason)


def kernel_ok():
    p = run(['dmesg'])
    require(p.returncode == 0 and not fault.search(p.stdout), 'Kernel or driver fault')
    return p.stdout


require(os.geteuid() == 0 and os.uname().release == args.expected_release, 'Wrong host/kernel')
require(not out.exists(), 'Fresh named E2E attempt required')
compatible = Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')
require(b'apple,j700' in compatible and b'apple,t8140' in compatible, 'Wrong device')
out.mkdir(mode=0o700)
try:
    kernel_ok()
    profile = run(['nmcli', '-g', 'connection.uuid', 'connection', 'show', 'id', args.profile])
    profile_uuid = profile.stdout.strip()
    require(profile.returncode == 0 and re.fullmatch(r'[0-9a-f-]{36}', profile_uuid),
            'Missing or ambiguous profile')
    ssid_query = run(['nmcli', '-g', '802-11-wireless.ssid', 'connection', 'show',
                      'uuid', profile_uuid])
    ssid = ssid_query.stdout.strip()
    require(ssid_query.returncode == 0 and ssid, 'Profile has no SSID')
    devices = [p.name for p in Path('/sys/class/net').iterdir() if (p / 'wireless').exists()]
    require(len(devices) == 1, 'Expected one wireless interface')
    iface = devices[0]
    result['kernel_release'] = os.uname().release
    result['interface'] = iface
    require(run(['ip', 'link', 'set', iface, 'up']).returncode == 0, 'Interface up failed')
    require(run(['nmcli', 'device', 'set', iface, 'managed', 'yes']).returncode == 0,
            'NetworkManager admission failed')
    scan = run(['nmcli', '--wait', '30', 'device', 'wifi', 'rescan', 'ifname', iface], 35)
    result['rescan_exit'] = scan.returncode
    # NetworkManager may already own a scan when the explicit request arrives.
    # Poll its cache without starting a second scan and check firmware health.
    names = []
    for attempt in range(20):
        time.sleep(2)
        kernel_ok()
        listings = run(['nmcli', '-t', '-f', 'SSID', 'device', 'wifi', 'list',
                        '--rescan', 'no', 'ifname', iface])
        require(listings.returncode == 0, 'Scan result read failed')
        names = listings.stdout.splitlines()
        if ssid in names:
            break
    result['visible_networks'] = len([name for name in names if name])
    result['requested_network_visible'] = ssid in names
    require(result['requested_network_visible'], 'Requested network absent from scan')
    result['steps'].append('REQUESTED_NETWORK_SCAN_OBSERVED')
    if not args.connect:
        result['result'] = 'WIFI_SCAN_OBSERVED'
    else:
        require(run(['nmcli', 'connection', 'modify', 'uuid', profile_uuid,
                     '802-11-wireless.band', args.band]).returncode == 0, 'Band selection failed')
        connection = run(['nmcli', '--wait', '60', 'connection', 'up', 'uuid',
                          profile_uuid, 'ifname', iface], 65)
        result['activation_exit'] = connection.returncode
        link = run(['iw', 'dev', iface, 'link']).stdout
        freq = re.search(r'freq: (\d+)', link)
        result['frequency_mhz'] = int(freq[1]) if freq else None
        require(freq and ((int(freq[1]) < 3000) == (args.band == 'bg')), 'Unexpected associated band')
        require(connection.returncode == 0, 'Saved profile activation failed')
        for sample in range(3):
            addresses = json.loads(run(['ip', '-j', '-4', 'address', 'show', 'dev', iface]).stdout)
            require(any(a.get('scope') == 'global' for link in addresses
                        for a in link.get('addr_info', [])), 'No DHCP IPv4 address')
            routes = json.loads(run(['ip', '-j', '-4', 'route', 'show', 'default', 'dev', iface]).stdout)
            require(routes and routes[0].get('gateway'), 'No Wi-Fi default gateway')
            ping = run(['ping', '-I', iface, '-c', '3', '-W', '3', routes[0]['gateway']], 15)
            require(ping.returncode == 0 and re.search(r'\b0(?:\.0+)?% packet loss', ping.stdout)
                    and re.search(r'\b3 received\b', ping.stdout), 'Gateway packet loss')
            dns = run(['getent', 'ahostsv4', 'example.com'])
            require(dns.returncode == 0 and dns.stdout.strip(), 'DNS lookup failed')
            payload = out / ('https-' + str(sample) + '.html')
            transfer = run(['curl', '--fail', '--silent', '--show-error', '--interface', iface,
                            '--max-time', '20', '--output', str(payload), 'https://example.com/'], 25)
            require(transfer.returncode == 0 and payload.is_file(), 'Wi-Fi HTTPS transfer failed')
            body = payload.read_bytes()
            require(b'Example Domain' in body and len(body) > 100, 'Unexpected HTTPS response')
            kernel_ok()
            result['samples'].append({'gateway_received': 3, 'gateway_loss_percent': 0,
                                      'dns': True, 'tls_verified': True, 'download_bytes': len(body),
                                      'download_sha256': hashlib.sha256(body).hexdigest()})
            if sample != 2:
                time.sleep(10)
        require(all(run(['pgrep', '-x', name]).returncode == 0
                    for name in ('plasmashell', 'kwin_x11')), 'KDE not running')
        result['steps'].extend(['WPA2_SAVED_PROFILE_ACTIVATED', 'DHCP_ROUTE_OBSERVED',
                                'GATEWAY_ROUNDTRIPS_VERIFIED', 'DNS_TLS_DOWNLOAD_VERIFIED',
                                'KDE_RUNNING_NO_KERNEL_FAULT'])
        result['result'] = 'WIFI_PHYSICAL_NETWORK_E2E_PASS'
except subprocess.TimeoutExpired:
    result['result'] = 'WIFI_NETWORK_E2E_FAILED'
    result['error'] = 'External command timed out'
except Exception as error:
    result['result'] = 'WIFI_NETWORK_E2E_FAILED'
    result['error'] = str(error)
finally:
    log = run(['dmesg']).stdout
    result['kernel_faults'] = len(fault.findall(log))
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print('ARTIFACT=' + str(out / 'result.json'))
raise SystemExit(0 if result['result'] in ('WIFI_SCAN_OBSERVED', 'WIFI_PHYSICAL_NETWORK_E2E_PASS') else 1)
