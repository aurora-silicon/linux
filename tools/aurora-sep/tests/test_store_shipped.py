# SPDX-License-Identifier: MIT
"""The host store's KUnit suite tests against the driver as it shipped.

drivers/soc/apple/store_shipped.rs is drivers/soc/apple/store.rs as it was
before the store validated what it reads, changed only so that the tests can
give it an in-memory file. Undoing exactly those changes must give back the
shipped file, identified by its git blob id.
"""

import hashlib
from pathlib import Path
import unittest

SHIPPED = Path(__file__).resolve().parents[3] / "drivers/soc/apple/store_shipped.rs"
BLOB = "dc37025ce3b8adbde9df751b6169342c532f0c08"

# (as shipped, as in store_shipped.rs)
SEAM = (
    ("use crate::shim;\n",
     "use super::StoreIo;\n"),
    ("pub(crate) struct Store {\n"
     "    file: shim::StoreFile,\n",
     "pub(crate) struct Store<F> {\n"
     "    file: F,\n"),
    ("impl Store {\n"
     "    pub(crate) fn open() -> Result<Store> {\n"
     "        let file = shim::StoreFile::open(STORE_PATH)?;\n"
     "\n",
     "impl<F: StoreIo> Store<F> {\n"
     "    pub(crate) fn open(file: F) -> Result<Self> {\n"),
)


@unittest.skipUnless(SHIPPED.is_file(), "not in a kernel tree")
class Seam(unittest.TestCase):
    def test_only_the_io_seam_differs(self):
        text = SHIPPED.read_text()
        for shipped, changed in SEAM:
            self.assertEqual(text.count(changed), 1, changed)
            text = text.replace(changed, shipped)
        data = text.encode()
        blob = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
        self.assertEqual(blob, BLOB)


if __name__ == "__main__":
    unittest.main()
