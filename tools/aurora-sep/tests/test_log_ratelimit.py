# SPDX-License-Identifier: GPL-2.0-only
"""Request-path messages in apple_sep are rate-limited.

Anyone who can open /dev/sep-key or /dev/sep-bio can make the driver refuse
a request as often as they like. A message printed per request therefore
goes through the rate-limited macros of drivers/soc/apple/ratelimit.rs (or
dev_dbg!), so a caller cannot flood the kernel log and hide real warnings.
An expected refusal of something the caller supplied is not a warning: it
uses dev_info_refusal! (once a minute per reason). Only modules that run at
probe time alone, and messages behind a *_trace module parameter, may use
the plain macros.
"""
from pathlib import Path
import re
import unittest

APPLE = Path(__file__).resolve().parents[3] / "drivers/soc/apple"
# Device-tree parsing, shared-memory layout and the platform profile run once,
# from probe, before any character device exists.
PROBE_ONLY = {"dt.rs", "shmem.rs", "profile.rs", "ratelimit.rs"}
PLAIN = re.compile(r"\b(?:dev|pr)_(?:err|warn|notice|info)!\(")
TRACE = re.compile(r"\b\w+_trace\.value\(\) != 0")
PLAIN_C = re.compile(r"\b(?:dev|pr)_(?:err|warn|notice|info)\(")


def modules():
    sep = (APPLE / "sep.rs").read_text()
    names = re.findall(r"^\s*(?:pub(?:\(crate\))? )?mod (\w+);", sep, re.M)
    return ["sep.rs"] + [f"{n}.rs" for n in names if (APPLE / f"{n}.rs").exists()]


def code(text):
    """`text` without // comments, so a mention in a comment is not a call."""
    return re.sub(r"//[^\n]*", "", text)


def message_call(text, message):
    """The macro name that prints the string literal starting with `message`."""
    at = text.index(message)
    calls = list(re.finditer(r"\b(\w+)!\(", text[max(0, at - 400):at]))
    if not calls:
        raise AssertionError(f"no macro before {message!r}")
    return calls[-1].group(1)


class LogRateLimit(unittest.TestCase):
    def test_macros_are_in_scope_for_every_module(self):
        sep = (APPLE / "sep.rs").read_text()
        first = re.search(r"^\s*(#\[macro_use\]\s*)?mod (\w+);", sep, re.M)
        self.assertEqual(first.group(2), "ratelimit")
        self.assertTrue(first.group(1), "mod ratelimit needs #[macro_use]")
        rl = (APPLE / "ratelimit.rs").read_text()
        for name in ("dev_err", "dev_warn", "dev_notice", "dev_info", "pr_err", "pr_warn", "pr_notice", "pr_info"):
            self.assertIn(f"macro_rules! {name}_ratelimited", rl)
        self.assertIn("macro_rules! dev_info_refusal", rl)
        self.assertIn("static RATELIMIT", rl, "each call site needs its own state")
        self.assertIn("kunit_tests(apple_sep_ratelimit)", rl)

    def test_request_path_modules_use_rate_limited_messages(self):
        bad = []
        for name in modules():
            if name in PROBE_ONLY:
                continue
            lines = code((APPLE / name).read_text()).splitlines()
            for n, line in enumerate(lines):
                if PLAIN.search(line) and not any(TRACE.search(l) for l in lines[max(0, n - 3):n]):
                    bad.append(f"{name}:{n + 1}: {line.strip()}")
        self.assertEqual(bad, [], "use dev_*_ratelimited!/pr_*_ratelimited! or dev_dbg!")

    def test_per_request_c_helpers_are_rate_limited(self):
        p256 = code((APPLE / "p256_shim.c").read_text())
        self.assertEqual(PLAIN_C.findall(p256), [])
        sensor = (APPLE / "sensor_shim.c").read_text()
        for message in ('"apple_sep: sensor: SMC rail rollback failed', '"apple_sep: sensor: SMC rail write failed'):
            at = sensor.index(message)
            self.assertTrue(sensor[:at].rstrip().endswith("pr_err_ratelimited("), message)

    def test_expected_refusals_are_not_warnings(self):
        # A record, sealed blob or password the caller supplies and the
        # enclave refuses is an expected outcome, not a driver fault.
        cases = {
            "refkey.rs": ['"sks: ref-key sign refused by the enclave',
                          '"sks: ref-key key exchange refused by the enclave',
                          '"trusted-keys: ref-key unseal failed'],
            "key.rs": ['"sep-key: uid {}\'s password refused by the enclave'],
            "sks.rs": ['"sks: LOAD_KEYBAG (user bag): status',
                       '"sks: LOAD_KEYBAG (user bag): operation status'],
            "sbio.rs": ['"verify: Touch ID waits for the first password since this boot; refusing',
                        '"verify: Touch ID is locked after too many failed attempts; refusing'],
        }
        for name, messages in cases.items():
            text = (APPLE / name).read_text()
            for message in messages:
                self.assertEqual(message_call(text, message), "dev_info_refusal", message)

    def test_per_request_narrative_is_debug(self):
        cases = {
            "key.rs": ['"sep-key: waiting for a Touch ID match', '"sep-key: touch-gated {:?} ended',
                       '"sep-key: waiting operation cancelled', '"sep-key: password-gated {:?} under uid'],
            "refkey.rs": ['"sks: ref-key identity unlock mailbox status', '"sks: ref-key create mailbox status'],
            "sks.rs": ['"sks: LOAD_KEYBAG reply mailbox status'],
            "sbio.rs": ['"sep-key: catacomb id {} bytes'],
            "trusted.rs": ['"trusted-keys: sealed {}-byte key', '"trusted-keys: unsealed {}-byte enclave blob'],
        }
        for name, messages in cases.items():
            text = (APPLE / name).read_text()
            for message in messages:
                self.assertEqual(message_call(text, message), "dev_dbg", message)


if __name__ == "__main__":
    unittest.main()
