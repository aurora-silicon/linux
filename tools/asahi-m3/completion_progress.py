"""Watchdog evidence from host-verified GPU completion state, independent of logs."""
import json
from pathlib import Path
import time

PATH = Path('/sys/kernel/debug/asahi-m3/progress')
FIELDS = {'version', 'generation_ns', 'completed', 'last_completion_ns', 'healthy'}


def parse(text):
    fields = text.split()
    pairs = [part.split('=') for part in fields]
    if len(pairs) != len(FIELDS) or any(len(p) != 2 for p in pairs):
        raise RuntimeError('Malformed GPU progress snapshot')
    values = {k: int(v) for k, v in pairs}
    if set(values) != FIELDS or any(v < 0 for v in values.values()):
        raise RuntimeError('Invalid GPU progress fields')
    if values['version'] != 1:
        raise RuntimeError('GPU progress ABI mismatch')
    if values['healthy'] not in (0, 1):
        raise RuntimeError('Invalid GPU health value')
    if values['generation_ns'] == 0:
        raise RuntimeError('Missing GPU runtime generation')
    if values['completed'] and values['last_completion_ns'] == 0:
        raise RuntimeError('Completion without timestamp')
    return values


class CompletionProgress:
    def __init__(self, evidence, path=PATH):
        self.path = path
        self.evidence = evidence
        self.previous = self._read()
        self.baseline = self.previous['completed']
        self.latest = None

    def _read(self):
        # Reading accesses host atomics only, never GPU registers or its worker
        # mutex. A missing/unloaded endpoint fails closed; no printk fallback.
        with self.path.open() as f:
            text = f.read(1025)
        if len(text) > 1024:
            raise RuntimeError('Oversized GPU progress snapshot')
        sample = parse(text)
        # Retain the terminal snapshot before rejecting it. Evidence collection
        # cannot make an unhealthy sample eligible to renew the watchdog.
        self._record(sample)
        if not sample['healthy']:
            raise RuntimeError('GPU runtime latched unhealthy')
        return sample

    def _record(self, sample):
        with self.evidence.open('a') as f:
            f.write(json.dumps(dict(monotonic=time.monotonic(), **sample))+'\n')

    def poll(self, now):
        sample = self._read()
        old = self.previous
        if sample['generation_ns'] != old['generation_ns']:
            raise RuntimeError('GPU runtime replaced during experiment')
        if sample['completed'] < old['completed'] or sample['last_completion_ns'] < old['last_completion_ns']:
            raise RuntimeError('GPU completion state regressed')
        if sample['completed'] > old['completed']:
            # Kernel and userspace both use CLOCK_MONOTONIC, not printk's raw
            # clock. Only genuinely new, recent verified retirements can renew.
            when = sample['last_completion_ns']/1e9
            age = time.monotonic()-when
            if not 0 <= age <= 2:
                raise RuntimeError(('Stale GPU completion', age))
            self.latest = when
        self.previous = sample
        return sample['completed']-self.baseline, self.latest

    def close(self):
        pass
