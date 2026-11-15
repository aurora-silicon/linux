"""Real HTTP resume behavior and checksum admission before package transactions."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
import test_m3_flow as flow

DATA = bytes(range(256)) * 256


class DownloadTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / 'package'
        self.requests = []
        self.mode = 'normal'
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                start = int(self.headers.get('Range', 'bytes=0-')[6:].split('-')[0])
                owner.requests.append(start)
                mode = owner.mode
                if mode.isdigit():
                    self.send_response(int(mode)); self.send_header('Content-Length', '0'); self.end_headers(); return
                if start >= len(DATA):
                    self.send_response(416); self.send_header('Content-Range', f'bytes */{len(DATA)}')
                    self.send_header('Content-Length', '0'); self.end_headers(); return
                if mode == 'ignore':
                    start = 0
                self.send_response(206 if start else 200)
                if start:
                    self.send_header('Content-Range', f'bytes {start}-{len(DATA)-1}/{len(DATA)}')
                self.send_header('Content-Length', str(len(DATA)-start)); self.end_headers()
                if mode == 'cut' and len(owner.requests) == 1:
                    self.wfile.write(DATA[:4096]); self.wfile.flush(); self.close_connection = True
                else:
                    self.wfile.write(DATA[start:])
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True); self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def fetch(self, extra=''):
        url = f'http://127.0.0.1:{self.server.server_port}/package'
        return subprocess.run(['bash', '-c',
            'set -e; export AURORA_SEP_SOURCE_ONLY=1; source "$1"; sleep(){ :; }; '+(extra+'; ' if extra else '')+
            'fetch_release_file "$2" "$3" && rc=0 || rc=$?; exit "$rc"',
            'test', str(flow.INSTALLER), url, str(self.out)], capture_output=True, text=True, timeout=15)

    def test_cut_transfer_resumes_with_range(self):
        self.mode = 'cut'
        result = self.fetch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.out.read_bytes(), DATA)
        self.assertEqual(self.requests, [0, 4096])

    def test_ignored_range_restarts_from_zero(self):
        self.out.write_bytes(DATA[:4096]); self.mode = 'ignore'
        result = self.fetch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.out.read_bytes(), DATA)
        self.assertEqual(self.requests, [4096, 0])

    def test_complete_file_416_retains_checksum_candidate(self):
        self.out.write_bytes(DATA)
        self.assertEqual(self.fetch().returncode, 0)
        self.assertEqual(self.out.read_bytes(), DATA)
        self.assertEqual(self.requests, [len(DATA)])

    def test_missing_file_is_not_retried(self):
        self.mode = '404'
        self.assertEqual(self.fetch().returncode, 22)
        self.assertEqual(len(self.requests), 1)

    def test_authorization_errors_are_not_missing_files_or_retried(self):
        for code in ('401', '403'):
            with self.subTest(code=code):
                self.mode = code; self.requests.clear()
                self.assertEqual(self.fetch().returncode, 101)
                self.assertEqual(len(self.requests), 1)

    def test_busy_server_errors_are_bounded_and_not_missing_assets(self):
        for code in ('408', '429', '500', '503'):
            with self.subTest(code=code):
                self.mode = code; self.requests.clear()
                self.assertEqual(self.fetch().returncode, 100)
                self.assertEqual(len(self.requests), 5)

    def test_transport_resets_and_stalls_retry_with_limits(self):
        # These curl failures have no HTTP response. Verify retry and stall options
        # without waiting for real 30/60-second deadlines in the fixture.
        for code in (18, 28, 92):
            with self.subTest(code=code):
                log = Path(self.temp.name) / 'calls'; log.write_text('')
                stub = 'curl(){ printf "%s\\n" "$*" >>"'+str(log)+'"; return '+str(code)+'; }'
                self.assertEqual(self.fetch(stub).returncode, code)
                calls = log.read_text().splitlines(); self.assertEqual(len(calls), 5)
                for call in calls:
                    self.assertIn('--connect-timeout 30 --speed-limit 1024 --speed-time 60', call)
                    self.assertIn('-C -', call)


class DownloadAdmissionTest(flow.M3FlowBase):
    def setUp(self):
        super().setUp(); self.mac('j293')
        self.extra_env.update(AURORA_RELEASE_URL='', AURORA_RELEASES_API='')

    def test_corrupt_prefix_and_416_never_reach_install(self):
        # Model a complete-length but corrupted resumed package accepted by HTTP.
        for answer in ('200', '416'):
            with self.subTest(answer=answer):
                script = self.tmp / 'bin/curl'
                script.write_text('#!/bin/bash\nout=\nwhile (($#)); do case $1 in -o) out=$2; shift ;; esac; shift; done\nprintf corrupt >"$out"\nprintf '+answer+'\nexit '+('22' if answer=='416' else '0')+'\n')
                script.chmod(0o755)
                run = self.install(check=False)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn('does not match its published checksum', run.stderr)
                self.assertNotIn('pacman -U', self.log())

    def test_exhausted_server_error_reports_network_before_install(self):
        script = self.tmp / 'bin/curl'
        script.write_text('#!/bin/bash\nprintf 503\nexit 22\n'); script.chmod(0o755)
        run = self.run_sh('sleep(){ :; }; install_all', check=False)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('network or a busy server', run.stderr)
        self.assertNotIn('packaging\n    mistake', run.stderr)
        self.assertNotIn('pacman -U', self.log())

    def test_authorization_refusal_does_not_claim_a_packaging_error(self):
        script = self.tmp / 'bin/curl'
        script.write_text('#!/bin/bash\nprintf 403\nexit 22\n'); script.chmod(0o755)
        run = self.install(check=False)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('server refused the request', run.stderr)
        self.assertNotIn('packaging\n    mistake', run.stderr)
        self.assertNotIn('all five tries', run.stderr)
        self.assertNotIn('pacman -U', self.log())
