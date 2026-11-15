#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Local DNS unit/socket tests; never accesses a physical radio or network."""
import socket
import struct
import subprocess
import threading
import unittest
from unittest import mock

import mt7932_e2e as e2e

QUESTION = b'\x07example\x03com\0' + struct.pack('!HH', 1, 1)
ADDRESS = b'\xcb\x00\x71\x05'  # Documentation-only address, never contacted.
IDENT = 1234


def record(owner=b'\xc0\x0c', kind=1, data=ADDRESS):
    return owner + struct.pack('!HHIH', kind, 1, 60, len(data)) + data


def reply(ident=IDENT, flags=0x8180, question=QUESTION, records=None):
    if records is None:
        records = [record()]
    return (struct.pack('!6H', ident, flags, 1, len(records), 0, 0) + question +
            b''.join(records))


class DNSAnswerTest(unittest.TestCase):
    def test_positive_compressed_a(self):
        self.assertEqual(e2e.dns_answer(reply(), IDENT), '203.0.113.5')

    def test_cname_answer(self):
        target = b'\x06target\x07example\x03com\0'
        packet = reply(records=[record(kind=5, data=target), record(owner=target)])
        self.assertEqual(e2e.dns_answer(packet, IDENT), '203.0.113.5')

    def test_reject_wrong_transaction_or_flags(self):
        for packet in (reply(ident=99), reply(flags=0x0180), reply(flags=0x8380),
                       reply(flags=0x8183), reply(flags=0x8980)):
            with self.subTest(packet=packet[:4]), self.assertRaises(RuntimeError):
                e2e.dns_answer(packet, IDENT)

    def test_reject_wrong_question_and_unrelated_answers(self):
        for packet in (reply(question=b'\x05other\x03com\0' + struct.pack('!HH', 1, 1)),
                       reply(question=QUESTION[:-4] + struct.pack('!HH', 28, 1)),
                       reply(records=[]), reply(records=[record(owner=b'\x05other\0')]),
                       reply(records=[record(data=b'\x7f\0\0\1')])):
            with self.subTest(packet=packet), self.assertRaises(RuntimeError):
                e2e.dns_answer(packet, IDENT)

    def test_reject_every_truncation(self):
        packet = reply()
        for size in range(len(packet)):
            with self.subTest(size=size), self.assertRaises(RuntimeError):
                e2e.dns_answer(packet[:size], IDENT)

    def test_reject_bad_and_cyclic_name_pointers(self):
        owner_offset = 12 + len(QUESTION)
        for owner in (b'\xff\xff', struct.pack('!H', 0xc000 | owner_offset), b'\x40'):
            with self.subTest(owner=owner), self.assertRaises(RuntimeError):
                e2e.dns_answer(reply(records=[record(owner=owner)]), IDENT)

    def test_upstream_selection_and_error_privacy(self):
        config = subprocess.CompletedProcess([], 0, '127.0.0.53\n192.0.2.53\n')
        with mock.patch.object(e2e, 'run', return_value=config) as run:
            with mock.patch.object(e2e, 'dns_query', return_value='203.0.113.5') as query:
                self.assertEqual(e2e.wifi_dns('testwifi'), '203.0.113.5')
                run.assert_called_once_with(['nmcli', '-g', 'IP4.DNS', 'device', 'show', 'testwifi'])
                query.assert_called_once_with('testwifi', '192.0.2.53')
            with mock.patch.object(e2e, 'dns_query', side_effect=OSError('private server details')):
                with self.assertRaisesRegex(RuntimeError, '^Wi-Fi-bound upstream DNS lookup failed$'):
                    e2e.wifi_dns('testwifi')

    def test_rf_failed_is_terminal(self):
        self.assertIsNotNone(e2e.fault.search('RF_FAILED'))


