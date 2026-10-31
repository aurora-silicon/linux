#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise orchestration with fake decoders. No video device is opened."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).with_name("run-fluster.py")
spec = importlib.util.spec_from_file_location("runner", RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class FlusterTest(unittest.TestCase):
    def test_format_marks(self):
        self.assertEqual(runner.format_marks("VP9", "yuv420p10le"), [])
        self.assertEqual(len(runner.format_marks("VP9", "yuv444p12le")), 2)
        self.assertIn("P210", runner.format_marks("H.264", "yuv422p10le")[0])
        self.assertTrue(runner.format_marks("AV1", "gray"))

    def test_continue_and_preserve(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fluster = root / "fluster"
            suites = fluster / "test_suites"
            suites.mkdir(parents=True)
            baseline = {"suites": {}, "vector_marks": {
                "suite0/vector": {"marks": ["hw-unsupported: H.264 interlace"]}}}
            for i in range(6):
                name = f"suite{i}"
                baseline["suites"][name] = {"passed": 0, "total": 1}
                (suites / f"{name}.json").write_text(json.dumps({
                    "name": name, "codec": "H.264", "test_vectors": [
                        {"name": "vector", "input_file": "input.264", "output_format": "yuv420p"}]}))
            (fluster / "fluster.py").write_text('''
import pathlib, sys
args = sys.argv
name = args[args.index('--testsuites') + 1]
path = pathlib.Path(args[args.index('--summary-output') + 1])
path.write_text('vector,result\\nvector,FAIL\\n')
print('fake decoder ' + name)
sys.exit(1 if name == 'suite1' else 0)
''')
            baseline_path = root / "baseline.json"
            baseline_path.write_text(json.dumps(baseline))
            results = root / "results"
            cmd = [sys.executable, str(RUNNER), "--fluster", str(fluster),
                   "--resources", str(root / "resources"), "--results", str(results),
                   "--baseline", str(baseline_path)]
            plan = subprocess.run(cmd + ["--plan"], capture_output=True, text=True, check=True)
            self.assertEqual(len(plan.stdout.splitlines()), 6)
            self.assertFalse(results.exists())
            run = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertEqual(run.returncode, 1, run.stderr)
            manifest = json.loads((results / "run.json").read_text())
            self.assertEqual([r["rc"] for r in manifest["runs"]], [0, 1, 0, 0, 0, 0])
            self.assertEqual(manifest["baseline"], baseline)
            self.assertIn("hw-unsupported", (results / "vectors.csv").read_text())
            self.assertIn("not inspected", (results / "vectors.csv").read_text())
            final = (results / "suite5/results.csv").read_bytes()
            rerun = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertNotEqual(rerun.returncode, 0)
            self.assertEqual(final, (results / "suite5/results.csv").read_bytes())
            # Changed vector counts cannot silently reuse the old denominator.
            fixture = suites / "suite0.json"
            changed = json.loads(fixture.read_text())
            changed["test_vectors"] = []
            fixture.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, "catalogue changed"):
                runner.load_suites(suites, baseline)


if __name__ == "__main__":
    unittest.main()
