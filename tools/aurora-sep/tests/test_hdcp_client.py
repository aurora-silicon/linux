# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the Apple SEP HDCP 2.2 transmitter client."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
APPLE = ROOT / "drivers/soc/apple"
HEADER = ROOT / "include/linux/apple-sep-hdcp.h"


class HdcpClientChecks(unittest.TestCase):
    def test_header_has_disabled_stubs_for_every_declared_api(self):
        text = HEADER.read_text()
        self.assertIn("#if IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE)", text)
        self.assertIn("#else /* !IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE) */", text)
        enabled, disabled = text.split(
            "#else /* !IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE) */", 1
        )
        decl_re = re.compile(r"\b(apple_sep_hdcp_\w+)\s*\(")
        enabled_fns = set(decl_re.findall(enabled))
        disabled_fns = set(decl_re.findall(disabled))
        self.assertGreaterEqual(len(enabled_fns), 13)
        self.assertEqual(enabled_fns, disabled_fns)

    def test_sep_wires_hdcp_lifecycle_and_forgets_registered_ool(self):
        sep = (APPLE / "sep.rs").read_text()
        self.assertIn("mod hdcp;", sep)
        self.assertIn("hdcp::EP_HDCP => self.on_hdcp(msg),", sep)
        self.assertIn("self.hdcp_discovered(&f);", sep)
        self.assertIn("data.register_hdcp_kernel()", sep)
        self.assertIn("self.unregister_hdcp_kernel();", sep)
        remove_fn = sep[sep.index("fn remove(&self)"):]
        self.assertIn("self.hdcp_wq.notify_all();", remove_fn)
        self.assertIn("&self.ool_hdcp,", remove_fn)

    def test_hdcp_uses_runtime_enumeration_and_ool_sync(self):
        hdcp = (APPLE / "hdcp.rs").read_text()
        self.assertIn("Command::QueryChannels", hdcp)
        self.assertIn("Command::GetChannelInfo", hdcp)
        self.assertIn("ool_repoison(&sep.ool_hdcp", hdcp)
        self.assertIn("ool_await_written(&sep.ool_hdcp", hdcp)
        self.assertIn("hdcp_scrub_buffers", hdcp)
        self.assertIn("PHASE_READY", hdcp)
        self.assertIn("pm::SleepGate::Open", hdcp)

    def test_shim_scrubs_buffers(self):
        shim = (APPLE / "hdcp_shim.c").read_text()
        self.assertIn("kfree_sensitive(in);", shim)
        self.assertIn("memzero_explicit(", shim)


if __name__ == "__main__":
    unittest.main()
