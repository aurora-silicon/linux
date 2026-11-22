#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""aurora-sep-ready names the authselect step only when fingerprints are off."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

READY = Path(__file__).resolve().parents[1] / "aurora-sep-ready"
COMMAND = "sudo authselect enable-feature with-fingerprint"


def function(source, name):
    match = re.search(r"^%s\(\) \{\n.*?^\}\n" % name, source, re.M | re.S)
    if match is None:
        raise AssertionError("no %s() in aurora-sep-ready" % name)
    return match.group(0)


class PamHintTest(unittest.TestCase):
    def hint(self, authselect, **extra_env):
        """Runs pam_hint() with `authselect` as the stub's body, or none."""
        source = READY.read_text()
        script = function(source, "log") + function(source, "pam_hint") + "pam_hint\n"
        with tempfile.TemporaryDirectory() as bindir:
            if authselect is not None:
                stub = Path(bindir) / "authselect"
                stub.write_text("#!/bin/sh\n" + authselect)
                stub.chmod(0o755)
            env = dict(os.environ, PATH=bindir + ":/usr/bin:/bin")
            env.pop("AURORA_SEP_OFFER_FINGERPRINT", None)
            env.update(extra_env)
            if authselect is None:
                env["PATH"] = bindir
            result = subprocess.run(
                ["/bin/sh", "-c", script], env=env, capture_output=True, text=True
            )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        return result.stderr

    def test_off_names_the_command_once(self):
        lines = self.hint('echo "local with-silent-lastlog with-mdns4"\n').splitlines()
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("aurora-sep-ready: "))
        self.assertTrue(lines[0].endswith(COMMAND))

    def test_on_is_silent(self):
        self.assertEqual(self.hint('echo "local with-fingerprint with-mdns4"\n'), "")
        self.assertEqual(self.hint('echo "sssd with-mdns4 with-fingerprint"\n'), "")

    def test_a_similar_feature_is_not_mistaken_for_it(self):
        self.assertIn(COMMAND, self.hint('echo "local with-fingerprint-x"\n'))

    def test_no_authselect_configuration_is_silent(self):
        self.assertEqual(self.hint('echo "No existing configuration detected." >&2\nexit 2\n'), "")

    def test_no_authselect_is_silent(self):
        self.assertEqual(self.hint(None), "")

    # A distribution that sets AURORA_SEP_OFFER_FINGERPRINT=1 has the feature
    # turned on once, and a later "disable-feature" is left alone.
    OFFER_STUB = (
        'case "$1" in\n'
        'current) cat "$STATE" ;;\n'
        'enable-feature) echo "local $2" >"$STATE" ;;\n'
        'esac\n'
    )

    def offer(self, state_dir, features):
        state = Path(state_dir) / "features"
        state.write_text(features + "\n")
        log = self.hint(self.OFFER_STUB, STATE=str(state), AURORA_SEP_STATE_DIR=state_dir,
                        AURORA_SEP_OFFER_FINGERPRINT="1")
        return log, state.read_text().split()

    def test_offer_turns_the_feature_on_once(self):
        with tempfile.TemporaryDirectory() as state_dir:
            log, features = self.offer(state_dir, "local with-mdns4")
            self.assertIn("with-fingerprint", features)
            self.assertIn("fingerprint sign-in is available", log)
            marker = Path(state_dir) / "fingerprint-sign-in-offered"
            self.assertTrue(marker.exists())
            # The user turned it off again: only name the command.
            log, features = self.offer(state_dir, "local with-mdns4")
            self.assertNotIn("with-fingerprint", features)
            self.assertTrue(log.strip().endswith(COMMAND))

    def test_offer_records_a_feature_that_is_already_on(self):
        with tempfile.TemporaryDirectory() as state_dir:
            log, _ = self.offer(state_dir, "local with-fingerprint")
            self.assertEqual(log, "")
            self.assertTrue((Path(state_dir) / "fingerprint-sign-in-offered").exists())

    def test_without_the_offer_nothing_is_changed(self):
        with tempfile.TemporaryDirectory() as state_dir:
            log = self.hint('echo "local with-mdns4"\n', AURORA_SEP_STATE_DIR=state_dir)
            self.assertTrue(log.strip().endswith(COMMAND))
            self.assertEqual(os.listdir(state_dir), [])


if __name__ == "__main__":
    unittest.main()
