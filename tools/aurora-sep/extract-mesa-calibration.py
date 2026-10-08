#!/usr/bin/env python3
"""Carve the per-device Mesa calibration from an Apple iBoot System Container.

Most Macs keep it as a comb record wrapping an FSCl IMG4. The MacBook Neo
(J700) keeps it as a standalone IMG4 whose IM4P type is FSC2. Manifest
presence is checked; cryptographic signatures are not verified.
"""

import argparse
import hashlib
import os
from pathlib import Path
import stat
import sys
import tempfile


ISC_PARTITION_NAME = "iBootSystemContainer"
SYS_BLOCK = Path("/sys/class/block")
DEV_ROOT = Path("/dev")
SCAN_CHUNK_SIZE = 4 * 1024 * 1024
SCAN_OVERLAP = 4 * 1024 * 1024


class DERError(ValueError):
    pass


class TLV:
    def __init__(self, data, start, require_complete=True):
        if start < 0:
            raise DERError("negative DER offset")
        self.start = start
        try:
            self.tag = data[start]
            first_length = data[start + 1]
        except IndexError as error:
            raise DERError("truncated DER header") from error

        if first_length < 0x80:
            length = first_length
            header_length = 2
        else:
            length_bytes = first_length & 0x7f
            if length_bytes == 0:
                raise DERError("indefinite DER length")
            if length_bytes > 4:
                raise DERError("unreasonably large DER length")
            length_start = start + 2
            length_end = length_start + length_bytes
            if length_end > len(data):
                raise DERError("truncated DER length")
            encoded = data[length_start:length_end]
            if encoded[0] == 0:
                raise DERError("non-minimal DER length")
            length = int.from_bytes(encoded, "big")
            if length < 0x80:
                raise DERError("non-minimal DER length")
            header_length = 2 + length_bytes

        self.content_start = start + header_length
        self.end = self.content_start + length
        if require_complete and self.end > len(data):
            raise DERError("DER value extends beyond input")

    def content(self, data):
        return data[self.content_start:self.end]


def child_tlvs(data, container):
    offset = container.content_start
    while offset < container.end:
        child = TLV(data, offset)
        if child.end > container.end:
            raise DERError("DER child extends beyond its container")
        yield child
        offset = child.end
    if offset != container.end:
        raise DERError("malformed DER container")


def take_child(data, children, tag, value=None):
    try:
        child = next(children)
    except StopIteration as error:
        raise DERError("missing DER child") from error
    if child.tag != tag:
        raise DERError(f"unexpected DER tag 0x{child.tag:02x}")
    if value is not None and child.content(data) != value:
        raise DERError("unexpected DER value")
    return child

def validate_calibration(data, start):
    """Return the end offset if data[start:] begins with a Mesa calibration."""
    outer = TLV(data, start)
    if outer.tag != 0x30:
        raise DERError("outer object is not a sequence")
    outer_children = child_tlvs(data, outer)
    take_child(data, outer_children, 0x16, b"comb")

    fdrd = take_child(data, outer_children, 0x30)
    fdrd_children = child_tlvs(data, fdrd)
    take_child(data, fdrd_children, 0x16, b"fdrd")
    img4_octets = take_child(data, fdrd_children, 0x04)
    try:
        next(fdrd_children)
    except StopIteration:
        pass
    else:
        raise DERError("unexpected data in fdrd container")

    secb = take_child(data, outer_children, 0x30)
    secb_children = child_tlvs(data, secb)
    take_child(data, secb_children, 0x16, b"secb")
    try:
        next(outer_children)
    except StopIteration:
        pass
    else:
        raise DERError("unexpected data after secb container")

    img4 = TLV(data, img4_octets.content_start)
    if img4.tag != 0x30 or img4.end != img4_octets.end:
        raise DERError("fdrd is not one complete IMG4 sequence")
    img4_children = child_tlvs(data, img4)
    take_child(data, img4_children, 0x16, b"IMG4")

    im4p = take_child(data, img4_children, 0x30)
    im4p_children = child_tlvs(data, im4p)
    take_child(data, im4p_children, 0x16, b"IM4P")
    take_child(data, im4p_children, 0x16, b"FSCl")
    take_child(data, im4p_children, 0x16)
    payload = take_child(data, im4p_children, 0x04).content(data)
    if b"CALB" not in payload[:64]:
        raise DERError("FSCl payload lacks its CALB header")

    # An FSCl calibration has IM4M manifest markers after the IM4P. Requiring
    # both markers avoids accepting an arbitrary, truncated FSCl object.
    remainder = data[im4p.end:img4.end]
    if b"IM4M" not in remainder or b"FSCl" not in remainder:
        raise DERError("IMG4 lacks an FSCl manifest")

    return outer.end


