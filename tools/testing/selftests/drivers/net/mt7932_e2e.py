#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Physical station E2E: explicit Wi-Fi route, gateway round trips, DNS/TLS data.

Manual hardware test; activates and selects a band on the supplied saved profile.
Run once per named attempt. Output contains no credentials or unit addresses.
The saved NetworkManager profile supplies credentials on the target only.
"""
import argparse
from contextlib import contextmanager
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

fault = re.compile(r'translation fault|stale exception latched|Kernel panic|Internal error:|'
                   r'SError Interrupt|BUG:|Unable to handle kernel|WIFI_TERMINAL|WIFI_STARTUP_FAILED|'
                   r'qualification stopped:|CAL_PROCEDURE_FAILED|D7_CALIBRATION_FAILED|'
                   r'SCAN_INTERFACE_SETUP_FAILED|RF_FAILED|recovery-required=1|WARNING:|'
                   r'UBSAN:|hung_task|blocked for more than|WIFI_FLR_REFUSED_OR_FAILED', re.I)

# Both radio drivers report progress at debug level, so a message of theirs at
# info or notice level during scans, association or traffic is log spam.
driver_message = re.compile(r'^\[\s*(\d+\.\d+)\] (?:mt7932-fullmac|mt7932_bt_pcie) '
                            r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]: ', re.M)
log_stamp = re.compile(r'^\[\s*(\d+\.\d+)\]', re.M)


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


def last_log_time(log):
    """Timestamp of the newest kernel log message, or 0.0 if there is none."""
    stamps = log_stamp.findall(log)
    return float(stamps[-1]) if stamps else 0.0


def driver_info_messages(log, since):
    """Radio driver messages in a ``dmesg --level=notice,info`` log after @since."""
    return [m.group(0) for m in driver_message.finditer(log) if float(m.group(1)) > since]


def dns_name(packet, offset):
    """Decode a bounded DNS name, rejecting bad offsets and compression cycles."""
    labels, seen, end, size = [], set(), None, 1
    while True:
        require(offset < len(packet) and offset not in seen, 'Invalid DNS name')
        seen.add(offset)
        length = packet[offset]
        if length & 0xc0 == 0xc0:
            require(offset + 1 < len(packet), 'Truncated DNS pointer')
            if end is None:
                end = offset + 2
            offset = ((length & 0x3f) << 8) | packet[offset + 1]
            continue
        require(not (length & 0xc0), 'Invalid DNS label')
        offset += 1
        if not length:
            return b'.'.join(labels).lower(), end if end is not None else offset
        require(offset + length <= len(packet), 'Truncated DNS label')
        size += length + 1
        require(size <= 255, 'DNS name too long')
        labels.append(packet[offset:offset + length])
        offset += length


def dns_answer(packet, query_id):
    """Accept a matching, complete successful A response for example.com."""
    require(len(packet) >= 12, 'Truncated DNS header')
    ident, flags, questions, answers, _, _ = struct.unpack_from('!6H', packet)
    require(ident == query_id and flags & 0x8000 and not (flags & 0x7a0f),
            'DNS reply rejected')  # QR required; opcode, TC and rcode must be zero.
    require(questions == 1 and answers > 0, 'DNS positive answer missing')
    name, offset = dns_name(packet, 12)
    require(offset + 4 <= len(packet) and name == b'example.com' and
            struct.unpack_from('!HH', packet, offset) == (1, 1), 'DNS question mismatch')
    offset += 4
    addresses, aliases = {}, {}
    for _ in range(answers):
        owner, offset = dns_name(packet, offset)
        require(offset + 10 <= len(packet), 'Truncated DNS record')
        kind, cls, _, size = struct.unpack_from('!HHIH', packet, offset)
        offset += 10
        require(offset + size <= len(packet), 'Truncated DNS answer')
        if cls == 1 and kind == 1:
            require(size == 4, 'Invalid DNS A answer')
            address = ipaddress.IPv4Address(packet[offset:offset + size])
            require(not (address.is_unspecified or address.is_multicast or address.is_loopback),
                    'Unusable DNS A answer')
            addresses.setdefault(owner, str(address))
        elif cls == 1 and kind == 5:
            target, end = dns_name(packet, offset)
            require(end == offset + size, 'Invalid DNS CNAME answer')
            aliases[owner] = target
        offset += size
    name, seen = b'example.com', set()
    while name not in seen:
        if name in addresses:
            return addresses[name]
        seen.add(name)
        require(name in aliases, 'DNS answer does not resolve question')
        name = aliases[name]
    raise RuntimeError('DNS CNAME cycle')


def dns_query(iface, server, port=53, timeout=5):
    """Connect UDP to one DNS peer with both requests and replies bound to iface."""
    query_id = int.from_bytes(os.urandom(2), 'big')
    query = (struct.pack('!6H', query_id, 0x100, 1, 0, 0, 0) +
             b'\x07example\x03com\0' + struct.pack('!HH', 1, 1))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, os.fsencode(iface) + b'\0')
        sock.connect((server, port))
        sock.send(query)
        return dns_answer(sock.recv(4096), query_id)


def wifi_dns(iface):
    servers = run(['nmcli', '-g', 'IP4.DNS', 'device', 'show', iface])
    require(servers.returncode == 0, 'Wi-Fi DNS configuration unavailable')
    for line in servers.stdout.splitlines():
        try:
            address = ipaddress.IPv4Address(line.strip())
            if address.is_loopback or address.is_unspecified or address.is_multicast:
                continue
            return dns_query(iface, str(address))
        except (OSError, ValueError, RuntimeError):
            continue
    # Never disclose the unit's server addresses or fall back to a global resolver.
    raise RuntimeError('Wi-Fi-bound upstream DNS lookup failed')


def profile_security(profile_uuid):
    key = run(['nmcli', '-g', '802-11-wireless-security.key-mgmt',
               'connection', 'show', 'uuid', profile_uuid])
    require(key.returncode == 0, 'Profile security unavailable')
    mode = key.stdout.strip()
    # No wireless-security setting is open; key-mgmt "none" is static WEP.
    require(mode in ('', 'wpa-psk'), 'Unsupported profile authentication')
    return 'WPA2' if mode == 'wpa-psk' else 'OPEN'


@contextmanager
def profile_band(profile_uuid, band):
    original = run(['nmcli', '-g', '802-11-wireless.band', 'connection',
                    'show', 'uuid', profile_uuid])
    require(original.returncode == 0, 'Original profile band unavailable')
    try:
        require(run(['nmcli', 'connection', 'modify', 'uuid', profile_uuid,
                     '802-11-wireless.band', band]).returncode == 0, 'Band selection failed')
        yield
    finally:
        # Also restore after activation errors, timeouts or interrupted testing.
        require(run(['nmcli', 'connection', 'modify', 'uuid', profile_uuid,
                     '802-11-wireless.band', original.stdout.strip()]).returncode == 0,
                'Profile band restoration failed')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--expected-release', required=True)
    parser.add_argument('--profile', required=True, help='Unique existing NetworkManager profile name')
    parser.add_argument('--connect', action='store_true')
    parser.add_argument('--attempt', default='e2e')
    parser.add_argument('--band', choices=['bg', 'a'], default='bg')
    parser.add_argument('--desktop', choices=['kde', 'none'], default='kde',
                        help='Desktop qualification; none checks network without desktop requirements')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]{1,48}', args.attempt):
        parser.error('Invalid attempt name')
    out = Path('/tmp/neo-wifi-network-' + args.attempt)
    result = {'result': 'NOT_STARTED', 'steps': [], 'samples': []}
    require(os.geteuid() == 0 and os.uname().release == args.expected_release, 'Wrong host/kernel')
    require(not out.exists(), 'Fresh named E2E attempt required')
    compatible = Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')
    require(b'apple,j700' in compatible and b'apple,t8140' in compatible, 'Wrong device')
    out.mkdir(mode=0o700)
    started = None
    try:
        started = last_log_time(kernel_ok())
        profile = run(['nmcli', '-g', 'connection.uuid', 'connection', 'show', 'id', args.profile])
        profile_uuid = profile.stdout.strip()
        require(profile.returncode == 0 and re.fullmatch(r'[0-9a-f-]{36}', profile_uuid),
                'Missing or ambiguous profile')
        result['authentication'] = profile_security(profile_uuid)
        ssid_query = run(['nmcli', '-g', '802-11-wireless.ssid', 'connection', 'show',
                          'uuid', profile_uuid])
        ssid = ssid_query.stdout.strip()
        require(ssid_query.returncode == 0 and ssid, 'Profile has no SSID')
        devices = [p.name for p in Path('/sys/class/net').iterdir() if (p / 'wireless').exists()]
        require(len(devices) == 1, 'Expected one wireless interface')
        iface = devices[0]
        result['kernel_release'] = os.uname().release
        result['desktop_qualification'] = args.desktop
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
            with profile_band(profile_uuid, args.band):
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
                    dns_address = wifi_dns(iface)
                    payload = out / ('https-' + str(sample) + '.html')
                    transfer = run(['curl', '--fail', '--silent', '--show-error', '--interface', iface,
                                    '--noproxy', '*', '--resolve', 'example.com:443:' + dns_address,
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
                if args.desktop == 'kde':
                    require(all(run(['pgrep', '-x', name]).returncode == 0
                                for name in ('plasmashell', 'kwin_x11')), 'KDE not running')
                    result['steps'].append('KDE_RUNNING_NO_KERNEL_FAULT')
                result['steps'].extend([result['authentication'] + '_SAVED_PROFILE_ACTIVATED', 'DHCP_ROUTE_OBSERVED',
                                        'GATEWAY_ROUNDTRIPS_VERIFIED', 'WIFI_BOUND_DNS_TLS_DOWNLOAD_VERIFIED'])
            result['result'] = 'WIFI_PHYSICAL_NETWORK_E2E_PASS'
    except subprocess.TimeoutExpired:
        result['result'] = 'WIFI_NETWORK_E2E_FAILED'
        result['error'] = 'External command timed out'
    except Exception as error:
        result['result'] = 'WIFI_NETWORK_E2E_FAILED'
        result['error'] = str(error)
    finally:
        final_log = run(['dmesg'])
        log = final_log.stdout
        result['kernel_faults'] = len(fault.findall(log))
        if result['result'] in ('WIFI_SCAN_OBSERVED', 'WIFI_PHYSICAL_NETWORK_E2E_PASS') and (
                final_log.returncode != 0 or result['kernel_faults']):
            result['result'] = 'WIFI_NETWORK_E2E_FAILED'
            result['error'] = 'Final kernel log unavailable or contains a driver fault'
        info_log = run(['dmesg', '--level=notice,info'])
        if started is not None and info_log.returncode == 0:
            result['driver_info_messages'] = len(driver_info_messages(info_log.stdout, started))
        if result['result'] in ('WIFI_SCAN_OBSERVED', 'WIFI_PHYSICAL_NETWORK_E2E_PASS') and (
                info_log.returncode != 0 or result.get('driver_info_messages')):
            result['result'] = 'WIFI_NETWORK_E2E_FAILED'
            result['error'] = 'Radio drivers logged routine events at info level'
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        print('ARTIFACT=' + str(out / 'result.json'))
    return 0 if result['result'] in ('WIFI_SCAN_OBSERVED', 'WIFI_PHYSICAL_NETWORK_E2E_PASS') else 1


if __name__ == '__main__':
    raise SystemExit(main())
