"""Offline watchdog checks: idle GPU, faults, reloads, stale and malformed state."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from completion_progress import CompletionProgress, parse


def snapshot(count=7, stamp=9_000_000_000, generation=1, healthy=1):
    return f'version=1 generation_ns={generation} completed={count} last_completion_ns={stamp} healthy={healthy}\n'


class ProgressTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)/'progress'
        self.path.write_text(snapshot())
        self.reader = CompletionProgress(Path(self.directory.name)/'evidence', self.path)

    def test_existing_completion_is_not_new_progress(self):
        self.assertEqual(self.reader.poll(100), (0, None))

    def test_new_completion_and_idle(self):
        self.path.write_text(snapshot(8))
        with patch('completion_progress.time.monotonic', return_value=10):
            self.assertEqual(self.reader.poll(10), (1, 9))
        self.assertEqual(self.reader.poll(100), (1, 9))

    def test_timestamp_alone_never_advances_count(self):
        self.path.write_text(snapshot(stamp=10_000_000_000))
        self.assertEqual(self.reader.poll(10), (0, None))

    def test_fault_reload_regression_staleness(self):
        for sample in [snapshot(healthy=0), snapshot(generation=2), snapshot(6),
                       snapshot(stamp=8_000_000_000), snapshot(8, 10_000_000_001),
                       snapshot(8, 7_000_000_000)]:
            with self.subTest(sample=sample):
                self.path.write_text(sample)
                with patch('completion_progress.time.monotonic', return_value=10):
                    with self.assertRaises(RuntimeError): self.reader.poll(10)

    def test_missing_file_and_malformed_snapshot(self):
        self.path.unlink()
        with self.assertRaises(FileNotFoundError): self.reader.poll(10)
        for sample in ['',snapshot().replace('healthy=1','version=1'),
                       snapshot().replace('version=1','version=2'),
                       snapshot(count=-1),snapshot(1,0)]:
            with self.subTest(sample=sample),self.assertRaises((RuntimeError,ValueError)):
                parse(sample)

if __name__=='__main__': unittest.main()