def validate_fsc2_image(data, start):
    """Validate an FSC2 IMG4 structure and manifest presence, not its signature."""
    img4 = TLV(data, start)
    if img4.tag != 0x30:
        raise DERError("IMG4 is not a sequence")
    img4_children = child_tlvs(data, img4)
    take_child(data, img4_children, 0x16, b"IMG4")

    im4p = take_child(data, img4_children, 0x30)
    im4p_children = child_tlvs(data, im4p)
    take_child(data, im4p_children, 0x16, b"IM4P")
    take_child(data, im4p_children, 0x16, b"FSC2")
    take_child(data, im4p_children, 0x16)
    payload = take_child(data, im4p_children, 0x04).content(data)
    if b"CALB" not in payload[:256]:
        raise DERError("FSC2 payload lacks its CALB header")

    # Optional IM4P fields must still be bounded DER objects.
    for _ in im4p_children:
        pass
    manifest = take_child(data, img4_children, 0xA0)
    manifest_children = child_tlvs(data, manifest)
    im4m = take_child(data, manifest_children, 0x30)
    im4m_children = child_tlvs(data, im4m)
    take_child(data, im4m_children, 0x16, b"IM4M")
    for _ in im4m_children:
        pass
    if next(manifest_children, None) is not None:
        raise DERError("manifest wrapper contains more than one IM4M")
    if next(img4_children, None) is not None:
        raise DERError("unexpected data after FSC2 manifest")
    return img4.end


def find_comb_ranges(data):
    """Recognize comb ranges, including a body split across scan windows."""
    ranges = set()
    signature = b"\x16\x04comb"
    search_at = 0
    while True:
        marker = data.find(signature, search_at)
        if marker < 0:
            break
        for header_length in range(2, 7):
            start = marker - header_length
            if start < 0 or data[start] != 0x30:
                continue
            try:
                outer = TLV(data, start, require_complete=False)
                if outer.content_start != marker:
                    continue
                child = TLV(data, marker)
                if child.end > outer.end:
                    continue
            except DERError:
                continue
            ranges.add((start, outer.end))
        search_at = marker + 1
    return ranges


def find_calibrations(data):
    results = find_comb_calibrations(data)
    comb_ranges = find_comb_ranges(data)
    # A standalone FSC2 IMG4 must not be carved out of a comb container.
    signature = b"\x16\x04IMG4"
    search_at = 0
    while True:
        marker = data.find(signature, search_at)
        if marker < 0:
            break
        for header_length in range(2, 7):
            start = marker - header_length
            if start < 0 or data[start] != 0x30:
                continue
            if (any(begin < start < end for begin, end in comb_ranges)
                    or any(begin <= start < end for begin, end in results)):
                continue
            try:
                end = validate_fsc2_image(data, start)
            except DERError:
                continue
            candidate = (start, end)
            if candidate not in results:
                results.append(candidate)
        search_at = marker + 1
    return results


def find_comb_calibrations(data):
    results = []
    signature = b"\x16\x04comb"
    search_at = 0
    while True:
        marker = data.find(signature, search_at)
        if marker < 0:
            break
        # The outer SEQUENCE header immediately precedes the comb IA5String.
        # Try every legal header width supported by TLV above.
        for header_length in range(2, 7):
            start = marker - header_length
            if start < 0 or data[start] != 0x30:
                continue
            try:
                end = validate_calibration(data, start)
            except DERError:
                continue
            candidate = (start, end)
            if candidate not in results:
                results.append(candidate)
        search_at = marker + 1
    return results


def discover_isc_inputs(sys_block=SYS_BLOCK, dev_root=DEV_ROOT):
    """Find every iSC by GPT partition name, independent of disk numbering."""
    candidates = []
    try:
        block_devices = sorted(sys_block.iterdir())
    except OSError as error:
        block_devices = []
        sysfs_error = error
    else:
        sysfs_error = None

    for block_device in block_devices:
        try:
            properties = {}
            for line in (block_device / "uevent").read_text().splitlines():
                key, separator, value = line.partition("=")
                if separator:
                    properties[key] = value
        except OSError:
            continue
        if properties.get("PARTNAME") != ISC_PARTITION_NAME:
            continue
        device_name = properties.get("DEVNAME", block_device.name)
        if device_name.startswith("/dev/"):
            device_name = device_name[5:]
        device = dev_root / device_name
        if device.exists():
            candidates.append(device)

    # This standard udev link also covers chroots where /sys is unavailable.
    by_partlabel = dev_root / "disk/by-partlabel" / ISC_PARTITION_NAME
    if by_partlabel.exists():
        candidates.append(by_partlabel)

    # Preserve order while removing aliases or duplicate sysfs entries.
    unique = []
    seen = set()
    for candidate in candidates:
        try:
            identity = candidate.resolve()
        except OSError:
            identity = candidate
        if identity not in seen:
            seen.add(identity)
            unique.append(candidate)
    if not unique and sysfs_error is not None:
        raise RuntimeError(
            f"cannot inspect {sys_block} and {by_partlabel} is unavailable: "
            f"{sysfs_error}"
        ) from sysfs_error
    return unique


