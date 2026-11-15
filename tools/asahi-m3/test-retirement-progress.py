#!/usr/bin/env python3
"""Offline fail-closed checks for the M3 replay watchdog evidence parser."""
from pathlib import Path
import tempfile
import os
import socket
import time
import threading
import unittest
from unittest.mock import patch
from retirement_progress import completion, RetirementProgress

RENDER = 'M3_RETIRED draw=128 ta=7a008000/7a008000 fragment=3d008000/3d008000 queues=2/3 read=2/3 idle=1 cache_flush=1 events=2 faults=0 ordered_barrier=1 batch=1'
COMPUTE = 'M3_COMPUTE_RETIRED sequence=128 context=14 stamp=0xc0018000/0xc0018000 queue=0/0 head=0 idle=1 cache_flush=1'

class ProgressTests(unittest.TestCase):
    def test_real_retirements(self):
        self.assertEqual(completion(RENDER), ('render',128))
        self.assertEqual(completion(COMPUTE), ('compute',128))

    def test_stamp_and_queue_disagreement(self):
        for text in [RENDER.replace('read=2/3','read=3/3'),
                     RENDER.replace('/7a008000','/7a008001'),
                     COMPUTE.replace('head=0','head=1')]:
            with self.subTest(text=text), self.assertRaises(AssertionError):
                completion(text)

    def test_fault(self):
        with self.assertRaises(RuntimeError):
            completion('M3 scheduler: execution failed ENOMEM')

    def test_unrelated_liveness_does_not_count(self):
        self.assertIsNone(completion('CPU alive'))
        self.assertIsNone(completion(RENDER.replace('idle=1','idle=0')))

    def poll(self, messages, monotonic=10):
        with tempfile.TemporaryDirectory() as directory:
            r=RetirementProgress.__new__(RetirementProgress)
            r.fd=123; r.sequence=None; r.engines={}; r.count=0
            r.latest=None; r.error=None; r.lock=threading.Lock()
            r.evidence=Path(directory)/'records'
            with patch('retirement_progress.time.monotonic',return_value=monotonic), \
                 patch('retirement_progress.time.clock_gettime',return_value=10):
                for raw in messages:
                    r._record(raw)
                return r.count, r.latest

    def test_reader_error_is_fatal(self):
        r=RetirementProgress.__new__(RetirementProgress)
        r.lock=threading.Lock(); r.error=BrokenPipeError('kmsg overflow')
        with self.assertRaises(BrokenPipeError):
            r.poll(10)

    def test_dead_reader_is_fatal(self):
        r=RetirementProgress.__new__(RetirementProgress)
        r.lock=threading.Lock(); r.error=None
        r.thread=threading.Thread(target=lambda: None)
        with self.assertRaises(RuntimeError):
            r.poll(10)

    def test_reader_drains_burst_without_guard_polling(self):
        # Datagram socket preserves the record boundaries of /dev/kmsg.
        receiver, sender = socket.socketpair(type=socket.SOCK_DGRAM)
        with tempfile.TemporaryDirectory() as directory:
            fd=os.dup(receiver.fileno())
            with patch('retirement_progress.os.open',return_value=fd), \
                 patch('retirement_progress.os.lseek'):
                r=RetirementProgress(Path(directory)/'records')
            try:
                for i in range(1,501):
                    micros=int(time.clock_gettime(time.CLOCK_MONOTONIC_RAW)*1e6)
                    text=COMPUTE.replace('sequence=128',f'sequence={i}')
                    sender.send(f'6,{i},{micros},-;{text}\n'.encode())
                deadline=time.monotonic()+2
                while r.count<500 and r.error is None and time.monotonic()<deadline:
                    time.sleep(.001)
                # The thermal/watchdog loop has not called poll once yet.
                self.assertEqual(r.poll(time.monotonic())[0],500)
                self.assertEqual(len(r.evidence.read_text().splitlines()),500)
            finally:
                r.close(); receiver.close(); sender.close()

    def test_fresh_records(self):
        self.assertEqual(self.poll(['6,1,9000000,-;'+RENDER,
                                    '6,2,9500000,-;'+COMPUTE]),(2,9.5))

    def test_raw_clock_offset(self):
        self.assertEqual(self.poll(['6,1,9500000,-;'+RENDER],monotonic=8),(1,7.5))

    def test_bounded_sched_clock_skew(self):
        self.assertEqual(self.poll(['6,1,10001000,-;'+RENDER]),(1,10))
        with self.assertRaises(AssertionError):
            self.poll(['6,1,10100000,-;'+RENDER])

    def test_stale_record(self):
        with self.assertRaises(AssertionError):
            self.poll(['6,1,3000000,-;'+RENDER])

    def test_missing_record(self):
        with self.assertRaises(AssertionError):
            self.poll(['6,1,9000000,-;'+RENDER,'6,3,9500000,-;'+COMPUTE])

    def test_duplicate_retirement(self):
        with self.assertRaises(AssertionError):
            self.poll(['6,1,9000000,-;'+RENDER,'6,2,9500000,-;'+RENDER])

if __name__ == '__main__':
    unittest.main()
