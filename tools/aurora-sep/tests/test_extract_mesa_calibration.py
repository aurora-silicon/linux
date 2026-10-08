"""Offline safety and format tests for the bundled calibration extractor."""

import importlib.util
import contextlib
import io
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch


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


def synthetic_fsc2_image(kind=b"FSC2", with_manifest=True, payload=None,
                         manifest=None, im4p_tail=b"", image_tail=b""):
    """A standalone IMG4 as the MacBook Neo stores it, structurally."""
    if payload is None:
        payload = b"CALB" + b"x" * 64
    im4p = tlv(0x30, ia5(b"IM4P") + ia5(kind) + ia5(b"test")
                + tlv(0x04, payload) + im4p_tail)
    if manifest is None:
        manifest = tlv(0xA0, tlv(0x30, ia5(b"IM4M"))) if with_manifest else b""
    return tlv(0x30, ia5(b"IMG4") + im4p + manifest + image_tail)


def wrap_comb(image):
    fdrd = tlv(0x30, ia5(b"fdrd") + tlv(0x04, image))
    return tlv(0x30, ia5(b"comb") + fdrd + tlv(0x30, ia5(b"secb")))


class CalibrationExtractorTests(unittest.TestCase):
    def test_scans_only_complete_manifest_marked_record(self):
        record = synthetic_record()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "isc.img"
            source.write_bytes(b"prefix" + record + b"suffix")
            self.assertEqual(extractor.scan_input(source), [(6, record)])

    def test_rejects_record_without_manifest_markers(self):
        self.assertEqual(extractor.find_calibrations(synthetic_record(False)), [])

    def test_scans_a_standalone_fsc2_image(self):
        image = synthetic_fsc2_image()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "isc.img"
            source.write_bytes(b"prefix" + image + b"suffix")
            self.assertEqual(extractor.scan_input(source), [(6, image)])

    def test_rejects_fsc2_image_without_manifest_or_of_another_type(self):
        self.assertEqual(extractor.find_calibrations(synthetic_fsc2_image(with_manifest=False)), [])
        self.assertEqual(extractor.find_calibrations(synthetic_fsc2_image(kind=b"FSCx")), [])

    def test_the_fscl_image_inside_a_comb_record_is_found_once(self):
        record = synthetic_record()
        self.assertEqual(extractor.find_calibrations(record), [(0, len(record))])

    def test_rejects_untyped_manifest_marker(self):
        image = synthetic_fsc2_image(manifest=tlv(0xA0, b"junkIM4Mjunk"))
        self.assertEqual(extractor.find_calibrations(image), [])

    def test_rejects_manifest_with_wrong_marker_tag_or_prefix(self):
        for contents in (tlv(0x04, b"IM4M"), ia5(b"notIM4M")):
            with self.subTest(contents=contents):
                image = synthetic_fsc2_image(manifest=tlv(0xA0, tlv(0x30, contents)))
                self.assertEqual(extractor.find_calibrations(image), [])

    def test_rejects_manifest_wrapper_with_two_sequences(self):
        manifest = tlv(0xA0, tlv(0x30, ia5(b"IM4M")) * 2)
        self.assertEqual(extractor.find_calibrations(
            synthetic_fsc2_image(manifest=manifest)), [])

    def test_rejects_truncated_manifest_child(self):
        manifest = tlv(0xA0, tlv(0x30, ia5(b"IM4M") + b"\x04\x05x"))
        self.assertEqual(extractor.find_calibrations(
            synthetic_fsc2_image(manifest=manifest)), [])

    def test_accepts_bounded_manifest_fields_without_signature_verification(self):
        contents = (ia5(b"IM4M") + tlv(0x02, b"\x00") + tlv(0x31, b"")
                    + tlv(0x04, b"synthetic") + tlv(0x30, b""))
        image = synthetic_fsc2_image(manifest=tlv(0xA0, tlv(0x30, contents)))
        self.assertEqual(extractor.find_calibrations(image), [(0, len(image))])

    def test_rejects_truncated_im4p_tail(self):
        image = synthetic_fsc2_image(im4p_tail=b"\x04\x05x")
        self.assertEqual(extractor.find_calibrations(image), [])

    def test_accepts_bounded_optional_im4p_fields(self):
        image = synthetic_fsc2_image(im4p_tail=tlv(0x30, tlv(0x02, b"\x01")))
        self.assertEqual(extractor.find_calibrations(image), [(0, len(image))])

    def test_rejects_data_after_fsc2_manifest(self):
        for tail in (ia5(b"extra"), b"\x04\x05x"):
            with self.subTest(tail=tail):
                self.assertEqual(extractor.find_calibrations(
                    synthetic_fsc2_image(image_tail=tail)), [])

    def test_requires_calb_within_fsc2_header(self):
        for payload in (b"x" * 300, b"x" * 253 + b"CALB"):
            self.assertEqual(extractor.find_calibrations(
                synthetic_fsc2_image(payload=payload)), [])
        image = synthetic_fsc2_image(payload=b"x" * 252 + b"CALB")
        self.assertEqual(extractor.find_calibrations(image), [(0, len(image))])

    def test_rejects_negative_and_malformed_der_bounds(self):
        with self.assertRaises(extractor.DERError):
            extractor.TLV(b"\x30\x00", -1)
        malformed = (b"", b"\x30", b"\x30\x80", b"\x30\x81", b"\x30\x81\x01x",
                     b"\x30\x82\x00\x80", b"\x30\x85" + b"x" * 5,
                     b"\x30\x84\xff\xff\xff\xff" + ia5(b"IMG4"))
        for data in malformed:
            with self.subTest(data=data):
                self.assertEqual(extractor.find_calibrations(data), [])

    def test_rejects_every_truncated_fsc2_prefix(self):
        image = synthetic_fsc2_image()
        for end in range(len(image)):
            with self.subTest(end=end):
                self.assertEqual(extractor.find_calibrations(image[:end]), [])

    def test_does_not_carve_fsc2_from_an_unsupported_comb(self):
        record = wrap_comb(synthetic_fsc2_image())
        self.assertEqual(extractor.find_calibrations(record), [])

    def test_does_not_carve_fsc2_from_a_truncated_comb(self):
        image = synthetic_fsc2_image()
        record = wrap_comb(image)
        self.assertEqual(extractor.find_calibrations(record[:-1]), [])

    def test_nested_fsc2_in_legacy_comb_is_not_a_second_candidate(self):
        nested = synthetic_fsc2_image()
        im4p = tlv(0x30, ia5(b"IM4P") + ia5(b"FSCl") + ia5(b"test")
                    + tlv(0x04, b"CALB" + nested + b"x" * 320))
        record = wrap_comb(tlv(0x30, ia5(b"IMG4") + im4p + ia5(b"IM4M") + ia5(b"FSCl")))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "synthetic.der"
            source.write_bytes(b"x" * 110 + record)
            with patch.object(extractor, "SCAN_CHUNK_SIZE", 64), patch.object(extractor, "SCAN_OVERLAP", 1024):
                self.assertEqual(extractor.scan_input(source), [(110, record)])

    def test_standalone_fsc2_across_chunk_and_der_header_boundaries(self):
        image = synthetic_fsc2_image()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "synthetic.der"
            with patch.object(extractor, "SCAN_CHUNK_SIZE", 64), patch.object(extractor, "SCAN_OVERLAP", 256):
                for offset in range(54, 70):
                    with self.subTest(offset=offset):
                        source.write_bytes(b"x" * offset + image + b"x" * 300)
                        self.assertEqual(extractor.scan_input(source), [(offset, image)])

    def test_nested_standalone_image_is_not_carved_twice(self):
        nested = synthetic_fsc2_image()
        outer = synthetic_fsc2_image(payload=b"CALB" + nested + b"x" * 320)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "synthetic.der"
            source.write_bytes(outer)
            with patch.object(extractor, "SCAN_CHUNK_SIZE", 64), patch.object(extractor, "SCAN_OVERLAP", 1024):
                self.assertEqual(extractor.scan_input(source), [(0, outer)])

    def test_main_deduplicates_identical_copies_and_rejects_different_ones(self):
        image = synthetic_fsc2_image()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "synthetic.der"
            output = Path(directory) / "calibration.bin"
            for second, expected in ((image, 0), (synthetic_fsc2_image(payload=b"CALBy"), 1)):
                source.write_bytes(image + b"padding" + second)
                with patch("sys.argv", [str(SCRIPT), str(source), "-o", str(output)]), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(extractor.main(), expected)
                self.assertEqual(output.read_bytes(), image)
                self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)

    def test_packaged_extractor_is_identical(self):
        packaged = SCRIPT.parent / "aurora-touchid" / "extract-mesa-calibration"
        self.assertEqual(SCRIPT.read_bytes(), packaged.read_bytes())

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