def scan_input(path):
    """Scan a file or block device without retaining the whole iSC in memory."""
    mode = path.stat().st_mode
    if not (stat.S_ISREG(mode) or stat.S_ISBLK(mode)):
        raise RuntimeError(f"{path} is not a regular file or block device")

    candidates = {}
    comb_ranges = set()
    overlap = b""
    total = 0
    with path.open("rb", buffering=0) as source:
        while True:
            chunk = source.read(SCAN_CHUNK_SIZE)
            if not chunk:
                break
            total += len(chunk)
            window = overlap + chunk
            window_start = total - len(window)
            comb_ranges.update((window_start + start, window_start + end)
                               for start, end in find_comb_ranges(window))
            for start, end in find_calibrations(window):
                absolute_start = window_start + start
                candidates.setdefault(absolute_start, bytes(window[start:end]))
            overlap = window[-SCAN_OVERLAP:]
    # A nested image can be complete before its enclosing object is complete.
    enclosing_ranges = comb_ranges | {(start, start + len(blob))
                                      for start, blob in candidates.items()}
    return sorted((start, blob) for start, blob in candidates.items()
                  if not any(begin < start < end for begin, end in enclosing_ranges))


def write_private(path, blob):
    path.parent.mkdir(parents=True, exist_ok=True)
    old_umask = os.umask(0o077)
    temporary = None
    try:
        fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        temporary = Path(name)
        with os.fdopen(fd, "wb") as output:
            output.write(blob)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, stat.S_IRUSR | stat.S_IWUSR)
        os.replace(temporary, path)
        temporary = None
    finally:
        os.umask(old_umask)
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def validate_output(path, inputs):
    """Reject output targets that could replace an input or special file."""
    if path.is_symlink():
        raise RuntimeError(f"output {path} is a symbolic link")
    if not path.exists():
        return
    for input_path in inputs:
        try:
            if os.path.samefile(path, input_path):
                raise RuntimeError(f"output {path} aliases input {input_path}")
        except FileNotFoundError:
            pass
    if not stat.S_ISREG(path.stat().st_mode):
        raise RuntimeError(f"output {path} is not a regular file")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", nargs="?", type=Path,
        help=(
            "raw iBoot System Container or dump; omit to discover all "
            "iBootSystemContainer partitions"
        ),
    )
    parser.add_argument(
        "-o", "--output", type=Path, default=Path("mesa_calibration.bin"),
        help="output file (default: ./mesa_calibration.bin)",
    )
    return parser.parse_args(argv)


def main():
    args = parse_args()
    try:
        inputs = [args.input] if args.input is not None else discover_isc_inputs()
        if not inputs:
            raise RuntimeError(
                "no iBootSystemContainer partition found; pass its block device "
                "or a raw partition dump explicitly"
            )
        validate_output(args.output, inputs)

        candidates = []
        for input_path in inputs:
            for offset, blob in scan_input(input_path):
                candidates.append((input_path, offset, blob))
        if not candidates:
            raise RuntimeError(
                "no manifest-marked Mesa FSCl or FSC2 calibration found; expected the raw "
                "iBoot System Container (Apple Silicon boot partition)"
            )
        blobs = {}
        for input_path, offset, blob in candidates:
            blobs.setdefault(blob, []).append((input_path, offset))
        if len(blobs) != 1:
            locations = ", ".join(
                f"{input_path}@0x{offset:x}"
                for input_path, offset, _ in candidates
            )
            raise RuntimeError(
                f"found different calibrations at {locations}; "
                "pass the intended iSC explicitly"
            )

        blob, locations = next(iter(blobs.items()))
        write_private(args.output, blob)
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    digest = hashlib.sha256(blob).hexdigest()
    formatted_locations = ", ".join(
        f"{input_path}@0x{offset:x}" for input_path, offset in locations
    )
    print(f"found Mesa calibration at {formatted_locations}")
    print(f"wrote {len(blob)} bytes to {args.output}")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