class DNSSocketTest(unittest.TestCase):
    def test_bound_loopback_query(self):
        # Observe the real kernel socket option before connect, without mocking
        # setsockopt/connect/send/recv. All traffic stays on loopback.
        bound = []

        class ObservedSocket(socket.socket):
            def connect(self, peer):
                bound.append(self.getsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, 32))
                return super().connect(peer)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0))
            server.settimeout(2)
            errors = []

            def respond():
                try:
                    query, peer = server.recvfrom(4096)
                    self.assertEqual(query[12:], QUESTION)
                    ident = struct.unpack_from('!H', query)[0]
                    server.sendto(reply(ident=ident), peer)
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=respond)
            worker.start()
            try:
                with mock.patch.object(e2e.socket, 'socket', ObservedSocket):
                    self.assertEqual(e2e.dns_query('lo', '127.0.0.1', server.getsockname()[1]),
                                     '203.0.113.5')
            finally:
                worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(bound, [b'lo\0'])

    def test_bound_query_times_out_without_response(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent_server:
            silent_server.bind(('127.0.0.1', 0))
            with self.assertRaises(TimeoutError):
                e2e.dns_query('lo', '127.0.0.1', silent_server.getsockname()[1], timeout=0.03)
            silent_server.settimeout(1)
            query, _ = silent_server.recvfrom(4096)
            self.assertEqual(query[12:], QUESTION)

    def test_invalid_interface_fails_without_fallback(self):
        with self.assertRaises(OSError):
            e2e.dns_query('no-such-neo-if', '127.0.0.1', timeout=0.03)


class QualificationTest(unittest.TestCase):
    def test_authentication_comes_from_profile(self):
        for key, expected in (('', 'OPEN'), ('wpa-psk', 'WPA2')):
            with self.subTest(key=key), mock.patch.object(
                    e2e, 'run', return_value=subprocess.CompletedProcess([], 0, key + '\n')):
                self.assertEqual(e2e.profile_security('test-profile'), expected)
        with mock.patch.object(e2e, 'run', return_value=subprocess.CompletedProcess([], 0, 'sae\n')):
            with self.assertRaisesRegex(RuntimeError, 'Unsupported profile authentication'):
                e2e.profile_security('test-profile')

    def test_restore_band_after_success_or_timeout(self):
        for original in ('', 'bg', 'a'):
            for timeout in (False, True):
                with self.subTest(original=original, timeout=timeout):
                    outputs = [subprocess.CompletedProcess([], 0, original + '\n'),
                               subprocess.CompletedProcess([], 0, ''),
                               subprocess.CompletedProcess([], 0, '')]
                    with mock.patch.object(e2e, 'run', side_effect=outputs) as run:
                        try:
                            with e2e.profile_band('test-profile', 'a'):
                                if timeout:
                                    raise subprocess.TimeoutExpired('activation', 60)
                        except subprocess.TimeoutExpired:
                            self.assertTrue(timeout)
                        self.assertEqual(run.call_args_list[-1], mock.call(
                            ['nmcli', 'connection', 'modify', 'uuid', 'test-profile',
                             '802-11-wireless.band', original]))

    def test_restore_band_even_if_selection_times_out(self):
        outputs = [subprocess.CompletedProcess([], 0, 'bg\n'),
                   subprocess.TimeoutExpired('modify', 30),
                   subprocess.CompletedProcess([], 0, '')]
        with mock.patch.object(e2e, 'run', side_effect=outputs) as run:
            with self.assertRaises(subprocess.TimeoutExpired):
                with e2e.profile_band('test-profile', 'a'):
                    self.fail('Timed out selection must not activate the profile')
            self.assertEqual(run.call_args_list[-1].args[0][-1], 'bg')

    def test_restoration_failure_prevents_a_pass(self):
        outputs = [subprocess.CompletedProcess([], 0, 'bg\n'),
                   subprocess.CompletedProcess([], 0, ''),
                   subprocess.CompletedProcess([], 1, '')]
        with mock.patch.object(e2e, 'run', side_effect=outputs):
            with self.assertRaisesRegex(RuntimeError, 'Profile band restoration failed'):
                with e2e.profile_band('test-profile', 'a'):
                    pass

    def test_new_fault_markers_are_terminal(self):
        for message in ('WARNING: suspicious RCU usage', 'UBSAN: out-of-bounds',
                        'hung_task', 'task worker blocked for more than 120 seconds',
                        'WIFI_FLR_REFUSED_OR_FAILED error=-16'):
            with self.subTest(message=message):
                self.assertIsNotNone(e2e.fault.search(message))


class DriverLogTest(unittest.TestCase):
    LOG = ('[   10.000001] mt7932-fullmac 0000:01:00.0: probe message\n'
           '[   20.500000] usb 1-1: new high-speed USB device\n'
           '[   21.000000] mt7932-fullmac 0000:01:00.0: SCAN_REQUEST: channels=38\n'
           '[   21.250000] mt7932_bt_pcie 0000:01:00.1: controller recovered\n'
           '[   22.000000] Bluetooth: hci0: unrelated core message\n')

    def test_last_log_time(self):
        self.assertEqual(e2e.last_log_time(self.LOG), 22.0)
        self.assertEqual(e2e.last_log_time(''), 0.0)

    def test_only_radio_messages_after_start_count(self):
        self.assertEqual(len(e2e.driver_info_messages(self.LOG, 0.0)), 3)
        self.assertEqual(e2e.driver_info_messages(self.LOG, 20.5),
                         ['[   21.000000] mt7932-fullmac 0000:01:00.0: ',
                          '[   21.250000] mt7932_bt_pcie 0000:01:00.1: '])
        self.assertEqual(e2e.driver_info_messages(self.LOG, 22.0), [])

    def test_quiet_log_has_no_driver_messages(self):
        self.assertEqual(e2e.driver_info_messages('[    1.000000] usb 1-1: reset\n', 0.0), [])


if __name__ == '__main__':
    unittest.main()
