"""Offline safety and format tests for the bundled calibration extractor."""

import importlib.util
from pathlib import Path
import stat
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "extract-mesa-calibration.py"
SPEC = importlib.util.spec_from_file_location("extract_mesa_calibration", SCRIPT)
extractor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(extractor)


def tlv(tag, content):
    length = len(content)
    if length < 128:
        encoded_length = bytes([length])
    else:
        width = (length.bit_length() + 7) // 8
        encoded_length = bytes([0x80 | width]) + length.to_bytes(width, "big")
    return bytes([tag]) + encoded_length + content


def ia5(value):
    return tlv(0x16, value)


def synthetic_record(with_manifest=True):
    """Build a structural fixture, not a cryptographically signed IMG4."""
    im4p = tlv(0x30, ia5(b"IM4P") + ia5(b"FSCl") + ia5(b"test")
                + tlv(0x04, b"CALB" + b"x" * 64))
    manifest = ia5(b"IM4M") + ia5(b"FSCl") if with_manifest else b""
    img4 = tlv(0x30, ia5(b"IMG4") + im4p + manifest)
    fdrd = tlv(0x30, ia5(b"fdrd") + tlv(0x04, img4))
    secb = tlv(0x30, ia5(b"secb"))
    return tlv(0x30, ia5(b"comb") + fdrd + secb)


class CalibrationExtractorTests(unittest.TestCase):
    def test_scans_only_complete_manifest_marked_record(self):
        record = synthetic_record()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "isc.img"
            source.write_bytes(b"prefix" + record + b"suffix")
            self.assertEqual(extractor.scan_input(source), [(6, record)])

    def test_rejects_record_without_manifest_markers(self):
        self.assertEqual(extractor.find_calibrations(synthetic_record(False)), [])

    def test_rejects_input_output_aliases_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "isc.img"
            source.write_bytes(synthetic_record())
            with self.assertRaises(RuntimeError):
                extractor.validate_output(source, [source])
            symlink = Path(directory) / "output.bin"
            symlink.symlink_to(source)
            with self.assertRaises(RuntimeError):
                extractor.validate_output(symlink, [source])

    def test_private_output_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "mesa_calibration.bin"
            extractor.write_private(output, b"sample")
            self.assertEqual(output.read_bytes(), b"sample")
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)


if __name__ == "__main__":
    unittest.main()
