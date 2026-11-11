# SPDX-License-Identifier: GPL-2.0-only
"""The first use of an existing identity keybag runs in the boot policy window.

After a boot policy change the enclave rewrites one existing session record
at the keybag's first use: the first unlock, or Touch ID's DESIGNATE_KEYBAG
when Touch ID comes up first (https://github.com/aurora-silicon/linux/issues/1190). The xART guard lets that rewrite
through only inside crate::xart_guard::policy_window, so every designate of
a keybag that already existed has to run in it.
"""
from pathlib import Path
import re
import unittest

APPLE = Path(__file__).resolve().parents[3] / "drivers/soc/apple"
DESIGNATE = "sks_designate_user_keybag("
PREPARE = "sks_prepare_identity_refkey("


def function(text, name):
    """The source of `fn name`, from its signature to its closing brace."""
    match = re.search(rf"\bfn {name}\b", text)
    if match is None:
        raise AssertionError(f"fn {name} not found")
    opening = text.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def functions(text):
    """(name, start, end) of every fn with a body."""
    found = []
    for match in re.finditer(r"\bfn (\w+)\b[^;{]*\{", text):
        depth, end = 1, match.end()
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        found.append((match.group(1), match.start(), end))
    return found


def calls(name):
    """(file, caller, caller's source, offset of the call in it) per call."""
    found = []
    for path in sorted(APPLE.glob("*.rs")):
        text = path.read_text()
        spans = functions(text)
        for match in re.finditer(re.escape(name), text):
            if text[max(0, match.start() - 3):match.start()] == "fn ":
                continue
            # The innermost function around the call.
            caller, start, end = max(
                (s for s in spans if s[1] < match.start() < s[2]),
                key=lambda s: s[1])
            found.append((path.name, caller, text[start:end],
                          match.start() - start))
    return found


def statement(body, at):
    """The statement around offset `at`: from the previous `;`, `{` or `}`."""
    start = max(body.rfind(c, 0, at) for c in ";{}")
    # A closure opened inside the statement does not end it.
    while body[start] == "{" and body[start - 3:start].strip().endswith("||"):
        start = max(body.rfind(c, 0, start) for c in ";{}")
    return body[start + 1:at]


class PolicyWindowTests(unittest.TestCase):
    def setUp(self):
        self.sbio = (APPLE / "sbio.rs").read_text()
        self.activate = function(self.sbio, "activate_touchid")

    def assert_cached_refkey_window(self, body, at):
        # A deferred closure's wrapper comes after its definition. Match its
        # complete dispatch rather than accepting any later window in the fn.
        # Preserve offsets while ignoring words in comments.
        clean = re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: " " * len(m[0]),
                       body, flags=re.S)
        closure = re.search(r"\blet\s+(?P<binding>unlock)\s*=\s*\|\|\s*\{", clean)
        self.assertIsNotNone(closure)
        opening = closure.end() - 1
        depth, end = 1, opening + 1
        while depth and end < len(clean):
            depth += (clean[end] == "{") - (clean[end] == "}")
            end += 1
        self.assertEqual(depth, 0)
        self.assertTrue(opening < at < end)
        self.assertEqual(clean.count(PREPARE), 1)
        # The initrd deliberately leaves the real root's window unused; its
        # guarded store is read-only. Only that branch may call unlock directly.
        dispatch = re.search(
            r"\blet\s+unlocked\s*=\s*if\s+self\.keystore_only\.load\(Relaxed\)\s*"
            r"\{\s*(?P<initrd>unlock)\(\)\s*\}\s*else\s*"
            r"\{\s*crate::xart_guard::policy_window\((?P<root>unlock)\)\s*\};",
            clean)
        self.assertIsNotNone(dispatch)
        self.assertGreater(dispatch.start(), end)
        self.assertEqual(
            [m.start() for m in re.finditer(r"\bunlock\b", clean)],
            [closure.start("binding"), dispatch.start("initrd"), dispatch.start("root")])

    def test_touchid_designates_inside_the_window(self):
        at = self.activate.index(DESIGNATE)
        self.assertIn("crate::xart_guard::policy_window(", statement(self.activate, at))

    def test_touchid_designates_before_the_ref_key_can_be_created(self):
        # Creating the machine ref-key closes the window for the boot.
        self.assertLess(self.activate.index(DESIGNATE),
                        self.activate.index("sks_machine_refkey("))

    def test_touchid_designates_a_keybag_that_already_existed(self):
        recovered = self.activate.index("sks_recover(&stored)")
        self.assertLess(recovered, self.activate.index(DESIGNATE))
        self.assertNotIn("sks_provision_identity_keybag(", self.activate)

    def test_cached_refkey_window_wraps_deferred_unlock(self):
        body = function((APPLE / "refkey.rs").read_text(), "refkey_unseal_cached")
        self.assert_cached_refkey_window(body, body.index(PREPARE))

    def test_cached_refkey_rejects_unguarded_real_root_unlock(self):
        body = function((APPLE / "refkey.rs").read_text(), "refkey_unseal_cached")
        wrapped = "crate::xart_guard::policy_window(unlock)"
        self.assertEqual(body.count(wrapped), 1)
        body = body.replace(wrapped, "unlock()")
        with self.assertRaises(AssertionError):
            self.assert_cached_refkey_window(body, body.index(PREPARE))

    def test_cached_refkey_rejects_extra_unguarded_unlock(self):
        body = function((APPLE / "refkey.rs").read_text(), "refkey_unseal_cached")
        dispatch = "let unlocked = if self.keystore_only.load(Relaxed)"
        self.assertEqual(body.count(dispatch), 1)
        body = body.replace(dispatch, "unlock();\n            " + dispatch)
        with self.assertRaises(AssertionError):
            self.assert_cached_refkey_window(body, body.index(PREPARE))

    def test_no_other_path_designates_outside_a_window(self):
        # A new designate of an existing keybag needs the window too; add it
        # here once it runs in one.
        sites = {(f, outer) for f, outer, _body, _at in calls(DESIGNATE)}
        self.assertEqual(sites, {("sbio.rs", "activate_touchid"),
                                 ("refkey.rs", "sks_prepare_identity_refkey")})
        # The ref-key path designates inside its own unlock's window, or
        # after a ref-key create has closed the window for the boot.
        callers = calls(PREPARE)
        self.assertTrue(callers)
        for f, outer, body, at in callers:
            with self.subTest(caller=outer):
                if (f, outer) == ("refkey.rs", "refkey_unseal_cached"):
                    self.assert_cached_refkey_window(body, at)
                    continue
                before = body[:at]
                self.assertTrue("crate::xart_guard::policy_window(" in before
                                or "crate::xart_guard::close_policy_window(" in before,
                                f"{f}: {outer}")


if __name__ == "__main__":
    unittest.main()
