# SPDX-License-Identifier: GPL-2.0-only
"""The sensor status is read again after registration on every profile.

On an M1 Pro (T6000/J316s) the unified kernels lost the sensor attach on 4
boots in 15: right after registration the session share was dropped by the
sensor or its reply refused by the enclave (SESSION_COMMIT 0xfffffffe), and
every later attempt that boot was refused SESSION_SHARE with 0x1. They had
stopped reading the sensor status after registration on profiles without a
nonce session. The 7.1 and 7.2.10 kernels that read it on every profile
never failed in 13 boots. Guard the read, and that nothing reaches the
sensor or the patch step before it.
"""

import re
import unittest
from pathlib import Path

SBIO = Path(__file__).resolve().parents[3] / 'drivers/soc/apple/sbio.rs'


def method(source, name):
    match = re.search(r'\n    (pub\(crate\) )?fn ' + name + r'\b', source)
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[opening:end]


class SensorRecheckTest(unittest.TestCase):
    def test_status_is_read_after_registration_on_every_profile(self):
        body = method(SBIO.read_text(), 'wake_sensor')
        after = body[body.index('self.register_sensor(&id)'):]
        read = after.index('sensor::status()')
        # Nothing between the registration and the read: no profile branch,
        # no patch step, no other sensor request.
        between = after[:read]
        self.assertNotIn('nonce_session', between)
        self.assertNotIn('apply_sensor_patch', between)
        self.assertNotIn('PatchLoaded', between)
        self.assertEqual(between.count('sensor::'), 0, between)
        # The decision uses that read, through registration::patch_loaded.
        decision = after[read:]
        self.assertIn('registration::patch_loaded(', decision)
        self.assertLess(decision.index('registration::patch_loaded('),
                        decision.index('apply_sensor_patch'))


if __name__ == '__main__':
    unittest.main()
