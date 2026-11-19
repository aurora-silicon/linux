"""Kernel completion evidence for bounded M3 replays; never trust process liveness."""
import os
import re
import select
import threading
import time

RENDER = re.compile(r'M3_RETIRED draw=(\d+) ta=([0-9a-f]+)/([0-9a-f]+) fragment=([0-9a-f]+)/([0-9a-f]+) queues=(\d+)/(\d+) read=(\d+)/(\d+) idle=1 cache_flush=1 events=2 faults=0 ordered_barrier=1 batch=(\d+)')
COMPUTE = re.compile(r'M3_COMPUTE_RETIRED sequence=(\d+) context=\d+ stamp=0x([0-9a-f]+)/0x([0-9a-f]+) queue=(\d+)/(\d+) head=(\d+) idle=1 cache_flush=1')
FAULT = re.compile(r'execution failed|firmware error event|firmware crashed|GPU fault|BUG:|Oops:|SError|Kernel panic', re.I)

def completion(text):
    if FAULT.search(text):
        raise RuntimeError('Kernel fault during progress-guarded replay')
    m = RENDER.search(text)
    if m:
        v = m.groups()
        assert v[1] == v[2] and v[3] == v[4], 'Unmatched render stamps'
        assert v[5:7] == v[7:9], 'Unconsumed render queue'
        assert 1 <= int(v[9]) <= 16, 'Invalid batch count'
        return 'render', int(v[0])
    m = COMPUTE.search(text)
    if m:
        v = m.groups()
        assert v[1] == v[2], 'Unmatched compute stamp'
        assert v[3] == v[4] == v[5], 'Unconsumed compute queue'
        return 'compute', int(v[0])
    return None

class RetirementProgress:
    def __init__(self, evidence):
        self.fd = os.open('/dev/kmsg', os.O_RDONLY | os.O_NONBLOCK)
        os.lseek(self.fd, 0, os.SEEK_END)
        self.evidence = evidence
        self.sequence = None
        self.engines = {}
        self.count = 0
        self.latest = None
        self.error = None
        self.lock = threading.Lock()
        self.stop = threading.Event()
        # A CTS context-creation burst can wrap kmsg during the thermal loop's
        # 0.5-second sleep. Drain independently; this thread never feeds the
        # watchdog. Any loss/parser/I/O error still fails the guard closed.
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _record(self, raw):
        header, message = raw.split(';', 1)
        _, sequence, micros, *_ = header.split(',')
        sequence, when = int(sequence), int(micros) / 1e6
        if self.sequence is not None:
            assert sequence == self.sequence + 1, 'Kernel progress log lost records'
        self.sequence = sequence
        event = completion(message)
        if event is None:
            return
        engine, value = event
        assert value > self.engines.get(engine, -1), 'Retirement counter regressed'
        # printk uses sched_clock (raw time), not NTP-adjusted MONOTONIC.
        age = time.clock_gettime(time.CLOCK_MONOTONIC_RAW) - when
        assert -.01 <= age <= 2, ('Stale kernel retirement', age, when)
        self.engines[engine] = value
        with self.evidence.open('a') as log:
            log.write(raw)
        with self.lock:
            self.count += 1
            self.latest = time.monotonic() - max(age, 0)

    def _reader(self):
        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([self.fd], [], [], .05)
                if not ready:
                    continue
                try:
                    raw = os.read(self.fd, 65536).decode(errors='strict')
                except BlockingIOError:
                    continue
                if not raw:
                    raise RuntimeError('Kernel progress stream closed')
                self._record(raw)
        except BaseException as exc:
            with self.lock:
                self.error = exc

    def poll(self, now):
        with self.lock:
            if self.error is not None:
                raise self.error
            if not self.thread.is_alive():
                raise RuntimeError('Kernel progress reader stopped')
            return self.count, self.latest

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        assert not self.thread.is_alive(), 'Kernel progress reader did not stop'
        os.close(self.fd)
